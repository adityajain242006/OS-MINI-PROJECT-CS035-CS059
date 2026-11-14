/*
 * job.c - FR1: job file parsing, input validation, lifecycle status
 *
 * Job file format (one job per line, '#' starts a comment):
 *     <id> <arrival> <burst> <priority>
 * Limits: id 1..9999, arrival 0..100000, burst 1..10000, priority 0..99
 * (lower priority number = higher priority). IDs must be unique.
 */
#include "job.h"
#include "common.h"

#include <errno.h>
#include <string.h>

const char *job_status_name(JobStatus s)
{
    switch (s) {
    case JOB_NEW:       return "NEW";
    case JOB_READY:     return "READY";
    case JOB_RUNNING:   return "RUNNING";
    case JOB_COMPLETED: return "COMPLETED";
    }
    return "?";
}

void jobs_reset(Job *jobs, int n)
{
    for (int i = 0; i < n; i++) {
        jobs[i].remaining  = jobs[i].burst;
        jobs[i].start      = -1;
        jobs[i].completion = 0;
        jobs[i].turnaround = 0;
        jobs[i].waiting    = 0;
        jobs[i].response   = 0;
        jobs[i].status     = JOB_NEW;
    }
}

int jobs_load(const char *path, Job *jobs, int max, int *count)
{
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        ERR("cannot open job file '%s': %s\n", path, strerror(errno));
        return -1;
    }

    static const char *field_names[4] = { "job id", "arrival time", "CPU burst", "priority" };
    static const long  lo[4] = { 1, 0, 1, 0 };
    static const long  hi[4] = { JOB_ID_MAX, JOB_ARRIVAL_MAX, JOB_BURST_MAX, JOB_PRIO_MAX };

    char line[512];
    int lineno = 0, n = 0, errors = 0;

    while (fgets(line, sizeof line, fp) != NULL) {
        lineno++;

        /* Reject over-long lines instead of silently splitting them. */
        size_t len = strlen(line);
        if (len == sizeof line - 1 && line[len - 1] != '\n' && !feof(fp)) {
            ERR("%s:%d: line too long (max %zu characters)\n", path, lineno, sizeof line - 2);
            errors++;
            int c;
            while ((c = fgetc(fp)) != '\n' && c != EOF)
                ;
            continue;
        }

        char *hash = strchr(line, '#');          /* strip comment */
        if (hash != NULL)
            *hash = '\0';

        char *tok[4] = { NULL, NULL, NULL, NULL };
        int ntok = 0;
        char *save = NULL;
        for (char *p = strtok_r(line, " \t\r\n", &save); p != NULL; p = strtok_r(NULL, " \t\r\n", &save)) {
            if (ntok < 4)
                tok[ntok] = p;
            ntok++;
        }
        if (ntok == 0)
            continue;                            /* blank / comment-only line */

        if (ntok != 4) {
            ERR("%s:%d: expected 4 fields (id arrival burst priority), found %d\n", path, lineno, ntok);
            errors++;
            continue;
        }

        int v[4];
        int bad = 0;
        for (int k = 0; k < 4; k++) {
            if (parse_int_range(tok[k], lo[k], hi[k], &v[k]) != 0) {
                ERR("%s:%d: invalid %s '%s' (expected an integer in %ld..%ld)\n",
                    path, lineno, field_names[k], tok[k], lo[k], hi[k]);
                bad = 1;
            }
        }
        if (bad) {
            errors++;
            continue;
        }

        int dup = 0;
        for (int i = 0; i < n; i++) {
            if (jobs[i].id == v[0]) {
                dup = 1;
                break;
            }
        }
        if (dup) {
            ERR("%s:%d: duplicate job id %d (job IDs must be unique)\n", path, lineno, v[0]);
            errors++;
            continue;
        }
        if (n >= max) {
            ERR("%s:%d: too many jobs (maximum is %d)\n", path, lineno, max);
            errors++;
            continue;
        }

        Job *j = &jobs[n++];
        memset(j, 0, sizeof *j);
        j->id       = v[0];
        j->arrival  = v[1];
        j->burst    = v[2];
        j->priority = v[3];
    }
    fclose(fp);

    if (errors > 0) {
        ERR("%s: %d invalid line(s) - job file rejected, no jobs loaded\n", path, errors);
        return errors;
    }
    if (n == 0) {
        ERR("%s: file contains no jobs\n", path);
        return 1;
    }
    jobs_reset(jobs, n);
    *count = n;
    return 0;
}

void jobs_print(const Job *jobs, int n)
{
    OUT("  %-5s %-8s %-6s %-9s %-10s\n", "Job", "Arrival", "Burst", "Priority", "Status");
    OUT("  %-5s %-8s %-6s %-9s %-10s\n", "-----", "-------", "-----", "--------", "------");
    for (int i = 0; i < n; i++) {
        char id[16];
        snprintf(id, sizeof id, "J%d", jobs[i].id);
        OUT("  %-5s %-8d %-6d %-9d %-10s\n", id, jobs[i].arrival, jobs[i].burst,
            jobs[i].priority, job_status_name(jobs[i].status));
    }
}
