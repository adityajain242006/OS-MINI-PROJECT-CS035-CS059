/*
 * logger.c - FR3: concurrent activity logger
 *
 * Design
 *   - A bounded circular buffer of LogItem shared by all threads.
 *   - One pthread mutex (q->mtx) protects the buffer, its counters and the
 *     "done" flag.
 *   - Condition variable not_full : producers wait on it while the buffer is full.
 *     Condition variable not_empty: consumers wait on it while it is empty.
 *     Both waits are inside while-loops (handles spurious wake-ups).
 *   - Producers turn job activity (SUBMITTED / STARTED / COMPLETED) into events.
 *   - Consumers take events out and append them to logs/campuscompute.log
 *     through applog(), which has its own mutex for the shared FILE*.
 *
 * Clean termination
 *   main joins every producer, then sets done = 1 under the mutex and
 *   broadcasts not_empty. A consumer exits only when the buffer is empty AND
 *   done == 1, so no event is ever lost. main then joins every consumer and
 *   destroys the mutex / condition variables.
 */
#include "logger.h"
#include "common.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ----------------------------- applog() ----------------------------- */

static pthread_mutex_t g_log_mtx = PTHREAD_MUTEX_INITIALIZER;

int applog(const char *module, const char *fmt, ...)
{
    int rc = 0;
    pthread_mutex_lock(&g_log_mtx);

    if (mkdir(LOG_DIR, 0755) == -1 && errno != EEXIST) {
        rc = -1;
    } else {
        /* opened per call in append mode: safe across fork() and never left open */
        FILE *fp = fopen(LOG_FILE, "a");
        if (fp == NULL) {
            rc = -1;
        } else {
            char ts[40];
            timestamp_now(ts, sizeof ts);
            fprintf(fp, "[%s] [%-6s] ", ts, module);
            va_list ap;
            va_start(ap, fmt);
            vfprintf(fp, fmt, ap);
            va_end(ap);
            fputc('\n', fp);
            if (fclose(fp) != 0)
                rc = -1;
        }
    }
    pthread_mutex_unlock(&g_log_mtx);
    return rc;
}

/* ------------------------- bounded buffer queue ------------------------- */

typedef struct {
    int  seq;              /* global event number          */
    int  producer;         /* which producer created it    */
    char text[96];         /* human-readable event         */
} LogItem;

typedef struct {
    LogItem *buf;
    int capacity, head, tail, count;

    pthread_mutex_t mtx;
    pthread_cond_t  not_full;
    pthread_cond_t  not_empty;
    int done;              /* set by main after all producers have finished */

    /* statistics (protected by mtx) */
    int next_seq, produced, consumed, logged, full_waits, empty_waits;
    int write_errors;

    /* event source */
    const Job *jobs;
    int njobs, producers, per_producer;
} LogQueue;

typedef struct {
    LogQueue *q;
    int index;             /* 1-based thread number for messages */
} ThreadArg;

static const char *PHASES[3] = { "SUBMITTED", "STARTED", "COMPLETED" };

/* Build the k-th event of producer p: producers interleave over the job list. */
static void make_event(const LogQueue *q, int p, int k, char *out, size_t n)
{
    int g = k * q->producers + (p - 1);
    if (q->jobs != NULL && q->njobs > 0) {
        const Job *j = &q->jobs[g % q->njobs];
        snprintf(out, n, "JOB %d %s (burst=%d, prio=%d)", j->id,
                 PHASES[(g / q->njobs) % 3], j->burst, j->priority);
    } else {
        snprintf(out, n, "JOB %d %s", g % 5 + 1, PHASES[(g / 5) % 3]);
    }
}

static void *producer_main(void *arg)
{
    ThreadArg *ta = arg;
    LogQueue *q = ta->q;

    for (int k = 0; k < q->per_producer; k++) {
        LogItem item;
        item.producer = ta->index;
        make_event(q, ta->index, k, item.text, sizeof item.text);
        sleep_ms(1);                               /* simulate work between events */

        pthread_mutex_lock(&q->mtx);
        while (q->count == q->capacity) {          /* buffer FULL -> wait */
            q->full_waits++;
            OUT("    [producer-%d] buffer FULL (%d/%d) -> waiting on not_full\n",
                ta->index, q->count, q->capacity);
            pthread_cond_wait(&q->not_full, &q->mtx);
        }
        item.seq = ++q->next_seq;
        q->buf[q->tail] = item;
        q->tail = (q->tail + 1) % q->capacity;
        q->count++;
        q->produced++;
        OUT("[producer-%d] put  #%03d %-38s buffer %d/%d\n",
            ta->index, item.seq, item.text, q->count, q->capacity);
        pthread_cond_signal(&q->not_empty);       /* wake one consumer */
        pthread_mutex_unlock(&q->mtx);
    }

    pthread_mutex_lock(&q->mtx);
    OUT("[producer-%d] finished %d events -> thread exiting\n", ta->index, q->per_producer);
    pthread_mutex_unlock(&q->mtx);
    return NULL;
}

static void *consumer_main(void *arg)
{
    ThreadArg *ta = arg;
    LogQueue *q = ta->q;

    for (;;) {
        pthread_mutex_lock(&q->mtx);
        while (q->count == 0 && !q->done) {        /* buffer EMPTY -> wait */
            q->empty_waits++;
            OUT("    [consumer-%d] buffer EMPTY -> waiting on not_empty\n", ta->index);
            pthread_cond_wait(&q->not_empty, &q->mtx);
        }
        if (q->count == 0 && q->done) {            /* drained and no more producers */
            OUT("[consumer-%d] buffer empty and producers done -> thread exiting\n", ta->index);
            pthread_mutex_unlock(&q->mtx);
            break;
        }
        LogItem item = q->buf[q->head];
        q->head = (q->head + 1) % q->capacity;
        q->count--;
        q->consumed++;
        OUT("[consumer-%d] take #%03d %-38s buffer %d/%d\n",
            ta->index, item.seq, item.text, q->count, q->capacity);
        pthread_cond_signal(&q->not_full);        /* wake one producer */
        pthread_mutex_unlock(&q->mtx);

        /* Write outside the queue lock; applog() serialises the file itself. */
        int rc = applog("LOGGER", "#%03d producer-%d -> consumer-%d : %s",
                        item.seq, item.producer, ta->index, item.text);

        pthread_mutex_lock(&q->mtx);
        if (rc == 0)
            q->logged++;
        else
            q->write_errors++;
        pthread_mutex_unlock(&q->mtx);

        sleep_ms(3);                               /* slow "disk" -> buffer fills up */
    }
    return NULL;
}

int logger_run(int producers, int consumers, int per_producer, int capacity,
               const Job *jobs, int njobs, LoggerStats *st)
{
    memset(st, 0, sizeof *st);
    st->producers = producers;
    st->consumers = consumers;
    st->per_producer = per_producer;
    st->capacity = capacity;

    LogQueue q;
    memset(&q, 0, sizeof q);
    q.capacity = capacity;
    q.jobs = jobs;
    q.njobs = njobs;
    q.producers = producers;
    q.per_producer = per_producer;
    q.buf = calloc((size_t)capacity, sizeof *q.buf);
    if (q.buf == NULL) {
        ERR("logger: out of memory\n");
        return -1;
    }
    if (pthread_mutex_init(&q.mtx, NULL) != 0 ||
        pthread_cond_init(&q.not_full, NULL) != 0 ||
        pthread_cond_init(&q.not_empty, NULL) != 0) {
        ERR("logger: failed to initialise mutex/condition variables\n");
        free(q.buf);
        return -1;
    }

    if (applog("LOGGER", "---- session start: %d producer(s), %d consumer(s), %d event(s) each, buffer %d ----",
               producers, consumers, per_producer, capacity) != 0) {
        ERR("logger: cannot write %s: %s\n", LOG_FILE, strerror(errno));
        pthread_cond_destroy(&q.not_empty);
        pthread_cond_destroy(&q.not_full);
        pthread_mutex_destroy(&q.mtx);
        free(q.buf);
        return -1;
    }

    OUT("Bounded buffer: %d slots | mutex + condition variables (not_full, not_empty)\n", capacity);
    OUT("Producers: %d x %d events | Consumers: %d | Log file: %s\n\n",
        producers, per_producer, consumers, LOG_FILE);

    pthread_t ptid[LOGGER_MAX_THREADS], ctid[LOGGER_MAX_THREADS];
    ThreadArg parg[LOGGER_MAX_THREADS], carg[LOGGER_MAX_THREADS];
    int pc = 0, cc = 0, rc = 0;

    for (int i = 0; i < consumers; i++) {
        carg[i] = (ThreadArg){ &q, i + 1 };
        int e = pthread_create(&ctid[i], NULL, consumer_main, &carg[i]);
        if (e != 0) {
            ERR("logger: pthread_create(consumer-%d) failed: %s\n", i + 1, strerror(e));
            rc = -1;
            break;
        }
        cc++;
    }
    for (int i = 0; rc == 0 && i < producers; i++) {
        parg[i] = (ThreadArg){ &q, i + 1 };
        int e = pthread_create(&ptid[i], NULL, producer_main, &parg[i]);
        if (e != 0) {
            ERR("logger: pthread_create(producer-%d) failed: %s\n", i + 1, strerror(e));
            rc = -1;
            break;
        }
        pc++;
    }

    /* 1. wait for every producer */
    for (int i = 0; i < pc; i++) {
        pthread_join(ptid[i], NULL);
        st->producers_joined++;
    }
    pthread_mutex_lock(&q.mtx);
    OUT("[main] joined %d producer thread(s); setting done=1 and broadcasting not_empty\n", pc);
    q.done = 1;                                    /* 2. tell consumers no more input */
    pthread_cond_broadcast(&q.not_empty);
    pthread_mutex_unlock(&q.mtx);

    /* 3. consumers drain the buffer and exit; join them */
    for (int i = 0; i < cc; i++) {
        pthread_join(ctid[i], NULL);
        st->consumers_joined++;
    }
    OUT("[main] joined %d consumer thread(s)\n", cc);

    /* 4. release synchronisation objects */
    pthread_cond_destroy(&q.not_empty);
    pthread_cond_destroy(&q.not_full);
    pthread_mutex_destroy(&q.mtx);
    free(q.buf);

    st->produced    = q.produced;
    st->consumed    = q.consumed;
    st->logged      = q.logged;
    st->full_waits  = q.full_waits;
    st->empty_waits = q.empty_waits;
    st->threads_after = count_threads();

    int expected = producers * per_producer;
    st->ok = (rc == 0 && q.write_errors == 0 &&
              st->produced == expected && st->consumed == expected && st->logged == expected &&
              st->producers_joined == producers && st->consumers_joined == consumers);

    applog("LOGGER", "---- session end: produced=%d consumed=%d logged=%d full_waits=%d empty_waits=%d ----",
           st->produced, st->consumed, st->logged, st->full_waits, st->empty_waits);

    OUT("\nSummary: produced=%d consumed=%d written-to-log=%d (expected %d) -> %s\n",
        st->produced, st->consumed, st->logged, expected, st->ok ? "no events lost" : "MISMATCH");
    OUT("Synchronisation: producers blocked on FULL %d time(s), consumers blocked on EMPTY %d time(s)\n",
        st->full_waits, st->empty_waits);
    OUT("Threads: %d/%d producers + %d/%d consumers joined; threads alive now = %d (main only)\n",
        st->producers_joined, producers, st->consumers_joined, consumers, st->threads_after);
    OUT("Mutex and condition variables destroyed; buffer freed.\n");
    return st->ok ? 0 : -1;
}
