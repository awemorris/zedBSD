/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The English text of an errno value, for the lines the engine and the
 * browser print and for the error text a page's console shows.
 *
 * The engine does not call strerror for these.  The layer returns errno
 * values (os.h) that Windows' C runtime does not describe (EPROTO and the
 * network values its mingw headers number differently), and its strerror
 * answers "Unknown error" for them.  This function gives the same text on
 * every system, the one the POSIX systems' strerror gives for the value.
 *
 * os-error.c implements it with ISO C only, so the same file is built
 * on every system and no system header beyond <errno.h> is needed.
 */

#ifndef KEILAND_BROWSER_OS_ERROR_H
#define KEILAND_BROWSER_OS_ERROR_H

/*
 * Gives the text that describes an errno value.
 *
 * error: an errno value of <errno.h>, or any int.
 * Returns a NUL-terminated English text that lives as long as the program
 * and is never modified (never NULL, so it can be passed to printf's %s).
 * A value the table does not list gives "Unknown error".  Every thread may
 * call it.
 */
const char *wb_os_error_text(int error);

#endif
