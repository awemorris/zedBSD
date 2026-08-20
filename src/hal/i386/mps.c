/* Intel MultiProcessor Specification table discovery for i386. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <hal/hal.h>
#include "apic-topology.h"
#include "defs.h"

struct mp_float {
	char signature[4]; uint32 config; uint8 length, revision, checksum, feature[5];
} __attribute__((packed));
struct mp_header {
	char signature[4]; uint16 length; uint8 revision, checksum; char oem[8];
	char product[12]; uint32 oem_table; uint16 oem_length, entries;
	uint32 lapic; uint16 extended_length; uint8 extended_checksum, reserved;
} __attribute__((packed));
struct mp_cpu { uint8 type, id, version, flags; uint32 signature, features; uint32 reserved[2]; } __attribute__((packed));
struct mp_bus { uint8 type, id; char name[6]; } __attribute__((packed));
struct mp_ioapic { uint8 type, id, version, flags; uint32 address; } __attribute__((packed));
struct mp_interrupt { uint8 type, interrupt_type; uint16 flags; uint8 source_bus, source_irq, destination_apic, destination_irq; } __attribute__((packed));

static const void *phys(uint32 address) { return (const void *)(uintptr_t)(address | SYS_START); }
static int equal(const void *a,const char*b,size_t n){const uint8*p=a;size_t i;for(i=0;i<n;i++)if(p[i]!=(uint8)b[i])return 0;return 1;}
static int checksum(const void *p,size_t n){const uint8*b=p,sum0=0;uint8 sum=sum0;while(n--)sum=(uint8)(sum+*b++);return sum==0;}

static const struct mp_float *scan(uint32 first,uint32 end)
{
	uint32 at;
	for(at=first;at+sizeof(struct mp_float)<=end;at+=16U){
		const struct mp_float*f=phys(at);
		if(equal(f->signature,"_MP_",4)&&f->length==1&&checksum(f,16))return f;
	}
	return NULL;
}

static const struct mp_float *find_float(void)
{
	const uint16 *bda=phys(0x400U); uint32 ebda=(uint32)bda[7]<<4;
	const struct mp_float*f=NULL;
	if(ebda>=0x400U&&ebda<0xa0000U)f=scan(ebda,ebda+1024U);
	if(f==NULL){uint32 base=(uint32)*(const uint16*)phys(0x413U)*1024U;if(base>=1024U&&base<=0xa0000U)f=scan(base-1024U,base);}
	if(f==NULL)f=scan(0xf0000U,0x100000U);
	return f;
}

static int add_route(struct i386_apic_topology*t,const struct mp_interrupt*i)
{
	struct i386_apic_route*r; unsigned polarity=i->flags&3U,trigger=(i->flags>>2)&3U;
	if(i->interrupt_type!=0||i->source_irq>=16||t->route_count>=I386_APIC_ROUTE_MAX)return HAL_OK;
	r=&t->routes[t->route_count++]; r->source_irq=i->source_irq;
	r->ioapic_id=i->destination_apic;r->ioapic_pin=i->destination_irq;
	r->polarity_low=polarity==3U;r->level_triggered=trigger==3U;return HAL_OK;
}

int i386_mps_discover(struct i386_apic_topology *result)
{
	const struct mp_float*f;const struct mp_header*h;const uint8*p,*end;unsigned entry;
	if(result==NULL)
		return HAL_ERR_INVALID;
	hal_memset(result,0,sizeof(*result));
	f=find_float();if(f==NULL||f->config==0)return HAL_ERR_UNSUPPORTED;
	h=phys(f->config);if(!equal(h->signature,"PCMP",4)||h->length<sizeof(*h)||
	   f->config>0x08000000U-h->length||!checksum(h,h->length))return HAL_ERR_INVALID;
	result->lapic_address=h->lapic;result->imcr_present=(f->feature[1]&0x80U)!=0;
	p=(const uint8*)h+sizeof(*h);end=(const uint8*)h+h->length;
	for(entry=0;entry<h->entries;entry++){
		uint8 type,length;if(p>=end)return HAL_ERR_INVALID;type=*p;
		length=type==0?20U:8U;if(p+length>end)return HAL_ERR_INVALID;
		if(type==0){const struct mp_cpu*c=(const void*)p;if((c->flags&1U)!=0){struct i386_apic_cpu*out;if(result->cpu_count>=I386_APIC_MAX_CPUS)return HAL_ERR_UNSUPPORTED;out=&result->cpus[result->cpu_count++];out->apic_id=c->id;out->bootstrap=(c->flags&2U)!=0;}}
		else if(type==2){const struct mp_ioapic*i=(const void*)p;if((i->flags&1U)!=0){struct i386_ioapic_desc*out;if(result->ioapic_count>=I386_IOAPIC_MAX)return HAL_ERR_UNSUPPORTED;out=&result->ioapics[result->ioapic_count++];out->apic_id=i->id;out->address=i->address;out->gsi_base=0;}}
		else if(type==3)(void)add_route(result,(const void*)p);
		else if(type>4)return HAL_ERR_INVALID;
		p+=length;
	}
	if(result->cpu_count==0||result->ioapic_count==0||result->lapic_address==0)return HAL_ERR_UNSUPPORTED;
	return HAL_OK;
}
