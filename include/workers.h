/*
 * workers.h - FR6: worker processes with fork() and failure detection via waitpid()
 */
#ifndef CC_WORKERS_H
#define CC_WORKERS_H

#define WORKERS_DEFAULT       3
#define WORKERS_MAX           16
#define SIMULATED_EXIT_CODE   3     /* exit status used by an injected failure */

typedef enum {
    FAIL_NONE = 0,
    FAIL_EXIT,        /* worker calls exit(3)        -> parent sees WIFEXITED, status 3 */
    FAIL_SIGNAL       /* worker calls abort()        -> parent sees WIFSIGNALED, SIGABRT */
} FailMode;

typedef struct {
    int total;
    int normal;              /* exited with status 0                          */
    int failed;              /* non-zero exit or killed by a signal            */
    int simulated_detected;  /* failures that match the injected one           */
    int unexpected;          /* failures we did NOT inject (real problems)     */
    int unreaped;            /* children still not collected after the run     */
} WorkerStats;

/*
 * Fork n workers. If mode != FAIL_NONE, worker number fail_index (1-based)
 * performs a clearly labelled SIMULATED failure. Returns 0 if every outcome
 * was detected as expected, -1 otherwise.
 */
int workers_run(int n, FailMode mode, int fail_index, WorkerStats *st);

#endif /* CC_WORKERS_H */
