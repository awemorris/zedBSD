/* zedBSD fallback-path x64 UEFI loader. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "include/uefi.h"
#include "elf64.h"
#include "memory-map.h"
#include "load-options.h"
#include "bootloader/include/amd64-handoff.h"

#define PAGE_SIZE 4096ULL
#define LOW_BLOCK_PAGES 16U
#define LOW_BLOCK_MAX 0x001fffffULL
#define MAX_MEMORY_RANGES 256U
#define MAX_KERNEL_FILE 0x04000000ULL
#define BOOT_SECTOR_BUFFER 4096U

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

static const struct zedbsd_boot_parameter_record image_boot_parameters = {
	.magic = ZEDBSD_BOOT_PARAMETER_RECORD_MAGIC,
	.version = ZEDBSD_BOOT_PARAMETER_RECORD_VERSION,
	.size = ZEDBSD_BOOT_PARAMETER_RECORD_SIZE,
	.flags = ZEDBSD_BOOT_PARAMETER_RECORD_FLAG_TEXT,
	.length = sizeof(ZEDBSD_IMAGE_BOOT_PARAMETERS_TEXT) - 1U,
	.reserved = 0,
	.text = ZEDBSD_IMAGE_BOOT_PARAMETERS_TEXT,
};

_Static_assert(sizeof(ZEDBSD_IMAGE_BOOT_PARAMETERS_TEXT) - 1U <=
	       ZEDBSD_BOOT_PARAMETERS_TEXT_MAX,
	       "image boot parameters must fit transport storage");

_Static_assert(ZBL6_HANDOFF_V5_UEFI_SIZE <=
	       MEMORY_RANGES_OFFSET - HANDOFF_OFFSET,
	       "UEFI parameter handoff must fit its low-memory slot");

struct loader_context {
	EFI_HANDLE image;
	EFI_SYSTEM_TABLE *system;
	EFI_BOOT_SERVICES *boot;
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

static void __attribute__((noreturn))
fail_status(struct loader_context *context, const char *operation,
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
		if (amount == 0)
			return EFI_LOAD_ERROR;
		cursor += amount;
		size -= amount;
	}
	return EFI_SUCCESS;
}

static uint16_t
little16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8);
}

static uint32_t
little32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	       ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static EFI_STATUS
fat_volume_serial(EFI_BLOCK_IO_PROTOCOL *block, uint32_t *serial)
{
	static uint8_t sector[BOOT_SECTOR_BUFFER]
	    __attribute__((aligned(BOOT_SECTOR_BUFFER)));
	EFI_BLOCK_IO_MEDIA *media;
	EFI_STATUS status;
	unsigned offset;

	if (block == 0 || block->Media == 0 || block->ReadBlocks == 0 ||
	    serial == 0)
		return EFI_INVALID_PARAMETER;
	media = block->Media;
	if (!media->MediaPresent || !media->LogicalPartition ||
	    media->BlockSize < 512U || media->BlockSize > sizeof(sector) ||
	    (media->IoAlign > 1U &&
	     ((uintptr_t)sector & (media->IoAlign - 1U)) != 0))
		return EFI_UNSUPPORTED;
	status = block->ReadBlocks(block, media->MediaId, 0, media->BlockSize,
				   sector);
	if (EFI_ERROR(status))
		return status;
	if (little16(sector + 11U) != media->BlockSize || sector[13] == 0 ||
	    sector[16] == 0 || sector[510] != 0x55U || sector[511] != 0xaaU)
		return EFI_UNSUPPORTED;
	offset = little16(sector + 22U) != 0 ? 39U : 67U;
	if (sector[offset - 1U] != 0x29U)
		return EFI_UNSUPPORTED;
	*serial = little32(sector + offset);
	return EFI_SUCCESS;
}

static int
framebuffer_from_gop(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
		     struct zbl6_framebuffer *framebuffer)
{
	EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *mode;
	EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
	uint64_t required;

	if (gop == 0 || gop->Mode == 0 || gop->Mode->Info == 0)
		return 0;
	mode = gop->Mode;
	info = mode->Info;
	if (info->HorizontalResolution == 0 || info->VerticalResolution == 0 ||
	    info->PixelsPerScanLine < info->HorizontalResolution ||
	    (info->PixelFormat != PixelRedGreenBlueReserved8BitPerColor &&
	     info->PixelFormat != PixelBlueGreenRedReserved8BitPerColor))
		return 0;
	required =
	    (uint64_t)info->PixelsPerScanLine * info->VerticalResolution * 4U;
	if (required == 0 || required > mode->FrameBufferSize ||
	    mode->FrameBufferBase > UINT64_MAX - required)
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
		uint32_t boot_volume_serial,
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
	uint64_t framebuffer_aligned, framebuffer_end;
	unsigned framebuffer_pages;

	byte_zero(low, LOW_BLOCK_PAGES * PAGE_SIZE);
	if (transition_size == 0 || transition_size > PAGE_SIZE)
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
	framebuffer_aligned = framebuffer->physical_base & ~0x1fffffULL;
	framebuffer_end = framebuffer->physical_base + framebuffer->size;
	framebuffer_pages =
	    (unsigned)((framebuffer_end - framebuffer_aligned + 0x1fffffULL) /
		       0x200000ULL);
	if (framebuffer_pages == 0 || framebuffer_pages > 496U)
		halt();
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
	common->root_partition_scheme = 1;
	common->root_partition_index = 1;
	common->loader_partition_index = 2;
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
	EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *filesystem;
	EFI_BLOCK_IO_PROTOCOL *block;
	EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
	EFI_FILE_PROTOCOL *root, *kernel;
	EFI_BOOT_SERVICES *boot;
	EFI_STATUS status;
	static uint8_t first_sector[ZBL_ELF_HEADER_BYTES];
	struct zbl_elf64_plan plan;
	EFI_PHYSICAL_ADDRESS kernel_address, low_address;
	UINTN kernel_pages, map_size, map_capacity, map_key;
	UINTN descriptor_size;
	UINT32 descriptor_version;
	EFI_MEMORY_DESCRIPTOR *map;
	struct zbl6_handoff_v5_uefi *handoff;
	struct zbl6_handoff_v3 *handoff_common;
	struct zbl6_framebuffer framebuffer;
	struct zedbsd_boot_parameter_record parameter_record;
	struct zbl6_memory_range *ranges;
	uint32_t range_count;
	uint32_t boot_volume_serial;
	enum zbl_uefi_map_result map_result;
	enum zbl_uefi_load_options_result option_result;
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
	if (!framebuffer_from_gop(gop, &framebuffer))
		fail_status(&context, "Validate GOP", EFI_UNSUPPORTED);

	status = boot->HandleProtocol(image, &EFI_LOADED_IMAGE_PROTOCOL_GUID,
				      (void **)&loaded);
	if (EFI_ERROR(status))
		fail_status(&context, "LoadedImage", status);
	option_result = zbl_uefi_load_options_record(&parameter_record,
	    loaded->LoadOptions, loaded->LoadOptionsSize,
	    image_boot_parameters.text, sizeof(image_boot_parameters.text));
	if (option_result != ZBL_UEFI_LOAD_OPTIONS_OK) {
		console_ascii(&context, "UEFI LoadOptions rejected: ");
		console_ascii(&context,
		    zbl_uefi_load_options_result_name(option_result));
		console_ascii(&context, "\n");
		fail_status(&context, "Validate LoadOptions", EFI_INVALID_PARAMETER);
	}
	status = boot->HandleProtocol(loaded->DeviceHandle,
				      &EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID,
				      (void **)&filesystem);
	if (EFI_ERROR(status))
		fail_status(&context, "SimpleFS", status);
	status = boot->HandleProtocol(
	    loaded->DeviceHandle, &EFI_BLOCK_IO_PROTOCOL_GUID, (void **)&block);
	if (EFI_ERROR(status))
		fail_status(&context, "BlockIO", status);
	status = fat_volume_serial(block, &boot_volume_serial);
	if (EFI_ERROR(status))
		fail_status(&context, "Boot UUID", status);
	status = filesystem->OpenVolume(filesystem, &root);
	if (EFI_ERROR(status))
		fail_status(&context, "OpenVolume", status);
	status = root->Open(root, &kernel, (const CHAR16 *)L"\\VMUNIX.X64",
			    EFI_FILE_MODE_READ, 0);
	if (EFI_ERROR(status))
		fail_status(&context, "Open VMUNIX.X64", status);
	status = read_exact(kernel, 0, first_sector, sizeof(first_sector));
	if (EFI_ERROR(status))
		fail_status(&context, "Read ELF header", status);
	if (!zbl_elf64_plan(first_sector, MAX_KERNEL_FILE, &plan))
		fail_status(&context, "Validate ELF64", EFI_LOAD_ERROR);

	kernel_address = plan.physical_start;
	kernel_pages =
	    (UINTN)((plan.physical_end - plan.physical_start + PAGE_SIZE - 1U) /
		    PAGE_SIZE);
	status = boot->AllocatePages(AllocateAddress, EfiLoaderData,
				     kernel_pages, &kernel_address);
	if (EFI_ERROR(status) || kernel_address != plan.physical_start)
		fail_status(&context, "Allocate kernel",
			    EFI_ERROR(status) ? status : EFI_LOAD_ERROR);
	byte_zero((void *)(uintptr_t)kernel_address,
		  kernel_pages * (UINTN)PAGE_SIZE);
	for (index = 0; index < plan.segment_count; index++) {
		const struct zbl_elf64_segment *segment = &plan.segment[index];
		status = read_exact(kernel, segment->offset,
				    (void *)(uintptr_t)segment->physical,
				    (UINTN)segment->file_size);
		if (EFI_ERROR(status))
			fail_status(&context, "Read ELF segment", status);
	}
	status = kernel->Close(kernel);
	if (EFI_ERROR(status))
		fail_status(&context, "Close kernel", status);
	status = root->Close(root);
	if (EFI_ERROR(status))
		fail_status(&context, "Close root", status);
	console_ascii(&context, "A64 UEFI ELF\n");

	low_address = LOW_BLOCK_MAX;
	status = boot->AllocatePages(AllocateMaxAddress, EfiLoaderData,
				     LOW_BLOCK_PAGES, &low_address);
	if (EFI_ERROR(status) || low_address < 0x00010000ULL ||
	    low_address + LOW_BLOCK_PAGES * PAGE_SIZE > 0x00200000ULL)
		fail_status(&context, "Allocate bootstrap",
			    EFI_ERROR(status) ? status : EFI_LOAD_ERROR);
	build_bootstrap(low_address, &plan, &framebuffer, boot_volume_serial,
	    &parameter_record);
	handoff = (void *)(uintptr_t)(low_address + HANDOFF_OFFSET);
	handoff_common = &handoff->common.common;
	ranges = (void *)(uintptr_t)(low_address + MEMORY_RANGES_OFFSET);
	handoff_common->rsdp = find_acpi_rsdp(system);
	if (handoff_common->rsdp == 0)
		fail_status(&context, "Locate ACPI RSDP", EFI_NOT_FOUND);
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
		fail_status(&context, "Size memory map", status);
	map_capacity = map_size + descriptor_size * 64U + PAGE_SIZE * 4U;
	status = boot->AllocatePool(EfiLoaderData, map_capacity, (void **)&map);
	if (EFI_ERROR(status))
		fail_status(&context, "Allocate memory map", status);
	/* Validate a non-final snapshot while console and failure reporting are
	 * still available.  The final snapshot is normalized only after boot
	 * services have exited so its map key is consumed immediately. */
	map_size = map_capacity;
	status = boot->GetMemoryMap(&map_size, map, &map_key,
				    &descriptor_size, &descriptor_version);
	if (EFI_ERROR(status))
		fail_status(&context, "Preflight memory map", status);
	map_result = zbl_uefi_normalize_memory_map(
	    map, map_size, descriptor_size, ranges, MAX_MEMORY_RANGES,
	    &range_count);
	if (map_result != ZBL_UEFI_MAP_OK) {
		console_ascii(&context, "UEFI map rejected: ");
		console_ascii(&context, zbl_uefi_map_result_name(map_result));
		console_ascii(&context, "\n");
		fail_status(&context, "Normalize memory map", EFI_LOAD_ERROR);
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
			fail_status(&context, "Final memory map", status);
		status = boot->ExitBootServices(image, map_key);
		if (status == EFI_SUCCESS)
			break;
		if (status != EFI_INVALID_PARAMETER)
			fail_status(&context, "ExitBootServices", status);
	}
	if (status != EFI_SUCCESS)
		fail_status(&context, "ExitBootServices retries", status);
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
