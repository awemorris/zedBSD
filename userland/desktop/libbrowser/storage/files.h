/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The files backend of the storage (files.c): browser_storage_files_open
 * of <browser/storage.h> and the table it hands out.  This header holds
 * the layout on the disk, which the tests and the instance need to know.
 *
 * Layout under the directory given to browser_storage_files_open:
 *   lock                       the lock file (wb_os_lock_open)
 *   cookies/<key>              one BROWSER_STORAGE_COOKIES record
 *   cache/<key>                one BROWSER_STORAGE_CACHE record
 *   local-storage/<key>.ls     one BROWSER_STORAGE_LOCAL record
 *   space-<n>/<key>            a record of any other space n (decimal)
 * A space's directory is created on its first write.  Every write goes
 * through wb_os_file_replace, so a record file is always whole.  Files
 * whose names are not valid keys (with the ".ls" suffix removed in
 * local-storage), including the replace's temporary files, are ignored by
 * list and never removed by clear.
 *
 * The table's calls and their errors (beyond <browser/storage.h>):
 *   read    a missing record or a missing space directory is found 0;
 *           the file is read whole (wb_os_file_read) and appended to the
 *           sink in pieces of at most 1 MiB.  EINVAL for an invalid key.
 *   write   EINVAL for an invalid key; the errors of
 *           wb_os_make_directories and wb_os_file_replace, ENOSPC kept,
 *           others passed through as the backend's error.
 *   remove  ENOENT from the system is 0.  EINVAL for an invalid key.
 *   list    each valid key once; a missing space directory lists nothing.
 *   clear   removes each listed record; the first failure is returned
 *           after trying the rest.
 *   close   releases the lock and frees the context.
 * All OS work goes through base/os-file.h; the backend has no #ifdef.
 */

#ifndef KEILAND_BROWSER_STORAGE_FILES_H
#define KEILAND_BROWSER_STORAGE_FILES_H

#include <browser/storage.h>

/* The names of the spaces' directories and the localStorage suffix. */
#define WB_STORAGE_FILES_COOKIES	"cookies"
#define WB_STORAGE_FILES_CACHE		"cache"
#define WB_STORAGE_FILES_LOCAL		"local-storage"
#define WB_STORAGE_FILES_LOCAL_SUFFIX	".ls"
#define WB_STORAGE_FILES_OTHER		"space-"

/* The largest piece one sink append takes during a read (1 MiB). */
#define WB_STORAGE_FILES_PIECE		(1024UL * 1024UL)

#endif
