/*
 * WS008 NOCT-T020: zedBSD amd64 executable-memory acceptance probe.
 *
 * This deliberately tests the public mmap/mprotect/munmap contract before
 * Noct is involved.  Faulting accesses run in children so a correct W^X
 * policy cannot terminate the test driver itself.
 *
 * Copyright (c) 2026, Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#if !defined(__x86_64__)
#error "NOCT-T020 native code is defined only for amd64"
#endif

#define NATIVE_RESULT UINT32_C(0x5a17c0de)

typedef uint32_t (*native_function_t)(void);

static int
fail(const char *stage)
{
	fprintf(stderr, "NOCT-T020-FAIL stage=%s errno=%d\n", stage, errno);
	return 1;
}

static int
mark(const char *text)
{
	if (puts(text) < 0)
		return fail("marker-write");
	return 0;
}

static int
wait_child(pid_t child, const char *stage, int expect_signal)
{
	int status;
	pid_t result;

	do {
		result = waitpid(child, &status, 0);
	} while (result < 0 && errno == EINTR);
	if (result != child) {
		(void)fail(stage);
		return 0;
	}
	if (expect_signal != 0) {
		if (WIFSIGNALED(status) && WTERMSIG(status) == expect_signal)
			return 1;
	} else if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		return 1;
	}
	fprintf(stderr,
		"NOCT-T020-FAIL stage=%s status=%d expected-signal=%d\n", stage,
		status, expect_signal);
	return 0;
}

static int
expect_write_fault(volatile unsigned char *address)
{
	pid_t child;

	if (fflush(NULL) != 0)
		return 0;
	child = fork();
	if (child < 0)
		return 0;
	if (child == 0) {
		address[0] ^= 0xffU;
		_exit(120);
	}
	return wait_child(child, "write-after-rx", SIGSEGV);
}

static int
expect_read_fault(volatile unsigned char *address)
{
	pid_t child;

	if (fflush(NULL) != 0)
		return 0;
	child = fork();
	if (child < 0)
		return 0;
	if (child == 0) {
		volatile unsigned char value;

		value = address[0];
		(void)value;
		_exit(121);
	}
	return wait_child(child, "read-after-unmap", SIGSEGV);
}

static int
expect_write_success(volatile unsigned char *address)
{
	pid_t child;

	if (fflush(NULL) != 0)
		return 0;
	child = fork();
	if (child < 0)
		return 0;
	if (child == 0) {
		address[0] ^= 0xffU;
		_exit(0);
	}
	return wait_child(child, "failed-mprotect-atomicity", 0);
}

static int
expect_mmap_failure(size_t length, int protection, int flags,
		    int expected_errno, const char *stage)
{
	void *mapping;

	errno = 0;
	mapping = mmap(NULL, length, protection, flags, -1, 0);
	if (mapping == MAP_FAILED && errno == expected_errno)
		return 1;
	if (mapping != MAP_FAILED)
		(void)munmap(mapping, length);
	(void)fail(stage);
	return 0;
}

static int
expect_mprotect_failure(void *address, size_t length, int protection,
			int expected_errno, const char *stage)
{
	errno = 0;
	if (mprotect(address, length, protection) == -1 &&
	    errno == expected_errno)
		return 1;
	(void)fail(stage);
	return 0;
}

int
main(void)
{
	/* mov eax, 0x5a17c0de; ret (System V AMD64 ABI). */
	static const unsigned char native_code[] = {0xb8U, 0xdeU, 0xc0U,
						    0x17U, 0x5aU, 0xc3U};
	volatile unsigned char *fault_address;
	unsigned char *mapping;
	native_function_t function;
	long configured_page_size;
	size_t page_size;
	size_t mapping_size;
	uint32_t result;

	configured_page_size = sysconf(_SC_PAGESIZE);
	if (configured_page_size <= 0)
		return fail("page-size");
	page_size = (size_t)configured_page_size;
	if (page_size > SIZE_MAX / 2U || page_size < sizeof(native_code))
		return fail("page-size-range");
	mapping_size = page_size * 2U;

	if (!expect_mmap_failure(0, PROT_READ | PROT_WRITE,
				 MAP_PRIVATE | MAP_ANONYMOUS, EINVAL,
				 "mmap-zero") ||
	    !expect_mmap_failure(page_size, PROT_READ | PROT_WRITE | PROT_EXEC,
				 MAP_PRIVATE | MAP_ANONYMOUS, EACCES,
				 "mmap-wx") ||
	    !expect_mmap_failure(page_size, PROT_READ | PROT_WRITE,
				 MAP_PRIVATE | MAP_SHARED | MAP_ANONYMOUS,
				 EINVAL, "mmap-private-shared"))
		return 1;

	mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		return fail("mmap-rw");
	if (mapping[0] != 0U || mapping[mapping_size - 1U] != 0U) {
		(void)munmap(mapping, mapping_size);
		return fail("mmap-zero-fill");
	}
	if (!expect_mprotect_failure(mapping, mapping_size,
				     PROT_READ | PROT_WRITE | PROT_EXEC, EACCES,
				     "mprotect-wx") ||
	    !expect_mprotect_failure(mapping, 0, PROT_READ, EINVAL,
				     "mprotect-zero") ||
	    !expect_mprotect_failure(mapping + 1U, page_size, PROT_READ, EINVAL,
				     "mprotect-unaligned") ||
	    !expect_mprotect_failure(mapping, page_size, 0x08, EINVAL,
				     "mprotect-invalid-protection")) {
		(void)munmap(mapping, mapping_size);
		return 1;
	}

	/* Leave a one-page hole and verify a two-page protection transaction
	 * fails without changing the valid first page. */
	if (munmap(mapping + page_size, page_size) != 0)
		return fail("munmap-tail");
	if (!expect_mprotect_failure(mapping, mapping_size, PROT_READ, EINVAL,
				     "mprotect-gap") ||
	    !expect_write_success(mapping)) {
		(void)munmap(mapping, page_size);
		return 1;
	}

	memcpy(mapping, native_code, sizeof(native_code));
	if (memcmp(mapping, native_code, sizeof(native_code)) != 0) {
		(void)munmap(mapping, page_size);
		return fail("rw-code-copy");
	}
	if (mark("NOCT-T020-RW-OK") != 0) {
		(void)munmap(mapping, page_size);
		return 1;
	}

	/* A one-byte request deliberately exercises page-range rounding. */
	if (mprotect(mapping, 1U, PROT_READ | PROT_EXEC) != 0) {
		(void)munmap(mapping, page_size);
		return fail("mprotect-rx");
	}
	__builtin___clear_cache((char *)mapping,
				(char *)mapping + sizeof(native_code));
	if (mark("NOCT-T020-RX-OK") != 0)
		return 1;

	_Static_assert(
	    sizeof(function) == sizeof(mapping),
	    "amd64 object and function pointers must have equal size");
	memcpy(&function, &mapping, sizeof(function));
	result = function();
	if (result != NATIVE_RESULT)
		return fail("native-result");
	if (mark("NOCT-T020-EXEC-OK") != 0)
		return 1;

	/* A normal protection fault is SIGSEGV.  SIGBUS is reserved by the
	 * zedBSD VM path for an inaccessible file-backed page past EOF. */
	if (!expect_write_fault(mapping))
		return 1;
	if (mark("NOCT-T020-INVALID-OK") != 0)
		return 1;

	fault_address = mapping;
	/* Like mprotect above, a one-byte unmap rounds to the complete page. */
	if (munmap(mapping, 1U) != 0)
		return fail("munmap-code");
	if (!expect_read_fault(fault_address))
		return 1;
	if (mark("NOCT-T020-UNMAP-OK") != 0)
		return 1;
	return mark("NOCT-T020-PASS");
}
