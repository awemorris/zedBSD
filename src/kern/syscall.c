/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/syscall.h"
#include "kern/file.h"
#include "kern/filedesc.h"
#include "kern/inode.h"
#include "kern/namei.h"
#include "kern/process.h"
#include "kern/sched.h"
#include "kern/thread.h"
#include "kern/uaccess.h"
#include "kern/vmspace.h"

#include <boots/dirent.h>
#include <boots/syscall.h>
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
	case INODE_REG: return BOOTS_DT_REG;
	case INODE_DIR: return BOOTS_DT_DIR;
	case INODE_BLOCK: return BOOTS_DT_BLK;
	case INODE_CHAR: return BOOTS_DT_CHR;
	default: return BOOTS_DT_UNKNOWN;
	}
}

static intptr_t sys_getdents_call(const uintptr_t args[6])
{
	struct process *process = current_process();
	struct file *file = process != NULL ? filedesc_get(process->fd, (int)args[0]) : NULL;
	struct boots_dirent output;
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
	if (args[3] != (MAP_PRIVATE | MAP_ANONYMOUS) ||
	    (int)args[4] != -1 || args[5] != 0)
		return -EOPNOTSUPP;
	if (args[0] != 0 && (args[0] & 4095U) != 0)
		return -EINVAL;
	error = vm_prot((int)args[2], &prot);
	if (error == 0)
		error = vmspace_map_find(process->vmspace, args[0], args[1], prot, &mapped);
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

static intptr_t syscall_dispatch(uint32_t number, const uintptr_t args[6])
{
	switch (number) {
	case BOOTS_SYS_exit: exit1((int)args[0]);
	case BOOTS_SYS_open: return sys_open_call(args);
	case BOOTS_SYS_close: return sys_close_call(args);
	case BOOTS_SYS_read: return sys_read_call(args);
	case BOOTS_SYS_write: return sys_write_call(args);
	case BOOTS_SYS_lseek: return sys_lseek_call(args);
	case BOOTS_SYS_fstat: return sys_fstat_call(args);
	case BOOTS_SYS_getdents: return sys_getdents_call(args);
	case BOOTS_SYS_chdir: return sys_chdir_call(args);
	case BOOTS_SYS_getcwd: return sys_getcwd_call(args);
	case BOOTS_SYS_mmap: return sys_mmap_call(args);
	case BOOTS_SYS_munmap: return sys_munmap_call(args);
	case BOOTS_SYS_mprotect: return sys_mprotect_call(args);
	case BOOTS_SYS_ioctl: return sys_ioctl_call(args);
	case BOOTS_SYS_clock_gettime: return sys_clock_gettime_call(args);
	case BOOTS_SYS_nanosleep: return sys_nanosleep_call(args);
	default: return -ENOSYS;
	}
}

void syscall_init(void)
{
	hal_syscall_set_handler(syscall_dispatch);
}
