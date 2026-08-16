# zedBSD pthread implementation

Copyright (C) 2026 Awe Morris
SPDX-License-Identifier: Zlib

`pthread_t` is the zedBSD thread identifier. Kernel thread primitives create,
join, detach, cancel and direct signals; libc owns pthread policy, attributes,
cleanup handlers, thread-specific data and synchronization object layouts.
HAL tasks remain CPU-context objects and do not acquire pthread semantics.

By default libc maps a guard page followed by a usable stack. A caller may
supply its own stack with `pthread_attr_setstack()`; libc never unmaps a
caller-owned stack. Detached threads are reclaimed by the process reaper, while
joinable threads retain their exit value until one successful join.

Mutexes, condition variables, rwlocks, barriers, spin locks and semaphores use
the `usync` syscall. Process-shared keys derive from the shared VM object and
offset, not merely the virtual address. Condition variables retain their
selected clock and reacquire the mutex before returning after wakeup, timeout
or cancellation.

Cancellation is deferred by default. Blocking zedBSD libc operations call
`pthread_testcancel()` at defined cancellation points and execute cleanup
handlers in LIFO order. The asynchronous mode is accepted, but cancellation is
still delivered at the next libc checkpoint; true arbitrary-instruction
redirection is not part of this milestone.

`pthread_atfork()` runs prepare handlers in reverse registration order and
parent/child handlers in registration order. The child resets libc-private
locks after a multithreaded fork so that the single surviving thread does not
inherit an unreachable owner.
