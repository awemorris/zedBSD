/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The English long names of the Windows time zones (os-zones.c), which
 * Date's toString writes on Windows as browsers do ("Japan Standard Time"
 * for the zone Windows calls "Tokyo Standard Time").
 *
 * The table is data from the Unicode CLDR: each Windows zone key is mapped
 * to its IANA zone for the territory "001" (windowsZones.xml), that zone to
 * the metazone it uses now (metaZones.xml, the usesMetazone without a "to"
 * date), and the metazone to its English long standard and daylight names
 * (common/main/en.xml).  The table is pure data and compiles on every
 * system (the tests run it on the host); only os-time-win32.c uses it.
 */

#ifndef KEILAND_BROWSER_OS_ZONES_H
#define KEILAND_BROWSER_OS_ZONES_H

#include <stddef.h>

/*
 * One Windows zone's names: its key as the registry and
 * DYNAMIC_TIME_ZONE_INFORMATION's TimeZoneKeyName give it (ASCII), and the
 * English long names (ASCII).  daylight is NULL when the metazone has no
 * daylight name (a zone without daylight saving).  The entries are static
 * and live as long as the program.
 */
struct wb_os_zone_names {
	const char *key;
	const char *standard;
	const char *daylight;
};

/*
 * Finds the names of a Windows zone.
 *
 * key: the Windows zone key, compared exactly (case and spaces matter);
 *      NULL finds nothing.
 * Returns the entry, or NULL when the table does not know the key.
 * The table is sorted by key in strcmp order and searched by halves; the
 * tests check the order.
 */
const struct wb_os_zone_names *wb_os_zone_find(const char *key);

/*
 * Gives the table and its length, for the tests (order, uniqueness, every
 * key non-empty, every standard name non-NULL).
 */
const struct wb_os_zone_names *wb_os_zone_table(size_t *count);

#endif
