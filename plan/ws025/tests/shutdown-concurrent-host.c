/* Exercises real shutdown serialization through deterministic thread gates. */
#include <kern/system-device.h>
#include <kern/readahead.h>
#include "../../ws018/tests/mount-thread-host.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
static unsigned begins, syncs, aborts, commits, net, usb, pci, waits;
static int results[3];
static unsigned read_begins, read_ends, trims;
static int read_fail, trim_fail, wb_fail;
static struct readahead_boundary *read_owner;
int readahead_boundary_begin(struct readahead_boundary *boundary, struct mount *mount)
{
 assert(mount==NULL && !boundary->active && read_owner==NULL);read_begins++;
 if(read_fail)return EAGAIN;
 boundary->active=1;read_owner=boundary;return 0;
}
void readahead_boundary_end(struct readahead_boundary *boundary)
{
 assert(boundary==read_owner && boundary->active);read_ends++;
 boundary->active=0;read_owner=NULL;
}
int readahead_trim(void) { assert(read_owner!=NULL);trims++;return trim_fail?EIO:0; }
int writeback_shutdown_begin(void) { assert(read_owner!=NULL);begins++; return wb_fail?EIO:0; }
int mount_sync_all(void)
{
 assert(read_owner!=NULL);syncs++;
 if(syncs==1) { host_gate_pause(1); return EIO; }
 return 0;
}
void writeback_shutdown_finish(int committed)
{ assert(read_owner!=NULL);if(committed)commits++;else aborts++; }
void net_shutdown_for_boot(void) { assert(commits==1 && read_owner!=NULL);net++; }
void drv_usb_shutdown(void)
{ assert(net==1);usb++;host_gate_pause(2); }
void drv_pci_shutdown(void) { assert(usb==1);pci++; }
void shutdown_test_yield(void)
{ __atomic_add_fetch(&waits,1,__ATOMIC_RELEASE);host_thread_yield(); }
static void caller(void *argument)
{ *(int *)argument=system_shutdown_prepare(); }
int main(void)
{
 void *first,*second,*third;
 unsigned before;
 /* Admission and scratch failures leave all device layers available for retry. */
 read_fail=1;assert(system_shutdown_prepare()==EAGAIN && begins==0 && read_owner==NULL);read_fail=0;
 trim_fail=1;assert(system_shutdown_prepare()==EIO && begins==0 && read_owner==NULL);trim_fail=0;
 wb_fail=1;assert(system_shutdown_prepare()==EIO && begins==1 && read_owner==NULL);wb_fail=0;
 assert(read_begins==3 && read_ends==2 && trims==2 && net==0 && usb==0 && pci==0);
 begins=0;
 host_gate_reset(1);host_gate_reset(2);
 first=host_thread_start(caller,&results[0]);host_gate_wait(1);
 second=host_thread_start(caller,&results[1]);
 while(__atomic_load_n(&waits,__ATOMIC_ACQUIRE)==0)host_thread_yield();
 assert(begins==1 && syncs==1 && commits==0 && net==0 && usb==0 && pci==0);
 host_gate_release(1);
 host_gate_wait(2);
 /* The second caller retries the first failure; the third joins successful teardown. */
 before=__atomic_load_n(&waits,__ATOMIC_ACQUIRE);
 third=host_thread_start(caller,&results[2]);
 while(__atomic_load_n(&waits,__ATOMIC_ACQUIRE)==before)host_thread_yield();
 assert(begins==2 && syncs==2 && aborts==1 && commits==1);
 assert(net==1 && usb==1 && pci==0);
 host_gate_release(2);
 host_thread_join(first);host_thread_join(second);host_thread_join(third);
 assert(results[0]==EIO && results[1]==0 && results[2]==0);
 assert(begins==2 && syncs==2 && aborts==1 && commits==1);
 assert(net==1 && usb==1 && pci==1);
 assert(read_begins==5 && read_ends==3 && trims==4 && read_owner!=NULL);
 assert(system_shutdown_prepare()==0 && pci==1 && begins==2);
 assert(read_begins==5 && read_owner!=NULL);
 puts("shutdown concurrency PASS: failed owner, waiter retry, join through device teardown, idempotence");
 return 0;
}
