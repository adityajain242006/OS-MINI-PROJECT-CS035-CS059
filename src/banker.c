/*
 * banker.c - FR5: resource-safety analysis with the Banker's algorithm
 *
 * Safety algorithm
 *   Work = Available; Finish[i] = false for all i
 *   repeat passes over P0..Pn-1:
 *       if !Finish[i] and Need[i] <= Work (element-wise):
 *           Work = Work + Allocation[i]; Finish[i] = true; append Pi to sequence
 *   until a full pass makes no progress.
 *   State is SAFE iff every Finish[i] is true; the append order is a safe sequence.
 */
#include "banker.h"
#include "common.h"
#include "logger.h"

#include <ctype.h>
#include <errno.h>
#include <string.h>

#define MAX_NUMBERS (2 + BANKER_MAX_R + 2 * BANKER_MAX_P * BANKER_MAX_R)

/* Read every integer in the file (comments skipped). Returns count or -1. */
static int read_numbers(const char *path, int *vals, int maxvals)
{
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        ERR("cannot open Banker state file '%s': %s\n", path, strerror(errno));
        return -1;
    }
    char line[512];
    int lineno = 0, count = 0, bad = 0;
    while (fgets(line, sizeof line, fp) != NULL) {
        lineno++;
        char *hash = strchr(line, '#');
        if (hash != NULL)
            *hash = '\0';
        char *save = NULL;
        for (char *t = strtok_r(line, " \t\r\n,", &save); t != NULL; t = strtok_r(NULL, " \t\r\n,", &save)) {
            int v;
            if (parse_int_range(t, 0, BANKER_MAX_VALUE, &v) != 0) {
                ERR("%s:%d: invalid value '%s' (expected an integer in 0..%d)\n", path, lineno, t, BANKER_MAX_VALUE);
                bad++;
                continue;
            }
            if (count >= maxvals) {
                ERR("%s:%d: too many values in file\n", path, lineno);
                bad++;
                break;
            }
            vals[count++] = v;
        }
    }
    fclose(fp);
    return bad ? -1 : count;
}

int banker_load(const char *path, BankerState *s)
{
    int vals[MAX_NUMBERS];
    int cnt = read_numbers(path, vals, MAX_NUMBERS);
    if (cnt < 0)
        return -1;
    if (cnt < 2) {
        ERR("%s: missing header line '<processes> <resource types>'\n", path);
        return -1;
    }
    memset(s, 0, sizeof *s);
    s->n = vals[0];
    s->m = vals[1];
    if (s->n < 1 || s->n > BANKER_MAX_P || s->m < 1 || s->m > BANKER_MAX_R) {
        ERR("%s: need 1..%d processes and 1..%d resource types (got %d and %d)\n",
            path, BANKER_MAX_P, BANKER_MAX_R, s->n, s->m);
        return -1;
    }
    int expected = 2 + s->m + 2 * s->n * s->m;
    if (cnt != expected) {
        ERR("%s: expected %d numbers for %d processes x %d resources (2 header + %d Available + %d Allocation + %d Maximum), found %d\n",
            path, expected, s->n, s->m, s->m, s->n * s->m, s->n * s->m, cnt);
        return -1;
    }
    int k = 2;
    for (int j = 0; j < s->m; j++)
        s->available[j] = vals[k++];
    for (int i = 0; i < s->n; i++)
        for (int j = 0; j < s->m; j++)
            s->allocation[i][j] = vals[k++];
    for (int i = 0; i < s->n; i++)
        for (int j = 0; j < s->m; j++)
            s->maximum[i][j] = vals[k++];

    int errors = 0;
    for (int i = 0; i < s->n; i++) {
        for (int j = 0; j < s->m; j++) {
            if (s->allocation[i][j] > s->maximum[i][j]) {
                ERR("%s: P%d resource %c: Allocation (%d) exceeds Maximum (%d)\n",
                    path, i, 'A' + j, s->allocation[i][j], s->maximum[i][j]);
                errors++;
            }
            s->need[i][j] = s->maximum[i][j] - s->allocation[i][j];   /* Need = Max - Alloc */
        }
    }
    return errors ? -1 : 0;
}

static void print_vec(const int *v, int m)
{
    OUT("[");
    for (int j = 0; j < m; j++)
        OUT("%s%d", j ? " " : "", v[j]);
    OUT("]");
}

static void print_row(const int *v, int m, int width)
{
    int used = 0;
    for (int j = 0; j < m; j++) {
        char b[16];
        int len = snprintf(b, sizeof b, "%d", v[j]);
        OUT("%*s%s", j ? 1 : 0, "", b);
        used += len + (j ? 1 : 0);
    }
    OUT("%*s", width - used, "");
}

void banker_print_state(const BankerState *s)
{
    int w = s->m * 4 + 2;                    /* column width per matrix */
    if (w < 12)
        w = 12;
    OUT("Processes: %d   Resource types: %d (", s->n, s->m);
    for (int j = 0; j < s->m; j++)
        OUT("%s%c", j ? " " : "", 'A' + j);
    OUT(")\n");
    OUT("Available : ");
    print_vec(s->available, s->m);
    OUT("\n\n      %-*s%-*s%-*s\n", w, "Allocation", w, "Maximum", w, "Need=Max-Alloc");
    for (int i = 0; i < s->n; i++) {
        OUT("  P%-3d", i);
        print_row(s->allocation[i], s->m, w);
        print_row(s->maximum[i], s->m, w);
        print_row(s->need[i], s->m, w);
        OUT("\n");
    }
}

int banker_check(const BankerState *s, int verbose, BankerResult *r)
{
    memset(r, 0, sizeof *r);
    int work[BANKER_MAX_R];
    int finish[BANKER_MAX_P] = { 0 };
    memcpy(work, s->available, sizeof work);

    if (verbose) {
        OUT("\nSafety algorithm: Work = Available = ");
        print_vec(work, s->m);
        OUT(", Finish[all] = false\n");
    }

    int pass = 0, progress = 1;
    while (progress && r->seq_len < s->n) {
        progress = 0;
        pass++;
        if (verbose)
            OUT(" Pass %d:\n", pass);
        for (int i = 0; i < s->n; i++) {
            if (finish[i])
                continue;
            int fits = 1;
            for (int j = 0; j < s->m; j++)
                if (s->need[i][j] > work[j])
                    fits = 0;
            if (verbose) {
                OUT("  P%-2d Need ", i);
                print_vec(s->need[i], s->m);
                OUT(" <= Work ");
                print_vec(work, s->m);
                OUT(" ? %s", fits ? "YES" : "NO ");
            }
            if (fits) {
                for (int j = 0; j < s->m; j++)
                    work[j] += s->allocation[i][j];
                finish[i] = 1;
                r->sequence[r->seq_len++] = i;
                progress = 1;
                if (verbose) {
                    OUT(" -> P%d can finish, releases ", i);
                    print_vec(s->allocation[i], s->m);
                    OUT(", Work = ");
                    print_vec(work, s->m);
                    OUT("\n");
                }
            } else if (verbose) {
                OUT(" -> must wait\n");
            }
        }
    }
    if (verbose && r->seq_len < s->n)
        OUT(" No process could proceed in the last pass -> stop.\n");

    for (int i = 0; i < s->n; i++)
        if (!finish[i])
            r->blocked[r->nblocked++] = i;
    r->safe = (r->nblocked == 0);

    char seq[256] = "";
    size_t len = 0;
    for (int k = 0; k < r->seq_len; k++)
        len += (size_t)snprintf(seq + len, sizeof seq - len, "%sP%d", k ? ", " : "", r->sequence[k]);

    if (r->safe) {
        OUT("\nRESULT: SAFE state. Safe sequence: < %s >\n", seq);
        OUT("        (every process can obtain its remaining Need and finish in this order)\n");
        applog("BANKER", "SAFE state, safe sequence < %s >", seq);
    } else {
        char blk[128] = "";
        size_t bl = 0;
        for (int k = 0; k < r->nblocked; k++)
            bl += (size_t)snprintf(blk + bl, sizeof blk - bl, "%sP%d", k ? " " : "", r->blocked[k]);
        OUT("\nRESULT: UNSAFE state. No safe sequence exists.\n");
        OUT("        Could finish: %s | Can never be guaranteed to finish: %s\n",
            r->seq_len ? seq : "(none)", blk);
        OUT("        Granting this state could lead to DEADLOCK.\n");
        applog("BANKER", "UNSAFE state, finished=< %s > blocked=%s", seq, blk);
    }
    return r->safe;
}
