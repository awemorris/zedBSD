/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_USERLAND_SH_BUILTINS_H
#define ZEDBSD_USERLAND_SH_BUILTINS_H

int sh_builtin_dispatch(int argc, char **argv, int *handled);
void sh_hash_clear(void);
int sh_hash_sync_path(const char *path);
const char *sh_hash_lookup(const char *name);
int sh_hash_store(const char *name, const char *path);
void sh_hash_print(void);

#endif
