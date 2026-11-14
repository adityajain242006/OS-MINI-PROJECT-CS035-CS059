/*
 * main.c - command-line interface
 *
 * CampusCompute - Intelligent Job Scheduling and Resource Management
 * OS Mini-Project (UE25CS242A), Level 3
 * Team: Amudan Desikan (PES1UG25CS059), Aditya Jain (PES1UG25CS035)
 *
 * Every feature is a separate sub-command so it can be tested on its own.
 * Invalid commands or arguments print an error plus the relevant usage line
 * and exit with status 2; runtime failures exit with status 1.
 */
#include "common.h"
#include "ipc.h"
#include "job.h"
#include "logger.h"
#include "scheduler.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------ usage text ------------------------------ */

#define U_JOBS     "jobs <jobfile>"
#define U_SCHED    "schedule <fcfs|rr|priority|compare> <jobfile> [-q N] [-v]"
#define U_LOGGER   "logger [-p producers] [-c consumers] [-n events] [-b buffer] [-f jobfile]"
#define U_IPC      "ipc [jobfile]"

static void print_help(void)
{
    printf("CampusCompute v%s - Intelligent Job Scheduling and Resource Management\n", CC_VERSION);
    printf("OS Mini-Project UE25CS242A (run from the project directory)\n\n");
    printf("Usage: ./%s <command> [options]\n\n", CC_PROG);
    printf("Commands:\n");
    printf("  help                                 show this help\n");
    printf("  jobs <jobfile>                       FR1 load, validate and list jobs\n");
    printf("  schedule fcfs <jobfile> [-v]         FR2 First-Come, First-Served\n");
    printf("  schedule rr <jobfile> [-q N] [-v]    FR2 Round Robin (default quantum %d)\n", DEFAULT_QUANTUM);
    printf("  schedule priority <jobfile> [-v]     FR2 non-preemptive Priority (lower number = higher)\n");
    printf("  schedule compare <jobfile> [-q N]    FR2 all three policies on the same job set\n");
    printf("  logger [-p N] [-c N] [-n N] [-b N] [-f jobfile]\n");
    printf("                                       FR3 pthread producer-consumer logger\n");
    printf("  ipc [jobfile]                        FR4 POSIX shared memory + named semaphores\n\n");
    printf("Options:\n");
    printf("  -v  lifecycle trace (NEW/READY/RUNNING/COMPLETED)   -q  RR time quantum 1..%d\n", MAX_QUANTUM);
    printf("  -p/-c  producer/consumer threads 1..%d   -n  events per producer 1..%d\n",
           LOGGER_MAX_THREADS, LOGGER_MAX_EVENTS);
    printf("  -b  buffer slots 1..%d   -f  job file used as the event source\n\n", LOGGER_MAX_CAPACITY);
    printf("Examples:\n");
    printf("  ./%s schedule rr tests/jobs.txt -q 2\n\n", CC_PROG);
    printf("Log file: %s   Exit status: 0 ok, 1 runtime error, 2 usage error\n", LOG_FILE);
}

/* Print "error: <msg>" + the command's usage line; return EXIT_USAGE. */
static int usage_error(const char *usage, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static int usage_error(const char *usage, const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "error: %s\n", msg);
    if (usage != NULL)
        fprintf(stderr, "usage: ./%s %s\n", CC_PROG, usage);
    fprintf(stderr, "Run './%s help' to see all commands.\n", CC_PROG);
    applog("CLI", "rejected: %s", msg);
    return EXIT_USAGE;
}

/* Parse "-x VALUE" at argv[*i]; advances *i. Returns 0 or EXIT_USAGE. */
static int opt_int(int argc, char **argv, int *i, long lo, long hi, int *out, const char *usage)
{
    const char *opt = argv[*i];
    if (*i + 1 >= argc)
        return usage_error(usage, "option '%s' needs a value", opt);
    const char *val = argv[++(*i)];
    if (parse_int_range(val, lo, hi, out) != 0)
        return usage_error(usage, "invalid value '%s' for %s (expected an integer %ld..%ld)", val, opt, lo, hi);
    return 0;
}

/* -------------------------------- FR1 ---------------------------------- */

static int cmd_jobs(int argc, char **argv)
{
    if (argc < 3)
        return usage_error(U_JOBS, "missing job file");
    if (argc > 3)
        return usage_error(U_JOBS, "unexpected argument '%s'", argv[3]);
    Job jobs[MAX_JOBS];
    int n = 0;
    int rc = jobs_load(argv[2], jobs, MAX_JOBS, &n);
    if (rc != 0)
        return EXIT_RUNTIME;
    printf("Loaded %d job(s) from %s - all fields validated\n", n, argv[2]);
    printf("(fields: id 1..%d, arrival 0..%d, burst 1..%d, priority 0..%d with lower = higher; IDs unique)\n\n",
           JOB_ID_MAX, JOB_ARRIVAL_MAX, JOB_BURST_MAX, JOB_PRIO_MAX);
    jobs_print(jobs, n);
    printf("\nAll jobs start in state NEW; the scheduler moves them NEW -> READY -> RUNNING -> COMPLETED\n");
    printf("(use 'schedule <policy> %s -v' to see the full lifecycle trace)\n", argv[2]);
    applog("JOBS", "loaded %d jobs from %s", n, argv[2]);
    return EXIT_OK;
}

/* -------------------------------- FR2 ---------------------------------- */

static int cmd_schedule(int argc, char **argv)
{
    if (argc < 3)
        return usage_error(U_SCHED, "missing scheduling policy");
    const char *pol = argv[2];
    int compare = 0;
    Policy p = POL_FCFS;
    if (strcmp(pol, "fcfs") == 0)            p = POL_FCFS;
    else if (strcmp(pol, "rr") == 0)         p = POL_RR;
    else if (strcmp(pol, "priority") == 0)   p = POL_PRIORITY;
    else if (strcmp(pol, "compare") == 0)    compare = 1;
    else
        return usage_error(U_SCHED, "unknown scheduling policy '%s' (use fcfs, rr, priority or compare)", pol);

    if (argc < 4)
        return usage_error(U_SCHED, "missing job file");
    const char *file = argv[3];

    int quantum = DEFAULT_QUANTUM, verbose = 0, q_given = 0;
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "-q") == 0) {
            int rc = opt_int(argc, argv, &i, 1, MAX_QUANTUM, &quantum, U_SCHED);
            if (rc)
                return rc;
            q_given = 1;
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else {
            return usage_error(U_SCHED, "unknown option '%s'", argv[i]);
        }
    }
    if (q_given && !compare && p != POL_RR)
        return usage_error(U_SCHED, "option -q only applies to 'rr' and 'compare'");

    Job jobs[MAX_JOBS];
    int n = 0;
    if (jobs_load(file, jobs, MAX_JOBS, &n) != 0)
        return EXIT_RUNTIME;
    printf("Job set: %s (%d jobs)\n", file, n);

    if (!compare) {
        SchedResult r;
        if (schedule_run(p, jobs, n, quantum, verbose, &r) != 0)
            return EXIT_RUNTIME;
        schedule_print(&r);
        schedule_free(&r);
        return EXIT_OK;
    }

    SchedResult res[3];
    Policy order[3] = { POL_FCFS, POL_RR, POL_PRIORITY };
    for (int k = 0; k < 3; k++) {
        if (schedule_run(order[k], jobs, n, quantum, verbose, &res[k]) != 0) {
            for (int j = 0; j < k; j++)
                schedule_free(&res[j]);
            return EXIT_RUNTIME;
        }
    }
    printf("Policies: FCFS | Round Robin (q=%d) | Priority (non-preemptive, lower number = higher)\n", quantum);
    printf("Tie-break (all): equal arrival -> lower job ID; Priority ties -> earlier arrival -> lower ID\n\n");
    for (int k = 0; k < 3; k++) {
        printf("%-15s order: ", order[k] == POL_RR ? "RR" : policy_short(order[k]));
        for (int s = 0; s < res[k].nseg; s++) {
            if (res[k].seg[s].job_id < 0)
                printf("idle");
            else
                printf("J%d", res[k].seg[s].job_id);
            printf("(%d-%d)", res[k].seg[s].start, res[k].seg[s].end);
            if (s + 1 < res[k].nseg)
                printf(((s + 1) % 8 == 0) ? "\n%-22s" : " ", "");
        }
        printf("\n");
    }
    schedule_print_compare(res, 3);
    for (int k = 0; k < 3; k++)
        schedule_free(&res[k]);
    return EXIT_OK;
}

/* -------------------------------- FR3 ---------------------------------- */

static int cmd_logger(int argc, char **argv)
{
    int p = LOGGER_DEFAULT_PRODUCERS, c = LOGGER_DEFAULT_CONSUMERS;
    int ev = LOGGER_DEFAULT_EVENTS, cap = LOGGER_DEFAULT_CAPACITY;
    const char *jobfile = NULL;
    for (int i = 2; i < argc; i++) {
        int rc = 0;
        if (strcmp(argv[i], "-p") == 0)
            rc = opt_int(argc, argv, &i, 1, LOGGER_MAX_THREADS, &p, U_LOGGER);
        else if (strcmp(argv[i], "-c") == 0)
            rc = opt_int(argc, argv, &i, 1, LOGGER_MAX_THREADS, &c, U_LOGGER);
        else if (strcmp(argv[i], "-n") == 0)
            rc = opt_int(argc, argv, &i, 1, LOGGER_MAX_EVENTS, &ev, U_LOGGER);
        else if (strcmp(argv[i], "-b") == 0)
            rc = opt_int(argc, argv, &i, 1, LOGGER_MAX_CAPACITY, &cap, U_LOGGER);
        else if (strcmp(argv[i], "-f") == 0) {
            if (i + 1 >= argc)
                return usage_error(U_LOGGER, "option '-f' needs a file name");
            jobfile = argv[++i];
        } else
            return usage_error(U_LOGGER, "unknown option '%s'", argv[i]);
        if (rc)
            return rc;
    }

    Job jobs[MAX_JOBS];
    int n = 0;
    if (jobfile != NULL) {
        if (jobs_load(jobfile, jobs, MAX_JOBS, &n) != 0)
            return EXIT_RUNTIME;
    } else if (access(DEFAULT_JOBS, R_OK) == 0 && jobs_load(DEFAULT_JOBS, jobs, MAX_JOBS, &n) == 0) {
        jobfile = DEFAULT_JOBS;
    }
    printf("Concurrent activity logger (POSIX threads)\n");
    printf("Event source: %s\n", jobfile ? jobfile : "synthetic job IDs");

    LoggerStats st;
    int rc = logger_run(p, c, ev, cap, n ? jobs : NULL, n, &st);
    return rc == 0 ? EXIT_OK : EXIT_RUNTIME;
}

/* -------------------------------- FR4 ---------------------------------- */

static int cmd_ipc(int argc, char **argv)
{
    if (argc > 3)
        return usage_error(U_IPC, "unexpected argument '%s'", argv[3]);
    const char *file = argc == 3 ? argv[2] : DEFAULT_JOBS;
    Job jobs[MAX_JOBS];
    int n = 0;
    if (jobs_load(file, jobs, MAX_JOBS, &n) != 0)
        return EXIT_RUNTIME;
    printf("Shared-memory IPC: dispatcher (parent) sends %d job records from %s to accountant (child)\n\n", n, file);
    IpcStats st;
    return ipc_run(jobs, n, &st) == 0 ? EXIT_OK : EXIT_RUNTIME;
}

/* ------------------------------ dispatch ------------------------------ */

typedef struct {
    const char *name;
    int (*fn)(int, char **);
} Command;

static int cmd_help(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    print_help();
    return EXIT_OK;
}

static const Command COMMANDS[] = {
    { "help",     cmd_help     },
    { "jobs",     cmd_jobs     },
    { "schedule", cmd_schedule },
    { "logger",   cmd_logger   },
    { "ipc",      cmd_ipc      },
};

int main(int argc, char **argv)
{
    /* line-buffered stdout so output from parent/children/threads stays in
     * order even when piped (e.g. into tee) */
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc < 2) {
        fprintf(stderr, "error: no command given\n\n");
        print_help();
        return EXIT_USAGE;
    }
    const char *name = argv[1];
    if (strcmp(name, "-h") == 0 || strcmp(name, "--help") == 0)
        name = "help";

    for (size_t i = 0; i < sizeof COMMANDS / sizeof COMMANDS[0]; i++) {
        if (strcmp(name, COMMANDS[i].name) == 0) {
            char line[256] = "";
            size_t len = 0;
            for (int k = 1; k < argc && len < sizeof line - 1; k++)
                len += (size_t)snprintf(line + len, sizeof line - len, "%s%s", k > 1 ? " " : "", argv[k]);
            applog("CLI", "command: %s", line);
            return COMMANDS[i].fn(argc, argv);
        }
    }
    fprintf(stderr, "error: unknown command '%s'\n", argv[1]);
    fprintf(stderr, "valid commands:");
    for (size_t i = 0; i < sizeof COMMANDS / sizeof COMMANDS[0]; i++)
        fprintf(stderr, " %s", COMMANDS[i].name);
    fprintf(stderr, "\nRun './%s help' for usage and examples.\n", CC_PROG);
    applog("CLI", "rejected: unknown command '%s'", argv[1]);
    return EXIT_USAGE;
}
