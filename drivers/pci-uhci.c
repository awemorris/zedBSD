/*
 * PCI UHCI host controller driver
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#include <drivers/pci-uhci.h>
#include <drivers/pci.h>
#include <drivers/usb.h>
#include <errno.h>
#include <hal/hal.h>
#include <string.h>

#define UHCI_USBCMD       0x00U
#define UHCI_USBSTS       0x02U
#define UHCI_USBINTR      0x04U
#define UHCI_FRNUM        0x06U
#define UHCI_FLBASEADD    0x08U
#define UHCI_SOFMOD       0x0cU
#define UHCI_PORTSC1      0x10U
#define UHCI_PORTSC2      0x12U

#define UHCI_CMD_RUN      0x0001U
#define UHCI_CMD_HCRESET  0x0002U
#define UHCI_CMD_GRESET   0x0004U
#define UHCI_CMD_CF       0x0040U
#define UHCI_STS_ALL      0x003fU
#define UHCI_PORT_CCS     0x0001U
#define UHCI_PORT_CSC     0x0002U
#define UHCI_PORT_PE      0x0004U
#define UHCI_PORT_PEC     0x0008U
#define UHCI_PORT_LSDA    0x0100U
#define UHCI_PORT_RESET   0x0200U
#define UHCI_LINK_TERM    0x00000001U
#define UHCI_LINK_QH      0x00000002U
#define UHCI_LINK_DEPTH   0x00000004U
#define UHCI_TD_ACTIVE    0x00800000U
#define UHCI_TD_IOC       0x01000000U
#define UHCI_TD_STALLED   0x00400000U
#define UHCI_TD_ERRORS    0x007e0000U
#define UHCI_PID_OUT      0xe1U
#define UHCI_PID_IN       0x69U
#define UHCI_PID_SETUP    0x2dU
#define UHCI_MAX_TDS      255U

struct uhci_qh { volatile uint32_t head, element, reserved[2]; };
struct uhci_td { volatile uint32_t link, status, token, buffer; };
struct uhci_request {
	struct drv_usb_urb *urb;
	struct drv_dma_buffer schedule, bounce;
	struct uhci_qh *qh;
	struct uhci_td *tds;
	unsigned td_count, data_first, data_count;
	unsigned final_toggle;
	bool input;
};

struct uhci_controller {
	struct drv_pci_device *pci;
	uint16_t io_base;
	struct drv_dma_buffer frame_list;
	struct drv_usb_hcd hcd;
	struct drv_usb_bus *bus;
	struct drv_pci_irq irq;
	void *irq_cookie;
	struct uhci_request *active;
	struct uhci_controller *next;
};

static struct uhci_controller *uhci_controllers;

static uint8_t in8(uint16_t port)
{ uint8_t v; __asm__ volatile("inb %w1,%0":"=a"(v):"Nd"(port)); return v; }
static uint16_t in16(uint16_t port)
{ uint16_t v; __asm__ volatile("inw %w1,%0":"=a"(v):"Nd"(port)); return v; }
static void out8(uint16_t port,uint8_t v)
{ __asm__ volatile("outb %0,%w1"::"a"(v),"Nd"(port)); }
static void out16(uint16_t port,uint16_t v)
{ __asm__ volatile("outw %0,%w1"::"a"(v),"Nd"(port)); }
static void out32(uint16_t port,uint32_t v)
{ __asm__ volatile("outl %0,%w1"::"a"(v),"Nd"(port)); }

static struct uhci_controller *hcd_controller(struct drv_usb_hcd *hcd)
{ return (struct uhci_controller *)hcd->private_data[0]; }

static void io_pause(unsigned count)
{
	while (count-- != 0) (void)in8(0x80U);
}

static int uhci_start(struct drv_usb_hcd *hcd)
{
	struct uhci_controller *controller = hcd_controller(hcd);
	uint32_t *frames;
	unsigned index, timeout;
	int error;
	error = drv_dma_alloc_coherent(hcd->dma, 4096U, 4096U,
	    &controller->frame_list);
	if (error != 0) return error;
	frames = controller->frame_list.address;
	for (index = 0; index < 1024U; index++) frames[index] = UHCI_LINK_TERM;
	out16(controller->io_base + UHCI_USBCMD, UHCI_CMD_GRESET);
	io_pause(10000U);
	out16(controller->io_base + UHCI_USBCMD, 0);
	out16(controller->io_base + UHCI_USBCMD, UHCI_CMD_HCRESET);
	for (timeout = 0; timeout < 100000U; timeout++)
		if ((in16(controller->io_base + UHCI_USBCMD) & UHCI_CMD_HCRESET) == 0)
			break;
	if (timeout == 100000U) {
		drv_dma_free_coherent(hcd->dma, &controller->frame_list);
		return ETIMEDOUT;
	}
	out16(controller->io_base + UHCI_USBINTR, 0);
	out16(controller->io_base + UHCI_USBSTS, UHCI_STS_ALL);
	out16(controller->io_base + UHCI_FRNUM, 0);
	out32(controller->io_base + UHCI_FLBASEADD,
	    (uint32_t)controller->frame_list.device_address);
	out8(controller->io_base + UHCI_SOFMOD, 64U);
	out16(controller->io_base + UHCI_USBCMD, UHCI_CMD_CF | UHCI_CMD_RUN);
	if ((in16(controller->io_base + UHCI_USBSTS) & 0x20U) != 0) {
		drv_dma_free_coherent(hcd->dma, &controller->frame_list);
		return EIO;
	}
	return 0;
}

static void uhci_stop(struct drv_usb_hcd *hcd)
{
	struct uhci_controller *controller = hcd_controller(hcd);
	out16(controller->io_base + UHCI_USBCMD, 0);
	out16(controller->io_base + UHCI_USBINTR, 0);
	if (controller->frame_list.address != NULL)
		drv_dma_free_coherent(hcd->dma, &controller->frame_list);
}

static uint32_t uhci_token(uint8_t pid,unsigned address,unsigned endpoint,
	unsigned toggle,unsigned length)
{
	uint32_t encoded_length=length==0?0x7ffU:length-1U;
	return pid|((uint32_t)address<<8)|((uint32_t)endpoint<<15)|
	    ((uint32_t)toggle<<19)|(encoded_length<<21);
}

static void uhci_request_free(struct uhci_controller*c,struct uhci_request*r)
{
	if(!r)return;
	if(r->schedule.address)drv_dma_free_coherent(c->hcd.dma,&r->schedule);
	if(r->bounce.address)drv_dma_free_coherent(c->hcd.dma,&r->bounce);
	hal_free(r);
}

static int uhci_add_td(struct uhci_request*r,uint8_t pid,unsigned address,
	unsigned endpoint,unsigned toggle,unsigned length,uint32_t buffer)
{
	struct uhci_td*td;uint32_t physical;
	if(r->td_count>=UHCI_MAX_TDS)return E2BIG;
	td=&r->tds[r->td_count];physical=(uint32_t)r->schedule.device_address+
	    16U+r->td_count*sizeof(*td);
	if(r->td_count!=0)r->tds[r->td_count-1U].link=physical|UHCI_LINK_DEPTH;
	td->link=UHCI_LINK_TERM;td->status=UHCI_TD_ACTIVE|(3U<<27);
	td->token=uhci_token(pid,address,endpoint,toggle,length);td->buffer=buffer;
	r->td_count++;return 0;
}

static int uhci_build_request(struct uhci_controller*c,struct drv_usb_urb*urb,
	struct uhci_request**result)
{
	struct uhci_request*r;struct drv_usb_endpoint*ep=drv_usb_urb_endpoint(urb);
	const struct drv_usb_control_request*control=drv_usb_urb_control_request(urb);
	size_t length=drv_usb_urb_length(urb),offset=0;unsigned packet,address,endpoint,toggle=0;int error;
	r=hal_malloc(sizeof(*r));if(!r)return ENOMEM;memset(r,0,sizeof(*r));r->urb=urb;
	if((error=drv_dma_alloc_coherent(c->hcd.dma,4096U,16U,&r->schedule))!=0){hal_free(r);return error;}
	if((error=drv_dma_alloc_coherent(c->hcd.dma,length+8U,16U,&r->bounce))!=0){uhci_request_free(c,r);return error;}
	memset(r->schedule.address,0,4096U);r->qh=r->schedule.address;
	r->tds=(struct uhci_td*)((uint8_t*)r->schedule.address+16U);
	address=drv_usb_device_address(drv_usb_urb_device(urb));
	endpoint=drv_usb_endpoint_address(ep)&0x0fU;
	packet=drv_usb_endpoint_max_packet_size(ep);if(packet==0)packet=8U;
	if(control){memcpy(r->bounce.address,control,sizeof(*control));
		error=uhci_add_td(r,UHCI_PID_SETUP,address,0,0,8U,(uint32_t)r->bounce.device_address);if(error)goto fail;
		r->input=(control->request_type&DRV_USB_DIR_IN)!=0;
		if(!r->input&&length)memcpy((uint8_t*)r->bounce.address+8U,
		    drv_usb_urb_buffer(urb),length);
		r->data_first=r->td_count;toggle=1;
		while(offset<length){unsigned chunk=length-offset>packet?packet:(unsigned)(length-offset);error=uhci_add_td(r,r->input?UHCI_PID_IN:UHCI_PID_OUT,address,0,toggle,chunk,(uint32_t)(r->bounce.device_address+8U+offset));if(error)goto fail;offset+=chunk;toggle^=1U;r->data_count++;}
		error=uhci_add_td(r,r->input?UHCI_PID_OUT:UHCI_PID_IN,address,0,1,0,0);if(error)goto fail;
	}else{
		r->input=drv_usb_endpoint_is_input(ep);r->data_first=0;
		toggle=(unsigned)drv_usb_endpoint_hcd_data(ep,0)&1U;
		if(!r->input&&length)memcpy((uint8_t*)r->bounce.address+8U,drv_usb_urb_buffer(urb),length);
		while(offset<length){unsigned chunk=length-offset>packet?packet:(unsigned)(length-offset);error=uhci_add_td(r,r->input?UHCI_PID_IN:UHCI_PID_OUT,address,endpoint,toggle,chunk,(uint32_t)(r->bounce.device_address+8U+offset));if(error)goto fail;offset+=chunk;toggle^=1U;r->data_count++;}
		if(length==0){error=uhci_add_td(r,r->input?UHCI_PID_IN:UHCI_PID_OUT,address,endpoint,toggle,0,0);if(error)goto fail;toggle^=1U;}r->data_count=r->td_count;r->final_toggle=toggle;
	}
	r->tds[r->td_count-1U].status|=UHCI_TD_IOC;r->qh->head=UHCI_LINK_TERM;
	r->qh->element=(uint32_t)r->schedule.device_address+16U;*result=r;return 0;
fail:uhci_request_free(c,r);return error;
}

static int uhci_urb_enqueue(struct drv_usb_hcd *hcd, struct drv_usb_urb *urb)
{
	struct uhci_controller*c=hcd_controller(hcd);struct uhci_request*r;uint32_t*frames;unsigned i;bool enabled;int error;
	enabled=hal_irq_disable();if(c->active){if(enabled)hal_irq_enable();return EBUSY;}
	error=uhci_build_request(c,urb,&r);if(error){if(enabled)hal_irq_enable();return error;}
	c->active=r;drv_usb_urb_set_hcd_data(urb,r);frames=c->frame_list.address;
	for(i=0;i<1024U;i++)frames[i]=(uint32_t)r->schedule.device_address|UHCI_LINK_QH;
	if(enabled)hal_irq_enable();
	return 0;
}
static int uhci_urb_dequeue(struct drv_usb_hcd *hcd, struct drv_usb_urb *urb)
{
	struct uhci_controller*c=hcd_controller(hcd);struct uhci_request*r=drv_usb_urb_hcd_data(urb);uint32_t*frames;unsigned i;bool enabled;
	if(!r)return EINVAL;
	enabled=hal_irq_disable();frames=c->frame_list.address;
	for(i=0;i<1024U;i++)frames[i]=UHCI_LINK_TERM;
	c->active=NULL;
	drv_usb_urb_set_hcd_data(urb,NULL);if(enabled)hal_irq_enable();uhci_request_free(c,r);return 0;
}

static int uhci_irq(void *argument)
{
	struct uhci_controller*c=argument;struct uhci_request*r;struct drv_usb_urb*urb;
	uint16_t status;uint32_t*frames;size_t actual=0;unsigned i;enum drv_usb_urb_status result=DRV_USB_URB_COMPLETE;
	status=in16(c->io_base+UHCI_USBSTS);if((status&UHCI_STS_ALL)==0)return 0;
	out16(c->io_base+UHCI_USBSTS,status);r=c->active;if(!r)return 1;
	for(i=0;i<r->td_count;i++)if(r->tds[i].status&UHCI_TD_ACTIVE)return 1;
	for(i=0;i<r->td_count;i++)if(r->tds[i].status&UHCI_TD_ERRORS){result=(r->tds[i].status&UHCI_TD_STALLED)?DRV_USB_URB_STALL:DRV_USB_URB_IO_ERROR;break;}
	for(i=0;i<r->data_count;i++){uint32_t n=r->tds[r->data_first+i].status&0x7ffU;if(n!=0x7ffU)actual+=n+1U;}
	if(result==DRV_USB_URB_COMPLETE&&drv_usb_urb_control_request(r->urb)==NULL)
		(void)drv_usb_endpoint_set_hcd_data(drv_usb_urb_endpoint(r->urb),0,r->final_toggle);
	if(r->input&&actual)memcpy(drv_usb_urb_buffer(r->urb),(uint8_t*)r->bounce.address+8U,actual);
	frames=c->frame_list.address;for(i=0;i<1024U;i++)frames[i]=UHCI_LINK_TERM;
	urb=r->urb;c->active=NULL;drv_usb_urb_set_hcd_data(urb,NULL);uhci_request_free(c,r);
	drv_usb_hcd_complete(&c->hcd,urb,result,actual);return 1;
}
static int uhci_endpoint_enable(struct drv_usb_hcd *hcd,
	struct drv_usb_endpoint *endpoint)
{ (void)hcd; (void)endpoint; return 0; }
static void uhci_endpoint_disable(struct drv_usb_hcd *hcd,
	struct drv_usb_endpoint *endpoint)
{ (void)hcd; (void)endpoint; }
static uint32_t uhci_frame_number(struct drv_usb_hcd *hcd)
{ struct uhci_controller*c=hcd_controller(hcd);return in16(c->io_base+UHCI_FRNUM)&0x7ffU; }

static int uhci_root_hub_status(struct drv_usb_hcd *hcd, void *buffer,
	size_t size, size_t *actual)
{
	struct uhci_controller *c=hcd_controller(hcd);uint8_t bits=0;
	uint16_t p1=in16(c->io_base+UHCI_PORTSC1),p2=in16(c->io_base+UHCI_PORTSC2);
	if(buffer==NULL||size<1)return EINVAL;
	if(p1&(UHCI_PORT_CSC|UHCI_PORT_PEC))bits|=2U;
	if(p2&(UHCI_PORT_CSC|UHCI_PORT_PEC))bits|=4U;
	*(uint8_t*)buffer=bits;if(actual)*actual=1;return 0;
}

static int uhci_root_hub_control(struct drv_usb_hcd *hcd,
	const struct drv_usb_control_request *request, void *buffer, size_t size,
	size_t *actual)
{
	struct uhci_controller*c=hcd_controller(hcd);uint16_t port,value;
	if(!request||request->index<1||request->index>2)return EINVAL;
	port=c->io_base+(request->index==1?UHCI_PORTSC1:UHCI_PORTSC2);
	value=in16(port);
	if(request->request==0&&buffer&&size>=4){uint32_t status=0;if(value&UHCI_PORT_CCS)status|=1U;if(value&UHCI_PORT_PE)status|=2U;if(value&UHCI_PORT_RESET)status|=0x10U;if(value&UHCI_PORT_LSDA)status|=0x200U;if(value&UHCI_PORT_CSC)status|=0x10000U;if(value&UHCI_PORT_PEC)status|=0x20000U;memcpy(buffer,&status,4);if(actual)*actual=4;return 0;}
	if(request->request==3){if(request->value==4)value|=UHCI_PORT_RESET;else if(request->value==1)value|=UHCI_PORT_PE;else return ENOTSUP;out16(port,value);if(actual)*actual=0;return 0;}
	if(request->request==1){if(request->value==16)value|=UHCI_PORT_CSC;else if(request->value==17)value|=UHCI_PORT_PEC;else if(request->value==4)value&=~UHCI_PORT_RESET;else if(request->value==1)value&=~UHCI_PORT_PE;else return ENOTSUP;out16(port,value);if(actual)*actual=0;return 0;}
	return ENOTSUP;
}

static const struct drv_usb_hcd_ops uhci_ops = {
	.start=uhci_start,.stop=uhci_stop,.urb_enqueue=uhci_urb_enqueue,
	.urb_dequeue=uhci_urb_dequeue,.endpoint_enable=uhci_endpoint_enable,
	.endpoint_disable=uhci_endpoint_disable,.frame_number=uhci_frame_number,
	.root_hub_status=uhci_root_hub_status,.root_hub_control=uhci_root_hub_control
};

static int uhci_attach(struct drv_pci_device *device,const struct drv_pci_id*id)
{
	struct uhci_controller*c;struct drv_pci_bar bar;unsigned index;int error;
	(void)id;c=hal_malloc(sizeof(*c));if(!c)return ENOMEM;memset(c,0,sizeof(*c));
	for(index=0;index<drv_pci_device_bar_count(device);index++)
		if(drv_pci_device_bar(device,index,&bar)==0&&bar.type==DRV_PCI_BAR_IO)break;
	if(index==drv_pci_device_bar_count(device)||bar.bus_address>0xffffU){hal_free(c);return ENODEV;}
	c->pci=device;c->io_base=(uint16_t)bar.bus_address;c->hcd.name="UHCI";
	c->hcd.ops=&uhci_ops;c->hcd.dma=drv_pci_device_dma(device);
	c->hcd.root_port_count=2;c->hcd.private_data[0]=(uintptr_t)c;
	if((error=drv_pci_device_enable_io(device))!=0||(error=drv_pci_device_set_bus_master(device,true))!=0||(error=drv_usb_hcd_register(&c->hcd,&c->bus))!=0){hal_free(c);return error;}
	{unsigned count=0;error=drv_pci_device_allocate_irqs(device,
	    DRV_PCI_IRQ_ALLOW_INTX,1,1,&c->irq,&count);
	 if(error==0)error=drv_pci_device_establish_irq(device,&c->irq,uhci_irq,c,
	    "uhci",&c->irq_cookie);
	 if(error!=0){(void)drv_usb_hcd_unregister(&c->hcd);hal_free(c);return error;}}
	out16(c->io_base+UHCI_USBINTR,0x000dU);
	drv_pci_device_set_driver_data(device,c);
	hal_printf("uhci: PCI controller at I/O %04x, ports=%u\n",c->io_base,c->hcd.root_port_count);
	c->next=uhci_controllers;uhci_controllers=c;
	return 0;
}

static int uhci_detach(struct drv_pci_device *device,unsigned flags)
{struct uhci_controller*c=drv_pci_device_driver_data(device);(void)flags;if(!c)return 0;out16(c->io_base+UHCI_USBINTR,0);if(c->irq_cookie)drv_pci_device_disestablish_irq(device,c->irq_cookie);drv_pci_device_free_irqs(device,&c->irq,1);if(drv_usb_hcd_unregister(&c->hcd)!=0)return EBUSY;(void)drv_pci_device_set_bus_master(device,false);drv_pci_device_set_driver_data(device,NULL);hal_free(c);return 0;}
static const struct drv_pci_id uhci_ids[]={
	{DRV_PCI_ANY_ID,DRV_PCI_ANY_ID,DRV_PCI_ANY_ID,DRV_PCI_ANY_ID,0x0c0300U,0xffffffU,0}
};
static struct drv_pci_driver uhci_driver={.name="uhci",.ids=uhci_ids,.id_count=1,.attach=uhci_attach,.detach=uhci_detach};
int drv_pci_uhci_driver_register(void){return drv_pci_driver_register(&uhci_driver);}
void drv_pci_uhci_probe_roots(void){struct uhci_controller*c;for(c=uhci_controllers;c;c=c->next)drv_usb_hcd_root_hub_changed(&c->hcd);}
