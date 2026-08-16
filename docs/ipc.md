# zedBSD local IPC

Copyright (C) 2026 Awe Morris
SPDX-License-Identifier: Zlib

AF_UNIX supports stream and datagram sockets, `socketpair()`, pathname bind and
connect, poll readiness, `sendmsg()`/`recvmsg()` and `SCM_RIGHTS`. File
descriptors are transferred as referenced kernel file objects and installed as
new descriptors in the receiver. `MSG_CMSG_CLOEXEC` is honored.

The current pathname implementation is a kernel registry. It deliberately does
not create VFS socket inodes, so endpoint names do not survive the last close.
Descriptor-table exhaustion during `SCM_RIGHTS` receive is a known limitation:
the receive fails but currently consumes that message.

POSIX shared memory and named semaphores use tmpfs-backed objects. Unlink
removes the name while existing open mappings or descriptors retain the object.
Unnamed process-shared semaphores and pthread objects identify wait channels by
shared VM object and object offset.

POSIX message queues are fixed-layout shared objects with priority-ordered
messages, blocking/nonblocking and timed operations. `mq_notify()` implements
one-shot `SIGEV_SIGNAL` notification on an empty-to-nonempty transition.
`SIGEV_THREAD` is not implemented.

System V IPC is outside the R2 profile.
