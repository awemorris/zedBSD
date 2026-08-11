/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * Boots filesystem-backed stdio for kernel mode Noct.
 * This will be removed after moving Noct to userspace.
 */

#ifndef BOOTS_STDIO_FS_H
#define BOOTS_STDIO_FS_H

struct boots_filesystem;
struct boots_environment;
struct boots_namespace;
struct cwdinfo;

void boots_stdio_set_filesystem(struct boots_filesystem *filesystem);
void boots_stdio_set_namespace(struct boots_namespace *namespace);
void boots_stdio_set_context(struct cwdinfo *context);
void boots_stdio_set_environment(struct boots_environment *environment);
int boots_stdio_close_all(void);

#endif
