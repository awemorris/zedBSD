/* I/O APIC implementation shared by the i386 PC/AT and PC-98 BSPs. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <hal/hal.h>
#include "defs.h"
#include "ioapic.h"

struct controller{volatile uint32*base;uint8 id;uint32 gsi_base,entries;};
struct route{struct controller*io;uint8 pin,low,level,destination,present;};
static struct controller controllers[I386_IOAPIC_MAX];static struct route routes[16];static unsigned count;static volatile unsigned lock;
static uint32 rd(struct controller*c,uint8 r){c->base[0]=r;hal_io_mb();return c->base[4];}
static void wr(struct controller*c,uint8 r,uint32 v){c->base[0]=r;hal_io_mb();c->base[4]=v;hal_io_mb();}
static struct controller*by_id(uint8 id){unsigned i;for(i=0;i<count;i++)if(controllers[i].id==id)return&controllers[i];return NULL;}
static struct controller*by_gsi(uint32 g,unsigned*pin){unsigned i;for(i=0;i<count;i++)if(g>=controllers[i].gsi_base&&g-controllers[i].gsi_base<controllers[i].entries){*pin=g-controllers[i].gsi_base;return&controllers[i];}return NULL;}
static void write_route(int irq,int masked){struct route*r=&routes[irq];uint32 low=(uint32)(0xe0+irq);bool enabled;if(!r->present)return;if(r->low)low|=1U<<13;if(r->level)low|=1U<<15;if(masked)low|=1U<<16;enabled=hal_irq_disable();while(__atomic_exchange_n(&lock,1U,__ATOMIC_ACQUIRE))__asm__ volatile("pause");wr(r->io,(uint8)(0x11U+r->pin*2U),(uint32)r->destination<<24);wr(r->io,(uint8)(0x10U+r->pin*2U),low);__atomic_store_n(&lock,0U,__ATOMIC_RELEASE);if(enabled)hal_irq_enable();}
int i386_ioapic_init(const struct i386_apic_topology*t,uint8 destination)
{
	unsigned i;if(t==NULL||t->ioapic_count==0)return HAL_ERR_INVALID;hal_memset(routes,0,sizeof(routes));count=t->ioapic_count;
	for(i=0;i<count;i++){controllers[i].base=(volatile uint32*)(uintptr_t)t->ioapics[i].address;controllers[i].id=t->ioapics[i].apic_id;controllers[i].gsi_base=t->ioapics[i].gsi_base;controllers[i].entries=((rd(&controllers[i],1)>>16)&0xffU)+1U;}
	for(i=0;i<t->route_count;i++){const struct i386_apic_route*x=&t->routes[i];struct route*r;unsigned pin=x->ioapic_pin;struct controller*c;if(x->source_irq>=16)continue;c=x->ioapic_id==0xffU?by_gsi(pin,&pin):by_id(x->ioapic_id);if(c==NULL||pin>=c->entries)return HAL_ERR_INVALID;r=&routes[x->source_irq];r->io=c;r->pin=(uint8)pin;r->low=x->polarity_low;r->level=x->level_triggered;r->destination=destination;r->present=1;write_route((int)x->source_irq,1);}
	return HAL_OK;
}
void i386_ioapic_mask(int irq){if(irq>=0&&irq<16)write_route(irq,1);}
void i386_ioapic_unmask(int irq){if(irq>=0&&irq<16)write_route(irq,0);}
int i386_ioapic_route(int irq,uint8 cpu){if(irq<0||irq>=16||!routes[irq].present)return HAL_ERR_INVALID;routes[irq].destination=cpu;write_route(irq,1);return HAL_OK;}
