/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ELFCLASS32 1
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ELFDATA2MSB 2
#define EM_386 3
#define EM_68K 4
#define EM_SPARCV9 43
#define EM_X86_64 62
#define EM_AARCH64 183

static const char *
machine_name(unsigned value)
{
	switch (value) {
	case EM_386:
		return "Intel 80386";
	case EM_X86_64:
		return "AMD x86-64";
	case EM_AARCH64:
		return "AArch64";
	case EM_SPARCV9:
		return "SPARC V9";
	case EM_68K:
		return "Motorola 68000";
	default:
		return "unknown machine";
	}
}
static unsigned
word(const unsigned char *p, int little)
{
	return little ? (unsigned)p[0] | (unsigned)p[1] << 8
		      : (unsigned)p[0] << 8 | (unsigned)p[1];
}
static int
text_data(const unsigned char *p, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++)
		if (p[i] != 0 && p[i] != '\n' && p[i] != '\r' && p[i] != '\t' &&
		    !isprint(p[i]))
			return 0;
	return 1;
}
static int
classify(const char *path, int follow)
{
	struct stat st;
	unsigned char data[512];
	ssize_t n;
	int fd;
	if ((follow ? stat(path, &st) : lstat(path, &st)) != 0) {
		command_error("file", path);
		return 0;
	}
	printf("%s: ", path);
	if (S_ISDIR(st.st_mode)) {
		puts("directory");
		return 1;
	}
	if (S_ISLNK(st.st_mode)) {
		char target[512];
		n = readlink(path, (char *)target, sizeof(target) - 1U);
		if (n < 0) {
			command_error("file", path);
			return 0;
		}
		target[n] = '\0';
		printf("symbolic link to %s\n", target);
		return 1;
	}
	if (S_ISCHR(st.st_mode)) {
		puts("character special");
		return 1;
	}
	if (S_ISBLK(st.st_mode)) {
		puts("block special");
		return 1;
	}
	if (S_ISFIFO(st.st_mode)) {
		puts("fifo");
		return 1;
	}
	if (S_ISSOCK(st.st_mode)) {
		puts("socket");
		return 1;
	}
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		command_error("file", path);
		return 0;
	}
	do
		n = read(fd, data, sizeof(data));
	while (n < 0 && errno == EINTR);
	close(fd);
	if (n < 0) {
		command_error("file", path);
		return 0;
	}
	if (n == 0) {
		puts("empty");
		return 1;
	}
	if (n >= 20 && data[0] == 0x7f && data[1] == 'E' && data[2] == 'L' &&
	    data[3] == 'F' &&
	    (data[4] == ELFCLASS32 || data[4] == ELFCLASS64) &&
	    (data[5] == ELFDATA2LSB || data[5] == ELFDATA2MSB)) {
		int little = data[5] == ELFDATA2LSB;
		printf("ELF %s-bit %s-endian, %s\n",
		       data[4] == ELFCLASS32 ? "32" : "64",
		       little ? "little" : "big",
		       machine_name(word(data + 18, little)));
		return 1;
	}
	if (n >= 2 && data[0] == '#' && data[1] == '!') {
		size_t i = 2;
		while (i < (size_t)n && (data[i] == ' ' || data[i] == '\t'))
			i++;
		printf("script text executable for ");
		while (i < (size_t)n && data[i] != '\n' && data[i] != '\r')
			putchar(data[i++]);
		putchar('\n');
		return 1;
	}
	if (n >= 2 && data[0] == 'B' && data[1] == 'M') {
		puts("BMP image data");
		return 1;
	}
	if (text_data(data, (size_t)n)) {
		puts("text");
		return 1;
	}
	puts("data");
	return 1;
}
int
main(int argc, char **argv)
{
	int follow = 0, index = 1, failed = 0;
	if (index < argc && !strcmp(argv[index], "-L")) {
		follow = 1;
		index++;
	}
	if (index < argc && !strcmp(argv[index], "--"))
		index++;
	if (index == argc) {
		fprintf(stderr, "usage: file [-L] file...\n");
		return 1;
	}
	for (; index < argc; index++)
		if (!classify(argv[index], follow))
			failed = 1;
	return failed;
}
