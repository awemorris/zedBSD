/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef BOOTS_UAPI_SYSCALL_H
#define BOOTS_UAPI_SYSCALL_H

enum boots_syscall_number {
	BOOTS_SYS_exit = 1,
	BOOTS_SYS_open = 2,
	BOOTS_SYS_close = 3,
	BOOTS_SYS_read = 4,
	BOOTS_SYS_write = 5,
	BOOTS_SYS_lseek = 6,
	BOOTS_SYS_fstat = 7,
	BOOTS_SYS_getdents = 8,
	BOOTS_SYS_chdir = 9,
	BOOTS_SYS_getcwd = 10,
	BOOTS_SYS_mmap = 11,
	BOOTS_SYS_munmap = 12,
	BOOTS_SYS_mprotect = 13,
	BOOTS_SYS_ioctl = 14,
	BOOTS_SYS_clock_gettime = 15,
	BOOTS_SYS_nanosleep = 16,
};

#endif
