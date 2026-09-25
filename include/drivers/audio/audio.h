/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Audio backend operations and dynamic device registration.
 */

#ifndef DRIVERS_AUDIO_H
#define DRIVERS_AUDIO_H

#include <uapi/audio.h>
#include <drivers/generic/dma.h>

#include <stdint.h>

struct drv_audio_device;

/*
 * Hardware operations of one audio function.
 *
 * The framework owns the ring: it allocates one coherent buffer of
 * fragment_bytes * fragment_count through the DMA device the backend
 * registered with and gives it to prepare().  The hardware walks the ring
 * and calls drv_audio_interrupt() once per fragment.  An instance whose
 * formats depend on its hardware keeps its own copy of this table.
 *
 * prepare, start and stop run in thread context without framework locks
 * and may sleep.  stop does not return until the hardware no longer
 * touches the ring.  position runs under the framework's IRQ-safe lock,
 * including from drv_audio_interrupt(), and must neither sleep nor take
 * a lock that the interrupt path holds.  get_volume and set_volume run in
 * thread context and may run concurrently with stream operations.
 */
struct drv_audio_ops {
	unsigned playback;
	unsigned capture;

	const struct audio_format *formats;
	unsigned format_count;

	int (*prepare)(void *private_data, int capture,
		       const struct audio_format *format,
		       const struct drv_dma_buffer *buffer,
		       uint32_t fragment_bytes, uint32_t fragment_count);
	int (*start)(void *private_data, int capture);
	void (*stop)(void *private_data, int capture);

	/* Byte offset of the hardware within the ring. */
	uint32_t (*position)(void *private_data, int capture);

	/* Optional; without set_volume, KERN_AUDIO_SET_VOLUME is ENOTSUP. */
	int (*get_volume)(void *private_data, struct audio_volume *volume);
	int (*set_volume)(void *private_data, const struct audio_volume *volume);
};

/*
 * Publishes /dev/dspN and /dev/mixerN.  ops, formats, private_data and
 * dma stay borrowed until a successful unregister.  dma is the device's
 * own DMA owner, such as drv_pci_device_dma(), so that ring addresses
 * are bus addresses the hardware can use.
 */
int
drv_audio_register(
	const struct drv_audio_ops *ops,
	void *private_data,
	struct drv_dma_device *dma,
	struct drv_audio_device **device);

/*
 * Withdraws both nodes.  EBUSY means /dev/dspN is open or a mixer request
 * is running; the handle and backend stay registered for a retry.  Success
 * consumes the handle.  The backend must have stopped calling
 * drv_audio_interrupt() before this is called.
 */
int
drv_audio_unregister(
	struct drv_audio_device *device);

/* Reports from the interrupt handler that one fragment has completed. */
void
drv_audio_interrupt(
	struct drv_audio_device *device,
	int capture);

#endif
