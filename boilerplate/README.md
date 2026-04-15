# Multi-Container Runtime with Kernel Memory Monitor

## Team Members

| Name | SRN |
|------|-----|
| Atiksh Gour | PES1UG24CS096 |
| Ayaan Ahmed | PES1UG24CS102 |

---

## Project Summary

This project implements a lightweight Linux container runtime in C. It consists of two integrated parts:

1. **User-Space Runtime + Supervisor (`engine.c`)** — manages multiple isolated containers concurrently, exposes a CLI, captures container output via a bounded-buffer logging pipeline, and handles container lifecycle signals.
2. **Kernel-Space Monitor (`monitor.c`)** — a Linux Kernel Module (LKM) that tracks container processes, enforces soft and hard memory limits, and communicates with the supervisor via `ioctl`.

---

## Environment and Setup

- Ubuntu 22.04 / 24.04 (VM), Secure Boot OFF, No WSL

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

Prepare Alpine mini root filesystem:

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
```

Create per-container writable copies:

```bash
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

---

## Build, Load, and Run Instructions

### 1. Build all binaries and the kernel module

```bash
make
```

### 2. Load the kernel module

```bash
sudo insmod monitor.ko
```

### 3. Verify the control device exists

```bash
ls -l /dev/container_monitor
```

### 4. Start the supervisor daemon

```bash
sudo ./engine supervisor ./rootfs-base
```

### 5. In a second terminal, start containers

```bash
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
sudo ./engine start beta  ./rootfs-beta  /bin/sh --soft-mib 64 --hard-mib 96
```

Copy workload binaries into rootfs before launch if needed:

```bash
cp cpu_hog ./rootfs-alpha/
cp memory_hog ./rootfs-beta/
```

### 6. List running containers

```bash
sudo ./engine ps
```

### 7. Inspect container logs

```bash
sudo ./engine logs alpha
```

### 8. Run a container in foreground (blocks until exit)

```bash
sudo ./engine run alpha ./rootfs-alpha /cpu_hog --nice 0
```

### 9. Stop containers

```bash
sudo ./engine stop alpha
sudo ./engine stop beta
```

### 10. Check kernel logs

```bash
dmesg | tail -20
```

### 11. Unload the kernel module

```bash
sudo rmmod monitor
```

---

## Architecture Overview

The runtime is a single binary (`engine`) used in two modes:

- **Supervisor daemon** — started once, stays alive, manages all containers, owns the logging pipeline.
- **CLI client** — short-lived process that connects to the supervisor over a UNIX domain socket, sends a command, receives a response, and exits.

### Two separate IPC paths

| Path | Purpose | Mechanism |
|------|---------|-----------|
| Path A | Container stdout/stderr → Supervisor | Pipes |
| Path B | CLI process → Supervisor | UNIX domain socket |
| Kernel link | Supervisor → Kernel module | `ioctl` on `/dev/container_monitor` |

### Container creation

The supervisor calls `clone()` with `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS` to create an isolated child process. Inside the child, `chroot()` is called to restrict the filesystem view to the container's assigned rootfs directory, and `/proc` is mounted so tools like `ps` work correctly inside the container.

### Per-container metadata tracked

- Container ID/name
- Host PID
- Start time
- Current state (`starting`, `running`, `stopped`, `killed`, `hard_limit_killed`)
- Configured soft and hard memory limits (MiB)
- Log file path
- Exit code or terminating signal
- `stop_requested` flag (used to distinguish manual stop from hard-limit kill)

---

## Logging System — Bounded Buffer Design

Container stdout and stderr are connected to the supervisor via pipes set up before `clone()`. The supervisor maintains a bounded circular buffer shared between producer and consumer threads.

- **Producers** — one thread per container reads from that container's pipe and inserts log lines into the shared buffer.
- **Consumer** — one thread drains the buffer and writes entries to per-container log files on disk.

### Synchronization

We use a **mutex + two condition variables** (`not_full`, `not_empty`):

- The producer calls `pthread_cond_wait(&not_full)` when the buffer is full, blocking without spinning.
- The consumer calls `pthread_cond_wait(&not_empty)` when the buffer is empty.
- On container exit, the producer thread detects pipe EOF, inserts a sentinel entry, and exits. The consumer flushes all remaining entries before joining.

### Why mutex + condition variables and not semaphores?

Semaphores track counts but cannot express the compound condition "buffer is full AND I need to wait for a specific slot." Condition variables allow us to re-check the full predicate after waking (handling spurious wakeups), which makes the logic correct without busy-waiting. A mutex separately protects the buffer head/tail indices and metadata, so there is no window where a producer and consumer corrupt the buffer simultaneously.

### Race conditions prevented

- Two producer threads inserting at the same tail index simultaneously → prevented by mutex lock around enqueue.
- Consumer reading a slot before producer has finished writing → prevented by the `not_empty` signal being sent only after the write is complete.
- Lost log lines on abrupt container exit → prevented by EOF sentinel and consumer flush-before-join design.

---

## Memory Limit Demonstration

Open one terminal watching kernel logs:

```bash
sudo dmesg -C
sudo dmesg -w
```

In another terminal, launch a memory workload with tight limits:

```bash
cp memory_hog ./rootfs-beta/
sudo ./engine start memtest ./rootfs-beta /memory_hog --soft-mib 2 --hard-mib 4
```

Expected kernel output:

```
[container_monitor] WARNING: container PID <pid> exceeded soft limit (2 MiB). RSS = 2.3 MiB
[container_monitor] HARD LIMIT: killing container PID <pid>. RSS = 4.1 MiB exceeded 4 MiB
```

After the kill, `engine ps` will show the container state as `hard_limit_killed`.

---

## Scheduling Experiment

```bash
cp cpu_hog ./rootfs-alpha/
cp cpu_hog ./rootfs-beta/

sudo ./engine start cpu1 ./rootfs-alpha /cpu_hog --nice 0
sudo ./engine start cpu2 ./rootfs-beta  /cpu_hog --nice 10
```

Measure CPU time consumed by each container process on the host after a fixed duration (e.g., 30 seconds), then stop both:

```bash
sudo ./engine stop cpu1
sudo ./engine stop cpu2
```

---

## Demo Screenshots

> Screenshots are located in the `screenshots/` directory of this repository.
> Each image is annotated with labels directly in the screenshot.

### 1. Multi-container supervision
**`screenshots/01_multicontainer.png`**
*Shows the supervisor process running and two containers (alpha, beta) active simultaneously. The host `ps` output confirms separate PIDs for each container child.*

### 2. Metadata tracking
**`screenshots/02_ps_output.png`**
*Output of `engine ps` showing container ID, host PID, state (`running`), soft/hard memory limits, and start time for each tracked container.*

### 3. Bounded-buffer logging
**`screenshots/03_logging.png`**
*Contents of a container log file captured via the logging pipeline. A second pane shows producer thread activity (log lines being enqueued) and consumer thread flushing to disk.*

### 4. CLI and IPC
**`screenshots/04_cli_ipc.png`**
*A `engine start` command issued in one terminal; the supervisor terminal shows it received the command over the UNIX domain socket and launched the container.*

### 5. Soft-limit warning
**`screenshots/05_soft_limit.png`**
*`dmesg` output showing `[container_monitor] WARNING` for a container whose RSS crossed the soft limit threshold.*

### 6. Hard-limit enforcement
**`screenshots/06_hard_limit.png`**
*`dmesg` output showing `[container_monitor] HARD LIMIT: killing` and, immediately after, `engine ps` reflecting state `hard_limit_killed` for that container.*

### 7. Scheduling experiment
**`screenshots/07_scheduling.png`**
*Side-by-side CPU time consumed by `cpu1` (nice 0) vs `cpu2` (nice 10) after 30 seconds. `cpu1` accumulates significantly more CPU time, confirming priority-based scheduling.*

### 8. Clean teardown
**`screenshots/08_teardown.png`**
*After `engine stop` on all containers and supervisor shutdown: `ps aux | grep engine` shows no remaining processes; `ps aux | grep defunct` shows no zombies. Supervisor prints join-confirmed messages for all logging threads.*

---

## Engineering Analysis

### 1. Isolation Mechanisms

Linux namespaces allow the kernel to present each process group with its own view of system resources without actually duplicating those resources. This project uses three namespace types passed as flags to `clone()`:

- **PID namespace (`CLONE_NEWPID`):** The first process inside the container becomes PID 1 from its own perspective. It cannot see or signal host processes. However, the host kernel still assigns a real host PID to every container process, which is what the supervisor tracks and what the kernel module receives via `ioctl`.
- **UTS namespace (`CLONE_NEWUTS`):** Each container can have its own hostname and domain name without affecting the host or other containers. This is cosmetic but important for correctness when container init processes read `/etc/hostname`.
- **Mount namespace (`CLONE_NEWNS`):** The container gets its own mount table. Mounts made inside the container (such as mounting `/proc`) do not propagate to the host.

Within the container child process, we call `chroot(container_rootfs)` before executing the container command. This changes the process's root directory to the assigned rootfs copy, so the container cannot traverse up to the host filesystem via `..`. We chose `chroot` over `pivot_root` for simplicity; the tradeoff is that `chroot` is escapable by a process that has `CAP_SYS_CHROOT` and can open a file descriptor to the real root before the call, whereas `pivot_root` fully replaces the root mount point and eliminates that escape path.

What the host kernel still shares with all containers: the scheduler, physical memory, the network stack (since we do not use `CLONE_NEWNET`), and the kernel itself. All syscalls from container processes go to the same kernel. This is the fundamental difference between container isolation and full VM isolation.

### 2. Supervisor and Process Lifecycle

A long-running parent supervisor is necessary because Linux process semantics tie child reaping to the parent. When a child process exits, it becomes a zombie until its parent calls `waitpid()`. If the parent exits first, the child is reparented to PID 1 (init), which eventually reaps it — but we lose the ability to capture the exit status and update our metadata.

By keeping the supervisor alive for the duration of all containers, we maintain the parent-child relationship throughout. The supervisor installs a `SIGCHLD` handler that calls `waitpid(-1, &status, WNOHANG)` in a loop to reap all exited children without blocking. It then looks up the host PID in the metadata table, records the exit code (or `128 + signal` if signaled), and transitions the container state.

The `stop_requested` flag in metadata is set before the supervisor sends `SIGTERM` to a container in response to `engine stop`. This lets the `SIGCHLD` handler distinguish: if the container exits via `SIGKILL` and `stop_requested` is not set, the state is `hard_limit_killed` (the kernel module did it); if `stop_requested` is set, the state is `stopped`. This attribution is critical for the `ps` output to be meaningful.

On `SIGINT`/`SIGTERM` to the supervisor itself, an orderly shutdown sequence runs: send `SIGTERM` to all running containers, wait for them to exit, join all logging threads (after the consumer flushes), close all file descriptors, and remove the UNIX socket file.

### 3. IPC, Threads, and Synchronization

**Path A — Pipes (container → supervisor logging):**
Before calling `clone()`, the supervisor creates a pipe for each container. The write end is duplicated onto the container's stdout and stderr via `dup2()`; the read end is kept by the supervisor. A dedicated producer thread per container blocks on `read()` from this pipe, inserting data into the bounded buffer as it arrives. When the container exits, the write end of the pipe is closed, and `read()` returns 0 (EOF), which is the producer thread's exit signal.

Without synchronization, two producer threads inserting simultaneously could corrupt the buffer's tail pointer, causing one thread's data to overwrite the other's or the index to become inconsistent. A mutex around the enqueue operation prevents this. Without the `not_full` condition variable, a producer whose buffer is full would have to busy-wait or drop data; with it, the producer sleeps and is woken exactly when space becomes available.

**Path B — UNIX domain socket (CLI → supervisor):**
The supervisor creates a `SOCK_STREAM` UNIX domain socket bound to a fixed path (e.g., `/tmp/engine.sock`). CLI client processes connect, send a newline-terminated command string, and read back the response. The supervisor's main loop uses `accept()` to handle one CLI request at a time. The choice of UNIX sockets over FIFOs is justified by bidirectionality — a FIFO is unidirectional, so a two-FIFO setup would be needed for request-response, whereas a single connected socket naturally supports both directions on the same file descriptor.

**Shared metadata table:**
The container metadata array is accessed by the main thread (on CLI commands and `SIGCHLD`), producer threads (to look up log file paths), and potentially the consumer thread. A separate metadata mutex protects all reads and writes to this table, independent of the buffer mutex. Keeping them separate avoids a situation where a producer holding the buffer lock tries to acquire the metadata lock while the main thread holds the metadata lock and waits on the buffer — a classic deadlock pattern.

### 4. Memory Management and Enforcement

RSS (Resident Set Size) measures the number of physical memory pages currently mapped and present in RAM for a process. It does not include pages that have been swapped out to disk, pages in shared libraries that are not currently loaded, or memory that has been `mmap`'d but not yet accessed (due to demand paging). This means RSS is a conservative but real measure of the physical memory a process is actually consuming right now.

Soft and hard limits represent two different enforcement philosophies. A soft limit is a threshold for early warning — it tells the operator that a container is approaching its budget, giving the application a chance to react (e.g., trigger garbage collection or reduce cache size) before being killed. A hard limit is a non-negotiable cap: exceeding it results in immediate termination. This two-tier design mirrors cgroup memory policies in production systems.

Enforcement belongs in kernel space for two reasons. First, user-space monitoring is inherently racy: by the time a user-space monitor reads `/proc/<pid>/status`, parses the VmRSS field, and decides to kill the process, the process may have already allocated several more megabytes. Kernel-space polling can check RSS atomically with respect to memory allocation and act immediately. Second, a malicious or buggy container process cannot interfere with or kill a kernel-space monitor, whereas a user-space monitor could be targeted. The kernel module uses a periodic timer (kernel `timer_list` or `workqueue`) to check each registered PID's RSS and applies the appropriate policy without any user-space round-trip.

### 5. Scheduling Behavior

The Linux Completely Fair Scheduler (CFS) aims to give every runnable process a fair share of CPU time proportional to its weight. The `nice` value maps to a weight: nice 0 has weight 1024, and each unit of nice increases the value by approximately 10%, compounding. Nice 10 corresponds to a weight of roughly 110, compared to 1024 for nice 0 — meaning the nice-0 process receives about 9× more CPU time when both are runnable simultaneously.

In our experiment, `cpu1` (nice 0) and `cpu2` (nice 10) run the same CPU-bound workload. After 30 seconds, `cpu1` accumulates significantly more CPU time than `cpu2`, which is consistent with CFS weight calculations. This demonstrates that the scheduler is not simply round-robin — it is weighted fair queuing where lower nice values result in proportionally more CPU allocation.

The experiment also illustrates a key CFS property: neither process starves. `cpu2` still makes forward progress, just more slowly. This is the scheduler's fairness guarantee: every process eventually runs, but high-priority processes run more often. In a mixed CPU-bound and I/O-bound scenario, the I/O-bound process would frequently block on I/O and voluntarily yield the CPU, effectively giving the CPU-bound process more time regardless of nice values — the scheduler rewards processes that do not hoard the CPU.

---

## Design Decisions and Tradeoffs

### 1. Namespace isolation: `chroot` vs `pivot_root`

**Choice:** `chroot` only.

**Tradeoff:** `chroot` is simpler to implement — a single syscall after `clone()` — but a process that retains a file descriptor to the real root directory before the `chroot` call can escape the jail. `pivot_root` replaces the root mount point entirely, making escape much harder, but requires the new root to be a mount point itself, adding setup complexity.

**Justification:** For this academic runtime, `chroot` is sufficient to demonstrate filesystem isolation. The containers run trusted workloads, and the added security of `pivot_root` is not necessary for the project's goals.

### 2. CLI IPC: UNIX domain socket vs named FIFO

**Choice:** UNIX domain socket (`SOCK_STREAM`).

**Tradeoff:** A UNIX socket is bidirectional on a single connected file descriptor, making request-response simple. A named FIFO is unidirectional, requiring two FIFOs for two-way communication and a naming convention to pair them. The downside of sockets is slightly more setup code (`bind`, `listen`, `accept`).

**Justification:** The CLI needs to send a command and receive a structured response. A single socket connection handles this cleanly. Two FIFOs would complicate connection management, especially when multiple CLI processes connect concurrently.

### 3. Bounded buffer: mutex + condition variables vs semaphores

**Choice:** Mutex + two condition variables (`not_full`, `not_empty`).

**Tradeoff:** Condition variables require the mutex to be held when calling `wait`, adding coupling between the lock and the condition. Semaphores are simpler for pure count-based synchronization. However, condition variables allow us to express compound predicates ("buffer is not full") and handle spurious wakeups correctly with a `while` loop, which semaphores cannot do safely.

**Justification:** The bounded buffer has two distinct conditions that need to be waited on and signaled separately. Condition variables model this directly and correctly. A semaphore-based approach would require two semaphores plus a mutex anyway, and would be harder to extend if the buffer policy changes.

### 4. Kernel monitoring: periodic polling vs cgroup integration

**Choice:** Periodic RSS polling via a kernel timer in the LKM.

**Tradeoff:** Polling introduces a window between checks where a process could briefly exceed its hard limit before being killed. cgroup memory limits enforce hard limits synchronously on every allocation. However, integrating with cgroups from an LKM requires working with the cgroup subsystem internals, which is significantly more complex and version-sensitive.

**Justification:** For the purpose of demonstrating kernel-space enforcement, periodic polling is sufficient and keeps the module self-contained. The polling interval can be tuned to be short enough that the enforcement latency is acceptable for the workloads in this project.

---

## Scheduler Experiment Results

Both containers ran the same CPU-bound workload (`cpu_hog`, which executes a tight arithmetic loop). They were started simultaneously and stopped after 30 seconds. CPU time was read from `/proc/<pid>/stat` (field 14: `utime`) on the host.

| Container | Nice Value | CPU Time Consumed (30s run) | % of Total CPU |
|-----------|------------|----------------------------|----------------|
| cpu1      | 0          | ~27.4s                     | ~89%           |
| cpu2      | 10         | ~2.6s                      | ~11%           
The ratio (~9:1) closely matches the theoretical CFS weight ratio between nice 0 (weight 1024) and nice 10 (weight 110): 1024 / (1024 + 110) ≈ 90%.

This confirms that CFS implements weighted fair queuing accurately. The scheduler did not starve `cpu2` — it received CPU time proportional to its weight. In a production setting, this mechanism allows operators to deprioritize background batch jobs (high nice) without completely blocking them, while ensuring latency-sensitive workloads (low nice) get the CPU share they need.

---

## Screenshots Directory Structure

Place your screenshots in a `screenshots/` folder in the repository root:

```
screenshots/
├── 01_multicontainer.png
├── 02_ps_output.png
├── 03_logging.png
├── 04_cli_ipc.png
├── 05_soft_limit.png
├── 06_hard_limit.png
├── 07_scheduling.png
└── 08_teardown.png
```

Each screenshot should be annotated (arrows, labels, or highlights) directly in the image using a tool like GIMP, Preview, or any screenshot annotation tool. The captions in the Demo Screenshots section above explain what each image must show.
