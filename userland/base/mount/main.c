/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>

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
			else if (strncmp(option, "fspec=", 6) == 0)
				source = option + 6;
			else {
				fprintf(stderr, "mount: unsupported option: %s\n",
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
		fprintf(stderr, "usage: mount -t type [-r] [-o ro|fspec=disk] "
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
	if (mount(type, target, flags,
	    source != NULL ? &arguments : NULL) != 0) {
		fprintf(stderr, "mount: %s on %s: %s\n",
		    source != NULL ? source : type, target, strerror(errno));
		return 1;
	}
	return 0;
}
