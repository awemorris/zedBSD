/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The interface between the host and the Mali command stream firmware.
 *
 * The firmware publishes it in the shared section of the
 * microcontroller's address space: a global block first, then one block
 * per command stream group slot, each followed by the blocks of the
 * group's streams.  Every block has three parts: what the firmware
 * describes (control), what the host asks (input) and what the firmware
 * answers (output).  A request is a set of bits the host flips in its
 * input word; the firmware acknowledges it by flipping the same bits of
 * the matching output word.  The layout reader here only reads and
 * checks, so it runs unchanged in a host test.
 */

#ifndef KERN_DRIVERS_GPU_MALI_INTERFACE_H
#define KERN_DRIVERS_GPU_MALI_INTERFACE_H

#include <stdint.h>

/* The most group slots and streams of one group the reader accepts. */
#define MALI_GROUPS_MAX			31U
#define MALI_STREAMS_MAX		32U

/*
 * The fewest group slots and streams of a group a working firmware
 * offers; fewer means the firmware did not finish its start.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_fw.c (MIN_CSGS,
 * MIN_CS_PER_CSG, MAX_CSGS, MAX_CS_PER_CSG).
 */
#define MALI_GROUPS_MIN			3U
#define MALI_STREAMS_MIN		8U

/*
 * Where the groups' blocks begin after the global one, and where a
 * group's first stream block begins after the group's own.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_fw.c
 * (CSF_GROUP_CONTROL_OFFSET, CSF_STREAM_CONTROL_OFFSET).
 */
#define MALI_IFACE_GROUP_OFFSET		0x1000U
#define MALI_IFACE_STREAM_OFFSET	0x40U

/*
 * The version word: major in the top byte, minor in the next, patch in
 * the low half.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_fw.c (CSF_IFACE_VERSION,
 * CSF_IFACE_VERSION_MAJOR, _MINOR, _PATCH).
 */
#define MALI_IFACE_MAJOR_SHIFT		24U
#define MALI_IFACE_MINOR_SHIFT		16U
#define MALI_IFACE_BYTE_MASK		0xffU
#define MALI_IFACE_PATCH_MASK		0xffffU

/*
 * The global block, as byte offsets inside each of its parts.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_fw.h (the field order of
 * struct panthor_fw_global_control_iface, panthor_fw_global_input_iface
 * and panthor_fw_global_output_iface).
 */
#define MALI_GLB_CONTROL_VERSION	0x00U
#define MALI_GLB_CONTROL_FEATURES	0x04U
#define MALI_GLB_CONTROL_INPUT		0x08U
#define MALI_GLB_CONTROL_OUTPUT		0x0cU
#define MALI_GLB_CONTROL_GROUPS		0x10U
#define MALI_GLB_CONTROL_GROUP_STRIDE	0x14U
#define MALI_GLB_CONTROL_BYTES		0x20U
#define MALI_GLB_INPUT_REQUEST		0x00U
#define MALI_GLB_INPUT_ACK_IRQ_MASK	0x04U
#define MALI_GLB_INPUT_DOORBELL		0x08U
#define MALI_GLB_INPUT_PROGRESS_TIMER	0x10U
#define MALI_GLB_INPUT_POWEROFF_TIMER	0x14U
#define MALI_GLB_INPUT_CORE_MASK	0x18U
#define MALI_GLB_INPUT_IDLE_TIMER	0x80U
#define MALI_GLB_INPUT_BYTES		0x84U
#define MALI_GLB_OUTPUT_ACK		0x00U
#define MALI_GLB_OUTPUT_DOORBELL_ACK	0x08U
#define MALI_GLB_OUTPUT_HALT_STATUS	0x10U
#define MALI_GLB_OUTPUT_BYTES		0x1cU

/*
 * The global request bits the driver uses.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_fw.h (GLB_CFG_PROGRESS_TIMER,
 * GLB_CFG_ALLOC_EN, GLB_CFG_POWEROFF_TIMER, GLB_COUNTER_EN, GLB_PING,
 * GLB_IDLE_EN, GLB_IDLE, GLB_STATE_MASK, GLB_REQ_MASK).
 */
#define MALI_GLB_PROGRESS_TIMER		0x00000002U
#define MALI_GLB_ALLOCATE		0x00000004U
#define MALI_GLB_POWEROFF_TIMER		0x00000008U
#define MALI_GLB_COUNTER		0x00000080U
#define MALI_GLB_PING			0x00000100U
#define MALI_GLB_IDLE_ENABLE		0x00000400U
#define MALI_GLB_STATE_MASK		0x00007000U
#define MALI_GLB_IDLE			0x04000000U
#define MALI_GLB_REQUEST_MASK		0x000007ffU

/*
 * A timer word: the count in the low 31 bits, and bit 31 when it counts
 * the GPU's own cycles rather than the system timer's.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_fw.h (GLB_TIMER_VAL,
 * GLB_TIMER_SOURCE_GPU_COUNTER); panthor_fw.c (panthor_fw_conv_timeout:
 * the count is in units of 1024 cycles).
 */
#define MALI_TIMER_COUNT_MASK		0x7fffffffU
#define MALI_TIMER_GPU_COUNTER		0x80000000U
#define MALI_TIMER_SCALE_SHIFT		10U

/*
 * A group slot's block.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_fw.h (the field order of
 * struct panthor_fw_csg_control_iface, panthor_fw_csg_input_iface and
 * panthor_fw_csg_output_iface).
 */
#define MALI_CSG_CONTROL_FEATURES	0x00U
#define MALI_CSG_CONTROL_INPUT		0x04U
#define MALI_CSG_CONTROL_OUTPUT		0x08U
#define MALI_CSG_CONTROL_SUSPEND	0x0cU
#define MALI_CSG_CONTROL_PROTM_SUSPEND	0x10U
#define MALI_CSG_CONTROL_STREAMS	0x14U
#define MALI_CSG_CONTROL_STREAM_STRIDE	0x18U
#define MALI_CSG_CONTROL_BYTES		0x1cU
#define MALI_CSG_INPUT_REQUEST		0x00U
#define MALI_CSG_INPUT_ACK_IRQ_MASK	0x04U
#define MALI_CSG_INPUT_DOORBELL		0x08U
#define MALI_CSG_INPUT_ALLOW_COMPUTE	0x20U
#define MALI_CSG_INPUT_ALLOW_FRAGMENT	0x28U
#define MALI_CSG_INPUT_ALLOW_OTHER	0x30U
#define MALI_CSG_INPUT_ENDPOINT		0x34U
#define MALI_CSG_INPUT_ENDPOINT64	0x38U
#define MALI_CSG_INPUT_SUSPEND		0x40U
#define MALI_CSG_INPUT_PROTM_SUSPEND	0x48U
#define MALI_CSG_INPUT_CONFIG		0x50U
#define MALI_CSG_INPUT_BYTES		0x58U
#define MALI_CSG_OUTPUT_ACK		0x00U
#define MALI_CSG_OUTPUT_DOORBELL_ACK	0x08U
#define MALI_CSG_OUTPUT_STATE		0x18U
#define MALI_CSG_OUTPUT_BYTES		0x20U

/*
 * A group's request bits and its endpoint word.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_fw.h (CSG_STATE_MASK,
 * CSG_STATE_START, CSG_ENDPOINT_CONFIG, CSG_EP_REQ_COMPUTE,
 * CSG_EP_REQ_FRAGMENT, CSG_EP_REQ_TILER, CSG_EP_REQ_PRIORITY);
 * panthor_fw.c (panthor_fw_has_64bit_ep_req: interface 4.0.0 on).
 */
#define MALI_CSG_STATE_MASK		0x00000007U
#define MALI_CSG_STATE_START		0x00000001U
#define MALI_CSG_ENDPOINT_CONFIG	0x00000010U
#define MALI_CSG_ENDPOINT_FRAGMENT_SHIFT	8U
#define MALI_CSG_ENDPOINT_TILER_SHIFT	16U
#define MALI_CSG_ENDPOINT_PRIORITY_SHIFT	28U
#define MALI_CSG_ENDPOINT_CORES_MASK	0xffU
#define MALI_CSG_ENDPOINT_TILERS_MASK	0xfU
#define MALI_CSG_PRIORITY_HIGHEST	0xfU
#define MALI_IFACE_ENDPOINT64_VERSION	0x04000000U

/*
 * A stream's block.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_fw.h (the field order of
 * struct panthor_fw_cs_control_iface, panthor_fw_cs_input_iface and
 * panthor_fw_cs_output_iface).
 */
#define MALI_CS_CONTROL_FEATURES	0x00U
#define MALI_CS_CONTROL_INPUT		0x04U
#define MALI_CS_CONTROL_OUTPUT		0x08U
#define MALI_CS_CONTROL_BYTES		0x0cU
#define MALI_CS_INPUT_REQUEST		0x00U
#define MALI_CS_INPUT_CONFIG		0x04U
#define MALI_CS_INPUT_ACK_IRQ_MASK	0x0cU
#define MALI_CS_INPUT_RING_BASE		0x10U
#define MALI_CS_INPUT_RING_SIZE		0x18U
#define MALI_CS_INPUT_RING_INPUT	0x30U
#define MALI_CS_INPUT_RING_OUTPUT	0x38U
#define MALI_CS_INPUT_BYTES		0x58U
#define MALI_CS_OUTPUT_ACK		0x00U
#define MALI_CS_OUTPUT_COMMAND_POINTER	0x40U
#define MALI_CS_OUTPUT_FAULT		0x80U
#define MALI_CS_OUTPUT_FATAL		0x84U
#define MALI_CS_OUTPUT_BYTES		0xd8U

/*
 * A stream's request bits, its configuration word, and the features word
 * whose low byte is one less than the stream's work registers.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_fw.h (CS_STATE_MASK,
 * CS_STATE_START, CS_IDLE_SYNC_WAIT, CS_IDLE_EMPTY, CS_CONFIG_PRIORITY,
 * CS_CONFIG_DOORBELL, CS_FEATURES_WORK_REGS).
 */
#define MALI_CS_STATE_MASK		0x00000007U
#define MALI_CS_STATE_START		0x00000001U
#define MALI_CS_IDLE_SYNC_WAIT		0x00000100U
#define MALI_CS_IDLE_EMPTY		0x00000400U
#define MALI_CS_CONFIG_DOORBELL_SHIFT	8U
#define MALI_CS_FEATURES_REGISTERS_MASK	0xffU

/*
 * The ring buffer's two small blocks the host and the firmware share:
 * where the host has written to and where the firmware has read to.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_fw.h (struct
 * panthor_fw_ringbuf_input_iface, panthor_fw_ringbuf_output_iface).
 */
#define MALI_RING_INPUT_INSERT		0x00U
#define MALI_RING_INPUT_EXTRACT		0x08U
#define MALI_RING_OUTPUT_EXTRACT	0x00U
#define MALI_RING_OUTPUT_ACTIVE		0x08U

/*
 * One stream slot's interface: the firmware's description (control),
 * what the host asks (input) and what the firmware answers (output).
 * Each pointer is into the shared section's view the reader was given,
 * valid while that view is.
 */
struct mali_stream_interface {
	volatile uint8_t *control;
	volatile uint8_t *input;
	volatile uint8_t *output;
	uint32_t features;
};

/*
 * One group slot's interface, in the same three parts, with what it
 * reports and the interfaces of its streams.
 */
struct mali_group_interface {
	volatile uint8_t *control;
	volatile uint8_t *input;
	volatile uint8_t *output;
	uint32_t features;
	uint32_t suspend_bytes;
	uint32_t protected_suspend_bytes;
	uint32_t stream_count;
	uint32_t stream_stride;
	struct mali_stream_interface streams[MALI_STREAMS_MAX];
};

/*
 * The firmware's whole interface, as the reader found it: the global
 * block's three parts, the version and features it reports, and every
 * group slot it offers.
 */
struct mali_interfaces {
	volatile uint8_t *control;
	volatile uint8_t *input;
	volatile uint8_t *output;
	uint32_t version;
	uint32_t features;
	uint32_t group_count;
	uint32_t group_stride;
	struct mali_group_interface groups[MALI_GROUPS_MAX];
};

/*
 * Reads and checks the interface in a view of the shared section, which
 * the microcontroller sees at shared_address and which is bytes long.
 */
int mali_iface_read(volatile uint8_t *shared, uint32_t shared_address, uint32_t bytes, struct mali_interfaces *interfaces);

/*
 * Builds the input word that flips the bits of mask away from what the
 * firmware last acknowledged, keeping the other bits of the request.
 */
uint32_t mali_iface_toggle(uint32_t request, uint32_t acknowledge, uint32_t mask);

/*
 * Builds the input word that sets the bits of mask to those of value,
 * keeping the other bits of the request.
 */
uint32_t mali_iface_update(uint32_t request, uint32_t value, uint32_t mask);

/*
 * Reports whether the firmware acknowledged every bit of mask: nonzero
 * when the acknowledge word matches the request word under the mask.
 */
int mali_iface_acked(uint32_t request, uint32_t acknowledge, uint32_t mask);

/*
 * Builds a timer word for a time in microseconds, counted on the GPU's
 * clock of rate hertz.
 */
uint32_t mali_iface_timer(uint64_t microseconds, uint64_t rate);

/*
 * Reads one 32-bit word of an interface part.
 */
uint32_t mali_iface_read32(const volatile uint8_t *part, uint32_t offset);

/*
 * Reads one 64-bit word of an interface part, low half first.
 */
uint64_t mali_iface_read64(const volatile uint8_t *part, uint32_t offset);

/*
 * Writes one 32-bit word of an interface part.
 */
void mali_iface_write32(volatile uint8_t *part, uint32_t offset, uint32_t value);

/*
 * Writes one 64-bit word of an interface part, low half first.
 */
void mali_iface_write64(volatile uint8_t *part, uint32_t offset, uint64_t value);

#endif
