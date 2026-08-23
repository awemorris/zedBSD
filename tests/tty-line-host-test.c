/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <kern/tty.h>
#include <kern/waitq.h>

#include <stdio.h>

void waitq_wake_all(struct wait_queue *queue) { (void)queue; }

int
main(void)
{
	if (!tty_test_vlnext_ixon())
		return 1;
	puts("zedBSD TTY line-discipline host tests: PASS");
	return 0;
}
