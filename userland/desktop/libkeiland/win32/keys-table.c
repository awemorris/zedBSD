/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The keys of the Windows backend: scan code set 1 to evdev codes.
 */

#include "userland/desktop/libkeiland/win32/keys-table.h"

#include <errno.h>
#include <stddef.h>

/* Not implemented yet. */
uint32_t
keiui_win32_key_code(
	unsigned scan,
	int extended,
	unsigned virtual_key)
{
	(void)scan;
	(void)extended;
	(void)virtual_key;

	/* A skeleton: the function is not written yet. */
	return 0;
}
