/*
 * scheduler.h - FR2: FCFS, Round Robin and non-preemptive Priority scheduling
 */
#ifndef CC_SCHEDULER_H
#define CC_SCHEDULER_H

#include "job.h"

#define DEFAULT_QUANTUM 2
#define MAX_QUANTUM     1000

typedef enum { POL_FCFS = 0, POL_RR, POL_PRIORITY } Policy;

/* One block of the Gantt chart; job_id == -1 means the CPU was idle. */
typedef struct {
    int job_id;
    int start;
    int end;
} Segment;

/* Lifecycle event used by the -v trace (NEW->READY, READY->RUNNING, ...). */
typedef struct {
    int time;
    int order;      /* ordering of events that happen at the same time */
    int seq;        /* insertion order, keeps the sort stable          */
    int job_id;
    JobStatus from;
    JobStatus to;
    int left;       /* remaining burst after a preemption              */
} SchedEvent;

typedef struct {
    Policy  policy;
    int     quantum;          /* RR only */
    int     verbose;          /* print lifecycle trace */

    Job     jobs[MAX_JOBS];   /* private copy, sorted by job ID after the run */
    int     n;

    Segment *seg;   int nseg, seg_cap;
    SchedEvent *ev; int nev,  ev_cap;

    double  avg_tat, avg_wt, avg_rt;
    int     makespan;         /* completion time of the last job   */
    int     idle_time;        /* total time the CPU was idle        */
    int     dispatches;       /* number of (non-idle) Gantt blocks  */
    int     all_completed;    /* 1 if every job reached COMPLETED   */
} SchedResult;

const char *policy_name(Policy p);
const char *policy_short(Policy p);

/* Run one policy on a job set. Returns 0 on success, -1 on allocation failure. */
int  schedule_run(Policy p, const Job *in, int n, int quantum, int verbose, SchedResult *r);
void schedule_free(SchedResult *r);

/* Print rules, Gantt chart, optional lifecycle trace, metrics table and averages. */
void schedule_print(const SchedResult *r);

/* Side-by-side comparison of several results computed on the same job set. */
void schedule_print_compare(const SchedResult *res, int count);

#endif /* CC_SCHEDULER_H */
