/*
 * main.c - FR8: command-line interface, error handling and integrated demo
 *
 * CampusCompute - Intelligent Job Scheduling and Resource Management
 * OS Mini-Project (UE25CS242A), Level 3
 * Team: Amudan Desikan (PES1UG25CS059), Aditya Jain (PES1UG25CS035)
 *
 * Every feature is a separate sub-command so it can be tested on its own.
 * Invalid commands or arguments print an error plus the relevant usage line
 * and exit with status 2; runtime failures exit with status 1.
 */
#include "banker.h"
#include "common.h"
#include "ipc.h"
#include "job.h"
#include "logger.h"
#include "rlimit_demo.h"
#include "scheduler.h"
#include "workers.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

/* ------------------------------ usage text ------------------------------ */

#define U_JOBS     "jobs <jobfile>"
#define U_SCHED    "schedule <fcfs|rr|priority|compare> <jobfile> [-q N] [-v]"
#define U_LOGGER   "logger [-p producers] [-c consumers] [-n events] [-b buffer] [-f jobfile]"
#define U_IPC      "ipc [jobfile]"
#define U_BANKER   "banker <statefile>"
#define U_WORKERS  "workers [-n count] [--fail K | --crash K]"
#define U_RLIMIT   "rlimit [cpu|nofile|all]"
#define U_DEMO     "demo [--full]"
#define U_CLEANUP  "cleanup [--check]"

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
    printf("  ipc [jobfile]                        FR4 POSIX shared memory + named semaphores\n");
    printf("  banker <statefile>                   FR5 Banker's safety algorithm\n");
    printf("  workers [-n N] [--fail K | --crash K]\n");
    printf("                                       FR6 fork()/waitpid() workers, simulated failure\n");
    printf("  rlimit [cpu|nofile|all]              FR7 setrlimit() demonstration\n");
    printf("  demo [--full]                        integrated run of FR1-FR8 with summary\n");
    printf("  cleanup [--check]                    verify/remove leftover program IPC objects\n\n");
    printf("Options:\n");
    printf("  -v  lifecycle trace (NEW/READY/RUNNING/COMPLETED)   -q  RR time quantum 1..%d\n", MAX_QUANTUM);
    printf("  -p/-c  producer/consumer threads 1..%d   -n  events per producer 1..%d (workers: 1..%d)\n",
           LOGGER_MAX_THREADS, LOGGER_MAX_EVENTS, WORKERS_MAX);
    printf("  -b  buffer slots 1..%d   --fail K  worker K exits with status %d   --crash K  worker K calls abort()\n\n",
           LOGGER_MAX_CAPACITY, SIMULATED_EXIT_CODE);
    printf("Examples:\n");
    printf("  ./%s schedule rr tests/jobs.txt -q 2\n", CC_PROG);
    printf("  ./%s banker tests/banker_unsafe.txt\n", CC_PROG);
    printf("  ./%s workers --fail 2\n\n", CC_PROG);
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

/* -------------------------------- FR5 ---------------------------------- */

static int cmd_banker(int argc, char **argv)
{
    if (argc < 3)
        return usage_error(U_BANKER, "missing state file");
    if (argc > 3)
        return usage_error(U_BANKER, "unexpected argument '%s'", argv[3]);
    BankerState s;
    if (banker_load(argv[2], &s) != 0)
        return EXIT_RUNTIME;
    printf("Banker's algorithm - state file: %s\n", argv[2]);
    banker_print_state(&s);
    BankerResult r;
    banker_check(&s, 1, &r);
    return EXIT_OK;   /* SAFE and UNSAFE are both valid analysis results */
}

/* -------------------------------- FR6 ---------------------------------- */

static int cmd_workers(int argc, char **argv)
{
    int n = WORKERS_DEFAULT, k = 0;
    FailMode mode = FAIL_NONE;
    for (int i = 2; i < argc; i++) {
        int rc = 0;
        if (strcmp(argv[i], "-n") == 0) {
            rc = opt_int(argc, argv, &i, 1, WORKERS_MAX, &n, U_WORKERS);
        } else if (strcmp(argv[i], "--fail") == 0 || strcmp(argv[i], "--crash") == 0) {
            if (mode != FAIL_NONE)
                return usage_error(U_WORKERS, "use only one of --fail / --crash");
            mode = strcmp(argv[i], "--fail") == 0 ? FAIL_EXIT : FAIL_SIGNAL;
            rc = opt_int(argc, argv, &i, 1, WORKERS_MAX, &k, U_WORKERS);
        } else {
            return usage_error(U_WORKERS, "unknown option '%s'", argv[i]);
        }
        if (rc)
            return rc;
    }
    if (mode != FAIL_NONE && k > n)
        return usage_error(U_WORKERS, "worker %d does not exist (only %d workers)", k, n);

    printf("Worker processes: fork() x %d, status collected with waitpid()\n", n);
    WorkerStats st;
    return workers_run(n, mode, k, &st) == 0 ? EXIT_OK : EXIT_RUNTIME;
}

/* -------------------------------- FR7 ---------------------------------- */

static int cmd_rlimit(int argc, char **argv)
{
    int which = RL_TEST_ALL;
    if (argc > 3)
        return usage_error(U_RLIMIT, "unexpected argument '%s'", argv[3]);
    if (argc == 3) {
        if (strcmp(argv[2], "cpu") == 0)         which = RL_TEST_CPU;
        else if (strcmp(argv[2], "nofile") == 0) which = RL_TEST_NOFILE;
        else if (strcmp(argv[2], "all") == 0)    which = RL_TEST_ALL;
        else
            return usage_error(U_RLIMIT, "unknown limit '%s' (use cpu, nofile or all)", argv[2]);
    }
    printf("Resource limits with setrlimit() - each limit is applied inside a child process\n\n");
    RlimitStats st;
    return rlimit_run(which, &st) == 0 ? EXIT_OK : EXIT_RUNTIME;
}

/* ------------------------------ cleanup ------------------------------- */

/* Count other running campuscompute processes by scanning /proc. */
static int other_instances(void)
{
    DIR *d = opendir("/proc");
    if (d == NULL)
        return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        int pid;
        if (parse_int_range(e->d_name, 1, 1L << 30, &pid) != 0 || pid == (int)getpid())
            continue;
        char path[64], comm[64] = "";
        snprintf(path, sizeof path, "/proc/%d/comm", pid);
        FILE *fp = fopen(path, "r");
        if (fp == NULL)
            continue;
        if (fgets(comm, sizeof comm, fp) != NULL) {
            comm[strcspn(comm, "\n")] = '\0';
            if (strcmp(comm, CC_PROG) == 0) {
                printf("  running: pid %d (%s)\n", pid, comm);
                n++;
            }
        }
        fclose(fp);
    }
    closedir(d);
    return n;
}

static int cmd_cleanup(int argc, char **argv)
{
    int check = 0;
    if (argc == 3 && strcmp(argv[2], "--check") == 0)
        check = 1;
    else if (argc != 2)
        return usage_error(U_CLEANUP, "unexpected argument '%s'", argv[2]);

    printf("Cleanup%s (only objects named '%s*' created by this program)\n\n",
           check ? " check" : "", IPC_PREFIX);
    printf("Stale program IPC objects:\n");
    int handled = ipc_cleanup_stale(check);
    if (handled == 0)
        printf("  none found\n");
    printf("Program IPC objects now in /dev/shm:\n");
    int left = ipc_list_objects("  ");
    printf("Other campuscompute processes running:\n");
    int others = other_instances();
    if (others == 0)
        printf("  none\n");
    errno = 0;
    int unreaped = !(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);
    printf("Unreaped child processes of this process: %s\n", unreaped ? "some" : "none");
    printf("Threads in this process: %d (main only)\n", count_threads());
    applog("CLEANUP", "%s: stale=%d remaining=%d other_instances=%d",
           check ? "check" : "cleanup", handled, left, others);
    return (left == 0 || check) ? EXIT_OK : EXIT_RUNTIME;
}

/* ------------------------------ demo ---------------------------------- */

static const char *strsignal_abbrev(int sig)
{
    switch (sig) {
    case SIGXCPU: return "SIGXCPU";
    case SIGKILL: return "SIGKILL";
    default:      return "signal";
    }
}

typedef struct {
    const char *fr;
    const char *what;
    int pass;
    char detail[256];
} DemoRow;

static int cmd_demo(int argc, char **argv)
{
    int full = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--full") == 0)
            full = 1;
        else
            return usage_error(U_DEMO, "unknown option '%s'", argv[i]);
    }
    g_brief = !full;

    DemoRow row[9];
    memset(row, 0, sizeof row);
    double t0 = mono_seconds();

    printf("================ CampusCompute integrated demonstration ================\n");
    printf("Mode  : %s\n", full ? "full output" : "compact (use 'demo --full' for every detail)");
    printf("Inputs: %s, %s, %s\n\n", DEFAULT_JOBS, BANKER_SAFE, BANKER_UNSAFE);
    applog("DEMO", "integrated demonstration started (%s)", full ? "full" : "compact");

    /* FR1 */
    printf("[1/8] FR1 Job management\n");
    Job jobs[MAX_JOBS];
    int n = 0;
    int jl = jobs_load(DEFAULT_JOBS, jobs, MAX_JOBS, &n);
    row[0] = (DemoRow){ "FR1", "Job management", jl == 0, "" };
    if (jl == 0) {
        jobs_print(jobs, n);
        snprintf(row[0].detail, sizeof row[0].detail, "%d jobs loaded and validated", n);
    } else {
        snprintf(row[0].detail, sizeof row[0].detail, "cannot load %s", DEFAULT_JOBS);
    }
    printf("      %s\n", row[0].detail);

    /* FR2 */
    printf("[2/8] FR2 CPU scheduling (same job set)\n");
    row[1] = (DemoRow){ "FR2", "CPU scheduling", 0, "skipped (no jobs)" };
    if (jl == 0) {
        SchedResult res[3];
        Policy order[3] = { POL_FCFS, POL_RR, POL_PRIORITY };
        int ok = 1, ran = 0;
        for (int k = 0; k < 3; k++) {
            if (schedule_run(order[k], jobs, n, DEFAULT_QUANTUM, 0, &res[k]) != 0) {
                ok = 0;
                break;
            }
            ran++;
            ok &= res[k].all_completed;
            if (full)
                schedule_print(&res[k]);
        }
        if (ran == 3) {
            if (full)
                schedule_print_compare(res, 3);
            for (int k = 0; k < 3; k++)
                printf("      %-15s avg TAT=%6.2f  avg WT=%6.2f  avg RT=%6.2f  all completed=%s\n",
                       order[k] == POL_RR ? "RR (q=2)" : policy_short(order[k]),
                       res[k].avg_tat, res[k].avg_wt, res[k].avg_rt, res[k].all_completed ? "yes" : "NO");
        }
        for (int k = 0; k < ran; k++)
            schedule_free(&res[k]);
        row[1].pass = ok && ran == 3;
        snprintf(row[1].detail, sizeof row[1].detail, "FCFS, RR(q=%d), Priority run on %d jobs", DEFAULT_QUANTUM, n);
    }

    /* FR3 */
    printf("[3/8] FR3 Concurrent logger (2 producers, 2 consumers, buffer 4)\n");
    LoggerStats ls;
    int lrc = logger_run(2, 2, 5, 4, jl == 0 ? jobs : NULL, jl == 0 ? n : 0, &ls);
    row[2] = (DemoRow){ "FR3", "Threaded logger", lrc == 0, "" };
    snprintf(row[2].detail, sizeof row[2].detail, "%d produced/%d consumed/%d logged, %d threads joined",
             ls.produced, ls.consumed, ls.logged, ls.producers_joined + ls.consumers_joined);
    printf("      %s (FULL waits=%d, EMPTY waits=%d)\n", row[2].detail, ls.full_waits, ls.empty_waits);

    /* FR4 */
    printf("[4/8] FR4 Shared-memory IPC\n");
    IpcStats is;
    memset(&is, 0, sizeof is);
    int irc = jl == 0 ? ipc_run(jobs, n, &is) : -1;
    row[3] = (DemoRow){ "FR4", "Shared-memory IPC", irc == 0, "" };
    snprintf(row[3].detail, sizeof row[3].detail, "%d records, burst total %d%s%d, IPC objects %s",
             is.sent, is.total_sent, is.verified ? "=" : "!=", is.total_reported,
             is.cleaned ? "removed" : "NOT removed");
    printf("      %s\n", row[3].detail);

    /* FR5 */
    printf("[5/8] FR5 Banker's safety algorithm\n");
    BankerState bs;
    BankerResult br_safe, br_unsafe;
    memset(&br_safe, 0, sizeof br_safe);
    memset(&br_unsafe, 0, sizeof br_unsafe);
    int safe_ok = 0, unsafe_ok = 0;
    if (banker_load(BANKER_SAFE, &bs) == 0) {
        if (full) banker_print_state(&bs);
        safe_ok = banker_check(&bs, full, &br_safe) == 1;
    }
    if (banker_load(BANKER_UNSAFE, &bs) == 0) {
        if (full) banker_print_state(&bs);
        unsafe_ok = banker_check(&bs, full, &br_unsafe) == 0;
    }
    char seq[64] = "";
    size_t sl = 0;
    for (int k = 0; k < br_safe.seq_len; k++)
        sl += (size_t)snprintf(seq + sl, sizeof seq - sl, "%sP%d", k ? "," : "", br_safe.sequence[k]);
    row[4] = (DemoRow){ "FR5", "Banker's algorithm", safe_ok && unsafe_ok, "" };
    char blk[64] = "";
    size_t bl = 0;
    for (int k = 0; k < br_unsafe.nblocked; k++)
        bl += (size_t)snprintf(blk + bl, sizeof blk - bl, "%sP%d", k ? " " : "", br_unsafe.blocked[k]);
    snprintf(row[4].detail, sizeof row[4].detail, "%s <%s>; %s (%s blocked)",
             safe_ok ? "SAFE" : "safe file ??", seq, unsafe_ok ? "UNSAFE" : "unsafe file ??", blk);
    printf("      %s\n", row[4].detail);

    /* FR6 */
    printf("[6/8] FR6 Worker processes (normal run, then simulated failure)\n");
    WorkerStats w1, w2;
    int w1rc = workers_run(3, FAIL_NONE, 0, &w1);
    int w2rc = workers_run(3, FAIL_EXIT, 2, &w2);
    row[5] = (DemoRow){ "FR6", "Worker failure detect.", w1rc == 0 && w2rc == 0, "" };
    snprintf(row[5].detail, sizeof row[5].detail,
             "%d/%d normal; worker 2 exit(%d) %s [SIMULATED]",
             w1.normal, w1.total, SIMULATED_EXIT_CODE, w2.simulated_detected ? "detected" : "MISSED");
    printf("      %s\n", row[5].detail);

    /* FR7 */
    printf("[7/8] FR7 Resource limits\n");
    RlimitStats rs;
    int rrc = rlimit_run(RL_TEST_ALL, &rs);
    row[6] = (DemoRow){ "FR7", "Resource limits", rrc == 0, "" };
    snprintf(row[6].detail, sizeof row[6].detail,
             "CPU %ds -> %s at %.2fs; NOFILE %d -> %s after %d opens",
             RL_CPU_SOFT_SECONDS, rs.cpu_enforced ? strsignal_abbrev(rs.cpu_signal) : "no signal", rs.cpu_used,
             RL_NOFILE_SOFT, rs.nofile_enforced ? "EMFILE" : "no EMFILE", rs.nofile_opened);
    printf("      %s\n", row[6].detail);

    /* FR8: exercise the validators with known-bad input */
    printf("[8/8] FR8 Input validation / error handling\n");
    Job bad[MAX_JOBS];
    int nb = 0, qv = 0;
    int bad_file   = jobs_load(INVALID_JOBS, bad, MAX_JOBS, &nb) > 0;
    int bad_q0     = parse_int_range("0", 1, MAX_QUANTUM, &qv) != 0;
    int bad_qtext  = parse_int_range("abc", 1, MAX_QUANTUM, &qv) != 0;
    BankerState tmp;
    int bad_banker = banker_load(INVALID_JOBS, &tmp) != 0;
    row[7] = (DemoRow){ "FR8", "Error handling", bad_file && bad_q0 && bad_qtext && bad_banker, "" };
    snprintf(row[7].detail, sizeof row[7].detail, "bad job file, -q 0, -q abc, bad banker file: %s",
             row[7].pass ? "all rejected" : "NOT all rejected");
    printf("      %s\n", row[7].detail);

    /* Cleanup verification */
    g_brief = 1;
    int leftover = ipc_list_objects("");
    g_brief = !full;
    errno = 0;
    int no_children = (waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);
    int threads = count_threads();
    row[8] = (DemoRow){ "ALL", "Cleanup verification", leftover == 0 && no_children && threads == 1, "" };
    snprintf(row[8].detail, sizeof row[8].detail,
             "IPC objects left=%d, zombies=%s, threads alive=%d",
             leftover < 0 ? 0 : leftover, no_children ? "0" : "SOME", threads);

    g_brief = 0;
    int passed = 0;
    printf("\n=============================== DEMO SUMMARY ===============================\n");
    printf("  %-4s %-22s %-6s %s\n", "Req", "Feature", "Result", "Evidence");
    for (int k = 0; k < 9; k++) {
        printf("  %-4s %-22s %-6s %s\n", row[k].fr, row[k].what, row[k].pass ? "PASS" : "FAIL", row[k].detail);
        passed += row[k].pass;
    }
    printf("  --------------------------------------------------------------------------\n");
    printf("  %d/9 checks passed in %.1f s | events logged to %s\n", passed, mono_seconds() - t0, LOG_FILE);
    printf("  (worker 2's failure above was SIMULATED on purpose to test detection)\n");
    applog("DEMO", "integrated demonstration finished: %d/9 checks passed", passed);
    return passed == 9 ? EXIT_OK : EXIT_RUNTIME;
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
    { "banker",   cmd_banker   },
    { "workers",  cmd_workers  },
    { "rlimit",   cmd_rlimit   },
    { "demo",     cmd_demo     },
    { "cleanup",  cmd_cleanup  },
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
