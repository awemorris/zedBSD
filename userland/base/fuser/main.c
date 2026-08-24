/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <zedbsd/system.h>

static void
usage(void)
{
	fprintf(stderr, "usage: fuser [-cfu] file ...\n");
}

static void
print_flags(unsigned flags)
{
	if ((flags & ZEDBSD_SYSTEM_FILE_USAGE_CWD) != 0)
		putchar('c');
	if ((flags & ZEDBSD_SYSTEM_FILE_USAGE_ROOT) != 0)
		putchar('r');
	if ((flags & ZEDBSD_SYSTEM_FILE_USAGE_EXECUTABLE) != 0)
		putchar('e');
	if ((flags & ZEDBSD_SYSTEM_FILE_USAGE_OPEN) != 0)
		putchar('f');
	if ((flags & ZEDBSD_SYSTEM_FILE_USAGE_MAPPED) != 0)
		putchar('m');
	if ((flags & ZEDBSD_SYSTEM_FILE_USAGE_SOCKET) != 0)
		putchar('s');
}

static int
show_file(int descriptor, const char *path, int mount_query, int show_user)
{
	struct system_file_usage query;
	int found = 0;

	memset(&query, 0, sizeof(query));
	query.version = ZEDBSD_SYSTEM_FILE_USAGE_VERSION;
	query.struct_size = sizeof(query);
	query.cursor_pid = -1;
	query.query_flags =
	    mount_query ? ZEDBSD_SYSTEM_FILE_USAGE_QUERY_MOUNT : 0;
	if (strlen(path) >= sizeof(query.path)) {
		fprintf(stderr, "fuser: %s: path is too long\n", path);
		return -1;
	}
	strcpy(query.path, path);
	printf("%s:", path);
	for (;;) {
		if (ioctl(descriptor, ZEDBSD_SYSTEM_GET_FILE_USAGE, &query) !=
		    0) {
			if (errno == ENOENT)
				break;
			fprintf(stderr, "fuser: %s: %s\n", path,
				strerror(errno));
			putchar('\n');
			return -1;
		}
		printf(" %d", query.pid);
		print_flags(query.usage_flags);
		if (show_user) {
			struct passwd *account = getpwuid(query.uid);
			if (account != NULL)
				printf("(%s)", account->pw_name);
			else
				printf("(%u)", query.uid);
		}
		found = 1;
	}
	putchar('\n');
	return found;
}

int
main(int argc, char **argv)
{
	int descriptor, option, mount_query = 0, show_user = 0;
	int found = 0, failed = 0, index;

	while ((option = getopt(argc, argv, "cfu")) != -1) {
		switch (option) {
		case 'c':
			mount_query = 1;
			break;
		case 'f':
			break;
		case 'u':
			show_user = 1;
			break;
		default:
			usage();
			return 2;
		}
	}
	if (optind == argc) {
		usage();
		return 2;
	}
	descriptor = open("/dev/system", O_RDONLY);
	if (descriptor < 0) {
		fprintf(stderr, "fuser: /dev/system: %s\n", strerror(errno));
		return 1;
	}
	for (index = optind; index < argc; index++) {
		int result =
		    show_file(descriptor, argv[index], mount_query, show_user);
		if (result > 0)
			found = 1;
		else if (result < 0)
			failed = 1;
	}
	(void)close(descriptor);
	return failed || !found;
}
