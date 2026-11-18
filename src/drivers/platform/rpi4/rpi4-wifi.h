/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Raspberry Pi 4 onboard Wi-Fi: the probe and its thread.
 *
 * The chip (Broadcom/Cypress BCM43455) sits on the legacy EMMC controller's
 * SDIO bus.  The probe only reads the device tree and starts a thread; the
 * thread waits for the root file system and the firmware package's files,
 * routes the chip's pins, starts the host, and has the BCM43455 driver
 * (src/drivers/wifi/bcm43455/) power the chip, learn its identity and
 * publish it as wlanN.
 */

#ifndef KERN_DRIVERS_RPI4_WIFI_H
#define KERN_DRIVERS_RPI4_WIFI_H

#include <stdint.h>

/*
 * Looks for the onboard Wi-Fi and starts its thread.
 *
 * fdt_phys is the device tree's physical address and sd_base the
 * controller the SD card driver bound to, zero for none.  Touches no
 * register, no mailbox and no power line.  Returns ENODEV for a board or
 * emulator without the chip, EBUSY when the SD card uses its controller,
 * and EALREADY for a second call.  Thread context only.
 */
int drv_rpi4_wifi_probe(uint64_t fdt_phys, uintptr_t sd_base);

/*
 * Stops the publishing thread and waits until it has ended.
 *
 * A thread stopped before the publication gives the host and the chip's
 * power back; a published chip is left to its network device, which the
 * network's shutdown closes.  Returns at once when there is no thread.
 * Called before a shutdown or reboot, so no wlanN appears while the
 * network stops.  Thread context only; never called by the thread itself.
 */
void drv_rpi4_wifi_stop(void);

#endif
