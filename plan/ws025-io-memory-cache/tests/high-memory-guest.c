/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define REQUIRE(x) do { if (!(x)) { printf("HIGH GUEST FAIL line=%d errno=%d\n", __LINE__, errno); return 1; } } while (0)

int
main(void)
{
	volatile uint64_t *words;
	unsigned round;
	unsigned index;
	pid_t child;
	int status;

	/* Repeats mapping, shared fork pages, private writes and complete teardown. */
	for (round = 0; round < 4; round++) {
		words = mmap((void *)(uintptr_t)0x40000000U, 65536, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
		REQUIRE(words == (void *)(uintptr_t)0x40000000U);
		for (index = 0; index < 65536 / sizeof(*words); index++) {
			REQUIRE(words[index] == 0);
			words[index] = UINT64_C(0xabcdef1200000000) + round + index;
		}
		printf("HIGH GUEST fork round=%u\n", round);
		child = fork();
		REQUIRE(child >= 0);
		if (child == 0) {
			for (index = 0; index < 65536 / sizeof(*words); index++) {
				if (words[index] != UINT64_C(0xabcdef1200000000) + round + index)
					_exit(2);
				words[index] = UINT64_C(0x1234567800000000) + index;
			}
			for (index = 0; index < 65536 / sizeof(*words); index++) {
				if (words[index] != UINT64_C(0x1234567800000000) + index)
					_exit(3);
			}
			if (munmap((void *)words, 65536) != 0)
				_exit(4);
			_exit(0);
		}
		REQUIRE(waitpid(child, &status, 0) == child);
		REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
		for (index = 0; index < 65536 / sizeof(*words); index++) {
			REQUIRE(words[index] == UINT64_C(0xabcdef1200000000) + round + index);
			words[index] ^= UINT64_C(0xffff00000000ffff);
		}
		REQUIRE(munmap((void *)words, 65536) == 0);
		printf("HIGH GUEST round=%u PASS\n", round);
	}
	puts("HIGH GUEST PASS");
	return 0;
}
