/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The installer's model on the host -- the index read into
 * the catalog (every unit, its dependencies, the items, the closure of the
 * defaults), a probe's lines read into disks and partitions, and the rules
 * of the screens (a live disk refused, partitions chosen refused without
 * GPT or with another system's zedbsd.cfg, an ESP that is enough, the free
 * room and a new partition's place).  Built by
 * plan/tools/installer/host/installer-model-test.sh with the index of a build.
 */

#include "userland/desktop/installer/installer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

/* The words of the screens are English here. */
const char *
kl_tr(
	const char *english)
{
	return english;
}

static void
check(
	int condition,
	const char *what)
{
	if (condition) {
		printf("ok %s\n", what);
		return;
	}
	printf("FAIL %s\n", what);
	failures++;
}

static void
probe_feed(
	struct inst_probe *probe,
	const char *text)
{
	char line[1024];
	const char *end;
	size_t length;

	memset(probe, 0, sizeof(*probe));
	while (*text != '\0') {
		end = strchr(text, '\n');
		length = end != NULL ? (size_t)(end - text) : strlen(text);
		memcpy(line, text, length);
		line[length] = '\0';
		(void)inst_probe_line(probe, line);
		text += length;
		if (*text == '\n')
			text++;
	}
}

int
main(void)
{
	static struct inst_app app;
	const struct inst_disk *disk;
	const struct inst_part *esp;
	uint64_t start;
	uint64_t sectors;
	uint64_t esp_start;
	uint64_t bytes;
	int error;
	int index;

	/* The index of a build. */
	error = inst_catalog_read(&app.catalog);
	check(error == 0, "the index is read");
	check(app.catalog.unit_count >= 10U, "units are read");
	check(app.catalog.item_count >= 10U, "items are read");
	check(app.catalog.license_count > 0U, "the firmware's licences are read");
	check(app.catalog.loader_bytes > 0U, "the loader's size is read");
	index = inst_catalog_unit_find(&app.catalog, "base");
	check(index >= 0 && app.catalog.unit[index].dependency_count > 0U, "base's dependency (a unit after it) is resolved");
	check(inst_catalog_has(&app.catalog, "base"), "the defaults install base");
	check(inst_catalog_has(&app.catalog, "desktop"), "the defaults install the desktop");
	check(inst_catalog_has(&app.catalog, "firmware"), "the defaults install the firmware");
	check(inst_catalog_has(&app.catalog, "network-openssh"), "the defaults install OpenSSH");
	check(inst_catalog_has(&app.catalog, "fonts-hand-hershey"), "the desktop brings its fonts");
	check(!inst_catalog_has(&app.catalog, "desktop-gtk4"), "GTK4 is off by default");
	index = inst_item_find(&app.catalog, "gtk4");
	check(index >= 0, "the GTK4 item is there");
	app.catalog.item[index].chosen = 1;
	inst_catalog_choose(&app.catalog);
	check(inst_catalog_has(&app.catalog, "libs-cairo"), "GTK4 brings cairo");
	index = inst_item_find(&app.catalog, "other/libs-cairo");
	check(index >= 0 && inst_item_by_dependency(&app.catalog, (unsigned)index), "cairo's item comes with GTK4");

	/* A probe: the live USB, an empty NVMe, another system's SATA disk, an MBR disk. */
	probe_feed(&app.probe,
		"firmware\tuefi\n"
		"memory\t8583294976\n"
		"disk\tsda\t5\tremovable\t512\t4194304\t2\t1\tgpt\t0\t4\t" "aa\n"
		"disk\tnvme0n1\t9\tnvme\t512\t25165824\t0\t0\tnone\t0\t0\t" "00\n"
		"disk\tsdb\t11\tfixed\t512\t29360128\t0\t0\tgpt\t0\t2\t" "bb\n"
		"part\t11\t1\t2048\t614400\tC12A7328-F81F-11D2-BA4B-00A0C93EC93B\tc1\t0\tsdb1\tvfat\t1234-5678\t0\t0\tEFI system partition\tOTHERESP\n"
		"fat\t11\t1\t300000000\tno\tyes\tno\n"
		"part\t11\t2\t616448\t8388608\tEBD0A0A2-B9E5-4433-87C0-68B6B72699C7\tc2\t0\tsdb2\t-\t-\t0\t0\tBasic data partition\t-\n"
		"disk\tsdc\t12\tremovable\t512\t4194304\t0\t0\tmbr\t0\t1\t" "cc\n"
		"end\n");
	check(app.probe.uefi == 1 && app.probe.disk_count == 4U && app.probe.part_count == 2U, "the probe is read");
	disk = inst_disk_find(&app.probe, 5);
	check(disk != NULL && inst_disk_refusal(&app, disk) != NULL, "the live disk is refused");
	disk = inst_disk_find(&app.probe, 9);
	check(disk != NULL && inst_disk_refusal(&app, disk) == NULL, "the empty NVMe disk takes an installation");
	check(disk != NULL && inst_chosen_refusal(&app, disk) != NULL, "the empty disk's partitions cannot be chosen (no GPT)");
	disk = inst_disk_find(&app.probe, 12);
	check(disk != NULL && inst_chosen_refusal(&app, disk) != NULL, "an MBR disk's partitions cannot be chosen");
	disk = inst_disk_find(&app.probe, 11);
	check(disk != NULL && inst_chosen_refusal(&app, disk) == NULL, "another system's GPT disk's partitions can be chosen");
	esp = disk != NULL ? inst_disk_esp(&app, disk) : NULL;
	check(esp != NULL && esp->slot == 1U, "its ESP is enough");
	error = disk != NULL ? inst_largest_gap(disk, &app.probe, &start, &sectors) : -1;
	check(error == 0 && start == 616448U + 8388608U && start % 2048U == 0U, "the largest gap is after the basic data partition");
	error = inst_place(disk, &app.probe, 4ULL << 30, 0, &esp_start, &start, &sectors);
	check(error == 0 && start == 9005056U && sectors == (4ULL << 30) / 512U, "a new partition goes in the first room it fits");
	error = inst_place(disk, &app.probe, 64ULL << 30, 0, &esp_start, &start, &sectors);
	check(error != 0, "a partition too large has no room");

	/* zedbsd.cfg on a FAT, or a FAT not known, refuses the partitions. */
	probe_feed(&app.probe,
		"firmware\tuefi\n"
		"disk\tsdb\t11\tfixed\t512\t29360128\t0\t0\tgpt\t0\t1\t" "bb\n"
		"part\t11\t1\t2048\t614400\tC12A7328-F81F-11D2-BA4B-00A0C93EC93B\tc1\t0\tsdb1\tvfat\t1234-5678\t0\t0\tEFI\t-\n"
		"fat\t11\t1\t300000000\tunknown\tno\tno\n"
		"end\n");
	disk = inst_disk_find(&app.probe, 11);
	check(disk != NULL && inst_chosen_refusal(&app, disk) != NULL, "a FAT not known refuses the partitions");

	/* An ESP formatted after it appeared: no file system type named yet, but the FAT was looked into. */
	probe_feed(&app.probe,
		"firmware\tuefi\n"
		"disk\tsdb\t11\tfixed\t512\t29360128\t0\t0\tgpt\t0\t2\t" "bb\n"
		"part\t11\t1\t2048\t1048576\tC12A7328-F81F-11D2-BA4B-00A0C93EC93B\tc1\t0\tsdb1\t-\t-\t0\t0\tEFI System\t-\n"
		"fat\t11\t1\t536000000\tno\tno\tno\n"
		"part\t11\t2\t1050624\t12582912\t00000000-0000-0000-0000-000000000000\tc2\t0\tsdb2\t-\t-\t0\t0\t-\t-\n"
		"end\n");
	disk = inst_disk_find(&app.probe, 11);
	esp = disk != NULL ? inst_disk_esp(&app, disk) : NULL;
	check(esp != NULL && esp->slot == 1U, "an ESP made on S4 is enough before its type is named");

	/* Sizes. */
	error = inst_size_parse("20 GiB", &bytes);
	check(error == 0 && bytes == 20ULL << 30, "20 GiB");
	error = inst_size_parse("512M", &bytes);
	check(error == 0 && bytes == 512ULL << 20, "512M");
	error = inst_size_parse("1.5 G", &bytes);
	check(error == 0 && bytes == 3ULL << 29, "1.5 G");
	error = inst_size_parse("ten", &bytes);
	check(error != 0, "ten is refused");

	printf("installer-model-test: %s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
	return failures == 0 ? 0 : 1;
}
