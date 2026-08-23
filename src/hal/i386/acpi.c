/* ACPI RSDT/MADT discovery for 32-bit PC/AT. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <hal/hal.h>
#include "apic-topology.h"
#include "defs.h"

struct rsdp { char signature[8];uint8_t checksum;char oem[6];uint8_t revision;uint32_t rsdt;uint32_t length;uint64_t xsdt;uint8_t extended_checksum;uint8_t reserved[3]; } __attribute__((packed));
struct sdt { char signature[4];uint32_t length;uint8_t revision,checksum;char oem[6],table[8];uint32_t oem_revision,creator,creator_revision; } __attribute__((packed));
struct madt { struct sdt header;uint32_t lapic,flags;uint8_t entries[]; } __attribute__((packed));
static const void*phys(uint32_t a){return(const void*)(uintptr_t)(a|SYS_START);}
static int eq(const void*a,const char*b,size_t n){const uint8_t*p=a;size_t i;for(i=0;i<n;i++)if(p[i]!=(uint8_t)b[i])return 0;return 1;}
static int sumok(const void*p,size_t n){const uint8_t*b=p;uint8_t s=0;while(n--)s=(uint8_t)(s+*b++);return s==0;}
static const struct rsdp*scan(uint32_t a,uint32_t e){for(;a+20U<=e;a+=16U){const struct rsdp*r=phys(a);if(eq(r->signature,"RSD PTR ",8)&&sumok(r,20))return r;}return NULL;}
static const struct rsdp*find(void){const uint16_t*b=phys(0x400U);uint32_t e=(uint32_t)b[7]<<4;const struct rsdp*r=NULL;if(e>=0x400U&&e<0xa0000U)r=scan(e,e+1024U);return r?r:scan(0xe0000U,0x100000U);}
static const struct sdt*table(uint32_t a){const struct sdt*s;if(a==0||a>0x08000000U-sizeof(*s))return NULL;s=phys(a);if(s->length<sizeof(*s)||s->length>0x100000U||a>0x08000000U-s->length||!sumok(s,s->length))return NULL;return s;}
int i386_acpi_discover(struct i386_apic_topology*r)
{
	const struct rsdp*p;const struct sdt*root,*s;const struct madt*m=NULL;unsigned i,n;uint8_t overridden[16];const uint8_t*e,*end;
	if(r==NULL)
		return HAL_ERR_INVALID;
	hal_memset(r,0,sizeof(*r));p=find();
	if(p==NULL)return HAL_ERR_UNSUPPORTED;
	hal_memset(overridden,0,sizeof(overridden));
	root=table(p->rsdt);if(root==NULL||!eq(root->signature,"RSDT",4)||(root->length-sizeof(*root))%4U)return HAL_ERR_INVALID;
	n=(root->length-sizeof(*root))/4U;for(i=0;i<n;i++){s=table(((const uint32_t*)((const uint8_t*)root+sizeof(*root)))[i]);if(s&&eq(s->signature,"APIC",4)){m=(const void*)s;break;}}
	if(m==NULL||m->header.length<sizeof(*m))
		return HAL_ERR_UNSUPPORTED;
	r->lapic_address=m->lapic;e=m->entries;end=(const uint8_t*)m+m->header.length;
	while(e+2<=end){uint8_t type=e[0],len=e[1];if(len<2||e+len>end)return HAL_ERR_INVALID;
		if(type==0&&len>=8&&(*(const uint32_t*)(e+4)&3U)){if(r->cpu_count>=I386_APIC_MAX_CPUS)return HAL_ERR_UNSUPPORTED;r->cpus[r->cpu_count].apic_id=e[3];r->cpus[r->cpu_count].bootstrap=0;r->cpu_count++;}
		else if(type==1&&len>=12){struct i386_ioapic_desc*o;if(r->ioapic_count>=I386_IOAPIC_MAX)return HAL_ERR_UNSUPPORTED;o=&r->ioapics[r->ioapic_count++];o->apic_id=e[2];o->address=*(const uint32_t*)(e+4);o->gsi_base=*(const uint32_t*)(e+8);}
		else if(type==2&&len>=10&&e[2]==0&&e[3]<16){struct i386_apic_route*o;if(r->route_count>=I386_APIC_ROUTE_MAX)return HAL_ERR_UNSUPPORTED;o=&r->routes[r->route_count++];o->source_irq=e[3];o->ioapic_id=0xff;o->ioapic_pin=(uint8_t)(*(const uint32_t*)(e+4));o->polarity_low=(*(const uint16_t*)(e+8)&3U)==3U;o->level_triggered=((*(const uint16_t*)(e+8)>>2)&3U)==3U;overridden[e[3]]=1;}
		e+=len;}
	for(i=0;i<16;i++)if(!overridden[i]){struct i386_apic_route*o;if(r->route_count>=I386_APIC_ROUTE_MAX)return HAL_ERR_UNSUPPORTED;o=&r->routes[r->route_count++];o->source_irq=(uint8_t)i;o->ioapic_id=0xff;o->ioapic_pin=(uint8_t)i;o->polarity_low=0;o->level_triggered=0;}
	if(r->cpu_count==0||r->ioapic_count==0||r->lapic_address==0)
		return HAL_ERR_UNSUPPORTED;
	return HAL_OK;
}
