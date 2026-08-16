# zedBSD POSIX R2 profile

Copyright (C) 2026 Awe Morris
SPDX-License-Identifier: Zlib

The POSIX R2 milestone is a zedBSD development profile. It is not a claim of
POSIX certification. The public baseline remains POSIX.1-2008, with selected
Issue 8 interfaces where the kernel and libc can provide tested semantics.

The profile includes descriptor event waiting, console TTY and foreground
process groups, queued and thread-directed signals, pthread lifecycle and
synchronization, AF_UNIX streams and datagrams, descriptor passing, POSIX
shared memory, semaphores and message queues. The authoritative machine-readable
inventory is `tests/posix-r2-api.csv`; `scripts/check-posix-api-matrix.py`
validates its source and test references.

The following option groups are deliberately not advertised:

- POSIX per-process timers;
- asynchronous and prioritized I/O;
- realtime scheduling and priority protocols;
- typed memory objects;
- robust mutexes, XSI/System V IPC and STREAMS.

Current implementation boundaries are intentional and visible:

- `/tmp`, `/run` and `/shm` use a small flat tmpfs; hierarchical tmpfs and
  `/dev/shm` are future work.
- AF_UNIX pathname endpoints use a kernel registry rather than persistent VFS
  socket inodes. Closing the last endpoint removes the name.
- `waitid()` supports `P_PID` and `P_ALL` with `WEXITED`; `WNOWAIT` is not
  implemented.
- asynchronous pthread cancellation is observed at zedBSD libc cancellation
  points. It does not redirect an arbitrary running user instruction.
- POSIX record locks, FIFOs and resource-limit mutation are outside this
  milestone and must not be inferred from the R2 marker.

Unsupported option groups return `ENOTSUP` where an entry point exists or are
absent and advertised as `-1`. Kernel functions return positive errno values;
the syscall boundary returns negative errno; ordinary libc functions return
their POSIX sentinel and set thread-local `errno`. pthread functions return the
error number directly.

The integration marker `R2:01-06:PASS` demonstrates the selected profile on a
running kernel. It does not replace the API matrix or the limitations above.
