#include <errno.h>
#include <hal/hal.h>
#include <kern/disk.h>
#include <kern/platform.h>
#include <kern/sun-disklabel.h>
#include <kern/sun4u/boot.h>
#include "drivers/sun4u-cmd646.h"

size_t kern_platform_init(const struct zedbsd_handoff*h,struct zedbsd_device*d,size_t capacity){const struct zedbsd_sun4u_handoff*s=(const void*)h;if(!h||!d||!capacity||h->magic!=ZEDBSD_HANDOFF_MAGIC||h->version!=ZEDBSD_HANDOFF_VERSION_SUN4U||h->size<sizeof(*s)||s->extension_magic!=ZEDBSD_SUN4U_HANDOFF_MAGIC||s->extension_version!=ZEDBSD_SUN4U_HANDOFF_VERSION||s->ide_vendor!=0x1095||s->ide_device!=0x0646)return 0;partition_set_scheme(&partition_scheme_sun);disk_registry_reset();if(sun4u_cmd646_init(s->ide_primary_command,s->ide_primary_control)!=0)return 0;hal_memset(d,0,sizeof(*d));d->device_class=ZEDBSD_DEV_IDE;d->display_index=0;d->bios_id=0x80;d->flags=ZEDBSD_DEV_PRESENT|ZEDBSD_DEV_BOOT_ORIGIN;d->sector_size=512;return 1;}
void kern_platform_refresh_devices(const struct zedbsd_device*d,size_t n){(void)d;(void)n;}
struct disk*kern_platform_block_device(const struct zedbsd_device*d){return d&&d->device_class==ZEDBSD_DEV_IDE?sun4u_cmd646_disk():NULL;}
int kern_platform_boot_linux(struct zedbsd_filesystem*f,const char*p,const char*a,const struct zedbsd_device*d,unsigned n,int b){(void)f;(void)p;(void)a;(void)d;(void)n;(void)b;return EOPNOTSUPP;}
int kern_platform_graphics_init(uint64_t(*m)(void*),int(*k)(void*,int),void(*d)(void*)){(void)m;(void)k;(void)d;return EOPNOTSUPP;}
int kern_platform_graphics_enter(struct kern_graphics_mode*m){(void)m;return EOPNOTSUPP;}int kern_platform_graphics_clear(void){return EOPNOTSUPP;}void kern_platform_graphics_leave(void){}
int kern_platform_graphics_fill(const struct kern_graphics_rect*r,uint32_t c){(void)r;(void)c;return EOPNOTSUPP;}int kern_platform_graphics_line(unsigned a,unsigned b,unsigned c,unsigned d,uint32_t e){(void)a;(void)b;(void)c;(void)d;(void)e;return EOPNOTSUPP;}int kern_platform_graphics_pattern_fill(const struct kern_graphics_rect*r,uint32_t c,uint64_t p){(void)r;(void)c;(void)p;return EOPNOTSUPP;}int kern_platform_graphics_blit(unsigned x,unsigned y,const struct kern_graphics_image*i,uint64_t k,int t){(void)x;(void)y;(void)i;(void)k;(void)t;return EOPNOTSUPP;}int kern_platform_graphics_flush(const struct kern_graphics_rect*r,size_t n){(void)r;(void)n;return EOPNOTSUPP;}int kern_platform_graphics_get_glyph(uint32_t c,uint8_t b[32],unsigned*w,unsigned*h){(void)c;(void)b;(void)w;(void)h;return EOPNOTSUPP;}
void kern_platform_restore_text(void){}void kern_platform_debug_write(const char*s){if(s)hal_cons_write(s);}void kern_platform_halt(void){(void)hal_irq_disable();for(;;)hal_halt();}void kern_platform_reboot(void){hal_reset();for(;;)hal_halt();}
