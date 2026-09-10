#include <kern/vm-kernel-map.h>
/* Actual uaccess view owner with deterministic lease/HAL failure boundaries. */
#include <kern/uaccess.h>
#include <kern/vmspace.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);abort();} } while(0)
struct vm_kernel_map { unsigned pins,live; };
struct vmspace_user_lease { unsigned held; };
static struct vm_kernel_map mapping;
static struct vmspace_user_lease lease;
static unsigned step,fail_step,available=1,retired,direction,dirty;
static int acquire(struct uaccess_pin *pin,struct uaccess_view *view)
{ return uaccess_view_acquire(pin,view); }
static unsigned char storage[8192];
static int fail(void) { return ++step==fail_step; }
void hal_fatal(const char *file,int line,const char *message)
{ fprintf(stderr,"%s:%d %s\n",file,line,message);abort(); }
unsigned vm_kernel_map_capabilities(void) { return available; }
int vmspace_user_input_lease_acquire(const struct vmspace_pinned_page *pages,size_t count,struct vmspace_user_lease **out)
{ CHECK(!direction);CHECK(pages && count==2 && !lease.held);if(fail())return EBUSY;lease.held=1;*out=&lease;return 0; }
int vmspace_user_lease_acquire(const struct vmspace_pinned_page *pages,size_t count,struct vmspace_user_lease **out)
{ CHECK(direction && pages && count==2 && !lease.held);if(fail())return EBUSY;lease.held=1;*out=&lease;return 0; }
void vm_private_page_mark_dirty(struct vm_private_page *page)
{ CHECK(direction && lease.held && mapping.live && mapping.pins && page);dirty++; }
void vmspace_user_lease_release(struct vmspace_user_lease *owner)
{ CHECK(owner==&lease && lease.held && !mapping.live && !mapping.pins);lease.held=0; }
int vm_kernel_map_reserve(size_t size,struct vm_kernel_map **out)
{ CHECK(lease.held && size==8192);if(fail())return HAL_ERR_NOMEM;*out=&mapping;mapping.live=1;return HAL_OK; }
#ifndef OMIT_BORROW
int vm_kernel_map_borrow(struct vm_kernel_map *m,const hal_physaddr_t *pages,size_t count,int writable)
{ CHECK(m==&mapping && lease.held && count==2 && writable==(int)direction);CHECK(pages[0]==4096 && pages[1]==12288);return fail()?HAL_ERR_NOMEM:HAL_OK; }
#endif
int vm_kernel_map_pin(struct vm_kernel_map *m,void **out)
{ CHECK(m==&mapping && m->live && lease.held);if(fail())return HAL_ERR_NOMEM;m->pins++;*out=storage;return HAL_OK; }
void vm_kernel_map_unpin(struct vm_kernel_map *m)
{ CHECK(m==&mapping && m->pins && lease.held);m->pins--; }
int vm_kernel_map_release(struct vm_kernel_map *m)
{ CHECK(m==&mapping && m->live && lease.held);if(m->pins)return HAL_ERR_BUSY;m->live=0;retired++;return HAL_OK; }
int main(void)
{
 struct uaccess_pin pin;
 struct vmspace_pinned_page pages[2];
 struct uaccess_view view;
 unsigned i,before;
 memset(&pin,0,sizeof(pin));memset(pages,0,sizeof(pages));memset(storage,0x39,sizeof(storage));
 for(direction=0;direction<1;direction++) {
 pin.address=4096;pin.size=8192;pin.prot=direction?HAL_SPACE_WRITE:HAL_SPACE_READ;pin.active=1;pin.page_count=2;pin.pages=pages;
 pages[0].owner.private_page=(struct vm_private_page *)&pages[0];
 pages[1].owner.private_page=(struct vm_private_page *)&pages[1];
 pages[0].memory.paddr=4096;pages[1].memory.paddr=12288;
#ifdef OMIT_BORROW
 CHECK(acquire(&pin,&view)==ENOTSUP && !lease.held && step==0);
 puts("uaccess view: PASS (absent optional HAL symbol fallback)");
 continue;
#endif
 available=0;CHECK(acquire(&pin,&view)==ENOTSUP && step==0);available=1;
 pin.first_offset=1;CHECK(acquire(&pin,&view)==EINVAL);pin.first_offset=0;
 pin.prot=direction?HAL_SPACE_READ:HAL_SPACE_WRITE;CHECK(acquire(&pin,&view)==EINVAL);pin.prot=direction?HAL_SPACE_WRITE:HAL_SPACE_READ;
 pin.page_count=1;CHECK(acquire(&pin,&view)==EINVAL);pin.page_count=2;
 for(i=1;i<=4;i++) {
  step=0;fail_step=i;before=retired;
  CHECK(acquire(&pin,&view)!=0);
  CHECK(!view.pin && !pin.view_active && pin.active && pin.pages==pages);
  CHECK(!lease.held && !mapping.live && !mapping.pins);
  CHECK(retired==before+(i>=3));
 }
 step=fail_step=dirty=0;
 CHECK(acquire(&pin,&view)==0 && pin.view_active && mapping.pins==1);
 CHECK(view.address==storage && view.size==sizeof(storage));
 CHECK(dirty==(direction?2U:0U));
 CHECK(acquire(&pin,&view)==EBUSY && view.address==storage);
 mapping.pins++;before=retired;
 CHECK(uaccess_view_release(&view)==EBUSY);
 CHECK(mapping.pins==2 && lease.held && pin.view_active && retired==before);
 mapping.pins--;
 CHECK(uaccess_view_release(&view)==0 && retired==before+1);
 CHECK(!lease.held && !mapping.pins && !pin.view_active && !view.pin);
 CHECK(pin.active && pin.pages==pages);
 for(i=0;i<sizeof(storage);i++)CHECK(storage[i]==(direction && i==0?0x5a:0x39));
 step=0;
 }
#ifndef OMIT_BORROW
 puts("uaccess input views: PASS (failure rollback, retirement ordering, dirty accounting, busy retry)");
#endif
 return 0;
}
