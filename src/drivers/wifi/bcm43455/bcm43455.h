/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * BCM43455 (Broadcom/Cypress) FullMAC Wi-Fi over SDIO.
 *
 * The driver loads the chip's firmware, its regulatory table and the
 * board's NVRAM settings, talks to the running firmware in SDPCM frames
 * carrying BCDC messages, and publishes the chip as a wlanN network
 * device with a WLAN station.  It reaches the chip only through the bus
 * operations below, which the board's SDIO host provides (Raspberry Pi
 * 4: src/drivers/platform/rpi4/rpi4-wifi.c) and a host test replaces.
 */

#ifndef KERN_DRIVERS_BCM43455_H
#define KERN_DRIVERS_BCM43455_H

#include <stddef.h>
#include <stdint.h>

#include "drivers/wifi/bcm43455/bcm43455-firmware.h"

/*
 * What the driver needs of the SDIO bus.
 *
 * Every operation runs in thread context.  read_byte and write_byte are
 * CMD52; read and write are CMD53, with an incrementing address (the
 * backplane) or a fixed one (increment 0: the frame function's FIFO), a
 * length up to the function's block size in byte mode and a longer one in
 * whole blocks.  sleep waits without busy waiting and returns ECANCELED when
 * the board stops the driver.  interrupt_wait waits for the card's
 * interrupt until deadline (the scheduler's clock): 0 for an interrupt,
 * which leaves it disarmed until interrupt_rearm; EAGAIN for a wakeup
 * from interrupt_kick; ETIMEDOUT at the deadline.  power_up resets the
 * chip through its power line and enumerates its SDIO interface up to
 * function 1 enabled and both block sizes set; power_down cuts the
 * chip's power.
 */
struct bcm43455_bus_ops {
	int (*read_byte)(void *context, unsigned function, uint32_t address, uint8_t *value);
	int (*write_byte)(void *context, unsigned function, uint32_t address, uint8_t value);
	int (*read)(void *context, unsigned function, uint32_t address, int increment, uint8_t *buffer, size_t length);
	int (*write)(void *context, unsigned function, uint32_t address, int increment, uint8_t *buffer, size_t length);
	unsigned (*block_size)(void *context, unsigned function);
	int (*enable_function)(void *context, unsigned function, unsigned timeout_ms);
	int (*disable_function)(void *context, unsigned function);
	int (*sleep)(void *context, unsigned milliseconds);
	int (*interrupt_wait)(void *context, uint64_t deadline);
	void (*interrupt_enable)(void *context, int enable);
	void (*interrupt_rearm)(void *context);
	void (*interrupt_kick)(void *context);
	int (*power_up)(void *context);
	void (*power_down)(void *context);
};

/* One chip on its bus: opaque to the board's driver, which only passes it back. */
struct bcm43455_device;

/*
 * Makes a device on a bus.
 *
 * The device takes the files: from here on they are released with the
 * device.  ops and context must outlive the device.  Touches no hardware.
 */
int bcm43455_device_create(const struct bcm43455_bus_ops *ops, void *context, struct bcm43455_files *files, struct bcm43455_device **result);

/*
 * Gives the chip the board's address, which every start sets before the
 * address is read: for a chip whose OTP carries none (Raspberry Pi 5,
 * where the boot firmware puts it in the device tree's local-mac-address).
 * Returns EINVAL for an address of all zeros or a multicast one.  Call it
 * before bcm43455_device_probe(); touches no hardware.
 */
int bcm43455_device_set_address(struct bcm43455_device *device, const uint8_t mac[6]);

/*
 * Brings the chip up once to learn its address, firmware version,
 * capabilities and channels, then powers it down.
 *
 * Returns EADDRNOTAVAIL for a chip whose address is zero, multicast, or
 * the placeholder the board's NVRAM carries, and ENODEV for another chip.
 * Thread context, no lock held.
 */
int bcm43455_device_probe(struct bcm43455_device *device);

/*
 * Publishes the probed chip as the first free wlanN and attaches its
 * station.
 *
 * Returns ENODEV when the network or the WLAN layer is shutting down,
 * which is a normal end for a device found too late.  After success the
 * device lives for as long as the kernel runs.  The caller serializes
 * this against its own stop.
 */
int bcm43455_device_publish(struct bcm43455_device *device);

/*
 * Releases a device that was never published, with its files.
 */
void bcm43455_device_destroy(struct bcm43455_device *device);

#endif
