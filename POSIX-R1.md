# zedBSD POSIX R1 compatibility profile

Copyright (C) 2026 Awe Morris
SPDX-License-Identifier: Zlib

This document inventories the user-visible interfaces implemented by the
current kernel, UAPI, and static libc. It is an incremental compatibility
profile, not a claim of POSIX certification. The authoritative syscall numbers
are in `include/uapi/zedbsd/syscall.h`; the all-architecture runtime gate is
`scripts/test-posix-r1.sh`.

## Process, identity, and signals

- `_exit`, `fork`, `execve`, `wait`, `waitpid`, `getpid`, and `getppid`.
- `getpgrp`, `getpgid`, `setpgid`, `setsid`, and `getsid`, including orphaned
  stopped-process-group `SIGHUP`/`SIGCONT` behavior.
- `getuid`, `geteuid`, `getgid`, `getegid`, `getgroups`, `setuid`, `seteuid`,
  `setgid`, `setegid`, `setgroups`, `setreuid`, and `setregid`.
- `sigaction`, `sigprocmask`, `sigpending`, `sigsuspend`, `kill`, and the
  private `sigreturn` trampoline contract. `SA_SIGINFO`, nested delivery,
  synchronous fault information, `SIGCHLD` information, and restartable
  blocking calls are covered by the R1 runtime test.

`SA_ONSTACK` and `sigaltstack` are not part of R1. A signal handler may update
`uc_sigmask`; replacing the saved machine register context is intentionally
rejected.

## File descriptors and I/O

- `open`, `openat`, `close`, `read`, `write`, `pread`, `pwrite`, `readv`,
  `writev`, `lseek`, `fsync`, and `fdatasync`.
- `dup`, `dup2`, `dup3`, `fcntl`, `pipe`, and `pipe2`. Implemented `fcntl`
  commands are `F_DUPFD`, `F_DUPFD_CLOEXEC`, `F_GETFD`, `F_SETFD`, `F_GETFL`,
  and `F_SETFL`.
- `fstat`, `getdents` (used by the libc directory API), and device `ioctl`.
- `O_APPEND`, `O_CLOEXEC`, and supported nonblocking file/socket paths follow
  the shared open-file-description contract.

There is no `poll`, `select`, `epoll`, or kqueue interface in R1. Terminal job
control and a general TTY subsystem remain later work.

## Namespace, attributes, and permissions

- `chdir`, `getcwd`, `stat`, `lstat`, `fstatat`, `access`, and `faccessat`.
- `mkdir`, `mkdirat`, `unlink`, `unlinkat`, `rmdir`, `rename`, and `renameat`.
- `linkat`, `symlinkat`, and `readlinkat`.
- `truncate`, `ftruncate`, `umask`, `chmod`, `fchmod`, `fchmodat`, `chown`,
  `fchown`, `lchown`, `fchownat`, `utimensat`, and `futimens`.

Generic VFS permission and credential checks are implemented. Filesystem
capabilities still apply: FAT cannot persist the full Unix owner/mode/link
model and returns the relevant capability error, while the supported UFS1
profile persists these attributes. FAT12/16 names are normalized according to
the zedBSD FAT policy; FAT32 LFN behavior is filesystem-specific.

## Virtual memory and time

- `mmap`, `munmap`, `mprotect`, `msync`, and `brk`/`sbrk`.
- Anonymous and file-backed private/shared mappings, strict commit accounting,
  retained writeback errors, and past-EOF `SIGBUS` are part of the tested R1
  VM contract. zedBSD does not overcommit committed anonymous memory.
- `clock_gettime`, `clock_getres`, `clock_settime`, and `nanosleep` use the
  time64 UAPI. Supported clocks include the R1 realtime and monotonic paths.

## Sockets

- `socket`, `bind`, `connect`, `listen`, `accept`, `sendto`, `recvfrom`,
  `shutdown`, `getsockname`, `getpeername`, `setsockopt`, and `getsockopt`.
- AF_INET provides IPv4, ICMP/raw, UDP, and active/passive TCP paths. AF_PACKET
  provides the zedBSD L2 raw-socket contract.
- Blocking/nonblocking behavior, timeouts, close wakeup, bind rollback,
  `SO_REUSEADDR`, listener backlog, accepted-child lifetime, and stale TCP
  timeout events have host coverage.

AF_UNIX, IPv6, ancillary data, and advanced routing/socket options are outside
R1.

## zedBSD extensions

- `spawn` is the early direct process-creation convenience interface retained
  alongside `fork` plus `execve`.
- `sysctl` supplies a BSD-style numeric/name kernel-control interface. The
  current `/bin/sysctl` exposes the buffer-cache limit and statistics.
- `/dev/system`, `/dev/console`, and `/dev/graphics` use zedBSD-specific ioctl
  contracts. `/dev/system` includes the resource snapshot used by SMP tests.

## Filesystem scope

FAT16 is the boot filesystem. Normal images contain a separate UFS1 root. The
read-write UFS allocator deliberately accepts only the canonical zedBSD
single-cylinder-group profile with 512-byte device sectors, 8192-byte blocks,
and 1024-byte fragments. UFS2, journal/soft updates/WAPBL, snapshot, quota,
ACL, and extended-attribute features are unsupported and are not silently
mounted read-write.

## Validation

The current completion gate is:

```sh
./build.sh check pc98
./build.sh check pcat
./build.sh check amd64
./build.sh check arm64
./build.sh check sparcv9
./scripts/test-posix-r1.sh all
./scripts/test-amd64-smp.sh
./scripts/test-amd64-smp-stress.sh
```

The runtime matrix covers PC-98/i386, PC/AT i386, amd64, aarch64, and sparcv9.
Passing this project profile must not be described as passing an external POSIX
conformance suite.
