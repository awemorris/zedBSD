/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * VCHIQ: the message and bulk transfer channel to the Raspberry Pi 4's
 * VideoCore firmware.
 *
 * A client opens a service by its four-character code, sends messages of up
 * to one slot, and moves larger data with bulk transfers the VPU's DMA
 * performs directly to or from physically contiguous memory.  The firmware's
 * services (MMAL's video decoder, the audio service) sit behind it.
 */

#ifndef KERN_DRIVERS_RPI4_VCHIQ_H
#define KERN_DRIVERS_RPI4_VCHIQ_H

#include <stdbool.h>
#include <stdint.h>

/* The directions of a bulk transfer: from the ARM's memory, or into it. */
#define DRV_RPI4_VCHIQ_BULK_TRANSMIT	0U
#define DRV_RPI4_VCHIQ_BULK_RECEIVE	1U

/* The actual length a bulk completion reports when the transfer was aborted. */
#define DRV_RPI4_VCHIQ_BULK_ABORTED	(-1)

/* Makes a service's four-character code from its characters, first character highest. */
#define DRV_RPI4_VCHIQ_FOURCC(a, b, c, d) \
	(((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

struct drv_rpi4_vchiq_service;

/*
 * What a service's client is told.
 *
 * Every callback runs on VCHIQ's receive thread, without VCHIQ's locks
 * held.  It must return promptly and must not send on any service: the
 * receive thread is what hands the VPU's slots back, so a send from it
 * could wait for itself.  message's data is valid only during the call.
 */
struct drv_rpi4_vchiq_service_ops {
	void (*message)(void *context, const void *data, uint32_t size);
	void (*bulk_done)(void *context, void *bulk_context, int32_t actual);
	void (*closed)(void *context);
};

/*
 * Finds VCHIQ in the device tree, hands its shared memory to the firmware
 * and starts connecting.
 *
 * The connection completes in the background; drv_rpi4_vchiq_open() waits
 * for it.  An emulated board or a tree without VCHIQ gives ENODEV.
 */
int drv_rpi4_vchiq_init(uint64_t fdt_physical);

/* Reports whether the firmware has answered the connection. */
bool drv_rpi4_vchiq_connected(void);

/*
 * Opens a service of the firmware.
 *
 * Waits up to two seconds for the connection and one second for the
 * firmware's answer.  Thread context only.
 */
int drv_rpi4_vchiq_open(uint32_t fourcc, uint16_t version, uint16_t version_min, const struct drv_rpi4_vchiq_service_ops *ops, void *context, struct drv_rpi4_vchiq_service **result);

/* Sends one message of up to 4088 bytes on an open service.  Thread context only. */
int drv_rpi4_vchiq_send(struct drv_rpi4_vchiq_service *service, const void *data, uint32_t size);

/*
 * Sends an optional message and then queues one bulk transfer, with nothing
 * of the service's between them.
 *
 * physical and bytes name physically contiguous memory the VPU reads
 * (TRANSMIT) or writes (RECEIVE); the memory must not be touched by the CPU
 * until bulk_done reports the transfer.  size zero sends no message.
 * Waits up to one second while the direction already has the four bulks
 * the protocol allows outstanding.  Thread context only.
 */
int drv_rpi4_vchiq_send_bulk(struct drv_rpi4_vchiq_service *service, const void *data, uint32_t size, unsigned direction, uint64_t physical, uint32_t bytes, void *bulk_context);

/* Reports the version the firmware's side of a service speaks. */
uint16_t drv_rpi4_vchiq_peer_version(const struct drv_rpi4_vchiq_service *service);

/*
 * Closes a service and waits up to one second for the firmware to agree.
 *
 * The service is freed either way; its client is not called again after
 * this returns.  Thread context only.
 */
int drv_rpi4_vchiq_close(struct drv_rpi4_vchiq_service *service);

/* Logs a service's state, ports and bulk queues (for a stall's analysis). */
void drv_rpi4_vchiq_report(struct drv_rpi4_vchiq_service *service);

#endif
