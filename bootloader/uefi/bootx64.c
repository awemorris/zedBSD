/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The x64 UEFI loader.
 *
 * It finds the one volume on the boot disk that holds zedbsd.cfg, reads the
 * configuration and the kernel it names from there, picks the display mode,
 * draws the boot logo, places the kernel, builds the bootstrap page tables
 * and the handoff, exits boot services and enters the kernel.
 */

#include "include/uefi.h"
#include "elf64.h"
#include "framebuffer.h"
#include "video.h"
#include "logo.h"
#include "memory-map.h"
#include "volume-discovery.h"
#include "zedbsd-config.h"
#include "root-config.h"
#include "bootloader/common/ufs-read.h"
#include "boot-keys.h"
#include "bootloader/common/boot-override.h"
#include "bootloader/include/amd64-handoff.h"
#include "bootloader/include/amd64-kernel-image.h"
#include "bootloader/include/boot-parameter-handoff.h"

/* The page size, and the low block of bootstrap pages below 2 MiB. */
#define PAGE_SIZE			4096ULL
#define LOW_BLOCK_PAGES			16U
#define LOW_BLOCK_MAX			0x001fffffULL

/* The bounds of the normalized memory map and the kernel file. */
#define MAX_MEMORY_RANGES		256U
#define MAX_KERNEL_FILE			0x04000000ULL

/* The sizes of the loader's file buffers. */
#define BOOT_SECTOR_BUFFER		4096U
#define CONFIG_READ_BUFFER		(ZBL_KERN_CONFIG_FILE_MAX + 1U)
#define FILE_INFO_BUFFER		1024U

/* The GOP mode a boot with a logo asks for: the boot splash's size. */
#define SPLASH_MODE_WIDTH		1920U
#define SPLASH_MODE_HEIGHT		1080U

/* The GOP mode a boot with Ctrl held asks for: the early console's 80x30 fills it. */
#define TEXT_MODE_WIDTH			640U
#define TEXT_MODE_HEIGHT		480U

/* The UTF-16 storage of a file path with its leading backslash and terminator. */
#define KERNEL_PATH_CHAR16_STORAGE	(ZBL_KERN_CONFIG_KERNEL_PATH_STORAGE_SIZE + 1U)

/* The layout of the low block: transition code, page tables, handoff and map. */
#define LOW_TRAMPOLINE_OFFSET		0x0000U
#define LOW_PML4_OFFSET			0x1000U
#define LOW_PDPT_OFFSET			0x2000U
#define LOW_PD_OFFSET			0x3000U
#define HIGH_PDPT_OFFSET		0x4000U
#define HIGH_PD_OFFSET			0x5000U
#define HANDOFF_OFFSET			0x6000U
#define MEMORY_RANGES_OFFSET		0x7000U
#define BOOT_ALLOCATIONS_OFFSET		0xa000U
#define FRAMEBUFFER_PD_OFFSET		0x9000U
#define RUNTIME_DESCRIPTORS_OFFSET	0xb000U
#define TRANSITION_STACK_TOP		0x10000U

/* Kernel placement: candidates retried and map lines printed on failure. */
#define KERNEL_PLACE_ATTEMPTS		8U
#define KERNEL_MAP_DUMP_LIMIT		48U

/* The page table entry bits the bootstrap tables use. */
#define PTE_PRESENT			0x001ULL
#define PTE_WRITE			0x002ULL
#define PTE_LARGE			0x080ULL

/* The geometry of the progress panel in the top-right corner of the screen. */
#define DIAGNOSTIC_BLOCK_WIDTH		24U
#define DIAGNOSTIC_BLOCK_HEIGHT		16U
#define DIAGNOSTIC_BLOCK_GAP		8U
#define DIAGNOSTIC_MARGIN		8U
#define DIAGNOSTIC_PANEL_WIDTH		136U
#define DIAGNOSTIC_PANEL_HEIGHT		40U
#define DIAGNOSTIC_PIXEL		0x00ffffffU

/*
 * What every step of the loader needs: the firmware's tables, whether the
 * console is quiet, and the boot keys.
 *
 * efi_main() owns the one instance for the whole run.
 */
struct loader_context {
	EFI_HANDLE image;
	EFI_SYSTEM_TABLE *system;
	EFI_BOOT_SERVICES *boot;

	/* The boot logo is on the screen: progress text stays off it (errors still show). */
	int quiet_console;

	/* The boot keys (Ctrl, Shift) read from the firmware while the loader runs. */
	struct zbl_uefi_boot_keys keys;
};

/*
 * The volume that holds zedbsd.cfg, with the files open on it.
 *
 * Discovery fills it; the configuration and the kernel are read from it and
 * its files are closed before the memory map is taken.
 */
struct discovered_volume {
	struct kern_boot_provenance provenance;
	EFI_HANDLE handle;
	EFI_FILE_PROTOCOL *root;
	EFI_FILE_PROTOCOL *config;
	struct zbl_uefi_fat_info fat;
	struct zbl_uefi_partition_path path;
	char uuid[ZBL_UEFI_FAT_UUID_SIZE];
	size_t order;
	size_t match_count;

	/* Which files zedbsd.cfg puts on the root partition (kernel=rootpart:...). */
	struct zbl_uefi_root_config root_files;
};

/*
 * The UFS root partition the kernel or the logo is read from.
 *
 * It is opened on first use, when zedbsd.cfg puts a file on it, and read
 * through the firmware's block device until the kernel is loaded.
 */
struct root_volume {
	int mounted;
	EFI_BLOCK_IO_PROTOCOL *block;
	UINT32 media_id;
	UINT32 block_size;
	struct zbl_ufs_volume ufs;
};

/*
 * A file the loader reads: on the FAT volume that holds zedbsd.cfg, or on
 * the UFS root partition.
 *
 * Exactly one of fat and on_root describes it; size is its length.
 */
struct boot_file {
	EFI_FILE_PROTOCOL *fat;
	int on_root;
	struct zbl_ufs_file ufs;
	UINT64 size;
};

/*
 * Where the kernel image goes.  The default ("auto") is the linked address
 * when the firmware still has it free, otherwise the lowest 2 MiB-aligned run
 * of conventional memory below one GiB that holds the whole image.  The
 * kernel_phys= boot parameter overrides it: "link" insists on the linked
 * address (the historical behaviour), "auto" is the default, and a
 * hexadecimal address places the image there or fails.
 */
struct kernel_placement_policy {
	int link_only;
	uint64_t fixed;
};

/* The copied transition code, entered with the CR3, stack, handoff and kernel entry. */
typedef void(EFIAPI *transition_fn)(uint64_t, uint64_t, uint64_t, uint64_t) __attribute__((noreturn));

/* The bounds of the transition code in transition.S, copied into the low block. */
extern uint8_t zbl_transition_start[];
extern uint8_t zbl_transition_quiet[];
extern uint8_t zbl_transition_end[];

/*
 * The buffer a candidate volume's boot sector is read into.
 *
 * It is aligned to its size so it meets any block device's I/O alignment up
 * to that size; it is reused for every candidate.
 */
static uint8_t boot_sector[BOOT_SECTOR_BUFFER] __attribute__((aligned(BOOT_SECTOR_BUFFER)));

/* The buffer zedbsd.cfg is read into, one byte larger than the parser takes so a longer file is seen. */
static uint8_t config_buffer[CONFIG_READ_BUFFER];

/* zedbsd.cfg with the rootpart: prefixes taken off, which the parser reads. */
static uint8_t config_split_buffer[CONFIG_READ_BUFFER];

/*
 * The UFS reader's block and indirect-block storage.
 *
 * Page alignment meets any block device's IoAlign, so the firmware reads
 * straight into it.  It is used only while the root partition is read.
 */
static struct zbl_ufs_work root_work __attribute__((aligned(4096)));

/* The root partition, opened when zedbsd.cfg first needs a file on it. */
static struct root_volume root_partition;

/* The buffer GetInfo fills with a file's information. */
static uint8_t file_info_buffer[FILE_INFO_BUFFER] __attribute__((aligned(8)));

/* The path of the configuration file on a volume, in UTF-16. */
static const CHAR16 kern_config_path[] = {
	'\\', 'Z', 'E', 'D', 'B', 'S', 'D', '.', 'C', 'F', 'G', 0
};

static void byte_zero(void *pointer, UINTN size);
static void byte_copy(void *destination, const void *source, UINTN size);
static int guid_equal(const EFI_GUID *left, const EFI_GUID *right);
static uint64_t find_acpi_rsdp(const EFI_SYSTEM_TABLE *system);
static void debug_byte(uint8_t byte);
static void debug_port(const char *string);
static void console_ascii(struct loader_context *context, const char *string);
static void console_hex64(struct loader_context *context, const char *label, uint64_t value);
static void framebuffer_stage(const struct zbl6_framebuffer *framebuffer, unsigned stage);
static void framebuffer_map_error(const struct zbl6_framebuffer *framebuffer, enum zbl_memory_result result);
static void __attribute__((noreturn)) halt(void);
static void console_status(struct loader_context *context, const char *operation, EFI_STATUS status);
static void __attribute__((noreturn)) fail_status(struct loader_context *context, const char *operation, EFI_STATUS status);
static EFI_STATUS read_exact(EFI_FILE_PROTOCOL *file, UINT64 position, void *buffer, UINTN size);
static EFI_STATUS close_file(EFI_FILE_PROTOCOL *file);
static EFI_STATUS close_file_pair(EFI_FILE_PROTOCOL *file, EFI_FILE_PROTOCOL *root);
static EFI_STATUS release_selection(struct zbl_uefi_volume_selection *selection);
static EFI_STATUS discovery_abort(struct zbl_uefi_volume_selection *selection, EFI_STATUS status);
static EFI_STATUS regular_file_size(EFI_FILE_PROTOCOL *file, UINT64 *file_size);
static EFI_STATUS probe_readable_file(EFI_FILE_PROTOCOL *file);
static EFI_STATUS read_bounded_file(EFI_FILE_PROTOCOL *file, void *buffer, UINTN capacity, UINTN *size);
static int kernel_path_utf16(const char *source, CHAR16 output[KERNEL_PATH_CHAR16_STORAGE]);
static void console_candidate_result(struct loader_context *context, const char *label, UINTN order, const char *result);
static int block_media_usable(const EFI_BLOCK_IO_MEDIA *media);
static EFI_STATUS examine_candidate(struct loader_context *context, EFI_HANDLE handle, size_t order, const struct zbl_uefi_partition_path *loaded_path, struct zbl_uefi_volume_selection *selection, struct zbl_uefi_partition_path *selected_path);
static EFI_STATUS discover_config_volume(struct loader_context *context, const EFI_LOADED_IMAGE_PROTOCOL *loaded, struct discovered_volume *discovered);
static EFI_STATUS close_discovered_volume(struct discovered_volume *discovered);
static void __attribute__((noreturn)) fail_discovered(struct loader_context *context, struct discovered_volume *discovered, const char *operation, EFI_STATUS status);
static EFI_STATUS load_selected_configuration(struct loader_context *context, struct discovered_volume *discovered, struct zbl_uefi_kern_config *configuration);
static EFI_STATUS free_pages(EFI_BOOT_SERVICES *boot, EFI_PHYSICAL_ADDRESS address, UINTN pages);
static EFI_STATUS free_pool(EFI_BOOT_SERVICES *boot, void *pool);
static void __attribute__((noreturn)) fail_kernel_load(struct loader_context *context, struct discovered_volume *discovered, EFI_FILE_PROTOCOL *kernel, const char *operation, EFI_STATUS status, EFI_PHYSICAL_ADDRESS kernel_address, UINTN kernel_pages, int pages_allocated);
static void __attribute__((noreturn)) fail_boot_allocations(struct loader_context *context, const char *operation, EFI_STATUS status, void *pool, int pool_allocated, EFI_PHYSICAL_ADDRESS low_address, UINTN low_pages, int low_pages_allocated, EFI_PHYSICAL_ADDRESS kernel_address, UINTN kernel_pages);
static int framebuffer_from_gop(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, struct zbl6_framebuffer *framebuffer, struct zbl_uefi_framebuffer_mapping *mapping);
static int token_is(const char *text, size_t length, const char *literal);
static int kernel_placement_parse(const char *text, size_t length, struct kernel_placement_policy *policy);
static int show_logo(struct loader_context *context, struct discovered_volume *discovered, const struct zbl_uefi_kern_config *configuration, const struct zbl6_framebuffer *framebuffer);
static EFI_STATUS apply_boot_keys(struct loader_context *context, struct zbl_uefi_kern_config *configuration, unsigned held);
static void notice_boot_keys(struct loader_context *context, const struct zbl_uefi_kern_config *configuration, unsigned held);
static void console_map_entry(struct loader_context *context, const EFI_MEMORY_DESCRIPTOR *descriptor);
static void console_kernel_map(struct loader_context *context, const EFI_MEMORY_DESCRIPTOR *map, UINTN map_size, UINTN descriptor_size);
static EFI_STATUS allocate_kernel_at(EFI_BOOT_SERVICES *boot, EFI_PHYSICAL_ADDRESS wanted, UINTN pages);
static EFI_STATUS place_kernel_at(struct loader_context *context, struct zbl_elf64_plan *plan, EFI_PHYSICAL_ADDRESS wanted, UINTN pages);
static EFI_STATUS place_kernel(struct loader_context *context, struct zbl_elf64_plan *plan, const struct kernel_placement_policy *policy, EFI_PHYSICAL_ADDRESS *address, UINTN pages);
static void give_runtime_addresses(const EFI_SYSTEM_TABLE *system, const uint8_t *map, UINTN map_size, UINTN descriptor_size, UINT32 descriptor_version, uint64_t low_base, struct zbl6_handoff_v8_uefi *handoff);
static void build_bootstrap(uint64_t low_base, const struct zbl_elf64_plan *plan, const struct zbl6_framebuffer *framebuffer, const struct zbl_uefi_framebuffer_mapping *framebuffer_mapping, uint32_t boot_volume_serial, uint8_t partition_scheme, const struct kern_boot_parameter_record *parameters, const struct kern_boot_provenance *provenance);
static int root_block_read(void *device_context, uint64_t block, uint32_t count, void *buffer);
static void __attribute__((noreturn)) fail_root(struct loader_context *context, struct discovered_volume *discovered, const char *reason);
static void open_root_volume(struct loader_context *context, struct discovered_volume *discovered);
static EFI_STATUS boot_file_open(struct loader_context *context, struct discovered_volume *discovered, const char *path, int on_root, struct boot_file *file);
static EFI_STATUS boot_file_read(struct boot_file *file, UINT64 position, void *buffer, UINTN size);
static void load_kernel(struct loader_context *context, struct discovered_volume *discovered, const struct zbl_uefi_kern_config *configuration, struct zbl_elf64_plan *plan, EFI_PHYSICAL_ADDRESS *kernel_address, UINTN *kernel_pages);

/*
 * Loads the configured kernel and enters it.
 *
 * Every failure before ExitBootServices is reported on the console and
 * stops the machine; the function does not return to the firmware once the
 * configuration has been found.
 */
EFI_STATUS EFIAPI
efi_main(
	EFI_HANDLE image,
	EFI_SYSTEM_TABLE *system)
{
	static struct discovered_volume discovered;
	static struct zbl_uefi_kern_config configuration;
	struct loader_context context;
	EFI_LOADED_IMAGE_PROTOCOL *loaded;
	EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
	EFI_BOOT_SERVICES *boot;
	EFI_STATUS status;
	EFI_STATUS reported;
	struct zbl_elf64_plan plan;
	EFI_PHYSICAL_ADDRESS kernel_address;
	EFI_PHYSICAL_ADDRESS low_address;
	UINTN kernel_pages;
	UINTN map_size;
	UINTN map_capacity;
	UINTN map_key;
	UINTN descriptor_size;
	UINT32 descriptor_version;
	EFI_MEMORY_DESCRIPTOR *map;
	struct zbl6_handoff_v8_uefi *handoff;
	struct zbl6_handoff_v3 *handoff_common;
	struct zbl6_framebuffer framebuffer;
	struct zbl_uefi_framebuffer_mapping framebuffer_mapping;
	struct zbl6_memory_range_v6 *ranges;
	struct zbl6_boot_allocation *allocations;
	transition_fn enter;
	uint64_t entry_offset;
	uint64_t cr4;
	uint32_t range_count;
	enum zbl_memory_result map_result;
	int low_pages_allocated;
	int map_allocated;
	int quiet_boot;
	char logo_path[ZBL_LOGO_PATH_MAX + 1U];
	int logo_named;
	int logo_shown;
	int usable;
	unsigned attempt;
	unsigned held;
	UINT32 wish_width;
	UINT32 wish_height;

	/* Nothing has been allocated yet. */
	map = 0;
	low_pages_allocated = 0;
	map_allocated = 0;
	quiet_boot = 0;

	/* Refuses a system table without boot services. */
	if (system == 0 || system->BootServices == 0)
		return EFI_INVALID_PARAMETER;

	/* Starts the loader's context with a console that is not quiet. */
	context.image = image;
	context.system = system;
	context.boot = system->BootServices;
	context.quiet_console = 0;
	boot = context.boot;
	console_ascii(&context, "A64 UEFI ENTRY\n");

	/*
	 * Asks the firmware for the boot keys as early as possible and reads
	 * what is queued already.  Nothing waits for a key.
	 */
	zbl_uefi_boot_keys_open(&context.keys, system);
	(void)zbl_uefi_boot_keys_sample(&context.keys);

	/* Finds the GOP whose framebuffer the kernel receives. */
	status = boot->LocateProtocol(&EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID, 0, (void **)&gop);
	if (EFI_ERROR(status))
		fail_status(&context, "Locate GOP", status);

	/* Stops on a GOP whose framebuffer the kernel cannot use. */
	usable = framebuffer_from_gop(gop, &framebuffer, &framebuffer_mapping);
	if (!usable)
		fail_status(&context, "Validate GOP", EFI_UNSUPPORTED);

	/* Finds the loader's own image and the device it was loaded from. */
	status = boot->HandleProtocol(image, &EFI_LOADED_IMAGE_PROTOCOL_GUID, (void **)&loaded);
	if (EFI_ERROR(status))
		fail_status(&context, "LoadedImage", status);

	/* Stops on an image without a device. */
	if (loaded == 0 || loaded->DeviceHandle == 0)
		fail_status(&context, "Validate LoadedImage", EFI_INVALID_PARAMETER);

	/* The configuration comes from zedbsd.cfg, never from the load options. */
	if (loaded->LoadOptionsSize != 0U)
		console_ascii(&context, "A64 UEFI LoadOptions ignored\n");

	/* Finds the volume on the boot disk that holds zedbsd.cfg. */
	status = discover_config_volume(&context, loaded, &discovered);
	if (EFI_ERROR(status))
		fail_status(&context, "Discover zedbsd.cfg", status);

	/* Reads and parses zedbsd.cfg. */
	status = load_selected_configuration(&context, &discovered, &configuration);
	if (EFI_ERROR(status))
		fail_discovered(&context, &discovered, "Load zedbsd.cfg", status);

	/*
	 * The boot keys held so far choose the console's kernel messages
	 * (Ctrl) and the console login (Shift), before anything reads the
	 * parameters for the logo and the video mode.
	 */
	held = zbl_uefi_boot_keys_sample(&context.keys);
	status = apply_boot_keys(&context, &configuration, held);
	if (status != EFI_SUCCESS)
		fail_discovered(&context, &discovered, "Override parameters", status);

	/*
	 * Applies the optional display request before retaining framebuffer
	 * addresses.  When there is no video=, a boot with Ctrl held wishes for
	 * 640x480, where the early console's 80 columns fill the screen, and a
	 * boot with a logo (logo=) for the GOP's 1920x1080, the boot splash's
	 * size.
	 */
	logo_named = zbl_logo_path(configuration.parameter_record.text,
				   configuration.parameter_record.length,
				   logo_path,
				   sizeof(logo_path));

	/* Chooses the mode to wish for when video= names none. */
	if ((held & ZBL_BOOT_OVERRIDE_KMSG) != 0U) {
		/* Ctrl: the kernel messages are read on screen. */
		wish_width = TEXT_MODE_WIDTH;
		wish_height = TEXT_MODE_HEIGHT;
	} else if (logo_named > 0) {
		/* The splash fills the screen at its own size. */
		wish_width = SPLASH_MODE_WIDTH;
		wish_height = SPLASH_MODE_HEIGHT;
	} else {
		/* The current mode stays. */
		wish_width = 0U;
		wish_height = 0U;
	}

	/* Sets the mode video= names, or the wished one when the firmware has it. */
	status = zbl_uefi_video_select(boot,
				       gop,
				       configuration.parameter_record.text,
				       configuration.parameter_record.length,
				       wish_width,
				       wish_height);
	if (EFI_ERROR(status))
		fail_discovered(&context, &discovered, "Select configured video mode", status);

	/* SetMode can replace the framebuffer base, size, stride, and pixel format. */
	usable = framebuffer_from_gop(gop, &framebuffer, &framebuffer_mapping);
	if (!usable)
		fail_discovered(&context, &discovered, "Validate selected GOP", EFI_UNSUPPORTED);

	/* Tells which boot keys were seen, after SetMode has cleared the screen. */
	notice_boot_keys(&context, &configuration, held);

	/* Draws the boot logo (logo=); with it on the screen the progress text stays off. */
	logo_shown = show_logo(&context, &discovered, &configuration, &framebuffer);
	if (logo_shown)
		context.quiet_console = 1;

	/* A quiet boot (kmsg=quiet) draws no progress blocks. */
	quiet_boot = zbl_parameter_present(configuration.parameter_record.text,
					   configuration.parameter_record.length,
					   "kmsg=quiet");

	/* Reads the configured kernel into its physical home and closes the volume. */
	load_kernel(&context, &discovered, &configuration, &plan, &kernel_address, &kernel_pages);
	console_ascii(&context, "A64 UEFI ELF\n");

	/* Allocates the low block for the bootstrap page tables, the handoff and the transition. */
	low_address = LOW_BLOCK_MAX;
	status = boot->AllocatePages(AllocateMaxAddress, EfiLoaderData, LOW_BLOCK_PAGES, &low_address);
	if (EFI_ERROR(status) ||
	    low_address < 0x00010000ULL ||
	    low_address + LOW_BLOCK_PAGES * PAGE_SIZE > 0x00200000ULL) {
		/* Reports the firmware's error, or a block outside the window the transition needs. */
		reported = EFI_LOAD_ERROR;
		if (EFI_ERROR(status))
			reported = status;

		fail_boot_allocations(&context,
				      "Allocate bootstrap",
				      reported,
				      0,
				      0,
				      low_address,
				      LOW_BLOCK_PAGES,
				      !EFI_ERROR(status),
				      kernel_address,
				      kernel_pages);
	}

	/* Builds the bootstrap page tables and the handoff in the low block. */
	low_pages_allocated = 1;
	build_bootstrap(low_address,
			&plan,
			&framebuffer,
			&framebuffer_mapping,
			discovered.fat.volume_serial,
			(uint8_t)discovered.path.style,
			&configuration.parameter_record,
			&discovered.provenance);

	/* Finds the handoff and the memory ranges in the low block. */
	handoff = (void *)(uintptr_t)(low_address + HANDOFF_OFFSET);
	handoff_common = &handoff->prefix.common.common;
	ranges = (void *)(uintptr_t)(low_address + MEMORY_RANGES_OFFSET);

	/* Stops on a firmware without an ACPI RSDP, which the kernel needs. */
	handoff_common->rsdp = find_acpi_rsdp(system);
	if (handoff_common->rsdp == 0) {
		fail_boot_allocations(&context,
				      "Locate ACPI RSDP",
				      EFI_NOT_FOUND,
				      0,
				      0,
				      low_address,
				      LOW_BLOCK_PAGES,
				      low_pages_allocated,
				      kernel_address,
				      kernel_pages);
	}

	/* Marks the RSDP in the handoff and reports the addresses the kernel will see. */
	handoff_common->flags |= ZBL6_HANDOFF_FLAG_ACPI_RSDP;
	console_hex64(&context, "A64 RSDP ", handoff_common->rsdp);
	console_hex64(&context, "A64 GOP  ", framebuffer.physical_base);
	console_hex64(&context, "A64 LOW  ", low_address);

	/* Reports CR4 as the firmware left it. */
	__asm__ volatile("movq %%cr4,%0" : "=r"(cr4));
	console_hex64(&context, "A64 CR4  ", cr4);

	/* Asks the firmware for the size of its memory map. */
	map_size = 0;
	status = boot->GetMemoryMap(&map_size, 0, &map_key, &descriptor_size, &descriptor_version);
	if (status != EFI_BUFFER_TOO_SMALL || descriptor_size == 0) {
		fail_boot_allocations(&context,
				      "Size memory map",
				      status,
				      0,
				      0,
				      low_address,
				      LOW_BLOCK_PAGES,
				      low_pages_allocated,
				      kernel_address,
				      kernel_pages);
	}

	/* Stops on a map size whose buffer, with room to grow, does not fit. */
	if (descriptor_size > (UINTPTR_MAX - PAGE_SIZE * 4U) / 64U ||
	    map_size > UINTPTR_MAX - descriptor_size * 64U - PAGE_SIZE * 4U) {
		fail_boot_allocations(&context,
				      "Bound memory map",
				      EFI_BAD_BUFFER_SIZE,
				      0,
				      0,
				      low_address,
				      LOW_BLOCK_PAGES,
				      low_pages_allocated,
				      kernel_address,
				      kernel_pages);
	}

	/* Allocates the map buffer with room for the descriptors the allocation itself adds. */
	map_capacity = map_size + descriptor_size * 64U + PAGE_SIZE * 4U;
	status = boot->AllocatePool(EfiLoaderData, map_capacity, (void **)&map);
	if (EFI_ERROR(status)) {
		fail_boot_allocations(&context,
				      "Allocate memory map",
				      status,
				      0,
				      0,
				      low_address,
				      LOW_BLOCK_PAGES,
				      low_pages_allocated,
				      kernel_address,
				      kernel_pages);
	}

	/* Describes the kernel image, which the kernel keeps. */
	map_allocated = 1;
	allocations = (void *)(uintptr_t)(low_address + BOOT_ALLOCATIONS_OFFSET);
	allocations[0].base = plan.physical_start;
	allocations[0].size = (plan.physical_end + PAGE_SIZE - 1U) / PAGE_SIZE * PAGE_SIZE - plan.physical_start;
	allocations[0].owner = ZBL6_BOOT_OWNER_KERNEL;
	allocations[0].lifetime = ZBL6_BOOT_KEEP;

	/* Describes the low block, which the kernel may reuse after init. */
	allocations[1].base = low_address;
	allocations[1].size = LOW_BLOCK_PAGES * PAGE_SIZE;
	allocations[1].owner = ZBL6_BOOT_OWNER_BOOTSTRAP;
	allocations[1].lifetime = ZBL6_BOOT_AFTER_INIT;

	/* Stops on a loader image whose page-rounded extent does not fit. */
	allocations[2].base = (uintptr_t)loaded->ImageBase & ~(uint64_t)(PAGE_SIZE - 1U);
	if ((uintptr_t)loaded->ImageBase > UINT64_MAX - (PAGE_SIZE - 1U) ||
	    loaded->ImageSize > UINT64_MAX - (uintptr_t)loaded->ImageBase - (PAGE_SIZE - 1U)) {
		fail_boot_allocations(&context,
				      "Loaded image extent",
				      EFI_LOAD_ERROR,
				      map,
				      map_allocated,
				      low_address,
				      LOW_BLOCK_PAGES,
				      low_pages_allocated,
				      kernel_address,
				      kernel_pages);
	}

	/* Describes the loader image, which the kernel may reuse after init. */
	allocations[2].size = ((uintptr_t)loaded->ImageBase + loaded->ImageSize + PAGE_SIZE - 1U) / PAGE_SIZE * PAGE_SIZE - allocations[2].base;
	allocations[2].owner = ZBL6_BOOT_OWNER_LOADER;
	allocations[2].lifetime = ZBL6_BOOT_AFTER_INIT;

	/* Stops on a map buffer whose page-rounded extent does not fit. */
	allocations[3].base = (uintptr_t)map & ~(uint64_t)(PAGE_SIZE - 1U);
	if ((uintptr_t)map > UINT64_MAX - (PAGE_SIZE - 1U) ||
	    map_capacity > UINT64_MAX - (uintptr_t)map - (PAGE_SIZE - 1U)) {
		fail_boot_allocations(&context,
				      "Map buffer extent",
				      EFI_LOAD_ERROR,
				      map,
				      map_allocated,
				      low_address,
				      LOW_BLOCK_PAGES,
				      low_pages_allocated,
				      kernel_address,
				      kernel_pages);
	}

	/* Describes the map buffer, which the kernel may reuse after init. */
	allocations[3].size = ((uintptr_t)map + map_capacity + PAGE_SIZE - 1U) / PAGE_SIZE * PAGE_SIZE - allocations[3].base;
	allocations[3].owner = ZBL6_BOOT_OWNER_MEMORY_MAP;
	allocations[3].lifetime = ZBL6_BOOT_AFTER_INIT;

	/* Fills the memory envelope of the handoff; the range count follows the final map. */
	handoff->memory.source = ZBL6_MEMORY_SOURCE_UEFI;
	handoff->memory.flags = ZBL6_MEMORY_MAP_COMPLETE;
	handoff->memory.range_entry_size = sizeof(*ranges);
	handoff->memory.ranges = handoff_common->memory_ranges;
	handoff->memory.allocation_count = 4;
	handoff->memory.allocation_entry_size = sizeof(*allocations);
	handoff->memory.allocations = low_address + BOOT_ALLOCATIONS_OFFSET;
	handoff->memory.bootstrap_cr3 = handoff_common->bootstrap_cr3;
	handoff->memory.kernel_phys_start = plan.physical_start;
	handoff->memory.kernel_phys_end = plan.physical_end;

	/*
	 * Validate a non-final snapshot while console and failure reporting are
	 * still available.  The final snapshot is normalized only after boot
	 * services have exited so its map key is consumed immediately.
	 */
	map_size = map_capacity;
	status = boot->GetMemoryMap(&map_size, map, &map_key, &descriptor_size, &descriptor_version);
	if (EFI_ERROR(status)) {
		fail_boot_allocations(&context,
				      "Preflight memory map",
				      status,
				      map,
				      map_allocated,
				      low_address,
				      LOW_BLOCK_PAGES,
				      low_pages_allocated,
				      kernel_address,
				      kernel_pages);
	}

	/* Stops on a map the normalizer refuses, while the refusal can still be shown. */
	map_result = zbl_uefi_normalize_memory_map_v6(map, map_size, descriptor_size, ranges, MAX_MEMORY_RANGES, &range_count);
	if (map_result != ZBL_MEMORY_OK) {
		console_ascii(&context, "UEFI map rejected: ");
		console_hex64(&context, "code ", map_result);
		console_ascii(&context, "\n");
		fail_boot_allocations(&context,
				      "Normalize memory map",
				      EFI_LOAD_ERROR,
				      map,
				      map_allocated,
				      low_address,
				      LOW_BLOCK_PAGES,
				      low_pages_allocated,
				      kernel_address,
				      kernel_pages);
	}

	/* Reports the map and the meaning of the progress blocks. */
	console_hex64(&context, "A64 MAPSZ ", map_size);
	console_hex64(&context, "A64 DESCSZ ", descriptor_size);
	console_hex64(&context, "A64 DESCVER ", descriptor_version);
	console_hex64(&context, "A64 RANGES ", range_count);
	console_ascii(&context, "A64 MARK 1=EBS 2=MAP 3=CR3 4=KERN\n");
	console_ascii(&context, "A64 UEFI READY\n");

	/* Takes the final map and exits boot services, retrying when the map changed in between. */
	for (attempt = 0; attempt < 3U; attempt++) {
		/* Takes the map whose key ExitBootServices consumes. */
		map_size = map_capacity;
		status = boot->GetMemoryMap(&map_size, map, &map_key, &descriptor_size, &descriptor_version);
		if (EFI_ERROR(status)) {
			fail_boot_allocations(&context,
					      "Final memory map",
					      status,
					      map,
					      map_allocated,
					      low_address,
					      LOW_BLOCK_PAGES,
					      low_pages_allocated,
					      kernel_address,
					      kernel_pages);
		}

		/* Exits boot services; a stale key is retried. */
		status = boot->ExitBootServices(image, map_key);
		if (status == EFI_SUCCESS)
			break;

		/* Any failure other than a stale key stops the boot. */
		if (status != EFI_INVALID_PARAMETER) {
			fail_boot_allocations(&context,
					      "ExitBootServices",
					      status,
					      map,
					      map_allocated,
					      low_address,
					      LOW_BLOCK_PAGES,
					      low_pages_allocated,
					      kernel_address,
					      kernel_pages);
		}
	}

	/* Stops when every retry found the map changed. */
	if (status != EFI_SUCCESS) {
		fail_boot_allocations(&context,
				      "ExitBootServices retries",
				      status,
				      map,
				      map_allocated,
				      low_address,
				      LOW_BLOCK_PAGES,
				      low_pages_allocated,
				      kernel_address,
				      kernel_pages);
	}

	/* Draws the first progress block, not on a quiet boot's logo. */
	if (!quiet_boot)
		framebuffer_stage(&framebuffer, 1U);

	debug_port("A64 UEFI BOOT SERVICES EXITED\n");

	/* Normalizes the final map; a refusal now can only be shown in the progress panel. */
	map_result = zbl_uefi_normalize_memory_map_v6(map, map_size, descriptor_size, ranges, MAX_MEMORY_RANGES, &range_count);
	if (map_result != ZBL_MEMORY_OK) {
		framebuffer_map_error(&framebuffer, map_result);
		debug_port("A64 UEFI FINAL MAP REJECTED\n");
		halt();
	}

	/* Draws the second progress block, not on a quiet boot's logo. */
	if (!quiet_boot)
		framebuffer_stage(&framebuffer, 2U);

	/* Publishes the final range count. */
	handoff_common->memory_range_count = range_count;
	handoff->memory.range_count = range_count;

	/* Moves the firmware's runtime services into the kernel's runtime window, when they fit. */
	give_runtime_addresses(system,
			       (const uint8_t *)map,
			       map_size,
			       descriptor_size,
			       descriptor_version,
			       low_address,
			       handoff);
	debug_port("A64 UEFI EXIT\n");

	/* Chooses the transition: a quiet boot enters past the third progress block. */
	entry_offset = LOW_TRAMPOLINE_OFFSET;
	if (quiet_boot)
		entry_offset += (uint64_t)(zbl_transition_quiet - zbl_transition_start);

	/* Enters the kernel through the copied transition; it does not return. */
	enter = (transition_fn)(uintptr_t)(low_address + entry_offset);
	enter(handoff_common->bootstrap_cr3, low_address + TRANSITION_STACK_TOP, low_address + HANDOFF_OFFSET, plan.entry);
}

/* Clears bytes without the C library. */
static void
byte_zero(
	void *pointer,
	UINTN size)
{
	uint8_t *bytes;

	/* Clears the bytes one at a time. */
	bytes = pointer;
	while (size != 0) {
		*bytes = 0;
		bytes++;
		size--;
	}
}

/* Copies bytes without the C library. */
static void
byte_copy(
	void *destination,
	const void *source,
	UINTN size)
{
	uint8_t *out;
	const uint8_t *in;

	/* Copies the bytes one at a time, front to back. */
	out = destination;
	in = source;
	while (size != 0) {
		*out = *in;
		out++;
		in++;
		size--;
	}
}

/* Reports whether two GUIDs are equal. */
static int
guid_equal(
	const EFI_GUID *left,
	const EFI_GUID *right)
{
	const uint8_t *a;
	const uint8_t *b;
	UINTN index;

	/* Compares the GUIDs byte by byte. */
	a = (const void *)left;
	b = (const void *)right;
	for (index = 0; index < sizeof(*left); index++) {
		if (a[index] != b[index])
			return 0;
	}

	/* Succeeded: the GUIDs are equal. */
	return 1;
}

/* Finds the ACPI RSDP among the firmware's configuration tables, preferring ACPI 2.0's. */
static uint64_t
find_acpi_rsdp(
	const EFI_SYSTEM_TABLE *system)
{
	const EFI_CONFIGURATION_TABLE *entry;
	UINTN index;
	void *acpi10;
	int is_acpi20;
	int is_acpi10;

	/* No ACPI 1.0 table seen yet. */
	acpi10 = 0;

	/* Refuses a missing or unbounded table list. */
	if (system->ConfigurationTable == 0 ||
	    system->NumberOfTableEntries > 1024U)
		return 0;

	/* Looks at each configuration table. */
	for (index = 0; index < system->NumberOfTableEntries; index++) {
		/* The ACPI 2.0 RSDP is taken at once. */
		entry = &system->ConfigurationTable[index];
		is_acpi20 = guid_equal(&entry->VendorGuid, &EFI_ACPI_20_TABLE_GUID);
		if (is_acpi20)
			return (uint64_t)(uintptr_t)entry->VendorTable;

		/* An ACPI 1.0 RSDP is kept in case no 2.0 one comes. */
		is_acpi10 = guid_equal(&entry->VendorGuid, &EFI_ACPI_TABLE_GUID);
		if (is_acpi10)
			acpi10 = entry->VendorTable;
	}

	/* Reports the ACPI 1.0 RSDP, or zero when there is none. */
	return (uint64_t)(uintptr_t)acpi10;
}

/* Writes one byte to I/O port E9, an emulator's debug console. */
static void
debug_byte(
	uint8_t byte)
{
	/* Writes the byte to the port. */
	__asm__ volatile("outb %0,%1" : : "a"(byte), "Nd"((uint16_t)0xe9));
}

/* Writes a string to I/O port E9. */
static void
debug_port(
	const char *string)
{
	/* Writes the string one byte at a time. */
	while (*string != 0) {
		debug_byte((uint8_t)*string);
		string++;
	}
}

/* Writes an ASCII string to the debug port and, unless the console is quiet, to the firmware console. */
static void
console_ascii(
	struct loader_context *context,
	const char *string)
{
	CHAR16 buffer[128];
	UINTN used;

	/* The debug port gets every message. */
	used = 0;
	debug_port(string);

	/* A quiet console keeps the progress text off the logo. */
	if (context->quiet_console)
		return;

	/* A firmware without a text console shows nothing. */
	if (context->system->ConOut == 0 ||
	    context->system->ConOut->OutputString == 0)
		return;

	/* Converts the string to UTF-16 with CRLF line ends, writing out each full buffer. */
	while (*string != 0) {
		/* A line feed is preceded by a carriage return. */
		if (*string == '\n' && used + 2U < 128U) {
			buffer[used] = '\r';
			used++;
		}

		/* Takes the character. */
		buffer[used] = (uint8_t)*string;
		used++;
		string++;

		/*
		 * Writes out a full buffer.  A carriage return can carry the count
		 * past 126 to 127, so the test is not for equality; the terminator
		 * then still fits at index 127.
		 */
		if (used >= 126U) {
			buffer[used] = 0;
			context->system->ConOut->OutputString(context->system->ConOut, buffer);
			used = 0;
		}
	}

	/* Writes out the rest. */
	buffer[used] = 0;
	context->system->ConOut->OutputString(context->system->ConOut, buffer);
}

/* Writes a label and a 64-bit value in hexadecimal on its own line. */
static void
console_hex64(
	struct loader_context *context,
	const char *label,
	uint64_t value)
{
	/* The hexadecimal digits. */
	static const char digits[] = "0123456789abcdef";
	char message[64];
	UINTN used;
	int shift;

	/* Copies the label. */
	used = 0;
	while (*label != 0 && used + 1U < sizeof(message)) {
		message[used] = *label;
		used++;
		label++;
	}

	/* Stops on a label too long to leave room for the value. */
	if (used + 20U >= sizeof(message))
		halt();

	/* Appends 0x and the sixteen digits. */
	message[used] = '0';
	used++;
	message[used] = 'x';
	used++;
	for (shift = 60; shift >= 0; shift -= 4) {
		message[used] = digits[(value >> shift) & 15U];
		used++;
	}

	/* Ends the line. */
	message[used] = '\n';
	used++;
	message[used] = 0;

	/* Writes the message. */
	console_ascii(context, message);
}

/*
 * Leave a firmware-independent, photographable progress code in the GOP
 * framebuffer.  Stage one clears the panel and every stage adds one white
 * block.  Later code duplicates this exact geometry after the CR3 switch and
 * at the first kernel instruction.
 */
static void
framebuffer_stage(
	const struct zbl6_framebuffer *framebuffer,
	unsigned stage)
{
	volatile uint32_t *pixels;
	unsigned block;
	unsigned origin_x;
	unsigned x;
	unsigned y;

	/* Views the framebuffer as pixels. */
	pixels = (volatile uint32_t *)(uintptr_t)framebuffer->physical_base;

	/* Stops on a stage beyond the four blocks or a screen smaller than the panel. */
	if (stage == 0 ||
	    stage > 4U ||
	    framebuffer->width < DIAGNOSTIC_PANEL_WIDTH ||
	    framebuffer->height < DIAGNOSTIC_PANEL_HEIGHT)
		halt();

	/* The first stage clears the panel in the top-right corner. */
	origin_x = framebuffer->width - DIAGNOSTIC_PANEL_WIDTH;
	if (stage == 1U) {
		for (y = 0; y < DIAGNOSTIC_PANEL_HEIGHT; y++) {
			for (x = 0; x < DIAGNOSTIC_PANEL_WIDTH; x++) {
				pixels[(uint64_t)y * framebuffer->stride + origin_x + x] = 0;
			}
		}
	}

	/* Draws the stage's white block. */
	block = stage - 1U;
	for (y = 0; y < DIAGNOSTIC_BLOCK_HEIGHT; y++) {
		for (x = 0; x < DIAGNOSTIC_BLOCK_WIDTH; x++) {
			pixels[(uint64_t)(y + DIAGNOSTIC_MARGIN) * framebuffer->stride +
			       origin_x +
			       DIAGNOSTIC_MARGIN +
			       block * (DIAGNOSTIC_BLOCK_WIDTH + DIAGNOSTIC_BLOCK_GAP) +
			       x] = DIAGNOSTIC_PIXEL;
		}
	}

	/* Makes the stores visible before anything else happens. */
	__asm__ volatile("sfence" : : : "memory");
}

/* Shows a memory map refusal after ExitBootServices as small blocks, one for each step of the result code. */
static void
framebuffer_map_error(
	const struct zbl6_framebuffer *framebuffer,
	enum zbl_memory_result result)
{
	volatile uint32_t *pixels;
	unsigned block;
	unsigned origin_x;
	unsigned x;
	unsigned y;

	/* Views the framebuffer as pixels. */
	pixels = (volatile uint32_t *)(uintptr_t)framebuffer->physical_base;

	/* A code outside the known refusals is shown as an invalid map. */
	if (result <= ZBL_MEMORY_OK || result > ZBL_MEMORY_OVERLAP)
		result = ZBL_MEMORY_INVALID;

	/* Draws one small block for each step of the code under the progress blocks. */
	origin_x = framebuffer->width - DIAGNOSTIC_PANEL_WIDTH;
	for (block = 0; block < (unsigned)result; block++) {
		for (y = 0; y < 6U; y++) {
			for (x = 0; x < 8U; x++) {
				pixels[(uint64_t)(y + 30U) * framebuffer->stride +
				       origin_x + DIAGNOSTIC_MARGIN +
				       block * 12U + x] = DIAGNOSTIC_PIXEL;
			}
		}
	}

	/* Makes the stores visible before the machine stops. */
	__asm__ volatile("sfence" : : : "memory");
}

/* Stops the machine. */
static void __attribute__((noreturn))
halt(
	void)
{
	/* Halts with interrupts off, again after any NMI. */
	for (;;) {
		__asm__ volatile("cli; hlt");
	}
}

/* Writes an operation's name and the status it returned. */
static void
console_status(
	struct loader_context *context,
	const char *operation,
	EFI_STATUS status)
{
	char message[96];
	/* The hexadecimal digits. */
	static const char digits[] = "0123456789abcdef";
	UINTN index;
	int shift;

	/* Copies the operation's name. */
	index = 0;
	while (*operation != 0 && index + 1U < sizeof(message)) {
		message[index] = *operation;
		index++;
		operation++;
	}

	/* Appends ": 0x" and the status when there is room. */
	if (index + 20U < sizeof(message)) {
		message[index] = ':';
		index++;
		message[index] = ' ';
		index++;
		message[index] = '0';
		index++;
		message[index] = 'x';
		index++;
		for (shift = 60; shift >= 0; shift -= 4) {
			message[index] = digits[(status >> shift) & 15U];
			index++;
		}

		message[index] = '\n';
		index++;
	}

	/* Writes the message. */
	message[index] = 0;
	console_ascii(context, message);
}

/* Reports the operation that failed and stops the machine. */
static void __attribute__((noreturn))
fail_status(
	struct loader_context *context,
	const char *operation,
	EFI_STATUS status)
{
	/* The error that stops the boot is shown even over the boot logo. */
	context->quiet_console = 0;
	console_status(context, operation, status);
	halt();
}

/* Reads exactly size bytes of a file from a position. */
static EFI_STATUS
read_exact(
	EFI_FILE_PROTOCOL *file,
	UINT64 position,
	void *buffer,
	UINTN size)
{
	EFI_STATUS status;
	uint8_t *cursor;
	UINTN amount;

	/* Moves to the position. */
	cursor = buffer;
	status = file->SetPosition(file, position);
	if (EFI_ERROR(status))
		return status;

	/* Reads until every byte is in, refusing a short or long read. */
	while (size != 0) {
		/* Reads as much as remains. */
		amount = size;
		status = file->Read(file, &amount, cursor);
		if (EFI_ERROR(status))
			return status;

		/* Refuses the end of the file before the bytes, and a firmware that read more than asked. */
		if (amount == 0 || amount > size)
			return EFI_LOAD_ERROR;

		/* Moves past what was read. */
		cursor += amount;
		size -= amount;
	}

	/* Succeeded: the buffer holds the bytes. */
	return EFI_SUCCESS;
}

/* Closes one file, refusing a protocol without Close. */
static EFI_STATUS
close_file(
	EFI_FILE_PROTOCOL *file)
{
	EFI_STATUS status;

	/* A protocol without Close cannot release the file. */
	if (file->Close == 0)
		return EFI_INVALID_PARAMETER;

	/* Closes the file. */
	status = file->Close(file);
	if (EFI_ERROR(status))
		return status;

	/* Succeeded: the file is closed. */
	return EFI_SUCCESS;
}

/* Closes a file and its volume's root, reporting the first failure. */
static EFI_STATUS
close_file_pair(
	EFI_FILE_PROTOCOL *file,
	EFI_FILE_PROTOCOL *root)
{
	EFI_STATUS first;
	EFI_STATUS status;

	/* Closes the file first. */
	first = EFI_SUCCESS;
	if (file != 0) {
		status = close_file(file);
		if (EFI_ERROR(status))
			first = status;
	}

	/* Closes the root even when the file failed, keeping the first failure. */
	if (root != 0) {
		status = close_file(root);
		if (!EFI_ERROR(first) && EFI_ERROR(status))
			first = status;
	}

	/* Reports the first failure. */
	if (EFI_ERROR(first))
		return first;

	/* Succeeded: both are closed. */
	return EFI_SUCCESS;
}

/* Closes the files the selection still holds. */
static EFI_STATUS
release_selection(
	struct zbl_uefi_volume_selection *selection)
{
	struct zbl_uefi_volume_match match;
	EFI_STATUS status;
	int taken;

	/* A selection without a match holds nothing. */
	taken = zbl_uefi_volume_selection_take(selection, &match);
	if (!taken)
		return EFI_SUCCESS;

	/* Closes the match's configuration file and root. */
	status = close_file_pair((EFI_FILE_PROTOCOL *)match.config, (EFI_FILE_PROTOCOL *)match.root);
	if (EFI_ERROR(status))
		return status;

	/* Succeeded: the selection holds nothing. */
	return EFI_SUCCESS;
}

/* Releases the selection and reports why discovery stopped, or the cleanup's failure. */
static EFI_STATUS
discovery_abort(
	struct zbl_uefi_volume_selection *selection,
	EFI_STATUS status)
{
	EFI_STATUS cleanup;

	/* Reports a failed cleanup in place of the reason. */
	cleanup = release_selection(selection);
	if (EFI_ERROR(cleanup))
		return cleanup;

	/* Reports the reason discovery stopped. */
	return status;
}

/* Reports a regular file's size, refusing a directory. */
static EFI_STATUS
regular_file_size(
	EFI_FILE_PROTOCOL *file,
	UINT64 *file_size)
{
	EFI_FILE_INFO *info;
	const UINTN minimum = __builtin_offsetof(EFI_FILE_INFO, FileName) + sizeof(CHAR16);
	EFI_STATUS status;
	UINTN required;

	/* Views the shared buffer as the file's information. */
	info = (EFI_FILE_INFO *)(void *)file_info_buffer;
	required = 0U;

	/* Refuses a missing file, protocol or result. */
	if (file == 0 || file->GetInfo == 0 || file_size == 0)
		return EFI_INVALID_PARAMETER;

	/* Asks how large the information is; a firmware that answers otherwise is refused. */
	status = file->GetInfo(file, &EFI_FILE_INFO_ID, &required, 0);
	if (status != EFI_BUFFER_TOO_SMALL || required < minimum) {
		if (EFI_ERROR(status))
			return status;

		return EFI_DEVICE_ERROR;
	}

	/* Refuses information larger than the buffer. */
	if (required > sizeof(file_info_buffer))
		return EFI_BAD_BUFFER_SIZE;

	/* Reads the information into the cleared buffer. */
	byte_zero(file_info_buffer, required);
	status = file->GetInfo(file, &EFI_FILE_INFO_ID, &required, file_info_buffer);
	if (EFI_ERROR(status))
		return status;

	/* Refuses information whose sizes disagree. */
	if (required < minimum ||
	    info->Size < minimum ||
	    info->Size > required)
		return EFI_DEVICE_ERROR;

	/* Refuses a directory. */
	if ((info->Attribute & EFI_FILE_DIRECTORY) != 0U)
		return EFI_UNSUPPORTED;

	/* Publishes the size. */
	*file_size = info->FileSize;

	/* Succeeded: the file is a regular file of that size. */
	return EFI_SUCCESS;
}

/* Reports whether a file can be sized and read, leaving it at its start. */
static EFI_STATUS
probe_readable_file(
	EFI_FILE_PROTOCOL *file)
{
	EFI_STATUS status;
	UINTN amount;
	UINT64 file_size;
	uint8_t byte;

	/* Reads at most one byte. */
	amount = 1U;

	/* Refuses a protocol without the services the loader uses. */
	if (file == 0 ||
	    file->Read == 0 ||
	    file->SetPosition == 0 ||
	    file->Close == 0)
		return EFI_INVALID_PARAMETER;

	/* Refuses a directory and a file whose size cannot be read. */
	status = regular_file_size(file, &file_size);
	if (EFI_ERROR(status))
		return status;

	/* Only the check matters here, not the size. */
	(void)file_size;

	/* Moves to the start. */
	status = file->SetPosition(file, 0U);
	if (EFI_ERROR(status))
		return status;

	/* Reads the first byte. */
	status = file->Read(file, &amount, &byte);
	if (EFI_ERROR(status))
		return status;

	/* Refuses a firmware that read more than asked. */
	if (amount > 1U)
		return EFI_DEVICE_ERROR;

	/* Moves back to the start for the real read. */
	status = file->SetPosition(file, 0U);
	if (EFI_ERROR(status))
		return status;

	/* Succeeded: the file is readable. */
	return EFI_SUCCESS;
}

/* Reads a whole file into a buffer, stopping when the buffer is full. */
static EFI_STATUS
read_bounded_file(
	EFI_FILE_PROTOCOL *file,
	void *buffer,
	UINTN capacity,
	UINTN *size)
{
	EFI_STATUS status;
	uint8_t *bytes;
	UINTN used;
	UINTN amount;

	/* Views the buffer as bytes. */
	bytes = buffer;
	used = 0U;

	/* Refuses missing services and buffers. */
	if (file == 0 ||
	    file->Read == 0 ||
	    file->SetPosition == 0 ||
	    buffer == 0 ||
	    capacity == 0U ||
	    size == 0)
		return EFI_INVALID_PARAMETER;

	/* Moves to the start. */
	*size = 0U;
	status = file->SetPosition(file, 0U);
	if (EFI_ERROR(status))
		return status;

	/* Reads until the end of the file or a full buffer. */
	while (used < capacity) {
		/* Reads as much as the buffer has room for. */
		amount = capacity - used;
		status = file->Read(file, &amount, bytes + used);
		if (EFI_ERROR(status))
			return status;

		/* Refuses a firmware that read more than asked. */
		if (amount > capacity - used)
			return EFI_DEVICE_ERROR;

		/* The end of the file. */
		if (amount == 0U)
			break;

		used += amount;
	}

	/* Publishes the size read. */
	*size = used;

	/* Succeeded: the buffer holds the file, or its first capacity bytes. */
	return EFI_SUCCESS;
}

/* Converts a relative slash path to a UTF-16 backslash path from the volume's root. */
static int
kernel_path_utf16(
	const char *source,
	CHAR16 output[KERNEL_PATH_CHAR16_STORAGE])
{
	UINTN length;

	/* Starts at the volume's root. */
	length = 0U;

	/* Refuses a missing path or output. */
	if (source == 0 || output == 0)
		return 0;

	/* Copies each character after the leading backslash, turning slashes into backslashes. */
	output[0] = '\\';
	while (source[length] != 0) {
		/* Refuses a path too long for the output. */
		if (length >= ZBL_KERN_CONFIG_KERNEL_PATH_MAX)
			return 0;

		/* A slash separates directories as a backslash does on the firmware. */
		if (source[length] == '/')
			output[length + 1U] = '\\';
		else
			output[length + 1U] = (uint8_t)source[length];

		length++;
	}

	/* Refuses an empty path. */
	if (length == 0U)
		return 0;

	/* Terminates the path. */
	output[length + 1U] = 0;

	/* Succeeded: the output holds the path. */
	return 1;
}

/* Writes why a candidate volume was passed over. */
static void
console_candidate_result(
	struct loader_context *context,
	const char *label,
	UINTN order,
	const char *result)
{
	/* Writes the candidate's place in the order and the result's name. */
	console_hex64(context, label, order);
	console_ascii(context, "A64 CFG DETAIL ");
	console_ascii(context, result);
	console_ascii(context, "\n");
}

/* Reports whether a candidate's medium has a block size and alignment the boot sector buffer meets. */
static int
block_media_usable(
	const EFI_BLOCK_IO_MEDIA *media)
{
	/* The block size is zero or larger than the buffer, or the medium has no last block. */
	if (media->BlockSize == 0U ||
	    media->BlockSize > sizeof(boot_sector) ||
	    media->LastBlock == UINT64_MAX)
		return 0;

	/* An alignment requirement must be a power of two the buffer meets. */
	if (media->IoAlign > 1U &&
	    ((media->IoAlign & (media->IoAlign - 1U)) != 0U ||
	     media->IoAlign > BOOT_SECTOR_BUFFER ||
	     ((uintptr_t)boot_sector & (media->IoAlign - 1U)) != 0U))
		return 0;

	/* Succeeded: the boot sector can be read into the buffer. */
	return 1;
}

/*
 * Examines one candidate volume and records it when it holds zedbsd.cfg.
 *
 * A candidate that is not usable is reported and passed over with
 * EFI_SUCCESS.  Only a failure that ends the whole discovery is returned as
 * an error, after the selection has been released.
 */
static EFI_STATUS
examine_candidate(
	struct loader_context *context,
	EFI_HANDLE handle,
	size_t order,
	const struct zbl_uefi_partition_path *loaded_path,
	struct zbl_uefi_volume_selection *selection,
	struct zbl_uefi_partition_path *selected_path)
{
	EFI_BOOT_SERVICES *boot;
	EFI_DEVICE_PATH_PROTOCOL *device_path;
	EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *filesystem;
	EFI_BLOCK_IO_PROTOCOL *block;
	EFI_BLOCK_IO_MEDIA *media;
	EFI_FILE_PROTOCOL *root;
	EFI_FILE_PROTOCOL *config;
	struct zbl_uefi_partition_path path;
	struct zbl_uefi_fat_info fat;
	EFI_STATUS status;
	EFI_STATUS close_status;
	EFI_STATUS reported;
	enum zbl_uefi_device_path_result path_result;
	enum zbl_uefi_fat_result fat_result;
	enum zbl_uefi_volume_match_action action;
	const char *operation;
	int same_disk;
	int usable;

	/* Nothing is open on the candidate yet. */
	boot = context->boot;
	device_path = 0;
	filesystem = 0;
	block = 0;
	root = 0;
	config = 0;

	/* Reads the candidate's device path. */
	status = boot->HandleProtocol(handle, &EFI_DEVICE_PATH_PROTOCOL_GUID, (void **)&device_path);
	if (EFI_ERROR(status)) {
		console_status(context, "A64 CFG DevicePath", status);
		return EFI_SUCCESS;
	}

	/* Passes over a candidate whose path does not name one partition. */
	path_result = zbl_uefi_partition_path_parse(device_path, ZBL_UEFI_DEVICE_PATH_MAX_BYTES, &path);
	if (path_result != ZBL_UEFI_DEVICE_PATH_OK) {
		console_candidate_result(context, "A64 CFG PATH ", order, zbl_uefi_device_path_result_name(path_result));
		return EFI_SUCCESS;
	}

	/* Passes over a partition of another disk. */
	same_disk = zbl_uefi_partition_paths_same_disk(loaded_path, &path);
	if (!same_disk) {
		console_hex64(context, "A64 CFG OTHER DISK ", order);
		return EFI_SUCCESS;
	}

	/* Finds the candidate's block device. */
	status = boot->HandleProtocol(handle, &EFI_BLOCK_IO_PROTOCOL_GUID, (void **)&block);
	if (EFI_ERROR(status)) {
		console_status(context, "A64 CFG BlockIO", status);
		return EFI_SUCCESS;
	}

	/* Passes over a block device without a medium or a read service. */
	if (block == 0 || block->Media == 0 || block->ReadBlocks == 0) {
		console_hex64(context, "A64 CFG INVALID BLOCK ", order);
		return EFI_SUCCESS;
	}

	/* Passes over an empty drive. */
	media = block->Media;
	if (!media->MediaPresent) {
		console_hex64(context, "A64 CFG NO MEDIA ", order);
		return EFI_SUCCESS;
	}

	/* Passes over a whole disk: only a partition holds the configuration. */
	if (!media->LogicalPartition) {
		console_hex64(context, "A64 CFG NOT PARTITION ", order);
		return EFI_SUCCESS;
	}

	/* Passes over a medium whose blocks the boot sector buffer cannot take. */
	usable = block_media_usable(media);
	if (!usable) {
		console_hex64(context, "A64 CFG BLOCK BOUNDS ", order);
		return EFI_SUCCESS;
	}

	/* Reads the partition's boot sector. */
	status = block->ReadBlocks(block, media->MediaId, 0U, media->BlockSize, boot_sector);
	if (EFI_ERROR(status)) {
		/* A changed medium is told apart from a failed read. */
		if (status == EFI_MEDIA_CHANGED)
			operation = "A64 CFG media changed";
		else
			operation = "A64 CFG read BPB";

		console_status(context, operation, status);
		return EFI_SUCCESS;
	}

	/* Passes over a volume that is not FAT16 or FAT32 with a serial. */
	fat_result = zbl_uefi_fat_bpb_parse(boot_sector, media->BlockSize, media->BlockSize, media->LastBlock + 1U, &fat);
	if (fat_result != ZBL_UEFI_FAT_OK) {
		console_candidate_result(context, "A64 CFG FAT ", order, zbl_uefi_fat_result_name(fat_result));
		return EFI_SUCCESS;
	}

	/* Finds the volume's file system. */
	status = boot->HandleProtocol(handle, &EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID, (void **)&filesystem);
	if (EFI_ERROR(status) ||
	    filesystem == 0 ||
	    filesystem->OpenVolume == 0) {
		/* Reports the firmware's error, or a file system without OpenVolume. */
		reported = EFI_INVALID_PARAMETER;
		if (EFI_ERROR(status))
			reported = status;

		console_status(context, "A64 CFG SimpleFS", reported);
		return EFI_SUCCESS;
	}

	/* Opens the volume's root directory. */
	status = filesystem->OpenVolume(filesystem, &root);
	if (EFI_ERROR(status) ||
	    root == 0 ||
	    root->Open == 0 ||
	    root->Close == 0) {
		/* A root that cannot be closed ends the discovery. */
		close_status = close_file_pair(0, root);
		if (EFI_ERROR(close_status)) {
			status = discovery_abort(selection, close_status);
			return status;
		}

		/* Reports the firmware's error, or a root without Open or Close. */
		reported = EFI_INVALID_PARAMETER;
		if (EFI_ERROR(status))
			reported = status;

		console_status(context, "A64 CFG OpenVolume", reported);
		return EFI_SUCCESS;
	}

	/* Opens zedbsd.cfg, the marker of the volume to boot from. */
	status = root->Open(root, &config, kern_config_path, EFI_FILE_MODE_READ, 0U);
	if (EFI_ERROR(status) || config == 0) {
		/* Files that cannot be closed end the discovery. */
		close_status = close_file_pair(config, root);
		if (EFI_ERROR(close_status)) {
			status = discovery_abort(selection, close_status);
			return status;
		}

		/* A volume without the file is told apart from a failed open. */
		if (status == EFI_NOT_FOUND) {
			console_hex64(context, "A64 CFG MISSING ", order);
		} else {
			/* Reports the firmware's error, or an open that gave no file. */
			reported = EFI_INVALID_PARAMETER;
			if (EFI_ERROR(status))
				reported = status;

			console_status(context, "A64 CFG open marker", reported);
		}

		return EFI_SUCCESS;
	}

	/* Passes over a marker that cannot be read. */
	status = probe_readable_file(config);
	if (EFI_ERROR(status)) {
		/* Files that cannot be closed end the discovery. */
		close_status = close_file_pair(config, root);
		if (EFI_ERROR(close_status)) {
			status = discovery_abort(selection, close_status);
			return status;
		}

		console_status(context, "A64 CFG unreadable", status);
		return EFI_SUCCESS;
	}

	/* Records the match; an inconsistent selection ends the discovery. */
	action = zbl_uefi_volume_selection_record_match(selection, handle, order, &fat, root, config);
	if (action == ZBL_UEFI_VOLUME_MATCH_INVALID) {
		/* Reports a failed close in place of the inconsistency. */
		close_status = close_file_pair(config, root);
		reported = EFI_LOAD_ERROR;
		if (EFI_ERROR(close_status))
			reported = close_status;

		status = discovery_abort(selection, reported);
		return status;
	}

	/* The first match keeps its files open; a later one closes them. */
	console_hex64(context, "A64 CFG MATCH ", order);
	if (action == ZBL_UEFI_VOLUME_MATCH_KEEP) {
		*selected_path = path;
	} else {
		/* Files that cannot be closed end the discovery. */
		close_status = close_file_pair(config, root);
		if (EFI_ERROR(close_status)) {
			status = discovery_abort(selection, close_status);
			return status;
		}
	}

	/* Succeeded: the candidate was examined. */
	return EFI_SUCCESS;
}

/*
 * Finds the one volume on the boot disk that holds zedbsd.cfg.
 *
 * The loader's own volume is examined first, then every other volume in the
 * firmware's order; when several hold the file, the first one is used.
 */
static EFI_STATUS
discover_config_volume(
	struct loader_context *context,
	const EFI_LOADED_IMAGE_PROTOCOL *loaded,
	struct discovered_volume *discovered)
{
	EFI_BOOT_SERVICES *boot;
	EFI_DEVICE_PATH_PROTOCOL *loaded_device_path;
	struct zbl_uefi_partition_path loaded_path;
	struct zbl_uefi_partition_path selected_path;
	struct zbl_uefi_volume_selection selection;
	struct zbl_uefi_volume_match selected;
	EFI_HANDLE *firmware_handles;
	EFI_HANDLE ordered_handles[ZBL_UEFI_VOLUME_MAX_HANDLES];
	UINTN firmware_handle_count;
	size_t ordered_count;
	size_t order;
	size_t index;
	EFI_STATUS status;
	EFI_STATUS close_status;
	enum zbl_uefi_device_path_result path_result;
	enum zbl_uefi_handle_order_result order_result;
	enum zbl_uefi_volume_selection_result selection_result;
	int copied;
	int taken;

	/* Nothing has been asked of the firmware yet. */
	boot = context->boot;
	loaded_device_path = 0;
	firmware_handles = 0;
	firmware_handle_count = 0U;
	ordered_count = 0U;

	/* Refuses a missing image or result, and boot services without the calls discovery uses. */
	if (loaded == 0 ||
	    discovered == 0 ||
	    loaded->DeviceHandle == 0 ||
	    boot->HandleProtocol == 0 ||
	    boot->LocateHandleBuffer == 0 ||
	    boot->FreePool == 0)
		return EFI_INVALID_PARAMETER;

	/* Starts with an empty result and selection. */
	byte_zero(discovered, sizeof(*discovered));
	byte_zero(&selected_path, sizeof(selected_path));
	zbl_uefi_volume_selection_init(&selection);

	/* Reads the device path of the volume the loader was loaded from. */
	status = boot->HandleProtocol(loaded->DeviceHandle, &EFI_DEVICE_PATH_PROTOCOL_GUID, (void **)&loaded_device_path);
	if (EFI_ERROR(status))
		return status;

	/* Refuses a loader that was not loaded from a partition. */
	path_result = zbl_uefi_partition_path_parse(loaded_device_path, ZBL_UEFI_DEVICE_PATH_MAX_BYTES, &loaded_path);
	if (path_result != ZBL_UEFI_DEVICE_PATH_OK) {
		console_ascii(context, "A64 loaded device path rejected: ");
		console_ascii(context, zbl_uefi_device_path_result_name(path_result));
		console_ascii(context, "\n");
		return EFI_UNSUPPORTED;
	}

	/* Asks for every volume with a file system; none at all leaves only the loader's own. */
	status = boot->LocateHandleBuffer(ByProtocol,
					  &EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID,
					  0,
					  &firmware_handle_count,
					  &firmware_handles);
	if (status == EFI_NOT_FOUND) {
		/* Gives back a buffer the firmware returned anyway. */
		if (firmware_handles != 0) {
			close_status = boot->FreePool(firmware_handles);
			firmware_handles = 0;
			if (EFI_ERROR(close_status))
				return close_status;
		}

		firmware_handle_count = 0U;
		status = EFI_SUCCESS;
	}

	/* Refuses a failed search, giving back any buffer it returned. */
	if (EFI_ERROR(status)) {
		if (firmware_handles != 0) {
			close_status = boot->FreePool(firmware_handles);
			firmware_handles = 0;
			if (EFI_ERROR(close_status))
				return close_status;
		}

		return status;
	}

	/* Refuses a count without a buffer. */
	if (firmware_handle_count != 0U && firmware_handles == 0)
		return EFI_DEVICE_ERROR;

	/* Orders the handles with the loader's own first. */
	order_result = zbl_uefi_volume_order_handles(loaded->DeviceHandle,
						     (void *const *)firmware_handles,
						     firmware_handle_count,
						     ordered_handles,
						     ZBL_UEFI_VOLUME_MAX_HANDLES,
						     &ordered_count);

	/* Gives back the firmware's buffer. */
	if (firmware_handles != 0) {
		status = boot->FreePool(firmware_handles);
		firmware_handles = 0;
		if (EFI_ERROR(status))
			return status;
	}

	/* Refuses an order that could not be built: a bad list, or too many handles. */
	if (order_result != ZBL_UEFI_HANDLE_ORDER_OK) {
		if (order_result == ZBL_UEFI_HANDLE_ORDER_INVALID_ARGUMENT)
			return EFI_DEVICE_ERROR;

		return EFI_BAD_BUFFER_SIZE;
	}

	/* Examines each candidate in order. */
	for (order = 0U; order < ordered_count; order++) {
		status = examine_candidate(context, ordered_handles[order], order, &loaded_path, &selection, &selected_path);
		if (EFI_ERROR(status))
			return status;
	}

	/* Refuses a disk without zedbsd.cfg. */
	selection_result = zbl_uefi_volume_selection_finish(&selection);
	if (selection_result == ZBL_UEFI_VOLUME_SELECTION_NOT_FOUND) {
		console_ascii(context, "zedbsd.cfg was not found on the boot disk\n");
		return EFI_NOT_FOUND;
	}

	/* Refuses an inconsistent selection. */
	if (selection_result == ZBL_UEFI_VOLUME_SELECTION_INVALID_ARGUMENT) {
		status = discovery_abort(&selection, EFI_LOAD_ERROR);
		return status;
	}

	/* Warns that the first of several files is used. */
	if (selection_result == ZBL_UEFI_VOLUME_SELECTION_MULTIPLE) {
		console_ascii(context, "WARNING: multiple zedbsd.cfg files on the boot disk; using the first\n");
		console_hex64(context, "A64 CFG MATCHES ", selection.match_count);
	}

	/* Records where the loader came from and where the configuration was found. */
	discovered->provenance.version = KERN_BOOT_PROVENANCE_VERSION;
	discovered->provenance.config_matches = (uint32_t)selection.match_count;
	copied = zbl_uefi_partition_identity_copy(&loaded_path, &discovered->provenance.firmware);
	if (!copied) {
		status = discovery_abort(&selection, EFI_LOAD_ERROR);
		return status;
	}

	/* Records the configuration's partition. */
	copied = zbl_uefi_partition_identity_copy(&selected_path, &discovered->provenance.configuration);
	if (!copied) {
		status = discovery_abort(&selection, EFI_LOAD_ERROR);
		return status;
	}

	/* Takes the selected volume and its open files out of the selection. */
	discovered->match_count = selection.match_count;
	taken = zbl_uefi_volume_selection_take(&selection, &selected);
	if (!taken) {
		status = discovery_abort(&selection, EFI_LOAD_ERROR);
		return status;
	}

	/* Publishes the selected volume. */
	discovered->handle = selected.handle;
	discovered->root = selected.root;
	discovered->config = selected.config;
	discovered->fat = selected.fat;
	discovered->path = selected_path;
	discovered->order = selected.order;
	for (index = 0U; index < sizeof(discovered->uuid); index++)
		discovered->uuid[index] = selected.uuid[index];

	/* Reports the selected volume. */
	console_hex64(context, "A64 CFG SELECTED ", discovered->order);
	console_ascii(context, "A64 CFG UUID ");
	console_ascii(context, discovered->uuid);
	console_ascii(context, "\n");

	/* Succeeded: the volume and its files are the caller's. */
	return EFI_SUCCESS;
}

/* Closes the files still open on the selected volume. */
static EFI_STATUS
close_discovered_volume(
	struct discovered_volume *discovered)
{
	EFI_STATUS status;

	/* There is no volume. */
	if (discovered == 0)
		return EFI_INVALID_PARAMETER;

	/* Closes the configuration file and the root; they are gone either way. */
	status = close_file_pair(discovered->config, discovered->root);
	discovered->config = 0;
	discovered->root = 0;
	if (EFI_ERROR(status))
		return status;

	/* Succeeded: nothing is open on the volume. */
	return EFI_SUCCESS;
}

/* Closes the selected volume, then reports the operation that failed and stops. */
static void __attribute__((noreturn))
fail_discovered(
	struct loader_context *context,
	struct discovered_volume *discovered,
	const char *operation,
	EFI_STATUS status)
{
	EFI_STATUS cleanup;

	/* Reports a failed close before the failure itself. */
	cleanup = close_discovered_volume(discovered);
	if (EFI_ERROR(cleanup))
		console_status(context, "Close selected volume", cleanup);

	fail_status(context, operation, status);
}

/* Reads zedbsd.cfg from the selected volume, closes it and parses it. */
static EFI_STATUS
load_selected_configuration(
	struct loader_context *context,
	struct discovered_volume *discovered,
	struct zbl_uefi_kern_config *configuration)
{
	EFI_STATUS read_status;
	EFI_STATUS close_status;
	UINTN config_size;
	size_t split_size;
	enum zbl_uefi_kern_config_result config_result;
	enum zbl_uefi_root_config_result root_result;
	const char *root_reason;

	/* Refuses a volume without an open configuration file. */
	if (discovered == 0 || discovered->config == 0 || configuration == 0)
		return EFI_INVALID_PARAMETER;

	/* Reads the file and closes it, whatever the read gave. */
	read_status = read_bounded_file(discovered->config, config_buffer, sizeof(config_buffer), &config_size);
	close_status = close_file_pair(discovered->config, 0);
	discovered->config = 0;
	if (EFI_ERROR(read_status)) {
		/* A failed close is reported beside the failed read. */
		if (EFI_ERROR(close_status))
			console_status(context, "Close zedbsd.cfg", close_status);

		return read_status;
	}

	/* Refuses a file that could not be closed. */
	if (EFI_ERROR(close_status))
		return close_status;

	/* Takes the rootpart: prefixes off, noting which files are on the root partition. */
	root_result = zbl_uefi_root_config_split(config_buffer,
						 config_size,
						 config_split_buffer,
						 sizeof(config_split_buffer),
						 &split_size,
						 &discovered->root_files);
	if (root_result != ZBL_UEFI_ROOT_CONFIG_OK) {
		root_reason = zbl_uefi_root_config_result_name(root_result);
		console_ascii(context, "zedbsd.cfg rejected: ");
		console_ascii(context, root_reason);
		console_ascii(context, "\n");
		return EFI_LOAD_ERROR;
	}

	/* Parses the file against the selected volume's UUID. */
	config_result = zbl_uefi_kern_config_parse(configuration,
						   config_split_buffer,
						   split_size,
						   discovered->uuid,
						   sizeof(discovered->uuid));
	if (config_result != ZBL_UEFI_KERN_CONFIG_OK) {
		console_ascii(context, "zedbsd.cfg rejected: ");
		console_ascii(context, zbl_uefi_kern_config_result_name(config_result));
		console_ascii(context, "\n");
		return EFI_LOAD_ERROR;
	}

	/* Reports the kernel path, its volume and the parameters. */
	console_ascii(context, "A64 KERNEL ");
	if (discovered->root_files.kernel_on_root)
		console_ascii(context, "rootpart:/");
	console_ascii(context, configuration->kernel_path);
	console_ascii(context, "\n");
	console_ascii(context, "A64 PARAMS ");
	console_ascii(context, configuration->parameter_record.text);
	console_ascii(context, "\n");

	/* Succeeded: the configuration is parsed. */
	return EFI_SUCCESS;
}

/* Gives pages back to the firmware, refusing boot services without FreePages. */
static EFI_STATUS
free_pages(
	EFI_BOOT_SERVICES *boot,
	EFI_PHYSICAL_ADDRESS address,
	UINTN pages)
{
	EFI_STATUS status;

	/* Boot services without FreePages cannot give the pages back. */
	if (boot->FreePages == 0)
		return EFI_INVALID_PARAMETER;

	/* Gives the pages back. */
	status = boot->FreePages(address, pages);
	if (EFI_ERROR(status))
		return status;

	/* Succeeded: the pages are the firmware's again. */
	return EFI_SUCCESS;
}

/* Gives pool storage back to the firmware, refusing boot services without FreePool. */
static EFI_STATUS
free_pool(
	EFI_BOOT_SERVICES *boot,
	void *pool)
{
	EFI_STATUS status;

	/* Boot services without FreePool cannot give the storage back. */
	if (boot->FreePool == 0)
		return EFI_INVALID_PARAMETER;

	/* Gives the storage back. */
	status = boot->FreePool(pool);
	if (EFI_ERROR(status))
		return status;

	/* Succeeded: the storage is the firmware's again. */
	return EFI_SUCCESS;
}

/* Closes the kernel and its volume, frees the kernel's pages, then reports the failure and stops. */
static void __attribute__((noreturn))
fail_kernel_load(
	struct loader_context *context,
	struct discovered_volume *discovered,
	EFI_FILE_PROTOCOL *kernel,
	const char *operation,
	EFI_STATUS status,
	EFI_PHYSICAL_ADDRESS kernel_address,
	UINTN kernel_pages,
	int pages_allocated)
{
	EFI_STATUS cleanup;

	/* Closes the kernel and the volume's root. */
	cleanup = close_file_pair(kernel, discovered->root);
	discovered->root = 0;
	discovered->config = 0;
	if (EFI_ERROR(cleanup))
		console_status(context, "Close kernel volume", cleanup);

	/* Gives the kernel's pages back when they were allocated. */
	if (pages_allocated) {
		cleanup = free_pages(context->boot, kernel_address, kernel_pages);
		if (EFI_ERROR(cleanup))
			console_status(context, "Free kernel pages", cleanup);
	}

	fail_status(context, operation, status);
}

/* Frees the loader's pool, the low block and the kernel's pages, then reports the failure and stops. */
static void __attribute__((noreturn))
fail_boot_allocations(
	struct loader_context *context,
	const char *operation,
	EFI_STATUS status,
	void *pool,
	int pool_allocated,
	EFI_PHYSICAL_ADDRESS low_address,
	UINTN low_pages,
	int low_pages_allocated,
	EFI_PHYSICAL_ADDRESS kernel_address,
	UINTN kernel_pages)
{
	EFI_STATUS cleanup;

	/* Gives the pool storage back when it was allocated. */
	if (pool_allocated) {
		cleanup = free_pool(context->boot, pool);
		if (EFI_ERROR(cleanup))
			console_status(context, "Free loader pool", cleanup);
	}

	/* Gives the low block back when it was allocated. */
	if (low_pages_allocated) {
		cleanup = free_pages(context->boot, low_address, low_pages);
		if (EFI_ERROR(cleanup))
			console_status(context, "Free bootstrap pages", cleanup);
	}

	/* Gives the kernel's pages back. */
	cleanup = free_pages(context->boot, kernel_address, kernel_pages);
	if (EFI_ERROR(cleanup))
		console_status(context, "Free kernel pages", cleanup);

	fail_status(context, operation, status);
}

/* Takes the framebuffer of the GOP's active mode, when the kernel can use it. */
static int
framebuffer_from_gop(
	EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
	struct zbl6_framebuffer *framebuffer,
	struct zbl_uefi_framebuffer_mapping *mapping)
{
	EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *mode;
	EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
	int planned;

	/* Refuses a GOP without an active mode. */
	if (gop == 0 || gop->Mode == 0 || gop->Mode->Info == 0 || mapping == 0)
		return 0;

	/* Refuses a mode description shorter than the structure. */
	mode = gop->Mode;
	if (mode->SizeOfInfo < sizeof(*info))
		return 0;

	/* Refuses an empty mode, rows narrower than the width, and a pixel layout the kernel does not take. */
	info = mode->Info;
	if (info->HorizontalResolution == 0 ||
	    info->VerticalResolution == 0 ||
	    info->PixelsPerScanLine < info->HorizontalResolution ||
	    (info->PixelFormat != PixelRedGreenBlueReserved8BitPerColor &&
	     info->PixelFormat != PixelBlueGreenRedReserved8BitPerColor))
		return 0;

	/* Refuses a framebuffer the bootstrap tables cannot map. */
	planned = zbl_uefi_framebuffer_mapping_plan(mode->FrameBufferBase,
						    mode->FrameBufferSize,
						    info->HorizontalResolution,
						    info->VerticalResolution,
						    info->PixelsPerScanLine,
						    4U,
						    mapping);
	if (!planned)
		return 0;

	/* Describes the framebuffer. */
	framebuffer->physical_base = mode->FrameBufferBase;
	framebuffer->size = mode->FrameBufferSize;
	framebuffer->width = info->HorizontalResolution;
	framebuffer->height = info->VerticalResolution;
	framebuffer->stride = info->PixelsPerScanLine;

	/* Names the pixel layout: red or blue in the low byte. */
	if (info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor)
		framebuffer->format = ZBL6_FRAMEBUFFER_RGBX8888;
	else
		framebuffer->format = ZBL6_FRAMEBUFFER_BGRX8888;

	/* Succeeded: the framebuffer is described. */
	return 1;
}

/* Reports whether a text of a length is exactly a literal. */
static int
token_is(
	const char *text,
	size_t length,
	const char *literal)
{
	size_t index;

	/* Compares the text with the literal; a literal that ends early differs. */
	for (index = 0; index < length; index++) {
		if (literal[index] == 0 || text[index] != literal[index])
			return 0;
	}

	/* A literal that goes on past the text differs. */
	if (literal[length] != 0)
		return 0;

	/* Succeeded: the text is the literal. */
	return 1;
}

/* Parses at most one optional kernel_phys token from bounded parameter text. */
static int
kernel_placement_parse(
	const char *text,
	size_t length,
	struct kernel_placement_policy *policy)
{
	/* The token's name. */
	static const char prefix[] = "kernel_phys=";
	size_t prefix_length;
	size_t position;
	size_t start;
	size_t end;
	uint64_t value;
	unsigned digit;
	char byte;
	int found;
	int matches;

	/* Starts with the default placement. */
	prefix_length = sizeof(prefix) - 1U;
	policy->link_only = 0;
	policy->fixed = 0;

	/* Refuses a text that is unbounded or not terminated where its length says. */
	if (text == 0 ||
	    length > KERN_BOOT_PARAMETERS_TEXT_MAX ||
	    text[length] != '\0')
		return -1;

	/* Looks at each space-separated token. */
	position = 0;
	found = 0;
	while (position < length) {
		/* Finds the end of the token, refusing a blank or non-ASCII byte. */
		start = position;
		while (position < length && text[position] != ' ') {
			if ((unsigned char)text[position] < 33 ||
			    (unsigned char)text[position] > 126)
				return -1;

			position++;
		}

		/* Moves past the separating space. */
		end = position;
		if (position < length)
			position++;

		/* A token too short to carry a value is not kernel_phys=. */
		if (end - start <= prefix_length)
			continue;

		/* A token of another name belongs to someone else. */
		matches = token_is(text + start, prefix_length, prefix);
		if (!matches)
			continue;

		/* Refuses a second kernel_phys= token. */
		if (found)
			return -1;

		/* "auto" keeps the default. */
		found = 1;
		start += prefix_length;
		matches = token_is(text + start, end - start, "auto");
		if (matches)
			continue;

		/* "link" insists on the linked address. */
		matches = token_is(text + start, end - start, "link");
		if (matches) {
			policy->link_only = 1;
			continue;
		}

		/* An explicit home: 0x followed by up to 16 hexadecimal digits. */
		if (end - start < 3U ||
		    end - start > 18U ||
		    text[start] != '0' ||
		    (text[start + 1] != 'x' && text[start + 1] != 'X'))
			return -1;

		/* Reads the digits. */
		value = 0;
		for (start += 2U; start < end; start++) {
			/* Decodes the digit, refusing anything that is not one. */
			byte = text[start];
			if (byte >= '0' && byte <= '9')
				digit = (unsigned)(byte - '0');
			else if (byte >= 'a' && byte <= 'f')
				digit = (unsigned)(byte - 'a') + 10U;
			else if (byte >= 'A' && byte <= 'F')
				digit = (unsigned)(byte - 'A') + 10U;
			else
				return -1;

			value = (value << 4) | digit;
		}

		/* Refuses an address that is zero, unaligned, below the link address or outside the window. */
		if (value == 0 ||
		    (value & (AMD64_KERNEL_PHYS_ALIGN - 1U)) != 0 ||
		    value < AMD64_KERNEL_LINK_PHYS_START ||
		    value >= AMD64_KERNEL_PHYS_LIMIT)
			return -1;

		/* The kernel goes at the address. */
		policy->fixed = value;
	}

	/* Succeeded: reports whether the text names a placement. */
	return found;
}

/*
 * Draws the boot logo zedbsd.cfg names (logo=PATH on the selected volume).
 *
 * Returns 1 when the logo is on the screen.  A missing or unreadable logo
 * is not an error: the boot goes on without it.
 */
static int
show_logo(
	struct loader_context *context,
	struct discovered_volume *discovered,
	const struct zbl_uefi_kern_config *configuration,
	const struct zbl6_framebuffer *framebuffer)
{
	char path[ZBL_LOGO_PATH_MAX + 1U];
	struct boot_file file;
	EFI_STATUS status;
	UINT64 size;
	void *buffer;
	int found;
	int drawn;
	int failed;

	/* The logo's path, when zedbsd.cfg names one. */
	found = zbl_logo_path(configuration->parameter_record.text,
			      configuration->parameter_record.length,
			      path,
			      sizeof(path));
	if (found < 0)
		console_ascii(context, "A64 LOGO rejected\n");

	/* There is no logo to draw. */
	if (found <= 0)
		return 0;

	/* Opens the logo file, on the FAT volume or on the root partition. */
	status = boot_file_open(context, discovered, path, discovered->root_files.logo_on_root, &file);
	failed = EFI_ERROR(status);
	if (failed) {
		console_ascii(context, "A64 LOGO missing\n");
		return 0;
	}

	/* Its size, bounded. */
	size = file.size;
	if (size == 0U || size > ZBL_UEFI_LOGO_FILE_MAX) {
		(void)close_file_pair(file.fat, 0);
		return 0;
	}

	/* Its bytes, in pool storage given back before the memory map is taken. */
	buffer = 0;
	status = context->boot->AllocatePool(EfiLoaderData, (UINTN)size, &buffer);
	failed = EFI_ERROR(status);
	if (failed || buffer == 0) {
		(void)close_file_pair(file.fat, 0);
		return 0;
	}

	/* The logo on the screen when the whole file was read. */
	status = boot_file_read(&file, 0U, buffer, (UINTN)size);
	failed = EFI_ERROR(status);
	(void)close_file_pair(file.fat, 0);
	drawn = 0;
	if (!failed)
		drawn = zbl_uefi_logo_draw(buffer, (size_t)size, framebuffer);

	/* Gives the storage back. */
	(void)context->boot->FreePool(buffer);

	/* Notes a drawn logo on the debug port. */
	if (drawn)
		debug_port("A64 LOGO shown\n");

	/* Reports whether the logo is on the screen. */
	return drawn;
}

/* Rewrites the boot parameters for the boot keys held so far. */
static EFI_STATUS
apply_boot_keys(
	struct loader_context *context,
	struct zbl_uefi_kern_config *configuration,
	unsigned held)
{
	int rewritten;

	/* No boot key: the parameters stay exactly as zedbsd.cfg made them. */
	if (held == 0U)
		return EFI_SUCCESS;

	/*
	 * Ctrl replaces kmsg= with kmsg=console and drops logo=; Shift
	 * replaces login= with login=console.
	 */
	rewritten = zbl_boot_override_apply(&configuration->parameter_record, held);
	if (rewritten != 0) {
		console_ascii(context, "A64 PARAMS OVERRIDE rejected\n");
		return EFI_INVALID_PARAMETER;
	}

	/* Succeeded: the record the kernel will receive carries the keys' choices. */
	return EFI_SUCCESS;
}

/* Tells which boot keys changed the boot, and the parameters the kernel gets. */
static void
notice_boot_keys(
	struct loader_context *context,
	const struct zbl_uefi_kern_config *configuration,
	unsigned held)
{
	/* Nothing to tell when no boot key was held. */
	if (held == 0U)
		return;

	/* Ctrl: the kernel messages are shown on the console. */
	if ((held & ZBL_BOOT_OVERRIDE_KMSG) != 0U)
		console_ascii(context, "Boot: kernel messages (Ctrl)\n");

	/* Shift: the login is on the console. */
	if ((held & ZBL_BOOT_OVERRIDE_LOGIN) != 0U)
		console_ascii(context, "Boot: console login (Shift)\n");

	/* The rewritten parameters, beside the A64 PARAMS line of zedbsd.cfg's. */
	console_ascii(context, "A64 PARAMS OVERRIDE ");
	console_ascii(context, configuration->parameter_record.text);
	console_ascii(context, "\n");
}

/* Prints one firmware descriptor as "type start pages" for placement diagnosis. */
static void
console_map_entry(
	struct loader_context *context,
	const EFI_MEMORY_DESCRIPTOR *descriptor)
{
	/* The hexadecimal digits and the line's label. */
	static const char digits[] = "0123456789abcdef";
	static const char label[] = "A64 KERN MAP t=";
	char message[64];
	UINTN used;
	UINTN index;
	uint64_t values[2];
	unsigned value;
	int shift;

	/* Copies the label. */
	used = 0;
	for (index = 0; label[index] != 0; index++) {
		message[used] = label[index];
		used++;
	}

	/* Appends the type in two digits. */
	message[used] = digits[(descriptor->Type >> 4) & 15U];
	used++;
	message[used] = digits[descriptor->Type & 15U];
	used++;

	/* Appends the start and, after a plus sign, the page count, in twelve digits each. */
	values[0] = descriptor->PhysicalStart;
	values[1] = descriptor->NumberOfPages;
	for (value = 0; value < 2; value++) {
		message[used] = ' ';
		used++;
		if (value == 1) {
			message[used] = '+';
			used++;
		}

		/* Appends the value's digits. */
		for (shift = 44; shift >= 0; shift -= 4) {
			message[used] = digits[(values[value] >> shift) & 15U];
			used++;
		}
	}

	/* Ends the line and writes it. */
	message[used] = '\n';
	used++;
	message[used] = 0;
	console_ascii(context, message);
}

/* Reports the firmware map below the bootstrap window when placement fails. */
static void
console_kernel_map(
	struct loader_context *context,
	const EFI_MEMORY_DESCRIPTOR *map,
	UINTN map_size,
	UINTN descriptor_size)
{
	const EFI_MEMORY_DESCRIPTOR *descriptor;
	UINTN offset;
	unsigned printed;

	/* Prints each descriptor below the window, up to the limit. */
	printed = 0;
	for (offset = 0;
	     offset + descriptor_size <= map_size;
	     offset += descriptor_size) {
		/* A descriptor above the window does not matter for the placement. */
		descriptor = (const void *)((const uint8_t *)map + offset);
		if (descriptor->PhysicalStart >= AMD64_KERNEL_PHYS_LIMIT)
			continue;

		/* Stops at the limit, saying more were left out. */
		if (printed == KERNEL_MAP_DUMP_LIMIT) {
			console_ascii(context, "A64 KERN MAP ...\n");
			break;
		}

		/* Prints the descriptor. */
		printed++;
		console_map_entry(context, descriptor);
	}
}

/* Owns exactly the requested pages at the requested address, or nothing. */
static EFI_STATUS
allocate_kernel_at(
	EFI_BOOT_SERVICES *boot,
	EFI_PHYSICAL_ADDRESS wanted,
	UINTN pages)
{
	EFI_PHYSICAL_ADDRESS address;
	EFI_STATUS status;

	/* Asks for the pages at the address. */
	address = wanted;
	status = boot->AllocatePages(AllocateAddress, EfiLoaderData, pages, &address);
	if (EFI_ERROR(status))
		return status;

	/* Gives back pages a firmware put elsewhere. */
	if (address != wanted) {
		(void)boot->FreePages(address, pages);
		return EFI_LOAD_ERROR;
	}

	/* Succeeded: the pages at the address are the loader's. */
	return EFI_SUCCESS;
}

/* Rebases the plan onto pages already allocated at an address, giving them back when it cannot. */
static EFI_STATUS
place_kernel_at(
	struct loader_context *context,
	struct zbl_elf64_plan *plan,
	EFI_PHYSICAL_ADDRESS wanted,
	UINTN pages)
{
	int placed;

	/* Refuses an address the plan cannot be rebased onto. */
	placed = zbl_elf64_place(plan, wanted);
	if (!placed) {
		(void)context->boot->FreePages(wanted, pages);
		return EFI_LOAD_ERROR;
	}

	/* Reports the kernel's home. */
	console_hex64(context, "A64 KERN LOAD ", wanted);

	/* Succeeded: the plan is rebased onto the pages. */
	return EFI_SUCCESS;
}

/*
 * Allocates the physical home of the kernel image and rebases the plan onto
 * it.  On success *address owns `pages` pages; on failure nothing is owned.
 */
static EFI_STATUS
place_kernel(
	struct loader_context *context,
	struct zbl_elf64_plan *plan,
	const struct kernel_placement_policy *policy,
	EFI_PHYSICAL_ADDRESS *address,
	UINTN pages)
{
	EFI_BOOT_SERVICES *boot;
	EFI_MEMORY_DESCRIPTOR *map;
	const EFI_MEMORY_DESCRIPTOR *descriptor;
	EFI_STATUS status;
	UINTN map_size;
	UINTN map_key;
	UINTN descriptor_size;
	UINTN capacity;
	UINTN offset;
	UINTN best_offset;
	UINTN skipped[KERNEL_PLACE_ATTEMPTS];
	UINT32 descriptor_version;
	uint64_t bytes;
	uint64_t wanted;
	uint64_t best;
	uint64_t start;
	uint64_t end;
	unsigned attempt;
	unsigned skip;
	unsigned skip_count;
	const char *operation;
	int busy;

	/* Reports the linked extent. */
	boot = context->boot;
	map = 0;
	bytes = (uint64_t)pages * PAGE_SIZE;
	console_hex64(context, "A64 KERN LINK ", plan->link_physical_start);
	console_hex64(context, "A64 KERN SIZE ", plan->link_physical_end - plan->link_physical_start);

	/* The linked address (or an explicit one) is tried first. */
	if (policy->fixed != 0)
		wanted = policy->fixed;
	else
		wanted = plan->link_physical_start;

	/* Takes the address when the firmware has it free. */
	status = allocate_kernel_at(boot, wanted, pages);
	if (!EFI_ERROR(status)) {
		status = place_kernel_at(context, plan, wanted, pages);
		if (EFI_ERROR(status))
			return status;

		*address = wanted;
		return EFI_SUCCESS;
	}

	/* Reports which address was refused. */
	if (policy->fixed != 0)
		operation = "Allocate kernel at kernel_phys";
	else
		operation = "Allocate kernel at linked address";

	console_status(context, operation, status);

	/* An explicit or linked-only placement has no other address to try. */
	if (policy->fixed != 0 || policy->link_only)
		return status;

	/* Otherwise the lowest aligned run of conventional memory is used. */
	map_size = 0;
	status = boot->GetMemoryMap(&map_size, 0, &map_key, &descriptor_size, &descriptor_version);
	if (status != EFI_BUFFER_TOO_SMALL ||
	    descriptor_size == 0 ||
	    map_size > UINTPTR_MAX - descriptor_size * 8U) {
		/* Reports the firmware's error, or an answer the loader cannot use. */
		if (EFI_ERROR(status))
			return status;

		return EFI_LOAD_ERROR;
	}

	/* Allocates the map with room for the descriptors the allocation adds. */
	capacity = map_size + descriptor_size * 8U;
	status = boot->AllocatePool(EfiLoaderData, capacity, (void **)&map);
	if (EFI_ERROR(status))
		return status;

	/* Reads the map. */
	map_size = capacity;
	status = boot->GetMemoryMap(&map_size, map, &map_key, &descriptor_size, &descriptor_version);
	if (EFI_ERROR(status)) {
		(void)boot->FreePool(map);
		return status;
	}

	/* Tries the lowest fitting run, then the next, skipping runs the firmware refused. */
	skip_count = 0;
	for (attempt = 0; attempt < KERNEL_PLACE_ATTEMPTS; attempt++) {
		/* Finds the lowest aligned start of conventional memory that holds the image. */
		best = UINT64_MAX;
		best_offset = 0;
		for (offset = 0;
		     offset + descriptor_size <= map_size;
		     offset += descriptor_size) {
			/* Only conventional memory can take the kernel. */
			descriptor = (const void *)((const uint8_t *)map + offset);
			busy = 0;
			if (descriptor->Type != EfiConventionalMemory)
				continue;

			/* A run refused before is skipped. */
			for (skip = 0; skip < skip_count; skip++) {
				if (skipped[skip] == offset)
					busy = 1;
			}

			if (busy)
				continue;

			/* A run whose end does not fit is skipped. */
			start = descriptor->PhysicalStart;
			if (descriptor->NumberOfPages > (UINT64_MAX - start) / PAGE_SIZE)
				continue;

			/* Cuts the run to the bootstrap window above the linked address. */
			end = start + descriptor->NumberOfPages * PAGE_SIZE;
			if (end > AMD64_KERNEL_PHYS_LIMIT)
				end = AMD64_KERNEL_PHYS_LIMIT;
			if (start < AMD64_KERNEL_LINK_PHYS_START)
				start = AMD64_KERNEL_LINK_PHYS_START;

			/* A run whose aligned start leaves too little room is skipped. */
			start = (start + AMD64_KERNEL_PHYS_ALIGN - 1U) & ~(AMD64_KERNEL_PHYS_ALIGN - 1U);
			if (start >= end || end - start < bytes)
				continue;

			/* The lowest start so far is the best. */
			if (start < best) {
				best = start;
				best_offset = offset;
			}
		}

		/* No run holds the image. */
		if (best == UINT64_MAX)
			break;

		/* Takes the best run when the firmware gives it. */
		status = allocate_kernel_at(boot, best, pages);
		if (!EFI_ERROR(status)) {
			(void)boot->FreePool(map);
			status = place_kernel_at(context, plan, best, pages);
			if (EFI_ERROR(status))
				return status;

			*address = best;
			console_ascii(context, "A64 KERN RELOCATED away from the linked address\n");
			return EFI_SUCCESS;
		}

		/* Skips the refused run in the next attempt. */
		console_hex64(context, "A64 KERN candidate refused ", best);
		skipped[skip_count] = best_offset;
		skip_count++;
	}

	/* Reports that nothing fits, with the map below the window. */
	console_ascii(context, "A64 KERN no free 2 MiB-aligned run below 1 GiB holds the image\n");
	console_kernel_map(context, map, map_size, descriptor_size);
	(void)boot->FreePool(map);

	/* Reports that no home was found. */
	return EFI_NOT_FOUND;
}

/* Builds the bootstrap page tables, copies the transition and fills the handoff in the low block. */
static void
build_bootstrap(
	uint64_t low_base,
	const struct zbl_elf64_plan *plan,
	const struct zbl6_framebuffer *framebuffer,
	const struct zbl_uefi_framebuffer_mapping *framebuffer_mapping,
	uint32_t boot_volume_serial,
	uint8_t partition_scheme,
	const struct kern_boot_parameter_record *parameters,
	const struct kern_boot_provenance *provenance)
{
	uint8_t *low;
	uint64_t *pml4;
	uint64_t *low_pdpt;
	uint64_t *low_pd;
	uint64_t *high_pdpt;
	uint64_t *high_pd;
	uint64_t *framebuffer_pd;
	struct zbl6_handoff_v8_uefi *handoff;
	struct zbl6_handoff_v3 *common;
	UINTN transition_size;
	unsigned index;
	unsigned first_slot;
	unsigned slot_count;
	uint64_t entry;
	uint64_t framebuffer_aligned;
	unsigned framebuffer_pages;

	/* Finds the tables and the handoff in the low block. */
	low = (void *)(uintptr_t)low_base;
	pml4 = (void *)(low + LOW_PML4_OFFSET);
	low_pdpt = (void *)(low + LOW_PDPT_OFFSET);
	low_pd = (void *)(low + LOW_PD_OFFSET);
	high_pdpt = (void *)(low + HIGH_PDPT_OFFSET);
	high_pd = (void *)(low + HIGH_PD_OFFSET);
	framebuffer_pd = (void *)(low + FRAMEBUFFER_PD_OFFSET);
	handoff = (void *)(low + HANDOFF_OFFSET);
	common = &handoff->prefix.common.common;
	transition_size = (UINTN)(zbl_transition_end - zbl_transition_start);

	/* Clears the low block. */
	byte_zero(low, LOW_BLOCK_PAGES * PAGE_SIZE);

	/* Stops on a transition larger than its page. */
	if (transition_size == 0 || transition_size > PAGE_SIZE)
		halt();

	/* Stops on a partition scheme the handoff does not name. */
	if (partition_scheme != ZBL6_PARTITION_SCHEME_MBR &&
	    partition_scheme != ZBL6_PARTITION_SCHEME_GPT)
		halt();

	/* Copies the transition code to the start of the block. */
	byte_copy(low + LOW_TRAMPOLINE_OFFSET, zbl_transition_start, transition_size);

	/* Links the identity window at 0 and the kernel window at -2 GiB. */
	pml4[0] = (low_base + LOW_PDPT_OFFSET) | PTE_PRESENT | PTE_WRITE;
	low_pdpt[0] = (low_base + LOW_PD_OFFSET) | PTE_PRESENT | PTE_WRITE;
	pml4[511] = (low_base + HIGH_PDPT_OFFSET) | PTE_PRESENT | PTE_WRITE;
	high_pdpt[510] = (low_base + HIGH_PD_OFFSET) | PTE_PRESENT | PTE_WRITE;

	/* Maps the first GiB with large pages in both windows. */
	for (index = 0; index < 512; index++) {
		entry = (uint64_t)index * 0x200000ULL | PTE_PRESENT | PTE_WRITE | PTE_LARGE;
		low_pd[index] = entry;
		high_pd[index] = entry;
	}

	/*
	 * The slots of the linked virtual range show the image's actual home.
	 * Both extents are 2 MiB aligned, so whole large pages move; with the
	 * linked placement this rewrites the identity entries unchanged.
	 */
	first_slot = (unsigned)(plan->link_physical_start / 0x200000ULL);
	slot_count = (unsigned)((plan->link_physical_end + 0x1fffffULL) / 0x200000ULL) - first_slot;
	if (first_slot + slot_count > 512U)
		halt();

	/* Points each slot of the linked range at the image's home. */
	for (index = 0; index < slot_count; index++) {
		high_pd[first_slot + index] =
		    (plan->physical_start + (uint64_t)index * 0x200000ULL) |
		    PTE_PRESENT | PTE_WRITE | PTE_LARGE;
	}

	/* Stops without a checked framebuffer mapping. */
	if (framebuffer_mapping == 0)
		halt();

	/* Maps the framebuffer at the top of the kernel window, from PD entry 16. */
	framebuffer_aligned = framebuffer_mapping->aligned_base;
	framebuffer_pages = framebuffer_mapping->large_page_count;
	high_pdpt[511] = (low_base + FRAMEBUFFER_PD_OFFSET) | PTE_PRESENT | PTE_WRITE;
	for (index = 0; index < framebuffer_pages; index++) {
		framebuffer_pd[16U + index] =
		    (framebuffer_aligned + (uint64_t)index * 0x200000ULL) |
		    PTE_PRESENT | PTE_WRITE | PTE_LARGE;
	}

	/* Fills the handoff's identity and what it carries. */
	common->magic = ZBL6_HANDOFF_MAGIC;
	common->version = ZBL6_HANDOFF_V8_VERSION;
	common->size = sizeof(*handoff);
	common->flags = ZBL6_HANDOFF_FLAG_UEFI |
			ZBL6_HANDOFF_FLAG_MEMORY_MAP |
			ZBL6_HANDOFF_FLAG_FRAMEBUFFER |
			ZBL6_HANDOFF_FLAG_BOOT_UUID |
			ZBL6_HANDOFF_FLAG_BOOT_PARAMETERS |
			ZBL6_HANDOFF_FLAG_BOOT_ALLOCATIONS;

	/* Fills the boot device: the partition is named by UUID, not by index. */
	common->boot_drive = 0x80;
	common->root_partition_scheme = partition_scheme;
	common->root_partition_index = ZBL6_PARTITION_INDEX_UNKNOWN;
	common->loader_partition_index = ZBL6_PARTITION_INDEX_UNKNOWN;

	/* Fills the memory map's place, the kernel's extent and the bootstrap CR3. */
	common->memory_range_entry_size = sizeof(struct zbl6_memory_range_v6);
	common->memory_ranges = low_base + MEMORY_RANGES_OFFSET;
	common->kernel_phys_start = plan->physical_start;
	common->kernel_phys_end = plan->physical_end;
	common->bootstrap_cr3 = low_base + LOW_PML4_OFFSET;

	/* Fills the framebuffer. */
	common->framebuffer_base = framebuffer->physical_base;
	common->framebuffer_size = framebuffer->size;
	common->framebuffer_width = framebuffer->width;
	common->framebuffer_height = framebuffer->height;
	common->framebuffer_stride = framebuffer->stride;
	common->framebuffer_format = framebuffer->format;

	/* Fills the boot volume, the parameters and where they came from. */
	handoff->prefix.common.boot_volume_serial = boot_volume_serial;
	byte_copy(&handoff->prefix.parameters, parameters, sizeof(*parameters));
	byte_copy(&handoff->provenance, provenance, sizeof(*provenance));
}

/*
 * Gives the firmware's runtime services their addresses in the kernel's
 * runtime window and describes them in the handoff.
 *
 * It runs after ExitBootServices, on the firmware's identity map, where
 * UEFI allows the one SetVirtualAddressMap call of the boot.  The runtime
 * descriptors are copied to the low block and given consecutive parts of
 * the window in map order.  When they do not fit the window or the copy,
 * or the firmware refuses the map, the handoff carries no runtime services
 * and the boot goes on without them.
 */
static void
give_runtime_addresses(
	const EFI_SYSTEM_TABLE *system,
	const uint8_t *map,
	UINTN map_size,
	UINTN descriptor_size,
	UINT32 descriptor_version,
	uint64_t low_base,
	struct zbl6_handoff_v8_uefi *handoff)
{
	EFI_RUNTIME_SERVICES *runtime;
	const EFI_MEMORY_DESCRIPTOR *descriptor;
	EFI_MEMORY_DESCRIPTOR *copy;
	uint8_t *copies;
	UINTN offset;
	UINTN copied_bytes;
	uint64_t window_used;
	uint32_t count;
	EFI_STATUS status;

	/* Refuses descriptors without the fields the window needs, or larger than the copy. */
	if (descriptor_size < ZBL6_UEFI_MEMORY_DESCRIPTOR_MIN_SIZE ||
	    descriptor_size > ZBL6_UEFI_RUNTIME_MAP_MAX_BYTES ||
	    (descriptor_size & 7U) != 0)
		return;

	/* Refuses a firmware whose runtime services cannot be moved. */
	runtime = system->RuntimeServices;
	if (runtime == 0 || runtime->SetVirtualAddressMap == 0)
		return;

	/* Copies each runtime descriptor and gives it the next free part of the window. */
	copies = (uint8_t *)(uintptr_t)(low_base + RUNTIME_DESCRIPTORS_OFFSET);
	copied_bytes = 0;
	window_used = 0;
	count = 0;
	for (offset = 0;
	     offset + descriptor_size <= map_size;
	     offset += descriptor_size) {
		descriptor = (const void *)(map + offset);

		/* Skips memory the firmware does not use at runtime, and empty ranges. */
		if ((descriptor->Attribute & EFI_MEMORY_RUNTIME) == 0 ||
		    descriptor->NumberOfPages == 0)
			continue;

		/* Leaves the services where they are when the range does not fit what is left of the window. */
		if (descriptor->NumberOfPages > (ZBL6_UEFI_RUNTIME_VIRTUAL_SIZE - window_used) / PAGE_SIZE) {
			debug_port("A64 UEFI RUNTIME WINDOW FULL\n");
			return;
		}

		/* Leaves them too when the copy would leave its place in the low block. */
		if (descriptor_size > ZBL6_UEFI_RUNTIME_MAP_MAX_BYTES - copied_bytes) {
			debug_port("A64 UEFI RUNTIME MAP FULL\n");
			return;
		}

		/* Copies the descriptor and names its place in the window. */
		byte_copy(copies + copied_bytes, descriptor, descriptor_size);
		copy = (void *)(copies + copied_bytes);
		copy->VirtualStart = ZBL6_UEFI_RUNTIME_VIRTUAL_BASE + window_used;
		window_used += descriptor->NumberOfPages * PAGE_SIZE;
		copied_bytes += descriptor_size;
		count++;
	}

	/* A firmware without runtime memory has nothing to move. */
	if (count == 0)
		return;

	/* Moves the runtime services; after a refusal the kernel does not call them. */
	status = runtime->SetVirtualAddressMap(copied_bytes,
					       descriptor_size,
					       descriptor_version,
					       (EFI_MEMORY_DESCRIPTOR *)copies);
	if (status != EFI_SUCCESS) {
		debug_port("A64 UEFI RUNTIME MAP REFUSED\n");
		return;
	}

	/* Describes the moved services; the system table keeps its physical address. */
	handoff->runtime.system_table = (uintptr_t)system;
	handoff->runtime.descriptors = low_base + RUNTIME_DESCRIPTORS_OFFSET;
	handoff->runtime.descriptor_count = count;
	handoff->runtime.descriptor_size = (uint32_t)descriptor_size;
	handoff->runtime.descriptor_version = descriptor_version;

	/* Tells the HAL the runtime part of the handoff is filled. */
	handoff->prefix.common.common.flags |= ZBL6_HANDOFF_FLAG_UEFI_RUNTIME;
}

/*
 * Reads the configured kernel into its physical home and closes the volume.
 *
 * Every failure is reported and stops the machine; on return the plan is
 * placed and the kernel's pages are allocated.
 */
static void
load_kernel(
	struct loader_context *context,
	struct discovered_volume *discovered,
	const struct zbl_uefi_kern_config *configuration,
	struct zbl_elf64_plan *plan,
	EFI_PHYSICAL_ADDRESS *kernel_address,
	UINTN *kernel_pages)
{
	static uint8_t first_sector[ZBL_ELF_HEADER_BYTES];
	static struct boot_file kernel_file;
	const struct zbl_elf64_segment *segment;
	struct kernel_placement_policy placement;
	EFI_FILE_PROTOCOL *kernel;
	EFI_STATUS status;
	UINT64 kernel_file_size;
	unsigned index;
	int kernel_pages_allocated;
	int planned;
	int parsed;

	/* Nothing is open or allocated yet. */
	kernel = 0;
	kernel_pages_allocated = 0;

	/* Opens the kernel on the selected volume or on the root partition. */
	status = boot_file_open(context,
				discovered,
				configuration->kernel_path,
				discovered->root_files.kernel_on_root,
				&kernel_file);
	if (EFI_ERROR(status))
		fail_kernel_load(context, discovered, 0, "Open configured kernel", status, 0U, 0U, 0);
	kernel = kernel_file.fat;

	/* Refuses a kernel file shorter than its headers or larger than the bound. */
	kernel_file_size = kernel_file.size;
	if (kernel_file_size < sizeof(first_sector) || kernel_file_size > MAX_KERNEL_FILE)
		fail_kernel_load(context, discovered, kernel, "Validate kernel file", EFI_BAD_BUFFER_SIZE, 0U, 0U, 0);

	/* Reads the headers. */
	status = boot_file_read(&kernel_file, 0, first_sector, sizeof(first_sector));
	if (EFI_ERROR(status))
		fail_kernel_load(context, discovered, kernel, "Read ELF header", status, 0U, 0U, 0);

	/* Refuses a kernel that is not an ELF64 image the loader can place. */
	planned = zbl_elf64_plan(first_sector, kernel_file_size, plan);
	if (!planned)
		fail_kernel_load(context, discovered, kernel, "Validate ELF64", EFI_LOAD_ERROR, 0U, 0U, 0);

	/* Refuses a malformed kernel_phys= parameter. */
	parsed = kernel_placement_parse(configuration->parameter_record.text,
					configuration->parameter_record.length,
					&placement);
	if (parsed < 0)
		fail_kernel_load(context, discovered, kernel, "Parse kernel_phys", EFI_INVALID_PARAMETER, 0U, 0U, 0);

	/* Allocates the kernel's home and rebases the plan onto it. */
	*kernel_pages = (UINTN)((plan->link_physical_end - plan->link_physical_start + PAGE_SIZE - 1U) / PAGE_SIZE);
	*kernel_address = 0;
	status = place_kernel(context, plan, &placement, kernel_address, *kernel_pages);
	if (EFI_ERROR(status))
		fail_kernel_load(context, discovered, kernel, "Place kernel", status, 0U, 0U, 0);

	/* Clears the home, so every byte past a segment's file part is zero. */
	kernel_pages_allocated = 1;
	byte_zero((void *)(uintptr_t)*kernel_address, *kernel_pages * (UINTN)PAGE_SIZE);

	/* Reads each segment's file part into its place. */
	for (index = 0; index < plan->segment_count; index++) {
		segment = &plan->segment[index];
		status = boot_file_read(&kernel_file, segment->offset, (void *)(uintptr_t)segment->physical, (UINTN)segment->file_size);
		if (EFI_ERROR(status)) {
			fail_kernel_load(context,
					 discovered,
					 kernel,
					 "Read ELF segment",
					 status,
					 *kernel_address,
					 *kernel_pages,
					 kernel_pages_allocated);
		}
	}

	/* Closes the kernel and the volume; nothing more is read from it. */
	status = close_file_pair(kernel, discovered->root);
	kernel = 0;
	discovered->root = 0;
	if (EFI_ERROR(status)) {
		fail_kernel_load(context,
				 discovered,
				 0,
				 "Close kernel volume",
				 status,
				 *kernel_address,
				 *kernel_pages,
				 kernel_pages_allocated);
	}
}

/* Reads blocks of the root partition through the firmware; 0 on success. */
static int
root_block_read(
	void *device_context,
	uint64_t block,
	uint32_t count,
	void *buffer)
{
	struct root_volume *root;
	EFI_STATUS status;

	/* Reads the blocks with the medium the partition was opened on. */
	root = device_context;
	status = root->block->ReadBlocks(root->block, root->media_id, block, (UINTN)count * root->block_size, buffer);
	if (EFI_ERROR(status))
		return 1;

	/* Succeeded: the buffer holds the blocks. */
	return 0;
}

/*
 * Reports why the root partition cannot be read and stops the machine.
 *
 * There is no fallback to a kernel on the FAT volume: the configuration
 * names the root, and a boot from somewhere else would hide the fault.
 */
static void __attribute__((noreturn))
fail_root(
	struct loader_context *context,
	struct discovered_volume *discovered,
	const char *reason)
{
	static char message[80];
	static const char prefix[] = "A64 KERN UFS ";
	UINTN index;

	/* Copies the prefix of the message. */
	index = 0;
	while (prefix[index] != 0) {
		message[index] = prefix[index];
		index++;
	}

	/* Appends the reason, as much of it as fits. */
	while (*reason != 0 && index + 1U < sizeof(message)) {
		message[index] = *reason;
		index++;
		reason++;
	}

	/* Ends the message. */
	message[index] = 0;

	/* Reports it over the logo and stops. */
	fail_discovered(context, discovered, message, EFI_LOAD_ERROR);
}

/*
 * Finds the root partition zedbsd.cfg names and checks its UFS volume.
 *
 * The partition is the one GPT partition of the boot disk whose PARTUUID
 * is rootpart=PARTUUID=.  No other disk may have a partition of the same
 * PARTUUID: the kernel would then refuse to mount the root, so the loader
 * stops first and says why.
 */
static void
open_root_volume(
	struct loader_context *context,
	struct discovered_volume *discovered)
{
	struct zbl_uefi_partition_path path;
	struct zbl_ufs_device device;
	EFI_BOOT_SERVICES *boot;
	EFI_DEVICE_PATH_PROTOCOL *device_path;
	EFI_BLOCK_IO_PROTOCOL *block;
	EFI_BLOCK_IO_MEDIA *media;
	EFI_HANDLE *handles;
	EFI_HANDLE found;
	EFI_STATUS status;
	UINTN handle_count;
	UINTN index;
	UINTN byte;
	enum zbl_uefi_device_path_result path_result;
	enum zbl_ufs_result mounted;
	const char *mount_reason;
	unsigned same_disk_matches;
	unsigned other_disk_matches;
	int same_disk;
	int equal;
	int connect;

	/* Opens the root once; later files reuse it. */
	if (root_partition.mounted)
		return;

	/*
	 * Connects every controller, so that each disk's partitions have block
	 * devices: a firmware that connected only the boot disk would hide a
	 * second disk that carries the same PARTUUID.
	 */
	boot = context->boot;
	handles = 0;
	handle_count = 0;
	status = boot->LocateHandleBuffer(AllHandles, 0, 0, &handle_count, &handles);
	connect = 1;
	if (EFI_ERROR(status))
		connect = 0;
	if (handles == 0)
		connect = 0;
	if (boot->ConnectController == 0)
		connect = 0;
	if (connect) {
		for (index = 0; index < handle_count; index++)
			(void)boot->ConnectController(handles[index], 0, 0, 1);
	}

	/* Releases the list of every handle. */
	if (handles != 0)
		(void)boot->FreePool(handles);

	/* Asks for every block device: the firmware puts no file system on UFS. */
	handles = 0;
	handle_count = 0;
	status = boot->LocateHandleBuffer(ByProtocol, &EFI_BLOCK_IO_PROTOCOL_GUID, 0, &handle_count, &handles);
	if (EFI_ERROR(status) || handles == 0)
		fail_root(context, discovered, "no block devices");

	/* Compares each GPT partition's PARTUUID with the root's. */
	found = 0;
	same_disk_matches = 0;
	other_disk_matches = 0;
	for (index = 0; index < handle_count; index++) {
		/* Reads the handle's device path; one that names no partition is passed over. */
		device_path = 0;
		status = boot->HandleProtocol(handles[index], &EFI_DEVICE_PATH_PROTOCOL_GUID, (void **)&device_path);
		if (EFI_ERROR(status) || device_path == 0)
			continue;
		path_result = zbl_uefi_partition_path_parse(device_path, ZBL_UEFI_DEVICE_PATH_MAX_BYTES, &path);
		if (path_result != ZBL_UEFI_DEVICE_PATH_OK)
			continue;
		if (path.style != ZBL_UEFI_PARTITION_GPT)
			continue;

		/* Compares the HD() node's signature, which is the PARTUUID. */
		equal = 1;
		for (byte = 0; byte < sizeof(discovered->root_files.partuuid); byte++) {
			if (path.bytes[path.partition_offset + 24U + byte] != discovered->root_files.partuuid[byte]) {
				equal = 0;
				break;
			}
		}

		/* Passes over a partition with another PARTUUID. */
		if (!equal)
			continue;

		/* Counts the match on the boot disk apart from one on another disk. */
		same_disk = zbl_uefi_partition_paths_same_disk(&discovered->path, &path);
		if (same_disk) {
			same_disk_matches++;
			found = handles[index];
		} else {
			other_disk_matches++;
		}
	}

	/* Releases the list of block devices. */
	(void)boot->FreePool(handles);

	/* Requires exactly one root on the boot disk and none elsewhere. */
	if (other_disk_matches != 0)
		fail_root(context, discovered, "duplicate PARTUUID");
	if (same_disk_matches == 0)
		fail_root(context, discovered, "root partition not found");
	if (same_disk_matches > 1)
		fail_root(context, discovered, "duplicate PARTUUID");

	/* Takes the partition's block device, which has to hold a medium of usable blocks. */
	block = 0;
	status = boot->HandleProtocol(found, &EFI_BLOCK_IO_PROTOCOL_GUID, (void **)&block);
	if (EFI_ERROR(status))
		fail_root(context, discovered, "no block device");
	if (block == 0)
		fail_root(context, discovered, "no block device");
	if (block->Media == 0)
		fail_root(context, discovered, "no block device");
	if (block->ReadBlocks == 0)
		fail_root(context, discovered, "no block device");
	media = block->Media;
	if (!media->MediaPresent || !media->LogicalPartition)
		fail_root(context, discovered, "no partition medium");
	if (media->BlockSize != 512U && media->BlockSize != 4096U)
		fail_root(context, discovered, "unsupported block size");
	if (media->IoAlign > 4096U)
		fail_root(context, discovered, "unsupported alignment");
	if (media->LastBlock == UINT64_MAX)
		fail_root(context, discovered, "unsupported size");

	/* Checks the UFS volume on it. */
	root_partition.block = block;
	root_partition.media_id = media->MediaId;
	root_partition.block_size = media->BlockSize;
	device.context = &root_partition;
	device.block_size = media->BlockSize;
	device.block_count = media->LastBlock + 1U;
	device.read = root_block_read;
	mounted = zbl_ufs_mount(&root_partition.ufs, &device, &root_work);
	if (mounted != ZBL_UFS_OK) {
		mount_reason = zbl_ufs_result_name(mounted);
		fail_root(context, discovered, mount_reason);
	}

	/* Succeeded: the root partition is open for reads. */
	root_partition.mounted = 1;
	console_ascii(context, "A64 ROOT UFS\n");
}

/*
 * Opens a file the configuration names, on the FAT volume or the root.
 *
 * A path on the root is absolute there; the parser gives the kernel's
 * without its leading slash, which is put back.  A file that cannot be
 * found or read on the root stops the machine with the reason.
 */
static EFI_STATUS
boot_file_open(
	struct loader_context *context,
	struct discovered_volume *discovered,
	const char *path,
	int on_root,
	struct boot_file *file)
{
	static CHAR16 wide[KERNEL_PATH_CHAR16_STORAGE];
	static char absolute[ZBL_KERN_CONFIG_KERNEL_PATH_STORAGE_SIZE + 1U];
	enum zbl_ufs_result looked_up;
	const char *lookup_reason;
	EFI_STATUS status;
	UINTN index;
	UINTN at;
	int converted;

	/* Starts with no file. */
	file->fat = 0;
	file->on_root = on_root;
	file->size = 0;

	/* Opens a file on the FAT volume through the firmware. */
	if (!on_root) {
		converted = kernel_path_utf16(path, wide);
		if (!converted || discovered->root == 0)
			return EFI_INVALID_PARAMETER;

		/* Opens the file the path names. */
		status = discovered->root->Open(discovered->root, &file->fat, wide, EFI_FILE_MODE_READ, 0U);
		if (EFI_ERROR(status))
			return status;
		if (file->fat == 0)
			return EFI_INVALID_PARAMETER;

		/* Takes the file's size, closing a file that has none. */
		status = regular_file_size(file->fat, &file->size);
		if (EFI_ERROR(status)) {
			(void)close_file(file->fat);
			file->fat = 0;
			return status;
		}

		/* Succeeded: the FAT file is open. */
		return EFI_SUCCESS;
	}

	/* Makes the path absolute on the root. */
	at = 0;
	if (path[0] != '/') {
		absolute[at] = '/';
		at++;
	}

	/* Copies the path, as much of it as fits. */
	for (index = 0; path[index] != 0 && at + 1U < sizeof(absolute); index++) {
		absolute[at] = path[index];
		at++;
	}

	/* Ends the path. */
	absolute[at] = 0;

	/* Finds the file on the root partition. */
	open_root_volume(context, discovered);
	looked_up = zbl_ufs_lookup(&root_partition.ufs, absolute, &file->ufs);
	if (looked_up != ZBL_UFS_OK) {
		lookup_reason = zbl_ufs_result_name(looked_up);
		fail_root(context, discovered, lookup_reason);
	}

	/* Succeeded: the file on the root is found. */
	file->size = file->ufs.size;
	return EFI_SUCCESS;
}

/* Reads exactly size bytes of a boot file from a position. */
static EFI_STATUS
boot_file_read(
	struct boot_file *file,
	UINT64 position,
	void *buffer,
	UINTN size)
{
	enum zbl_ufs_result read_result;
	EFI_STATUS status;

	/* Reads a FAT file through the firmware. */
	if (!file->on_root) {
		status = read_exact(file->fat, position, buffer, size);
		if (EFI_ERROR(status))
			return status;
		return EFI_SUCCESS;
	}

	/* Reads a file of the root through the UFS reader. */
	read_result = zbl_ufs_read(&root_partition.ufs, &file->ufs, position, buffer, size);
	if (read_result != ZBL_UFS_OK)
		return EFI_DEVICE_ERROR;

	/* Succeeded: the buffer holds the bytes. */
	return EFI_SUCCESS;
}

/*
 * The runtime descriptors sit above the boot allocations and leave the top
 * of the low block to the transition stack.
 */
_Static_assert(RUNTIME_DESCRIPTORS_OFFSET >= BOOT_ALLOCATIONS_OFFSET + ZBL6_MAX_BOOT_ALLOCATIONS * ZBL6_BOOT_ALLOCATION_SIZE &&
	       RUNTIME_DESCRIPTORS_OFFSET + ZBL6_UEFI_RUNTIME_MAP_MAX_BYTES + 0x2000U <= TRANSITION_STACK_TOP,
	       "UEFI runtime descriptors must fit their low-memory slot");

/* The handoff must fit between its offset and the memory ranges in the low block. */
_Static_assert(ZBL6_HANDOFF_V8_UEFI_SIZE <= MEMORY_RANGES_OFFSET - HANDOFF_OFFSET,
	       "UEFI parameter handoff must fit its low-memory slot");

/* The discovery's partition styles are the handoff's partition schemes. */
_Static_assert(ZBL_UEFI_PARTITION_MBR == ZBL6_PARTITION_SCHEME_MBR,
	       "MBR discovery and handoff values must match");
_Static_assert(ZBL_UEFI_PARTITION_GPT == ZBL6_PARTITION_SCHEME_GPT,
	       "GPT discovery and handoff values must match");
