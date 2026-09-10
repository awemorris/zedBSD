/* Exercise actual controller registry and teardown claims without MMIO. */
#include <stdio.h>
#include <stdlib.h>
#include "src/drivers/pci/pci-nvme.c"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "NVMe registry line %d: %s\n", __LINE__, #x); abort(); } } while (0)
static uint64_t ticks;
unsigned long spin_lock_irqsave(struct spinlock *lock)
{ CHECK(!lock->held.value); lock->held.value=1; return 0; }
void spin_unlock_irqrestore(struct spinlock *lock, unsigned long flags)
{ (void)flags; CHECK(lock->held.value); lock->held.value=0; }
void waitq_wake_all(struct wait_queue *queue) { (void)queue; }
uint64_t clock_ticks(void) { return ticks++; }
void sched_yield(void) { ticks++; }
int drv_pci_device_set_driver_data(struct drv_pci_device *device, void *data)
{ CHECK(device!=NULL && data!=NULL); return 0; }

static void test_reset_isolation(void)
{
 struct nvme_controller a={0},b={0},snapshot;
 unsigned char aq[4096],ac[4096],bq[4096],bc[4096];
 unsigned i;
 a.io_slot_count=b.io_slot_count=1;
 a.io_queue_depth=b.io_queue_depth=4;
 a.io_owned=b.io_owned=1;a.io_pending=b.io_pending=1;
 a.io_submission=(void *)aq;a.io_completion=(void *)ac;
 b.io_submission=(void *)bq;b.io_completion=(void *)bc;
 a.io_submission_dma.address=aq;a.io_submission_dma.size=sizeof(aq);
 a.io_completion_dma.address=ac;a.io_completion_dma.size=sizeof(ac);
 b.io_submission_dma.address=bq;b.io_submission_dma.size=sizeof(bq);
 b.io_completion_dma.address=bc;b.io_completion_dma.size=sizeof(bc);
 memset(aq,0x31,sizeof(aq));memset(ac,0x32,sizeof(ac));
 memset(bq,0x61,sizeof(bq));memset(bc,0x62,sizeof(bc));
 drv_nvme_io_lifecycle_init(&a.io_slots[0].lifecycle);
 CHECK(drv_nvme_io_lifecycle_online(&a.io_slots[0].lifecycle)==0);
 CHECK(drv_nvme_io_lifecycle_begin_bio(&a.io_slots[0].lifecycle)==0);
 CHECK(drv_nvme_io_lifecycle_submit(&a.io_slots[0].lifecycle,7,1)==0);
 a.io_slots[0].state=NVME_IO_SLOT_ACTIVE;a.io_slots[0].posted=1;
 b.io_slots[0]=a.io_slots[0];snapshot=b;
 nvme_io_fail_all_locked(&a,ETIMEDOUT);
 CHECK(a.io_slots[0].error==ETIMEDOUT && a.io_pending==0);
 CHECK(memcmp(&b,&snapshot,sizeof(b))==0);
 CHECK(nvme_io_queue_memory_reset(&a)==EBUSY);
 a.io_owned=0; /* The caller has now consumed its failed BIO. */
 CHECK(nvme_io_queue_memory_reset(&a)==0);
 CHECK(a.io_slots[0].state==NVME_IO_SLOT_FREE && a.io_epoch==1);
 nvme_io_quarantine(&a,EIO,1);
 CHECK(memcmp(&b,&snapshot,sizeof(b))==0);
 for(i=0;i<sizeof(aq);i++)CHECK(aq[i]==0 && ac[i]==0 && bq[i]==0x61 && bc[i]==0x62);
}

int main(void)
{
 struct nvme_controller a={0},b={0},c={0},*claimed=NULL;
 int devices[3];
 a.pci=(void *)&devices[0];b.pci=(void *)&devices[1];c.pci=(void *)&devices[2];
 a.controller_enabled=b.controller_enabled=c.controller_enabled=1;
 nvme_publish_controller(&a);nvme_publish_controller(&b);nvme_publish_controller(&c);
 CHECK(nvme_controllers==&a && a.next==&b && b.next==&c && c.next==NULL);
 CHECK(nvme_detach_claim(b.pci,&claimed)==0 && claimed==&b);
 CHECK(!a.detach_busy && b.detach_busy && !c.detach_busy);
 CHECK(nvme_shutdown_claim(b.pci,&claimed)==EBUSY);
 CHECK(nvme_shutdown_claim(c.pci,&claimed)==0 && claimed==&c);
 CHECK(!a.stopping && c.stopping);
 nvme_detach_release(&c,1);nvme_detach_release(&b,1);

 /* A busy controller times out only its own claim; its sibling still works. */
 b.probe_busy=1;
 CHECK(nvme_detach_claim(b.pci,&claimed)==EBUSY && !b.detach_busy);
 CHECK(nvme_detach_claim(a.pci,&claimed)==0 && claimed==&a);
 nvme_detach_release(&a,1);b.probe_busy=0;

 /* Removing a middle node, then the head, must retain the unrelated node. */
 nvme_unpublish_controller(&b);
 CHECK(nvme_controllers==&a && a.next==&c && b.next==NULL);
 CHECK(nvme_detach_claim(b.pci,&claimed)==EBUSY);
 nvme_unpublish_controller(&a);
 CHECK(nvme_controllers==&c && a.next==NULL);
 CHECK(nvme_shutdown_claim(c.pci,&claimed)==0 && claimed==&c);
 nvme_detach_release(&c,1);nvme_unpublish_controller(&c);
 CHECK(nvme_controllers==NULL);
 test_reset_isolation();
 puts("NVMe registry ownership and timeout/reset isolation: PASS");
 return 0;
}
