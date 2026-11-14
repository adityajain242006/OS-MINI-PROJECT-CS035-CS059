/*
 * rlimit_demo.h - FR7: per-process resource limits with setrlimit()
 */
#ifndef CC_RLIMIT_DEMO_H
#define CC_RLIMIT_DEMO_H

#define RL_CPU_SOFT_SECONDS  1     /* RLIMIT_CPU soft limit -> SIGXCPU          */
#define RL_CPU_HARD_SECONDS  2     /* RLIMIT_CPU hard limit -> SIGKILL          */
#define RL_CPU_GUARD_SECONDS 6     /* wall-clock guard if the limit is ignored  */
#define RL_NOFILE_SOFT       8     /* RLIMIT_NOFILE soft limit -> EMFILE        */

enum { RL_TEST_CPU = 1, RL_TEST_NOFILE = 2, RL_TEST_ALL = 3 };

typedef struct {
    int cpu_tested, cpu_enforced, cpu_signal;
    double cpu_used;               /* child CPU seconds (from wait4 rusage) */
    int nofile_tested, nofile_enforced, nofile_opened, nofile_preopen;
} RlimitStats;

/* Run the selected demonstrations in child processes. Returns 0 if every
 * selected limit was observed to take effect, -1 otherwise. */
int rlimit_run(int which, RlimitStats *st);

#endif /* CC_RLIMIT_DEMO_H */
