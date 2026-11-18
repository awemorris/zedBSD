/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Exposes the platform-independent Vulkan and direct-display declarations. */

#ifndef KERN_VULKAN_H
#define KERN_VULKAN_H

#include "vulkan_core.h"
#include "vulkan_video.h"
#include "vulkan_display_control.h"

#ifdef VK_USE_PLATFORM_WAYLAND_KHR
#include "vulkan_wayland.h"
#endif

#endif
