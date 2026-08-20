/* PIC/PIT or APIC runtime selection for i386. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <hal/hal.h>
#include "apic-topology.h"
#include "interrupt-controller.h"
#include "ioapic.h"
#include "lapic.h"
#include "pic.h"
#include "asm.h"
#include "smp.h"

static struct i386_apic_topology topology;
static int apic_mode;
static volatile unsigned calibration_active;
static volatile unsigned calibration_stage;
static uint32 timer_ticks;

static void imcr_to_apic(void)
{
	asm_outb(0x22U,0x70U);asm_outb(0x23U,0x01U);
}

int i386_interrupt_select(void)
{
	int error;
#if defined(HAL_BOARD_PC98)
	error=i386_mps_discover(&topology);
#else
	error=i386_acpi_discover(&topology);
	if(error!=HAL_OK)error=i386_mps_discover(&topology);
#endif
	if(error!=HAL_OK||topology.cpu_count<2U)return HAL_ERR_UNSUPPORTED;
	if(i386_lapic_init(topology.lapic_address)!=HAL_OK)return HAL_ERR_UNSUPPORTED;
	/* Measure the local timer against two already configured PIT ticks. */
	calibration_stage=0;calibration_active=1;asm_sti();while(calibration_stage<2U)asm_hlt();asm_cli();calibration_active=0;
	timer_ticks=i386_lapic_timer_elapsed();if(timer_ticks<100U)return HAL_ERR_UNSUPPORTED;
	if(i386_ioapic_init(&topology,i386_lapic_id())!=HAL_OK)return HAL_ERR_UNSUPPORTED;
	/* No legacy interrupt may escape while the routing mode changes. */
	pic_init();if(topology.imcr_present)imcr_to_apic();apic_mode=1;
	i386_lapic_timer_start(timer_ticks);
	i386_smp_configure(&topology);
	return HAL_OK;
}

int i386_interrupt_calibration_tick(void)
{
	if(!calibration_active||apic_mode||calibration_stage>=2U)return 0;
	pic_send_eoi(0);
	if(calibration_stage==0U){i386_lapic_timer_prepare();calibration_stage=1U;}
	else calibration_stage=2U;
	return 1;
}
int i386_interrupt_uses_apic(void){return apic_mode;}
int i386_interrupt_validate(int irq){return apic_mode?irq:pic_get_irq_in_service();}
void i386_interrupt_mask(int irq){if(apic_mode)i386_ioapic_mask(irq);else pic_set_irq_mask(irq,1);}
void i386_interrupt_unmask(int irq){if(apic_mode)i386_ioapic_unmask(irq);else pic_set_irq_mask(irq,0);}
void i386_interrupt_eoi(int irq){if(apic_mode){(void)irq;i386_lapic_eoi();}else pic_send_eoi(irq);}
int i386_interrupt_route(int irq,hal_cpu_id_t cpu){uint8 apic_id;if(!apic_mode)return cpu==0?HAL_OK:HAL_ERR_UNSUPPORTED;if(i386_smp_apic_id(cpu,&apic_id)!=HAL_OK)return HAL_ERR_INVALID;return i386_ioapic_route(irq,apic_id);}
uint32 i386_interrupt_timer_ticks(void){return timer_ticks;}
