/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the UEFI loader's rootpart: split of zedbsd.cfg and of
 * the shared parser on the split text.
 */

#include "bootloader/uefi/root-config.h"
#include "bootloader/uefi/zedbsd-config.h"
#include "bootloader/common/logo-path.h"

#include <stdio.h>
#include <string.h>

static void check(int condition, const char *what);
static enum zbl_uefi_root_config_result split(const char *text, char *output, struct zbl_uefi_root_config *root);

/* How many checks failed. */
static int failures;

/*
 * Runs the checks.
 */
int
main(void)
{
	static struct zbl_uefi_kern_config configuration;
	struct zbl_uefi_root_config root;
	enum zbl_uefi_root_config_result result;
	enum zbl_uefi_kern_config_result parsed;
	char output[4096];
	char logo[ZBL_LOGO_PATH_MAX + 1];
	int found;
	static const unsigned char expected_guid[16] = {
		0x78, 0x56, 0x34, 0x12, 0xbc, 0x9a, 0xf0, 0xde,
		0x81, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef
	};

	/* Splits the installed system's configuration. */
	result = split("kernel=rootpart:/boot/vmunix\r\nrootpart=PARTUUID=12345678-9abc-def0-8123-456789ABCDEF\r\n"
	    "logo=rootpart:/boot/logo.ppm\nkmsg=quiet\n", output, &root);
	check(result == ZBL_UEFI_ROOT_CONFIG_OK, "the installed configuration");
	check(root.kernel_on_root && root.logo_on_root, "both files on the root");
	check(memcmp(root.partuuid, expected_guid, 16) == 0, "the PARTUUID in GPT byte order");
	check(strcmp(output, "kernel=boot/vmunix\r\nrootpart=PARTUUID=12345678-9abc-def0-8123-456789ABCDEF\r\n"
	    "logo=boot/logo.ppm\nkmsg=quiet\n") == 0, "the prefixes are taken off");

	/* The shared parser takes the split text: the kernel path and the logo. */
	parsed = zbl_uefi_kern_config_parse(&configuration, output, strlen(output), "ABCD-1234", 10);
	check(parsed == ZBL_UEFI_KERN_CONFIG_OK, "the shared parser takes the split text");
	check(strcmp(configuration.kernel_path, "boot/vmunix") == 0, "the kernel path");
	found = zbl_logo_path(configuration.parameter_record.text, configuration.parameter_record.length, logo, sizeof(logo));
	check(found > 0 && strcmp(logo, "boot/logo.ppm") == 0, "the logo path");

	/* The shared parser refuses the unsplit text, as the BIOS loaders do. */
	strcpy(output, "kernel=rootpart:/boot/vmunix\nrootpart=PARTUUID=12345678-9abc-def0-8123-456789abcdef\n");
	parsed = zbl_uefi_kern_config_parse(&configuration, output, strlen(output), "ABCD-1234", 10);
	check(parsed == ZBL_UEFI_KERN_CONFIG_INVALID_KERNEL_PATH, "the shared parser refuses rootpart:");

	/* The shared logo rule refuses rootpart: too, as the BIOS loaders read it. */
	found = zbl_logo_path("logo=rootpart:/boot/logo.ppm", 29, logo, sizeof(logo));
	check(found < 0, "the shared logo rule refuses rootpart:");

	/* A path that escapes is refused by the shared parser after the split. */
	result = split("kernel=rootpart:/boot/../vmunix\nrootpart=PARTUUID=12345678-9abc-def0-8123-456789abcdef\n", output, &root);
	check(result == ZBL_UEFI_ROOT_CONFIG_OK, "split of a path with ..");
	parsed = zbl_uefi_kern_config_parse(&configuration, output, strlen(output), "ABCD-1234", 10);
	check(parsed == ZBL_UEFI_KERN_CONFIG_INVALID_KERNEL_PATH, ".. is refused after the split");

	/* Today's image is unchanged: nothing on the root, no rootpart= needed. */
	result = split("kernel=vmunix\nrootpart=PARTLABEL=zedBSD-root\nlogo=logo.ppm\n", output, &root);
	check(result == ZBL_UEFI_ROOT_CONFIG_OK, "today's configuration");
	check(!root.kernel_on_root && !root.logo_on_root, "nothing on the root");
	check(strcmp(output, "kernel=vmunix\nrootpart=PARTLABEL=zedBSD-root\nlogo=logo.ppm\n") == 0, "copied as it is");

	/* Refuses a root file without a usable rootpart= line. */
	result = split("kernel=rootpart:/boot/vmunix\n", output, &root);
	check(result == ZBL_UEFI_ROOT_CONFIG_NO_ROOTPART, "no rootpart=");
	result = split("kernel=rootpart:/boot/vmunix\nrootpart=PARTLABEL=zedBSD-root\n", output, &root);
	check(result == ZBL_UEFI_ROOT_CONFIG_ROOTPART_NOT_PARTUUID, "PARTLABEL is refused");
	result = split("kernel=rootpart:/boot/vmunix\nrootpart=PARTUUID=1234\n", output, &root);
	check(result == ZBL_UEFI_ROOT_CONFIG_BAD_PARTUUID, "a short PARTUUID");
	result = split("kernel=rootpart:/boot/vmunix\nrootpart=PARTUUID=00000000-0000-0000-0000-000000000000\n", output, &root);
	check(result == ZBL_UEFI_ROOT_CONFIG_BAD_PARTUUID, "the zero PARTUUID");
	result = split("kernel=rootpart:/boot/vmunix\nrootpart=PARTUUID=12345678-9abc-def0-8123-456789abcdef\n"
	    "rootpart=PARTUUID=12345678-9abc-def0-8123-456789abcdee\n", output, &root);
	check(result == ZBL_UEFI_ROOT_CONFIG_DUPLICATE_ROOTPART, "two rootpart= lines");

	/* Reports the verdict. */
	if (failures != 0) {
		printf("root-config-test: FAIL (%d)\n", failures);
		return 1;
	}
	printf("root-config-test: PASS\n");
	return 0;
}

/* Counts and prints a failed check. */
static void
check(
	int condition,
	const char *what)
{
	/* Reports a failure by name. */
	if (!condition) {
		printf("FAIL: %s\n", what);
		failures++;
	}
}

/* Splits a text, terminating the copy. */
static enum zbl_uefi_root_config_result
split(
	const char *text,
	char *output,
	struct zbl_uefi_root_config *root)
{
	enum zbl_uefi_root_config_result result;
	size_t size;

	/* Splits the text and terminates the copy for comparison. */
	size = 0;
	result = zbl_uefi_root_config_split(text, strlen(text), output, 4095, &size, root);
	output[size] = '\0';
	return result;
}
