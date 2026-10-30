/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Raspberry Pi 4 SD controllers' shared interrupt line.
 *
 * The eMMC2 controller (SD card) and the legacy EMMC controller (the
 * Wi-Fi's SDIO) raise the same GIC SPI 126.  The kernel takes one handler
 * per line, so both controllers' drivers add their handler here instead,
 * and the dispatcher asks each in turn whether the interrupt was its own.
 */

#ifndef KERN_DRIVERS_RPI4_MMC_IRQ_H
#define KERN_DRIVERS_RPI4_MMC_IRQ_H

#include <stdbool.h>

/* How many controllers may share the line. */
#define DRV_RPI4_MMC_IRQ_SLOTS		2U

/*
 * One controller's handler.
 *
 * Runs in interrupt context with the dispatcher's lock held, so it must not
 * sleep or take a lock that is held while the dispatcher's lock is taken.
 * Returns true when the interrupt was this controller's and the controller
 * has stopped raising it.
 */
typedef bool (*drv_rpi4_mmc_irq_fn)(void *argument);

/*
 * Adds a controller's handler to the shared line.
 *
 * The first call installs the dispatcher on irq, which stays installed for
 * as long as the kernel runs.  Returns EINVAL for another line number or a
 * missing handler, EEXIST for a handler and argument already added, EBUSY
 * when both slots are taken, or the kernel's error from installing the
 * dispatcher.  Thread context only.
 */
int drv_rpi4_mmc_irq_add(int irq, drv_rpi4_mmc_irq_fn handler, void *argument);

/*
 * Removes a controller's handler from the shared line.
 *
 * Once it returns the handler is not called again.  The caller stops its
 * controller from raising the line first.  Returns EINVAL for a line that is
 * not the dispatcher's and ENOENT for a handler that was not added.
 */
int drv_rpi4_mmc_irq_remove(int irq, drv_rpi4_mmc_irq_fn handler, void *argument);

#endif
