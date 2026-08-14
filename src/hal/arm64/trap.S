	.section .text, "ax"
	.balign 2048
	.global arm64_vectors
arm64_vectors:
	.macro VECTOR target, number
	b \target
	.space 124
	.endm
	VECTOR arm64_sync_current, 0
	VECTOR arm64_irq_common, 1
	VECTOR arm64_sync_current, 2
	VECTOR arm64_sync_current, 3
	VECTOR arm64_sync_current, 4
	VECTOR arm64_irq_common, 5
	VECTOR arm64_sync_current, 6
	VECTOR arm64_sync_current, 7
	VECTOR arm64_sync_lower, 8
	VECTOR arm64_irq_common, 9
	VECTOR arm64_sync_lower, 10
	VECTOR arm64_sync_lower, 11
	VECTOR arm64_sync_lower, 12
	VECTOR arm64_irq_common, 13
	VECTOR arm64_sync_lower, 14
	VECTOR arm64_sync_lower, 15

	.macro SAVE_FRAME
	sub sp,sp,#288
	stp x0,x1,[sp,#0]
	stp x2,x3,[sp,#16]
	stp x4,x5,[sp,#32]
	stp x6,x7,[sp,#48]
	stp x8,x9,[sp,#64]
	stp x10,x11,[sp,#80]
	stp x12,x13,[sp,#96]
	stp x14,x15,[sp,#112]
	stp x16,x17,[sp,#128]
	stp x18,x19,[sp,#144]
	stp x20,x21,[sp,#160]
	stp x22,x23,[sp,#176]
	stp x24,x25,[sp,#192]
	stp x26,x27,[sp,#208]
	stp x28,x29,[sp,#224]
	str x30,[sp,#240]
	mrs x0,elr_el1
	mrs x1,spsr_el1
	mrs x2,esr_el1
	mrs x3,far_el1
	stp x0,x1,[sp,#248]
	stp x2,x3,[sp,#264]
	mrs x4,sp_el0
	str x4,[sp,#280]
	.endm
	.macro RESTORE_FRAME
	ldp x0,x1,[sp,#248]
	msr elr_el1,x0
	msr spsr_el1,x1
	ldr x2,[sp,#280]
	msr sp_el0,x2
	ldp x0,x1,[sp,#0]
	ldp x2,x3,[sp,#16]
	ldp x4,x5,[sp,#32]
	ldp x6,x7,[sp,#48]
	ldp x8,x9,[sp,#64]
	ldp x10,x11,[sp,#80]
	ldp x12,x13,[sp,#96]
	ldp x14,x15,[sp,#112]
	ldp x16,x17,[sp,#128]
	ldp x18,x19,[sp,#144]
	ldp x20,x21,[sp,#160]
	ldp x22,x23,[sp,#176]
	ldp x24,x25,[sp,#192]
	ldp x26,x27,[sp,#208]
	ldp x28,x29,[sp,#224]
	ldr x30,[sp,#240]
	add sp,sp,#288
	.endm

arm64_irq_common:
	SAVE_FRAME
	mov x0,sp
	bl arm64_irq_handler
	RESTORE_FRAME
	eret
arm64_sync_current:
	SAVE_FRAME
	mov x0,sp
	mov x1,#0
	bl arm64_sync_handler
	b .
arm64_sync_lower:
	SAVE_FRAME
	mov x0,sp
	mov x1,#8
	bl arm64_sync_handler
	RESTORE_FRAME
	eret
