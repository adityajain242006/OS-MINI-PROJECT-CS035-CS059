/*
 * common.h - shared constants, output macros and small utilities
 *
 * CampusCompute - Intelligent Job Scheduling and Resource Management
 * OS Mini-Project (UE25CS242A), Level 3
 * Team: Amudan Desikan (PES1UG25CS059), Aditya Jain (PES1UG25CS035)
 */
#ifndef CC_COMMON_H
#define CC_COMMON_H

#include <stdio.h>
#include <stddef.h>

#define CC_VERSION      "1.0"
#define CC_PROG         "campuscompute"

#define LOG_DIR         "logs"
#define LOG_FILE        "logs/campuscompute.log"
#define DEFAULT_JOBS    "tests/jobs.txt"
#define BANKER_SAFE     "tests/banker_safe.txt"
#define BANKER_UNSAFE   "tests/banker_unsafe.txt"
#define INVALID_JOBS    "tests/jobs_invalid.txt"

/* Exit codes used by the CLI */
#define EXIT_OK         0
#define EXIT_RUNTIME    1   /* a feature ran but failed (I/O error, IPC error, ...) */
#define EXIT_USAGE      2   /* invalid command / malformed arguments */

/*
 * g_brief = 1 while the integrated demo runs in compact mode.
 * Detailed per-module output goes through OUT()/ERR() so the demo can show
 * a one-screen summary; normal commands always run with g_brief = 0.
 */
extern int g_brief;

#define OUT(...) do { if (!g_brief) printf(__VA_ARGS__); } while (0)
#define ERR(...) do { if (!g_brief) fprintf(stderr, "error: " __VA_ARGS__); } while (0)

/* util.c */
int    parse_int_range(const char *s, long lo, long hi, int *out);
void   timestamp_now(char *buf, size_t n);
void   sleep_ms(int ms);
double mono_seconds(void);
int    count_threads(void);
int    count_open_fds(void);
void   child_exit(int code) __attribute__((noreturn));

#endif /* CC_COMMON_H */
