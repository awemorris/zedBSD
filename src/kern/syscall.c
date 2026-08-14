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
#include "kern/pipe.h"
#include "kern/sched.h"
#include "kern/signal.h"
#include "kern/thread.h"
#include "kern/uaccess.h"
#include "kern/vm-object.h"
#include "kern/vmspace.h"

#include <zedbsd/dirent.h>
#include <zedbsd/syscall.h>
#include <zedbsd/process.h>
#include <errno.h>
#include <fcntl.h>
#include <hal/hal.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SYSCALL_IO_CHUNK 512U
#define SYSCALL_SOCKET_OPTION_MAX 128U
#define CLOCK_HZ 100U
#define SYSCALL_EXT __attribute__((section(".hightext")))

static struct process *current_process(void)
{
	return curthread != NULL ? curthread->proc : NULL;
}

static struct socket *
descriptor_socket(struct process *process, int descriptor)
{
	struct file *file = process != NULL && process->fd != NULL ?
		filedesc_get(process->fd, descriptor) : NULL;

	return socket_from_file(file);
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
sys_bind_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct socket *socket = descriptor_socket(process, (int)args[0]);
	struct sockaddr_storage address;
	int error;

	if (socket == NULL)
		return -EBADF;
	if (socket->ops == NULL || socket->ops->bind == NULL)
		return -EOPNOTSUPP;
	error = copy_sockaddr_in(args[1], (socklen_t)args[2], &address);
	if (error == 0)
		error = socket->ops->bind(socket, (struct sockaddr *)&address,
		    (socklen_t)args[2]);
	return error == 0 ? 0 : -error;
}

static intptr_t
sys_connect_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct socket *socket = descriptor_socket(process, (int)args[0]);
	struct sockaddr_storage address;
	int error;

	if (socket == NULL)
		return -EBADF;
	if (socket->ops == NULL || socket->ops->connect == NULL)
		return -EOPNOTSUPP;
	error = copy_sockaddr_in(args[1], (socklen_t)args[2], &address);
	if (error == 0)
		error = socket->ops->connect(socket, (struct sockaddr *)&address,
		    (socklen_t)args[2]);
	return error == 0 ? 0 : -error;
}

static intptr_t
sys_listen_call(const uintptr_t args[6])
{
	struct socket *socket = descriptor_socket(current_process(),
	    (int)args[0]);
	int error;

	if (socket == NULL)
		return -EBADF;
	if (socket->ops == NULL || socket->ops->listen == NULL)
		return -EOPNOTSUPP;
	error = socket->ops->listen(socket, (int)args[1]);
	return error == 0 ? 0 : -error;
}

static intptr_t
sys_accept_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct socket *socket = descriptor_socket(process, (int)args[0]);
	struct sockaddr_storage address;
	struct socket *accepted = NULL;
	struct file *file = NULL;
	socklen_t length = sizeof(address);
	int descriptor, error;

	if (socket == NULL)
		return -EBADF;
	if ((args[1] == 0) != (args[2] == 0))
		return -EINVAL;
	if (socket->ops == NULL || socket->ops->accept == NULL)
		return -EOPNOTSUPP;
	memset(&address, 0, sizeof(address));
	error = socket->ops->accept(socket, &accepted,
	    args[1] != 0 ? (struct sockaddr *)&address : NULL,
	    args[1] != 0 ? &length : NULL);
	if (error != 0)
		return -error;
	if (accepted == NULL)
		return -EIO;
	error = socket_file_create(accepted, &file);
	if (error == 0)
		error = filedesc_install(process->fd, file, &descriptor);
	if (error != 0) {
		if (file != NULL)
			(void)file_close(file);
		else
			socket_release(accepted);
		return -error;
	}
	error = copy_sockaddr_out(args[1], args[2], &address, length);
	if (error != 0) {
		(void)filedesc_close(process->fd, descriptor);
		return -error;
	}
	return descriptor;
}

static intptr_t
sys_sendto_call(const uintptr_t args[6])
{
	struct socket *socket = descriptor_socket(current_process(),
	    (int)args[0]);
	struct sockaddr_storage address;
	const struct sockaddr *destination = NULL;
	void *buffer;
	ssize_t result;
	int error;

	if (socket == NULL)
		return -EBADF;
	if (socket->ops == NULL || socket->ops->sendto == NULL)
		return -EOPNOTSUPP;
	if ((args[4] == 0) != (args[5] == 0))
		return -EINVAL;
	if (args[2] > PACKET_BUF_STORAGE_SIZE)
		return -EMSGSIZE;
	if (args[4] != 0) {
		error = copy_sockaddr_in(args[4], (socklen_t)args[5], &address);
		if (error != 0)
			return -error;
		destination = (const struct sockaddr *)&address;
	}
	if (args[2] == 0)
		return socket->ops->sendto(socket, "", 0, (int)args[3],
		    destination, (socklen_t)args[5]);
	buffer = kern_malloc((size_t)args[2]);
	if (buffer == NULL)
		return -ENOMEM;
	error = copyin(args[1], buffer, (size_t)args[2]);
	result = error == 0 ? socket->ops->sendto(socket, buffer,
	    (size_t)args[2], (int)args[3], destination,
	    (socklen_t)args[5]) : -error;
	kern_free(buffer);
	return result;
}

static intptr_t
sys_recvfrom_call(const uintptr_t args[6])
{
	struct socket *socket = descriptor_socket(current_process(),
	    (int)args[0]);
	struct sockaddr_storage address;
	socklen_t length = sizeof(address);
	size_t capacity;
	void *buffer;
	ssize_t result;
	int error;

	if (socket == NULL)
		return -EBADF;
	if (socket->ops == NULL || socket->ops->recvfrom == NULL)
		return -EOPNOTSUPP;
	if ((args[4] == 0) != (args[5] == 0))
		return -EINVAL;
	if (args[2] == 0)
		return 0;
	capacity = args[2] > PACKET_BUF_STORAGE_SIZE ?
		PACKET_BUF_STORAGE_SIZE : (size_t)args[2];
	buffer = kern_malloc(capacity);
	if (buffer == NULL)
		return -ENOMEM;
	memset(&address, 0, sizeof(address));
	result = socket->ops->recvfrom(socket, buffer, capacity, (int)args[3],
	    args[4] != 0 ? (struct sockaddr *)&address : NULL,
	    args[4] != 0 ? &length : NULL);
	if (result >= 0) {
		error = copyout(buffer, args[1], (size_t)result);
		if (error == 0)
			error = copy_sockaddr_out(args[4], args[5], &address, length);
		if (error != 0)
			result = -error;
	}
	kern_free(buffer);
	return result;
}

static intptr_t
sys_shutdown_call(const uintptr_t args[6])
{
	struct socket *socket = descriptor_socket(current_process(),
	    (int)args[0]);
	int error;

	if (socket == NULL)
		return -EBADF;
	if (socket->ops == NULL || socket->ops->shutdown == NULL)
		return -EOPNOTSUPP;
	error = socket->ops->shutdown(socket, (int)args[1]);
	return error == 0 ? 0 : -error;
}

static intptr_t
sys_socket_name_call(const uintptr_t args[6], int peer)
{
	struct socket *socket = descriptor_socket(current_process(),
	    (int)args[0]);
	struct sockaddr_storage address;
	socklen_t length = sizeof(address);
	int error;

	if (socket == NULL)
		return -EBADF;
	if (args[1] == 0 || args[2] == 0)
		return -EINVAL;
	memset(&address, 0, sizeof(address));
	if (peer) {
		if (socket->ops == NULL || socket->ops->getpeername == NULL)
			return -EOPNOTSUPP;
		error = socket->ops->getpeername(socket,
		    (struct sockaddr *)&address, &length);
	} else {
		if (socket->ops == NULL || socket->ops->getsockname == NULL)
			return -EOPNOTSUPP;
		error = socket->ops->getsockname(socket,
		    (struct sockaddr *)&address, &length);
	}
	if (error == 0)
		error = copy_sockaddr_out(args[1], args[2], &address, length);
	return error == 0 ? 0 : -error;
}

static intptr_t
sys_setsockopt_call(const uintptr_t args[6])
{
	struct socket *socket = descriptor_socket(current_process(),
	    (int)args[0]);
	uint8_t value[SYSCALL_SOCKET_OPTION_MAX];
	int error;

	if (socket == NULL)
		return -EBADF;
	if (args[4] > sizeof(value) || (args[4] != 0 && args[3] == 0))
		return -EINVAL;
	error = args[4] == 0 ? 0 : copyin(args[3], value, (size_t)args[4]);
	if (error == 0)
		error = socket_setsockopt_common(socket, (int)args[1],
		    (int)args[2], value, (socklen_t)args[4]);
	if (error == EOPNOTSUPP && socket->ops != NULL &&
	    socket->ops->setsockopt != NULL)
		error = socket->ops->setsockopt(socket, (int)args[1],
		    (int)args[2], value, (socklen_t)args[4]);
	return error == 0 ? 0 : -error;
}

static intptr_t
sys_getsockopt_call(const uintptr_t args[6])
{
	struct socket *socket = descriptor_socket(current_process(),
	    (int)args[0]);
	uint8_t value[SYSCALL_SOCKET_OPTION_MAX];
	socklen_t length;
	int error;

	if (socket == NULL)
		return -EBADF;
	if (args[3] == 0 || args[4] == 0)
		return -EINVAL;
	error = copyin(args[4], &length, sizeof(length));
	if (error != 0)
		return -error;
	if (length > sizeof(value))
		length = sizeof(value);
	error = socket_getsockopt_common(socket, (int)args[1], (int)args[2],
	    value, &length);
	if (error == EOPNOTSUPP && socket->ops != NULL &&
	    socket->ops->getsockopt != NULL)
		error = socket->ops->getsockopt(socket, (int)args[1],
		    (int)args[2], value, &length);
	if (error == 0)
		error = copyout(value, args[3], length);
	if (error == 0)
		error = copyout(&length, args[4], sizeof(length));
	return error == 0 ? 0 : -error;
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

static intptr_t sys_read_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct file *file;
	uint8_t buffer[SYSCALL_IO_CHUNK];
	size_t done = 0, length = (size_t)args[2];
	if (process == NULL || (file = filedesc_get(process->fd, (int)args[0])) == NULL)
		return -EBADF;
	while (done < length) {
		size_t chunk = length - done > sizeof(buffer) ? sizeof(buffer) : length - done;
		ssize_t count = file_read(file, buffer, chunk);
		int error;
		if (count < 0) return done != 0 ? (intptr_t)done : count;
		if (count == 0) break;
		error = copyout(buffer, args[1] + done, (size_t)count);
		if (error != 0) return done != 0 ? (intptr_t)done : -error;
		done += (size_t)count;
		if ((size_t)count < chunk) break;
	}
	return (intptr_t)done;
}

static intptr_t sys_write_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct file *file;
	uint8_t buffer[SYSCALL_IO_CHUNK];
	size_t done = 0, length = (size_t)args[2];
	if (process == NULL || (file = filedesc_get(process->fd, (int)args[0])) == NULL)
		return -EBADF;
	while (done < length) {
		size_t chunk = length - done > sizeof(buffer) ? sizeof(buffer) : length - done;
		ssize_t count;
		int error = copyin(args[1] + done, buffer, chunk);
		if (error != 0) return done != 0 ? (intptr_t)done : -error;
		count = file_write(file, buffer, chunk);
		if (count < 0) return done != 0 ? (intptr_t)done : count;
		done += (size_t)count;
		if ((size_t)count < chunk) break;
	}
	return (intptr_t)done;
}

static intptr_t sys_lseek_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct file *file = process != NULL ? filedesc_get(process->fd, (int)args[0]) : NULL;
	return file == NULL ? -EBADF : file_seek(file, (off_t)args[1], (int)args[2]);
}

static intptr_t sys_fstat_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct file *file = process != NULL ? filedesc_get(process->fd, (int)args[0]) : NULL;
	struct stat status;
	int error;
	if (file == NULL) return -EBADF;
	if (file->f_inode == NULL) return -EINVAL;
	error = inode_getattr(file->f_inode, &status);
	if (error == 0) error = copyout(&status, args[1], sizeof(status));
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
	struct file *file = process != NULL ? filedesc_get(process->fd, (int)args[0]) : NULL;
	struct zedbsd_dirent output;
	struct dirent entry;
	int eof, error;
	if (file == NULL) return -EBADF;
	if (args[2] < sizeof(output)) return -EINVAL;
	error = file_readdir(file, &entry, &eof);
	if (error != 0) return -error;
	if (eof) return 0;
	memset(&output, 0, sizeof(output));
	output.d_ino = entry.d_ino;
	output.d_type = dirent_type(entry.d_type);
	strncpy(output.d_name, entry.d_name, sizeof(output.d_name) - 1U);
	error = copyout(&output, args[1], sizeof(output));
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
	const char *path;
	size_t length;
	int error;
	if (process == NULL || process->cwdi == NULL) return -EINVAL;
	path = fs_getcwd(process->cwdi);
	if (path == NULL) return -ENOENT;
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
	if (args[1] == 0 || args[1] > SIZE_MAX - 4095U)
		return -EINVAL;
	if (args[0] != 0 && (args[0] & 4095U) != 0)
		return -EINVAL;
	if ((args[3] & MAP_FIXED_NOREPLACE) != 0 && args[0] == 0)
		return -EINVAL;
	if ((args[3] & MAP_ANONYMOUS) != 0) {
		if ((int)args[4] != -1 || args[5] != 0)
			return -EINVAL;
		if (shared)
			return -EOPNOTSUPP;
	} else {
		if ((args[5] & 4095U) != 0 || (off_t)args[5] < 0)
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
		    (args[1] + 4095U) & ~4095U, prot, file,
		    (off_t)args[5], data_size, NULL);
		mapped = args[0];
	} else if (error == 0 && file != NULL &&
	    (args[3] & MAP_FIXED_NOREPLACE) != 0) {
		error = vmspace_map_file(process->vmspace, args[0],
		    (args[1] + 4095U) & ~4095U, prot, file,
		    (off_t)args[5], args[0], data_size, NULL);
		mapped = args[0];
	} else if (error == 0 &&
	    (args[3] & MAP_FIXED_NOREPLACE) != 0) {
		size_t size = (args[1] + 4095U) & ~4095U;
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
	if (args[1] == 0 || args[1] > SIZE_MAX - 4095U)
		return -EINVAL;
	size = (args[1] + 4095U) & ~4095U;
	error = process == NULL ? EINVAL :
		vmspace_unmap(process->vmspace, args[0], size);
	return error == 0 ? 0 : -error;
}

static intptr_t sys_mprotect_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	uint32_t prot;
	int error = vm_prot((int)args[2], &prot);
	if (error == 0 && (args[1] == 0 || args[1] > SIZE_MAX - 4095U))
		error = EINVAL;
	if (error == 0)
		error = process == NULL ? EINVAL :
			vmspace_protect(process->vmspace, args[0],
			    (args[1] + 4095U) & ~4095U, prot);
	return error == 0 ? 0 : -error;
}

static intptr_t sys_msync_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	size_t size;
	int error;
	if (args[1] == 0 || args[1] > SIZE_MAX - 4095U)
		return -EINVAL;
	size = (args[1] + 4095U) & ~4095U;
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
	struct file *file = process != NULL ? filedesc_get(process->fd, (int)args[0]) : NULL;
	int error = file == NULL ? EBADF : file_ioctl(file, args[1], args[2]);
	return error == 0 ? 0 : -error;
}

static intptr_t sys_clock_gettime_call(const uintptr_t args[6])
{
	struct timespec time;
	uint64_t ticks;
	int error;
	if ((clockid_t)args[0] == CLOCK_MONOTONIC) {
		ticks = sched_ticks();
		time.tv_sec = (time_t)(ticks / CLOCK_HZ);
		time.tv_nsec = (long)((ticks % CLOCK_HZ) *
			(1000000000UL / CLOCK_HZ));
	} else if ((clockid_t)args[0] == CLOCK_REALTIME) {
		zedbsd_clock_realtime(&time.tv_sec, &time.tv_nsec);
	} else {
		return -EINVAL;
	}
	error = copyout(&time, args[1], sizeof(time));
	return error == 0 ? 0 : -error;
}

static intptr_t sys_nanosleep_call(const uintptr_t args[6])
{
	struct timespec request;
	struct timespec remaining;
	uint64_t ticks, deadline, left;
	int error = copyin(args[0], &request, sizeof(request));
	if (error != 0) return -error;
	if (request.tv_sec < 0 || request.tv_nsec < 0 || request.tv_nsec >= 1000000000L)
		return -EINVAL;
	ticks = (uint64_t)request.tv_sec * CLOCK_HZ +
		((uint64_t)request.tv_nsec * CLOCK_HZ + 999999999ULL) / 1000000000ULL;
	if (ticks == 0) return 0;
	if (signal_pending_unblocked(curthread))
		return -EINTR;
	deadline = sched_ticks() + ticks;
	sched_sleep(deadline);
	if (signal_pending_unblocked(curthread) && sched_ticks() < deadline) {
		left = deadline - sched_ticks();
		if (args[1] != 0) {
			remaining.tv_sec = (time_t)(left / CLOCK_HZ);
			remaining.tv_nsec = (long)((left % CLOCK_HZ) *
			    (1000000000UL / CLOCK_HZ));
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
#if defined(ZEDBSD_USER_ABI_AARCH64) || defined(ZEDBSD_USER_ABI_SPARCV9)
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

static intptr_t
sys_positional_call(const uintptr_t args[6], int writing)
{
	struct process *process = current_process();
	struct file *file;
	uint8_t buffer[SYSCALL_IO_CHUNK];
	size_t done = 0, length = (size_t)args[2];
	off_t offset = (off_t)args[3];
	if (process == NULL ||
	    (file = filedesc_get(process->fd, (int)args[0])) == NULL)
		return -EBADF;
	if (offset < 0)
		return -EINVAL;
	while (done < length) {
		size_t chunk = length - done > sizeof(buffer) ?
		    sizeof(buffer) : length - done;
		ssize_t count;
		int error;
		if (writing) {
			error = copyin(args[1] + done, buffer, chunk);
			if (error != 0)
				return done != 0 ? (intptr_t)done : -error;
			count = file_pwrite(file, buffer, chunk,
			    offset + (off_t)done);
		} else {
			count = file_pread(file, buffer, chunk,
			    offset + (off_t)done);
		}
		if (count < 0)
			return done != 0 ? (intptr_t)done : count;
		if (count == 0)
			break;
		if (!writing) {
			error = copyout(buffer, args[1] + done, (size_t)count);
			if (error != 0)
				return done != 0 ? (intptr_t)done : -error;
		}
		done += (size_t)count;
		if ((size_t)count < chunk)
			break;
	}
	return (intptr_t)done;
}

#ifdef ZEDBSD_USER_ABI_AARCH64
struct syscall_iovec { uint64_t base, length; };
#else
struct syscall_iovec { uint32_t base, length; };
#endif

static intptr_t
sys_vector_call(const uintptr_t args[6], int writing)
{
	int count = (int)args[2], i;
	intptr_t total = 0;
	if (count < 0 || count > 16)
		return -EINVAL;
	for (i = 0; i < count; i++) {
		struct syscall_iovec vector;
		uintptr_t scalar[6] = { args[0], 0, 0, 0, 0, 0 };
		intptr_t result;
		int error = copyin(args[1] + (uintptr_t)i * sizeof(vector),
		    &vector, sizeof(vector));
		if (error != 0)
			return total != 0 ? total : -error;
		if (vector.length > (uint64_t)INT32_MAX - (uint64_t)total)
			return total != 0 ? total : -EINVAL;
		scalar[1] = (uintptr_t)vector.base;
		scalar[2] = (uintptr_t)vector.length;
		result = writing ? sys_write_call(scalar) : sys_read_call(scalar);
		if (result < 0)
			return total != 0 ? total : result;
		total += result;
		if ((uintptr_t)result < (uintptr_t)vector.length)
			break;
	}
	return total;
}

static intptr_t
sys_fsync_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct file *file = process != NULL ?
	    filedesc_get(process->fd, (int)args[0]) : NULL;
	int error = file == NULL ? EBADF :
	    vm_object_sync_inode(file->f_inode);
	if (error == 0 && file != NULL)
		error = file_fsync(file);
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
		file = filedesc_get(process->fd, (int)args[0]);
		if (file == NULL)
			return -EBADF;
		if ((file->f_flags & O_ACCMODE) == O_RDONLY)
			return -EBADF;
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
		vm_object_truncate_inode(inode, length);
	if (!by_fd)
		path_release(&path);
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
	error = vfs_access(parent.p_inode, process->cred, W_OK | X_OK);
	if (error != 0) {
		path_release(&parent);
		goto out_held;
	}
	if (number == ZEDBSD_SYS_mkdir)
		error = inode_mkdir(parent.p_inode, &name,
		    ((mode_t)option & 07777U) & ~process->umask, &created);
	else if (number == ZEDBSD_SYS_unlink)
		error = inode_unlink(parent.p_inode, &name);
	else if (number == ZEDBSD_SYS_rmdir)
		error = inode_rmdir(parent.p_inode, &name);
	else {
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
			error = vfs_access(other_parent.p_inode, process->cred,
			    W_OK | X_OK);
		if (error == 0) {
			error = inode_rename(parent.p_inode, &name,
			    other_parent.p_inode, &other_name, 0);
			if (error == 0)
				namecache_remove(other_parent.p_inode, &other_name);
		}
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

static int
inode_chown_allowed(const struct inode *inode, const struct ucred *cred,
		    uid_t uid, gid_t gid)
{
	if (inode == NULL || cred == NULL)
		return 0;
	if (cred_is_superuser(cred))
		return 1;
	if (cred->euid != inode->i_uid)
		return 0;
	if (uid != (uid_t)-1 && uid != inode->i_uid)
		return 0;
	return gid == (gid_t)-1 || gid == inode->i_gid ||
		cred_in_group(cred, gid);
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
	if (!inode_chown_allowed(inode, process->cred, uid, gid))
		error = EPERM;
	else {
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
valid_utime_nsec(int32_t nanoseconds)
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
		error = vfs_access(parent.p_inode, process->cred, W_OK | X_OK);
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
		error = vfs_access(parent.p_inode, process->cred, W_OK | X_OK);
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
		if ((action.sa_flags & ~SA_RESTART) != 0 ||
		    (action.sa_handler > 1U && action.sa_handler >= VM_USER_TOP) ||
		    (action.sa_handler > 1U &&
		    (action.sa_restorer == 0 || action.sa_restorer >= VM_USER_TOP)))
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
	if (curthread == NULL || args[0] == 0 ||
	    (uint32_t)args[0] != curthread->signal_token ||
	    hal_task_signal_return((uint32_t)args[0], &restored) != 0)
		return -EINVAL;
	curthread->signal_mask = curthread->signal_saved_mask;
	curthread->signal_token = 0;
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
		file = filedesc_get(process->fd, (int)args[0]);
		return file != NULL ? file->f_flags : -EBADF;
	case F_SETFL:
		file = filedesc_get(process->fd, (int)args[0]);
		if (file == NULL)
			return -EBADF;
		if (((int)args[2] & ~(O_APPEND | O_NONBLOCK)) != 0)
			return -EINVAL;
		file->f_flags = (file->f_flags & ~(O_APPEND | O_NONBLOCK)) |
		    ((int)args[2] & (O_APPEND | O_NONBLOCK));
		return 0;
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

static intptr_t
sys_waitpid_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	int status = 0;
	pid_t result;
	int error;
	if (args[3] != 0 || args[4] != 0 || args[5] != 0)
		return -EINVAL;
	result = process_waitpid(process, (pid_t)args[0], &status,
	    (int)args[2]);
	if (result <= 0 || args[1] == 0)
		return result;
	error = copyout(&status, args[1], sizeof(status));
	return error == 0 ? result : -error;
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
		target = pid == 0 ? process : process_find(pid);
		return target != NULL ? target->pgrp : -ESRCH;
	case ZEDBSD_SYS_setpgid:
		error = process_setpgid(process, (pid_t)args[0],
		    (pid_t)args[1]);
		return error == 0 ? 0 : -error;
	case ZEDBSD_SYS_setsid:
		return process_setsid(process);
	case ZEDBSD_SYS_getsid:
		pid = (pid_t)args[0];
		target = pid == 0 ? process : process_find(pid);
		return target != NULL ? target->session : -ESRCH;
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

static intptr_t
sys_wait_call(const uintptr_t args[6])
{
	struct process *parent = current_process();
	struct process *child;
	char result[PROCESS_RESULT_MAX];
	size_t capacity;
	pid_t pid = (pid_t)args[0];
	int status, error;
	if (parent == NULL || pid <= 0 || args[2] != 0 || args[5] != 0 ||
	    (args[3] == 0) != (args[4] == 0))
		return -EINVAL;
	child = process_find(pid);
	if (child == NULL || child->parent != parent)
		return -ECHILD;
	capacity = args[4] > sizeof(result) ? sizeof(result) : (size_t)args[4];
	memset(result, 0, sizeof(result));
	error = process_wait(child, &status, capacity != 0 ? result : NULL,
			     capacity);
	if (error != 0)
		return -error;
	if (args[1] != 0 && (error = copyout(&status, args[1], sizeof(status))) != 0)
		return -error;
	if (capacity != 0 && (error = copyout(result, args[3], capacity)) != 0)
		return -error;
	return pid;
}

static intptr_t syscall_dispatch(uint32_t number, const uintptr_t args[6])
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
	case ZEDBSD_SYS_getcwd: return sys_getcwd_call(args);
	case ZEDBSD_SYS_mmap: return sys_mmap_call(args);
	case ZEDBSD_SYS_munmap: return sys_munmap_call(args);
	case ZEDBSD_SYS_mprotect: return sys_mprotect_call(args);
	case ZEDBSD_SYS_ioctl: return sys_ioctl_call(args);
	case ZEDBSD_SYS_clock_gettime: return sys_clock_gettime_call(args);
	case ZEDBSD_SYS_nanosleep: return sys_nanosleep_call(args);
	case ZEDBSD_SYS_spawn: return sys_spawn_call(args);
	case ZEDBSD_SYS_wait: return sys_wait_call(args);
	case ZEDBSD_SYS_brk: return sys_brk_call(args);
	case ZEDBSD_SYS_socket: return sys_socket_call(args);
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

void syscall_init(void)
{
	hal_syscall_set_handler(syscall_dispatch);
	signal_init();
}
