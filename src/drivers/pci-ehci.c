/*
 * PCI EHCI host controller driver
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#include <drivers/pci-ehci.h>
#include <drivers/pci.h>
#include <drivers/usb.h>
#include <errno.h>
#include <hal/hal.h>
#include <kern/sched.h>
#include <string.h>

#define EHCI_USBCMD 0x00U
#define EHCI_USBSTS 0x04U
#define EHCI_USBINTR 0x08U
#define EHCI_FRINDEX 0x0cU
#define EHCI_CTRLDSSEGMENT 0x10U
#define EHCI_PERIODICLISTBASE 0x14U
#define EHCI_ASYNCLISTADDR 0x18U
#define EHCI_CONFIGFLAG 0x40U
#define EHCI_PORTSC(n) (0x44U+4U*(n))
#define EHCI_CMD_RUN 0x00000001U
#define EHCI_CMD_RESET 0x00000002U
#define EHCI_CMD_PERIODIC 0x00000010U
#define EHCI_CMD_ASYNC 0x00000020U
#define EHCI_STS_HALTED 0x00001000U
#define EHCI_STS_ALL 0x0000003fU
#define EHCI_LINK_TERM 0x00000001U
#define EHCI_LINK_QH 0x00000002U
#define EHCI_QTD_ACTIVE 0x00000080U
#define EHCI_QTD_HALTED 0x00000040U
#define EHCI_QTD_ERRORS 0x0000007eU
#define EHCI_QTD_IOC 0x00008000U
#define EHCI_PID_OUT 0U
#define EHCI_PID_IN 1U
#define EHCI_PID_SETUP 2U
#define EHCI_PORT_CONNECT 0x00000001U
#define EHCI_PORT_CONNECT_CHANGE 0x00000002U
#define EHCI_PORT_ENABLE 0x00000004U
#define EHCI_PORT_ENABLE_CHANGE 0x00000008U
#define EHCI_PORT_RESET 0x00000100U
#define EHCI_PORT_POWER 0x00001000U
#define EHCI_PORT_OWNER 0x00002000U
#define EHCI_MAX_QTDS 124U
#define EHCI_PCI_COMMAND 0x04U
#define EHCI_PCI_COMMAND_MASTER 0x0004U
#define EHCI_QUIESCE_TICKS 100U

struct ehci_qtd {
	volatile uint32_t next, alternate, token, buffer[5];
};
struct ehci_qh {
	volatile uint32_t horizontal, characteristics, capabilities, current;
	volatile uint32_t next, alternate, token, buffer[5];
};
struct ehci_request {
	struct drv_usb_urb *urb;
	struct drv_dma_buffer schedule, bounce;
	struct ehci_qh *qh;
	struct ehci_qtd *qtds;
	unsigned qtd_count, data_first, data_count;
	uint16_t requested[EHCI_MAX_QTDS];
	bool input, control;
};
struct ehci_controller {
	struct drv_pci_device *pci;
	struct drv_pci_mapping registers;
	struct drv_pci_enable_state pci_enable_state;
	volatile uint8_t *capability,*operational;
	struct drv_dma_buffer periodic, async_head_memory;
	struct ehci_qh *async_head;
	struct drv_usb_hcd hcd;
	struct drv_usb_bus *bus;
	struct drv_pci_irq irq;
	void *irq_cookie;
	struct ehci_request *active;
	struct ehci_controller *next;
	unsigned bar_claimed, bar_mapped, pci_state_saved;
	unsigned hcd_registered, irq_allocated, dma_quiesced;
	unsigned listed, quarantined;
};
static struct ehci_controller *ehci_controllers;

static uint8_t rd8(volatile uint8_t*p,unsigned o){return p[o];}
static uint32_t rd32(volatile uint8_t*p,unsigned o){return *(volatile uint32_t*)(p+o);}
static void wr32(volatile uint8_t*p,unsigned o,uint32_t v){*(volatile uint32_t*)(p+o)=v;hal_io_mb();}
static struct ehci_controller*hcd_controller(struct drv_usb_hcd*h){return(void*)h->private_data[0];}

static int ehci_ownership(struct ehci_controller*c)
{
	uint32_t hcc=rd32(c->capability,8U);unsigned eecp=(hcc>>8)&0xffU,guard=0;
	while(eecp>=0x40U&&guard++<32U){uint32_t cap;if(drv_pci_device_config_read32(c->pci,eecp,&cap)!=0)return EIO;if((cap&0xffU)==1U){cap|=0x01000000U;if(drv_pci_device_config_write32(c->pci,eecp,cap)!=0)return EIO;for(guard=0;guard<1000000U;guard++){if(drv_pci_device_config_read32(c->pci,eecp,&cap)!=0)return EIO;if((cap&0x00010000U)==0)return 0;}return ETIMEDOUT;}eecp=(cap>>8)&0xffU;}
	return 0;
}

static int ehci_start(struct drv_usb_hcd*hcd)
{
	struct ehci_controller*c=hcd_controller(hcd);uint32_t*periodic;unsigned i,timeout;int error;
	if((error=ehci_ownership(c))!=0)return error;
	if((error=drv_dma_alloc_coherent(hcd->dma,4096U,4096U,&c->periodic))!=0)return error;
	if((error=drv_dma_alloc_coherent(hcd->dma,4096U,64U,&c->async_head_memory))!=0){drv_dma_free_coherent(hcd->dma,&c->periodic);return error;}
	periodic=c->periodic.address;for(i=0;i<1024U;i++)periodic[i]=EHCI_LINK_TERM;
	memset(c->async_head_memory.address,0,4096U);c->async_head=c->async_head_memory.address;
	c->async_head->horizontal=(uint32_t)c->async_head_memory.device_address|EHCI_LINK_QH;
	c->async_head->characteristics=(1U<<15)|(2U<<12)|(64U<<16);
	c->async_head->capabilities=1U<<30;c->async_head->next=EHCI_LINK_TERM;c->async_head->alternate=EHCI_LINK_TERM;
	wr32(c->operational,EHCI_USBCMD,rd32(c->operational,EHCI_USBCMD)&~EHCI_CMD_RUN);
	wr32(c->operational,EHCI_USBCMD,EHCI_CMD_RESET);
	for(timeout=0;timeout<1000000U;timeout++)if((rd32(c->operational,EHCI_USBCMD)&EHCI_CMD_RESET)==0)break;
	if(timeout==1000000U){error=ETIMEDOUT;goto fail;}
	wr32(c->operational,EHCI_CTRLDSSEGMENT,0);wr32(c->operational,EHCI_PERIODICLISTBASE,(uint32_t)c->periodic.device_address);
	wr32(c->operational,EHCI_ASYNCLISTADDR,(uint32_t)c->async_head_memory.device_address);
	wr32(c->operational,EHCI_USBSTS,EHCI_STS_ALL);wr32(c->operational,EHCI_USBINTR,0);
	wr32(c->operational,EHCI_CONFIGFLAG,1);wr32(c->operational,EHCI_USBCMD,EHCI_CMD_RUN|EHCI_CMD_ASYNC);
	for(timeout=0;timeout<1000000U;timeout++)if((rd32(c->operational,EHCI_USBSTS)&EHCI_STS_HALTED)==0){c->dma_quiesced=0;return 0;}
	error=EIO;
fail:drv_dma_free_coherent(hcd->dma,&c->async_head_memory);drv_dma_free_coherent(hcd->dma,&c->periodic);return error;
}

static int
ehci_bus_master_disable(struct ehci_controller *c)
{
	uint16_t command;
	int error;

	error = drv_pci_device_set_bus_master(c->pci, false);
	if (error != 0)
		return error;
	error = drv_pci_device_config_read16(c->pci, EHCI_PCI_COMMAND,
	    &command);
	if (error != 0)
		return error;
	return (command & EHCI_PCI_COMMAND_MASTER) == 0 ? 0 : EIO;
}

static int
ehci_irq_disestablish(struct ehci_controller *c)
{
	uint64_t started;
	int error;

	if (c->irq_cookie == NULL)
		return 0;
	started = sched_ticks();
	for (;;) {
		error = drv_pci_device_disestablish_irq_checked(c->pci,
		    c->irq_cookie);
		if (error != EBUSY)
			break;
		if (sched_ticks() - started >= EHCI_QUIESCE_TICKS) {
			hal_printf(
			    "ehci: IRQ removal timed out; retaining controller ownership\n");
			return EBUSY;
		}
		sched_yield();
	}
	if (error != 0) {
		hal_printf(
		    "ehci: checked IRQ removal failed (%d); retaining controller ownership\n",
		    error);
		return error;
	}
	c->irq_cookie = NULL;
	return 0;
}

static int
ehci_quiesce(struct drv_usb_hcd *hcd)
{
	struct ehci_controller *c = hcd_controller(hcd);
	uint64_t started;
	uint32_t command;
	int halt_error = 0, master_error, irq_error;

	if (c->dma_quiesced)
		return 0;
	if (c->active != NULL)
		return EBUSY;
	wr32(c->operational, EHCI_USBINTR, 0);
	command = rd32(c->operational, EHCI_USBCMD);
	wr32(c->operational, EHCI_USBCMD,
	    command & ~(EHCI_CMD_RUN | EHCI_CMD_PERIODIC | EHCI_CMD_ASYNC));
	started = sched_ticks();
	while ((rd32(c->operational, EHCI_USBSTS) & EHCI_STS_HALTED) == 0) {
		if (sched_ticks() - started >= EHCI_QUIESCE_TICKS) {
			halt_error = ETIMEDOUT;
			break;
		}
		sched_yield();
	}
	master_error = ehci_bus_master_disable(c);
	if (halt_error != 0) {
		hal_printf(
		    "ehci: controller halt timed out (master=%d); retaining DMA and IRQ state\n",
		    master_error);
		return halt_error;
	}
	if (master_error != 0) {
		hal_printf(
		    "ehci: bus-master disable failed (%d); retaining DMA and IRQ state\n",
		    master_error);
		return master_error;
	}
	irq_error = ehci_irq_disestablish(c);
	if (irq_error != 0)
		return irq_error;
	c->dma_quiesced = 1;
	return 0;
}

static void
ehci_stop(struct drv_usb_hcd *hcd)
{
	struct ehci_controller *c = hcd_controller(hcd);

	if (!c->dma_quiesced) {
		hal_printf("ehci: refusing to release DMA before checked quiesce\n");
		c->quarantined = 1;
		return;
	}
	if (c->async_head_memory.address) {
		drv_dma_free_coherent(hcd->dma, &c->async_head_memory);
		memset(&c->async_head_memory, 0,
		    sizeof(c->async_head_memory));
	}
	if (c->periodic.address) {
		drv_dma_free_coherent(hcd->dma, &c->periodic);
		memset(&c->periodic, 0, sizeof(c->periodic));
	}
	c->async_head = NULL;
}

static uint32_t qtd_token(unsigned pid,unsigned toggle,unsigned length){return EHCI_QTD_ACTIVE|(3U<<10)|(pid<<8)|((uint32_t)length<<16)|((uint32_t)toggle<<31);}
static void qtd_buffer(struct ehci_qtd*q,uint32_t address,size_t length){unsigned i,pages;q->buffer[0]=address;pages=(unsigned)(((address&0xfffU)+length+4095U)/4096U);for(i=1;i<5&&i<pages;i++)q->buffer[i]=(address+(uint32_t)i*4096U)&~0xfffU;}
static int add_qtd(struct ehci_request*r,unsigned pid,unsigned toggle,unsigned length,uint32_t buffer)
{struct ehci_qtd*q;uint32_t physical;if(r->qtd_count>=EHCI_MAX_QTDS)return E2BIG;q=&r->qtds[r->qtd_count];physical=(uint32_t)r->schedule.device_address+128U+r->qtd_count*sizeof(*q);if(r->qtd_count)r->qtds[r->qtd_count-1U].next=physical;q->next=EHCI_LINK_TERM;q->alternate=EHCI_LINK_TERM;q->token=qtd_token(pid,toggle,length);qtd_buffer(q,buffer,length);r->requested[r->qtd_count]=(uint16_t)length;r->qtd_count++;return 0;}
static void request_free(struct ehci_controller*c,struct ehci_request*r){if(!r)return;if(r->schedule.address)drv_dma_free_coherent(c->hcd.dma,&r->schedule);if(r->bounce.address)drv_dma_free_coherent(c->hcd.dma,&r->bounce);hal_free(r);}
static int build_request(struct ehci_controller*c,struct drv_usb_urb*urb,struct ehci_request**result)
{
	struct ehci_request*r;struct drv_usb_endpoint*ep=drv_usb_urb_endpoint(urb);const struct drv_usb_control_request*control=drv_usb_urb_control_request(urb);size_t length=drv_usb_urb_length(urb),offset=0;unsigned packet,address,endpoint,toggle=0,initial_toggle=0;int error;
	r=hal_malloc(sizeof(*r));if(!r)return ENOMEM;memset(r,0,sizeof(*r));r->urb=urb;
	if((error=drv_dma_alloc_coherent(c->hcd.dma,4096U,64U,&r->schedule))!=0){hal_free(r);return error;}
	if((error=drv_dma_alloc_coherent(c->hcd.dma,length+8U,64U,&r->bounce))!=0){request_free(c,r);return error;}
	memset(r->schedule.address,0,4096U);r->qh=r->schedule.address;r->qtds=(void*)((uint8_t*)r->schedule.address+128U);
	address=drv_usb_device_address(drv_usb_urb_device(urb));endpoint=drv_usb_endpoint_address(ep)&15U;packet=drv_usb_endpoint_max_packet_size(ep);if(!packet)packet=64U;
	r->control=control!=NULL;if(control){memcpy(r->bounce.address,control,8U);if((error=add_qtd(r,EHCI_PID_SETUP,0,8U,(uint32_t)r->bounce.device_address))!=0)goto fail;r->input=(control->request_type&DRV_USB_DIR_IN)!=0;if(!r->input&&length)memcpy((uint8_t*)r->bounce.address+8U,drv_usb_urb_buffer(urb),length);r->data_first=r->qtd_count;toggle=1;while(offset<length){unsigned chunk=length-offset>0x4000U?0x4000U:(unsigned)(length-offset);if((error=add_qtd(r,r->input?EHCI_PID_IN:EHCI_PID_OUT,toggle,chunk,(uint32_t)(r->bounce.device_address+8U+offset)))!=0)goto fail;offset+=chunk;toggle^=1U;r->data_count++;}if((error=add_qtd(r,r->input?EHCI_PID_OUT:EHCI_PID_IN,1,0,0))!=0)goto fail;
	}else{r->input=drv_usb_endpoint_is_input(ep);initial_toggle=(unsigned)drv_usb_endpoint_hcd_data(ep,0)&1U;toggle=initial_toggle;if(!r->input&&length)memcpy((uint8_t*)r->bounce.address+8U,drv_usb_urb_buffer(urb),length);r->data_first=0;while(offset<length){unsigned chunk=length-offset>0x4000U?0x4000U:(unsigned)(length-offset);if((error=add_qtd(r,r->input?EHCI_PID_IN:EHCI_PID_OUT,toggle,chunk,(uint32_t)(r->bounce.device_address+8U+offset)))!=0)goto fail;offset+=chunk;toggle^=1U;r->data_count++;}if(!length&&add_qtd(r,r->input?EHCI_PID_IN:EHCI_PID_OUT,toggle,0,0)!=0){error=E2BIG;goto fail;}r->data_count=r->qtd_count;}
	r->qtds[r->qtd_count-1U].token|=EHCI_QTD_IOC;r->qh->horizontal=(uint32_t)c->async_head_memory.device_address|EHCI_LINK_QH;
	r->qh->characteristics=(address&0x7fU)|((endpoint&15U)<<8)|(2U<<12)|(r->control?(1U<<14):0U)|((packet&0x7ffU)<<16)|(4U<<28);r->qh->capabilities=1U<<30;r->qh->next=(uint32_t)r->schedule.device_address+128U;r->qh->alternate=EHCI_LINK_TERM;r->qh->token=r->control?0U:((uint32_t)initial_toggle<<31);*result=r;return 0;
fail:request_free(c,r);return error;
}
static int ehci_urb_enqueue(struct drv_usb_hcd*hcd,struct drv_usb_urb*urb){struct ehci_controller*c=hcd_controller(hcd);struct ehci_request*r;bool enabled;int error;enabled=hal_irq_disable();if(c->active){if(enabled)hal_irq_enable();return EBUSY;}error=build_request(c,urb,&r);if(error){if(enabled)hal_irq_enable();return error;}c->active=r;drv_usb_urb_set_hcd_data(urb,r);c->async_head->horizontal=(uint32_t)r->schedule.device_address|EHCI_LINK_QH;if(enabled)hal_irq_enable();return 0;}
static int ehci_urb_dequeue(struct drv_usb_hcd*hcd,struct drv_usb_urb*urb){struct ehci_controller*c=hcd_controller(hcd);struct ehci_request*r=drv_usb_urb_hcd_data(urb);bool enabled;if(!r)return EINVAL;enabled=hal_irq_disable();c->async_head->horizontal=(uint32_t)c->async_head_memory.device_address|EHCI_LINK_QH;c->active=NULL;drv_usb_urb_set_hcd_data(urb,NULL);if(enabled)hal_irq_enable();request_free(c,r);return 0;}
static int ehci_endpoint_enable(struct drv_usb_hcd*h,struct drv_usb_endpoint*e){(void)h;(void)e;return 0;}
static int ehci_endpoint_disable(struct drv_usb_hcd*h,struct drv_usb_endpoint*e){(void)h;(void)e;return 0;}
static uint32_t ehci_frame_number(struct drv_usb_hcd*h){struct ehci_controller*c=hcd_controller(h);return rd32(c->operational,EHCI_FRINDEX)>>3;}
static int ehci_root_status(struct drv_usb_hcd*h,void*b,size_t n,size_t*a){struct ehci_controller*c=hcd_controller(h);uint8_t bits=0;unsigned p;if(!b||n<1)return EINVAL;for(p=0;p<h->root_port_count;p++){uint32_t s=rd32(c->operational,EHCI_PORTSC(p));if(s&(EHCI_PORT_CONNECT_CHANGE|EHCI_PORT_ENABLE_CHANGE))bits|=(uint8_t)(2U<<p);}*(uint8_t*)b=bits;if(a)*a=1;return 0;}
static int ehci_root_control(struct drv_usb_hcd*h,const struct drv_usb_control_request*r,void*b,size_t n,size_t*a)
{struct ehci_controller*c=hcd_controller(h);unsigned p;uint32_t s;if(!r||r->index<1||r->index>h->root_port_count)return EINVAL;p=r->index-1U;s=rd32(c->operational,EHCI_PORTSC(p));if(r->request==0&&b&&n>=4){uint32_t v=0;if(!(s&EHCI_PORT_OWNER)){if(s&EHCI_PORT_CONNECT)v|=1U;if(s&EHCI_PORT_ENABLE)v|=2U;if(s&EHCI_PORT_RESET)v|=0x10U;v|=0x400U;}if(s&EHCI_PORT_CONNECT_CHANGE)v|=0x10000U;if(s&EHCI_PORT_ENABLE_CHANGE)v|=0x20000U;memcpy(b,&v,4);if(a)*a=4;return 0;}if(r->request==3&&r->value==4){if((s&0x0c00U)!=0){wr32(c->operational,EHCI_PORTSC(p),s|EHCI_PORT_OWNER);if(a)*a=0;return 0;}wr32(c->operational,EHCI_PORTSC(p),s|EHCI_PORT_RESET|EHCI_PORT_POWER);if(a)*a=0;return 0;}if(r->request==1){if(r->value==4)s&=~EHCI_PORT_RESET;else if(r->value==16)s|=EHCI_PORT_CONNECT_CHANGE;else if(r->value==17)s|=EHCI_PORT_ENABLE_CHANGE;else return ENOTSUP;wr32(c->operational,EHCI_PORTSC(p),s);if(a)*a=0;return 0;}if(r->request==3&&r->value==1){if(a)*a=0;return 0;}return ENOTSUP;}
static int ehci_irq(void*argument){struct ehci_controller*c=argument;struct ehci_request*r;struct drv_usb_urb*urb;uint32_t status;size_t actual=0;unsigned i;enum drv_usb_urb_status result=DRV_USB_URB_COMPLETE;status=rd32(c->operational,EHCI_USBSTS);if(!(status&EHCI_STS_ALL))return 0;wr32(c->operational,EHCI_USBSTS,status);r=c->active;if(!r)return 1;for(i=0;i<r->qtd_count;i++)if(r->qtds[i].token&EHCI_QTD_ACTIVE)return 1;for(i=0;i<r->qtd_count;i++)if(r->qtds[i].token&EHCI_QTD_ERRORS){result=(r->qtds[i].token&EHCI_QTD_HALTED)?DRV_USB_URB_STALL:DRV_USB_URB_IO_ERROR;break;}for(i=0;i<r->data_count;i++){uint32_t remaining=(r->qtds[r->data_first+i].token>>16)&0x7fffU;actual+=r->requested[r->data_first+i]-remaining;}if(result==DRV_USB_URB_COMPLETE&&!r->control)(void)drv_usb_endpoint_set_hcd_data(drv_usb_urb_endpoint(r->urb),0,(r->qh->token>>31)&1U);if(r->input&&actual)memcpy(drv_usb_urb_buffer(r->urb),(uint8_t*)r->bounce.address+8U,actual);c->async_head->horizontal=(uint32_t)c->async_head_memory.device_address|EHCI_LINK_QH;urb=r->urb;c->active=NULL;drv_usb_urb_set_hcd_data(urb,NULL);request_free(c,r);drv_usb_hcd_complete(&c->hcd,urb,result,actual);return 1;}
static const struct drv_usb_hcd_ops ehci_ops={.start=ehci_start,.quiesce=ehci_quiesce,.stop=ehci_stop,.urb_enqueue=ehci_urb_enqueue,.urb_dequeue=ehci_urb_dequeue,.endpoint_enable=ehci_endpoint_enable,.endpoint_disable=ehci_endpoint_disable,.frame_number=ehci_frame_number,.root_hub_status=ehci_root_status,.root_hub_control=ehci_root_control};

static void
ehci_publish(struct ehci_controller *c)
{
	if (c->listed)
		return;
	drv_pci_device_set_driver_data(c->pci, c);
	c->next = ehci_controllers;
	ehci_controllers = c;
	c->listed = 1;
}

static void
ehci_unpublish(struct ehci_controller *c)
{
	struct ehci_controller **link;

	if (!c->listed)
		return;
	for (link = &ehci_controllers; *link != NULL; link = &(*link)->next) {
		if (*link == c) {
			*link = c->next;
			break;
		}
	}
	c->next = NULL;
	c->listed = 0;
}

static int
ehci_pci_release(struct ehci_controller *c)
{
	int error;

	if (c->pci_state_saved) {
		error = ehci_bus_master_disable(c);
		if (error != 0)
			return error;
	}
	if (c->bar_mapped) {
		drv_pci_device_unmap_bar(c->pci, &c->registers);
		c->bar_mapped = 0;
		c->capability = NULL;
		c->operational = NULL;
	}
	if (c->pci_state_saved) {
		error = drv_pci_device_restore_enable_state(c->pci,
		    &c->pci_enable_state);
		if (error != 0)
			return error;
		c->pci_state_saved = 0;
	}
	if (c->bar_claimed) {
		drv_pci_device_release_bar(c->pci, 0);
		c->bar_claimed = 0;
	}
	return 0;
}

static int
ehci_cleanup(struct ehci_controller *c)
{
	int error;

	if (c->hcd_registered) {
		error = drv_usb_hcd_unregister(&c->hcd);
		if (error != 0)
			return error;
		c->hcd_registered = 0;
	}
	if (c->irq_allocated) {
		/* A registered HCD removes the checked IRQ from quiesce. */
		if (c->irq_cookie != NULL)
			return EBUSY;
		drv_pci_device_free_irqs(c->pci, &c->irq, 1);
		c->irq_allocated = 0;
	}
	return ehci_pci_release(c);
}

static int
ehci_attach(struct drv_pci_device *d, const struct drv_pci_id *id)
{
	struct ehci_controller *c;
	unsigned count = 0, ports;
	const char *stage = "allocation";
	int cleanup_error, error;

	(void)id;
	c = hal_malloc(sizeof(*c));
	if (c == NULL)
		return ENOMEM;
	memset(c, 0, sizeof(*c));
	c->pci = d;
	c->dma_quiesced = 1;
	stage = "BAR claim";
	error = drv_pci_device_claim_bar(d, 0);
	if (error != 0)
		goto fail;
	c->bar_claimed = 1;
	stage = "PCI command save";
	error = drv_pci_device_save_enable_state(d, &c->pci_enable_state);
	if (error != 0)
		goto fail;
	c->pci_state_saved = 1;
	stage = "BAR map";
	error = drv_pci_device_map_bar(d, 0,
	    DRV_PCI_MAP_READ | DRV_PCI_MAP_WRITE | DRV_PCI_MAP_NOCACHE,
	    &c->registers);
	if (error != 0)
		goto fail;
	c->bar_mapped = 1;
	c->capability = c->registers.address;
	c->operational = c->capability + rd8(c->capability, 0);
	ports = rd32(c->capability, 4) & 15U;
	if (ports == 0) {
		error = ENODEV;
		stage = "capabilities";
		goto fail;
	}
	c->hcd.name = "EHCI";
	c->hcd.ops = &ehci_ops;
	c->hcd.dma = drv_pci_device_dma(d);
	c->hcd.root_port_count = ports;
	c->hcd.private_data[0] = (uintptr_t)c;
	stage = "PCI enable";
	if ((error = drv_pci_device_enable_memory(d)) != 0 ||
	    (error = drv_pci_device_set_bus_master(d, true)) != 0)
		goto fail;
	stage = "HCD registration";
	error = drv_usb_hcd_register(&c->hcd, &c->bus);
	if (error != 0)
		goto fail;
	c->hcd_registered = 1;
	stage = "IRQ allocation";
	error = drv_pci_device_allocate_irqs(d, DRV_PCI_IRQ_ALLOW_INTX,
	    1, 1, &c->irq, &count);
	if (error != 0)
		goto fail;
	c->irq_allocated = 1;
	stage = "IRQ establishment";
	error = drv_pci_device_establish_irq(d, &c->irq, ehci_irq, c,
	    "ehci", &c->irq_cookie);
	if (error != 0)
		goto fail;
	wr32(c->operational, EHCI_USBINTR, 0x37U);
	ehci_publish(c);
	hal_printf("ehci: PCI controller, ports=%u version=%x\n", ports,
	    *(volatile uint16_t *)(c->capability + 2));
	return 0;

fail:
	cleanup_error = ehci_cleanup(c);
	if (cleanup_error != 0) {
		c->quarantined = 1;
		ehci_publish(c);
		hal_printf(
		    "ehci: attach failed at %s (%d), cleanup failed (%d); controller quarantined\n",
		    stage, error, cleanup_error);
		return 0;
	}
	hal_free(c);
	return error;
}

static int
ehci_detach(struct drv_pci_device *d, unsigned flags)
{
	struct ehci_controller *c = drv_pci_device_driver_data(d);
	int error;

	(void)flags;
	if (c == NULL)
		return 0;
	error = ehci_cleanup(c);
	if (error != 0) {
		c->quarantined = 1;
		return error;
	}
	ehci_unpublish(c);
	drv_pci_device_set_driver_data(d, NULL);
	hal_free(c);
	return 0;
}
static const struct drv_pci_id ehci_ids[]={{DRV_PCI_ANY_ID,DRV_PCI_ANY_ID,DRV_PCI_ANY_ID,DRV_PCI_ANY_ID,0x0c0320U,0xffffffU,0}};
static struct drv_pci_driver ehci_driver={.name="ehci",.ids=ehci_ids,.id_count=1,.attach=ehci_attach,.detach=ehci_detach};
int drv_pci_ehci_driver_register(void){return drv_pci_driver_register(&ehci_driver);}
void drv_pci_ehci_probe_roots(void){struct ehci_controller*c;for(c=ehci_controllers;c;c=c->next)if(!c->quarantined)drv_usb_hcd_root_hub_changed(&c->hcd);}
