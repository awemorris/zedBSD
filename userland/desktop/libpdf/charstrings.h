/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The private declarations of libpdf's charstring fonts (stage 3 of
 * design-pdf.md): the font programs whose glyphs are charstrings, a Type 1
 * program (/FontFile, cff.c's sibling type1.c) and a Compact Font Format
 * program (/FontFile3 Type1C and CIDFontType0C, cff.c), read into one
 * shape that charstrings.c answers from and that the two interpreters
 * draw with.  font.c sees only the functions of internal.h.
 */

#ifndef LIBPDF_CHARSTRINGS_H
#define LIBPDF_CHARSTRINGS_H

#include <stddef.h>

#include "internal.h"

/* The count of CFF standard strings (SIDs 0 to 390). */
#define PDF_CFF_STANDARD_STRINGS 391

/* The most glyphs, subroutines, and private dictionaries (font DICTs of a CID font) a program may have. */
#define CHARSTRINGS_GLYPHS_MAX 65536
#define CHARSTRINGS_SUBRS_MAX 65536
#define CHARSTRINGS_PRIVATES_MAX 256

/* The deepest subroutine call and the most operators one glyph may run. */
#define CHARSTRINGS_CALL_DEPTH 10
#define CHARSTRINGS_OPERATORS_MAX 65536

/* The deepest argument stack (Type 2's limit, above Type 1's 24) and Type 2's transient array. */
#define CHARSTRINGS_STACK_MAX 48
#define CHARSTRINGS_TRANSIENT_MAX 32

/*
 * The kinds of program.
 */
enum charstrings_kind {
	CHARSTRINGS_TYPE1 = 1,
	CHARSTRINGS_CFF
};

/*
 * A range of a program's bytes: a charstring, a subroutine or a name.
 */
struct charstrings_range {
	const unsigned char *data;
	size_t size;
};

/*
 * The private dictionary a glyph is drawn with: its local subroutines and
 * Type 2's widths, and (for a font DICT of a CID-keyed CFF font) the
 * matrix from glyph space to ems.
 */
struct charstrings_private {
	struct charstrings_range *subrs;
	size_t subrs_count;
	double default_width;
	double nominal_width;
	double matrix[6];
};

/*
 * One glyph's name and number, kept sorted by name for the lookups.
 */
struct charstrings_name {
	const unsigned char *name;
	size_t length;
	unsigned glyph;
};

/*
 * One CID and its glyph, kept sorted by CID (a CID-keyed CFF font).
 */
struct charstrings_cid {
	unsigned cid;
	unsigned glyph;
};

/*
 * A charstring font program, read.
 *
 * The ranges point into the program's bytes: the caller's (a CFF font's
 * decoded stream, which the caller keeps as long as the program) or
 * owned (a Type 1 font's decrypted private part).  names holds each
 * glyph's name, sorted by name in sorted_names; a CID-keyed font has
 * cids instead.  builtin is the program's own encoding, code to glyph (0
 * for none).  privates holds one private dictionary, or one per font
 * DICT with private_of naming each glyph's.  matrix maps glyph space to
 * ems.
 */
struct pdf_charstrings {
	enum charstrings_kind kind;
	unsigned char *owned;
	size_t glyphs;
	struct charstrings_range *charstrings;
	struct charstrings_range *names;
	struct charstrings_name *sorted_names;
	size_t names_count;
	struct charstrings_cid *cids;
	size_t cids_count;
	int cid_keyed;
	unsigned builtin[256];
	int has_builtin;
	struct charstrings_range *global_subrs;
	size_t global_subrs_count;
	struct charstrings_private *privates;
	size_t privates_count;
	unsigned char *private_of;
	double matrix[6];
	int len_iv;
};

/*
 * Where a glyph's outline goes while its charstring runs: the current
 * point in glyph space, whether a subpath is open, and the sink the
 * points go to in ems.
 */
struct charstrings_path {
	const double *matrix;
	pdf_charstrings_emit emit;
	void *context;
	double x;
	double y;
	double start_x;
	double start_y;
	int open;
	int error;
};

/* The fixed CFF data (cffdata.c). */
extern const char *const pdf_cff_standard_strings[PDF_CFF_STANDARD_STRINGS];
extern const unsigned short pdf_cff_standard_encoding[256];
extern const unsigned short pdf_cff_expert_charset[];
extern const size_t pdf_cff_expert_charset_count;
extern const unsigned short pdf_cff_expert_subset_charset[];
extern const size_t pdf_cff_expert_subset_charset_count;

/* The path a charstring draws (charstrings.c). */
void charstrings_move(struct charstrings_path *path, double x, double y);
void charstrings_line(struct charstrings_path *path, double x, double y);
void charstrings_curve(struct charstrings_path *path, double x1, double y1, double x2, double y2, double x3, double y3);
void charstrings_close(struct charstrings_path *path);

/* The shared parts of reading a program (charstrings.c). */
int charstrings_sort_names(struct pdf_charstrings *font);
int charstrings_standard_glyph(const struct pdf_charstrings *font, unsigned code, unsigned *glyph);

/* The two interpreters (type1.c, cff.c). */
void pdf_type1_run(struct pdf_charstrings *font, unsigned glyph, struct charstrings_path *path, double *width);
void pdf_cff_run(struct pdf_charstrings *font, unsigned glyph, struct charstrings_path *path, double *width);

#endif /* LIBPDF_CHARSTRINGS_H */
