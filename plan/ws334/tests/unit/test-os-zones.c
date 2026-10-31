/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The unit test of the Windows zone name table (base/os-zones.h): the shape
 * of the table, rows read by hand from the Unicode CLDR 48 files, the keys
 * left out, and the lookup of every key and of keys that must find nothing.
 */

#include "check.h"

#include "base/os-zones.h"

#include <stddef.h>
#include <string.h>

/*
 * The number of rows: the "001" mapZone entries of windowsZones.xml whose
 * zone has a standard name by the rule in os-zones.h.  Counted as 139
 * entries (grep of territory="001" in windowsZones.xml) minus 12 whose zone
 * has no current metazone and no own English long standard name.
 */
#define EXPECTED_ROWS	127U

/* The longest name the zone-name writer of os-time.h accepts, in bytes. */
#define NAME_BYTES_MAX	63U

/*
 * One row read by hand from the CLDR files: the key, the standard name and
 * the daylight name (NULL when CLDR gives none).
 */
struct expected_row {
	const char *key;
	const char *standard;
	const char *daylight;
};

/*
 * The hand-read rows.  Each is the chain windowsZones.xml (key to zone),
 * metaZones.xml (zone to its metazone without an end date) and en.xml
 * (metazone long names, then the zone's own long names over them).
 */
static const struct expected_row expected_rows[] = {
	{ "Tokyo Standard Time", "Japan Standard Time", "Japan Daylight Time" },
	{ "Pacific Standard Time", "Pacific Standard Time", "Pacific Daylight Time" },
	{ "W. Europe Standard Time", "Central European Standard Time", "Central European Summer Time" },
	{ "GMT Standard Time", "Greenwich Mean Time", "British Summer Time" },
	{ "UTC", "Coordinated Universal Time", NULL },
	{ "India Standard Time", "India Standard Time", NULL },
	{ "Arabian Standard Time", "Gulf Standard Time", NULL },
	{ "China Standard Time", "China Standard Time", "China Daylight Time" },
	{ "AUS Eastern Standard Time", "Australian Eastern Standard Time", "Australian Eastern Daylight Time" },
	{ "Eastern Standard Time", "Eastern Standard Time", "Eastern Daylight Time" },
	{ "Afghanistan Standard Time", "Afghanistan Time", NULL },
	{ "New Zealand Standard Time", "New Zealand Standard Time", "New Zealand Daylight Time" },
	{ "Newfoundland Standard Time", "Newfoundland Standard Time", "Newfoundland Daylight Time" },
	{ "Namibia Standard Time", "Central Africa Time", NULL },
	{ "Sri Lanka Standard Time", "India Standard Time", NULL },
	{ "Sao Tome Standard Time", "Greenwich Mean Time", NULL },
	{ "Greenwich Standard Time", "Greenwich Mean Time", NULL },
	{ "Yukon Standard Time", "Yukon Time", NULL },
	{ "Saint Pierre Standard Time", "St. Pierre & Miquelon Standard Time", "St. Pierre & Miquelon Daylight Time" },
	{ "Turkey Standard Time", "T\303\274rkiye Standard Time", "T\303\274rkiye Summer Time" },
	{ "Korea Standard Time", "Korean Standard Time", "Korean Daylight Time" },
	{ "Central Asia Standard Time", "Kyrgyzstan Time", NULL },
	{ "Hawaiian Standard Time", "Hawaii-Aleutian Standard Time", NULL },
	{ "Egypt Standard Time", "Eastern European Standard Time", "Eastern European Summer Time" },
	{ "Haiti Standard Time", "Eastern Standard Time", "Eastern Daylight Time" },
	{ "Mountain Standard Time (Mexico)", "Mexican Pacific Standard Time", "Mexican Pacific Daylight Time" },
	{ "Cuba Standard Time", "Cuba Standard Time", "Cuba Daylight Time" },
	{ "West Bank Standard Time", "Eastern European Standard Time", "Eastern European Summer Time" },
	{ "Central Standard Time (Mexico)", "Central Standard Time", "Central Daylight Time" },
	{ "Russia Time Zone 10", "Magadan Standard Time", "Magadan Summer Time" }
};

/*
 * The "001" keys of windowsZones.xml that the table leaves out: their zone
 * has no metazone without an end date and no own English long standard name
 * (the Etc/GMT zones, and the zones that left their metazone for good).
 */
static const char *const absent_keys[] = {
	"Dateline Standard Time",
	"UTC-11",
	"UTC-09",
	"UTC-08",
	"Magallanes Standard Time",
	"UTC-02",
	"Morocco Standard Time",
	"Jordan Standard Time",
	"Syria Standard Time",
	"Bougainville Standard Time",
	"UTC+12",
	"UTC+13"
};

/*
 * Keys that must not be found: the empty string, a differing case, spaces
 * around a real key, a prefix, an extension, and bytes outside ASCII.
 */
static const char *const unknown_keys[] = {
	"",
	" ",
	"tokyo standard time",
	"TOKYO STANDARD TIME",
	"Tokyo Standard Time ",
	" Tokyo Standard Time",
	"Tokyo Standard Tim",
	"Tokyo Standard Timex",
	"Tokyo",
	"Japan Standard Time",
	"UTC ",
	"utc",
	"A",
	"~",
	"\377",
	"\303\274",
	"Z"
};

/*
 * The FNV-1a (64 bit) digest of the whole table, over each row as the key,
 * a NUL, the standard name, a NUL, the daylight name (or the one byte 0x01
 * when it is NULL) and a NUL.  The expected value was made from a separate
 * extraction of the CLDR files (an XPath pass of xmllint) and stands in
 * for a second copy of all the rows.
 */
#define EXPECTED_DIGEST	0x11c5de6d30210a86ULL

static void test_shape(void);
static void test_expected_rows(void);
static void test_absent_keys(void);
static void test_find_every_key(void);
static void test_unknown_keys(void);
static void test_digest(void);
static void digest_bytes(unsigned long long *digest, const char *bytes, size_t length);

/*
 * Runs the checks on the zone table and prints the summary.
 */
int
main(
	int argc,
	char **argv)
{
	int differs;
	int status;

	/* The harness passes the system's name. */
	CHECK(argc == 2);
	differs = 1;
	if (argc == 2)
		differs = strcmp(argv[1], "posix") != 0 && strcmp(argv[1], "win32") != 0;
	CHECK(differs == 0);

	/* The table, its known rows and the lookups. */
	test_shape();
	test_expected_rows();
	test_absent_keys();
	test_find_every_key();
	test_unknown_keys();
	test_digest();

	/* Reports the summary line and the status. */
	status = check_finish("os-zones");

	/* Succeeded when every check passed. */
	return status;
}

/*
 * Checks the table's order, uniqueness, field rules and length.
 */
static void
test_shape(void)
{
	const struct wb_os_zone_names *table;
	size_t count;
	size_t i;
	size_t again;
	int order;

	/* Reads the table and its length, and the same table without a length. */
	count = 0;
	table = wb_os_zone_table(&count);
	CHECK(table != NULL);
	CHECK_EQ(count, EXPECTED_ROWS);
	CHECK(wb_os_zone_table(NULL) == table);
	again = 0;
	CHECK(wb_os_zone_table(&again) == table);
	CHECK_EQ(again, count);
	if (table == NULL)
		return;

	/* Checks every row's fields and its order against the next row. */
	for (i = 0; i < count; i++) {
		CHECK(table[i].key != NULL);
		CHECK(table[i].standard != NULL);
		if (table[i].key != NULL)
			CHECK(table[i].key[0] != '\0');
		if (table[i].standard != NULL) {
			CHECK(table[i].standard[0] != '\0');
			CHECK(strlen(table[i].standard) <= NAME_BYTES_MAX);
		}
		if (table[i].daylight != NULL) {
			CHECK(table[i].daylight[0] != '\0');
			CHECK(strlen(table[i].daylight) <= NAME_BYTES_MAX);
		}

		/* Strictly ascending in strcmp order means sorted with no duplicate. */
		if (i + 1 < count && table[i].key != NULL && table[i + 1].key != NULL) {
			order = strcmp(table[i].key, table[i + 1].key);
			CHECK(order < 0);
		}
	}
}

/*
 * Checks the rows read by hand from the CLDR files.
 */
static void
test_expected_rows(void)
{
	const struct wb_os_zone_names *names;
	size_t i;

	/* Finds each hand-read row and compares both names. */
	for (i = 0; i < sizeof(expected_rows) / sizeof(expected_rows[0]); i++) {
		names = wb_os_zone_find(expected_rows[i].key);
		CHECK(names != NULL);
		if (names == NULL)
			continue;

		CHECK(strcmp(names->key, expected_rows[i].key) == 0);
		CHECK(names->standard != NULL);
		if (names->standard != NULL)
			CHECK(strcmp(names->standard, expected_rows[i].standard) == 0);

		if (expected_rows[i].daylight == NULL) {
			/* CLDR gives this zone no daylight name. */
			CHECK(names->daylight == NULL);
		} else {
			CHECK(names->daylight != NULL);
			if (names->daylight != NULL)
				CHECK(strcmp(names->daylight, expected_rows[i].daylight) == 0);
		}
	}
}

/*
 * Checks that the keys CLDR gives no standard name are not in the table.
 */
static void
test_absent_keys(void)
{
	size_t i;

	/* Looks up each left-out key. */
	for (i = 0; i < sizeof(absent_keys) / sizeof(absent_keys[0]); i++)
		CHECK(wb_os_zone_find(absent_keys[i]) == NULL);
}

/*
 * Checks that every row is found, by its own key, as that very row.
 */
static void
test_find_every_key(void)
{
	const struct wb_os_zone_names *table;
	const struct wb_os_zone_names *names;
	size_t count;
	size_t i;

	/* Reads the table. */
	count = 0;
	table = wb_os_zone_table(&count);
	if (table == NULL)
		return;

	/* Looks up each row's key, which must give that row's entry. */
	for (i = 0; i < count; i++) {
		names = wb_os_zone_find(table[i].key);
		CHECK(names == &table[i]);
	}
}

/*
 * Checks that keys that are not exactly a row's key find nothing.
 */
static void
test_unknown_keys(void)
{
	size_t i;

	/* A missing key finds nothing. */
	CHECK(wb_os_zone_find(NULL) == NULL);

	/* Looks up each key that must not match. */
	for (i = 0; i < sizeof(unknown_keys) / sizeof(unknown_keys[0]); i++)
		CHECK(wb_os_zone_find(unknown_keys[i]) == NULL);
}

/*
 * Checks every byte of every row against the digest made from a separate
 * extraction.
 */
static void
test_digest(void)
{
	const struct wb_os_zone_names *table;
	size_t count;
	size_t i;
	unsigned long long digest;

	/* Reads the table. */
	count = 0;
	table = wb_os_zone_table(&count);
	if (table == NULL)
		return;

	/* Hashes each row as key, standard and daylight, each ended by a NUL. */
	digest = 0xcbf29ce484222325ULL;
	for (i = 0; i < count; i++) {
		digest_bytes(&digest, table[i].key, strlen(table[i].key) + 1);
		digest_bytes(&digest, table[i].standard, strlen(table[i].standard) + 1);
		if (table[i].daylight != NULL) {
			digest_bytes(&digest, table[i].daylight, strlen(table[i].daylight) + 1);
		} else {
			digest_bytes(&digest, "\001", 2);
		}
	}

	/* Compares with the digest of the separate extraction. */
	CHECK(digest == EXPECTED_DIGEST);
}

/*
 * Feeds bytes into an FNV-1a (64 bit) digest.
 */
static void
digest_bytes(
	unsigned long long *digest,
	const char *bytes,
	size_t length)
{
	size_t i;

	/* Mixes each byte in: xor it, then multiply by the FNV prime. */
	for (i = 0; i < length; i++) {
		*digest ^= (unsigned char)bytes[i];
		*digest *= 0x100000001b3ULL;
	}
}
