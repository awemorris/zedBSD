/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * xdg_activation_v1 (activation.c, ws089-p016): the tokens that let a
 * program bring another program's window to the front, shared with the
 * dispatch and with the launcher, which gives each program it starts a
 * token of its own.
 */

#ifndef KWL_ACTIVATION_H
#define KWL_ACTIVATION_H

#include "kwl.h"

/* The room a token's text takes, its NUL counted. */
#define KWL_ACTIVATION_TOKEN_SIZE	33U

int kwl_activation_request(struct kwl_object *object, uint32_t opcode, const unsigned char *bytes, size_t size);
int kwl_activation_issue(struct kwl_server *server, const char *app_id, const char *via, char *token, size_t size);

#endif
