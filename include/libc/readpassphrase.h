/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_READPASSPHRASE_H
#define LIBC_READPASSPHRASE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RPP_ECHO_OFF    0x00	/* Do not show what is typed (the default). */
#define RPP_ECHO_ON     0x01	/* Show it, for a value that is not secret. */
#define RPP_REQUIRE_TTY 0x02	/* Fail rather than read from anything else. */
#define RPP_FORCELOWER  0x04	/* Fold the answer to lower case. */
#define RPP_FORCEUPPER  0x08	/* Fold the answer to upper case. */
#define RPP_SEVENBIT    0x10	/* Strip the eighth bit of every byte. */
#define RPP_STDIN       0x20	/* Read standard input, not the terminal. */

/*
 * Asks at the terminal for something the screen must not keep.
 *
 * The prompt and the answer go to and come from the controlling terminal
 * rather than standard input, so that a redirected program still reaches the
 * person, and echo is restored even when the read is interrupted.
 */
char *readpassphrase(const char *, char *, size_t, int);

#ifdef __cplusplus
}
#endif

#endif
