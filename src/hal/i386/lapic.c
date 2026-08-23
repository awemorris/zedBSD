/* xAPIC implementation shared by the i386 PC/AT and PC-98 BSPs. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <hal/hal.h>
#include "defs.h"
#include "lapic.h"

#define LAPIC_ID 0x020U
#define LAPIC_EOI 0x0b0U
#define LAPIC_SVR 0x0f0U
#define LAPIC_ESR 0x280U
#define LAPIC_ICR_LOW 0x300U
#define LAPIC_ICR_HIGH 0x310U
#define LAPIC_LVT_TIMER 0x320U
#define LAPIC_TIMER_INITIAL 0x380U
#define LAPIC_TIMER_CURRENT 0x390U
#define LAPIC_TIMER_DIVIDE 0x3e0U
#define LAPIC_ENABLE 0x100U
#define LAPIC_MASKED 0x10000U
#define LAPIC_PERIODIC 0x20000U
#define LAPIC_TIMER_VECTOR 0xe0U
#define LAPIC_SPURIOUS_VECTOR 0xdfU

static volatile uint32_t *base;
static uint32_t rd(unsigned r){return base[r/4U];}
static void wr(unsigned r,uint32_t v){base[r/4U]=v;hal_io_mb();}
static int wait_icr(void){unsigned n;for(n=0;n<1000000U;n++){if(!(rd(LAPIC_ICR_LOW)&(1U<<12)))return HAL_OK;__asm__ volatile("pause");}return HAL_ERR_TIMEOUT;}

int i386_lapic_init(uint32_t address)
{
	uint32_t lo,hi; if(address<0xfec00000U||address>=0xff000000U||(address&0xfffU))return HAL_ERR_UNSUPPORTED;
	base=(volatile uint32_t*)(uintptr_t)address;
	__asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(0x1bU));
	lo=(lo&0x00000fffU)|address|(1U<<11);__asm__ volatile("wrmsr"::"a"(lo),"d"(hi),"c"(0x1bU));
	i386_lapic_init_cpu();return HAL_OK;
}
void i386_lapic_init_cpu(void){wr(LAPIC_SVR,LAPIC_ENABLE|LAPIC_SPURIOUS_VECTOR);wr(LAPIC_ESR,0);wr(LAPIC_ESR,0);wr(LAPIC_LVT_TIMER,LAPIC_MASKED|LAPIC_TIMER_VECTOR);wr(LAPIC_TIMER_DIVIDE,3U);wr(LAPIC_EOI,0);}
uint8_t i386_lapic_id(void){return(uint8_t)(rd(LAPIC_ID)>>24);}
void i386_lapic_eoi(void){wr(LAPIC_EOI,0);}
static int send(uint8_t id,uint32_t low){if(wait_icr()!=HAL_OK)return HAL_ERR_TIMEOUT;wr(LAPIC_ICR_HIGH,(uint32_t)id<<24);wr(LAPIC_ICR_LOW,low);return wait_icr();}
int i386_lapic_send_init(uint8_t id){volatile unsigned n;int e=send(id,0x0000c500U);if(e!=HAL_OK)return e;for(n=0;n<100000U;n++)__asm__ volatile("pause");return send(id,0x00008500U);}
int i386_lapic_send_startup(uint8_t id,uint8_t vector){return send(id,0x00004600U|vector);}
int i386_lapic_send_fixed(uint8_t id,uint8_t vector){return send(id,vector);}
void i386_lapic_timer_prepare(void){wr(LAPIC_TIMER_DIVIDE,3U);wr(LAPIC_LVT_TIMER,LAPIC_MASKED|LAPIC_TIMER_VECTOR);wr(LAPIC_TIMER_INITIAL,0xffffffffU);}
uint32_t i386_lapic_timer_elapsed(void){return 0xffffffffU-rd(LAPIC_TIMER_CURRENT);}
void i386_lapic_timer_start(uint32_t ticks){wr(LAPIC_TIMER_DIVIDE,3U);wr(LAPIC_LVT_TIMER,LAPIC_PERIODIC|LAPIC_TIMER_VECTOR);wr(LAPIC_TIMER_INITIAL,ticks?ticks:1U);}
void i386_lapic_timer_stop(void){wr(LAPIC_LVT_TIMER,LAPIC_MASKED|LAPIC_TIMER_VECTOR);wr(LAPIC_TIMER_INITIAL,0);}
