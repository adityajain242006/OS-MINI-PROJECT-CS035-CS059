/*
 * rlimit_demo.c - FR7: resource limits and their observed effect
 *
 * Each limit is applied inside a forked CHILD, so the main program is never
 * restricted (the parent's own limits are printed to prove this).
 *
 * 1) RLIMIT_CPU  soft=1 s, hard=2 s. The child runs a CPU-bound loop. When it
 *    has used 1 s of CPU time the kernel sends SIGXCPU, whose default action
 *    terminates the process. The parent collects it with wait4(), which is
 *    waitpid() plus resource usage, and reports the signal and CPU time used.
 *    RLIMIT_CORE is set to 0 in the child so no core file is produced.
 *
 * 2) RLIMIT_NOFILE soft=8. The child opens /dev/null repeatedly; once 8
 *    descriptors are in use open() fails with EMFILE. The child reports the
 *    number of successful opens through its exit status (documented below).
 */
#include "rlimit_demo.h"
#include "common.h"
#include "logger.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

/* exit codes used by the NOFILE child */
#define NOFILE_NOT_ENFORCED 250
#define NOFILE_SETUP_ERROR  251

static void fmt_lim(rlim_t v, char *buf, size_t n)
{
    if (v == RLIM_INFINITY)
        snprintf(buf, n, "unlimited");
    else
        snprintf(buf, n, "%llu", (unsigned long long)v);
}

static void print_limit(const char *who, const char *name, int resource)
{
    struct rlimit rl;
    char s[32], h[32];
    if (getrlimit(resource, &rl) == -1) {
        OUT("  %s %s: getrlimit failed: %s\n", who, name, strerror(errno));
        return;
    }
    fmt_lim(rl.rlim_cur, s, sizeof s);
    fmt_lim(rl.rlim_max, h, sizeof h);
    OUT("  %s %-13s soft=%-10s hard=%s\n", who, name, s, h);
}

/* ------------------------------ RLIMIT_CPU ------------------------------ */

static void cpu_child(void)
{
    struct rlimit nocore = { 0, 0 };
    setrlimit(RLIMIT_CORE, &nocore);              /* no core file on SIGXCPU */

    print_limit("[child] before:", "RLIMIT_CPU", RLIMIT_CPU);
    struct rlimit cur, lim;
    getrlimit(RLIMIT_CPU, &cur);
    lim.rlim_cur = RL_CPU_SOFT_SECONDS;
    lim.rlim_max = RL_CPU_HARD_SECONDS;
    if (cur.rlim_max != RLIM_INFINITY && cur.rlim_max < lim.rlim_max)
        lim.rlim_max = cur.rlim_max;              /* cannot raise the hard limit */
    if (setrlimit(RLIMIT_CPU, &lim) == -1) {
        fprintf(stderr, "error: [child] setrlimit(RLIMIT_CPU): %s\n", strerror(errno));
        child_exit(2);
    }
    print_limit("[child] after: ", "RLIMIT_CPU", RLIMIT_CPU);
    OUT("  [child pid=%d] starting CPU-bound loop (no sleeping)...\n", (int)getpid());
    fflush(stdout);

    /* Busy loop. If the kernel never enforces the limit, give up after a
     * wall-clock guard so the demo cannot hang. */
    double t0 = mono_seconds();
    volatile unsigned long long x = 0;
    for (;;) {
        for (int i = 0; i < 5000000; i++)
            x += (unsigned long long)i ^ (x >> 3);
        if (mono_seconds() - t0 > RL_CPU_GUARD_SECONDS) {
            OUT("  [child] %d s passed without SIGXCPU - limit not enforced here\n", RL_CPU_GUARD_SECONDS);
            child_exit(99);
        }
    }
}

static int demo_cpu(RlimitStats *st)
{
    st->cpu_tested = 1;
    OUT("--- Demo 1: RLIMIT_CPU (CPU-time limit) ---\n");
    print_limit("[parent]       ", "RLIMIT_CPU", RLIMIT_CPU);

    double t0 = mono_seconds();
    fflush(stdout);
    pid_t pid = fork();
    if (pid == -1) {
        ERR("fork: %s\n", strerror(errno));
        return -1;
    }
    if (pid == 0)
        cpu_child();

    int status = 0;
    struct rusage ru;
    if (wait4(pid, &status, 0, &ru) == -1) {
        ERR("wait4: %s\n", strerror(errno));
        return -1;
    }
    double wall = mono_seconds() - t0;
    st->cpu_used = (double)ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 +
                   (double)ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;

    if (WIFSIGNALED(status)) {
        st->cpu_signal = WTERMSIG(status);
        st->cpu_enforced = (st->cpu_signal == SIGXCPU || st->cpu_signal == SIGKILL);
        OUT("  [parent] wait4 -> child pid=%d killed by signal %d (%s)\n",
            (int)pid, st->cpu_signal, strsignal(st->cpu_signal));
    } else if (WIFEXITED(status)) {
        OUT("  [parent] wait4 -> child pid=%d exited with status %d\n", (int)pid, WEXITSTATUS(status));
    }
    OUT("  [parent] child CPU time used = %.2f s, wall time = %.2f s\n", st->cpu_used, wall);
    print_limit("[parent] still:", "RLIMIT_CPU", RLIMIT_CPU);

    if (st->cpu_enforced)
        OUT("  Observed effect: once the child had used ~%d s of CPU time the kernel sent %s,\n"
            "  which terminated it. The parent's own limit is unchanged, so only the child was restricted.\n\n",
            RL_CPU_SOFT_SECONDS, st->cpu_signal == SIGXCPU ? "SIGXCPU" : "SIGKILL (hard limit)");
    else
        OUT("  Observed effect: the CPU limit was NOT enforced on this system.\n\n");

    applog("RLIMIT", "RLIMIT_CPU soft=%ds: child pid=%d %s after %.2fs CPU",
           RL_CPU_SOFT_SECONDS, (int)pid, st->cpu_enforced ? "terminated by signal" : "not limited",
           st->cpu_used);
    return st->cpu_enforced ? 0 : -1;
}

/* ---------------------------- RLIMIT_NOFILE ----------------------------- */

/* Child exit status: number of files opened before EMFILE (0..RL_NOFILE_SOFT),
 * NOFILE_NOT_ENFORCED or NOFILE_SETUP_ERROR. */
static void nofile_child(void)
{
    int preopen = count_open_fds();
    print_limit("[child] before:", "RLIMIT_NOFILE", RLIMIT_NOFILE);
    struct rlimit cur, lim;
    getrlimit(RLIMIT_NOFILE, &cur);
    lim.rlim_cur = RL_NOFILE_SOFT;
    lim.rlim_max = cur.rlim_max;                  /* lower only the soft limit */
    if (cur.rlim_max != RLIM_INFINITY && cur.rlim_max < lim.rlim_cur)
        lim.rlim_cur = cur.rlim_max;
    if (setrlimit(RLIMIT_NOFILE, &lim) == -1) {
        fprintf(stderr, "error: [child] setrlimit(RLIMIT_NOFILE): %s\n", strerror(errno));
        child_exit(NOFILE_SETUP_ERROR);
    }
    print_limit("[child] after: ", "RLIMIT_NOFILE", RLIMIT_NOFILE);
    OUT("  [child pid=%d] %d descriptors already open (stdin/stdout/stderr...); opening /dev/null repeatedly\n",
        (int)getpid(), preopen);

    int fds[64], opened = 0, err = 0;
    for (;;) {
        int fd = open("/dev/null", O_RDONLY);
        if (fd == -1) {
            err = errno;
            OUT("  [child] open #%d FAILED: %s (%s)\n", opened + 1,
                err == EMFILE ? "EMFILE" : "error", strerror(err));
            break;
        }
        OUT("  [child] open #%d -> fd %d\n", opened + 1, fd);
        if (opened < 64)
            fds[opened] = fd;
        opened++;
        if (opened >= 64)
            break;                                 /* limit clearly not enforced */
    }
    for (int i = 0; i < opened && i < 64; i++)
        close(fds[i]);
    OUT("  [child] closed the %d test descriptor(s)\n", opened);
    child_exit(err == EMFILE ? opened : NOFILE_NOT_ENFORCED);
}

static int demo_nofile(RlimitStats *st)
{
    st->nofile_tested = 1;
    OUT("--- Demo 2: RLIMIT_NOFILE (open-file limit) ---\n");
    print_limit("[parent]       ", "RLIMIT_NOFILE", RLIMIT_NOFILE);

    fflush(stdout);
    pid_t pid = fork();
    if (pid == -1) {
        ERR("fork: %s\n", strerror(errno));
        return -1;
    }
    if (pid == 0)
        nofile_child();

    int status = 0;
    if (waitpid(pid, &status, 0) == -1) {
        ERR("waitpid: %s\n", strerror(errno));
        return -1;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) <= RL_NOFILE_SOFT) {
        st->nofile_enforced = 1;
        st->nofile_opened = WEXITSTATUS(status);
        st->nofile_preopen = RL_NOFILE_SOFT - st->nofile_opened;
        OUT("  [parent] waitpid -> child pid=%d reported %d successful open() call(s) before EMFILE\n",
            (int)pid, st->nofile_opened);
        print_limit("[parent] still:", "RLIMIT_NOFILE", RLIMIT_NOFILE);
        OUT("  Observed effect: with soft limit %d and %d descriptor(s) already in use, only %d more file(s)\n"
            "  could be opened; the next open() failed with EMFILE (Too many open files).\n\n",
            RL_NOFILE_SOFT, st->nofile_preopen, st->nofile_opened);
    } else {
        OUT("  [parent] child status 0x%x - open-file limit not observed\n\n", status);
    }
    applog("RLIMIT", "RLIMIT_NOFILE soft=%d: child pid=%d %s after %d open(s)",
           RL_NOFILE_SOFT, (int)pid, st->nofile_enforced ? "got EMFILE" : "not limited", st->nofile_opened);
    return st->nofile_enforced ? 0 : -1;
}

int rlimit_run(int which, RlimitStats *st)
{
    memset(st, 0, sizeof *st);
    int rc = 0;
    if (which & RL_TEST_CPU)
        rc |= demo_cpu(st);
    if (which & RL_TEST_NOFILE)
        rc |= demo_nofile(st);
    OUT("Limits were applied only inside child processes; both children have been reaped.\n");
    return rc ? -1 : 0;
}
