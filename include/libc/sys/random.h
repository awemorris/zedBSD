/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_SYS_RANDOM_H
#define LIBC_SYS_RANDOM_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Flags for getrandom.
 *
 * zedBSD draws from the processor's own random instruction, which answers
 * from the first instruction a program executes.  There is no pool waiting
 * to be seeded, so a draw never blocks and GRND_NONBLOCK never has anything
 * to report, while the other two choose between sources this system does not
 * keep apart.  The values are the ones every system carrying this call uses,
 * because software passes them as constants it compiled elsewhere.
 */
#define GRND_NONBLOCK 0x0001
#define GRND_RANDOM   0x0002
#define GRND_INSECURE 0x0004

ssize_t getrandom(void *, size_t, unsigned int);
int getentropy(void *, size_t);

#ifdef __cplusplus
}
#endif

#endif
