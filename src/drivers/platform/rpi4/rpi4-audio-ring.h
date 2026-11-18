/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The arithmetic of the Raspberry Pi 4's headphone jack (ws207-p002,
 * rpi4-audio.c): 16-bit stereo samples into the PWM's duty words, and
 * the DMA's ring of control blocks that feeds them to the PWM's FIFO.
 * Without hardware access; the host test builds it.
 */

#ifndef KERN_DRIVERS_RPI4_AUDIO_RING_H
#define KERN_DRIVERS_RPI4_AUDIO_RING_H

#include <stddef.h>
#include <stdint.h>

/* A legacy DMA channel's transfer information bits (BCM2711 peripherals, 4.2.1). */
#define RPI4_DMA_TI_INTEN		0x00000001U
#define RPI4_DMA_TI_WAIT_RESP		0x00000008U
#define RPI4_DMA_TI_DEST_DREQ		0x00000040U
#define RPI4_DMA_TI_SRC_INC		0x00000100U
#define RPI4_DMA_TI_PERMAP_SHIFT	16U

/* The DREQ of PWM1 (DMA_CNTRL_MUX_2 selects it at reset). */
#define RPI4_DMA_DREQ_PWM1		1U

/*
 * One control block as the DMA reads it: 32 bytes, 32-byte aligned, every
 * address a bus address.
 */
struct rpi4_dma_cb {
	uint32_t ti;
	uint32_t source;
	uint32_t destination;
	uint32_t length;
	uint32_t stride;
	uint32_t next;
	uint32_t reserved[2];
};

uint32_t rpi4_audio_duty(int16_t sample, uint32_t range);
void rpi4_audio_convert(const int16_t *pcm, uint32_t *words, size_t frames, uint32_t range, int right_first);
void rpi4_audio_chain(struct rpi4_dma_cb *cbs, unsigned count, uint32_t cb_bus, uint32_t data_bus, uint32_t fragment_bytes, uint32_t fifo_bus);
unsigned rpi4_audio_fragment(uint32_t source_bus, uint32_t data_bus, uint32_t fragment_bytes, unsigned count);
uint32_t rpi4_audio_position(uint32_t source_bus, uint32_t data_bus, uint32_t fragment_bytes, unsigned count);

#endif
