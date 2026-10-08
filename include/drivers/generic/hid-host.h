/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * /dev/hid-host (ws143-p005; the node's interface is <uapi/hid-host.h>).
 */

#ifndef KERN_DRIVERS_GENERIC_HID_HOST_H
#define KERN_DRIVERS_GENERIC_HID_HOST_H

#include <uapi/hid-host.h>

int drv_hid_host_register(void);
int drv_hid_host_setup_valid(const struct hid_host_setup *setup);

#endif
