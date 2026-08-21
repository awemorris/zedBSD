#include <errno.h>
#include <hal/hal.h>
#include <kern/disk.h>
#include <kern/platform.h>
#include <kern/sun-disklabel.h>
#include <kern/sun4u/boot.h>
#include "drivers/sun4u-cmd646.h"

size_t kern_platform_init(const struct boot_handoff*h,struct boot_device*d,size_t capacity){const struct sun4u_boot_handoff*s=(const void*)h;if(!h||!d||!capacity||h->magic!=ZEDBSD_HANDOFF_MAGIC||h->version!=ZEDBSD_HANDOFF_VERSION_SUN4U||h->size<sizeof(*s)||s->extension_magic!=ZEDBSD_SUN4U_HANDOFF_MAGIC||s->extension_version!=ZEDBSD_SUN4U_HANDOFF_VERSION||s->ide_vendor!=0x1095||s->ide_device!=0x0646)return 0;partition_set_scheme(&partition_scheme_sun);disk_registry_reset();if(sun4u_cmd646_init(s->ide_primary_command,s->ide_primary_control)!=0)return 0;hal_memset(d,0,sizeof(*d));d->device_class=ZEDBSD_DEV_IDE;d->display_index=0;d->bios_id=0x80;d->flags=ZEDBSD_DEV_PRESENT|ZEDBSD_DEV_BOOT_ORIGIN;d->sector_size=512;return 1;}
void kern_platform_refresh_devices(const struct boot_device*d,size_t n){(void)d;(void)n;}
int kern_platform_input_init(void){return 0;}
struct disk*kern_platform_block_device(const struct boot_device*d){return d&&d->device_class==ZEDBSD_DEV_IDE?sun4u_cmd646_disk():NULL;}
void kern_platform_debug_write(const char*s){if(s)hal_cons_write(s);}void kern_platform_halt(void){(void)hal_irq_disable();for(;;)hal_halt();}void kern_platform_reboot(void){hal_reset();for(;;)hal_halt();}
