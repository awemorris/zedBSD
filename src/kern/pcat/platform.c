/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/platform.h"
#include "kern/disk.h"
#include "kern/partition.h"
#include "kern/mbr-partition.h"
#include "drivers/pcat-ide.h"
#include <errno.h>
#include <hal/hal.h>

size_t
kern_platform_init(const struct zedbsd_handoff *handoff,
    struct zedbsd_device *devices, size_t capacity)
{
	size_t count = 0;
	if (handoff == 0 || devices == 0 || capacity == 0 ||
	    handoff->magic != ZEDBSD_HANDOFF_MAGIC ||
	    handoff->version != ZEDBSD_HANDOFF_VERSION_MULTIBOOT) return 0;
	partition_set_scheme(&partition_scheme_mbr);
	disk_registry_reset();
	(void)zedbsd_ide_pcat_init();
	for (unsigned slot = 0; slot < 4U && count < capacity; slot++) {
		struct disk *disk = zedbsd_ide_pcat_bios_unit((uint8_t)(0x80U+slot));
		struct zedbsd_device *device;
		if (disk == 0) continue;
		device = &devices[count];
		device->device_class=ZEDBSD_DEV_IDE; device->display_index=(uint8_t)count;
		device->bios_id=(uint8_t)(0x80U+slot); device->flags=ZEDBSD_DEV_PRESENT;
		if (device->bios_id == handoff->boot_bios_id)
			device->flags |= ZEDBSD_DEV_BOOT_ORIGIN;
		device->sector_size=512; device->cylinders=0; device->heads=0;
		device->sectors=0; device->controller_location=(uint8_t)slot;
		for (unsigned i=0;i<sizeof(device->reserved);i++) device->reserved[i]=0;
		count++;
	}
	return count;
}

void kern_platform_refresh_devices(const struct zedbsd_device *d, size_t n)
{ (void)d; (void)n; }

struct disk *kern_platform_block_device(const struct zedbsd_device *device)
{
	if (device == 0 || device->device_class != ZEDBSD_DEV_IDE) return 0;
	return zedbsd_ide_pcat_bios_unit(device->bios_id);
}

int kern_platform_boot_linux(struct zedbsd_filesystem *fs, const char *path,
    const char *args, const struct zedbsd_device *devices, unsigned count,
    int boot_device)
{
	(void)fs; (void)path; (void)args; (void)devices; (void)count;
	(void)boot_device; return EOPNOTSUPP;
}

int kern_platform_graphics_init(uint64_t (*ms)(void *),
    int (*key)(void *, int), void (*drain)(void *))
{ (void)ms; (void)key; (void)drain; return 0; }
int kern_platform_graphics_enter(struct kern_graphics_mode *m)
{ (void)m; return ENODEV; }
int kern_platform_graphics_clear(void) { return ENODEV; }
void kern_platform_graphics_leave(void) { }
int kern_platform_graphics_fill(const struct kern_graphics_rect *r, uint32_t c)
{ (void)r; (void)c; return ENODEV; }
int kern_platform_graphics_line(unsigned a,unsigned b,unsigned c,unsigned d,uint32_t e)
{ (void)a;(void)b;(void)c;(void)d;(void)e;return ENODEV; }
int kern_platform_graphics_pattern_fill(const struct kern_graphics_rect *r,
    uint32_t c,uint64_t p) { (void)r;(void)c;(void)p;return ENODEV; }
int kern_platform_graphics_blit(unsigned x,unsigned y,
    const struct kern_graphics_image *i,uint64_t m,int t)
{ (void)x;(void)y;(void)i;(void)m;(void)t;return ENODEV; }
int kern_platform_graphics_flush(const struct kern_graphics_rect *r,size_t n)
{ (void)r;(void)n;return ENODEV; }
void kern_platform_restore_text(void) { hal_cons_reset(); }
void kern_platform_debug_write(const char *text)
{
	while (text != 0 && *text != '\0') {
		uint8_t c=(uint8_t)*text++; hal_cons_putc(c);
#ifdef HAL_PCAT_DEBUGCON
		__asm__ volatile("outb %0,$0xe9" : : "a"(c));
#endif
	}
}
void kern_platform_halt(void) { for (;;) __asm__ volatile("cli; hlt"); }
void kern_platform_reboot(void)
{
	for (unsigned spin=0;spin<1000000U;spin++) {
		uint8_t status; __asm__ volatile("inb $0x64,%0":"=a"(status));
		if (!(status&2U)) break;
	}
	__asm__ volatile("movb $0xfe,%%al; outb %%al,$0x64":::"eax");
	kern_platform_halt();
}
