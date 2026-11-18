/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The editing operations of the on-screen keyboard's tool face (kl_edit_v1,
 * ws102-p017, plan/ws102/design.md section 2.10): the protocol's requests and
 * the keyboard shortcuts that stand in for the face's buttons until p016.
 */

#ifndef KERN_KEILAND_EDIT_H
#define KERN_KEILAND_EDIT_H

#include "kwl.h"

int kwl_edit_request(struct kwl_object *object, uint32_t opcode, const unsigned char *bytes, size_t size);
void kwl_edit_object_gone(struct kwl_object *object);
int kwl_edit_key(struct kwl_server *server, uint32_t key, uint32_t state);

#endif
