/* zedBSD configured x64 UEFI loader. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "include/uefi.h"
#include "elf64.h"
#include "framebuffer.h"
#include "memory-map.h"
#include "volume-discovery.h"
#include "zedbsd-config.h"
#include "bootloader/include/amd64-handoff.h"

#define PAGE_SIZE 4096ULL
#define LOW_BLOCK_PAGES 16U
#define LOW_BLOCK_MAX 0x001fffffULL
#define MAX_MEMORY_RANGES 256U
#define MAX_KERNEL_FILE 0x04000000ULL
#define BOOT_SECTOR_BUFFER 4096U
#define CONFIG_READ_BUFFER (ZBL_ZEDBSD_CONFIG_FILE_MAX + 1U)
#define FILE_INFO_BUFFER 1024U
#define KERNEL_PATH_CHAR16_STORAGE \
	(ZBL_ZEDBSD_CONFIG_KERNEL_PATH_STORAGE_SIZE + 1U)

#define LOW_TRAMPOLINE_OFFSET 0x0000U
#define LOW_PML4_OFFSET 0x1000U
#define LOW_PDPT_OFFSET 0x2000U
#define LOW_PD_OFFSET 0x3000U
#define HIGH_PDPT_OFFSET 0x4000U
#define HIGH_PD_OFFSET 0x5000U
#define HANDOFF_OFFSET 0x6000U
#define MEMORY_RANGES_OFFSET 0x7000U
#define FRAMEBUFFER_PD_OFFSET 0x9000U
#define TRANSITION_STACK_TOP 0x10000U

#define PTE_PRESENT 0x001ULL
#define PTE_WRITE 0x002ULL
#define PTE_LARGE 0x080ULL

#define DIAGNOSTIC_BLOCK_WIDTH 24U
#define DIAGNOSTIC_BLOCK_HEIGHT 16U
#define DIAGNOSTIC_BLOCK_GAP 8U
#define DIAGNOSTIC_MARGIN 8U
#define DIAGNOSTIC_PANEL_WIDTH 136U
#define DIAGNOSTIC_PANEL_HEIGHT 40U
#define DIAGNOSTIC_PIXEL 0x00ffffffU

extern uint8_t zbl_transition_start[];
extern uint8_t zbl_transition_end[];

_Static_assert(ZBL6_HANDOFF_V5_UEFI_SIZE <=
	       MEMORY_RANGES_OFFSET - HANDOFF_OFFSET,
	       "UEFI parameter handoff must fit its low-memory slot");

_Static_assert(ZBL_UEFI_PARTITION_MBR == ZBL6_PARTITION_SCHEME_MBR,
	       "MBR discovery and handoff values must match");
_Static_assert(ZBL_UEFI_PARTITION_GPT == ZBL6_PARTITION_SCHEME_GPT,
	       "GPT discovery and handoff values must match");

static uint8_t boot_sector[BOOT_SECTOR_BUFFER]
    __attribute__((aligned(BOOT_SECTOR_BUFFER)));
static uint8_t config_buffer[CONFIG_READ_BUFFER];
static uint8_t file_info_buffer[FILE_INFO_BUFFER]
    __attribute__((aligned(8)));

static const CHAR16 zedbsd_config_path[] = {
	'\\', 'Z', 'E', 'D', 'B', 'S', 'D', '.', 'C', 'F', 'G', 0
};

struct loader_context {
	EFI_HANDLE image;
	EFI_SYSTEM_TABLE *system;
	EFI_BOOT_SERVICES *boot;
};

struct discovered_volume {
	EFI_HANDLE handle;
	EFI_FILE_PROTOCOL *root;
	EFI_FILE_PROTOCOL *config;
	struct zbl_uefi_fat_info fat;
	struct zbl_uefi_partition_path path;
	char uuid[ZBL_UEFI_FAT_UUID_SIZE];
	size_t order;
	size_t match_count;
};

typedef void(EFIAPI *transition_fn)(uint64_t, uint64_t, uint64_t, uint64_t)
    __attribute__((noreturn));

static void __attribute__((noreturn)) halt(void);

static void
byte_zero(void *pointer, UINTN size)
{
	uint8_t *bytes = pointer;
	while (size-- != 0)
		*bytes++ = 0;
}

static void
byte_copy(void *destination, const void *source, UINTN size)
{
	uint8_t *out = destination;
	const uint8_t *in = source;
	while (size-- != 0)
		*out++ = *in++;
}

static int
guid_equal(const EFI_GUID *left, const EFI_GUID *right)
{
	const uint8_t *a = (const void *)left;
	const uint8_t *b = (const void *)right;
	UINTN index;

	for (index = 0; index < sizeof(*left); index++)
		if (a[index] != b[index])
			return 0;
	return 1;
}

static uint64_t
find_acpi_rsdp(const EFI_SYSTEM_TABLE *system)
{
	UINTN index;
	void *acpi10 = 0;

	if (system->ConfigurationTable == 0 ||
	    system->NumberOfTableEntries > 1024U)
		return 0;
	for (index = 0; index < system->NumberOfTableEntries; index++) {
		const EFI_CONFIGURATION_TABLE *entry =
		    &system->ConfigurationTable[index];
		if (guid_equal(&entry->VendorGuid, &EFI_ACPI_20_TABLE_GUID))
			return (uint64_t)(uintptr_t)entry->VendorTable;
		if (guid_equal(&entry->VendorGuid, &EFI_ACPI_TABLE_GUID))
			acpi10 = entry->VendorTable;
	}
	return (uint64_t)(uintptr_t)acpi10;
}

static void
debug_byte(uint8_t byte)
{
	__asm__ volatile("outb %0,%1" : : "a"(byte), "Nd"((uint16_t)0xe9));
}

static void
debug_port(const char *string)
{
	while (*string != 0)
		debug_byte((uint8_t)*string++);
}

static void
console_ascii(struct loader_context *context, const char *string)
{
	CHAR16 buffer[128];
	UINTN used = 0;

	debug_port(string);
	if (context->system->ConOut == 0 ||
	    context->system->ConOut->OutputString == 0)
		return;
	while (*string != 0) {
		if (*string == '\n' && used + 2U < 128U)
			buffer[used++] = '\r';
		buffer[used++] = (uint8_t)*string++;
		if (used == 126U) {
			buffer[used] = 0;
			context->system->ConOut->OutputString(
			    context->system->ConOut, buffer);
			used = 0;
		}
	}
	buffer[used] = 0;
	context->system->ConOut->OutputString(context->system->ConOut, buffer);
}

static void
console_hex64(struct loader_context *context, const char *label, uint64_t value)
{
	static const char digits[] = "0123456789abcdef";
	char message[64];
	UINTN used = 0;
	int shift;

	while (*label != 0 && used + 1U < sizeof(message))
		message[used++] = *label++;
	if (used + 20U >= sizeof(message))
		halt();
	message[used++] = '0';
	message[used++] = 'x';
	for (shift = 60; shift >= 0; shift -= 4)
		message[used++] = digits[(value >> shift) & 15U];
	message[used++] = '\n';
	message[used] = 0;
	console_ascii(context, message);
}

/*
 * Leave a firmware-independent, photographable progress code in the GOP
 * framebuffer.  Stage one clears the panel and every stage adds one white
 * block.  Later code duplicates this exact geometry after the CR3 switch and
 * at the first kernel instruction.
 */
static void
framebuffer_stage(const struct zbl6_framebuffer *framebuffer, unsigned stage)
{
	volatile uint32_t *pixels =
	    (volatile uint32_t *)(uintptr_t)framebuffer->physical_base;
	unsigned block, origin_x, x, y;

	if (stage == 0 || stage > 4U ||
	    framebuffer->width < DIAGNOSTIC_PANEL_WIDTH ||
	    framebuffer->height < DIAGNOSTIC_PANEL_HEIGHT)
		halt();
	origin_x = framebuffer->width - DIAGNOSTIC_PANEL_WIDTH;
	if (stage == 1U)
		for (y = 0; y < DIAGNOSTIC_PANEL_HEIGHT; y++)
			for (x = 0; x < DIAGNOSTIC_PANEL_WIDTH; x++)
				pixels[(uint64_t)y * framebuffer->stride +
				       origin_x + x] = 0;
	block = stage - 1U;
	for (y = 0; y < DIAGNOSTIC_BLOCK_HEIGHT; y++)
		for (x = 0; x < DIAGNOSTIC_BLOCK_WIDTH; x++)
			pixels[(uint64_t)(y + DIAGNOSTIC_MARGIN) *
				   framebuffer->stride +
			       origin_x + DIAGNOSTIC_MARGIN +
			       block * (DIAGNOSTIC_BLOCK_WIDTH +
					DIAGNOSTIC_BLOCK_GAP) +
			       x] = DIAGNOSTIC_PIXEL;
	__asm__ volatile("sfence" : : : "memory");
}

static void
framebuffer_map_error(const struct zbl6_framebuffer *framebuffer,
		      enum zbl_uefi_map_result result)
{
	volatile uint32_t *pixels =
	    (volatile uint32_t *)(uintptr_t)framebuffer->physical_base;
	unsigned block, origin_x, x, y;

	if (result <= ZBL_UEFI_MAP_OK || result > ZBL_UEFI_MAP_EMPTY)
		result = ZBL_UEFI_MAP_INVALID_ARGUMENT;
	origin_x = framebuffer->width - DIAGNOSTIC_PANEL_WIDTH;
	for (block = 0; block < (unsigned)result; block++)
		for (y = 0; y < 6U; y++)
			for (x = 0; x < 8U; x++)
				pixels[(uint64_t)(y + 30U) * framebuffer->stride +
				       origin_x + DIAGNOSTIC_MARGIN +
				       block * 12U + x] =
				    DIAGNOSTIC_PIXEL;
	__asm__ volatile("sfence" : : : "memory");
}

static void __attribute__((noreturn))
halt(void)
{
	for (;;) {
		__asm__ volatile("cli; hlt");
	}
}

static void
console_status(struct loader_context *context, const char *operation,
	EFI_STATUS status)
{
	char message[96];
	static const char digits[] = "0123456789abcdef";
	UINTN index = 0;
	int shift;

	while (*operation != 0 && index + 1U < sizeof(message))
		message[index++] = *operation++;
	if (index + 20U < sizeof(message)) {
		message[index++] = ':';
		message[index++] = ' ';
		message[index++] = '0';
		message[index++] = 'x';
		for (shift = 60; shift >= 0; shift -= 4)
			message[index++] = digits[(status >> shift) & 15U];
		message[index++] = '\n';
	}
	message[index] = 0;
	console_ascii(context, message);
}

static void __attribute__((noreturn))
fail_status(struct loader_context *context, const char *operation,
	    EFI_STATUS status)
{
	console_status(context, operation, status);
	halt();
}

static EFI_STATUS
read_exact(EFI_FILE_PROTOCOL *file, UINT64 position, void *buffer, UINTN size)
{
	EFI_STATUS status;
	uint8_t *cursor = buffer;

	status = file->SetPosition(file, position);
	if (EFI_ERROR(status))
		return status;
	while (size != 0) {
		UINTN amount = size;
		status = file->Read(file, &amount, cursor);
		if (EFI_ERROR(status))
			return status;
		if (amount == 0 || amount > size)
			return EFI_LOAD_ERROR;
		cursor += amount;
		size -= amount;
	}
	return EFI_SUCCESS;
}

static EFI_STATUS
close_file_pair(EFI_FILE_PROTOCOL *file, EFI_FILE_PROTOCOL *root)
{
	EFI_STATUS first = EFI_SUCCESS;
	EFI_STATUS status;

	if (file != 0) {
		status = file->Close == 0 ? EFI_INVALID_PARAMETER :
		    file->Close(file);
		if (EFI_ERROR(status))
			first = status;
	}
	if (root != 0) {
		status = root->Close == 0 ? EFI_INVALID_PARAMETER :
		    root->Close(root);
		if (!EFI_ERROR(first) && EFI_ERROR(status))
			first = status;
	}
	return first;
}

static EFI_STATUS
release_selection(struct zbl_uefi_volume_selection *selection)
{
	struct zbl_uefi_volume_match match;

	if (!zbl_uefi_volume_selection_take(selection, &match))
		return EFI_SUCCESS;
	return close_file_pair((EFI_FILE_PROTOCOL *)match.config,
	    (EFI_FILE_PROTOCOL *)match.root);
}

static EFI_STATUS
discovery_abort(struct zbl_uefi_volume_selection *selection,
	EFI_STATUS status)
{
	EFI_STATUS cleanup = release_selection(selection);

	return EFI_ERROR(cleanup) ? cleanup : status;
}

static EFI_STATUS
regular_file_size(EFI_FILE_PROTOCOL *file, UINT64 *file_size)
{
	EFI_FILE_INFO *info = (EFI_FILE_INFO *)(void *)file_info_buffer;
	const UINTN minimum = __builtin_offsetof(EFI_FILE_INFO, FileName) +
	    sizeof(CHAR16);
	EFI_STATUS status;
	UINTN required = 0U;

	if (file == 0 || file->GetInfo == 0 || file_size == 0)
		return EFI_INVALID_PARAMETER;
	status = file->GetInfo(file, &EFI_FILE_INFO_ID, &required, 0);
	if (status != EFI_BUFFER_TOO_SMALL || required < minimum)
		return EFI_ERROR(status) ? status : EFI_DEVICE_ERROR;
	if (required > sizeof(file_info_buffer))
		return EFI_BAD_BUFFER_SIZE;
	byte_zero(file_info_buffer, required);
	status = file->GetInfo(file, &EFI_FILE_INFO_ID, &required,
	    file_info_buffer);
	if (EFI_ERROR(status))
		return status;
	if (required < minimum || info->Size < minimum ||
	    info->Size > required)
		return EFI_DEVICE_ERROR;
	if ((info->Attribute & EFI_FILE_DIRECTORY) != 0U)
		return EFI_UNSUPPORTED;
	*file_size = info->FileSize;
	return EFI_SUCCESS;
}

static EFI_STATUS
probe_readable_file(EFI_FILE_PROTOCOL *file)
{
	EFI_STATUS status;
	UINTN amount = 1U;
	UINT64 file_size;
	uint8_t byte;

	if (file == 0 || file->Read == 0 || file->SetPosition == 0 ||
	    file->Close == 0)
		return EFI_INVALID_PARAMETER;
	status = regular_file_size(file, &file_size);
	if (EFI_ERROR(status))
		return status;
	(void)file_size;
	status = file->SetPosition(file, 0U);
	if (EFI_ERROR(status))
		return status;
	status = file->Read(file, &amount, &byte);
	if (EFI_ERROR(status))
		return status;
	if (amount > 1U)
		return EFI_DEVICE_ERROR;
	return file->SetPosition(file, 0U);
}

static EFI_STATUS
read_bounded_file(EFI_FILE_PROTOCOL *file, void *buffer, UINTN capacity,
	UINTN *size)
{
	EFI_STATUS status;
	uint8_t *bytes = buffer;
	UINTN used = 0U;

	if (file == 0 || file->Read == 0 || file->SetPosition == 0 ||
	    buffer == 0 || capacity == 0U || size == 0)
		return EFI_INVALID_PARAMETER;
	*size = 0U;
	status = file->SetPosition(file, 0U);
	if (EFI_ERROR(status))
		return status;
	while (used < capacity) {
		UINTN amount = capacity - used;

		status = file->Read(file, &amount, bytes + used);
		if (EFI_ERROR(status))
			return status;
		if (amount > capacity - used)
			return EFI_DEVICE_ERROR;
		if (amount == 0U)
			break;
		used += amount;
	}
	*size = used;
	return EFI_SUCCESS;
}

static int
kernel_path_utf16(const char *source,
	CHAR16 output[KERNEL_PATH_CHAR16_STORAGE])
{
	UINTN length = 0U;

	if (source == 0 || output == 0)
		return 0;
	output[0] = '\\';
	while (source[length] != 0) {
		if (length >= ZBL_ZEDBSD_CONFIG_KERNEL_PATH_MAX)
			return 0;
		output[length + 1U] = source[length] == '/' ? '\\' :
		    (uint8_t)source[length];
		length++;
	}
	if (length == 0U)
		return 0;
	output[length + 1U] = 0;
	return 1;
}

static void
console_candidate_result(struct loader_context *context, const char *label,
	UINTN order, const char *result)
{
	console_hex64(context, label, order);
	console_ascii(context, "A64 CFG DETAIL ");
	console_ascii(context, result);
	console_ascii(context, "\n");
}

static EFI_STATUS
discover_config_volume(struct loader_context *context,
	const EFI_LOADED_IMAGE_PROTOCOL *loaded,
	struct discovered_volume *discovered)
{
	EFI_BOOT_SERVICES *boot = context->boot;
	EFI_DEVICE_PATH_PROTOCOL *loaded_device_path = 0;
	struct zbl_uefi_partition_path loaded_path;
	struct zbl_uefi_partition_path selected_path;
	struct zbl_uefi_volume_selection selection;
	struct zbl_uefi_volume_match selected;
	EFI_HANDLE *firmware_handles = 0;
	EFI_HANDLE ordered_handles[ZBL_UEFI_VOLUME_MAX_HANDLES];
	UINTN firmware_handle_count = 0U;
	size_t ordered_count = 0U;
	EFI_STATUS status;
	EFI_STATUS close_status;
	enum zbl_uefi_device_path_result path_result;
	enum zbl_uefi_handle_order_result order_result;
	enum zbl_uefi_volume_selection_result selection_result;

	if (loaded == 0 || discovered == 0 || loaded->DeviceHandle == 0 ||
	    boot->HandleProtocol == 0 || boot->LocateHandleBuffer == 0 ||
	    boot->FreePool == 0)
		return EFI_INVALID_PARAMETER;
	byte_zero(discovered, sizeof(*discovered));
	byte_zero(&selected_path, sizeof(selected_path));
	zbl_uefi_volume_selection_init(&selection);
	status = boot->HandleProtocol(loaded->DeviceHandle,
	    &EFI_DEVICE_PATH_PROTOCOL_GUID, (void **)&loaded_device_path);
	if (EFI_ERROR(status))
		return status;
	path_result = zbl_uefi_partition_path_parse(loaded_device_path,
	    ZBL_UEFI_DEVICE_PATH_MAX_BYTES, &loaded_path);
	if (path_result != ZBL_UEFI_DEVICE_PATH_OK) {
		console_ascii(context, "A64 loaded device path rejected: ");
		console_ascii(context,
		    zbl_uefi_device_path_result_name(path_result));
		console_ascii(context, "\n");
		return EFI_UNSUPPORTED;
	}

	status = boot->LocateHandleBuffer(ByProtocol,
	    &EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID, 0,
	    &firmware_handle_count, &firmware_handles);
	if (status == EFI_NOT_FOUND) {
		if (firmware_handles != 0) {
			close_status = boot->FreePool(firmware_handles);
			firmware_handles = 0;
			if (EFI_ERROR(close_status))
				return close_status;
		}
		firmware_handle_count = 0U;
		status = EFI_SUCCESS;
	}
	if (EFI_ERROR(status)) {
		if (firmware_handles != 0) {
			close_status = boot->FreePool(firmware_handles);
			firmware_handles = 0;
			if (EFI_ERROR(close_status))
				return close_status;
		}
		return status;
	}
	if (firmware_handle_count != 0U && firmware_handles == 0)
		return EFI_DEVICE_ERROR;
	order_result = zbl_uefi_volume_order_handles(loaded->DeviceHandle,
	    (void *const *)firmware_handles, firmware_handle_count,
	    ordered_handles, ZBL_UEFI_VOLUME_MAX_HANDLES, &ordered_count);
	if (firmware_handles != 0) {
		status = boot->FreePool(firmware_handles);
		firmware_handles = 0;
		if (EFI_ERROR(status))
			return status;
	}
	if (order_result != ZBL_UEFI_HANDLE_ORDER_OK)
		return order_result == ZBL_UEFI_HANDLE_ORDER_INVALID_ARGUMENT ?
		    EFI_DEVICE_ERROR : EFI_BAD_BUFFER_SIZE;

	for (size_t order = 0U; order < ordered_count; order++) {
		EFI_DEVICE_PATH_PROTOCOL *device_path = 0;
		EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *filesystem = 0;
		EFI_BLOCK_IO_PROTOCOL *block = 0;
		EFI_BLOCK_IO_MEDIA *media;
		EFI_FILE_PROTOCOL *root = 0;
		EFI_FILE_PROTOCOL *config = 0;
		struct zbl_uefi_partition_path path;
		struct zbl_uefi_fat_info fat;
		enum zbl_uefi_fat_result fat_result;
		enum zbl_uefi_volume_match_action action;

		status = boot->HandleProtocol(ordered_handles[order],
		    &EFI_DEVICE_PATH_PROTOCOL_GUID, (void **)&device_path);
		if (EFI_ERROR(status)) {
			console_status(context, "A64 CFG DevicePath", status);
			continue;
		}
		path_result = zbl_uefi_partition_path_parse(device_path,
		    ZBL_UEFI_DEVICE_PATH_MAX_BYTES, &path);
		if (path_result != ZBL_UEFI_DEVICE_PATH_OK) {
			console_candidate_result(context, "A64 CFG PATH ", order,
			    zbl_uefi_device_path_result_name(path_result));
			continue;
		}
		if (!zbl_uefi_partition_paths_same_disk(&loaded_path, &path)) {
			console_hex64(context, "A64 CFG OTHER DISK ", order);
			continue;
		}
		status = boot->HandleProtocol(ordered_handles[order],
		    &EFI_BLOCK_IO_PROTOCOL_GUID, (void **)&block);
		if (EFI_ERROR(status)) {
			console_status(context, "A64 CFG BlockIO", status);
			continue;
		}
		if (block == 0 || block->Media == 0 || block->ReadBlocks == 0) {
			console_hex64(context, "A64 CFG INVALID BLOCK ", order);
			continue;
		}
		media = block->Media;
		if (!media->MediaPresent) {
			console_hex64(context, "A64 CFG NO MEDIA ", order);
			continue;
		}
		if (!media->LogicalPartition) {
			console_hex64(context, "A64 CFG NOT PARTITION ", order);
			continue;
		}
		if (media->BlockSize == 0U ||
		    media->BlockSize > sizeof(boot_sector) ||
		    media->LastBlock == UINT64_MAX ||
		    (media->IoAlign > 1U &&
		     ((media->IoAlign & (media->IoAlign - 1U)) != 0U ||
		      media->IoAlign > BOOT_SECTOR_BUFFER ||
		      ((uintptr_t)boot_sector & (media->IoAlign - 1U)) != 0U))) {
			console_hex64(context, "A64 CFG BLOCK BOUNDS ", order);
			continue;
		}
		status = block->ReadBlocks(block, media->MediaId, 0U,
		    media->BlockSize, boot_sector);
		if (EFI_ERROR(status)) {
			console_status(context,
			    status == EFI_MEDIA_CHANGED ?
			    "A64 CFG media changed" : "A64 CFG read BPB",
			    status);
			continue;
		}
		fat_result = zbl_uefi_fat_bpb_parse(boot_sector,
		    media->BlockSize, media->BlockSize, media->LastBlock + 1U,
		    &fat);
		if (fat_result != ZBL_UEFI_FAT_OK) {
			console_candidate_result(context, "A64 CFG FAT ", order,
			    zbl_uefi_fat_result_name(fat_result));
			continue;
		}
		status = boot->HandleProtocol(ordered_handles[order],
		    &EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID,
		    (void **)&filesystem);
		if (EFI_ERROR(status) || filesystem == 0 ||
		    filesystem->OpenVolume == 0) {
			console_status(context, "A64 CFG SimpleFS",
			    EFI_ERROR(status) ? status : EFI_INVALID_PARAMETER);
			continue;
		}
		status = filesystem->OpenVolume(filesystem, &root);
		if (EFI_ERROR(status) || root == 0 || root->Open == 0 ||
		    root->Close == 0) {
			close_status = close_file_pair(0, root);
			if (EFI_ERROR(close_status))
				return discovery_abort(&selection, close_status);
			console_status(context, "A64 CFG OpenVolume",
			    EFI_ERROR(status) ? status : EFI_INVALID_PARAMETER);
			continue;
		}
		status = root->Open(root, &config, zedbsd_config_path,
		    EFI_FILE_MODE_READ, 0U);
		if (EFI_ERROR(status) || config == 0) {
			close_status = close_file_pair(config, root);
			if (EFI_ERROR(close_status))
				return discovery_abort(&selection, close_status);
			if (status == EFI_NOT_FOUND)
				console_hex64(context, "A64 CFG MISSING ", order);
			else
				console_status(context, "A64 CFG open marker",
				    EFI_ERROR(status) ? status :
				    EFI_INVALID_PARAMETER);
			continue;
		}
		status = probe_readable_file(config);
		if (EFI_ERROR(status)) {
			close_status = close_file_pair(config, root);
			if (EFI_ERROR(close_status))
				return discovery_abort(&selection, close_status);
			console_status(context, "A64 CFG unreadable", status);
			continue;
		}
		action = zbl_uefi_volume_selection_record_match(&selection,
		    ordered_handles[order], order, &fat, root, config);
		if (action == ZBL_UEFI_VOLUME_MATCH_INVALID) {
			close_status = close_file_pair(config, root);
			return discovery_abort(&selection,
			    EFI_ERROR(close_status) ? close_status : EFI_LOAD_ERROR);
		}
		console_hex64(context, "A64 CFG MATCH ", order);
		if (action == ZBL_UEFI_VOLUME_MATCH_KEEP) {
			selected_path = path;
		} else {
			close_status = close_file_pair(config, root);
			if (EFI_ERROR(close_status))
				return discovery_abort(&selection, close_status);
		}
	}

	selection_result = zbl_uefi_volume_selection_finish(&selection);
	if (selection_result == ZBL_UEFI_VOLUME_SELECTION_NOT_FOUND) {
		console_ascii(context,
		    "zedbsd.cfg was not found on the boot disk\n");
		return EFI_NOT_FOUND;
	}
	if (selection_result == ZBL_UEFI_VOLUME_SELECTION_INVALID_ARGUMENT)
		return discovery_abort(&selection, EFI_LOAD_ERROR);
	if (selection_result == ZBL_UEFI_VOLUME_SELECTION_MULTIPLE) {
		console_ascii(context,
		    "WARNING: multiple zedbsd.cfg files on the boot disk; "
		    "using the first\n");
		console_hex64(context, "A64 CFG MATCHES ",
		    selection.match_count);
	}
	discovered->match_count = selection.match_count;
	if (!zbl_uefi_volume_selection_take(&selection, &selected))
		return discovery_abort(&selection, EFI_LOAD_ERROR);
	discovered->handle = selected.handle;
	discovered->root = selected.root;
	discovered->config = selected.config;
	discovered->fat = selected.fat;
	discovered->path = selected_path;
	discovered->order = selected.order;
	for (size_t index = 0U; index < sizeof(discovered->uuid); index++)
		discovered->uuid[index] = selected.uuid[index];
	console_hex64(context, "A64 CFG SELECTED ", discovered->order);
	console_ascii(context, "A64 CFG UUID ");
	console_ascii(context, discovered->uuid);
	console_ascii(context, "\n");
	return EFI_SUCCESS;
}

static EFI_STATUS
close_discovered_volume(struct discovered_volume *discovered)
{
	EFI_STATUS status;

	if (discovered == 0)
		return EFI_INVALID_PARAMETER;
	status = close_file_pair(discovered->config, discovered->root);
	discovered->config = 0;
	discovered->root = 0;
	return status;
}

static void __attribute__((noreturn))
fail_discovered(struct loader_context *context,
	struct discovered_volume *discovered, const char *operation,
	EFI_STATUS status)
{
	EFI_STATUS cleanup = close_discovered_volume(discovered);

	if (EFI_ERROR(cleanup))
		console_status(context, "Close selected volume", cleanup);
	fail_status(context, operation, status);
}

static EFI_STATUS
load_selected_configuration(struct loader_context *context,
	struct discovered_volume *discovered,
	struct zbl_uefi_zedbsd_config *configuration)
{
	EFI_STATUS read_status;
	EFI_STATUS close_status;
	UINTN config_size;
	enum zbl_uefi_zedbsd_config_result config_result;

	if (discovered == 0 || discovered->config == 0 || configuration == 0)
		return EFI_INVALID_PARAMETER;
	read_status = read_bounded_file(discovered->config, config_buffer,
	    sizeof(config_buffer), &config_size);
	close_status = close_file_pair(discovered->config, 0);
	discovered->config = 0;
	if (EFI_ERROR(read_status)) {
		if (EFI_ERROR(close_status))
			console_status(context, "Close zedbsd.cfg", close_status);
		return read_status;
	}
	if (EFI_ERROR(close_status))
		return close_status;
	config_result = zbl_uefi_zedbsd_config_parse(configuration,
	    config_buffer, config_size, discovered->uuid,
	    sizeof(discovered->uuid));
	if (config_result != ZBL_UEFI_ZEDBSD_CONFIG_OK) {
		console_ascii(context, "zedbsd.cfg rejected: ");
		console_ascii(context,
		    zbl_uefi_zedbsd_config_result_name(config_result));
		console_ascii(context, "\n");
		return EFI_LOAD_ERROR;
	}
	console_ascii(context, "A64 KERNEL ");
	console_ascii(context, configuration->kernel_path);
	console_ascii(context, "\n");
	console_ascii(context, "A64 PARAMS ");
	console_ascii(context, configuration->parameter_record.text);
	console_ascii(context, "\n");
	return EFI_SUCCESS;
}

static void __attribute__((noreturn))
fail_kernel_load(struct loader_context *context,
	struct discovered_volume *discovered, EFI_FILE_PROTOCOL *kernel,
	const char *operation, EFI_STATUS status,
	EFI_PHYSICAL_ADDRESS kernel_address, UINTN kernel_pages,
	int pages_allocated)
{
	EFI_STATUS cleanup;

	cleanup = close_file_pair(kernel, discovered->root);
	discovered->root = 0;
	discovered->config = 0;
	if (EFI_ERROR(cleanup))
		console_status(context, "Close kernel volume", cleanup);
	if (pages_allocated) {
		cleanup = context->boot->FreePages == 0 ? EFI_INVALID_PARAMETER :
		    context->boot->FreePages(kernel_address, kernel_pages);
		if (EFI_ERROR(cleanup))
			console_status(context, "Free kernel pages", cleanup);
	}
	fail_status(context, operation, status);
}

static void __attribute__((noreturn))
fail_boot_allocations(struct loader_context *context, const char *operation,
	EFI_STATUS status, void *pool, int pool_allocated,
	EFI_PHYSICAL_ADDRESS low_address, UINTN low_pages,
	int low_pages_allocated, EFI_PHYSICAL_ADDRESS kernel_address,
	UINTN kernel_pages)
{
	EFI_STATUS cleanup;

	if (pool_allocated) {
		cleanup = context->boot->FreePool == 0 ? EFI_INVALID_PARAMETER :
		    context->boot->FreePool(pool);
		if (EFI_ERROR(cleanup))
			console_status(context, "Free loader pool", cleanup);
	}
	if (low_pages_allocated) {
		cleanup = context->boot->FreePages == 0 ? EFI_INVALID_PARAMETER :
		    context->boot->FreePages(low_address, low_pages);
		if (EFI_ERROR(cleanup))
			console_status(context, "Free bootstrap pages", cleanup);
	}
	cleanup = context->boot->FreePages == 0 ? EFI_INVALID_PARAMETER :
	    context->boot->FreePages(kernel_address, kernel_pages);
	if (EFI_ERROR(cleanup))
		console_status(context, "Free kernel pages", cleanup);
	fail_status(context, operation, status);
}

static int
framebuffer_from_gop(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
		     struct zbl6_framebuffer *framebuffer,
		     struct zbl_uefi_framebuffer_mapping *mapping)
{
	EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *mode;
	EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;

	if (gop == 0 || gop->Mode == 0 || gop->Mode->Info == 0 ||
	    mapping == 0)
		return 0;
	mode = gop->Mode;
	if (mode->SizeOfInfo < sizeof(*info))
		return 0;
	info = mode->Info;
	if (info->HorizontalResolution == 0 || info->VerticalResolution == 0 ||
	    info->PixelsPerScanLine < info->HorizontalResolution ||
	    (info->PixelFormat != PixelRedGreenBlueReserved8BitPerColor &&
	     info->PixelFormat != PixelBlueGreenRedReserved8BitPerColor))
		return 0;
	if (!zbl_uefi_framebuffer_mapping_plan(mode->FrameBufferBase,
	    mode->FrameBufferSize, info->HorizontalResolution,
	    info->VerticalResolution, info->PixelsPerScanLine, 4U, mapping))
		return 0;
	framebuffer->physical_base = mode->FrameBufferBase;
	framebuffer->size = mode->FrameBufferSize;
	framebuffer->width = info->HorizontalResolution;
	framebuffer->height = info->VerticalResolution;
	framebuffer->stride = info->PixelsPerScanLine;
	framebuffer->format =
	    info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor
		? ZBL6_FRAMEBUFFER_RGBX8888
		: ZBL6_FRAMEBUFFER_BGRX8888;
	return 1;
}

static void
build_bootstrap(uint64_t low_base, const struct zbl_elf64_plan *plan,
		const struct zbl6_framebuffer *framebuffer,
		const struct zbl_uefi_framebuffer_mapping *framebuffer_mapping,
		uint32_t boot_volume_serial, uint8_t partition_scheme,
		const struct zedbsd_boot_parameter_record *parameters)
{
	uint8_t *low = (void *)(uintptr_t)low_base;
	uint64_t *pml4 = (void *)(low + LOW_PML4_OFFSET);
	uint64_t *low_pdpt = (void *)(low + LOW_PDPT_OFFSET);
	uint64_t *low_pd = (void *)(low + LOW_PD_OFFSET);
	uint64_t *high_pdpt = (void *)(low + HIGH_PDPT_OFFSET);
	uint64_t *high_pd = (void *)(low + HIGH_PD_OFFSET);
	uint64_t *framebuffer_pd = (void *)(low + FRAMEBUFFER_PD_OFFSET);
	struct zbl6_handoff_v5_uefi *handoff =
	    (void *)(low + HANDOFF_OFFSET);
	struct zbl6_handoff_v3 *common = &handoff->common.common;
	UINTN transition_size =
	    (UINTN)(zbl_transition_end - zbl_transition_start);
	unsigned index;
	uint64_t framebuffer_aligned;
	unsigned framebuffer_pages;

	byte_zero(low, LOW_BLOCK_PAGES * PAGE_SIZE);
	if (transition_size == 0 || transition_size > PAGE_SIZE)
		halt();
	if (partition_scheme != ZBL6_PARTITION_SCHEME_MBR &&
	    partition_scheme != ZBL6_PARTITION_SCHEME_GPT)
		halt();
	byte_copy(low + LOW_TRAMPOLINE_OFFSET, zbl_transition_start,
		  transition_size);
	pml4[0] = (low_base + LOW_PDPT_OFFSET) | PTE_PRESENT | PTE_WRITE;
	low_pdpt[0] = (low_base + LOW_PD_OFFSET) | PTE_PRESENT | PTE_WRITE;
	pml4[511] = (low_base + HIGH_PDPT_OFFSET) | PTE_PRESENT | PTE_WRITE;
	high_pdpt[510] = (low_base + HIGH_PD_OFFSET) | PTE_PRESENT | PTE_WRITE;
	for (index = 0; index < 512; index++) {
		uint64_t entry = (uint64_t)index * 0x200000ULL | PTE_PRESENT |
				 PTE_WRITE | PTE_LARGE;
		low_pd[index] = entry;
		high_pd[index] = entry;
	}
	if (framebuffer_mapping == 0)
		halt();
	framebuffer_aligned = framebuffer_mapping->aligned_base;
	framebuffer_pages = framebuffer_mapping->large_page_count;
	high_pdpt[511] =
	    (low_base + FRAMEBUFFER_PD_OFFSET) | PTE_PRESENT | PTE_WRITE;
	for (index = 0; index < framebuffer_pages; index++)
		framebuffer_pd[16U + index] =
		    (framebuffer_aligned + (uint64_t)index * 0x200000ULL) |
		    PTE_PRESENT | PTE_WRITE | PTE_LARGE;
	common->magic = ZBL6_HANDOFF_MAGIC;
	common->version = ZBL6_HANDOFF_V5_VERSION;
	common->size = sizeof(*handoff);
	common->flags = ZBL6_HANDOFF_FLAG_UEFI |
			ZBL6_HANDOFF_FLAG_MEMORY_MAP |
			ZBL6_HANDOFF_FLAG_FRAMEBUFFER |
			ZBL6_HANDOFF_FLAG_BOOT_UUID |
			ZBL6_HANDOFF_FLAG_BOOT_PARAMETERS;
	common->boot_drive = 0x80;
	common->root_partition_scheme = partition_scheme;
	common->root_partition_index = ZBL6_PARTITION_INDEX_UNKNOWN;
	common->loader_partition_index = ZBL6_PARTITION_INDEX_UNKNOWN;
	common->memory_range_entry_size =
	    sizeof(struct zbl6_memory_range);
	common->memory_ranges = low_base + MEMORY_RANGES_OFFSET;
	common->kernel_phys_start = plan->physical_start;
	common->kernel_phys_end = plan->physical_end;
	common->bootstrap_cr3 = low_base + LOW_PML4_OFFSET;
	common->framebuffer_base = framebuffer->physical_base;
	common->framebuffer_size = framebuffer->size;
	common->framebuffer_width = framebuffer->width;
	common->framebuffer_height = framebuffer->height;
	common->framebuffer_stride = framebuffer->stride;
	common->framebuffer_format = framebuffer->format;
	handoff->common.boot_volume_serial = boot_volume_serial;
	byte_copy(&handoff->parameters, parameters, sizeof(*parameters));
}

EFI_STATUS EFIAPI
efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *system)
{
	struct loader_context context;
	EFI_LOADED_IMAGE_PROTOCOL *loaded;
	EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
	EFI_FILE_PROTOCOL *kernel = 0;
	EFI_BOOT_SERVICES *boot;
	EFI_STATUS status;
	static uint8_t first_sector[ZBL_ELF_HEADER_BYTES];
	static struct discovered_volume discovered;
	static struct zbl_uefi_zedbsd_config configuration;
	static CHAR16 kernel_file_path[KERNEL_PATH_CHAR16_STORAGE];
	struct zbl_elf64_plan plan;
	EFI_PHYSICAL_ADDRESS kernel_address, low_address;
	UINTN kernel_pages, map_size, map_capacity, map_key;
	UINTN descriptor_size;
	UINT32 descriptor_version;
	UINT64 kernel_file_size;
	EFI_MEMORY_DESCRIPTOR *map = 0;
	struct zbl6_handoff_v5_uefi *handoff;
	struct zbl6_handoff_v3 *handoff_common;
	struct zbl6_framebuffer framebuffer;
	struct zbl_uefi_framebuffer_mapping framebuffer_mapping;
	struct zbl6_memory_range *ranges;
	uint32_t range_count;
	enum zbl_uefi_map_result map_result;
	int kernel_pages_allocated = 0;
	int low_pages_allocated = 0;
	int map_allocated = 0;
	unsigned index, attempt;

	if (system == 0 || system->BootServices == 0)
		return EFI_INVALID_PARAMETER;
	context.image = image;
	context.system = system;
	context.boot = system->BootServices;
	boot = context.boot;
	console_ascii(&context, "A64 UEFI ENTRY\n");
	status = boot->LocateProtocol(&EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID, 0,
				      (void **)&gop);
	if (EFI_ERROR(status))
		fail_status(&context, "Locate GOP", status);
	if (!framebuffer_from_gop(gop, &framebuffer, &framebuffer_mapping))
		fail_status(&context, "Validate GOP", EFI_UNSUPPORTED);

	status = boot->HandleProtocol(image, &EFI_LOADED_IMAGE_PROTOCOL_GUID,
				      (void **)&loaded);
	if (EFI_ERROR(status))
		fail_status(&context, "LoadedImage", status);
	if (loaded == 0 || loaded->DeviceHandle == 0)
		fail_status(&context, "Validate LoadedImage", EFI_INVALID_PARAMETER);
	if (loaded->LoadOptionsSize != 0U)
		console_ascii(&context, "A64 UEFI LoadOptions ignored\n");
	status = discover_config_volume(&context, loaded, &discovered);
	if (EFI_ERROR(status))
		fail_status(&context, "Discover zedbsd.cfg", status);
	status = load_selected_configuration(&context, &discovered,
	    &configuration);
	if (EFI_ERROR(status))
		fail_discovered(&context, &discovered, "Load zedbsd.cfg", status);
	if (!kernel_path_utf16(configuration.kernel_path, kernel_file_path))
		fail_discovered(&context, &discovered,
		    "Prepare kernel path", EFI_INVALID_PARAMETER);
	status = discovered.root->Open(discovered.root, &kernel,
	    kernel_file_path, EFI_FILE_MODE_READ, 0U);
	if (EFI_ERROR(status) || kernel == 0)
		fail_kernel_load(&context, &discovered, kernel,
		    "Open configured kernel",
		    EFI_ERROR(status) ? status : EFI_INVALID_PARAMETER,
		    0U, 0U, 0);
	status = regular_file_size(kernel, &kernel_file_size);
	if (EFI_ERROR(status) || kernel_file_size < sizeof(first_sector) ||
	    kernel_file_size > MAX_KERNEL_FILE)
		fail_kernel_load(&context, &discovered, kernel,
		    "Validate kernel file",
		    EFI_ERROR(status) ? status : EFI_BAD_BUFFER_SIZE,
		    0U, 0U, 0);
	status = read_exact(kernel, 0, first_sector, sizeof(first_sector));
	if (EFI_ERROR(status))
		fail_kernel_load(&context, &discovered, kernel,
		    "Read ELF header", status, 0U, 0U, 0);
	if (!zbl_elf64_plan(first_sector, kernel_file_size, &plan))
		fail_kernel_load(&context, &discovered, kernel,
		    "Validate ELF64", EFI_LOAD_ERROR, 0U, 0U, 0);

	kernel_address = plan.physical_start;
	kernel_pages =
	    (UINTN)((plan.physical_end - plan.physical_start + PAGE_SIZE - 1U) /
		    PAGE_SIZE);
	status = boot->AllocatePages(AllocateAddress, EfiLoaderData,
				     kernel_pages, &kernel_address);
	if (EFI_ERROR(status) || kernel_address != plan.physical_start)
		fail_kernel_load(&context, &discovered, kernel,
		    "Allocate kernel",
		    EFI_ERROR(status) ? status : EFI_LOAD_ERROR,
		    kernel_address, kernel_pages, !EFI_ERROR(status));
	kernel_pages_allocated = 1;
	byte_zero((void *)(uintptr_t)kernel_address,
		  kernel_pages * (UINTN)PAGE_SIZE);
	for (index = 0; index < plan.segment_count; index++) {
		const struct zbl_elf64_segment *segment = &plan.segment[index];
		status = read_exact(kernel, segment->offset,
				    (void *)(uintptr_t)segment->physical,
				    (UINTN)segment->file_size);
		if (EFI_ERROR(status))
			fail_kernel_load(&context, &discovered, kernel,
			    "Read ELF segment", status, kernel_address,
			    kernel_pages, kernel_pages_allocated);
	}
	status = close_file_pair(kernel, discovered.root);
	kernel = 0;
	discovered.root = 0;
	if (EFI_ERROR(status))
		fail_kernel_load(&context, &discovered, 0,
		    "Close kernel volume", status, kernel_address,
		    kernel_pages, kernel_pages_allocated);
	console_ascii(&context, "A64 UEFI ELF\n");

	low_address = LOW_BLOCK_MAX;
	status = boot->AllocatePages(AllocateMaxAddress, EfiLoaderData,
				     LOW_BLOCK_PAGES, &low_address);
	if (EFI_ERROR(status) || low_address < 0x00010000ULL ||
	    low_address + LOW_BLOCK_PAGES * PAGE_SIZE > 0x00200000ULL)
		fail_boot_allocations(&context, "Allocate bootstrap",
		    EFI_ERROR(status) ? status : EFI_LOAD_ERROR, 0, 0,
		    low_address, LOW_BLOCK_PAGES, !EFI_ERROR(status),
		    kernel_address, kernel_pages);
	low_pages_allocated = 1;
	build_bootstrap(low_address, &plan, &framebuffer, &framebuffer_mapping,
	    discovered.fat.volume_serial, (uint8_t)discovered.path.style,
	    &configuration.parameter_record);
	handoff = (void *)(uintptr_t)(low_address + HANDOFF_OFFSET);
	handoff_common = &handoff->common.common;
	ranges = (void *)(uintptr_t)(low_address + MEMORY_RANGES_OFFSET);
	handoff_common->rsdp = find_acpi_rsdp(system);
	if (handoff_common->rsdp == 0)
		fail_boot_allocations(&context, "Locate ACPI RSDP",
		    EFI_NOT_FOUND, 0, 0, low_address, LOW_BLOCK_PAGES,
		    low_pages_allocated, kernel_address, kernel_pages);
	handoff_common->flags |= ZBL6_HANDOFF_FLAG_ACPI_RSDP;
	console_hex64(&context, "A64 RSDP ", handoff_common->rsdp);
	console_hex64(&context, "A64 GOP  ", framebuffer.physical_base);
	console_hex64(&context, "A64 LOW  ", low_address);
	{
		uint64_t cr4;

		__asm__ volatile("movq %%cr4,%0" : "=r"(cr4));
		console_hex64(&context, "A64 CR4  ", cr4);
	}

	map_size = 0;
	status = boot->GetMemoryMap(&map_size, 0, &map_key, &descriptor_size,
				    &descriptor_version);
	if (status != EFI_BUFFER_TOO_SMALL || descriptor_size == 0)
		fail_boot_allocations(&context, "Size memory map", status,
		    0, 0, low_address, LOW_BLOCK_PAGES, low_pages_allocated,
		    kernel_address, kernel_pages);
	if (descriptor_size > (UINTPTR_MAX - PAGE_SIZE * 4U) / 64U ||
	    map_size > UINTPTR_MAX - descriptor_size * 64U - PAGE_SIZE * 4U)
		fail_boot_allocations(&context, "Bound memory map",
		    EFI_BAD_BUFFER_SIZE, 0, 0, low_address, LOW_BLOCK_PAGES,
		    low_pages_allocated, kernel_address, kernel_pages);
	map_capacity = map_size + descriptor_size * 64U + PAGE_SIZE * 4U;
	status = boot->AllocatePool(EfiLoaderData, map_capacity, (void **)&map);
	if (EFI_ERROR(status))
		fail_boot_allocations(&context, "Allocate memory map", status,
		    0, 0, low_address, LOW_BLOCK_PAGES, low_pages_allocated,
		    kernel_address, kernel_pages);
	map_allocated = 1;
	/* Validate a non-final snapshot while console and failure reporting are
	 * still available.  The final snapshot is normalized only after boot
	 * services have exited so its map key is consumed immediately. */
	map_size = map_capacity;
	status = boot->GetMemoryMap(&map_size, map, &map_key,
				    &descriptor_size, &descriptor_version);
	if (EFI_ERROR(status))
		fail_boot_allocations(&context, "Preflight memory map", status,
		    map, map_allocated, low_address, LOW_BLOCK_PAGES,
		    low_pages_allocated, kernel_address, kernel_pages);
	map_result = zbl_uefi_normalize_memory_map(
	    map, map_size, descriptor_size, ranges, MAX_MEMORY_RANGES,
	    &range_count);
	if (map_result != ZBL_UEFI_MAP_OK) {
		console_ascii(&context, "UEFI map rejected: ");
		console_ascii(&context, zbl_uefi_map_result_name(map_result));
		console_ascii(&context, "\n");
		fail_boot_allocations(&context, "Normalize memory map",
		    EFI_LOAD_ERROR, map, map_allocated, low_address,
		    LOW_BLOCK_PAGES, low_pages_allocated, kernel_address,
		    kernel_pages);
	}
	console_hex64(&context, "A64 MAPSZ ", map_size);
	console_hex64(&context, "A64 DESCSZ ", descriptor_size);
	console_hex64(&context, "A64 DESCVER ", descriptor_version);
	console_hex64(&context, "A64 RANGES ", range_count);
	console_ascii(&context, "A64 MARK 1=EBS 2=MAP 3=CR3 4=KERN\n");
	console_ascii(&context, "A64 UEFI READY\n");

	for (attempt = 0; attempt < 3U; attempt++) {
		map_size = map_capacity;
		status =
		    boot->GetMemoryMap(&map_size, map, &map_key,
				       &descriptor_size, &descriptor_version);
		if (EFI_ERROR(status))
			fail_boot_allocations(&context, "Final memory map", status,
			    map, map_allocated, low_address, LOW_BLOCK_PAGES,
			    low_pages_allocated, kernel_address, kernel_pages);
		status = boot->ExitBootServices(image, map_key);
		if (status == EFI_SUCCESS)
			break;
		if (status != EFI_INVALID_PARAMETER)
			fail_boot_allocations(&context, "ExitBootServices", status,
			    map, map_allocated, low_address, LOW_BLOCK_PAGES,
			    low_pages_allocated, kernel_address, kernel_pages);
	}
	if (status != EFI_SUCCESS)
		fail_boot_allocations(&context, "ExitBootServices retries",
		    status, map, map_allocated, low_address, LOW_BLOCK_PAGES,
		    low_pages_allocated, kernel_address, kernel_pages);
	framebuffer_stage(&framebuffer, 1U);
	debug_port("A64 UEFI BOOT SERVICES EXITED\n");
	map_result = zbl_uefi_normalize_memory_map(
	    map, map_size, descriptor_size, ranges, MAX_MEMORY_RANGES,
	    &range_count);
	if (map_result != ZBL_UEFI_MAP_OK) {
		framebuffer_map_error(&framebuffer, map_result);
		debug_port("A64 UEFI FINAL MAP REJECTED\n");
		halt();
	}
	framebuffer_stage(&framebuffer, 2U);
	handoff_common->memory_range_count = range_count;
	debug_port("A64 UEFI EXIT\n");
	((transition_fn)(uintptr_t)(low_address + LOW_TRAMPOLINE_OFFSET))(
	    handoff_common->bootstrap_cr3, low_address + TRANSITION_STACK_TOP,
	    low_address + HANDOFF_OFFSET, plan.entry);
}
