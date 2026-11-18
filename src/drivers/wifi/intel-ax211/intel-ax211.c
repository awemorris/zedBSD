/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Intel AX211 firmware and descriptor core.
 *
 * Parses the pinned firmware and PNVM files and encodes the descriptors and
 * the boot context the device reads.
 */

#include "intel-ax211-internal.h"
#include <kern/kcrt.h>

/*
 * The record types of the firmware file and of the PNVM file.
 *
 * What the pinned API 89 image and its PNVM contain, and what the loaders in
 * this file do with each type:
 * - RUNTIME_SECTION (19) is the only code record the driver loads.  Its body
 *   is a 32-bit device destination followed by the section bytes; the two
 *   mark destinations of intel-ax211-internal.h split the list into the
 *   LMAC, UMAC and paged groups, which the scratch area then lists
 *   separately.  The same type carries the sections of the PNVM file.
 * - INIT_SECTION (20) and WAKE_SECTION (21) hold the images of the
 *   initialization and wake-on-WLAN modes.  The driver counts them against
 *   the section limit but never loads them: the driver loads only the runtime
 *   image and selects the initialization stage inside it with the
 *   stage-select command (intel-ax211-init.h).
 * - RADIO_PROFILE (23), CPU_COUNT (27), IMAGE_VERSION (36), LOADER_IMAGE (52)
 *   and COMMAND_VERSION_TABLE (48) are required and may appear only once; an
 *   image without one of them is refused.  CPU_COUNT must be 1 or 2 and is 2
 *   on this image.  INTERFACE_FLAGS (29) and FEATURE_FLAGS (30) each carry an
 *   index and a word of capability bits, at most once per index (4 and 5
 *   words); the runtime code tests single bits of them
 *   (intel-ax211-runtime.h).  IMAGE_VERSION is three words: major (89),
 *   minor and serial, which must equal the pinned values.  LOADER_IMAGE is
 *   the 13944-byte image loader that the device runs first from the address
 *   in the loader image register.  COMMAND_VERSION_TABLE is 868 bytes, 217
 *   entries of four bytes, and is the only source of the layout version of
 *   each command.
 * - PNVM_BOARD (64), PNVM_REVISION (62) and HARDWARE_KIND (58) frame the
 *   boards of the PNVM file.  A board starts with its 12-byte signature; the
 *   revision word, the 4-byte hardware-kind records (MAC type and radio kind,
 *   16 bits each) and the sections that follow belong to it until the next
 *   signature.  The pinned file holds 4 boards with 8 hardware-kind records
 *   and 8 sections between them.
 * - Every other listed type is recognised and skipped.  An unknown type
 *   makes the loader refuse the image, so an image that grew a record the
 *   driver has never seen is not booted with a guessed meaning.
 * - TRACE_RANGE_FIRST to TRACE_RANGE_LAST is the block of debug
 *   configuration records (0x1000005 to 0x100000a).  0x1000000 to 0x1000004,
 *   0x100000b, 0x100000c, 65, 69, 0x101 and 1092 are further types that
 *   ax211_ignored_firmware_tlv() skips by number.
 *
 * Values referenced: Linux v6.17 drivers/net/wireless/intel/iwlwifi/fw/file.h
 * (enum iwl_ucode_tlv_type) and OpenBSD 0f464d413c50 sys/dev/pci/if_iwxreg.h
 * (IWX_UCODE_TLV_*, which also names types 35, 53 and 59).
 */
enum {
	AX211_RECORD_PROBE_LIMIT = 6,
	AX211_RECORD_PAN_SUPPORT = 7,
	AX211_RECORD_OLD_FLAGS = 18,
	AX211_RECORD_RUNTIME_SECTION = 19,
	AX211_RECORD_INIT_SECTION = 20,
	AX211_RECORD_WAKE_SECTION = 21,
	AX211_RECORD_CALIBRATION_DEFAULTS = 22,
	AX211_RECORD_RADIO_PROFILE = 23,
	AX211_RECORD_CPU_COUNT = 27,
	AX211_RECORD_CIPHER_SCHEMES = 28,
	AX211_RECORD_INTERFACE_FLAGS = 29,
	AX211_RECORD_FEATURE_FLAGS = 30,
	AX211_RECORD_SCAN_CHANNEL_LIMIT = 31,
	AX211_RECORD_PAGED_SIZE = 32,
	AX211_RECORD_SNIFFER_SECTION = 34,
	AX211_RECORD_SDIO_DMA_ADDRESS = 35,
	AX211_RECORD_IMAGE_VERSION = 36,
	AX211_RECORD_TRACE_DESTINATION = 38,
	AX211_RECORD_TRACE_SETUP = 39,
	AX211_RECORD_TRACE_TRIGGER = 40,
	AX211_RECORD_COMMAND_VERSION_TABLE = 48,
	AX211_RECORD_BACKGROUND_SCAN_LIMITS = 50,
	AX211_RECORD_MEMORY_SEGMENTS = 51,
	AX211_RECORD_LOADER_IMAGE = 52,
	AX211_RECORD_FMAC_INTERFACE = 53,
	AX211_RECORD_UPPER_CPU_TRACE_ADDRESSES = 54,
	AX211_RECORD_LOWER_CPU_TRACE_ADDRESSES = 55,
	AX211_RECORD_RECOVERY_BUFFER = 57,
	AX211_RECORD_HARDWARE_KIND = 58,
	AX211_RECORD_FMAC_RECOVERY_BUFFER = 59,
	AX211_RECORD_SEQUENCER_REVISION = 60,
	AX211_RECORD_RADIO_INTEGRATION_REVISION = 61,
	AX211_RECORD_PNVM_REVISION = 62,
	AX211_RECORD_PNVM_BOARD = 64,
	AX211_RECORD_SECTION_TABLE_ADDRESS = 66,
	AX211_RECORD_SUSPEND_KEY_ADDRESS = 67,
	AX211_RECORD_PROGRAM_COUNTER_ADDRESS = 68,
	AX211_RECORD_PNVM_PAYLOAD = 74,
	AX211_RECORD_STATION_LIMIT = 0x100,
	AX211_RECORD_BEACON_LIMIT = 0x102,
	AX211_RECORD_TRACE_RANGE_FIRST = 0x1000005,
	AX211_RECORD_TRACE_RANGE_LAST = 0x100000a
};

static uint16_t ax211_get_le16(const uint8_t *bytes);
static uint32_t ax211_get_le32(const uint8_t *bytes);
static void ax211_put_le16(uint8_t *bytes, uint16_t value);
static void ax211_put_le32(uint8_t *bytes, uint32_t value);
static void ax211_put_le64(uint8_t *bytes, uint64_t value);
static int ax211_tlv_span(uint32_t value, size_t remaining, size_t *span);
static int ax211_ignored_firmware_tlv(uint32_t type);
static int ax211_pnvm_finish(const struct intel_ax211_pnvm_manifest *candidate, int active, int version_seen, int hardware_match, uint16_t mac_type, uint16_t rf_id, struct intel_ax211_pnvm_manifest *manifest);

/*
 * Reports whether a PCI identity is the one supported AX211: 1 when it is, 0
 * when not.
 */
int
drv_intel_ax211_identity_matches(
	const struct intel_ax211_identity *identity)
{
	/* Rejects a missing identity. */
	if (identity == NULL)
		return 0;

	/* Rejects another vendor, device or subsystem. */
	if (identity->vendor != INTEL_AX211_PCI_VENDOR_ID ||
	    identity->device != INTEL_AX211_PCI_DEVICE_ID ||
	    identity->subvendor != INTEL_AX211_PCI_SUBVENDOR_ID ||
	    identity->subdevice != INTEL_AX211_PCI_SUBDEVICE_ID) {
		return 0;
	}

	/* Rejects another revision. */
	if (identity->revision != INTEL_AX211_PCI_REVISION)
		return 0;

	/* The identity is the supported one. */
	return 1;
}

/*
 * Reports whether a MAC type is So or SoF: 1 when it is, 0 when not.
 */
int
drv_intel_ax211_mac_type_supported(
	uint16_t mac_type)
{
	/* Rejects any other MAC. */
	if (mac_type != INTEL_AX211_SILICON_SO &&
	    mac_type != INTEL_AX211_SILICON_SOF) {
		return 0;
	}

	/* The MAC is supported. */
	return 1;
}

/*
 * Parses the pinned firmware file into *manifest: its version, API and radio
 * profile, the loader image, the command-version table and the runtime
 * sections.
 */
int
drv_intel_ax211_firmware_parse(
	const uint8_t *bytes,
	size_t length,
	struct intel_ax211_firmware_manifest *manifest)
{
	uint32_t api_index;
	uint32_t capability_index;
	struct intel_ax211_section *section;
	uint32_t destination;
	uint32_t word;
	uint32_t magic;
	int separator;
	int ignored;
	uint32_t type;
	uint32_t tlv_length;
	const uint8_t *data;
	int result;
	struct intel_ax211_firmware_manifest parsed;
	size_t offset;
	size_t span;
	size_t cpu_separator = SIZE_MAX;
	size_t paging_separator = SIZE_MAX;
	uint32_t seen = 0;
	uint8_t api_seen = 0;
	uint8_t capa_seen = 0;
	size_t init_sections = 0;
	size_t wow_sections = 0;

	/* Refuses missing arguments. */
	if (bytes == NULL || manifest == NULL)
		return INTEL_AX211_INVALID;

	/* Refuses a file shorter than its header. */
	if (length < INTEL_AX211_FIRMWARE_FILE_HEADER_SIZE)
		return INTEL_AX211_TRUNCATED;

	/* Refuses a header without the zero word and the magic. */
	word = ax211_get_le32(bytes);
	magic = ax211_get_le32(bytes + 4U);
	if (word != 0U || magic != INTEL_AX211_FIRMWARE_FILE_MAGIC)
		return INTEL_AX211_INVALID;

	/* Takes the header's version and build. */
	kern_memset(&parsed, 0, sizeof(parsed));
	parsed.header_version = ax211_get_le32(bytes + 72U);
	parsed.build = ax211_get_le32(bytes + 76U);
	offset = INTEL_AX211_FIRMWARE_FILE_HEADER_SIZE;

	/* Parses the records one after another. */
	while (offset < length) {
		/* Refuses a record header cut short. */
		if (length - offset < INTEL_AX211_FIRMWARE_RECORD_HEADER_SIZE)
			return INTEL_AX211_TRUNCATED;
		type = ax211_get_le32(bytes + offset);
		tlv_length = ax211_get_le32(bytes + offset + 4U);
		offset += INTEL_AX211_FIRMWARE_RECORD_HEADER_SIZE;

		/* Refuses a record body beyond the file. */
		result = ax211_tlv_span(tlv_length, length - offset, &span);
		if (result != INTEL_AX211_SUCCESS)
			return result;
		data = bytes + offset;

		/* Takes the record by its type. */
		switch (type) {
		case AX211_RECORD_RUNTIME_SECTION:
			/* Refuses a section without its destination. */
			if (tlv_length < 4U)
				return INTEL_AX211_INVALID;

			/* Refuses a section beyond the limit. */
			if (parsed.runtime_count >= INTEL_AX211_FIRMWARE_SECTION_LIMIT)
				return INTEL_AX211_OVERFLOW;
			destination = ax211_get_le32(data);
			separator = 0;

			/* A mark destination separates the section groups. */
			if (destination == INTEL_AX211_UPPER_CPU_IMAGE_MARK) {
				/* Refuses a mark of another length. */
				if (tlv_length != 8U)
					return INTEL_AX211_INVALID;

				/* Refuses a mark whose second word is set. */
				word = ax211_get_le32(data + 4U);
				if (word != 0U)
					return INTEL_AX211_INVALID;

				/* Refuses a second mark. */
				if (cpu_separator != SIZE_MAX)
					return INTEL_AX211_DUPLICATE;
				cpu_separator = parsed.runtime_count;
				separator = 1;
			} else if (destination ==
				   INTEL_AX211_PAGED_IMAGE_MARK) {
				/* Refuses a mark of another length. */
				if (tlv_length != 8U)
					return INTEL_AX211_INVALID;

				/* Refuses a mark whose second word is set. */
				word = ax211_get_le32(data + 4U);
				if (word != 0U)
					return INTEL_AX211_INVALID;

				/* Refuses a second mark. */
				if (paging_separator != SIZE_MAX)
					return INTEL_AX211_DUPLICATE;
				paging_separator = parsed.runtime_count;
				separator = 1;
			} else if (tlv_length == 4U) {
				/* Refuses an empty section. */
				return INTEL_AX211_INVALID;
			}

			/* Records the section; a mark has no bytes. */
			section = &parsed.runtime[parsed.runtime_count++];
			section->destination = destination;
			section->file_offset = offset + 4U;
			if (separator)
				section->length = 0U;
			else
				section->length = (size_t)tlv_length - 4U;
			break;
		case AX211_RECORD_INIT_SECTION:
			/* Refuses a section without its destination. */
			if (tlv_length < 4U)
				return INTEL_AX211_INVALID;

			/* Counts the section against the limit. */
			if (++init_sections > INTEL_AX211_FIRMWARE_SECTION_LIMIT)
				return INTEL_AX211_OVERFLOW;
			break;
		case AX211_RECORD_WAKE_SECTION:
			/* Refuses a section without its destination. */
			if (tlv_length < 4U)
				return INTEL_AX211_INVALID;

			/* Counts the section against the limit. */
			if (++wow_sections > INTEL_AX211_FIRMWARE_SECTION_LIMIT)
				return INTEL_AX211_OVERFLOW;
			break;
		case AX211_RECORD_RADIO_PROFILE:
			/* Refuses a second radio profile. */
			if ((seen & 1U) != 0U)
				return INTEL_AX211_DUPLICATE;

			/* Refuses a profile of another length. */
			if (tlv_length != 4U)
				return INTEL_AX211_INVALID;
			seen |= 1U;
			parsed.radio_profile = ax211_get_le32(data);
			break;
		case AX211_RECORD_CPU_COUNT:
			/* Refuses a second CPU count. */
			if ((seen & 2U) != 0U)
				return INTEL_AX211_DUPLICATE;

			/* Refuses a count of another length. */
			if (tlv_length != 4U)
				return INTEL_AX211_INVALID;
			seen |= 2U;
			parsed.cpu_count = ax211_get_le32(data);

			/* Refuses a count other than 1 or 2. */
			if (parsed.cpu_count == 0U || parsed.cpu_count > 2U)
				return INTEL_AX211_INVALID;
			break;
		case AX211_RECORD_INTERFACE_FLAGS:
			/* Refuses flags of another length. */
			if (tlv_length != 8U)
				return INTEL_AX211_INVALID;

			/* Refuses an index beyond the four words. */
			api_index = ax211_get_le32(data);
			if (api_index >= 4U)
				return INTEL_AX211_OVERFLOW;

			/* Refuses a second word of the same index. */
			if ((api_seen & (uint8_t)(1U << api_index)) != 0U)
				return INTEL_AX211_DUPLICATE;
			api_seen |= (uint8_t)(1U << api_index);
			parsed.api_changes[api_index] =
				ax211_get_le32(data + 4U);
			break;
		case AX211_RECORD_FEATURE_FLAGS:
			/* Refuses flags of another length. */
			if (tlv_length != 8U)
				return INTEL_AX211_INVALID;

			/* Refuses an index beyond the five words. */
			capability_index = ax211_get_le32(data);
			if (capability_index >= 5U)
				return INTEL_AX211_OVERFLOW;

			/* Refuses a second word of the same index. */
			if ((capa_seen & (uint8_t)(1U << capability_index)) !=
			    0U) {
				return INTEL_AX211_DUPLICATE;
			}

			/* Records the word. */
			capa_seen |= (uint8_t)(1U << capability_index);
			parsed.capabilities[capability_index] =
				ax211_get_le32(data + 4U);
			break;
		case AX211_RECORD_IMAGE_VERSION:
			/* Refuses a second version. */
			if ((seen & 4U) != 0U)
				return INTEL_AX211_DUPLICATE;

			/* Refuses a version of another length. */
			if (tlv_length != 12U)
				return INTEL_AX211_INVALID;
			seen |= 4U;
			parsed.api_major = ax211_get_le32(data);
			parsed.api_minor = ax211_get_le32(data + 4U);
			parsed.api_serial = ax211_get_le32(data + 8U);
			break;
		case AX211_RECORD_LOADER_IMAGE:
			/* Refuses a second loader image. */
			if ((seen & 8U) != 0U)
				return INTEL_AX211_DUPLICATE;

			/* Refuses an empty loader image. */
			if (tlv_length == 0U)
				return INTEL_AX211_INVALID;
			seen |= 8U;
			parsed.iml_offset = offset;
			parsed.iml_length = tlv_length;
			break;
		case AX211_RECORD_COMMAND_VERSION_TABLE:
			/* Refuses a second table. */
			if ((seen & 16U) != 0U)
				return INTEL_AX211_DUPLICATE;

			/* Refuses an empty table and one of partial entries. */
			if (tlv_length == 0U || (tlv_length & 3U) != 0U)
				return INTEL_AX211_INVALID;
			seen |= 16U;
			parsed.command_versions_offset = offset;
			parsed.command_versions_length = tlv_length;
			break;
		default:
			/* Refuses a type that is not known to be skippable. */
			ignored = ax211_ignored_firmware_tlv(type);
			if (!ignored)
				return INTEL_AX211_UNSUPPORTED;
			break;
		}

		/* Moves to the next record. */
		offset += span;
	}

	/* Refuses an image without a required record or a mark. */
	if ((seen & 31U) != 31U ||
	    parsed.runtime_count == 0U ||
	    cpu_separator == SIZE_MAX ||
	    paging_separator == SIZE_MAX) {
		return INTEL_AX211_MISSING;
	}

	/* Refuses an image with an empty section group. */
	if (cpu_separator == 0U ||
	    paging_separator <= cpu_separator + 1U ||
	    paging_separator + 1U >= parsed.runtime_count) {
		return INTEL_AX211_MISSING;
	}

	/* Refuses an image of another version. */
	if (parsed.api_major != INTEL_AX211_FIRMWARE_API ||
	    parsed.api_minor != INTEL_AX211_FIRMWARE_MINOR ||
	    parsed.api_serial != INTEL_AX211_FIRMWARE_SERIAL) {
		return INTEL_AX211_IDENTITY_MISMATCH;
	}

	/* Succeeded: counts the section groups and reports the manifest. */
	parsed.lmac_count = cpu_separator;
	parsed.umac_count = paging_separator - cpu_separator - 1U;
	parsed.paging_count = parsed.runtime_count - paging_separator - 1U;
	*manifest = parsed;
	return INTEL_AX211_SUCCESS;
}

/*
 * Reports whether two board signatures are equal: 1 when they are, 0 when
 * not.
 */
int
drv_intel_ax211_sku_equal(
	const struct intel_ax211_board_signature *left,
	const struct intel_ax211_board_signature *right)
{
	/* Rejects missing signatures. */
	if (left == NULL || right == NULL)
		return 0;

	/* Rejects a differing word. */
	if (left->data[0] != right->data[0] ||
	    left->data[1] != right->data[1] ||
	    left->data[2] != right->data[2]) {
		return 0;
	}

	/* The signatures are equal. */
	return 1;
}

/*
 * Selects the PNVM data of the board sku names and checks that it is for the
 * given MAC and radio.
 */
int
drv_intel_ax211_pnvm_select(
	const uint8_t *bytes,
	size_t length,
	const struct intel_ax211_board_signature *sku,
	uint16_t mac_type,
	uint16_t rf_id,
	struct intel_ax211_pnvm_manifest *manifest)
{
	struct intel_ax211_board_signature found;
	uint16_t found_mac;
	uint16_t found_rf;
	struct intel_ax211_section *section;
	size_t data_length;
	uint32_t destination;
	uint32_t type;
	uint32_t tlv_length;
	size_t span;
	const uint8_t *data;
	int result;
	struct intel_ax211_pnvm_manifest candidate;
	size_t offset = 0;
	int active = 0;
	int version_seen = 0;
	int hardware_match = 0;

	/* Refuses missing arguments. */
	if (bytes == NULL || sku == NULL || manifest == NULL)
		return INTEL_AX211_INVALID;
	kern_memset(&candidate, 0, sizeof(candidate));

	/* Parses the records one after another. */
	while (offset < length) {
		/* Refuses a record header cut short. */
		if (length - offset < INTEL_AX211_FIRMWARE_RECORD_HEADER_SIZE)
			return INTEL_AX211_TRUNCATED;
		type = ax211_get_le32(bytes + offset);
		tlv_length = ax211_get_le32(bytes + offset + 4U);
		offset += INTEL_AX211_FIRMWARE_RECORD_HEADER_SIZE;

		/* Refuses a record body beyond the file. */
		result = ax211_tlv_span(tlv_length, length - offset, &span);
		if (result != INTEL_AX211_SUCCESS)
			return result;
		data = bytes + offset;

		/* Takes the record by its type. */
		if (type == AX211_RECORD_PNVM_BOARD) {
			/* Refuses a signature of another length. */
			if (tlv_length != 12U)
				return INTEL_AX211_INVALID;

			/* Finishes the board that was being collected. */
			if (active) {
				/* Succeeded: the board was found. */
				result = ax211_pnvm_finish(
					&candidate, active, version_seen,
					hardware_match, mac_type, rf_id,
					manifest);
				if (result == INTEL_AX211_SUCCESS)
					return INTEL_AX211_SUCCESS;

				/*
				 * Reports a failure other than another radio.
				 */
				if (result != INTEL_AX211_IDENTITY_MISMATCH)
					return result;
			}

			/* Starts the next board; collects it only for sku. */
			found.data[0] = ax211_get_le32(data);
			found.data[1] = ax211_get_le32(data + 4U);
			found.data[2] = ax211_get_le32(data + 8U);
			active = drv_intel_ax211_sku_equal(&found, sku);
			version_seen = 0;
			hardware_match = 0;
			kern_memset(&candidate, 0, sizeof(candidate));
			candidate.sku = found;
		} else if (type == AX211_RECORD_PNVM_REVISION) {
			/* Refuses a revision without its word. */
			if (tlv_length < 4U)
				return INTEL_AX211_INVALID;

			/* Records the revision of the collected board. */
			if (active) {
				/* Refuses a second revision. */
				if (version_seen)
					return INTEL_AX211_DUPLICATE;
				version_seen = 1;
				candidate.version = ax211_get_le32(data);
			}
		} else if (type == AX211_RECORD_HARDWARE_KIND) {
			/* Refuses a hardware kind without its two codes. */
			if (tlv_length < 4U)
				return INTEL_AX211_INVALID;

			/* Records a hardware kind of the collected board. */
			if (active) {
				found_mac = ax211_get_le16(data);

				/*
				 * Notes a kind that is the given MAC and radio.
				 */
				found_rf = ax211_get_le16(data + 2U);
				if (found_mac == mac_type &&
				    found_rf == rf_id) {
					hardware_match = 1;
					candidate.mac_type = found_mac;
					candidate.rf_id = found_rf;
				}
			}
		} else if (type == AX211_RECORD_RUNTIME_SECTION) {
			/* Refuses a section without its destination. */
			if (tlv_length < 4U)
				return INTEL_AX211_INVALID;

			/* Records a section of the collected board. */
			destination = ax211_get_le32(data);
			if (active &&
			    destination != INTEL_AX211_PNVM_IMAGE_MARK) {
				/* Refuses an empty section. */
				data_length = (size_t)tlv_length - 4U;
				if (data_length == 0U)
					return INTEL_AX211_INVALID;

				/* Refuses a section beyond the limit. */
				if (candidate.section_count >=
				    INTEL_AX211_PNVM_SECTION_LIMIT) {
					return INTEL_AX211_OVERFLOW;
				}

				/* Refuses a total length that overflows. */
				if (candidate.total_length >
				    SIZE_MAX - data_length) {
					return INTEL_AX211_OVERFLOW;
				}

				/* Records the section. */
				section = &candidate.section
						   [candidate.section_count++];
				section->destination = destination;
				section->file_offset = offset + 4U;
				section->length = data_length;
				candidate.total_length += data_length;
			}
		} else {
			/* Refuses any other type. */
			return INTEL_AX211_UNSUPPORTED;
		}

		/* Moves to the next record. */
		offset += span;
	}

	/* Finishes the last board. */
	result = ax211_pnvm_finish(&candidate, active, version_seen,
				   hardware_match, mac_type, rf_id, manifest);
	if (result != INTEL_AX211_SUCCESS)
		return result;

	/* Succeeded: *manifest holds the board's data. */
	return INTEL_AX211_SUCCESS;
}

/*
 * Counts what a PNVM file holds: its boards, revisions, hardware kinds and
 * sections.
 */
int
drv_intel_ax211_pnvm_inspect(
	const uint8_t *bytes,
	size_t length,
	struct intel_ax211_pnvm_inventory *inventory)
{
	uint16_t mac_type;
	uint16_t rf_type;
	size_t section_length;
	uint32_t destination;
	uint32_t type;
	uint32_t tlv_length;
	size_t span;
	const uint8_t *data;
	int supported;
	int result;
	struct intel_ax211_pnvm_inventory found;
	size_t offset = 0;

	/* Refuses missing arguments. */
	if (bytes == NULL || inventory == NULL)
		return INTEL_AX211_INVALID;
	kern_memset(&found, 0, sizeof(found));

	/* Counts the records one after another. */
	while (offset < length) {
		/* Refuses a record header cut short. */
		if (length - offset < INTEL_AX211_FIRMWARE_RECORD_HEADER_SIZE)
			return INTEL_AX211_TRUNCATED;
		type = ax211_get_le32(bytes + offset);
		tlv_length = ax211_get_le32(bytes + offset + 4U);
		offset += INTEL_AX211_FIRMWARE_RECORD_HEADER_SIZE;

		/* Refuses a record body beyond the file. */
		result = ax211_tlv_span(tlv_length, length - offset, &span);
		if (result != INTEL_AX211_SUCCESS)
			return result;
		data = bytes + offset;

		/* Counts the record by its type. */
		if (type == AX211_RECORD_PNVM_BOARD) {
			/* Refuses a signature of another length. */
			if (tlv_length != 12U)
				return INTEL_AX211_INVALID;
			found.sku_count++;
		} else if (type == AX211_RECORD_PNVM_REVISION) {
			/* Refuses a revision without its word. */
			if (tlv_length < 4U)
				return INTEL_AX211_INVALID;
			found.version_count++;
		} else if (type == AX211_RECORD_HARDWARE_KIND) {
			/* Refuses a hardware kind without its two codes. */
			if (tlv_length < 4U)
				return INTEL_AX211_INVALID;
			mac_type = ax211_get_le16(data);
			rf_type = ax211_get_le16(data + 2U);
			found.hardware_type_count++;

			/*
			 * Counts a kind with a supported MAC and the GF radio.
			 */
			supported =
				drv_intel_ax211_mac_type_supported(mac_type);
			if (supported && rf_type == INTEL_AX211_RADIO_GF)
				found.supported_hardware_type_count++;
		} else if (type == AX211_RECORD_RUNTIME_SECTION) {
			/* Refuses a section without its destination. */
			if (tlv_length < 4U)
				return INTEL_AX211_INVALID;

			/* Counts a section that is not a mark. */
			destination = ax211_get_le32(data);
			if (destination != INTEL_AX211_PNVM_IMAGE_MARK) {
				/* Refuses an empty section. */
				section_length = (size_t)tlv_length - 4U;
				if (section_length == 0U)
					return INTEL_AX211_INVALID;

				/* Refuses a total length that overflows. */
				if (found.total_section_length >
				    SIZE_MAX - section_length) {
					return INTEL_AX211_OVERFLOW;
				}

				/* Counts the section. */
				found.total_section_length += section_length;
				found.section_count++;
			}
		} else {
			/* Refuses any other type. */
			return INTEL_AX211_UNSUPPORTED;
		}

		/* Moves to the next record. */
		offset += span;
	}

	/* Refuses a file without boards, revisions, kinds or sections. */
	if (found.sku_count == 0U ||
	    found.version_count == 0U ||
	    found.hardware_type_count == 0U ||
	    found.section_count == 0U) {
		return INTEL_AX211_MISSING;
	}

	/* Succeeded: reports the counts. */
	*inventory = found;
	return INTEL_AX211_SUCCESS;
}

/*
 * Encodes the boot context as the 104 little-endian bytes the device reads.
 */
int
drv_intel_ax211_boot_context_encode(
	uint8_t output[104],
	const struct intel_ax211_boot_context *context)
{
	/* Refuses missing arguments. */
	if (output == NULL || context == NULL)
		return INTEL_AX211_INVALID;

	/* Refuses ring size codes other than the rings' sizes. */
	if (context->command_ring_size_code !=
		    INTEL_AX211_COMMAND_RING_CB_SIZE ||
	    context->response_ring_size_code !=
		    INTEL_AX211_RX_RING_CB_SIZE) {
		return INTEL_AX211_INVALID;
	}

	/* Succeeded: writes every field at its offset. */
	kern_memset(output, 0, INTEL_AX211_BOOT_CONTEXT_SIZE);
	ax211_put_le16(output, context->version);
	ax211_put_le16(output + 2U, INTEL_AX211_BOOT_CONTEXT_SIZE / 4U);
	ax211_put_le32(output + 4U, context->config);
	ax211_put_le64(output + 8U, context->status_page_address);
	ax211_put_le64(output + 16U, context->completion_head_array_address);
	ax211_put_le64(output + 24U, context->transfer_tail_array_address);
	ax211_put_le64(output + 32U, context->completion_tail_array_address);
	ax211_put_le64(output + 40U, context->transfer_head_array_address);
	ax211_put_le16(output + 48U, context->completion_index_slots);
	ax211_put_le16(output + 50U, context->transfer_index_slots);
	ax211_put_le64(output + 52U, context->command_ring_address);
	ax211_put_le64(output + 60U, context->response_ring_address);
	ax211_put_le16(output + 68U, context->command_ring_size_code);
	ax211_put_le16(output + 70U, context->response_ring_size_code);
	ax211_put_le16(output + 72U, context->command_ring_doorbell);
	ax211_put_le16(output + 74U, context->response_ring_doorbell);
	ax211_put_le16(output + 76U, context->command_ring_vector);
	ax211_put_le16(output + 78U, context->response_ring_vector);
	output[80] = context->command_entry_header_words;
	output[81] = context->command_entry_footer_words;
	output[82] = context->response_entry_header_words;
	output[83] = context->response_entry_footer_words;
	ax211_put_le16(output + 84U, context->ring_control_flags);
	ax211_put_le16(output + 86U, context->status_page_vector);
	ax211_put_le64(output + 88U, context->scratch_area_address);
	ax211_put_le32(output + 96U, context->scratch_area_size);
	return INTEL_AX211_SUCCESS;
}

/*
 * Encodes one receive transfer descriptor: the buffer id and its address.
 */
int
drv_intel_ax211_rx_transfer_descriptor_encode(
	uint8_t output[16],
	uint16_t buffer_id,
	uint64_t address)
{
	/* Refuses a missing output. */
	if (output == NULL)
		return INTEL_AX211_INVALID;

	/* Succeeded: writes the id and the address. */
	kern_memset(output, 0, INTEL_AX211_RX_TRANSFER_DESCRIPTOR_SIZE);
	ax211_put_le16(output, buffer_id);
	ax211_put_le64(output + 8U, address);
	return INTEL_AX211_SUCCESS;
}

/*
 * Decodes one receive completion descriptor: the buffer it filled and its
 * flags.
 */
int
drv_intel_ax211_rx_completion_descriptor_decode(
	const uint8_t input[32],
	uint16_t *buffer_id,
	uint8_t *flags)
{
	/* Refuses missing arguments. */
	if (input == NULL || buffer_id == NULL || flags == NULL)
		return INTEL_AX211_INVALID;

	/* Succeeded: reads the id and the flags. */
	*buffer_id = ax211_get_le16(input + 4U);
	*flags = input[6];
	return INTEL_AX211_SUCCESS;
}

/*
 * Encodes one TFD from buffer_count transfer buffers.
 */
int
drv_intel_ax211_tfd_encode(
	uint8_t output[256],
	const struct intel_ax211_tfd_buffer *buffers,
	size_t buffer_count)
{
	size_t offset;
	size_t index;

	/* Refuses missing arguments and a buffer count out of range. */
	if (output == NULL ||
	    buffers == NULL ||
	    buffer_count == 0U ||
	    buffer_count > INTEL_AX211_TRANSFER_DESCRIPTOR_BUFFERS) {
		return INTEL_AX211_INVALID;
	}

	/* Writes the buffer count. */
	kern_memset(output, 0, INTEL_AX211_TRANSFER_DESCRIPTOR_SIZE);
	ax211_put_le16(output, (uint16_t)buffer_count);

	/* Writes each buffer's length and address. */
	for (index = 0; index < buffer_count; index++) {
		offset = 2U + index * 10U;

		/* Clears the TFD and refuses an empty or oversized buffer. */
		if (buffers[index].length == 0U ||
		    buffers[index].length > INTEL_AX211_TRANSFER_BUFFER_LENGTH_MAX) {
			drv_intel_ax211_scrub(output, INTEL_AX211_TRANSFER_DESCRIPTOR_SIZE);

			/* Reports the failure. */
			return INTEL_AX211_INVALID;
		}

		/* Writes the buffer. */
		ax211_put_le16(output + offset, buffers[index].length);
		ax211_put_le64(output + offset + 2U, buffers[index].address);
	}

	/* Succeeded. */
	return INTEL_AX211_SUCCESS;
}

/*
 * Encodes the 4-byte narrow command header.
 */
int
drv_intel_ax211_narrow_command_encode(
	uint8_t output[4],
	uint8_t opcode,
	uint8_t flags,
	const struct intel_ax211_ring_token *token)
{
	/* Refuses missing arguments. */
	if (output == NULL || token == NULL)
		return INTEL_AX211_INVALID;

	/* Succeeded: writes the opcode, the flags and the slot. */
	output[0] = opcode;
	output[1] = flags;
	output[2] = token->index;
	output[3] = token->queue;
	return INTEL_AX211_SUCCESS;
}

/*
 * Encodes the 8-byte wide command header.
 */
int
drv_intel_ax211_wide_command_encode(
	uint8_t output[8],
	const struct intel_ax211_command_id *command,
	uint16_t payload_length,
	const struct intel_ax211_ring_token *token)
{
	/* Refuses missing arguments and an oversized payload. */
	if (output == NULL ||
	    command == NULL ||
	    token == NULL ||
	    payload_length > INTEL_AX211_MAX_COMMAND_PAYLOAD) {
		return INTEL_AX211_INVALID;
	}

	/*
	 * Succeeded: writes the command, the slot, the length and the version.
	 */
	output[0] = command->opcode;
	output[1] = command->group;
	output[2] = token->index;
	output[3] = token->queue;
	ax211_put_le16(output + 4U, payload_length);
	output[6] = 0;
	output[7] = command->version;
	return INTEL_AX211_SUCCESS;
}

/*
 * Decodes the header of one received event and locates its payload.
 */
int
drv_intel_ax211_event_decode(
	const uint8_t *bytes,
	size_t length,
	struct intel_ax211_event *event)
{
	uint32_t length_flags;
	size_t frame_length;

	/* Refuses missing arguments. */
	if (bytes == NULL || event == NULL)
		return INTEL_AX211_INVALID;

	/* Refuses an event shorter than its header. */
	if (length < INTEL_AX211_EVENT_HEADER_SIZE)
		return INTEL_AX211_TRUNCATED;
	length_flags = ax211_get_le32(bytes);

	/* Refuses a frame shorter than a command header. */
	frame_length = (size_t)(length_flags & 0x3fffU);
	if (frame_length < INTEL_AX211_NARROW_COMMAND_HEADER_SIZE)
		return INTEL_AX211_INVALID;

	/* Refuses a frame beyond the event. */
	if (frame_length > length - 4U)
		return INTEL_AX211_TRUNCATED;

	/* Succeeded: describes the command, the slot and the payload. */
	kern_memset(event, 0, sizeof(*event));
	event->command.opcode = bytes[4];
	event->flags = bytes[5];
	event->index = bytes[6];
	event->queue = bytes[7];
	event->rx_queue = (uint8_t)((length_flags >> 16) & 0x3fU);
	event->payload_offset = INTEL_AX211_EVENT_HEADER_SIZE;
	event->payload_length = frame_length - 4U;
	return INTEL_AX211_SUCCESS;
}

/*
 * Initializes an empty ring of a power-of-two capacity.
 */
int
drv_intel_ax211_ring_init(
	struct intel_ax211_ring *ring,
	uint8_t queue,
	uint16_t capacity)
{
	/* Refuses a missing ring and a capacity that is not a power of two. */
	if (ring == NULL ||
	    capacity == 0U ||
	    capacity > INTEL_AX211_COMMAND_RING_SIZE ||
	    (capacity & (uint16_t)(capacity - 1U)) != 0U) {
		return INTEL_AX211_INVALID;
	}

	/* Succeeded: the ring is empty. */
	kern_memset(ring, 0, sizeof(*ring));
	ring->capacity = capacity;
	ring->queue = queue;
	return INTEL_AX211_SUCCESS;
}

/*
 * Reserves the next slot of a ring.
 */
int
drv_intel_ax211_ring_reserve(
	struct intel_ax211_ring *ring,
	struct intel_ax211_ring_token *token)
{
	/* Refuses missing arguments and an uninitialized ring. */
	if (ring == NULL || token == NULL || ring->capacity == 0U)
		return INTEL_AX211_INVALID;

	/* Refuses a full ring. */
	if (ring->used == ring->capacity)
		return INTEL_AX211_FULL;

	/* Succeeded: reports the slot and advances the head. */
	token->queue = ring->queue;
	token->index = (uint8_t)ring->head;
	ring->head = (uint16_t)((ring->head + 1U) & (ring->capacity - 1U));
	ring->used++;
	return INTEL_AX211_SUCCESS;
}

/*
 * Retires the oldest slot of a ring, which token must name.
 */
int
drv_intel_ax211_ring_complete(
	struct intel_ax211_ring *ring,
	const struct intel_ax211_ring_token *token)
{
	/* Refuses missing arguments and an uninitialized ring. */
	if (ring == NULL || token == NULL || ring->capacity == 0U)
		return INTEL_AX211_INVALID;

	/* Refuses an empty ring and a token that is not the oldest slot. */
	if (ring->used == 0U ||
	    token->queue != ring->queue ||
	    token->index != (uint8_t)ring->tail) {
		return INTEL_AX211_STALE;
	}

	/* Succeeded: advances the tail. */
	ring->tail = (uint16_t)((ring->tail + 1U) & (ring->capacity - 1U));
	ring->used--;
	return INTEL_AX211_SUCCESS;
}

/*
 * Returns the number of free slots of a ring; 0 for an invalid ring.
 */
size_t
drv_intel_ax211_ring_available(
	const struct intel_ax211_ring *ring)
{
	/* An invalid ring has no free slot. */
	if (ring == NULL || ring->capacity == 0U || ring->used > ring->capacity)
		return 0;

	/* Reports the free slots. */
	return (size_t)(ring->capacity - ring->used);
}

/*
 * Clears memory through a volatile pointer, so that the clearing stays.
 */
void
drv_intel_ax211_scrub(
	void *memory,
	size_t length)
{
	volatile uint8_t *bytes = (volatile uint8_t *)memory;

	/* Nothing to clear. */
	if (memory == NULL)
		return;

	/* Clears every byte. */
	while (length-- != 0U)
		*bytes++ = 0;
}

/*
 * Copies data into a staging buffer, clearing it first.
 */
int
drv_intel_ax211_staging_set(
	struct intel_ax211_staging *staging,
	const void *data,
	size_t length)
{
	/* Refuses a missing buffer and missing data. */
	if (staging == NULL || (data == NULL && length != 0U))
		return INTEL_AX211_INVALID;
	drv_intel_ax211_staging_clear(staging);

	/* Refuses data larger than the buffer. */
	if (length > sizeof(staging->bytes))
		return INTEL_AX211_OVERFLOW;

	/* Succeeded: copies the data. */
	if (length != 0U)
		kern_memcpy(staging->bytes, data, length);
	staging->length = length;
	return INTEL_AX211_SUCCESS;
}

/*
 * Clears a staging buffer.
 */
void
drv_intel_ax211_staging_clear(
	struct intel_ax211_staging *staging)
{
	/* Nothing to clear. */
	if (staging == NULL)
		return;

	/* Clears the bytes and the length. */
	drv_intel_ax211_scrub(staging->bytes, sizeof(staging->bytes));
	staging->length = 0;
}

/* Reads a little-endian 16-bit value. */
static uint16_t
ax211_get_le16(
	const uint8_t *bytes)
{
	/* Assembles the value from its two bytes, least significant first. */
	return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

/* Reads a little-endian 32-bit value. */
static uint32_t
ax211_get_le32(
	const uint8_t *bytes)
{
	/* Assembles the word from its four bytes, least significant first. */
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	       ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/* Stores a 16-bit value little-endian. */
static void
ax211_put_le16(
	uint8_t *bytes,
	uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

/* Stores a 32-bit value little-endian. */
static void
ax211_put_le32(
	uint8_t *bytes,
	uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

/* Stores a 64-bit value little-endian. */
static void
ax211_put_le64(
	uint8_t *bytes,
	uint64_t value)
{
	unsigned int index;

	/* Stores the eight bytes, least significant first. */
	for (index = 0; index < 8; index++)
		bytes[index] = (uint8_t)(value >> (index * 8));
}

/*
 * Rounds a record length up to whole words and checks that the record fits
 * in what remains of the file.
 */
static int
ax211_tlv_span(
	uint32_t value,
	size_t remaining,
	size_t *span)
{
	size_t length = (size_t)value;

	/* Refuses a length that overflows when rounded. */
	if (length > SIZE_MAX - 3U)
		return INTEL_AX211_OVERFLOW;

	/* Refuses a rounded length beyond the file. */
	length = (length + 3U) & ~(size_t)3U;
	if (length > remaining)
		return INTEL_AX211_TRUNCATED;

	/* Succeeded: reports the span. */
	*span = length;
	return INTEL_AX211_SUCCESS;
}

/*
 * Reports whether a firmware record type is one the parser skips: 1 when it
 * is, 0 when not.
 */
static int
ax211_ignored_firmware_tlv(
	uint32_t type)
{
	/* Types 1 to 18 and the records between the sections and the flags. */
	if ((type >= 1U && type <= 18U) ||
	    (type >= AX211_RECORD_WAKE_SECTION &&
	     type <= AX211_RECORD_CALIBRATION_DEFAULTS) ||
	    (type >= AX211_RECORD_CIPHER_SCHEMES &&
	     type <= AX211_RECORD_SCAN_CHANNEL_LIMIT)) {
		return 1;
	}

	/* The paging, sniffer, SDIO and trace records. */
	if (type == AX211_RECORD_PAGED_SIZE ||
	    type == AX211_RECORD_SNIFFER_SECTION ||
	    type == AX211_RECORD_SDIO_DMA_ADDRESS ||
	    type == AX211_RECORD_TRACE_DESTINATION ||
	    type == AX211_RECORD_TRACE_SETUP ||
	    type == AX211_RECORD_TRACE_TRIGGER) {
		return 1;
	}

	/* The scan-limit to trace-address and recovery to revision ranges. */
	if ((type >= AX211_RECORD_BACKGROUND_SCAN_LIMITS &&
	     type <= AX211_RECORD_LOWER_CPU_TRACE_ADDRESSES) ||
	    (type >= AX211_RECORD_RECOVERY_BUFFER &&
	     type <= AX211_RECORD_RADIO_INTEGRATION_REVISION)) {
		return 1;
	}

	/* The PNVM records and the address records 65 to 69. */
	if (type == AX211_RECORD_PNVM_REVISION ||
	    type == AX211_RECORD_PNVM_BOARD ||
	    type == 65U ||
	    (type >= AX211_RECORD_SECTION_TABLE_ADDRESS && type <= 69U) ||
	    type == AX211_RECORD_PNVM_PAYLOAD) {
		return 1;
	}

	/* The station and beacon limits, and type 0x101. */
	if (type == AX211_RECORD_STATION_LIMIT ||
	    type == 0x101U ||
	    type == AX211_RECORD_BEACON_LIMIT) {
		return 1;
	}

	/* The debug configuration records. */
	if ((type >= 0x1000000U && type <= 0x1000004U) ||
	    (type >= AX211_RECORD_TRACE_RANGE_FIRST &&
	     type <= AX211_RECORD_TRACE_RANGE_LAST) ||
	    type == 0x100000bU ||
	    type == 0x100000cU) {
		return 1;
	}

	/* Type 1092. */
	if (type == 1092U)
		return 1;

	/* The type is not skipped. */
	return 0;
}

/*
 * Finishes one collected PNVM board: reports it in *manifest when it has a
 * revision, sections, and the given MAC and radio.
 */
static int
ax211_pnvm_finish(
	const struct intel_ax211_pnvm_manifest *candidate,
	int active,
	int version_seen,
	int hardware_match,
	uint16_t mac_type,
	uint16_t rf_id,
	struct intel_ax211_pnvm_manifest *manifest)
{
	/* No board of the signature was collected. */
	if (!active)
		return INTEL_AX211_MISSING;

	/* Refuses a board without a revision or sections. */
	if (!version_seen || candidate->section_count == 0U)
		return INTEL_AX211_MISSING;

	/* Refuses a board for another MAC or radio. */
	if (!hardware_match ||
	    candidate->mac_type != mac_type ||
	    candidate->rf_id != rf_id) {
		return INTEL_AX211_IDENTITY_MISMATCH;
	}

	/* Succeeded: reports the board. */
	*manifest = *candidate;
	return INTEL_AX211_SUCCESS;
}
