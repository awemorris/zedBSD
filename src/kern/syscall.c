/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/syscall.h"
#include "kern/file.h"
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
#include <sys/mount.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SYSCALL_IO_CHUNK 512U
#define SYSCALL_SOCKET_OPTION_MAX 128U
#define SYSCALL_PAGE_MASK (ZEDBSD_PAGE_SIZE - 1U)
#define SYSCALL_EXT __attribute__((section(".hightext")))

static intptr_t syscall_dispatch(uint32_t, const uintptr_t [6]);
static intptr_t syscall_dispatch_body(uint32_t, const uintptr_t [6]);
static struct process *current_process(void);

#define POLL_SIGNAL_BIT(n) (1U << ((unsigned)(n) - 1U))

struct poll_mask_guard {
	struct thread *thread;
	sigset_t saved;
	unsigned active;
};

static int
poll_mask_enter(uintptr_t address, struct poll_mask_guard *guard)
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
	process = guard->thread != NULL ? guard->thread->proc : NULL;
	if (process == NULL)
		return EINVAL;
	requested &= ~(POLL_SIGNAL_BIT(SIGKILL) | POLL_SIGNAL_BIT(SIGSTOP));
	irq = spin_lock_irqsave(&process->lock);
	guard->saved = guard->thread->signal_mask;
	guard->thread->signal_mask = requested;
	guard->active = 1;
	spin_unlock_irqrestore(&process->lock, irq);
	return 0;
}

static void
poll_mask_leave(struct poll_mask_guard *guard)
{
	struct process *process;
	unsigned long irq;
	if (guard == NULL || !guard->active || guard->thread == NULL ||
	    (process = guard->thread->proc) == NULL)
		return;
	irq = spin_lock_irqsave(&process->lock);
	guard->thread->signal_mask = guard->saved;
	guard->active = 0;
	spin_unlock_irqrestore(&process->lock, irq);
}

static int
poll_timeout(uintptr_t address, uint64_t *deadline, int *immediate)
{
	struct timespec timeout;
	uint64_t ticks;
	int error;

	*deadline = 0;
	*immediate = 0;
	if (address == 0)
		return 0;
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
	return kern_deadline_after(sched_ticks(), ticks, deadline);
}

static intptr_t
sys_ppoll_call(const uintptr_t args[6])
{
	struct pollfd fds[KERN_OPEN_MAX];
	struct uaccess_pin pin;
	struct poll_mask_guard guard;
	struct process *process = current_process();
	nfds_t count = (nfds_t)args[1];
	uint64_t deadline;
	int immediate, ready = 0, error;
	size_t bytes;

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
	error = poll_timeout(args[2], &deadline, &immediate);
	if (error == 0)
		error = poll_mask_enter(args[3], &guard);
	if (error == 0)
		error = kern_poll_wait(process, fds, count, deadline, immediate,
		    &ready);
	if (args[3] != 0)
		poll_mask_leave(&guard);
	if (error == 0 && bytes != 0)
		error = copyout_pinned(&pin, 0, fds, bytes);
	uaccess_unpin(&pin);
	return error != 0 ? -error : ready;
}

static int
pselect_pin(uintptr_t address, struct uaccess_pin *pin,
	zedbsd_fd_set_t *value)
{
	int error;
	memset(pin, 0, sizeof(*pin));
	memset(value, 0, sizeof(*value));
	if (address == 0)
		return 0;
	error = uaccess_pin(address, sizeof(*value),
	    HAL_SPACE_READ | HAL_SPACE_WRITE, pin);
	return error != 0 ? error : copyin_pinned(pin, 0, value,
	    sizeof(*value));
}

static intptr_t
sys_pselect_call(const uintptr_t args[6])
{
	struct pollfd fds[KERN_OPEN_MAX];
	struct uaccess_pin read_pin, write_pin, except_pin;
	zedbsd_fd_set_t input_read, input_write, input_except;
	zedbsd_fd_set_t output_read, output_write, output_except;
	struct poll_mask_guard guard;
	struct process *process = current_process();
	uint64_t deadline;
	uint32_t valid_mask;
	int nfds = (int)args[0], immediate, ready = 0, error, result = 0;
	nfds_t count = 0, i;

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
	valid_mask = nfds == 32 ? UINT32_MAX :
	    (nfds == 0 ? 0U : ((uint32_t)1U << (unsigned)nfds) - 1U);
	input_read.bits[0] &= valid_mask;
	input_write.bits[0] &= valid_mask;
	input_except.bits[0] &= valid_mask;
	for (i = 0; i < (nfds_t)nfds; i++) {
		uint32_t bit = (uint32_t)1U << i;
		short events = 0;
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
	error = poll_timeout(args[4], &deadline, &immediate);
	if (error == 0)
		error = poll_mask_enter(args[5], &guard);
	if (error == 0)
		error = kern_poll_wait(process, fds, count, deadline, immediate,
		    &ready);
	if (args[5] != 0)
		poll_mask_leave(&guard);
	if (error != 0)
		goto out;
	(void)ready;
	memset(&output_read, 0, sizeof(output_read));
	memset(&output_write, 0, sizeof(output_write));
	memset(&output_except, 0, sizeof(output_except));
	for (i = 0; i < count; i++) {
		uint32_t bit = (uint32_t)1U << (unsigned)fds[i].fd;
		short revents = fds[i].revents;
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
	return error != 0 ? -error : result;
}

#define SYSCALL_SYSCTL_VALUE_MAX 256U

static intptr_t
sys_sysctl_call(const uintptr_t args[6])
{
	int name[CTL_MAXNAME];
	uint8_t old_value[SYSCALL_SYSCTL_VALUE_MAX];
	uint8_t new_value[SYSCALL_SYSCTL_VALUE_MAX];
	size_t old_length = 0;
	unsigned namelen = (unsigned)args[1];
	struct process *process = current_process();
	int error;
	if (args[0] == 0 || namelen == 0 || namelen > CTL_MAXNAME ||
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
		if (args[2] != 0 && old_length > sizeof(old_value))
			old_length = sizeof(old_value);
	}
	if (args[4] != 0) {
		error = copyin(args[4], new_value, (size_t)args[5]);
		if (error != 0)
			return -error;
	} else if (args[5] != 0) {
		return -EINVAL;
	}
	error = kern_sysctl(name, namelen, args[2] != 0 ? old_value : NULL,
	    args[3] != 0 ? &old_length : NULL,
	    args[4] != 0 ? new_value : NULL, (size_t)args[5],
	    process != NULL && cred_is_superuser(process->cred));
	if (args[3] != 0) {
		int copy_error = copyout(&old_length, args[3], sizeof(old_length));
		if (copy_error != 0)
			return -copy_error;
	}
	if (error == 0 && args[2] != 0 && old_length != 0) {
		error = copyout(old_value, args[2], old_length);
		if (error != 0)
			return -error;
	}
	return error == 0 ? 0 : -error;
}

static struct process *current_process(void)
{
	return curthread != NULL ? curthread->proc : NULL;
}

static int
descriptor_socket(struct process *process, int descriptor,
	struct socket_file_ref *reference)
{
	return process == NULL || process->fd == NULL ? EBADF :
	    socket_file_ref_get(process->fd, descriptor, reference);
}

static intptr_t
socket_result(struct socket_file_ref *reference, intptr_t result)
{
	socket_file_ref_put(reference);
	return result;
}

static int
copy_sockaddr_in(uintptr_t address, socklen_t length,
		 struct sockaddr_storage *storage)
{
	if (address == 0 || storage == NULL || length < sizeof(sa_family_t) ||
	    length > sizeof(*storage))
		return EINVAL;
	memset(storage, 0, sizeof(*storage));
	return copyin(address, storage, length);
}

static int
copy_sockaddr_out(uintptr_t address, uintptr_t length_address,
		  const struct sockaddr_storage *storage, socklen_t actual)
{
	socklen_t capacity;
	int error;

	if (address == 0 && length_address == 0)
		return 0;
	if (address == 0 || length_address == 0 || storage == NULL)
		return EINVAL;
	error = copyin(length_address, &capacity, sizeof(capacity));
	if (error != 0)
		return error;
	if (capacity != 0) {
		socklen_t copied = capacity < actual ? capacity : actual;
		error = copyout(storage, address, copied);
		if (error != 0)
			return error;
	}
	return copyout(&actual, length_address, sizeof(actual));
}

struct sockaddr_output_pin {
	struct uaccess_pin address;
	struct uaccess_pin length;
	socklen_t capacity;
};

static void
sockaddr_output_unpin(struct sockaddr_output_pin *pin)
{
	if (pin == NULL)
		return;
	uaccess_unpin(&pin->address);
	uaccess_unpin(&pin->length);
}

static int
sockaddr_output_pin(uintptr_t address, uintptr_t length_address,
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
	error = uaccess_pin(length_address, sizeof(pin->capacity), PROT_WRITE,
	    &pin->length);
	if (error != 0)
		return error;
	error = copyin_pinned(&pin->length, 0, &pin->capacity,
	    sizeof(pin->capacity));
	if (error != 0) {
		sockaddr_output_unpin(pin);
		return error;
	}
	bytes = pin->capacity < sizeof(struct sockaddr_storage) ?
	    pin->capacity : sizeof(struct sockaddr_storage);
	error = uaccess_pin(address, bytes, PROT_WRITE, &pin->address);
	if (error != 0)
		sockaddr_output_unpin(pin);
	return error;
}

static int
copy_sockaddr_out_pinned(const struct sockaddr_output_pin *pin,
			 const struct sockaddr_storage *storage,
			 socklen_t actual)
{
	size_t copied;
	int error;

	if (pin == NULL || storage == NULL)
		return EINVAL;
	if (!pin->length.active)
		return 0;
	copied = pin->capacity < actual ? pin->capacity : actual;
	error = copied == 0 ? 0 :
	    copyout_pinned(&pin->address, 0, storage, copied);
	if (error == 0)
		error = copyout_pinned(&pin->length, 0, &actual, sizeof(actual));
	return error;
}

static intptr_t
sys_socket_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct socket *socket;
	struct file *file = NULL;
	int descriptor, error;

	if (process == NULL || process->fd == NULL || args[3] != 0 ||
	    args[4] != 0 || args[5] != 0)
		return -EINVAL;
	if (((int)args[0] == AF_PACKET || (int)args[1] == SOCK_RAW) &&
	    !cred_is_superuser(process->cred))
		return -EPERM;
	error = socket_create((int)args[0], (int)args[1], (int)args[2],
	    &socket);
	if (error != 0)
		return -error;
	error = socket_file_create(socket, &file);
	if (error != 0) {
		socket_release(socket);
		return -error;
	}
	error = filedesc_install(process->fd, file, &descriptor);
	if (error != 0) {
		(void)file_close(file);
		return -error;
	}
	return descriptor;
}

static intptr_t
sys_socketpair_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct socket *left_socket = NULL, *right_socket = NULL;
	struct file *left_file = NULL, *right_file = NULL;
	int descriptors[2] = { -1, -1 };
	int supplied_type = (int)args[1];
	int type = supplied_type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
	int file_flags = (supplied_type & SOCK_NONBLOCK) != 0 ? O_NONBLOCK : 0;
	unsigned descriptor_flags = (supplied_type & SOCK_CLOEXEC) != 0 ?
	    FILEDESC_CLOEXEC : 0;
	int error;

	if (process == NULL || process->fd == NULL || args[3] == 0 ||
	    args[4] != 0 || args[5] != 0 ||
	    (type != SOCK_STREAM && type != SOCK_DGRAM))
		return -EINVAL;
	if ((int)args[0] != AF_UNIX)
		return -EAFNOSUPPORT;
	error = unix_socket_pair_create(type, (int)args[2], &left_socket,
	    &right_socket);
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
	left_file->f_flags |= file_flags;
	right_file->f_flags |= file_flags;
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

static intptr_t
sys_bind_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct socket_file_ref reference;
	struct socket *socket;
	struct sockaddr_storage address;
	int error;

	if (descriptor_socket(process, (int)args[0], &reference) != 0)
		return -EBADF;
	socket = reference.socket;
	if (socket->ops == NULL || socket->ops->bind == NULL)
		return socket_result(&reference, -EOPNOTSUPP);
	error = copy_sockaddr_in(args[1], (socklen_t)args[2], &address);
	if (error == 0)
		error = socket->family == AF_UNIX ?
		    unix_socket_bind_path(socket, process->cwdi, process->cred,
		    process->umask, (struct sockaddr *)&address,
		    (socklen_t)args[2]) :
		    socket->ops->bind(socket, (struct sockaddr *)&address,
		    (socklen_t)args[2]);
	return socket_result(&reference, error == 0 ? 0 : -error);
}

static intptr_t
sys_connect_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct socket_file_ref reference;
	struct socket *socket;
	struct sockaddr_storage address;
	int error;

	if (descriptor_socket(process, (int)args[0], &reference) != 0)
		return -EBADF;
	socket = reference.socket;
	if (socket->ops == NULL || socket->ops->connect == NULL)
		return socket_result(&reference, -EOPNOTSUPP);
	error = copy_sockaddr_in(args[1], (socklen_t)args[2], &address);
	if (error == 0)
		error = socket->family == AF_UNIX ?
		    unix_socket_connect_path(socket, process->cwdi, process->cred,
		    (struct sockaddr *)&address, (socklen_t)args[2],
		    (reference.file->f_flags & O_NONBLOCK) != 0 ?
		    SOCKET_IO_NONBLOCK : 0) :
		    socket->ops->connect(socket, (struct sockaddr *)&address,
		    (socklen_t)args[2],
		    (reference.file->f_flags & O_NONBLOCK) != 0 ?
		    SOCKET_IO_NONBLOCK : 0);
	return socket_result(&reference, error == 0 ? 0 : -error);
}

static intptr_t
sys_listen_call(const uintptr_t args[6])
{
	struct socket_file_ref reference;
	struct socket *socket;
	int error;

	if (descriptor_socket(current_process(), (int)args[0], &reference) != 0)
		return -EBADF;
	socket = reference.socket;
	if (socket->ops == NULL || socket->ops->listen == NULL)
		return socket_result(&reference, -EOPNOTSUPP);
	error = socket->ops->listen(socket, (int)args[1]);
	return socket_result(&reference, error == 0 ? 0 : -error);
}

static intptr_t
sys_accept_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct socket_file_ref reference;
	struct socket *socket;
	struct sockaddr_storage address;
	struct sockaddr_output_pin output;
	struct socket *accepted = NULL;
	struct file *file = NULL;
	socklen_t length = sizeof(address);
	int descriptor, error;

	if (descriptor_socket(process, (int)args[0], &reference) != 0)
		return -EBADF;
	socket = reference.socket;
	if ((args[1] == 0) != (args[2] == 0))
		return socket_result(&reference, -EINVAL);
	if (socket->ops == NULL || socket->ops->accept == NULL)
		return socket_result(&reference, -EOPNOTSUPP);
	error = sockaddr_output_pin(args[1], args[2], &output);
	if (error != 0)
		return socket_result(&reference, -error);
	memset(&address, 0, sizeof(address));
	error = socket->ops->accept(socket, &accepted,
	    args[1] != 0 ? (struct sockaddr *)&address : NULL,
	    args[1] != 0 ? &length : NULL,
	    (reference.file->f_flags & O_NONBLOCK) != 0 ?
	    SOCKET_IO_NONBLOCK : 0);
	if (error != 0) {
		sockaddr_output_unpin(&output);
		return socket_result(&reference, -error);
	}
	if (accepted == NULL) {
		sockaddr_output_unpin(&output);
		return socket_result(&reference, -EIO);
	}
	socket_file_ref_put(&reference);
	error = socket_file_create(accepted, &file);
	if (error == 0)
		error = filedesc_install(process->fd, file, &descriptor);
	if (error != 0) {
		if (file != NULL)
			(void)file_close(file);
		else
			socket_release(accepted);
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

static intptr_t
sys_sendto_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct socket_file_ref reference;
	struct socket *socket;
	struct sockaddr_storage address;
	const struct sockaddr *destination = NULL;
	void *buffer;
	ssize_t result;
	int error;

	if (descriptor_socket(current_process(), (int)args[0], &reference) != 0)
		return -EBADF;
	socket = reference.socket;
	if (socket->ops == NULL || socket->ops->sendto == NULL)
		return socket_result(&reference, -EOPNOTSUPP);
	if ((args[4] == 0) != (args[5] == 0))
		return socket_result(&reference, -EINVAL);
	if (args[2] > PACKET_BUF_STORAGE_SIZE)
		return socket_result(&reference, -EMSGSIZE);
	if (args[4] != 0) {
		error = copy_sockaddr_in(args[4], (socklen_t)args[5], &address);
		if (error != 0)
			return socket_result(&reference, -error);
		destination = (const struct sockaddr *)&address;
	}
	if (args[2] == 0) {
		result = socket->family == AF_UNIX ?
		    unix_socket_send_message_at(socket, process->cwdi,
		    process->cred, "", 0,
		    (int)socket_file_effective_flags(&reference, (int)args[3]),
		    destination, (socklen_t)args[5], NULL, 0) :
		    socket->ops->sendto(socket, "", 0,
		    (int)socket_file_effective_flags(&reference, (int)args[3]),
		    destination, (socklen_t)args[5]);
		return socket_result(&reference, result);
	}
	buffer = kern_malloc((size_t)args[2]);
	if (buffer == NULL)
		return socket_result(&reference, -ENOMEM);
	error = copyin(args[1], buffer, (size_t)args[2]);
	result = error != 0 ? -error :
	    socket->family == AF_UNIX ?
	    unix_socket_send_message_at(socket, process->cwdi, process->cred,
	    buffer, (size_t)args[2],
	    (int)socket_file_effective_flags(&reference, (int)args[3]),
	    destination, (socklen_t)args[5], NULL, 0) :
	    socket->ops->sendto(socket, buffer, (size_t)args[2],
	    (int)socket_file_effective_flags(&reference, (int)args[3]), destination,
	    (socklen_t)args[5]);
	kern_free(buffer);
	return socket_result(&reference, result);
}

static intptr_t
sys_recvfrom_call(const uintptr_t args[6])
{
	struct socket_file_ref reference;
	struct socket *socket;
	struct sockaddr_storage address;
	struct sockaddr_output_pin output;
	struct uaccess_pin data_pin;
	socklen_t length = sizeof(address);
	size_t capacity;
	void *buffer;
	ssize_t result;
	int error;

	if (descriptor_socket(current_process(), (int)args[0], &reference) != 0)
		return -EBADF;
	socket = reference.socket;
	if (socket->ops == NULL || socket->ops->recvfrom == NULL)
		return socket_result(&reference, -EOPNOTSUPP);
	if ((args[4] == 0) != (args[5] == 0))
		return socket_result(&reference, -EINVAL);
	if (args[2] == 0)
		return socket_result(&reference, 0);
	capacity = args[2] > PACKET_BUF_STORAGE_SIZE ?
		PACKET_BUF_STORAGE_SIZE : (size_t)args[2];
	error = uaccess_pin(args[1], capacity, PROT_WRITE, &data_pin);
	if (error != 0)
		return socket_result(&reference, -error);
	error = sockaddr_output_pin(args[4], args[5], &output);
	if (error != 0) {
		uaccess_unpin(&data_pin);
		return socket_result(&reference, -error);
	}
	buffer = kern_malloc(capacity);
	if (buffer == NULL) {
		sockaddr_output_unpin(&output);
		uaccess_unpin(&data_pin);
		return socket_result(&reference, -ENOMEM);
	}
	memset(&address, 0, sizeof(address));
	result = socket->ops->recvfrom(socket, buffer, capacity,
	    (int)socket_file_effective_flags(&reference, (int)args[3]),
	    args[4] != 0 ? (struct sockaddr *)&address : NULL,
	    args[4] != 0 ? &length : NULL);
	if (result >= 0) {
		error = copyout_pinned(&data_pin, 0, buffer, (size_t)result);
		if (error == 0)
			error = copy_sockaddr_out_pinned(&output, &address, length);
		if (error != 0)
			result = -error;
	}
	kern_free(buffer);
	sockaddr_output_unpin(&output);
	uaccess_unpin(&data_pin);
	return socket_result(&reference, result);
}

static intptr_t
sys_sendmsg_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct zedbsd_sendmsg_request request;
	struct socket_file_ref reference;
	struct sockaddr_storage address;
	struct sockaddr *destination = NULL;
	struct file *files[ZEDBSD_MSG_FD_MAX];
	int descriptors[ZEDBSD_MSG_FD_MAX];
	void *buffer = NULL;
	unsigned index, count = 0;
	ssize_t result;
	int error;

	if (args[1] == 0 || args[2] != 0 || args[3] != 0 || args[4] != 0 ||
	    args[5] != 0)
		return -EINVAL;
	error = copyin(args[1], &request, sizeof(request));
	if (error != 0)
		return -error;
	if (request.reserved != 0 ||
	    request.data_length > PACKET_BUF_STORAGE_SIZE ||
	    request.name_length > sizeof(address) ||
	    request.descriptor_count > ZEDBSD_MSG_FD_MAX ||
	    (request.data_length != 0 && request.data == 0) ||
	    (request.name_length != 0 && request.name == 0) ||
	    (request.descriptor_count != 0 && request.descriptors == 0))
		return -EINVAL;
	if (descriptor_socket(process, (int)args[0], &reference) != 0)
		return -EBADF;
	if (request.name_length != 0) {
		error = copy_sockaddr_in((uintptr_t)request.name,
		    request.name_length, &address);
		if (error != 0)
			return socket_result(&reference, -error);
		destination = (struct sockaddr *)&address;
	}
	if (request.data_length != 0) {
		buffer = kern_malloc((size_t)request.data_length);
		if (buffer == NULL)
			return socket_result(&reference, -ENOMEM);
		error = copyin((uintptr_t)request.data, buffer,
		    (size_t)request.data_length);
		if (error != 0) {
			kern_free(buffer);
			return socket_result(&reference, -error);
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
	result = unix_socket_send_message_at(reference.socket,
	    process->cwdi, process->cred,
	    request.data_length != 0 ? buffer : "",
	    (size_t)request.data_length,
	    (int)socket_file_effective_flags(&reference, (int)request.flags),
	    destination, request.name_length, files, count);
	kern_free(buffer);
	return socket_result(&reference, result);
fail:
	for (index = 0; index < count; index++)
		(void)file_close(files[index]);
	kern_free(buffer);
	return socket_result(&reference, -error);
}

static intptr_t
sys_recvmsg_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct zedbsd_recvmsg_request request;
	struct socket_file_ref reference;
	struct sockaddr_storage address;
	struct unix_recv_transaction transaction;
	struct filedesc_reservation reservation;
	int descriptors[ZEDBSD_MSG_FD_MAX];
	void *buffer = NULL;
	socklen_t name_length;
	unsigned file_count, truncated = 0, index;
	ssize_t result;
	int error;

	if (args[1] == 0 || args[2] != 0 || args[3] != 0 || args[4] != 0 ||
	    args[5] != 0)
		return -EINVAL;
	error = copyin(args[1], &request, sizeof(request));
	if (error != 0)
		return -error;
	if (request.reserved != 0 || request.reserved2 != 0 ||
	    request.data_capacity > PACKET_BUF_STORAGE_SIZE ||
	    request.name_capacity > sizeof(address) ||
	    request.descriptor_capacity > ZEDBSD_MSG_FD_MAX ||
	    (request.data_capacity != 0 && request.data == 0) ||
	    (request.name_capacity != 0 && request.name == 0) ||
	    (request.descriptor_capacity != 0 && request.descriptors == 0))
		return -EINVAL;
	if (descriptor_socket(process, (int)args[0], &reference) != 0)
		return -EBADF;
	if (request.data_capacity != 0) {
		buffer = kern_malloc((size_t)request.data_capacity);
		if (buffer == NULL)
			return socket_result(&reference, -ENOMEM);
	}
	name_length = request.name_capacity;
	memset(&transaction, 0, sizeof(transaction));
	memset(&reservation, 0, sizeof(reservation));
	result = unix_socket_receive_begin(reference.socket,
	    request.data_capacity != 0 ? buffer : "",
	    (size_t)request.data_capacity,
	    (int)socket_file_effective_flags(&reference, (int)request.flags),
	    request.name_capacity != 0 ? (struct sockaddr *)&address : NULL,
	    request.name_capacity != 0 ? &name_length : NULL,
	    request.descriptor_capacity, &transaction);
	if (result < 0) {
		kern_free(buffer);
		return socket_result(&reference, result);
	}
	file_count = transaction.file_count;
	truncated = transaction.control_truncated;
	if (transaction.active) {
		error = filedesc_reserve_many(process->fd, file_count,
		    (request.flags & MSG_CMSG_CLOEXEC) != 0 ?
		    FILEDESC_CLOEXEC : 0, &reservation);
		if (error != 0) {
			unix_socket_receive_abort(&transaction);
			kern_free(buffer);
			return socket_result(&reference, -error);
		}
		for (index = 0; index < file_count; index++)
			descriptors[index] = reservation.slots[index];
	}
	if (result != 0)
		error = copyout(buffer, (uintptr_t)request.data, (size_t)result);
	else
		error = 0;
	if (error == 0 && request.name_capacity != 0) {
		socklen_t copied = request.name_capacity < name_length ?
		    request.name_capacity : name_length;
		if (copied != 0)
			error = copyout(&address, (uintptr_t)request.name, copied);
	}
	if (error == 0 && file_count != 0)
		error = copyout(descriptors, (uintptr_t)request.descriptors,
		    file_count * sizeof(descriptors[0]));
	request.data_length = (uint64_t)result;
	request.name_length = name_length;
	request.descriptor_count = file_count;
	request.output_flags = truncated ? MSG_CTRUNC : 0;
	if (error == 0)
		error = copyout(&request, args[1], sizeof(request));
	if (error != 0) {
		filedesc_abort_reserved(&reservation);
		unix_socket_receive_abort(&transaction);
		kern_free(buffer);
		return socket_result(&reference, -error);
	}
	if (transaction.active) {
		error = filedesc_commit_reserved(&reservation, transaction.files,
		    descriptors);
		if (error != 0) {
			filedesc_abort_reserved(&reservation);
			unix_socket_receive_abort(&transaction);
			kern_free(buffer);
			return socket_result(&reference, -error);
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
	return socket_result(&reference, result);
}

static intptr_t
sys_shutdown_call(const uintptr_t args[6])
{
	struct socket_file_ref reference;
	struct socket *socket;
	int error;

	if (descriptor_socket(current_process(), (int)args[0], &reference) != 0)
		return -EBADF;
	socket = reference.socket;
	if (socket->ops == NULL || socket->ops->shutdown == NULL)
		return socket_result(&reference, -EOPNOTSUPP);
	error = socket->ops->shutdown(socket, (int)args[1]);
	return socket_result(&reference, error == 0 ? 0 : -error);
}

static intptr_t
sys_socket_name_call(const uintptr_t args[6], int peer)
{
	struct socket_file_ref reference;
	struct socket *socket;
	struct sockaddr_storage address;
	socklen_t length = sizeof(address);
	int error;

	if (descriptor_socket(current_process(), (int)args[0], &reference) != 0)
		return -EBADF;
	socket = reference.socket;
	if (args[1] == 0 || args[2] == 0)
		return socket_result(&reference, -EINVAL);
	memset(&address, 0, sizeof(address));
	if (peer) {
		if (socket->ops == NULL || socket->ops->getpeername == NULL)
			return socket_result(&reference, -EOPNOTSUPP);
		error = socket->ops->getpeername(socket,
		    (struct sockaddr *)&address, &length);
	} else {
		if (socket->ops == NULL || socket->ops->getsockname == NULL)
			return socket_result(&reference, -EOPNOTSUPP);
		error = socket->ops->getsockname(socket,
		    (struct sockaddr *)&address, &length);
	}
	if (error == 0)
		error = copy_sockaddr_out(args[1], args[2], &address, length);
	return socket_result(&reference, error == 0 ? 0 : -error);
}

static intptr_t
sys_setsockopt_call(const uintptr_t args[6])
{
	struct socket_file_ref reference;
	struct socket *socket;
	uint8_t value[SYSCALL_SOCKET_OPTION_MAX];
	int error;

	if (descriptor_socket(current_process(), (int)args[0], &reference) != 0)
		return -EBADF;
	socket = reference.socket;
	if (args[4] > sizeof(value) || (args[4] != 0 && args[3] == 0))
		return socket_result(&reference, -EINVAL);
	error = args[4] == 0 ? 0 : copyin(args[3], value, (size_t)args[4]);
	if (error == 0)
		error = socket_setsockopt_common(socket, (int)args[1],
		    (int)args[2], value, (socklen_t)args[4]);
	if (error == ENOPROTOOPT && socket->ops != NULL &&
	    socket->ops->setsockopt != NULL)
		error = socket->ops->setsockopt(socket, (int)args[1],
		    (int)args[2], value, (socklen_t)args[4]);
	return socket_result(&reference, error == 0 ? 0 : -error);
}

static intptr_t
sys_getsockopt_call(const uintptr_t args[6])
{
	struct socket_file_ref reference;
	struct socket *socket;
	uint8_t value[SYSCALL_SOCKET_OPTION_MAX];
	socklen_t length;
	int error;

	if (descriptor_socket(current_process(), (int)args[0], &reference) != 0)
		return -EBADF;
	socket = reference.socket;
	if (args[3] == 0 || args[4] == 0)
		return socket_result(&reference, -EINVAL);
	error = copyin(args[4], &length, sizeof(length));
	if (error != 0)
		return socket_result(&reference, -error);
	if (length > sizeof(value))
		length = sizeof(value);
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
	return socket_result(&reference, error == 0 ? 0 : -error);
}

static int
syscall_context_at(struct process *process, int dirfd,
		   struct cwdinfo *temporary, struct cwdinfo **context,
		   struct file **held)
{
	if (process == NULL || temporary == NULL || context == NULL || held == NULL)
		return EINVAL;
	*held = NULL;
	if (dirfd == AT_FDCWD) {
		*context = process->cwdi;
		return 0;
	}
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

static intptr_t sys_open_call(const uintptr_t args[6], int at)
{
	struct process *process = current_process();
	struct cwdinfo temporary, *context;
	struct file *file, *held;
	char path[PATH_MAX];
	uintptr_t path_address = at ? args[1] : args[0];
	int flags = (int)(at ? args[2] : args[1]);
	mode_t mode = (mode_t)(at ? args[3] : args[2]);
	int descriptor, error;
	if (process == NULL || process->fd == NULL || process->cwdi == NULL)
		return -EINVAL;
	error = copyinstr(path_address, path, sizeof(path), NULL);
	held = NULL;
	if (error == 0 && path[0] == '/')
		context = process->cwdi;
	else if (error == 0)
		error = syscall_context_at(process, at ? (int)args[0] : AT_FDCWD,
		    &temporary, &context, &held);
	if (error == 0)
		error = file_openat_cred(context, process->cred, path,
		    flags & ~O_CLOEXEC, (mode & 07777U) & ~process->umask,
		    &file);
	if (held != NULL) (void)file_close(held);
	if (error != 0) return -error;
	error = filedesc_install_from(process->fd, file,
	    (flags & O_CLOEXEC) != 0 ? FILEDESC_CLOEXEC : 0,
	    0, &descriptor);
	if (error != 0) {
		(void)file_close(file);
		return -error;
	}
	return descriptor;
}

static intptr_t sys_close_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	int error = process == NULL || process->fd == NULL ? EBADF :
		filedesc_close(process->fd, (int)args[0]);
	return error == 0 ? 0 : -error;
}

static SYSCALL_EXT intptr_t sys_read_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct file *file;
	struct uaccess_pin pin;
	uint8_t buffer[SYSCALL_IO_CHUNK];
	size_t done = 0, length = (size_t)args[2];
	intptr_t result;
	int error;
	if (process == NULL ||
	    (file = filedesc_get_ref(process->fd, (int)args[0])) == NULL)
		return -EBADF;
	error = uaccess_pin(args[1], length, HAL_SPACE_WRITE, &pin);
	if (error != 0) {
		(void)file_close(file);
		return -error;
	}
	while (done < length) {
		size_t chunk = length - done > sizeof(buffer) ? sizeof(buffer) : length - done;
		ssize_t count = file_read(file, buffer, chunk);
		if (count < 0) {
			result = done != 0 ? (intptr_t)done : count;
			goto out;
		}
		if (count == 0) break;
		error = copyout_pinned(&pin, done, buffer, (size_t)count);
		if (error != 0) {
			result = done != 0 ? (intptr_t)done : -error;
			goto out;
		}
		done += (size_t)count;
		if ((size_t)count < chunk) break;
	}
	result = (intptr_t)done;
out:
	uaccess_unpin(&pin);
	(void)file_close(file);
	return result;
}

static SYSCALL_EXT intptr_t sys_write_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct file *file;
	struct uaccess_pin pin;
	uint8_t buffer[SYSCALL_IO_CHUNK];
	size_t done = 0, length = (size_t)args[2];
	intptr_t result;
	int error;
	if (process == NULL ||
	    (file = filedesc_get_ref(process->fd, (int)args[0])) == NULL)
		return -EBADF;
	error = uaccess_pin(args[1], length, HAL_SPACE_READ, &pin);
	if (error != 0) {
		(void)file_close(file);
		return -error;
	}
	while (done < length) {
		size_t chunk = length - done > sizeof(buffer) ? sizeof(buffer) : length - done;
		ssize_t count;
		error = copyin_pinned(&pin, done, buffer, chunk);
		if (error != 0) {
			result = done != 0 ? (intptr_t)done : -error;
			goto out;
		}
		count = file_write(file, buffer, chunk);
		if (count < 0) {
			result = done != 0 ? (intptr_t)done : count;
			goto out;
		}
		done += (size_t)count;
		if ((size_t)count < chunk) break;
	}
	result = (intptr_t)done;
out:
	if (result > 0 && file->f_inode != NULL)
		(void)vfs_clear_setid_on_write(file->f_inode, process->cred);
	uaccess_unpin(&pin);
	(void)file_close(file);
	return result;
}

static intptr_t sys_lseek_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct file *file = process != NULL ?
	    filedesc_get_ref(process->fd, (int)args[0]) : NULL;
	off_t result;
	if (file == NULL)
		return -EBADF;
	result = file_seek(file, (off_t)args[1], (int)args[2]);
	(void)file_close(file);
	return result;
}

static intptr_t sys_fstat_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct file *file = process != NULL ?
	    filedesc_get_ref(process->fd, (int)args[0]) : NULL;
	struct stat status;
	int error;
	if (file == NULL) return -EBADF;
	if (file->f_inode == NULL) {
		(void)file_close(file);
		return -EINVAL;
	}
	error = inode_getattr(file->f_inode, &status);
	if (error == 0) error = copyout(&status, args[1], sizeof(status));
	(void)file_close(file);
	return error == 0 ? 0 : -error;
}

static uint32_t dirent_type(enum inode_type type)
{
	switch (type) {
	case INODE_REG: return ZEDBSD_DT_REG;
	case INODE_DIR: return ZEDBSD_DT_DIR;
	case INODE_BLOCK: return ZEDBSD_DT_BLK;
	case INODE_CHAR: return ZEDBSD_DT_CHR;
	default: return ZEDBSD_DT_UNKNOWN;
	}
}

static intptr_t sys_getdents_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct file *file = process != NULL ?
	    filedesc_get_ref(process->fd, (int)args[0]) : NULL;
	struct uaccess_pin pin;
	struct zedbsd_dirent output;
	struct dirent entry;
	int eof, error;
	if (file == NULL) return -EBADF;
	if (args[2] < sizeof(output)) {
		(void)file_close(file);
		return -EINVAL;
	}
	error = uaccess_pin(args[1], sizeof(output), HAL_SPACE_WRITE, &pin);
	if (error != 0) {
		(void)file_close(file);
		return -error;
	}
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
	return error == 0 ? (intptr_t)sizeof(output) : -error;
}

static intptr_t sys_chdir_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	char path[PATH_MAX];
	int error;
	if (process == NULL || process->cwdi == NULL) return -EINVAL;
	error = copyinstr(args[0], path, sizeof(path), NULL);
	if (error == 0) error = fs_chdir(process->cwdi, path);
	return error == 0 ? 0 : -error;
}

static intptr_t sys_getcwd_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	char path[PATH_MAX];
	size_t length;
	int error;
	if (process == NULL || process->cwdi == NULL) return -EINVAL;
	error = fs_getcwd(process->cwdi, path, sizeof(path));
	if (error != 0) return -error;
	length = strlen(path) + 1U;
	if (length > args[1]) return -ERANGE;
	error = copyout(path, args[0], length);
	return error == 0 ? (intptr_t)args[0] : -error;
}

static int vm_prot(int prot, uint32_t *result)
{
	uint32_t value = 0;
	if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) return EINVAL;
	if ((prot & (PROT_WRITE | PROT_EXEC)) == (PROT_WRITE | PROT_EXEC)) return EACCES;
	if (prot & PROT_READ) value |= HAL_SPACE_READ;
	if (prot & PROT_WRITE) value |= HAL_SPACE_WRITE;
	if (prot & PROT_EXEC) value |= HAL_SPACE_EXEC;
	*result = value;
	return 0;
}

static intptr_t sys_mmap_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct file *file = NULL;
	uintptr_t mapped;
	uint32_t prot;
	size_t data_size = 0;
	int shared;
	int error;
	if (process == NULL || process->vmspace == NULL) return -EINVAL;
	if ((args[3] & MAP_FIXED) != 0)
		return -EINVAL;
	if ((args[3] & (MAP_PRIVATE | MAP_SHARED)) != MAP_PRIVATE &&
	    (args[3] & (MAP_PRIVATE | MAP_SHARED)) != MAP_SHARED)
		return -EINVAL;
	if ((args[3] & ~(MAP_PRIVATE | MAP_SHARED | MAP_ANONYMOUS |
	    MAP_FIXED_NOREPLACE)) != 0)
		return -EOPNOTSUPP;
	shared = (args[3] & MAP_SHARED) != 0;
	if (args[1] == 0 || args[1] > SIZE_MAX - SYSCALL_PAGE_MASK)
		return -EINVAL;
	if (args[0] != 0 && (args[0] & SYSCALL_PAGE_MASK) != 0)
		return -EINVAL;
	if ((args[3] & MAP_FIXED_NOREPLACE) != 0 && args[0] == 0)
		return -EINVAL;
	if ((args[3] & MAP_ANONYMOUS) != 0) {
		if ((int)args[4] != -1 || args[5] != 0)
			return -EINVAL;
		if (shared)
			return -EOPNOTSUPP;
	} else {
		if ((args[5] & SYSCALL_PAGE_MASK) != 0 || (off_t)args[5] < 0)
			return -EINVAL;
		file = filedesc_get_ref(process->fd, (int)args[4]);
		if (file == NULL)
			return -EBADF;
		if (file->f_inode == NULL || file->f_inode->i_type != INODE_REG ||
		    (file->f_flags & O_ACCMODE) == O_WRONLY) {
			(void)file_close(file);
			return -EACCES;
		}
		if (shared && (args[2] & PROT_WRITE) != 0 &&
		    ((file->f_flags & O_ACCMODE) == O_RDONLY ||
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
	error = vm_prot((int)args[2], &prot);
	if (error == 0 && file != NULL && shared &&
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
		size_t size = (args[1] + SYSCALL_PAGE_MASK) & ~SYSCALL_PAGE_MASK;
		error = vmspace_map_anon_fixed_noreplace(process->vmspace,
			args[0], size, prot, NULL);
		mapped = args[0];
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
	return error == 0 ? (intptr_t)mapped : -error;
}

static intptr_t sys_munmap_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	size_t size;
	int error;
	if (args[1] == 0 || args[1] > SIZE_MAX - SYSCALL_PAGE_MASK)
		return -EINVAL;
	size = (args[1] + SYSCALL_PAGE_MASK) & ~SYSCALL_PAGE_MASK;
	error = process == NULL ? EINVAL :
		vmspace_unmap(process->vmspace, args[0], size);
	return error == 0 ? 0 : -error;
}

static intptr_t sys_mprotect_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	uint32_t prot;
	int error = vm_prot((int)args[2], &prot);
	if (error == 0 && (args[1] == 0 ||
	    args[1] > SIZE_MAX - SYSCALL_PAGE_MASK))
		error = EINVAL;
	if (error == 0)
		error = process == NULL ? EINVAL :
			vmspace_protect(process->vmspace, args[0],
			    (args[1] + SYSCALL_PAGE_MASK) & ~SYSCALL_PAGE_MASK, prot);
	return error == 0 ? 0 : -error;
}

static intptr_t sys_msync_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	size_t size;
	int error;
	if (args[1] == 0 || args[1] > SIZE_MAX - SYSCALL_PAGE_MASK)
		return -EINVAL;
	size = (args[1] + SYSCALL_PAGE_MASK) & ~SYSCALL_PAGE_MASK;
	error = process == NULL ? EINVAL : vmspace_sync(process->vmspace,
	    args[0], size, (int)args[2]);
	return error == 0 ? 0 : -error;
}

static intptr_t sys_brk_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	uintptr_t result;
	int error;

	if (process == NULL || process->vmspace == NULL)
		return -EINVAL;
	error = vmspace_brk(process->vmspace, args[0], &result);
	return error == 0 ? (intptr_t)result : -error;
}

static intptr_t sys_ioctl_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct file *file = process != NULL ?
	    filedesc_get_ref(process->fd, (int)args[0]) : NULL;
	int error = file == NULL ? EBADF : file_ioctl(file, args[1], args[2]);
	if (file != NULL)
		(void)file_close(file);
	return error == 0 ? 0 : -error;
}

static intptr_t sys_clock_gettime_call(const uintptr_t args[6])
{
	struct timespec time;
	int error = kern_clock_gettime((clockid_t)args[0], &time);
	if (error != 0) return -error;
	error = copyout(&time, args[1], sizeof(time));
	return error == 0 ? 0 : -error;
}

static intptr_t sys_clock_getres_call(const uintptr_t args[6])
{
	struct timespec resolution;
	int error = kern_clock_getres((clockid_t)args[0],
	    args[1] == 0 ? NULL : &resolution);
	if (error == 0 && args[1] != 0)
		error = copyout(&resolution, args[1], sizeof(resolution));
	return error == 0 ? 0 : -error;
}

static intptr_t sys_clock_settime_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct timespec requested;
	int error;

	if (process == NULL || process->cred == NULL)
		return -EINVAL;
	error = copyin(args[1], &requested, sizeof(requested));
	if (error == 0)
		error = kern_clock_settime((clockid_t)args[0], &requested,
		    process->cred);
	return error == 0 ? 0 : -error;
}

static intptr_t sys_timer_create_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct sigevent event;
	const struct sigevent *eventp = NULL;
	timer_t id;
	int error;

	if (process == NULL || args[2] == 0)
		return -EINVAL;
	if (args[1] != 0) {
		error = copyin(args[1], &event, sizeof(event));
		if (error != 0)
			return -error;
		eventp = &event;
	}
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

static intptr_t sys_timer_delete_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	int error = process == NULL ? EINVAL :
	    process_timer_delete(process, (timer_t)args[0]);
	return error == 0 ? 0 : -error;
}

static intptr_t sys_timer_settime_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct itimerspec requested, previous;
	int error;

	if (process == NULL || args[2] == 0)
		return -EINVAL;
	error = copyin(args[2], &requested, sizeof(requested));
	if (error == 0)
		error = process_timer_settime(process, (timer_t)args[0],
		    (int)args[1], &requested, args[3] == 0 ? NULL : &previous);
	if (error == 0 && args[3] != 0)
		error = copyout(&previous, args[3], sizeof(previous));
	return error == 0 ? 0 : -error;
}

static intptr_t sys_timer_gettime_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct itimerspec current;
	int error;

	if (process == NULL || args[1] == 0)
		return -EINVAL;
	error = process_timer_gettime(process, (timer_t)args[0], &current);
	if (error == 0)
		error = copyout(&current, args[1], sizeof(current));
	return error == 0 ? 0 : -error;
}

static intptr_t sys_timer_getoverrun_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	int overrun, error;

	if (process == NULL)
		return -EINVAL;
	error = process_timer_getoverrun(process, (timer_t)args[0], &overrun);
	return error == 0 ? overrun : -error;
}

static intptr_t sys_mount_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct zedbsd_mount_args requested;
	struct fat_mount_args internal;
	char type[NAME_MAX + 1U], directory[PATH_MAX];
	int flags = (int)args[2];
	int error;

	if (process == NULL || process->cred == NULL)
		return -EINVAL;
	if (!cred_is_superuser(process->cred))
		return -EPERM;
	if ((flags & ~(int)MNT_RDONLY) != 0)
		return -EINVAL;
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
	if (error == 0)
		error = mount(type, directory,
		    (flags & MNT_RDONLY) != 0 ? MOUNT_READ_ONLY : 0,
		    internal.fspec != NULL ? &internal : NULL);
	return error == 0 ? 0 : -error;
}

static intptr_t sys_unmount_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	char directory[PATH_MAX];
	int error;

	if (process == NULL || process->cred == NULL)
		return -EINVAL;
	if (!cred_is_superuser(process->cred))
		return -EPERM;
	if (args[1] != 0)
		return -EINVAL;
	error = copyinstr(args[0], directory, sizeof(directory), NULL);
	if (error == 0)
		error = unmount(directory, 0);
	return error == 0 ? 0 : -error;
}

static intptr_t sys_statvfs_call(const uintptr_t args[6], int by_fd)
{
	struct process *process = current_process();
	struct statvfs status;
	struct path path;
	struct file *file = NULL;
	char pathname[PATH_MAX];
	int error;

	if (process == NULL || process->cwdi == NULL)
		return -EINVAL;
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
	return error == 0 ? 0 : -error;
}

static intptr_t
sys_quotactl_call(const uintptr_t args[6])
{
	struct process *process=current_process();
	struct zedbsd_quota_ctl request;
	struct path path;
	char pathname[PATH_MAX];
	int error,allowed=0;
	if(process==NULL||process->cred==NULL||process->cwdi==NULL||args[1]==0)
		return -EINVAL;
	error=copyinstr(args[0],pathname,sizeof(pathname),NULL);
	if(error==0)error=copyin(args[1],&request,sizeof(request));
	if(error==0&&(request.size!=sizeof(request)||
	    request.version!=ZEDBSD_QUOTA_VERSION||
	    request.type>ZEDBSD_QUOTA_GROUP||
	    request.command<ZEDBSD_QUOTA_GET||
	    request.command>ZEDBSD_QUOTA_SYNC))error=EINVAL;
	if(error==0) {
		allowed=cred_is_superuser(process->cred);
		if(request.command==ZEDBSD_QUOTA_GET&&!allowed) {
			if(request.type==ZEDBSD_QUOTA_USER)
				allowed=request.id==process->cred->ruid||
				    request.id==process->cred->euid||
				    request.id==process->cred->suid;
			else allowed=cred_in_group(process->cred,(gid_t)request.id);
		}
		if(!allowed)error=EPERM;
	}
	if(error==0)error=namei_path_at(process->cwdi,pathname,&path);
	if(error==0){error=mount_quotactl(path.p_mount,&request);path_release(&path);}
	if(error==0)error=copyout(&request,args[1],sizeof(request));
	return error==0?0:-error;
}

static intptr_t
sys_snapshotctl_call(const uintptr_t args[6])
{
	struct process *process=current_process();struct zedbsd_snapshot_ctl request;
	struct path path;char pathname[PATH_MAX];int error;
	if(process==NULL||process->cred==NULL||process->cwdi==NULL||args[1]==0)
		return -EINVAL;
	if(!cred_is_superuser(process->cred))return -EPERM;
	error=copyinstr(args[0],pathname,sizeof(pathname),NULL);
	if(error==0)error=copyin(args[1],&request,sizeof(request));
	if(error==0&&(request.size!=sizeof(request)||
	    request.version!=ZEDBSD_SNAPSHOT_VERSION||
	    request.command<ZEDBSD_SNAPSHOT_CREATE||
	    request.command>ZEDBSD_SNAPSHOT_STATUS))error=EINVAL;
	if(error==0)error=namei_path_at(process->cwdi,pathname,&path);
	if(error==0){error=mount_snapshotctl(path.p_mount,&request);path_release(&path);}
	if(error==0)error=copyout(&request,args[1],sizeof(request));
	return error==0?0:-error;
}

static intptr_t sys_nanosleep_call(const uintptr_t args[6])
{
	struct timespec request;
	struct timespec remaining;
	uint64_t ticks, deadline, left;
	int error = copyin(args[0], &request, sizeof(request));
	if (error != 0) return -error;
	error = kern_duration_to_ticks_ceil(&request, &ticks);
	if (error != 0) return -error;
	if (ticks == 0) return 0;
	if (signal_pending_unblocked(curthread))
		return -EINTR;
	error = kern_deadline_after(sched_ticks(), ticks, &deadline);
	if (error != 0) return -error;
	sched_sleep(deadline);
	if (signal_pending_unblocked(curthread) && sched_ticks() < deadline) {
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
	return 0;
}

struct syscall_exec_args {
	char *argv[ZEDBSD_SPAWN_ARG_MAX + 1U];
	char *envp[ZEDBSD_SPAWN_ENV_MAX + 1U];
	char strings[ZEDBSD_SPAWN_STRING_MAX];
	size_t used;
};

static int
copy_exec_vector(uintptr_t address, char **vector, unsigned maximum,
		 struct syscall_exec_args *copy, int optional)
{
	unsigned index;
	if (address == 0) {
		if (!optional)
			return EFAULT;
		vector[0] = NULL;
		return 0;
	}
	for (index = 0; index < maximum; index++) {
#ifdef ZEDBSD_USER_ABI_LP64
		uintptr_t pointer;
#else
		uint32_t pointer;
#endif
		size_t length;
		int error = copyin(address + index * sizeof(pointer), &pointer,
				   sizeof(pointer));
		if (error != 0)
			return error;
		if (pointer == 0) {
			vector[index] = NULL;
			return index == 0 && !optional ? EINVAL : 0;
		}
		if (copy->used >= sizeof(copy->strings))
			return E2BIG;
		vector[index] = copy->strings + copy->used;
		error = copyinstr(pointer, vector[index],
				  sizeof(copy->strings) - copy->used, &length);
		if (error != 0)
			return error == ENAMETOOLONG ? E2BIG : error;
		copy->used += length;
	}
	return E2BIG;
}

static SYSCALL_EXT intptr_t
sys_positional_call(const uintptr_t args[6], int writing)
{
	struct process *process = current_process();
	struct file *file;
	struct uaccess_pin pin;
	uint8_t buffer[SYSCALL_IO_CHUNK];
	size_t done = 0, length = (size_t)args[2];
	off_t offset = (off_t)args[3];
	intptr_t result;
	int error;
	if (process == NULL ||
	    (file = filedesc_get_ref(process->fd, (int)args[0])) == NULL)
		return -EBADF;
	if (offset < 0) {
		(void)file_close(file);
		return -EINVAL;
	}
	error = uaccess_pin(args[1], length,
	    writing ? HAL_SPACE_READ : HAL_SPACE_WRITE, &pin);
	if (error != 0) {
		(void)file_close(file);
		return -error;
	}
	while (done < length) {
		size_t chunk = length - done > sizeof(buffer) ?
		    sizeof(buffer) : length - done;
		ssize_t count;
		off_t current;
		error = off_add_size(offset, done, &current);
		if (error != 0) {
			result = done != 0 ? (intptr_t)done : -error;
			goto out;
		}
		if (writing) {
			error = copyin_pinned(&pin, done, buffer, chunk);
			if (error != 0)
				goto copy_error;
			count = file_pwrite(file, buffer, chunk, current);
		} else {
			count = file_pread(file, buffer, chunk, current);
		}
		if (count < 0) {
			result = done != 0 ? (intptr_t)done : count;
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
		if ((size_t)count < chunk)
			break;
	}
	result = (intptr_t)done;
	goto out;
copy_error:
	result = done != 0 ? (intptr_t)done : -error;
out:
	if (writing && result > 0 && file->f_inode != NULL)
		(void)vfs_clear_setid_on_write(file->f_inode, process->cred);
	uaccess_unpin(&pin);
	(void)file_close(file);
	return result;
}

#ifdef ZEDBSD_USER_ABI_LP64
struct syscall_iovec { uint64_t base, length; };
#else
struct syscall_iovec { uint32_t base, length; };
#endif

static SYSCALL_EXT intptr_t
sys_vector_call(const uintptr_t args[6], int writing)
{
	struct syscall_iovec vectors[16];
	struct uaccess_pin pins[16];
	int count = (int)args[2], i, pinned = 0;
	intptr_t total = 0;
	int error;
	if (count < 0 || count > 16)
		return -EINVAL;
	if (count == 0)
		return 0;
	if ((size_t)count > SIZE_MAX / sizeof(vectors[0]))
		return -EOVERFLOW;
	error = copyin(args[1], vectors, (size_t)count * sizeof(vectors[0]));
	if (error != 0)
		return -error;
	for (i = 0; i < count; i++) {
		if (vectors[i].length > (uint64_t)SSIZE_MAX - (uint64_t)total) {
			error = EINVAL;
			goto fail;
		}
		error = uaccess_pin((uintptr_t)vectors[i].base,
		    (size_t)vectors[i].length,
		    writing ? HAL_SPACE_READ : HAL_SPACE_WRITE, &pins[i]);
		if (error != 0)
			goto fail;
		pinned++;
		total += (intptr_t)vectors[i].length;
	}
	total = 0;
	for (i = 0; i < count; i++) {
		uintptr_t scalar[6] = { args[0], (uintptr_t)vectors[i].base,
		    (uintptr_t)vectors[i].length, 0, 0, 0 };
		intptr_t result;
		result = writing ? sys_write_call(scalar) : sys_read_call(scalar);
		if (result < 0) {
			total = total != 0 ? total : result;
			goto out;
		}
		total += result;
		if ((uintptr_t)result < (uintptr_t)vectors[i].length)
			break;
	}
	goto out;
fail:
	total = -error;
out:
	while (pinned != 0)
		uaccess_unpin(&pins[--pinned]);
	return total;
}

static intptr_t
sys_fsync_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct file *file = process != NULL ?
	    filedesc_get_ref(process->fd, (int)args[0]) : NULL;
	int error = file == NULL ? EBADF :
	    file_vm_inode(file) == NULL ? EINVAL :
	    vm_object_sync_inode(file_vm_inode(file));
	/* Then flush this descriptor's own backend/open-file state. */
	if (error == 0 && file != NULL)
		error = file_fsync(file);
	if (file != NULL)
		(void)file_close(file);
	return error == 0 ? 0 : -error;
}

static intptr_t
sys_stat_path_call(const uintptr_t args[6], int at, int nofollow)
{
	struct process *process = current_process();
	struct cwdinfo temporary, *context;
	struct file *held = NULL;
	struct path path;
	struct stat status;
	char pathname[PATH_MAX];
	uintptr_t pathname_address = at ? args[1] : args[0];
	uintptr_t status_address = at ? args[2] : args[1];
	int error;
	if (process == NULL || process->cwdi == NULL)
		return -EINVAL;
	if (at && ((int)args[3] & ~AT_SYMLINK_NOFOLLOW) != 0)
		return -EINVAL;
	error = copyinstr(pathname_address, pathname, sizeof(pathname), NULL);
	if (error == 0 && pathname[0] == '/') {
		context = process->cwdi;
		held = NULL;
	} else if (error == 0) {
		error = syscall_context_at(process, at ? (int)args[0] : AT_FDCWD,
		    &temporary, &context, &held);
	}
	if (error == 0)
		error = namei_path_flags_at(context, pathname,
			nofollow || (at && ((int)args[3] & AT_SYMLINK_NOFOLLOW) != 0) ?
			NAMEI_NOFOLLOW_FINAL : 0, &path);
	if (error == 0) {
		error = inode_getattr(path.p_inode, &status);
		path_release(&path);
	}
	if (error == 0)
		error = copyout(&status, status_address, sizeof(status));
	if (held != NULL)
		(void)file_close(held);
	return error == 0 ? 0 : -error;
}

static intptr_t
sys_truncate_call(const uintptr_t args[6], int by_fd)
{
	struct process *process = current_process();
	struct inode *inode = NULL;
	struct path path;
	struct file *file = NULL;
	char pathname[PATH_MAX];
	off_t length = (off_t)args[1];
	int error;
	if (process == NULL || length < 0)
		return -EINVAL;
	if (by_fd) {
		file = filedesc_get_ref(process->fd, (int)args[0]);
		if (file == NULL)
			return -EBADF;
		if ((file->f_flags & O_ACCMODE) == O_RDONLY) {
			(void)file_close(file);
			return -EBADF;
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
	if (inode == NULL || inode->i_type != INODE_REG)
		error = inode != NULL && inode->i_type == INODE_DIR ? EISDIR : EINVAL;
	else if (!by_fd &&
	    (error = vfs_access(inode, process->cred, W_OK)) != 0)
		;
	else
		error = inode_truncate(inode, length);
	if (error == 0)
		error = vfs_clear_setid_on_write(inode, process->cred);
	if (error == 0)
		vm_object_truncate_inode(inode, length);
	if (!by_fd)
		path_release(&path);
	else
		(void)file_close(file);
	return error == 0 ? 0 : -error;
}

static SYSCALL_EXT intptr_t
sys_mutation_common(uint32_t number, int old_dirfd, uintptr_t old_address,
		    uintptr_t option, int new_dirfd, uintptr_t new_address)
{
	struct process *process = current_process();
	struct cwdinfo temporary, other_temporary, *context, *other_context;
	struct file *held = NULL, *other_held = NULL;
	struct path parent, other_parent;
	struct componentname name, other_name;
	struct inode *created = NULL;
	char pathname[PATH_MAX], other_pathname[PATH_MAX];
	char storage[NAME_MAX + 1U], other_storage[NAME_MAX + 1U];
	int error, other_valid = 0;
	if (process == NULL || process->cwdi == NULL)
		return -EINVAL;
	error = copyinstr(old_address, pathname, sizeof(pathname), NULL);
	if (error != 0)
		goto out_held;
	if (pathname[0] == '/')
		context = process->cwdi;
	else {
		error = syscall_context_at(process, old_dirfd, &temporary,
		    &context, &held);
		if (error != 0)
			goto out_held;
	}
	error = namei_parent_path_at(context, pathname, &parent, &name,
	    storage);
	if (error != 0)
		goto out_held;
	if (number == ZEDBSD_SYS_mkdir) {
		error = vfs_may_create(parent.p_inode, process->cred);
		if (error == 0)
			error = inode_mkdir(parent.p_inode, &name,
			    ((mode_t)option & 07777U) & ~process->umask, &created);
	} else if (number == ZEDBSD_SYS_unlink ||
	    number == ZEDBSD_SYS_rmdir) {
		struct inode *victim;

		error = inode_lookup(parent.p_inode, &name, &victim);
		if (error == 0) {
			error = vfs_may_remove(parent.p_inode, victim,
			    process->cred);
			inode_release(victim);
		}
		if (error == 0)
			error = number == ZEDBSD_SYS_unlink ?
			    inode_unlink(parent.p_inode, &name) :
			    inode_rmdir(parent.p_inode, &name);
	} else {
		struct inode *source = NULL, *target = NULL;

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
		if (error == 0)
			error = inode_lookup(parent.p_inode, &name, &source);
		if (error == 0) {
			int target_error = inode_lookup(other_parent.p_inode,
			    &other_name, &target);
			if (target_error != 0 && target_error != ENOENT)
				error = target_error;
		}
		if (error == 0)
			error = vfs_may_rename(parent.p_inode, source,
			    other_parent.p_inode, target, process->cred);
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
	return error == 0 ? 0 : -error;
}

static intptr_t
sys_mutation_call(uint32_t number, const uintptr_t args[6])
{
	return sys_mutation_common(number, AT_FDCWD, args[0], args[1],
		AT_FDCWD, args[1]);
}

static intptr_t
sys_mutation_at_call(uint32_t number, const uintptr_t args[6])
{
	if (number == ZEDBSD_SYS_mkdirat)
		return sys_mutation_common(ZEDBSD_SYS_mkdir, (int)args[0],
			args[1], args[2], AT_FDCWD, 0);
	if (number == ZEDBSD_SYS_unlinkat) {
		if ((args[2] & ~AT_REMOVEDIR) != 0)
			return -EINVAL;
		return sys_mutation_common((args[2] & AT_REMOVEDIR) != 0 ?
			ZEDBSD_SYS_rmdir : ZEDBSD_SYS_unlink, (int)args[0],
			args[1], 0, AT_FDCWD, 0);
	}
	return sys_mutation_common(ZEDBSD_SYS_rename, (int)args[0], args[1],
		0, (int)args[2], args[3]);
}

static intptr_t
sys_umask_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	mode_t old;
	if (process == NULL)
		return -EINVAL;
	old = process->umask;
	process->umask = (mode_t)args[0] & 0777U;
	return old;
}

static int
replace_cred(struct process *process, struct ucred *replacement)
{
	struct ucred *old;
	if (process == NULL || replacement == NULL || process == &process0)
		return EPERM;
	old = process->cred;
	process->cred = replacement;
	cred_release(old);
	return 0;
}

static intptr_t
sys_cred_get_call(uint32_t number, const uintptr_t args[6])
{
	struct process *process = current_process();
	struct ucred *cred = process != NULL ? process->cred : NULL;
	int error;
	if (cred == NULL)
		return -EINVAL;
	switch (number) {
	case ZEDBSD_SYS_getuid: return cred->ruid;
	case ZEDBSD_SYS_geteuid: return cred->euid;
	case ZEDBSD_SYS_getgid: return cred->rgid;
	case ZEDBSD_SYS_getegid: return cred->egid;
	case ZEDBSD_SYS_getgroups:
		if ((int)args[0] < 0 ||
		    ((unsigned)args[0] != 0 && (unsigned)args[0] < cred->ngroups))
			return -EINVAL;
		if (args[0] != 0) {
			error = copyout(cred->groups, args[1],
			    cred->ngroups * sizeof(cred->groups[0]));
			if (error != 0)
				return -error;
		}
		return cred->ngroups;
	default: return -EINVAL;
	}
}

static int
uid_permitted(const struct ucred *cred, uid_t id)
{
	return cred_is_superuser(cred) || id == cred->ruid || id == cred->euid ||
	    id == cred->suid;
}

static int
gid_permitted(const struct ucred *cred, gid_t id)
{
	return cred_is_superuser(cred) || id == cred->rgid || id == cred->egid ||
	    id == cred->sgid;
}

static intptr_t
sys_cred_set_call(uint32_t number, const uintptr_t args[6])
{
	struct process *process = current_process();
	struct ucred *old = process != NULL ? process->cred : NULL;
	struct ucred *cred;
	uid_t ruid, euid;
	gid_t rgid, egid;
	int error;
	if (old == NULL || process == &process0)
		return -EPERM;
	cred = cred_copy(old);
	if (cred == NULL)
		return -ENOMEM;
	switch (number) {
	case ZEDBSD_SYS_setuid:
		if (!uid_permitted(old, (uid_t)args[0])) { error = EPERM; break; }
		if (cred_is_superuser(old))
			cred->ruid = cred->euid = cred->suid = (uid_t)args[0];
		else
			cred->euid = (uid_t)args[0];
		error = 0;
		break;
	case ZEDBSD_SYS_seteuid:
		if (!uid_permitted(old, (uid_t)args[0])) { error = EPERM; break; }
		cred->euid = (uid_t)args[0]; error = 0; break;
	case ZEDBSD_SYS_setgid:
		if (!gid_permitted(old, (gid_t)args[0])) { error = EPERM; break; }
		if (cred_is_superuser(old))
			cred->rgid = cred->egid = cred->sgid = (gid_t)args[0];
		else
			cred->egid = (gid_t)args[0];
		error = 0;
		break;
	case ZEDBSD_SYS_setegid:
		if (!gid_permitted(old, (gid_t)args[0])) { error = EPERM; break; }
		cred->egid = (gid_t)args[0]; error = 0; break;
	case ZEDBSD_SYS_setreuid:
		ruid = (uid_t)args[0]; euid = (uid_t)args[1];
		if ((ruid != (uid_t)-1 && !uid_permitted(old, ruid)) ||
		    (euid != (uid_t)-1 && !uid_permitted(old, euid))) {
			error = EPERM; break;
		}
		if (ruid != (uid_t)-1) cred->ruid = ruid;
		if (euid != (uid_t)-1) cred->euid = euid;
		error = 0; break;
	case ZEDBSD_SYS_setregid:
		rgid = (gid_t)args[0]; egid = (gid_t)args[1];
		if ((rgid != (gid_t)-1 && !gid_permitted(old, rgid)) ||
		    (egid != (gid_t)-1 && !gid_permitted(old, egid))) {
			error = EPERM; break;
		}
		if (rgid != (gid_t)-1) cred->rgid = rgid;
		if (egid != (gid_t)-1) cred->egid = egid;
		error = 0; break;
	case ZEDBSD_SYS_setgroups:
		if (!cred_is_superuser(old)) { error = EPERM; break; }
		if (args[0] > KERN_NGROUPS_MAX) { error = EINVAL; break; }
		error = args[0] == 0 ? 0 : copyin(args[1], cred->groups,
		    (size_t)args[0] * sizeof(cred->groups[0]));
		if (error == 0) cred->ngroups = (unsigned)args[0];
		break;
	default: error = EINVAL; break;
	}
	if (error == 0)
		error = replace_cred(process, cred);
	else
		cred_release(cred);
	return error == 0 ? 0 : -error;
}

static intptr_t
sys_access_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct path path;
	struct ucred real;
	char pathname[PATH_MAX];
	int error;
	if (process == NULL || process->cred == NULL ||
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
	return error == 0 ? 0 : -error;
}

static SYSCALL_EXT int
sys_resolve_path_at(struct process *process, int dirfd, uintptr_t address,
		    unsigned namei_flags, struct path *path, struct file **held)
{
	struct cwdinfo temporary, *context;
	char pathname[PATH_MAX];
	int error;

	if (process == NULL || process->cwdi == NULL || path == NULL || held == NULL)
		return EINVAL;
	*held = NULL;
	error = copyinstr(address, pathname, sizeof(pathname), NULL);
	if (error != 0)
		return error;
	if (pathname[0] == '/')
		context = process->cwdi;
	else {
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

struct syscall_inode_ref {
	struct inode *inode;
	struct file *file;
	struct file *held;
	struct path path;
	int has_path;
};

static int
sys_inode_ref_acquire(struct process *process, uintptr_t object, int by_fd,
	int nofollow, struct syscall_inode_ref *reference)
{
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
	error = sys_resolve_path_at(process, AT_FDCWD, object,
		nofollow ? NAMEI_NOFOLLOW_FINAL : 0, &reference->path,
		&reference->held);
	if (error == 0) {
		reference->inode = reference->path.p_inode;
		reference->has_path = 1;
	}
	return error;
}

static void
sys_inode_ref_release(struct syscall_inode_ref *reference)
{
	if (reference->has_path)
		path_release(&reference->path);
	if (reference->held != NULL)
		(void)file_close(reference->held);
	if (reference->file != NULL)
		(void)file_close(reference->file);
}

static SYSCALL_EXT intptr_t
sys_getxattr_call(const uintptr_t args[6], int by_fd, int nofollow)
{
	struct process *process = current_process();
	struct syscall_inode_ref reference;
	char name[INODE_XATTR_NAME_MAX + 1U];
	void *value = NULL;
	ssize_t result;
	size_t size = (size_t)args[3];
	int error;
	if (process == NULL || process->cred == NULL ||
	    size > INODE_XATTR_SIZE_MAX || (size != 0 && args[2] == 0))
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
	result = vfs_getxattr(reference.inode, process->cred, name, value, size);
	if (result >= 0 && size != 0 && (size_t)result > size)
		error = EIO;
	else if (result >= 0 && result != 0 && size != 0)
		error = copyout(value, args[2], (size_t)result);
	else
		error = result < 0 ? (int)-result : 0;
	kern_free(value);
	sys_inode_ref_release(&reference);
	return error == 0 ? result : -error;
}

static SYSCALL_EXT intptr_t
sys_setxattr_call(const uintptr_t args[6], int by_fd, int nofollow)
{
	struct process *process = current_process();
	struct syscall_inode_ref reference;
	char name[INODE_XATTR_NAME_MAX + 1U];
	void *value = NULL;
	size_t size = (size_t)args[3];
	int error;
	if (process == NULL || process->cred == NULL ||
	    size > INODE_XATTR_SIZE_MAX || (size != 0 && args[2] == 0))
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
	return error == 0 ? 0 : -error;
}

static SYSCALL_EXT intptr_t
sys_listxattr_call(const uintptr_t args[6], int by_fd, int nofollow)
{
	struct process *process = current_process();
	struct syscall_inode_ref reference;
	void *list = NULL;
	ssize_t result;
	size_t size = (size_t)args[2];
	int error;
	if (process == NULL || process->cred == NULL ||
	    size > INODE_XATTR_SIZE_MAX || (size != 0 && args[1] == 0))
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
	result = vfs_listxattr(reference.inode, process->cred, list, size);
	if (result >= 0 && size != 0 && (size_t)result > size)
		error = EIO;
	else if (result >= 0 && result != 0 && size != 0)
		error = copyout(list, args[1], (size_t)result);
	else
		error = result < 0 ? (int)-result : 0;
	kern_free(list);
	sys_inode_ref_release(&reference);
	return error == 0 ? result : -error;
}

static SYSCALL_EXT intptr_t
sys_removexattr_call(const uintptr_t args[6], int by_fd, int nofollow)
{
	struct process *process = current_process();
	struct syscall_inode_ref reference;
	char name[INODE_XATTR_NAME_MAX + 1U];
	int error;
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
	return error == 0 ? 0 : -error;
}

static int
inode_chmod_allowed(const struct inode *inode, const struct ucred *cred)
{
	return inode != NULL && cred != NULL &&
		(cred_is_superuser(cred) || cred->euid == inode->i_uid);
}

static SYSCALL_EXT intptr_t
sys_chmod_common(int dirfd, uintptr_t pathname, int fd, mode_t mode, int flags)
{
	struct process *process = current_process();
	struct file *file = NULL, *held = NULL;
	struct path path;
	struct inode *inode;
	struct stat status;
	int by_fd = fd >= 0, error;

	if (process == NULL || process->cred == NULL ||
	    (flags & ~AT_SYMLINK_NOFOLLOW) != 0)
		return -EINVAL;
	if (by_fd) {
		file = filedesc_get_ref(process->fd, fd);
		if (file == NULL || file->f_inode == NULL) {
			if (file != NULL) (void)file_close(file);
			return -EBADF;
		}
		inode = file->f_inode;
	} else {
		error = sys_resolve_path_at(process, dirfd, pathname,
			(flags & AT_SYMLINK_NOFOLLOW) != 0 ?
			NAMEI_NOFOLLOW_FINAL : 0, &path, &held);
		if (error != 0)
			return -error;
		inode = path.p_inode;
	}
	if (!inode_chmod_allowed(inode, process->cred))
		error = EPERM;
	else {
		error = inode_getattr(inode, &status);
		if (error == 0) {
			status.st_mode = (status.st_mode & S_IFMT) | (mode & 07777U);
			if (!cred_is_superuser(process->cred) &&
			    !cred_in_group(process->cred, inode->i_gid))
				status.st_mode &= ~(mode_t)S_ISGID;
			error = inode_setattr(inode, &status, INODE_ATTR_MODE);
		}
	}
	if (by_fd)
		(void)file_close(file);
	else {
		path_release(&path);
		if (held != NULL) (void)file_close(held);
	}
	return error == 0 ? 0 : -error;
}

static SYSCALL_EXT intptr_t
sys_chown_common(int dirfd, uintptr_t pathname, int fd, uid_t uid, gid_t gid,
		 int flags)
{
	struct process *process = current_process();
	struct file *file = NULL, *held = NULL;
	struct path path;
	struct inode *inode;
	struct stat status;
	unsigned mask = 0;
	int by_fd = fd >= 0, error;

	if (process == NULL || process->cred == NULL ||
	    (flags & ~AT_SYMLINK_NOFOLLOW) != 0)
		return -EINVAL;
	if (by_fd) {
		file = filedesc_get_ref(process->fd, fd);
		if (file == NULL || file->f_inode == NULL) {
			if (file != NULL) (void)file_close(file);
			return -EBADF;
		}
		inode = file->f_inode;
	} else {
		error = sys_resolve_path_at(process, dirfd, pathname,
			(flags & AT_SYMLINK_NOFOLLOW) != 0 ?
			NAMEI_NOFOLLOW_FINAL : 0, &path, &held);
		if (error != 0)
			return -error;
		inode = path.p_inode;
	}
	error = vfs_may_chown(inode, process->cred, uid, gid);
	if (error == 0) {
		error = inode_getattr(inode, &status);
		if (error == 0 && uid != (uid_t)-1 && uid != status.st_uid) {
			status.st_uid = uid;
			mask |= INODE_ATTR_UID;
		}
		if (error == 0 && gid != (gid_t)-1 && gid != status.st_gid) {
			status.st_gid = gid;
			mask |= INODE_ATTR_GID;
		}
		if (error == 0 && mask != 0 && (status.st_mode &
		    (S_ISUID | S_ISGID)) != 0) {
			status.st_mode &= ~(mode_t)(S_ISUID | S_ISGID);
			mask |= INODE_ATTR_MODE;
		}
		if (error == 0 && mask != 0)
			error = inode_setattr(inode, &status, mask);
	}
	if (by_fd)
		(void)file_close(file);
	else {
		path_release(&path);
		if (held != NULL) (void)file_close(held);
	}
	return error == 0 ? 0 : -error;
}

static int
valid_utime_nsec(long nanoseconds)
{
	return (nanoseconds >= 0 && nanoseconds < 1000000000L) ||
		nanoseconds == UTIME_NOW || nanoseconds == UTIME_OMIT;
}

static SYSCALL_EXT intptr_t
sys_utimens_common(int dirfd, uintptr_t pathname, int fd,
		   uintptr_t times_address, int flags)
{
	struct process *process = current_process();
	struct file *file = NULL, *held = NULL;
	struct path path;
	struct inode *inode;
	struct stat status;
	struct timespec times[2];
	unsigned mask = 0;
	int by_fd = fd >= 0, explicit_time = 0, error;

	if (process == NULL || process->cred == NULL ||
	    (flags & ~AT_SYMLINK_NOFOLLOW) != 0)
		return -EINVAL;
	if (times_address == 0) {
		memset(times, 0, sizeof(times));
		times[0].tv_nsec = times[1].tv_nsec = UTIME_NOW;
	} else {
		error = copyin(times_address, times, sizeof(times));
		if (error != 0)
			return -error;
		if (!valid_utime_nsec(times[0].tv_nsec) ||
		    !valid_utime_nsec(times[1].tv_nsec))
			return -EINVAL;
	}
	if (by_fd) {
		file = filedesc_get_ref(process->fd, fd);
		if (file == NULL || file->f_inode == NULL) {
			if (file != NULL) (void)file_close(file);
			return -EBADF;
		}
		inode = file->f_inode;
	} else {
		error = sys_resolve_path_at(process, dirfd, pathname,
			(flags & AT_SYMLINK_NOFOLLOW) != 0 ?
			NAMEI_NOFOLLOW_FINAL : 0, &path, &held);
		if (error != 0)
			return -error;
		inode = path.p_inode;
	}
	error = inode_getattr(inode, &status);
	if (error == 0 && times[0].tv_nsec != UTIME_OMIT) {
		if (times[0].tv_nsec == UTIME_NOW)
			mask |= INODE_ATTR_ATIME_NOW;
		else {
			status.st_atim = times[0];
			mask |= INODE_ATTR_ATIME;
			explicit_time = 1;
		}
	}
	if (error == 0 && times[1].tv_nsec != UTIME_OMIT) {
		if (times[1].tv_nsec == UTIME_NOW)
			mask |= INODE_ATTR_MTIME_NOW;
		else {
			status.st_mtim = times[1];
			mask |= INODE_ATTR_MTIME;
			explicit_time = 1;
		}
	}
	if (error == 0 && mask != 0 &&
	    !inode_chmod_allowed(inode, process->cred)) {
		if (explicit_time || vfs_access(inode, process->cred, W_OK) != 0)
			error = EACCES;
	}
	if (error == 0 && mask != 0)
		error = inode_setattr(inode, &status, mask);
	if (by_fd)
		(void)file_close(file);
	else {
		path_release(&path);
		if (held != NULL) (void)file_close(held);
	}
	return error == 0 ? 0 : -error;
}

static SYSCALL_EXT intptr_t
sys_faccessat_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct file *held = NULL;
	struct path path;
	struct ucred check;
	int mode = (int)args[2], flags = (int)args[3], error;

	if (process == NULL || process->cred == NULL ||
	    (mode & ~(R_OK | W_OK | X_OK)) != 0 ||
	    (flags & ~(AT_EACCESS | AT_SYMLINK_NOFOLLOW)) != 0)
		return -EINVAL;
	error = sys_resolve_path_at(process, (int)args[0], args[1],
		(flags & AT_SYMLINK_NOFOLLOW) != 0 ? NAMEI_NOFOLLOW_FINAL : 0,
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
	return error == 0 ? 0 : -error;
}

static SYSCALL_EXT int
sys_parent_path_at(struct process *process, int dirfd, const char *pathname,
		   struct cwdinfo *temporary, struct cwdinfo **context,
		   struct file **held, struct path *parent,
		   struct componentname *name, char storage[NAME_MAX + 1U])
{
	int error;
	*held = NULL;
	if (pathname[0] == '/')
		*context = process->cwdi;
	else {
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

static SYSCALL_EXT intptr_t
sys_linkat_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct cwdinfo temporary, *context;
	struct file *old_held = NULL, *new_held = NULL;
	struct path target, parent;
	struct componentname name;
	char new_path[PATH_MAX], storage[NAME_MAX + 1U];
	int flags = (int)args[4], error, parent_valid = 0;

	if (process == NULL || process->cred == NULL ||
	    (flags & ~AT_SYMLINK_FOLLOW) != 0)
		return -EINVAL;
	error = sys_resolve_path_at(process, (int)args[0], args[1],
		(flags & AT_SYMLINK_FOLLOW) != 0 ? 0 : NAMEI_NOFOLLOW_FINAL,
		&target, &old_held);
	if (error != 0)
		return -error;
	error = copyinstr(args[3], new_path, sizeof(new_path), NULL);
	if (error == 0)
		error = sys_parent_path_at(process, (int)args[2], new_path,
			&temporary, &context, &new_held, &parent, &name, storage);
	if (error == 0)
		parent_valid = 1;
	if (error == 0)
		error = vfs_may_create(parent.p_inode, process->cred);
	if (error == 0)
		error = inode_link(parent.p_inode, &name, target.p_inode);
	if (error == 0)
		namecache_remove(parent.p_inode, &name);
	if (parent_valid)
		path_release(&parent);
	if (new_held != NULL)
		(void)file_close(new_held);
	path_release(&target);
	if (old_held != NULL)
		(void)file_close(old_held);
	return error == 0 ? 0 : -error;
}

static SYSCALL_EXT intptr_t
sys_symlinkat_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct cwdinfo temporary, *context;
	struct file *held = NULL;
	struct path parent;
	struct componentname name;
	struct inode *created = NULL;
	char target[PATH_MAX], pathname[PATH_MAX], storage[NAME_MAX + 1U];
	int error, parent_valid = 0;

	if (process == NULL || process->cred == NULL)
		return -EINVAL;
	error = copyinstr(args[0], target, sizeof(target), NULL);
	if (error == 0)
		error = copyinstr(args[2], pathname, sizeof(pathname), NULL);
	if (error == 0) {
		error = sys_parent_path_at(process, (int)args[1], pathname,
			&temporary, &context, &held, &parent, &name, storage);
		if (error == 0)
			parent_valid = 1;
	}
	if (error == 0)
		error = vfs_may_create(parent.p_inode, process->cred);
	if (error == 0)
		error = inode_symlink(parent.p_inode, &name, target, &created);
	if (error == 0)
		namecache_remove(parent.p_inode, &name);
	if (created != NULL)
		inode_release(created);
	if (parent_valid)
		path_release(&parent);
	if (held != NULL)
		(void)file_close(held);
	return error == 0 ? 0 : -error;
}

static SYSCALL_EXT intptr_t
sys_readlinkat_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct file *held = NULL;
	struct path path;
	char buffer[PATH_MAX];
	ssize_t count;
	int error;

	if (process == NULL || args[3] == 0)
		return -EINVAL;
	error = sys_resolve_path_at(process, (int)args[0], args[1],
		NAMEI_NOFOLLOW_FINAL, &path, &held);
	if (error != 0)
		return -error;
	count = inode_readlink(path.p_inode, buffer,
		args[3] < sizeof(buffer) ? (size_t)args[3] : sizeof(buffer));
	if (count >= 0)
		error = copyout(buffer, args[2], (size_t)count);
	else
		error = (int)-count;
	path_release(&path);
	if (held != NULL)
		(void)file_close(held);
	return error == 0 ? count : -error;
}

static intptr_t
sys_sigaction_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	int signo = (int)args[0], error;
	struct sigaction action, old;
	struct signal_action *current;
	if (process == NULL || signo <= 0 || signo >= NSIG)
		return -EINVAL;
	current = &process->signal_actions[signo];
	memset(&old, 0, sizeof(old));
	old.sa_handler = current->handler;
	old.sa_mask = current->mask;
	old.sa_flags = current->flags;
	old.sa_restorer = current->restorer;
	if (args[1] != 0) {
		error = copyin(args[1], &action, sizeof(action));
		if (error != 0)
			return -error;
		if (signo == SIGKILL || signo == SIGSTOP)
			return -EINVAL;
		if ((action.sa_flags & ~(SA_RESTART | SA_NOCLDSTOP |
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
		current->handler = (uintptr_t)action.sa_handler;
		current->mask = action.sa_mask &
		    ~(1U << (SIGKILL-1)) & ~(1U << (SIGSTOP-1));
		current->flags = action.sa_flags;
		current->restorer = (uintptr_t)action.sa_restorer;
	}
	if (args[2] != 0) {
		error = copyout(&old, args[2], sizeof(old));
		if (error != 0)
			return -error;
	}
	return 0;
}

static intptr_t
sys_sigprocmask_call(const uintptr_t args[6])
{
	sigset_t set, old;
	int error;
	if (curthread == NULL)
		return -EINVAL;
	old = curthread->signal_mask;
	if (args[1] != 0) {
		error = copyin(args[1], &set, sizeof(set));
		if (error != 0)
			return -error;
		set &= ~(1U << (SIGKILL-1)) & ~(1U << (SIGSTOP-1));
		if ((int)args[0] == SIG_BLOCK)
			curthread->signal_mask |= set;
		else if ((int)args[0] == SIG_UNBLOCK)
			curthread->signal_mask &= ~set;
		else if ((int)args[0] == SIG_SETMASK)
			curthread->signal_mask = set;
		else
			return -EINVAL;
	}
	if (args[2] != 0) {
		error = copyout(&old, args[2], sizeof(old));
		if (error != 0)
			return -error;
	}
	return 0;
}

static intptr_t
sys_sigpending_call(const uintptr_t args[6])
{
	sigset_t pending;
	int error;
	if (curthread == NULL || curthread->proc == NULL || args[0] == 0)
		return -EINVAL;
	pending = curthread->signal_pending | curthread->proc->signal_pending;
	error = copyout(&pending, args[0], sizeof(pending));
	return error == 0 ? 0 : -error;
}

static SYSCALL_EXT intptr_t
sys_mknodat_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct cwdinfo temporary, *context;
	struct file *held = NULL;
	struct path parent;
	struct componentname name;
	struct inode *created = NULL;
	char pathname[PATH_MAX], storage[NAME_MAX + 1U];
	mode_t mode = (mode_t)args[2];
	int error;
	if (process == NULL || process->cwdi == NULL)
		return -EINVAL;
	if ((mode & S_IFMT) != S_IFIFO || args[3] != 0)
		return -EOPNOTSUPP;
	path_init(&parent);
	error = copyinstr(args[1], pathname, sizeof(pathname), NULL);
	if (error != 0)
		return -error;
	if (pathname[0] == '/')
		context = process->cwdi;
	else {
		error = syscall_context_at(process, (int)args[0], &temporary,
		    &context, &held);
		if (error != 0)
			return -error;
	}
	error = namei_parent_path_at(context, pathname, &parent, &name, storage);
	if (error == 0)
		error = vfs_may_create(parent.p_inode, process->cred);
	if (error == 0)
		error = inode_mknod(parent.p_inode, &name, INODE_FIFO,
		    S_IFIFO | ((mode & 07777U) & ~process->umask), 0, &created);
	if (created != NULL)
		inode_release(created);
	path_release(&parent);
	if (held != NULL)
		(void)file_close(held);
	return error == 0 ? 0 : -error;
}

static intptr_t
sys_fchdir_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct file *file;
	int error;

	if (process == NULL || process->cwdi == NULL || process->fd == NULL)
		return -EINVAL;
	file = filedesc_get_ref(process->fd, (int)args[0]);
	if (file == NULL)
		return -EBADF;
	error = fs_chdir_path(process->cwdi, &file->f_path);
	(void)file_close(file);
	return error == 0 ? 0 : -error;
}

static intptr_t
sys_sigaltstack_call(const uintptr_t args[6])
{
	struct zedbsd_sigaltstack requested, old;
	int error;
	if (curthread == NULL)
		return -EINVAL;
	memset(&old, 0, sizeof(old));
	old.ss_sp = (uapi_ptr_t)curthread->signal_altstack_base;
	old.ss_size = curthread->signal_altstack_size;
	old.ss_flags = (int32_t)curthread->signal_altstack_flags;
	if (curthread->signal_on_altstack_depth != 0)
		old.ss_flags |= SS_ONSTACK;
	if (args[0] != 0) {
		error = copyin(args[0], &requested, sizeof(requested));
		if (error != 0) return -error;
		if (curthread->signal_on_altstack_depth != 0)
			return -EPERM;
		if (requested.ss_flags == SS_DISABLE) {
			curthread->signal_altstack_base = 0;
			curthread->signal_altstack_size = 0;
			curthread->signal_altstack_flags = SS_DISABLE;
		} else {
			if (requested.ss_flags != 0 || requested.ss_size < MINSIGSTKSZ ||
			    requested.ss_size > SIZE_MAX ||
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
		if (error != 0) return -error;
	}
	return 0;
}

static intptr_t
sys_sigtimedwait_call(const uintptr_t args[6])
{
	sigset_t set;
	struct timespec timeout, *timeout_pointer = NULL;
	struct signal_info selected;
	siginfo_t info;
	int signo, error;
	if (curthread == NULL || args[0] == 0)
		return -EINVAL;
	error = copyin(args[0], &set, sizeof(set));
	if (error != 0) return -error;
	if (args[2] != 0) {
		error = copyin(args[2], &timeout, sizeof(timeout));
		if (error != 0) return -error;
		timeout_pointer = &timeout;
	}
	memset(&selected, 0, sizeof(selected));
	error = signal_timedwait(curthread, set, timeout_pointer, &selected,
	    &signo);
	if (error != 0) return -error;
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
		if (error != 0) return -error;
	}
	return signo;
}

static intptr_t
sys_sigqueue_call(const uintptr_t args[6])
{
	struct process *sender = current_process();
	struct process *target;
	struct signal_info info;
	pid_t pid = (pid_t)args[0];
	int signo = (int)args[1], error;
	if (sender == NULL || pid <= 0 || signo <= 0 || signo >= NSIG)
		return -EINVAL;
	error = signal_kill(sender, pid, 0);
	if (error != 0) return -error;
	target = process_find_ref(pid);
	if (target == NULL) return -ESRCH;
	memset(&info, 0, sizeof(info));
	info.code = SI_QUEUE;
	info.pid = sender->pid;
	info.uid = sender->cred != NULL ? sender->cred->euid : 0;
	info.value = (uint64_t)args[2];
	error = signal_send_process_info(target, signo, &info);
	process_release(target);
	return error == 0 ? 0 : -error;
}

static intptr_t
sys_thread_create_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct thread *thread;
	struct uaccess_pin pin;
	tid_t tid;
	int error;
	if (process == NULL || process->vmspace == NULL || args[0] == 0 ||
	    args[1] == 0 || args[4] != 0 || args[5] == 0)
		return -EINVAL;
	memset(&pin, 0, sizeof(pin));
	error = uaccess_pin(args[5], sizeof(tid), HAL_SPACE_WRITE, &pin);
	if (error != 0) return -error;
	error = thread_create(process, args[0], args[1], &thread);
	if (error == 0) {
		hal_task_set_tls(thread->task, args[3]);
		tid = thread->tid;
		error = copyout_pinned(&pin, 0, &tid, sizeof(tid));
		if (error == 0) thread_start(thread);
		else { thread->detached = 1; thread_start(thread); }
	}
	uaccess_unpin(&pin);
	return error == 0 ? 0 : -error;
}

static intptr_t
sys_thread_exit_call(const uintptr_t args[6])
{
	if (curthread == NULL || curthread->proc == &process0)
		return -EINVAL;
	curthread->user_exit_value = args[0];
	thread_exit(0);
}

static intptr_t
sys_thread_join_call(const uintptr_t args[6])
{
	struct thread *target;
	struct process *process = current_process();
	uintptr_t value;
	unsigned long irq;
	int error;
	if (process == NULL || (tid_t)args[0] == curthread->tid)
		return -EDEADLK;
	target = thread_find_ref((tid_t)args[0]);
	if (target == NULL || target->proc != process) {
		if (target != NULL) thread_release(target);
		return -ESRCH;
	}
	irq = spin_lock_irqsave(&process->lock);
	if (target->detached || target->join_claimed) error = EINVAL;
	else { target->join_claimed = 1; error = 0; }
	while (error == 0 && target->state != THREAD_ZOMBIE) {
		uint64_t sequence = waitq_sequence(&target->join_waitq);
		error = waitq_sleep(&target->join_waitq, &process->lock,
		    sequence, 0, WAITQ_INTERRUPTIBLE);
		if (error == EAGAIN)
			error = 0;
	}
	spin_unlock_irqrestore(&process->lock, irq);
	if (error != 0 && error != EINTR) {
		thread_release(target);
		return -error;
	}
	value = target->user_exit_value;
	if (error == 0) error = thread_wait(target, NULL);
	if (error == 0 && args[1] != 0)
		error = copyout(&value, args[1], sizeof(value));
	if (error != 0 && target->state != THREAD_DEAD) {
		irq = spin_lock_irqsave(&process->lock);
		target->join_claimed = 0;
		spin_unlock_irqrestore(&process->lock, irq);
	}
	thread_release(target);
	return error == 0 ? 0 : -error;
}

static intptr_t
sys_thread_detach_call(const uintptr_t args[6])
{
	struct thread *target = thread_find_ref((tid_t)args[0]);
	struct process *process = current_process();
	unsigned long irq;
	int error = 0, reap = 0;
	if (target == NULL || process == NULL || target->proc != process) {
		if (target != NULL) thread_release(target);
		return -ESRCH;
	}
	irq = spin_lock_irqsave(&process->lock);
	if (target->detached || target->join_claimed) error = EINVAL;
	else { target->detached = 1; reap = target->state == THREAD_ZOMBIE; }
	spin_unlock_irqrestore(&process->lock, irq);
	if (error == 0 && reap) error = thread_wait(target, NULL);
	thread_release(target);
	return error == 0 ? 0 : -error;
}

static intptr_t
sys_thread_self_call(const uintptr_t args[6])
{
	if (curthread == NULL || args[2] != 0 || args[3] != 0 ||
	    args[4] != 0 || args[5] != 0)
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

static intptr_t
sys_thread_kill_call(const uintptr_t args[6])
{
	struct thread *target = thread_find_ref((tid_t)args[0]);
	struct process *process = current_process();
	int error;
	if (target == NULL || process == NULL || target->proc != process) {
		if (target != NULL) thread_release(target);
		return -ESRCH;
	}
	error = args[1] == 0 ? 0 : signal_send_thread(target, (int)args[1]);
	thread_release(target);
	return error == 0 ? 0 : -error;
}

static intptr_t
sys_thread_cancel_call(const uintptr_t args[6])
{
	struct thread *current = curthread;
	struct thread *target;
	unsigned operation = (unsigned)args[1];
	unsigned long irq;
	int pending;
	if (current == NULL || current->proc == NULL || args[2] != 0 ||
	    args[3] != 0 || args[4] != 0 || args[5] != 0 ||
	    operation > ZEDBSD_THREAD_CANCEL_CLEAR)
		return -EINVAL;
	if (operation == ZEDBSD_THREAD_CANCEL_REQUEST) {
		target = thread_find_ref((tid_t)args[0]);
		if (target == NULL || target->proc != current->proc) {
			if (target != NULL)
				thread_release(target);
			return -ESRCH;
		}
		irq = spin_lock_irqsave(&target->proc->lock);
		target->cancel_pending = 1;
		spin_unlock_irqrestore(&target->proc->lock, irq);
		sched_wakeup(target);
		thread_release(target);
		return 0;
	}
	if (args[0] != 0 && (tid_t)args[0] != current->tid)
		return -EINVAL;
	irq = spin_lock_irqsave(&current->proc->lock);
	pending = current->cancel_pending != 0;
	if (operation == ZEDBSD_THREAD_CANCEL_CLEAR)
		current->cancel_pending = 0;
	spin_unlock_irqrestore(&current->proc->lock, irq);
	return pending;
}

static intptr_t
sys_usync_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct vm_object *shared_object = NULL;
	struct vm_region *region;
	struct timespec timeout;
	uintptr_t key_object, key_offset;
	uint64_t ticks, deadline = 0;
	int error;

	if (process == NULL || (args[5] & ~ZEDBSD_USYNC_PRIVATE) != 0)
		return -EINVAL;
	if ((args[5] & ZEDBSD_USYNC_PRIVATE) != 0) {
		key_object = (uintptr_t)process;
		key_offset = args[0];
	} else {
		region = vmspace_find_region(process->vmspace, args[0],
		    sizeof(uint32_t));
		if (region == NULL || (region->flags & VM_REGION_SHARED) == 0 ||
		    region->object == NULL)
			return -EINVAL;
		shared_object = region->object;
		vm_object_ref(shared_object);
		key_object = (uintptr_t)shared_object;
		key_offset = (uintptr_t)region->file_offset +
		    args[0] - region->start;
	}
	if ((unsigned)args[1] == ZEDBSD_USYNC_WAIT) {
		if (args[4] != 0) {
			error = EINVAL;
			goto out;
		}
		if (args[3] != 0) {
			error = copyin(args[3], &timeout, sizeof(timeout));
			if (error == 0)
				error = kern_duration_to_ticks_ceil(&timeout, &ticks);
			if (error == 0)
				error = kern_deadline_after(sched_ticks(), ticks, &deadline);
			if (error != 0)
				goto out;
		}
		error = usync_wait(args[0], (uint32_t)args[2],
		    key_object, key_offset, deadline);
		goto out;
	}
	if ((unsigned)args[1] == ZEDBSD_USYNC_WAKE) {
		if (args[2] != 0 || args[3] != 0)
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
	return error == 0 ? 0 : -error;
}

static intptr_t
sys_sigsuspend_call(const uintptr_t args[6])
{
	sigset_t mask;
	int error;
	if (curthread == NULL || curthread->proc == NULL || args[0] == 0)
		return -EINVAL;
	error = copyin(args[0], &mask, sizeof(mask));
	if (error != 0)
		return -error;
	mask &= ~(1U << (SIGKILL - 1)) & ~(1U << (SIGSTOP - 1));
	curthread->signal_suspend_mask = curthread->signal_mask;
	curthread->signal_mask = mask;
	curthread->signal_suspended = 1;
	while (!signal_pending_unblocked(curthread))
		sched_sleep(0);
	/* The user-return hook restores the original mask before entering the
	 * selected handler.  Its sigreturn therefore resumes this call at EINTR. */
	return -EINTR;
}

static intptr_t
sys_sigreturn_call(const uintptr_t args[6])
{
	intptr_t restored;
	struct thread_signal_level *level;
	ucontext_t context;
	sigset_t restored_mask;
	int error;

	if (curthread == NULL || args[0] == 0 || args[1] == 0 ||
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
	/* The first signal ABI exposes machine state for diagnosis but does not
	 * yet permit userland to replace it.  Only the signal mask is mutable. */
	restored_mask = context.uc_sigmask & ~(1U << (SIGKILL - 1)) &
	    ~(1U << (SIGSTOP - 1));
	context.uc_sigmask = level->saved_ucontext.uc_sigmask;
	if (memcmp(&context, &level->saved_ucontext, sizeof(context)) != 0)
		return -EINVAL;
	if ((level->restart_on_return ? hal_task_signal_restart(
	    level->token, level->restart_number, level->restart_args, &restored) :
	    hal_task_signal_return((uint32_t)args[0], &restored)) != 0)
		return -EINVAL;
	if (level->used_altstack && curthread->signal_on_altstack_depth != 0)
		curthread->signal_on_altstack_depth--;
	curthread->signal_mask = restored_mask;
	memset(level, 0, sizeof(*level));
	curthread->signal_depth--;
	curthread->signal_token = curthread->signal_depth == 0 ? 0 :
	    curthread->signal_levels[curthread->signal_depth - 1U].token;
	curthread->syscall_restart_on_return = 0;
	curthread->syscall_restart_valid = 0;
	return restored;
}

static intptr_t
sys_dup_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	int result, error;
	if (process == NULL || process->fd == NULL)
		return -EBADF;
	error = filedesc_dup(process->fd, (int)args[0], 0, 0, &result);
	return error == 0 ? result : -error;
}

static intptr_t
sys_dup2_call(const uintptr_t args[6], int is_dup3)
{
	struct process *process = current_process();
	unsigned flags = 0;
	int error;
	if (process == NULL || process->fd == NULL)
		return -EBADF;
	if (is_dup3) {
		if (((int)args[2] & ~O_CLOEXEC) != 0)
			return -EINVAL;
		if (((int)args[2] & O_CLOEXEC) != 0)
			flags = FILEDESC_CLOEXEC;
	}
	error = filedesc_dup2(process->fd, (int)args[0], (int)args[1],
	    flags, is_dup3);
	return error == 0 ? (intptr_t)(int)args[1] : -error;
}

static intptr_t
sys_fcntl_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct file *file;
	unsigned flags;
	int result, error, command = (int)args[1];
	if (process == NULL || process->fd == NULL)
		return -EBADF;
	switch (command) {
	case F_DUPFD:
	case F_DUPFD_CLOEXEC:
		error = filedesc_dup(process->fd, (int)args[0], (int)args[2],
		    command == F_DUPFD_CLOEXEC ? FILEDESC_CLOEXEC : 0, &result);
		return error == 0 ? result : -error;
	case F_GETFD:
		error = filedesc_get_flags(process->fd, (int)args[0], &flags);
		return error == 0 ?
		    ((flags & FILEDESC_CLOEXEC) != 0 ? FD_CLOEXEC : 0) : -error;
	case F_SETFD:
		if (((int)args[2] & ~FD_CLOEXEC) != 0)
			return -EINVAL;
		error = filedesc_set_flags(process->fd, (int)args[0],
		    ((int)args[2] & FD_CLOEXEC) != 0 ? FILEDESC_CLOEXEC : 0);
		return error == 0 ? 0 : -error;
	case F_GETFL:
		file = filedesc_get_ref(process->fd, (int)args[0]);
		if (file == NULL)
			return -EBADF;
		result = file->f_flags;
		(void)file_close(file);
		return result;
	case F_SETFL:
		file = filedesc_get_ref(process->fd, (int)args[0]);
		if (file == NULL)
			return -EBADF;
		if (((int)args[2] & ~(O_APPEND | O_NONBLOCK)) != 0) {
			(void)file_close(file);
			return -EINVAL;
		}
		file->f_flags = (file->f_flags & ~(O_APPEND | O_NONBLOCK)) |
		    ((int)args[2] & (O_APPEND | O_NONBLOCK));
		(void)file_close(file);
		return 0;
	case F_GETLK:
	case F_SETLK:
	case F_SETLKW: {
		struct zedbsd_flock_request request;
		error = copyin(args[2], &request, sizeof(request));
		if (error != 0)
			return -error;
		file = filedesc_get_ref(process->fd, (int)args[0]);
		if (file == NULL)
			return -EBADF;
		error = record_lock_fcntl(process, file, command, &request);
		(void)file_close(file);
		if (error == 0 && command == F_GETLK)
			error = copyout(&request, args[2], sizeof(request));
		return error == 0 ? 0 : -error;
	}
	default:
		return -EINVAL;
	}
}

static intptr_t
sys_pipe2_call(const uintptr_t args[6], int plain)
{
	struct process *process = current_process();
	struct file *read_file, *write_file;
	int descriptors[2] = { -1, -1 };
	int flags = plain ? 0 : (int)args[1];
	unsigned fdflags;
	int error;

	if (process == NULL || process->fd == NULL)
		return -EBADF;
	error = pipe_create(flags, &read_file, &write_file);
	if (error != 0)
		return -error;
	fdflags = (flags & O_CLOEXEC) != 0 ? FILEDESC_CLOEXEC : 0;
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

static intptr_t
sys_fork_call(const uintptr_t args[6])
{
	struct process *parent = current_process();
	struct process *child;
	int error;
	(void)args;
	error = process_fork(parent, &child);
	return error == 0 ? child->pid : -error;
}

static intptr_t
sys_execve_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct syscall_exec_args *copy;
	char path[PATH_MAX];
	int error;
	if (process == NULL || args[3] != 0 || args[4] != 0 || args[5] != 0)
		return -EINVAL;
	error = copyinstr(args[0], path, sizeof(path), NULL);
	if (error != 0)
		return -error;
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
	return error == 0 ? 0 : -error;
}

static SYSCALL_EXT intptr_t
sys_waitpid_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct process_wait_event event;
	struct uaccess_pin pin;
	int status = 0;
	pid_t result;
	int error = 0;
	if (args[3] != 0 || args[4] != 0 || args[5] != 0)
		return -EINVAL;
	if (args[1] != 0) {
		error = uaccess_pin(args[1], sizeof(status), HAL_SPACE_WRITE, &pin);
		if (error != 0)
			return -error;
	} else
		memset(&pin, 0, sizeof(pin));
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
	status = event.status;
	error = copyout_pinned(&pin, 0, &status, sizeof(status));
	if (error == 0)
		error = process_wait_commit(&event);
	if (error != 0)
		process_wait_abort(&event);
	uaccess_unpin(&pin);
	return error == 0 ? result : -error;
}

static SYSCALL_EXT intptr_t
sys_waitid_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct process_wait_event event;
	siginfo_t information;
	idtype_t type = (idtype_t)args[0];
	id_t id = (id_t)args[1];
	int options = (int)args[3];
	pid_t selector, result;
	unsigned mask = 0;
	int error;

	if (process == NULL || args[2] == 0 || args[4] != 0 || args[5] != 0 ||
	    (options & ~(WEXITED | WSTOPPED | WCONTINUED | WNOHANG |
	    WNOWAIT)) != 0)
		return -EINVAL;
	if ((options & WEXITED) != 0) mask |= PROCESS_WAIT_EVENT_EXITED;
	if ((options & WSTOPPED) != 0) mask |= PROCESS_WAIT_EVENT_STOPPED;
	if ((options & WCONTINUED) != 0) mask |= PROCESS_WAIT_EVENT_CONTINUED;
	if (mask == 0)
		return -EINVAL;
	if (type == P_ALL)
		selector = -1;
	else if (type == P_PID && id > 0 && id <= INT32_MAX)
		selector = (pid_t)id;
	else if (type == P_PGID && id <= INT32_MAX)
		selector = id == 0 ? 0 : -(pid_t)id;
	else
		return -EINVAL;
	result = process_wait_select_mask(process, selector,
	    options & WNOHANG, mask, &event);
	if (result < 0)
		return result;
	memset(&information, 0, sizeof(information));
	if (result == 0)
		return copyout(&information, args[2], sizeof(information)) == 0 ?
		    0 : -EFAULT;
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
	error = copyout(&information, args[2], sizeof(information));
	if (error != 0 || (options & WNOWAIT) != 0)
		process_wait_abort(&event);
	else {
		error = process_wait_commit(&event);
		if (error != 0)
			process_wait_abort(&event);
	}
	return error == 0 ? 0 : -error;
}

static SYSCALL_EXT intptr_t
sys_resource_limit_call(const uintptr_t args[6], int setting)
{
	struct process *process = current_process();
	struct zedbsd_rlimit limit;
	int error;
	if (process == NULL || args[2] != 0 || args[3] != 0 || args[4] != 0 ||
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
	return error == 0 ? 0 : -error;
}

static intptr_t
sys_process_identity_call(uint32_t number, const uintptr_t args[6])
{
	struct process *process = current_process();
	struct process *target;
	pid_t pid;
	int error;
	if (process == NULL)
		return -EINVAL;
	switch (number) {
	case ZEDBSD_SYS_getpid: return process->pid;
	case ZEDBSD_SYS_getppid:
		return process->parent != NULL ? process->parent->pid : 0;
	case ZEDBSD_SYS_getpgrp: return process->pgrp;
	case ZEDBSD_SYS_getpgid:
		pid = (pid_t)args[0];
		if (pid == 0)
			return process->pgrp;
		target = process_find_ref(pid);
		if (target != NULL) {
			pid_t value = target->pgrp;
			process_release(target);
			return value;
		}
		return -ESRCH;
	case ZEDBSD_SYS_setpgid:
		error = process_setpgid(process, (pid_t)args[0],
		    (pid_t)args[1]);
		return error == 0 ? 0 : -error;
	case ZEDBSD_SYS_setsid:
		return process_setsid(process);
	case ZEDBSD_SYS_getsid:
		pid = (pid_t)args[0];
		if (pid == 0)
			return process->session;
		target = process_find_ref(pid);
		if (target != NULL) {
			pid_t value = target->session;
			process_release(target);
			return value;
		}
		return -ESRCH;
	default:
		return -ENOSYS;
	}
}

static intptr_t
sys_spawn_call(const uintptr_t args[6])
{
	struct process *parent = current_process();
	struct process *child;
	struct syscall_exec_args *copy;
	char path[PATH_MAX];
	int error;
	if (parent == NULL || args[4] != 0 || args[5] != 0 ||
	    (args[3] & ~ZEDBSD_SPAWN_RESULT) != 0)
		return -EINVAL;
	error = copyinstr(args[0], path, sizeof(path), NULL);
	if (error != 0)
		return -error;
	copy = kern_calloc(1, sizeof(*copy));
	if (copy == NULL)
		return -ENOMEM;
	error = copy_exec_vector(args[1], copy->argv, ZEDBSD_SPAWN_ARG_MAX,
				 copy, 0);
	if (error == 0)
		error = copy_exec_vector(args[2], copy->envp, ZEDBSD_SPAWN_ENV_MAX,
					 copy, 1);
	if (error == 0)
		error = process_spawn_from(parent, path, copy->argv, copy->envp,
			(args[3] & ZEDBSD_SPAWN_RESULT) ? PROCESS_SPAWN_RESULT : 0,
			&child);
	kern_free(copy);
	return error == 0 ? child->pid : -error;
}

static SYSCALL_EXT intptr_t
sys_wait_call(const uintptr_t args[6])
{
	struct process *parent = current_process();
	struct process *child;
	struct uaccess_pin status_pin, result_pin;
	char result[PROCESS_RESULT_MAX];
	size_t capacity;
	pid_t pid = (pid_t)args[0];
	int status, error;
	if (parent == NULL || pid <= 0 || args[2] != 0 || args[5] != 0 ||
	    (args[3] == 0) != (args[4] == 0))
		return -EINVAL;
	child = process_find_ref(pid);
	if (child == NULL)
		return -ECHILD;
	if (child->parent != parent) {
		process_release(child);
		return -ECHILD;
	}
	capacity = args[4] > sizeof(result) ? sizeof(result) : (size_t)args[4];
	memset(&status_pin, 0, sizeof(status_pin));
	memset(&result_pin, 0, sizeof(result_pin));
	if (args[1] != 0) {
		error = uaccess_pin(args[1], sizeof(status), HAL_SPACE_WRITE,
		    &status_pin);
		if (error != 0)
		{
			process_release(child);
			return -error;
		}
	}
	if (capacity != 0) {
		error = uaccess_pin(args[3], capacity, HAL_SPACE_WRITE, &result_pin);
		if (error != 0) {
			uaccess_unpin(&status_pin);
			process_release(child);
			return -error;
		}
	}
	memset(result, 0, sizeof(result));
	error = process_wait(child, &status, capacity != 0 ? result : NULL,
			     capacity);
	if (error != 0) {
		uaccess_unpin(&result_pin);
		uaccess_unpin(&status_pin);
		process_release(child);
		return -error;
	}
	if (args[1] != 0)
		error = copyout_pinned(&status_pin, 0, &status, sizeof(status));
	if (error == 0 && capacity != 0)
		error = copyout_pinned(&result_pin, 0, result, capacity);
	uaccess_unpin(&result_pin);
	uaccess_unpin(&status_pin);
	process_release(child);
	if (error != 0)
		return -error;
	return pid;
}

static intptr_t
syscall_dispatch_body(uint32_t number, const uintptr_t args[6])
{
	switch (number) {
	case ZEDBSD_SYS_exit: exit1((int)args[0]);
	case ZEDBSD_SYS_open: return sys_open_call(args, 0);
	case ZEDBSD_SYS_openat: return sys_open_call(args, 1);
	case ZEDBSD_SYS_close: return sys_close_call(args);
	case ZEDBSD_SYS_read: return sys_read_call(args);
	case ZEDBSD_SYS_write: return sys_write_call(args);
	case ZEDBSD_SYS_lseek: return sys_lseek_call(args);
	case ZEDBSD_SYS_fstat: return sys_fstat_call(args);
	case ZEDBSD_SYS_getdents: return sys_getdents_call(args);
	case ZEDBSD_SYS_chdir: return sys_chdir_call(args);
	case ZEDBSD_SYS_fchdir: return sys_fchdir_call(args);
	case ZEDBSD_SYS_mknodat: return sys_mknodat_call(args);
	case ZEDBSD_SYS_getcwd: return sys_getcwd_call(args);
	case ZEDBSD_SYS_mmap: return sys_mmap_call(args);
	case ZEDBSD_SYS_munmap: return sys_munmap_call(args);
	case ZEDBSD_SYS_mprotect: return sys_mprotect_call(args);
	case ZEDBSD_SYS_ioctl: return sys_ioctl_call(args);
	case ZEDBSD_SYS_sysctl: return sys_sysctl_call(args);
	case ZEDBSD_SYS_ppoll: return sys_ppoll_call(args);
	case ZEDBSD_SYS_pselect: return sys_pselect_call(args);
	case ZEDBSD_SYS_sigaltstack: return sys_sigaltstack_call(args);
	case ZEDBSD_SYS_sigtimedwait: return sys_sigtimedwait_call(args);
	case ZEDBSD_SYS_sigqueue: return sys_sigqueue_call(args);
	case ZEDBSD_SYS_thread_create: return sys_thread_create_call(args);
	case ZEDBSD_SYS_thread_exit: return sys_thread_exit_call(args);
	case ZEDBSD_SYS_thread_join: return sys_thread_join_call(args);
	case ZEDBSD_SYS_thread_detach: return sys_thread_detach_call(args);
	case ZEDBSD_SYS_thread_self: return sys_thread_self_call(args);
	case ZEDBSD_SYS_thread_kill: return sys_thread_kill_call(args);
	case ZEDBSD_SYS_thread_cancel: return sys_thread_cancel_call(args);
	case ZEDBSD_SYS_usync: return sys_usync_call(args);
	case ZEDBSD_SYS_clock_gettime: return sys_clock_gettime_call(args);
	case ZEDBSD_SYS_clock_getres: return sys_clock_getres_call(args);
	case ZEDBSD_SYS_clock_settime: return sys_clock_settime_call(args);
	case ZEDBSD_SYS_timer_create: return sys_timer_create_call(args);
	case ZEDBSD_SYS_timer_delete: return sys_timer_delete_call(args);
	case ZEDBSD_SYS_timer_settime: return sys_timer_settime_call(args);
	case ZEDBSD_SYS_timer_gettime: return sys_timer_gettime_call(args);
	case ZEDBSD_SYS_timer_getoverrun: return sys_timer_getoverrun_call(args);
	case ZEDBSD_SYS_mount: return sys_mount_call(args);
	case ZEDBSD_SYS_unmount: return sys_unmount_call(args);
	case ZEDBSD_SYS_statvfs: return sys_statvfs_call(args, 0);
	case ZEDBSD_SYS_fstatvfs: return sys_statvfs_call(args, 1);
	case ZEDBSD_SYS_getxattr: return sys_getxattr_call(args, 0, 0);
	case ZEDBSD_SYS_lgetxattr: return sys_getxattr_call(args, 0, 1);
	case ZEDBSD_SYS_fgetxattr: return sys_getxattr_call(args, 1, 0);
	case ZEDBSD_SYS_setxattr: return sys_setxattr_call(args, 0, 0);
	case ZEDBSD_SYS_lsetxattr: return sys_setxattr_call(args, 0, 1);
	case ZEDBSD_SYS_fsetxattr: return sys_setxattr_call(args, 1, 0);
	case ZEDBSD_SYS_listxattr: return sys_listxattr_call(args, 0, 0);
	case ZEDBSD_SYS_llistxattr: return sys_listxattr_call(args, 0, 1);
	case ZEDBSD_SYS_flistxattr: return sys_listxattr_call(args, 1, 0);
	case ZEDBSD_SYS_removexattr: return sys_removexattr_call(args, 0, 0);
	case ZEDBSD_SYS_lremovexattr: return sys_removexattr_call(args, 0, 1);
	case ZEDBSD_SYS_fremovexattr: return sys_removexattr_call(args, 1, 0);
	case ZEDBSD_SYS_quotactl: return sys_quotactl_call(args);
	case ZEDBSD_SYS_snapshotctl: return sys_snapshotctl_call(args);
	case ZEDBSD_SYS_nanosleep: return sys_nanosleep_call(args);
	case ZEDBSD_SYS_spawn: return sys_spawn_call(args);
	case ZEDBSD_SYS_wait: return sys_wait_call(args);
	case ZEDBSD_SYS_brk: return sys_brk_call(args);
	case ZEDBSD_SYS_socket: return sys_socket_call(args);
	case ZEDBSD_SYS_socketpair: return sys_socketpair_call(args);
	case ZEDBSD_SYS_sendmsg: return sys_sendmsg_call(args);
	case ZEDBSD_SYS_recvmsg: return sys_recvmsg_call(args);
	case ZEDBSD_SYS_bind: return sys_bind_call(args);
	case ZEDBSD_SYS_connect: return sys_connect_call(args);
	case ZEDBSD_SYS_listen: return sys_listen_call(args);
	case ZEDBSD_SYS_accept: return sys_accept_call(args);
	case ZEDBSD_SYS_sendto: return sys_sendto_call(args);
	case ZEDBSD_SYS_recvfrom: return sys_recvfrom_call(args);
	case ZEDBSD_SYS_shutdown: return sys_shutdown_call(args);
	case ZEDBSD_SYS_getsockname: return sys_socket_name_call(args, 0);
	case ZEDBSD_SYS_getpeername: return sys_socket_name_call(args, 1);
	case ZEDBSD_SYS_setsockopt: return sys_setsockopt_call(args);
	case ZEDBSD_SYS_getsockopt: return sys_getsockopt_call(args);
	case ZEDBSD_SYS_fork: return sys_fork_call(args);
	case ZEDBSD_SYS_execve: return sys_execve_call(args);
	case ZEDBSD_SYS_waitpid: return sys_waitpid_call(args);
	case ZEDBSD_SYS_waitid: return sys_waitid_call(args);
	case ZEDBSD_SYS_getrlimit: return sys_resource_limit_call(args, 0);
	case ZEDBSD_SYS_setrlimit: return sys_resource_limit_call(args, 1);
	case ZEDBSD_SYS_getpid:
	case ZEDBSD_SYS_getppid:
	case ZEDBSD_SYS_getpgrp:
	case ZEDBSD_SYS_getpgid:
	case ZEDBSD_SYS_setpgid:
	case ZEDBSD_SYS_setsid:
	case ZEDBSD_SYS_getsid:
		return sys_process_identity_call(number, args);
	case ZEDBSD_SYS_dup: return sys_dup_call(args);
	case ZEDBSD_SYS_dup2: return sys_dup2_call(args, 0);
	case ZEDBSD_SYS_dup3: return sys_dup2_call(args, 1);
	case ZEDBSD_SYS_fcntl: return sys_fcntl_call(args);
	case ZEDBSD_SYS_pipe: return sys_pipe2_call(args, 1);
	case ZEDBSD_SYS_pipe2: return sys_pipe2_call(args, 0);
	case ZEDBSD_SYS_pread: return sys_positional_call(args, 0);
	case ZEDBSD_SYS_pwrite: return sys_positional_call(args, 1);
	case ZEDBSD_SYS_readv: return sys_vector_call(args, 0);
	case ZEDBSD_SYS_writev: return sys_vector_call(args, 1);
	case ZEDBSD_SYS_fsync:
	case ZEDBSD_SYS_fdatasync: return sys_fsync_call(args);
	case ZEDBSD_SYS_stat: return sys_stat_path_call(args, 0, 0);
	case ZEDBSD_SYS_lstat: return sys_stat_path_call(args, 0, 1);
	case ZEDBSD_SYS_fstatat: return sys_stat_path_call(args, 1, 0);
	case ZEDBSD_SYS_truncate: return sys_truncate_call(args, 0);
	case ZEDBSD_SYS_ftruncate: return sys_truncate_call(args, 1);
	case ZEDBSD_SYS_mkdir:
	case ZEDBSD_SYS_unlink:
	case ZEDBSD_SYS_rmdir:
	case ZEDBSD_SYS_rename: return sys_mutation_call(number, args);
	case ZEDBSD_SYS_mkdirat:
	case ZEDBSD_SYS_unlinkat:
	case ZEDBSD_SYS_renameat: return sys_mutation_at_call(number, args);
	case ZEDBSD_SYS_umask: return sys_umask_call(args);
	case ZEDBSD_SYS_getuid:
	case ZEDBSD_SYS_geteuid:
	case ZEDBSD_SYS_getgid:
	case ZEDBSD_SYS_getegid:
	case ZEDBSD_SYS_getgroups: return sys_cred_get_call(number, args);
	case ZEDBSD_SYS_setuid:
	case ZEDBSD_SYS_seteuid:
	case ZEDBSD_SYS_setgid:
	case ZEDBSD_SYS_setegid:
	case ZEDBSD_SYS_setgroups:
	case ZEDBSD_SYS_setreuid:
	case ZEDBSD_SYS_setregid: return sys_cred_set_call(number, args);
	case ZEDBSD_SYS_access: return sys_access_call(args);
	case ZEDBSD_SYS_sigaction: return sys_sigaction_call(args);
	case ZEDBSD_SYS_sigprocmask: return sys_sigprocmask_call(args);
	case ZEDBSD_SYS_sigpending: return sys_sigpending_call(args);
	case ZEDBSD_SYS_kill: {
		int error = signal_kill(current_process(), (pid_t)args[0],
		    (int)args[1]);
		return error == 0 ? 0 : -error;
	}
	case ZEDBSD_SYS_sigreturn: return sys_sigreturn_call(args);
	case ZEDBSD_SYS_msync: return sys_msync_call(args);
	case ZEDBSD_SYS_chmod:
		return sys_chmod_common(AT_FDCWD, args[0], -1,
			(mode_t)args[1], 0);
	case ZEDBSD_SYS_fchmod:
		return sys_chmod_common(AT_FDCWD, 0, (int)args[0],
			(mode_t)args[1], 0);
	case ZEDBSD_SYS_fchmodat:
		return sys_chmod_common((int)args[0], args[1], -1,
			(mode_t)args[2], (int)args[3]);
	case ZEDBSD_SYS_chown:
	case ZEDBSD_SYS_lchown:
		return sys_chown_common(AT_FDCWD, args[0], -1,
			(uid_t)args[1], (gid_t)args[2],
			number == ZEDBSD_SYS_lchown ? AT_SYMLINK_NOFOLLOW : 0);
	case ZEDBSD_SYS_fchown:
		return sys_chown_common(AT_FDCWD, 0, (int)args[0],
			(uid_t)args[1], (gid_t)args[2], 0);
	case ZEDBSD_SYS_fchownat:
		return sys_chown_common((int)args[0], args[1], -1,
			(uid_t)args[2], (gid_t)args[3], (int)args[4]);
	case ZEDBSD_SYS_utimensat:
		return sys_utimens_common((int)args[0], args[1], -1,
			args[2], (int)args[3]);
	case ZEDBSD_SYS_futimens:
		return sys_utimens_common(AT_FDCWD, 0, (int)args[0], args[1], 0);
	case ZEDBSD_SYS_faccessat: return sys_faccessat_call(args);
	case ZEDBSD_SYS_linkat: return sys_linkat_call(args);
	case ZEDBSD_SYS_symlinkat: return sys_symlinkat_call(args);
	case ZEDBSD_SYS_readlinkat: return sys_readlinkat_call(args);
	case ZEDBSD_SYS_sigsuspend: return sys_sigsuspend_call(args);
	default: return -ENOSYS;
	}
}

static int
syscall_restartable(uint32_t number)
{
	switch (number) {
	case ZEDBSD_SYS_read:
	case ZEDBSD_SYS_write:
	case ZEDBSD_SYS_readv:
	case ZEDBSD_SYS_writev:
	case ZEDBSD_SYS_waitpid:
	case ZEDBSD_SYS_waitid:
	case ZEDBSD_SYS_accept:
	case ZEDBSD_SYS_recvfrom:
	case ZEDBSD_SYS_socketpair:
		return 1;
	default:
		return 0;
	}
}

static intptr_t
syscall_dispatch(uint32_t number, const uintptr_t args[6])
{
	struct thread *thread = curthread;
	intptr_t result;

	if (thread != NULL && number != ZEDBSD_SYS_sigreturn) {
		thread->syscall_restart_number = number;
		memcpy(thread->syscall_restart_args, args,
		    sizeof(thread->syscall_restart_args));
		thread->syscall_restart_valid = 0;
		thread->syscall_restart_on_return = 0;
	}
	result = syscall_dispatch_body(number, args);
	if (thread != NULL && number != ZEDBSD_SYS_sigreturn)
		thread->syscall_restart_valid = result == -EINTR &&
		    syscall_restartable(number);
	return result;
}

void syscall_init(void)
{
	poll_init();
	usync_init();
	hal_syscall_set_handler(syscall_dispatch);
	signal_init();
}
