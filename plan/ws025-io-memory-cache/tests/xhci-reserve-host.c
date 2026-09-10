/* Production xHCI request/reservation paths, deterministic coherent-DMA boundary.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
typedef int tid_t; /* Host libc lacks zedBSD thread IDs. */
#include "../../../src/drivers/pci/pci-xhci.c"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);abort();}}while(0)
struct drv_usb_urb { void *reservation;int bulk; };
static unsigned allocations, frees, dma_allocations, dma_frees, fail_heap, fail_dma;
void *hal_malloc(size_t n) {if(fail_heap)return NULL;allocations++;return malloc(n);}
void hal_free(void *p) {if(p){frees++;free(p);}}
void *drv_usb_urb_transfer_reservation(const struct drv_usb_urb *u) {return u?u->reservation:NULL;}
unsigned long spin_lock_irqsave(struct spinlock *s)
{CHECK(!s->held.value);s->held.value=1;return 0;}
void spin_unlock_irqrestore(struct spinlock *s,unsigned long f)
{(void)f;CHECK(s->held.value);s->held.value=0;}
int drv_dma_alloc_coherent(struct drv_dma_device *d,size_t n,size_t a,struct drv_dma_buffer *b)
{
 (void)d;if(fail_dma)return ENOMEM;dma_allocations++;memset(b,0,sizeof(*b));
 b->address=aligned_alloc(a,(n+a-1)&~(a-1));CHECK(b->address);b->size=n;
 b->device_address=UINT64_C(0x100000000);return 0;
}
void drv_dma_free_coherent(struct drv_dma_device *d,struct drv_dma_buffer *b)
{(void)d;CHECK(b->address);dma_frees++;free(b->address);memset(b,0,sizeof(*b));}
struct drv_dma_vector { void *address;unsigned count;struct drv_dma_segment segments[32]; };
static unsigned vectors_live;
struct drv_usb_endpoint *drv_usb_urb_endpoint(const struct drv_usb_urb *u)
{ return u && u->bulk?(struct drv_usb_endpoint *)(uintptr_t)u:NULL; }
enum drv_usb_transfer_type drv_usb_endpoint_type(const struct drv_usb_endpoint *e)
{ (void)e;return DRV_USB_TRANSFER_BULK; }
int drv_dma_vector_create(struct drv_dma_device *d,size_t size,struct drv_dma_vector **out)
{
 struct drv_dma_vector *v;unsigned n;(void)d;
 v=calloc(1,sizeof(*v));CHECK(v);v->address=malloc(size);CHECK(v->address);
 v->count=(unsigned)((size+4095)/4096);
 for(n=0;n<v->count;n++){v->segments[n].address=UINT64_C(0x100000000)+n*8192;v->segments[n].length=4096;}
 vectors_live++;*out=v;return 0;
}
int drv_dma_vector_free(struct drv_dma_vector *v)
{ CHECK(vectors_live);vectors_live--;free(v->address);free(v);return 0; }
void *drv_dma_vector_address(const struct drv_dma_vector *v) { return v->address; }
unsigned drv_dma_vector_count(const struct drv_dma_vector *v) { return v->count; }
int drv_dma_vector_segment(const struct drv_dma_vector *v,unsigned n,struct drv_dma_segment *segment)
{ if(n>=v->count)return EINVAL;*segment=v->segments[n];return 0; }
void hal_io_wmb(void) {}
void hal_fatal(const char *file,int line,const char *message)
{ fprintf(stderr,"%s:%d %s\n",file,line,message);abort(); }
int main(void)
{
 struct xhci_controller c={0};struct drv_usb_urb u={0},other={0};
 struct xhci_request *r,*held;struct xhci_urb_reservation *reservation;
 struct xhci_ring ring={0};struct xhci_trb trbs[XHCI_RING_TRBS]={0};
 size_t actual,capacity;unsigned i,old_heap,old_dma;int error;
 c.hcd.private_data[0]=(uintptr_t)&c;
 fail_heap=1;CHECK(xhci_urb_reserve(&c.hcd,&u,65536,&u.reservation)==ENOMEM);fail_heap=0;
 fail_dma=1;CHECK(xhci_urb_reserve(&c.hcd,&u,65536,&u.reservation)==ENOMEM);fail_dma=0;
 CHECK(allocations==frees);
 CHECK(xhci_urb_reserve(&c.hcd,&u,65536,&u.reservation)==0);
 CHECK(xhci_urb_reserve(&c.hcd,&other,65536,&other.reservation)==0);
 CHECK(drv_dma_alloc_coherent(NULL,8192,8192,&c.transfer_reserve)==0);
 old_heap=allocations;old_dma=dma_allocations;
 for(i=0;i<1000;i++) {
  r=xhci_request_alloc(&c,&c.hcd,65536,DRV_USB_URB_RECLAIM_SAFE,&u,&error);
  CHECK(r&&error==0&&!c.transfer_reserve_busy);CHECK(r->bounce.device_address>UINT32_MAX);
  CHECK(normal_trb_count(r->bounce.device_address,65536)==1);
  memset(r->bounce.address,0x63,65536);
  CHECK(xhci_request_alloc(&c,&c.hcd,65536,0,&u,&error)==NULL&&error==EBUSY);
  xhci_request_release(&c,r);
 }
 CHECK(allocations==old_heap&&dma_allocations==old_dma);
 held=xhci_request_alloc(&c,&c.hcd,65536,0,&u,&error);CHECK(held);
 r=xhci_request_alloc(&c,&c.hcd,65536,0,&other,&error);CHECK(r);xhci_request_release(&c,r);
 r=xhci_request_alloc(&c,&c.hcd,8192,DRV_USB_URB_RECLAIM_SAFE,NULL,&error);
 CHECK(r&&c.transfer_reserve_busy);xhci_request_release(&c,r);
 CHECK(xhci_request_alloc(&c,&c.hcd,65536,0,&u,&error)==NULL&&error==EBUSY);
 xhci_request_release(&c,held);reservation=u.reservation;reservation->generation=UINT64_MAX;
 CHECK(xhci_request_alloc(&c,&c.hcd,65536,0,&u,&error)==NULL&&error==EOVERFLOW);
 xhci_urb_unreserve(&c.hcd,u.reservation);xhci_urb_unreserve(&c.hcd,other.reservation);
 drv_dma_free_coherent(NULL,&c.transfer_reserve);CHECK(allocations==frees&&dma_allocations==dma_frees);
 u.bulk=1;u.reservation=NULL;
 CHECK(xhci_urb_reserve(&c.hcd,&u,65536,&u.reservation)==0);
 r=xhci_request_alloc(&c,&c.hcd,65536,0,&u,&error);CHECK(r && r->vector && !r->bounce.address);
 CHECK(xhci_urb_reserve_buffer(&c.hcd,u.reservation,&capacity)==r->staging && capacity==65536);
 r->length=65536;CHECK(xhci_sg_plan(r,65536)==0 && r->normal_count==16);
 CHECK(xhci_sg_short(r,5,777,&actual) && actual==6*4096-777);
 CHECK(!xhci_sg_short(r,16,0,&actual) && !xhci_sg_short(r,0,4097,&actual));
 ring.trbs=trbs;ring.cycle=1;xhci_sg_enqueue(&ring,r,1,512,0);CHECK(ring.enqueue==16);
 for(i=0;i<16;i++){
  CHECK(trbs[i].parameter_high==1 && (trbs[i].status&0x1ffffU)==4096);
  CHECK(trbs[i].control&DRV_XHCI_TRB_ISP);
  CHECK((trbs[i].control&XHCI_TRB_CHAIN)!=0 || i==15);
 }
 CHECK((trbs[15].control&XHCI_TRB_IOC)!=0 && (trbs[15].status>>17)==0);
 r->length=512;CHECK(xhci_sg_plan(r,512)==0);ring.enqueue=0;
 xhci_sg_enqueue(&ring,r,0,512,1);CHECK(ring.enqueue==2 && (trbs[0].status>>17)==1 && trbs[1].status==0);
 r->vector->segments[0].address=UINT64_C(0x10000ff00);
 CHECK(xhci_sg_plan(r,4096)==0 && r->normal_count==2 && r->normal[0].length==256);
 CHECK(xhci_sg_short(r,1,20,&actual) && actual==4076);
 r->vector->segments[0].address=UINT64_MAX;CHECK(xhci_sg_plan(r,4096)==EINVAL);
 r->vector->count=33;CHECK(xhci_sg_plan(r,4096)==EINVAL);r->vector->count=16;
 CHECK(xhci_sg_plan(r,65537)==EINVAL);
 xhci_request_release(&c,r);xhci_urb_unreserve(&c.hcd,u.reservation);
 CHECK(vectors_live==0 && allocations==frees && dma_allocations==dma_frees);
 puts("xHCI reserve PASS: allocation failure, 64 KiB/high DMA, warm reuse, isolated busy owner, separate reclaim reserve, generation exhaustion");
 return 0;
}
