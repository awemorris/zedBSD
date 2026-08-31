/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the zedBSD userland glob interface.
 */

#ifndef ZEDBSD_USERLAND_SH_GLOB_H
#define ZEDBSD_USERLAND_SH_GLOB_H

#include "userland/base/sh/expand.h"

int sh_glob_fields(struct sh_field_list *, const char **);

#endif
