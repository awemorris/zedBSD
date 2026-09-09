/* Test-only layout constants emitted by the target compiler; not linked. */
#include <stddef.h>
#include <kern/process.h>
#include <kern/thread.h>
#include "src/hal/amd64/task.h"
char layout_process_threads[offsetof(struct process, threads) + 1];
char layout_thread_next[offsetof(struct thread, proc_next) + 1];
char layout_thread_state[offsetof(struct thread, state) + 1];
char layout_thread_tid[offsetof(struct thread, tid) + 1];
char layout_thread_sched[offsetof(struct thread, sched) + 1];
char layout_thread_task[offsetof(struct thread, task) + 1];
char layout_thread_entry[offsetof(struct thread, kernel_entry) + 1];
char layout_task_rsp[offsetof(struct amd64_task, resume_rsp) + 1];
char layout_sched_cpu[offsetof(struct sched, cpu) + 1];
char layout_sched_priority[offsetof(struct sched, priority) + 1];
char layout_sched_queue[offsetof(struct sched, queue_kind) + 1];
