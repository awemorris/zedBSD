/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Physical memory and device mappings for drivers.
 *
 * Each call forwards to the HAL. RAM is reached through the direct map, so
 * kern_pmem_to_kernel() is a pure address translation; device memory is not
 * direct-mapped and needs kern_device_map().
 */

#include <uapi/errno.h>
#include <hal/hal.h>

#include "kern/clock.h"
#include "kern/platform.h"
#include "kern/pmem.h"

/* The longest name of one boot framebuffer value, terminator included. */
#define BOOT_FRAMEBUFFER_NAME_MAX	64U

static int boot_framebuffer_value(const char *prefix, const char *value_name, uint64_t *value);

/*
 * Reports an errno for one HAL status.
 */
static int
pmem_error(
	int status)
{
	/* Maps the statuses the memory paths can report. */
	switch (status) {
	case HAL_OK:
		return 0;
	case HAL_ERR_INVALID:
		return EINVAL;
	case HAL_ERR_NOMEM:
		return ENOMEM;
	case HAL_ERR_BUSY:
		return EBUSY;
	case HAL_ERR_UNSUPPORTED:
		return ENOTSUP;
	default:
		return EIO;
	}
}

/*
 * Allocates one physical RAM run.
 */
int
kern_pmem_alloc(
	size_t size,
	size_t alignment,
	struct kern_pmem *run)
{
	int status;

	/* Requires a destination for the run. */
	if (run == NULL)
		return EINVAL;

	/* Records the requested size so the release can repeat it. */
	status = hal_pmem_alloc(size, alignment, &run->paddr);
	if (status != HAL_OK)
		return pmem_error(status);
	run->size = size;
	return 0;
}

/*
 * Allocates one physical RAM run a device can reach.
 *
 * max_address is the highest physical address the device can address, and
 * boundary, when not zero, is a power-of-two block the run must not cross.
 */
int
kern_pmem_alloc_limited(
	size_t size,
	size_t alignment,
	uint64_t max_address,
	size_t boundary,
	struct kern_pmem *run)
{
	int status;

	/* Requires a destination for the run. */
	if (run == NULL)
		return EINVAL;

	/* Searches only the range the device can reach. */
	status = hal_pmem_alloc_limited(size, alignment,
					(hal_physaddr_t)max_address, boundary,
					&run->paddr);
	if (status != HAL_OK)
		return pmem_error(status);
	run->size = size;
	return 0;
}

/*
 * Releases one physical RAM run.
 */
int
kern_pmem_free(
	struct kern_pmem *run)
{
	int status;

	/* Ignores an empty run so a failed allocation is safe to release. */
	if (run == NULL)
		return EINVAL;
	if (run->size == 0)
		return 0;

	/* Releases the run and empties the caller's record. */
	status = hal_pmem_free(&run->paddr, run->size);
	if (status != HAL_OK)
		return pmem_error(status);
	run->size = 0;
	return 0;
}

/*
 * Translates a physical RAM address to its kernel address.
 */
void *
kern_pmem_to_kernel(
	hal_physaddr_t address)
{
	/* RAM is direct-mapped; device memory has no kernel alias. */
	return hal_pmem_to_kernel(address);
}

/*
 * Maps one device range into kernel space.
 */
int
kern_device_map(
	uint64_t address,
	size_t size,
	unsigned attributes,
	void **mapped)
{
	uint32_t flags;

	/* Every device mapping is readable and writable. */
	flags = HAL_SPACE_READ | HAL_SPACE_WRITE;

	/* Selects write-through when the caller asked for it. */
	if ((attributes & KERN_DEVICE_WRITETHROUGH) != 0)
		flags |= HAL_SPACE_WRITETHRU;
	else
		flags |= HAL_SPACE_NOCACHE;
	return pmem_error(hal_space_map_device((hal_physaddr_t)address, size,
					       flags, mapped));
}

/*
 * Removes one device mapping.
 */
int
kern_device_unmap(
	void *mapped,
	size_t size)
{
	/* Releases the kernel window the mapping occupied. */
	return pmem_error(hal_space_unmap_device(mapped, size));
}

/*
 * Maps one firmware range at the kernel address the firmware was told.
 */
int
kern_firmware_map(
	uint64_t address,
	void *mapped,
	size_t size,
	unsigned protection,
	unsigned attributes)
{
	uint32_t flags;
	int status;

	/* Translates the access rights the firmware's range needs. */
	flags = 0;
	if ((protection & KERN_PROT_READ) != 0)
		flags |= HAL_SPACE_READ;
	if ((protection & KERN_PROT_WRITE) != 0)
		flags |= HAL_SPACE_WRITE;
	if ((protection & KERN_PROT_EXEC) != 0)
		flags |= HAL_SPACE_EXEC;

	/* Refuses an unknown cache attribute. */
	if (attributes != KERN_FIRMWARE_CACHED && attributes != KERN_FIRMWARE_UNCACHED)
		return EINVAL;

	/* Keeps the firmware's device ranges, such as its flash, out of the caches. */
	if (attributes == KERN_FIRMWARE_UNCACHED)
		flags |= HAL_SPACE_NOCACHE;

	/* Maps the range into the system space at the named address. */
	status = hal_space_map(HAL_SPACE_SYS, mapped, (hal_physaddr_t)address, size, flags);
	if (status != HAL_OK)
		return pmem_error(status);

	/* Succeeded: the range stays mapped for the kernel's life. */
	return 0;
}

/*
 * Reports the page size of one translation level.
 */
size_t
kern_page_size(
	int level)
{
	/* Level 1 is the smallest page the architecture supports. */
	return hal_space_get_page_size(level);
}

/*
 * Reports physical memory accounting.
 */
void
kern_memstat(
	struct kern_memstat *statistics)
{
	struct hal_memstat platform;

	/* Ignores an absent destination. */
	if (statistics == NULL)
		return;

	/* Copies the subset the kernel states portably. */
	hal_get_memstat(&platform);
	statistics->physical_total = platform.physical_total;
	statistics->physical_reserved = platform.physical_reserved;
	statistics->physical_allocated = platform.physical_allocated;
	statistics->physical_free = platform.physical_free;
	statistics->task_stack_bytes = platform.task_stack_bytes;
	statistics->task_count = platform.task_count;
	statistics->space_count = platform.space_count;
	statistics->page_table_count = platform.page_table_count;
}

/*
 * Looks up one boot handoff object by name.
 */
void *
kern_boot_handoff(
	const char *name)
{
	/* The platform keeps ownership of the object. */
	return hal_get_arch_handoff(name);
}

/*
 * Reads the boot framebuffer the platform hands over under a name prefix.
 *
 * The platform names each value <prefix>.phys, .size, .width, .height,
 * .pitch and .format; all of them are there or none is.
 */
int
kern_boot_framebuffer(
	const char *prefix,
	struct kern_boot_framebuffer *framebuffer)
{
	uint64_t physical_base;
	uint64_t size;
	uint64_t width;
	uint64_t height;
	uint64_t pitch;
	uint64_t format;
	int error;

	/* Refuses a missing prefix or destination. */
	if (prefix == NULL || framebuffer == NULL)
		return EINVAL;

	/* Starts from no framebuffer, which is what a failure leaves. */
	framebuffer->physical_base = 0;
	framebuffer->size = 0;
	framebuffer->width = 0;
	framebuffer->height = 0;
	framebuffer->pitch = 0;
	framebuffer->format = 0;

	/* Finds the framebuffer's physical base; without it there is no screen. */
	error = boot_framebuffer_value(prefix, "phys", &physical_base);
	if (error != 0)
		return error;

	/* Reads the size of the framebuffer's window. */
	error = boot_framebuffer_value(prefix, "size", &size);
	if (error != 0)
		return error;

	/* Reads the width in pixels. */
	error = boot_framebuffer_value(prefix, "width", &width);
	if (error != 0)
		return error;

	/* Reads the height in pixels. */
	error = boot_framebuffer_value(prefix, "height", &height);
	if (error != 0)
		return error;

	/* Reads the distance from one row to the next. */
	error = boot_framebuffer_value(prefix, "pitch", &pitch);
	if (error != 0)
		return error;

	/* Reads the pixel format. */
	error = boot_framebuffer_value(prefix, "format", &format);
	if (error != 0)
		return error;

	/* Refuses a value the kernel's record cannot hold. */
	if (width > UINT32_MAX ||
	    height > UINT32_MAX ||
	    pitch > UINT32_MAX ||
	    format > UINT32_MAX)
		return EINVAL;

	/* Fills the kernel's record. */
	framebuffer->physical_base = physical_base;
	framebuffer->size = size;
	framebuffer->width = (uint32_t)width;
	framebuffer->height = (uint32_t)height;
	framebuffer->pitch = (uint32_t)pitch;
	framebuffer->format = (uint32_t)format;

	/* Succeeded: the platform handed over a framebuffer. */
	return 0;
}

/*
 * Reads the monotonic counter and its frequency.
 */
bool
kern_rtc_read_counter(
	uint64_t *counter,
	uint64_t *frequency_hz)
{
	/* Only differences between samples are meaningful. */
	return hal_rtc_read_counter(counter, frequency_hz);
}

/*
 * Reads one boot framebuffer value by its name, <prefix>.<value_name>.
 */
static int
boot_framebuffer_value(
	const char *prefix,
	const char *value_name,
	uint64_t *value)
{
	char name[BOOT_FRAMEBUFFER_NAME_MAX];
	const uint64_t *object;
	size_t length;
	size_t index;

	/* Copies the prefix, leaving room for the separator. */
	length = 0;
	while (prefix[length] != '\0') {
		/* Refuses a prefix that leaves no room for a value's name. */
		if (length + 2U >= sizeof(name))
			return EINVAL;

		name[length] = prefix[length];
		length++;
	}
	name[length++] = '.';

	/* Appends the value's own name and the terminator. */
	for (index = 0; value_name[index] != '\0'; index++) {
		/* Refuses a name too long for the buffer. */
		if (length + 1U >= sizeof(name))
			return EINVAL;

		name[length++] = value_name[index];
	}
	name[length] = '\0';

	/* Asks the platform for the value; a missing one means no framebuffer. */
	object = hal_get_arch_handoff(name);
	if (object == NULL)
		return ENOENT;

	/* Succeeded: the value the platform keeps under the name. */
	*value = *object;
	return 0;
}
