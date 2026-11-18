/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the application of hunks to lines.
 */

#ifndef USERLAND_BASE_PATCH_APPLY_H
#define USERLAND_BASE_PATCH_APPLY_H

#include "userland/base/patch/patch.h"

/* What happened to a hunk. */
#define PATCH_APPLIED 0
#define PATCH_REJECTED 1
#define PATCH_SKIPPED 2

int patch_apply(const struct patch_options *options, const struct patch_file *file, const struct patch_lines *text, struct patch_lines *result, int *outcomes);
int patch_apply_ed(const struct patch_file *file, const struct patch_lines *text, struct patch_lines *result);

#endif
