/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Chips&Media Wave5 video decoder core (the Wave511 of the JH7110).
 *
 * The core is a processor of its own running firmware that parses the
 * bitstream and keeps the reference pictures itself.  The host boots the
 * firmware, then talks to it through command registers: one instance per
 * decoding session, a sequence set up from the first picture, frame
 * buffers registered in pairs (a compressed one the core references and a
 * linear one it writes the picture to), and one command per picture.
 *
 * The board's glue powers the core, starts its clocks and releases its
 * resets before drv_wave5_attach() and holds it in reset after
 * drv_wave5_stop(); the core's code never touches power, clocks or resets.
 * Every call that issues a command sleeps, so it is made from a thread.
 */

#ifndef KERN_DRIVERS_VPU_WAVE5_H
#define KERN_DRIVERS_VPU_WAVE5_H

#include <stddef.h>
#include <stdint.h>

/* The codecs an instance decodes. */
#define DRV_WAVE5_H264		0U
#define DRV_WAVE5_H265		1U

/* The most frame buffer pairs one instance registers. */
#define DRV_WAVE5_MAX_BUFFERS	31U

/* How drv_wave5_load() moves the core's read position to the new bytes. */
#define DRV_WAVE5_REWIND_QUERY		0U
#define DRV_WAVE5_REWIND_REGISTER	1U

/* A decoded index that names no frame buffer: nothing decoded, or a skipped picture. */
#define DRV_WAVE5_INDEX_NONE	(-1)
#define DRV_WAVE5_INDEX_SKIP	(-2)

struct drv_wave5;
struct drv_wave5_stream;

/*
 * Where one core is and what its bus master reaches.
 *
 * The secondary memory is the core's on-chip line buffer, named by the
 * address the core itself sees it at.
 */
struct drv_wave5_place {
	uint64_t registers;
	uint64_t register_size;
	uint32_t interrupt;
	uint64_t dma_limit;
	uint32_t sram_address;
	uint32_t sram_size;
};

/*
 * What a booted core reports about itself.
 *
 * The names are four characters, most significant first, as the
 * firmware's identity query gives them.
 */
struct drv_wave5_identity {
	uint32_t product_code;
	char product_name[5];
	uint32_t product_version;
	uint32_t firmware_version;
	uint32_t customer;
	uint32_t standards[2];
	uint32_t features;
	uint32_t configuration[2];
};

/*
 * What the core found in a sequence's first picture.
 *
 * Success is the firmware's own verdict; error is its reason when it is
 * zero.  The sizes are the coded picture's, the crop what is visible.
 */
struct drv_wave5_sequence {
	uint32_t success;
	uint32_t error;
	uint32_t width;
	uint32_t height;
	uint32_t required;
	uint32_t crop_left;
	uint32_t crop_right;
	uint32_t crop_top;
	uint32_t crop_bottom;
	uint32_t luma_depth;
	uint32_t chroma_depth;
	uint32_t profile;
	uint32_t read_position;
};

/*
 * One linear frame buffer: the bus addresses of an NV12 picture's luma
 * plane and its interleaved chroma plane.
 */
struct drv_wave5_picture {
	uint64_t luma;
	uint64_t chroma;
};

/*
 * What one picture's decode reported.
 *
 * Decoded is the frame buffer the picture went into and display the one
 * the core gives out now, each an index or a negative DRV_WAVE5_INDEX_*.
 * Ticks is the core's time for the picture in its own cycles, wall the
 * host's time from the command to the result in microseconds.  Queue is
 * the firmware's queue status after the command and after the result
 * (queued commands in bits 16 to 23, waiting reports below).
 */
struct drv_wave5_result {
	uint32_t success;
	uint32_t error;
	uint32_t warning;
	int32_t decoded;
	int32_t display;
	int32_t order;
	uint32_t type;
	uint32_t notification;
	uint32_t display_flags;
	uint32_t width;
	uint32_t height;
	uint64_t ticks;
	uint64_t wall;
	uint32_t read_position;
	uint32_t queue[2];
};

/*
 * Boots the firmware on a powered core and starts taking its interrupts.
 *
 * The firmware is copied; the caller keeps its bytes.  ENODEV means the
 * core is not a Wave5 decoder the driver knows; ETIMEDOUT that the
 * firmware did not answer.  On failure nothing is left running.
 */
int drv_wave5_attach(const struct drv_wave5_place *place, const uint8_t *firmware, size_t size, struct drv_wave5 **core);

/*
 * Stops using a core: no command is issued and no interrupt taken after
 * it returns, so the glue may hold the core in reset and power it off.
 */
void drv_wave5_stop(struct drv_wave5 *core);

/*
 * Frees a stopped core and every instance still open on it.
 *
 * The core must be held in reset, since the memory it may still be
 * writing is returned.
 */
void drv_wave5_detach(struct drv_wave5 *core);

/* Reports what the booted core said about itself. */
void drv_wave5_identity(const struct drv_wave5 *core, struct drv_wave5_identity *identity);

/*
 * Reports how many completions the host took as interrupts and how many a
 * waiter found by polling the core instead, and the reasons they carried,
 * ORed together since the last call.
 */
void drv_wave5_interrupts(struct drv_wave5 *core, unsigned *taken, unsigned *polled, uint32_t *reasons);

/* Reports the reason the firmware gave for the last command it refused. */
uint32_t drv_wave5_last_reason(struct drv_wave5 *core);

/*
 * Opens an instance that decodes one codec from a bitstream ring of the
 * given size (a multiple of 1 KiB).
 */
int drv_wave5_open(struct drv_wave5 *core, unsigned codec, size_t ring_size, struct drv_wave5_stream **stream);

/*
 * Destroys an instance and frees its memory.
 *
 * When the firmware does not let the instance go, the memory is kept
 * until drv_wave5_detach() and EIO is reported.
 */
int drv_wave5_close(struct drv_wave5_stream *stream);

/*
 * Puts one picture's bytes at the start of the ring as the bytes the next
 * command reads.
 *
 * Rewind names how the core is told to start reading there again: with
 * the read position query, or only with the read position the next
 * command carries.  EFBIG means the bytes do not fit.
 */
int drv_wave5_load(struct drv_wave5_stream *stream, const uint8_t *bytes, size_t length, unsigned rewind);

/*
 * Puts one picture's bytes right after the bytes the last command read, as
 * the bytes the next command reads; the core's own read position is
 * already there.  EFBIG means the ring has no room left before its end.
 */
int drv_wave5_append(struct drv_wave5_stream *stream, const uint8_t *bytes, size_t length);

/*
 * Reports where the ring's bytes start: the ring's kernel view and its
 * bus address, for a caller that tests what the core reads.
 */
void drv_wave5_ring(struct drv_wave5_stream *stream, uint8_t **view, uint64_t *address);

/*
 * Sets the instance's sequence up from the loaded picture.
 *
 * The picture is not consumed for decoding: load it again before
 * drv_wave5_decode().  EIO means the firmware refused the sequence; the
 * sequence's success and error say why.
 */
int drv_wave5_init_sequence(struct drv_wave5_stream *stream, struct drv_wave5_sequence *sequence);

/*
 * Registers count frame buffer pairs: the caller's linear buffers, all of
 * the given stride, and compressed buffers the instance allocates itself
 * for the sequence's size.
 */
int drv_wave5_register(struct drv_wave5_stream *stream, const struct drv_wave5_picture *linear, unsigned count, uint32_t stride);

/*
 * Points one registered pair's linear buffer at another picture, leaving
 * its compressed buffer as it is.
 */
int drv_wave5_replace(struct drv_wave5_stream *stream, unsigned index, const struct drv_wave5_picture *linear);

/*
 * Sets and clears frame buffers' display flags: a set flag keeps the core
 * from decoding into that pair.  Reports the flags afterwards.
 */
int drv_wave5_display_flags(struct drv_wave5_stream *stream, uint32_t set, uint32_t clear, uint32_t *flags);

/*
 * Decodes the loaded picture and waits for its result.
 *
 * In decode order: the core is told to give each picture out as soon as
 * it is decoded.  EIO means the command was refused; a decode the
 * firmware reports as failed still returns 0 with the result's success
 * clear.  ETIMEDOUT means the completion did not come; the result is what
 * the firmware answered then.
 */
int drv_wave5_decode(struct drv_wave5_stream *stream, struct drv_wave5_result *result);

/* Asks the core where it is reading in the ring now, as a bus address. */
int drv_wave5_read_position(struct drv_wave5_stream *stream, uint32_t *position);

#endif
