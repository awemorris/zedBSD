/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The UEFI variable store, reached through the firmware's runtime services.
 *
 * The loader moved the runtime services to a kernel window with
 * SetVirtualAddressMap before the kernel started.  This driver maps each
 * runtime range at the address the firmware was told, checks the system
 * table and the runtime services table, and then calls GetVariable,
 * SetVariable and GetNextVariableName one at a time, with interrupts off
 * and the processor's floating-point state saved around each call.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <uapi/errno.h>
#include <uapi/system.h>

#include "kern/irq.h"
#include "kern/klog.h"
#include "kern/lock.h"
#include "kern/platform.h"
#include "kern/pmem.h"

#include "uefi-variables.h"

/*
 * The UEFI values the driver uses.
 *
 * Values referenced: UEFI Specification 2.10, 4.3 (EFI_SYSTEM_TABLE_SIGNATURE),
 * 8.1 (EFI_RUNTIME_SERVICES_SIGNATURE), 7.2 (EFI_MEMORY_TYPE
 * EfiRuntimeServicesCode, the EFI_MEMORY_WB attribute), Appendix D (the
 * status codes).
 */
#define UEFI_PAGE_SIZE				4096U
#define UEFI_SYSTEM_TABLE_SIGNATURE		0x5453595320494249ULL
#define UEFI_RUNTIME_SERVICES_SIGNATURE		0x56524553544e5552ULL
#define UEFI_RUNTIME_SERVICES_CODE		5U
#define UEFI_MEMORY_WB				0x0000000000000008ULL
#define UEFI_ERROR				0x8000000000000000ULL
#define UEFI_SUCCESS				0ULL
#define UEFI_INVALID_PARAMETER			(UEFI_ERROR | 2U)
#define UEFI_UNSUPPORTED			(UEFI_ERROR | 3U)
#define UEFI_BUFFER_TOO_SMALL			(UEFI_ERROR | 5U)
#define UEFI_DEVICE_ERROR			(UEFI_ERROR | 7U)
#define UEFI_WRITE_PROTECTED			(UEFI_ERROR | 8U)
#define UEFI_OUT_OF_RESOURCES			(UEFI_ERROR | 9U)
#define UEFI_NOT_FOUND				(UEFI_ERROR | 14U)
#define UEFI_SECURITY_VIOLATION			(UEFI_ERROR | 26U)

/* The UCS-2 units of the longest name, with its terminator. */
#define UEFI_NAME_UNITS				KERN_SYSTEM_FIRMWARE_VARIABLE_NAME_MAX

/* The characters of a GUID's text form, without its terminator. */
#define UEFI_GUID_TEXT_LENGTH			36U

/* The size of the area FXSAVE writes. */
#define UEFI_FXSAVE_SIZE			512U

/*
 * The MXCSR and the x87 control word UEFI requires on entry to a service:
 * every exception masked, round to nearest, double-extended precision.
 *
 * Values referenced: UEFI Specification 2.10, 2.3.4.1 (x64 Platforms,
 * "MXCSR ... 0x1F80", "x87 FPU control word ... 0x037F").
 */
#define UEFI_MXCSR				0x1f80U

/*
 * A vendor GUID as UEFI lays it out in memory.
 *
 * Values referenced: UEFI Specification 2.10, Appendix A (EFI_GUID).
 */
struct uefi_guid {
	uint32_t data1;
	uint16_t data2;
	uint16_t data3;
	uint8_t data4[8];
};

/*
 * The header every UEFI table starts with; the driver checks a table by
 * its signature and size before reading further.
 *
 * Values referenced: UEFI Specification 2.10, 4.2 (EFI_TABLE_HEADER).
 */
struct uefi_table_header {
	uint64_t signature;
	uint32_t revision;
	uint32_t header_size;
	uint32_t crc32;
	uint32_t reserved;
};

/*
 * The system table up to the pointer to the runtime services, which is all
 * the driver reads of it.  After SetVirtualAddressMap its pointers are
 * addresses in the runtime window.
 *
 * Values referenced: UEFI Specification 2.10, 4.3 (EFI_SYSTEM_TABLE).
 */
struct uefi_system_table {
	struct uefi_table_header header;
	uint64_t firmware_vendor;
	uint32_t firmware_revision;
	uint32_t padding;
	uint64_t console_in_handle;
	uint64_t console_in;
	uint64_t console_out_handle;
	uint64_t console_out;
	uint64_t standard_error_handle;
	uint64_t standard_error;
	uint64_t runtime_services;
};

/*
 * The runtime services table up to SetVariable.
 *
 * Values referenced: UEFI Specification 2.10, 4.5 (EFI_RUNTIME_SERVICES).
 */
struct uefi_runtime_services {
	struct uefi_table_header header;
	uint64_t get_time;
	uint64_t set_time;
	uint64_t get_wakeup_time;
	uint64_t set_wakeup_time;
	uint64_t set_virtual_address_map;
	uint64_t convert_pointer;
	uint64_t get_variable;
	uint64_t get_next_variable_name;
	uint64_t set_variable;
};

/*
 * One UEFI memory descriptor as the firmware's map gives it; the HAL's copy
 * may space the descriptors wider than this.
 *
 * Values referenced: UEFI Specification 2.10, 7.2 (EFI_MEMORY_DESCRIPTOR).
 */
struct uefi_memory_descriptor {
	uint32_t type;
	uint32_t padding;
	uint64_t physical_start;
	uint64_t virtual_start;
	uint64_t page_count;
	uint64_t attribute;
};

/*
 * The variable services, called with the Microsoft x64 convention UEFI
 * uses.
 *
 * Values referenced: UEFI Specification 2.10, 8.2 (GetVariable,
 * GetNextVariableName, SetVariable).
 */
typedef uint64_t (__attribute__((ms_abi)) *uefi_get_variable_fn)(uint16_t *name, struct uefi_guid *vendor, uint32_t *attributes, uint64_t *size, void *data);
typedef uint64_t (__attribute__((ms_abi)) *uefi_get_next_variable_name_fn)(uint64_t *size, uint16_t *name, struct uefi_guid *vendor);
typedef uint64_t (__attribute__((ms_abi)) *uefi_set_variable_fn)(uint16_t *name, struct uefi_guid *vendor, uint32_t attributes, uint64_t size, void *data);

/*
 * The firmware's runtime services as the driver reaches them.
 *
 * Filled once by drv_pcat_uefi_variables_init() and not changed after
 * ready is set.  lock lets one call into the firmware at a time, which
 * UEFI requires, and protects the name buffers and the save area.
 */
struct uefi_runtime_state {
	int ready;
	struct mutex lock;
	const uint8_t *map;
	uint64_t map_count;
	uint64_t map_entry_size;
	uefi_get_variable_fn get_variable;
	uefi_get_next_variable_name_fn get_next_variable_name;
	uefi_set_variable_fn set_variable;
	uint16_t name_units[UEFI_NAME_UNITS];
	uint8_t fp_area[UEFI_FXSAVE_SIZE] __attribute__((aligned(16)));
};

_Static_assert(offsetof(struct uefi_system_table, runtime_services) == 88U, "UEFI system table layout");
_Static_assert(offsetof(struct uefi_runtime_services, set_variable) == 88U, "UEFI runtime services layout");
_Static_assert(sizeof(struct uefi_memory_descriptor) == 40U, "UEFI memory descriptor layout");
_Static_assert(KERN_SYSTEM_FIRMWARE_VARIABLE_SPACE_MAX > UEFI_GUID_TEXT_LENGTH, "a GUID's text fits the space");

/*
 * The one instance of the runtime services' state, for the kernel's life.
 * Its zero value means the services are not reachable.
 */
static struct uefi_runtime_state uefi_runtime;

static int map_runtime_ranges(void);
static int runtime_range_holds(uint64_t address, uint64_t size, int code);
static int runtime_address_of(uint64_t physical, uint64_t size, uint64_t *address);
static bool firmware_enter(void);
static void firmware_leave(bool enabled);
static int status_error(uint64_t status);
static int hex_value(char character);
static int guid_parse(const char *text, struct uefi_guid *guid);
static void guid_text(const struct uefi_guid *guid, char *text);
static int name_to_units(const char *name, uint16_t *units, size_t capacity);
static int name_from_units(const uint16_t *units, size_t count, char *name, size_t size);

/*
 * Maps the firmware's runtime services and checks their tables.
 */
int
drv_pcat_uefi_variables_init(
	void)
{
	const struct uefi_system_table *system;
	const struct uefi_runtime_services *services;
	uint64_t *system_physical;
	uint64_t *count;
	uint64_t *entry_size;
	uint64_t system_address;
	int holds;
	int error;

	/* Stays off when the boot gave no runtime services. */
	system_physical = kern_boot_handoff("uefi.system-table");
	if (system_physical == NULL)
		return ENODEV;

	/* Reads where the runtime ranges are; the platform keeps them for the kernel's life. */
	uefi_runtime.map = kern_boot_handoff("uefi.runtime-map");
	count = kern_boot_handoff("uefi.runtime-map-count");
	entry_size = kern_boot_handoff("uefi.runtime-map-entry-size");
	if (uefi_runtime.map == NULL || count == NULL || entry_size == NULL) {
		kern_logf("uefi: the platform gives no runtime map; variables are off\n");
		return ENODEV;
	}

	/* Refuses a descriptor smaller than its fields. */
	uefi_runtime.map_count = *count;
	uefi_runtime.map_entry_size = *entry_size;
	if (uefi_runtime.map_entry_size < sizeof(struct uefi_memory_descriptor)) {
		kern_logf("uefi: runtime descriptors of %llu bytes are too small\n",
			  (unsigned long long)uefi_runtime.map_entry_size);
		return ENODEV;
	}

	/* Maps every runtime range at the address the firmware was told. */
	error = map_runtime_ranges();
	if (error != 0) {
		kern_logf("uefi: the runtime ranges could not be mapped (error %d)\n", error);
		return error;
	}

	/* Finds the system table in the window. */
	error = runtime_address_of(*system_physical, sizeof(*system), &system_address);
	if (error != 0) {
		kern_logf("uefi: the system table is outside the runtime ranges\n");
		return error;
	}

	/* Refuses a system table without its signature or too short for its fields. */
	system = (const struct uefi_system_table *)(uintptr_t)system_address;
	if (system->header.signature != UEFI_SYSTEM_TABLE_SIGNATURE ||
	    system->header.header_size < sizeof(*system)) {
		kern_logf("uefi: the system table's header is not valid\n");
		return ENODEV;
	}

	/* Refuses runtime services the window does not hold. */
	holds = runtime_range_holds(system->runtime_services, sizeof(*services), 0);
	if (!holds) {
		kern_logf("uefi: the runtime services table is outside the runtime ranges\n");
		return ENODEV;
	}

	/* Refuses a runtime services table without its signature or too short for its fields. */
	services = (const struct uefi_runtime_services *)(uintptr_t)system->runtime_services;
	if (services->header.signature != UEFI_RUNTIME_SERVICES_SIGNATURE ||
	    services->header.header_size < sizeof(*services)) {
		kern_logf("uefi: the runtime services table's header is not valid\n");
		return ENODEV;
	}

	/* Refuses a GetVariable outside the firmware's runtime code. */
	holds = runtime_range_holds(services->get_variable, 1U, 1);
	if (!holds) {
		kern_logf("uefi: GetVariable is outside the runtime code\n");
		return ENODEV;
	}

	/* Refuses a GetNextVariableName outside the firmware's runtime code. */
	holds = runtime_range_holds(services->get_next_variable_name, 1U, 1);
	if (!holds) {
		kern_logf("uefi: GetNextVariableName is outside the runtime code\n");
		return ENODEV;
	}

	/* Refuses a SetVariable outside the firmware's runtime code. */
	holds = runtime_range_holds(services->set_variable, 1U, 1);
	if (!holds) {
		kern_logf("uefi: SetVariable is outside the runtime code\n");
		return ENODEV;
	}

	/* Prepares the lock that lets one call into the firmware at a time. */
	error = mutex_init(&uefi_runtime.lock, LOCK_RANK_DEVICE, "uefi variables");
	if (error != 0)
		return error;

	/* Keeps the three services; ready lets /dev/system reach them. */
	uefi_runtime.get_variable = (uefi_get_variable_fn)(uintptr_t)services->get_variable;
	uefi_runtime.get_next_variable_name = (uefi_get_next_variable_name_fn)(uintptr_t)services->get_next_variable_name;
	uefi_runtime.set_variable = (uefi_set_variable_fn)(uintptr_t)services->set_variable;
	uefi_runtime.ready = 1;
	kern_logf("uefi: variables through the runtime services, %llu ranges\n",
		  (unsigned long long)uefi_runtime.map_count);

	/* Succeeded: the variable calls reach the firmware. */
	return 0;
}

/*
 * Reads one firmware variable.
 */
int
drv_firmware_variable_get(
	const char *space,
	const char *name,
	uint32_t *attributes,
	void *value,
	size_t *size)
{
	struct uefi_guid vendor;
	uint64_t status;
	uint64_t data_size;
	uint32_t found_attributes;
	bool enabled;
	int error;

	/* Answers that the firmware's variables are out of reach. */
	if (!uefi_runtime.ready)
		return EOPNOTSUPP;

	/* Reads the vendor GUID the space names. */
	error = guid_parse(space, &vendor);
	if (error != 0)
		return error;

	/* Reads the variable under the lock, which also owns the name buffer. */
	status = UEFI_SUCCESS;
	mutex_lock(&uefi_runtime.lock);

	error = name_to_units(name, uefi_runtime.name_units, UEFI_NAME_UNITS);
	if (error == 0) {
		data_size = *size;
		found_attributes = 0;
		enabled = firmware_enter();
		status = uefi_runtime.get_variable(uefi_runtime.name_units,
						   &vendor,
						   &found_attributes,
						   &data_size,
						   value);
		firmware_leave(enabled);
	}

	mutex_unlock(&uefi_runtime.lock);

	/* Reports a name the firmware cannot hold. */
	if (error != 0)
		return error;

	/* Reports the size a larger variable needs. */
	if (status == UEFI_BUFFER_TOO_SMALL) {
		*size = (size_t)data_size;
		return ERANGE;
	}

	/* Reports why the firmware refused the read. */
	if (status != UEFI_SUCCESS)
		return status_error(status);

	/* Succeeded: the value, its size and its attributes are the caller's. */
	*size = (size_t)data_size;
	*attributes = found_attributes;
	return 0;
}

/*
 * Writes or deletes one firmware variable.
 */
int
drv_firmware_variable_set(
	const char *space,
	const char *name,
	uint32_t attributes,
	const void *value,
	size_t size)
{
	struct uefi_guid vendor;
	uint64_t status;
	bool enabled;
	int error;

	/* Answers that the firmware's variables are out of reach. */
	if (!uefi_runtime.ready)
		return EOPNOTSUPP;

	/* Reads the vendor GUID the space names. */
	error = guid_parse(space, &vendor);
	if (error != 0)
		return error;

	/*
	 * Writes the variable under the lock, which also owns the name buffer.
	 * The firmware takes the value by a non-constant pointer but only reads
	 * it.
	 */
	status = UEFI_SUCCESS;
	mutex_lock(&uefi_runtime.lock);

	error = name_to_units(name, uefi_runtime.name_units, UEFI_NAME_UNITS);
	if (error == 0) {
		enabled = firmware_enter();
		status = uefi_runtime.set_variable(uefi_runtime.name_units,
						   &vendor,
						   attributes,
						   size,
						   (void *)(uintptr_t)value);
		firmware_leave(enabled);
	}

	mutex_unlock(&uefi_runtime.lock);

	/* Reports a name the firmware cannot hold. */
	if (error != 0)
		return error;

	/* Reports why the firmware refused the write. */
	if (status != UEFI_SUCCESS)
		return status_error(status);

	/* Succeeded: the firmware stored, or deleted, the variable. */
	return 0;
}

/*
 * Finds the firmware variable after the one named.
 */
int
drv_firmware_variable_next(
	char *space,
	size_t space_size,
	char *name,
	size_t name_size)
{
	struct uefi_guid vendor;
	uint64_t status;
	uint64_t units_size;
	size_t count;
	bool enabled;
	int error;

	/* Answers that the firmware's variables are out of reach. */
	if (!uefi_runtime.ready)
		return EOPNOTSUPP;

	/* Refuses a space buffer too small for a GUID's text. */
	if (space_size <= UEFI_GUID_TEXT_LENGTH)
		return EINVAL;

	/* Starts the walk with an empty name, whose space the firmware ignores. */
	kern_memset(&vendor, 0, sizeof(vendor));
	if (name[0] != '\0') {
		error = guid_parse(space, &vendor);
		if (error != 0)
			return error;
	}

	/* Asks for the next name under the lock, which also owns the name buffer. */
	status = UEFI_SUCCESS;
	mutex_lock(&uefi_runtime.lock);

	error = name_to_units(name, uefi_runtime.name_units, UEFI_NAME_UNITS);
	if (error == 0) {
		units_size = sizeof(uefi_runtime.name_units);
		enabled = firmware_enter();
		status = uefi_runtime.get_next_variable_name(&units_size,
							     uefi_runtime.name_units,
							     &vendor);
		firmware_leave(enabled);

		/* Turns the firmware's name back into UTF-8 while the buffer is still owned. */
		if (status == UEFI_SUCCESS) {
			count = (size_t)(units_size / sizeof(uint16_t));
			error = name_from_units(uefi_runtime.name_units, count, name, name_size);
		}
	}

	mutex_unlock(&uefi_runtime.lock);

	/* Reports a name the firmware or the caller cannot hold. */
	if (error != 0)
		return error;

	/* Reports a next name longer than the longest the interface carries. */
	if (status == UEFI_BUFFER_TOO_SMALL)
		return ENAMETOOLONG;

	/* Reports the end of the walk, or why the firmware refused it. */
	if (status != UEFI_SUCCESS)
		return status_error(status);

	/* Succeeded: the space and the name are the next variable's. */
	guid_text(&vendor, space);
	return 0;
}

/*
 * Maps every runtime range at the address the loader gave the firmware.
 *
 * The firmware's code is executable and writable, since its images keep
 * their data beside their code in the same range; the rest is not
 * executable.  A range the firmware does not mark write-back is uncached.
 */
static int
map_runtime_ranges(
	void)
{
	const struct uefi_memory_descriptor *descriptor;
	unsigned protection;
	unsigned attributes;
	uint64_t index;
	int error;

	/* Maps the ranges one at a time in the loader's order. */
	for (index = 0; index < uefi_runtime.map_count; index++) {
		descriptor = (const void *)(uefi_runtime.map + index * uefi_runtime.map_entry_size);

		/* Lets the firmware run its code and change its data. */
		protection = KERN_PROT_READ | KERN_PROT_WRITE;
		if (descriptor->type == UEFI_RUNTIME_SERVICES_CODE)
			protection |= KERN_PROT_EXEC;

		/* Keeps the firmware's device ranges, such as its flash, uncached. */
		attributes = KERN_FIRMWARE_UNCACHED;
		if ((descriptor->attribute & UEFI_MEMORY_WB) != 0)
			attributes = KERN_FIRMWARE_CACHED;

		/* Maps the range at its address in the window. */
		error = kern_firmware_map(descriptor->physical_start,
					  (void *)(uintptr_t)descriptor->virtual_start,
					  (size_t)(descriptor->page_count * UEFI_PAGE_SIZE),
					  protection,
					  attributes);
		if (error != 0)
			return error;
	}

	/* Succeeded: every range is where the firmware expects it. */
	return 0;
}

/* Reports whether a range of the window, or of its code when asked, holds an extent. */
static int
runtime_range_holds(
	uint64_t address,
	uint64_t size,
	int code)
{
	const struct uefi_memory_descriptor *descriptor;
	uint64_t index;
	uint64_t bytes;

	/* Looks for the range whose virtual extent contains the whole extent. */
	for (index = 0; index < uefi_runtime.map_count; index++) {
		descriptor = (const void *)(uefi_runtime.map + index * uefi_runtime.map_entry_size);
		bytes = descriptor->page_count * UEFI_PAGE_SIZE;

		/* Skips a range of data when code was asked for. */
		if (code && descriptor->type != UEFI_RUNTIME_SERVICES_CODE)
			continue;

		/* Reports the range that holds the extent. */
		if (address >= descriptor->virtual_start &&
		    address - descriptor->virtual_start < bytes &&
		    size <= bytes - (address - descriptor->virtual_start))
			return 1;
	}

	/* No range holds it. */
	return 0;
}

/* Finds the window address of a physical extent inside one runtime range. */
static int
runtime_address_of(
	uint64_t physical,
	uint64_t size,
	uint64_t *address)
{
	const struct uefi_memory_descriptor *descriptor;
	uint64_t index;
	uint64_t bytes;

	/* Looks for the range whose physical extent contains the whole extent. */
	for (index = 0; index < uefi_runtime.map_count; index++) {
		descriptor = (const void *)(uefi_runtime.map + index * uefi_runtime.map_entry_size);
		bytes = descriptor->page_count * UEFI_PAGE_SIZE;

		/* Translates through the range that holds the extent. */
		if (physical >= descriptor->physical_start &&
		    physical - descriptor->physical_start < bytes &&
		    size <= bytes - (physical - descriptor->physical_start)) {
			*address = descriptor->virtual_start + (physical - descriptor->physical_start);
			return 0;
		}
	}

	/* No range holds it. */
	return ENODEV;
}

/*
 * Prepares the processor for a call into the firmware.
 *
 * Interrupts go off first, so no thread switch runs while the processor's
 * floating-point state belongs to the firmware; that state is saved and
 * the control words are set to the values UEFI requires.
 */
static bool
firmware_enter(
	void)
{
	uint32_t mxcsr;
	bool enabled;

	/* Keeps this processor to the call. */
	enabled = kern_irq_disable();

	/* Saves the floating-point state the call may change, and gives the firmware its own. */
	mxcsr = UEFI_MXCSR;
	__asm__ volatile("fxsave64 %0" : "=m"(uefi_runtime.fp_area));
	__asm__ volatile("fninit");
	__asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));

	/* Reports whether interrupts were on, for firmware_leave(). */
	return enabled;
}

/* Restores the processor after a call into the firmware. */
static void
firmware_leave(
	bool enabled)
{
	/* Gives the floating-point state back to the thread that had it. */
	__asm__ volatile("fxrstor64 %0" : : "m"(uefi_runtime.fp_area));

	/* Lets interrupts in again when they were on before the call. */
	if (enabled)
		kern_irq_enable();
}

/* Translates a UEFI status into an errno value. */
static int
status_error(
	uint64_t status)
{
	/* Picks the errno value closest to the firmware's reason. */
	switch (status) {
	case UEFI_NOT_FOUND:
		return ENOENT;
	case UEFI_BUFFER_TOO_SMALL:
		return ERANGE;
	case UEFI_INVALID_PARAMETER:
		return EINVAL;
	case UEFI_UNSUPPORTED:
		return EOPNOTSUPP;
	case UEFI_OUT_OF_RESOURCES:
		return ENOSPC;
	case UEFI_WRITE_PROTECTED:
		return EROFS;
	case UEFI_SECURITY_VIOLATION:
		return EACCES;
	case UEFI_DEVICE_ERROR:
		return EIO;
	default:
		return EIO;
	}
}

/* Reads one hexadecimal digit, or reports -1. */
static int
hex_value(
	char character)
{
	/* Reads a decimal digit. */
	if (character >= '0' && character <= '9')
		return character - '0';

	/* Reads a lowercase letter digit. */
	if (character >= 'a' && character <= 'f')
		return character - 'a' + 10;

	/* Reads an uppercase letter digit. */
	if (character >= 'A' && character <= 'F')
		return character - 'A' + 10;

	/* Not a hexadecimal digit. */
	return -1;
}

/*
 * Reads a GUID's text form, such as "8be4df61-93ca-11d2-aa0d-00e098032b8c".
 *
 * The first three groups are numbers stored little-endian; the last two
 * are bytes in the order written.
 */
static int
guid_parse(
	const char *text,
	struct uefi_guid *guid)
{
	uint8_t bytes[16];
	unsigned position;
	unsigned index;
	int high;
	int low;

	/* Reads the sixteen bytes in the order they are written, past the four dashes. */
	position = 0;
	for (index = 0; index < 16U; index++) {
		/* Steps over the dash before the second to fifth groups. */
		if (index == 4U || index == 6U || index == 8U || index == 10U) {
			if (text[position] != '-')
				return EINVAL;
			position++;
		}

		/* Reads the two digits of the byte. */
		high = hex_value(text[position]);
		if (high < 0)
			return EINVAL;
		low = hex_value(text[position + 1U]);
		if (low < 0)
			return EINVAL;
		bytes[index] = (uint8_t)(high * 16 + low);
		position += 2U;
	}

	/* Refuses text that goes on past the GUID. */
	if (text[position] != '\0')
		return EINVAL;

	/* Stores the numbers of the first three groups in the processor's order. */
	guid->data1 = (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
		      (uint32_t)bytes[2] << 8 | bytes[3];
	guid->data2 = (uint16_t)(bytes[4] << 8 | bytes[5]);
	guid->data3 = (uint16_t)(bytes[6] << 8 | bytes[7]);
	kern_memcpy(guid->data4, &bytes[8], sizeof(guid->data4));

	/* Succeeded: the GUID is the one the text names. */
	return 0;
}

/* Writes a GUID's lowercase text form and its terminator. */
static void
guid_text(
	const struct uefi_guid *guid,
	char *text)
{
	static const char digits[] = "0123456789abcdef";
	uint8_t bytes[16];
	unsigned position;
	unsigned index;

	/* Lays the GUID out in the order its text is written. */
	bytes[0] = (uint8_t)(guid->data1 >> 24);
	bytes[1] = (uint8_t)(guid->data1 >> 16);
	bytes[2] = (uint8_t)(guid->data1 >> 8);
	bytes[3] = (uint8_t)guid->data1;
	bytes[4] = (uint8_t)(guid->data2 >> 8);
	bytes[5] = (uint8_t)guid->data2;
	bytes[6] = (uint8_t)(guid->data3 >> 8);
	bytes[7] = (uint8_t)guid->data3;
	kern_memcpy(&bytes[8], guid->data4, sizeof(guid->data4));

	/* Writes two digits for each byte, with the dashes between the groups. */
	position = 0;
	for (index = 0; index < 16U; index++) {
		/* Writes the dash before the second to fifth groups. */
		if (index == 4U || index == 6U || index == 8U || index == 10U) {
			text[position] = '-';
			position++;
		}

		/* Writes the byte's two digits. */
		text[position] = digits[bytes[index] >> 4];
		text[position + 1U] = digits[bytes[index] & 15U];
		position += 2U;
	}

	/* Ends the text. */
	text[position] = '\0';
}

/*
 * Turns a UTF-8 name into the UCS-2 the firmware takes, with its
 * terminator.  UCS-2 holds the Basic Multilingual Plane alone, so other
 * characters and malformed UTF-8 are refused.
 */
static int
name_to_units(
	const char *name,
	uint16_t *units,
	size_t capacity)
{
	const uint8_t *bytes;
	uint32_t character;
	uint32_t smallest;
	size_t count;

	/* Converts one character at a time until the terminator. */
	bytes = (const uint8_t *)name;
	count = 0;
	while (*bytes != 0) {
		/* Reads one character of one, two or three bytes, with the smallest its length may carry. */
		if (bytes[0] < 0x80U) {
			character = bytes[0];
			smallest = 0;
			bytes += 1;
		} else if (bytes[0] >= 0xc0U && bytes[0] <= 0xdfU &&
			   (bytes[1] & 0xc0U) == 0x80U) {
			character = (uint32_t)(bytes[0] & 0x1fU) << 6 | (bytes[1] & 0x3fU);
			smallest = 0x80U;
			bytes += 2;
		} else if (bytes[0] >= 0xe0U && bytes[0] <= 0xefU &&
			   (bytes[1] & 0xc0U) == 0x80U &&
			   (bytes[2] & 0xc0U) == 0x80U) {
			character = (uint32_t)(bytes[0] & 0x0fU) << 12 |
				    (uint32_t)(bytes[1] & 0x3fU) << 6 |
				    (bytes[2] & 0x3fU);
			smallest = 0x800U;
			bytes += 3;
		} else {
			/* Refuses a character outside UCS-2 or a malformed sequence. */
			return EILSEQ;
		}

		/* Refuses an overlong form, which names its character twice. */
		if (character < smallest)
			return EILSEQ;

		/* Refuses the surrogates, which UCS-2 does not hold alone. */
		if (character >= 0xd800U && character <= 0xdfffU)
			return EILSEQ;

		/* Refuses a name longer than the buffer leaves room for with its terminator. */
		if (count + 1U >= capacity)
			return ENAMETOOLONG;
		units[count] = (uint16_t)character;
		count++;
	}

	/* Ends the name. */
	units[count] = 0;

	/* Succeeded: the name is in UCS-2. */
	return 0;
}

/*
 * Turns a UCS-2 name of at most count units, ending at its terminator,
 * into UTF-8 with its terminator.
 */
static int
name_from_units(
	const uint16_t *units,
	size_t count,
	char *name,
	size_t size)
{
	size_t index;
	size_t length;
	uint16_t unit;

	/* Converts one unit at a time until the terminator. */
	length = 0;
	for (index = 0; index < count && units[index] != 0; index++) {
		unit = units[index];

		/* Refuses a surrogate, which UTF-8 cannot carry alone. */
		if (unit >= 0xd800U && unit <= 0xdfffU)
			return EILSEQ;

		/* Writes the unit in one, two or three bytes. */
		if (unit < 0x80U) {
			if (length + 1U >= size)
				return ENAMETOOLONG;
			name[length] = (char)unit;
			length += 1U;
		} else if (unit < 0x800U) {
			if (length + 2U >= size)
				return ENAMETOOLONG;
			name[length] = (char)(0xc0U | unit >> 6);
			name[length + 1U] = (char)(0x80U | (unit & 0x3fU));
			length += 2U;
		} else {
			if (length + 3U >= size)
				return ENAMETOOLONG;
			name[length] = (char)(0xe0U | unit >> 12);
			name[length + 1U] = (char)(0x80U | (unit >> 6 & 0x3fU));
			name[length + 2U] = (char)(0x80U | (unit & 0x3fU));
			length += 3U;
		}
	}

	/* Ends the name. */
	name[length] = '\0';

	/* Succeeded: the name is in UTF-8. */
	return 0;
}
