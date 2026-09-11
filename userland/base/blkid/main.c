/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * blkid - print block-device attributes.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <uapi/blkid.h>

static int identify(const char *path);
static unsigned selected_tags;
static int export_output;
static unsigned tag_flag(const char *tag);
static void export_value(const char *tag, const char *value);

/*
 * Runs the blkid command.
 */
int
main(
	int argc,
	char **argv)
{
	int status, index, first;
	unsigned flag;

	status = 0;
	selected_tags = 0;
	export_output = 0;
	first = 1;
	while (first < argc && argv[first][0] == '-') {
		if (strcmp(argv[first], "--") == 0) {
			first++;
			break;
		}
		if (first + 1 >= argc)
			goto usage;
		if (strcmp(argv[first], "-o") == 0) {
			if (strcmp(argv[first + 1], "export") == 0)
				export_output = 1;
			else if (strcmp(argv[first + 1], "full") == 0)
				export_output = 0;
			else
				goto usage;
		} else if (strcmp(argv[first], "-s") == 0) {
			flag = tag_flag(argv[first + 1]);
			if (flag == 0)
				goto usage;
			selected_tags |= flag;
		} else {
			goto usage;
		}
		first += 2;
	}
	if (selected_tags == 0)
		selected_tags = ~0U;

	if (first >= argc)
		goto usage;

	/* Process each remaining command-line operand. */
	for (index = first; index < argc; index++)
		status |= identify(argv[index]);

	/* Returns the computed result. */
	if (fflush(stdout) != 0 && status == 0)
		status = 1;
	if (ferror(stdout) && status == 0)
		status = 1;
	return status;
usage:
	fprintf(stderr, "usage: blkid [-o full|export] [-s TAG] [--] device ...\n");
	return 2;
}

/* Supports the identify operation. */
static int
identify(
	const char *path)
{
	int error;
	struct block_identity id;
	int fd;

	fd = open(path, O_RDONLY);

	/* Checks the file descriptor. */
	if (fd < 0) {
		fprintf(stderr, "blkid: %s: %s\n", path, strerror(errno));

		/* Reports operation failure. */
		return 1;
	}

	/* Handles a failed ioctl operation. */
	if (ioctl(fd, BLKGETIDENTITY, &id) != 0) {
		error = errno;
		close(fd);

		/* Handles an operation failure. */
		if (error == ENOENT || error == ENOTTY || error == EOPNOTSUPP ||
		    error == ENXIO)

			/* Reports successful completion. */
			return export_output ? 2 : 0;
		fprintf(stderr, "blkid: %s: %s\n", path, strerror(error));

		/* Reports operation failure. */
		return 1;
	}
	if (close(fd) != 0) {
		fprintf(stderr, "blkid: %s: %s\n", path, strerror(errno));
		return 1;
	}
	if (export_output) {
		export_value("DEVNAME", path);
		if (id.flags & selected_tags & KERN_BLKID_TYPE)
			export_value("TYPE", id.type);
		if (id.flags & selected_tags & KERN_BLKID_UUID)
			export_value("UUID", id.uuid);
		if (id.flags & selected_tags & KERN_BLKID_PARTUUID)
			export_value("PARTUUID", id.partuuid);
		if (id.flags & selected_tags & KERN_BLKID_LABEL)
			export_value("LABEL", id.label);
		if (id.flags & selected_tags & KERN_BLKID_PARTLABEL)
			export_value("PARTLABEL", id.partlabel);
		putchar('\n');
		return (id.flags & selected_tags) != 0 ? 0 : 2;
	}
	printf("%s:", path);

	/* Handles the id condition. */
	if (id.flags & selected_tags & KERN_BLKID_LABEL)
		printf(" LABEL=\"%s\"", id.label);

	/* Handles the id condition. */
	if (id.flags & selected_tags & KERN_BLKID_UUID)
		printf(" UUID=\"%s\"", id.uuid);

	/* Handles the id condition. */
	if (id.flags & selected_tags & KERN_BLKID_TYPE)
		printf(" TYPE=\"%s\"", id.type);

	/* Handles the id condition. */
	if (id.flags & selected_tags & KERN_BLKID_PARTLABEL)
		printf(" PARTLABEL=\"%s\"", id.partlabel);

	/* Handles the id condition. */
	if (id.flags & selected_tags & KERN_BLKID_PARTUUID)
		printf(" PARTUUID=\"%s\"", id.partuuid);
	putchar('\n');

	/* Reports successful completion. */
	return 0;
}

/* Select only documented identity fields; unknown names are an invocation error. */
static unsigned
tag_flag(const char *tag)
{
	if (strcmp(tag, "TYPE") == 0) return KERN_BLKID_TYPE;
	if (strcmp(tag, "UUID") == 0) return KERN_BLKID_UUID;
	if (strcmp(tag, "PARTUUID") == 0) return KERN_BLKID_PARTUUID;
	if (strcmp(tag, "LABEL") == 0) return KERN_BLKID_LABEL;
	if (strcmp(tag, "PARTLABEL") == 0) return KERN_BLKID_PARTLABEL;
	return 0;
}

/* Escape whitespace, separators and backslashes so one value stays one record. */
static void
export_value(const char *tag, const char *value)
{
	unsigned char byte;

	printf("%s=", tag);
	while (*value != '\0') {
		byte = (unsigned char)*value++;
		if (byte <= 0x20U || byte >= 0x7fU || byte == '\\' ||
		    byte == '=' || byte == '\"')
			printf("\\x%02x", (unsigned)byte);
		else
			putchar(byte);
	}
	putchar('\n');
}
