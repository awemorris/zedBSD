#include <kern/uaccess.h>
#include <kern/vmspace.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
struct vmspace_user_lease { int held; };
static struct vmspace_user_lease lease;
static unsigned dirty;
static int refuse;
int vmspace_user_lease_acquire(const struct vmspace_pinned_page *pages,size_t count,struct vmspace_user_lease **out)
{
 assert(pages && count==2 && !lease.held);
 if(refuse)return EBUSY;
 lease.held=1;*out=&lease;return 0;
}
void vmspace_user_lease_release(struct vmspace_user_lease *owner)
{ assert(owner==&lease && lease.held);lease.held=0; }
void vm_private_page_mark_dirty(struct vm_private_page *page)
{ assert(page && lease.held);dirty++; }
int main(void)
{
 struct uaccess_pin pin;struct uaccess_output output;
 struct vmspace_pinned_page pages[2];struct vm_private_page owners[2];
 unsigned char data[2][4096];unsigned i;
 memset(&pin,0,sizeof(pin));memset(pages,0,sizeof(pages));memset(data,0x66,sizeof(data));
 pin.address=4096;pin.size=8192;pin.prot=HAL_SPACE_WRITE;pin.active=1;pin.pages=pages;pin.page_count=2;
 for(i=0;i<2;i++){pages[i].kind=VMSPACE_PINNED_PRIVATE;pages[i].owner.private_page=&owners[i];pages[i].memory.vaddr=data[1-i];pages[i].memory.size=4096;}
 pin.first_offset=1;assert(uaccess_output_acquire(&pin,&output)==EINVAL);pin.first_offset=0;
 pin.prot=HAL_SPACE_READ;assert(uaccess_output_acquire(&pin,&output)==EINVAL);pin.prot=HAL_SPACE_WRITE;
 pin.page_count=1;assert(uaccess_output_acquire(&pin,&output)==EINVAL);pin.page_count=2;
 pages[1].memory.vaddr=NULL;assert(uaccess_output_acquire(&pin,&output)==EINVAL);pages[1].memory.vaddr=data[0];
 pages[1].kind=VMSPACE_PINNED_OBJECT;assert(uaccess_output_acquire(&pin,&output)==EINVAL);pages[1].kind=VMSPACE_PINNED_PRIVATE;
 assert(!lease.held && !dirty && !pin.view_active);
 refuse=1;assert(uaccess_output_acquire(&pin,&output)==EBUSY && !output.pin && !pin.view_active);refuse=0;
 assert(uaccess_output_acquire(&pin,&output)==0 && lease.held && dirty==2 && pin.view_active);
 assert(output.destination.spans==output.spans && output.destination.count==2);
 assert(output.spans[0].address==data[1] && output.spans[1].address==data[0]);
 assert(uaccess_output_acquire(&pin,&output)==EBUSY && output.pin==&pin);
 ((unsigned char *)output.spans[0].address)[0]=0x39;
 assert(data[1][0]==0x39 && data[0][0]==0x66);
 assert(uaccess_output_release(&output)==0 && !lease.held && !pin.view_active && pin.active);
 assert(uaccess_output_release(&output)==EINVAL);
 puts("uaccess output: PASS spans, lease refusal, dirty, parent lifetime; no map symbols");
 return 0;
}
