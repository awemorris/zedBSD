/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parts of bluetoothd's Intel firmware load without system calls
 * (ws143-p003, see intel.h and plan/ws143/design.md section 3).
 *
 * A .sfi file is a header (RSA, 644 bytes; on hardware variants from 0x17
 * an ECDSA header of 320 bytes follows it) and then a run of HCI commands
 * (opcode, length, parameters).  The header goes in Secure Send fragments
 * by its parts; the commands go in fragments of up to 252 bytes, cut at a
 * command's end once what waits is a multiple of 4.  Nothing in the file
 * is interpreted beyond those frames and the boot parameter (the licence
 * of Intel's firmware forbids more).
 */

#include "userland/base/bluetoothd/intel.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The TLV types of Read Version's answer that bluetoothd reads. */
#define INTEL_TLV_CNVI_TOP		0x10U
#define INTEL_TLV_CNVR_TOP		0x11U
#define INTEL_TLV_CNVI_BT		0x12U
#define INTEL_TLV_IMAGE_TYPE		0x1cU
#define INTEL_TLV_TIME_STAMP		0x1dU
#define INTEL_TLV_BUILD_TYPE		0x1eU
#define INTEL_TLV_BUILD_NUMBER		0x1fU
#define INTEL_TLV_LIMITED_CCE		0x2eU
#define INTEL_TLV_SBE_TYPE		0x2fU
#define INTEL_TLV_OTP_ADDRESS		0x30U

/* The headers of a .sfi file: RSA, then (variant 0x17 and later) ECDSA. */
#define INTEL_RSA_HEADER		644U
#define INTEL_ECDSA_HEADER		320U

/* Where a CSS header keeps its version, and the versions of the two engines. */
#define INTEL_CSS_VERSION_OFFSET	8U
#define INTEL_CSS_RSA			0x00010000U
#define INTEL_CSS_ECDSA			0x00020000U

/* The byte that marks the ECDSA header after the RSA one. */
#define INTEL_ECDSA_MARK		0x06U

/* The secure boot engines: RSA and ECDSA. */
#define INTEL_SBE_RSA			0x00U
#define INTEL_SBE_ECDSA			0x01U

/* The last variant with the RSA header alone, and the first with both. */
#define INTEL_VARIANT_RSA_LAST		0x14U
#define INTEL_VARIANT_BOTH_FIRST	0x17U

/* The size of an HCI command's frame in the file (opcode and length). */
#define INTEL_COMMAND_HEAD		3U

/*
 * The header's parts sent before the commands: for RSA the CSS header, the
 * public key in two halves, four bytes skipped and the signature in two
 * halves; for ECDSA, after the RSA header, its CSS header, key and
 * signature.  Constant tables of the layout.
 */
static const struct btd_intel_fragment intel_rsa_parts[] = {
	{ BTD_INTEL_FRAGMENT_CSS, 0U, 128U },
	{ BTD_INTEL_FRAGMENT_KEY, 128U, 128U },
	{ BTD_INTEL_FRAGMENT_KEY, 256U, 128U },
	{ BTD_INTEL_FRAGMENT_SIGNATURE, 388U, 128U },
	{ BTD_INTEL_FRAGMENT_SIGNATURE, 516U, 128U }
};
static const struct btd_intel_fragment intel_ecdsa_parts[] = {
	{ BTD_INTEL_FRAGMENT_CSS, INTEL_RSA_HEADER, 128U },
	{ BTD_INTEL_FRAGMENT_KEY, INTEL_RSA_HEADER + 128U, 96U },
	{ BTD_INTEL_FRAGMENT_SIGNATURE, INTEL_RSA_HEADER + 224U, 96U }
};

static uint32_t intel_le32(const uint8_t *bytes);
static int intel_tlv(struct btd_intel_version *version, uint8_t type, const uint8_t *value, size_t length);
static int intel_add(struct btd_intel_plan *plan, uint8_t type, uint32_t offset, uint32_t length);
static int intel_header(const uint8_t *file, size_t length, const struct btd_intel_version *version, struct btd_intel_plan *plan, size_t *body, const char **reason);

/*
 * Takes Intel Read Version's TLV answer apart (its return parameters, the
 * status first).  Returns 0, EIO for a status that is not 0, or EBADMSG for
 * a field whose length is not its own or runs past the answer, a field
 * given twice, or an answer without the image type.  Types it does not use
 * are passed over.
 */
int
btd_intel_version_parse(
	const uint8_t *returned,
	size_t length,
	struct btd_intel_version *version)
{
	uint64_t seen;
	uint64_t bit;
	size_t offset;
	uint8_t type;
	uint8_t size;
	int error;

	/* Nothing seen yet. */
	memset(version, 0, sizeof(*version));
	seen = 0U;

	/* The status first; a failed command has nothing to read. */
	if (length < 1U)
		return EBADMSG;
	if (returned[0] != 0U)
		return EIO;

	/* Each [type][length][value] in turn; a lone byte at the end is not a field. */
	offset = 1U;
	while (length - offset >= 2U) {
		/* The field's type and length, and its value inside the answer. */
		type = returned[offset];
		size = returned[offset + 1U];
		offset += 2U;
		if ((size_t)size > length - offset)
			return EBADMSG;

		/* A type of the TLV range (0x10 to 0x4f) given twice is a broken answer. */
		if (type >= 0x10U && type < 0x50U) {
			bit = (uint64_t)1 << (type - 0x10U);
			if ((seen & bit) != 0U)
				return EBADMSG;
			seen |= bit;
		}

		/* Keeps the field, when bluetoothd reads it. */
		error = intel_tlv(version, type, returned + offset, size);
		if (error != 0)
			return error;

		/* The next field. */
		offset += size;
	}

	/* The image type is always needed. */
	if (!version->have_image_type)
		return EBADMSG;

	/* Succeeded: the fields that were there. */
	return 0;
}

/*
 * Gives the hardware variant: bits 16 to 21 of the CNVi BT id.
 */
uint8_t
btd_intel_variant(
	const struct btd_intel_version *version)
{
	/* The six bits. */
	return (uint8_t)((version->cnvi_bt >> 16) & 0x3fU);
}

/*
 * Packs a CNVi or CNVR top id into the 16 bits a firmware file is named by:
 * the id's type and step.
 */
uint16_t
btd_intel_pack_id(
	uint32_t top)
{
	uint32_t packed;

	/* Bits 24 to 27 go to 8 to 11, bits 0 to 3 to 12 to 15, bits 4 to 11 to 0 to 7. */
	packed = (top & 0x0f000000U) >> 16;
	packed |= (top & 0x0000000fU) << 12;
	packed |= (top & 0x00000ff0U) >> 4;

	/* Succeeded: the packed id. */
	return (uint16_t)packed;
}

/*
 * Writes the path of the controller's firmware file:
 * FOLDER/ibt-CNVI-CNVR.SUFFIX.  Returns 0, EBADMSG when Read Version did not
 * give the ids, or ENAMETOOLONG.
 */
int
btd_intel_file_name(
	const struct btd_intel_version *version,
	const char *folder,
	const char *suffix,
	char *path,
	size_t size)
{
	int written;

	/* Both ids are needed. */
	if (!version->have_cnvi_top || !version->have_cnvr_top)
		return EBADMSG;

	/* The path. */
	written = snprintf(path,
			   size,
			   "%s/ibt-%04x-%04x.%s",
			   folder,
			   (unsigned)btd_intel_pack_id(version->cnvi_top),
			   (unsigned)btd_intel_pack_id(version->cnvr_top),
			   suffix);
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;

	/* Succeeded: the path. */
	return 0;
}

/*
 * Tells whether a controller in its bootloader can be loaded the way this
 * driver knows (design section 3, steps 2 and 3).  Returns 0, EBADMSG when
 * Read Version left out a field the decision needs, or ENOTSUP with the
 * reason.
 */
int
btd_intel_loadable(
	const struct btd_intel_version *version,
	const char **reason)
{
	uint8_t variant;

	/* The fields the decision reads. */
	*reason = "the version answer lacks a field";
	if (!version->have_cnvi_top ||
	    !version->have_cnvr_top ||
	    !version->have_cnvi_bt ||
	    !version->have_limited_cce ||
	    !version->have_sbe_type)
		return EBADMSG;

	/* Fragments acknowledged by Command Complete are another method, not known here. */
	if (version->limited_cce != 0U) {
		*reason = "limited-cce";
		return ENOTSUP;
	}

	/* Only the RSA and ECDSA secure boot engines. */
	if (version->sbe_type > INTEL_SBE_ECDSA) {
		*reason = "secure-boot-engine";
		return ENOTSUP;
	}

	/* Variants 0x15 and 0x16 have no known file layout; up to 0x14 the engine is RSA alone. */
	variant = btd_intel_variant(version);
	if (variant > INTEL_VARIANT_RSA_LAST && variant < INTEL_VARIANT_BOTH_FIRST) {
		*reason = "hardware-variant";
		return ENOTSUP;
	}

	/* Up to variant 0x14 the engine is RSA alone. */
	if (variant <= INTEL_VARIANT_RSA_LAST && version->sbe_type != INTEL_SBE_RSA) {
		*reason = "secure-boot-engine";
		return ENOTSUP;
	}

	/* Succeeded: it can be loaded. */
	*reason = NULL;
	return 0;
}

/*
 * Gives how many fragments a plan for a file of a length can need at most:
 * the header's five, and one for every 4 bytes of the commands.
 */
size_t
btd_intel_plan_capacity(
	size_t file_length)
{
	/* The header's fragments and the commands' (each at least 4 bytes). */
	return 5U + file_length / 4U + 1U;
}

/*
 * Plans a .sfi file's Secure Send fragments for a controller (design
 * section 3, steps 3 to 5).  plan->fragments and plan->capacity are the
 * caller's.  Returns 0, EINVAL with the reason when the file does not fit
 * the controller or its commands run past its end, ENOTSUP for a variant
 * with no known layout, or ENOSPC when the array is too small.
 */
int
btd_intel_plan_make(
	const uint8_t *file,
	size_t length,
	const struct btd_intel_version *version,
	struct btd_intel_plan *plan,
	const char **reason)
{
	const uint8_t *command;
	size_t body;
	size_t sent;
	size_t ready;
	uint16_t opcode;
	uint8_t parameters;
	int error;

	/* An empty plan. */
	plan->count = 0U;
	plan->have_boot_parameter = 0;
	plan->boot_parameter = 0U;

	/* The header's fragments, and where the commands start. */
	error = intel_header(file, length, version, plan, &body, reason);
	if (error != 0)
		return error;

	/*
	 * The commands: counted one frame at a time; a full 252 bytes goes as a
	 * fragment at once, and at a command's end what waits goes when it is
	 * a multiple of 4.  What is left at the file's end, not a multiple of
	 * 4, is not sent (as FreeBSD's iwmbtfw does).
	 */
	sent = body;
	ready = 0U;
	while (length - sent - ready >= INTEL_COMMAND_HEAD) {
		/* The next command's frame must lie inside the file. */
		command = file + sent + ready;
		opcode = (uint16_t)(command[0] | (command[1] << 8));
		parameters = command[2];
		if (sent + ready + INTEL_COMMAND_HEAD + parameters > length) {
			*reason = "a command runs past the file's end";
			return EINVAL;
		}

		/* The boot parameter Intel Reset will take. */
		if (opcode == BTD_INTEL_BOOT_PARAMETER && parameters >= 4U) {
			plan->boot_parameter = intel_le32(command + INTEL_COMMAND_HEAD);
			plan->have_boot_parameter = 1;
		}

		/* The command waits with the ones before it. */
		ready += INTEL_COMMAND_HEAD + parameters;

		/* Every full fragment goes. */
		while (ready >= BTD_INTEL_FRAGMENT_MAX) {
			error = intel_add(plan, BTD_INTEL_FRAGMENT_COMMANDS, (uint32_t)sent, BTD_INTEL_FRAGMENT_MAX);
			if (error != 0)
				return error;
			sent += BTD_INTEL_FRAGMENT_MAX;
			ready -= BTD_INTEL_FRAGMENT_MAX;
		}

		/* What waits goes at a command's end when it is a multiple of 4. */
		if (ready > 0U && (ready % 4U) == 0U) {
			error = intel_add(plan, BTD_INTEL_FRAGMENT_COMMANDS, (uint32_t)sent, (uint32_t)ready);
			if (error != 0)
				return error;
			sent += ready;
			ready = 0U;
		}
	}

	/* A file without the boot parameter cannot be booted. */
	if (!plan->have_boot_parameter) {
		*reason = "the file has no boot parameter";
		return EINVAL;
	}

	/* Succeeded: the fragments and the boot parameter. */
	*reason = NULL;
	return 0;
}

/*
 * Gives the next record of a .ddc file: [length L][L bytes], sent whole
 * (L + 1 bytes) in one Intel Write DDC.  Returns 1 with the record, 0 at
 * the file's end, or -1 for a record of length 0 or one that runs past the
 * end.
 */
int
btd_intel_ddc_next(
	const uint8_t *file,
	size_t length,
	size_t *offset,
	const uint8_t **record,
	size_t *record_length)
{
	size_t size;

	/* The end of the file. */
	if (*offset >= length)
		return 0;

	/* The record's length must be some and lie inside the file. */
	size = file[*offset];
	if (size == 0U)
		return -1;
	if (*offset + 1U + size > length)
		return -1;

	/* The record, with its length byte. */
	*record = file + *offset;
	*record_length = size + 1U;
	*offset += size + 1U;

	/* Succeeded: a record. */
	return 1;
}

/* Reads a little-endian 32-bit value. */
static uint32_t
intel_le32(
	const uint8_t *bytes)
{
	uint32_t value;

	/* The four bytes, least significant first. */
	value = (uint32_t)bytes[0];
	value |= (uint32_t)bytes[1] << 8;
	value |= (uint32_t)bytes[2] << 16;
	value |= (uint32_t)bytes[3] << 24;

	/* Succeeded: the value. */
	return value;
}

/* Keeps one TLV field of Read Version's answer; returns EBADMSG for a known field of the wrong length. */
static int
intel_tlv(
	struct btd_intel_version *version,
	uint8_t type,
	const uint8_t *value,
	size_t length)
{
	/* Each field bluetoothd reads, with the length it must have. */
	switch (type) {
	case INTEL_TLV_CNVI_TOP:
		if (length != 4U)
			return EBADMSG;
		version->cnvi_top = intel_le32(value);
		version->have_cnvi_top = 1;
		break;
	case INTEL_TLV_CNVR_TOP:
		if (length != 4U)
			return EBADMSG;
		version->cnvr_top = intel_le32(value);
		version->have_cnvr_top = 1;
		break;
	case INTEL_TLV_CNVI_BT:
		if (length != 4U)
			return EBADMSG;
		version->cnvi_bt = intel_le32(value);
		version->have_cnvi_bt = 1;
		break;
	case INTEL_TLV_IMAGE_TYPE:
		if (length != 1U)
			return EBADMSG;
		version->image_type = value[0];
		version->have_image_type = 1;
		break;
	case INTEL_TLV_TIME_STAMP:
		if (length != 2U)
			return EBADMSG;
		version->timestamp = (uint16_t)(value[0] | (value[1] << 8));
		break;
	case INTEL_TLV_BUILD_TYPE:
		if (length != 1U)
			return EBADMSG;
		version->build_type = value[0];
		break;
	case INTEL_TLV_BUILD_NUMBER:
		if (length != 4U)
			return EBADMSG;
		version->build_number = intel_le32(value);
		break;
	case INTEL_TLV_LIMITED_CCE:
		if (length != 1U)
			return EBADMSG;
		version->limited_cce = value[0];
		version->have_limited_cce = 1;
		break;
	case INTEL_TLV_SBE_TYPE:
		if (length != 1U)
			return EBADMSG;
		version->sbe_type = value[0];
		version->have_sbe_type = 1;
		break;
	case INTEL_TLV_OTP_ADDRESS:
		if (length != 6U)
			return EBADMSG;
		memcpy(version->address, value, 6U);
		version->have_address = 1;
		break;
	default:
		/* A field bluetoothd does not read. */
		break;
	}

	/* Succeeded: the field is kept or passed over. */
	return 0;
}

/* Adds one fragment to the plan; returns ENOSPC when the caller's array is full. */
static int
intel_add(
	struct btd_intel_plan *plan,
	uint8_t type,
	uint32_t offset,
	uint32_t length)
{
	/* The array must have room. */
	if (plan->count >= plan->capacity)
		return ENOSPC;

	/* The fragment at the end. */
	plan->fragments[plan->count].type = type;
	plan->fragments[plan->count].offset = offset;
	plan->fragments[plan->count].length = length;
	plan->count++;

	/* Succeeded: added. */
	return 0;
}

/* Checks a .sfi file's header against the controller and plans its fragments; gives where the commands start. */
static int
intel_header(
	const uint8_t *file,
	size_t length,
	const struct btd_intel_version *version,
	struct btd_intel_plan *plan,
	size_t *body,
	const char **reason)
{
	const struct btd_intel_fragment *parts;
	size_t count;
	size_t index;
	uint8_t variant;
	uint32_t css;
	int error;

	/* The layout follows the hardware variant. */
	variant = btd_intel_variant(version);
	*reason = "the file does not fit the controller";
	if (variant <= INTEL_VARIANT_RSA_LAST) {
		/* The RSA header alone, with the RSA CSS version. */
		*body = INTEL_RSA_HEADER;
		if (length < *body)
			return EINVAL;
		css = intel_le32(file + INTEL_CSS_VERSION_OFFSET);
		if (css != INTEL_CSS_RSA)
			return EINVAL;
	} else if (variant >= INTEL_VARIANT_BOTH_FIRST) {
		/* Both headers: the ECDSA one is marked and has the ECDSA CSS version. */
		*body = INTEL_RSA_HEADER + INTEL_ECDSA_HEADER;
		if (length < *body)
			return EINVAL;
		if (file[INTEL_RSA_HEADER] != INTEL_ECDSA_MARK)
			return EINVAL;
		css = intel_le32(file + INTEL_RSA_HEADER + INTEL_CSS_VERSION_OFFSET);
		if (css != INTEL_CSS_ECDSA)
			return EINVAL;
	} else {
		/* Variants 0x15 and 0x16. */
		*reason = "hardware-variant";
		return ENOTSUP;
	}

	/* The engine's header parts. */
	if (version->sbe_type == INTEL_SBE_RSA) {
		parts = intel_rsa_parts;
		count = sizeof(intel_rsa_parts) / sizeof(intel_rsa_parts[0]);
	} else if (version->sbe_type == INTEL_SBE_ECDSA) {
		parts = intel_ecdsa_parts;
		count = sizeof(intel_ecdsa_parts) / sizeof(intel_ecdsa_parts[0]);
	} else {
		/* Another engine. */
		*reason = "secure-boot-engine";
		return ENOTSUP;
	}

	/* Each part as a fragment. */
	for (index = 0U; index < count; index++) {
		error = intel_add(plan, parts[index].type, parts[index].offset, parts[index].length);
		if (error != 0)
			return error;
	}

	/* Succeeded: the header's fragments. */
	return 0;
}
