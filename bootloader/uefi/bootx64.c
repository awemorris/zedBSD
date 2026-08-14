/* zedBSD fallback-path x64 UEFI loader. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "include/uefi.h"
#include "elf64.h"
#include "bootloader/include/amd64-handoff.h"

#define PAGE_SIZE               4096ULL
#define LOW_BLOCK_PAGES         16U
#define LOW_BLOCK_MAX           0x001fffffULL
#define MAX_MEMORY_RANGES       256U
#define MAX_KERNEL_FILE         0x04000000ULL

#define LOW_TRAMPOLINE_OFFSET   0x0000U
#define LOW_PML4_OFFSET         0x1000U
#define LOW_PDPT_OFFSET         0x2000U
#define LOW_PD_OFFSET           0x3000U
#define HIGH_PDPT_OFFSET        0x4000U
#define HIGH_PD_OFFSET          0x5000U
#define HANDOFF_OFFSET          0x6000U
#define MEMORY_RANGES_OFFSET    0x7000U
#define TRANSITION_STACK_TOP    0x10000U

#define PTE_PRESENT             0x001ULL
#define PTE_WRITE               0x002ULL
#define PTE_LARGE               0x080ULL

extern uint8_t zbl_transition_start[];
extern uint8_t zbl_transition_end[];

struct loader_context {
	EFI_HANDLE image;
	EFI_SYSTEM_TABLE *system;
	EFI_BOOT_SERVICES *boot;
};

typedef void (EFIAPI *transition_fn)(uint64_t, uint64_t, uint64_t,
	uint64_t) __attribute__((noreturn));

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
read_exact(EFI_FILE_PROTOCOL *file, UINT64 position, void *buffer,
	UINTN size)
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

static uint32_t
memory_type(UINT32 uefi_type)
{
	switch (uefi_type) {
	case EfiConventionalMemory:
		return ZBL6_MEMORY_USABLE;
	case EfiACPIReclaimMemory:
		return ZBL6_MEMORY_ACPI_RECLAIM;
	case EfiACPIMemoryNVS:
		return ZBL6_MEMORY_ACPI_NVS;
	case EfiMemoryMappedIO:
	case EfiMemoryMappedIOPortSpace:
		return ZBL6_MEMORY_MMIO;
	default:
		return ZBL6_MEMORY_RESERVED;
	}
}

static int
normalize_memory_map(const void *raw_map, UINTN map_size,
	UINTN descriptor_size, struct zbl6_memory_range *ranges,
	uint32_t *range_count)
{
	const uint8_t *cursor = raw_map;
	UINTN offset;
	uint32_t count = 0;

	if (descriptor_size < sizeof(EFI_MEMORY_DESCRIPTOR) ||
	    map_size % descriptor_size != 0)
		return 0;
	for (offset = 0; offset < map_size; offset += descriptor_size) {
		const EFI_MEMORY_DESCRIPTOR *descriptor =
		    (const void *)(cursor + offset);
		uint64_t base = descriptor->PhysicalStart;
		uint64_t size;
		uint32_t type;

		if (descriptor->NumberOfPages == 0)
			continue;
		if (descriptor->NumberOfPages > UINT64_MAX / PAGE_SIZE)
			return 0;
		size = descriptor->NumberOfPages * PAGE_SIZE;
		if (base > UINT64_MAX - size)
			return 0;
		type = memory_type(descriptor->Type);
		if (count != 0) {
			struct zbl6_memory_range *previous = &ranges[count - 1U];
			uint64_t previous_end = previous->base + previous->size;
			if (base < previous_end)
				return 0;
			if (base == previous_end && previous->type == type) {
				previous->size += size;
				continue;
			}
		}
		if (count == MAX_MEMORY_RANGES)
			return 0;
		ranges[count].base = base;
		ranges[count].size = size;
		ranges[count].type = type;
		ranges[count].flags = 0;
		count++;
	}
	*range_count = count;
	return count != 0;
}

static void
build_bootstrap(uint64_t low_base, const struct zbl_elf64_plan *plan)
{
	uint8_t *low = (void *)(uintptr_t)low_base;
	uint64_t *pml4 = (void *)(low + LOW_PML4_OFFSET);
	uint64_t *low_pdpt = (void *)(low + LOW_PDPT_OFFSET);
	uint64_t *low_pd = (void *)(low + LOW_PD_OFFSET);
	uint64_t *high_pdpt = (void *)(low + HIGH_PDPT_OFFSET);
	uint64_t *high_pd = (void *)(low + HIGH_PD_OFFSET);
	struct zbl6_handoff_v2 *handoff = (void *)(low + HANDOFF_OFFSET);
	UINTN transition_size = (UINTN)(zbl_transition_end -
	    zbl_transition_start);
	unsigned index;

	byte_zero(low, LOW_BLOCK_PAGES * PAGE_SIZE);
	if (transition_size == 0 || transition_size > PAGE_SIZE)
		halt();
	byte_copy(low + LOW_TRAMPOLINE_OFFSET, zbl_transition_start,
	    transition_size);
	pml4[0] = (low_base + LOW_PDPT_OFFSET) | PTE_PRESENT | PTE_WRITE;
	low_pdpt[0] = (low_base + LOW_PD_OFFSET) | PTE_PRESENT | PTE_WRITE;
	pml4[511] = (low_base + HIGH_PDPT_OFFSET) | PTE_PRESENT | PTE_WRITE;
	high_pdpt[510] = (low_base + HIGH_PD_OFFSET) |
	    PTE_PRESENT | PTE_WRITE;
	for (index = 0; index < 512; index++) {
		uint64_t entry = (uint64_t)index * 0x200000ULL |
		    PTE_PRESENT | PTE_WRITE | PTE_LARGE;
		low_pd[index] = entry;
		high_pd[index] = entry;
	}
	handoff->magic = ZBL6_HANDOFF_MAGIC;
	handoff->version = ZBL6_HANDOFF_V2_VERSION;
	handoff->size = sizeof(*handoff);
	handoff->flags = ZBL6_HANDOFF_FLAG_UEFI |
	    ZBL6_HANDOFF_FLAG_MEMORY_MAP;
	handoff->boot_drive = 0x80;
	handoff->root_partition_scheme = 1;
	handoff->root_partition_index = 1;
	handoff->loader_partition_index = 2;
	handoff->memory_range_entry_size = sizeof(struct zbl6_memory_range);
	handoff->memory_ranges = low_base + MEMORY_RANGES_OFFSET;
	handoff->kernel_phys_start = plan->physical_start;
	handoff->kernel_phys_end = plan->physical_end;
	handoff->bootstrap_cr3 = low_base + LOW_PML4_OFFSET;
}

EFI_STATUS EFIAPI
efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *system)
{
	struct loader_context context;
	EFI_LOADED_IMAGE_PROTOCOL *loaded;
	EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *filesystem;
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
	struct zbl6_handoff_v2 *handoff;
	struct zbl6_memory_range *ranges;
	uint32_t range_count;
	unsigned index, attempt;

	if (system == 0 || system->BootServices == 0)
		return EFI_INVALID_PARAMETER;
	context.image = image;
	context.system = system;
	context.boot = system->BootServices;
	boot = context.boot;
	console_ascii(&context, "A64 UEFI ENTRY\n");

	status = boot->HandleProtocol(image,
	    &EFI_LOADED_IMAGE_PROTOCOL_GUID, (void **)&loaded);
	if (EFI_ERROR(status))
		fail_status(&context, "LoadedImage", status);
	status = boot->HandleProtocol(loaded->DeviceHandle,
	    &EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID, (void **)&filesystem);
	if (EFI_ERROR(status))
		fail_status(&context, "SimpleFS", status);
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
	kernel_pages = (UINTN)((plan.physical_end - plan.physical_start +
	    PAGE_SIZE - 1U) / PAGE_SIZE);
	status = boot->AllocatePages(AllocateAddress, EfiLoaderData,
	    kernel_pages, &kernel_address);
	if (EFI_ERROR(status) || kernel_address != plan.physical_start)
		fail_status(&context, "Allocate kernel", EFI_ERROR(status) ?
		    status : EFI_LOAD_ERROR);
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
		fail_status(&context, "Allocate bootstrap", EFI_ERROR(status) ?
		    status : EFI_LOAD_ERROR);
	build_bootstrap(low_address, &plan);
	handoff = (void *)(uintptr_t)(low_address + HANDOFF_OFFSET);
	ranges = (void *)(uintptr_t)(low_address + MEMORY_RANGES_OFFSET);

	map_size = 0;
	status = boot->GetMemoryMap(&map_size, 0, &map_key,
	    &descriptor_size, &descriptor_version);
	if (status != EFI_BUFFER_TOO_SMALL || descriptor_size == 0)
		fail_status(&context, "Size memory map", status);
	map_capacity = map_size + descriptor_size * 16U + PAGE_SIZE;
	status = boot->AllocatePool(EfiLoaderData, map_capacity, (void **)&map);
	if (EFI_ERROR(status))
		fail_status(&context, "Allocate memory map", status);
	console_ascii(&context, "A64 UEFI READY\n");

	for (attempt = 0; attempt < 3U; attempt++) {
		map_size = map_capacity;
		status = boot->GetMemoryMap(&map_size, map, &map_key,
		    &descriptor_size, &descriptor_version);
		if (EFI_ERROR(status))
			fail_status(&context, "Get memory map", status);
		if (!normalize_memory_map(map, map_size, descriptor_size,
		    ranges, &range_count))
			fail_status(&context, "Normalize memory map", EFI_LOAD_ERROR);
		handoff->memory_range_count = range_count;
		status = boot->ExitBootServices(image, map_key);
		if (status == EFI_SUCCESS)
			break;
		if (status != EFI_INVALID_PARAMETER)
			fail_status(&context, "ExitBootServices", status);
	}
	if (status != EFI_SUCCESS)
		halt();
	debug_port("A64 UEFI EXIT\n");
	((transition_fn)(uintptr_t)(low_address + LOW_TRAMPOLINE_OFFSET))(
	    handoff->bootstrap_cr3, low_address + TRANSITION_STACK_TOP,
	    low_address + HANDOFF_OFFSET, plan.entry);
}
