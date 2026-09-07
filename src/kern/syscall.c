/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The system call layer.
 *
 * syscall_dispatch() is the handler the HAL invokes for every user trap.
 * It copies the arguments, runs one sys_*_call() handler, and applies the
 * restart policy: a handler interrupted by a transparent stop is
 * redispatched with its original arguments, an interruptible handler
 * that returned EINTR is restarted after a handler with SA_RESTART, and
 * a wait deadline survives the redispatch.  Handlers reach user memory
 * only through copyin/copyout and pinned uaccess windows.
 */

#include "kern/io-pool.h"
#include "kern/syscall.h"
#include "kern/file.h"
#include "kern/io-stats.h"
#include "kern/filedesc.h"
#include "kern/cred.h"
#include "kern/clock.h"
#include "kern/inode.h"
#include "kern/exec.h"
#include "kern/kmem.h"
#include "kern/namei.h"
#include "kern/namecache.h"
#include "kern/net/packet-buf.h"
#include "kern/net/socket.h"
#include "kern/process.h"
#include "kern/process-timer.h"
#include "kern/record-lock.h"
#include "kern/resource-limit.h"
#include "kern/pipe.h"
#include "kern/poll.h"
#include "kern/page.h"
#include "kern/sched.h"
#include "kern/signal.h"
#include "kern/sysctl.h"
#include "kern/thread.h"
#include "kern/uaccess.h"
#include "kern/usync.h"
#include "kern/vm-object.h"
#include "kern/vmspace.h"

#include <zedbsd/dirent.h>
#include <zedbsd/atomic.h>
#include <zedbsd/fcntl.h>
#include <zedbsd/resource.h>
#include <zedbsd/syscall.h>
#include <zedbsd/process.h>
#include <zedbsd/poll.h>
#include <zedbsd/quota.h>
#include <zedbsd/snapshot.h>
#include <zedbsd/select.h>
#include <zedbsd/usync.h>
#include <zedbsd/thread.h>
#include <errno.h>
#include <fcntl.h>
#include <hal/hal.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/mount.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>

#define SYSCALL_IO_CHUNK 512U
#define SYSCALL_SOCKET_BUFFER_MAX (64U * 1024U)
#define SOCKET_SEND_FLAGS (MSG_DONTWAIT | MSG_NOSIGNAL)
#define SOCKET_RECV_FLAGS (MSG_DONTWAIT | MSG_PEEK | MSG_TRUNC | MSG_WAITALL)
#define SYSCALL_SOCKET_OPTION_MAX 128U
#define SYSCALL_PAGE_MASK (ZEDBSD_PAGE_SIZE - 1U)
#define SYSCALL_ATOMIC_CHUNK 128U
#define SYSCALL_EXT __attribute__((section(".hightext")))
#define SYSCALL_SYSCTL_VALUE_MAX 512U
#define SYSCALL_SYSCTL_OUTPUT_MAX (1024U * 1024U)

#define SIGNAL_VALID_MASK ((sigset_t)(UINT64_MAX >> 1U))
#define POLL_SIGNAL_BIT(n) \
	((sigset_t)1ULL << ((unsigned)(n) - 1U))

_Static_assert(KERN_PIPE_BUF <= SYSCALL_IO_CHUNK,
    "writev PIPE_BUF coalescing buffer is too small");

struct poll_mask_guard {
	struct thread *thread;
	sigset_t saved;
	unsigned active;
};

struct sockaddr_output_pin {
	struct uaccess_pin address;
	struct uaccess_pin length;
	socklen_t capacity;
};

struct syscall_exec_args {
	char *argv[ZEDBSD_SPAWN_ARG_MAX + 1U];
	char *envp[ZEDBSD_SPAWN_ENV_MAX + 1U];
	char strings[ZEDBSD_SPAWN_STRING_MAX];
	size_t used;
};

struct syscall_inode_ref {
	struct inode *inode;
	struct file *file;
	struct file *held;
	struct path path;
	int has_path;
};

#ifdef ZEDBSD_USER_ABI_LP64
struct syscall_iovec {
	uint64_t base;
	uint64_t length;
};
#else
struct syscall_iovec {
	uint32_t base;
	uint32_t length;
};
#endif

static struct mutex user_atomic_lock;

#ifdef ZEDBSD_SYSCALL_STOP_TEST
#endif

static int poll_mask_enter(uintptr_t address, struct poll_mask_guard *guard);
static void poll_mask_leave(struct poll_mask_guard *guard, int defer_restore);
static int poll_mask_defer_restore(const struct poll_mask_guard *guard, int error);
#ifdef ZEDBSD_SYSCALL_STOP_TEST
static int syscall_stop_should_redispatch(enum signal_stop_return_result result);
#endif
static int poll_timeout(uintptr_t address, uint64_t *deadline, int *immediate);
static intptr_t sys_ppoll_call(const uintptr_t args[6]);
static int pselect_pin(uintptr_t address, struct uaccess_pin *pin, fd_set *value);
static intptr_t sys_pselect_call(const uintptr_t args[6]);
static intptr_t sys_sysctl_call(const uintptr_t args[6]);
static struct process *current_process(void);
static struct ucred * peercred_snapshot_ref(struct process *process, struct zedbsd_peercred *snapshot);
static int descriptor_socket(struct process *process, int descriptor, struct socket_file_ref *reference);
static intptr_t socket_result(struct socket_file_ref *reference, intptr_t result);
static int copy_sockaddr_in(uintptr_t address, socklen_t length, struct sockaddr_storage *storage);
static int copy_sockaddr_out(uintptr_t address, uintptr_t length_address, const struct sockaddr_storage *storage, socklen_t actual);
static void sockaddr_output_unpin(struct sockaddr_output_pin *pin);
static int sockaddr_output_pin(uintptr_t address, uintptr_t length_address, struct sockaddr_output_pin *pin);
static int copy_sockaddr_out_pinned(const struct sockaddr_output_pin *pin, const struct sockaddr_storage *storage, socklen_t actual);
static intptr_t sys_socket_call(const uintptr_t args[6]);
static intptr_t sys_socketpair_call(const uintptr_t args[6]);
static intptr_t sys_bind_call(const uintptr_t args[6]);
static intptr_t sys_connect_call(const uintptr_t args[6]);
static intptr_t sys_listen_call(const uintptr_t args[6]);
static intptr_t sys_accept_call(const uintptr_t args[6]);
static intptr_t sys_sendto_call(const uintptr_t args[6]);
static intptr_t sys_recvfrom_call(const uintptr_t args[6]);
static intptr_t sys_sendmsg_call(const uintptr_t args[6]);
static intptr_t sys_recvmsg_call(const uintptr_t args[6]);
static intptr_t sys_shutdown_call(const uintptr_t args[6]);
static intptr_t sys_socket_name_call(const uintptr_t args[6], int peer);
static intptr_t sys_setsockopt_call(const uintptr_t args[6]);
static intptr_t sys_getsockopt_call(const uintptr_t args[6]);
static int syscall_context_at(struct process *process, int dirfd, struct cwdinfo *temporary, struct cwdinfo **context, struct file **held);
static intptr_t sys_open_call(const uintptr_t args[6], int at);
static intptr_t sys_close_call(const uintptr_t args[6]);
static SYSCALL_EXT intptr_t sys_read_call(const uintptr_t args[6]);
static uint8_t *syscall_regular_buffer(struct file *file, size_t length, uint8_t *fallback, size_t *capacity);
static SYSCALL_EXT intptr_t sys_write_call(const uintptr_t args[6]);
static intptr_t sys_lseek_call(const uintptr_t args[6]);
static intptr_t sys_fstat_call(const uintptr_t args[6]);
static uint32_t dirent_type(enum inode_type type);
static intptr_t sys_getdents_call(const uintptr_t args[6]);
static intptr_t sys_chdir_call(const uintptr_t args[6]);
static intptr_t sys_getcwd_call(const uintptr_t args[6]);
static int vm_prot(int prot, uint32_t *result);
static intptr_t sys_mmap_call(const uintptr_t args[6]);
static intptr_t sys_munmap_call(const uintptr_t args[6]);
static intptr_t sys_mprotect_call(const uintptr_t args[6]);
static intptr_t sys_msync_call(const uintptr_t args[6]);
static intptr_t sys_brk_call(const uintptr_t args[6]);
static intptr_t sys_ioctl_call(const uintptr_t args[6]);
static intptr_t sys_clock_gettime_call(const uintptr_t args[6]);
static intptr_t sys_clock_getres_call(const uintptr_t args[6]);
static intptr_t sys_clock_settime_call(const uintptr_t args[6]);
static intptr_t sys_timer_create_call(const uintptr_t args[6]);
static intptr_t sys_timer_delete_call(const uintptr_t args[6]);
static intptr_t sys_timer_settime_call(const uintptr_t args[6]);
static intptr_t sys_timer_gettime_call(const uintptr_t args[6]);
static intptr_t sys_timer_getoverrun_call(const uintptr_t args[6]);
static intptr_t sys_mount_call(const uintptr_t args[6]);
static intptr_t sys_unmount_call(const uintptr_t args[6]);
static intptr_t sys_statvfs_call(const uintptr_t args[6], int by_fd);
static intptr_t sys_quotactl_call(const uintptr_t args[6]);
static intptr_t sys_snapshotctl_call(const uintptr_t args[6]);
static intptr_t sys_nanosleep_call(const uintptr_t args[6]);
static int copy_exec_vector(uintptr_t address, char **vector, unsigned maximum, struct syscall_exec_args *copy, int optional);
static SYSCALL_EXT intptr_t sys_positional_call(const uintptr_t args[6], int writing);
static SYSCALL_EXT intptr_t sys_vector_call(const uintptr_t args[6], int writing);
static intptr_t sys_fsync_call(const uintptr_t args[6]);
static intptr_t sys_stat_path_call(const uintptr_t args[6], int at, int nofollow);
static intptr_t sys_truncate_call(const uintptr_t args[6], int by_fd);
static SYSCALL_EXT intptr_t sys_mutation_common(uint32_t number, int old_dirfd, uintptr_t old_address, uintptr_t option, int new_dirfd, uintptr_t new_address);
static intptr_t sys_mutation_call(uint32_t number, const uintptr_t args[6]);
static intptr_t sys_mutation_at_call(uint32_t number, const uintptr_t args[6]);
static intptr_t sys_umask_call(const uintptr_t args[6]);
static int replace_cred(struct process *process, struct ucred *replacement);
static intptr_t sys_cred_get_call(uint32_t number, const uintptr_t args[6]);
static int uid_permitted(const struct ucred *cred, uid_t id);
static int gid_permitted(const struct ucred *cred, gid_t id);
static intptr_t sys_cred_getres_call(uint32_t number, const uintptr_t args[6]);
static intptr_t sys_getentropy_call(const uintptr_t args[6]);
static int user_atomic_copy(const struct uaccess_pin *source, struct uaccess_pin *destination, size_t size);
static int user_atomic_equal(const struct uaccess_pin *left, const struct uaccess_pin *right, size_t size, int *equal);
static intptr_t sys_atomic_call(const uintptr_t args[6]);
static intptr_t sys_cred_set_call(uint32_t number, const uintptr_t args[6]);
static intptr_t sys_access_call(const uintptr_t args[6]);
static SYSCALL_EXT int sys_resolve_path_at(struct process *process, int dirfd, uintptr_t address, unsigned namei_flags, struct path *path, struct file **held);
static int sys_inode_ref_acquire(struct process *process, uintptr_t object, int by_fd, int nofollow, struct syscall_inode_ref *reference);
static void sys_inode_ref_release(struct syscall_inode_ref *reference);
static SYSCALL_EXT intptr_t sys_getxattr_call(const uintptr_t args[6], int by_fd, int nofollow);
static SYSCALL_EXT intptr_t sys_setxattr_call(const uintptr_t args[6], int by_fd, int nofollow);
static SYSCALL_EXT intptr_t sys_listxattr_call(const uintptr_t args[6], int by_fd, int nofollow);
static SYSCALL_EXT intptr_t sys_removexattr_call(const uintptr_t args[6], int by_fd, int nofollow);
static int inode_chmod_allowed(const struct inode *inode, const struct ucred *cred);
static SYSCALL_EXT intptr_t sys_chmod_common(int dirfd, uintptr_t pathname, int fd, mode_t mode, int flags);
static SYSCALL_EXT intptr_t sys_chown_common(int dirfd, uintptr_t pathname, int fd, uid_t uid, gid_t gid, int flags);
static int valid_utime_nsec(long nanoseconds);
static SYSCALL_EXT intptr_t sys_utimens_common(int dirfd, uintptr_t pathname, int fd, uintptr_t times_address, int flags);
static SYSCALL_EXT intptr_t sys_faccessat_call(const uintptr_t args[6]);
static SYSCALL_EXT int sys_parent_path_at(struct process *process, int dirfd, const char *pathname, struct cwdinfo *temporary, struct cwdinfo **context, struct file **held, struct path *parent, struct componentname *name, char storage[NAME_MAX + 1U]);
static SYSCALL_EXT intptr_t sys_linkat_call(const uintptr_t args[6]);
static SYSCALL_EXT intptr_t sys_symlinkat_call(const uintptr_t args[6]);
static SYSCALL_EXT intptr_t sys_readlinkat_call(const uintptr_t args[6]);
static intptr_t sys_sigaction_call(const uintptr_t args[6]);
static intptr_t sys_sigprocmask_call(const uintptr_t args[6]);
static intptr_t sys_sigpending_call(const uintptr_t args[6]);
static SYSCALL_EXT intptr_t sys_mknodat_call(const uintptr_t args[6]);
static intptr_t sys_fchdir_call(const uintptr_t args[6]);
static intptr_t sys_sigaltstack_call(const uintptr_t args[6]);
static intptr_t sys_sigtimedwait_call(const uintptr_t args[6]);
static intptr_t sys_sigqueue_call(const uintptr_t args[6]);
static intptr_t sys_thread_create_call(const uintptr_t args[6]);
static intptr_t sys_thread_exit_call(const uintptr_t args[6]);
static int thread_join_claim_locked(struct thread *target, tid_t owner, unsigned stop_redispatch);
static void thread_join_release_locked(struct thread *target, tid_t owner);
static intptr_t sys_thread_join_call(const uintptr_t args[6]);
static intptr_t sys_thread_detach_call(const uintptr_t args[6]);
static intptr_t sys_thread_self_call(const uintptr_t args[6]);
static intptr_t sys_thread_kill_call(const uintptr_t args[6]);
static intptr_t sys_thread_cancel_call(const uintptr_t args[6]);
static intptr_t sys_usync_call(const uintptr_t args[6]);
static intptr_t sys_sigsuspend_call(const uintptr_t args[6]);
static intptr_t sys_sigreturn_call(const uintptr_t args[6]);
static intptr_t sys_dup_call(const uintptr_t args[6]);
static intptr_t sys_dup2_call(const uintptr_t args[6], int is_dup3);
static intptr_t sys_fcntl_call(const uintptr_t args[6]);
static intptr_t sys_pipe2_call(const uintptr_t args[6], int plain);
static intptr_t sys_fork_call(const uintptr_t args[6]);
static intptr_t sys_sched_yield_call(const uintptr_t args[6]);
static intptr_t sys_times_call(const uintptr_t args[6]);
static int priority_matches(struct process *target, struct process *caller, int which, id_t who);
static intptr_t sys_getpriority_call(const uintptr_t args[6]);
static intptr_t sys_setpriority_call(const uintptr_t args[6]);
static void ticks_to_timeval(uint64_t ticks, struct timeval *value);
static intptr_t sys_getrusage_call(const uintptr_t args[6]);
static int timeval_ticks(const struct timeval *value, uint64_t *ticks);
static void timer_snapshot(struct process *process, int which, struct itimerval *value);
static intptr_t sys_getitimer_call(const uintptr_t args[6]);
static intptr_t sys_setitimer_call(const uintptr_t args[6]);
static intptr_t sys_execve_call(const uintptr_t args[6]);
static intptr_t sys_fexecve_call(const uintptr_t args[6]);
static SYSCALL_EXT intptr_t sys_waitpid_call(const uintptr_t args[6]);
static SYSCALL_EXT intptr_t sys_waitid_call(const uintptr_t args[6]);
static SYSCALL_EXT intptr_t sys_resource_limit_call(const uintptr_t args[6], int setting);
static intptr_t sys_process_identity_call(uint32_t number, const uintptr_t args[6]);
static intptr_t syscall_dispatch_body(uint32_t number, const uintptr_t args[6]);
static int syscall_restartable(uint32_t number);
static intptr_t syscall_dispatch(uint32_t number, const uintptr_t args[6]);

/*
 * Clears the restart bookkeeping of a thread at the start of a system
 * call.
 */
void
syscall_restart_state_begin(
	struct thread *thread)
{
	if (thread == NULL)
		return;
	thread->syscall_stop_redispatch = 0;
	thread->syscall_wait_deadline = 0;
	thread->syscall_wait_deadline_valid = 0;
}

/*
 * Clears the restart bookkeeping of a thread at the end of a system
 * call.
 */
void
syscall_restart_state_finish(
	struct thread *thread)
{
	if (thread == NULL)
		return;
	thread->syscall_stop_redispatch = 0;
	thread->syscall_wait_deadline = 0;
	thread->syscall_wait_deadline_valid = 0;
}

/*
 * Computes a fresh wait deadline and records it for a later redispatch.
 */
int
syscall_restart_deadline_rearm(
	uint64_t ticks,
	uint64_t *deadline)
{
	struct thread *thread;
	int error;

	thread = curthread;
	if (deadline == NULL)
		return EINVAL;
	error = kern_deadline_after(sched_ticks(), ticks, deadline);
	if (error == 0 && thread != NULL) {
		thread->syscall_wait_deadline = *deadline;
		thread->syscall_wait_deadline_valid = 1;
	}
	return error;
}

/*
 * Reports the wait deadline of a system call, reusing the one recorded
 * before a transparent stop.
 */
int
syscall_restart_deadline_after(
	uint64_t ticks,
	uint64_t *deadline)
{
	struct thread *thread;
	int error;

	thread = curthread;
	if (deadline == NULL)
		return EINVAL;
	if (thread != NULL &&
	    thread->syscall_stop_redispatch &&
	    thread->syscall_wait_deadline_valid) {
		*deadline = thread->syscall_wait_deadline;
		return 0;
	}
	error = syscall_restart_deadline_rearm(ticks, deadline);
	return error;
}

/*
 * Prepares a thread to redispatch its system call after a transparent
 * stop.
 */
void
syscall_restart_prepare_stop(
	struct thread *thread)
{
	struct process *process;
	unsigned long irq;

	if (thread == NULL)
		return;

	/*
	 * ppoll()/pselect() defer restoration for an ordinary caught
	 * signal.  A transparent stop does not cross a user-handler
	 * boundary, so restore now before re-entering the syscall.
	 */
	process = thread->proc;
	if (process != NULL) {
		irq = spin_lock_irqsave(&process->lock);
		if (thread->signal_suspended) {
			thread->signal_mask = thread->signal_suspend_mask;
			thread->signal_suspended = 0;
		}
		spin_unlock_irqrestore(&process->lock, irq);
	}
	thread->syscall_stop_redispatch = 1;
}

#ifdef ZEDBSD_SYSCALL_STOP_TEST
/*
 * Simulates the stop-redispatch decision of the dispatcher for a test.
 */
intptr_t
syscall_test_stop_ready_cycle(
	enum signal_stop_return_result stop_result,
	intptr_t redispatched_result,
	int *body_calls)
{
	if (body_calls == NULL)
		return -EINVAL;
	*body_calls = 1;
	if (!syscall_stop_should_redispatch(stop_result))
		return -EINTR;
	(*body_calls)++;
	return redispatched_result;
}

/*
 * Runs one temporary signal mask cycle for a test.
 */
int
syscall_test_poll_mask_cycle(
	uint64_t requested,
	int error,
	unsigned stop_interrupted)
{
	struct poll_mask_guard guard;
	int enter_error;

	enter_error = poll_mask_enter((uintptr_t)&requested, &guard);
	if (enter_error != 0)
		return enter_error;
	guard.thread->stop_interrupted = stop_interrupted;
	poll_mask_leave(&guard, poll_mask_defer_restore(&guard, error));
	return 0;
}

/*
 * Claims a thread for joining on behalf of a test.
 */
int
syscall_test_thread_join_claim(
	struct thread *target,
	tid_t owner,
	unsigned stop_redispatch)
{
	int error;

	if (target == NULL)
		return EINVAL;
	error = thread_join_claim_locked(target, owner, stop_redispatch);
	return error;
}

/*
 * Releases a join claim on behalf of a test.
 */
void
syscall_test_thread_join_release(
	struct thread *target,
	tid_t owner)
{
	if (target != NULL)
		thread_join_release_locked(target, owner);
}

/*
 * Runs the thread join handler for a test.
 */
intptr_t
syscall_test_thread_join_call(
	const uintptr_t args[6])
{
	intptr_t result;

	result = sys_thread_join_call(args);
	return result;
}

/*
 * Runs the thread cancel handler for a test.
 */
intptr_t
syscall_test_thread_cancel_call(
	const uintptr_t args[6])
{
	intptr_t result;

	result = sys_thread_cancel_call(args);
	return result;
}
#endif

/*
 * Initializes the system call layer and registers the dispatcher.
 */
void
syscall_init(
	void)
{
	poll_init();
	usync_init();
	(void)mutex_init(&user_atomic_lock, LOCK_RANK_USER_ATOMIC,
	    "user atomic");
	hal_syscall_set_handler(syscall_dispatch);
	signal_init();
}

/* Installs a temporary signal mask for ppoll() or pselect(). */
static int
poll_mask_enter(
	uintptr_t address,
	struct poll_mask_guard *guard)
{
	struct process *process;
	sigset_t requested;
	unsigned long irq;
	int error;

	memset(guard, 0, sizeof(*guard));
	if (address == 0)
		return 0;
	error = copyin(address, &requested, sizeof(requested));
	if (error != 0)
		return error;
	guard->thread = curthread;
	if (guard->thread != NULL)
		process = guard->thread->proc;
	else
		process = NULL;
	if (process == NULL)
		return EINVAL;

	/* SIGKILL and SIGSTOP can never be masked. */
	requested &= SIGNAL_VALID_MASK &
	    ~(POLL_SIGNAL_BIT(SIGKILL) | POLL_SIGNAL_BIT(SIGSTOP));
	irq = spin_lock_irqsave(&process->lock);
	guard->saved = guard->thread->signal_mask;
	guard->thread->signal_mask = requested;
	guard->active = 1;
	spin_unlock_irqrestore(&process->lock, irq);
	return 0;
}

/* Restores the signal mask saved by poll_mask_enter(), now or at handler entry. */
static void
poll_mask_leave(
	struct poll_mask_guard *guard,
	int defer_restore)
{
	struct process *process;
	unsigned long irq;

	if (guard == NULL || !guard->active || guard->thread == NULL)
		return;
	process = guard->thread->proc;
	if (process == NULL)
		return;
	irq = spin_lock_irqsave(&process->lock);
	if (defer_restore) {
		/*
		 * Keep the temporary mask installed until the selected
		 * handler is entered.  Restoring it here would reopen the
		 * classic pselect()/ppoll() lost-signal window.
		 */
		guard->thread->signal_suspend_mask = guard->saved;
		guard->thread->signal_suspended = 1;
	} else {
		guard->thread->signal_mask = guard->saved;
	}
	guard->active = 0;
	spin_unlock_irqrestore(&process->lock, irq);
}

/* Tests whether a poll ended by a caught signal must keep its mask until the handler runs. */
static int
poll_mask_defer_restore(
	const struct poll_mask_guard *guard,
	int error)
{
	if (error != EINTR)
		return 0;
	if (guard == NULL)
		return 0;
	if (guard->thread == NULL)
		return 0;
	if (guard->thread->stop_interrupted)
		return 0;
	return 1;
}

#ifdef ZEDBSD_SYSCALL_STOP_TEST
/* Tests whether a stop result asks for a redispatch. */
static int
syscall_stop_should_redispatch(
	enum signal_stop_return_result result)
{
	if (result == SIGNAL_STOP_RETURN_REDISPATCH)
		return 1;
	return 0;
}
#endif

/* Converts a user timeout into a deadline, reusing one saved across a stop. */
static int
poll_timeout(
	uintptr_t address,
	uint64_t *deadline,
	int *immediate)
{
	struct timespec timeout;
	uint64_t ticks;
	int error;

	*deadline = 0;
	*immediate = 0;
	if (address == 0)
		return 0;
	if (curthread != NULL &&
	    curthread->syscall_stop_redispatch &&
	    curthread->syscall_wait_deadline_valid) {
		*deadline = curthread->syscall_wait_deadline;
		return 0;
	}
	error = copyin(address, &timeout, sizeof(timeout));
	if (error != 0)
		return error;
	error = kern_duration_to_ticks_ceil(&timeout, &ticks);
	if (error != 0)
		return error;
	if (ticks == 0) {
		*immediate = 1;
		return 0;
	}
	error = syscall_restart_deadline_after(ticks, deadline);
	return error;
}

/* Handles ppoll(2). */
static intptr_t
sys_ppoll_call(
	const uintptr_t args[6])
{
	struct pollfd fds[KERN_OPEN_MAX];
	struct uaccess_pin pin;
	struct poll_mask_guard guard;
	struct process *process;
	nfds_t count;
	uint64_t deadline;
	int immediate;
	int ready;
	int error;
	size_t bytes;

	process = current_process();
	count = (nfds_t)args[1];
	ready = 0;

	/* Pins and copies the descriptor array. */
	memset(&pin, 0, sizeof(pin));
	memset(&guard, 0, sizeof(guard));
	if (args[1] > KERN_OPEN_MAX)
		return -EINVAL;
	bytes = (size_t)count * sizeof(fds[0]);
	if (bytes != 0) {
		error = uaccess_pin(args[0], bytes,
		    HAL_SPACE_READ | HAL_SPACE_WRITE, &pin);
		if (error != 0)
			return -error;
		error = copyin_pinned(&pin, 0, fds, bytes);
		if (error != 0) {
			uaccess_unpin(&pin);
			return -error;
		}
	} else {
		memset(fds, 0, sizeof(fds));
	}

	/* Waits under the temporary mask and copies the results back. */
	error = poll_timeout(args[2], &deadline, &immediate);
	if (error == 0)
		error = poll_mask_enter(args[3], &guard);
	if (error == 0)
		error = kern_poll_wait(process, fds, count, deadline, immediate,
		    &ready);
	if (args[3] != 0)
		poll_mask_leave(&guard, poll_mask_defer_restore(&guard, error));
	if (error == 0 && bytes != 0)
		error = copyout_pinned(&pin, 0, fds, bytes);
	uaccess_unpin(&pin);
	if (error != 0)
		return -error;
	return ready;
}

/* Pins and copies in one optional fd_set of pselect(). */
static int
pselect_pin(
	uintptr_t address,
	struct uaccess_pin *pin,
	fd_set *value)
{
	int error;

	memset(pin, 0, sizeof(*pin));
	memset(value, 0, sizeof(*value));
	if (address == 0)
		return 0;
	error = uaccess_pin(address, sizeof(*value),
	    HAL_SPACE_READ | HAL_SPACE_WRITE, pin);
	if (error != 0)
		return error;
	error = copyin_pinned(pin, 0, value, sizeof(*value));
	return error;
}

/* Handles pselect(2) on top of the poll machinery. */
static intptr_t
sys_pselect_call(
	const uintptr_t args[6])
{
	struct pollfd fds[KERN_OPEN_MAX];
	struct uaccess_pin read_pin;
	struct uaccess_pin write_pin;
	struct uaccess_pin except_pin;
	fd_set input_read;
	fd_set input_write;
	fd_set input_except;
	fd_set output_read;
	fd_set output_write;
	fd_set output_except;
	struct poll_mask_guard guard;
	struct process *process;
	uint64_t deadline;
	uint32_t valid_mask;
	int nfds;
	int immediate;
	int ready;
	int error;
	int result;
	nfds_t count;
	nfds_t i;
	uint32_t bit;
	short events;
	short revents;

	process = current_process();
	nfds = (int)args[0];
	ready = 0;
	result = 0;
	count = 0;

	/* Pins the three sets. */
	memset(&read_pin, 0, sizeof(read_pin));
	memset(&write_pin, 0, sizeof(write_pin));
	memset(&except_pin, 0, sizeof(except_pin));
	memset(&guard, 0, sizeof(guard));
	if (nfds < 0 || nfds > KERN_OPEN_MAX)
		return -EINVAL;
	error = pselect_pin(args[1], &read_pin, &input_read);
	if (error == 0)
		error = pselect_pin(args[2], &write_pin, &input_write);
	if (error == 0)
		error = pselect_pin(args[3], &except_pin, &input_except);
	if (error != 0)
		goto out;

	/* Converts the sets into a pollfd array. */
	if (nfds == 32)
		valid_mask = UINT32_MAX;
	else if (nfds == 0)
		valid_mask = 0U;
	else
		valid_mask = ((uint32_t)1U << (unsigned)nfds) - 1U;
	input_read.bits[0] &= valid_mask;
	input_write.bits[0] &= valid_mask;
	input_except.bits[0] &= valid_mask;
	for (i = 0; i < (nfds_t)nfds; i++) {
		bit = (uint32_t)1U << i;
		events = 0;
		if ((input_read.bits[0] & bit) != 0)
			events |= POLLIN | POLLRDNORM;
		if ((input_write.bits[0] & bit) != 0)
			events |= POLLOUT | POLLWRNORM;
		if ((input_except.bits[0] & bit) != 0)
			events |= POLLPRI;
		if (events != 0) {
			fds[count].fd = (int)i;
			fds[count].events = events;
			fds[count].revents = 0;
			count++;
		}
	}

	/* Waits under the temporary mask. */
	error = poll_timeout(args[4], &deadline, &immediate);
	if (error == 0)
		error = poll_mask_enter(args[5], &guard);
	if (error == 0)
		error = kern_poll_wait(process, fds, count, deadline, immediate,
		    &ready);
	if (args[5] != 0)
		poll_mask_leave(&guard, poll_mask_defer_restore(&guard, error));
	if (error != 0)
		goto out;
	(void)ready;

	/* Converts the results back into sets. */
	memset(&output_read, 0, sizeof(output_read));
	memset(&output_write, 0, sizeof(output_write));
	memset(&output_except, 0, sizeof(output_except));
	for (i = 0; i < count; i++) {
		bit = (uint32_t)1U << (unsigned)fds[i].fd;
		revents = fds[i].revents;
		if ((revents & POLLNVAL) != 0) {
			error = EBADF;
			goto out;
		}
		if ((input_read.bits[0] & bit) != 0 &&
		    (revents & (POLLIN | POLLRDNORM | POLLERR | POLLHUP)) != 0) {
			output_read.bits[0] |= bit;
			result++;
		}
		if ((input_write.bits[0] & bit) != 0 &&
		    (revents & (POLLOUT | POLLWRNORM | POLLERR)) != 0) {
			output_write.bits[0] |= bit;
			result++;
		}
		if ((input_except.bits[0] & bit) != 0 &&
		    (revents & POLLPRI) != 0) {
			output_except.bits[0] |= bit;
			result++;
		}
	}
	if (read_pin.active)
		error = copyout_pinned(&read_pin, 0, &output_read,
		    sizeof(output_read));
	if (error == 0 && write_pin.active)
		error = copyout_pinned(&write_pin, 0, &output_write,
		    sizeof(output_write));
	if (error == 0 && except_pin.active)
		error = copyout_pinned(&except_pin, 0, &output_except,
		    sizeof(output_except));
out:
	uaccess_unpin(&except_pin);
	uaccess_unpin(&write_pin);
	uaccess_unpin(&read_pin);
	if (error != 0)
		return -error;
	return result;
}

/* Handles sysctl(2). */
static intptr_t
sys_sysctl_call(
	const uintptr_t args[6])
{
	int name[CTL_MAXNAME];
	uint8_t old_value[SYSCALL_SYSCTL_VALUE_MAX];
	uint8_t new_value[SYSCALL_SYSCTL_VALUE_MAX];
	uint8_t *old_output;
	size_t old_length;
	unsigned namelen;
	struct process *process;
	uint8_t *old_argument;
	size_t *old_length_argument;
	uint8_t *new_argument;
	int superuser;
	int error;
	int copy_error;

	old_output = old_value;
	old_length = 0;
	namelen = (unsigned)args[1];
	process = current_process();

	/* Copies the name and sizes an output buffer for the old value. */
	if (args[0] == 0 ||
	    namelen == 0 ||
	    namelen > CTL_MAXNAME ||
	    args[5] > sizeof(new_value))
		return -EINVAL;
	error = copyin(args[0], name, namelen * sizeof(name[0]));
	if (error != 0)
		return -error;
	if (args[2] != 0 && args[3] == 0)
		return -EINVAL;
	if (args[3] != 0) {
		error = copyin(args[3], &old_length, sizeof(old_length));
		if (error != 0)
			return -error;
		if (args[2] != 0 && old_length > sizeof(old_value)) {
			if (old_length > SYSCALL_SYSCTL_OUTPUT_MAX)
				return -ENOMEM;
			old_output = kern_malloc(old_length);
			if (old_output == NULL)
				return -ENOMEM;
		}
	}
	if (args[4] != 0) {
		error = copyin(args[4], new_value, (size_t)args[5]);
		if (error != 0)
			goto out;
	} else if (args[5] != 0) {
		error = EINVAL;
		goto out;
	}

	/* Runs the request and copies the old value and its length back. */
	if (args[2] != 0)
		old_argument = old_output;
	else
		old_argument = NULL;
	if (args[3] != 0)
		old_length_argument = &old_length;
	else
		old_length_argument = NULL;
	if (args[4] != 0)
		new_argument = new_value;
	else
		new_argument = NULL;
	superuser = process != NULL && cred_is_superuser(process->cred);
	error = kern_sysctl(name, namelen, old_argument, old_length_argument,
	    new_argument, (size_t)args[5], superuser);
	if (args[3] != 0) {
		copy_error = copyout(&old_length, args[3], sizeof(old_length));
		if (copy_error != 0) {
			error = copy_error;
			goto out;
		}
	}
	if (error == 0 && args[2] != 0 && old_length != 0)
		error = copyout(old_output, args[2], old_length);
out:
	if (old_output != old_value)
		kern_free(old_output);
	if (error != 0)
		return -error;
	return 0;
}

/* Reports the process of the current thread, or NULL. */
static struct process *
current_process(
	void)
{
	if (curthread == NULL)
		return NULL;
	return curthread->proc;
}

/* Takes a credential reference and fills a peer credential snapshot. */
static struct ucred *
peercred_snapshot_ref(
	struct process *process,
	struct zedbsd_peercred *snapshot)
{
	struct ucred *credential;

	if (process == NULL || snapshot == NULL)
		return NULL;
	credential = cred_process_ref(process);
	if (credential == NULL)
		return NULL;
	snapshot->pid = process->pid;
	snapshot->euid = credential->euid;
	snapshot->egid = credential->egid;
	return credential;
}

/* Takes a socket reference from a descriptor. */
static int
descriptor_socket(
	struct process *process,
	int descriptor,
	struct socket_file_ref *reference)
{
	int error;

	if (process == NULL || process->fd == NULL)
		return EBADF;
	error = socket_file_ref_get(process->fd, descriptor, reference);
	return error;
}

/* Drops a socket reference and passes a result through. */
static intptr_t
socket_result(
	struct socket_file_ref *reference,
	intptr_t result)
{
	socket_file_ref_put(reference);
	return result;
}

/* Copies a socket address in from user memory. */
static int
copy_sockaddr_in(
	uintptr_t address,
	socklen_t length,
	struct sockaddr_storage *storage)
{
	int error;

	if (address == 0 ||
	    storage == NULL ||
	    length < sizeof(sa_family_t) ||
	    length > sizeof(*storage))
		return EINVAL;
	memset(storage, 0, sizeof(*storage));
	error = copyin(address, storage, length);
	return error;
}

/* Copies a socket address and its length out to user memory. */
static int
copy_sockaddr_out(
	uintptr_t address,
	uintptr_t length_address,
	const struct sockaddr_storage *storage,
	socklen_t actual)
{
	socklen_t capacity;
	int error;
	socklen_t copied;

	if (address == 0 && length_address == 0)
		return 0;
	if (address == 0 || length_address == 0 || storage == NULL)
		return EINVAL;
	error = copyin(length_address, &capacity, sizeof(capacity));
	if (error != 0)
		return error;
	if (capacity != 0) {
		if (capacity < actual)
			copied = capacity;
		else
			copied = actual;
		error = copyout(storage, address, copied);
		if (error != 0)
			return error;
	}
	error = copyout(&actual, length_address, sizeof(actual));
	return error;
}

/* Releases the pins of a socket address output. */
static void
sockaddr_output_unpin(
	struct sockaddr_output_pin *pin)
{
	if (pin == NULL)
		return;
	uaccess_unpin(&pin->address);
	uaccess_unpin(&pin->length);
}

/* Pins the address and length outputs of a socket call before it may sleep. */
static int
sockaddr_output_pin(
	uintptr_t address,
	uintptr_t length_address,
	struct sockaddr_output_pin *pin)
{
	size_t bytes;
	int error;

	if (pin == NULL)
		return EINVAL;
	memset(pin, 0, sizeof(*pin));
	if (address == 0 && length_address == 0)
		return 0;
	if (address == 0 || length_address == 0)
		return EINVAL;
	error = uaccess_pin(length_address, sizeof(pin->capacity),
	    HAL_SPACE_READ | HAL_SPACE_WRITE, &pin->length);
	if (error != 0)
		return error;
	error = copyin_pinned(&pin->length, 0, &pin->capacity,
	    sizeof(pin->capacity));
	if (error != 0) {
		sockaddr_output_unpin(pin);
		return error;
	}
	if (pin->capacity < sizeof(struct sockaddr_storage))
		bytes = pin->capacity;
	else
		bytes = sizeof(struct sockaddr_storage);
	error = uaccess_pin(address, bytes, PROT_WRITE, &pin->address);
	if (error != 0)
		sockaddr_output_unpin(pin);
	return error;
}

/* Copies a socket address and its length out through pinned outputs. */
static int
copy_sockaddr_out_pinned(
	const struct sockaddr_output_pin *pin,
	const struct sockaddr_storage *storage,
	socklen_t actual)
{
	size_t copied;
	int error;

	if (pin == NULL || storage == NULL)
		return EINVAL;
	if (!pin->length.active)
		return 0;
	if (pin->capacity < actual)
		copied = pin->capacity;
	else
		copied = actual;
	if (copied == 0)
		error = 0;
	else
		error = copyout_pinned(&pin->address, 0, storage, copied);
	if (error == 0)
		error = copyout_pinned(&pin->length, 0, &actual, sizeof(actual));
	return error;
}

/* Handles socket(2). */
static intptr_t
sys_socket_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct socket *socket;
	struct file *file;
	int supplied_type;
	int type;
	int file_flags;
	unsigned descriptor_flags;
	int descriptor;
	int error;

	process = current_process();
	file = NULL;
	supplied_type = (int)args[1];
	type = supplied_type &
	    ~(SOCK_NONBLOCK | SOCK_CLOEXEC | SOCK_CLOFORK);
	if ((supplied_type & SOCK_NONBLOCK) != 0)
		file_flags = O_NONBLOCK;
	else
		file_flags = 0;
	descriptor_flags = 0;
	if ((supplied_type & SOCK_CLOEXEC) != 0)
		descriptor_flags |= FILEDESC_CLOEXEC;
	if ((supplied_type & SOCK_CLOFORK) != 0)
		descriptor_flags |= FILEDESC_CLOFORK;

	/* Raw and packet sockets need privilege. */
	if (process == NULL ||
	    process->fd == NULL ||
	    args[3] != 0 ||
	    args[4] != 0 ||
	    args[5] != 0)
		return -EINVAL;
	if (type != SOCK_RAW && type != SOCK_DGRAM && type != SOCK_STREAM)
		return -EINVAL;
	if (((int)args[0] == AF_PACKET || type == SOCK_RAW) &&
	    !cred_is_superuser(process->cred))
		return -EPERM;

	/* Creates the socket, wraps it in a file, and installs a descriptor. */
	error = socket_create((int)args[0], type, (int)args[2],
	    &socket);
	if (error != 0)
		return -error;
	error = socket_file_create(socket, &file);
	if (error != 0) {
		socket_release(socket);
		return -error;
	}
	file_status_flags_update(file, O_NONBLOCK, file_flags);
	error = filedesc_install_from(process->fd, file, descriptor_flags, 0,
	    &descriptor);
	if (error != 0) {
		(void)file_close(file);
		return -error;
	}
	return descriptor;
}

/* Handles socketpair(2). */
static intptr_t
sys_socketpair_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct ucred *credential;
	struct zedbsd_peercred creator;
	struct socket *left_socket;
	struct socket *right_socket;
	struct file *left_file;
	struct file *right_file;
	int descriptors[2];
	int supplied_type;
	int type;
	int file_flags;
	unsigned descriptor_flags;
	int error;

	process = current_process();
	left_socket = NULL;
	right_socket = NULL;
	left_file = NULL;
	right_file = NULL;
	descriptors[0] = -1;
	descriptors[1] = -1;
	supplied_type = (int)args[1];
	type = supplied_type &
	    ~(SOCK_NONBLOCK | SOCK_CLOEXEC | SOCK_CLOFORK);
	if ((supplied_type & SOCK_NONBLOCK) != 0)
		file_flags = O_NONBLOCK;
	else
		file_flags = 0;
	descriptor_flags = 0;
	if ((supplied_type & SOCK_CLOEXEC) != 0)
		descriptor_flags |= FILEDESC_CLOEXEC;
	if ((supplied_type & SOCK_CLOFORK) != 0)
		descriptor_flags |= FILEDESC_CLOFORK;

	/* Only AF_UNIX stream and datagram pairs exist. */
	if (process == NULL ||
	    process->fd == NULL ||
	    args[3] == 0 ||
	    args[4] != 0 ||
	    args[5] != 0 ||
	    (type != SOCK_STREAM && type != SOCK_DGRAM))
		return -EINVAL;
	if ((int)args[0] != AF_UNIX)
		return -EAFNOSUPPORT;

	/* Creates both sockets and their files. */
	credential = peercred_snapshot_ref(process, &creator);
	if (credential == NULL)
		return -EINVAL;
	error = unix_socket_pair_create(type, (int)args[2], &creator,
	    &left_socket, &right_socket);
	cred_release(credential);
	if (error == 0)
		error = socket_file_create(left_socket, &left_file);
	if (error == 0)
		error = socket_file_create(right_socket, &right_file);
	if (error != 0) {
		if (left_file != NULL)
			(void)file_close(left_file);
		else if (left_socket != NULL)
			socket_release(left_socket);
		if (right_file != NULL)
			(void)file_close(right_file);
		else if (right_socket != NULL)
			socket_release(right_socket);
		return -error;
	}

	/* Installs both descriptors and reports them. */
	file_status_flags_update(left_file, O_NONBLOCK, file_flags);
	file_status_flags_update(right_file, O_NONBLOCK, file_flags);
	error = filedesc_install_pair(process->fd, left_file, descriptor_flags,
	    right_file, descriptor_flags, descriptors);
	if (error == 0)
		error = copyout(descriptors, args[3], sizeof(descriptors));
	if (error != 0) {
		if (descriptors[0] >= 0) {
			(void)filedesc_close(process->fd, descriptors[0]);
			(void)filedesc_close(process->fd, descriptors[1]);
		} else {
			(void)file_close(left_file);
			(void)file_close(right_file);
		}
		return -error;
	}
	return 0;
}

/* Handles bind(2). */
static intptr_t
sys_bind_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct ucred *credential;
	struct socket_file_ref reference;
	struct socket *socket;
	struct sockaddr_storage address;
	intptr_t result;
	int error;

	process = current_process();
	credential = NULL;

	if (descriptor_socket(process, (int)args[0], &reference) != 0)
		return -EBADF;
	socket = reference.socket;
	if (socket->ops == NULL || socket->ops->bind == NULL) {
		result = socket_result(&reference, -EOPNOTSUPP);
		return result;
	}

	/* A path binding goes through the namespace with the caller's credential. */
	error = copy_sockaddr_in(args[1], (socklen_t)args[2], &address);
	if (error == 0 && socket->family == AF_UNIX) {
		credential = cred_process_ref(process);
		if (credential == NULL)
			error = EINVAL;
		else
			error = unix_socket_bind_path(socket, process->cwdi,
			    credential, process->umask,
			    (struct sockaddr *)&address, (socklen_t)args[2]);
		cred_release(credential);
	} else if (error == 0) {
		error = socket->ops->bind(socket, (struct sockaddr *)&address,
		    (socklen_t)args[2]);
	}
	if (error != 0)
		result = -error;
	else
		result = 0;
	result = socket_result(&reference, result);
	return result;
}

/* Handles connect(2). */
static intptr_t
sys_connect_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct ucred *credential;
	struct zedbsd_peercred connector;
	struct socket_file_ref reference;
	struct socket *socket;
	struct sockaddr_storage address;
	intptr_t result;
	int io_flags;
	int error;

	process = current_process();
	credential = NULL;

	if (descriptor_socket(process, (int)args[0], &reference) != 0)
		return -EBADF;
	socket = reference.socket;
	if (socket->ops == NULL || socket->ops->connect == NULL) {
		result = socket_result(&reference, -EOPNOTSUPP);
		return result;
	}

	/* A path connection goes through the namespace with the caller's identity. */
	error = copy_sockaddr_in(args[1], (socklen_t)args[2], &address);
	if (error == 0 && socket->family == AF_UNIX) {
		credential = peercred_snapshot_ref(process, &connector);
		if (credential == NULL) {
			error = EINVAL;
		} else {
			if ((file_status_flags_get(reference.file) & O_NONBLOCK) != 0)
				io_flags = SOCKET_IO_NONBLOCK;
			else
				io_flags = 0;
			error = unix_socket_connect_path(socket, process->cwdi,
			    credential, &connector, (struct sockaddr *)&address,
			    (socklen_t)args[2], io_flags);
		}
	} else if (error == 0) {
		if ((file_status_flags_get(reference.file) & O_NONBLOCK) != 0)
			io_flags = SOCKET_IO_NONBLOCK;
		else
			io_flags = 0;
		error = socket->ops->connect(socket, (struct sockaddr *)&address,
		    (socklen_t)args[2], io_flags);
	}
	cred_release(credential);
	if (error != 0)
		result = -error;
	else
		result = 0;
	result = socket_result(&reference, result);
	return result;
}

/* Handles listen(2). */
static intptr_t
sys_listen_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct socket_file_ref reference;
	struct socket *socket;
	struct ucred *credential;
	struct zedbsd_peercred listener;
	intptr_t result;
	int error;

	process = current_process();
	credential = NULL;

	if (descriptor_socket(process, (int)args[0], &reference) != 0)
		return -EBADF;
	socket = reference.socket;

	/* A listening AF_UNIX socket records the listener's identity. */
	if (socket->family == AF_UNIX) {
		credential = peercred_snapshot_ref(process, &listener);
		if (credential == NULL)
			error = EINVAL;
		else
			error = unix_socket_listen(socket, (int)args[1], &listener);
		cred_release(credential);
		if (error != 0)
			result = -error;
		else
			result = 0;
		result = socket_result(&reference, result);
		return result;
	}
	if (socket->ops == NULL || socket->ops->listen == NULL) {
		result = socket_result(&reference, -EOPNOTSUPP);
		return result;
	}
	error = socket->ops->listen(socket, (int)args[1]);
	if (error != 0)
		result = -error;
	else
		result = 0;
	result = socket_result(&reference, result);
	return result;
}

/* Handles accept(2) and accept4(2). */
static intptr_t
sys_accept_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct socket_file_ref reference;
	struct socket *socket;
	struct sockaddr_storage address;
	struct sockaddr_output_pin output;
	struct socket *accepted;
	struct file *file;
	struct file *files[1];
	struct filedesc_reservation reservation;
	struct sockaddr *address_argument;
	socklen_t *length_argument;
	int supplied_flags;
	unsigned descriptor_flags;
	socklen_t length;
	int io_flags;
	int descriptor;
	intptr_t result;
	int error;

	process = current_process();
	accepted = NULL;
	file = NULL;
	supplied_flags = (int)args[3];
	descriptor_flags = 0;
	if ((supplied_flags & SOCK_CLOEXEC) != 0)
		descriptor_flags |= FILEDESC_CLOEXEC;
	if ((supplied_flags & SOCK_CLOFORK) != 0)
		descriptor_flags |= FILEDESC_CLOFORK;
	length = sizeof(address);

	/* Validates the flags and the address output pair. */
	if ((supplied_flags &
	    ~(SOCK_NONBLOCK | SOCK_CLOEXEC | SOCK_CLOFORK)) != 0 ||
	    args[4] != 0 || args[5] != 0)
		return -EINVAL;
	if (descriptor_socket(process, (int)args[0], &reference) != 0)
		return -EBADF;
	socket = reference.socket;
	if ((args[1] == 0) != (args[2] == 0)) {
		result = socket_result(&reference, -EINVAL);
		return result;
	}
	if (socket->ops == NULL || socket->ops->accept == NULL) {
		result = socket_result(&reference, -EOPNOTSUPP);
		return result;
	}

	/* Reserves every output before the accept may sleep. */
	error = sockaddr_output_pin(args[1], args[2], &output);
	if (error != 0) {
		result = socket_result(&reference, -error);
		return result;
	}
	memset(&reservation, 0, sizeof(reservation));
	error = filedesc_reserve_many(process->fd, 1, descriptor_flags,
	    &reservation);
	if (error == 0)
		error = socket_file_reserve(&file);
	if (error != 0) {
		filedesc_abort_reserved(&reservation);
		sockaddr_output_unpin(&output);
		result = socket_result(&reference, -error);
		return result;
	}

	/* Accepts a connection. */
	memset(&address, 0, sizeof(address));
	if (args[1] != 0) {
		address_argument = (struct sockaddr *)&address;
		length_argument = &length;
	} else {
		address_argument = NULL;
		length_argument = NULL;
	}
	if ((file_status_flags_get(reference.file) & O_NONBLOCK) != 0)
		io_flags = SOCKET_IO_NONBLOCK;
	else
		io_flags = 0;
	error = socket->ops->accept(socket, &accepted, address_argument,
	    length_argument, io_flags);
	if (error != 0) {
		(void)file_close(file);
		filedesc_abort_reserved(&reservation);
		sockaddr_output_unpin(&output);
		result = socket_result(&reference, -error);
		return result;
	}
	if (accepted == NULL) {
		(void)file_close(file);
		filedesc_abort_reserved(&reservation);
		sockaddr_output_unpin(&output);
		result = socket_result(&reference, -EIO);
		return result;
	}

	/* Publishes the accepted socket and reports the peer address. */
	socket_file_ref_put(&reference);
	error = socket_file_attach(file, accepted);
	if (error == 0 && (supplied_flags & SOCK_NONBLOCK) != 0)
		file_status_flags_update(file, O_NONBLOCK, O_NONBLOCK);
	files[0] = file;
	if (error == 0)
		error = filedesc_commit_reserved(&reservation, files, &descriptor);
	if (error != 0) {
		filedesc_abort_reserved(&reservation);
		if (file->f_data == NULL)
			socket_release(accepted);
		(void)file_close(file);
		sockaddr_output_unpin(&output);
		return -error;
	}
	error = copy_sockaddr_out_pinned(&output, &address, length);
	sockaddr_output_unpin(&output);
	if (error != 0) {
		(void)filedesc_close(process->fd, descriptor);
		return -error;
	}
	return descriptor;
}

/* Handles sendto(2). */
static intptr_t
sys_sendto_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct socket_file_ref reference;
	struct socket *socket;
	struct sockaddr_storage address;
	const struct sockaddr *destination;
	void *buffer;
	ssize_t result;
	int io_flags;
	int error;
	size_t amount;

	process = current_process();
	destination = NULL;

	/* Validates the flags, the destination pair, and the datagram size. */
	if (descriptor_socket(current_process(), (int)args[0], &reference) != 0)
		return -EBADF;
	socket = reference.socket;
	if (socket->ops == NULL || socket->ops->sendto == NULL) {
		result = socket_result(&reference, -EOPNOTSUPP);
		return result;
	}
	if (((int)args[3] & ~SOCKET_SEND_FLAGS) != 0) {
		result = socket_result(&reference, -EOPNOTSUPP);
		return result;
	}
	if ((args[4] == 0) != (args[5] == 0)) {
		result = socket_result(&reference, -EINVAL);
		return result;
	}
	if (socket->type != SOCK_STREAM &&
	    args[2] > PACKET_BUF_STORAGE_SIZE) {
		result = socket_result(&reference, -EMSGSIZE);
		return result;
	}
	if (args[4] != 0) {
		error = copy_sockaddr_in(args[4], (socklen_t)args[5], &address);
		if (error != 0) {
			result = socket_result(&reference, -error);
			return result;
		}
		destination = (const struct sockaddr *)&address;
	}

	/* An empty send needs no buffer. */
	if (args[2] == 0) {
		io_flags = (int)socket_file_effective_flags(&reference, (int)args[3]);
		if (socket->family == AF_UNIX)
			result = unix_socket_send_message_at(socket, process->cwdi,
			    process->cred, "", 0, io_flags, destination,
			    (socklen_t)args[5], NULL, 0);
		else
			result = socket->ops->sendto(socket, "", 0, io_flags,
			    destination, (socklen_t)args[5]);
		result = socket_result(&reference, result);
		return result;
	}

	/* Copies at most one buffer's worth and sends it. */
	if (socket->type == SOCK_STREAM && args[2] > SYSCALL_SOCKET_BUFFER_MAX)
		amount = SYSCALL_SOCKET_BUFFER_MAX;
	else
		amount = (size_t)args[2];
	buffer = kern_malloc(amount);
	if (buffer == NULL) {
		result = socket_result(&reference, -ENOMEM);
		return result;
	}
	error = copyin(args[1], buffer, amount);
	if (error != 0) {
		result = -error;
	} else {
		io_flags = (int)socket_file_effective_flags(&reference, (int)args[3]);
		if (socket->family == AF_UNIX)
			result = unix_socket_send_message_at(socket, process->cwdi,
			    process->cred, buffer, amount, io_flags, destination,
			    (socklen_t)args[5], NULL, 0);
		else
			result = socket->ops->sendto(socket, buffer, amount,
			    io_flags, destination, (socklen_t)args[5]);
	}
	kern_free(buffer);
	result = socket_result(&reference, result);
	return result;
}

/* Handles recvfrom(2). */
static intptr_t
sys_recvfrom_call(
	const uintptr_t args[6])
{
	struct socket_file_ref reference;
	struct socket *socket;
	struct sockaddr_storage address;
	struct sockaddr_output_pin output;
	struct uaccess_pin data_pin;
	struct sockaddr *address_argument;
	socklen_t *length_argument;
	socklen_t length;
	size_t capacity;
	void *buffer;
	uint8_t empty;
	ssize_t result;
	int error;
	size_t copied;

	length = sizeof(address);
	empty = 0;

	/* Validates the flags and the address output pair. */
	if (descriptor_socket(current_process(), (int)args[0], &reference) != 0)
		return -EBADF;
	socket = reference.socket;
	if (socket->ops == NULL || socket->ops->recvfrom == NULL) {
		result = socket_result(&reference, -EOPNOTSUPP);
		return result;
	}
	if (((int)args[3] & ~SOCKET_RECV_FLAGS) != 0) {
		result = socket_result(&reference, -EOPNOTSUPP);
		return result;
	}
	if ((args[4] == 0) != (args[5] == 0)) {
		result = socket_result(&reference, -EINVAL);
		return result;
	}

	/* A zero-length stream receive is a successful no-op. */
	if (socket->type == SOCK_STREAM && args[2] == 0) {
		result = socket_result(&reference, 0);
		return result;
	}

	/* Pins the outputs and allocates a bounded buffer. */
	if (socket->type == SOCK_STREAM) {
		if (args[2] > SYSCALL_SOCKET_BUFFER_MAX)
			capacity = SYSCALL_SOCKET_BUFFER_MAX;
		else
			capacity = (size_t)args[2];
	} else {
		if (args[2] > PACKET_BUF_STORAGE_SIZE)
			capacity = PACKET_BUF_STORAGE_SIZE;
		else
			capacity = (size_t)args[2];
	}
	error = uaccess_pin(args[1], capacity, PROT_WRITE, &data_pin);
	if (error != 0) {
		result = socket_result(&reference, -error);
		return result;
	}
	error = sockaddr_output_pin(args[4], args[5], &output);
	if (error != 0) {
		uaccess_unpin(&data_pin);
		result = socket_result(&reference, -error);
		return result;
	}
	if (capacity != 0)
		buffer = kern_malloc(capacity);
	else
		buffer = &empty;
	if (capacity != 0 && buffer == NULL) {
		sockaddr_output_unpin(&output);
		uaccess_unpin(&data_pin);
		result = socket_result(&reference, -ENOMEM);
		return result;
	}

	/* Receives and copies the data and the source address out. */
	memset(&address, 0, sizeof(address));
	if (args[4] != 0) {
		address_argument = (struct sockaddr *)&address;
		length_argument = &length;
	} else {
		address_argument = NULL;
		length_argument = NULL;
	}
	result = socket->ops->recvfrom(socket, buffer, capacity,
	    (int)socket_file_effective_flags(&reference, (int)args[3]),
	    address_argument, length_argument);
	if (result >= 0) {
		if ((size_t)result < capacity)
			copied = (size_t)result;
		else
			copied = capacity;
		error = copyout_pinned(&data_pin, 0, buffer, copied);
		if (error == 0)
			error = copy_sockaddr_out_pinned(&output, &address, length);
		if (error != 0)
			result = -error;
	}
	if (capacity != 0)
		kern_free(buffer);
	sockaddr_output_unpin(&output);
	uaccess_unpin(&data_pin);
	result = socket_result(&reference, result);
	return result;
}

/* Handles sendmsg(2). */
static intptr_t
sys_sendmsg_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct sendmsg_args request;
	struct socket_file_ref reference;
	struct sockaddr_storage address;
	struct sockaddr *destination;
	struct file *files[ZEDBSD_MSG_FD_MAX];
	int descriptors[ZEDBSD_MSG_FD_MAX];
	void *buffer;
	void *data;
	unsigned index;
	unsigned count;
	ssize_t result;
	int error;

	process = current_process();
	destination = NULL;
	buffer = NULL;
	count = 0;

	/* Copies and validates the request. */
	if (args[1] == 0 ||
	    args[2] != 0 ||
	    args[3] != 0 ||
	    args[4] != 0 ||
	    args[5] != 0)
		return -EINVAL;
	error = copyin(args[1], &request, sizeof(request));
	if (error != 0)
		return -error;
	if (request.reserved != 0 ||
	    request.data_length > SIZE_MAX ||
	    request.name_length > sizeof(address) ||
	    request.descriptor_count > ZEDBSD_MSG_FD_MAX ||
	    (request.data_length != 0 && request.data == 0) ||
	    (request.name_length != 0 && request.name == 0) ||
	    (request.descriptor_count != 0 && request.descriptors == 0))
		return -EINVAL;
	if ((request.flags & ~SOCKET_SEND_FLAGS) != 0)
		return -EOPNOTSUPP;
	if (descriptor_socket(process, (int)args[0], &reference) != 0)
		return -EBADF;
	if (reference.socket->type != SOCK_STREAM &&
	    request.data_length > PACKET_BUF_STORAGE_SIZE) {
		result = socket_result(&reference, -EMSGSIZE);
		return result;
	}
	if (reference.socket->type == SOCK_STREAM &&
	    request.data_length > SYSCALL_SOCKET_BUFFER_MAX)
		request.data_length = SYSCALL_SOCKET_BUFFER_MAX;

	/* Copies the destination, the data, and the descriptors in. */
	if (request.name_length != 0) {
		error = copy_sockaddr_in((uintptr_t)request.name,
		    request.name_length, &address);
		if (error != 0) {
			result = socket_result(&reference, -error);
			return result;
		}
		destination = (struct sockaddr *)&address;
	}
	if (request.data_length != 0) {
		buffer = kern_malloc((size_t)request.data_length);
		if (buffer == NULL) {
			result = socket_result(&reference, -ENOMEM);
			return result;
		}
		error = copyin((uintptr_t)request.data, buffer,
		    (size_t)request.data_length);
		if (error != 0) {
			kern_free(buffer);
			result = socket_result(&reference, -error);
			return result;
		}
	}
	if (request.descriptor_count != 0) {
		error = copyin((uintptr_t)request.descriptors, descriptors,
		    request.descriptor_count * sizeof(descriptors[0]));
		if (error != 0)
			goto fail;
		for (count = 0; count < request.descriptor_count; count++) {
			files[count] = filedesc_get_ref(current_process()->fd,
			    descriptors[count]);
			if (files[count] == NULL) {
				error = EBADF;
				goto fail;
			}
		}
	}

	/* Only AF_UNIX carries descriptors. */
	if (request.data_length != 0)
		data = buffer;
	else
		data = "";
	if (reference.socket->family == AF_UNIX) {
		result = unix_socket_send_message_at(reference.socket,
		    process->cwdi, process->cred, data,
		    (size_t)request.data_length,
		    (int)socket_file_effective_flags(&reference,
		    (int)request.flags), destination, request.name_length, files,
		    count);
	} else if (count != 0) {
		error = EOPNOTSUPP;
		goto fail;
	} else if (reference.socket->ops == NULL ||
	    reference.socket->ops->sendto == NULL) {
		result = -EOPNOTSUPP;
	} else {
		result = reference.socket->ops->sendto(reference.socket, data,
		    (size_t)request.data_length,
		    (int)socket_file_effective_flags(&reference,
		    (int)request.flags), destination, request.name_length);
	}
	kern_free(buffer);
	result = socket_result(&reference, result);
	return result;
fail:
	for (index = 0; index < count; index++)
		(void)file_close(files[index]);
	kern_free(buffer);
	result = socket_result(&reference, -error);
	return result;
}

/* Handles recvmsg(2). */
static intptr_t
sys_recvmsg_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct recvmsg_args request;
	struct socket_file_ref reference;
	struct sockaddr_storage address;
	struct unix_recv_transaction transaction;
	struct filedesc_reservation reservation;
	int descriptors[ZEDBSD_MSG_FD_MAX];
	void *buffer;
	void *data;
	uint8_t *buffer_argument;
	struct sockaddr *address_argument;
	socklen_t *name_length_argument;
	size_t buffer_capacity;
	socklen_t name_length;
	unsigned file_count;
	unsigned truncated;
	unsigned index;
	unsigned descriptor_flags;
	unsigned output_flags;
	ssize_t result;
	int error;
	ssize_t wire_result;
	size_t copied;
	int receive_flags;
	int wait_all;
	ssize_t part;
	socklen_t amount;
	socklen_t name_copied;

	process = current_process();
	buffer = NULL;
	truncated = 0;

	/* Copies and validates the request. */
	if (args[1] == 0 ||
	    args[2] != 0 ||
	    args[3] != 0 ||
	    args[4] != 0 ||
	    args[5] != 0)
		return -EINVAL;
	error = copyin(args[1], &request, sizeof(request));
	if (error != 0)
		return -error;
	if (request.reserved != 0 ||
	    request.reserved2 != 0 ||
	    request.data_capacity > SIZE_MAX ||
	    request.name_capacity > sizeof(address) ||
	    request.descriptor_capacity > ZEDBSD_MSG_FD_MAX ||
	    (request.data_capacity != 0 && request.data == 0) ||
	    (request.name_capacity != 0 && request.name == 0) ||
	    (request.descriptor_capacity != 0 && request.descriptors == 0))
		return -EINVAL;
	if ((request.flags & ~(SOCKET_RECV_FLAGS | MSG_CMSG_CLOEXEC |
	    MSG_CMSG_CLOFORK)) != 0)
		return -EOPNOTSUPP;
	if (descriptor_socket(process, (int)args[0], &reference) != 0)
		return -EBADF;

	/* Bounds the buffer; an empty stream receive completes at once. */
	buffer_capacity = (size_t)request.data_capacity;
	if (reference.socket->type == SOCK_STREAM &&
	    buffer_capacity > SYSCALL_SOCKET_BUFFER_MAX)
		buffer_capacity = SYSCALL_SOCKET_BUFFER_MAX;
	if (reference.socket->type != SOCK_STREAM &&
	    buffer_capacity > PACKET_BUF_STORAGE_SIZE)
		buffer_capacity = PACKET_BUF_STORAGE_SIZE;
	if (reference.socket->type == SOCK_STREAM && buffer_capacity == 0) {
		request.data_length = 0;
		request.name_length = 0;
		request.descriptor_count = 0;
		request.output_flags = 0;
		error = copyout(&request, args[1], sizeof(request));
		if (error != 0)
			result = -error;
		else
			result = 0;
		result = socket_result(&reference, result);
		return result;
	}
	if (buffer_capacity != 0) {
		buffer = kern_malloc(buffer_capacity);
		if (buffer == NULL) {
			result = socket_result(&reference, -ENOMEM);
			return result;
		}
	}
	name_length = request.name_capacity;
	if (request.name_capacity != 0) {
		address_argument = (struct sockaddr *)&address;
		name_length_argument = &name_length;
	} else {
		address_argument = NULL;
		name_length_argument = NULL;
	}

	/* A non-AF_UNIX socket receives through recvfrom without descriptors. */
	if (reference.socket->family != AF_UNIX) {
		wire_result = 0;
		if (reference.socket->ops == NULL ||
		    reference.socket->ops->recvfrom == NULL) {
			kern_free(buffer);
			result = socket_result(&reference, -EOPNOTSUPP);
			return result;
		}
		receive_flags = (int)socket_file_effective_flags(&reference,
		    (int)request.flags);

		/* Ancillary descriptor flags are meaningful only to AF_UNIX. */
		receive_flags &= ~(MSG_CMSG_CLOEXEC | MSG_CMSG_CLOFORK);
		wait_all = 0;
		if (reference.socket->type == SOCK_STREAM &&
		    (receive_flags & MSG_WAITALL) != 0 &&
		    (receive_flags & MSG_PEEK) == 0)
			wait_all = 1;
		receive_flags &= ~MSG_WAITALL;
		if (reference.socket->type != SOCK_STREAM)
			receive_flags |= MSG_TRUNC;
		memset(&address, 0, sizeof(address));
		do {
			if (buffer_capacity != 0)
				buffer_argument = (uint8_t *)buffer + (size_t)wire_result;
			else
				buffer_argument = (uint8_t *)"";
			part = reference.socket->ops->recvfrom(
			    reference.socket, buffer_argument,
			    buffer_capacity - (size_t)wire_result, receive_flags,
			    address_argument, name_length_argument);
			if (part < 0) {
				if (wire_result == 0)
					wire_result = part;
				break;
			}
			if (part == 0)
				break;
			wire_result += part;
		} while (wait_all && (size_t)wire_result < buffer_capacity);
		if (wire_result < 0) {
			kern_free(buffer);
			result = socket_result(&reference, wire_result);
			return result;
		}

		/* Copies the data and the source address out. */
		if ((size_t)wire_result < buffer_capacity)
			copied = (size_t)wire_result;
		else
			copied = buffer_capacity;
		if (copied != 0)
			error = copyout(buffer, (uintptr_t)request.data, copied);
		else
			error = 0;
		if (error == 0 && request.name_capacity != 0) {
			if (request.name_capacity < name_length)
				amount = request.name_capacity;
			else
				amount = name_length;
			if (amount != 0)
				error = copyout(&address, (uintptr_t)request.name,
				    amount);
		}
		if ((request.flags & MSG_TRUNC) != 0)
			request.data_length = (uint64_t)wire_result;
		else
			request.data_length = (uint64_t)copied;
		request.name_length = name_length;
		request.descriptor_count = 0;
		if ((size_t)wire_result > copied)
			request.output_flags = MSG_TRUNC;
		else
			request.output_flags = 0;
		if (error == 0)
			error = copyout(&request, args[1], sizeof(request));
		kern_free(buffer);
		if (error != 0)
			result = -error;
		else
			result = (ssize_t)request.data_length;
		result = socket_result(&reference, result);
		return result;
	}

	/* Begins an AF_UNIX receive transaction that may carry descriptors. */
	memset(&transaction, 0, sizeof(transaction));
	memset(&reservation, 0, sizeof(reservation));
	if (buffer_capacity != 0)
		data = buffer;
	else
		data = "";
	result = unix_socket_receive_begin(reference.socket, data,
	    buffer_capacity,
	    (int)socket_file_effective_flags(&reference,
	    (int)request.flags & ~(MSG_CMSG_CLOEXEC | MSG_CMSG_CLOFORK)),
	    address_argument, name_length_argument,
	    request.descriptor_capacity, &transaction);
	if (result < 0) {
		kern_free(buffer);
		result = socket_result(&reference, result);
		return result;
	}
	file_count = transaction.file_count;
	truncated = transaction.control_truncated;
	if (transaction.active) {
		descriptor_flags = 0;
		if ((request.flags & MSG_CMSG_CLOEXEC) != 0)
			descriptor_flags |= FILEDESC_CLOEXEC;
		if ((request.flags & MSG_CMSG_CLOFORK) != 0)
			descriptor_flags |= FILEDESC_CLOFORK;
		error = filedesc_reserve_many(process->fd, file_count,
		    descriptor_flags, &reservation);
		if (error != 0) {
			unix_socket_receive_abort(&transaction);
			kern_free(buffer);
			result = socket_result(&reference, -error);
			return result;
		}
		for (index = 0; index < file_count; index++)
			descriptors[index] = reservation.slots[index];
	}

	/* Copies the data, the source address, and the descriptors out. */
	if (transaction.copied != 0)
		error = copyout(buffer, (uintptr_t)request.data,
		    transaction.copied);
	else
		error = 0;
	if (error == 0 && request.name_capacity != 0) {
		if (request.name_capacity < name_length)
			name_copied = request.name_capacity;
		else
			name_copied = name_length;
		if (name_copied != 0)
			error = copyout(&address, (uintptr_t)request.name, name_copied);
	}
	if (error == 0 && file_count != 0)
		error = copyout(descriptors, (uintptr_t)request.descriptors,
		    file_count * sizeof(descriptors[0]));
	request.data_length = (uint64_t)result;
	request.name_length = name_length;
	request.descriptor_count = file_count;
	output_flags = 0;
	if (truncated)
		output_flags |= MSG_CTRUNC;
	if (transaction.data_truncated)
		output_flags |= MSG_TRUNC;
	request.output_flags = output_flags;
	if (error == 0)
		error = copyout(&request, args[1], sizeof(request));
	if (error != 0) {
		filedesc_abort_reserved(&reservation);
		unix_socket_receive_abort(&transaction);
		kern_free(buffer);
		result = socket_result(&reference, -error);
		return result;
	}

	/* Publishes the descriptors and commits or peeks the transaction. */
	if (transaction.active) {
		error = filedesc_commit_reserved(&reservation, transaction.files,
		    descriptors);
		if (error != 0) {
			filedesc_abort_reserved(&reservation);
			unix_socket_receive_abort(&transaction);
			kern_free(buffer);
			result = socket_result(&reference, -error);
			return result;
		}

		/* The descriptor table owns these references after publication. */
		for (index = 0; index < transaction.file_count; index++)
			transaction.files[index] = NULL;
		if ((request.flags & MSG_PEEK) != 0)
			unix_socket_receive_abort(&transaction);
		else
			unix_socket_receive_commit(&transaction);
	}
	kern_free(buffer);
	result = socket_result(&reference, result);
	return result;
}

/* Handles shutdown(2). */
static intptr_t
sys_shutdown_call(
	const uintptr_t args[6])
{
	struct socket_file_ref reference;
	struct socket *socket;
	intptr_t result;
	int error;

	if (descriptor_socket(current_process(), (int)args[0], &reference) != 0)
		return -EBADF;
	socket = reference.socket;
	if (socket->ops == NULL || socket->ops->shutdown == NULL) {
		result = socket_result(&reference, -EOPNOTSUPP);
		return result;
	}
	error = socket->ops->shutdown(socket, (int)args[1]);
	if (error != 0)
		result = -error;
	else
		result = 0;
	result = socket_result(&reference, result);
	return result;
}

/* Handles getsockname(2) and getpeername(2). */
static intptr_t
sys_socket_name_call(
	const uintptr_t args[6],
	int peer)
{
	struct socket_file_ref reference;
	struct socket *socket;
	struct sockaddr_storage address;
	socklen_t length;
	intptr_t result;
	int error;

	length = sizeof(address);

	if (descriptor_socket(current_process(), (int)args[0], &reference) != 0)
		return -EBADF;
	socket = reference.socket;
	if (args[1] == 0 || args[2] == 0) {
		result = socket_result(&reference, -EINVAL);
		return result;
	}

	/* Asks the protocol for the requested end of the connection. */
	memset(&address, 0, sizeof(address));
	if (peer) {
		if (socket->ops == NULL || socket->ops->getpeername == NULL) {
			result = socket_result(&reference, -EOPNOTSUPP);
			return result;
		}
		error = socket->ops->getpeername(socket,
		    (struct sockaddr *)&address, &length);
	} else {
		if (socket->ops == NULL || socket->ops->getsockname == NULL) {
			result = socket_result(&reference, -EOPNOTSUPP);
			return result;
		}
		error = socket->ops->getsockname(socket,
		    (struct sockaddr *)&address, &length);
	}
	if (error == 0)
		error = copy_sockaddr_out(args[1], args[2], &address, length);
	if (error != 0)
		result = -error;
	else
		result = 0;
	result = socket_result(&reference, result);
	return result;
}

/* Handles setsockopt(2). */
static intptr_t
sys_setsockopt_call(
	const uintptr_t args[6])
{
	struct socket_file_ref reference;
	struct socket *socket;
	uint8_t value[SYSCALL_SOCKET_OPTION_MAX];
	intptr_t result;
	int error;

	if (descriptor_socket(current_process(), (int)args[0], &reference) != 0)
		return -EBADF;
	socket = reference.socket;
	if (args[4] > sizeof(value) || (args[4] != 0 && args[3] == 0)) {
		result = socket_result(&reference, -EINVAL);
		return result;
	}

	/* The common layer handles generic options; the protocol handles the rest. */
	if (args[4] == 0)
		error = 0;
	else
		error = copyin(args[3], value, (size_t)args[4]);
	if (error == 0)
		error = socket_setsockopt_common(socket, (int)args[1],
		    (int)args[2], value, (socklen_t)args[4]);
	if (error == ENOPROTOOPT && socket->ops != NULL &&
	    socket->ops->setsockopt != NULL)
		error = socket->ops->setsockopt(socket, (int)args[1],
		    (int)args[2], value, (socklen_t)args[4]);
	if (error != 0)
		result = -error;
	else
		result = 0;
	result = socket_result(&reference, result);
	return result;
}

/* Handles getsockopt(2). */
static intptr_t
sys_getsockopt_call(
	const uintptr_t args[6])
{
	struct socket_file_ref reference;
	struct socket *socket;
	uint8_t value[SYSCALL_SOCKET_OPTION_MAX];
	socklen_t length;
	intptr_t result;
	int error;

	if (descriptor_socket(current_process(), (int)args[0], &reference) != 0)
		return -EBADF;
	socket = reference.socket;
	if (args[3] == 0 || args[4] == 0) {
		result = socket_result(&reference, -EINVAL);
		return result;
	}
	error = copyin(args[4], &length, sizeof(length));
	if (error != 0) {
		result = socket_result(&reference, -error);
		return result;
	}
	if (length > sizeof(value))
		length = sizeof(value);

	/* The common layer handles generic options; the protocol handles the rest. */
	error = socket_getsockopt_common(socket, (int)args[1], (int)args[2],
	    value, &length);
	if (error == ENOPROTOOPT && socket->ops != NULL &&
	    socket->ops->getsockopt != NULL)
		error = socket->ops->getsockopt(socket, (int)args[1],
		    (int)args[2], value, &length);
	if (error == 0)
		error = copyout(value, args[3], length);
	if (error == 0)
		error = copyout(&length, args[4], sizeof(length));
	if (error != 0)
		result = -error;
	else
		result = 0;
	result = socket_result(&reference, result);
	return result;
}

/* Resolves a dirfd into a lookup context, holding the directory file. */
static int
syscall_context_at(
	struct process *process,
	int dirfd,
	struct cwdinfo *temporary,
	struct cwdinfo **context,
	struct file **held)
{
	if (process == NULL || temporary == NULL || context == NULL || held == NULL)
		return EINVAL;
	*held = NULL;
	if (dirfd == AT_FDCWD) {
		*context = process->cwdi;
		return 0;
	}

	/* A descriptor context copies the cwdinfo with the directory as cwd. */
	*held = filedesc_get_ref(process->fd, dirfd);
	if (*held == NULL)
		return EBADF;
	if ((*held)->f_inode == NULL || (*held)->f_inode->i_type != INODE_DIR) {
		(void)file_close(*held);
		*held = NULL;
		return ENOTDIR;
	}
	*temporary = *process->cwdi;
	temporary->cwd = (*held)->f_path;
	*context = temporary;
	return 0;
}

/* Handles open(2) and openat(2). */
static intptr_t
sys_open_call(
	const uintptr_t args[6],
	int at)
{
	struct process *process;
	struct ucred *credential;
	struct cwdinfo temporary;
	struct cwdinfo *context;
	struct file *file;
	struct file *held;
	struct file *files[1];
	struct filedesc_reservation reservation;
	char path[PATH_MAX];
	uintptr_t path_address;
	int flags;
	mode_t mode;
	int dirfd;
	unsigned descriptor_flags;
	int descriptor;
	int error;

	process = current_process();
	if (at) {
		path_address = args[1];
		flags = (int)args[2];
		mode = (mode_t)args[3];
		dirfd = (int)args[0];
	} else {
		path_address = args[0];
		flags = (int)args[1];
		mode = (mode_t)args[2];
		dirfd = AT_FDCWD;
	}

	if (process == NULL || process->fd == NULL || process->cwdi == NULL)
		return -EINVAL;
	credential = cred_process_ref(process);
	if (credential == NULL)
		return -EINVAL;

	/* Reserves the descriptor before opening, then opens with the caller's credential. */
	memset(&reservation, 0, sizeof(reservation));
	error = copyinstr(path_address, path, sizeof(path), NULL);
	held = NULL;
	if (error == 0 && path[0] == '/')
		context = process->cwdi;
	else if (error == 0)
		error = syscall_context_at(process, dirfd, &temporary, &context,
		    &held);
	if (error == 0) {
		descriptor_flags = 0;
		if ((flags & O_CLOEXEC) != 0)
			descriptor_flags |= FILEDESC_CLOEXEC;
		if ((flags & O_CLOFORK) != 0)
			descriptor_flags |= FILEDESC_CLOFORK;
		error = filedesc_reserve_many(process->fd, 1, descriptor_flags,
		    &reservation);
	}
	if (error == 0)
		error = file_openat_cred(context, credential, path,
		    flags & ~(O_CLOEXEC | O_CLOFORK),
		    (mode & 07777U) & ~process->umask,
		    &file);
	if (held != NULL)
		(void)file_close(held);
	cred_release(credential);
	if (error != 0) {
		filedesc_abort_reserved(&reservation);
		return -error;
	}
	files[0] = file;
	error = filedesc_commit_reserved(&reservation, files, &descriptor);
	if (error != 0) {
		filedesc_abort_reserved(&reservation);
		(void)file_close(file);
		return -error;
	}
	return descriptor;
}

/* Handles close(2). */
static intptr_t
sys_close_call(
	const uintptr_t args[6])
{
	struct process *process;
	int error;

	process = current_process();
	if (process == NULL || process->fd == NULL)
		error = EBADF;
	else
		error = filedesc_close(process->fd, (int)args[0]);
	if (error != 0)
		return -error;
	return 0;
}

/* Borrows shared scratch without allocation or waiting; streams retain stack I/O. */
static uint8_t *
syscall_regular_buffer(struct file *file, size_t length, uint8_t *fallback,
	size_t *capacity)
{
	uint8_t *buffer;

	*capacity = SYSCALL_IO_CHUNK;
	if (length <= SYSCALL_IO_CHUNK || file->f_inode == NULL ||
	    file->f_inode->i_type != INODE_REG)
		return fallback;
	buffer = io_pool_borrow(length, capacity);
	if (buffer == NULL)
		return fallback;
	return buffer;
}

/* Handles read(2) through a bounce buffer. */
static SYSCALL_EXT intptr_t
sys_read_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct file *file;
	struct file_io io;
	struct uaccess_pin pin;
	uint8_t small_buffer[SYSCALL_IO_CHUNK];
	uint8_t *buffer = small_buffer;
	size_t capacity = sizeof(small_buffer);
	size_t done;
	size_t length;
	intptr_t result;
	int error;
	size_t chunk;
	ssize_t count;

	process = current_process();
	done = 0;
	length = (size_t)args[2];

	/* Pins the user buffer and starts the transfer. */
	if (process == NULL)
		return -EBADF;
	file = filedesc_get_ref(process->fd, (int)args[0]);
	if (file == NULL)
		return -EBADF;
	error = uaccess_pin(args[1], length, HAL_SPACE_WRITE, &pin);
	if (error != 0) {
		(void)file_close(file);
		return -error;
	}
	buffer = syscall_regular_buffer(file, length, small_buffer, &capacity);
	error = file_io_begin(file, FILE_IO_READ, 0, 0, &io);
	if (error != 0) {
		if (buffer != small_buffer)
			io_pool_release(buffer);
		uaccess_unpin(&pin);
		(void)file_close(file);
		return -error;
	}

	/* Copies one chunk at a time until the request or the data runs out. */
	while (done < length) {
		if (length - done > capacity)
			chunk = capacity;
		else
			chunk = length - done;
		io_stats_record(IO_SYSCALL_READ, chunk);
		count = file_io_transfer(&io, buffer, chunk);
		if (count < 0) {
			if (done != 0)
				result = (intptr_t)done;
			else
				result = count;
			goto out;
		}
		if (count == 0)
			break;
		error = copyout_pinned(&pin, done, buffer, (size_t)count);
		if (error != 0) {
			if (done != 0)
				result = (intptr_t)done;
			else
				result = -error;
			goto out;
		}
		done += (size_t)count;
		if ((size_t)count < chunk)
			break;

		/*
		 * A stream/device read completes after the first successful
		 * backend transfer.  Re-entering it merely because the syscall
		 * bounce buffer was filled can turn an available short read
		 * into a second block.
		 */
		if (file->f_inode == NULL ||
		    (file->f_inode->i_type != INODE_REG &&
		     file->f_inode->i_type != INODE_BLOCK))
			break;
	}
	result = (intptr_t)done;
out:
	result = file_io_complete(&io, result);
	if (buffer != small_buffer)
		io_pool_release(buffer);
	uaccess_unpin(&pin);
	(void)file_close(file);
	return result;
}

/* Handles write(2) through a bounce buffer. */
static SYSCALL_EXT intptr_t
sys_write_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct file *file;
	struct file_io io;
	struct uaccess_pin pin;
	uint8_t small_buffer[SYSCALL_IO_CHUNK];
	uint8_t *buffer = small_buffer;
	size_t capacity = sizeof(small_buffer);
	size_t done;
	size_t length;
	intptr_t result;
	int error;
	size_t chunk;
	int limited;
	ssize_t count;

	process = current_process();
	done = 0;
	length = (size_t)args[2];

	/* Pins the user buffer and starts the transfer under the size limit. */
	if (process == NULL)
		return -EBADF;
	file = filedesc_get_ref(process->fd, (int)args[0]);
	if (file == NULL)
		return -EBADF;
	error = uaccess_pin(args[1], length, HAL_SPACE_READ, &pin);
	if (error != 0) {
		(void)file_close(file);
		return -error;
	}
	buffer = syscall_regular_buffer(file, length, small_buffer, &capacity);
	error = file_io_begin_cred(file, FILE_IO_WRITE, 0, 0, process->cred,
	    &io);
	if (error != 0) {
		if (buffer != small_buffer)
			io_pool_release(buffer);
		uaccess_unpin(&pin);
		(void)file_close(file);
		return -error;
	}
	file_io_set_growth_limit(&io,
	    resource_limit_current(process, RLIMIT_FSIZE));

	/* Copies one chunk at a time; a hit size limit raises SIGXFSZ. */
	while (done < length) {
		if (length - done > capacity)
			chunk = capacity;
		else
			chunk = length - done;
		error = copyin_pinned(&pin, done, buffer, chunk);
		if (error != 0) {
			if (done != 0)
				result = (intptr_t)done;
			else
				result = -error;
			goto out;
		}
		io_stats_record(IO_SYSCALL_WRITE, chunk);
		count = file_io_transfer(&io, buffer, chunk);
		limited = file_io_take_growth_limit_hit(&io);
		if (limited)
			(void)signal_send_thread(curthread, SIGXFSZ);
		if (count < 0) {
			if (done != 0)
				result = (intptr_t)done;
			else
				result = count;
			goto out;
		}
		done += (size_t)count;
		if (limited)
			break;
		if ((size_t)count < chunk)
			break;
	}
	result = (intptr_t)done;
out:
	result = file_io_complete(&io, result);
	if (buffer != small_buffer)
		io_pool_release(buffer);
	uaccess_unpin(&pin);
	(void)file_close(file);
	return result;
}

/* Handles lseek(2). */
static intptr_t
sys_lseek_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct file *file;
	off_t result;

	process = current_process();
	if (process != NULL)
		file = filedesc_get_ref(process->fd, (int)args[0]);
	else
		file = NULL;

	if (file == NULL)
		return -EBADF;
	result = file_seek(file, (off_t)args[1], (int)args[2]);
	(void)file_close(file);
	return result;
}

/* Handles fstat(2). */
static intptr_t
sys_fstat_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct file *file;
	struct stat status;
	int error;

	process = current_process();
	if (process != NULL)
		file = filedesc_get_ref(process->fd, (int)args[0]);
	else
		file = NULL;

	if (file == NULL)
		return -EBADF;
	if (file->f_inode == NULL) {
		(void)file_close(file);
		return -EINVAL;
	}
	error = inode_getattr(file->f_inode, &status);
	if (error == 0)
		error = copyout(&status, args[1], sizeof(status));
	(void)file_close(file);
	if (error != 0)
		return -error;
	return 0;
}

/* Maps an inode type to its dirent type code. */
static uint32_t
dirent_type(
	enum inode_type type)
{
	switch (type) {
	case INODE_REG:
		return ZEDBSD_DT_REG;
	case INODE_DIR:
		return ZEDBSD_DT_DIR;
	case INODE_BLOCK:
		return ZEDBSD_DT_BLK;
	case INODE_CHAR:
		return ZEDBSD_DT_CHR;
	case INODE_FIFO:
		return ZEDBSD_DT_FIFO;
	case INODE_SYMLINK:
		return ZEDBSD_DT_LNK;
	case INODE_SOCKET:
		return ZEDBSD_DT_SOCK;
	default:
		return ZEDBSD_DT_UNKNOWN;
	}
}

/* Handles getdents(2), one entry per call. */
static intptr_t
sys_getdents_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct file *file;
	struct uaccess_pin pin;
	struct dirent_record output;
	struct dirent entry;
	int eof;
	int error;

	process = current_process();
	if (process != NULL)
		file = filedesc_get_ref(process->fd, (int)args[0]);
	else
		file = NULL;

	/* The buffer must hold one record. */
	if (file == NULL)
		return -EBADF;
	if (args[2] < sizeof(output)) {
		(void)file_close(file);
		return -EINVAL;
	}
	error = uaccess_pin(args[1], sizeof(output), HAL_SPACE_WRITE, &pin);
	if (error != 0) {
		(void)file_close(file);
		return -error;
	}

	/* Reads one entry and copies it out. */
	error = file_readdir(file, &entry, &eof);
	if (error != 0) {
		uaccess_unpin(&pin);
		(void)file_close(file);
		return -error;
	}
	if (eof) {
		uaccess_unpin(&pin);
		(void)file_close(file);
		return 0;
	}
	memset(&output, 0, sizeof(output));
	output.d_ino = entry.d_ino;
	output.d_type = dirent_type(entry.d_type);
	strncpy(output.d_name, entry.d_name, sizeof(output.d_name) - 1U);
	error = copyout_pinned(&pin, 0, &output, sizeof(output));
	uaccess_unpin(&pin);
	(void)file_close(file);
	if (error != 0)
		return -error;
	return (intptr_t)sizeof(output);
}

/* Handles chdir(2). */
static intptr_t
sys_chdir_call(
	const uintptr_t args[6])
{
	struct process *process;
	char path[PATH_MAX];
	int error;

	process = current_process();
	if (process == NULL || process->cwdi == NULL)
		return -EINVAL;
	error = copyinstr(args[0], path, sizeof(path), NULL);
	if (error == 0)
		error = fs_chdir(process->cwdi, path);
	if (error != 0)
		return -error;
	return 0;
}

/* Handles getcwd(2). */
static intptr_t
sys_getcwd_call(
	const uintptr_t args[6])
{
	struct process *process;
	char path[PATH_MAX];
	size_t length;
	int error;

	process = current_process();
	if (process == NULL || process->cwdi == NULL)
		return -EINVAL;
	error = fs_getcwd(process->cwdi, path, sizeof(path));
	if (error != 0)
		return -error;
	length = strlen(path) + 1U;
	if (length > args[1])
		return -ERANGE;
	error = copyout(path, args[0], length);
	if (error != 0)
		return -error;
	return (intptr_t)args[0];
}

/* Converts mmap protection bits into HAL space protection. */
static int
vm_prot(
	int prot,
	uint32_t *result)
{
	uint32_t value;

	value = 0;
	if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC))
		return EINVAL;
	if ((prot & (PROT_WRITE | PROT_EXEC)) == (PROT_WRITE | PROT_EXEC))
		return EACCES;
	if (prot & PROT_READ)
		value |= HAL_SPACE_READ;
	if (prot & PROT_WRITE)
		value |= HAL_SPACE_WRITE;
	if (prot & PROT_EXEC)
		value |= HAL_SPACE_EXEC;
	*result = value;
	return 0;
}

/* Handles mmap(2). */
static intptr_t
sys_mmap_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct file *file;
	uintptr_t mapped;
	uint32_t prot;
	size_t data_size;
	int fixed;
	int shared;
	int error;
	size_t size;

	process = current_process();
	file = NULL;
	data_size = 0;

	/* Validates the flags, the length, and the alignment. */
	if (process == NULL || process->vmspace == NULL)
		return -EINVAL;
	if ((args[3] & (MAP_PRIVATE | MAP_SHARED)) != MAP_PRIVATE &&
	    (args[3] & (MAP_PRIVATE | MAP_SHARED)) != MAP_SHARED)
		return -EINVAL;
	if ((args[3] & ~(MAP_PRIVATE | MAP_SHARED | MAP_ANONYMOUS |
	    MAP_FIXED_NOREPLACE)) != 0)
		return -EOPNOTSUPP;
	fixed = (args[3] & MAP_FIXED) != 0;
	if (fixed && (args[3] & MAP_FIXED_NOREPLACE) != 0)
		return -EINVAL;
	shared = (args[3] & MAP_SHARED) != 0;
	if (args[1] == 0 || args[1] > SIZE_MAX - SYSCALL_PAGE_MASK)
		return -EINVAL;
	if (args[0] != 0 && (args[0] & SYSCALL_PAGE_MASK) != 0)
		return -EINVAL;
	if ((args[3] & MAP_FIXED_NOREPLACE) != 0 && args[0] == 0)
		return -EINVAL;

	/* A file mapping needs a readable regular file and a bounded data size. */
	if ((args[3] & MAP_ANONYMOUS) != 0) {
		if ((int)args[4] != -1 || args[5] != 0)
			return -EINVAL;
	} else {
		if ((args[5] & SYSCALL_PAGE_MASK) != 0 || (off_t)args[5] < 0)
			return -EINVAL;
		file = filedesc_get_ref(process->fd, (int)args[4]);
		if (file == NULL)
			return -EBADF;
		if (file->f_inode == NULL ||
		    file->f_inode->i_type != INODE_REG ||
		    (file_status_flags_get(file) & O_ACCMODE) == O_WRONLY) {
			(void)file_close(file);
			return -EACCES;
		}
		if (shared && (args[2] & PROT_WRITE) != 0 &&
		    ((file_status_flags_get(file) & O_ACCMODE) == O_RDONLY ||
		     file->f_ops == NULL || file->f_ops->pwrite == NULL)) {
			(void)file_close(file);
			return -EACCES;
		}
		if (file->f_inode->i_size > (off_t)args[5])
			data_size = (size_t)(file->f_inode->i_size -
			    (off_t)args[5]);
		if (data_size > args[1])
			data_size = args[1];
	}

	/* Picks the mapping primitive for the flag combination. */
	error = vm_prot((int)args[2], &prot);
	if (error == 0 && fixed && file != NULL) {
		size = (args[1] + SYSCALL_PAGE_MASK) &
		    ~SYSCALL_PAGE_MASK;
		error = vmspace_map_file_fixed(process->vmspace, args[0], size,
		    prot, file, (off_t)args[5], data_size, shared, NULL);
		mapped = args[0];
	} else if (error == 0 && fixed) {
		size = (args[1] + SYSCALL_PAGE_MASK) &
		    ~SYSCALL_PAGE_MASK;
		error = vmspace_map_anon_fixed(process->vmspace, args[0], size,
		    prot, shared, NULL);
		mapped = args[0];
	} else if (error == 0 && file != NULL && shared &&
	    (args[3] & MAP_FIXED_NOREPLACE) != 0) {
		error = vmspace_map_file_shared(process->vmspace, args[0],
		    (args[1] + SYSCALL_PAGE_MASK) & ~SYSCALL_PAGE_MASK, prot, file,
		    (off_t)args[5], data_size, NULL);
		mapped = args[0];
	} else if (error == 0 && file != NULL &&
	    (args[3] & MAP_FIXED_NOREPLACE) != 0) {
		error = vmspace_map_file(process->vmspace, args[0],
		    (args[1] + SYSCALL_PAGE_MASK) & ~SYSCALL_PAGE_MASK, prot, file,
		    (off_t)args[5], args[0], data_size, NULL);
		mapped = args[0];
	} else if (error == 0 &&
	    (args[3] & MAP_FIXED_NOREPLACE) != 0) {
		size = (args[1] + SYSCALL_PAGE_MASK) & ~SYSCALL_PAGE_MASK;
		error = vmspace_map_anon_fixed_noreplace(process->vmspace,
			args[0], size, prot, NULL);
		mapped = args[0];
	} else if (error == 0 && file == NULL && shared) {
		error = vmspace_map_anon_shared_find(process->vmspace, args[0],
		    args[1], prot, &mapped);
	} else if (error == 0 && file == NULL) {
		error = vmspace_map_find(process->vmspace, args[0], args[1], prot,
			&mapped);
	} else if (error == 0 && shared) {
		error = vmspace_map_file_shared_find(process->vmspace, args[0],
		    args[1], prot, file, (off_t)args[5], data_size, &mapped);
	} else if (error == 0) {
		error = vmspace_map_file_find(process->vmspace, args[0], args[1],
		    prot, file, (off_t)args[5], data_size, &mapped);
	}
	if (file != NULL)
		(void)file_close(file);
	if (error != 0)
		return -error;
	return (intptr_t)mapped;
}

/* Handles munmap(2). */
static intptr_t
sys_munmap_call(
	const uintptr_t args[6])
{
	struct process *process;
	size_t size;
	int error;

	process = current_process();
	if (args[1] == 0 || args[1] > SIZE_MAX - SYSCALL_PAGE_MASK)
		return -EINVAL;
	size = (args[1] + SYSCALL_PAGE_MASK) & ~SYSCALL_PAGE_MASK;
	if (process == NULL)
		error = EINVAL;
	else
		error = vmspace_unmap(process->vmspace, args[0], size);
	if (error != 0)
		return -error;
	return 0;
}

/* Handles mprotect(2). */
static intptr_t
sys_mprotect_call(
	const uintptr_t args[6])
{
	struct process *process;
	uint32_t prot;
	int error;

	process = current_process();
	error = vm_prot((int)args[2], &prot);
	if (error == 0 && (args[1] == 0 ||
	    args[1] > SIZE_MAX - SYSCALL_PAGE_MASK))
		error = EINVAL;
	if (error == 0) {
		if (process == NULL)
			error = EINVAL;
		else
			error = vmspace_protect(process->vmspace, args[0],
			    (args[1] + SYSCALL_PAGE_MASK) & ~SYSCALL_PAGE_MASK, prot);
	}
	if (error != 0)
		return -error;
	return 0;
}

/* Handles msync(2). */
static intptr_t
sys_msync_call(
	const uintptr_t args[6])
{
	struct process *process;
	size_t size;
	int error;

	process = current_process();
	if (args[1] == 0 || args[1] > SIZE_MAX - SYSCALL_PAGE_MASK)
		return -EINVAL;
	size = (args[1] + SYSCALL_PAGE_MASK) & ~SYSCALL_PAGE_MASK;
	if (process == NULL)
		error = EINVAL;
	else
		error = vmspace_sync(process->vmspace, args[0], size,
		    (int)args[2]);
	if (error != 0)
		return -error;
	return 0;
}

/* Handles brk(2). */
static intptr_t
sys_brk_call(
	const uintptr_t args[6])
{
	struct process *process;
	uintptr_t result;
	int error;

	process = current_process();
	if (process == NULL || process->vmspace == NULL)
		return -EINVAL;
	error = vmspace_brk(process->vmspace, args[0], &result);
	if (error != 0)
		return -error;
	return (intptr_t)result;
}

/* Handles ioctl(2). */
static intptr_t
sys_ioctl_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct file *file;
	int error;

	process = current_process();
	if (process != NULL)
		file = filedesc_get_ref(process->fd, (int)args[0]);
	else
		file = NULL;
	if (file == NULL)
		error = EBADF;
	else
		error = file_ioctl(file, args[1], args[2]);

	if (file != NULL)
		(void)file_close(file);
	if (error != 0)
		return -error;
	return 0;
}

/* Handles clock_gettime(2). */
static intptr_t
sys_clock_gettime_call(
	const uintptr_t args[6])
{
	struct timespec time;
	int error;

	error = kern_clock_gettime((clockid_t)args[0], &time);
	if (error != 0)
		return -error;
	error = copyout(&time, args[1], sizeof(time));
	if (error != 0)
		return -error;
	return 0;
}

/* Handles clock_getres(2). */
static intptr_t
sys_clock_getres_call(
	const uintptr_t args[6])
{
	struct timespec resolution;
	struct timespec *resolution_argument;
	int error;

	if (args[1] == 0)
		resolution_argument = NULL;
	else
		resolution_argument = &resolution;
	error = kern_clock_getres((clockid_t)args[0], resolution_argument);
	if (error == 0 && args[1] != 0)
		error = copyout(&resolution, args[1], sizeof(resolution));
	if (error != 0)
		return -error;
	return 0;
}

/* Handles clock_settime(2). */
static intptr_t
sys_clock_settime_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct timespec requested;
	int error;

	process = current_process();
	if (process == NULL || process->cred == NULL)
		return -EINVAL;
	error = copyin(args[1], &requested, sizeof(requested));
	if (error == 0)
		error = kern_clock_settime((clockid_t)args[0], &requested,
		    process->cred);
	if (error != 0)
		return -error;
	return 0;
}

/* Handles timer_create(2). */
static intptr_t
sys_timer_create_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct sigevent event;
	const struct sigevent *eventp;
	timer_t id;
	int error;

	process = current_process();
	eventp = NULL;

	if (process == NULL || args[2] == 0)
		return -EINVAL;
	if (args[1] != 0) {
		error = copyin(args[1], &event, sizeof(event));
		if (error != 0)
			return -error;
		eventp = &event;
	}

	/* A timer whose id cannot be reported is deleted again. */
	error = process_timer_create(process, (clockid_t)args[0], eventp, &id);
	if (error != 0)
		return -error;
	error = copyout(&id, args[2], sizeof(id));
	if (error != 0) {
		(void)process_timer_delete(process, id);
		return -error;
	}
	return 0;
}

/* Handles timer_delete(2). */
static intptr_t
sys_timer_delete_call(
	const uintptr_t args[6])
{
	struct process *process;
	int error;

	process = current_process();
	if (process == NULL)
		error = EINVAL;
	else
		error = process_timer_delete(process, (timer_t)args[0]);

	if (error != 0)
		return -error;
	return 0;
}

/* Handles timer_settime(2). */
static intptr_t
sys_timer_settime_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct itimerspec requested;
	struct itimerspec previous;
	struct itimerspec *previous_argument;
	int error;

	process = current_process();
	if (process == NULL || args[2] == 0)
		return -EINVAL;
	if (args[3] == 0)
		previous_argument = NULL;
	else
		previous_argument = &previous;
	error = copyin(args[2], &requested, sizeof(requested));
	if (error == 0)
		error = process_timer_settime(process, (timer_t)args[0],
		    (int)args[1], &requested, previous_argument);
	if (error == 0 && args[3] != 0)
		error = copyout(&previous, args[3], sizeof(previous));
	if (error != 0)
		return -error;
	return 0;
}

/* Handles timer_gettime(2). */
static intptr_t
sys_timer_gettime_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct itimerspec current;
	int error;

	process = current_process();
	if (process == NULL || args[1] == 0)
		return -EINVAL;
	error = process_timer_gettime(process, (timer_t)args[0], &current);
	if (error == 0)
		error = copyout(&current, args[1], sizeof(current));
	if (error != 0)
		return -error;
	return 0;
}

/* Handles timer_getoverrun(2). */
static intptr_t
sys_timer_getoverrun_call(
	const uintptr_t args[6])
{
	struct process *process;
	int overrun;
	int error;

	process = current_process();
	if (process == NULL)
		return -EINVAL;
	error = process_timer_getoverrun(process, (timer_t)args[0], &overrun);
	if (error != 0)
		return -error;
	return overrun;
}

/* Handles mount(2). */
static intptr_t
sys_mount_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct mount_args requested;
	struct fat_mount_args internal;
	struct fat_mount_args *mount_arguments;
	char type[NAME_MAX + 1U];
	char directory[PATH_MAX];
	int flags;
	int mount_flags;
	int error;

	process = current_process();
	flags = (int)args[2];

	/* Only the superuser mounts, and only with the supported flags. */
	if (process == NULL || process->cred == NULL)
		return -EINVAL;
	if (!cred_is_superuser(process->cred))
		return -EPERM;
	if ((flags & ~(int)(MNT_RDONLY | MNT_NOSUID)) != 0)
		return -EINVAL;

	/* Copies the type, the directory, and the optional arguments. */
	error = copyinstr(args[0], type, sizeof(type), NULL);
	if (error == 0)
		error = copyinstr(args[1], directory, sizeof(directory), NULL);
	memset(&requested, 0, sizeof(requested));
	memset(&internal, 0, sizeof(internal));
	if (error == 0 && args[3] != 0) {
		error = copyin(args[3], &requested, sizeof(requested));
		if (error == 0 && (requested.size != sizeof(requested) ||
		    requested.version != ZEDBSD_MOUNT_ARGS_VERSION ||
		    memchr(requested.fspec, '\0', sizeof(requested.fspec)) == NULL))
			error = EINVAL;
		if (error == 0 && requested.fspec[0] != '\0')
			internal.fspec = requested.fspec;
	}
	if (error == 0) {
		mount_flags = 0;
		if ((flags & MNT_RDONLY) != 0)
			mount_flags |= MOUNT_READ_ONLY;
		if ((flags & MNT_NOSUID) != 0)
			mount_flags |= MOUNT_NOSUID;
		if (internal.fspec != NULL)
			mount_arguments = &internal;
		else
			mount_arguments = NULL;
		error = mount(type, directory, mount_flags, mount_arguments);
	}
	if (error != 0)
		return -error;
	return 0;
}

/* Handles unmount(2). */
static intptr_t
sys_unmount_call(
	const uintptr_t args[6])
{
	struct process *process;
	char directory[PATH_MAX];
	int error;

	process = current_process();
	if (process == NULL || process->cred == NULL)
		return -EINVAL;
	if (!cred_is_superuser(process->cred))
		return -EPERM;
	if (args[1] != 0)
		return -EINVAL;
	error = copyinstr(args[0], directory, sizeof(directory), NULL);
	if (error == 0)
		error = unmount(directory, 0);
	if (error != 0)
		return -error;
	return 0;
}

/* Handles statvfs(2) and fstatvfs(2). */
static intptr_t
sys_statvfs_call(
	const uintptr_t args[6],
	int by_fd)
{
	struct process *process;
	struct statvfs status;
	struct path path;
	struct file *file;
	char pathname[PATH_MAX];
	int error;

	process = current_process();
	file = NULL;

	if (process == NULL || process->cwdi == NULL)
		return -EINVAL;

	/* Finds the mount by descriptor or by path. */
	if (by_fd) {
		file = filedesc_get_ref(process->fd, (int)args[0]);
		if (file == NULL)
			return -EBADF;
		error = mount_statvfs(file->f_path.p_mount, &status);
	} else {
		error = copyinstr(args[0], pathname, sizeof(pathname), NULL);
		if (error == 0)
			error = namei_path_at(process->cwdi, pathname, &path);
		if (error == 0) {
			error = mount_statvfs(path.p_mount, &status);
			path_release(&path);
		}
	}
	if (file != NULL)
		(void)file_close(file);
	if (error == 0)
		error = copyout(&status, args[1], sizeof(status));
	if (error != 0)
		return -error;
	return 0;
}

/* Handles quotactl(2). */
static intptr_t
sys_quotactl_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct quota_control request;
	struct path path;
	char pathname[PATH_MAX];
	int error;
	int allowed;

	process = current_process();
	allowed = 0;

	/* Copies and validates the request. */
	if (process == NULL ||
	    process->cred == NULL ||
	    process->cwdi == NULL ||
	    args[1] == 0)
		return -EINVAL;
	error = copyinstr(args[0], pathname, sizeof(pathname), NULL);
	if (error == 0)
		error = copyin(args[1], &request, sizeof(request));
	if (error == 0 &&
	    (request.size != sizeof(request) ||
	    request.version != ZEDBSD_QUOTA_VERSION ||
	    request.type > ZEDBSD_QUOTA_GROUP ||
	    request.command < ZEDBSD_QUOTA_GET ||
	    request.command > ZEDBSD_QUOTA_SYNC))
		error = EINVAL;

	/* An unprivileged caller may only query its own quotas. */
	if (error == 0) {
		allowed = cred_is_superuser(process->cred);
		if (request.command == ZEDBSD_QUOTA_GET && !allowed) {
			if (request.type == ZEDBSD_QUOTA_USER) {
				allowed = 0;
				if (request.id == process->cred->ruid ||
				    request.id == process->cred->euid ||
				    request.id == process->cred->suid)
					allowed = 1;
			} else {
				allowed = cred_in_group(process->cred,
				    (gid_t)request.id);
			}
		}
		if (!allowed)
			error = EPERM;
	}

	/* Runs the request on the path's mount and copies it back. */
	if (error == 0)
		error = namei_path_at(process->cwdi, pathname, &path);
	if (error == 0) {
		error = mount_quotactl(path.p_mount, &request);
		path_release(&path);
	}
	if (error == 0)
		error = copyout(&request, args[1], sizeof(request));
	if (error != 0)
		return -error;
	return 0;
}

/* Handles snapshotctl(2). */
static intptr_t
sys_snapshotctl_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct snapshot_control request;
	struct path path;
	char pathname[PATH_MAX];
	int error;

	process = current_process();

	/* Only the superuser controls snapshots. */
	if (process == NULL ||
	    process->cred == NULL ||
	    process->cwdi == NULL ||
	    args[1] == 0)
		return -EINVAL;
	if (!cred_is_superuser(process->cred))
		return -EPERM;

	/* Copies and validates the request, then runs it on the path's mount. */
	error = copyinstr(args[0], pathname, sizeof(pathname), NULL);
	if (error == 0)
		error = copyin(args[1], &request, sizeof(request));
	if (error == 0 &&
	    (request.size != sizeof(request) ||
	    request.version != ZEDBSD_SNAPSHOT_VERSION ||
	    request.command < ZEDBSD_SNAPSHOT_CREATE ||
	    request.command > ZEDBSD_SNAPSHOT_STATUS))
		error = EINVAL;
	if (error == 0)
		error = namei_path_at(process->cwdi, pathname, &path);
	if (error == 0) {
		error = mount_snapshotctl(path.p_mount, &request);
		path_release(&path);
	}
	if (error == 0)
		error = copyout(&request, args[1], sizeof(request));
	if (error != 0)
		return -error;
	return 0;
}

/* Handles nanosleep(2). */
static intptr_t
sys_nanosleep_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct timespec request;
	struct timespec remaining;
	uint64_t ticks;
	uint64_t deadline;
	uint64_t left;
	unsigned long irq;
	int error;
	uint64_t now;

	process = current_process();
	error = copyin(args[0], &request, sizeof(request));

	/* Converts the request into a deadline. */
	if (process == NULL || curthread == NULL)
		return -EINVAL;
	if (error != 0)
		return -error;
	error = kern_duration_to_ticks_ceil(&request, &ticks);
	if (error != 0)
		return -error;
	if (ticks == 0)
		return 0;
	error = kern_deadline_after(sched_ticks(), ticks, &deadline);
	if (error != 0)
		return -error;

	/* Sleeps until the deadline, a signal, a stop, or termination. */
	irq = spin_lock_irqsave(&process->lock);
	for (;;) {
		now = sched_ticks();
		if (now >= deadline) {
			spin_unlock_irqrestore(&process->lock, irq);
			return 0;
		}
		if (curthread->terminate_requested) {
			spin_unlock_irqrestore(&process->lock, irq);
			return -EINTR;
		}
		if (process->stop_requested) {
			spin_unlock_irqrestore(&process->lock, irq);
			process_stop_current(0);
			irq = spin_lock_irqsave(&process->lock);
			continue;
		}
		if (!signal_pending_unblocked_locked(curthread)) {
			sched_sleep_locked(deadline, &process->lock);
			continue;
		}
		spin_unlock_irqrestore(&process->lock, irq);
		if (signal_stop_before_return(curthread) ==
		    SIGNAL_STOP_RETURN_REDISPATCH) {
			irq = spin_lock_irqsave(&process->lock);
			continue;
		}

		/* Reports the remaining time to a caller interrupted by a signal. */
		left = kern_deadline_remaining(sched_ticks(), deadline);
		if (args[1] != 0) {
			remaining.tv_sec = (time_t)(left / KERN_CLOCK_HZ);
			remaining.tv_nsec = (long)((left % KERN_CLOCK_HZ) *
			    (KERN_NSEC_PER_SEC / KERN_CLOCK_HZ));
			error = copyout(&remaining, args[1], sizeof(remaining));
			if (error != 0)
				return -error;
		}
		return -EINTR;
	}
}

/* Copies a NULL-terminated user string vector into the exec argument block. */
static int
copy_exec_vector(
	uintptr_t address,
	char **vector,
	unsigned maximum,
	struct syscall_exec_args *copy,
	int optional)
{
	unsigned index;
	size_t length;
	int error;
#ifdef ZEDBSD_USER_ABI_LP64
	uintptr_t pointer;
#else
	uint32_t pointer;
#endif

	/* A missing vector is allowed only where the caller says so. */
	if (address == 0) {
		if (!optional)
			return EFAULT;
		vector[0] = NULL;
		return 0;
	}

	/* Copies each string until the terminating null pointer. */
	for (index = 0; index < maximum; index++) {
		error = copyin(address + index * sizeof(pointer), &pointer,
				   sizeof(pointer));
		if (error != 0)
			return error;
		if (pointer == 0) {
			vector[index] = NULL;
			if (index == 0 && !optional)
				return EINVAL;
			return 0;
		}
		if (copy->used >= sizeof(copy->strings))
			return E2BIG;
		vector[index] = copy->strings + copy->used;
		error = copyinstr(pointer, vector[index],
				  sizeof(copy->strings) - copy->used, &length);
		if (error != 0) {
			if (error == ENAMETOOLONG)
				return E2BIG;
			return error;
		}
		copy->used += length;
	}
	return E2BIG;
}

/* Handles pread(2) and pwrite(2) through a bounce buffer. */
static SYSCALL_EXT intptr_t
sys_positional_call(
	const uintptr_t args[6],
	int writing)
{
	struct process *process;
	struct file *file;
	struct file_io io;
	struct uaccess_pin pin;
	uint8_t small_buffer[SYSCALL_IO_CHUNK];
	uint8_t *buffer = small_buffer;
	size_t capacity = sizeof(small_buffer);
	size_t done;
	size_t length;
	off_t offset;
	uint32_t access;
	enum file_io_kind operation;
	struct ucred *credential;
	intptr_t result;
	int error;
	size_t chunk;
	int limited;
	ssize_t count;

	process = current_process();
	done = 0;
	length = (size_t)args[2];
	offset = (off_t)args[3];

	/* Pins the user buffer and starts the transfer at the offset. */
	if (process == NULL)
		return -EBADF;
	file = filedesc_get_ref(process->fd, (int)args[0]);
	if (file == NULL)
		return -EBADF;
	if (offset < 0) {
		(void)file_close(file);
		return -EINVAL;
	}
	if (writing)
		access = HAL_SPACE_READ;
	else
		access = HAL_SPACE_WRITE;
	error = uaccess_pin(args[1], length, access, &pin);
	if (error != 0) {
		(void)file_close(file);
		return -error;
	}
	if (writing) {
		operation = FILE_IO_PWRITE;
		credential = process->cred;
	} else {
		operation = FILE_IO_PREAD;
		credential = NULL;
	}
	buffer = syscall_regular_buffer(file, length, small_buffer, &capacity);
	error = file_io_begin_cred(file, operation, offset, 0, credential, &io);
	if (error != 0) {
		if (buffer != small_buffer)
			io_pool_release(buffer);
		uaccess_unpin(&pin);
		(void)file_close(file);
		return -error;
	}
	if (writing)
		file_io_set_growth_limit(&io,
		    resource_limit_current(process, RLIMIT_FSIZE));

	/* Copies one chunk at a time in the requested direction. */
	while (done < length) {
		if (length - done > capacity)
			chunk = capacity;
		else
			chunk = length - done;
		limited = 0;
		if (writing) {
			error = copyin_pinned(&pin, done, buffer, chunk);
			if (error != 0)
				goto copy_error;
			io_stats_record(IO_SYSCALL_WRITE, chunk);
			count = file_io_transfer(&io, buffer, chunk);
			limited = file_io_take_growth_limit_hit(&io);
			if (limited)
				(void)signal_send_thread(curthread, SIGXFSZ);
		} else {
			io_stats_record(IO_SYSCALL_READ, chunk);
			count = file_io_transfer(&io, buffer, chunk);
		}
		if (count < 0) {
			if (done != 0)
				result = (intptr_t)done;
			else
				result = count;
			goto out;
		}
		if (count == 0)
			break;
		if (!writing) {
			error = copyout_pinned(&pin, done, buffer, (size_t)count);
			if (error != 0)
				goto copy_error;
		}
		done += (size_t)count;
		if (limited)
			break;
		if ((size_t)count < chunk)
			break;
	}
	result = (intptr_t)done;
	goto out;
copy_error:
	if (done != 0)
		result = (intptr_t)done;
	else
		result = -error;
out:
	result = file_io_complete(&io, result);
	if (buffer != small_buffer)
		io_pool_release(buffer);
	uaccess_unpin(&pin);
	(void)file_close(file);
	return result;
}

/* Handles readv(2) and writev(2) through a bounce buffer. */
static SYSCALL_EXT intptr_t
sys_vector_call(
	const uintptr_t args[6],
	int writing)
{
	struct syscall_iovec vectors[16];
	struct uaccess_pin pins[16];
	struct process *process;
	struct file *file;
	struct file_io io;
	uint8_t small_buffer[SYSCALL_IO_CHUNK];
	uint8_t *buffer = small_buffer;
	size_t capacity = sizeof(small_buffer);
	int count;
	int i;
	int pinned;
	intptr_t total;
	int io_started;
	int error;
	size_t amount;
	size_t length;
	size_t done;
	size_t chunk;
	int limited;
	ssize_t result;
	uint32_t access;
	enum file_io_kind operation;
	struct ucred *credential;

	process = current_process();
	file = NULL;
	count = (int)args[2];
	pinned = 0;
	total = 0;
	io_started = 0;

	/* Copies the vector and pins every element. */
	if (process == NULL || process->fd == NULL)
		return -EBADF;
	if (count < 0 || count > 16)
		return -EINVAL;
	if (count == 0)
		return 0;
	if ((size_t)count > SIZE_MAX / sizeof(vectors[0]))
		return -EOVERFLOW;
	error = copyin(args[1], vectors, (size_t)count * sizeof(vectors[0]));
	if (error != 0)
		return -error;
	if (writing)
		access = HAL_SPACE_READ;
	else
		access = HAL_SPACE_WRITE;
	for (i = 0; i < count; i++) {
		if (vectors[i].length > (uint64_t)SSIZE_MAX - (uint64_t)total) {
			error = EINVAL;
			goto fail;
		}
		error = uaccess_pin((uintptr_t)vectors[i].base,
		    (size_t)vectors[i].length, access, &pins[i]);
		if (error != 0)
			goto fail;
		pinned++;
		total += (intptr_t)vectors[i].length;
	}

	/* Starts the transfer under the size limit. */
	file = filedesc_get_ref(process->fd, (int)args[0]);
	if (file == NULL) {
		error = EBADF;
		goto fail;
	}
	if (writing) {
		operation = FILE_IO_WRITE;
		credential = process->cred;
	} else {
		operation = FILE_IO_READ;
		credential = NULL;
	}
	buffer = syscall_regular_buffer(file, (size_t)total, small_buffer,
	    &capacity);
	error = file_io_begin_cred(file, operation, 0, 0, credential, &io);
	if (error != 0)
		goto fail;
	io_started = 1;
	if (writing)
		file_io_set_growth_limit(&io,
		    resource_limit_current(process, RLIMIT_FSIZE));

	/*
	 * POSIX requires a writev no larger than PIPE_BUF to be
	 * indivisible.  Coalesce it before the one and only pipe backend
	 * call.
	 */
	if (writing && pipe_file_is_pipe(file) &&
	    (uint64_t)total <= KERN_PIPE_BUF) {
		amount = 0;
		for (i = 0; i < count; i++) {
			length = (size_t)vectors[i].length;
			error = copyin_pinned(&pins[i], 0, buffer + amount, length);
			if (error != 0) {
				total = -error;
				goto out;
			}
			amount += length;
		}
		io_stats_record(IO_SYSCALL_WRITE, amount);
		total = file_io_transfer(&io, buffer, amount);
		goto out;
	}

	/* Transfers each element one chunk at a time. */
	total = 0;
	for (i = 0; i < count; i++) {
		done = 0;
		length = (size_t)vectors[i].length;
		while (done < length) {
			if (length - done > capacity)
				chunk = capacity;
			else
				chunk = length - done;
			limited = 0;
			if (writing) {
				error = copyin_pinned(&pins[i], done, buffer, chunk);
				if (error != 0) {
					if (total == 0)
						total = -error;
					goto out;
				}
			}
			io_stats_record(writing ? IO_SYSCALL_WRITE : IO_SYSCALL_READ, chunk);
			result = file_io_transfer(&io, buffer, chunk);
			if (writing) {
				limited = file_io_take_growth_limit_hit(&io);
				if (limited)
					(void)signal_send_thread(curthread, SIGXFSZ);
			}
			if (result < 0) {
				if (total == 0)
					total = result;
				goto out;
			}
			if (result == 0)
				goto out;
			if (!writing) {
				error = copyout_pinned(&pins[i], done, buffer,
				    (size_t)result);
				if (error != 0) {
					if (total == 0)
						total = -error;
					goto out;
				}
			}
			done += (size_t)result;
			total += result;
			if (limited)
				goto out;
			if ((size_t)result < chunk)
				goto out;
			if (!writing && (file->f_inode == NULL ||
			    (file->f_inode->i_type != INODE_REG &&
			     file->f_inode->i_type != INODE_BLOCK)))
				goto out;
		}
	}
	goto out;
fail:
	total = -error;
out:
	if (io_started)
		total = file_io_complete(&io, total);
	if (buffer != small_buffer)
		io_pool_release(buffer);
	if (file != NULL)
		(void)file_close(file);
	while (pinned != 0) {
		pinned--;
		uaccess_unpin(&pins[pinned]);
	}
	return total;
}

/* Handles fsync(2) and fdatasync(2). */
static intptr_t
sys_fsync_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct file *file;
	int error;

	process = current_process();
	if (process != NULL)
		file = filedesc_get_ref(process->fd, (int)args[0]);
	else
		file = NULL;

	/* Uses one drain and observer owner for both direct and syscall fsync. */
	if (file == NULL)
		error = EBADF;
	else if (file_vm_inode(file) == NULL)
		error = EINVAL;
	else
		error = file_fsync(file);
	if (file != NULL)
		(void)file_close(file);
	if (error != 0)
		return -error;
	return 0;
}

/* Handles stat(2), lstat(2), and fstatat(2). */
static intptr_t
sys_stat_path_call(
	const uintptr_t args[6],
	int at,
	int nofollow)
{
	struct process *process;
	struct cwdinfo temporary;
	struct cwdinfo *context;
	struct file *held;
	struct path path;
	struct stat status;
	char pathname[PATH_MAX];
	uintptr_t pathname_address;
	uintptr_t status_address;
	int dirfd;
	unsigned namei_flags;
	int error;

	process = current_process();
	held = NULL;
	if (at) {
		pathname_address = args[1];
		status_address = args[2];
		dirfd = (int)args[0];
	} else {
		pathname_address = args[0];
		status_address = args[1];
		dirfd = AT_FDCWD;
	}

	if (process == NULL || process->cwdi == NULL)
		return -EINVAL;
	if (at && ((int)args[3] & ~AT_SYMLINK_NOFOLLOW) != 0)
		return -EINVAL;

	/* Resolves the path in the requested context. */
	error = copyinstr(pathname_address, pathname, sizeof(pathname), NULL);
	if (error == 0 && pathname[0] == '/') {
		context = process->cwdi;
		held = NULL;
	} else if (error == 0) {
		error = syscall_context_at(process, dirfd, &temporary, &context,
		    &held);
	}
	if (error == 0) {
		namei_flags = 0;
		if (nofollow ||
		    (at && ((int)args[3] & AT_SYMLINK_NOFOLLOW) != 0))
			namei_flags = NAMEI_NOFOLLOW_FINAL;
		error = namei_path_flags_at(context, pathname, namei_flags, &path);
	}
	if (error == 0) {
		error = inode_getattr(path.p_inode, &status);
		path_release(&path);
	}
	if (error == 0)
		error = copyout(&status, status_address, sizeof(status));
	if (held != NULL)
		(void)file_close(held);
	if (error != 0)
		return -error;
	return 0;
}

/* Handles truncate(2) and ftruncate(2). */
static intptr_t
sys_truncate_call(
	const uintptr_t args[6],
	int by_fd)
{
	struct process *process;
	struct inode *inode;
	struct path path;
	struct file *file;
	char pathname[PATH_MAX];
	off_t length;
	int error;
	int limit_exceeded;

	process = current_process();
	inode = NULL;
	file = NULL;
	length = (off_t)args[1];
	limit_exceeded = 0;

	if (process == NULL || length < 0)
		return -EINVAL;

	/* Finds the inode by descriptor or by path. */
	if (by_fd) {
		file = filedesc_get_ref(process->fd, (int)args[0]);
		if (file == NULL)
			return -EBADF;
		if ((file_status_flags_get(file) & O_ACCMODE) == O_RDONLY) {
			(void)file_close(file);
			return -EINVAL;
		}
		inode = file->f_inode;
	} else {
		error = copyinstr(args[0], pathname, sizeof(pathname), NULL);
		if (error != 0)
			return -error;
		error = namei_path_at(process->cwdi, pathname, &path);
		if (error != 0)
			return -error;
		inode = path.p_inode;
	}

	/* Truncates a writable regular file under the size limit. */
	if (inode == NULL || inode->i_type != INODE_REG) {
		if (inode != NULL && inode->i_type == INODE_DIR)
			error = EISDIR;
		else
			error = EINVAL;
	} else {
		error = 0;
		if (!by_fd)
			error = vfs_access(inode, process->cred, W_OK);
		if (error == 0)
			error = inode_truncate_limited_cred(inode, length,
			    resource_limit_current(process, RLIMIT_FSIZE),
			    process->cred, &limit_exceeded);
	}
	if (limit_exceeded)
		(void)signal_send_thread(curthread, SIGXFSZ);
	if (!by_fd)
		path_release(&path);
	else
		(void)file_close(file);
	if (error != 0)
		return -error;
	return 0;
}

/* Performs mkdir, unlink, rmdir, or rename with resolved parents. */
static SYSCALL_EXT intptr_t
sys_mutation_common(
	uint32_t number,
	int old_dirfd,
	uintptr_t old_address,
	uintptr_t option,
	int new_dirfd,
	uintptr_t new_address)
{
	struct process *process;
	struct ucred *credential;
	struct cwdinfo temporary;
	struct cwdinfo other_temporary;
	struct cwdinfo *context;
	struct cwdinfo *other_context;
	struct file *held;
	struct file *other_held;
	struct path parent;
	struct path other_parent;
	struct componentname name;
	struct componentname other_name;
	struct inode_creation_request creation;
	struct inode *created;
	struct inode *source;
	struct inode *target;
	char pathname[PATH_MAX];
	char other_pathname[PATH_MAX];
	char storage[NAME_MAX + 1U];
	char other_storage[NAME_MAX + 1U];
	mode_t process_umask;
	int error;
	int other_valid;
	struct inode *victim;
	int target_error;

	process = current_process();
	credential = NULL;
	held = NULL;
	other_held = NULL;
	created = NULL;
	source = NULL;
	target = NULL;
	other_valid = 0;

	if (process == NULL || process->cwdi == NULL)
		return -EINVAL;
	credential = cred_process_ref(process);
	if (credential == NULL)
		return -EINVAL;
	process_umask = process->umask;

	/* Resolves the parent of the first path. */
	error = copyinstr(old_address, pathname, sizeof(pathname), NULL);
	if (error != 0)
		goto out_held;
	if (pathname[0] == '/') {
		context = process->cwdi;
	} else {
		error = syscall_context_at(process, old_dirfd, &temporary,
		    &context, &held);
		if (error != 0)
			goto out_held;
	}
	error = namei_parent_path_at(context, pathname, &parent, &name,
	    storage);
	if (error != 0)
		goto out_held;

	/* Performs the operation under the mount's namespace transaction. */
	if (number == ZEDBSD_SYS_mkdir) {
		mount_vfs_transaction_enter(parent.p_mount);
		error = inode_creation_request_user(parent.p_inode,
			    credential, INODE_DIR,
			    ((mode_t)option & 07777U) & ~process_umask, 0, NULL,
			    &creation);
		if (error == 0)
			error = inode_mkdir(parent.p_inode, &name, &creation,
			    &created);
		mount_vfs_transaction_leave(parent.p_mount);
	} else if (number == ZEDBSD_SYS_unlink ||
	    number == ZEDBSD_SYS_rmdir) {
		mount_vfs_transaction_enter(parent.p_mount);
		error = mount_namespace_check_name(parent.p_inode, &name);
		if (error == 0)
			error = inode_lookup(parent.p_inode, &name, &victim);
		if (error == 0) {
			error = vfs_may_remove(parent.p_inode, victim, credential);
			inode_release(victim);
		}
		if (error == 0) {
			if (number == ZEDBSD_SYS_unlink)
				error = inode_unlink(parent.p_inode, &name);
			else
				error = inode_rmdir(parent.p_inode, &name);
		}
		mount_vfs_transaction_leave(parent.p_mount);
	} else {
		/* A rename resolves the second parent on the same mount. */
		error = copyinstr(new_address, other_pathname,
		    sizeof(other_pathname), NULL);
		if (error == 0 && other_pathname[0] == '/')
			other_context = process->cwdi;
		else if (error == 0)
			error = syscall_context_at(process, new_dirfd,
			    &other_temporary, &other_context, &other_held);
		if (error == 0) {
			error = namei_parent_path_at(other_context, other_pathname,
			    &other_parent, &other_name, other_storage);
			if (error == 0)
				other_valid = 1;
		}
		if (error == 0 && parent.p_mount != other_parent.p_mount)
			error = EXDEV;
		if (error == 0)
			mount_vfs_transaction_enter(parent.p_mount);
		if (error == 0)
			error = mount_namespace_check_name(parent.p_inode, &name);
		if (error == 0)
			error = mount_namespace_check_name(other_parent.p_inode,
			    &other_name);
		if (error == 0)
			error = inode_lookup(parent.p_inode, &name, &source);
		if (error == 0) {
			target_error = inode_lookup(other_parent.p_inode,
			    &other_name, &target);
			if (target_error != 0 && target_error != ENOENT)
				error = target_error;
		}
		if (error == 0)
			error = vfs_may_rename(parent.p_inode, source,
			    other_parent.p_inode, target, credential);
		if (error == 0) {
			error = inode_rename(parent.p_inode, &name,
			    other_parent.p_inode, &other_name, 0);
			if (error == 0)
				namecache_remove(other_parent.p_inode, &other_name);
		}
		if (target != NULL)
			inode_release(target);
		if (source != NULL)
			inode_release(source);
		if (other_valid && parent.p_mount == other_parent.p_mount)
			mount_vfs_transaction_leave(parent.p_mount);
		if (other_valid)
			path_release(&other_parent);
		if (other_held != NULL)
			(void)file_close(other_held);
	}
	if (created != NULL)
		inode_release(created);
	if (error == 0)
		namecache_remove(parent.p_inode, &name);
	path_release(&parent);
out_held:
	if (held != NULL)
		(void)file_close(held);
	if (credential != NULL)
		cred_release(credential);
	if (error != 0)
		return -error;
	return 0;
}

/* Handles mkdir(2), unlink(2), rmdir(2), and rename(2). */
static intptr_t
sys_mutation_call(
	uint32_t number,
	const uintptr_t args[6])
{
	intptr_t result;

	result = sys_mutation_common(number, AT_FDCWD, args[0], args[1],
		AT_FDCWD, args[1]);
	return result;
}

/* Handles mkdirat(2), unlinkat(2), and renameat(2). */
static intptr_t
sys_mutation_at_call(
	uint32_t number,
	const uintptr_t args[6])
{
	uint32_t operation;
	intptr_t result;

	if (number == ZEDBSD_SYS_mkdirat) {
		result = sys_mutation_common(ZEDBSD_SYS_mkdir, (int)args[0],
			args[1], args[2], AT_FDCWD, 0);
		return result;
	}
	if (number == ZEDBSD_SYS_unlinkat) {
		if ((args[2] & ~AT_REMOVEDIR) != 0)
			return -EINVAL;
		if ((args[2] & AT_REMOVEDIR) != 0)
			operation = ZEDBSD_SYS_rmdir;
		else
			operation = ZEDBSD_SYS_unlink;
		result = sys_mutation_common(operation, (int)args[0],
			args[1], 0, AT_FDCWD, 0);
		return result;
	}
	result = sys_mutation_common(ZEDBSD_SYS_rename, (int)args[0], args[1],
		0, (int)args[2], args[3]);
	return result;
}

/* Handles umask(2). */
static intptr_t
sys_umask_call(
	const uintptr_t args[6])
{
	struct process *process;
	mode_t old;

	process = current_process();
	if (process == NULL)
		return -EINVAL;
	old = process->umask;
	process->umask = (mode_t)args[0] & 0777U;
	return old;
}

/* Installs a replacement credential on a process. */
static int
replace_cred(
	struct process *process,
	struct ucred *replacement)
{
	int error;

	error = process_cred_replace(process, replacement);
	return error;
}

/* Handles getuid(2), geteuid(2), getgid(2), getegid(2), and getgroups(2). */
static intptr_t
sys_cred_get_call(
	uint32_t number,
	const uintptr_t args[6])
{
	struct process *process;
	struct ucred *cred;
	intptr_t result;
	int error;

	process = current_process();
	if (process != NULL)
		cred = cred_current_ref();
	else
		cred = NULL;
	error = 0;

	if (cred == NULL)
		return -EINVAL;
	switch (number) {
	case ZEDBSD_SYS_getuid:
		result = cred->ruid;
		break;
	case ZEDBSD_SYS_geteuid:
		result = cred->euid;
		break;
	case ZEDBSD_SYS_getgid:
		result = cred->rgid;
		break;
	case ZEDBSD_SYS_getegid:
		result = cred->egid;
		break;
	case ZEDBSD_SYS_getgroups:
		/* A zero count only asks for the group count. */
		if ((int)args[0] < 0 ||
		    ((unsigned)args[0] != 0 &&
		    (unsigned)args[0] < cred->ngroups)) {
			error = EINVAL;
			break;
		}
		if (args[0] != 0)
			error = copyout(cred->groups, args[1],
			    cred->ngroups * sizeof(cred->groups[0]));
		result = cred->ngroups;
		break;
	default:
		error = EINVAL;
		result = 0;
		break;
	}
	cred_release(cred);
	if (error != 0)
		return -error;
	return result;
}

/* Tests whether a credential may adopt a user id. */
static int
uid_permitted(
	const struct ucred *cred,
	uid_t id)
{
	if (cred_is_superuser(cred))
		return 1;
	if (id == cred->ruid)
		return 1;
	if (id == cred->euid)
		return 1;
	if (id == cred->suid)
		return 1;
	return 0;
}

/* Tests whether a credential may adopt a group id. */
static int
gid_permitted(
	const struct ucred *cred,
	gid_t id)
{
	if (cred_is_superuser(cred))
		return 1;
	if (id == cred->rgid)
		return 1;
	if (id == cred->egid)
		return 1;
	if (id == cred->sgid)
		return 1;
	return 0;
}

/* Handles getresuid(2) and getresgid(2). */
static intptr_t
sys_cred_getres_call(
	uint32_t number,
	const uintptr_t args[6])
{
	struct uaccess_pin pins[3];
	struct ucred *cred;
	uint32_t values[3];
	size_t i;
	int error;

	error = 0;

	/* Pins all three outputs before reading the credential. */
	memset(pins, 0, sizeof(pins));
	for (i = 0; i < 3; i++) {
		error = uaccess_pin(args[i], sizeof(values[i]), HAL_SPACE_WRITE,
		    &pins[i]);
		if (error != 0)
			break;
	}
	if (error == 0)
		cred = cred_current_ref();
	else
		cred = NULL;
	if (error == 0 && cred == NULL)
		error = EINVAL;
	if (error == 0 && number == ZEDBSD_SYS_getresuid) {
		values[0] = cred->ruid;
		values[1] = cred->euid;
		values[2] = cred->suid;
	} else if (error == 0 && number == ZEDBSD_SYS_getresgid) {
		values[0] = cred->rgid;
		values[1] = cred->egid;
		values[2] = cred->sgid;
	} else if (error == 0) {
		error = EINVAL;
	}
	for (i = 0; error == 0 && i < 3; i++)
		error = copyout_pinned(&pins[i], 0, &values[i], sizeof(values[i]));
	cred_release(cred);
	for (i = 0; i < 3; i++)
		uaccess_unpin(&pins[i]);
	if (error != 0)
		return -error;
	return 0;
}

/* Handles getentropy(2). */
static intptr_t
sys_getentropy_call(
	const uintptr_t args[6])
{
	struct uaccess_pin pin;
	uint8_t entropy[GETENTROPY_MAX];
	size_t size;
	int error;

	size = (size_t)args[1];

	memset(&pin, 0, sizeof(pin));
	if (size > GETENTROPY_MAX)
		return -EINVAL;
	if (size == 0U)
		return 0;
	error = uaccess_pin(args[0], size, HAL_SPACE_WRITE, &pin);
	if (error != 0)
		return -error;
	if (!hal_entropy_fill(entropy, size))
		error = ENOSYS;
	else
		error = copyout_pinned(&pin, 0, entropy, size);
	memset_explicit(entropy, 0, sizeof(entropy));
	uaccess_unpin(&pin);
	if (error != 0)
		return -error;
	return 0;
}

/* Copies between two pinned user objects in bounded chunks. */
static int
user_atomic_copy(
	const struct uaccess_pin *source,
	struct uaccess_pin *destination,
	size_t size)
{
	uint8_t bytes[SYSCALL_ATOMIC_CHUNK];
	size_t offset;
	int error;
	size_t amount;

	offset = 0;
	while (offset < size) {
		amount = size - offset;
		if (amount > sizeof(bytes))
			amount = sizeof(bytes);
		error = copyin_pinned(source, offset, bytes, amount);
		if (error == 0)
			error = copyout_pinned(destination, offset, bytes, amount);
		if (error != 0)
			return error;
		offset += amount;
	}
	return 0;
}

/* Compares two pinned user objects in bounded chunks. */
static int
user_atomic_equal(
	const struct uaccess_pin *left,
	const struct uaccess_pin *right,
	size_t size,
	int *equal)
{
	uint8_t left_bytes[SYSCALL_ATOMIC_CHUNK];
	uint8_t right_bytes[SYSCALL_ATOMIC_CHUNK];
	size_t offset;
	int error;
	size_t amount;

	offset = 0;
	*equal = 1;
	while (offset < size) {
		amount = size - offset;
		if (amount > sizeof(left_bytes))
			amount = sizeof(left_bytes);
		error = copyin_pinned(left, offset, left_bytes, amount);
		if (error == 0)
			error = copyin_pinned(right, offset, right_bytes, amount);
		if (error != 0)
			return error;
		if (memcmp(left_bytes, right_bytes, amount) != 0) {
			*equal = 0;
			return 0;
		}
		offset += amount;
	}
	return 0;
}

/* Handles the atomic(2) fallback for non-lock-free objects. */
static intptr_t
sys_atomic_call(
	const uintptr_t args[6])
{
	struct uaccess_pin object;
	struct uaccess_pin first;
	struct uaccess_pin second;
	size_t size;
	unsigned operation;
	uint32_t object_prot;
	uint32_t first_prot;
	uint32_t second_prot;
	int equal;
	int error;

	size = (size_t)args[3];
	operation = (unsigned)args[4];
	second_prot = 0;
	equal = 0;

	/* Pins every operand with the access its operation needs. */
	memset(&object, 0, sizeof(object));
	memset(&first, 0, sizeof(first));
	memset(&second, 0, sizeof(second));
	if (size == 0 ||
	    operation > ZEDBSD_ATOMIC_COMPARE_EXCHANGE ||
	    args[0] == 0 ||
	    args[1] == 0 ||
	    (operation >= ZEDBSD_ATOMIC_EXCHANGE && args[2] == 0))
		return -EINVAL;
	if (operation == ZEDBSD_ATOMIC_LOAD)
		object_prot = HAL_SPACE_READ;
	else if (operation == ZEDBSD_ATOMIC_STORE)
		object_prot = HAL_SPACE_WRITE;
	else
		object_prot = HAL_SPACE_READ | HAL_SPACE_WRITE;
	if (operation == ZEDBSD_ATOMIC_LOAD)
		first_prot = HAL_SPACE_WRITE;
	else if (operation == ZEDBSD_ATOMIC_COMPARE_EXCHANGE)
		first_prot = HAL_SPACE_READ | HAL_SPACE_WRITE;
	else
		first_prot = HAL_SPACE_READ;
	if (operation == ZEDBSD_ATOMIC_EXCHANGE)
		second_prot = HAL_SPACE_WRITE;
	else if (operation == ZEDBSD_ATOMIC_COMPARE_EXCHANGE)
		second_prot = HAL_SPACE_READ;
	error = uaccess_pin(args[0], size, object_prot, &object);
	if (error == 0)
		error = uaccess_pin(args[1], size, first_prot, &first);
	if (error == 0 && second_prot != 0)
		error = uaccess_pin(args[2], size, second_prot, &second);
	if (error != 0)
		goto out;

	/*
	 * One kernel mutex covers every non-lock-free atomic object,
	 * including shared mappings seen at different virtual addresses by
	 * two processes.  Pins make every backing page resident before the
	 * transaction starts.
	 */
	mutex_lock(&user_atomic_lock);
	switch (operation) {
	case ZEDBSD_ATOMIC_LOAD:
		error = user_atomic_copy(&object, &first, size);
		break;
	case ZEDBSD_ATOMIC_STORE:
		error = user_atomic_copy(&first, &object, size);
		break;
	case ZEDBSD_ATOMIC_EXCHANGE:
		error = user_atomic_copy(&object, &second, size);
		if (error == 0)
			error = user_atomic_copy(&first, &object, size);
		break;
	case ZEDBSD_ATOMIC_COMPARE_EXCHANGE:
		error = user_atomic_equal(&object, &first, size, &equal);
		if (error == 0 && equal)
			error = user_atomic_copy(&second, &object, size);
		else if (error == 0)
			error = user_atomic_copy(&object, &first, size);
		break;
	default:
		error = EINVAL;
		break;
	}
	mutex_unlock(&user_atomic_lock);
out:
	uaccess_unpin(&second);
	uaccess_unpin(&first);
	uaccess_unpin(&object);
	if (error != 0)
		return -error;
	if (operation == ZEDBSD_ATOMIC_COMPARE_EXCHANGE)
		return equal;
	return 0;
}

/* Handles the credential-setting system calls. */
static intptr_t
sys_cred_set_call(
	uint32_t number,
	const uintptr_t args[6])
{
	struct process *process;
	struct ucred *old;
	struct ucred *cred;
	uid_t ruid;
	uid_t euid;
	gid_t rgid;
	gid_t egid;
	uid_t uids[3];
	gid_t gids[3];
	unsigned i;
	int error;

	process = current_process();
	if (process != NULL)
		old = cred_current_ref();
	else
		old = NULL;

	/* Works on a copy of the current credential. */
	if (old == NULL || process == &process0) {
		cred_release(old);
		return -EPERM;
	}
	cred = cred_copy(old);
	if (cred == NULL) {
		cred_release(old);
		return -ENOMEM;
	}
	switch (number) {
	case ZEDBSD_SYS_setuid:
		if (!uid_permitted(old, (uid_t)args[0])) {
			error = EPERM;
			break;
		}
		if (cred_is_superuser(old)) {
			cred->ruid = (uid_t)args[0];
			cred->euid = (uid_t)args[0];
			cred->suid = (uid_t)args[0];
		} else {
			cred->euid = (uid_t)args[0];
		}
		error = 0;
		break;
	case ZEDBSD_SYS_seteuid:
		if (!uid_permitted(old, (uid_t)args[0])) {
			error = EPERM;
			break;
		}
		cred->euid = (uid_t)args[0];
		error = 0;
		break;
	case ZEDBSD_SYS_setgid:
		if (!gid_permitted(old, (gid_t)args[0])) {
			error = EPERM;
			break;
		}
		if (cred_is_superuser(old)) {
			cred->rgid = (gid_t)args[0];
			cred->egid = (gid_t)args[0];
			cred->sgid = (gid_t)args[0];
		} else {
			cred->egid = (gid_t)args[0];
		}
		error = 0;
		break;
	case ZEDBSD_SYS_setegid:
		if (!gid_permitted(old, (gid_t)args[0])) {
			error = EPERM;
			break;
		}
		cred->egid = (gid_t)args[0];
		error = 0;
		break;
	case ZEDBSD_SYS_setreuid:
		ruid = (uid_t)args[0];
		euid = (uid_t)args[1];
		if ((ruid != (uid_t)-1 && !uid_permitted(old, ruid)) ||
		    (euid != (uid_t)-1 && !uid_permitted(old, euid))) {
			error = EPERM;
			break;
		}
		if (ruid != (uid_t)-1)
			cred->ruid = ruid;
		if (euid != (uid_t)-1)
			cred->euid = euid;
		error = 0;
		break;
	case ZEDBSD_SYS_setregid:
		rgid = (gid_t)args[0];
		egid = (gid_t)args[1];
		if ((rgid != (gid_t)-1 && !gid_permitted(old, rgid)) ||
		    (egid != (gid_t)-1 && !gid_permitted(old, egid))) {
			error = EPERM;
			break;
		}
		if (rgid != (gid_t)-1)
			cred->rgid = rgid;
		if (egid != (gid_t)-1)
			cred->egid = egid;
		error = 0;
		break;
	case ZEDBSD_SYS_setresuid:
		uids[0] = (uid_t)args[0];
		uids[1] = (uid_t)args[1];
		uids[2] = (uid_t)args[2];
		for (i = 0; i < 3; i++) {
			if (uids[i] != (uid_t)-1 && !uid_permitted(old, uids[i])) {
				error = EPERM;
				break;
			}
		}
		if (i != 3)
			break;
		if (uids[0] != (uid_t)-1)
			cred->ruid = uids[0];
		if (uids[1] != (uid_t)-1)
			cred->euid = uids[1];
		if (uids[2] != (uid_t)-1)
			cred->suid = uids[2];
		error = 0;
		break;
	case ZEDBSD_SYS_setresgid:
		gids[0] = (gid_t)args[0];
		gids[1] = (gid_t)args[1];
		gids[2] = (gid_t)args[2];
		for (i = 0; i < 3; i++) {
			if (gids[i] != (gid_t)-1 && !gid_permitted(old, gids[i])) {
				error = EPERM;
				break;
			}
		}
		if (i != 3)
			break;
		if (gids[0] != (gid_t)-1)
			cred->rgid = gids[0];
		if (gids[1] != (gid_t)-1)
			cred->egid = gids[1];
		if (gids[2] != (gid_t)-1)
			cred->sgid = gids[2];
		error = 0;
		break;
	case ZEDBSD_SYS_setgroups:
		if (!cred_is_superuser(old)) {
			error = EPERM;
			break;
		}
		if (args[0] > KERN_NGROUPS_MAX) {
			error = EINVAL;
			break;
		}
		if (args[0] == 0)
			error = 0;
		else
			error = copyin(args[1], cred->groups,
			    (size_t)args[0] * sizeof(cred->groups[0]));
		if (error == 0)
			cred->ngroups = (unsigned)args[0];
		break;
	default:
		error = EINVAL;
		break;
	}

	/* Installs the copy, or drops it on failure. */
	if (error == 0) {
		error = replace_cred(process, cred);
		if (error != 0)
			cred_release(cred);
	} else {
		cred_release(cred);
	}
	cred_release(old);
	if (error != 0)
		return -error;
	return 0;
}

/* Handles access(2) with the real credential. */
static intptr_t
sys_access_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct path path;
	struct ucred real;
	char pathname[PATH_MAX];
	int error;

	process = current_process();
	if (process == NULL ||
	    process->cred == NULL ||
	    ((int)args[1] & ~(R_OK | W_OK | X_OK)) != 0)
		return -EINVAL;
	error = copyinstr(args[0], pathname, sizeof(pathname), NULL);
	if (error == 0)
		error = namei_path_at(process->cwdi, pathname, &path);
	if (error == 0) {
		real = *process->cred;
		real.euid = real.ruid;
		real.egid = real.rgid;
		error = vfs_access(path.p_inode, &real, (int)args[1]);
		path_release(&path);
	}
	if (error != 0)
		return -error;
	return 0;
}

/* Resolves a user path relative to a dirfd, holding the directory file. */
static SYSCALL_EXT int
sys_resolve_path_at(
	struct process *process,
	int dirfd,
	uintptr_t address,
	unsigned namei_flags,
	struct path *path,
	struct file **held)
{
	struct cwdinfo temporary;
	struct cwdinfo *context;
	char pathname[PATH_MAX];
	int error;

	if (process == NULL || process->cwdi == NULL || path == NULL || held == NULL)
		return EINVAL;
	*held = NULL;
	error = copyinstr(address, pathname, sizeof(pathname), NULL);
	if (error != 0)
		return error;
	if (pathname[0] == '/') {
		context = process->cwdi;
	} else {
		error = syscall_context_at(process, dirfd, &temporary, &context, held);
		if (error != 0)
			return error;
	}
	error = namei_path_flags_at(context, pathname, namei_flags, path);
	if (error != 0 && *held != NULL) {
		(void)file_close(*held);
		*held = NULL;
	}
	return error;
}

/* Takes an inode reference by descriptor or by path for the xattr calls. */
static int
sys_inode_ref_acquire(
	struct process *process,
	uintptr_t object,
	int by_fd,
	int nofollow,
	struct syscall_inode_ref *reference)
{
	unsigned namei_flags;
	int error;

	memset(reference, 0, sizeof(*reference));
	if (process == NULL || process->fd == NULL || process->cwdi == NULL)
		return EINVAL;
	if (by_fd) {
		reference->file = filedesc_get_ref(process->fd, (int)object);
		if (reference->file == NULL || reference->file->f_inode == NULL) {
			if (reference->file != NULL)
				(void)file_close(reference->file);
			memset(reference, 0, sizeof(*reference));
			return EBADF;
		}
		reference->inode = reference->file->f_inode;
		return 0;
	}
	if (nofollow)
		namei_flags = NAMEI_NOFOLLOW_FINAL;
	else
		namei_flags = 0;
	error = sys_resolve_path_at(process, AT_FDCWD, object, namei_flags,
		&reference->path, &reference->held);
	if (error == 0) {
		reference->inode = reference->path.p_inode;
		reference->has_path = 1;
	}
	return error;
}

/* Releases an inode reference taken by sys_inode_ref_acquire(). */
static void
sys_inode_ref_release(
	struct syscall_inode_ref *reference)
{
	if (reference->has_path)
		path_release(&reference->path);
	if (reference->held != NULL)
		(void)file_close(reference->held);
	if (reference->file != NULL)
		(void)file_close(reference->file);
}

/* Handles getxattr(2), lgetxattr(2), and fgetxattr(2). */
static SYSCALL_EXT intptr_t
sys_getxattr_call(
	const uintptr_t args[6],
	int by_fd,
	int nofollow)
{
	struct process *process;
	struct syscall_inode_ref reference;
	char name[INODE_XATTR_NAME_MAX + 1U];
	void *value;
	ssize_t result;
	size_t size;
	int error;

	process = current_process();
	value = NULL;
	size = (size_t)args[3];

	if (process == NULL ||
	    process->cred == NULL ||
	    size > INODE_XATTR_SIZE_MAX ||
	    (size != 0 && args[2] == 0))
		return -EINVAL;
	error = copyinstr(args[1], name, sizeof(name), NULL);
	if (error == 0)
		error = sys_inode_ref_acquire(process, args[0], by_fd, nofollow,
			&reference);
	if (error != 0)
		return -error;
	if (size != 0) {
		value = kern_malloc(size);
		if (value == NULL) {
			sys_inode_ref_release(&reference);
			return -ENOMEM;
		}
	}

	/* A zero size only asks for the attribute length. */
	result = vfs_getxattr(reference.inode, process->cred, name, value, size);
	if (result >= 0 && size != 0 && (size_t)result > size) {
		error = ERANGE;
	} else if (result >= 0 && result != 0 && size != 0) {
		error = copyout(value, args[2], (size_t)result);
	} else {
		if (result < 0)
			error = (int)-result;
		else
			error = 0;
	}
	kern_free(value);
	sys_inode_ref_release(&reference);
	if (error != 0)
		return -error;
	return result;
}

/* Handles setxattr(2), lsetxattr(2), and fsetxattr(2). */
static SYSCALL_EXT intptr_t
sys_setxattr_call(
	const uintptr_t args[6],
	int by_fd,
	int nofollow)
{
	struct process *process;
	struct syscall_inode_ref reference;
	char name[INODE_XATTR_NAME_MAX + 1U];
	void *value;
	size_t size;
	int error;

	process = current_process();
	value = NULL;
	size = (size_t)args[3];

	if (process == NULL ||
	    process->cred == NULL ||
	    size > INODE_XATTR_SIZE_MAX ||
	    (size != 0 && args[2] == 0))
		return -EINVAL;
	error = copyinstr(args[1], name, sizeof(name), NULL);
	if (error == 0 && size != 0) {
		value = kern_malloc(size);
		if (value == NULL)
			error = ENOMEM;
		else
			error = copyin(args[2], value, size);
	}
	if (error == 0)
		error = sys_inode_ref_acquire(process, args[0], by_fd, nofollow,
			&reference);
	if (error == 0) {
		error = vfs_setxattr(reference.inode, process->cred, name, value,
			size, (unsigned)args[4]);
		sys_inode_ref_release(&reference);
	}
	kern_free(value);
	if (error != 0)
		return -error;
	return 0;
}

/* Handles listxattr(2), llistxattr(2), and flistxattr(2). */
static SYSCALL_EXT intptr_t
sys_listxattr_call(
	const uintptr_t args[6],
	int by_fd,
	int nofollow)
{
	struct process *process;
	struct syscall_inode_ref reference;
	void *list;
	ssize_t result;
	size_t size;
	int error;

	process = current_process();
	list = NULL;
	size = (size_t)args[2];

	if (process == NULL ||
	    process->cred == NULL ||
	    size > INODE_XATTR_SIZE_MAX ||
	    (size != 0 && args[1] == 0))
		return -EINVAL;
	error = sys_inode_ref_acquire(process, args[0], by_fd, nofollow,
		&reference);
	if (error != 0)
		return -error;
	if (size != 0) {
		list = kern_malloc(size);
		if (list == NULL) {
			sys_inode_ref_release(&reference);
			return -ENOMEM;
		}
	}

	/* A zero size only asks for the list length. */
	result = vfs_listxattr(reference.inode, process->cred, list, size);
	if (result >= 0 && size != 0 && (size_t)result > size) {
		error = ERANGE;
	} else if (result >= 0 && result != 0 && size != 0) {
		error = copyout(list, args[1], (size_t)result);
	} else {
		if (result < 0)
			error = (int)-result;
		else
			error = 0;
	}
	kern_free(list);
	sys_inode_ref_release(&reference);
	if (error != 0)
		return -error;
	return result;
}

/* Handles removexattr(2), lremovexattr(2), and fremovexattr(2). */
static SYSCALL_EXT intptr_t
sys_removexattr_call(
	const uintptr_t args[6],
	int by_fd,
	int nofollow)
{
	struct process *process;
	struct syscall_inode_ref reference;
	char name[INODE_XATTR_NAME_MAX + 1U];
	int error;

	process = current_process();
	if (process == NULL || process->cred == NULL)
		return -EINVAL;
	error = copyinstr(args[1], name, sizeof(name), NULL);
	if (error == 0)
		error = sys_inode_ref_acquire(process, args[0], by_fd, nofollow,
			&reference);
	if (error == 0) {
		error = vfs_removexattr(reference.inode, process->cred, name);
		sys_inode_ref_release(&reference);
	}
	if (error != 0)
		return -error;
	return 0;
}

/* Tests whether a credential owns an inode or is the superuser. */
static int
inode_chmod_allowed(
	const struct inode *inode,
	const struct ucred *cred)
{
	if (inode == NULL)
		return 0;
	if (cred == NULL)
		return 0;
	if (cred_is_superuser(cred))
		return 1;
	if (cred->euid == inode->i_uid)
		return 1;
	return 0;
}

/* Performs chmod, fchmod, or fchmodat. */
static SYSCALL_EXT intptr_t
sys_chmod_common(
	int dirfd,
	uintptr_t pathname,
	int fd,
	mode_t mode,
	int flags)
{
	struct process *process;
	struct file *file;
	struct file *held;
	struct path path;
	struct inode *inode;
	struct stat status;
	unsigned namei_flags;
	int by_fd;
	int error;

	process = current_process();
	file = NULL;
	held = NULL;
	by_fd = fd >= 0;

	if (process == NULL ||
	    process->cred == NULL ||
	    (flags & ~AT_SYMLINK_NOFOLLOW) != 0)
		return -EINVAL;

	/* Finds the inode by descriptor or by path. */
	if (by_fd) {
		file = filedesc_get_ref(process->fd, fd);
		if (file == NULL || file->f_inode == NULL) {
			if (file != NULL)
				(void)file_close(file);
			return -EBADF;
		}
		inode = file->f_inode;
	} else {
		if ((flags & AT_SYMLINK_NOFOLLOW) != 0)
			namei_flags = NAMEI_NOFOLLOW_FINAL;
		else
			namei_flags = 0;
		error = sys_resolve_path_at(process, dirfd, pathname, namei_flags,
			&path, &held);
		if (error != 0)
			return -error;
		inode = path.p_inode;
	}

	/* A non-member owner cannot set the set-group-id bit. */
	if (!inode_chmod_allowed(inode, process->cred)) {
		error = EPERM;
	} else {
		error = inode_getattr(inode, &status);
		if (error == 0) {
			status.st_mode = (status.st_mode & S_IFMT) | (mode & 07777U);
			if (!cred_is_superuser(process->cred) &&
			    !cred_in_group(process->cred, inode->i_gid))
				status.st_mode &= ~(mode_t)S_ISGID;
			error = inode_setattr(inode, &status, INODE_ATTR_MODE);
		}
	}
	if (by_fd) {
		(void)file_close(file);
	} else {
		path_release(&path);
		if (held != NULL)
			(void)file_close(held);
	}
	if (error != 0)
		return -error;
	return 0;
}

/* Performs chown, lchown, fchown, or fchownat. */
static SYSCALL_EXT intptr_t
sys_chown_common(
	int dirfd,
	uintptr_t pathname,
	int fd,
	uid_t uid,
	gid_t gid,
	int flags)
{
	struct process *process;
	struct file *file;
	struct file *held;
	struct path path;
	struct inode *inode;
	struct stat status;
	unsigned mask;
	unsigned namei_flags;
	int by_fd;
	int error;

	process = current_process();
	file = NULL;
	held = NULL;
	mask = 0;
	by_fd = fd >= 0;

	if (process == NULL ||
	    process->cred == NULL ||
	    (flags & ~AT_SYMLINK_NOFOLLOW) != 0)
		return -EINVAL;

	/* Finds the inode by descriptor or by path. */
	if (by_fd) {
		file = filedesc_get_ref(process->fd, fd);
		if (file == NULL || file->f_inode == NULL) {
			if (file != NULL)
				(void)file_close(file);
			return -EBADF;
		}
		inode = file->f_inode;
	} else {
		if ((flags & AT_SYMLINK_NOFOLLOW) != 0)
			namei_flags = NAMEI_NOFOLLOW_FINAL;
		else
			namei_flags = 0;
		error = sys_resolve_path_at(process, dirfd, pathname, namei_flags,
			&path, &held);
		if (error != 0)
			return -error;
		inode = path.p_inode;
	}

	/* Applies the requested ids and clears set-id where required. */
	error = vfs_may_chown(inode, process->cred, uid, gid);
	if (error == 0) {
		error = inode_getattr(inode, &status);
		if (error == 0 && uid != (uid_t)-1) {
			status.st_uid = uid;
			mask |= INODE_ATTR_UID;
		}
		if (error == 0 && gid != (gid_t)-1) {
			status.st_gid = gid;
			mask |= INODE_ATTR_GID;
		}

		/*
		 * A successful unprivileged chown clears set-id even when
		 * both requested IDs are unchanged (or both are the -1
		 * sentinel).  A privileged caller retains the historical
		 * behavior of clearing on an explicit ownership request.
		 */
		if (error == 0 && (status.st_mode & (S_ISUID | S_ISGID)) != 0 &&
		    (!cred_is_superuser(process->cred) || uid != (uid_t)-1 ||
		     gid != (gid_t)-1)) {
			status.st_mode &= ~(mode_t)(S_ISUID | S_ISGID);
			mask |= INODE_ATTR_MODE;
		}
		if (error == 0 && mask != 0)
			error = inode_setattr(inode, &status, mask);
	}
	if (by_fd) {
		(void)file_close(file);
	} else {
		path_release(&path);
		if (held != NULL)
			(void)file_close(held);
	}
	if (error != 0)
		return -error;
	return 0;
}

/* Tests whether a utimens nanosecond field is a valid value or marker. */
static int
valid_utime_nsec(
	long nanoseconds)
{
	if (nanoseconds >= 0 && nanoseconds < 1000000000L)
		return 1;
	if (nanoseconds == UTIME_NOW)
		return 1;
	if (nanoseconds == UTIME_OMIT)
		return 1;
	return 0;
}

/* Performs utimensat or futimens. */
static SYSCALL_EXT intptr_t
sys_utimens_common(
	int dirfd,
	uintptr_t pathname,
	int fd,
	uintptr_t times_address,
	int flags)
{
	struct process *process;
	struct file *file;
	struct file *held;
	struct path path;
	struct inode *inode;
	struct stat status;
	struct timespec times[2];
	unsigned mask;
	unsigned namei_flags;
	int by_fd;
	int explicit_time;
	int error;

	process = current_process();
	file = NULL;
	held = NULL;
	mask = 0;
	by_fd = fd >= 0;
	explicit_time = 0;

	if (process == NULL ||
	    process->cred == NULL ||
	    (flags & ~AT_SYMLINK_NOFOLLOW) != 0)
		return -EINVAL;

	/* A missing times array means now for both timestamps. */
	if (times_address == 0) {
		memset(times, 0, sizeof(times));
		times[0].tv_nsec = UTIME_NOW;
		times[1].tv_nsec = UTIME_NOW;
	} else {
		error = copyin(times_address, times, sizeof(times));
		if (error != 0)
			return -error;
		if (!valid_utime_nsec(times[0].tv_nsec) ||
		    !valid_utime_nsec(times[1].tv_nsec))
			return -EINVAL;
	}

	/* Finds the inode by descriptor or by path. */
	if (by_fd) {
		file = filedesc_get_ref(process->fd, fd);
		if (file == NULL || file->f_inode == NULL) {
			if (file != NULL)
				(void)file_close(file);
			return -EBADF;
		}
		inode = file->f_inode;
	} else {
		if ((flags & AT_SYMLINK_NOFOLLOW) != 0)
			namei_flags = NAMEI_NOFOLLOW_FINAL;
		else
			namei_flags = 0;
		error = sys_resolve_path_at(process, dirfd, pathname, namei_flags,
			&path, &held);
		if (error != 0)
			return -error;
		inode = path.p_inode;
	}

	/* Builds the attribute mask from the two timestamps. */
	error = inode_getattr(inode, &status);
	if (error == 0 && times[0].tv_nsec != UTIME_OMIT) {
		if (times[0].tv_nsec == UTIME_NOW) {
			mask |= INODE_ATTR_ATIME_NOW;
		} else {
			status.st_atim = times[0];
			mask |= INODE_ATTR_ATIME;
			explicit_time = 1;
		}
	}
	if (error == 0 && times[1].tv_nsec != UTIME_OMIT) {
		if (times[1].tv_nsec == UTIME_NOW) {
			mask |= INODE_ATTR_MTIME_NOW;
		} else {
			status.st_mtim = times[1];
			mask |= INODE_ATTR_MTIME;
			explicit_time = 1;
		}
	}

	/* An explicit time needs ownership; "now" needs write access. */
	if (error == 0 && mask != 0 &&
	    !inode_chmod_allowed(inode, process->cred)) {
		if (explicit_time)
			error = EPERM;
		else if (vfs_access(inode, process->cred, W_OK) != 0)
			error = EACCES;
	}
	if (error == 0 && mask != 0)
		error = inode_setattr(inode, &status, mask);
	if (by_fd) {
		(void)file_close(file);
	} else {
		path_release(&path);
		if (held != NULL)
			(void)file_close(held);
	}
	if (error != 0)
		return -error;
	return 0;
}

/* Handles faccessat(2). */
static SYSCALL_EXT intptr_t
sys_faccessat_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct file *held;
	struct path path;
	struct ucred check;
	unsigned namei_flags;
	int mode;
	int flags;
	int error;

	process = current_process();
	held = NULL;
	mode = (int)args[2];
	flags = (int)args[3];

	if (process == NULL ||
	    process->cred == NULL ||
	    (mode & ~(R_OK | W_OK | X_OK)) != 0 ||
	    (flags & ~(AT_EACCESS | AT_SYMLINK_NOFOLLOW)) != 0)
		return -EINVAL;

	/* Checks with the real credential unless AT_EACCESS is given. */
	if ((flags & AT_SYMLINK_NOFOLLOW) != 0)
		namei_flags = NAMEI_NOFOLLOW_FINAL;
	else
		namei_flags = 0;
	error = sys_resolve_path_at(process, (int)args[0], args[1], namei_flags,
		&path, &held);
	if (error == 0) {
		check = *process->cred;
		if ((flags & AT_EACCESS) == 0) {
			check.euid = check.ruid;
			check.egid = check.rgid;
		}
		error = vfs_access(path.p_inode, &check, mode);
		path_release(&path);
	}
	if (held != NULL)
		(void)file_close(held);
	if (error != 0)
		return -error;
	return 0;
}

/* Resolves the parent directory and final component of a path relative to a dirfd. */
static SYSCALL_EXT int
sys_parent_path_at(
	struct process *process,
	int dirfd,
	const char *pathname,
	struct cwdinfo *temporary,
	struct cwdinfo **context,
	struct file **held,
	struct path *parent,
	struct componentname *name,
	char storage[NAME_MAX + 1U])
{
	int error;

	*held = NULL;
	if (pathname[0] == '/') {
		*context = process->cwdi;
	} else {
		error = syscall_context_at(process, dirfd, temporary, context, held);
		if (error != 0)
			return error;
	}
	error = namei_parent_path_at(*context, pathname, parent, name, storage);
	if (error != 0 && *held != NULL) {
		(void)file_close(*held);
		*held = NULL;
	}
	return error;
}

/* Handles linkat(2). */
static SYSCALL_EXT intptr_t
sys_linkat_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct cwdinfo temporary;
	struct cwdinfo *context;
	struct file *old_held;
	struct file *new_held;
	struct path target;
	struct path parent;
	struct componentname name;
	char new_path[PATH_MAX];
	char storage[NAME_MAX + 1U];
	unsigned namei_flags;
	int flags;
	int error;
	int parent_valid;

	process = current_process();
	old_held = NULL;
	new_held = NULL;
	flags = (int)args[4];
	parent_valid = 0;

	if (process == NULL ||
	    process->cred == NULL ||
	    (flags & ~AT_SYMLINK_FOLLOW) != 0)
		return -EINVAL;

	/* Resolves the target, then the parent of the new name. */
	if ((flags & AT_SYMLINK_FOLLOW) != 0)
		namei_flags = 0;
	else
		namei_flags = NAMEI_NOFOLLOW_FINAL;
	error = sys_resolve_path_at(process, (int)args[0], args[1], namei_flags,
		&target, &old_held);
	if (error != 0)
		return -error;
	error = copyinstr(args[3], new_path, sizeof(new_path), NULL);
	if (error == 0)
		error = sys_parent_path_at(process, (int)args[2], new_path,
			&temporary, &context, &new_held, &parent, &name, storage);
	if (error == 0)
		parent_valid = 1;

	/* Links under the mount's namespace transaction. */
	if (error == 0)
		mount_vfs_transaction_enter(parent.p_mount);
	if (error == 0)
		error = vfs_may_create(parent.p_inode, process->cred);
	if (error == 0)
		error = inode_link(parent.p_inode, &name, target.p_inode);
	if (error == 0)
		namecache_remove(parent.p_inode, &name);
	if (parent_valid)
		mount_vfs_transaction_leave(parent.p_mount);
	if (parent_valid)
		path_release(&parent);
	if (new_held != NULL)
		(void)file_close(new_held);
	path_release(&target);
	if (old_held != NULL)
		(void)file_close(old_held);
	if (error != 0)
		return -error;
	return 0;
}

/* Handles symlinkat(2). */
static SYSCALL_EXT intptr_t
sys_symlinkat_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct ucred *credential;
	struct cwdinfo temporary;
	struct cwdinfo *context;
	struct file *held;
	struct path parent;
	struct componentname name;
	struct inode_creation_request creation;
	struct inode *created;
	char target[PATH_MAX];
	char pathname[PATH_MAX];
	char storage[NAME_MAX + 1U];
	int error;
	int parent_valid;

	process = current_process();
	credential = NULL;
	held = NULL;
	created = NULL;
	parent_valid = 0;

	if (process == NULL || process->cred == NULL)
		return -EINVAL;
	credential = cred_process_ref(process);
	if (credential == NULL)
		return -EINVAL;

	/* Copies both strings and resolves the parent of the link. */
	error = copyinstr(args[0], target, sizeof(target), NULL);
	if (error == 0)
		error = copyinstr(args[2], pathname, sizeof(pathname), NULL);
	if (error == 0) {
		error = sys_parent_path_at(process, (int)args[1], pathname,
			&temporary, &context, &held, &parent, &name, storage);
		if (error == 0)
			parent_valid = 1;
	}

	/* Creates the link under the mount's namespace transaction. */
	if (error == 0)
		mount_vfs_transaction_enter(parent.p_mount);
	if (error == 0)
		error = inode_creation_request_user(parent.p_inode, credential,
		    INODE_SYMLINK, 0777U, 0, NULL, &creation);
	if (error == 0)
		error = inode_symlink(parent.p_inode, &name, target, &creation,
		    &created);
	if (error == 0)
		namecache_remove(parent.p_inode, &name);
	if (created != NULL)
		inode_release(created);
	if (parent_valid)
		mount_vfs_transaction_leave(parent.p_mount);
	if (parent_valid)
		path_release(&parent);
	if (held != NULL)
		(void)file_close(held);
	cred_release(credential);
	if (error != 0)
		return -error;
	return 0;
}

/* Handles readlinkat(2). */
static SYSCALL_EXT intptr_t
sys_readlinkat_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct file *held;
	struct path path;
	char buffer[PATH_MAX];
	size_t capacity;
	ssize_t count;
	int error;

	process = current_process();
	held = NULL;

	if (process == NULL || args[3] == 0)
		return -EINVAL;
	error = sys_resolve_path_at(process, (int)args[0], args[1],
		NAMEI_NOFOLLOW_FINAL, &path, &held);
	if (error != 0)
		return -error;

	/* Reads at most a buffer's worth and copies it out unterminated. */
	if (args[3] < sizeof(buffer))
		capacity = (size_t)args[3];
	else
		capacity = sizeof(buffer);
	count = inode_readlink(path.p_inode, buffer, capacity);
	if (count >= 0)
		error = copyout(buffer, args[2], (size_t)count);
	else
		error = (int)-count;
	path_release(&path);
	if (held != NULL)
		(void)file_close(held);
	if (error != 0)
		return -error;
	return count;
}

/* Handles sigaction(2). */
static intptr_t
sys_sigaction_call(
	const uintptr_t args[6])
{
	struct process *process;
	int signo;
	int error;
	int install;
	struct sigaction action;
	struct sigaction old;
	struct signal_action replacement;
	struct signal_action previous;
	struct signal_action *replacement_argument;

	process = current_process();
	signo = (int)args[0];
	install = args[1] != 0;

	if (process == NULL || signo <= 0 || signo >= NSIG)
		return -EINVAL;

	/* Validates a new action before touching the process. */
	if (install) {
		error = copyin(args[1], &action, sizeof(action));
		if (error != 0)
			return -error;
		if (signo == SIGKILL || signo == SIGSTOP)
			return -EINVAL;
		if (action.__sa_reserved != 0 ||
		    (action.sa_flags & ~(SA_RESTART | SA_NOCLDSTOP |
		    SA_NOCLDWAIT | SA_NODEFER | SA_RESETHAND |
		    SA_SIGINFO | SA_ONSTACK)) != 0 ||
		    (signo != SIGCHLD && (action.sa_flags &
		    (SA_NOCLDSTOP | SA_NOCLDWAIT)) != 0) ||
		    (action.sa_handler > 1U &&
		     !vmspace_user_range_valid((uintptr_t)action.sa_handler, 1)) ||
		    (action.sa_handler > 1U &&
		    (action.sa_restorer == 0 ||
		     !vmspace_user_range_valid((uintptr_t)action.sa_restorer, 1))))
			return -EINVAL;
	}
	if (install) {
		replacement.handler = (uintptr_t)action.sa_handler;
		replacement.mask = action.sa_mask;
		replacement.flags = action.sa_flags;
		replacement.restorer = (uintptr_t)action.sa_restorer;
	}

	/* Installs the action and reports the previous one. */
	if (install)
		replacement_argument = &replacement;
	else
		replacement_argument = NULL;
	error = signal_action_set(process, signo, replacement_argument,
	    &previous);
	if (error != 0)
		return -error;
	memset(&old, 0, sizeof(old));
	old.sa_handler = previous.handler;
	old.sa_mask = previous.mask;
	old.sa_flags = previous.flags;
	old.sa_restorer = previous.restorer;
	if (args[2] != 0) {
		error = copyout(&old, args[2], sizeof(old));
		if (error != 0)
			return -error;
	}
	return 0;
}

/* Handles sigprocmask(2). */
static intptr_t
sys_sigprocmask_call(
	const uintptr_t args[6])
{
	sigset_t set;
	sigset_t old;
	unsigned long irq;
	int operation;
	int error;

	operation = (int)args[0];

	if (curthread == NULL || curthread->proc == NULL)
		return -EINVAL;
	if (args[1] != 0) {
		error = copyin(args[1], &set, sizeof(set));
		if (error != 0)
			return -error;
		set &= SIGNAL_VALID_MASK & ~POLL_SIGNAL_BIT(SIGKILL) &
		    ~POLL_SIGNAL_BIT(SIGSTOP);
		if (operation != SIG_BLOCK && operation != SIG_UNBLOCK &&
		    operation != SIG_SETMASK)
			return -EINVAL;
	}

	/* Applies the operation under the process lock. */
	irq = spin_lock_irqsave(&curthread->proc->lock);
	old = curthread->signal_mask;
	if (args[1] != 0) {
		if (operation == SIG_BLOCK)
			curthread->signal_mask |= set;
		else if (operation == SIG_UNBLOCK)
			curthread->signal_mask &= ~set;
		else
			curthread->signal_mask = set;
	}
	spin_unlock_irqrestore(&curthread->proc->lock, irq);
	if (args[2] != 0) {
		error = copyout(&old, args[2], sizeof(old));
		if (error != 0)
			return -error;
	}
	return 0;
}

/* Handles sigpending(2). */
static intptr_t
sys_sigpending_call(
	const uintptr_t args[6])
{
	sigset_t pending;
	unsigned long irq;
	int error;

	if (curthread == NULL || curthread->proc == NULL || args[0] == 0)
		return -EINVAL;
	irq = spin_lock_irqsave(&curthread->proc->lock);
	pending = (curthread->signal_pending |
	    curthread->proc->signal_pending) & curthread->signal_mask;
	spin_unlock_irqrestore(&curthread->proc->lock, irq);
	error = copyout(&pending, args[0], sizeof(pending));
	if (error != 0)
		return -error;
	return 0;
}

/* Handles mknodat(2). */
static SYSCALL_EXT intptr_t
sys_mknodat_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct ucred *credential;
	struct cwdinfo temporary;
	struct cwdinfo *context;
	struct file *held;
	struct path parent;
	struct componentname name;
	struct inode_creation_request creation;
	struct inode *created;
	char pathname[PATH_MAX];
	char storage[NAME_MAX + 1U];
	mode_t mode;
	enum inode_type type;
	mode_t process_umask;
	int error;

	process = current_process();
	credential = NULL;
	held = NULL;
	created = NULL;
	mode = (mode_t)args[2];

	if (process == NULL || process->cwdi == NULL)
		return -EINVAL;
	credential = cred_process_ref(process);
	if (credential == NULL)
		return -EINVAL;
	process_umask = process->umask;

	/* Only FIFOs and, for the superuser, device nodes can be made. */
	if ((mode & S_IFMT) == S_IFIFO) {
		type = INODE_FIFO;
	} else if ((mode & S_IFMT) == S_IFCHR) {
		type = INODE_CHAR;
	} else if ((mode & S_IFMT) == S_IFBLK) {
		type = INODE_BLOCK;
	} else {
		error = EOPNOTSUPP;
		goto out_credential;
	}
	if (type == INODE_FIFO && args[3] != 0) {
		error = EINVAL;
		goto out_credential;
	}
	if ((type == INODE_CHAR || type == INODE_BLOCK) &&
	    !cred_is_superuser(credential)) {
		error = EPERM;
		goto out_credential;
	}

	/* Resolves the parent and creates the node under its transaction. */
	path_init(&parent);
	error = copyinstr(args[1], pathname, sizeof(pathname), NULL);
	if (error != 0)
		goto out_credential;
	if (pathname[0] == '/') {
		context = process->cwdi;
	} else {
		error = syscall_context_at(process, (int)args[0], &temporary,
		    &context, &held);
		if (error != 0)
			goto out_credential;
	}
	error = namei_parent_path_at(context, pathname, &parent, &name, storage);
	if (error == 0)
		mount_vfs_transaction_enter(parent.p_mount);
	if (error == 0)
		error = inode_creation_request_user(parent.p_inode, credential,
		    type, (mode & 07777U) & ~process_umask, (dev_t)args[3], NULL,
		    &creation);
	if (error == 0)
		error = inode_mknod(parent.p_inode, &name, &creation, &created);
	if (created != NULL)
		inode_release(created);
	if (parent.p_mount != NULL)
		mount_vfs_transaction_leave(parent.p_mount);
	path_release(&parent);
	if (held != NULL)
		(void)file_close(held);
out_credential:
	cred_release(credential);
	if (error != 0)
		return -error;
	return 0;
}

/* Handles fchdir(2). */
static intptr_t
sys_fchdir_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct file *file;
	int error;

	process = current_process();
	if (process == NULL || process->cwdi == NULL || process->fd == NULL)
		return -EINVAL;
	file = filedesc_get_ref(process->fd, (int)args[0]);
	if (file == NULL)
		return -EBADF;
	error = fs_chdir_path(process->cwdi, &file->f_path);
	(void)file_close(file);
	if (error != 0)
		return -error;
	return 0;
}

/* Handles sigaltstack(2). */
static intptr_t
sys_sigaltstack_call(
	const uintptr_t args[6])
{
	struct sigaltstack_record requested;
	struct sigaltstack_record old;
	int error;

	if (curthread == NULL)
		return -EINVAL;

	/* Snapshots the current stack, marking it in use when it is. */
	memset(&old, 0, sizeof(old));
	old.ss_sp = (uapi_ptr_t)curthread->signal_altstack_base;
	old.ss_size = curthread->signal_altstack_size;
	old.ss_flags = (int32_t)curthread->signal_altstack_flags;
	if (curthread->signal_on_altstack_depth != 0)
		old.ss_flags |= SS_ONSTACK;

	/* A new stack cannot be installed while the old one is in use. */
	if (args[0] != 0) {
		error = copyin(args[0], &requested, sizeof(requested));
		if (error != 0)
			return -error;
		if (curthread->signal_on_altstack_depth != 0)
			return -EPERM;
		if (requested.ss_flags == SS_DISABLE) {
			curthread->signal_altstack_base = 0;
			curthread->signal_altstack_size = 0;
			curthread->signal_altstack_flags = SS_DISABLE;
		} else {
			if (requested.ss_flags != 0 ||
			    requested.ss_size < MINSIGSTKSZ ||
			    user_range_check((uintptr_t)requested.ss_sp,
			    (size_t)requested.ss_size, HAL_SPACE_WRITE) != 0)
				return -EINVAL;
			curthread->signal_altstack_base = (uintptr_t)requested.ss_sp;
			curthread->signal_altstack_size = (size_t)requested.ss_size;
			curthread->signal_altstack_flags = 0;
		}
	}
	if (args[1] != 0) {
		error = copyout(&old, args[1], sizeof(old));
		if (error != 0)
			return -error;
	}
	return 0;
}

/* Handles sigtimedwait(2). */
static intptr_t
sys_sigtimedwait_call(
	const uintptr_t args[6])
{
	sigset_t set;
	struct signal_info selected;
	siginfo_t info;
	uint64_t deadline;
	int immediate;
	int signo;
	int error;

	if (curthread == NULL || args[0] == 0)
		return -EINVAL;
	error = copyin(args[0], &set, sizeof(set));
	if (error != 0)
		return -error;
	error = poll_timeout(args[2], &deadline, &immediate);
	if (error != 0)
		return -error;
	if (immediate)
		deadline = sched_ticks();

	/* Waits, then converts the kernel signal info to the user layout. */
	memset(&selected, 0, sizeof(selected));
	error = signal_timedwait(curthread, set, deadline, args[2] != 0,
	    &selected, &signo);
	if (error != 0)
		return -error;
	memset(&info, 0, sizeof(info));
	info.si_signo = signo;
	info.si_errno = selected.error;
	info.si_code = selected.code;
	info.si_pid = selected.pid;
	info.si_uid = selected.uid;
	info.si_status = selected.status;
	info.si_addr = selected.address;
	memcpy(&info.si_value, &selected.value, sizeof(selected.value));
	if (args[1] != 0) {
		error = copyout(&info, args[1], sizeof(info));
		if (error != 0)
			return -error;
	}
	return signo;
}

/* Handles sigqueue(2). */
static intptr_t
sys_sigqueue_call(
	const uintptr_t args[6])
{
	struct process *sender;
	struct process *target;
	struct signal_info info;
	pid_t pid;
	int signo;
	int error;

	sender = current_process();
	pid = (pid_t)args[0];
	signo = (int)args[1];

	/* A null signal only checks permission. */
	if (sender == NULL || pid <= 0 || signo < 0 || signo >= NSIG)
		return -EINVAL;
	error = signal_kill(sender, pid, 0);
	if (error != 0)
		return -error;
	if (signo == 0)
		return 0;

	/* Queues the signal with the sender's identity and value. */
	target = process_find_ref(pid);
	if (target == NULL)
		return -ESRCH;
	memset(&info, 0, sizeof(info));
	info.code = SI_QUEUE;
	info.pid = sender->pid;
	if (sender->cred != NULL)
		info.uid = sender->cred->euid;
	else
		info.uid = 0;
	info.value = (uint64_t)args[2];
	error = signal_send_process_info(target, signo, &info);
	process_release(target);
	if (error != 0)
		return -error;
	return 0;
}

/* Handles thread_create(2). */
static intptr_t
sys_thread_create_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct thread *thread;
	struct uaccess_pin pin;
	tid_t tid;
	int error;

	process = current_process();
	if (process == NULL ||
	    process->vmspace == NULL ||
	    args[0] == 0 ||
	    args[1] == 0 ||
	    args[4] != 0 ||
	    args[5] == 0)
		return -EINVAL;

	/* The new thread starts only once its id has been reported. */
	memset(&pin, 0, sizeof(pin));
	error = uaccess_pin(args[5], sizeof(tid), HAL_SPACE_WRITE, &pin);
	if (error != 0)
		return -error;
	error = thread_create(process, args[0], args[1], &thread);
	if (error == 0) {
		hal_task_set_tls(thread->task, args[3]);
		tid = thread->tid;
		error = copyout_pinned(&pin, 0, &tid, sizeof(tid));
		if (error == 0)
			thread_start(thread);
		else
			(void)thread_abort_new(thread);
	}
	uaccess_unpin(&pin);
	if (error != 0)
		return -error;
	return 0;
}

/* Handles thread_exit(2). */
static intptr_t
sys_thread_exit_call(
	const uintptr_t args[6])
{
	if (curthread == NULL || curthread->proc == &process0)
		return -EINVAL;
	curthread->user_exit_value = args[0];
	thread_exit(0);
}

/* Claims a thread for one joiner; the caller holds the process lock. */
static int
thread_join_claim_locked(
	struct thread *target,
	tid_t owner,
	unsigned stop_redispatch)
{
	if (target->detached)
		return EINVAL;
	if (!target->join_claimed) {
		target->join_claimed = 1;
		target->join_owner_tid = owner;
		return 0;
	}

	/* A redispatch after a stop keeps the claim it already owns. */
	if (stop_redispatch && target->join_owner_tid == owner)
		return 0;
	return EINVAL;
}

/* Releases a joiner's claim on a thread; the caller holds the process lock. */
static void
thread_join_release_locked(
	struct thread *target,
	tid_t owner)
{
	if (target->join_claimed && target->join_owner_tid == owner) {
		target->join_claimed = 0;
		target->join_owner_tid = 0;
	}
}

/* Handles thread_join(2). */
static intptr_t
sys_thread_join_call(
	const uintptr_t args[6])
{
	struct thread *target;
	struct process *process;
	struct uaccess_pin pin;
	uintptr_t value;
	unsigned long irq;
	int error;
	int pin_active;
	int retain_claim;
	unsigned cancelable;
	unsigned wait_flags;
	tid_t owner;
	uint64_t sequence;

	process = current_process();
	pin_active = 0;
	retain_claim = 0;

	if (process == NULL || (tid_t)args[0] == curthread->tid)
		return -EDEADLK;
	if ((args[2] & ~ZEDBSD_THREAD_JOIN_CANCELABLE) != 0 ||
	    args[3] != 0 || args[4] != 0 || args[5] != 0)
		return -EINVAL;
	cancelable = (args[2] & ZEDBSD_THREAD_JOIN_CANCELABLE) != 0;
	owner = curthread->tid;
	memset(&pin, 0, sizeof(pin));

	/* Claims the target for this joiner. */
	target = thread_find_ref((tid_t)args[0]);
	if (target == NULL || target->proc != process) {
		if (target != NULL)
			thread_release(target);
		return -ESRCH;
	}
	irq = spin_lock_irqsave(&process->lock);
	error = thread_join_claim_locked(target, owner,
	    curthread->syscall_stop_redispatch);
	spin_unlock_irqrestore(&process->lock, irq);
	if (error != 0)
		goto out;

	/*
	 * Pin before waiting or observing a consumable zombie.  A failed
	 * copyout must leave the target joinable, not reap it and report
	 * EFAULT afterward.
	 */
	if (args[1] != 0) {
		error = uaccess_pin(args[1], sizeof(value), HAL_SPACE_WRITE, &pin);
		if (error != 0)
			goto release;
		pin_active = 1;
	}

	/* Waits for the target to become a zombie. */
	irq = spin_lock_irqsave(&process->lock);
	if (cancelable &&
	    atomic_raw_load_acquire(&curthread->cancel_pending) != 0)
		error = EINTR;
	wait_flags = WAITQ_INTERRUPTIBLE;
	if (cancelable)
		wait_flags |= WAITQ_CANCELABLE;
	while (error == 0 && target->state != THREAD_ZOMBIE) {
		sequence = waitq_sequence(&target->join_waitq);
		error = waitq_sleep(&target->join_waitq, &process->lock,
		    sequence, 0, wait_flags);
		if (error == EAGAIN)
			error = 0;
		if (error == 0 && cancelable &&
		    atomic_raw_load_acquire(&curthread->cancel_pending) != 0)
			error = EINTR;
	}
	if (error == 0)
		value = target->user_exit_value;
	spin_unlock_irqrestore(&process->lock, irq);

	/*
	 * STOP is transparent to userspace.  Keep ownership across that one
	 * redispatch only; ordinary signal EINTR relinquishes it for
	 * another joiner.
	 */
	retain_claim = error == EINTR && curthread->stop_interrupted;
	if (error == 0 && pin_active)
		error = copyout_pinned(&pin, 0, &value, sizeof(value));
	if (error == 0)
		error = thread_wait(target, NULL);
release:
	if (error != 0 && !retain_claim) {
		irq = spin_lock_irqsave(&process->lock);
		thread_join_release_locked(target, owner);
		spin_unlock_irqrestore(&process->lock, irq);
	}
out:
	if (pin_active)
		uaccess_unpin(&pin);
	thread_release(target);
	if (error != 0)
		return -error;
	return 0;
}

/* Handles thread_detach(2). */
static intptr_t
sys_thread_detach_call(
	const uintptr_t args[6])
{
	struct thread *target;
	struct process *process;
	unsigned long irq;
	int error;
	int reap;

	target = thread_find_ref((tid_t)args[0]);
	process = current_process();
	error = 0;
	reap = 0;

	if (target == NULL || process == NULL || target->proc != process) {
		if (target != NULL)
			thread_release(target);
		return -ESRCH;
	}

	/* A detached zombie is reaped right away. */
	irq = spin_lock_irqsave(&process->lock);
	if (target->detached || target->join_claimed) {
		error = EINVAL;
	} else {
		target->detached = 1;
		reap = target->state == THREAD_ZOMBIE;
	}
	spin_unlock_irqrestore(&process->lock, irq);
	if (error == 0 && reap)
		error = thread_wait(target, NULL);
	thread_release(target);
	if (error != 0)
		return -error;
	return 0;
}

/* Handles thread_self(2). */
static intptr_t
sys_thread_self_call(
	const uintptr_t args[6])
{
	if (curthread == NULL ||
	    args[2] != 0 ||
	    args[3] != 0 ||
	    args[4] != 0 ||
	    args[5] != 0)
		return -EINVAL;
	if (args[0] == ZEDBSD_THREAD_SELF_TID && args[1] == 0)
		return curthread->tid;
	if (args[0] == ZEDBSD_THREAD_SELF_GET_TLS && args[1] == 0)
		return (intptr_t)hal_task_get_tls(curthread->task);
	if (args[0] == ZEDBSD_THREAD_SELF_SET_TLS) {
		hal_task_set_tls(curthread->task, args[1]);
		return 0;
	}
	return -EINVAL;
}

/* Handles thread_kill(2). */
static intptr_t
sys_thread_kill_call(
	const uintptr_t args[6])
{
	struct thread *target;
	struct process *process;
	int error;

	target = thread_find_ref((tid_t)args[0]);
	process = current_process();

	if (target == NULL || process == NULL || target->proc != process) {
		if (target != NULL)
			thread_release(target);
		return -ESRCH;
	}
	if (args[1] == 0)
		error = 0;
	else
		error = signal_send_thread(target, (int)args[1]);
	thread_release(target);
	if (error != 0)
		return -error;
	return 0;
}

/* Handles thread_cancel(2). */
static intptr_t
sys_thread_cancel_call(
	const uintptr_t args[6])
{
	struct thread *current;
	struct thread *target;
	unsigned operation;
	unsigned long irq;
	int pending;

	current = curthread;
	operation = (unsigned)args[1];

	if (current == NULL ||
	    current->proc == NULL ||
	    args[2] != 0 ||
	    args[3] != 0 ||
	    args[4] != 0 ||
	    args[5] != 0 ||
	    operation > ZEDBSD_THREAD_CANCEL_CLEAR)
		return -EINVAL;

	/* A request marks another thread of the process and interrupts it. */
	if (operation == ZEDBSD_THREAD_CANCEL_REQUEST) {
		target = thread_find_ref((tid_t)args[0]);
		if (target == NULL || target->proc != current->proc) {
			if (target != NULL)
				thread_release(target);
			return -ESRCH;
		}
		irq = spin_lock_irqsave(&target->proc->lock);
		atomic_raw_store_release(&target->cancel_pending, 1U);
		spin_unlock_irqrestore(&target->proc->lock, irq);

		/*
		 * This retained interrupt closes the RUNNING-to-wait-
		 * registration gap.  Non-cancelable waits treat it as a
		 * spurious wake because they do not inspect cancel_pending.
		 */
		sched_interrupt(target);
		thread_release(target);
		return 0;
	}

	/* A test or clear only applies to the calling thread. */
	if (args[0] != 0 && (tid_t)args[0] != current->tid)
		return -EINVAL;
	irq = spin_lock_irqsave(&current->proc->lock);
	pending = atomic_raw_load_acquire(&current->cancel_pending) != 0;
	if (operation == ZEDBSD_THREAD_CANCEL_CLEAR)
		atomic_raw_store_release(&current->cancel_pending, 0U);
	spin_unlock_irqrestore(&current->proc->lock, irq);
	return pending;
}

/* Handles usync(2), the user synchronization wait and wake primitive. */
static intptr_t
sys_usync_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct vm_object *shared_object;
	struct timespec timeout;
	uintptr_t key_object;
	uintptr_t key_offset;
	uint64_t ticks;
	uint64_t deadline;
	unsigned flags;
	int error;
	struct timespec now;
	struct kern_timespec absolute_time;
	struct kern_timespec current_time;
	struct kern_timespec duration;
	clockid_t clock;
	struct timespec relative;

	process = current_process();
	shared_object = NULL;
	deadline = 0;
	flags = (unsigned)args[5];

	if (process == NULL ||
	    (flags & ~(ZEDBSD_USYNC_PRIVATE | ZEDBSD_USYNC_CANCELABLE |
	    ZEDBSD_USYNC_ABSTIME | ZEDBSD_USYNC_CLOCK_REALTIME)) != 0 ||
	    ((flags & ZEDBSD_USYNC_CLOCK_REALTIME) != 0 &&
	    (flags & ZEDBSD_USYNC_ABSTIME) == 0))
		return -EINVAL;

	/* A private object is keyed by process; a shared one by its VM object. */
	if ((flags & ZEDBSD_USYNC_PRIVATE) != 0) {
		key_object = (uintptr_t)process;
		key_offset = args[0];
	} else {
		error = vmspace_shared_mapping_key(process->vmspace, args[0],
		    sizeof(uint32_t), &shared_object, &key_offset);
		if (error != 0)
			return -EINVAL;
		key_object = (uintptr_t)shared_object;
	}

	/* A wait converts its absolute or relative timeout to a deadline. */
	if ((unsigned)args[1] == ZEDBSD_USYNC_WAIT) {
		if (args[4] != 0) {
			error = EINVAL;
			goto out;
		}
		if (args[3] != 0) {
			if ((flags & ZEDBSD_USYNC_ABSTIME) != 0) {
				error = copyin(args[3], &timeout, sizeof(timeout));
				if (error == 0)
					error = kern_timespec_validate(&timeout);
				if ((flags & ZEDBSD_USYNC_CLOCK_REALTIME) != 0)
					clock = CLOCK_REALTIME;
				else
					clock = CLOCK_MONOTONIC;
				if (error == 0)
					error = kern_clock_gettime(clock, &now);
				if (error == 0) {
					absolute_time.tv_sec = timeout.tv_sec;
					absolute_time.tv_nsec = (int32_t)timeout.tv_nsec;
					current_time.tv_sec = now.tv_sec;
					current_time.tv_nsec = (int32_t)now.tv_nsec;
					if (kern_timespec_compare(&absolute_time,
					    &current_time) <= 0)
						error = ETIMEDOUT;
				}
				if (error == 0)
					error = kern_timespec_sub(&absolute_time,
					    &current_time, &duration);
				if (error == 0) {
					relative.tv_sec = (time_t)duration.tv_sec;
					relative.tv_nsec = (long)duration.tv_nsec;
					error = kern_duration_to_ticks_ceil(&relative,
					    &ticks);
				}
				if (error == 0)
					error = kern_deadline_after(clock_ticks(), ticks,
					    &deadline);
			} else if (curthread != NULL &&
			    curthread->syscall_stop_redispatch &&
			    curthread->syscall_wait_deadline_valid) {
				deadline = curthread->syscall_wait_deadline;
				error = 0;
			} else {
				error = copyin(args[3], &timeout, sizeof(timeout));
				if (error == 0)
					error = kern_duration_to_ticks_ceil(&timeout,
					    &ticks);
				if (error == 0)
					error = syscall_restart_deadline_after(ticks,
					    &deadline);
			}
			if (error != 0)
				goto out;
		}
		error = usync_wait(args[0], (uint32_t)args[2],
		    key_object, key_offset, deadline,
		    (flags & ZEDBSD_USYNC_CANCELABLE) != 0);
		goto out;
	}

	/* A wake takes only a count. */
	if ((unsigned)args[1] == ZEDBSD_USYNC_WAKE) {
		if (args[2] != 0 ||
		    args[3] != 0 ||
		    (flags & (ZEDBSD_USYNC_CANCELABLE | ZEDBSD_USYNC_ABSTIME |
		    ZEDBSD_USYNC_CLOCK_REALTIME)) != 0)
			error = EINVAL;
		else
			error = usync_wake(args[0], key_object, key_offset,
			    (unsigned)args[4]);
		goto out;
	}
	error = EINVAL;
out:
	if (shared_object != NULL)
		vm_object_put(shared_object);
	if (error != 0)
		return -error;
	return 0;
}

/* Handles sigsuspend(2). */
static intptr_t
sys_sigsuspend_call(
	const uintptr_t args[6])
{
	struct process *process;
	sigset_t mask;
	unsigned long irq;
	int error;

	if (curthread == NULL)
		return -EINVAL;
	process = curthread->proc;
	if (process == NULL || args[0] == 0)
		return -EINVAL;
	error = copyin(args[0], &mask, sizeof(mask));
	if (error != 0)
		return -error;
	mask &= SIGNAL_VALID_MASK & ~POLL_SIGNAL_BIT(SIGKILL) &
	    ~POLL_SIGNAL_BIT(SIGSTOP);

	/* Installs the temporary mask and sleeps until a signal is selected. */
	irq = spin_lock_irqsave(&process->lock);
	curthread->signal_suspend_mask = curthread->signal_mask;
	curthread->signal_mask = mask;
	curthread->signal_suspended = 1;
	for (;;) {
		if (curthread->terminate_requested) {
			spin_unlock_irqrestore(&process->lock, irq);
			return -EINTR;
		}
		if (process->stop_requested) {
			spin_unlock_irqrestore(&process->lock, irq);
			process_stop_current(0);
			irq = spin_lock_irqsave(&process->lock);
			continue;
		}
		if (signal_pending_unblocked_locked(curthread)) {
			spin_unlock_irqrestore(&process->lock, irq);
			if (signal_stop_before_return(curthread) ==
			    SIGNAL_STOP_RETURN_REDISPATCH) {
				irq = spin_lock_irqsave(&process->lock);
				continue;
			}
			break;
		}
		sched_sleep_locked(0, &process->lock);
	}

	/*
	 * The user-return hook restores the original mask before entering
	 * the selected handler.  Its sigreturn therefore resumes this call
	 * at EINTR.
	 */
	return -EINTR;
}

/* Handles sigreturn(2). */
static intptr_t
sys_sigreturn_call(
	const uintptr_t args[6])
{
	intptr_t restored;
	struct thread_signal_level *level;
	ucontext_t context;
	sigset_t restored_mask;
	uint32_t restart_number;
	uintptr_t restart_args[HAL_SYSCALL_ARGS];
	unsigned long irq;
	int restart;
	int error;

	/* The token and context must match the innermost signal level. */
	if (curthread == NULL ||
	    args[0] == 0 ||
	    args[1] == 0 ||
	    curthread->signal_depth == 0 ||
	    (uint32_t)args[0] != curthread->signal_token)
		return -EINVAL;
	level = &curthread->signal_levels[curthread->signal_depth - 1U];
	if (level->token != (uint32_t)args[0] ||
	    level->user_ucontext != args[1])
		return -EINVAL;
	error = copyin(args[1], &context, sizeof(context));
	if (error != 0)
		return -error;

	/*
	 * The first signal ABI exposes machine state for diagnosis but does
	 * not yet permit userland to replace it.  Only the signal mask is
	 * mutable.
	 */
	restored_mask = context.uc_sigmask & SIGNAL_VALID_MASK &
	    ~POLL_SIGNAL_BIT(SIGKILL) & ~POLL_SIGNAL_BIT(SIGSTOP);
	context.uc_sigmask = level->saved_ucontext.uc_sigmask;
	if (memcmp(&context, &level->saved_ucontext, sizeof(context)) != 0)
		return -EINVAL;

	/* Restores the machine state and pops the signal level. */
	restart = level->restart_on_return != 0;
	restart_number = level->restart_number;
	memcpy(restart_args, level->restart_args, sizeof(restart_args));
	if (hal_task_signal_return((uint32_t)args[0], &restored) != 0)
		return -EINVAL;
	if (level->used_altstack && curthread->signal_on_altstack_depth != 0)
		curthread->signal_on_altstack_depth--;
	irq = spin_lock_irqsave(&curthread->proc->lock);
	curthread->signal_mask = restored_mask;
	spin_unlock_irqrestore(&curthread->proc->lock, irq);
	memset(level, 0, sizeof(*level));
	curthread->signal_depth--;
	if (curthread->signal_depth == 0)
		curthread->signal_token = 0;
	else
		curthread->signal_token =
		    curthread->signal_levels[curthread->signal_depth - 1U].token;

	/* Arranges the redispatch of an interrupted restartable call. */
	curthread->syscall_restart_valid = 0;
	if (restart) {
		curthread->syscall_restart_number = restart_number;
		memcpy(curthread->syscall_restart_args, restart_args,
		    sizeof(curthread->syscall_restart_args));
		curthread->syscall_redispatch_valid = 1;
	}
	return restored;
}

/* Handles dup(2). */
static intptr_t
sys_dup_call(
	const uintptr_t args[6])
{
	struct process *process;
	int result;
	int error;

	process = current_process();
	if (process == NULL || process->fd == NULL)
		return -EBADF;
	error = filedesc_dup(process->fd, (int)args[0], 0, 0, &result);
	if (error != 0)
		return -error;
	return result;
}

/* Handles dup2(2) and dup3(2). */
static intptr_t
sys_dup2_call(
	const uintptr_t args[6],
	int is_dup3)
{
	struct process *process;
	unsigned flags;
	int error;

	process = current_process();
	flags = 0;

	if (process == NULL || process->fd == NULL)
		return -EBADF;
	if (is_dup3) {
		if (((int)args[2] & ~(O_CLOEXEC | O_CLOFORK)) != 0)
			return -EINVAL;
		if (((int)args[2] & O_CLOEXEC) != 0)
			flags |= FILEDESC_CLOEXEC;
		if (((int)args[2] & O_CLOFORK) != 0)
			flags |= FILEDESC_CLOFORK;
	}
	error = filedesc_dup2(process->fd, (int)args[0], (int)args[1],
	    flags, is_dup3);
	if (error != 0)
		return -error;
	return (intptr_t)(int)args[1];
}

/* Handles fcntl(2). */
static intptr_t
sys_fcntl_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct process *target;
	struct file *file;
	struct flock_record request;
	unsigned flags;
	unsigned dup_flags;
	unsigned descriptor_flags;
	int owner;
	int result;
	int error;
	int command;

	process = current_process();
	command = (int)args[1];

	if (process == NULL || process->fd == NULL)
		return -EBADF;
	switch (command) {
	case F_DUPFD:
	case F_DUPFD_CLOEXEC:
	case F_DUPFD_CLOFORK:
		if (command == F_DUPFD_CLOEXEC)
			dup_flags = FILEDESC_CLOEXEC;
		else if (command == F_DUPFD_CLOFORK)
			dup_flags = FILEDESC_CLOFORK;
		else
			dup_flags = 0;
		error = filedesc_dup(process->fd, (int)args[0], (int)args[2],
		    dup_flags, &result);
		if (error != 0)
			return -error;
		return result;
	case F_GETFD:
		error = filedesc_get_flags(process->fd, (int)args[0], &flags);
		if (error != 0)
			return -error;
		result = 0;
		if ((flags & FILEDESC_CLOEXEC) != 0)
			result |= FD_CLOEXEC;
		if ((flags & FILEDESC_CLOFORK) != 0)
			result |= FD_CLOFORK;
		return result;
	case F_SETFD:
		if (((int)args[2] & ~(FD_CLOEXEC | FD_CLOFORK)) != 0)
			return -EINVAL;
		descriptor_flags = 0;
		if (((int)args[2] & FD_CLOEXEC) != 0)
			descriptor_flags |= FILEDESC_CLOEXEC;
		if (((int)args[2] & FD_CLOFORK) != 0)
			descriptor_flags |= FILEDESC_CLOFORK;
		error = filedesc_set_flags(process->fd, (int)args[0],
		    descriptor_flags);
		if (error != 0)
			return -error;
		return 0;
	case F_GETFL:
		file = filedesc_get_ref(process->fd, (int)args[0]);
		if (file == NULL)
			return -EBADF;
		result = file_status_flags_get(file);
		(void)file_close(file);
		return result;
	case F_SETFL:
		file = filedesc_get_ref(process->fd, (int)args[0]);
		if (file == NULL)
			return -EBADF;

		/*
		 * F_GETFL returns the access mode and immutable open flags as
		 * well as the flags F_SETFL may change.  Applications
		 * conventionally pass that value back after toggling
		 * O_NONBLOCK or O_APPEND; ignore the immutable bits rather
		 * than rejecting the standard idiom.
		 */
		file_status_flags_update(file, O_APPEND | O_NONBLOCK,
		    (int)args[2]);
		(void)file_close(file);
		return 0;
	case F_GETOWN:
		file = filedesc_get_ref(process->fd, (int)args[0]);
		if (file == NULL)
			return -EBADF;
		result = atomic_int_load_acquire(&file->f_signal_owner);
		(void)file_close(file);
		error = copyout(&result, args[2], sizeof(result));
		if (error != 0)
			return -error;
		return 0;
	case F_SETOWN:
		/* The owner must be a process or group in the caller's session. */
		owner = (int)args[2];
		if (owner == INT_MIN)
			return -EINVAL;
		if (owner > 0) {
			target = process_find_ref((pid_t)owner);
			if (target == NULL)
				return -ESRCH;
			if (target->session != process->session)
				error = EPERM;
			else
				error = 0;
			process_release(target);
			if (error != 0)
				return -error;
		} else if (owner < 0 && !process_pgrp_in_session(process->session,
		    (pid_t)-owner)) {
			return -ESRCH;
		}
		file = filedesc_get_ref(process->fd, (int)args[0]);
		if (file == NULL)
			return -EBADF;
		atomic_int_store_release(&file->f_signal_owner, owner);
		(void)file_close(file);
		return 0;
	case F_GETLK:
	case F_SETLK:
	case F_SETLKW:
	case F_OFD_GETLK:
	case F_OFD_SETLK:
	case F_OFD_SETLKW:
		error = copyin(args[2], &request, sizeof(request));
		if (error != 0)
			return -error;
		file = filedesc_get_ref(process->fd, (int)args[0]);
		if (file == NULL)
			return -EBADF;
		error = record_lock_fcntl(process, file, command, &request);
		(void)file_close(file);
		if (error == 0 &&
		    (command == F_GETLK || command == F_OFD_GETLK))
			error = copyout(&request, args[2], sizeof(request));
		if (error != 0)
			return -error;
		return 0;
	default:
		return -EINVAL;
	}
}

/* Handles pipe(2) and pipe2(2). */
static intptr_t
sys_pipe2_call(
	const uintptr_t args[6],
	int plain)
{
	struct process *process;
	struct file *read_file;
	struct file *write_file;
	int descriptors[2];
	int flags;
	unsigned fdflags;
	int error;

	process = current_process();
	descriptors[0] = -1;
	descriptors[1] = -1;
	if (plain)
		flags = 0;
	else
		flags = (int)args[1];

	if (process == NULL || process->fd == NULL)
		return -EBADF;
	error = pipe_create(flags, &read_file, &write_file);
	if (error != 0)
		return -error;

	/* Installs both ends and reports them. */
	fdflags = 0;
	if ((flags & O_CLOEXEC) != 0)
		fdflags |= FILEDESC_CLOEXEC;
	if ((flags & O_CLOFORK) != 0)
		fdflags |= FILEDESC_CLOFORK;
	error = filedesc_install_pair(process->fd, read_file, fdflags,
	    write_file, fdflags, descriptors);
	if (error == 0)
		error = copyout(descriptors, args[0], sizeof(descriptors));
	if (error != 0) {
		if (descriptors[0] >= 0) {
			(void)filedesc_close(process->fd, descriptors[0]);
			(void)filedesc_close(process->fd, descriptors[1]);
		} else {
			(void)file_close(read_file);
			(void)file_close(write_file);
		}
		return -error;
	}
	return 0;
}

/* Handles fork(2). */
static intptr_t
sys_fork_call(
	const uintptr_t args[6])
{
	struct process *parent;
	struct process *child;
	int error;

	parent = current_process();
	(void)args;
	error = process_fork(parent, &child);
	if (error != 0)
		return -error;
	return child->pid;
}

/* Handles sched_yield(2). */
static intptr_t
sys_sched_yield_call(
	const uintptr_t args[6])
{
	if (args[0] != 0 ||
	    args[1] != 0 ||
	    args[2] != 0 ||
	    args[3] != 0 ||
	    args[4] != 0 ||
	    args[5] != 0)
		return -EINVAL;
	sched_yield();
	return 0;
}

/* Handles times(2). */
static intptr_t
sys_times_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct process_times_record result;
	size_t record_size;
	uint64_t user_ticks;
	uint64_t system_ticks;
	uint64_t child_user_ticks;
	uint64_t child_system_ticks;
	int error;

	process = current_process();

	/* The record size selects the ABI version. */
	if (process == NULL ||
	    args[0] == 0 ||
	    args[2] != 0 ||
	    args[3] != 0 ||
	    args[4] != 0 ||
	    args[5] != 0)
		return -EINVAL;
	if (args[1] == 0)
		record_size = ZEDBSD_PROCESS_TIMES_V1_SIZE;
	else
		record_size = (size_t)args[1];
	if (record_size != ZEDBSD_PROCESS_TIMES_V1_SIZE &&
	    record_size != sizeof(result))
		return -EINVAL;

	/* Samples the accounting counters. */
	user_ticks = atomic_u64_load_acquire(&process->user_ticks);
	system_ticks = atomic_u64_load_acquire(&process->system_ticks);
	child_user_ticks = atomic_u64_load_acquire(&process->child_user_ticks);
	child_system_ticks = atomic_u64_load_acquire(
	    &process->child_system_ticks);
	result.self_ticks = user_ticks + system_ticks;
	result.child_ticks = child_user_ticks + child_system_ticks;
	result.elapsed_ticks = sched_ticks();
	result.system_ticks = system_ticks;
	result.child_system_ticks = child_system_ticks;
	error = copyout(&result, args[0], record_size);
	if (error != 0)
		return -error;
	return 0;
}

/* Tests whether a process is selected by a priority which/who pair. */
static int
priority_matches(
	struct process *target,
	struct process *caller,
	int which,
	id_t who)
{
	struct ucred *target_cred;
	uid_t target_euid;

	target_euid = (uid_t)-1;

	/* A zero who names the caller's own process, group, or user. */
	if (who == 0) {
		if (which == PRIO_PROCESS)
			who = (id_t)caller->pid;
		else if (which == PRIO_PGRP)
			who = (id_t)caller->pgrp;
		else if (which == PRIO_USER)
			who = (id_t)caller->cred->euid;
	}
	if (which == PRIO_PROCESS) {
		if (target->pid == (pid_t)who)
			return 1;
		return 0;
	}
	if (which == PRIO_PGRP) {
		if (target->pgrp == (pid_t)who)
			return 1;
		return 0;
	}
	if (which == PRIO_USER) {
		target_cred = cred_process_ref(target);
		if (target_cred != NULL) {
			target_euid = target_cred->euid;
			cred_release(target_cred);
		}
		if (target_euid == (uid_t)who)
			return 1;
		return 0;
	}
	return 0;
}

/* Handles getpriority(2). */
static intptr_t
sys_getpriority_call(
	const uintptr_t args[6])
{
	struct process *caller;
	struct process *target;
	pid_t cursor;
	int which;
	int found;
	int best;
	int error;
	int value;

	caller = current_process();
	cursor = -1;
	which = (int)args[0];
	found = 0;
	best = 20;

	if (caller == NULL ||
	    args[2] == 0 ||
	    args[3] != 0 ||
	    args[4] != 0 ||
	    args[5] != 0 ||
	    (which != PRIO_PROCESS && which != PRIO_PGRP && which != PRIO_USER))
		return -EINVAL;

	/* Reports the lowest nice value among the selected processes. */
	for (;;) {
		target = process_find_next_ref(cursor);
		if (target == NULL)
			break;
		cursor = target->pid;
		if (priority_matches(target, caller, which, (id_t)args[1])) {
			value = atomic_int_load_relaxed(&target->nice_value);
			if (!found || value < best)
				best = value;
			found = 1;
		}
		process_release(target);
	}
	if (!found)
		return -ESRCH;
	error = copyout(&best, args[2], sizeof(best));
	if (error != 0)
		return -error;
	return 0;
}

/* Handles setpriority(2). */
static intptr_t
sys_setpriority_call(
	const uintptr_t args[6])
{
	struct process *caller;
	struct process *target;
	pid_t cursor;
	int which;
	int value;
	int found;
	int denied;
	struct ucred *target_cred;
	int old;

	caller = current_process();
	cursor = -1;
	which = (int)args[0];
	value = (int)args[2];
	found = 0;
	denied = 0;

	if (caller == NULL ||
	    args[3] != 0 ||
	    args[4] != 0 ||
	    args[5] != 0 ||
	    (which != PRIO_PROCESS && which != PRIO_PGRP && which != PRIO_USER))
		return -EINVAL;
	if (value < -20)
		value = -20;
	if (value > 20)
		value = 20;

	/* Applies the value to every selected process the caller may change. */
	for (;;) {
		target = process_find_next_ref(cursor);
		if (target == NULL)
			break;
		cursor = target->pid;
		if (priority_matches(target, caller, which, (id_t)args[1])) {
			old = atomic_int_load_relaxed(&target->nice_value);
			found = 1;
			target_cred = cred_process_ref(target);
			if (target_cred == NULL ||
			    (caller->cred->euid != 0 &&
			    caller->cred->euid != target_cred->euid)) {
				cred_release(target_cred);
				process_release(target);
				denied = 1;
				continue;
			}
			cred_release(target_cred);
			if (value < old && !cred_is_superuser(caller->cred)) {
				denied = 1;
				process_release(target);
				continue;
			}
			atomic_int_store_relaxed(&target->nice_value, value);
		}
		process_release(target);
	}
	if (!found)
		return -ESRCH;
	if (denied)
		return -EPERM;
	return 0;
}

/* Converts scheduler ticks to a timeval. */
static void
ticks_to_timeval(
	uint64_t ticks,
	struct timeval *value)
{
	value->tv_sec = (time_t)(ticks / KERN_CLOCK_HZ);
	value->tv_usec = (long)((ticks % KERN_CLOCK_HZ) *
	    (1000000U / KERN_CLOCK_HZ));
}

/* Handles getrusage(2). */
static intptr_t
sys_getrusage_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct rusage usage;
	uint64_t user_ticks;
	uint64_t system_ticks;
	int error;

	process = current_process();
	if (process == NULL ||
	    args[1] == 0 ||
	    args[2] != 0 ||
	    args[3] != 0 ||
	    args[4] != 0 ||
	    args[5] != 0)
		return -EINVAL;

	/* Reports the caller's own or its children's CPU time. */
	memset(&usage, 0, sizeof(usage));
	if ((int)args[0] == RUSAGE_SELF) {
		user_ticks = atomic_u64_load_acquire(&process->user_ticks);
		system_ticks = atomic_u64_load_acquire(&process->system_ticks);
	} else if ((int)args[0] == RUSAGE_CHILDREN) {
		user_ticks = atomic_u64_load_acquire(&process->child_user_ticks);
		system_ticks = atomic_u64_load_acquire(
		    &process->child_system_ticks);
	} else {
		return -EINVAL;
	}
	ticks_to_timeval(user_ticks, &usage.ru_utime);
	ticks_to_timeval(system_ticks, &usage.ru_stime);
	error = copyout(&usage, args[1], sizeof(usage));
	if (error != 0)
		return -error;
	return 0;
}

/* Converts a timeval to scheduler ticks, rounding up. */
static int
timeval_ticks(
	const struct timeval *value,
	uint64_t *ticks)
{
	uint64_t whole;
	uint64_t fraction;

	if (value->tv_sec < 0 || value->tv_usec < 0 || value->tv_usec >= 1000000)
		return EINVAL;
	if ((uint64_t)value->tv_sec > UINT64_MAX / KERN_CLOCK_HZ)
		return EOVERFLOW;
	whole = (uint64_t)value->tv_sec * KERN_CLOCK_HZ;
	fraction = ((uint64_t)value->tv_usec * KERN_CLOCK_HZ + 999999U) / 1000000U;
	if (whole > UINT64_MAX - fraction)
		return EOVERFLOW;
	*ticks = whole + fraction;
	return 0;
}

/* Reads an interval timer into the user layout. */
static void
timer_snapshot(
	struct process *process,
	int which,
	struct itimerval *value)
{
	uint64_t remaining;
	uint64_t interval;

	(void)process_itimer_get(process, which, &remaining, &interval);
	memset(value, 0, sizeof(*value));
	ticks_to_timeval(remaining, &value->it_value);
	ticks_to_timeval(interval, &value->it_interval);
}

/* Handles getitimer(2). */
static intptr_t
sys_getitimer_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct itimerval value;
	int which;
	int error;

	process = current_process();
	which = (int)args[0];

	if (process == NULL ||
	    which < 0 ||
	    which > 2 ||
	    args[1] == 0 ||
	    args[2] != 0 ||
	    args[3] != 0 ||
	    args[4] != 0 ||
	    args[5] != 0)
		return -EINVAL;
	timer_snapshot(process, which, &value);
	error = copyout(&value, args[1], sizeof(value));
	if (error != 0)
		return -error;
	return 0;
}

/* Handles setitimer(2). */
static intptr_t
sys_setitimer_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct itimerval value;
	struct itimerval old;
	uint64_t remaining;
	uint64_t interval;
	int which;
	int error;
	uint64_t old_remaining;
	uint64_t old_interval;

	process = current_process();
	which = (int)args[0];

	if (process == NULL ||
	    which < 0 ||
	    which > 2 ||
	    args[1] == 0 ||
	    args[3] != 0 ||
	    args[4] != 0 ||
	    args[5] != 0)
		return -EINVAL;

	/* Converts both intervals to ticks. */
	error = copyin(args[1], &value, sizeof(value));
	if (error != 0)
		return -error;
	error = timeval_ticks(&value.it_value, &remaining);
	if (error != 0)
		return -error;
	error = timeval_ticks(&value.it_interval, &interval);
	if (error != 0)
		return -error;

	/* Arms the timer and reports the previous setting. */
	(void)process_itimer_set(process, which, remaining, interval,
	    &old_remaining, &old_interval);
	memset(&old, 0, sizeof(old));
	ticks_to_timeval(old_remaining, &old.it_value);
	ticks_to_timeval(old_interval, &old.it_interval);
	if (args[2] != 0) {
		error = copyout(&old, args[2], sizeof(old));
		if (error != 0)
			return -error;
	}
	return 0;
}

/* Handles execve(2). */
static intptr_t
sys_execve_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct syscall_exec_args *copy;
	char path[PATH_MAX];
	int error;

	process = current_process();
	if (process == NULL || args[3] != 0 || args[4] != 0 || args[5] != 0)
		return -EINVAL;
	error = copyinstr(args[0], path, sizeof(path), NULL);
	if (error != 0)
		return -error;

	/* Copies both vectors into one heap block before the image loads. */
	copy = kern_calloc(1, sizeof(*copy));
	if (copy == NULL)
		return -ENOMEM;
	error = copy_exec_vector(args[1], copy->argv, ZEDBSD_SPAWN_ARG_MAX,
	    copy, 0);
	if (error == 0)
		error = copy_exec_vector(args[2], copy->envp,
		    ZEDBSD_SPAWN_ENV_MAX, copy, 1);
	if (error == 0)
		error = process_execve(process, path, copy->argv, copy->envp);
	kern_free(copy);
	if (error != 0)
		return -error;
	return 0;
}

/* Handles fexecve(2). */
static intptr_t
sys_fexecve_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct syscall_exec_args *copy;
	struct file *file;
	int error;

	process = current_process();
	if (process == NULL ||
	    process->fd == NULL ||
	    args[3] != 0 ||
	    args[4] != 0 ||
	    args[5] != 0)
		return -EINVAL;
	file = filedesc_get_ref(process->fd, (int)args[0]);
	if (file == NULL)
		return -EBADF;

	/* Copies both vectors into one heap block before the image loads. */
	copy = kern_calloc(1, sizeof(*copy));
	if (copy == NULL) {
		(void)file_close(file);
		return -ENOMEM;
	}
	error = copy_exec_vector(args[1], copy->argv, ZEDBSD_SPAWN_ARG_MAX,
	    copy, 0);
	if (error == 0)
		error = copy_exec_vector(args[2], copy->envp,
		    ZEDBSD_SPAWN_ENV_MAX, copy, 1);
	if (error == 0)
		error = process_fexecve(process, file, copy->argv, copy->envp);
	kern_free(copy);
	(void)file_close(file);
	if (error != 0)
		return -error;
	return 0;
}

/* Handles waitpid(2). */
static SYSCALL_EXT intptr_t
sys_waitpid_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct process_wait_event event;
	struct uaccess_pin pin;
	int status;
	pid_t result;
	int error;

	process = current_process();
	status = 0;
	error = 0;

	if (args[3] != 0 ||
	    args[4] != 0 ||
	    args[5] != 0 ||
	    ((int)args[2] & ~(WNOHANG | WUNTRACED | WCONTINUED)) != 0)
		return -EINVAL;

	/* Pins the status output before the child can be consumed. */
	if (args[1] != 0) {
		error = uaccess_pin(args[1], sizeof(status), HAL_SPACE_WRITE, &pin);
		if (error != 0)
			return -error;
	} else {
		memset(&pin, 0, sizeof(pin));
	}
	result = process_wait_select(process, (pid_t)args[0], (int)args[2],
	    &event);
	if (result <= 0 || args[1] == 0) {
		if (result > 0) {
			error = process_wait_commit(&event);
			if (error != 0) {
				process_wait_abort(&event);
				result = -error;
			}
		}
		uaccess_unpin(&pin);
		return result;
	}

	/* Reports the status, then consumes the event. */
	status = event.status;
	error = copyout_pinned(&pin, 0, &status, sizeof(status));
	if (error == 0)
		error = process_wait_commit(&event);
	if (error != 0)
		process_wait_abort(&event);
	uaccess_unpin(&pin);
	if (error != 0)
		return -error;
	return result;
}

/* Handles waitid(2). */
static SYSCALL_EXT intptr_t
sys_waitid_call(
	const uintptr_t args[6])
{
	struct process *process;
	struct process_wait_event event;
	siginfo_t information;
	idtype_t type;
	id_t id;
	int options;
	pid_t selector;
	pid_t result;
	unsigned mask;
	int error;

	process = current_process();
	type = (idtype_t)args[0];
	id = (id_t)args[1];
	options = (int)args[3];
	mask = 0;

	/* Converts the options and the id type into a wait selection. */
	if (process == NULL ||
	    args[2] == 0 ||
	    args[4] != 0 ||
	    args[5] != 0 ||
	    (options & ~(WEXITED | WSTOPPED | WCONTINUED | WNOHANG |
	    WNOWAIT)) != 0)
		return -EINVAL;
	if ((options & WEXITED) != 0)
		mask |= PROCESS_WAIT_EVENT_EXITED;
	if ((options & WSTOPPED) != 0)
		mask |= PROCESS_WAIT_EVENT_STOPPED;
	if ((options & WCONTINUED) != 0)
		mask |= PROCESS_WAIT_EVENT_CONTINUED;
	if (mask == 0)
		return -EINVAL;
	if (type == P_ALL) {
		selector = -1;
	} else if (type == P_PID && id > 0 && id <= INT32_MAX) {
		selector = (pid_t)id;
	} else if (type == P_PGID && id <= INT32_MAX) {
		if (id == 0)
			selector = 0;
		else
			selector = -(pid_t)id;
	} else {
		return -EINVAL;
	}
	result = process_wait_select_mask(process, selector,
	    options & WNOHANG, mask, &event);
	if (result < 0)
		return result;

	/* Fills the signal information for the selected event. */
	memset(&information, 0, sizeof(information));
	if (result == 0) {
		error = copyout(&information, args[2], sizeof(information));
		if (error != 0)
			return -EFAULT;
		return 0;
	}
	information.si_signo = SIGCHLD;
	information.si_pid = event.pid;
	information.si_uid = event.uid;
	if (event.kind == PROCESS_WAIT_STOPPED) {
		information.si_code = CLD_STOPPED;
		information.si_status = WSTOPSIG(event.status);
	} else if (event.kind == PROCESS_WAIT_CONTINUED) {
		information.si_code = CLD_CONTINUED;
		information.si_status = SIGCONT;
	} else if (WIFEXITED(event.status)) {
		information.si_code = CLD_EXITED;
		information.si_status = WEXITSTATUS(event.status);
	} else {
		information.si_code = CLD_KILLED;
		information.si_status = WTERMSIG(event.status);
	}

	/* WNOWAIT leaves the event for a later wait. */
	error = copyout(&information, args[2], sizeof(information));
	if (error != 0 || (options & WNOWAIT) != 0) {
		process_wait_abort(&event);
	} else {
		error = process_wait_commit(&event);
		if (error != 0)
			process_wait_abort(&event);
	}
	if (error != 0)
		return -error;
	return 0;
}

/* Handles getrlimit(2) and setrlimit(2). */
static SYSCALL_EXT intptr_t
sys_resource_limit_call(
	const uintptr_t args[6],
	int setting)
{
	struct process *process;
	struct rlimit_record limit;
	int error;

	process = current_process();
	if (process == NULL ||
	    args[2] != 0 ||
	    args[3] != 0 ||
	    args[4] != 0 ||
	    args[5] != 0)
		return -EINVAL;
	if (setting) {
		error = copyin(args[1], &limit, sizeof(limit));
		if (error == 0)
			error = resource_limit_set(process, (int)args[0], &limit);
	} else {
		error = resource_limit_get(process, (int)args[0], &limit);
		if (error == 0)
			error = copyout(&limit, args[1], sizeof(limit));
	}
	if (error != 0)
		return -error;
	return 0;
}

/* Handles the process and session identity system calls. */
static intptr_t
sys_process_identity_call(
	uint32_t number,
	const uintptr_t args[6])
{
	struct process *process;
	struct process *target;
	pid_t pid;
	pid_t value;
	intptr_t result;
	int error;

	process = current_process();
	if (process == NULL)
		return -EINVAL;
	switch (number) {
	case ZEDBSD_SYS_getpid:
		return process->pid;
	case ZEDBSD_SYS_getppid:
		result = process_parent_pid(process);
		return result;
	case ZEDBSD_SYS_getpgrp:
		return process->pgrp;
	case ZEDBSD_SYS_getpgid:
		pid = (pid_t)args[0];
		if (pid == 0)
			return process->pgrp;
		target = process_find_ref(pid);
		if (target != NULL) {
			value = target->pgrp;
			process_release(target);
			return value;
		}
		return -ESRCH;
	case ZEDBSD_SYS_setpgid:
		error = process_setpgid(process, (pid_t)args[0],
		    (pid_t)args[1]);
		if (error != 0)
			return -error;
		return 0;
	case ZEDBSD_SYS_setsid:
		result = process_setsid(process);
		return result;
	case ZEDBSD_SYS_getsid:
		pid = (pid_t)args[0];
		if (pid == 0)
			return process->session;
		target = process_find_ref(pid);
		if (target != NULL) {
			value = target->session;
			process_release(target);
			return value;
		}
		return -ESRCH;
	default:
		return -ENOSYS;
	}
}

/* Runs the handler of one system call number. */
static intptr_t
syscall_dispatch_body(
	uint32_t number,
	const uintptr_t args[6])
{
	intptr_t result;
	int error;

	switch (number) {
	case ZEDBSD_SYS_exit:
		exit1((int)args[0]);
	case ZEDBSD_SYS_open:
		result = sys_open_call(args, 0);
		break;
	case ZEDBSD_SYS_openat:
		result = sys_open_call(args, 1);
		break;
	case ZEDBSD_SYS_close:
		result = sys_close_call(args);
		break;
	case ZEDBSD_SYS_read:
		result = sys_read_call(args);
		break;
	case ZEDBSD_SYS_write:
		result = sys_write_call(args);
		break;
	case ZEDBSD_SYS_lseek:
		result = sys_lseek_call(args);
		break;
	case ZEDBSD_SYS_fstat:
		result = sys_fstat_call(args);
		break;
	case ZEDBSD_SYS_getdents:
		result = sys_getdents_call(args);
		break;
	case ZEDBSD_SYS_chdir:
		result = sys_chdir_call(args);
		break;
	case ZEDBSD_SYS_fchdir:
		result = sys_fchdir_call(args);
		break;
	case ZEDBSD_SYS_mknodat:
		result = sys_mknodat_call(args);
		break;
	case ZEDBSD_SYS_getcwd:
		result = sys_getcwd_call(args);
		break;
	case ZEDBSD_SYS_mmap:
		result = sys_mmap_call(args);
		break;
	case ZEDBSD_SYS_munmap:
		result = sys_munmap_call(args);
		break;
	case ZEDBSD_SYS_mprotect:
		result = sys_mprotect_call(args);
		break;
	case ZEDBSD_SYS_ioctl:
		result = sys_ioctl_call(args);
		break;
	case ZEDBSD_SYS_sysctl:
		result = sys_sysctl_call(args);
		break;
	case ZEDBSD_SYS_ppoll:
		result = sys_ppoll_call(args);
		break;
	case ZEDBSD_SYS_pselect:
		result = sys_pselect_call(args);
		break;
	case ZEDBSD_SYS_sigaltstack:
		result = sys_sigaltstack_call(args);
		break;
	case ZEDBSD_SYS_sigtimedwait:
		result = sys_sigtimedwait_call(args);
		break;
	case ZEDBSD_SYS_sigqueue:
		result = sys_sigqueue_call(args);
		break;
	case ZEDBSD_SYS_thread_create:
		result = sys_thread_create_call(args);
		break;
	case ZEDBSD_SYS_thread_exit:
		result = sys_thread_exit_call(args);
		break;
	case ZEDBSD_SYS_thread_join:
		result = sys_thread_join_call(args);
		break;
	case ZEDBSD_SYS_thread_detach:
		result = sys_thread_detach_call(args);
		break;
	case ZEDBSD_SYS_thread_self:
		result = sys_thread_self_call(args);
		break;
	case ZEDBSD_SYS_thread_kill:
		result = sys_thread_kill_call(args);
		break;
	case ZEDBSD_SYS_thread_cancel:
		result = sys_thread_cancel_call(args);
		break;
	case ZEDBSD_SYS_usync:
		result = sys_usync_call(args);
		break;
	case ZEDBSD_SYS_clock_gettime:
		result = sys_clock_gettime_call(args);
		break;
	case ZEDBSD_SYS_clock_getres:
		result = sys_clock_getres_call(args);
		break;
	case ZEDBSD_SYS_clock_settime:
		result = sys_clock_settime_call(args);
		break;
	case ZEDBSD_SYS_timer_create:
		result = sys_timer_create_call(args);
		break;
	case ZEDBSD_SYS_timer_delete:
		result = sys_timer_delete_call(args);
		break;
	case ZEDBSD_SYS_timer_settime:
		result = sys_timer_settime_call(args);
		break;
	case ZEDBSD_SYS_timer_gettime:
		result = sys_timer_gettime_call(args);
		break;
	case ZEDBSD_SYS_timer_getoverrun:
		result = sys_timer_getoverrun_call(args);
		break;
	case ZEDBSD_SYS_mount:
		result = sys_mount_call(args);
		break;
	case ZEDBSD_SYS_unmount:
		result = sys_unmount_call(args);
		break;
	case ZEDBSD_SYS_statvfs:
		result = sys_statvfs_call(args, 0);
		break;
	case ZEDBSD_SYS_fstatvfs:
		result = sys_statvfs_call(args, 1);
		break;
	case ZEDBSD_SYS_getxattr:
		result = sys_getxattr_call(args, 0, 0);
		break;
	case ZEDBSD_SYS_lgetxattr:
		result = sys_getxattr_call(args, 0, 1);
		break;
	case ZEDBSD_SYS_fgetxattr:
		result = sys_getxattr_call(args, 1, 0);
		break;
	case ZEDBSD_SYS_setxattr:
		result = sys_setxattr_call(args, 0, 0);
		break;
	case ZEDBSD_SYS_lsetxattr:
		result = sys_setxattr_call(args, 0, 1);
		break;
	case ZEDBSD_SYS_fsetxattr:
		result = sys_setxattr_call(args, 1, 0);
		break;
	case ZEDBSD_SYS_listxattr:
		result = sys_listxattr_call(args, 0, 0);
		break;
	case ZEDBSD_SYS_llistxattr:
		result = sys_listxattr_call(args, 0, 1);
		break;
	case ZEDBSD_SYS_flistxattr:
		result = sys_listxattr_call(args, 1, 0);
		break;
	case ZEDBSD_SYS_removexattr:
		result = sys_removexattr_call(args, 0, 0);
		break;
	case ZEDBSD_SYS_lremovexattr:
		result = sys_removexattr_call(args, 0, 1);
		break;
	case ZEDBSD_SYS_fremovexattr:
		result = sys_removexattr_call(args, 1, 0);
		break;
	case ZEDBSD_SYS_quotactl:
		result = sys_quotactl_call(args);
		break;
	case ZEDBSD_SYS_snapshotctl:
		result = sys_snapshotctl_call(args);
		break;
	case ZEDBSD_SYS_nanosleep:
		result = sys_nanosleep_call(args);
		break;
	case ZEDBSD_SYS_brk:
		result = sys_brk_call(args);
		break;
	case ZEDBSD_SYS_socket:
		result = sys_socket_call(args);
		break;
	case ZEDBSD_SYS_socketpair:
		result = sys_socketpair_call(args);
		break;
	case ZEDBSD_SYS_sendmsg:
		result = sys_sendmsg_call(args);
		break;
	case ZEDBSD_SYS_recvmsg:
		result = sys_recvmsg_call(args);
		break;
	case ZEDBSD_SYS_bind:
		result = sys_bind_call(args);
		break;
	case ZEDBSD_SYS_connect:
		result = sys_connect_call(args);
		break;
	case ZEDBSD_SYS_listen:
		result = sys_listen_call(args);
		break;
	case ZEDBSD_SYS_accept:
		result = sys_accept_call(args);
		break;
	case ZEDBSD_SYS_sendto:
		result = sys_sendto_call(args);
		break;
	case ZEDBSD_SYS_recvfrom:
		result = sys_recvfrom_call(args);
		break;
	case ZEDBSD_SYS_shutdown:
		result = sys_shutdown_call(args);
		break;
	case ZEDBSD_SYS_getsockname:
		result = sys_socket_name_call(args, 0);
		break;
	case ZEDBSD_SYS_getpeername:
		result = sys_socket_name_call(args, 1);
		break;
	case ZEDBSD_SYS_setsockopt:
		result = sys_setsockopt_call(args);
		break;
	case ZEDBSD_SYS_getsockopt:
		result = sys_getsockopt_call(args);
		break;
	case ZEDBSD_SYS_fork:
		result = sys_fork_call(args);
		break;
	case ZEDBSD_SYS_sched_yield:
		result = sys_sched_yield_call(args);
		break;
	case ZEDBSD_SYS_times:
		result = sys_times_call(args);
		break;
	case ZEDBSD_SYS_sync:
		result = -(long)mount_sync_all();
		break;
	case ZEDBSD_SYS_getpriority:
		result = sys_getpriority_call(args);
		break;
	case ZEDBSD_SYS_setpriority:
		result = sys_setpriority_call(args);
		break;
	case ZEDBSD_SYS_getrusage:
		result = sys_getrusage_call(args);
		break;
	case ZEDBSD_SYS_getitimer:
		result = sys_getitimer_call(args);
		break;
	case ZEDBSD_SYS_setitimer:
		result = sys_setitimer_call(args);
		break;
	case ZEDBSD_SYS_execve:
		result = sys_execve_call(args);
		break;
	case ZEDBSD_SYS_fexecve:
		result = sys_fexecve_call(args);
		break;
	case ZEDBSD_SYS_waitpid:
		result = sys_waitpid_call(args);
		break;
	case ZEDBSD_SYS_waitid:
		result = sys_waitid_call(args);
		break;
	case ZEDBSD_SYS_getrlimit:
		result = sys_resource_limit_call(args, 0);
		break;
	case ZEDBSD_SYS_setrlimit:
		result = sys_resource_limit_call(args, 1);
		break;
	case ZEDBSD_SYS_getpid:
	case ZEDBSD_SYS_getppid:
	case ZEDBSD_SYS_getpgrp:
	case ZEDBSD_SYS_getpgid:
	case ZEDBSD_SYS_setpgid:
	case ZEDBSD_SYS_setsid:
	case ZEDBSD_SYS_getsid:
		result = sys_process_identity_call(number, args);
		break;
	case ZEDBSD_SYS_dup:
		result = sys_dup_call(args);
		break;
	case ZEDBSD_SYS_dup2:
		result = sys_dup2_call(args, 0);
		break;
	case ZEDBSD_SYS_dup3:
		result = sys_dup2_call(args, 1);
		break;
	case ZEDBSD_SYS_fcntl:
		result = sys_fcntl_call(args);
		break;
	case ZEDBSD_SYS_pipe:
		result = sys_pipe2_call(args, 1);
		break;
	case ZEDBSD_SYS_pipe2:
		result = sys_pipe2_call(args, 0);
		break;
	case ZEDBSD_SYS_pread:
		result = sys_positional_call(args, 0);
		break;
	case ZEDBSD_SYS_pwrite:
		result = sys_positional_call(args, 1);
		break;
	case ZEDBSD_SYS_readv:
		result = sys_vector_call(args, 0);
		break;
	case ZEDBSD_SYS_writev:
		result = sys_vector_call(args, 1);
		break;
	case ZEDBSD_SYS_fsync:
	case ZEDBSD_SYS_fdatasync:
		result = sys_fsync_call(args);
		break;
	case ZEDBSD_SYS_stat:
		result = sys_stat_path_call(args, 0, 0);
		break;
	case ZEDBSD_SYS_lstat:
		result = sys_stat_path_call(args, 0, 1);
		break;
	case ZEDBSD_SYS_fstatat:
		result = sys_stat_path_call(args, 1, 0);
		break;
	case ZEDBSD_SYS_truncate:
		result = sys_truncate_call(args, 0);
		break;
	case ZEDBSD_SYS_ftruncate:
		result = sys_truncate_call(args, 1);
		break;
	case ZEDBSD_SYS_mkdir:
	case ZEDBSD_SYS_unlink:
	case ZEDBSD_SYS_rmdir:
	case ZEDBSD_SYS_rename:
		result = sys_mutation_call(number, args);
		break;
	case ZEDBSD_SYS_mkdirat:
	case ZEDBSD_SYS_unlinkat:
	case ZEDBSD_SYS_renameat:
		result = sys_mutation_at_call(number, args);
		break;
	case ZEDBSD_SYS_umask:
		result = sys_umask_call(args);
		break;
	case ZEDBSD_SYS_getuid:
	case ZEDBSD_SYS_geteuid:
	case ZEDBSD_SYS_getgid:
	case ZEDBSD_SYS_getegid:
	case ZEDBSD_SYS_getgroups:
		result = sys_cred_get_call(number, args);
		break;
	case ZEDBSD_SYS_getresuid:
	case ZEDBSD_SYS_getresgid:
		result = sys_cred_getres_call(number, args);
		break;
	case ZEDBSD_SYS_getentropy:
		result = sys_getentropy_call(args);
		break;
	case ZEDBSD_SYS_atomic:
		result = sys_atomic_call(args);
		break;
	case ZEDBSD_SYS_setuid:
	case ZEDBSD_SYS_seteuid:
	case ZEDBSD_SYS_setgid:
	case ZEDBSD_SYS_setegid:
	case ZEDBSD_SYS_setgroups:
	case ZEDBSD_SYS_setreuid:
	case ZEDBSD_SYS_setregid:
	case ZEDBSD_SYS_setresuid:
	case ZEDBSD_SYS_setresgid:
		result = sys_cred_set_call(number, args);
		break;
	case ZEDBSD_SYS_access:
		result = sys_access_call(args);
		break;
	case ZEDBSD_SYS_sigaction:
		result = sys_sigaction_call(args);
		break;
	case ZEDBSD_SYS_sigprocmask:
		result = sys_sigprocmask_call(args);
		break;
	case ZEDBSD_SYS_sigpending:
		result = sys_sigpending_call(args);
		break;
	case ZEDBSD_SYS_kill:
		error = signal_kill(current_process(), (pid_t)args[0],
		    (int)args[1]);
		if (error != 0)
			result = -error;
		else
			result = 0;
		break;
	case ZEDBSD_SYS_sigreturn:
		result = sys_sigreturn_call(args);
		break;
	case ZEDBSD_SYS_msync:
		result = sys_msync_call(args);
		break;
	case ZEDBSD_SYS_chmod:
		result = sys_chmod_common(AT_FDCWD, args[0], -1,
			(mode_t)args[1], 0);
		break;
	case ZEDBSD_SYS_fchmod:
		result = sys_chmod_common(AT_FDCWD, 0, (int)args[0],
			(mode_t)args[1], 0);
		break;
	case ZEDBSD_SYS_fchmodat:
		result = sys_chmod_common((int)args[0], args[1], -1,
			(mode_t)args[2], (int)args[3]);
		break;
	case ZEDBSD_SYS_chown:
		result = sys_chown_common(AT_FDCWD, args[0], -1,
			(uid_t)args[1], (gid_t)args[2], 0);
		break;
	case ZEDBSD_SYS_lchown:
		result = sys_chown_common(AT_FDCWD, args[0], -1,
			(uid_t)args[1], (gid_t)args[2], AT_SYMLINK_NOFOLLOW);
		break;
	case ZEDBSD_SYS_fchown:
		result = sys_chown_common(AT_FDCWD, 0, (int)args[0],
			(uid_t)args[1], (gid_t)args[2], 0);
		break;
	case ZEDBSD_SYS_fchownat:
		result = sys_chown_common((int)args[0], args[1], -1,
			(uid_t)args[2], (gid_t)args[3], (int)args[4]);
		break;
	case ZEDBSD_SYS_utimensat:
		result = sys_utimens_common((int)args[0], args[1], -1,
			args[2], (int)args[3]);
		break;
	case ZEDBSD_SYS_futimens:
		result = sys_utimens_common(AT_FDCWD, 0, (int)args[0], args[1], 0);
		break;
	case ZEDBSD_SYS_faccessat:
		result = sys_faccessat_call(args);
		break;
	case ZEDBSD_SYS_linkat:
		result = sys_linkat_call(args);
		break;
	case ZEDBSD_SYS_symlinkat:
		result = sys_symlinkat_call(args);
		break;
	case ZEDBSD_SYS_readlinkat:
		result = sys_readlinkat_call(args);
		break;
	case ZEDBSD_SYS_sigsuspend:
		result = sys_sigsuspend_call(args);
		break;
	default:
		result = -ENOSYS;
		break;
	}
	return result;
}

/* Tests whether a system call is restarted after a handler with SA_RESTART. */
static int
syscall_restartable(
	uint32_t number)
{
	switch (number) {
	case ZEDBSD_SYS_read:
	case ZEDBSD_SYS_write:
	case ZEDBSD_SYS_pread:
	case ZEDBSD_SYS_pwrite:
	case ZEDBSD_SYS_readv:
	case ZEDBSD_SYS_writev:
	case ZEDBSD_SYS_open:
	case ZEDBSD_SYS_openat:
	case ZEDBSD_SYS_fcntl: /* F_SETLKW is an interruptible slow operation. */
	case ZEDBSD_SYS_ioctl:
	case ZEDBSD_SYS_fsync:
	case ZEDBSD_SYS_fdatasync:
	case ZEDBSD_SYS_waitpid:
	case ZEDBSD_SYS_waitid:
	case ZEDBSD_SYS_accept:
	case ZEDBSD_SYS_sendto:
	case ZEDBSD_SYS_recvfrom:
	case ZEDBSD_SYS_sendmsg:
	case ZEDBSD_SYS_recvmsg:
	case ZEDBSD_SYS_thread_join:
	case ZEDBSD_SYS_socketpair:
		return 1;
	default:
		return 0;
	}
}

/* Runs one system call with accounting, redispatch, and restart policy. */
static intptr_t
syscall_dispatch(
	uint32_t number,
	const uintptr_t args[6])
{
	struct thread *thread;
	struct process *process;
	uintptr_t dispatch_args[HAL_SYSCALL_ARGS];
	uint32_t dispatch_number;
	int cred_guard;
	intptr_t result;
	enum signal_stop_return_result stop_result;

	thread = curthread;
	if (thread != NULL)
		process = thread->proc;
	else
		process = NULL;
	dispatch_number = number;
	cred_guard = number != ZEDBSD_SYS_exit &&
	    number != ZEDBSD_SYS_thread_exit;

	/*
	 * HAL calls the registered dispatcher with the active user frame
	 * installed and local IRQs masked.  Accounting and interruptibility
	 * are generic syscall policy; HAL only owns the masked frame-commit
	 * boundaries.
	 */
	if (hal_irq_disable())
		HAL_FATAL("syscall callback entered with IRQs enabled");
	sched_accounting_kernel_enter();
	hal_irq_enable();
	memcpy(dispatch_args, args, sizeof(dispatch_args));
	syscall_restart_state_begin(thread);
	if (cred_guard)
		process_cred_read_enter(process);

	/* Runs the handler, redispatching after a sigreturn or a transparent stop. */
	for (;;) {
		if (thread != NULL && dispatch_number != ZEDBSD_SYS_sigreturn) {
			thread->syscall_restart_number = dispatch_number;
			memcpy(thread->syscall_restart_args, dispatch_args,
			    sizeof(thread->syscall_restart_args));
			thread->syscall_restart_valid = 0;
			thread->syscall_redispatch_valid = 0;
			thread->stop_interrupted = 0;
		}
		result = syscall_dispatch_body(dispatch_number, dispatch_args);
		if (thread != NULL)
			thread->syscall_stop_redispatch = 0;
		if (thread != NULL && dispatch_number != ZEDBSD_SYS_sigreturn)
			thread->syscall_restart_valid = result == -EINTR &&
			    syscall_restartable(dispatch_number);
		if (thread != NULL && thread->terminate_requested) {
			if (cred_guard)
				process_cred_read_leave(process);
			thread_exit(0);
		}
		if (thread != NULL && dispatch_number == ZEDBSD_SYS_sigreturn &&
		    thread->syscall_redispatch_valid) {
			dispatch_number = thread->syscall_restart_number;
			memcpy(dispatch_args, thread->syscall_restart_args,
			    sizeof(dispatch_args));
			thread->syscall_redispatch_valid = 0;
			syscall_restart_state_begin(thread);
			continue;
		}
		if (thread != NULL && thread->stop_interrupted) {
			thread->stop_interrupted = 0;
			if (process_stop_requested(thread))
				process_stop_current(0);
			if (result == -EINTR) {
				syscall_restart_prepare_stop(thread);
				continue;
			}
		} else if (thread != NULL && result == -EINTR) {
			stop_result =
			    signal_stop_before_return(thread);
			if (stop_result == SIGNAL_STOP_RETURN_INTERRUPT)
				break;
			if (stop_result == SIGNAL_STOP_RETURN_REDISPATCH) {
				syscall_restart_prepare_stop(thread);
				continue;
			}
		}
		break;
	}
	syscall_restart_state_finish(thread);
	if (cred_guard)
		process_cred_read_leave(process);
	if (!hal_irq_disable())
		HAL_FATAL("syscall callback returned with IRQs disabled");
	sched_accounting_kernel_leave();
	return result;
}
