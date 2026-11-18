/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD mount userland command.
 */

#include "userland/base/common/fstab.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <unistd.h>
#include <uapi/mountinfo.h>

/* One mount option that sets a flag of mount(2). */
struct mount_flag_option {
	const char *name;
	int flag;
};

/*
 * The mount options that set a flag, in the order the list of mounts shows
 * them; "ro" comes first and is shown as "ro" or "rw".  Never changed.
 */
static const struct mount_flag_option mount_flag_options[] = {
	{ "ro", MNT_RDONLY },
	{ "nosuid", MNT_NOSUID },
	{ "writethru", MNT_WRITETHRU },
	{ "nojournal", MNT_NOJOURNAL },
	{ "noexec", MNT_NOEXEC },
};

static void print_mount_options(const struct kern_mount_info *entry);
static int option_flag(const char *option);
static int apply_command_options(char *options, int *flags, const char **source);
static const char *program_name(const char *path);
static int run_unmount(int argc, char **argv);
static int mount_all(void);
static int mount_fstab_entry(const char *source, const char *target, const char *type, char *options);
static const char *kernel_type_name(const char *type);

static int
list_mounts(void)
{
	struct kern_mount_query *query;
	unsigned i;
	int fd, error;
	query = calloc(1, sizeof(*query) + KERN_MOUNT_INFO_MAX *
	    sizeof(query->entries[0]));
	if (query == NULL) {
		fprintf(stderr, "mount: out of memory\n");
		return 1;
	}
	query->version = KERN_MOUNT_INFO_VERSION;
	query->struct_size = sizeof(*query);
	query->capacity = KERN_MOUNT_INFO_MAX;
	fd = open("/dev/system", O_RDONLY);
	error = fd < 0 ? errno : ioctl(fd, KERN_SYSTEM_GET_MOUNTS, query) < 0 ?
	    errno : 0;
	if (fd >= 0)
		close(fd);
	if (error != 0) {
		fprintf(stderr, "mount: list: %s\n", strerror(error));
		free(query);
		return 1;
	}
	for (i = 0; i < query->count; i++) {
		const struct kern_mount_info *entry = &query->entries[i];
		printf("%s%s on %s type %s (",
		    entry->device != 0 && !(entry->kind & KERN_MOUNT_INFO_BIND) ?
		    "/dev/" : "", entry->source[0] ? entry->source : entry->type,
		    entry->target, entry->type);
		print_mount_options(entry);
		printf(")\n");
	}
	free(query);
	return 0;
}

/*
 * Runs the mount command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	struct mount_args arguments;
	const char *type, *source, *target;
	int flags, i;
	int error;

	type = NULL;
	source = NULL;
	target = NULL;
	flags = 0;

	/* Handles the selected command-line operation. */
	if (strcmp(program_name(argv[0]), "umount") == 0) {
		/* Obtains the run unmount result. */
		function_result = run_unmount(argc, argv);

		/* Returns the computed result. */
		return function_result;
	}

	if (argc == 1)
		return list_mounts();
	/* Handles the selected command-line operation. */
	if (argc == 2 && strcmp(argv[1], "-a") == 0) {
		/* Obtains the mount all result. */
		function_result = mount_all();

		/* Returns the computed result. */
		return function_result;
	}

	memset(&arguments, 0, sizeof(arguments));

	/* Process each remaining command-line operand. */
	arguments.size = sizeof(arguments);
	arguments.version = KERN_MOUNT_ARGS_VERSION;
	for (i = 1; i < argc; i++) {
		/* Handles the selected command-line operation. */
		if (strcmp(argv[i], "-r") == 0) {
			flags |= MNT_RDONLY;
		} else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
			type = kernel_type_name(argv[++i]);
		} else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
			i++;

			/* Applies each option of the comma-separated list. */
			error = apply_command_options(argv[i], &flags, &source);
			if (error != 0)
				return 2;
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "mount: unknown option: %s\n", argv[i]);

			/* Reports operation failure. */
			return 2;
		} else if (target == NULL) {
			target = argv[i];
		} else if (source == NULL) {
			source = target;
			target = argv[i];
		} else {
			target = NULL;
			break;
		}
	}

	/* Handles the type availability. */
	if (type == NULL || target == NULL) {
		fprintf(stderr,
			"usage: mount -t type [-r] [-o ro|rw|nosuid|noexec|writethru|nojournal|fspec=disk[,...]] "
			"[disk] directory\n");

		/* Reports operation failure. */
		return 2;
	}

	/* Handles the source availability. */
	if (source != NULL) {
		/* Selects the matching prefix. */
		if (strncmp(source, "/dev/", 5) == 0)
			source += 5;

		/* Handles a failed strlen operation. */
		if (strlen(source) >= sizeof(arguments.fspec)) {
			fprintf(stderr, "mount: device name is too long\n");

			/* Reports operation failure. */
			return 2;
		}
		strcpy(arguments.fspec, source);
	}

	/* Handles a failed mount operation. */
	if (mount(type, target, flags, source != NULL ? &arguments : NULL) !=
	    0) {
		fprintf(stderr, "mount: %s on %s: %s\n",
			source != NULL ? source : type, target,
			strerror(errno));

		/* Reports operation failure. */
		return 1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Prints the options of one mounted filesystem, as mount -o names them. */
static void
print_mount_options(
	const struct kern_mount_info *entry)
{
	size_t index;

	/* A mount is either read-only or read-write. */
	if ((entry->flags & MNT_RDONLY) != 0)
		fputs("ro", stdout);
	else
		fputs("rw", stdout);

	/* Adds each other flag the mount carries, by its option's name. */
	for (index = 1; index < sizeof(mount_flag_options) / sizeof(mount_flag_options[0]); index++) {
		if ((entry->flags & (unsigned)mount_flag_options[index].flag) != 0)
			printf(",%s", mount_flag_options[index].name);
	}

	/* Marks a bind mount. */
	if ((entry->kind & KERN_MOUNT_INFO_BIND) != 0)
		fputs(",bind", stdout);
}

/*
 * Applies a comma-separated list of options from the command line.
 *
 * Each flag option sets its flag; "rw" clears the read-only flag that "-r" or
 * an earlier "ro" set, so the last of them wins; "defaults" sets nothing; and
 * "fspec=disk" names the disk.  The list is split in place, so a disk named by
 * fspec stays in the argument it came from.  Reports 0, or -1 after printing
 * the option it does not know.
 */
static int
apply_command_options(
	char *options,
	int *flags,
	const char **source)
{
	char *option;
	int flag, differs;

	/* Walks the options between the commas. */
	for (option = strtok(options, ",");
	     option != NULL;
	     option = strtok(NULL, ",")) {
		/* Takes the flag the option names, if it names one. */
		flag = option_flag(option);
		if (flag != 0) {
			*flags |= flag;
			continue;
		}

		/* "rw" asks for a writable mount. */
		differs = strcmp(option, "rw");
		if (differs == 0) {
			*flags &= ~MNT_RDONLY;
			continue;
		}

		/* "defaults" asks for nothing beyond the defaults. */
		differs = strcmp(option, "defaults");
		if (differs == 0)
			continue;

		/* "fspec=disk" names the disk to mount. */
		differs = strncmp(option, "fspec=", 6);
		if (differs == 0) {
			*source = option + 6;
			continue;
		}

		/* Refuses an option the command does not know. */
		fprintf(stderr, "mount: unsupported option: %s\n", option);
		return -1;
	}

	/* Succeeded: every option was applied. */
	return 0;
}

/*
 * Translates a filesystem type name other systems use into the kernel's.
 *
 * The kernel calls its FAT filesystem "fat"; an fstab line or a command
 * written for another system says msdosfs (the BSDs), vfat or msdos
 * (Linux).  Any other name is passed on unchanged.
 */
static const char *
kernel_type_name(
	const char *type)
{
	static const char *const fat_names[] = { "msdosfs", "vfat", "msdos" };
	size_t index;
	int differs;

	/* Compares the name with each FAT alias in turn. */
	for (index = 0; index < sizeof(fat_names) / sizeof(fat_names[0]); index++) {
		differs = strcmp(type, fat_names[index]);
		if (differs == 0)
			return "fat";
	}

	/* Succeeded: the name is the kernel's already. */
	return type;
}

/* Finds the mount(2) flag an option names; reports 0 when it names none. */
static int
option_flag(
	const char *option)
{
	size_t index;
	int differs;

	/* Compares the option with each name in turn. */
	for (index = 0; index < sizeof(mount_flag_options) / sizeof(mount_flag_options[0]); index++) {
		differs = strcmp(option, mount_flag_options[index].name);
		if (differs == 0)
			return mount_flag_options[index].flag;
	}

	/* The option names no flag. */
	return 0;
}

/* Supports the program name operation. */
static const char *
program_name(
	const char *path)
{
	const char *slash;

	slash = strrchr(path != NULL ? path : "", '/');

	/* Returns the computed result. */
	return slash != NULL ? slash + 1 : path;
}

/* Supports the run unmount operation. */
static int
run_unmount(
	int argc,
	char **argv)
{
	int flags;
	int index;
	int end_options;

	flags = 0;
	index = 1;
	end_options = 0;
	if (index < argc && strcmp(argv[index], "-f") == 0) {
		flags = MNT_FORCE;
		index++;
	}
	if (index < argc && strcmp(argv[index], "--") == 0) {
		end_options = 1;
		index++;
	}

	/* Validates the command-line arguments. */
	if (index != argc - 1 || (!end_options && argv[index][0] == '-')) {
		fprintf(stderr, "usage: umount [-f] [--] directory\n");

		/* Reports operation failure. */
		return 2;
	}

	/* Validates the command-line arguments. */
	if (unmount(argv[index], flags) != 0) {
		fprintf(stderr, "umount: %s: %s\n", argv[index], strerror(errno));

		/* Reports operation failure. */
		return 1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the mount all operation. */
static int
mount_all(
	void)
{
	struct command_fstab entry;
	FILE *stream;
	unsigned line_number;
	int result;
	int failed;

	stream = fopen(FSTAB_PATH, "r");
	line_number = 0;
	failed = 0;
	if (stream == NULL) {
		fprintf(stderr, "mount: %s: %s\n", FSTAB_PATH, strerror(errno));
		return 1;
	}

	/* Swap records belong to swapon, after normal filesystem mounting. */
	while ((result = command_fstab_next(stream, &entry, &line_number)) != 0) {
		if (result < 0) {
			fprintf(stderr, "mount: %s:%u: invalid entry or read failure\n",
			    FSTAB_PATH, line_number);
			failed = 1;
			if (ferror(stream))
				break;
			continue;
		}
		if (strcmp(entry.type, "swap") == 0)
			continue;
		if (mount_fstab_entry(entry.source, entry.target, entry.type,
		    entry.options) != 0)
			failed = 1;
	}
	if (fclose(stream) != 0)
		failed = 1;
	return failed;
}

/* Supports the mount fstab entry operation. */
static int
mount_fstab_entry(
	const char *source,
	const char *target,
	const char *type,
	char *options)
{
	struct mount_args arguments;
	char *option;
	int flags, nofail;
	int flag;

	flags = 0;
	nofail = 0;

	/* Selects the matching value. */
	if (strcmp(target, "/") == 0)
		return 0;
	memset(&arguments, 0, sizeof(arguments));
	arguments.size = sizeof(arguments);
	arguments.version = KERN_MOUNT_ARGS_VERSION;

	/* Selects the matching prefix. */
	if (strncmp(source, "/dev/", 5) == 0)
		source += 5;

	/* Handles a failed strlen operation. */
	if (strlen(source) >= sizeof(arguments.fspec)) {
		errno = ENAMETOOLONG;

		/* Reports operation failure. */
		return -1;
	}
	strcpy(arguments.fspec, source);

	/* Process each element required by the operation. */
	for (option = strtok(options, ","); option != NULL;
	     option = strtok(NULL, ",")) {
		/* Takes the flag the option names; nofail, rw and defaults set none. */
		flag = option_flag(option);
		if (flag != 0)
			flags |= flag;
		else if (strcmp(option, "nofail") == 0)
			nofail = 1;
		else if (strcmp(option, "rw") != 0 &&
			 strcmp(option, "defaults") != 0) {
			fprintf(stderr, "mount: unsupported fstab option: %s\n",
				option);

			/* Reports operation failure. */
			return -1;
		}
	}

	/* Handles the reported system error. */
	type = kernel_type_name(type);
	if (mount(type, target, flags, &arguments) == 0 ||
	    (nofail && (errno == ENOENT || errno == ENODEV)))

		/* Reports successful completion. */
		return 0;
	fprintf(stderr, "mount: %s on %s: %s\n", source, target,
		strerror(errno));

	/* Reports operation failure. */
	return -1;
}
