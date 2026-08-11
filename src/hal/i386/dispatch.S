#include "defs.h"

.code32
.text

/*
 * task entrypoint
 *
 * void asm_task_entrypoint();
 */
.global EXT_C(asm_task_entrypoint)
EXT_C(asm_task_entrypoint):
//	fninit
	iret

/*
 * スタックを切り替える
 *
 * void asm_task_dispatch(
 *	struct task_resume_frame *save,
 *	struct task_resume_frame *load)
 */
.global EXT_C(asm_task_dispatch)
EXT_C(asm_task_dispatch):

	movl	4(%esp), %eax	/* EAX = &from.resume_esp */
	movl	8(%esp), %ebx	/* EBX = &to.resume_esp */

	/*
	 * SAVE
	 */

	/* save context */
	pushf
	pushal
	pushl	%ds
	pushl	%es
	pushl	%fs
	pushl	%gs

	/* save stack-pointer */
	movl	%esp,  (%eax)	/* *(from->stackpointer) = %esp */

	/*
	 * LOAD
	 */

	/* load stack-pointer */
	movl	(%ebx), %esp	/* %esp = *(to->stackpointer) */

	/* load context */
	popl	%gs
	popl	%fs
	popl	%es
	popl	%ds
	popal
	popfl
	ret
