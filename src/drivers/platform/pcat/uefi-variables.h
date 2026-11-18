/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The UEFI variable store, reached through the firmware's runtime services.
 */

#ifndef KERN_DRIVERS_PLATFORM_PCAT_UEFI_VARIABLES_H
#define KERN_DRIVERS_PLATFORM_PCAT_UEFI_VARIABLES_H

#include <stddef.h>
#include <stdint.h>

/*
 * Maps the firmware's runtime services and checks their tables.
 *
 * Reports ENODEV when the boot gave no runtime services (a BIOS boot, a
 * loader that did not move them), and leaves the variable calls answering
 * EOPNOTSUPP.
 */
int drv_pcat_uefi_variables_init(void);

/*
 * The firmware's variables, as /dev/system offers them.
 *
 * The space is the vendor GUID's text and the name a UTF-8 string, both
 * ending in NUL; the buffers are the kernel's.  The calls report 0 or an
 * errno value, EOPNOTSUPP while the runtime services are not reachable.
 */
int drv_firmware_variable_get(const char *space, const char *name, uint32_t *attributes, void *value, size_t *size);
int drv_firmware_variable_set(const char *space, const char *name, uint32_t attributes, const void *value, size_t size);
int drv_firmware_variable_next(char *space, size_t space_size, char *name, size_t name_size);

#endif
