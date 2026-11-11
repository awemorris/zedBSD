/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The amd64 suspend-to-idle state (ws052-p006): what the boot path gives
 * hal_cpu_idle_suspend_supported() and hal_cpu_idle_suspend().
 */

#ifndef KERN_HAL_AMD64_IDLE_SUSPEND_H
#define KERN_HAL_AMD64_IDLE_SUSPEND_H

struct amd64_acpi_info;

void prekern_amd64_idle_suspend_init(const struct amd64_acpi_info *acpi);

#endif
