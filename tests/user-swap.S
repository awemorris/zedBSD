.code32
.text
.globl _start
.type _start, @function
_start:
	/* mmap(NULL, 8 MiB, RW, PRIVATE|ANONYMOUS, -1, 0) */
	movl $11, %eax
	xorl %ebx, %ebx
	movl $0x00800000, %ecx
	movl $3, %edx
	movl $0x21, %esi
	movl $-1, %edi
	xorl %ebp, %ebp
	int $0xc2
	testl %eax, %eax
	js failed
	movl %eax, %edi
	xorl %ecx, %ecx
write_pages:
	movl %ecx, %eax
	xorl $0x5a17c3e9, %eax
	movl %eax, (%edi,%ecx,1)
	addl $4096, %ecx
	cmpl $0x00800000, %ecx
	jb write_pages
	movl $0x007ff000, %ecx
verify_pages:
	movl %ecx, %eax
	xorl $0x5a17c3e9, %eax
	cmpl %eax, (%edi,%ecx,1)
	jne failed
	subl $4096, %ecx
	jnc verify_pages
	/*
	 * Read every page again in the opposite direction.  At 5 MiB the first
	 * verification itself forces page-ins followed by further reclaim.
	 * This second pass detects a page whose swap slot was released on
	 * page-in and which was then incorrectly discarded as clean.
	 */
	xorl %ecx, %ecx
verify_pages_again:
	movl %ecx, %eax
	xorl $0x5a17c3e9, %eax
	cmpl %eax, (%edi,%ecx,1)
	jne failed
	addl $4096, %ecx
	cmpl $0x00800000, %ecx
	jb verify_pages_again
	/* munmap the pressure region and prove resident frames/slots release. */
	movl %edi, %ebx
	movl $0x00800000, %ecx
	movl $12, %eax
	int $0xc2
	testl %eax, %eax
	jne failed
	/* Leave an unambiguous success trail in user_int_probe. */
	movl $0x53574150, %eax
	int $0xc2
	movl $0x53574150, %eax
	int $0xc2
	movl $0x53574150, %eax
	int $0xc2
	movl $1, %eax
	xorl %ebx, %ebx
	int $0xc2
failed:
	ud2
.size _start, .-_start
