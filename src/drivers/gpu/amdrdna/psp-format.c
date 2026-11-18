/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The words the driver and the security processor exchange: the frames of
 * the secure OS's ring, the commands a frame points at, the reply the
 * secure OS writes into the same buffer, and where memory training reads
 * and writes its data at the top of VRAM.
 *
 * Nothing here touches the hardware.  The callers build a command in
 * ordinary memory and copy it to where the security processor reads it,
 * so the same code runs in the kernel and in the host tests.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-hw.h"

/*
 * A ring frame: the command buffer's address (low, high), the fence's
 * address (low, high) and the value the secure OS writes there when it is
 * done; the rest of the 64 bytes stays zero.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/psp_gfx_if.h (struct psp_gfx_rb_frame).
 */
#define FORMAT_FRAME_COMMAND_LOW_AT	0x00U
#define FORMAT_FRAME_COMMAND_HIGH_AT	0x04U
#define FORMAT_FRAME_FENCE_LOW_AT	0x0cU
#define FORMAT_FRAME_FENCE_HIGH_AT	0x10U
#define FORMAT_FRAME_FENCE_VALUE_AT	0x14U

/*
 * A command buffer: the command's ID at byte 8, its arguments from byte
 * 28, and the reply from byte 864: the status, the session of a loaded
 * application, the address a loaded program took inside the trusted
 * region (low, high), and the size of trusted region the table of
 * contents needs.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/psp_gfx_if.h (struct psp_gfx_cmd_resp,
 * struct psp_gfx_resp).
 */
#define FORMAT_COMMAND_ID_AT		0x08U
#define FORMAT_ARGUMENTS_AT		0x1cU
#define FORMAT_REPLY_AT			0x360U
#define FORMAT_REPLY_STATUS_AT		0x00U
#define FORMAT_REPLY_SESSION_AT		0x04U
#define FORMAT_REPLY_PLACE_LOW_AT	0x08U
#define FORMAT_REPLY_PLACE_HIGH_AT	0x0cU
#define FORMAT_REPLY_REGION_BYTES_AT	0x10U

/*
 * The commands' IDs: load a security application, unload an application,
 * load an IP block's program, set up and tear down the trusted region,
 * read the table of contents, and start the run-list controller's load of
 * the graphics programs.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/psp_gfx_if.h (enum psp_gfx_cmd_id:
 * GFX_CMD_ID_UNLOAD_TA, GFX_CMD_ID_LOAD_ASD, GFX_CMD_ID_SETUP_TMR,
 * GFX_CMD_ID_LOAD_IP_FW, GFX_CMD_ID_DESTROY_TMR, GFX_CMD_ID_LOAD_TOC,
 * GFX_CMD_ID_AUTOLOAD_RLC).
 */
#define FORMAT_COMMAND_UNLOAD		0x00000002U
#define FORMAT_COMMAND_LOAD_APPLICATION	0x00000004U
#define FORMAT_COMMAND_SET_REGION	0x00000005U
#define FORMAT_COMMAND_LOAD_PROGRAM	0x00000006U
#define FORMAT_COMMAND_DROP_REGION	0x00000007U
#define FORMAT_COMMAND_CONTENTS		0x00000020U
#define FORMAT_COMMAND_START_RUN_LIST	0x00000021U

/*
 * The trusted region's flag word: bit 1 says the command carries both the
 * region's GPU address and its physical VRAM address.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/psp_gfx_if.h (struct psp_gfx_cmd_setup_tmr:
 * bitfield.virt_phy_addr) and amdgpu_psp.c (psp_prep_tmr_cmd_buf).
 */
#define FORMAT_REGION_BOTH_ADDRESSES	0x00000002U

/*
 * The frames of the ring in dwords, and the memory-training data: 4 KiB
 * each way, the security processor's copy 32 KiB below the top of VRAM,
 * the driver's copy 1 MiB below the reserve at the top, on a 1 MiB
 * boundary.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_psp.h
 * (GDDR6_MEM_TRAINING_DATA_SIZE_IN_BYTES, GDDR6_MEM_TRAINING_OFFSET) and
 * amdgpu_ttm.c (amdgpu_ttm_training_data_block_init).
 */
#define FORMAT_FRAME_DWORDS		(AMDRDNA_PSP_FRAME_BYTES / 4U)
#define FORMAT_TRAINING_FROM_TOP	0x8000U
#define FORMAT_MIB			0x100000ULL

static void format_put32(uint8_t *bytes, size_t at, uint32_t value);
static void format_arguments(uint8_t *command, unsigned index, uint32_t value);

/*
 * Fills one ring frame: the command buffer's and the fence's addresses,
 * and the value the fence gets when the command is done.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c (psp_ring_cmd_submit).
 */
void
amdrdna_psp_frame(
	uint8_t *frame,
	uint64_t command,
	uint64_t fence,
	uint32_t fence_sequence)
{
	size_t at;

	/* Starts from a zero frame. */
	for (at = 0U; at < AMDRDNA_PSP_FRAME_BYTES; at++)
		frame[at] = 0U;

	/* Where the command lies, where the fence lies, and what the fence will read. */
	format_put32(frame, FORMAT_FRAME_COMMAND_LOW_AT, (uint32_t)command);
	format_put32(frame, FORMAT_FRAME_COMMAND_HIGH_AT, (uint32_t)(command >> 32));
	format_put32(frame, FORMAT_FRAME_FENCE_LOW_AT, (uint32_t)fence);
	format_put32(frame, FORMAT_FRAME_FENCE_HIGH_AT, (uint32_t)(fence >> 32));
	format_put32(frame, FORMAT_FRAME_FENCE_VALUE_AT, fence_sequence);
}

/*
 * Steps the ring's write pointer: from the pointer the secure OS reports
 * (dwords) gives the frame the next command goes in and the pointer to
 * write once it is there.
 *
 * Returns 0, or EINVAL for a pointer that is not at a frame's start inside
 * the ring.
 */
int
amdrdna_psp_ring_step(
	uint32_t write_pointer,
	uint32_t *frame,
	uint32_t *next)
{
	uint32_t ring_dwords;

	/* A pointer inside the ring and at a frame's start. */
	ring_dwords = AMDRDNA_PSP_RING_BYTES / 4U;
	write_pointer %= ring_dwords;
	if (write_pointer % FORMAT_FRAME_DWORDS != 0U)
		return EINVAL;

	/* The frame at the pointer, and the pointer one frame on, wrapping at the ring's end. */
	*frame = write_pointer / FORMAT_FRAME_DWORDS;
	*next = (write_pointer + FORMAT_FRAME_DWORDS) % ring_dwords;

	/* Succeeded: the frame and the next pointer. */
	return 0;
}

/*
 * Starts a command: the whole buffer zero, the command's ID set.
 */
void
amdrdna_psp_command(
	uint8_t *command,
	uint32_t id)
{
	size_t at;

	/* Zero, then the ID. */
	for (at = 0U; at < AMDRDNA_PSP_COMMAND_BYTES; at++)
		command[at] = 0U;
	format_put32(command, FORMAT_COMMAND_ID_AT, id);
}

/*
 * Builds the command that loads one IP block's program: its GPU address
 * (on a 4 KiB boundary), its size and the block's program type.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c (psp_prep_load_ip_fw_cmd_buf).
 */
void
amdrdna_psp_command_program(
	uint8_t *command,
	uint64_t address,
	uint32_t bytes,
	uint32_t type)
{
	/* The command, then address, size and type. */
	amdrdna_psp_command(command, FORMAT_COMMAND_LOAD_PROGRAM);
	format_arguments(command, 0U, (uint32_t)address);
	format_arguments(command, 1U, (uint32_t)(address >> 32));
	format_arguments(command, 2U, bytes);
	format_arguments(command, 3U, type);
}

/*
 * Builds the command that reads the table of contents and answers with
 * the size of trusted region the programs need.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c (psp_prep_load_toc_cmd_buf).
 */
void
amdrdna_psp_command_contents(
	uint8_t *command,
	uint64_t address,
	uint32_t bytes)
{
	/* The command, then the table's address and size. */
	amdrdna_psp_command(command, FORMAT_COMMAND_CONTENTS);
	format_arguments(command, 0U, (uint32_t)address);
	format_arguments(command, 1U, (uint32_t)(address >> 32));
	format_arguments(command, 2U, bytes);
}

/*
 * Builds the command that sets up the trusted region: its GPU address,
 * its size, the flag that says both addresses follow, and its physical
 * VRAM address.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c (psp_prep_tmr_cmd_buf).
 */
void
amdrdna_psp_command_region(
	uint8_t *command,
	uint64_t address,
	uint32_t bytes,
	uint64_t physical)
{
	/* The command, then GPU address, size, flags and physical address. */
	amdrdna_psp_command(command, FORMAT_COMMAND_SET_REGION);
	format_arguments(command, 0U, (uint32_t)address);
	format_arguments(command, 1U, (uint32_t)(address >> 32));
	format_arguments(command, 2U, bytes);
	format_arguments(command, 3U, FORMAT_REGION_BOTH_ADDRESSES);
	format_arguments(command, 4U, (uint32_t)physical);
	format_arguments(command, 5U, (uint32_t)(physical >> 32));
}

/*
 * Builds the command that loads the display security application: its
 * GPU address and size, and no shared buffer.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c (psp_prep_ta_load_cmd_buf,
 * psp_asd_initialize: no shared memory).
 */
void
amdrdna_psp_command_application(
	uint8_t *command,
	uint64_t address,
	uint32_t bytes)
{
	/* The command, then the application's address and size; the shared buffer's three words stay zero. */
	amdrdna_psp_command(command, FORMAT_COMMAND_LOAD_APPLICATION);
	format_arguments(command, 0U, (uint32_t)address);
	format_arguments(command, 1U, (uint32_t)(address >> 32));
	format_arguments(command, 2U, bytes);
}

/*
 * Builds the command that unloads an application by its session.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c (psp_prep_ta_unload_cmd_buf).
 */
void
amdrdna_psp_command_unload(
	uint8_t *command,
	uint32_t session)
{
	/* The command, then the session. */
	amdrdna_psp_command(command, FORMAT_COMMAND_UNLOAD);
	format_arguments(command, 0U, session);
}

/*
 * Builds one of the commands without arguments: start the run-list
 * controller's load, or tear down the trusted region.
 */
void
amdrdna_psp_command_plain(
	uint8_t *command,
	bool tear_down)
{
	/* Tearing down the region, or starting the run-list controller. */
	if (tear_down) {
		amdrdna_psp_command(command, FORMAT_COMMAND_DROP_REGION);
	} else {
		amdrdna_psp_command(command, FORMAT_COMMAND_START_RUN_LIST);
	}
}

/*
 * Reads the reply the secure OS wrote into a command buffer.
 */
void
amdrdna_psp_reply_read(
	const uint8_t *command,
	struct amdrdna_psp_reply *reply)
{
	uint64_t low;
	uint64_t high;

	/* The status, the session, the program's place in the region, and the region size asked for. */
	reply->status = amdrdna_le32(command, FORMAT_REPLY_AT + FORMAT_REPLY_STATUS_AT);
	reply->session = amdrdna_le32(command, FORMAT_REPLY_AT + FORMAT_REPLY_SESSION_AT);
	low = amdrdna_le32(command, FORMAT_REPLY_AT + FORMAT_REPLY_PLACE_LOW_AT);
	high = amdrdna_le32(command, FORMAT_REPLY_AT + FORMAT_REPLY_PLACE_HIGH_AT);
	reply->place = (high << 32) | low;
	reply->region_bytes = amdrdna_le32(command, FORMAT_REPLY_AT + FORMAT_REPLY_REGION_BYTES_AT);
}

/*
 * Finds where memory training exchanges its data: the driver's copy
 * (handed to the boot loader) 1 MiB below the reserve at the top of VRAM,
 * on a 1 MiB boundary, and the security processor's copy 32 KiB below the
 * top.  reserve_bytes of 0 stands for the 64 KiB the block table takes.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c (amdgpu_ttm_reserve_tmr,
 * amdgpu_ttm_training_data_block_init).
 */
void
amdrdna_training_place(
	uint64_t vram_bytes,
	uint64_t reserve_bytes,
	uint64_t *driver_copy,
	uint64_t *processor_copy)
{
	uint64_t place;

	/* Without a reserve from the video BIOS, the block table's 64 KiB. */
	if (reserve_bytes == 0U)
		reserve_bytes = AMDRDNA_TRAINING_DEFAULT_RESERVE;

	/* 1 MiB below the reserve, rounded up to 1 MiB. */
	place = vram_bytes - reserve_bytes - FORMAT_MIB;
	place = (place + FORMAT_MIB - 1U) & ~(FORMAT_MIB - 1U);
	*driver_copy = place;

	/* 32 KiB below the top. */
	*processor_copy = vram_bytes - FORMAT_TRAINING_FROM_TOP;
}

/* Writes a little-endian 32-bit word at a byte offset. */
static void
format_put32(
	uint8_t *bytes,
	size_t at,
	uint32_t value)
{
	/* The four bytes from the least significant. */
	bytes[at] = (uint8_t)value;
	bytes[at + 1U] = (uint8_t)(value >> 8);
	bytes[at + 2U] = (uint8_t)(value >> 16);
	bytes[at + 3U] = (uint8_t)(value >> 24);
}

/* Writes the index-th argument word of a command. */
static void
format_arguments(
	uint8_t *command,
	unsigned index,
	uint32_t value)
{
	/* The arguments are consecutive words from their start. */
	format_put32(command, FORMAT_ARGUMENTS_AT + (size_t)index * 4U, value);
}
