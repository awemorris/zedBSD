#include <hal/types.h>
#include "../i386.h"

void bsp_init(void)
{
	bsp_cons_init();

	i386_pmem_init();

	irq_init();

	int_init();
	univ_init();
	task_init();
	sched_init();
	clock_init();
}
