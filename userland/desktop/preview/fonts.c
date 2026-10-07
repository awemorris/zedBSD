/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The substitute fonts keiland-preview carries in itself (ws177-p010,
 * plan/ws168/phase001/phase.md section 4.3): the program opens no file, so
 * the desktop's Mahora (sans, bold and mono; Mahora-LICENSE.txt, installed
 * with the desktop's fonts) is put in the program by the assembler's
 * .incbin and given to libpdf in memory, which then draws the fonts a PDF
 * does not embed.  Mahora covers ASCII; a character it lacks is not drawn
 * (the desktop's fallback faces are files the program cannot open).
 *
 * The paths are the repository's, as the builds compile from its top
 * (zedBSD's make, the Linux build, the host tests); a change of a font
 * does not rebuild this file by itself.
 */

#include "preview.h"

#include <pdf.h>

/*
 * The fonts' bytes, each from its start symbol to its end symbol, in the
 * program's read-only data.
 */
__asm__(
	".pushsection .rodata\n"
	".balign 16\n"
	"preview_font_sans:\n"
	".incbin \"userland/desktop/fonts/Mahora-Regular.ttf\"\n"
	"preview_font_sans_end:\n"
	".balign 16\n"
	"preview_font_bold:\n"
	".incbin \"userland/desktop/fonts/Mahora-Bold.ttf\"\n"
	"preview_font_bold_end:\n"
	".balign 16\n"
	"preview_font_mono:\n"
	".incbin \"userland/desktop/fonts/Mahora-Mono.ttf\"\n"
	"preview_font_mono_end:\n"
	".popsection\n");

/* The start and the end of each font's bytes (the assembler's symbols above, local to this file). */
extern const unsigned char preview_font_sans[] __asm__("preview_font_sans");
extern const unsigned char preview_font_sans_end[] __asm__("preview_font_sans_end");
extern const unsigned char preview_font_bold[] __asm__("preview_font_bold");
extern const unsigned char preview_font_bold_end[] __asm__("preview_font_bold_end");
extern const unsigned char preview_font_mono[] __asm__("preview_font_mono");
extern const unsigned char preview_font_mono_end[] __asm__("preview_font_mono_end");

/*
 * Gives libpdf the fonts carried in the program, under the names of the
 * files they are installed as.  Returns 0, or the first errno value
 * libpdf reported (the program then draws without that font).
 */
int
preview_fonts_register(void)
{
	int error;

	/* The sans, which stands in for a sans or serif font a document does not embed. */
	error = pdf_font_memory_add("keiland.ttf", preview_font_sans, (size_t)(preview_font_sans_end - preview_font_sans));
	if (error != 0)
		return error;

	/* The bold. */
	error = pdf_font_memory_add("keiland-bold.ttf", preview_font_bold, (size_t)(preview_font_bold_end - preview_font_bold));
	if (error != 0)
		return error;

	/* The monospaced. */
	error = pdf_font_memory_add("keiland-mono.ttf", preview_font_mono, (size_t)(preview_font_mono_end - preview_font_mono));
	if (error != 0)
		return error;

	/* Succeeded: the three fonts are libpdf's to draw with. */
	return 0;
}
