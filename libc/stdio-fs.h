/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * zedBSD filesystem-backed stdio for kernel mode Noct.
 * This will be removed after moving Noct to userspace.
 */

#ifndef ZEDBSD_STDIO_FS_H
#define ZEDBSD_STDIO_FS_H

struct bootfs;
struct environment;
struct bootfs_namespace;
struct cwdinfo;

void __stdio_set_filesystem(struct bootfs *filesystem);
void __stdio_set_namespace(struct bootfs_namespace *namespace);
void __stdio_set_context(struct cwdinfo *context);
void __stdio_set_environment(struct environment *environment);
int __stdio_close_all(void);

#endif
