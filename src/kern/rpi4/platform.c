#include <errno.h>
#include <hal/hal.h>
#include <kern/disk.h>
#include <kern/partition.h>
#include <kern/mbr-partition.h>
#include <kern/platform.h>
#include <kern/rpi4/boot.h>
#include "drivers/rpi4-sdhci.h"

size_t
kern_platform_init(const struct zedbsd_handoff *handoff,
	struct zedbsd_device *devices,size_t capacity)
{
	const struct zedbsd_rpi4_handoff *rpi4=(const void *)handoff;
	struct zedbsd_device *device;
	if(!handoff||!devices||capacity==0||
	   handoff->magic!=ZEDBSD_HANDOFF_MAGIC||
	   handoff->version!=ZEDBSD_HANDOFF_VERSION_MULTIBOOT||
	   handoff->size<sizeof(*rpi4)||
	   rpi4->extension_magic!=ZEDBSD_RPI4_HANDOFF_MAGIC||
	   rpi4->extension_version!=1||
	   rpi4->extension_size<sizeof(*rpi4)-sizeof(rpi4->common)||
	   rpi4->sdhci_phys==0)return 0;
	partition_set_scheme(&partition_scheme_mbr);disk_registry_reset();
	if(rpi4_sdhci_init((uintptr_t)rpi4->sdhci_phys)!=0) {
		/* QEMU raspi4b currently attaches -drive if=sd to the legacy
		 * Arasan controller, while real Pi 4 firmware boots from eMMC2. */
		if(rpi4->sdhci_phys!=0xfe340000ULL||
		   rpi4_sdhci_init(0xfe300000ULL)!=0)return 0;
		hal_puts("sdhci: using QEMU legacy-controller fallback\n");
	}
	device=&devices[0];
	device->device_class=ZEDBSD_DEV_SD;device->display_index=0;
	device->bios_id=0x80;device->flags=ZEDBSD_DEV_PRESENT;
	if(handoff->boot_bios_id==0x80)device->flags|=ZEDBSD_DEV_BOOT_ORIGIN;
	device->sector_size=512;device->cylinders=0;device->heads=0;
	device->sectors=0;device->controller_location=0;
	for(unsigned i=0;i<sizeof(device->reserved);i++)device->reserved[i]=0;
	return 1;
}
void kern_platform_refresh_devices(const struct zedbsd_device*d,size_t n){(void)d;(void)n;}
int kern_platform_input_init(void){return 0;}
struct disk *kern_platform_block_device(const struct zedbsd_device*d)
{return d&&d->device_class==ZEDBSD_DEV_SD?rpi4_sdhci_disk():NULL;}
void kern_platform_debug_write(const char*s){if(s)hal_cons_write(s);}
void kern_platform_halt(void){(void)hal_irq_disable();for(;;)hal_halt();}
void kern_platform_reboot(void){hal_reset();for(;;)hal_halt();}
