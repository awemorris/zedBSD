/*
 * ELF auxiliary vector
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_UAPI_AUXV_H
#define ZEDBSD_UAPI_AUXV_H

#define AT_NULL    0U
#define AT_PHDR    3U
#define AT_PHENT   4U
#define AT_PHNUM   5U
#define AT_PAGESZ  6U
#define AT_BASE    7U
#define AT_ENTRY   9U
#define AT_UID     11U
#define AT_EUID    12U
#define AT_GID     13U
#define AT_EGID    14U
#define AT_SECURE  23U
#define AT_EXECFN  31U

#endif
