/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Reading the PowerVR firmware file from the root file system.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_FIRMWARE_FILE_H
#define KERN_DRIVERS_GPU_POWERVR_FIRMWARE_FILE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Reads a firmware file whole into kernel memory, waiting a bounded time
 * for the root file system.
 */
int powervr_firmware_read(const char *path, size_t limit, uint8_t **bytes, size_t *size);

#endif
