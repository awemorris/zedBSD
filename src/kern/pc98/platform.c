/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/platform.h"
#include "kern/block.h"
#include "kern/partition.h"
#include "kern/pc98/partition.h"
#include "kern/pc98/linux-boot.h"
#include "hal/i386/bsp-pc98/display.h"
#include "noct/pc98-beui.h"
#include "drivers/pc98-ide.h"

size_t
kern_platform_init(const struct boots_handoff *handoff,
		   struct boots_device *devices, size_t capacity)
{
	const struct boots_device *initial;
	size_t count = 0;

	if (handoff == NULL || devices == NULL || capacity == 0 ||
	    handoff->magic != BOOTS_HANDOFF_MAGIC || handoff->version != 1 ||
	    handoff->size < sizeof(*handoff) || handoff->device_count == 0 ||
	    handoff->device_table == 0)
		return 0;
	initial = (const struct boots_device *)handoff->device_table;
	for (size_t index = 0; index < handoff->device_count && count < capacity;
	     index++) {
		if ((initial[index].device_class != BOOTS_DEV_FDD &&
		     initial[index].device_class != BOOTS_DEV_IDE &&
		     initial[index].device_class != BOOTS_DEV_SCSI) ||
		    !(initial[index].flags & BOOTS_DEV_PRESENT))
			continue;
		devices[count++] = initial[index];
	}
	if (count == 0)
		return 0;

	boots_partition_set_scheme(&boots_partition_scheme_pc98);
	boots_blkdev_reset();
	(void)boots_ide_pc98_init(devices, (unsigned)count);
	return count;
}

void
kern_platform_refresh_devices(const struct boots_device *devices, size_t count)
{
	(void)boots_ide_pc98_init(devices, (unsigned)count);
}

struct boots_blkdev *
kern_platform_block_device(const struct boots_device *device)
{
	if (device == NULL || device->device_class != BOOTS_DEV_IDE)
		return NULL;
	return boots_ide_pc98_bios_unit(device->bios_id);
}

int
kern_platform_boot_linux(struct boots_filesystem *filesystem,
			 const char *path, const char *arguments,
			 const struct boots_device *devices, unsigned count,
			 int boot_device)
{
	return pc98_linux_boot(filesystem, path, arguments, devices, count,
			       boot_device);
}

int
kern_platform_graphics_init(uint64_t (*milliseconds)(void *),
			    int (*key_state)(void *, int), void (*drain)(void *))
{
	return boots_pc98_beui_init(milliseconds, key_state, drain);
}

void
kern_platform_restore_text(void)
{
	(void)boots_pc98_display_text_restore();
	(void)boots_pc98_beui_clear_graphics();
}

void
kern_platform_debug_write(const char *text)
{
	while (*text) {
		uint8_t character = (uint8_t)*text++;

		asm volatile("outb %0,$0xe9" : : "a"(character));
	}
}

void
kern_platform_halt(void)
{
	for (;;)
		asm volatile("cli; hlt");
}

void
kern_platform_reboot(void)
{
	asm volatile("movb $0x0f,%%al; outb %%al,$0x37" ::: "eax");
	for (;;)
		;
}
