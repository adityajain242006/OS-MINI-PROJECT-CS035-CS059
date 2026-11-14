/*
 * banker.h - FR5: Banker's safety algorithm
 */
#ifndef CC_BANKER_H
#define CC_BANKER_H

#define BANKER_MAX_P 16
#define BANKER_MAX_R 8
#define BANKER_MAX_VALUE 1000

typedef struct {
    int n;                                   /* processes        */
    int m;                                   /* resource types   */
    int available[BANKER_MAX_R];
    int allocation[BANKER_MAX_P][BANKER_MAX_R];
    int maximum[BANKER_MAX_P][BANKER_MAX_R];
    int need[BANKER_MAX_P][BANKER_MAX_R];    /* Need = Maximum - Allocation */
} BankerState;

typedef struct {
    int safe;
    int sequence[BANKER_MAX_P];              /* order in which processes can finish */
    int seq_len;
    int blocked[BANKER_MAX_P];               /* processes that can never finish */
    int nblocked;
} BankerResult;

/*
 * Load a state file: n m, then Available (m values), Allocation (n rows of m),
 * Maximum (n rows of m). '#' starts a comment. Validates counts, ranges and
 * Allocation <= Maximum, then computes Need. Returns 0 or -1.
 */
int  banker_load(const char *path, BankerState *s);

void banker_print_state(const BankerState *s);

/* Run the safety algorithm (verbose = print every check). Returns r->safe. */
int  banker_check(const BankerState *s, int verbose, BankerResult *r);

#endif /* CC_BANKER_H */
