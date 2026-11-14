/*
 * scheduler.c - FR2: CPU scheduling algorithms and metrics
 *
 * Common definitions
 *   CT  = completion time
 *   TAT = CT - arrival          (turnaround time)
 *   WT  = TAT - burst           (waiting time)
 *   RT  = first start - arrival (response time)
 *
 * Tie-breaking rules (also printed with every result)
 *   FCFS     : earlier arrival first; equal arrival -> lower job ID first.
 *   RR       : ready queue is FIFO; jobs that arrive during a time slice are
 *              enqueued BEFORE the preempted job; equal arrival -> lower ID.
 *   Priority : non-preemptive, LOWER number = HIGHER priority;
 *              equal priority -> earlier arrival -> lower job ID.
 * If no job has arrived the CPU is shown as "idle" until the next arrival.
 */
#include "scheduler.h"
#include "common.h"
#include "logger.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

const char *policy_name(Policy p)
{
    switch (p) {
    case POL_FCFS:     return "FCFS (First-Come, First-Served)";
    case POL_RR:       return "Round Robin";
    case POL_PRIORITY: return "Priority Scheduling (non-preemptive)";
    }
    return "?";
}

const char *policy_short(Policy p)
{
    switch (p) {
    case POL_FCFS:     return "FCFS";
    case POL_RR:       return "RR";
    case POL_PRIORITY: return "Priority (NP)";
    }
    return "?";
}

/* ---------- helpers that record the timeline and lifecycle events ---------- */

static int add_segment(SchedResult *r, int job_id, int start, int end)
{
    if (end <= start)
        return 0;
    /* merge with previous block if the same job simply keeps running */
    if (r->nseg > 0) {
        Segment *last = &r->seg[r->nseg - 1];
        if (last->job_id == job_id && last->end == start) {
            last->end = end;
            return 0;
        }
    }
    if (r->nseg == r->seg_cap) {
        int cap = r->seg_cap ? r->seg_cap * 2 : 32;
        Segment *p = realloc(r->seg, (size_t)cap * sizeof *p);
        if (p == NULL)
            return -1;
        r->seg = p;
        r->seg_cap = cap;
    }
    r->seg[r->nseg++] = (Segment){ job_id, start, end };
    return 0;
}

enum { ORD_ARRIVE = 0, ORD_LEAVE = 1, ORD_DISPATCH = 2 };

static int add_event(SchedResult *r, int time, int order, int job_id,
                     JobStatus from, JobStatus to, int left)
{
    if (r->nev == r->ev_cap) {
        int cap = r->ev_cap ? r->ev_cap * 2 : 64;
        SchedEvent *p = realloc(r->ev, (size_t)cap * sizeof *p);
        if (p == NULL)
            return -1;
        r->ev = p;
        r->ev_cap = cap;
    }
    r->ev[r->nev] = (SchedEvent){ time, order, r->nev, job_id, from, to, left };
    r->nev++;
    return 0;
}

static int dispatch(SchedResult *r, Job *j, int t)
{
    if (j->start < 0)
        j->start = t;
    int rc = add_event(r, t, ORD_DISPATCH, j->id, j->status, JOB_RUNNING, j->remaining);
    j->status = JOB_RUNNING;
    return rc;
}

static int complete(SchedResult *r, Job *j, int t)
{
    j->completion = t;
    j->turnaround = j->completion - j->arrival;
    j->waiting    = j->turnaround - j->burst;
    j->response   = j->start - j->arrival;
    int rc = add_event(r, t, ORD_LEAVE, j->id, JOB_RUNNING, JOB_COMPLETED, 0);
    j->status = JOB_COMPLETED;
    return rc;
}

static int cmp_arrival_id(const void *a, const void *b)
{
    const Job *x = a, *y = b;
    if (x->arrival != y->arrival)
        return x->arrival - y->arrival;
    return x->id - y->id;
}

static int cmp_id(const void *a, const void *b)
{
    const Job *x = a, *y = b;
    return x->id - y->id;
}

static int cmp_event(const void *a, const void *b)
{
    const SchedEvent *x = a, *y = b;
    if (x->time != y->time)   return x->time - y->time;
    if (x->order != y->order) return x->order - y->order;
    return x->seq - y->seq;
}

/* ---------------------------- the algorithms ---------------------------- */

/* FCFS: run jobs to completion in (arrival, id) order. */
static int run_fcfs(SchedResult *r)
{
    int t = 0, rc = 0;
    for (int i = 0; i < r->n; i++) {
        Job *j = &r->jobs[i];
        if (t < j->arrival) {                      /* CPU idle until it arrives */
            rc |= add_segment(r, -1, t, j->arrival);
            t = j->arrival;
        }
        rc |= dispatch(r, j, t);
        rc |= add_segment(r, j->id, t, t + j->burst);
        t += j->burst;
        j->remaining = 0;
        rc |= complete(r, j, t);
    }
    return rc;
}

/* Is a "better" than b for non-preemptive priority? (lower number wins) */
static int prio_better(const Job *a, const Job *b)
{
    if (a->priority != b->priority) return a->priority < b->priority;
    if (a->arrival  != b->arrival)  return a->arrival  < b->arrival;
    return a->id < b->id;
}

/* Non-preemptive priority: when the CPU frees up, pick the best ready job. */
static int run_priority(SchedResult *r)
{
    int t = 0, done = 0, rc = 0;
    while (done < r->n) {
        Job *best = NULL;
        for (int i = 0; i < r->n; i++) {
            Job *j = &r->jobs[i];
            if (j->status == JOB_COMPLETED || j->arrival > t)
                continue;
            if (best == NULL || prio_better(j, best))
                best = j;
        }
        if (best == NULL) {                        /* nobody ready: idle */
            int next = INT_MAX;
            for (int i = 0; i < r->n; i++)
                if (r->jobs[i].status != JOB_COMPLETED && r->jobs[i].arrival < next)
                    next = r->jobs[i].arrival;
            rc |= add_segment(r, -1, t, next);
            t = next;
            continue;
        }
        rc |= dispatch(r, best, t);
        rc |= add_segment(r, best->id, t, t + best->burst);
        t += best->burst;
        best->remaining = 0;
        rc |= complete(r, best, t);
        done++;
    }
    return rc;
}

/* Round Robin with time quantum q and a circular FIFO ready queue. */
static int run_rr(SchedResult *r, int q)
{
    int queue[MAX_JOBS];
    int head = 0, qlen = 0;
    int next = 0;            /* next job (in arrival order) not yet admitted */
    int t = 0, done = 0, rc = 0;
    const int n = r->n;

#define ENQUEUE(idx) do { queue[(head + qlen) % MAX_JOBS] = (idx); qlen++; } while (0)

    while (next < n && r->jobs[next].arrival <= t)
        ENQUEUE(next++);

    while (done < n) {
        if (qlen == 0) {                           /* CPU idle until next arrival */
            int a = r->jobs[next].arrival;
            rc |= add_segment(r, -1, t, a);
            t = a;
            while (next < n && r->jobs[next].arrival <= t)
                ENQUEUE(next++);
            continue;
        }
        int k = queue[head];
        head = (head + 1) % MAX_JOBS;
        qlen--;

        Job *j = &r->jobs[k];
        int run = j->remaining < q ? j->remaining : q;
        rc |= dispatch(r, j, t);
        rc |= add_segment(r, j->id, t, t + run);
        t += run;
        j->remaining -= run;

        /* Arrivals during this slice join the queue BEFORE the preempted job. */
        while (next < n && r->jobs[next].arrival <= t)
            ENQUEUE(next++);

        if (j->remaining > 0) {
            rc |= add_event(r, t, ORD_LEAVE, j->id, JOB_RUNNING, JOB_READY, j->remaining);
            j->status = JOB_READY;
            ENQUEUE(k);
        } else {
            rc |= complete(r, j, t);
            done++;
        }
    }
#undef ENQUEUE
    return rc;
}

int schedule_run(Policy p, const Job *in, int n, int quantum, int verbose, SchedResult *r)
{
    memset(r, 0, sizeof *r);
    r->policy  = p;
    r->quantum = quantum;
    r->verbose = verbose;
    r->n       = n;
    memcpy(r->jobs, in, (size_t)n * sizeof *in);
    jobs_reset(r->jobs, n);
    qsort(r->jobs, (size_t)n, sizeof r->jobs[0], cmp_arrival_id);

    int rc = 0;
    /* NEW -> READY happens at each job's arrival time */
    for (int i = 0; i < n; i++)
        rc |= add_event(r, r->jobs[i].arrival, ORD_ARRIVE, r->jobs[i].id, JOB_NEW, JOB_READY, r->jobs[i].burst);
    for (int i = 0; i < n; i++)
        r->jobs[i].status = JOB_READY;

    switch (p) {
    case POL_FCFS:     rc |= run_fcfs(r);         break;
    case POL_RR:       rc |= run_rr(r, quantum);  break;
    case POL_PRIORITY: rc |= run_priority(r);     break;
    }
    if (rc != 0) {
        ERR("out of memory while scheduling\n");
        schedule_free(r);
        return -1;
    }

    qsort(r->ev, (size_t)r->nev, sizeof r->ev[0], cmp_event);
    qsort(r->jobs, (size_t)n, sizeof r->jobs[0], cmp_id);

    long sum_tat = 0, sum_wt = 0, sum_rt = 0;
    r->all_completed = 1;
    for (int i = 0; i < n; i++) {
        sum_tat += r->jobs[i].turnaround;
        sum_wt  += r->jobs[i].waiting;
        sum_rt  += r->jobs[i].response;
        if (r->jobs[i].status != JOB_COMPLETED)
            r->all_completed = 0;
        if (r->jobs[i].completion > r->makespan)
            r->makespan = r->jobs[i].completion;
    }
    r->avg_tat = (double)sum_tat / n;
    r->avg_wt  = (double)sum_wt / n;
    r->avg_rt  = (double)sum_rt / n;
    for (int i = 0; i < r->nseg; i++) {
        if (r->seg[i].job_id < 0)
            r->idle_time += r->seg[i].end - r->seg[i].start;
        else
            r->dispatches++;
    }

    char extra[32] = "";
    if (p == POL_RR)
        snprintf(extra, sizeof extra, " (q=%d)", quantum);
    applog("SCHED", "%s%s on %d jobs: avg TAT=%.2f avg WT=%.2f avg RT=%.2f makespan=%d",
           policy_short(p), extra, n, r->avg_tat, r->avg_wt, r->avg_rt, r->makespan);
    return 0;
}

void schedule_free(SchedResult *r)
{
    free(r->seg);
    free(r->ev);
    r->seg = NULL;
    r->ev = NULL;
    r->nseg = r->seg_cap = r->nev = r->ev_cap = 0;
}

/* ------------------------------- printing ------------------------------- */

static void print_rules(const SchedResult *r)
{
    switch (r->policy) {
    case POL_FCFS:
        OUT("Rule        : jobs run to completion in order of arrival time\n");
        OUT("Tie-break   : equal arrival time -> lower job ID runs first\n");
        break;
    case POL_RR:
        OUT("Time quantum: %d time unit(s)  (change with -q N, default %d)\n", r->quantum, DEFAULT_QUANTUM);
        OUT("Rule        : preempted job goes to the tail of the FIFO ready queue;\n");
        OUT("              jobs arriving during a slice are queued BEFORE the preempted job\n");
        OUT("Tie-break   : equal arrival time -> lower job ID queued first\n");
        break;
    case POL_PRIORITY:
        OUT("Rule        : non-preemptive; when the CPU is free, run the ready job with the highest priority\n");
        OUT("Convention  : LOWER priority number = HIGHER priority\n");
        OUT("Tie-break   : equal priority -> earlier arrival -> lower job ID\n");
        break;
    }
}

/* Gantt chart, wrapped every GANTT_PER_ROW blocks. */
#define GANTT_PER_ROW 10
static void print_gantt(const SchedResult *r)
{
    OUT("Gantt chart :\n");
    for (int base = 0; base < r->nseg; base += GANTT_PER_ROW) {
        char top[512] = "", bot[512] = "";
        size_t tl = 0, bl = 0;
        int last = base + GANTT_PER_ROW < r->nseg ? base + GANTT_PER_ROW : r->nseg;
        for (int i = base; i < last; i++) {
            char label[16], st[16];
            if (r->seg[i].job_id < 0)
                snprintf(label, sizeof label, "idle");
            else
                snprintf(label, sizeof label, "J%d", r->seg[i].job_id);
            snprintf(st, sizeof st, "%d", r->seg[i].start);
            int w = (int)strlen(label);
            if ((int)strlen(st) > w)
                w = (int)strlen(st);
            w += 3;
            tl += (size_t)snprintf(top + tl, sizeof top - tl, "| %-*s", w - 2, label);
            bl += (size_t)snprintf(bot + bl, sizeof bot - bl, "%-*s", w, st);
        }
        snprintf(top + tl, sizeof top - tl, "|");
        snprintf(bot + bl, sizeof bot - bl, "%d", r->seg[last - 1].end);
        OUT("  %s\n  %s\n", top, bot);
    }
}

static void print_trace(const SchedResult *r)
{
    OUT("Lifecycle trace (-v):\n");
    for (int i = 0; i < r->nev; i++) {
        const SchedEvent *e = &r->ev[i];
        const char *note = "";
        char buf[48] = "";
        if (e->from == JOB_NEW)
            note = "  (arrived)";
        else if (e->to == JOB_RUNNING)
            note = "  (dispatched)";
        else if (e->to == JOB_READY) {
            snprintf(buf, sizeof buf, "  (quantum expired, %d left)", e->left);
            note = buf;
        } else if (e->to == JOB_COMPLETED)
            note = "  (finished)";
        char id[16];
        snprintf(id, sizeof id, "J%d", e->job_id);
        OUT("  t=%-4d %-4s %-9s -> %-9s%s\n", e->time, id,
            job_status_name(e->from), job_status_name(e->to), note);
    }
}

void schedule_print(const SchedResult *r)
{
    if (r->policy == POL_RR)
        OUT("\n=== %s (q=%d) ===\n", policy_name(r->policy), r->quantum);
    else
        OUT("\n=== %s ===\n", policy_name(r->policy));
    print_rules(r);

    OUT("Order       : ");
    for (int i = 0; i < r->nseg; i++) {
        if (r->seg[i].job_id < 0)
            OUT("idle");
        else
            OUT("J%d", r->seg[i].job_id);
        if (i + 1 < r->nseg)
            OUT(((i + 1) % 12 == 0) ? " ->\n              " : " -> ");
    }
    OUT("\n");
    print_gantt(r);
    if (r->verbose)
        print_trace(r);

    OUT("Metrics     :\n");
    OUT("  %-5s %4s %4s %4s %5s %5s %5s %5s %5s  %s\n",
        "Job", "AT", "BT", "PR", "ST", "CT", "TAT", "WT", "RT", "Status");
    OUT("  %-5s %4s %4s %4s %5s %5s %5s %5s %5s  %s\n",
        "-----", "----", "----", "----", "-----", "-----", "-----", "-----", "-----", "---------");
    for (int i = 0; i < r->n; i++) {
        const Job *j = &r->jobs[i];
        char id[16];
        snprintf(id, sizeof id, "J%d", j->id);
        OUT("  %-5s %4d %4d %4d %5d %5d %5d %5d %5d  %s\n", id, j->arrival, j->burst, j->priority,
            j->start, j->completion, j->turnaround, j->waiting, j->response, job_status_name(j->status));
    }
    OUT("  (AT=arrival BT=burst PR=priority ST=first start CT=completion TAT=CT-AT WT=TAT-BT RT=ST-AT)\n");
    OUT("Average TAT = %.2f | Average WT = %.2f | Average RT = %.2f\n", r->avg_tat, r->avg_wt, r->avg_rt);
    OUT("Makespan = %d | CPU idle = %d | Throughput = %.3f jobs/unit | Jobs completed = %s\n",
        r->makespan, r->idle_time, r->makespan > 0 ? (double)r->n / r->makespan : 0.0,
        r->all_completed ? "all" : "NOT all");
}

static void print_best(const char *what, const SchedResult *res, int count, int field)
{
    double best = 0;
    for (int i = 0; i < count; i++) {
        double v = field == 0 ? res[i].avg_tat : field == 1 ? res[i].avg_wt : res[i].avg_rt;
        if (i == 0 || v < best - 1e-9)
            best = v;
    }
    OUT("%-28s: ", what);
    int first = 1;
    for (int i = 0; i < count; i++) {
        double v = field == 0 ? res[i].avg_tat : field == 1 ? res[i].avg_wt : res[i].avg_rt;
        if (v < best + 1e-9) {
            char name[32];
            if (res[i].policy == POL_RR)
                snprintf(name, sizeof name, "RR (q=%d)", res[i].quantum);
            else
                snprintf(name, sizeof name, "%s", policy_short(res[i].policy));
            OUT("%s%s", first ? "" : " = ", name);
            first = 0;
        }
    }
    OUT(" (%.2f)\n", best);
}

void schedule_print_compare(const SchedResult *res, int count)
{
    OUT("\n=== Scheduling comparison on the same job set (%d jobs) ===\n", res[0].n);
    OUT("  %-15s %8s %8s %8s %9s %11s\n", "Policy", "Avg TAT", "Avg WT", "Avg RT", "Makespan", "Dispatches");
    OUT("  %-15s %8s %8s %8s %9s %11s\n", "---------------", "-------", "-------", "-------", "--------", "----------");
    for (int i = 0; i < count; i++) {
        char name[32];
        if (res[i].policy == POL_RR)
            snprintf(name, sizeof name, "RR (q=%d)", res[i].quantum);
        else
            snprintf(name, sizeof name, "%s", policy_short(res[i].policy));
        OUT("  %-15s %8.2f %8.2f %8.2f %9d %11d\n", name, res[i].avg_tat, res[i].avg_wt,
            res[i].avg_rt, res[i].makespan, res[i].dispatches);
    }

    OUT("\nPer-job waiting time (WT):\n  %-5s", "Job");
    for (int i = 0; i < count; i++)
        OUT(" %14s", res[i].policy == POL_RR ? "RR" : policy_short(res[i].policy));
    OUT("\n");
    for (int k = 0; k < res[0].n; k++) {
        char id[16];
        snprintf(id, sizeof id, "J%d", res[0].jobs[k].id);
        OUT("  %-5s", id);
        for (int i = 0; i < count; i++)
            OUT(" %14d", res[i].jobs[k].waiting);
        OUT("\n");
    }
    OUT("\n");
    print_best("Lowest average waiting time", res, count, 1);
    print_best("Lowest average turnaround", res, count, 0);
    print_best("Lowest average response", res, count, 2);
}
