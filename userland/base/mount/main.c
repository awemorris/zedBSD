/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>

static int
mount_fstab_entry(const char *source, const char *target, const char *type,
		  char *options)
{
	struct mount_args arguments;
	char *option;
	int flags = 0, nofail = 0;

	if (strcmp(target, "/") == 0)
		return 0;
	memset(&arguments, 0, sizeof(arguments));
	arguments.size = sizeof(arguments);
	arguments.version = ZEDBSD_MOUNT_ARGS_VERSION;
	if (strncmp(source, "/dev/", 5) == 0)
		source += 5;
	if (strlen(source) >= sizeof(arguments.fspec)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	strcpy(arguments.fspec, source);
	for (option = strtok(options, ","); option != NULL;
	     option = strtok(NULL, ",")) {
		if (strcmp(option, "ro") == 0)
			flags |= MNT_RDONLY;
		else if (strcmp(option, "nosuid") == 0)
			flags |= MNT_NOSUID;
		else if (strcmp(option, "nofail") == 0)
			nofail = 1;
		else if (strcmp(option, "rw") != 0 &&
			 strcmp(option, "defaults") != 0) {
			fprintf(stderr, "mount: unsupported fstab option: %s\n",
				option);
			return -1;
		}
	}
	if (mount(type, target, flags, &arguments) == 0 ||
	    (nofail && (errno == ENOENT || errno == ENODEV)))
		return 0;
	fprintf(stderr, "mount: %s on %s: %s\n", source, target,
		strerror(errno));
	return -1;
}

static int
mount_all(void)
{
	FILE *stream = fopen("/etc/fstab", "r");
	char line[1024];
	unsigned line_number = 0;
	int failed = 0;

	if (stream == NULL) {
		fprintf(stderr, "mount: /etc/fstab: %s\n", strerror(errno));
		return 1;
	}
	while (fgets(line, sizeof(line), stream) != NULL) {
		char *source, *target, *type, *options, *extra, *cursor = line;
		line_number++;
		while (*cursor == ' ' || *cursor == '\t')
			cursor++;
		if (*cursor == '#' || *cursor == '\n' || *cursor == '\0')
			continue;
		source = strtok(cursor, " \t\r\n");
		target = strtok(NULL, " \t\r\n");
		type = strtok(NULL, " \t\r\n");
		options = strtok(NULL, " \t\r\n");
		extra = strtok(NULL, " \t\r\n");
		if (source == NULL || target == NULL || type == NULL ||
		    options == NULL || extra != NULL) {
			fprintf(stderr, "mount: /etc/fstab:%u: invalid entry\n",
				line_number);
			failed = 1;
			continue;
		}
		if (mount_fstab_entry(source, target, type, options) != 0)
			failed = 1;
	}
	if (ferror(stream))
		failed = 1;
	if (fclose(stream) != 0)
		failed = 1;
	return failed;
}

static const char *
program_name(const char *path)
{
	const char *slash = strrchr(path != NULL ? path : "", '/');
	return slash != NULL ? slash + 1 : path;
}

static int
run_unmount(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: umount directory\n");
		return 2;
	}
	if (unmount(argv[1], 0) != 0) {
		fprintf(stderr, "umount: %s: %s\n", argv[1], strerror(errno));
		return 1;
	}
	return 0;
}

int
main(int argc, char **argv)
{
	struct mount_args arguments;
	const char *type = NULL, *source = NULL, *target = NULL;
	int flags = 0, i;

	if (strcmp(program_name(argv[0]), "umount") == 0)
		return run_unmount(argc, argv);
	if (argc == 2 && strcmp(argv[1], "-a") == 0)
		return mount_all();
	memset(&arguments, 0, sizeof(arguments));
	arguments.size = sizeof(arguments);
	arguments.version = ZEDBSD_MOUNT_ARGS_VERSION;
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-r") == 0) {
			flags |= MNT_RDONLY;
		} else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
			type = argv[++i];
		} else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
			const char *option = argv[++i];
			if (strcmp(option, "ro") == 0)
				flags |= MNT_RDONLY;
			else if (strcmp(option, "nosuid") == 0)
				flags |= MNT_NOSUID;
			else if (strncmp(option, "fspec=", 6) == 0)
				source = option + 6;
			else {
				fprintf(stderr,
					"mount: unsupported option: %s\n",
					option);
				return 2;
			}
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "mount: unknown option: %s\n", argv[i]);
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
	if (type == NULL || target == NULL) {
		fprintf(stderr,
			"usage: mount -t type [-r] [-o ro|nosuid|fspec=disk] "
			"[disk] directory\n");
		return 2;
	}
	if (source != NULL) {
		if (strncmp(source, "/dev/", 5) == 0)
			source += 5;
		if (strlen(source) >= sizeof(arguments.fspec)) {
			fprintf(stderr, "mount: device name is too long\n");
			return 2;
		}
		strcpy(arguments.fspec, source);
	}
	if (mount(type, target, flags, source != NULL ? &arguments : NULL) !=
	    0) {
		fprintf(stderr, "mount: %s on %s: %s\n",
			source != NULL ? source : type, target,
			strerror(errno));
		return 1;
	}
	return 0;
}
