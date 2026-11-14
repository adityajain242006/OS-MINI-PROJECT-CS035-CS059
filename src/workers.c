/*
 * workers.c - FR6: worker failure detection
 *
 * The parent forks N worker processes. Each worker does real CPU work
 * (counting primes) and exits with status 0. Optionally one worker performs
 * a SIMULATED failure - either exit(3) or abort() - which is announced as
 * simulated in the output; it is not a real OS fault.
 *
 * The parent collects every child with waitpid(-1, &status, 0) and decodes
 * the status with WIFEXITED/WEXITSTATUS and WIFSIGNALED/WTERMSIG, so it can
 * tell normal completion, a non-zero exit and death-by-signal apart.
 * Afterwards waitpid(-1, ..., WNOHANG) == -1/ECHILD proves no zombie is left.
 */
#include "workers.h"
#include "common.h"
#include "logger.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

/* Real work for a worker: count primes <= limit by trial division. */
static int count_primes(int limit)
{
    int count = 0;
    for (int x = 2; x <= limit; x++) {
        int prime = 1;
        for (int d = 2; d * d <= x; d++) {
            if (x % d == 0) {
                prime = 0;
                break;
            }
        }
        count += prime;
    }
    return count;
}

static void worker_process(int index, FailMode mode, int fail_index)
{
    pid_t me = getpid();
    int limit = 20000 * index;
    OUT("[worker %d pid=%d] started: counting primes <= %d\n", index, (int)me, limit);

    if (mode != FAIL_NONE && index == fail_index) {
        int partial = count_primes(limit / 2);      /* do some work first */
        if (mode == FAIL_EXIT) {
            OUT("[worker %d pid=%d] [SIMULATED FAILURE] intentionally calling exit(%d) after %d primes\n"
                "                    (injected by --fail for testing - NOT a real OS fault)\n",
                index, (int)me, SIMULATED_EXIT_CODE, partial);
            child_exit(SIMULATED_EXIT_CODE);
        }
        /* FAIL_SIGNAL: disable core files so the simulated crash leaves nothing behind */
        struct rlimit nocore = { 0, 0 };
        setrlimit(RLIMIT_CORE, &nocore);
        OUT("[worker %d pid=%d] [SIMULATED FAILURE] intentionally calling abort() after %d primes\n"
            "                    (injected by --crash for testing - NOT a real OS fault)\n",
            index, (int)me, partial);
        fflush(NULL);
        abort();
    }

    int primes = count_primes(limit);
    OUT("[worker %d pid=%d] finished: %d primes found -> exit(0)\n", index, (int)me, primes);
    child_exit(0);
}

int workers_run(int n, FailMode mode, int fail_index, WorkerStats *st)
{
    memset(st, 0, sizeof *st);
    pid_t pids[WORKERS_MAX];
    int forked = 0, rc = 0;

    if (mode == FAIL_EXIT)
        OUT("Mode: failure injection - worker %d will SIMULATE a failure with exit(%d)\n\n", fail_index, SIMULATED_EXIT_CODE);
    else if (mode == FAIL_SIGNAL)
        OUT("Mode: failure injection - worker %d will SIMULATE a crash with abort() (SIGABRT)\n\n", fail_index);
    else
        OUT("Mode: normal - all %d workers should complete successfully\n\n", n);

    for (int i = 1; i <= n; i++) {
        fflush(stdout);                             /* avoid duplicated buffers in children */
        pid_t pid = fork();
        if (pid == -1) {
            ERR("fork for worker %d failed: %s\n", i, strerror(errno));
            rc = -1;
            break;
        }
        if (pid == 0)
            worker_process(i, mode, fail_index);    /* never returns */
        pids[forked++] = pid;
        OUT("[parent pid=%d] fork() -> worker %d is pid %d\n", (int)getpid(), i, (int)pid);
    }
    st->total = forked;

    /* collect children in the order they finish */
    for (int left = forked; left > 0; left--) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, 0);
        if (pid == -1) {
            if (errno == EINTR) {
                left++;
                continue;
            }
            ERR("waitpid: %s\n", strerror(errno));
            rc = -1;
            break;
        }
        int idx = 0;
        for (int k = 0; k < forked; k++)
            if (pids[k] == pid)
                idx = k + 1;
        int injected = (mode != FAIL_NONE && idx == fail_index);

        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            st->normal++;
            OUT("[parent] waitpid -> worker %d pid=%d: exited normally, status 0 -> COMPLETED\n", idx, (int)pid);
            applog("WORKER", "worker %d pid=%d exited normally (status 0)", idx, (int)pid);
        } else if (WIFEXITED(status)) {
            st->failed++;
            OUT("[parent] waitpid -> worker %d pid=%d: exited with status %d -> FAILURE DETECTED%s\n",
                idx, (int)pid, WEXITSTATUS(status), injected ? " (simulated)" : " (UNEXPECTED)");
            applog("WORKER", "worker %d pid=%d FAILED: exit status %d%s", idx, (int)pid,
                   WEXITSTATUS(status), injected ? " [SIMULATED]" : "");
            if (injected && mode == FAIL_EXIT && WEXITSTATUS(status) == SIMULATED_EXIT_CODE)
                st->simulated_detected++;
            else
                st->unexpected++;
        } else if (WIFSIGNALED(status)) {
            st->failed++;
            int sig = WTERMSIG(status);
            OUT("[parent] waitpid -> worker %d pid=%d: killed by signal %d (%s) -> FAILURE DETECTED%s\n",
                idx, (int)pid, sig, strsignal(sig), injected ? " (simulated)" : " (UNEXPECTED)");
            applog("WORKER", "worker %d pid=%d FAILED: killed by signal %d (%s)%s", idx, (int)pid,
                   sig, strsignal(sig), injected ? " [SIMULATED]" : "");
            if (injected && mode == FAIL_SIGNAL && sig == SIGABRT)
                st->simulated_detected++;
            else
                st->unexpected++;
        }
    }

    /* prove that no child is left unreaped (no zombies) */
    errno = 0;
    pid_t extra = waitpid(-1, NULL, WNOHANG);
    st->unreaped = (extra == -1 && errno == ECHILD) ? 0 : 1;

    int expected_failures = (mode == FAIL_NONE) ? 0 : 1;
    OUT("\nSummary: %d worker(s) | %d completed normally | %d failed (%d simulated, %d unexpected)\n",
        st->total, st->normal, st->failed, st->simulated_detected, st->unexpected);
    OUT("Reaping check: waitpid(-1, WNOHANG) -> %s\n",
        st->unreaped ? "children still pending!" : "ECHILD (all children collected, no zombies)");

    if (rc == 0 && (st->unexpected != 0 || st->simulated_detected != expected_failures ||
                    st->normal != n - expected_failures || st->unreaped))
        rc = -1;
    OUT("Detection result: %s\n", rc == 0 ? "every outcome reported correctly" : "UNEXPECTED outcome");
    return rc;
}
