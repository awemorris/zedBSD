/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The password check that login and sessiond share.
 */

#ifndef LOGIN_VERIFY_H
#define LOGIN_VERIFY_H

#include <pwd.h>
#include <stddef.h>

/* The size of the buffer a caller gives login_verify for the account's strings. */
#define LOGIN_VERIFY_BUFFER	2048U

int login_verify(const char *name, char *password, struct passwd *account, char *buffer, size_t size);

#endif
