/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <drivers/usb.h>
#include <drivers/usb-uas.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct drv_usb_device { int unused; };
struct drv_usb_endpoint { struct drv_usb_endpoint_descriptor desc; unsigned pipe; };
struct drv_usb_urb { void *buffer; size_t length, actual; unsigned pipe, stream; enum drv_usb_urb_status status; };
static unsigned scenario, configured, submitted, cancelled, allocated, freed;
static uint16_t tag;
static uint64_t ticks;
uint64_t sched_ticks(void) { return ticks; }
void sched_yield(void) { ticks++; }
enum drv_usb_speed drv_usb_device_speed(const struct drv_usb_device *d) { (void)d; return DRV_USB_SPEED_SUPER; }
unsigned drv_usb_device_hcd_capabilities(const struct drv_usb_device *d) { (void)d; return DRV_USB_HCD_CAP_TRANSFER_RESERVE | DRV_USB_HCD_CAP_BULK_STREAMS; }
const struct drv_usb_endpoint_descriptor *drv_usb_endpoint_descriptor(const struct drv_usb_endpoint *e) { return &e->desc; }
int drv_usb_endpoint_configure_streams(struct drv_usb_endpoint *e, unsigned n) { assert(e->pipe != 0 && n == 3); configured++; return 0; }
struct drv_usb_urb *drv_usb_urb_alloc(struct drv_usb_device *d, struct drv_usb_endpoint *e, unsigned n) { struct drv_usb_urb *u=calloc(1,sizeof(*u)); (void)d; assert(n==0 && u); u->pipe=e->pipe; allocated++; return u; }
void drv_usb_urb_free(struct drv_usb_urb *u) { if(u){freed++; free(u);} }
int drv_usb_urb_reserve_sync(struct drv_usb_urb *u, size_t n) { (void)u; assert(n); return 0; }
int drv_usb_urb_reserve_transfer(struct drv_usb_urb *u, size_t n) { return drv_usb_urb_reserve_sync(u,n); }
int drv_usb_urb_setup(struct drv_usb_urb *u, void *b, size_t n, unsigned f, unsigned t, drv_usb_urb_callback_t cb, void *a) { (void)f; assert(t && !cb && !a); u->buffer=b;u->length=n;u->stream=0;u->status=DRV_USB_URB_IDLE;return 0; }
int drv_usb_urb_setup_stream(struct drv_usb_urb *u, unsigned stream, void *b, size_t n, unsigned f, unsigned t, drv_usb_urb_callback_t cb, void *a) { int e=drv_usb_urb_setup(u,b,n,f,t,cb,a); assert(u->pipe!=0 && stream>=1 && stream<=3);u->stream=stream;return e; }
int drv_usb_urb_submit(struct drv_usb_urb *u)
{
	unsigned char *p=u->buffer;
	assert(!(submitted & (1U<<u->pipe)));
	if ((scenario==7 && u->pipe==1) || (scenario==8 && u->pipe>=2)) return EIO;
	if (u->pipe==0) { assert(submitted==0 && u->stream==0); tag=((unsigned)p[2]<<8)|p[3]; }
	else { assert(u->stream==tag); if(u->pipe>=2) assert(submitted & 2); }
	submitted |= 1U<<u->pipe;u->status=DRV_USB_URB_PENDING;return 0;
}
enum drv_usb_urb_status drv_usb_urb_status(const struct drv_usb_urb *u)
{
	if(u->status==DRV_USB_URB_CANCELLED) return u->status;
	if(scenario==6 && u->pipe!=0) return DRV_USB_URB_PENDING;
	if(scenario==3 && u->pipe>=2) return DRV_USB_URB_PENDING;
	if(scenario==5 && u->pipe==1) return DRV_USB_URB_PENDING;
	return DRV_USB_URB_COMPLETE;
}
int drv_usb_urb_wait_reusable(struct drv_usb_urb *u)
{
	unsigned char *p=u->buffer;
	if(u->status==DRV_USB_URB_CANCELLED) return EIO;
	if(scenario==5 && u->pipe>=2) return EIO;
	u->actual=u->length; u->status=DRV_USB_URB_COMPLETE;
	if(u->pipe==1) {
		assert(scenario==2 || (submitted & 12)); /* Status was not waited before data admission. */
		memset(p,0,u->length);p[0]=3;p[2]=tag>>8;p[3]=tag;
		p[6]=scenario==3?2:0;u->actual=16;
		if(scenario==4) p[3]^=0x40;
	}
	if(u->pipe==2) memset(p,0xa5,u->length);
	return 0;
}
size_t drv_usb_urb_actual_length(const struct drv_usb_urb *u) { return u->actual; }
int drv_usb_urb_cancel(struct drv_usb_urb *u) { if(u->status==DRV_USB_URB_PENDING){u->status=DRV_USB_URB_CANCELLED;cancelled++;}return 0; }
int drv_usb_urb_drain(struct drv_usb_urb *u,unsigned t) { (void)u;assert(t);return 0; }
int main(void)
{
	struct drv_usb_device device;
	struct drv_usb_endpoint endpoints[4];
	struct drv_usb_endpoint *pipes[4];
	struct drv_usb_uas_transport transport;
	struct drv_usb_uas_result result;
	unsigned char cdb[10]={0x28},data[512];
	unsigned i;
	int error;
	memset(endpoints,0,sizeof(endpoints));
	for(i=0;i<4;i++){endpoints[i].pipe=i;endpoints[i].desc.address=(i+1)|((i==1||i==2)?0x80:0);endpoints[i].desc.attributes=2;endpoints[i].desc.maximum_packet_size=1024;pipes[i]=&endpoints[i];}
	for(scenario=0;scenario<9;scenario++) {
		memset(&transport,0,sizeof(transport));ticks=0;configured=submitted=cancelled=allocated=freed=0;
		assert(drv_usb_uas_transport_init(&transport,&device,pipes,65536)==0 && configured==3);
		transport.next_tag=3;
		error=drv_usb_uas_transport_execute(&transport,0,cdb,10,scenario==2?DRV_USB_UAS_NO_DATA:(scenario==1?DRV_USB_UAS_WRITE:DRV_USB_UAS_READ),data,scenario==2?0:512,100,&result);
		if(scenario<=3) {
			assert(error==0 && transport.next_tag==1);
			assert(result.status==(scenario==3?2:0));
			assert(result.transferred==((scenario==2||scenario==3)?0:512));
			if(scenario==0) assert(data[511]==0xa5);
			if(scenario==3) assert(cancelled==1);
		} else {
			assert(error!=0 && transport.stopped);
			assert(drv_usb_uas_transport_recover(&transport,100)==EOPNOTSUPP);
			if(scenario==6) assert(cancelled==2);
		}
		assert(drv_usb_uas_transport_stop(&transport)==0 && allocated==freed);
	}
	puts("UAS SuperSpeed coordinated status/data: PASS");return 0;
}
