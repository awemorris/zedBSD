/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/archive.h"
#include "userland/base/common/elf_symbols.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct options {
	int prefix;
	int external;
	int numeric;
	int no_sort;
	int reverse;
	int portable;
	int undefined;
	int dynamic;
	char radix;
};

static struct options options = {.radix = 'x'};

static int
compare_name(const void *left, const void *right)
{
	const struct elf_symbol_record *a = left, *b = right;
	int result;
	if (options.numeric) {
		if (a->value < b->value)
			result = -1;
		else if (a->value > b->value)
			result = 1;
		else
			result = strcmp(a->name, b->name);
	} else
		result = strcmp(a->name, b->name);
	return options.reverse ? -result : result;
}

static void
print_value(uint64_t value, unsigned bits)
{
	if (options.radix == 'd')
		printf("%0*llu", bits == 64 ? 20 : 10,
		       (unsigned long long)value);
	else if (options.radix == 'o')
		printf("%0*llo", bits == 64 ? 22 : 11,
		       (unsigned long long)value);
	else
		printf("%0*llx", bits == 64 ? 16 : 8,
		       (unsigned long long)value);
}

static int
display_object(const void *data, size_t size, const char *file,
	       const char *member, int multiple)
{
	struct elf_symbol_table table;
	int result = 0;
	if (elf_symbols_read(data, size, options.dynamic, &table)) {
		fprintf(stderr, "nm: %s%s%s: %s\n", file, member ? "(" : "",
			member ? member : "",
			errno == ENOENT ? "no symbols" : "invalid object");
		return 1;
	}
	if (!options.no_sort && table.count > 1)
		qsort(table.symbols, table.count, sizeof(*table.symbols),
		      compare_name);
	if (multiple && !options.prefix && !options.portable)
		printf("\n%s%s%s%s:\n", file, member ? "(" : "",
		       member ? member : "", member ? ")" : "");
	for (size_t i = 0; i < table.count; i++) {
		const struct elf_symbol_record *symbol = &table.symbols[i];
		if (!*symbol->name ||
		    (options.external && symbol->binding == 0) ||
		    (options.undefined && symbol->section != 0))
			continue;
		if (options.prefix)
			printf("%s%s%s%s: ", file, member ? "(" : "",
			       member ? member : "", member ? ")" : "");
		if (options.portable) {
			printf("%s %c ", symbol->name, symbol->letter);
			print_value(symbol->value, table.bits);
			putchar(' ');
			print_value(symbol->size, table.bits);
			putchar('\n');
		} else {
			if (symbol->section == 0)
				printf("%*s", table.bits == 64 ? 16 : 8, "");
			else
				print_value(symbol->value, table.bits);
			printf(" %c %s\n", symbol->letter, symbol->name);
		}
	}
	elf_symbols_free(&table);
	return result;
}

static int
process_file(const char *path, int multiple)
{
	struct stat st;
	unsigned char *data = NULL;
	size_t done = 0;
	int fd = open(path, O_RDONLY);
	int result = 0;
	if (fd < 0 || fstat(fd, &st) || st.st_size < 0 ||
	    (uintmax_t)st.st_size > SIZE_MAX) {
		fprintf(stderr, "nm: %s: %s\n", path, strerror(errno));
		if (fd >= 0)
			close(fd);
		return 1;
	}
	data = malloc(st.st_size ? (size_t)st.st_size : 1);
	if (!data) {
		close(fd);
		return 1;
	}
	while (done < (size_t)st.st_size) {
		ssize_t n = read(fd, data + done, (size_t)st.st_size - done);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			fprintf(stderr, "nm: %s: short read\n", path);
			free(data);
			close(fd);
			return 1;
		}
		done += (size_t)n;
	}
	close(fd);
	if (done >= 8 && !memcmp(data, "!<arch>\n", 8)) {
		struct archive_file archive;
		if (archive_read_memory(data, done, &archive)) {
			fprintf(stderr, "nm: %s: invalid archive\n", path);
			result = 1;
		} else {
			for (size_t i = 0; i < archive.count; i++)
				if (!archive.members[i].special)
					result |= display_object(
					    archive.members[i].data,
					    archive.members[i].size, path,
					    archive.members[i].name, 1);
			archive_free(&archive);
		}
	} else
		result = display_object(data, done, path, NULL, multiple);
	free(data);
	return result;
}

static void
usage(void)
{
	fprintf(stderr, "usage: nm [-APglnopruv] [-t d|o|x] [file ...]\n");
}

int
main(int argc, char **argv)
{
	int ch, failed = 0;
	while ((ch = getopt(argc, argv, "ADAPglnopruvt:")) != -1) {
		switch (ch) {
		case 'A':
		case 'o':
			options.prefix = 1;
			break;
		case 'D':
			options.dynamic = 1;
			break;
		case 'g':
			options.external = 1;
			break;
		case 'l':
			break;
		case 'n':
		case 'v':
			options.numeric = 1;
			break;
		case 'p':
			options.no_sort = 1;
			break;
		case 'P':
			options.portable = 1;
			break;
		case 'r':
			options.reverse = 1;
			break;
		case 'u':
			options.undefined = 1;
			break;
		case 't':
			if (strlen(optarg) != 1 || !strchr("dox", optarg[0])) {
				usage();
				return 2;
			}
			options.radix = optarg[0];
			break;
		default:
			usage();
			return 2;
		}
	}
	if (optind == argc)
		return process_file("a.out", 0);
	for (int i = optind; i < argc; i++)
		failed |= process_file(argv[i], argc - optind > 1);
	return failed;
}
