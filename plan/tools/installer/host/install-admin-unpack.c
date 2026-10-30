/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
/*
 * The host driver of install-admin's index and unpacking (plan/tools/installer/host/install-admin-unpack-test.sh):
 *   install-admin-unpack unpack INDEX UNIT TARBALL ROOT   -> "RESULT errno entries"
 *   install-admin-unpack closure INDEX UNIT...            -> "ORDER unit unit ..." or "RESULT errno"
 *   install-admin-unpack digest INDEX HEX                 -> "RESULT errno"
 */
#include "userland/desktop/installer/admin/index.h"
#include "userland/desktop/installer/admin/unpack.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char text[INDEX_BYTES_MAX];
static struct installer_index index_read;

static size_t read_index(const char *path)
{
	FILE *file = fopen(path, "rb");
	size_t size;

	if (file == NULL) {
		perror(path);
		exit(2);
	}
	size = fread(text, 1, sizeof(text), file);
	fclose(file);
	return size;
}

static int count_entry(void *context, const char *path, enum unpack_kind kind, uint64_t bytes)
{
	(void)path;
	(void)kind;
	(void)bytes;
	(*(unsigned *)context)++;
	return 0;
}

int main(int argc, char **argv)
{
	unsigned chosen[INDEX_UNITS_MAX], order[INDEX_UNITS_MAX], order_count, slot, entries = 0;
	uint8_t *data, *tar;
	size_t data_length, tar_length, length;
	int error, fd, root, i;

	if (argc < 3)
		return 2;
	length = read_index(argv[2]);
	if (strcmp(argv[1], "digest") == 0) {
		printf("RESULT %d\n", index_digest_matches(text, length, argv[3]));
		return 0;
	}
	error = index_parse(text, length, &index_read);
	if (error != 0) {
		printf("RESULT %d parse\n", error);
		return 0;
	}
	if (strcmp(argv[1], "closure") == 0) {
		for (i = 3; i < argc; i++) {
			if (index_find_unit(&index_read, argv[i], &chosen[i - 3]) != 0) {
				printf("RESULT %d find\n", ENOENT);
				return 0;
			}
		}
		error = index_closure(&index_read, chosen, (unsigned)(argc - 3), order, &order_count);
		if (error != 0) {
			printf("RESULT %d\n", error);
			return 0;
		}
		printf("ORDER");
		for (slot = 0; slot < order_count; slot++)
			printf(" %s", index_read.unit[order[slot]].name);
		printf("\n");
		return 0;
	}
	if (strcmp(argv[1], "unpack") != 0 || argc != 6)
		return 2;
	error = index_find_unit(&index_read, argv[3], &slot);
	if (error != 0) {
		printf("RESULT %d find\n", error);
		return 0;
	}
	fd = open(argv[4], O_RDONLY);
	root = open(argv[5], O_RDONLY | O_DIRECTORY);
	if (fd < 0 || root < 0) {
		perror("open");
		return 2;
	}
	error = unpack_read_whole(fd, index_read.unit[slot].tar_bytes, index_read.unit[slot].sha256, &data, &data_length);
	if (error != 0) {
		printf("RESULT %d read\n", error);
		return 0;
	}
	error = unpack_gunzip(data, data_length, UINT64_C(4) << 30, &tar, &tar_length);
	free(data);
	if (error != 0) {
		printf("RESULT %d gunzip\n", error);
		return 0;
	}
	error = unpack_tar(root, tar, tar_length, &index_read, slot, count_entry, &entries);
	free(tar);
	printf("RESULT %d %u\n", error, entries);
	return 0;
}
