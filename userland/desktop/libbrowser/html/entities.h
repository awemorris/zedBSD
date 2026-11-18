/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The HTML named character references.
 *
 * The table is generated at build time from the WHATWG list
 * (tools/gen-entities.py); the list's licence notice is in the generated
 * file.
 */

#ifndef KEILAND_BROWSER_HTML_ENTITIES_H
#define KEILAND_BROWSER_HTML_ENTITIES_H

#include <stddef.h>
#include <stdint.h>

/*
 * One named character reference: its name without the ampersand (ending
 * with ';' unless it is one of the legacy names that may omit it) and the
 * one or two code points it stands for (second is zero for one).
 */
struct html_entity {
	const char *name;
	uint32_t first;
	uint32_t second;
};

/* The generated table, sorted by name in byte order, and its length. */
extern const struct html_entity html_entities[];
extern const size_t html_entity_count;

#endif
