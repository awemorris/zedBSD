/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * WS048 p003: host test of the Raspberry Pi 4 firmware property client.
 *
 * The production src/drivers/platform/rpi4/rpi4-firmware.c is compiled
 * unchanged against a model of the BCM2835 mailbox and of the VideoCore's
 * property handling.  The model checks that the request buffer is cleaned
 * before it is posted, that the posted word is the buffer's uncached bus
 * address with the property channel, and that the request is laid out as
 * the firmware expects; it then writes an answer the scenario chooses.
 *
 * Usage: firmware-host-test FIRMWARE.dtb
 */

#include "drivers/platform/rpi4/rpi4-firmware.h"
#include <kern/clock.h>
#include <kern/dcache.h>
#include <kern/device-io.h>
#include <kern/irq.h>
#include <kern/klog.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;

#define CHECK(expression)                                                   \
	do {                                                                 \
		checks++;                                                    \
		if (!(expression)) {                                        \
			fprintf(stderr, "ws048-firmware: failed at %s:%d: %s\n", \
			    __FILE__, __LINE__, #expression);                  \
			exit(EXIT_FAILURE);                                   \
		}                                                            \
	} while (0)

#define MAILBOX_BASE		0xfe00b880ULL
#define BUFFER_PHYSICAL		0x00123000ULL

/* How the modelled firmware answers. */
enum answer_kind {
	ANSWER_SUCCESS,
	ANSWER_REFUSED,
	ANSWER_UNKNOWN_TAG,
	ANSWER_NEVER,
	ANSWER_OTHER_CHANNEL_FIRST,
	ANSWER_MAILBOX_FULL
};

static enum answer_kind answer_kind;
static uint32_t mailbox_registers[0x40U / 4U];
static uint32_t buffer[4096U / 4U] __attribute__((aligned(4096)));
static bool buffer_clean;
static bool buffer_invalidated;
static uint32_t posted;
static uint32_t pending[4];
static unsigned pending_count;
static uint32_t seen_request[16];
static unsigned polls;
static int irq_depth;

static void answer(uint32_t request);
static unsigned char *read_file(const char *path, size_t *size);

int
main(
	int argc,
	char **argv)
{
	struct drv_pci_address address;
	struct drv_fdt fdt;
	unsigned char *blob;
	size_t size;
	uint32_t values[DRV_RPI4_FIRMWARE_MAX_VALUES];
	uint32_t answered;

	if (argc != 2) {
		fprintf(stderr, "usage: %s FIRMWARE.dtb\n", argv[0]);
		return EXIT_FAILURE;
	}

	/* Nothing can be sent before the mailbox is known. */
	CHECK(drv_rpi4_firmware_property(1U, values, 0, 1U, &answered) == ENODEV);

	blob = read_file(argv[1], &size);
	CHECK(drv_fdt_open(&fdt, blob, size) == 0);
	CHECK(drv_rpi4_firmware_init(&fdt) == 0);
	CHECK(drv_rpi4_firmware_init(&fdt) == 0);

	/* The VL805 notification: bus 1, device 0, function 0. */
	answer_kind = ANSWER_SUCCESS;
	memset(&address, 0, sizeof(address));
	address.bus = 1U;
	CHECK(drv_rpi4_firmware_notify_xhci_reset(&address) == 0);
	CHECK(posted == ((uint32_t)BUFFER_PHYSICAL | 0xc0000000U | 8U));
	CHECK(seen_request[0] == 32U);
	CHECK(seen_request[1] == 0);
	CHECK(seen_request[2] == 0x00030058U);
	CHECK(seen_request[3] == 4U);
	CHECK(seen_request[4] == 0);
	CHECK(seen_request[5] == 0x00100000U);
	CHECK(seen_request[6] == 0);
	CHECK(buffer_invalidated);
	CHECK(irq_depth == 0);

	/* Another address encodes device and function too. */
	address.bus = 2U;
	address.device = 3U;
	address.function = 1U;
	CHECK(drv_rpi4_firmware_notify_xhci_reset(&address) == 0);
	CHECK(seen_request[5] == ((2U << 20) | (3U << 15) | (1U << 12)));

	/* A tag with a larger value buffer, and an answer copied back. */
	values[0] = 0x11U;
	values[1] = 0x22U;
	CHECK(drv_rpi4_firmware_property(0x00010004U, values, 2U, 3U, &answered) == 0);
	CHECK(seen_request[0] == 48U);
	CHECK(seen_request[3] == 12U);
	CHECK(seen_request[5] == 0x11U);
	CHECK(seen_request[6] == 0x22U);
	CHECK(seen_request[7] == 0);
	CHECK(seen_request[8] == 0);
	CHECK(answered == 12U);
	CHECK(values[0] == 0xa0U);
	CHECK(values[1] == 0xa1U);
	CHECK(values[2] == 0xa2U);

	/* Display ownership ends with no value buffer and no invented value word. */
	CHECK(drv_rpi4_firmware_property(0x00030066U, NULL, 0, 0, &answered) == 0);
	CHECK(seen_request[0] == 32U);
	CHECK(seen_request[2] == 0x00030066U);
	CHECK(seen_request[3] == 0);
	CHECK(seen_request[4] == 0);
	CHECK(seen_request[5] == 0);
	CHECK(answered == 0);
	CHECK(buffer_invalidated && irq_depth == 0);

	/* The firmware refuses the request. */
	answer_kind = ANSWER_REFUSED;
	CHECK(drv_rpi4_firmware_notify_xhci_reset(&address) == EIO);

	/* The firmware does not know the tag. */
	answer_kind = ANSWER_UNKNOWN_TAG;
	CHECK(drv_rpi4_firmware_notify_xhci_reset(&address) == ENOTSUP);

	/* A word for another channel comes back first and is dropped. */
	answer_kind = ANSWER_OTHER_CHANNEL_FIRST;
	CHECK(drv_rpi4_firmware_notify_xhci_reset(&address) == 0);

	/* The firmware never answers: the wait gives up after about a second. */
	answer_kind = ANSWER_NEVER;
	polls = 0;
	CHECK(drv_rpi4_firmware_notify_xhci_reset(&address) == ETIMEDOUT);
	CHECK(polls >= 100000U);
	CHECK(irq_depth == 0);

	/* Mailbox 1 stays full. */
	answer_kind = ANSWER_MAILBOX_FULL;
	CHECK(drv_rpi4_firmware_notify_xhci_reset(&address) == ETIMEDOUT);

	/* Malformed requests. */
	CHECK(drv_rpi4_firmware_property(1U, values, 2U, 1U, &answered) == EINVAL);
	CHECK(drv_rpi4_firmware_property(1U, NULL, 0, 1U, &answered) == EINVAL);
	CHECK(drv_rpi4_firmware_property(1U, NULL, 1U, 0, &answered) == EINVAL);
	CHECK(drv_rpi4_firmware_property(1U, NULL, 0, 0, NULL) == EINVAL);
	CHECK(drv_rpi4_firmware_property(1U, values, 0, DRV_RPI4_FIRMWARE_MAX_VALUES + 1U, &answered) == EINVAL);
	CHECK(drv_rpi4_firmware_notify_xhci_reset(NULL) == EINVAL);

	free(blob);
	printf("ws048-firmware: %u checks passed\n", checks);
	return EXIT_SUCCESS;
}

/* Plays the VideoCore: reads the request and writes the answer the scenario asks for. */
static void
answer(
	uint32_t request)
{
	unsigned words;

	CHECK(buffer_clean);
	CHECK((request & 0xfU) == 8U);
	CHECK((request & 0xc0000000U) == 0xc0000000U);
	CHECK((request & 0x3ffffff0U) == (uint32_t)BUFFER_PHYSICAL);
	words = buffer[0] / 4U;
	CHECK(words <= 16U);
	CHECK((buffer[0] % 16U) == 0);
	memcpy(seen_request, buffer, words * 4U);
	buffer_clean = false;
	buffer_invalidated = false;

	pending_count = 0;
	if (answer_kind == ANSWER_NEVER || answer_kind == ANSWER_MAILBOX_FULL)
		return;
	if (answer_kind == ANSWER_OTHER_CHANNEL_FIRST)
		pending[pending_count++] = 0x12345671U;

	if (answer_kind == ANSWER_REFUSED) {
		buffer[1] = 0x80000001U;
	} else {
		buffer[1] = 0x80000000U;
	}
	if (answer_kind == ANSWER_UNKNOWN_TAG) {
		buffer[4] = 0;
	} else {
		buffer[4] = 0x80000000U | buffer[3];
		if (buffer[3] != 0)
			buffer[5] = 0xa0U;
		if (buffer[3] > 4U) {
			buffer[6] = 0xa1U;
			buffer[7] = 0xa2U;
		}
	}
	pending[pending_count++] = request;
}

/* Reads a whole file into memory. */
static unsigned char *
read_file(
	const char *path,
	size_t *size)
{
	unsigned char *bytes;
	FILE *file;
	long length;

	file = fopen(path, "rb");
	if (file == NULL) {
		perror(path);
		exit(EXIT_FAILURE);
	}
	fseek(file, 0, SEEK_END);
	length = ftell(file);
	fseek(file, 0, SEEK_SET);
	bytes = malloc((size_t)length);
	CHECK(bytes != NULL);
	CHECK(fread(bytes, 1, (size_t)length, file) == (size_t)length);
	fclose(file);
	*size = (size_t)length;
	return bytes;
}

/*
 * The kernel services the client calls.
 */

uint32_t
kern_mmio_read32(
	const volatile void *address)
{
	unsigned offset;
	uint32_t word;
	unsigned i;

	offset = (unsigned)((const volatile uint8_t *)address - (const volatile uint8_t *)mailbox_registers);
	polls++;
	if (offset == 0x38U)
		return answer_kind == ANSWER_MAILBOX_FULL ? 0x80000000U : 0;
	if (offset == 0x18U)
		return pending_count == 0 ? 0x40000000U : 0;
	if (offset == 0x00U) {
		CHECK(pending_count > 0);
		word = pending[0];
		for (i = 1; i < pending_count; i++)
			pending[i - 1U] = pending[i];
		pending_count--;
		return word;
	}
	CHECK(false);
	return 0;
}

void
kern_mmio_write32(
	volatile void *address,
	uint32_t value)
{
	unsigned offset;

	offset = (unsigned)((volatile uint8_t *)address - (volatile uint8_t *)mailbox_registers);
	CHECK(offset == 0x20U);
	CHECK(answer_kind != ANSWER_MAILBOX_FULL);
	posted = value;
	answer(value);
}

int
kern_device_map(
	uint64_t address,
	size_t size,
	unsigned attributes,
	void **mapped)
{
	CHECK(address == MAILBOX_BASE);
	CHECK(size == 0x40U);
	CHECK(attributes == KERN_DEVICE_UNCACHED);
	*mapped = mailbox_registers;
	return 0;
}

int
kern_pmem_alloc_limited(
	size_t size,
	size_t alignment,
	uint64_t max_address,
	size_t boundary,
	struct kern_pmem *run)
{
	CHECK(size == 4096U);
	CHECK(alignment >= 16U);
	CHECK(max_address < 0x40000000ULL);
	CHECK(boundary == 0);
	run->paddr = BUFFER_PHYSICAL;
	run->size = size;
	return 0;
}

int
kern_pmem_free(
	struct kern_pmem *run)
{
	(void)run;
	CHECK(false);
	return 0;
}

void *
kern_pmem_to_kernel(
	hal_physaddr_t address)
{
	CHECK(address == BUFFER_PHYSICAL);
	return buffer;
}

void
kern_dcache_clean_range(
	const void *address,
	size_t size)
{
	CHECK(address == buffer);
	CHECK(size == buffer[0]);
	buffer_clean = true;
}

void
kern_dcache_invalidate_range(
	const void *address,
	size_t size)
{
	CHECK(address == buffer);
	CHECK(size >= 16U);
	buffer_invalidated = true;
}

void
kern_usleep_range(
	unsigned min_us,
	unsigned max_us)
{
	CHECK(min_us <= max_us);
}

bool
kern_irq_disable(void)
{
	irq_depth++;
	return true;
}

void
kern_irq_enable(void)
{
	irq_depth--;
}

void
kern_logf(
	const char *format,
	...)
{
	va_list arguments;

	va_start(arguments, format);
	printf("  log: ");
	vprintf(format, arguments);
	va_end(arguments);
}
