/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * /dev/input/bridge (ws143-p005; the node's interface is <uapi/input-bridge.h>).
 */

#ifndef KERN_DRIVERS_GENERIC_INPUT_BRIDGE_H
#define KERN_DRIVERS_GENERIC_INPUT_BRIDGE_H

#include <uapi/input-bridge.h>

int drv_input_bridge_register(void);
int drv_input_bridge_setup_valid(const struct input_bridge_setup *setup);

#endif
