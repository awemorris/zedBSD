/*
 * Physical memory management: range descriptors over a page-usage
 * bitmap.  Purely range bookkeeping; page-table manipulation is
 * elsewhere (univ.c / future space API).
 */

#ifndef _SYS_ARCH_PMEM_H_
#define _SYS_ARCH_PMEM_H_

#include <sys/types.h>

/* Memory block descriptor. */
struct pmem_desc {
	void *vaddr;
	void *paddr;
	size_t size;
};

/* Error codes. */
#define PMEM_SUCCESS	(0)
#define PMEM_NOSPACE	(1)
#define PMEM_BADDESC	(2)

/* pmem side of page.c */
int pmem_alloc_lo(size_t size, struct pmem_desc *desc);	/* paddr < 1GB */
int pmem_free(struct pmem_desc *desc);
/*
 * Exclude a physical range from allocation: VRAM, ROM windows, the
 * PC-98 15-16MB hole, and the kernel's own load ranges are declared
 * this way by the BSP and the embedding kernel.
 */
void pmem_reserve(physaddr_t paddr, size_t size);

#endif
