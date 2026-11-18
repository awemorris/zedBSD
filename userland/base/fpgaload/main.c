/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The fpgaload command: writes a bitstream into the programmable logic
 * through /dev/fpga0, and shows, enables or disables the logic.
 *
 *   fpgaload FILE.bit|FILE.bin
 *   fpgaload --status | --enable | --disable
 *
 * A file is read whole, put in the order the configuration DMA takes
 * (bitstream.c), written to the device and programmed; the logic is
 * enabled by the program.  --enable is for a logic written another way,
 * over JTAG.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <uapi/fpga.h>

#include "bitstream.h"

/* The device, and the most bytes one write hands it. */
#define FPGA_DEVICE		"/dev/fpga0"
#define WRITE_CHUNK		(64U * 1024U)

static int show_status(int device);
static int read_file(const char *path, uint8_t **data, size_t *size);
static int load_file(int device, const char *path);
static int write_all(int device, const uint8_t *data, size_t size);
static int simple_request(int device, unsigned long request, const char *name);
static void usage(void);

/*
 * Runs the fpgaload command.
 */
int
main(
	int argc,
	char **argv)
{
	const char *argument;
	int is_status;
	int is_enable;
	int is_disable;
	int device;
	int status;

	/* Takes exactly one argument. */
	if (argc != 2) {
		usage();
		return 2;
	}

	/* Names what the argument asks for. */
	argument = argv[1];
	is_status = strcmp(argument, "--status") == 0;
	is_enable = strcmp(argument, "--enable") == 0;
	is_disable = strcmp(argument, "--disable") == 0;

	/* Opens the device; only root may. */
	device = open(FPGA_DEVICE, O_RDWR);
	if (device < 0) {
		fprintf(stderr, "fpgaload: %s: %s\n", FPGA_DEVICE, strerror(errno));
		return 1;
	}

	/* Selects what to do. */
	if (is_status) {
		status = show_status(device);
	} else if (is_enable) {
		status = simple_request(device, FPGA_ENABLE, "enable");
	} else if (is_disable) {
		status = simple_request(device, FPGA_DISABLE, "disable");
	} else if (argument[0] == '-') {
		usage();
		status = 2;
	} else {
		status = load_file(device, argument);
	}

	/* Lets the device go. */
	close(device);

	/* Reports how it went. */
	return status;
}

/* Says how the command is used. */
static void
usage(
	void)
{
	/* The forms the command takes. */
	fprintf(stderr, "usage: fpgaload FILE.bit|FILE.bin\n"
			"       fpgaload --status | --enable | --disable\n");
}

/* Shows the state of the logic.  Returns the command's exit status. */
static int
show_status(
	int device)
{
	struct fpga_status status;
	int error;

	/* Asks the device. */
	memset(&status, 0, sizeof(status));
	error = ioctl(device, FPGA_STATUS, &status);
	if (error != 0) {
		fprintf(stderr, "fpgaload: status: %s\n", strerror(errno));
		return 1;
	}

	/* One line, one name and value a field. */
	printf("DONE %u INIT %u ENABLED %u PROGRAMMING %u EMULATED %u error %u devcfg_status 0x%08x window 0x%08x+0x%x\n",
	       (status.flags & FPGA_STATUS_DONE) != 0 ? 1U : 0U,
	       (status.flags & FPGA_STATUS_INIT) != 0 ? 1U : 0U,
	       (status.flags & FPGA_STATUS_ENABLED) != 0 ? 1U : 0U,
	       (status.flags & FPGA_STATUS_PROGRAMMING) != 0 ? 1U : 0U,
	       (status.flags & FPGA_STATUS_EMULATED) != 0 ? 1U : 0U,
	       (unsigned)status.error,
	       (unsigned)status.devcfg_status,
	       (unsigned)status.window_base,
	       (unsigned)status.window_size);

	/* Succeeded: the state is shown. */
	return 0;
}

/* Asks the device for a request without an argument.  Returns the exit status. */
static int
simple_request(
	int device,
	unsigned long request,
	const char *name)
{
	int error;

	/* Sends the request. */
	error = ioctl(device, request);
	if (error != 0) {
		fprintf(stderr, "fpgaload: %s: %s\n", name, strerror(errno));
		return 1;
	}

	/* Succeeded: the device did as asked. */
	return 0;
}

/*
 * Reads a whole file into memory.  Returns 0, or an errno.  The caller
 * frees *data.
 */
static int
read_file(
	const char *path,
	uint8_t **data,
	size_t *size)
{
	FILE *stream;
	uint8_t *buffer;
	long length;
	size_t got;
	int error;

	/* Opens the file. */
	stream = fopen(path, "rb");
	if (stream == NULL)
		return errno;

	/* Finds its length at its end. */
	error = fseek(stream, 0, SEEK_END);
	if (error != 0) {
		error = errno;
		fclose(stream);
		return error;
	}

	/* Refuses an empty file, and one larger than any bitstream with its header. */
	length = ftell(stream);
	if (length <= 0 || (unsigned long)length > FPGA_BITSTREAM_MAX + WRITE_CHUNK) {
		fclose(stream);
		return EFBIG;
	}

	/* Goes back to the start for the read. */
	rewind(stream);

	/* The buffer the whole file goes in. */
	buffer = malloc((size_t)length);
	if (buffer == NULL) {
		fclose(stream);
		return ENOMEM;
	}

	/* Reads it whole; a short read is an error. */
	got = fread(buffer, 1, (size_t)length, stream);
	fclose(stream);
	if (got != (size_t)length) {
		free(buffer);
		return EIO;
	}

	/* Succeeded: the caller has the file. */
	*data = buffer;
	*size = (size_t)length;
	return 0;
}

/* Writes data to the device, a chunk at a time.  Returns 0, or an errno. */
static int
write_all(
	int device,
	const uint8_t *data,
	size_t size)
{
	size_t offset;
	size_t chunk;
	ssize_t written;

	/* Each chunk until the data is all written. */
	offset = 0;
	while (offset < size) {
		chunk = size - offset;
		if (chunk > WRITE_CHUNK)
			chunk = WRITE_CHUNK;
		written = write(device, data + offset, chunk);
		if (written < 0) {
			/* A signal only interrupts the write; anything else ends it. */
			if (errno == EINTR)
				continue;
			return errno;
		}

		/* A device that takes nothing would loop for ever. */
		if (written == 0)
			return EIO;

		/* The next chunk starts after what was taken. */
		offset += (size_t)written;
	}

	/* Succeeded: the device holds the data. */
	return 0;
}

/* Writes a file into the logic.  Returns the command's exit status. */
static int
load_file(
	int device,
	const char *path)
{
	uint8_t *file;
	uint8_t *data;
	size_t size;
	int error;

	/* Reads the file. */
	error = read_file(path, &file, &size);
	if (error != 0) {
		fprintf(stderr, "fpgaload: %s: %s\n", path, strerror(error));
		return 1;
	}

	/* Finds the configuration data and puts it in the DMA's order. */
	data = file;
	error = bitstream_prepare(&data, &size);
	if (error != 0) {
		fprintf(stderr, "fpgaload: %s: not a bitstream (no sync word, or a malformed header)\n", path);
		free(file);
		return 1;
	}

	/* Hands the data to the device. */
	error = write_all(device, data, size);
	free(file);
	if (error != 0) {
		fprintf(stderr, "fpgaload: write: %s\n", strerror(error));
		return 1;
	}

	/* Writes it into the logic, which the device then enables. */
	error = ioctl(device, FPGA_PROGRAM);
	if (error != 0) {
		fprintf(stderr, "fpgaload: program: %s\n", strerror(errno));
		return 1;
	}

	/* Says what the logic is now. */
	printf("fpgaload: %s: %zu bytes programmed\n", path, size);
	error = show_status(device);

	/* Reports a state that could not be read. */
	if (error != 0)
		return error;

	/* Succeeded: the logic holds the bitstream. */
	return 0;
}
