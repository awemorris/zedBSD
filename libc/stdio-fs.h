/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * zedBSD filesystem-backed stdio for kernel mode Noct.
 * This will be removed after moving Noct to userspace.
 */

#ifndef ZEDBSD_STDIO_FS_H
#define ZEDBSD_STDIO_FS_H

struct zedbsd_filesystem;
struct zedbsd_environment;
struct zedbsd_namespace;
struct cwdinfo;

void zedbsd_stdio_set_filesystem(struct zedbsd_filesystem *filesystem);
void zedbsd_stdio_set_namespace(struct zedbsd_namespace *namespace);
void zedbsd_stdio_set_context(struct cwdinfo *context);
void zedbsd_stdio_set_environment(struct zedbsd_environment *environment);
int zedbsd_stdio_close_all(void);

#endif
