/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/sh/applet.h"

#include <zedbsd/applet.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define APPLET_ADDRESS ((void *)0x00050000U)
#define APPLET_MAX_SIZE 0x00010000U

static uint32_t
crc32_image(const uint8_t *bytes, uint32_t length)
{
	uint32_t crc = 0xffffffffU;
	uint32_t i;
	for (i = 0; i < length; i++) {
		uint8_t byte = i >= 16U && i < 20U ? 0 : bytes[i];
		int bit;
		crc ^= byte;
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^
			    ((0U - (crc & 1U)) & 0xedb88320U);
	}
	return ~crc;
}

static void service_putc(char c) { (void)write(1, &c, 1); }
static void service_puts(const char *s)
{
	while (s != NULL && *s != '\0') service_putc(*s++);
}
static uint32_t service_key_read(void)
{
	unsigned char byte;
	return read(0, &byte, 1) == 1 ? byte : UINT32_MAX;
}

int
sh_run_applet(const char *path, int argc, char **argv)
{
	struct stat status;
	struct zedbsd_applet_header *header;
	struct zedbsd_applet_services services = {
		1, sizeof(services), service_putc, service_puts, service_key_read
	};
	size_t image_size, mapping_size;
	uint8_t *image;
	ssize_t count;
	off_t offset = 0;
	uint32_t result;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd < 0 || fstat(fd, &status) != 0) {
		if (fd >= 0) close(fd);
		return 0;
	}
	if (status.st_size < (off_t)sizeof(*header) ||
	    status.st_size > (off_t)APPLET_MAX_SIZE) {
		close(fd);
		return 0;
	}
	image_size = (size_t)status.st_size;
	mapping_size = (image_size + 4095U) & ~4095U;
	image = mmap(APPLET_ADDRESS, mapping_size, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (image == MAP_FAILED) {
		close(fd);
		return 0;
	}
	while ((size_t)offset < image_size) {
		count = read(fd, image + offset, image_size - (size_t)offset);
		if (count <= 0) break;
		offset += count;
	}
	close(fd);
	header = (struct zedbsd_applet_header *)image;
	if ((size_t)offset != image_size ||
	    header->magic != ZEDBSD_APPLET_MAGIC || header->abi_version != 1 ||
	    header->header_size != sizeof(*header) || header->flags != 0 ||
	    header->image_size != image_size ||
	    header->entry_offset < header->header_size ||
	    header->entry_offset >= image_size ||
	    crc32_image(image, (uint32_t)image_size) != header->crc32 ||
	    mprotect(image, mapping_size, PROT_READ | PROT_EXEC) != 0) {
		munmap(image, mapping_size);
		return 0;
	}
	result = ((zedbsd_applet_entry_t)(image + header->entry_offset))(
	    &services, (uint32_t)argc, (const char *const *)argv);
	if (munmap(image, mapping_size) != 0)
		return 0;
	if (result != 0) {
		fprintf(stderr, "applet status %u\n", result);
		return 0;
	}
	return 1;
}
