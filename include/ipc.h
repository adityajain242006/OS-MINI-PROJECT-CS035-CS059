/*
 * ipc.h - FR4: POSIX shared memory + named semaphores between two processes
 */
#ifndef CC_IPC_H
#define CC_IPC_H

#include "job.h"

/* Every IPC object this program creates starts with this prefix, followed by
 * the creating process's PID, e.g. /campus_shm_4211, /campus_sem_empty_4211.
 * Cleanup code only ever touches names with this prefix. */
#define IPC_PREFIX     "campus_"
#define IPC_TIMEOUT_S  5          /* sem_timedwait timeout (peer not responding) */

typedef struct {
    int sent;            /* job records written by the dispatcher (parent)  */
    int received;        /* records the accountant (child) reported reading  */
    int total_sent;      /* sum of bursts computed by the parent             */
    int total_reported;  /* sum of bursts computed by the child via shm      */
    int verified;        /* 1 if both sides agree                            */
    int cleaned;         /* 1 if every created object was unlinked           */
    int leftover;        /* program-created objects still in /dev/shm after  */
} IpcStats;

/* Run the dispatcher/accountant exchange. Returns 0 on success, -1 on error. */
int ipc_run(const Job *jobs, int njobs, IpcStats *st);

/*
 * Print the program-created IPC objects currently in /dev/shm (prefix
 * "campus_"). Returns how many were found, or -1 if /dev/shm is unreadable.
 */
int ipc_list_objects(const char *indent);

/*
 * Remove stale program-created IPC objects left behind by a crashed/killed
 * run (only names with our prefix, owned by this user, whose creating PID is
 * no longer running). check_only = 1 just reports. Returns objects removed
 * (or found, in check mode), -1 on error.
 */
int ipc_cleanup_stale(int check_only);

#endif /* CC_IPC_H */
