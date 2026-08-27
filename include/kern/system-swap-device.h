/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_SYSTEM_SWAP_DEVICE_H
#define ZEDBSD_KERN_SYSTEM_SWAP_DEVICE_H

#include <stdint.h>

/*
 * Perform one runtime-swap /dev/system request.  User memory is copied and
 * validated here before the swap-control facade is entered; enumeration is
 * copied out only after the facade has returned a complete kernel snapshot.
 */
int
system_swap_device_ioctl(
	unsigned long request,
	uintptr_t argument,
	int superuser);

#endif
