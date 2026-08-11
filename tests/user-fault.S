.code32
.section .text
.global _start
_start:
	ud2
1:
	jmp 1b
