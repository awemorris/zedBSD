/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <kern/writeback.h>
#include <kern/cache-memory.h>
#include <kern/disk.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../../ws018/tests/mount-thread-host.h"
static uint64_t target=16U*1024U*1024U;
bool hal_irq_disable(void) { return false; }
void hal_irq_enable(void) {}
void hal_fatal(const char *file,int line,const char *message)
{ fprintf(stderr,"%s:%d %s\n",file,line,message);abort(); }
void disk_ref(struct disk *disk) { refcount_get(&disk->d_refs); }
void disk_release(struct disk *disk) { assert(!refcount_put(&disk->d_refs)); }
void cache_memory_get_stats(struct cache_memory_stats *stats)
{ memset(stats,0,sizeof(*stats));stats->target_bytes=target; }
static void writer(void *argument)
{
 struct writeback_budget *budget=argument;
 struct writeback_ticket ticket={0};struct writeback_budget_stats stats;
 unsigned index;
 for(index=0;index<10000;index++) {
  while(writeback_ticket_reserve(budget,&ticket)==EAGAIN)host_thread_yield();
  assert(ticket.budget==budget);
  writeback_ticket_commit(&ticket,4096);writeback_ticket_release(&ticket);
  writeback_budget_snapshot(&stats);assert(stats.dirty+stats.reserved<=stats.high);
  writeback_budget_clean(budget,4096);
 }
}
int main(void)
{
 struct disk disk[5];struct writeback_budget budget[5]={0},duplicate={0};
 struct writeback_ticket tickets[64]={0},other={0};
 struct writeback_budget_stats stats;
 unsigned index,count;uint64_t high,low,device;void *threads[4];
 memset(disk,0,sizeof(disk));
 for(index=0;index<5;index++)refcount_init(&disk[index].d_refs,1);
 writeback_budget_limits(UINT64_MAX,&high,&low,&device);
 assert(high==64U*1024U*1024U && low==high/2 && device==high/4);
 target=4096;assert(writeback_budget_attach(&budget[0],&disk[0])==ENOMEM);
 target=16U*1024U*1024U;
 for(index=0;index<4;index++)assert(writeback_budget_attach(&budget[index],&disk[index])==0);
 assert(writeback_budget_attach(&budget[4],&disk[4])==EAGAIN);
 assert(writeback_budget_attach(&duplicate,&disk[0])==EEXIST);
 assert(refcount_load(&disk[0].d_refs)==2 && refcount_load(&disk[4].d_refs)==1);
 for(count=0;count<64;count++) {
  int error=writeback_ticket_reserve(&budget[0],&tickets[count]);
  if(error==EAGAIN)break;
  assert(error==0);
 }
 assert(count>0 && count<64);
 assert(writeback_ticket_reserve(&budget[0],&tickets[0])==EBUSY);
 assert(writeback_ticket_reserve(&budget[1],&other)==0);
 writeback_ticket_commit(&tickets[0],65536);writeback_ticket_release(&tickets[0]);
 for(index=1;index<count;index++)writeback_ticket_release(&tickets[index]);
 writeback_ticket_release(&other);writeback_ticket_release(&other);
 assert(writeback_budget_detach(&budget[0])==EBUSY);
 assert(writeback_budget_quiesce(&budget[0],1)==0);
 assert(writeback_ticket_reserve(&budget[0],&other)==EAGAIN);
 assert(writeback_budget_quiesce(&budget[0],0)==0);
 target=512U*1024U;
 assert(writeback_ticket_reserve(&budget[0],&other)==EAGAIN);
 assert(budget[0].dirty==65536);
 target=16U*1024U*1024U;
 for(index=0;index<4;index++)threads[index]=host_thread_start(writer,&budget[index%2]);
 for(index=0;index<4;index++)host_thread_join(threads[index]);
 writeback_budget_snapshot(&stats);
 assert(stats.dirty==65536 && stats.reserved==0 && stats.tickets==0 && stats.devices==4);
 writeback_budget_clean(&budget[0],65536);
 for(index=0;index<4;index++) {
  assert(writeback_budget_detach(&budget[index])==0);
  assert(refcount_load(&disk[index].d_refs)==1);
 }
 writeback_budget_snapshot(&stats);
 assert(stats.dirty==0 && stats.reserved==0 && stats.devices==0);
 puts("writeback credits PASS: before-lease reservation, independent device, quiesce/retry, shrink, exact retirement, 40000 concurrent commits");
 return 0;
}
