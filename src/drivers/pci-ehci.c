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
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/thread.h>
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
#define EHCI_CMD_IAAD 0x00000040U
#define EHCI_STS_USBINT 0x00000001U
#define EHCI_STS_USBERRINT 0x00000002U
#define EHCI_STS_HSE 0x00000010U
#define EHCI_STS_IAA 0x00000020U
#define EHCI_STS_HALTED 0x00001000U
#define EHCI_STS_ASYNC 0x00008000U
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
#define EHCI_RETIRE_TICKS 100U

enum ehci_request_state {
	EHCI_REQUEST_ACTIVE,
	EHCI_REQUEST_DEACTIVATING_COMPLETE,
	EHCI_REQUEST_DEACTIVATING_CANCEL,
	EHCI_REQUEST_WAIT_IAA_COMPLETE,
	EHCI_REQUEST_WAIT_IAA_CANCEL,
	EHCI_REQUEST_COMPLETING,
	EHCI_REQUEST_RETIRED_CANCEL,
	EHCI_REQUEST_FAILED
};

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
	enum ehci_request_state state;
	uint64_t retirement_generation, retirement_started;
	const char *failure_stage;
	int failure_error;
	unsigned failure_reported;
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
	struct spinlock active_lock;
	struct ehci_request *active;
	struct thread *retirement_worker;
	struct ehci_controller *next;
	uint64_t retirement_generation;
	unsigned bar_claimed, bar_mapped, pci_state_saved;
	unsigned hcd_registered, irq_allocated, dma_quiesced;
	unsigned quiescing, submitting, listed, quarantined;
	unsigned retirement_success_reported;
	volatile unsigned retirement_pending, retirement_stopping;
};
static struct ehci_controller *ehci_controllers;

static int ehci_retirement_worker_start(struct ehci_controller *);
static int ehci_retirement_worker_stop(struct ehci_controller *);
static void ehci_retirement_worker_wakeup(struct ehci_controller *);

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
	struct ehci_controller*c=hcd_controller(hcd);uint32_t*periodic;unsigned i,timeout;unsigned long irq;int error;
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
	for(timeout=0;timeout<1000000U;timeout++) {
		uint32_t status = rd32(c->operational, EHCI_USBSTS);

		if (status != UINT32_MAX &&
		    (status & (EHCI_STS_HALTED | EHCI_STS_ASYNC)) ==
		    EHCI_STS_ASYNC) {
			irq=spin_lock_irqsave(&c->active_lock);
			c->dma_quiesced=0;c->quiescing=0;
			spin_unlock_irqrestore(&c->active_lock,irq);
			return 0;
		}
	}
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
	uint32_t command, status;
	unsigned long irq;
	int halt_error = 0, master_error, irq_error;

	irq = spin_lock_irqsave(&c->active_lock);
	if (c->dma_quiesced) {
		spin_unlock_irqrestore(&c->active_lock, irq);
		/* A callback running on the retirement worker may have established
		 * hardware quiescence but cannot join itself.  A later external retry
		 * finishes the software-worker boundary before stop can release memory. */
		return ehci_retirement_worker_stop(c);
	}
	if (c->active != NULL || c->submitting) {
		spin_unlock_irqrestore(&c->active_lock, irq);
		return EBUSY;
	}
	/* Permanently close submission admission for this shutdown attempt.  A
	 * failed checked teardown may be retried but must not admit fresh DMA. */
	c->quiescing = 1;
	wr32(c->operational, EHCI_USBINTR, 0);
	command = rd32(c->operational, EHCI_USBCMD);
	wr32(c->operational, EHCI_USBCMD,
	    command & ~(EHCI_CMD_RUN | EHCI_CMD_PERIODIC | EHCI_CMD_ASYNC));
	spin_unlock_irqrestore(&c->active_lock, irq);
	started = sched_ticks();
	for (;;) {
		irq = spin_lock_irqsave(&c->active_lock);
		status = rd32(c->operational, EHCI_USBSTS);
		hal_io_mb();
		spin_unlock_irqrestore(&c->active_lock, irq);
		if ((status & EHCI_STS_HALTED) != 0)
			break;
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
	irq = spin_lock_irqsave(&c->active_lock);
	if (c->active != NULL || c->submitting) {
		spin_unlock_irqrestore(&c->active_lock, irq);
		__builtin_trap();
	}
	c->dma_quiesced = 1;
	spin_unlock_irqrestore(&c->active_lock, irq);
	return ehci_retirement_worker_stop(c);
}

static void
ehci_stop(struct drv_usb_hcd *hcd)
{
	struct ehci_controller *c = hcd_controller(hcd);
	unsigned long irq;
	int releasable;

	irq = spin_lock_irqsave(&c->active_lock);
	releasable = c->dma_quiesced && c->active == NULL && !c->submitting &&
	    c->retirement_worker == NULL;
	spin_unlock_irqrestore(&c->active_lock, irq);
	if (!releasable) {
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

static void
ehci_request_commit_toggle(struct ehci_request *request)
{
	/* Non-control QHs use the overlay data-toggle bit.  Once IAA proves the
	 * overlay retired, it is the next toggle even for a short transfer, halted
	 * error, or cancellation after some packets reached the device. */
	if (!request->control)
		(void)drv_usb_endpoint_set_hcd_data(
		    drv_usb_urb_endpoint(request->urb), 0,
		    (request->qh->token >> 31) & 1U);
}

static int
ehci_request_waiting_iaa(const struct ehci_request *request)
{
	return request != NULL &&
	    (request->state == EHCI_REQUEST_WAIT_IAA_COMPLETE ||
	    request->state == EHCI_REQUEST_WAIT_IAA_CANCEL);
}

static int
ehci_retirement_expired(const struct ehci_request *request)
{
	return sched_ticks() - request->retirement_started >= EHCI_RETIRE_TICKS;
}

/* active_lock is held and interrupts are disabled for all schedule changes. */
static void
ehci_request_deactivate_locked(struct ehci_request *request)
{
	unsigned index;

	for (index = 0; index < request->qtd_count; index++)
		request->qtds[index].token &= ~EHCI_QTD_ACTIVE;
	/* The overlay is controller-owned while the QH is scheduled.  Do not
	 * manufacture the retirement observation by clearing its Active bit in
	 * software: the worker must observe the controller refresh it from the
	 * now-inactive qTD chain before unlinking the QH. */
	hal_io_wmb();
}

static int
ehci_request_inactive_locked(const struct ehci_request *request)
{
	unsigned index;

	if ((request->qh->token & EHCI_QTD_ACTIVE) != 0)
		return 0;
	for (index = 0; index < request->qtd_count; index++)
		if ((request->qtds[index].token & EHCI_QTD_ACTIVE) != 0)
			return 0;
	return 1;
}

static void
ehci_retirement_fail_locked(struct ehci_controller *controller,
	struct ehci_request *request, int error, const char *stage)
{
	if (request->state == EHCI_REQUEST_FAILED)
		return;
	request->state = EHCI_REQUEST_FAILED;
	request->failure_error = error != 0 ? error : EIO;
	request->failure_stage = stage;
	controller->quiescing = 1;
	controller->quarantined = 1;
}

static void
ehci_retirement_report(struct ehci_controller *controller)
{
	struct ehci_request *request;
	const char *stage = NULL;
	unsigned long irq;
	int error = 0;

	irq = spin_lock_irqsave(&controller->active_lock);
	request = controller->active;
	if (request != NULL && request->state == EHCI_REQUEST_FAILED &&
	    !request->failure_reported) {
		request->failure_reported = 1;
		stage = request->failure_stage;
		error = request->failure_error;
	}
	spin_unlock_irqrestore(&controller->active_lock, irq);
	if (stage != NULL)
		hal_printf(
		    "ehci: request retirement failed at %s (%d); request and DMA retained\n",
		    stage, error);
}

/*
 * Begin the EHCI 4.8.2 handshake.  There is only one active request, so a
 * software generation together with a cleared, read-back IAA status prevents
 * a stale acknowledgement from satisfying this unlink.
 */
static int
ehci_retirement_begin_iaa_locked(struct ehci_controller *controller,
	struct ehci_request *request)
{
	uint32_t command, status;

	if (request->state != EHCI_REQUEST_DEACTIVATING_COMPLETE &&
	    request->state != EHCI_REQUEST_DEACTIVATING_CANCEL)
		return EINVAL;
	status = rd32(controller->operational, EHCI_USBSTS);
	command = rd32(controller->operational, EHCI_USBCMD);
	hal_io_mb();
	if (status == UINT32_MAX || command == UINT32_MAX)
		return EIO;
	if ((status & (EHCI_STS_HSE | EHCI_STS_HALTED)) != 0)
		return EIO;
	if ((command & (EHCI_CMD_RUN | EHCI_CMD_ASYNC)) !=
	    (EHCI_CMD_RUN | EHCI_CMD_ASYNC))
		return EIO;
	/* A prior doorbell can still be draining when software reaches this QH.
	 * It is not proof for this generation.  Wait for it to self-clear, then
	 * acknowledge its IAA below before issuing our own doorbell. */
	if ((status & EHCI_STS_ASYNC) == 0 ||
	    (command & EHCI_CMD_IAAD) != 0)
		return EAGAIN;
	if ((status & EHCI_STS_IAA) != 0) {
		wr32(controller->operational, EHCI_USBSTS, EHCI_STS_IAA);
		status = rd32(controller->operational, EHCI_USBSTS);
		hal_io_mb();
		if (status == UINT32_MAX || (status & EHCI_STS_IAA) != 0)
			return EIO;
	}
	controller->async_head->horizontal =
	    (uint32_t)controller->async_head_memory.device_address |
	    EHCI_LINK_QH;
	hal_io_wmb();
	controller->retirement_generation++;
	if (controller->retirement_generation == 0)
		controller->retirement_generation++;
	request->retirement_generation = controller->retirement_generation;
	request->state = request->state == EHCI_REQUEST_DEACTIVATING_COMPLETE ?
	    EHCI_REQUEST_WAIT_IAA_COMPLETE : EHCI_REQUEST_WAIT_IAA_CANCEL;
	wr32(controller->operational, EHCI_USBCMD, command | EHCI_CMD_IAAD);
	return 0;
}

/* Return zero only for the matching, acknowledged Async Advance. */
static int
ehci_retirement_observe_iaa_locked(struct ehci_controller *controller,
	struct ehci_request *request)
{
	uint32_t command, status;

	if (!ehci_request_waiting_iaa(request) ||
	    request->retirement_generation == 0 ||
	    request->retirement_generation != controller->retirement_generation)
		return EINVAL;
	status = rd32(controller->operational, EHCI_USBSTS);
	command = rd32(controller->operational, EHCI_USBCMD);
	hal_io_mb();
	if (status == UINT32_MAX || command == UINT32_MAX)
		return EIO;
	if ((status & (EHCI_STS_HSE | EHCI_STS_HALTED)) != 0 ||
	    (status & EHCI_STS_ASYNC) == 0 ||
	    (command & (EHCI_CMD_RUN | EHCI_CMD_ASYNC)) !=
	    (EHCI_CMD_RUN | EHCI_CMD_ASYNC))
		return EIO;
	if ((status & EHCI_STS_IAA) == 0 ||
	    (command & EHCI_CMD_IAAD) != 0)
		return EAGAIN;
	wr32(controller->operational, EHCI_USBSTS, EHCI_STS_IAA);
	status = rd32(controller->operational, EHCI_USBSTS);
	hal_io_mb();
	if (status == UINT32_MAX || (status & EHCI_STS_IAA) != 0)
		return EIO;
	request->state = request->state == EHCI_REQUEST_WAIT_IAA_COMPLETE ?
	    EHCI_REQUEST_COMPLETING : EHCI_REQUEST_RETIRED_CANCEL;
	return 0;
}

static void
ehci_complete_retired_request(struct ehci_controller *controller,
	struct ehci_request *request)
{
	struct drv_usb_urb *urb = request->urb;
	enum drv_usb_urb_status result = DRV_USB_URB_COMPLETE;
	size_t actual = 0, length = drv_usb_urb_length(urb);
	unsigned index;
	unsigned long irq;

	/* IAA has made every descriptor and bounce write stable. */
	hal_io_rmb();
	for (index = 0; index < request->qtd_count; index++) {
		if ((request->qtds[index].token & EHCI_QTD_ERRORS) != 0) {
			result = (request->qtds[index].token & EHCI_QTD_HALTED) != 0 ?
			    DRV_USB_URB_STALL : DRV_USB_URB_IO_ERROR;
			break;
		}
	}
	for (index = 0; index < request->data_count; index++) {
		unsigned qtd_index = request->data_first + index;
		uint32_t remaining;
		size_t transferred;

		if (qtd_index >= request->qtd_count) {
			result = DRV_USB_URB_IO_ERROR;
			actual = 0;
			break;
		}
		remaining = (request->qtds[qtd_index].token >> 16) & 0x7fffU;
		if (remaining > request->requested[qtd_index]) {
			result = DRV_USB_URB_IO_ERROR;
			actual = 0;
			break;
		}
		transferred = request->requested[qtd_index] - remaining;
		if (actual > length || transferred > length - actual) {
			result = DRV_USB_URB_IO_ERROR;
			actual = 0;
			break;
		}
		actual += transferred;
	}
	ehci_request_commit_toggle(request);
	if (request->input && actual != 0)
		memcpy(drv_usb_urb_buffer(urb),
		    (uint8_t *)request->bounce.address + 8U, actual);

	irq = spin_lock_irqsave(&controller->active_lock);
	if (controller->active != request ||
	    request->state != EHCI_REQUEST_COMPLETING ||
	    drv_usb_urb_hcd_data(urb) != request)
		__builtin_trap();
	controller->active = NULL;
	(void)drv_usb_urb_set_hcd_data(urb, NULL);
	spin_unlock_irqrestore(&controller->active_lock, irq);
	request_free(controller, request);
	drv_usb_hcd_complete(&controller->hcd, urb, result, actual);
}

static void
ehci_retirement_progress(struct ehci_controller *controller)
{
	for (;;) {
		struct ehci_request *complete = NULL;
		struct ehci_request *request;
		unsigned long irq;
		int error = 0, failed = 0, success = 0;

		irq = spin_lock_irqsave(&controller->active_lock);
		request = controller->active;
		if (request == NULL ||
		    __atomic_load_n(&controller->retirement_stopping,
		    __ATOMIC_ACQUIRE) != 0) {
			spin_unlock_irqrestore(&controller->active_lock, irq);
			return;
		}
		switch (request->state) {
		case EHCI_REQUEST_DEACTIVATING_COMPLETE:
		case EHCI_REQUEST_DEACTIVATING_CANCEL:
			if (ehci_request_inactive_locked(request)) {
				error = ehci_retirement_begin_iaa_locked(controller,
				    request);
				if (error == EAGAIN &&
				    ehci_retirement_expired(request)) {
					ehci_retirement_fail_locked(controller, request,
					    ETIMEDOUT, "async-advance setup");
					failed = 1;
				} else if (error != 0 && error != EAGAIN) {
					ehci_retirement_fail_locked(controller, request,
					    error, "async-advance setup");
					failed = 1;
				}
			} else if (ehci_retirement_expired(request)) {
				ehci_retirement_fail_locked(controller, request,
				    ETIMEDOUT, "descriptor deactivation");
				failed = 1;
			}
			break;
		case EHCI_REQUEST_WAIT_IAA_COMPLETE:
		case EHCI_REQUEST_WAIT_IAA_CANCEL:
			error = ehci_retirement_observe_iaa_locked(controller,
			    request);
			if (error == 0) {
				success = 1;
				if (request->state == EHCI_REQUEST_COMPLETING)
					complete = request;
			}
			else if (error != 0 && error != EAGAIN) {
				ehci_retirement_fail_locked(controller, request,
				    error, "async-advance acknowledgement");
				failed = 1;
			} else if (error == EAGAIN &&
			    ehci_retirement_expired(request)) {
				ehci_retirement_fail_locked(controller, request,
				    ETIMEDOUT, "async-advance acknowledgement");
				failed = 1;
			}
			break;
		case EHCI_REQUEST_COMPLETING:
			complete = request;
			break;
		case EHCI_REQUEST_RETIRED_CANCEL:
		case EHCI_REQUEST_FAILED:
		case EHCI_REQUEST_ACTIVE:
		default:
			failed = request->state == EHCI_REQUEST_FAILED;
			spin_unlock_irqrestore(&controller->active_lock, irq);
			if (failed)
				ehci_retirement_report(controller);
			return;
		}
		spin_unlock_irqrestore(&controller->active_lock, irq);
		if (success && __atomic_exchange_n(
		    &controller->retirement_success_reported, 1U,
		    __ATOMIC_ACQ_REL) == 0)
			hal_printf(
			    "ehci: checked async-advance retirement active\n");
		if (failed) {
			ehci_retirement_report(controller);
			return;
		}
		if (complete != NULL) {
			ehci_complete_retired_request(controller, complete);
			return;
		}
		sched_yield();
	}
}

static void
ehci_retirement_worker(void *argument)
{
	struct ehci_controller *controller = argument;

	for (;;) {
		if (__atomic_load_n(&controller->retirement_stopping,
		    __ATOMIC_ACQUIRE) != 0)
			return;
		if (__atomic_exchange_n(&controller->retirement_pending, 0U,
		    __ATOMIC_ACQ_REL) != 0) {
			ehci_retirement_progress(controller);
			continue;
		}
		kernel_wait_task();
	}
}

static void
ehci_retirement_worker_wakeup(struct ehci_controller *controller)
{
	struct thread *worker;
	unsigned long irq;

	__atomic_store_n(&controller->retirement_pending, 1U,
	    __ATOMIC_RELEASE);
	irq = spin_lock_irqsave(&controller->active_lock);
	worker = controller->retirement_worker;
	if (worker != NULL)
		kernel_notify_task(worker->task);
	spin_unlock_irqrestore(&controller->active_lock, irq);
}

static int
ehci_retirement_worker_start(struct ehci_controller *controller)
{
	struct thread *worker;
	unsigned long irq;
	int error;

	irq = spin_lock_irqsave(&controller->active_lock);
	if (controller->retirement_worker != NULL) {
		spin_unlock_irqrestore(&controller->active_lock, irq);
		return EALREADY;
	}
	spin_unlock_irqrestore(&controller->active_lock, irq);
	__atomic_store_n(&controller->retirement_stopping, 0U,
	    __ATOMIC_RELEASE);
	__atomic_store_n(&controller->retirement_pending, 0U,
	    __ATOMIC_RELEASE);
	error = kthread_create(ehci_retirement_worker, controller,
	    SCHED_PRIORITY_DEFAULT, &worker);
	if (error != 0)
		return error;
	irq = spin_lock_irqsave(&controller->active_lock);
	controller->retirement_worker = worker;
	spin_unlock_irqrestore(&controller->active_lock, irq);
	/* Publish before start so the already-registered HCD never admits a URB
	 * without a retirement owner.  A pre-start wake leaves retirement_pending
	 * set; the worker consumes that latch before its first wait. */
	thread_start(worker);
	return 0;
}

static int
ehci_retirement_worker_stop(struct ehci_controller *controller)
{
	struct thread *worker;
	unsigned long irq;
	int error;

	irq = spin_lock_irqsave(&controller->active_lock);
	worker = controller->retirement_worker;
	if (worker == NULL) {
		spin_unlock_irqrestore(&controller->active_lock, irq);
		return 0;
	}
	if (curthread == worker) {
		spin_unlock_irqrestore(&controller->active_lock, irq);
		return EBUSY;
	}
	if (__atomic_load_n(&controller->retirement_stopping,
	    __ATOMIC_ACQUIRE) != 0) {
		spin_unlock_irqrestore(&controller->active_lock, irq);
		return EBUSY;
	}
	__atomic_store_n(&controller->retirement_stopping, 1U,
	    __ATOMIC_RELEASE);
	spin_unlock_irqrestore(&controller->active_lock, irq);
	kernel_notify_task(worker->task);
	while (worker->state != THREAD_ZOMBIE)
		sched_yield();
	irq = spin_lock_irqsave(&controller->active_lock);
	if (controller->retirement_worker != worker)
		__builtin_trap();
	controller->retirement_worker = NULL;
	spin_unlock_irqrestore(&controller->active_lock, irq);
	error = thread_wait(worker, NULL);
	if (error != 0)
		return error;
	return 0;
}

static int build_request(struct ehci_controller*c,struct drv_usb_urb*urb,struct ehci_request**result)
{
	struct ehci_request*r;struct drv_usb_endpoint*ep=drv_usb_urb_endpoint(urb);const struct drv_usb_control_request*control=drv_usb_urb_control_request(urb);size_t length=drv_usb_urb_length(urb),offset=0;unsigned packet,address,endpoint,toggle=0,initial_toggle=0;int error;
	r=hal_malloc(sizeof(*r));if(!r)return ENOMEM;memset(r,0,sizeof(*r));r->urb=urb;r->state=EHCI_REQUEST_ACTIVE;
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
static int
ehci_urb_enqueue(struct drv_usb_hcd *hcd, struct drv_usb_urb *urb)
{
	struct ehci_controller *c = hcd_controller(hcd);
	struct ehci_request *r;
	unsigned long irq;
	int error;

	irq = spin_lock_irqsave(&c->active_lock);
	if (c->quiescing || c->dma_quiesced ||
	    c->retirement_worker == NULL || c->active != NULL ||
	    c->submitting) {
		error = c->quiescing || c->dma_quiesced ||
		    c->retirement_worker == NULL ? ENODEV : EBUSY;
		spin_unlock_irqrestore(&c->active_lock, irq);
		return error;
	}
	c->submitting = 1;
	spin_unlock_irqrestore(&c->active_lock, irq);
	error = build_request(c, urb, &r);
	if (error != 0) {
		irq = spin_lock_irqsave(&c->active_lock);
		if (!c->submitting)
			__builtin_trap();
		c->submitting = 0;
		spin_unlock_irqrestore(&c->active_lock, irq);
		return error;
	}
	irq = spin_lock_irqsave(&c->active_lock);
	if (!c->submitting)
		__builtin_trap();
	c->submitting = 0;
	if (c->quiescing || c->dma_quiesced ||
	    c->retirement_worker == NULL || c->active != NULL) {
		error = c->quiescing || c->dma_quiesced ||
		    c->retirement_worker == NULL ? ENODEV : EBUSY;
		spin_unlock_irqrestore(&c->active_lock, irq);
		request_free(c, r);
		return error;
	}
	c->active = r;
	drv_usb_urb_set_hcd_data(urb, r);
	c->async_head->horizontal =
	    (uint32_t)r->schedule.device_address | EHCI_LINK_QH;
	hal_io_wmb();
	spin_unlock_irqrestore(&c->active_lock, irq);
	return 0;
}

static int
ehci_urb_dequeue(struct drv_usb_hcd *hcd, struct drv_usb_urb *urb)
{
	struct ehci_controller *c = hcd_controller(hcd);
	struct ehci_request *r;
	unsigned long irq;
	int error, inline_retirement;

	irq = spin_lock_irqsave(&c->active_lock);
	r = c->active;
	if (r == NULL || r->urb != urb || drv_usb_urb_hcd_data(urb) != r) {
		spin_unlock_irqrestore(&c->active_lock, irq);
		return EBUSY;
	}
	if (r->state != EHCI_REQUEST_ACTIVE || c->retirement_worker == NULL) {
		spin_unlock_irqrestore(&c->active_lock, irq);
		return EBUSY;
	}
	/* Cancellation owns this terminal race.  Make every in-memory execution
	 * token inactive, then let the worker prove the QH overlay has settled and
	 * perform the checked Async Advance unlink. */
	r->state = EHCI_REQUEST_DEACTIVATING_CANCEL;
	r->retirement_started = sched_ticks();
	ehci_request_deactivate_locked(r);
	inline_retirement = curthread == c->retirement_worker;
	spin_unlock_irqrestore(&c->active_lock, irq);
	/* Completion callbacks execute on the retirement worker.  A callback may
	 * enqueue and synchronously cancel a replacement URB; waking ourselves and
	 * entering the wait loop would deadlock that cancellation forever. */
	if (inline_retirement)
		ehci_retirement_progress(c);
	else
		ehci_retirement_worker_wakeup(c);

	for (;;) {
		irq = spin_lock_irqsave(&c->active_lock);
		if (c->active != r || r->urb != urb ||
		    drv_usb_urb_hcd_data(urb) != r) {
			spin_unlock_irqrestore(&c->active_lock, irq);
			return EBUSY;
		}
		if (r->state == EHCI_REQUEST_RETIRED_CANCEL) {
			ehci_request_commit_toggle(r);
			c->active = NULL;
			(void)drv_usb_urb_set_hcd_data(urb, NULL);
			spin_unlock_irqrestore(&c->active_lock, irq);
			request_free(c, r);
			return 0;
		}
		if (r->state == EHCI_REQUEST_FAILED) {
			error = r->failure_error != 0 ? r->failure_error : EIO;
			spin_unlock_irqrestore(&c->active_lock, irq);
			ehci_retirement_report(c);
			return error;
		}
		if (r->state != EHCI_REQUEST_DEACTIVATING_CANCEL &&
		    r->state != EHCI_REQUEST_WAIT_IAA_CANCEL) {
			spin_unlock_irqrestore(&c->active_lock, irq);
			return EBUSY;
		}
		spin_unlock_irqrestore(&c->active_lock, irq);
		sched_yield();
	}
}
static int ehci_endpoint_enable(struct drv_usb_hcd*h,struct drv_usb_endpoint*e){(void)h;(void)e;return 0;}
static int ehci_endpoint_disable(struct drv_usb_hcd*h,struct drv_usb_endpoint*e){(void)h;(void)e;return 0;}
static uint32_t ehci_frame_number(struct drv_usb_hcd*h){struct ehci_controller*c=hcd_controller(h);return rd32(c->operational,EHCI_FRINDEX)>>3;}
static int ehci_root_status(struct drv_usb_hcd*h,void*b,size_t n,size_t*a){struct ehci_controller*c=hcd_controller(h);uint8_t bits=0;unsigned p;if(!b||n<1)return EINVAL;for(p=0;p<h->root_port_count;p++){uint32_t s=rd32(c->operational,EHCI_PORTSC(p));if(s&(EHCI_PORT_CONNECT_CHANGE|EHCI_PORT_ENABLE_CHANGE))bits|=(uint8_t)(2U<<p);}*(uint8_t*)b=bits;if(a)*a=1;return 0;}
static int ehci_root_control(struct drv_usb_hcd*h,const struct drv_usb_control_request*r,void*b,size_t n,size_t*a)
{struct ehci_controller*c=hcd_controller(h);unsigned p;uint32_t s;if(!r||r->index<1||r->index>h->root_port_count)return EINVAL;p=r->index-1U;s=rd32(c->operational,EHCI_PORTSC(p));if(r->request==0&&b&&n>=4){uint32_t v=0;if(!(s&EHCI_PORT_OWNER)){if(s&EHCI_PORT_CONNECT)v|=1U;if(s&EHCI_PORT_ENABLE)v|=2U;if(s&EHCI_PORT_RESET)v|=0x10U;v|=0x400U;}if(s&EHCI_PORT_CONNECT_CHANGE)v|=0x10000U;if(s&EHCI_PORT_ENABLE_CHANGE)v|=0x20000U;memcpy(b,&v,4);if(a)*a=4;return 0;}if(r->request==3&&r->value==4){if((s&0x0c00U)!=0){wr32(c->operational,EHCI_PORTSC(p),s|EHCI_PORT_OWNER);if(a)*a=0;return 0;}wr32(c->operational,EHCI_PORTSC(p),s|EHCI_PORT_RESET|EHCI_PORT_POWER);if(a)*a=0;return 0;}if(r->request==1){if(r->value==4)s&=~EHCI_PORT_RESET;else if(r->value==16)s|=EHCI_PORT_CONNECT_CHANGE;else if(r->value==17)s|=EHCI_PORT_ENABLE_CHANGE;else return ENOTSUP;wr32(c->operational,EHCI_PORTSC(p),s);if(a)*a=0;return 0;}if(r->request==3&&r->value==1){if(a)*a=0;return 0;}return ENOTSUP;}
static int
ehci_irq(void *argument)
{
	struct ehci_controller *c = argument;
	struct ehci_request *r;
	uint32_t acknowledge, status;
	unsigned long irq;
	int failed = 0, report_controller = 0, wake_worker = 0;

	irq = spin_lock_irqsave(&c->active_lock);
	status = rd32(c->operational, EHCI_USBSTS);
	hal_io_mb();
	r = c->active;
	if (status == UINT32_MAX) {
		report_controller = !c->quarantined;
		c->quiescing = 1;
		c->quarantined = 1;
		if (r != NULL && r->state != EHCI_REQUEST_FAILED) {
			ehci_retirement_fail_locked(c, r, EIO, "IRQ status read");
			failed = 1;
		}
		spin_unlock_irqrestore(&c->active_lock, irq);
		if (failed)
			ehci_retirement_report(c);
		else if (report_controller)
			hal_printf(
			    "ehci: invalid IRQ status; controller quarantined\n");
		return 1;
	}
	acknowledge = status & EHCI_STS_ALL;
	if (acknowledge == 0) {
		spin_unlock_irqrestore(&c->active_lock, irq);
		return 0;
	}
	/* The retirement worker owns the matching IAA acknowledgement.  Other
	 * status sources remain ordinary W1C events and a stale/duplicate IAA is
	 * harmless when no handshake is outstanding. */
	if (r != NULL && ehci_request_waiting_iaa(r) &&
	    (status & EHCI_STS_HSE) == 0)
		acknowledge &= ~EHCI_STS_IAA;
	if (acknowledge != 0)
		wr32(c->operational, EHCI_USBSTS, acknowledge);
	if ((status & EHCI_STS_HSE) != 0) {
		report_controller = !c->quarantined;
		c->quiescing = 1;
		c->quarantined = 1;
		if (r != NULL && r->state != EHCI_REQUEST_FAILED) {
			ehci_retirement_fail_locked(c, r, EIO,
			    "host-system error");
			failed = 1;
		}
	} else if (r != NULL && r->state == EHCI_REQUEST_ACTIVE &&
	    (status & (EHCI_STS_USBINT | EHCI_STS_USBERRINT)) != 0) {
		/* USBINT also denotes a short packet, while an error may leave later
		 * qTDs active.  In both cases this request is the terminal owner; stop
		 * every remaining token before the worker unlinks it. */
		r->state = EHCI_REQUEST_DEACTIVATING_COMPLETE;
		r->retirement_started = sched_ticks();
		ehci_request_deactivate_locked(r);
		wake_worker = 1;
	}
	if (r != NULL && ehci_request_waiting_iaa(r) &&
	    (status & EHCI_STS_IAA) != 0)
		wake_worker = 1;
	spin_unlock_irqrestore(&c->active_lock, irq);
	if (failed)
		ehci_retirement_report(c);
	else if (report_controller)
		hal_printf("ehci: host-system error; controller quarantined\n");
	if (wake_worker)
		ehci_retirement_worker_wakeup(c);
	return 1;
}
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
	spin_init(&c->active_lock, LOCK_RANK_DEVICE,
	    "EHCI active request");
	c->pci = d;
	c->dma_quiesced = 1;
	c->quiescing = 1;
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
	stage = "retirement worker";
	error = ehci_retirement_worker_start(c);
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
