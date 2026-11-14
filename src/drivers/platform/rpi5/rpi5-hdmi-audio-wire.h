/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The arithmetic of the Raspberry Pi 5's HDMI audio.
 *
 * The BCM2712 sends audio to an HDMI port as IEC 60958 subframes, which a
 * DMA channel moves from memory into the port's audio FIFO.  The audio
 * clock regeneration packet tells the sink how the audio clock relates to
 * the TMDS clock, and an audio InfoFrame describes the stream.  This file
 * holds the part of that which does not touch hardware -- the subframes,
 * the channel status block, the N and CTS of the clock regeneration, the
 * InfoFrame, the DMA's ring of control blocks and the stepping table -- so
 * it can be checked away from the hardware.
 */

#ifndef KERN_DRIVERS_RPI5_HDMI_AUDIO_WIRE_H
#define KERN_DRIVERS_RPI5_HDMI_AUDIO_WIRE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * The stream the driver offers: 16-bit stereo at 48 kHz, in fragments of
 * 1024 frames, eight to a ring.  The subframe ring the DMA reads holds the
 * same frames as 32-bit subframes, twice the bytes.
 */
#define DRV_RPI5_HDMI_AUDIO_RATE		48000U
#define DRV_RPI5_HDMI_AUDIO_CHANNELS		2U
#define DRV_RPI5_HDMI_AUDIO_FRAGMENT_FRAMES	1024U
#define DRV_RPI5_HDMI_AUDIO_FRAGMENT_COUNT	8U
#define DRV_RPI5_HDMI_AUDIO_PCM_FRAGMENT_BYTES	(DRV_RPI5_HDMI_AUDIO_FRAGMENT_FRAMES * DRV_RPI5_HDMI_AUDIO_CHANNELS * 2U)
#define DRV_RPI5_HDMI_AUDIO_WIRE_FRAGMENT_BYTES	(DRV_RPI5_HDMI_AUDIO_FRAGMENT_FRAMES * DRV_RPI5_HDMI_AUDIO_CHANNELS * 4U)

/*
 * The IEC 60958 framing: a block of 192 frames carries one 192-bit channel
 * status word, one bit per frame, which is 24 bytes.
 *
 * Values referenced: alsa-lib f44e0abd5382 src/pcm/pcm_iec958.c
 * (the subframe layout and the preamble values, iec958_subframe and the
 * preamble table of snd_pcm_iec958_open).
 */
#define DRV_RPI5_HDMI_AUDIO_BLOCK_FRAMES	192U
#define DRV_RPI5_HDMI_AUDIO_STATUS_BYTES	24U

/* The preamble of the left subframe that starts a block ('Z' or 'B'). */
#define DRV_RPI5_HDMI_AUDIO_PREAMBLE_BLOCK	0x08U

/* The preamble of every other left subframe ('X' or 'M'). */
#define DRV_RPI5_HDMI_AUDIO_PREAMBLE_LEFT	0x02U

/* The preamble of every right subframe ('Y' or 'W'). */
#define DRV_RPI5_HDMI_AUDIO_PREAMBLE_RIGHT	0x04U

/* The channel status bit (C), in bit 30 of a subframe. */
#define DRV_RPI5_HDMI_AUDIO_SUBFRAME_STATUS	0x40000000U

/* The parity bit (P), in bit 31: it makes bits 4 to 31 even. */
#define DRV_RPI5_HDMI_AUDIO_SUBFRAME_PARITY	0x80000000U

/*
 * The PHY's reference rate and VCO readback.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y ab4372076bb8
 * drivers/gpu/drm/vc4/vc4_hdmi_phy.c (the RM offset field and its
 * fixed-offset bit, the reference frequency field, the VCO clock divider
 * and its enable bit, the RM offset's scaling in phy_get_rm_offset).
 */

/* Bit 31 of the rate manager's offset: the offset is a fixed value. */
#define DRV_RPI5_HDMI_AUDIO_RATE_FIXED		0x80000000U

/* Bits 0 to 30 of the rate manager's offset: the VCO over the reference, scaled by 2^21. */
#define DRV_RPI5_HDMI_AUDIO_RATE_OFFSET_MASK	0x7fffffffU
#define DRV_RPI5_HDMI_AUDIO_RATE_OFFSET_SHIFT	21U

/* Bits 0 to 9 of the PLL reference register: the reference rate in MHz. */
#define DRV_RPI5_HDMI_AUDIO_REFERENCE_MHZ_MASK	0x000003ffU

/* Bits 0 to 9 of the VCO clock divider register: the divider, and bit 10: the divider is in use. */
#define DRV_RPI5_HDMI_AUDIO_VCO_DIVIDER_MASK	0x000003ffU
#define DRV_RPI5_HDMI_AUDIO_VCO_DIVIDER_ON	0x00000400U

/*
 * The TMDS character rates the driver accepts, in Hz.  The bounds are the
 * lowest HDMI rate (25.175 MHz rounds above 25 MHz) and the highest TMDS
 * character rate of HDMI 2.0.
 */
#define DRV_RPI5_HDMI_AUDIO_TMDS_MIN		25000000ULL
#define DRV_RPI5_HDMI_AUDIO_TMDS_MAX		600000000ULL

/*
 * The audio InfoFrame and the packet RAM slot that carries it.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y ab4372076bb8
 * include/linux/hdmi.h (the InfoFrame type, header size and audio
 * payload size), drivers/gpu/drm/vc4/vc4_hdmi_regs.h (the packet
 * stride).
 */
#define DRV_RPI5_HDMI_AUDIO_INFOFRAME_TYPE	0x84U
#define DRV_RPI5_HDMI_AUDIO_INFOFRAME_VERSION	1U
#define DRV_RPI5_HDMI_AUDIO_INFOFRAME_LENGTH	10U
#define DRV_RPI5_HDMI_AUDIO_INFOFRAME_BYTES	14U
#define DRV_RPI5_HDMI_AUDIO_PACKET_SLOT_BYTES	0x24U
#define DRV_RPI5_HDMI_AUDIO_PACKET_SLOT_WORDS	9U

/*
 * The 40-bit DMA channel's bits the control blocks and the channel status
 * use.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y ab4372076bb8
 * drivers/dma/bcm2835-dma.c (the 40-bit channel's CS, TI and SRCI/DESTI
 * bits, the control block's address shift).
 */

/* Transfer information: raise an interrupt at the end of this block. */
#define DRV_RPI5_HDMI_AUDIO_DMA_TI_INTERRUPT	0x00000001U

/* Transfer information: wait for each write to be acknowledged. */
#define DRV_RPI5_HDMI_AUDIO_DMA_TI_WAIT_WRITE	0x00000004U

/* Transfer information: bits 9 to 13 name the peripheral's DREQ. */
#define DRV_RPI5_HDMI_AUDIO_DMA_TI_DREQ_SHIFT	9U
#define DRV_RPI5_HDMI_AUDIO_DMA_TI_DREQ_MASK	0x1fU

/* Transfer information: pace the writes by the destination's DREQ. */
#define DRV_RPI5_HDMI_AUDIO_DMA_TI_PACE_WRITE	0x00008000U

/* Source and destination information: bits 0 to 7 hold address bits 32 to 39. */
#define DRV_RPI5_HDMI_AUDIO_DMA_HIGH_MASK	0x000000ffU

/* Source and destination information: bits 8 to 11 hold the burst length. */
#define DRV_RPI5_HDMI_AUDIO_DMA_BURST_SHIFT	8U

/* Source and destination information: the address moves on after each access. */
#define DRV_RPI5_HDMI_AUDIO_DMA_INCREMENT	0x00001000U

/* Source and destination information: each access is 128 bits wide. */
#define DRV_RPI5_HDMI_AUDIO_DMA_WIDE		0x00004000U

/* Channel status: the channel runs. */
#define DRV_RPI5_HDMI_AUDIO_DMA_CS_RUN		0x00000001U

/* Channel status: a block raised its interrupt (write 1 to clear). */
#define DRV_RPI5_HDMI_AUDIO_DMA_CS_INTERRUPT	0x00000004U

/* Channel status: the two protection bits the channel runs with. */
#define DRV_RPI5_HDMI_AUDIO_DMA_CS_PROTECTION	0x00000300U

/* Channel status: bits 16 to 19 the AXI priority, bits 20 to 23 the priority under panic. */
#define DRV_RPI5_HDMI_AUDIO_DMA_CS_PRIORITY_SHIFT	16U
#define DRV_RPI5_HDMI_AUDIO_DMA_CS_PANIC_SHIFT	20U

/* Channel status: wait for outstanding writes at the end of each block. */
#define DRV_RPI5_HDMI_AUDIO_DMA_CS_DRAIN_WRITES	0x10000000U

/* Channel status: do not pause when the debugger asks. */
#define DRV_RPI5_HDMI_AUDIO_DMA_CS_NO_DEBUG	0x20000000U

/* The control block's address goes into the channel shifted right by five bits. */
#define DRV_RPI5_HDMI_AUDIO_DMA_CB_SHIFT	5U
#define DRV_RPI5_HDMI_AUDIO_DMA_CB_ALIGN	32U

/*
 * The chip's stepping.  The BCM2712 C1 and D0 route the HDMI audio DREQs
 * differently and pack the audio FIFO's thresholds differently.
 */
enum drv_rpi5_hdmi_audio_stepping {
	DRV_RPI5_HDMI_AUDIO_STEPPING_UNKNOWN = 0,
	DRV_RPI5_HDMI_AUDIO_STEPPING_C,
	DRV_RPI5_HDMI_AUDIO_STEPPING_D0
};

/*
 * Why the PHY's readback gives no TMDS rate, or that it gives one.
 */
enum drv_rpi5_hdmi_audio_tmds_status {
	DRV_RPI5_HDMI_AUDIO_TMDS_OK = 0,
	DRV_RPI5_HDMI_AUDIO_TMDS_NOT_FIXED,
	DRV_RPI5_HDMI_AUDIO_TMDS_NO_DIVIDER,
	DRV_RPI5_HDMI_AUDIO_TMDS_NO_REFERENCE,
	DRV_RPI5_HDMI_AUDIO_TMDS_OUT_OF_RANGE
};

/*
 * The audio clock regeneration of one TMDS rate: the N the source sends
 * and the CTS that goes with it.
 *
 * exact says the CTS is the true value for a rate the table knows; when it
 * is false the CTS is the nearest whole number to a measured rate.
 */
struct drv_rpi5_hdmi_audio_clock {
	uint32_t n;
	uint32_t cts;
	bool exact;
};

/*
 * What a device tree DMA cell asks of the channel that serves it.
 *
 * The cell's low five bits are the DREQ; the bits above are a convention
 * of the Raspberry Pi device trees for the channel's priority and for the
 * shape of the transfer.  The driver may narrow the request (no burst,
 * 32-bit reads) before it builds the ring.
 */
struct drv_rpi5_hdmi_audio_dma_request {
	uint32_t dreq;
	uint32_t priority;
	uint32_t panic_priority;
	bool wide_source;
	bool wide_destination;
	bool wait_write;
	bool drain_writes;
	bool no_debug;
	bool burst;
};

/*
 * One control block of a 40-bit DMA channel, as the channel reads it from
 * memory: eight words, 32-byte aligned.  An address is split between its
 * low word and the low byte of the matching information word.
 */
struct drv_rpi5_hdmi_audio_dma_cb {
	uint32_t transfer;
	uint32_t source;
	uint32_t source_info;
	uint32_t destination;
	uint32_t destination_info;
	uint32_t length;
	uint32_t next;
	uint32_t reserved;
};

void drv_rpi5_hdmi_audio_channel_status(uint8_t *status);
uint32_t drv_rpi5_hdmi_audio_subframe(int16_t sample, bool right, uint32_t block_frame, const uint8_t *status);
void drv_rpi5_hdmi_audio_convert(const int16_t *pcm, uint32_t *words, uint64_t serial, const uint8_t *status);
uint64_t drv_rpi5_hdmi_audio_advance(uint64_t serial, uint32_t fragment);
enum drv_rpi5_hdmi_audio_tmds_status drv_rpi5_hdmi_audio_tmds_rate(uint32_t rate_offset, uint32_t reference, uint32_t vco_divider, uint64_t *tmds_hz);
void drv_rpi5_hdmi_audio_clock(uint64_t tmds_hz, bool hardware_cts, struct drv_rpi5_hdmi_audio_clock *clock);
void drv_rpi5_hdmi_audio_sample_divider(uint32_t clock_hz, uint32_t rate, uint32_t *periods, uint32_t *samples);
uint32_t drv_rpi5_hdmi_audio_sample_word(uint32_t periods, uint32_t samples);
void drv_rpi5_hdmi_audio_infoframe(uint8_t *frame, uint32_t channels);
void drv_rpi5_hdmi_audio_packet_words(const uint8_t *frame, uint32_t *words);
uint32_t drv_rpi5_hdmi_audio_packet_slot(uint32_t type);
void drv_rpi5_hdmi_audio_dma_decode(uint32_t cell, struct drv_rpi5_hdmi_audio_dma_request *request);
uint32_t drv_rpi5_hdmi_audio_dma_run(const struct drv_rpi5_hdmi_audio_dma_request *request);
bool drv_rpi5_hdmi_audio_dma_ring(struct drv_rpi5_hdmi_audio_dma_cb *cbs, uint32_t count, uint64_t cb_bus, uint64_t data_bus, uint32_t fragment_bytes, uint64_t fifo_bus, const struct drv_rpi5_hdmi_audio_dma_request *request);
bool drv_rpi5_hdmi_audio_dma_locate(uint32_t source, uint32_t source_info, uint64_t data_bus, uint32_t fragment_bytes, uint32_t count, uint32_t *fragment, uint32_t *pcm_offset);
enum drv_rpi5_hdmi_audio_stepping drv_rpi5_hdmi_audio_stepping(uint32_t hvs_version);
bool drv_rpi5_hdmi_audio_dreq(enum drv_rpi5_hdmi_audio_stepping stepping, uint32_t port, uint32_t tree_cell, uint32_t *dreq);
bool drv_rpi5_hdmi_audio_thresholds(enum drv_rpi5_hdmi_audio_stepping stepping, uint32_t *word);

#endif
