#include <hal/hal.h>
#include "../defs.h"
#include "mailbox.h"

#define MBOX_READ 0x00U
#define MBOX_STATUS 0x18U
#define MBOX_WRITE 0x20U
#define MBOX_FULL 0x80000000U
#define MBOX_EMPTY 0x40000000U
#define MBOX_PROPERTY 8U

static uint64_t now(void){uint64_t v;__asm__ volatile("mrs %0,cntpct_el0":"=r"(v));return v;}
static uint64_t freq(void){uint64_t v;__asm__ volatile("mrs %0,cntfrq_el0":"=r"(v));return v;}

int
rpi4_mailbox_property(uintptr_t mailbox_phys,uint32_t *message,size_t bytes)
{
	volatile uint8_t *base=(volatile uint8_t *)(ARM64_DIRECT_BASE+mailbox_phys);
	uintptr_t virtual_address=(uintptr_t)message;
	uint64_t physical;
	uint32_t request;
	uint64_t deadline;
	if(!message||bytes<12||(virtual_address&15U)||
	   virtual_address<ARM64_DIRECT_BASE)return -1;
	physical=virtual_address-ARM64_DIRECT_BASE;
	if(physical>=0x40000000ULL)return -1;
	request=(uint32_t)(physical|0xc0000000U)|MBOX_PROPERTY;
	hal_dcache_clean_range(virtual_address,bytes);
	deadline=now()+freq();
	while(hal_mmio_read32(base+MBOX_STATUS)&MBOX_FULL)
		if(now()>=deadline)return -1;
	hal_mmio_write32(base+MBOX_WRITE,request);
	for(;;){
		while(hal_mmio_read32(base+MBOX_STATUS)&MBOX_EMPTY)
			if(now()>=deadline)return -1;
		if(hal_mmio_read32(base+MBOX_READ)==request)break;
		if(now()>=deadline)return -1;
	}
	hal_dcache_invalidate_range(virtual_address,bytes);
	return message[1]==0x80000000U?0:-1;
}
