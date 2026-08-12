.code32
.text
.globl _start
.type _start, @function
_start:
	/* Touch every page of the ELF-declared 1 MiB usable stack. */
	movl $0x7feff000, %edi
	movl $256, %ecx
	movb $0x5a, %al
1:
	movb %al, (%edi)
	addl $4096, %edi
	loop 1b
	/* Leave an unambiguous success trail in user_int_probe. */
	movl $0x5354414b, %eax
	int $0xc2
	movl $0x5354414b, %eax
	int $0xc2
	movl $0x5354414b, %eax
	int $0xc2
	movl $1, %eax
	xorl %ebx, %ebx
	int $0xc2
	ud2
.size _start, .-_start
