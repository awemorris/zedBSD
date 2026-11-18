/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What can be installed, from the index the image carries
 * (/opt/installer/index, admin/index.h): the units with their sizes and
 * what each needs, the items of the page of what to install, the
 * firmware's licences and the loader's size.  The choice of the items
 * gives the units installed: theirs and everything they need, which the
 * page shows as coming with another item.  The helper checks the index
 * again (its SHA-256) and installs the units the installer names.
 */

#include "installer.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The largest index and the largest licence the installer reads. */
#define CATALOG_INDEX_MAX	(1024U * 1024U)
#define CATALOG_LICENSE_MAX	(1024U * 1024U)

/* The most fields of one record of the index. */
#define CATALOG_FIELDS_MAX	12U

/* The loader's name among the boot files. */
#define CATALOG_LOADER		"BOOTX64.EFI"

static int catalog_file(const char *path, size_t limit, char **text, size_t *length);
static unsigned catalog_split(char *line, char **field, unsigned most);
static int catalog_units(struct inst_catalog *catalog, char *text);
static int catalog_records(struct inst_catalog *catalog, char *text);
static int catalog_unit_record(struct inst_catalog *catalog, unsigned slot, char **field, unsigned count);
static int catalog_item_record(struct inst_catalog *catalog, char **field, unsigned count);
static void catalog_copy(char *target, size_t size, const char *source);
static void catalog_close_over(struct inst_catalog *catalog, unsigned unit);

/*
 * Reads the index into the catalog: its units, items, licences and the
 * loader's size, every item at its default.  Returns 0 or an errno value
 * (EINVAL for an index the installer does not understand).
 */
int
inst_catalog_read(
	struct inst_catalog *catalog)
{
	char *second;
	char *text;
	size_t length;
	int error;

	/* The file, whole. */
	memset(catalog, 0, sizeof(*catalog));
	error = catalog_file(INST_INDEX, CATALOG_INDEX_MAX, &text, &length);
	if (error != 0) {
		catalog->error = error;
		return error;
	}

	/* A copy for the second pass (each pass cuts its lines). */
	second = malloc(length + 1U);
	if (second == NULL) {
		free(text);
		catalog->error = ENOMEM;
		return ENOMEM;
	}

	/* Copies the text for the second pass. */
	memcpy(second, text, length + 1U);

	/* The units' names first, then every record (an item or a dependency may name a unit after it). */
	error = catalog_units(catalog, text);
	if (error == 0)
		error = catalog_records(catalog, second);
	free(text);
	free(second);
	if (error != 0) {
		catalog->error = error;
		return error;
	}

	/* The units the defaults choose. */
	catalog->read = 1;
	inst_catalog_choose(catalog);

	/* Succeeded: the catalog is read. */
	return 0;
}

/*
 * Works out the units the chosen items install, with every unit they
 * need, and their sizes.
 */
void
inst_catalog_choose(
	struct inst_catalog *catalog)
{
	const struct inst_item *item;
	unsigned i;
	unsigned j;

	/* None, then each chosen item's units and what they need. */
	memset(catalog->closure, 0, sizeof(catalog->closure));
	for (i = 0; i < catalog->item_count; i++) {
		item = &catalog->item[i];
		if (!item->chosen)
			continue;
		for (j = 0; j < item->unit_count; j++)
			catalog_close_over(catalog, item->unit[j]);
	}

	/* The sizes of what is installed. */
	catalog->chosen_bytes = 0;
	catalog->chosen_entries = 0;
	for (i = 0; i < catalog->unit_count; i++) {
		if (!catalog->closure[i])
			continue;
		catalog->chosen_bytes += catalog->unit[i].bytes;
		catalog->chosen_entries += catalog->unit[i].entries;
	}
}

/*
 * Reports whether an item that is not chosen is installed all the same,
 * every unit of it needed by another item's.
 */
int
inst_item_by_dependency(
	const struct inst_catalog *catalog,
	unsigned item)
{
	const struct inst_item *entry;
	unsigned j;

	/* A chosen item, or a heading, is not. */
	entry = &catalog->item[item];
	if (entry->chosen || entry->unit_count == 0U)
		return 0;

	/* Each of its units installed. */
	for (j = 0; j < entry->unit_count; j++) {
		if (!catalog->closure[entry->unit[j]])
			return 0;
	}

	/* Every unit of it comes with another. */
	return 1;
}

/* Reports the index of an item by its id, or -1. */
int
inst_item_find(
	const struct inst_catalog *catalog,
	const char *id)
{
	unsigned i;
	int same;

	/* Each item. */
	for (i = 0; i < catalog->item_count; i++) {
		same = strcmp(catalog->item[i].id, id);
		if (same == 0)
			return (int)i;
	}

	/* No such item. */
	return -1;
}

/* Reports the index of a unit by its name, or -1. */
int
inst_catalog_unit_find(
	const struct inst_catalog *catalog,
	const char *name)
{
	unsigned i;
	int same;

	/* Each unit. */
	for (i = 0; i < catalog->unit_count; i++) {
		same = strcmp(catalog->unit[i].name, name);
		if (same == 0)
			return (int)i;
	}

	/* No such unit. */
	return -1;
}

/* Reports whether a unit is installed by the choice. */
int
inst_catalog_has(
	const struct inst_catalog *catalog,
	const char *unit)
{
	int index;

	/* The unit, and whether the choice installs it. */
	index = inst_catalog_unit_find(catalog, unit);
	if (index < 0)
		return 0;
	if (!catalog->closure[index])
		return 0;

	/* It is installed. */
	return 1;
}

/*
 * Reads a licence of the firmware for the viewer, from its first page.
 * Returns 0 or an errno value (the viewer says so).
 */
int
inst_license_load(
	struct inst_app *app,
	unsigned index)
{
	char *text;
	size_t length;
	int error;

	/* The licence there is. */
	if (index >= app->catalog.license_count)
		return EINVAL;

	/* Its text, in place of the one shown. */
	error = catalog_file(app->catalog.license[index].path, CATALOG_LICENSE_MAX, &text, &length);
	if (error != 0)
		return error;
	inst_license_close(app);
	app->license_text = text;
	app->license_length = length;
	app->license_index = index;
	app->license_offset = 0;
	app->license_page_end = 0;
	app->license_depth = 0;

	/* Succeeded: the viewer shows it. */
	return 0;
}

/* Forgets the licence the viewer read. */
void
inst_license_close(
	struct inst_app *app)
{
	/* Its text. */
	free(app->license_text);
	app->license_text = NULL;
	app->license_length = 0;
	app->license_offset = 0;
	app->license_page_end = 0;
	app->license_depth = 0;
}

/*
 * Reads a regular file of at most a size whole, with a NUL after it.
 * Returns 0 with the text (the caller frees it), or an errno value.
 */
static int
catalog_file(
	const char *path,
	size_t limit,
	char **text,
	size_t *length)
{
	struct stat status;
	char *buffer;
	size_t done;
	ssize_t count;
	int error;
	int fd;
	int regular;

	/* The file, a regular one not larger than the limit. */
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return errno;
	error = fstat(fd, &status);
	if (error != 0) {
		error = errno;
		(void)close(fd);
		return error;
	}

	/* Refuses a file that is not regular or is larger than the limit. */
	regular = S_ISREG(status.st_mode);
	if (!regular || status.st_size < 0 || (uint64_t)status.st_size > limit) {
		(void)close(fd);
		return EFBIG;
	}

	/* Its bytes. */
	buffer = malloc((size_t)status.st_size + 1U);
	if (buffer == NULL) {
		(void)close(fd);
		return ENOMEM;
	}

	/* Reads until the whole file is in or the file ends early. */
	done = 0;
	while (done < (size_t)status.st_size) {
		count = read(fd, buffer + done, (size_t)status.st_size - done);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			break;
		done += (size_t)count;
	}

	/* Ends the text after what was read. */
	(void)close(fd);
	buffer[done] = '\0';


	/* Succeeded: the text. */
	*text = buffer;
	*length = done;
	return 0;
}

/* Cuts a line at its TABs into fields; returns how many. */
static unsigned
catalog_split(
	char *line,
	char **field,
	unsigned most)
{
	unsigned count;
	char *tab;

	/* Each field up to the next TAB. */
	count = 0;
	while (count < most) {
		field[count] = line;
		count++;
		tab = strchr(line, '\t');
		if (tab == NULL)
			break;
		*tab = '\0';
		line = tab + 1;
	}

	/* The fields found. */
	return count;
}

/* Reads the units' names (the first pass); returns 0 or EINVAL. */
static int
catalog_units(
	struct inst_catalog *catalog,
	char *text)
{
	char *field[CATALOG_FIELDS_MAX];
	char *line;
	char *end;
	unsigned count;
	int same;

	/* Each line. */
	for (line = text; line != NULL && *line != '\0'; line = end) {
		end = strchr(line, '\n');
		if (end != NULL) {
			*end = '\0';
			end++;
		}

		/* A unit's record: its name. */
		count = catalog_split(line, field, CATALOG_FIELDS_MAX);
		same = strcmp(field[0], "unit");
		if (same != 0 || count < 2U)
			continue;
		if (catalog->unit_count >= INST_UNITS_MAX)
			return EINVAL;
		catalog_copy(catalog->unit[catalog->unit_count].name, sizeof(catalog->unit[0].name), field[1]);
		catalog->unit_count++;
	}

	/* An index without units is not the installer's. */
	if (catalog->unit_count == 0U)
		return EINVAL;

	/* Succeeded: the names are known. */
	return 0;
}

/* Reads every record (the second pass); returns 0 or EINVAL. */
static int
catalog_records(
	struct inst_catalog *catalog,
	char *text)
{
	struct inst_license *license;
	char *field[CATALOG_FIELDS_MAX];
	char *line;
	char *end;
	unsigned count;
	unsigned slot;
	int same;
	int error;

	/* The units come in the order the first pass named them. */
	slot = 0;

	/* Each line. */
	for (line = text; line != NULL && *line != '\0'; line = end) {
		end = strchr(line, '\n');
		if (end != NULL) {
			*end = '\0';
			end++;
		}

		/* Splits the line into its fields. */
		count = catalog_split(line, field, CATALOG_FIELDS_MAX);

		/* A unit. */
		same = strcmp(field[0], "unit");
		if (same == 0) {
			error = catalog_unit_record(catalog, slot, field, count);
			if (error != 0)
				return error;
			slot++;
			continue;
		}

		/* An item. */
		same = strcmp(field[0], "item");
		if (same == 0) {
			error = catalog_item_record(catalog, field, count);
			if (error != 0)
				return error;
			continue;
		}

		/* A licence of the firmware. */
		same = strcmp(field[0], "license");
		if (same == 0 && count >= 4U && catalog->license_count < INST_LICENSES_MAX) {
			license = &catalog->license[catalog->license_count];
			catalog_copy(license->package, sizeof(license->package), field[2]);
			catalog_copy(license->path, sizeof(license->path), field[3]);
			catalog->license_count++;
			continue;
		}

		/* The loader, whose size an existing ESP must have room for. */
		same = strcmp(field[0], "boot");
		if (same == 0 && count >= 4U) {
			same = strcmp(field[1], CATALOG_LOADER);
			if (same == 0)
				catalog->loader_bytes = strtoull(field[3], NULL, 10);
		}
	}

	/* An index without items is not the installer's. */
	if (catalog->item_count == 0U)
		return EINVAL;

	/* Succeeded: the catalog is whole. */
	return 0;
}

/* Reads a unit's record: NAME LABEL GROUP VERSION TAR-BYTES BYTES ENTRIES SHA-256 DEPENDENCIES. */
static int
catalog_unit_record(
	struct inst_catalog *catalog,
	unsigned slot,
	char **field,
	unsigned count)
{
	struct inst_unit *unit;
	char *name;
	char *next;
	int found;
	int same;

	/* The record whole. */
	if (count < 10U || slot >= catalog->unit_count)
		return EINVAL;
	unit = &catalog->unit[slot];
	catalog_copy(unit->label, sizeof(unit->label), field[2]);
	catalog_copy(unit->group, sizeof(unit->group), field[3]);
	unit->bytes = strtoull(field[6], NULL, 10);
	unit->entries = strtoull(field[7], NULL, 10);
	unit->dependency_count = 0;

	/* Each unit it needs, by its name ("-" for none). */
	for (name = field[9]; name != NULL && *name != '\0'; name = next) {
		next = strchr(name, ' ');
		if (next != NULL) {
			*next = '\0';
			next++;
		}

		/* A dash or an empty word names no unit. */
		same = strcmp(name, "-");
		if (same == 0)
			continue;
		if (*name == '\0')
			continue;

		/* Finds the unit by its name. */
		found = inst_catalog_unit_find(catalog, name);
		if (found < 0 || unit->dependency_count >= INST_DEPENDENCIES_MAX)
			return EINVAL;
		unit->dependency[unit->dependency_count] = (unsigned)found;
		unit->dependency_count++;
	}

	/* Succeeded: the unit is read. */
	return 0;
}

/* Reads an item's record: ID PARENT DEFAULT LABEL UNITS. */
static int
catalog_item_record(
	struct inst_catalog *catalog,
	char **field,
	unsigned count)
{
	struct inst_item *item;
	char *name;
	char *next;
	int found;
	int same;

	/* The record whole. */
	if (count < 6U || catalog->item_count >= INST_ITEMS_MAX)
		return EINVAL;
	item = &catalog->item[catalog->item_count];
	catalog_copy(item->id, sizeof(item->id), field[1]);
	catalog_copy(item->label, sizeof(item->label), field[4]);

	/* The item it is under, which comes before it. */
	item->parent = -1;
	same = strcmp(field[2], "-");
	if (same != 0) {
		item->parent = inst_item_find(catalog, field[2]);
		if (item->parent < 0)
			return EINVAL;
	}

	/* Its default, which it starts at. */
	item->fallback = 0;
	same = strcmp(field[3], "on");
	if (same == 0) {
		item->fallback = 1;
	} else {
		same = strcmp(field[3], "required");
		if (same == 0)
			item->fallback = 2;
	}

	/* An item with a default starts chosen. */
	item->chosen = 0;
	if (item->fallback != 0)
		item->chosen = 1;

	/* Its units ("-" for a heading). */
	item->unit_count = 0;
	for (name = field[5]; name != NULL && *name != '\0'; name = next) {
		next = strchr(name, ' ');
		if (next != NULL) {
			*next = '\0';
			next++;
		}

		/* A dash or an empty word names no unit. */
		same = strcmp(name, "-");
		if (same == 0)
			continue;
		if (*name == '\0')
			continue;

		/* Finds the unit by its name. */
		found = inst_catalog_unit_find(catalog, name);
		if (found < 0 || item->unit_count >= INST_ITEM_UNITS_MAX)
			return EINVAL;
		item->unit[item->unit_count] = (unsigned)found;
		item->unit_count++;
	}

	/* A heading is not a choice. */
	if (item->unit_count == 0U)
		item->chosen = 0;
	catalog->item_count++;

	/* Succeeded: the item is read. */
	return 0;
}

/* Copies a text into a buffer, cut to fit with its NUL. */
static void
catalog_copy(
	char *target,
	size_t size,
	const char *source)
{
	/* The text, cut at the buffer's end. */
	(void)snprintf(target, size, "%s", source);
}

/* Marks a unit installed, and every unit it needs. */
static void
catalog_close_over(
	struct inst_catalog *catalog,
	unsigned unit)
{
	unsigned j;

	/* A unit marked already has its needs marked (this also ends a cycle). */
	if (catalog->closure[unit])
		return;
	catalog->closure[unit] = 1;

	/* What it needs. */
	for (j = 0; j < catalog->unit[unit].dependency_count; j++)
		catalog_close_over(catalog, catalog->unit[unit].dependency[j]);
}
