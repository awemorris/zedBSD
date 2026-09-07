/* Production core reservation ownership with deterministic HCD allocation/retirement.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#define main retained_recovery_main
#include "../../ws004-hardware/tests/usb-recovery-contract-test.c"
#undef main

static unsigned reserve_live, reserve_calls;
static int reserve_error, bad_view;
static size_t reserve_capacity;
static int reserve(struct drv_usb_hcd *h,struct drv_usb_urb *u,size_t size,void **out)
{ (void)h;(void)u;reserve_capacity=size;reserve_calls++;if(reserve_error)return reserve_error;*out=hal_malloc(size);if(!*out)return ENOMEM;reserve_live++;return 0; }
static void unreserve(struct drv_usb_hcd *h,void *p)
{ (void)h;CHECK(reserve_live);reserve_live--;hal_free(p); }
static void *reserve_buffer(struct drv_usb_hcd *h,void *p,size_t *size)
{ (void)h;*size=bad_view?0:reserve_capacity;return p; }
int main(void)
{
 struct fake_controller c;struct drv_usb_device *device;struct drv_usb_urb *u;
 struct drv_usb_hcd_ops ops;void *first,*staging;unsigned i;size_t baseline;
 unsigned char *client=malloc(65536);
 CHECK(client);memset(client,0x57,65536);
 CHECK(drv_usb_init()==0);
 /* Exercise registration, not only a capability changed after registration. */
 baseline=atomic_load(&live_allocations);
 memset(&c,0,sizeof(c));ops=fake_ops;
 c.hcd.name="shared-registration";c.hcd.ops=&ops;
 c.hcd.private_data[0]=(uintptr_t)&c;
 c.hcd.capabilities=DRV_USB_HCD_CAP_SHARED_STAGING;
 CHECK(drv_usb_hcd_register(&c.hcd,&c.bus)==EINVAL);
 ops.urb_reserve=reserve;ops.urb_unreserve=unreserve;
 c.hcd.capabilities|=DRV_USB_HCD_CAP_TRANSFER_RESERVE;
 CHECK(drv_usb_hcd_register(&c.hcd,&c.bus)==EINVAL);
 ops.urb_reserve_buffer=reserve_buffer;
 c.hcd.capabilities=DRV_USB_HCD_CAP_SHARED_STAGING;
 CHECK(drv_usb_hcd_register(&c.hcd,&c.bus)==EINVAL);
 c.hcd.capabilities=DRV_USB_HCD_CAP_TRANSFER_RESERVE;
 CHECK(drv_usb_hcd_register(&c.hcd,&c.bus)==EINVAL);
 c.hcd.capabilities|=DRV_USB_HCD_CAP_SHARED_STAGING;
 CHECK(drv_usb_hcd_register(&c.hcd,&c.bus)==0);
 CHECK(drv_usb_hcd_unregister(&c.hcd)==0);
 CHECK(atomic_load(&live_allocations)==baseline);
 CHECK(drv_usb_driver_register(&recovery_primary_driver)==0);
 CHECK(drv_usb_driver_register(&recovery_secondary_driver)==0);
 set_binding_mode(RECOVERY_BIND_CLAIM_SIBLING);
 baseline=atomic_load(&live_allocations);device=register_controller(&c);
 u=drv_usb_urb_alloc(device,find_endpoint(device,0,0x81),0);CHECK(u);
 CHECK(drv_usb_urb_reserve_transfer(u,65536)==EOPNOTSUPP);
 ops=*c.hcd.ops;ops.urb_reserve=reserve;ops.urb_unreserve=unreserve;
 c.hcd.ops=&ops;c.hcd.capabilities|=DRV_USB_HCD_CAP_TRANSFER_RESERVE;
 CHECK(drv_usb_urb_reserve_transfer(u,4096)==0);first=drv_usb_urb_transfer_reservation(u);staging=u->sync_buffer;
 for(i=1;i<=2;i++) {
  atomic_store(&allocation_failure_countdown,i);
  CHECK(drv_usb_urb_reserve_transfer(u,65536)==ENOMEM);
  CHECK(drv_usb_urb_transfer_reservation(u)==first&&u->sync_buffer==staging&&reserve_live==1);
 }
 reserve_error=EIO;CHECK(drv_usb_urb_reserve_transfer(u,65536)==EIO);reserve_error=0;
 CHECK(drv_usb_urb_transfer_reservation(u)==first&&u->sync_buffer==staging);
 CHECK(drv_usb_urb_reserve_transfer(u,65536)==0);CHECK(reserve_live==1);
 CHECK(drv_usb_urb_reserve_transfer(u,65537)==EMSGSIZE);
 CHECK(drv_usb_urb_setup(u,client,65537,0,10,NULL,NULL)==EINVAL);
 first=drv_usb_urb_transfer_reservation(u);i=reserve_calls;
 atomic_store(&allocation_forbidden,1);
 CHECK(drv_usb_urb_reserve_transfer(u,65536)==0&&reserve_calls==i);
 c.data_behavior=DATA_COMPLETE;
 for(i=0;i<100;i++) {
  CHECK(drv_usb_urb_setup(u,client,65536,0,10,NULL,NULL)==0);
  CHECK(drv_usb_urb_submit(u)==0);CHECK(drv_usb_urb_wait_reusable(u)==0);
  CHECK(drv_usb_urb_transfer_reservation(u)==first);
 }
 CHECK(atomic_load(&forbidden_allocations)==0);atomic_store(&allocation_forbidden,0);
 c.data_behavior=DATA_HOLD;
 CHECK(drv_usb_urb_setup(u,client,65536,0,10,NULL,NULL)==0);CHECK(drv_usb_urb_submit(u)==0);
 cancel_failures=100;CHECK(drv_usb_urb_wait_reusable(u)!=0);
 CHECK(drv_usb_urb_reserve_transfer(u,65536)==EBUSY);CHECK(reserve_live==1);
 free(client);drv_usb_urb_free(u);CHECK(reserve_live==1);
 c.held_data=NULL;fake_complete(&c,u,DRV_USB_URB_COMPLETE,65536);
 CHECK(reserve_live==0);cancel_failures=0;
 /* Shared staging is freed once by its reservation, including late DMA. */
 client=malloc(65536);CHECK(client);memset(client,0x67,65536);
 u=drv_usb_urb_alloc(device,find_endpoint(device,0,0x81),0);CHECK(u);
 CHECK(drv_usb_urb_reserve_transfer(u,4096)==0);
 first=u->transfer_reservation;staging=u->sync_buffer;
 ops.urb_reserve_buffer=reserve_buffer;c.hcd.capabilities|=DRV_USB_HCD_CAP_SHARED_STAGING;
 bad_view=1;CHECK(drv_usb_urb_reserve_transfer(u,65536)==EIO);bad_view=0;
 CHECK(u->transfer_reservation==first && u->sync_buffer==staging && !u->sync_shared);
 atomic_store(&allocation_failure_countdown,1);
 CHECK(drv_usb_urb_reserve_transfer(u,65536)==ENOMEM);
 CHECK(u->transfer_reservation==first && u->sync_buffer==staging);
 CHECK(drv_usb_urb_reserve_transfer(u,65536)==0);
 CHECK(u->sync_shared && u->sync_buffer==u->transfer_reservation && reserve_live==1);
 CHECK(drv_usb_urb_reserve_sync(u,65537)==EMSGSIZE);
 staging=u->sync_buffer;memset(staging,0x29,65536);
 CHECK(drv_usb_urb_setup(u,client,65536,0,10,NULL,NULL)==0);
 CHECK(*(unsigned char *)staging==0x29); /* Input does not copy client bytes. */
 c.data_behavior=DATA_HOLD;CHECK(drv_usb_urb_submit(u)==0);
 cancel_failures=100;CHECK(drv_usb_urb_wait_reusable(u)!=0);
 CHECK(drv_usb_urb_setup(u,client,65536,0,10,NULL,NULL)==EBUSY);
 CHECK(u->sync_client==NULL);free(client);drv_usb_urb_free(u);CHECK(reserve_live==1);
 memset(staging,0x81,65536);c.held_data=NULL;fake_complete(&c,u,DRV_USB_URB_COMPLETE,65536);
 CHECK(reserve_live==0);cancel_failures=0;
 client=malloc(65536);CHECK(client);memset(client,0x67,65536);
 u=drv_usb_urb_alloc(device,find_endpoint(device,0,0x02),0);CHECK(u);
 CHECK(drv_usb_urb_reserve_transfer(u,65536)==0);
 CHECK(drv_usb_urb_setup(u,client,65536,0,10,NULL,NULL)==0);
 CHECK(memcmp(client,u->sync_buffer,65536)==0);
 drv_usb_urb_free(u);free(client);CHECK(reserve_live==0);
 unregister_controller(&c);CHECK(atomic_load(&live_allocations)==baseline);
 CHECK(drv_usb_driver_unregister(&recovery_secondary_driver)==0);
 CHECK(drv_usb_driver_unregister(&recovery_primary_driver)==0);
 puts("USB reserve PASS: fallback, atomic growth/rollback, warm no allocation, late-completion lifetime");
 return 0;
}
