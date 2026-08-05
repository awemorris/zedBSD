/*
 * Context-switching management.
 */

#ifndef _SYS_ARCH_TASK_H_
#define _SYS_ARCH_TASK_H_

#include <sys/hal/univ.h>

/* Task handle (a pointer to task_info). */
typedef void *task_t;

/* task.c */
task_t task_create(univ_t u, void *start, void *param, void *user_sp);
void task_destroy(task_t t);
void task_switch(task_t t);
task_t task_get_current(void);

#endif
