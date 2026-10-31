/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The English long names of the Windows time zones, from the Unicode CLDR.
 */

#include "base/os-zones.h"

#include <stddef.h>
#include <string.h>

/*
 * The names of the Windows zones, sorted by key in strcmp order.
 *
 * It is constant data that lives as long as the program, and wb_os_zone_find
 * searches it by halves, so the order is part of its meaning.  A name is
 * ASCII except the one for "Turkey Standard Time", whose "u" with a
 * diaeresis is written as the two UTF-8 bytes of U+00FC.  A daylight name is
 * NULL where CLDR gives the zone no daylight name.
 *
 * Values referenced: Unicode CLDR 48 common/supplemental/windowsZones.xml,
 * metaZones.xml and common/main/en.xml (Unicode License V3, Copyright (C)
 * 2004-2025 Unicode, Inc.).  Each row is found as the header describes:
 * the "001" mapZone type of the key, that zone's metazone without an end
 * date, the metazone's English long names, then the zone's own English long
 * names over them.  The 139 keys of the "001" mapZone entries give 127 rows;
 * the other 12 have no standard name that way and are left out.
 */
static const struct wb_os_zone_names zone_table[] = {
	{ "AUS Central Standard Time", "Australian Central Standard Time", "Australian Central Daylight Time" },
	{ "AUS Eastern Standard Time", "Australian Eastern Standard Time", "Australian Eastern Daylight Time" },
	{ "Afghanistan Standard Time", "Afghanistan Time", NULL },
	{ "Alaskan Standard Time", "Alaska Standard Time", "Alaska Daylight Time" },
	{ "Aleutian Standard Time", "Hawaii-Aleutian Standard Time", "Hawaii-Aleutian Daylight Time" },
	{ "Altai Standard Time", "Krasnoyarsk Standard Time", "Krasnoyarsk Summer Time" },
	{ "Arab Standard Time", "Arabian Standard Time", "Arabian Daylight Time" },
	{ "Arabian Standard Time", "Gulf Standard Time", NULL },
	{ "Arabic Standard Time", "Arabian Standard Time", "Arabian Daylight Time" },
	{ "Argentina Standard Time", "Argentina Standard Time", "Argentina Summer Time" },
	{ "Astrakhan Standard Time", "Samara Standard Time", "Samara Summer Time" },
	{ "Atlantic Standard Time", "Atlantic Standard Time", "Atlantic Daylight Time" },
	{ "Aus Central W. Standard Time", "Australian Central Western Standard Time", "Australian Central Western Daylight Time" },
	{ "Azerbaijan Standard Time", "Azerbaijan Standard Time", "Azerbaijan Summer Time" },
	{ "Azores Standard Time", "Azores Standard Time", "Azores Summer Time" },
	{ "Bahia Standard Time", "Brasilia Standard Time", "Brasilia Summer Time" },
	{ "Bangladesh Standard Time", "Bangladesh Standard Time", "Bangladesh Summer Time" },
	{ "Belarus Standard Time", "Moscow Standard Time", "Moscow Summer Time" },
	{ "Canada Central Standard Time", "Central Standard Time", "Central Daylight Time" },
	{ "Cape Verde Standard Time", "Cape Verde Standard Time", "Cape Verde Summer Time" },
	{ "Caucasus Standard Time", "Armenia Standard Time", "Armenia Summer Time" },
	{ "Cen. Australia Standard Time", "Australian Central Standard Time", "Australian Central Daylight Time" },
	{ "Central America Standard Time", "Central Standard Time", "Central Daylight Time" },
	{ "Central Asia Standard Time", "Kyrgyzstan Time", NULL },
	{ "Central Brazilian Standard Time", "Amazon Standard Time", "Amazon Summer Time" },
	{ "Central Europe Standard Time", "Central European Standard Time", "Central European Summer Time" },
	{ "Central European Standard Time", "Central European Standard Time", "Central European Summer Time" },
	{ "Central Pacific Standard Time", "Solomon Islands Time", NULL },
	{ "Central Standard Time", "Central Standard Time", "Central Daylight Time" },
	{ "Central Standard Time (Mexico)", "Central Standard Time", "Central Daylight Time" },
	{ "Chatham Islands Standard Time", "Chatham Standard Time", "Chatham Daylight Time" },
	{ "China Standard Time", "China Standard Time", "China Daylight Time" },
	{ "Cuba Standard Time", "Cuba Standard Time", "Cuba Daylight Time" },
	{ "E. Africa Standard Time", "East Africa Time", NULL },
	{ "E. Australia Standard Time", "Australian Eastern Standard Time", "Australian Eastern Daylight Time" },
	{ "E. Europe Standard Time", "Eastern European Standard Time", "Eastern European Summer Time" },
	{ "E. South America Standard Time", "Brasilia Standard Time", "Brasilia Summer Time" },
	{ "Easter Island Standard Time", "Easter Island Standard Time", "Easter Island Summer Time" },
	{ "Eastern Standard Time", "Eastern Standard Time", "Eastern Daylight Time" },
	{ "Eastern Standard Time (Mexico)", "Eastern Standard Time", "Eastern Daylight Time" },
	{ "Egypt Standard Time", "Eastern European Standard Time", "Eastern European Summer Time" },
	{ "Ekaterinburg Standard Time", "Yekaterinburg Standard Time", "Yekaterinburg Summer Time" },
	{ "FLE Standard Time", "Eastern European Standard Time", "Eastern European Summer Time" },
	{ "Fiji Standard Time", "Fiji Standard Time", "Fiji Summer Time" },
	{ "GMT Standard Time", "Greenwich Mean Time", "British Summer Time" },
	{ "GTB Standard Time", "Eastern European Standard Time", "Eastern European Summer Time" },
	{ "Georgian Standard Time", "Georgia Standard Time", "Georgia Summer Time" },
	{ "Greenland Standard Time", "Greenland Standard Time", "Greenland Summer Time" },
	{ "Greenwich Standard Time", "Greenwich Mean Time", NULL },
	{ "Haiti Standard Time", "Eastern Standard Time", "Eastern Daylight Time" },
	{ "Hawaiian Standard Time", "Hawaii-Aleutian Standard Time", NULL },
	{ "India Standard Time", "India Standard Time", NULL },
	{ "Iran Standard Time", "Iran Standard Time", "Iran Daylight Time" },
	{ "Israel Standard Time", "Israel Standard Time", "Israel Daylight Time" },
	{ "Kaliningrad Standard Time", "Eastern European Standard Time", "Eastern European Summer Time" },
	{ "Korea Standard Time", "Korean Standard Time", "Korean Daylight Time" },
	{ "Libya Standard Time", "Eastern European Standard Time", "Eastern European Summer Time" },
	{ "Line Islands Standard Time", "Line Islands Time", NULL },
	{ "Lord Howe Standard Time", "Lord Howe Standard Time", "Lord Howe Daylight Time" },
	{ "Magadan Standard Time", "Magadan Standard Time", "Magadan Summer Time" },
	{ "Marquesas Standard Time", "Marquesas Time", NULL },
	{ "Mauritius Standard Time", "Mauritius Standard Time", "Mauritius Summer Time" },
	{ "Middle East Standard Time", "Eastern European Standard Time", "Eastern European Summer Time" },
	{ "Montevideo Standard Time", "Uruguay Standard Time", "Uruguay Summer Time" },
	{ "Mountain Standard Time", "Mountain Standard Time", "Mountain Daylight Time" },
	{ "Mountain Standard Time (Mexico)", "Mexican Pacific Standard Time", "Mexican Pacific Daylight Time" },
	{ "Myanmar Standard Time", "Myanmar Time", NULL },
	{ "N. Central Asia Standard Time", "Krasnoyarsk Standard Time", "Krasnoyarsk Summer Time" },
	{ "Namibia Standard Time", "Central Africa Time", NULL },
	{ "Nepal Standard Time", "Nepal Time", NULL },
	{ "New Zealand Standard Time", "New Zealand Standard Time", "New Zealand Daylight Time" },
	{ "Newfoundland Standard Time", "Newfoundland Standard Time", "Newfoundland Daylight Time" },
	{ "Norfolk Standard Time", "Norfolk Island Standard Time", "Norfolk Island Daylight Time" },
	{ "North Asia East Standard Time", "Irkutsk Standard Time", "Irkutsk Summer Time" },
	{ "North Asia Standard Time", "Krasnoyarsk Standard Time", "Krasnoyarsk Summer Time" },
	{ "North Korea Standard Time", "Korean Standard Time", "Korean Daylight Time" },
	{ "Omsk Standard Time", "Omsk Standard Time", "Omsk Summer Time" },
	{ "Pacific SA Standard Time", "Chile Standard Time", "Chile Summer Time" },
	{ "Pacific Standard Time", "Pacific Standard Time", "Pacific Daylight Time" },
	{ "Pacific Standard Time (Mexico)", "Pacific Standard Time", "Pacific Daylight Time" },
	{ "Pakistan Standard Time", "Pakistan Standard Time", "Pakistan Summer Time" },
	{ "Paraguay Standard Time", "Paraguay Standard Time", "Paraguay Summer Time" },
	{ "Qyzylorda Standard Time", "Kazakhstan Time", NULL },
	{ "Romance Standard Time", "Central European Standard Time", "Central European Summer Time" },
	{ "Russia Time Zone 10", "Magadan Standard Time", "Magadan Summer Time" },
	{ "Russia Time Zone 11", "Kamchatka Standard Time", "Kamchatka Summer Time" },
	{ "Russia Time Zone 3", "Samara Standard Time", "Samara Summer Time" },
	{ "Russian Standard Time", "Moscow Standard Time", "Moscow Summer Time" },
	{ "SA Eastern Standard Time", "French Guiana Time", NULL },
	{ "SA Pacific Standard Time", "Colombia Standard Time", "Colombia Summer Time" },
	{ "SA Western Standard Time", "Bolivia Time", NULL },
	{ "SE Asia Standard Time", "Indochina Time", NULL },
	{ "Saint Pierre Standard Time", "St. Pierre & Miquelon Standard Time", "St. Pierre & Miquelon Daylight Time" },
	{ "Sakhalin Standard Time", "Magadan Standard Time", "Magadan Summer Time" },
	{ "Samoa Standard Time", "Samoa Standard Time", "Samoa Daylight Time" },
	{ "Sao Tome Standard Time", "Greenwich Mean Time", NULL },
	{ "Saratov Standard Time", "Samara Standard Time", "Samara Summer Time" },
	{ "Singapore Standard Time", "Singapore Standard Time", NULL },
	{ "South Africa Standard Time", "South Africa Standard Time", NULL },
	{ "South Sudan Standard Time", "Central Africa Time", NULL },
	{ "Sri Lanka Standard Time", "India Standard Time", NULL },
	{ "Sudan Standard Time", "Central Africa Time", NULL },
	{ "Taipei Standard Time", "Taiwan Standard Time", "Taiwan Daylight Time" },
	{ "Tasmania Standard Time", "Australian Eastern Standard Time", "Australian Eastern Daylight Time" },
	{ "Tocantins Standard Time", "Brasilia Standard Time", "Brasilia Summer Time" },
	{ "Tokyo Standard Time", "Japan Standard Time", "Japan Daylight Time" },
	{ "Tomsk Standard Time", "Krasnoyarsk Standard Time", "Krasnoyarsk Summer Time" },
	{ "Tonga Standard Time", "Tonga Standard Time", "Tonga Summer Time" },
	{ "Transbaikal Standard Time", "Yakutsk Standard Time", "Yakutsk Summer Time" },
	{ "Turkey Standard Time", "T\303\274rkiye Standard Time", "T\303\274rkiye Summer Time" },
	{ "Turks And Caicos Standard Time", "Eastern Standard Time", "Eastern Daylight Time" },
	{ "US Eastern Standard Time", "Eastern Standard Time", "Eastern Daylight Time" },
	{ "US Mountain Standard Time", "Mountain Standard Time", "Mountain Daylight Time" },
	{ "UTC", "Coordinated Universal Time", NULL },
	{ "Ulaanbaatar Standard Time", "Ulaanbaatar Standard Time", "Ulaanbaatar Summer Time" },
	{ "Venezuela Standard Time", "Venezuela Time", NULL },
	{ "Vladivostok Standard Time", "Vladivostok Standard Time", "Vladivostok Summer Time" },
	{ "Volgograd Standard Time", "Moscow Standard Time", "Moscow Summer Time" },
	{ "W. Australia Standard Time", "Australian Western Standard Time", "Australian Western Daylight Time" },
	{ "W. Central Africa Standard Time", "West Africa Time", NULL },
	{ "W. Europe Standard Time", "Central European Standard Time", "Central European Summer Time" },
	{ "W. Mongolia Standard Time", "Khovd Standard Time", "Khovd Summer Time" },
	{ "West Asia Standard Time", "Uzbekistan Standard Time", "Uzbekistan Summer Time" },
	{ "West Bank Standard Time", "Eastern European Standard Time", "Eastern European Summer Time" },
	{ "West Pacific Standard Time", "Papua New Guinea Time", NULL },
	{ "Yakutsk Standard Time", "Yakutsk Standard Time", "Yakutsk Summer Time" },
	{ "Yukon Standard Time", "Yukon Time", NULL },
};

/* The number of rows in zone_table. */
#define ZONE_TABLE_COUNT	(sizeof(zone_table) / sizeof(zone_table[0]))

/*
 * Finds the names of a Windows zone.
 *
 * The key is compared exactly with the sorted keys by halving the range that
 * can still hold it.  A NULL key finds nothing.
 */
const struct wb_os_zone_names *
wb_os_zone_find(
	const char *key)
{
	const struct wb_os_zone_names *found;
	size_t low;
	size_t high;
	size_t middle;
	int order;

	/* Refuses a missing key before it reaches strcmp. */
	if (key == NULL)
		return NULL;

	/* Halves the range [low, high) until it is empty or holds the key. */
	found = NULL;
	low = 0;
	high = ZONE_TABLE_COUNT;
	while (low < high) {
		/* Takes the middle row of the range without overflowing the sum. */
		middle = low + (high - low) / 2;

		/* Orders the key against the middle row's key. */
		order = strcmp(key, zone_table[middle].key);
		if (order == 0) {
			/* The middle row is the zone asked for. */
			found = &zone_table[middle];
			break;
		}

		if (order < 0) {
			/* The key sorts before the middle row, so keep the lower half. */
			high = middle;
		} else {
			/* The key sorts after the middle row, so keep the upper half. */
			low = middle + 1;
		}
	}

	/* Succeeded: reports the row, or NULL when the table does not know the key. */
	return found;
}

/*
 * Gives the table and its length.
 *
 * A caller that does not want the length passes NULL for count.
 */
const struct wb_os_zone_names *
wb_os_zone_table(
	size_t *count)
{
	/* Reports the number of rows when the caller asked for it. */
	if (count != NULL)
		*count = ZONE_TABLE_COUNT;

	/* Succeeded: the whole table, which lives as long as the program. */
	return zone_table;
}
