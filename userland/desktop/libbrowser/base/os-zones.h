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
 * The table is pure data and compiles on every system (the tests run it on
 * the host); only os-time-win32.c uses it.  How its names are found is
 * stated with struct wb_os_zone_names.
 */

#ifndef KEILAND_BROWSER_OS_ZONES_H
#define KEILAND_BROWSER_OS_ZONES_H

#include <stddef.h>

/*
 * One Windows zone's names: its key as the registry and
 * DYNAMIC_TIME_ZONE_INFORMATION's TimeZoneKeyName give it (ASCII), and the
 * English long names (ASCII).  The entries are static and live as long as
 * the program.
 *
 * The names are the Unicode CLDR's English long time-zone names, found
 * this way: the key's IANA zone for the territory "001" in CLDR's Windows
 * zone mapping; that zone's current metazone (the one used without an end
 * date); the metazone's English long standard and daylight names; then any
 * English long standard or daylight name CLDR gives the IANA zone itself
 * replaces the metazone's (Europe/London's daylight "British Summer
 * Time", Etc/UTC's standard "Coordinated Universal Time").  daylight is
 * NULL when neither has a daylight name.  A key whose standard name is not
 * found that way (a zone without a metazone, such as Etc/GMT+12) is not in
 * the table.
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
