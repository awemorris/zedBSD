/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The files backend of the storage: one file per record under a directory.
 */

#include "storage/files.h"
#include "storage/records.h"
#include "base/os-file.h"
#include "base/base.h"

#include <errno.h>
#include <stddef.h>

/* Not implemented yet. */
int
browser_storage_files_open(
	const char *directory,
	const struct browser_storage_calls **calls,
	void **context)
{
	UNUSED_PARAMETER(directory);
	UNUSED_PARAMETER(calls);
	UNUSED_PARAMETER(context);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}
