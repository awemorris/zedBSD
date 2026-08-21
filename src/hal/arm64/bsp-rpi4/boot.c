#include <hal/hal.h>
#include "../bsp.h"
#include <kern/rpi4/boot.h>

static struct rpi4_fdt_info boot_info;
static uintptr_t boot_fdt;
static struct rpi4_boot_handoff kernel_handoff;

void
rpi4_boot_set_info(const struct rpi4_fdt_info *info, uintptr_t fdt_phys)
{
	boot_info = *info;
	boot_fdt = fdt_phys;
	hal_memset(&kernel_handoff,0,sizeof(kernel_handoff));
	kernel_handoff.common.magic=ZEDBSD_HANDOFF_MAGIC;
	kernel_handoff.common.version=ZEDBSD_HANDOFF_VERSION_MULTIBOOT;
	kernel_handoff.common.size=sizeof(kernel_handoff);
	kernel_handoff.common.device_count=1;
	kernel_handoff.common.boot_bios_id=0x80;
	kernel_handoff.common.boot_partition_scheme=ZEDBSD_PARTITION_SCHEME_MBR;
	kernel_handoff.common.boot_partition_index=1;
	kernel_handoff.common.boot_partition_lba=ZEDBSD_BOOT_PARTITION_LBA_UNKNOWN;
	kernel_handoff.extension_magic=ZEDBSD_RPI4_HANDOFF_MAGIC;
	kernel_handoff.extension_version=1;
	kernel_handoff.extension_size=sizeof(kernel_handoff)-sizeof(kernel_handoff.common);
	kernel_handoff.fdt_phys=fdt_phys;
	kernel_handoff.sdhci_phys=info->sdhci_base;
	kernel_handoff.sdhci_irq=info->sdhci_irq;
}

void
rpi4_boot_set_framebuffer(uint64 phys,uint64 size,uint32 width,uint32 height,
	uint32 pitch,uint32 format)
{
	kernel_handoff.framebuffer_phys=phys;
	kernel_handoff.framebuffer_size=size;
	kernel_handoff.framebuffer_width=width;
	kernel_handoff.framebuffer_height=height;
	kernel_handoff.framebuffer_pitch=pitch;
	kernel_handoff.framebuffer_format=format;
	if(size&&boot_info.reserved_count<RPI4_FDT_MAX_RESERVED){
		boot_info.reserved[boot_info.reserved_count].base=phys;
		boot_info.reserved[boot_info.reserved_count].size=size;
		boot_info.reserved_count++;
	}
}

const struct rpi4_fdt_info *rpi4_boot_info(void) { return &boot_info; }
uintptr_t rpi4_boot_fdt_phys(void) { return boot_fdt; }
const void *rpi4_kernel_handoff(void){return &kernel_handoff;}

void *
hal_get_arch_handoff(const char *name)
{
	(void)name;
	return NULL;
}
