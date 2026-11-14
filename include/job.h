/*
 * job.h - FR1: job representation, validation and lifecycle status
 */
#ifndef CC_JOB_H
#define CC_JOB_H

#define MAX_JOBS 64

/* Field limits enforced by the job-file validator */
#define JOB_ID_MAX       9999
#define JOB_ARRIVAL_MAX  100000
#define JOB_BURST_MAX    10000
#define JOB_PRIO_MAX     99

/* Lifecycle: NEW -> READY (arrived) -> RUNNING -> READY (preempted) ... -> COMPLETED */
typedef enum {
    JOB_NEW = 0,
    JOB_READY,
    JOB_RUNNING,
    JOB_COMPLETED
} JobStatus;

typedef struct {
    int id;          /* unique job ID (1..9999)                         */
    int arrival;     /* arrival time (time units, >= 0)                  */
    int burst;       /* total CPU burst required (> 0)                   */
    int priority;    /* 0..99, LOWER number = HIGHER priority            */

    /* filled in by the scheduler */
    int remaining;   /* CPU time still needed                            */
    int start;       /* first time the job got the CPU (-1 = never)      */
    int completion;  /* completion time (CT)                             */
    int turnaround;  /* TAT = CT - arrival                               */
    int waiting;     /* WT  = TAT - burst                                */
    int response;    /* RT  = first start - arrival                      */
    JobStatus status;
} Job;

const char *job_status_name(JobStatus s);

/*
 * Load and validate a job file ("id arrival burst priority" per line,
 * '#' starts a comment). Every malformed line is reported with its line
 * number. Returns 0 on success, -1 if the file cannot be opened, or the
 * number of invalid lines (> 0) - in that case no jobs are returned.
 */
int  jobs_load(const char *path, Job *jobs, int max, int *count);

/* Reset scheduler-owned fields so the same job set can be re-scheduled. */
void jobs_reset(Job *jobs, int n);

/* Print the job table (ID, arrival, burst, priority, status). */
void jobs_print(const Job *jobs, int n);

#endif /* CC_JOB_H */
