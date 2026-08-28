/* Host fixture shim; production builds use include/kern/thread.h. */
#ifndef ZEDBSD_WS004_HOST_THREAD_H
#define ZEDBSD_WS004_HOST_THREAD_H

struct thread;
struct thread *thread_current(void);

#endif
