/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The boot logo's tokens among the boot parameters.
 */

#ifndef KERN_BOOTLOADER_LOGO_PATH_H
#define KERN_BOOTLOADER_LOGO_PATH_H

/* The longest logo path the loaders take. */
#define ZBL_LOGO_PATH_MAX	63

#ifndef __ASSEMBLER__
#include <stddef.h>

int
zbl_logo_path(
	const char *text,
	size_t length,
	char *path,
	size_t capacity);

int
zbl_parameter_present(
	const char *text,
	size_t length,
	const char *token);

#endif
#endif
