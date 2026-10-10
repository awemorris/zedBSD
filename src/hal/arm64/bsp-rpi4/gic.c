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

static volatile uint32_t *dist;
static volatile uint32_t *cpuif;
static uint32_t irq_count;

static void write8(volatile uint32_t *base, unsigned offset, uint8_t value)
{
	volatile uint8_t *p=(volatile uint8_t *)base;p[offset]=value;
}

void
rpi4_gic_init(void)
{
	const struct rpi4_fdt_info *info=rpi4_boot_info();
	uint32_t lines,i;
	if(!info->gic_dist_base||!info->gic_cpu_base)HAL_FATAL("GIC missing from FDT");
	dist=(volatile uint32_t *)(ARM64_DIRECT_BASE+(uintptr_t)info->gic_dist_base);
	cpuif=(volatile uint32_t *)(ARM64_DIRECT_BASE+(uintptr_t)info->gic_cpu_base);
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

uint32_t rpi4_gic_ack(void)
{
	return cpuif[GICC_IAR/4];
}
void rpi4_gic_eoi(uint32_t value)
{
	cpuif[GICC_EOIR/4]=value;
	hal_io_mb();
}
void rpi4_gic_mask(uint32_t id){if(id<irq_count)dist[GICD_ICENABLER/4+id/32]=1U<<(id&31);}
void rpi4_gic_unmask(uint32_t id)
{
	if(id<irq_count){
		/* Direct QEMU boot enters secure EL1, where the primary IAR serves
		 * Group 0.  Firmware normally enters non-secure EL1; there this
		 * group write is ignored and the primary IAR aliases Group 1. */
		dist[GICD_IGROUPR/4+id/32]&=~(1U<<(id&31));
		dist[GICD_ISENABLER/4+id/32]=1U<<(id&31);
	}
}

/*
 * Configures the trigger mode of one disabled interrupt.
 */
int
rpi4_gic_set_trigger(
	uint32_t id,
	int trigger)
{
	uint32_t offset;
	uint32_t mask;
	uint32_t configuration;
	uint32_t requested;
	uint32_t observed;
	bool enabled;

	/* Requires an implemented interrupt and a defined trigger mode. */
	if (dist == NULL || id >= irq_count)
		return HAL_ERR_INVALID;

	/* Refuses an encoding outside the HAL's two trigger modes. */
	if (trigger != HAL_IRQ_TRIGGER_EDGE && trigger != HAL_IRQ_TRIGGER_LEVEL)
		return HAL_ERR_INVALID;

	/* Changes only the trigger bit, preserving the neighboring interrupts. */
	offset = GICD_ICFGR / 4U + id / 16U;
	mask = 1U << ((id % 16U) * 2U + 1U);
	requested = 0;
	if (trigger == HAL_IRQ_TRIGGER_EDGE)
		requested = mask;

	/* Serializes distributor updates while the requested line stays disabled. */
	enabled = hal_irq_disable();
	configuration = dist[offset];
	if ((dist[GICD_ISENABLER / 4U + id / 32U] & (1U << (id % 32U))) != 0) {
		if (enabled)
			hal_irq_enable();
		return HAL_ERR_BUSY;
	}

	/* Preserves fixed SGI/PPI configuration unless it already matches. */
	if (id < 32U) {
		if (enabled)
			hal_irq_enable();

		/* Refuses a private line that would need reprogramming. */
		if ((configuration & mask) != requested)
			return HAL_ERR_UNSUPPORTED;

		/* Succeeded: the private line already has the requested mode. */
		return HAL_OK;
	}

	/* Publishes the SPI trigger and detects a read-only distributor field. */
	dist[offset] = (configuration & ~mask) | requested;
	hal_io_mb();
	observed = dist[offset] & mask;
	if (enabled)
		hal_irq_enable();

	/* Refuses a controller that did not accept the requested trigger. */
	if (observed != requested)
		return HAL_ERR_UNSUPPORTED;

	/* Succeeded: the masked SPI is configured for the next unmask. */
	return HAL_OK;
}
