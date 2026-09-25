/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Defines the standard Vulkan calling convention on supported UNIX targets. */

#ifndef ZEDBSD_VK_PLATFORM_H
#define ZEDBSD_VK_PLATFORM_H

#ifndef VK_NO_STDDEF_H
#include <stddef.h>
#endif

#ifndef VK_NO_STDINT_H
#include <stdint.h>
#endif

#define VKAPI_ATTR
#define VKAPI_CALL
#define VKAPI_PTR

#endif
