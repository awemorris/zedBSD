/*
 * User address space ("universe") management.
 */

#ifndef _SYS_ARCH_UNIV_H_
#define _SYS_ARCH_UNIV_H_

#include <sys/types.h>

/* Universe handle (a pointer to univ_info). */
typedef void *univ_t;

/* The system universe (no structure actually exists for it). */
#define UNIV_SYS	(NULL)

/* Page attributes. */
#define PAGE_NONE	(0)
#define PAGE_READ	(1)
#define PAGE_WRITE	(2)
#define PAGE_EXEC	(4)
#define PAGE_NOCACHE	(8)

/* univ.c */
univ_t univ_create(void);
void univ_destroy(univ_t u);
int univ_check_handle(univ_t u);
void univ_switch(univ_t u);
void univ_set_entry(univ_t u, void *vaddr, void *paddr, size_t size,
		    int attr);

#endif
