/*
 * syscall
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_UAPI_SYSCALL_H
#define ZEDBSD_UAPI_SYSCALL_H

enum zedbsd_syscall_number {
	ZEDBSD_SYS_exit = 1,
	ZEDBSD_SYS_open = 2,
	ZEDBSD_SYS_close = 3,
	ZEDBSD_SYS_read = 4,
	ZEDBSD_SYS_write = 5,
	ZEDBSD_SYS_lseek = 6,
	ZEDBSD_SYS_fstat = 7,
	ZEDBSD_SYS_getdents = 8,
	ZEDBSD_SYS_chdir = 9,
	ZEDBSD_SYS_getcwd = 10,
	ZEDBSD_SYS_mmap = 11,
	ZEDBSD_SYS_munmap = 12,
	ZEDBSD_SYS_mprotect = 13,
	ZEDBSD_SYS_ioctl = 14,
	ZEDBSD_SYS_clock_gettime = 15,
	ZEDBSD_SYS_nanosleep = 16,
	ZEDBSD_SYS_spawn = 17,
	ZEDBSD_SYS_wait = 18,
	ZEDBSD_SYS_brk = 19,
	ZEDBSD_SYS_socket = 20,
	ZEDBSD_SYS_bind = 21,
	ZEDBSD_SYS_connect = 22,
	ZEDBSD_SYS_listen = 23,
	ZEDBSD_SYS_accept = 24,
	ZEDBSD_SYS_sendto = 25,
	ZEDBSD_SYS_recvfrom = 26,
	ZEDBSD_SYS_shutdown = 27,
	ZEDBSD_SYS_getsockname = 28,
	ZEDBSD_SYS_getpeername = 29,
	ZEDBSD_SYS_setsockopt = 30,
	ZEDBSD_SYS_getsockopt = 31,
};

#endif
