/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Wave5 decoder core's registers and the state its two files share:
 * the core's boot, commands and interrupts (wave5-core.c) and the
 * decoding instances (wave5-decode.c).
 */

#ifndef KERN_DRIVERS_VPU_WAVE5_PRIVATE_H
#define KERN_DRIVERS_VPU_WAVE5_PRIVATE_H

#include <stdint.h>

#include <kern/lock.h>
#include <kern/pmem.h>
#include <kern/waitq.h>

#include "drivers/vpu/wave5.h"

/*
 * The core's host registers: the processor's state, the fast I/O window
 * into the bus fabric, the interrupt lines between the host and the
 * firmware, the software reset, the code remapping, the busy flag, the
 * hardware configuration and the product number.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-regdefine.h
 * (W5_PO_CONF 0x0 to W5_VPU_RET_VPU_CONFIG1 0x9c, W5_PRODUCT_NAME 0x1040,
 * W5_PRODUCT_NUMBER 0x1044).
 */
#define VPU_POWER_ON_CONFIG	0x0000U
#define VPU_PROGRAM_COUNTER	0x0004U
#define VPU_FAST_IO_CONTROL	0x0020U
#define VPU_FAST_IO_DATA	0x0024U
#define VPU_IRQ_REASON_HOST	0x0030U
#define VPU_IRQ_REASON_CLEAR	0x0034U
#define VPU_HOST_IRQ		0x0038U
#define VPU_IRQ_CLEAR		0x003cU
#define VPU_IRQ_STATUS		0x0044U
#define VPU_IRQ_ENABLE		0x0048U
#define VPU_IRQ_REASON		0x004cU
#define VPU_RESET_REQUEST	0x0050U
#define VPU_RESET_STATUS	0x0054U
#define VPU_REMAP_CONTROL	0x0060U
#define VPU_REMAP_VIRTUAL	0x0064U
#define VPU_REMAP_PHYSICAL	0x0068U
#define VPU_REMAP_START		0x006cU
#define VPU_BUSY		0x0070U
#define VPU_CONFIG0		0x0098U
#define VPU_CONFIG1		0x009cU
#define VPU_PRODUCT_NAME	0x1040U
#define VPU_PRODUCT_NUMBER	0x1044U

/*
 * The command block: the command, its option, the firmware's verdict and
 * reason and the instance a command is for; the argument registers from
 * 0x110 up mean what each command makes them mean.  The queue status and
 * the per-instance completion masks follow at the block's end.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-regdefine.h
 * (W5_CMD_REG_BASE 0x100, W5_CMD_REG_END 0x200, W5_COMMAND 0x100,
 * W5_COMMAND_OPTION 0x104, W5_RET_SUCCESS 0x108, W5_RET_FAIL_REASON
 * 0x10c, W5_CMD_INSTANCE_INFO 0x110, W5_RET_QUEUE_STATUS 0x1e0,
 * W5_RET_QUEUE_CMD_DONE_INST 0x1e8, W5_RET_SEQ_DONE_INSTANCE_INFO 0x1fc).
 */
#define VPU_COMMAND_FIRST	0x0100U
#define VPU_COMMAND_END		0x0200U
#define VPU_COMMAND		0x0100U
#define VPU_OPTION		0x0104U
#define VPU_SUCCESS		0x0108U
#define VPU_FAIL_REASON		0x010cU
#define VPU_INSTANCE		0x0110U
#define VPU_QUEUE_STATUS	0x01e0U
#define VPU_PICTURE_DONE	0x01e8U
#define VPU_SEQUENCE_DONE	0x01fcU

/*
 * The commands, the queries (the option of the query command) and the
 * firmware's reason for a command it could not take because the core is
 * still working on the instance.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-regdefine.h
 * (enum W5_VPU_COMMAND, enum query_opt) and wave5-vpuerror.h
 * (WAVE5_SYSERR_VPU_STILL_RUNNING 0x1000).
 */
#define VPU_RUN_INIT		0x0001U
#define VPU_RUN_CREATE		0x0008U
#define VPU_RUN_FLUSH		0x0010U
#define VPU_RUN_DESTROY		0x0020U
#define VPU_RUN_SEQUENCE	0x0040U
#define VPU_RUN_BUFFERS		0x0080U
#define VPU_RUN_PICTURE		0x0100U
#define VPU_RUN_QUERY		0x4000U
#define VPU_ASK_IDENTITY	0U
#define VPU_ASK_RESULT		2U
#define VPU_ASK_DISPLAY		3U
#define VPU_ASK_READ_POSITION	5U
#define VPU_ASK_SET_POSITION	8U
#define VPU_REASON_STILL_RUNNING	0x00001000U

/*
 * The interrupt reasons a decoder waits for: a sequence set up, a picture
 * decoded, the bitstream run dry.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-vpuapi.h
 * (INT_WAVE5_INIT_SEQ 6, INT_WAVE5_DEC_PIC 8, INT_WAVE5_BSBUF_EMPTY 15).
 */
#define VPU_IRQ_SEQUENCE	(1U << 6)
#define VPU_IRQ_PICTURE		(1U << 8)
#define VPU_IRQ_STREAM_EMPTY	(1U << 15)

/* How long a command, a sequence and a picture are waited for, in milliseconds. */
#define VPU_COMMAND_MS		100U
#define VPU_BOOT_MS		1000U
#define VPU_DECODE_MS		500U

/* The most instances one core holds. */
#define VPU_MAX_INSTANCES	8U

/*
 * One run of physically contiguous memory the core reaches, with the
 * uncached view the host touches it through.
 */
struct wave5_memory {
	struct kern_pmem run;
	void *view;
};

/*
 * One registered frame buffer pair's compressed side: the picture, the
 * luma and chroma offset tables of its compression and its co-located
 * motion vectors.  The instance owns them for the sequence's life.
 */
struct wave5_compressed {
	struct wave5_memory pixels;
	struct wave5_memory luma_table;
	struct wave5_memory chroma_table;
	struct wave5_memory motion;
};

/*
 * One decoding instance of the firmware.
 *
 * Read and write are the bus addresses the next command reads the ring
 * between.  The compressed buffers and the task buffer exist from
 * drv_wave5_register() on.  An instance the firmware would not destroy
 * waits on the core's orphan list for drv_wave5_detach().
 */
struct drv_wave5_stream {
	struct drv_wave5 *core;
	unsigned codec;
	unsigned id;
	uint32_t standard;
	struct wave5_memory work;
	struct wave5_memory ring;
	size_t ring_size;
	uint32_t read;
	uint32_t write;
	int sequenced;
	struct drv_wave5_sequence sequence;
	uint32_t vlc_size;
	uint32_t parameter_size;
	struct wave5_memory task;
	unsigned count;
	uint32_t linear_stride;
	uint32_t compressed_stride;
	uint32_t compressed_luma;
	struct wave5_compressed compressed[DRV_WAVE5_MAX_BUFFERS];
	struct drv_wave5_stream *next_orphan;
};

/*
 * One booted Wave5 core.
 *
 * The command lock serializes every command, since the firmware takes one
 * at a time through the same registers.  The interrupt lock guards the
 * completion masks, the reasons and the count, which the interrupt
 * handler and a polling waiter both fill; the waiters sleep on the
 * queue.  Stopped is set once by drv_wave5_stop() and refuses every later
 * command.
 */
struct drv_wave5 {
	struct drv_wave5_place place;
	volatile uint8_t *registers;
	struct wave5_memory common;
	struct drv_wave5_identity identity;
	struct mutex command_lock;
	struct spinlock irq_lock;
	struct wait_queue irq_wait;
	uint32_t sequence_done;
	uint32_t picture_done;
	uint32_t reasons;
	unsigned interrupts;
	unsigned polled;
	int irq_installed;
	int stopped;
	uint32_t instances;
	struct drv_wave5_stream *orphans;
};

/* Reads and writes one host register. */
uint32_t drv_wave5_core_read(struct drv_wave5 *core, uint32_t offset);
void drv_wave5_core_write(struct drv_wave5 *core, uint32_t offset, uint32_t value);

/*
 * Issues one command for an instance (NULL for the core's own) under the
 * command lock the caller holds, and waits until the firmware took it.
 *
 * With check set, a command the firmware refused reports EIO and its
 * reason, or EBUSY when the instance is still running.
 */
int drv_wave5_core_command(struct drv_wave5 *core, const struct drv_wave5_stream *stream, uint32_t command, int check, uint32_t *reason);

/* Issues one query under the command lock the caller holds. */
int drv_wave5_core_query(struct drv_wave5 *core, const struct drv_wave5_stream *stream, uint32_t option);

/* Forgets an instance's completions from before the command about to be issued. */
void drv_wave5_core_forget(struct drv_wave5 *core, const struct drv_wave5_stream *stream);

/*
 * Waits for an instance's sequence (sequence set) or picture completion,
 * taking the interrupt or, failing it, polling the core every
 * millisecond.  ETIMEDOUT after the bound.
 */
int drv_wave5_core_wait(struct drv_wave5 *core, const struct drv_wave5_stream *stream, int sequence, unsigned milliseconds);

/* Takes zeroed contiguous memory the core reaches, with its uncached view. */
int drv_wave5_core_take(struct drv_wave5 *core, size_t size, struct wave5_memory *memory);

/* Gives such memory back; an empty one is left alone. */
void drv_wave5_core_give(struct wave5_memory *memory);

/* Reports a memory's bus address as the core's 32-bit registers take it. */
uint32_t drv_wave5_core_address(const struct wave5_memory *memory);

/* Frees an instance's memory, once the core no longer uses it. */
void drv_wave5_stream_free(struct drv_wave5_stream *stream);

#endif
