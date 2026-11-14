# CampusCompute

OS Mini-Project (UE25CS242A), Level 3

**Team**
- Amudan Desikan, PES1UG25CS059
- Aditya Jain, PES1UG25CS035

## Who did what

The project has two halves. Each of us built, ran and documented one of them.

**Amudan (screenshots S01 to S12)**
- Set up the Ubuntu environment and the build: the project layout and the Makefile.
- Job model and input validation (FR1).
- The three schedulers, FCFS, Round Robin and Priority, plus the compare mode (FR2).
- The threaded producer-consumer logger (FR3).
- The shared-memory exchange between the two processes (FR4).

**Aditya (screenshots S13 to S24)**
- Semaphore handling and IPC cleanup checks (FR4).
- Banker's algorithm (FR5).
- Worker processes and failure detection (FR6).
- Resource limits (FR7).
- CLI error handling (FR8).
- The integrated `demo` command, the log file, the `cleanup` command and the final ZIP packaging.

We tested each other's parts before taking the final screenshots.

---

## What it is

CampusCompute is a small command-line program in C. It pretends to be the job manager for a college computer lab. Each feature from the brief is its own sub-command, so it can be run and checked on its own:

- scheduling jobs with FCFS, Round Robin and Priority
- logging activity from several threads through a bounded buffer
- passing data between two processes through shared memory
- checking for deadlock with the Banker's algorithm
- catching worker processes that fail
- showing what happens when the OS limits a process's resources

Nothing in the output is hard-coded. Every number comes from running the algorithms on the input files in `tests/`.

## What you need

A Linux machine. We used Ubuntu:
- Amudan: Ubuntu 26.04 in a VM on a MacBook, with GCC 15.2.
- Aditya: an Ubuntu VM on his laptop.

WSL 2 on Windows should also work. WSL 1 won't, because it has no real `/dev/shm`.

Install the compiler and tools with:

```bash
sudo apt update
sudo apt install -y build-essential zip unzip
```

No other libraries are needed. The program only uses glibc, pthreads and librt.

## Building

From inside the `CampusCompute` folder:

```bash
make            # builds ./campuscompute
make clean      # deletes build/ and the executable
```

The build uses `-Wall -Wextra -Wpedantic` and should finish with no warnings.

## Running it

Run everything from the project folder, since the test files are referenced as `tests/...`.

```bash
./campuscompute help
```

**Jobs and scheduling (FR1, FR2)**
```bash
./campuscompute jobs tests/jobs.txt
./campuscompute schedule fcfs tests/jobs.txt -v        # -v also prints the state changes
./campuscompute schedule rr tests/jobs.txt -q 2        # -q sets the time quantum
./campuscompute schedule priority tests/jobs.txt
./campuscompute schedule compare tests/jobs.txt        # all three on the same jobs
```

**Logger (FR3)**
```bash
./campuscompute logger                       # 2 producers, 2 consumers, buffer of 4
./campuscompute logger -p 2 -c 2 -n 4 -b 2   # smaller buffer, so you can see the waits
```

**Shared memory (FR4)**
```bash
./campuscompute ipc
```

**Banker's algorithm (FR5)**
```bash
./campuscompute banker tests/banker_safe.txt
./campuscompute banker tests/banker_unsafe.txt
```

**Workers (FR6)**
```bash
./campuscompute workers              # all three finish normally
./campuscompute workers --fail 2     # worker 2 exits with status 3 on purpose
./campuscompute workers --crash 2    # worker 2 calls abort() on purpose
```
The failures are deliberate and labelled `[SIMULATED FAILURE]` in the output. They are not real crashes.

**Resource limits (FR7)**
```bash
./campuscompute rlimit
```

**Everything at once**
```bash
./campuscompute demo           # short version with a PASS/FAIL table at the end
./campuscompute demo --full    # same run, with all the details
make test                      # runs each command once
```

**Exit codes**

| Code | Meaning |
|---|---|
| 0 | Success |
| 1 | Something failed at runtime, e.g. a missing file |
| 2 | The command itself was wrong, e.g. a bad option. An error and the usage line are printed. |

## Decisions we made

**FCFS**
- Jobs run in order of arrival.
- If two jobs arrive together, the lower job ID goes first.

**Round Robin**
- The default quantum is 2. You can change it with `-q`, anywhere from 1 to 1000.
- A job that uses up its quantum goes to the back of the queue.
- A job that arrives while another is running joins the queue *before* the job that just got preempted. This is the usual textbook convention, and the program prints it so there's no confusion.

**Priority**
- Non-preemptive.
- A smaller number means a higher priority.
- Ties go to whichever job arrived first, and then to the lower ID.

**All schedulers**
- If no job has arrived yet, the CPU is shown as `idle`.
- `tests/jobs.txt` has J4 and J5 arriving at the same time with the same priority, so the tie rules actually get used.
- Every scheduler prints CT, TAT (CT − arrival), WT (TAT − burst) and RT (first run − arrival), along with the averages.

**Logger**
- One mutex and two condition variables, `not_full` and `not_empty`.
- Every wait sits inside a `while` loop.
- When the producers are done, main sets a `done` flag and wakes everyone. The consumers empty whatever is left and exit. Then main joins all the threads and destroys the mutex and the condition variables.

**Shared memory**
- The parent (the "dispatcher") writes job records into a shared slot. The child (the "accountant") reads them and sends back a total.
- Two named semaphores take turns guarding the slot.
- Every object name starts with `campus_` and ends with the process ID, so we never touch anyone else's objects.
- Everything is unlinked at the end.

**Workers**
- The parent forks the workers and collects each one with `waitpid()`.
- `WIFEXITED` and `WIFSIGNALED` tell a normal exit apart from an error code or a signal.
- At the end, the parent checks that no zombie processes are left.

**Resource limits**
- Both limits are applied inside child processes, so the main program is never affected.
- With `RLIMIT_CPU` at 1 second, the child gets killed by SIGXCPU.
- With `RLIMIT_NOFILE` at 8, `open()` starts failing with EMFILE.

**Logging**
- Every command appends timestamped lines to `logs/campuscompute.log`.

## Input files

- `tests/jobs.txt`: one job per line, in the form `id arrival burst priority`.
- `tests/jobs_invalid.txt`: a broken job file used to test validation. It has a text field, a zero burst, a repeated ID, a missing field and a priority out of range.
- `tests/banker_safe.txt`: the standard 5-process, 3-resource example from the textbook. Its safe sequence is P1, P3, P4, P0, P2.
- `tests/banker_unsafe.txt`: the same matrices with only `0 1 1` available. Here P0, P2 and P4 can never finish.

Banker files have this layout:
1. `n m`
2. the Available vector
3. n rows of Allocation
4. n rows of Maximum

The program works out Need itself, and anything after `#` is a comment.

## Cleaning up

Normally there's nothing to clean up. The program reaps its child processes, joins its threads and removes its shared memory and semaphores every time it runs.

If a run gets killed halfway, for example with Ctrl+C during `ipc`, use these:

```bash
./campuscompute cleanup --check   # shows leftover IPC objects, processes, zombies
./campuscompute cleanup           # removes leftover campus_* objects from dead runs
ls /dev/shm                       # should show no campus_* entries now
make clean                        # removes the build output
make distclean                    # also removes logs/ and the zip
```

`cleanup` only removes objects that:
- start with `campus_`,
- belong to the current user, and
- came from a run that is no longer active.

It never touches anything else in `/dev/shm`.

## Folder layout

```
CampusCompute/
├── Makefile
├── README.md
├── include/   header files, one per module
├── src/       main.c, job.c, scheduler.c, logger.c, ipc.c, banker.c,
│              workers.c, rlimit_demo.c, util.c
└── tests/     jobs.txt, jobs_invalid.txt, banker_safe.txt, banker_unsafe.txt
```

To make the zip again, run `make zip`. To check it, run `make zipcheck`.
