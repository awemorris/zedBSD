	.text
	.balign 16
	.global asm_task_dispatch
	.type asm_task_dispatch,%function
asm_task_dispatch:
	sub sp,sp,#96
	stp x19,x20,[sp,#0]
	stp x21,x22,[sp,#16]
	stp x23,x24,[sp,#32]
	stp x25,x26,[sp,#48]
	stp x27,x28,[sp,#64]
	stp x29,x30,[sp,#80]
	mov x2,sp
	str x2,[x0]
	ldr x2,[x1]
	mov sp,x2
	ldp x19,x20,[sp,#0]
	ldp x21,x22,[sp,#16]
	ldp x23,x24,[sp,#32]
	ldp x25,x26,[sp,#48]
	ldp x27,x28,[sp,#64]
	ldp x29,x30,[sp,#80]
	add sp,sp,#96
	ret
	.size asm_task_dispatch,.-asm_task_dispatch

	.global arm64_kernel_task_entry
arm64_kernel_task_entry:
	mov x0,x20
	blr x19
	bl arm64_task_returned
	b .

	.global arm64_user_task_entry
arm64_user_task_entry:
	msr sp_el0,x20
	msr elr_el1,x19
	msr spsr_el1,xzr
	eret

	.global arm64_fp_save
arm64_fp_save:
	stp q0,q1,[x0,#0]
	stp q2,q3,[x0,#32]
	stp q4,q5,[x0,#64]
	stp q6,q7,[x0,#96]
	stp q8,q9,[x0,#128]
	stp q10,q11,[x0,#160]
	stp q12,q13,[x0,#192]
	stp q14,q15,[x0,#224]
	stp q16,q17,[x0,#256]
	stp q18,q19,[x0,#288]
	stp q20,q21,[x0,#320]
	stp q22,q23,[x0,#352]
	stp q24,q25,[x0,#384]
	stp q26,q27,[x0,#416]
	stp q28,q29,[x0,#448]
	stp q30,q31,[x0,#480]
	mrs x1,fpcr
	mrs x2,fpsr
	str x1,[x0,#512]
	str x2,[x0,#520]
	ret

	.global arm64_fp_restore
arm64_fp_restore:
	ldp q0,q1,[x0,#0]
	ldp q2,q3,[x0,#32]
	ldp q4,q5,[x0,#64]
	ldp q6,q7,[x0,#96]
	ldp q8,q9,[x0,#128]
	ldp q10,q11,[x0,#160]
	ldp q12,q13,[x0,#192]
	ldp q14,q15,[x0,#224]
	ldp q16,q17,[x0,#256]
	ldp q18,q19,[x0,#288]
	ldp q20,q21,[x0,#320]
	ldp q22,q23,[x0,#352]
	ldp q24,q25,[x0,#384]
	ldp q26,q27,[x0,#416]
	ldp q28,q29,[x0,#448]
	ldp q30,q31,[x0,#480]
	ldr x1,[x0,#512]
	ldr x2,[x0,#520]
	msr fpcr,x1
	msr fpsr,x2
	ret
