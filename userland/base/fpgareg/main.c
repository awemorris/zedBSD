/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The fpgareg command: reads and writes the registers of the block the
 * programmable logic holds, through /dev/fpga0, and waits for its
 * interrupt.
 *
 *   fpgareg read OFFSET
 *   fpgareg write OFFSET VALUE
 *   fpgareg wait [--ack OFFSET] [TIMEOUT_MS]
 *
 * Offsets and values are numbers as strtoul takes them (0x for hex).  The
 * command knows no block's registers: wait reads the status register named
 * by --ack (0x18 unless given, the plio block's IRQ_STATUS), shows it, and
 * writes it back to clear it, so that the next wait does not find the
 * line still raised.
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

/* The device, the status register wait clears unless told, and its default timeout. */
#define FPGA_DEVICE		"/dev/fpga0"
#define DEFAULT_ACK_OFFSET	0x18U
#define DEFAULT_TIMEOUT_MS	1000U

static int parse_number(const char *text, uint32_t *value);
static int read_register(int device, uint32_t offset, uint32_t *value);
static int command_read(int device, int argc, char **argv);
static int command_write(int device, int argc, char **argv);
static int command_wait(int device, int argc, char **argv);
static void usage(void);

/*
 * Runs the fpgareg command.
 */
int
main(
	int argc,
	char **argv)
{
	int is_read;
	int is_write;
	int is_wait;
	int device;
	int status;

	/* Takes a subcommand. */
	if (argc < 2) {
		usage();
		return 2;
	}

	/* Names the subcommand. */
	is_read = strcmp(argv[1], "read") == 0;
	is_write = strcmp(argv[1], "write") == 0;
	is_wait = strcmp(argv[1], "wait") == 0;

	/* Opens the device; only root may. */
	device = open(FPGA_DEVICE, O_RDWR);
	if (device < 0) {
		fprintf(stderr, "fpgareg: %s: %s\n", FPGA_DEVICE, strerror(errno));
		return 1;
	}

	/* Selects the subcommand. */
	if (is_read) {
		status = command_read(device, argc - 2, argv + 2);
	} else if (is_write) {
		status = command_write(device, argc - 2, argv + 2);
	} else if (is_wait) {
		status = command_wait(device, argc - 2, argv + 2);
	} else {
		usage();
		status = 2;
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
	fprintf(stderr, "usage: fpgareg read OFFSET\n"
			"       fpgareg write OFFSET VALUE\n"
			"       fpgareg wait [--ack OFFSET] [TIMEOUT_MS]\n");
}

/* Reads a 32-bit number.  Returns 0, or EINVAL for anything else. */
static int
parse_number(
	const char *text,
	uint32_t *value)
{
	unsigned long number;
	char *end;

	/* Reads the whole text as one number that fits in 32 bits. */
	errno = 0;
	number = strtoul(text, &end, 0);
	if (errno != 0 || end == text || *end != '\0')
		return EINVAL;
	if (number > 0xffffffffUL)
		return EINVAL;

	/* Succeeded: the number. */
	*value = (uint32_t)number;
	return 0;
}

/* Reads one register.  Returns 0, or the errno the device gave. */
static int
read_register(
	int device,
	uint32_t offset,
	uint32_t *value)
{
	struct fpga_reg reg;
	int error;

	/* Asks the device for the register. */
	reg.offset = offset;
	reg.value = 0;
	error = ioctl(device, FPGA_REG_READ, &reg);
	if (error != 0)
		return errno;

	/* Succeeded: the register's value. */
	*value = reg.value;
	return 0;
}

/* fpgareg read OFFSET.  Returns the exit status. */
static int
command_read(
	int device,
	int argc,
	char **argv)
{
	uint32_t offset;
	uint32_t value;
	int error;

	/* Takes the offset. */
	if (argc != 1) {
		usage();
		return 2;
	}

	/* Refuses an offset that is not a number. */
	error = parse_number(argv[0], &offset);
	if (error != 0) {
		usage();
		return 2;
	}

	/* Reads the register. */
	error = read_register(device, offset, &value);
	if (error != 0) {
		fprintf(stderr, "fpgareg: read 0x%x: %s\n", (unsigned)offset, strerror(error));
		return 1;
	}

	/* Shows the value in hex, a whole word. */
	printf("0x%08x\n", (unsigned)value);

	/* Succeeded: the value is shown. */
	return 0;
}

/* fpgareg write OFFSET VALUE.  Returns the exit status. */
static int
command_write(
	int device,
	int argc,
	char **argv)
{
	struct fpga_reg reg;
	int error;

	/* Takes the offset and the value. */
	if (argc != 2) {
		usage();
		return 2;
	}

	/* Refuses an offset that is not a number. */
	error = parse_number(argv[0], &reg.offset);
	if (error != 0) {
		usage();
		return 2;
	}

	/* Refuses a value that is not a number. */
	error = parse_number(argv[1], &reg.value);
	if (error != 0) {
		usage();
		return 2;
	}

	/* Writes the register. */
	error = ioctl(device, FPGA_REG_WRITE, &reg);
	if (error != 0) {
		fprintf(stderr, "fpgareg: write 0x%x: %s\n", (unsigned)reg.offset, strerror(errno));
		return 1;
	}

	/* Succeeded: the register holds the value. */
	return 0;
}

/* fpgareg wait [--ack OFFSET] [TIMEOUT_MS].  Returns the exit status. */
static int
command_wait(
	int device,
	int argc,
	char **argv)
{
	struct fpga_irq_wait request;
	struct fpga_reg reg;
	uint32_t ack_offset;
	uint32_t status;
	int has_ack;
	int index;
	int error;

	/* Starts from the defaults: the plio block's status register, one second. */
	ack_offset = DEFAULT_ACK_OFFSET;
	request.timeout_ms = DEFAULT_TIMEOUT_MS;
	request.count = 0;
	index = 0;

	/* Takes --ack OFFSET when it comes first. */
	has_ack = 0;
	if (argc >= 2)
		has_ack = strcmp(argv[0], "--ack") == 0;
	if (has_ack) {
		error = parse_number(argv[index + 1], &ack_offset);
		if (error != 0) {
			usage();
			return 2;
		}

		/* The timeout, if any, follows the option. */
		index += 2;
	}

	/* Takes the timeout when one is left. */
	if (index < argc) {
		error = parse_number(argv[index], &request.timeout_ms);
		if (error != 0) {
			usage();
			return 2;
		}

		/* Nothing may follow the timeout. */
		index++;
	}

	/* Refuses anything left over. */
	if (index != argc) {
		usage();
		return 2;
	}

	/* Waits for the interrupt. */
	error = ioctl(device, FPGA_IRQ_WAIT, &request);
	if (error != 0) {
		fprintf(stderr, "fpgareg: wait: %s\n", strerror(errno));
		return 1;
	}

	/* Reads the block's status. */
	error = read_register(device, ack_offset, &status);
	if (error != 0) {
		fprintf(stderr, "fpgareg: read 0x%x: %s\n", (unsigned)ack_offset, strerror(error));
		return 1;
	}

	/* Writes it back to clear what it shows, which lowers the line. */
	reg.offset = ack_offset;
	reg.value = status;
	error = ioctl(device, FPGA_REG_WRITE, &reg);
	if (error != 0) {
		fprintf(stderr, "fpgareg: write 0x%x: %s\n", (unsigned)ack_offset, strerror(errno));
		return 1;
	}

	/* Shows how many interrupts came and what the block said. */
	printf("irq count %u status 0x%08x\n", (unsigned)request.count, (unsigned)status);

	/* Succeeded: the interrupt came and is cleared. */
	return 0;
}
