/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the amdrdna control tool's files share.
 */

#ifndef AMDRDNACTL_H
#define AMDRDNACTL_H

#include <stdint.h>

/* main.c: the driver's check and its reports (GPU_DIAGNOSE). */
int control_driver_check(int descriptor, const char *driver);
int control_report(int descriptor, uint32_t kind, uint32_t argument, uint64_t offset, void *buffer, uint64_t capacity, uint64_t *copied);
int control_report_part(int descriptor, uint32_t kind, uint32_t argument, uint64_t offset, void *buffer, uint64_t capacity, uint64_t *copied, uint64_t *total);

/* gpu-check.c: the GPU device check (amdrdnactl gpu). */
int control_gpu(const char *path);

/* modes.c: the display's modes (amdrdnactl modes). */
int control_modes(const char *path);

/* reset.c: the reset of the GPU (amdrdnactl reset). */
int control_reset(const char *path);

/* present.c: a mode set by a present (amdrdnactl present) and the output's off and on (amdrdnactl hotplug). */
int control_present(const char *path, uint32_t width, uint32_t height, unsigned seconds);
int control_hotplug(const char *path, int on);

#endif
