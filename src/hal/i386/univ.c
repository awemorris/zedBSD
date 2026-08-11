/*
 * Address-space (universe) management.  Restored from the working kt
 * snapshot; comments translated, logic unchanged.  The kernel half of
 * every universe is the 512MB direct map expressed with 4MB PDEs.
 */

#include <hal/runtime.h>
#include "univ.h"

struct univ_info *univ_list_head;	/* head of the universe list */
univ_t cur_univ;			/* currently selected universe */
int free_id_top;			/* next unused ID */

/*
 * Initialize universe management.
 */
void
univ_init(void)
{
	cur_univ = UNIV_SYS;
	free_id_top = 1;
}

/*
 * Create an address space.
 */
univ_t
univ_create(void)
{
	struct univ_info *ui;
	int i;

	ui = (struct univ_info *)hal_malloc(sizeof(struct univ_info));
	ui->univ_id = free_id_top++;
	ui->ptbl_head = NULL;
	ui->next = NULL;

	/* Kernel half: direct-map PDEs; user half empty. */
	for (i = 0; i < 1024; i++)
		ui->pdt[i] = 0;
	for (i = 0; i < 128; i++)
		ui->pdt[512 + i] = (i * 0x400000) |
			(PTE_PRESENT | PTE_USER | PTE_BIG | PTE_WRITE);

	return (univ_t)ui;
}

/*
 * Switch address spaces.
 */
void
univ_switch(univ_t u)
{
	struct univ_info *ui;

	if (u == cur_univ)
		return;
	ui = (struct univ_info *)u;
	asm_load_cr3((uint32)ui->pdt - SYS_START);
	cur_univ = u;
}

/*
 * Validate a universe handle.
 */
int
univ_check_handle(univ_t u)
{
	if (u == UNIV_SYS)
		return 1;
	return 0;
}
