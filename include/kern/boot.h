/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Boot handoff and BIOS call
 */

#ifndef ZEDBSD_ABI_H
#define ZEDBSD_ABI_H

#include <stddef.h>
#include <stdint.h>

#include <boot/parameters.h>

/*
 * Shared data contract between the real-mode Stage 1 and 32-bit Stage 2.
 */

#define ZEDBSD_STAGE2_MAGIC	0x53383942U  /* "B98S" */
#define ZEDBSD_HANDOFF_MAGIC	0x48323842U /* "B82H" */

struct boot_stage2_header {
	uint32_t magic;
	uint16_t version;
	uint16_t header_size;
	uint32_t image_size;
	uint32_t entry_offset;
	uint32_t payload_checksum;
} __attribute__((packed));

struct boot_handoff {
	uint32_t magic;
	uint16_t version;
	uint16_t size;
	uint8_t device_count;
	uint8_t boot_bios_id;
	/*
	 * Version 2 treats these bytes as a reserved zero word and selects the
	 * boot partition by LBA.  Version 3 uses an MBR partition index.
	 */
	uint8_t boot_partition_scheme;
	uint8_t boot_partition_index;
	uint32_t device_table;
	uint32_t bios_gateway;
	uint32_t boot_partition_lba;
} __attribute__((packed));

#define ZEDBSD_HANDOFF_VERSION_PC98		2U
#define ZEDBSD_HANDOFF_VERSION_MULTIBOOT	3U
#define ZEDBSD_HANDOFF_VERSION_SUN4U		4U
#define ZEDBSD_HANDOFF_VERSION_X68K		5U
#define ZEDBSD_PARTITION_SCHEME_LBA		0U
#define ZEDBSD_PARTITION_SCHEME_MBR		1U
#define ZEDBSD_PARTITION_SCHEME_SUN		2U
#define ZEDBSD_PARTITION_SCHEME_X68K		3U
#define ZEDBSD_BOOT_PARTITION_LBA_UNKNOWN	0xffffffffU

#define ZEDBSD_X68K_HANDOFF_MAGIC		0x58363848U /* "X68H" */
#define ZEDBSD_X68K_HANDOFF_VERSION		1U
#define ZEDBSD_X68K_MAX_MEMORY_REGIONS		4U

#define ZEDBSD_MEMORY_AVAILABLE			1U
#define ZEDBSD_MEMORY_RESERVED			2U

struct boot_memory_region32 {
	uint32_t base;
	uint32_t size;
	uint32_t type;
} __attribute__((packed));

struct x68k_boot_handoff {
	struct boot_handoff common;
	uint32_t extension_magic;
	uint16_t extension_version;
	uint16_t extension_size;
	uint32_t ram_bytes;
	uint32_t kernel_phys_start;
	uint32_t kernel_phys_end;
	uint32_t loader_phys_start;
	uint32_t loader_phys_end;
	uint32_t memory_region_count;
	struct boot_memory_region32
		memory_regions[ZEDBSD_X68K_MAX_MEMORY_REGIONS];
} __attribute__((packed));

_Static_assert(
	sizeof(struct x68k_boot_handoff) == 104,
	"zedBSD X68k handoff ABI must remain 104 bytes");

enum bios_service {
	ZEDBSD_BIOS_DISK_READ = 1,
	ZEDBSD_BIOS_KEY_READ = 2,
	ZEDBSD_BIOS_KEY_POLL = 3,
	ZEDBSD_BIOS_DISPLAY_RESET = 4,
	ZEDBSD_BIOS_RETURN_MENU = 5,

	/*
	 * Service 6 was the retired IPLware bridge; the number stays unused.
	 */
	ZEDBSD_BIOS_REPROBE = 7,
	ZEDBSD_BIOS_CHAIN_BOOT = 8,
	ZEDBSD_BIOS_CLOCK_SECOND = 9,

	/*
	 * Probe exactly request.bios_id; request.status is a device-class hint.
	 */
	ZEDBSD_BIOS_PROBE_FIXED = 10,

	/*
	 * One 512-byte fixed-disk write through the low-memory BIOS bounce
	 * area.
	 */
	ZEDBSD_BIOS_DISK_WRITE = 11,

	/*
	 * Stop displaying G-VRAM (INT 18h, AH=41h).
	 */
	ZEDBSD_BIOS_DISPLAY_STOP = 12,

	/*
	 * One byte of the BIOS real-time key state table; request.status
	 * selects the scan-code group (0..15).
	 */
	ZEDBSD_BIOS_KEY_STATE = 13,
};

struct bios_request {
	uint16_t service;
	uint16_t status;
	uint8_t bios_id;
	uint8_t heads;
	uint8_t sectors;
	uint8_t reserved;
	uint32_t lba;
	uint32_t buffer;
} __attribute__((packed));

typedef uint32_t (
	*bios_gateway_fn)(
	struct bios_request *request);

_Static_assert(
	sizeof(struct boot_stage2_header) == 20,
	"zedBSD Stage 2 header must remain 20 bytes");

_Static_assert(
	sizeof(struct boot_handoff) == 24,
	"zedBSD handoff version 2 must remain 24 bytes");

_Static_assert(
	sizeof(struct bios_request) == 16,
	"zedBSD BIOS request must remain 16 bytes");

enum boot_device_class {
	ZEDBSD_DEV_FDD = 1,
	ZEDBSD_DEV_IDE = 2,
	ZEDBSD_DEV_SCSI = 3,
	ZEDBSD_DEV_SD = 4,
};

enum boot_device_flags {
	ZEDBSD_DEV_PRESENT = 1U << 0,
	ZEDBSD_DEV_HAS_GEOMETRY = 1U << 1,
	ZEDBSD_DEV_BOOT_ORIGIN = 1U << 2,
};

/*
 * Firmware-discovered boot device descriptor shared with the kernel.
 */
struct boot_device {
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

_Static_assert(
	sizeof(struct boot_device) == 16,
	"zedBSD device descriptor ABI must remain 16 bytes");

/*
 * Architecture-independent boot parameters and boot-source ownership.
 *
 * This header is the public boot-interface ledger.  Declarations are appended
 * or changed only after a recorded architecture/API decision, not as an
 * implementation-refactoring convenience.
 */

#define KERN_BOOT_PARAMETERS_TEXT_MAX ZEDBSD_BOOT_PARAMETERS_TEXT_MAX
#define KERN_BOOT_PARAMETERS_STORAGE_SIZE ZEDBSD_BOOT_PARAMETERS_STORAGE_SIZE
#define KERN_BOOT_PARAMETERS_INIT_PATH_MAX 255U
#define KERN_BOOT_PARAMETERS_UNKNOWN_NAME_MAX 31U
#define KERN_BOOT_PARAMETER_OFFSET_ABSENT UINT16_MAX

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

/*
 * Values are offsets into storage so the structure remains self-contained
 * when a host fixture or a future handoff path copies it.
 */
struct kern_boot_parameters {
	char storage[KERN_BOOT_PARAMETERS_STORAGE_SIZE];
	uint16_t value_offset[KERN_BOOT_PARAMETER_COUNT];
	unsigned unknown_count;
	unsigned unknown_name_truncated;
	char unknown_name[KERN_BOOT_PARAMETERS_UNKNOWN_NAME_MAX + 1U];
};

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
kern_boot_parameters_current(void);

/*
 * True only when the valid kernel-global instance was initialized from an
 * actual parameter source.  This deliberately distinguishes an absent source
 * (NULL, zero capacity) from a present but empty string.
 */
int
kern_boot_parameters_source_present(void);

#define KERN_BOOT_SOURCE_SLOT_COUNT 4U

struct disk;
struct mount;
struct path;
enum bootfat_type;

struct kern_boot_source_reference {
	unsigned slot;
	/* Matches the stable kernel path limit without importing VFS internals. */
	char relative[256U];
};

enum kern_boot_root_mode {
	KERN_BOOT_ROOT_INVALID = 0,
	KERN_BOOT_ROOT_NATIVE,
	KERN_BOOT_ROOT_OVERLAY,
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

#endif
