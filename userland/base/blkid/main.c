/* blkid - print block-device attributes.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <zedbsd/blkid.h>

static int identify(const char *path)
{
	struct block_identity id;
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "blkid: %s: %s\n", path, strerror(errno));
		return 1;
	}
	if (ioctl(fd, BLKGETIDENTITY, &id) != 0) {
		int error = errno;
		close(fd);
		if (error == ENOENT || error == ENOTTY || error == EOPNOTSUPP ||
		    error == ENXIO) return 0;
		fprintf(stderr, "blkid: %s: %s\n", path, strerror(error));
		return 1;
	}
	close(fd);
	printf("%s:", path);
	if (id.flags & ZEDBSD_BLKID_LABEL) printf(" LABEL=\"%s\"", id.label);
	if (id.flags & ZEDBSD_BLKID_UUID) printf(" UUID=\"%s\"", id.uuid);
	if (id.flags & ZEDBSD_BLKID_TYPE) printf(" TYPE=\"%s\"", id.type);
	if (id.flags & ZEDBSD_BLKID_PARTLABEL)
		printf(" PARTLABEL=\"%s\"", id.partlabel);
	if (id.flags & ZEDBSD_BLKID_PARTUUID)
		printf(" PARTUUID=\"%s\"", id.partuuid);
	putchar('\n');
	return 0;
}

int main(int argc, char **argv)
{
	int status = 0, index;
	if (argc == 1) {
		fprintf(stderr, "usage: blkid device ...\n");
		return 2;
	}
	for (index = 1; index < argc; index++) status |= identify(argv[index]);
	return status;
}
