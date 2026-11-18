/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The test-only pen injector (CONFIG_INPUT_TEST_INJECT).
 */

#ifndef KERN_DRIVERS_GENERIC_INPUT_INJECT_H
#define KERN_DRIVERS_GENERIC_INPUT_INJECT_H

/* Publishes /dev/input-inject. */
int
drv_input_inject_register(void);

#endif
