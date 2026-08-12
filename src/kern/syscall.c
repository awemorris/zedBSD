/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/syscall.h"
#include "kern/file.h"
#include "kern/filedesc.h"
#include "kern/inode.h"
#include "kern/exec.h"
#include "kern/kmem.h"
#include "kern/namei.h"
#include "kern/process.h"
#include "kern/sched.h"
#include "kern/thread.h"
#include "kern/uaccess.h"
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
#include <sys/stat.h>
#include <time.h>

#define SYSCALL_IO_CHUNK 512U
#define CLOCK_HZ 100U

static struct process *current_process(void)
{
	return curthread != NULL ? curthread->proc : NULL;
}

static intptr_t sys_open_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct file *file;
	char path[PATH_MAX];
	int descriptor, error;
	if (process == NULL || process->fd == NULL || process->cwdi == NULL)
		return -EINVAL;
	error = copyinstr(args[0], path, sizeof(path), NULL);
	if (error != 0) return -error;
	error = file_openat(process->cwdi, path, (int)args[1], (mode_t)args[2], &file);
	if (error != 0) return -error;
	error = filedesc_install(process->fd, file, &descriptor);
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
	uintptr_t mapped;
	uint32_t prot;
	int error;
	if (process == NULL || process->vmspace == NULL) return -EINVAL;
	if ((args[3] & MAP_FIXED) != 0)
		return -EINVAL;
	if (args[3] != (MAP_PRIVATE | MAP_ANONYMOUS) &&
	    args[3] != (MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE))
		return -EOPNOTSUPP;
	if ((int)args[4] != -1 || args[5] != 0)
		return -EOPNOTSUPP;
	if ((args[3] & MAP_FIXED_NOREPLACE) != 0 && args[0] == 0)
		return -EINVAL;
	if (args[1] == 0 || args[1] > SIZE_MAX - 4095U)
		return -EINVAL;
	if (args[0] != 0 && (args[0] & 4095U) != 0)
		return -EINVAL;
	error = vm_prot((int)args[2], &prot);
	if (error == 0 && (args[3] & MAP_FIXED_NOREPLACE) != 0) {
		size_t size = (args[1] + 4095U) & ~4095U;
		error = vmspace_map_anon_fixed_noreplace(process->vmspace,
			args[0], size, prot, NULL);
		mapped = args[0];
	} else if (error == 0) {
		error = vmspace_map_find(process->vmspace, args[0], args[1], prot,
			&mapped);
	}
	return error == 0 ? (intptr_t)mapped : -error;
}

static intptr_t sys_munmap_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	int error = process == NULL ? EINVAL :
		vmspace_unmap(process->vmspace, args[0], args[1]);
	return error == 0 ? 0 : -error;
}

static intptr_t sys_mprotect_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	uint32_t prot;
	int error = vm_prot((int)args[2], &prot);
	if (error == 0)
		error = process == NULL ? EINVAL :
			vmspace_protect(process->vmspace, args[0], args[1], prot);
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
	if ((clockid_t)args[0] != CLOCK_MONOTONIC) return -EINVAL;
	ticks = sched_ticks();
	time.tv_sec = (time_t)(ticks / CLOCK_HZ);
	time.tv_nsec = (long)((ticks % CLOCK_HZ) * (1000000000UL / CLOCK_HZ));
	error = copyout(&time, args[1], sizeof(time));
	return error == 0 ? 0 : -error;
}

static intptr_t sys_nanosleep_call(const uintptr_t args[6])
{
	struct timespec request;
	uint64_t ticks;
	int error = copyin(args[0], &request, sizeof(request));
	(void)args[1];
	if (error != 0) return -error;
	if (request.tv_sec < 0 || request.tv_nsec < 0 || request.tv_nsec >= 1000000000L)
		return -EINVAL;
	ticks = (uint64_t)request.tv_sec * CLOCK_HZ +
		((uint64_t)request.tv_nsec * CLOCK_HZ + 999999999ULL) / 1000000000ULL;
	if (ticks == 0) return 0;
	sched_sleep(sched_ticks() + ticks);
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
		uint32_t pointer;
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
	case ZEDBSD_SYS_open: return sys_open_call(args);
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
	default: return -ENOSYS;
	}
}

void syscall_init(void)
{
	hal_syscall_set_handler(syscall_dispatch);
}
