/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PowerVR firmware file: checking its trailing description and
 * placing its program.
 *
 * The description sits in the file's last 4 KiB: a fixed header, then
 * the table of sections.  The device information (errata, enhancements,
 * features and the features' values) sits just before it.  Every field is
 * little endian and read a byte at a time, so the file need not be
 * aligned and the parser runs on any host.
 */

#include "drivers/gpu/powervr/firmware-image.h"
#include <kern/kcrt.h>
#include <uapi/errno.h>

/*
 * The trailing description: where it starts, its header and its table.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_fw_info.h
 * (FW_BLOCK_SIZE 4 KiB, PVR_FW_INFO_VERSION 3, PVR_FW_FLAGS_OPEN_SOURCE
 * bit 0, and the layouts of struct pvr_fw_info_header (48 bytes),
 * struct pvr_fw_layout_entry (24 bytes) and struct
 * pvr_fw_device_info_header (32 bytes)); pvr_fw.c
 * (FW_MAX_SUPPORTED_MAJOR_VERSION 1).
 */
#define FW_BLOCK_BYTES			0x1000U
#define FW_INFO_VERSION			3U
#define FW_FLAG_OPEN_SOURCE		0x1U
#define FW_MAJOR_VERSION_MAX		1U
#define FW_HEADER_BYTES			48U
#define FW_HEADER_INFO_VERSION		0U
#define FW_HEADER_LENGTH		4U
#define FW_HEADER_SECTION_COUNT		8U
#define FW_HEADER_SECTION_BYTES		12U
#define FW_HEADER_BVNC			16U
#define FW_HEADER_PAGE_SIZE		24U
#define FW_HEADER_FLAGS			28U
#define FW_HEADER_MAJOR			32U
#define FW_HEADER_MINOR			34U
#define FW_HEADER_BUILD			36U
#define FW_HEADER_DEVICE_INFO_BYTES	40U
#define FW_SECTION_BYTES		24U
#define FW_SECTION_ID			0U
#define FW_SECTION_MEMORY		4U
#define FW_SECTION_BASE			8U
#define FW_SECTION_MAX_SIZE		12U
#define FW_SECTION_ALLOC_SIZE		16U
#define FW_SECTION_ALLOC_OFFSET		20U
#define FW_DEVICE_HEADER_BYTES		32U

/*
 * Which features carry a value in the parameter list, one bit per feature
 * position (positions 0 to 63, then 64 to 70).
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_device_info.c
 * (features_mapping: the FEATURE_MAPPING_VALUE rows).
 */
#define FW_VALUED_FEATURES_LOW		0x92185c10f7b07e0aULL
#define FW_VALUED_FEATURES_HIGH		0x1cULL

/*
 * The ELF program the description is attached to: a 32-bit little-endian
 * file whose loadable segments are copied into the sections.
 *
 * Values referenced:
 * System V Application Binary Interface, ELF header and program header
 * (e_ident, e_phoff at 28, e_phentsize at 42, e_phnum at 44; p_type,
 * p_offset, p_vaddr, p_filesz and p_memsz of a 32-byte entry; PT_LOAD 1).
 */
#define ELF_IDENT_CLASS			4U
#define ELF_IDENT_DATA			5U
#define ELF_CLASS_32			1U
#define ELF_DATA_LITTLE			1U
#define ELF_HEADER_BYTES		52U
#define ELF_PROGRAM_OFFSET		28U
#define ELF_PROGRAM_ENTRY_BYTES		42U
#define ELF_PROGRAM_COUNT		44U
#define ELF_SEGMENT_BYTES		32U
#define ELF_SEGMENT_TYPE		0U
#define ELF_SEGMENT_FILE_OFFSET		4U
#define ELF_SEGMENT_ADDRESS		8U
#define ELF_SEGMENT_FILE_SIZE		16U
#define ELF_SEGMENT_MEMORY_SIZE		20U
#define ELF_SEGMENT_LOAD		1U

/*
 * The one core whose erratum the firmware cannot name.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/gpu/drm/img/img-rogue/hwdefs/rogue/km/cores/rgxcore_km_36.50.54.182.h
 * (the core 36.50.54.182 carries BRN 71317).
 */
#define HOST_QUIRK_BRANCH		36U
#define HOST_QUIRK_VERSION		50U
#define HOST_QUIRK_UNITS		54U
#define HOST_QUIRK_CONFIG		182U

static uint16_t get16(const uint8_t *bytes);
static uint32_t get32(const uint8_t *bytes);
static uint64_t get64(const uint8_t *bytes);
static int read_header(const uint8_t *bytes, size_t size, uint64_t core_bvnc, struct powervr_fw_image *image, uint32_t *device_info_bytes);
static int read_sections(struct powervr_fw_image *image, size_t table);
static int add_section_size(struct powervr_fw_image *image, const struct powervr_fw_section *section);
static int check_section_offsets(const struct powervr_fw_image *image);
static int read_device_info(struct powervr_fw_image *image, uint32_t device_info_bytes);
static int read_features(struct powervr_fw_image *image, const uint8_t *masks, uint64_t mask_words, const uint8_t *parameters, uint64_t parameter_words);
static int feature_is_valued(unsigned feature);
static uint8_t *section_destination(const struct powervr_fw_image *image, uint32_t address, uint32_t length, uint8_t *code, uint8_t *data);
static int load_segment(const struct powervr_fw_image *image, const uint8_t *segment, uint8_t *code, uint8_t *data);

/*
 * Packs a core's branch, version, unit count and configuration the way
 * the firmware names the core it was built for.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_device.h
 * (pvr_gpu_id_to_packed_bvnc: 16 bits each, branch highest).
 */
uint64_t
powervr_bvnc_pack(
	uint32_t branch,
	uint32_t version,
	uint32_t units,
	uint32_t config)
{
	uint64_t packed;

	/* Each field takes 16 bits, branch in the highest. */
	packed = ((uint64_t)(branch & 0xffffU) << 48) |
		 ((uint64_t)(version & 0xffffU) << 32) |
		 ((uint64_t)(units & 0xffffU) << 16) |
		 (uint64_t)(config & 0xffffU);

	/* Succeeded: the packed identity. */
	return packed;
}

/*
 * Checks a firmware file and describes it.
 */
int
powervr_fw_image_parse(
	const uint8_t *bytes,
	size_t size,
	uint64_t core_bvnc,
	struct powervr_fw_image *image)
{
	uint32_t device_info_bytes;
	int error;

	/* Starts from an empty description, so a failure leaves nothing half read. */
	kern_memset(image, 0, sizeof(*image));

	/* Refuses a file that cannot hold the description: under one block, or not whole blocks. */
	if (bytes == NULL || size < FW_BLOCK_BYTES)
		return EINVAL;
	if ((size % FW_BLOCK_BYTES) != 0U)
		return EINVAL;

	/* Reads the header and checks the file is for this core and this driver. */
	error = read_header(bytes, size, core_bvnc, image, &device_info_bytes);
	if (error != 0)
		return error;

	/* Reads the section table and sums each allocation's size. */
	error = read_sections(image, size - FW_BLOCK_BYTES + FW_HEADER_BYTES);
	if (error != 0)
		return error;

	/* Checks every section stays inside the allocation it names. */
	error = check_section_offsets(image);
	if (error != 0)
		return error;

	/* Reads the errata, enhancements and features. */
	error = read_device_info(image, device_info_bytes);
	if (error != 0)
		return error;

	/* Adds the erratum the firmware cannot name, for the one core that has it. */
	if (core_bvnc == powervr_bvnc_pack(HOST_QUIRK_BRANCH, HOST_QUIRK_VERSION, HOST_QUIRK_UNITS, HOST_QUIRK_CONFIG))
		image->host_quirks |= POWERVR_HOST_QUIRK_SMALL_PAGES;

	/* Succeeded: the description refers to the caller's bytes. */
	return 0;
}

/*
 * Finds a section by its identifier; NULL when the layout has none.
 */
const struct powervr_fw_section *
powervr_fw_image_section(
	const struct powervr_fw_image *image,
	uint32_t id)
{
	uint32_t index;

	/* Looks through the table in file order; identifiers are unique in practice. */
	for (index = 0; index < image->section_count; index++) {
		if (image->sections[index].id == id)
			return &image->sections[index];
	}

	/* The layout has no such section. */
	return NULL;
}

/*
 * Reports whether the core has a feature.
 */
int
powervr_fw_image_has_feature(
	const struct powervr_fw_image *image,
	unsigned feature)
{
	/* A position past the known features is never set. */
	if (feature >= POWERVR_FEATURE_COUNT)
		return 0;

	/* One bit per feature, 64 to a word. */
	if ((image->features[feature / 64U] & (1ULL << (feature % 64U))) == 0U)
		return 0;

	/* Succeeded: the core has the feature. */
	return 1;
}

/*
 * Reads the value of a valued feature; ENOENT when the core lacks it.
 */
int
powervr_fw_image_feature_value(
	const struct powervr_fw_image *image,
	unsigned feature,
	uint64_t *value)
{
	int present;
	int valued;

	/* Refuses a feature the core lacks. */
	present = powervr_fw_image_has_feature(image, feature);
	if (!present)
		return ENOENT;

	/* Refuses a feature that carries no value. */
	valued = feature_is_valued(feature);
	if (!valued)
		return EINVAL;

	/* Succeeded: the value the firmware gives for the feature. */
	*value = image->feature_values[feature];
	return 0;
}

/*
 * Reports whether the core has an erratum the firmware names.
 */
int
powervr_fw_image_has_quirk(
	const struct powervr_fw_image *image,
	unsigned quirk)
{
	/* A position past the known errata is never set. */
	if (quirk >= POWERVR_QUIRK_COUNT)
		return 0;

	/* One bit per erratum. */
	if ((image->quirks & (1ULL << quirk)) == 0U)
		return 0;

	/* Succeeded: the core has the erratum. */
	return 1;
}

/*
 * Places the program's loadable segments in the code and data
 * allocations.
 */
int
powervr_fw_image_load(
	const struct powervr_fw_image *image,
	uint8_t *code,
	uint8_t *data)
{
	const uint8_t *bytes;
	uint32_t program_offset;
	uint32_t entry_bytes;
	uint32_t count;
	uint32_t index;
	int error;

	/* Refuses the core allocations, which the MIPS firmware does not use. */
	if (image->core_code_size != 0U || image->core_data_size != 0U)
		return EINVAL;

	/* Checks the program is a 32-bit little-endian ELF file. */
	bytes = image->bytes;
	if (bytes[0] != 0x7fU || bytes[1] != 'E' || bytes[2] != 'L' || bytes[3] != 'F')
		return EINVAL;
	if (bytes[ELF_IDENT_CLASS] != ELF_CLASS_32 || bytes[ELF_IDENT_DATA] != ELF_DATA_LITTLE)
		return EINVAL;

	/* Finds the program header table and checks it lies inside the file. */
	program_offset = get32(bytes + ELF_PROGRAM_OFFSET);
	entry_bytes = get16(bytes + ELF_PROGRAM_ENTRY_BYTES);
	count = get16(bytes + ELF_PROGRAM_COUNT);
	if (entry_bytes < ELF_SEGMENT_BYTES)
		return EINVAL;
	if (program_offset < ELF_HEADER_BYTES || program_offset > image->size)
		return EINVAL;
	if ((uint64_t)count * entry_bytes > image->size - program_offset)
		return EINVAL;

	/* Copies every loadable segment into the section that holds its address. */
	for (index = 0; index < count; index++) {
		error = load_segment(image, bytes + program_offset + (size_t)index * entry_bytes, code, data);
		if (error != 0)
			return error;
	}

	/* Succeeded: the allocations hold the program. */
	return 0;
}

/* Reads a little-endian 16-bit field. */
static uint16_t
get16(
	const uint8_t *bytes)
{
	uint16_t word;

	/* Lowest byte first. */
	word = (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));

	/* Succeeded: the field. */
	return word;
}

/* Reads a little-endian 32-bit field. */
static uint32_t
get32(
	const uint8_t *bytes)
{
	uint32_t word;

	/* Lowest byte first. */
	word = (uint32_t)bytes[0] |
	       ((uint32_t)bytes[1] << 8) |
	       ((uint32_t)bytes[2] << 16) |
	       ((uint32_t)bytes[3] << 24);

	/* Succeeded: the field. */
	return word;
}

/* Reads a little-endian 64-bit field. */
static uint64_t
get64(
	const uint8_t *bytes)
{
	uint64_t word;

	/* The low half first. */
	word = (uint64_t)get32(bytes) | ((uint64_t)get32(bytes + 4) << 32);

	/* Succeeded: the field. */
	return word;
}

/* Reads the fixed header and checks the file is meant for this core and driver. */
static int
read_header(
	const uint8_t *bytes,
	size_t size,
	uint64_t core_bvnc,
	struct powervr_fw_image *image,
	uint32_t *device_info_bytes)
{
	const uint8_t *header;
	uint32_t info_version;
	uint32_t header_bytes;
	uint32_t section_bytes;
	uint32_t flags;

	/* The header opens the last block. */
	header = bytes + size - FW_BLOCK_BYTES;

	/* Refuses another description format or a header of another size. */
	info_version = get32(header + FW_HEADER_INFO_VERSION);
	header_bytes = get32(header + FW_HEADER_LENGTH);
	section_bytes = get32(header + FW_HEADER_SECTION_BYTES);
	if (info_version != FW_INFO_VERSION)
		return ENOEXEC;
	if (header_bytes != FW_HEADER_BYTES || section_bytes != FW_SECTION_BYTES)
		return EINVAL;

	/* Refuses a section table longer than the driver keeps. */
	image->section_count = get32(header + FW_HEADER_SECTION_COUNT);
	if (image->section_count > POWERVR_FW_SECTION_MAX)
		return EINVAL;

	/* Refuses a closed build and a major version the interface does not match. */
	flags = get32(header + FW_HEADER_FLAGS);
	image->version_major = get16(header + FW_HEADER_MAJOR);
	image->version_minor = get16(header + FW_HEADER_MINOR);
	image->version_build = get32(header + FW_HEADER_BUILD);
	if ((flags & FW_FLAG_OPEN_SOURCE) == 0U)
		return ENOEXEC;
	if (image->version_major == 0U || image->version_major > FW_MAJOR_VERSION_MAX)
		return ENOEXEC;

	/* Refuses firmware built for another core. */
	image->bvnc = get64(header + FW_HEADER_BVNC);
	if (image->bvnc != core_bvnc)
		return ENOEXEC;

	/* Keeps the processor's page size and where the device information starts. */
	image->page_size = get32(header + FW_HEADER_PAGE_SIZE);
	*device_info_bytes = get32(header + FW_HEADER_DEVICE_INFO_BYTES);
	if (*device_info_bytes < FW_DEVICE_HEADER_BYTES || *device_info_bytes > size - FW_BLOCK_BYTES)
		return EINVAL;

	/* Succeeded: the file is for this core; the description refers to the bytes. */
	image->bytes = bytes;
	image->size = size;
	return 0;
}

/* Reads the section table that follows the header and sums each allocation. */
static int
read_sections(
	struct powervr_fw_image *image,
	size_t table)
{
	struct powervr_fw_section *section;
	const uint8_t *entry;
	uint32_t index;
	int error;

	/* Refuses a table that runs past the end of the file. */
	if (table + (size_t)image->section_count * FW_SECTION_BYTES > image->size)
		return EINVAL;

	/* Reads each entry, refusing an empty or wrapping address range. */
	for (index = 0; index < image->section_count; index++) {
		entry = image->bytes + table + (size_t)index * FW_SECTION_BYTES;
		section = &image->sections[index];
		section->id = get32(entry + FW_SECTION_ID);
		section->memory = get32(entry + FW_SECTION_MEMORY);
		section->base = get32(entry + FW_SECTION_BASE);
		section->max_size = get32(entry + FW_SECTION_MAX_SIZE);
		section->alloc_size = get32(entry + FW_SECTION_ALLOC_SIZE);
		section->alloc_offset = get32(entry + FW_SECTION_ALLOC_OFFSET);
		if (section->alloc_size == 0U || section->base > UINT32_MAX - section->alloc_size)
			return EINVAL;

		/* Adds the section to its allocation's size. */
		error = add_section_size(image, section);
		if (error != 0)
			return error;
	}

	/* Succeeded: the table and the allocation sizes are known. */
	return 0;
}

/* Adds one section's size to the allocation it lives in. */
static int
add_section_size(
	struct powervr_fw_image *image,
	const struct powervr_fw_section *section)
{
	uint32_t *total;

	/* Picks the allocation the section names; a section with none adds nothing. */
	switch (section->memory) {
	case POWERVR_MEMORY_NONE:
		return 0;
	case POWERVR_MEMORY_CODE:
		total = &image->code_size;
		break;
	case POWERVR_MEMORY_DATA:
		total = &image->data_size;
		break;
	case POWERVR_MEMORY_CORE_CODE:
		total = &image->core_code_size;
		break;
	case POWERVR_MEMORY_CORE_DATA:
		total = &image->core_data_size;
		break;
	default:
		return EINVAL;
	}

	/* Refuses a total that would wrap. */
	if (*total > UINT32_MAX - section->alloc_size)
		return EINVAL;

	/* Succeeded: the allocation grows by the section. */
	*total += section->alloc_size;
	return 0;
}

/* Checks every section stays inside the allocation it names. */
static int
check_section_offsets(
	const struct powervr_fw_image *image)
{
	const struct powervr_fw_section *section;
	uint32_t limit;
	uint32_t index;

	/* Compares each section's end with its allocation's size. */
	for (index = 0; index < image->section_count; index++) {
		section = &image->sections[index];

		/* Finds the size of the allocation the section lives in. */
		switch (section->memory) {
		case POWERVR_MEMORY_CODE:
			limit = image->code_size;
			break;
		case POWERVR_MEMORY_DATA:
			limit = image->data_size;
			break;
		case POWERVR_MEMORY_CORE_CODE:
			limit = image->core_code_size;
			break;
		case POWERVR_MEMORY_CORE_DATA:
			limit = image->core_data_size;
			break;
		default:
			continue;
		}

		/* Refuses a section that would be written past its allocation. */
		if (section->alloc_offset > limit || section->alloc_size > limit - section->alloc_offset)
			return EINVAL;
	}

	/* Succeeded: every section fits its allocation. */
	return 0;
}

/* Reads the errata, enhancements, features and the features' values. */
static int
read_device_info(
	struct powervr_fw_image *image,
	uint32_t device_info_bytes)
{
	const uint8_t *info;
	const uint8_t *words;
	uint64_t quirk_words;
	uint64_t enhancement_words;
	uint64_t feature_words;
	uint64_t parameter_words;
	uint64_t total_words;
	int error;

	/* The device information ends where the last block starts. */
	info = image->bytes + image->size - FW_BLOCK_BYTES - device_info_bytes;
	quirk_words = get64(info);
	enhancement_words = get64(info + 8);
	feature_words = get64(info + 16);
	parameter_words = get64(info + 24);

	/* Refuses lists that do not fit the space the header gave them. */
	total_words = (device_info_bytes - FW_DEVICE_HEADER_BYTES) / 8U;
	if (quirk_words > total_words || enhancement_words > total_words)
		return EINVAL;
	if (feature_words > total_words || parameter_words > total_words)
		return EINVAL;
	if (quirk_words + enhancement_words + feature_words + parameter_words > total_words)
		return EINVAL;

	/* Keeps the first word of errata and of enhancements; later words name nothing known. */
	words = info + FW_DEVICE_HEADER_BYTES;
	if (quirk_words != 0U)
		image->quirks = get64(words) & ((1ULL << POWERVR_QUIRK_COUNT) - 1U);
	words += quirk_words * 8U;
	if (enhancement_words != 0U)
		image->enhancements = get64(words);
	words += enhancement_words * 8U;

	/* Reads the feature masks and the values that go with them. */
	error = read_features(image, words, feature_words, words + feature_words * 8U, parameter_words);
	if (error != 0)
		return error;

	/* Succeeded: the core's properties are known. */
	return 0;
}

/*
 * Reads the feature masks and gives each valued feature the next value of
 * the parameter list, in feature order.
 */
static int
read_features(
	struct powervr_fw_image *image,
	const uint8_t *masks,
	uint64_t mask_words,
	const uint8_t *parameters,
	uint64_t parameter_words)
{
	uint64_t next_parameter;
	unsigned feature;
	int present;
	int valued;

	/* Keeps the masks' words that cover known features. */
	if (mask_words > 0U)
		image->features[0] = get64(masks);
	if (mask_words > 1U)
		image->features[1] = get64(masks + 8) & ((1ULL << (POWERVR_FEATURE_COUNT - 64U)) - 1U);

	/* Walks the features in order, taking a value for each present valued one. */
	next_parameter = 0;
	for (feature = 0; feature < POWERVR_FEATURE_COUNT; feature++) {
		present = powervr_fw_image_has_feature(image, feature);
		valued = feature_is_valued(feature);
		if (!present || !valued)
			continue;

		/* Refuses a list that runs out before the features do. */
		if (next_parameter >= parameter_words)
			return EINVAL;

		/* Takes the next value. */
		image->feature_values[feature] = get64(parameters + next_parameter * 8U);
		next_parameter++;
	}

	/* Succeeded: every present valued feature has its value. */
	return 0;
}

/* Reports whether a feature carries a value in the parameter list. */
static int
feature_is_valued(
	unsigned feature)
{
	/* The low 64 positions, then the rest. */
	if (feature < 64U) {
		if ((FW_VALUED_FEATURES_LOW & (1ULL << feature)) == 0U)
			return 0;
	} else {
		if ((FW_VALUED_FEATURES_HIGH & (1ULL << (feature - 64U))) == 0U)
			return 0;
	}

	/* Succeeded: the feature carries a value. */
	return 1;
}

/*
 * Finds where an address range of the firmware's processor lands in the
 * allocations; NULL when no single section holds it all.
 */
static uint8_t *
section_destination(
	const struct powervr_fw_image *image,
	uint32_t address,
	uint32_t length,
	uint8_t *code,
	uint8_t *data)
{
	const struct powervr_fw_section *section;
	uint32_t index;
	uint32_t offset;

	/* Refuses an empty or wrapping range. */
	if (length == 0U || address > UINT32_MAX - length)
		return NULL;

	/* Looks for the section that holds the whole range. */
	for (index = 0; index < image->section_count; index++) {
		section = &image->sections[index];
		if (address < section->base)
			continue;
		offset = address - section->base;
		if (offset >= section->alloc_size || length > section->alloc_size - offset)
			continue;

		/* Lands in the allocation the section names. */
		if (section->memory == POWERVR_MEMORY_CODE)
			return code + section->alloc_offset + offset;
		if (section->memory == POWERVR_MEMORY_DATA)
			return data + section->alloc_offset + offset;

		/* A section of another kind cannot receive the program. */
		return NULL;
	}

	/* No section holds the range. */
	return NULL;
}

/* Copies one loadable segment into its section, clearing what the file does not cover. */
static int
load_segment(
	const struct powervr_fw_image *image,
	const uint8_t *segment,
	uint8_t *code,
	uint8_t *data)
{
	uint8_t *destination;
	uint32_t type;
	uint32_t file_offset;
	uint32_t address;
	uint32_t file_size;
	uint32_t memory_size;

	/* Skips a segment that is not loaded. */
	type = get32(segment + ELF_SEGMENT_TYPE);
	if (type != ELF_SEGMENT_LOAD)
		return 0;

	/* Reads where the segment comes from and goes to. */
	file_offset = get32(segment + ELF_SEGMENT_FILE_OFFSET);
	address = get32(segment + ELF_SEGMENT_ADDRESS);
	file_size = get32(segment + ELF_SEGMENT_FILE_SIZE);
	memory_size = get32(segment + ELF_SEGMENT_MEMORY_SIZE);

	/* Refuses a segment that reads past the file or claims more bytes than it occupies. */
	if (file_size > memory_size)
		return EINVAL;
	if (file_offset > image->size || file_size > image->size - file_offset)
		return EINVAL;

	/* Finds the section that holds the whole segment. */
	destination = section_destination(image, address, memory_size, code, data);
	if (destination == NULL)
		return EINVAL;

	/* Copies the file's bytes and clears the rest of the segment. */
	kern_memcpy(destination, image->bytes + file_offset, file_size);
	kern_memset(destination + file_size, 0, memory_size - file_size);

	/* Succeeded: the segment is in place. */
	return 0;
}
