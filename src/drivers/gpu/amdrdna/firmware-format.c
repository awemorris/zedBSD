/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Reading the headers of the firmware files the security processor loads.
 *
 * Every file starts with the same header, which says how large the file
 * is, how large the header is, its layout version, and where the program
 * the file carries lies in it.  Some kinds of file carry more than one
 * piece: the security processor's own file holds its boot pieces, and the
 * compute and run-list processors' files hold parts the security processor
 * takes one at a time.  This file finds the pieces and checks that every
 * one lies inside the file; it reads nothing of the programs themselves.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-hw.h"

/*
 * The header every file starts with: the file's size, the header's size,
 * the layout version (major, minor), the program's version, its size and
 * its offset in the file; the header ends at byte 32.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.h (struct
 * common_firmware_header).
 */
#define FORMAT_FILE_BYTES_AT		0x00U
#define FORMAT_HEADER_BYTES_AT		0x04U
#define FORMAT_MAJOR_AT			0x08U
#define FORMAT_MINOR_AT			0x0aU
#define FORMAT_VERSION_AT		0x10U
#define FORMAT_PROGRAM_BYTES_AT		0x14U
#define FORMAT_PROGRAM_AT		0x18U
#define FORMAT_COMMON_BYTES		0x20U

/*
 * The security processor's file (layout 1.1): after the common header,
 * three entries of a version, an offset and a size, for the secure OS, the
 * table of contents and the key database.  The offsets count from the
 * program's start, and the system driver is the program's first bytes,
 * up to the secure OS.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.h (struct
 * psp_firmware_header_v1_0, struct psp_firmware_header_v1_1, struct
 * psp_fw_legacy_bin_desc) and drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c
 * (psp_init_sos_base_fw, psp_init_sos_microcode).
 */
#define FORMAT_SECURE_OS_OFFSET_AT	0x24U
#define FORMAT_SECURE_OS_BYTES_AT	0x28U
#define FORMAT_CONTENTS_OFFSET_AT	0x30U
#define FORMAT_CONTENTS_BYTES_AT	0x34U
#define FORMAT_KEYS_OFFSET_AT		0x3cU
#define FORMAT_KEYS_BYTES_AT		0x40U
#define FORMAT_SECURE_HEADER_BYTES	0x44U

/*
 * The command processors' files (layout 1.0): after the common header,
 * the feature version, then the jump table's offset from the program's
 * start and its size, both in dwords.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.h (struct
 * gfx_firmware_header_v1_0) and amdgpu_ucode.c
 * (amdgpu_ucode_init_single_fw: the compute program without its jump
 * table, and the jump table on its own).
 */
#define FORMAT_JUMPS_OFFSET_AT		0x24U
#define FORMAT_JUMPS_DWORDS_AT		0x28U
#define FORMAT_COMMAND_HEADER_BYTES	0x2cU

/*
 * The run-list controller's file (layout 2.1): the sizes and offsets of
 * the three save-and-restore lists, the offsets counted from the file's
 * start; the header is 156 bytes.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.h (struct
 * rlc_firmware_header_v2_0, struct rlc_firmware_header_v2_1) and
 * drivers/gpu/drm/amd/amdgpu/amdgpu_rlc.c
 * (amdgpu_gfx_rlc_init_microcode_v2_1).
 * derived: 0x20 + 18 dwords = 0x68 for the direct list's length, then
 * three groups of four dwords from 0x6c.
 */
#define FORMAT_CONTROL_BYTES_AT		0x74U
#define FORMAT_CONTROL_OFFSET_AT	0x78U
#define FORMAT_GPM_BYTES_AT		0x84U
#define FORMAT_GPM_OFFSET_AT		0x88U
#define FORMAT_SRM_BYTES_AT		0x94U
#define FORMAT_SRM_OFFSET_AT		0x98U
#define FORMAT_RUN_LIST_HEADER_BYTES	0x9cU

static int format_common(const uint8_t *bytes, size_t length, size_t header_least, struct amdrdna_firmware_layout *layout);
static int format_secure(const uint8_t *bytes, size_t length, struct amdrdna_firmware_layout *layout);
static int format_compute(const uint8_t *bytes, size_t length, struct amdrdna_firmware_layout *layout);
static int format_run_list(const uint8_t *bytes, size_t length, struct amdrdna_firmware_layout *layout);
static int format_piece(struct amdrdna_firmware_layout *layout, size_t length, uint64_t offset, uint64_t size);

/*
 * Finds the pieces of one firmware file of a known kind.
 *
 * Returns 0 with the layout filled, or EINVAL for a file whose header is
 * not that kind's, does not match the file's size, or names a piece
 * outside the file.  The pieces are in the order the kind's indexes name.
 */
int
amdrdna_firmware_layout(
	enum amdrdna_firmware_kind kind,
	const uint8_t *bytes,
	size_t length,
	struct amdrdna_firmware_layout *layout)
{
	uint64_t program;
	uint64_t program_bytes;
	int error;

	/* Reads the common header with the least size each kind's header has. */
	switch (kind) {
	case AMDRDNA_FIRMWARE_SECURE:
		error = format_common(bytes, length, FORMAT_SECURE_HEADER_BYTES, layout);
		break;
	case AMDRDNA_FIRMWARE_COMPUTE:
		error = format_common(bytes, length, FORMAT_COMMAND_HEADER_BYTES, layout);
		break;
	case AMDRDNA_FIRMWARE_RUN_LIST:
		error = format_common(bytes, length, FORMAT_RUN_LIST_HEADER_BYTES, layout);
		break;
	case AMDRDNA_FIRMWARE_WHOLE:
		error = format_common(bytes, length, FORMAT_COMMON_BYTES, layout);
		break;
	default:
		error = EINVAL;
		break;
	}

	/* Refuses a file whose common header is not sound. */
	if (error != 0)
		return error;

	/* Cuts the file into the kind's pieces. */
	switch (kind) {
	case AMDRDNA_FIRMWARE_SECURE:
		error = format_secure(bytes, length, layout);
		break;
	case AMDRDNA_FIRMWARE_COMPUTE:
		error = format_compute(bytes, length, layout);
		break;
	case AMDRDNA_FIRMWARE_RUN_LIST:
		error = format_run_list(bytes, length, layout);
		break;
	default:
		/* The whole program is the one piece. */
		program = amdrdna_le32(bytes, FORMAT_PROGRAM_AT);
		program_bytes = amdrdna_le32(bytes, FORMAT_PROGRAM_BYTES_AT);
		error = format_piece(layout, length, program, program_bytes);
		break;
	}

	/* Refuses a file a piece of which does not lie inside it. */
	if (error != 0)
		return error;

	/* Succeeded: every piece lies inside the file. */
	return 0;
}

/*
 * Reads and checks the common header: the file's size must be the length
 * given, and the header must be at least as large as the kind needs and
 * inside the file.
 */
static int
format_common(
	const uint8_t *bytes,
	size_t length,
	size_t header_least,
	struct amdrdna_firmware_layout *layout)
{
	uint32_t file_bytes;
	uint32_t header_bytes;

	/* Starts with no pieces. */
	layout->header_major = 0U;
	layout->header_minor = 0U;
	layout->version = 0U;
	layout->piece_count = 0U;

	/* A file shorter than the header the kind needs is not a firmware file of it. */
	if (length < header_least)
		return EINVAL;

	/* The header must say the file is exactly as long as it is. */
	file_bytes = amdrdna_le32(bytes, FORMAT_FILE_BYTES_AT);
	if ((size_t)file_bytes != length)
		return EINVAL;

	/* The header must be as large as the kind needs, and inside the file. */
	header_bytes = amdrdna_le32(bytes, FORMAT_HEADER_BYTES_AT);
	if ((size_t)header_bytes < header_least || (size_t)header_bytes > length)
		return EINVAL;

	/* The layout version and the program's version. */
	layout->header_major = amdrdna_le16(bytes, FORMAT_MAJOR_AT);
	layout->header_minor = amdrdna_le16(bytes, FORMAT_MINOR_AT);
	layout->version = amdrdna_le32(bytes, FORMAT_VERSION_AT);

	/* Succeeded: the common header is sound. */
	return 0;
}

/*
 * Cuts the security processor's file: the system driver (the program's
 * bytes up to the secure OS), the secure OS, the table of contents and the
 * key database.  Only layout 1.1 lists the last two.
 */
static int
format_secure(
	const uint8_t *bytes,
	size_t length,
	struct amdrdna_firmware_layout *layout)
{
	uint64_t program;
	uint64_t secure_offset;
	uint64_t secure_bytes;
	uint64_t contents_offset;
	uint64_t contents_bytes;
	uint64_t keys_offset;
	uint64_t keys_bytes;
	int error;

	/* A layout other than 1.1 does not say where the table and the keys are. */
	if (layout->header_major != 1U || layout->header_minor != 1U)
		return EINVAL;

	/* Reads the program's place and the three entries, whose offsets count from the program's start. */
	program = amdrdna_le32(bytes, FORMAT_PROGRAM_AT);
	secure_offset = amdrdna_le32(bytes, FORMAT_SECURE_OS_OFFSET_AT);
	secure_bytes = amdrdna_le32(bytes, FORMAT_SECURE_OS_BYTES_AT);
	contents_offset = amdrdna_le32(bytes, FORMAT_CONTENTS_OFFSET_AT);
	contents_bytes = amdrdna_le32(bytes, FORMAT_CONTENTS_BYTES_AT);
	keys_offset = amdrdna_le32(bytes, FORMAT_KEYS_OFFSET_AT);
	keys_bytes = amdrdna_le32(bytes, FORMAT_KEYS_BYTES_AT);

	/* The system driver: the program's first bytes, up to the secure OS. */
	error = format_piece(layout, length, program, secure_offset);
	if (error != 0)
		return error;

	/* The secure OS. */
	error = format_piece(layout, length, program + secure_offset, secure_bytes);
	if (error != 0)
		return error;

	/* The table of contents. */
	error = format_piece(layout, length, program + contents_offset, contents_bytes);
	if (error != 0)
		return error;

	/* The key database. */
	error = format_piece(layout, length, program + keys_offset, keys_bytes);
	if (error != 0)
		return error;

	/* Succeeded: the four boot pieces lie inside the file. */
	return 0;
}

/*
 * Cuts a compute processor's file: its program without the jump table at
 * the program's end, and the jump table on its own.
 */
static int
format_compute(
	const uint8_t *bytes,
	size_t length,
	struct amdrdna_firmware_layout *layout)
{
	uint64_t program;
	uint64_t program_bytes;
	uint64_t jumps_offset;
	uint64_t jumps_bytes;
	int error;

	/* Reads the program's place and the jump table's, the table counted in dwords from the program's start. */
	program = amdrdna_le32(bytes, FORMAT_PROGRAM_AT);
	program_bytes = amdrdna_le32(bytes, FORMAT_PROGRAM_BYTES_AT);
	jumps_offset = (uint64_t)amdrdna_le32(bytes, FORMAT_JUMPS_OFFSET_AT) * 4U;
	jumps_bytes = (uint64_t)amdrdna_le32(bytes, FORMAT_JUMPS_DWORDS_AT) * 4U;

	/* The jump table must be part of the program and leave some of it. */
	if (jumps_bytes == 0U || jumps_bytes >= program_bytes)
		return EINVAL;

	/* The program without its jump table. */
	error = format_piece(layout, length, program, program_bytes - jumps_bytes);
	if (error != 0)
		return error;

	/* The jump table. */
	error = format_piece(layout, length, program + jumps_offset, jumps_bytes);
	if (error != 0)
		return error;

	/* Succeeded: both pieces lie inside the file. */
	return 0;
}

/*
 * Cuts the run-list controller's file: its program and the three
 * save-and-restore lists (control, GPM, SRM).  Only layout 2.1 and later
 * list them.
 */
static int
format_run_list(
	const uint8_t *bytes,
	size_t length,
	struct amdrdna_firmware_layout *layout)
{
	uint64_t program;
	uint64_t program_bytes;
	uint64_t control_offset;
	uint64_t control_bytes;
	uint64_t gpm_offset;
	uint64_t gpm_bytes;
	uint64_t srm_offset;
	uint64_t srm_bytes;
	int error;

	/* A layout before 2.1 does not list the save-and-restore lists. */
	if (layout->header_major != 2U || layout->header_minor < 1U)
		return EINVAL;

	/* Reads the program's place and the lists', the lists' offsets counted from the file's start. */
	program = amdrdna_le32(bytes, FORMAT_PROGRAM_AT);
	program_bytes = amdrdna_le32(bytes, FORMAT_PROGRAM_BYTES_AT);
	control_offset = amdrdna_le32(bytes, FORMAT_CONTROL_OFFSET_AT);
	control_bytes = amdrdna_le32(bytes, FORMAT_CONTROL_BYTES_AT);
	gpm_offset = amdrdna_le32(bytes, FORMAT_GPM_OFFSET_AT);
	gpm_bytes = amdrdna_le32(bytes, FORMAT_GPM_BYTES_AT);
	srm_offset = amdrdna_le32(bytes, FORMAT_SRM_OFFSET_AT);
	srm_bytes = amdrdna_le32(bytes, FORMAT_SRM_BYTES_AT);

	/* The program. */
	error = format_piece(layout, length, program, program_bytes);
	if (error != 0)
		return error;

	/* The control list. */
	error = format_piece(layout, length, control_offset, control_bytes);
	if (error != 0)
		return error;

	/* The GPM list. */
	error = format_piece(layout, length, gpm_offset, gpm_bytes);
	if (error != 0)
		return error;

	/* The SRM list. */
	error = format_piece(layout, length, srm_offset, srm_bytes);
	if (error != 0)
		return error;

	/* Succeeded: the program and the three lists lie inside the file. */
	return 0;
}

/* Records one piece after checking that it is not empty and lies inside the file. */
static int
format_piece(
	struct amdrdna_firmware_layout *layout,
	size_t length,
	uint64_t offset,
	uint64_t size)
{
	/* The layout holds a bounded number of pieces. */
	if (layout->piece_count >= AMDRDNA_FIRMWARE_PIECES)
		return EINVAL;

	/* An empty piece, or one past the file's end, is refused. */
	if (size == 0U)
		return EINVAL;
	if (offset > (uint64_t)length || size > (uint64_t)length - offset)
		return EINVAL;

	/* Records it. */
	layout->pieces[layout->piece_count].offset = (uint32_t)offset;
	layout->pieces[layout->piece_count].bytes = (uint32_t)size;
	layout->piece_count++;

	/* Succeeded: the piece lies inside the file. */
	return 0;
}
