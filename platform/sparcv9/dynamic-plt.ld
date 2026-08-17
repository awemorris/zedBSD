/* SPDX-License-Identifier: Zlib */
/*
 * SPARC V9 PLT entries are patched as executable instructions.  Isolate them
 * on their own pages so the runtime linker can drop write permission after
 * applying R_SPARC_JMP_SLOT relocations without affecting writable data.
 */
SECTIONS
{
	.plt ALIGN(CONSTANT(MAXPAGESIZE)) :
	{
		*(.plt)
		*(.iplt)
	}
	. = ALIGN(CONSTANT(MAXPAGESIZE));
}
INSERT AFTER .text;
