/* Actual private backing owners; pthread mutex substitutes the IRQ lock. */
#include <kern/vmspace.h>
#include <kern/vm-reclaim.h>
#include <kern/swap.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); abort(); } } while (0)
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned wakes;
void spin_init(struct spinlock *s,enum lock_rank rank,const char *name)
{ (void)s;(void)rank;(void)name; }
unsigned long spin_lock_irqsave(struct spinlock *s)
{ (void)s;CHECK(pthread_mutex_lock(&lock)==0);return 0; }
void spin_unlock_irqrestore(struct spinlock *s,unsigned long irq)
{ (void)s;(void)irq;CHECK(pthread_mutex_unlock(&lock)==0); }
void waitq_init(struct wait_queue *q,const char *name)
{ memset(q,0,sizeof(*q));q->name=name; }
void waitq_wake_all(struct wait_queue *q) { q->sequence++;wakes++; }
void hal_fatal(const char *file,int line,const char *message)
{ fprintf(stderr,"%s:%d %s\n",file,line,message);abort(); }
struct swap_backend *swap_system_backend(void) { return NULL; }
void swap_free_slot(struct swap_backend *s,uint32_t slot)
{ (void)s;(void)slot;abort(); }
void vm_private_page_free_metadata(struct vm_private_page *p) { (void)p;abort(); }
int hal_pmem_free(struct hal_pmem *p) { (void)p;abort(); }

struct attempt { struct vm_private_page *page;int result; };
static void *upgrade(void *arg)
{
 struct attempt *a=arg;
 a->result=vm_private_page_io_try_upgrade(a->page,2);
 return NULL;
}
static void refused(struct vm_private_page *p,unsigned pins,int expected)
{
 unsigned refs=refcount_load(&p->refs),flags=p->flags,count=p->pin_count;
 uint64_t generation=p->generation;
 CHECK(vm_private_page_io_try_upgrade(p,pins)==expected);
 CHECK(p->flags==flags && p->pin_count==count && p->generation==generation);
 CHECK(refcount_load(&p->refs)==refs);
}
int main(void)
{
 struct vm_private_page p;
 struct hal_pmem memory;
 unsigned char bytes[4096];
 pthread_t threads[8];
 struct attempt attempts[8];
 unsigned i,round,winners,before;
 memset(&p,0,sizeof(p));memset(bytes,0xa5,sizeof(bytes));
 vm_private_page_init(&p);
 p.pmem.vaddr=bytes;p.pmem.size=sizeof(bytes);p.pmem.paddr=4096;
 CHECK(vm_private_page_io_try_upgrade(NULL,1)==EINVAL);
 refused(&p,0,EINVAL);refused(&p,1,EBUSY);
 p.flags=VM_PAGE_RESIDENT;
 CHECK(vm_private_page_pin(&p,&memory)==0);
 CHECK(vm_private_page_pin(&p,&memory)==0);
 CHECK(p.pin_count==2 && refcount_load(&p.refs)==3);
 refused(&p,1,EBUSY);refused(&p,3,EBUSY);
 p.active_operations=1;refused(&p,2,EBUSY);p.active_operations=0;
 p.flags|=VM_PAGE_BUSY;refused(&p,2,EBUSY);p.flags&=~VM_PAGE_BUSY;
 p.flags&=~VM_PAGE_RESIDENT;refused(&p,2,EBUSY);p.flags|=VM_PAGE_RESIDENT;
 for(round=0;round<64;round++) {
  winners=0;
  for(i=0;i<8;i++) {
   attempts[i].page=&p;
   CHECK(pthread_create(&threads[i],NULL,upgrade,&attempts[i])==0);
  }
  for(i=0;i<8;i++) {
   CHECK(pthread_join(threads[i],NULL)==0);
   CHECK(attempts[i].result==0 || attempts[i].result==EBUSY);
   winners+=attempts[i].result==0;
  }
  CHECK(winners==1 && p.pin_count==2 && refcount_load(&p.refs)==3);
  CHECK(vm_private_page_pin(&p,&memory)==EBUSY);
  CHECK(vm_private_page_operation_try_begin(&p)==EBUSY);
  CHECK(vm_private_page_io_try_acquire(&p)==EBUSY);
  before=wakes;vm_private_page_io_release(&p);
  CHECK(wakes==before+1 && !(p.flags&VM_PAGE_BUSY));
 }
 p.generation=UINT64_MAX;
 CHECK(vm_private_page_io_try_upgrade(&p,2)==0 && p.generation==1);
 vm_private_page_io_release(&p);
 vm_private_page_unpin(&p);vm_private_page_unpin(&p);
 CHECK(p.pin_count==0 && refcount_load(&p.refs)==1);
 for(i=0;i<sizeof(bytes);i++)CHECK(bytes[i]==0xa5);
 puts("private pin upgrade: PASS (64 concurrent rounds, ownership conservation)");
 return 0;
}
