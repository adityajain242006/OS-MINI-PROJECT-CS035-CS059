/*
 * logger.h - FR3: concurrent activity logger (bounded producer-consumer queue)
 *            plus applog(), the program-wide append-only event log.
 */
#ifndef CC_LOGGER_H
#define CC_LOGGER_H

#include "job.h"

#define LOGGER_DEFAULT_PRODUCERS 2
#define LOGGER_DEFAULT_CONSUMERS 2
#define LOGGER_DEFAULT_EVENTS    5      /* events per producer */
#define LOGGER_DEFAULT_CAPACITY  4      /* bounded buffer slots */
#define LOGGER_MAX_THREADS       8
#define LOGGER_MAX_EVENTS        1000
#define LOGGER_MAX_CAPACITY      64

typedef struct {
    int producers, consumers, per_producer, capacity;
    int produced;          /* items inserted into the buffer           */
    int consumed;          /* items removed from the buffer            */
    int logged;            /* items written to the log file            */
    int full_waits;        /* times a producer blocked on "buffer full" */
    int empty_waits;       /* times a consumer blocked on "buffer empty" */
    int producers_joined;
    int consumers_joined;
    int threads_after;     /* threads alive after shutdown (expect 1)   */
    int ok;                /* 1 if produced == consumed == logged == expected and all joined */
} LoggerStats;

/*
 * Run the threaded logger. Producers turn job-lifecycle activity into log
 * events; consumers remove them from the bounded buffer and append them to
 * logs/campuscompute.log. jobs may be NULL (synthetic job IDs are used).
 * Returns 0 on success, -1 on error.
 */
int logger_run(int producers, int consumers, int per_producer, int capacity,
               const Job *jobs, int njobs, LoggerStats *st);

/*
 * Append one timestamped line "[time] [MODULE] message" to logs/campuscompute.log.
 * Thread-safe (mutex protected); creates the logs/ directory on first use.
 * Returns 0 on success, -1 if the log could not be written.
 */
int applog(const char *module, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif /* CC_LOGGER_H */
