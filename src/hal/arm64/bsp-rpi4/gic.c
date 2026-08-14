#include <hal/hal.h>
#include "../bsp.h"
#include "../defs.h"
#include "gic.h"

#define GICD_CTLR 0x000
#define GICD_TYPER 0x004
#define GICD_IGROUPR 0x080
#define GICD_ISENABLER 0x100
#define GICD_ICENABLER 0x180
#define GICD_ICPENDR 0x280
#define GICD_IPRIORITYR 0x400
#define GICD_ITARGETSR 0x800
#define GICD_ICFGR 0xc00
#define GICC_CTLR 0x000
#define GICC_PMR 0x004
#define GICC_IAR 0x00c
#define GICC_EOIR 0x010

static volatile uint32 *dist;
static volatile uint32 *cpuif;
static uint32 irq_count;

static void write8(volatile uint32 *base, unsigned offset, uint8 value)
{
	volatile uint8 *p=(volatile uint8 *)base;p[offset]=value;
}

void
rpi4_gic_init(void)
{
	const struct rpi4_fdt_info *info=rpi4_boot_info();
	uint32 lines,i;
	if(!info->gic_dist_base||!info->gic_cpu_base)HAL_FATAL("GIC missing from FDT");
	dist=(volatile uint32 *)(ARM64_DIRECT_BASE+(uintptr_t)info->gic_dist_base);
	cpuif=(volatile uint32 *)(ARM64_DIRECT_BASE+(uintptr_t)info->gic_cpu_base);
	dist[GICD_CTLR/4]=0;
	lines=((dist[GICD_TYPER/4]&0x1f)+1)*32;if(lines>1020)lines=1020;irq_count=lines;
	for(i=0;i<(lines+31)/32;i++){
		dist[GICD_ICENABLER/4+i]=0xffffffffU;
		dist[GICD_ICPENDR/4+i]=0xffffffffU;
		dist[GICD_IGROUPR/4+i]=0xffffffffU;
	}
	for(i=0;i<lines;i++)write8(dist,GICD_IPRIORITYR+i,0xa0);
	/* CNTP PPI is Group 0, acknowledged through the primary IAR. */
	dist[GICD_IGROUPR/4]&=~(1U<<30);
	for(i=32;i<lines;i++)write8(dist,GICD_ITARGETSR+i,1);
	for(i=2;i<(lines+15)/16;i++)dist[GICD_ICFGR/4+i]=0;
	cpuif[GICC_PMR/4]=0xff;
	/* Enable both views; on a non-secure CPU bit 0 aliases Group 1. */
	cpuif[GICC_CTLR/4]=3;
	dist[GICD_CTLR/4]=3;
	hal_io_mb();
}

uint32 rpi4_gic_ack(void)
{
	return cpuif[GICC_IAR/4];
}
void rpi4_gic_eoi(uint32 value)
{
	cpuif[GICC_EOIR/4]=value;
	hal_io_mb();
}
void rpi4_gic_mask(uint32 id){if(id<irq_count)dist[GICD_ICENABLER/4+id/32]=1U<<(id&31);}
void rpi4_gic_unmask(uint32 id){if(id<irq_count)dist[GICD_ISENABLER/4+id/32]=1U<<(id&31);}
