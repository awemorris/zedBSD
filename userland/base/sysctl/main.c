/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <zedbsd/sysctl.h>

#define NAME_MAX 64U

static int
show_name(const char *name)
{
	if (strcmp(name, "vfs.bufcache.stats") == 0) {
		struct bufcache_stats stats;
		size_t length = sizeof(stats);
		if (sysctlbyname(name, &stats, &length, NULL, 0) != 0)
			return -1;
		printf("%s: buffers=%llu hits=%llu misses=%llu read_bios=%llu "
		       "write_bios=%llu evictions=%llu waits=%llu "
		       "writeback_errors=%llu\n",
		       name, (unsigned long long)stats.buffers,
		       (unsigned long long)stats.hits,
		       (unsigned long long)stats.misses,
		       (unsigned long long)stats.read_bios,
		       (unsigned long long)stats.write_bios,
		       (unsigned long long)stats.evictions,
		       (unsigned long long)stats.waits,
		       (unsigned long long)stats.writeback_errors);
		return 0;
	} else {
		uint64_t value;
		size_t length = sizeof(value);
		if (sysctlbyname(name, &value, &length, NULL, 0) != 0)
			return -1;
		printf("%s: %llu\n", name, (unsigned long long)value);
		return 0;
	}
}
static int
show_all(void)
{
	static const char *const names[] = {
	    "vfs.bufcache.max_bytes",
	    "vfs.bufcache.current_bytes",
	    "vfs.bufcache.dirty_bytes",
	    "vfs.bufcache.stats",
	};
	unsigned i;
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (show_name(names[i]) != 0) {
			fprintf(stderr, "sysctl: %s: %s\n", names[i],
				strerror(errno));
			return 1;
		}
	return 0;
}

static int
set_name(const char *argument, const char *equal)
{
	char name[NAME_MAX];
	char *end;
	unsigned long long parsed;
	uint64_t value;
	size_t name_length = (size_t)(equal - argument);
	if (name_length == 0 || name_length >= sizeof(name) || equal[1] == '\0')
		return EINVAL;
	memcpy(name, argument, name_length);
	name[name_length] = '\0';
	errno = 0;
	parsed = strtoull(equal + 1, &end, 10);
	if (errno != 0 || *end != '\0')
		return EINVAL;
	value = (uint64_t)parsed;
	if ((unsigned long long)value != parsed)
		return ERANGE;
	if (sysctlbyname(name, NULL, NULL, &value, sizeof(value)) != 0)
		return errno;
	return show_name(name) == 0 ? 0 : errno;
}

int
main(int argc, char **argv)
{
	const char *equal;
	int error;
	if (argc == 2 && strcmp(argv[1], "-a") == 0)
		return show_all();
	if (argc != 2) {
		fprintf(stderr, "usage: sysctl name[=value] | sysctl -a\n");
		return 2;
	}
	equal = strchr(argv[1], '=');
	if (equal == NULL) {
		if (show_name(argv[1]) == 0)
			return 0;
		error = errno;
	} else {
		error = set_name(argv[1], equal);
	}
	fprintf(stderr, "sysctl: %s: %s\n", argv[1], strerror(error));
	return 1;
}
