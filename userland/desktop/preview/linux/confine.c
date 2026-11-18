/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * keiland-preview's confinement on Linux (WS168 p003,
 * plan/ws168/preview-sandbox-design.md section 7): no new privileges, then a
 * seccomp filter that lets the process read and write the descriptors it
 * has, map anonymous memory that cannot run, read the clock and random
 * bytes and end; opening a file (openat, which the C library uses) is
 * refused with EACCES (the shared libpdf looks for a substitute font file,
 * which it then goes without, ws168-p004) and any other system call ends
 * the process (SECCOMP_RET_KILL_PROCESS).  It is entered before a byte of
 * the input is read; what ran before it (the dynamic linker and the C
 * library's start) read nothing the caller did not trust.
 */

#include "../preview.h"

#include <errno.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

/* The machine the filter is for (another one's calls are refused). */
#if defined(__x86_64__)
#define CONFINE_ARCH		AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define CONFINE_ARCH		AUDIT_ARCH_AARCH64
#endif

/* Where the filter reads the call's number, its machine and its third argument (the protection of mmap and mprotect). */
#define CONFINE_NR		(offsetof(struct seccomp_data, nr))
#define CONFINE_ARCH_AT		(offsetof(struct seccomp_data, arch))
#define CONFINE_ARG2		(offsetof(struct seccomp_data, args[2]))

/* A call let through as it is: its number compared, allowed when it matches. */
#define CONFINE_ALLOW(name) \
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (name), 0, 1), \
	BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)

/* A call let through when its protection cannot run code. */
#define CONFINE_ALLOW_NO_EXEC(name) \
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (name), 0, 3), \
	BPF_STMT(BPF_LD | BPF_W | BPF_ABS, CONFINE_ARG2), \
	BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, PROT_EXEC, 1, 0), \
	BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)

/*
 * Enters the confinement.  Returns 0, or an errno value when the system
 * does not have it (the program then makes no preview).
 */
int
preview_confine(void)
{
	struct sock_filter filter[] = {
		/* The machine's calls only. */
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, CONFINE_ARCH_AT),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, CONFINE_ARCH, 1, 0),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),

		/* mmap and mprotect without PROT_EXEC (the number reloaded after each look at the protection). */
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, CONFINE_NR),
		CONFINE_ALLOW_NO_EXEC(__NR_mmap),
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, CONFINE_NR),
		CONFINE_ALLOW_NO_EXEC(__NR_mprotect),
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, CONFINE_NR),

		/* The descriptors it has, memory, the clock, random bytes, the end. */
		CONFINE_ALLOW(__NR_read),
		CONFINE_ALLOW(__NR_write),
		CONFINE_ALLOW(__NR_pread64),
		CONFINE_ALLOW(__NR_lseek),
		CONFINE_ALLOW(__NR_fstat),
		CONFINE_ALLOW(__NR_close),
		CONFINE_ALLOW(__NR_munmap),
		CONFINE_ALLOW(__NR_mremap),
		CONFINE_ALLOW(__NR_madvise),
		CONFINE_ALLOW(__NR_brk),
		CONFINE_ALLOW(__NR_futex),
		CONFINE_ALLOW(__NR_clock_gettime),
		CONFINE_ALLOW(__NR_getrandom),
		CONFINE_ALLOW(__NR_rt_sigreturn),
		CONFINE_ALLOW(__NR_exit),
		CONFINE_ALLOW(__NR_exit_group),

		/* Opening a file is refused, not fatal. */
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_openat, 0, 1),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EACCES),

		/* Anything else ends the process. */
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS)
	};
	struct sock_fprog program;
	int status;

	/* No new privileges, which a filter set without CAP_SYS_ADMIN needs. */
	status = prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
	if (status != 0)
		return errno;

	/* The filter. */
	program.len = (unsigned short)(sizeof(filter) / sizeof(filter[0]));
	program.filter = filter;
	status = prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program, 0, 0);
	if (status != 0)
		return errno;
	return 0;
}
