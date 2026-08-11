/*
 * i386 address-space internals.
 */

#ifndef _SYS_ARCH_X86_UNIV_H_
#define _SYS_ARCH_X86_UNIV_H_

#include <hal/univ.h>	/* interface definition */
#include "asm.h"

/* Page-table bookkeeping for one universe. */
struct ptbl_info {
	uintptr_t vaddr;
	uint32 *pte;
	struct ptbl_info *next;
};

struct univ_info {
	uint32 pdt[1024];	/* page directory (4KB aligned via alloc) */
	int univ_id;
	struct ptbl_info *ptbl_head;
	struct univ_info *next;
};

void univ_init(void);

#endif
