/*
 * util.c - small helpers shared by all modules
 */
#include "common.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int g_brief = 0;

/*
 * Strict integer parser: the whole string must be a base-10 integer inside
 * [lo, hi]. Returns 0 on success, -1 on any error ("12abc", "", overflow...).
 */
int parse_int_range(const char *s, long lo, long hi, int *out)
{
    if (s == NULL || *s == '\0')
        return -1;
    errno = 0;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0')
        return -1;
    if (v < lo || v > hi)
        return -1;
    *out = (int)v;
    return 0;
}

/* "YYYY-MM-DD HH:MM:SS.mmm" in local time */
void timestamp_now(char *buf, size_t n)
{
    struct timespec ts;
    struct tm tmv;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tmv);
    char base[32];
    strftime(base, sizeof base, "%Y-%m-%d %H:%M:%S", &tmv);
    snprintf(buf, n, "%s.%03ld", base, ts.tv_nsec / 1000000L);
}

void sleep_ms(int ms)
{
    struct timespec req = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&req, &req) == -1 && errno == EINTR)
        ;
}

double mono_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Count entries of a /proc directory, ignoring "." and "..". */
static int count_dir_entries(const char *path)
{
    DIR *d = opendir(path);
    if (d == NULL)
        return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0)
            n++;
    }
    closedir(d);
    return n;
}

/* Number of threads currently alive in this process (from /proc/self/task). */
int count_threads(void)
{
    return count_dir_entries("/proc/self/task");
}

/* Number of open file descriptors, excluding the one used to read /proc/self/fd. */
int count_open_fds(void)
{
    int n = count_dir_entries("/proc/self/fd");
    return n > 0 ? n - 1 : n;
}

/*
 * Used by every forked child: flush stdio buffers, then _exit() so the child
 * never runs the parent's atexit handlers or flushes duplicated buffers.
 */
void child_exit(int code)
{
    fflush(NULL);
    _exit(code);
}
