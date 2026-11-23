/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Host of ssh_config a WSL distribution gets after its first login
 * (wsl-record.c): the text of ssh\config with a Host wsl-NAME block added,
 * holding the user logged in as, the port when it is not SSH's own, and
 * the key the server took when it is not the one read by default.
 *
 * It does not include a header of Windows, so the tests run it on the
 * host; ssh-thread.c reads and writes the file.
 */

#ifndef MIKUTERM_WSL_RECORD_H
#define MIKUTERM_WSL_RECORD_H

#include <stddef.h>

/* The start of the Host a distribution has, and the key ssh reads when no IdentityFile is given. */
#define WSL_RECORD_PREFIX		"wsl-"
#define WSL_RECORD_DEFAULT_KEY		"~/.ssh/id_ed25519"

/* SSH's own port, which a Host without Port connects to. */
#define WSL_RECORD_SSH_PORT		22U

int wsl_record_text(const char *text, size_t size, const char *name, const char *user, unsigned port, const char *identity, char **out, size_t *out_size);

#endif
