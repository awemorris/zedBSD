/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Unicode's case mappings and the properties the final sigma rule needs,
 * from tables generated at build time from the Unicode Character Database
 * (tools/gen-unicode-case.py; the tables are not kept in the source tree).
 */

#ifndef KEILAND_BROWSER_BASE_UNICODE_H
#define KEILAND_BROWSER_BASE_UNICODE_H

#include <stddef.h>
#include <stdint.h>

/* The most code points one code point's case mapping gives. */
#define WB_CASE_MAX		3U

/*
 * A simple case mapping: a code point to one other.
 */
struct wb_case_simple {
	uint32_t code_point;
	uint32_t mapping;
};

/*
 * A full case mapping of SpecialCasing: a code point to up to three.
 */
struct wb_case_full {
	uint32_t code_point;
	uint32_t count;
	uint32_t points[WB_CASE_MAX];
};

/*
 * A range of code points with a property.
 */
struct wb_code_range {
	uint32_t first;
	uint32_t last;
};

/* The generated tables (each sorted by code point, constant for the life of the program). */
extern const struct wb_case_simple wb_case_lower[];
extern const size_t wb_case_lower_count;
extern const struct wb_case_simple wb_case_upper[];
extern const size_t wb_case_upper_count;
extern const struct wb_case_full wb_case_lower_full[];
extern const size_t wb_case_lower_full_count;
extern const struct wb_case_full wb_case_upper_full[];
extern const size_t wb_case_upper_full_count;
extern const struct wb_code_range wb_case_cased[];
extern const size_t wb_case_cased_count;
extern const struct wb_code_range wb_case_ignorable[];
extern const size_t wb_case_ignorable_count;

/* Case mapping (unicode.c). */
unsigned wb_case_map(uint32_t code_point, int upper, uint32_t mapped[WB_CASE_MAX]);
int wb_case_is_cased(uint32_t code_point);
int wb_case_is_ignorable(uint32_t code_point);

#endif
