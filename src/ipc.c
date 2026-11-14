/*
 * ipc.c - FR4: inter-process communication with POSIX shared memory
 *
 * Two cooperating processes:
 *   dispatcher (parent) - writes job records into a one-slot shared mailbox
 *   accountant (child)  - opens the SAME objects by name, reads each record,
 *                         totals the CPU bursts and writes a reply back.
 *
 * Synchronisation: two named POSIX semaphores (classic one-slot handoff)
 *   /campus_sem_empty_<pid>  initial 1 : slot is free, dispatcher may write
 *   /campus_sem_full_<pid>   initial 0 : slot holds data, accountant may read
 *   dispatcher: sem_wait(empty) -> write slot -> sem_post(full)
 *   accountant: sem_wait(full)  -> read slot  -> sem_post(empty)
 * Waits use sem_timedwait() so a dead peer causes an error, never a hang.
 *
 * Cleanup: munmap + close, sem_close, sem_unlink, shm_unlink - only for the
 * objects this run created (names carry our prefix and PID).
 */
#include "ipc.h"
#include "common.h"
#include "logger.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Layout of the shared-memory region */
typedef struct {
    /* mailbox slot: dispatcher -> accountant */
    int seq;
    int job_id, burst, priority;
    int sender_pid;
    int done;                 /* 1 = no more records (sentinel) */

    /* reply area: accountant -> dispatcher */
    int reply_ready;
    int reply_count;
    int reply_total_burst;
    int reply_max_burst_job;
    int reader_pid;
} SharedBlock;

typedef struct {
    char shm[64], sem_empty[64], sem_full[64];
} IpcNames;

static void make_names(IpcNames *nm, pid_t owner)
{
    snprintf(nm->shm, sizeof nm->shm, "/" IPC_PREFIX "shm_%d", (int)owner);
    snprintf(nm->sem_empty, sizeof nm->sem_empty, "/" IPC_PREFIX "sem_empty_%d", (int)owner);
    snprintf(nm->sem_full, sizeof nm->sem_full, "/" IPC_PREFIX "sem_full_%d", (int)owner);
}

/* sem_wait with a timeout; returns 0 or -1 (errno = ETIMEDOUT etc.) */
static int sem_wait_timeout(sem_t *s, int seconds)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += seconds;
    while (sem_timedwait(s, &ts) == -1) {
        if (errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

int ipc_list_objects(const char *indent)
{
    DIR *d = opendir("/dev/shm");
    if (d == NULL) {
        OUT("%s(cannot read /dev/shm: %s)\n", indent, strerror(errno));
        return -1;
    }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, IPC_PREFIX, strlen(IPC_PREFIX)) == 0 ||
            strncmp(e->d_name, "sem." IPC_PREFIX, strlen("sem." IPC_PREFIX)) == 0) {
            OUT("%s/dev/shm/%s\n", indent, e->d_name);
            n++;
        }
    }
    closedir(d);
    if (n == 0)
        OUT("%s(none - no program-created IPC objects)\n", indent);
    return n;
}

/* ------------------------------ child side ------------------------------ */

static void accountant_process(const IpcNames *nm)
{
    pid_t me = getpid();

    /* Open the objects BY NAME - this is what makes them "named" IPC. */
    int fd = shm_open(nm->shm, O_RDWR, 0);
    if (fd == -1) {
        fprintf(stderr, "error: [accountant] shm_open(%s): %s\n", nm->shm, strerror(errno));
        child_exit(1);
    }
    SharedBlock *blk = mmap(NULL, sizeof *blk, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (blk == MAP_FAILED) {
        fprintf(stderr, "error: [accountant] mmap: %s\n", strerror(errno));
        child_exit(1);
    }
    sem_t *empty = sem_open(nm->sem_empty, 0);
    sem_t *full  = sem_open(nm->sem_full, 0);
    if (empty == SEM_FAILED || full == SEM_FAILED) {
        fprintf(stderr, "error: [accountant] sem_open: %s\n", strerror(errno));
        munmap(blk, sizeof *blk);
        child_exit(1);
    }
    OUT("[accountant pid=%d] opened %s and both semaphores by name\n", (int)me, nm->shm);

    int count = 0, total = 0, max_burst = -1, max_job = 0, status = 0;
    for (;;) {
        if (sem_wait_timeout(full, IPC_TIMEOUT_S) == -1) {
            fprintf(stderr, "error: [accountant] sem_wait(full): %s (dispatcher not responding)\n",
                    strerror(errno));
            status = 1;
            break;
        }
        if (blk->done) {
            OUT("[accountant pid=%d] sem_wait(full) ok -> end-of-data marker received\n", (int)me);
            break;
        }
        count++;
        total += blk->burst;
        if (blk->burst > max_burst) {
            max_burst = blk->burst;
            max_job = blk->job_id;
        }
        OUT("[accountant pid=%d] sem_wait(full) ok -> read  msg #%d: J%d burst=%d prio=%d (from pid %d) -> sem_post(empty)\n",
            (int)me, blk->seq, blk->job_id, blk->burst, blk->priority, blk->sender_pid);
        sem_post(empty);
    }

    if (status == 0) {
        /* Write the reply into shared memory, then hand the slot back. */
        blk->reply_count = count;
        blk->reply_total_burst = total;
        blk->reply_max_burst_job = max_job;
        blk->reader_pid = (int)me;
        blk->reply_ready = 1;
        OUT("[accountant pid=%d] wrote reply: %d jobs, total burst=%d, longest=J%d -> sem_post(empty)\n",
            (int)me, count, total, max_job);
        sem_post(empty);
    }

    sem_close(empty);
    sem_close(full);
    munmap(blk, sizeof *blk);
    child_exit(status);
}

/* ----------------------------- parent side ------------------------------ */

int ipc_run(const Job *jobs, int njobs, IpcStats *st)
{
    memset(st, 0, sizeof *st);
    IpcNames nm;
    make_names(&nm, getpid());

    int created_shm = 0, created_empty = 0, created_full = 0, rc = 0;
    int fd = -1;
    SharedBlock *blk = MAP_FAILED;
    sem_t *empty = SEM_FAILED, *full = SEM_FAILED;
    pid_t child = -1;

    OUT("IPC objects for this run (prefix '%s' + PID %d):\n", IPC_PREFIX, (int)getpid());
    OUT("  shared memory : %s  (%zu bytes)\n", nm.shm, sizeof(SharedBlock));
    OUT("  semaphore     : %s (initial 1 = slot free)\n", nm.sem_empty);
    OUT("  semaphore     : %s  (initial 0 = no data)\n\n", nm.sem_full);

    /* O_EXCL: never reuse or clobber an object we did not create */
    fd = shm_open(nm.shm, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd == -1) {
        ERR("shm_open(%s): %s\n", nm.shm, strerror(errno));
        rc = -1;
        goto cleanup;
    }
    created_shm = 1;
    if (ftruncate(fd, sizeof(SharedBlock)) == -1) {
        ERR("ftruncate(%s): %s\n", nm.shm, strerror(errno));
        rc = -1;
        goto cleanup;
    }
    blk = mmap(NULL, sizeof *blk, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (blk == MAP_FAILED) {
        ERR("mmap(%s): %s\n", nm.shm, strerror(errno));
        rc = -1;
        goto cleanup;
    }
    memset(blk, 0, sizeof *blk);

    empty = sem_open(nm.sem_empty, O_CREAT | O_EXCL, 0600, 1);
    if (empty == SEM_FAILED) {
        ERR("sem_open(%s): %s\n", nm.sem_empty, strerror(errno));
        rc = -1;
        goto cleanup;
    }
    created_empty = 1;
    full = sem_open(nm.sem_full, O_CREAT | O_EXCL, 0600, 0);
    if (full == SEM_FAILED) {
        ERR("sem_open(%s): %s\n", nm.sem_full, strerror(errno));
        rc = -1;
        goto cleanup;
    }
    created_full = 1;
    applog("IPC", "created %s, %s, %s", nm.shm, nm.sem_empty, nm.sem_full);

    OUT("Created. Program IPC objects now present:\n");
    ipc_list_objects("  ");
    OUT("\n");

    fflush(stdout);
    child = fork();
    if (child == -1) {
        ERR("fork: %s\n", strerror(errno));
        rc = -1;
        goto cleanup;
    }
    if (child == 0)
        accountant_process(&nm);                   /* never returns */

    OUT("[dispatcher pid=%d] forked accountant pid=%d\n", (int)getpid(), (int)child);

    /* send every job, one record at a time through the mailbox slot */
    for (int i = 0; i < njobs; i++) {
        if (sem_wait_timeout(empty, IPC_TIMEOUT_S) == -1) {
            ERR("[dispatcher] sem_wait(empty): %s (accountant not responding)\n", strerror(errno));
            rc = -1;
            break;
        }
        blk->seq = i + 1;
        blk->job_id = jobs[i].id;
        blk->burst = jobs[i].burst;
        blk->priority = jobs[i].priority;
        blk->sender_pid = (int)getpid();
        blk->done = 0;
        st->sent++;
        st->total_sent += jobs[i].burst;
        OUT("[dispatcher pid=%d] sem_wait(empty) ok -> wrote msg #%d: J%d burst=%d prio=%d -> sem_post(full)\n",
            (int)getpid(), i + 1, jobs[i].id, jobs[i].burst, jobs[i].priority);
        sem_post(full);
    }

    if (rc == 0) {                                 /* end-of-data marker */
        if (sem_wait_timeout(empty, IPC_TIMEOUT_S) == -1) {
            ERR("[dispatcher] sem_wait(empty): %s\n", strerror(errno));
            rc = -1;
        } else {
            blk->done = 1;
            OUT("[dispatcher pid=%d] sem_wait(empty) ok -> wrote end-of-data marker -> sem_post(full)\n",
                (int)getpid());
            sem_post(full);
        }
    }
    if (rc == 0) {                                 /* wait for the reply */
        if (sem_wait_timeout(empty, IPC_TIMEOUT_S) == -1 || !blk->reply_ready) {
            ERR("[dispatcher] no reply from accountant\n");
            rc = -1;
        } else {
            st->received = blk->reply_count;
            st->total_reported = blk->reply_total_burst;
            OUT("[dispatcher pid=%d] sem_wait(empty) ok -> read reply from pid %d: %d jobs, total burst=%d, longest=J%d\n",
                (int)getpid(), blk->reader_pid, blk->reply_count, blk->reply_total_burst, blk->reply_max_burst_job);
        }
    }

    /* reap the child; if something went wrong make sure it does not linger */
    {
        int status = 0;
        if (rc != 0)
            kill(child, SIGTERM);
        if (waitpid(child, &status, 0) == -1) {
            ERR("waitpid: %s\n", strerror(errno));
            rc = -1;
        } else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            ERR("accountant process failed (status 0x%x)\n", status);
            rc = -1;
        } else {
            OUT("[dispatcher pid=%d] waitpid: accountant pid=%d exited with status 0\n", (int)getpid(), (int)child);
        }
    }

    st->verified = (rc == 0 && st->received == st->sent && st->total_reported == st->total_sent);
    if (rc == 0)
        OUT("\nVerification: dispatcher total burst=%d, accountant total burst=%d -> %s\n",
            st->total_sent, st->total_reported, st->verified ? "MATCH (data exchanged correctly)" : "MISMATCH");

cleanup:
    OUT("\nCleanup (only objects created by this run):\n");
    if (blk != MAP_FAILED) {
        munmap(blk, sizeof *blk);
        OUT("  %-48s ok\n", "munmap shared region");
    }
    if (fd != -1) {
        close(fd);
        OUT("  %-48s ok\n", "close shm descriptor");
    }
    int clean_ok = 1;
    if (empty != SEM_FAILED) sem_close(empty);
    if (full != SEM_FAILED)  sem_close(full);
    if (created_empty) {
        int ok = sem_unlink(nm.sem_empty) == 0;
        clean_ok &= ok;
        char what[96];
        snprintf(what, sizeof what, "sem_close + sem_unlink %s", nm.sem_empty);
        OUT("  %-48s %s\n", what, ok ? "ok" : strerror(errno));
    }
    if (created_full) {
        int ok = sem_unlink(nm.sem_full) == 0;
        clean_ok &= ok;
        char what[96];
        snprintf(what, sizeof what, "sem_close + sem_unlink %s", nm.sem_full);
        OUT("  %-48s %s\n", what, ok ? "ok" : strerror(errno));
    }
    if (created_shm) {
        int ok = shm_unlink(nm.shm) == 0;
        clean_ok &= ok;
        char what[96];
        snprintf(what, sizeof what, "shm_unlink %s", nm.shm);
        OUT("  %-48s %s\n", what, ok ? "ok" : strerror(errno));
    }
    OUT("Program IPC objects remaining in /dev/shm:\n");
    st->leftover = ipc_list_objects("  ");
    st->cleaned = clean_ok && st->leftover == 0;
    applog("IPC", "exchange %s: sent=%d received=%d total=%d/%d; cleanup %s",
           rc == 0 ? "ok" : "FAILED", st->sent, st->received, st->total_sent, st->total_reported,
           st->cleaned ? "complete" : "INCOMPLETE");
    return (rc == 0 && st->verified && st->cleaned) ? 0 : -1;
}

/* --------------------------- stale cleanup ----------------------------- */

/* Is pid alive and is it another campuscompute process? */
static int pid_is_campuscompute(int pid)
{
    char path[64], comm[64] = "";
    snprintf(path, sizeof path, "/proc/%d/comm", pid);
    FILE *fp = fopen(path, "r");
    if (fp == NULL)
        return 0;
    if (fgets(comm, sizeof comm, fp) == NULL)
        comm[0] = '\0';
    fclose(fp);
    comm[strcspn(comm, "\n")] = '\0';
    return strcmp(comm, CC_PROG) == 0;
}

int ipc_cleanup_stale(int check_only)
{
    DIR *d = opendir("/dev/shm");
    if (d == NULL) {
        ERR("cannot read /dev/shm: %s\n", strerror(errno));
        return -1;
    }
    int handled = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *name = e->d_name;
        int is_sem = 0;
        if (strncmp(name, "sem." IPC_PREFIX, strlen("sem." IPC_PREFIX)) == 0) {
            is_sem = 1;
            name += 4;                              /* strip "sem." */
        } else if (strncmp(name, IPC_PREFIX, strlen(IPC_PREFIX)) != 0) {
            continue;                               /* not ours: never touch */
        }

        char full[300];
        snprintf(full, sizeof full, "/dev/shm/%s", e->d_name);
        struct stat sb;
        if (stat(full, &sb) == -1 || sb.st_uid != getuid())
            continue;                               /* other user's object */

        const char *us = strrchr(name, '_');
        int owner = us ? atoi(us + 1) : 0;
        if (owner > 0 && pid_is_campuscompute(owner) && owner != (int)getpid()) {
            OUT("  /dev/shm/%s : in use by running pid %d - skipped\n", e->d_name, owner);
            continue;
        }

        handled++;
        if (check_only) {
            OUT("  /dev/shm/%s : stale (creator pid %d not running)\n", e->d_name, owner);
            continue;
        }
        char obj[300];
        snprintf(obj, sizeof obj, "/%s", name);
        int ok = is_sem ? sem_unlink(obj) == 0 : shm_unlink(obj) == 0;
        OUT("  removed /dev/shm/%s ... %s\n", e->d_name, ok ? "ok" : strerror(errno));
        applog("IPC", "cleanup: removed stale %s", e->d_name);
    }
    closedir(d);
    return handled;
}
