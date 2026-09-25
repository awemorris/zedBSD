/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Architecture-independent boot parameters and boot-source ownership.
 *
 * This header is the public boot-interface ledger.  Declarations are
 * appended or changed only after a recorded architecture/API
 * decision, not as an implementation-refactoring convenience.
 */

#ifndef KERN_BOOT_H
#define KERN_BOOT_H

#define KERN_HANDOFF_MAGIC				0x48323842U
#define KERN_HANDOFF_VERSION_PC98			2U
#define KERN_HANDOFF_VERSION_MULTIBOOT			3U
#define KERN_HANDOFF_VERSION_SUN4U			4U
#define KERN_HANDOFF_VERSION_X68K			5U

#define KERN_BOOT_SOURCE_SLOT_COUNT			4U

#define KERN_BOOT_PARAMETERS_TEXT_MAX			3071
#define KERN_BOOT_PARAMETERS_STORAGE_SIZE		(KERN_BOOT_PARAMETERS_TEXT_MAX + 1)
#define KERN_BOOT_PARAMETERS_DEFAULT_TEXT		KERN_IMAGE_BOOT_PARAMETERS_TEXT
#define KERN_BOOT_PARAMETERS_INIT_PATH_MAX		255U
#define KERN_BOOT_PARAMETERS_UNKNOWN_NAME_MAX		31U

#define KERN_BOOT_PARAMETER_OFFSET_ABSENT		UINT16_MAX
#define KERN_BOOT_PARAMETER_RECORD_MAGIC		0x31525042
#define KERN_BOOT_PARAMETER_RECORD_VERSION		1
#define KERN_BOOT_PARAMETER_RECORD_FLAG_TEXT		(1 << 0)
#define KERN_BOOT_PARAMETER_RECORD_HEADER_SIZE		16
#define KERN_BOOT_PARAMETER_RECORD_SIZE 		(KERN_BOOT_PARAMETER_RECORD_HEADER_SIZE + KERN_BOOT_PARAMETERS_STORAGE_SIZE)
#define KERN_BOOT_PARAMETER_RECORD_MAGIC_OFFSET		0U
#define KERN_BOOT_PARAMETER_RECORD_VERSION_OFFSET 	4U
#define KERN_BOOT_PARAMETER_RECORD_SIZE_OFFSET		6U
#define KERN_BOOT_PARAMETER_RECORD_FLAGS_OFFSET		8U
#define KERN_BOOT_PARAMETER_RECORD_LENGTH_OFFSET	10U
#define KERN_BOOT_PARAMETER_RECORD_RESERVED_OFFSET	12U
#define KERN_BOOT_PARAMETER_RECORD_TEXT_OFFSET 		16U

#define KERN_PARTITION_SCHEME_LBA			0U
#define KERN_PARTITION_SCHEME_MBR			1U
#define KERN_PARTITION_SCHEME_SUN			2U
#define KERN_PARTITION_SCHEME_X68K			3U
#define KERN_PARTITION_SCHEME_GPT			4U
#define KERN_PARTITION_INDEX_UNKNOWN			0U

#define KERN_BOOT_PARTITION_LBA_UNKNOWN			0xffffffffU

#define KERN_IMAGE_BOOT_PARAMETERS_TEXT 		"overlay-root=boot0:rootfs.img overlay-data=boot0:data.img swap0=boot0:swapfile"

#define KERN_MEMORY_AVAILABLE				1U
#define KERN_MEMORY_RESERVED				2U

#define KERN_BOOT_PROVENANCE_VERSION			1U
#define KERN_BOOT_PROVENANCE_SIZE			88U
#define KERN_BOOT_SOURCE_SELECTOR_SIZE			46U

/* XXX: To be removed. */
#define KERN_X68K_HANDOFF_MAGIC				0x58363848U /* "X68H" */
#define KERN_X68K_HANDOFF_VERSION			1U
#define KERN_X68K_MAX_MEMORY_REGIONS			4U

/* XXX: To be removed. Use the common one. */
#define KERN_PC98_HANDOFF_COMMON_SIZE			24
#define KERN_PC98_PARAMETER_RECORD_OFFSET		KERN_PC98_HANDOFF_COMMON_SIZE
#define KERN_PC98_PARAMETER_HANDOFF_SIZE		(KERN_PC98_HANDOFF_COMMON_SIZE + KERN_BOOT_PARAMETER_RECORD_SIZE)

/* XXX: To be removed. Use the common one. */
#define KERN_RPI4_HANDOFF_MAGIC	0x34495052U

/* XXX: To be removed. Use the common one. */
#define KERN_SUN4U_HANDOFF_MAGIC	0x53345548U /* "S4UH" */
#define KERN_SUN4U_HANDOFF_VERSION	1U
#define KERN_SUN4U_MAX_MEMORY_RANGES	16U
#define KERN_SUN4U_BOOTPATH_SIZE	256U

#ifndef __ASSEMBLER__

#include <stddef.h>
#include <stdint.h>

/* Declared by the name lookup; only pointers are used. */
struct path;

/*
 * The FAT variants a boot source may be.
 *
 * This lives here, rather than with the file system in <kern/fat.h>, because
 * the boot-source declarations below name it and the boot loader reads this
 * header: an enumeration cannot be declared without its members, and the file
 * system header carries the whole VFS with it.
 */
enum bootfat_type {
	KERN_FAT12 = 12,
	KERN_FAT16 = 16,
	KERN_FAT32 = 32,
};

enum kern_boot_root_mode {
	KERN_BOOT_ROOT_INVALID = 0,
	KERN_BOOT_ROOT_NATIVE,
	KERN_BOOT_ROOT_OVERLAY,
};

enum kern_boot_parameter_key {
	KERN_BOOT_PARAMETER_BOOT0,
	KERN_BOOT_PARAMETER_BOOT1,
	KERN_BOOT_PARAMETER_BOOT2,
	KERN_BOOT_PARAMETER_BOOT3,
	KERN_BOOT_PARAMETER_ROOTPART,
	KERN_BOOT_PARAMETER_OVERLAY_ROOT,
	KERN_BOOT_PARAMETER_OVERLAY_DATA,
	KERN_BOOT_PARAMETER_SWAP0,
	KERN_BOOT_PARAMETER_SWAP1,
	KERN_BOOT_PARAMETER_SWAP2,
	KERN_BOOT_PARAMETER_SWAP3,
	KERN_BOOT_PARAMETER_INIT,
	KERN_BOOT_PARAMETER_COUNT
};

enum kern_boot_source_failure_stage {
	KERN_BOOT_SOURCE_FAILURE_NONE = 0,
	KERN_BOOT_SOURCE_FAILURE_SELECTOR,
	KERN_BOOT_SOURCE_FAILURE_RESOLVE,
	KERN_BOOT_SOURCE_FAILURE_PARTITION,
	KERN_BOOT_SOURCE_FAILURE_DUPLICATE,
	KERN_BOOT_SOURCE_FAILURE_FILESYSTEM,
	KERN_BOOT_SOURCE_FAILURE_MOUNT,
};

enum kern_boot_device_class {
	KERN_DEV_FDD = 1,
	KERN_DEV_IDE = 2,
	KERN_DEV_SCSI = 3,
	KERN_DEV_SD = 4,
};

enum kern_boot_device_flags {
	KERN_DEV_PRESENT = 1U << 0,
	KERN_DEV_HAS_GEOMETRY = 1U << 1,
	KERN_DEV_BOOT_ORIGIN = 1U << 2,
};

struct kern_boot_handoff {
	uint32_t magic;
	uint16_t version;
	uint16_t size;
	uint8_t device_count;
	uint8_t boot_bios_id;
	uint8_t boot_partition_scheme;
	uint8_t boot_partition_index;
	uint32_t device_table;
	uint32_t bios_gateway;
	uint32_t boot_partition_lba;
} __attribute__((packed));

/*
 * Values are offsets into storage so the structure remains
 * self-contained when a host fixture or a future handoff path copies
 * it.
 */
struct kern_boot_parameters {
	char storage[KERN_BOOT_PARAMETERS_STORAGE_SIZE];
	uint16_t value_offset[KERN_BOOT_PARAMETER_COUNT];
	unsigned unknown_count;
	unsigned unknown_name_truncated;
	char unknown_name[KERN_BOOT_PARAMETERS_UNKNOWN_NAME_MAX + 1U];
};

struct kern_boot_parameter_record {
	uint32_t magic;
	uint16_t version;
	uint16_t size;
	uint16_t flags;
	uint16_t length;
	uint32_t reserved;
	char text[KERN_BOOT_PARAMETERS_STORAGE_SIZE];
} __attribute__((packed));

/* Firmware-discovered boot device descriptor shared with the kernel. */
struct kern_boot_device {
	uint8_t device_class;
	uint8_t display_index;
	uint8_t bios_id;
	uint8_t flags;
	uint16_t sector_size;
	uint16_t cylinders;
	uint8_t heads;
	uint8_t sectors;
	uint8_t controller_location;
	uint8_t reserved[5];
} __attribute__((packed));

struct kern_boot_source_reference {
	unsigned slot;

	/* Matches the stable kernel path limit without importing VFS internals. */
	char relative[256U];
};

struct kern_boot_source_slot {
	/* disk is borrowed from runtime_mount while runtime_mount is non-NULL. */
	struct disk *disk;

	/* Owned private mount until release or promotion. */
	struct mount *mount;

	/*
	 * System-lifetime lookup anchor.  Normally identical to mount; after a
	 * boot filesystem is promoted to the namespace root it remains a borrowed
	 * pointer to that root mount while mount becomes NULL.
	 */
	struct mount *runtime_mount;
	unsigned configured;
	unsigned retained;
	unsigned promoted;
};

struct kern_boot_source_context {
	struct kern_boot_source_slot slot[KERN_BOOT_SOURCE_SLOT_COUNT];
	unsigned failure_slot;
	enum kern_boot_source_failure_stage failure_stage;
	int cleanup_error;
	/* Immutable once set.  Published contexts have system lifetime. */
	unsigned runtime_published;
};

struct kern_boot_partition_identity {
	uint32_t scheme;
	uint32_t index;
	uint64_t first_lba;
	uint64_t block_count;
	uint8_t signature[16];
};

struct kern_boot_provenance {
	uint32_t version;
	uint32_t config_matches;
	struct kern_boot_partition_identity firmware;
	struct kern_boot_partition_identity configuration;
};

/* XXX: To be removed. Use the common one. */
struct kern_pc98_parameter_handoff {
	struct kern_boot_handoff common;
	struct kern_boot_parameter_record parameters;
} __attribute__((packed));

/* XXX: To be removed. Use the common one. */
struct rpi4_boot_handoff {
	struct kern_boot_handoff common;
	uint32_t extension_magic;
	uint16_t extension_version;
	uint16_t extension_size;
	uint64_t fdt_phys;
	uint64_t framebuffer_phys;
	uint64_t framebuffer_size;
	uint64_t sdhci_phys;
	uint32_t framebuffer_width;
	uint32_t framebuffer_height;
	uint32_t framebuffer_pitch;
	uint32_t framebuffer_format;
	uint32_t sdhci_irq;
	uint32_t reserved[3];
} __attribute__((packed));

/* XXX: To be removed. Use the common one. */
struct kern_sun4u_memory_range {
	uint64_t base;
	uint64_t size;
} __attribute__((packed));

/* XXX: To be removed. Use the common one. */
struct kern_sun4u_boot_handoff {
	struct kern_boot_handoff common;
	uint32_t extension_magic;
	uint16_t extension_version;
	uint16_t extension_size;
	uint64_t tick_frequency;
	uint8_t installed_count;
	uint8_t available_count;
	uint8_t boot_channel;
	uint8_t boot_drive;
	struct kern_sun4u_memory_range installed[KERN_SUN4U_MAX_MEMORY_RANGES];
	struct kern_sun4u_memory_range available[KERN_SUN4U_MAX_MEMORY_RANGES];
	uint64_t pci_io_base;
	uint32_t serial_io_offset;
	uint16_t ide_vendor;
	uint16_t ide_device;
	uint16_t ide_primary_command;
	uint16_t ide_primary_control;
	uint16_t ide_secondary_command;
	uint16_t ide_secondary_control;
	char bootpath[KERN_SUN4U_BOOTPATH_SIZE];
} __attribute__((packed));

/*
 * Parse at most input_capacity readable bytes, including the terminating NUL.
 * A NULL input with zero capacity denotes an empty parameter set.  On error,
 * parameters is reset to an empty, safely inspectable result.
 *
 * EINVAL       invalid arguments, syntax, control data, or relative init path
 * EILSEQ       a non-ASCII byte before the terminating NUL
 * E2BIG        text exceeds 3071 bytes or lacks NUL at that maximum boundary
 * EEXIST       a known name occurs more than once
 * ENAMETOOLONG init path is 256 bytes or longer
 */
int
kern_boot_parameters_parse(
	struct kern_boot_parameters *parameters,
	const char *input,
	size_t input_capacity);

const char *
kern_boot_parameters_value(
	const struct kern_boot_parameters *parameters,
	enum kern_boot_parameter_key key);

const char *
kern_boot_parameters_boot(
	const struct kern_boot_parameters *parameters,
	unsigned index);

const char *
kern_boot_parameters_swap(
	const struct kern_boot_parameters *parameters,
	unsigned index);

const char *
kern_boot_parameters_rootpart(
	const struct kern_boot_parameters *parameters);

const char *
kern_boot_parameters_overlay_root(
	const struct kern_boot_parameters *parameters);

const char *
kern_boot_parameters_overlay_data(
	const struct kern_boot_parameters *parameters);

const char *
kern_boot_parameters_init_path(
	const struct kern_boot_parameters *parameters);

unsigned
kern_boot_parameters_unknown_count(
	const struct kern_boot_parameters *parameters);

const char *
kern_boot_parameters_unknown_name(
	const struct kern_boot_parameters *parameters,
	int *truncated);

/* Kernel-global parse-once instance consumed by init and later VFS phases. */
int
kern_boot_parameters_initialize(
	const char *input,
	size_t input_capacity);

const struct kern_boot_parameters *
kern_boot_parameters_current(
	void);

/*
 * True only when the valid kernel-global instance was initialized from an
 * actual parameter source.  This deliberately distinguishes an absent source
 * (NULL, zero capacity) from a present but empty string.
 */
int
kern_boot_parameters_source_present(
	void);

int
kern_boot_provenance_set(
	const struct kern_boot_provenance *record);

const char *
kern_boot_source_selector(
	unsigned configuration);

uint64_t
kern_boot_config_matches(
	void);

int
kern_boot_source_selector_validate(
	const char *selector);

int
kern_boot_source_reference_parse(
	const char *text,
	struct kern_boot_source_reference *reference);

int
kern_boot_source_root_mode(
	const char *rootpart,
	const char *overlay_root,
	const char *overlay_data,
	enum kern_boot_root_mode *mode);

int
kern_boot_source_fat_type_supported(
	enum bootfat_type type);

const char *
kern_boot_source_failure_stage_name(
	enum kern_boot_source_failure_stage stage);

void
kern_boot_source_context_init(
	struct kern_boot_source_context *context);

int
kern_boot_source_context_mount(
	struct kern_boot_source_context *context,
	const struct kern_boot_parameters *parameters,
	struct disk *loader_origin,
	const char *loader_origin_selector);

int
kern_boot_source_lookup(
	struct kern_boot_source_context *context,
	const char *text,
	unsigned *slot_out,
	struct path *path_out);

int
kern_boot_source_retain_slot(
	struct kern_boot_source_context *context,
	unsigned slot);

/*
 * Runtime bootN selectors require every configured private boot mount to
 * survive root selection.  Retain is performed before root selection can
 * release unused mounts; publication happens only after the root namespace
 * and swap-control facade are ready.
 */
int
kern_boot_source_retain_configured(
	struct kern_boot_source_context *context);

int
kern_boot_source_publish_runtime(
	struct kern_boot_source_context *context);

int
kern_boot_source_runtime_lookup(
	struct kern_boot_source_context *context,
	const char *text,
	struct path *path_out);

int
kern_boot_source_find_disk(
	const struct kern_boot_source_context *context,
	const struct disk *disk,
	unsigned *slot_out);

int
kern_boot_source_promote_root(
	struct kern_boot_source_context *context,
	unsigned slot,
	struct mount **root_out);

int
kern_boot_source_release_unused(
	struct kern_boot_source_context *context);

int
kern_boot_source_context_destroy(
	struct kern_boot_source_context *context);

#endif /* __ASSEMBLER__ */

#endif
