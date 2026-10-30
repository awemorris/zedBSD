/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host test of the Raspberry Pi 4 firmware mailbox on several CPUs.
 *
 * The production src/drivers/platform/rpi4/rpi4-firmware.c is compiled
 * unchanged against a model of the mailbox and of the VideoCore.  Real
 * threads stand for the CPUs: the GPU, the PCIe host, audio and the
 * wireless drivers all prepare the mailbox and send property requests at
 * the same time.  Built with ThreadSanitizer, it reports any unordered
 * access to the shared mailbox state; the model itself checks that the
 * mailbox is prepared exactly once and that every caller gets its own
 * answer.
 *
 * Usage: firmware-smp-host-test race|retry FIRMWARE.dtb
 *
 * race:  every thread prepares the mailbox, then sends requests.
 * retry: the first preparation fails; exactly one caller sees the failure
 *        and the next preparation succeeds.
 */

#include "drivers/platform/rpi4/rpi4-firmware.h"
#include <kern/clock.h>
#include <kern/dcache.h>
#include <kern/device-io.h>
#include <kern/irq.h>
#include <kern/klog.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(expression)                                                     \
	do {                                                                  \
		if (!(expression)) {                                          \
			fprintf(stderr, "firmware-smp: failed at %s:%d: %s\n",  \
			    __FILE__, __LINE__, #expression);                   \
			abort();                                              \
		}                                                             \
	} while (0)

#define MAILBOX_BASE		0xfe00b880ULL
#define BUFFER_PHYSICAL		0x00123000ULL

/* How many CPUs the test plays and how many requests each sends. */
#define CPU_COUNT		8U
#define REQUESTS_PER_CPU	2000U

/* The tag every request carries; the model answers its value plus one. */
#define TEST_TAG		0x00030002U

/*
 * The model's mailbox and VideoCore.  Only the caller that holds the
 * driver's request buffer touches them, so they are deliberately plain: a
 * second caller touching them at the same time is what TSan reports.
 */
static uint32_t mailbox_registers[0x40U / 4U];
static uint32_t buffer[4096U / 4U] __attribute__((aligned(4096)));
static uint32_t pending;
static unsigned pending_count;

/* The device tree every CPU prepares the mailbox from. */
static struct drv_fdt fdt;

/* Whether the first allocation is made to fail (the retry scenario). */
static int fail_first_allocation;

/* How often the driver prepared the mailbox; the model counts atomically. */
static unsigned allocations;
static unsigned mappings;

/* Every CPU starts together, so the preparations really overlap. */
static pthread_barrier_t start_line;

/* One CPU's record of what it saw. */
struct cpu_result {
	unsigned index;
	int init_error;
	unsigned answers;
};

static void *cpu_main(void *argument);
static void answer(uint32_t request);
static unsigned char *read_file(const char *path, size_t *size);

int
main(
	int argc,
	char **argv)
{
	pthread_t threads[CPU_COUNT];
	struct cpu_result results[CPU_COUNT];
	unsigned char *blob;
	size_t size;
	unsigned failures;
	unsigned answers;
	unsigned i;

	if (argc != 3) {
		fprintf(stderr, "usage: %s race|retry FIRMWARE.dtb\n", argv[0]);
		return EXIT_FAILURE;
	}
	if (strcmp(argv[1], "retry") == 0) {
		fail_first_allocation = 1;
	} else {
		CHECK(strcmp(argv[1], "race") == 0);
	}

	blob = read_file(argv[2], &size);
	CHECK(drv_fdt_open(&fdt, blob, size) == 0);
	CHECK(pthread_barrier_init(&start_line, NULL, CPU_COUNT) == 0);

	/* Starts every CPU and waits for all of them. */
	for (i = 0; i < CPU_COUNT; i++) {
		results[i].index = i;
		results[i].init_error = -1;
		results[i].answers = 0;
		CHECK(pthread_create(&threads[i], NULL, cpu_main, &results[i]) == 0);
	}
	for (i = 0; i < CPU_COUNT; i++)
		CHECK(pthread_join(threads[i], NULL) == 0);

	/* Counts the failed preparations and the answers. */
	failures = 0;
	answers = 0;
	for (i = 0; i < CPU_COUNT; i++) {
		if (results[i].init_error != 0) {
			CHECK(results[i].init_error == ENOMEM);
			failures++;
		}
		answers += results[i].answers;
	}

	/* The mailbox was prepared once; a failed attempt is retried once. */
	if (fail_first_allocation) {
		CHECK(failures == 1U);
		CHECK(__atomic_load_n(&allocations, __ATOMIC_SEQ_CST) == 2U);
	} else {
		CHECK(failures == 0);
		CHECK(__atomic_load_n(&allocations, __ATOMIC_SEQ_CST) == 1U);
	}
	CHECK(__atomic_load_n(&mappings, __ATOMIC_SEQ_CST) == 1U);
	CHECK(answers == (CPU_COUNT - failures) * REQUESTS_PER_CPU);

	printf("firmware-smp %s: %u CPUs, %u preparations, %u failed, %u answers\n",
	    argv[1], CPU_COUNT, allocations, failures, answers);
	return EXIT_SUCCESS;
}

/*
 * One CPU: prepares the mailbox at the same moment as the others, then
 * sends requests that only it can recognize the answers of.
 */
static void *
cpu_main(
	void *argument)
{
	struct cpu_result *result;
	uint32_t values[1];
	uint32_t answered;
	unsigned request;
	int error;

	result = argument;
	pthread_barrier_wait(&start_line);

	/* Half of the CPUs also try a request while the mailbox may be in preparation. */
	if ((result->index & 1U) != 0) {
		values[0] = 0;
		error = drv_rpi4_firmware_property(TEST_TAG, values, 1U, 1U, &answered);
		CHECK(error == 0 || error == ENODEV);
	}

	result->init_error = drv_rpi4_firmware_init(&fdt);
	if (result->init_error != 0)
		return NULL;

	/* Sends requests whose value names this CPU and the request. */
	for (request = 0; request < REQUESTS_PER_CPU; request++) {
		values[0] = (result->index << 24) | request;
		answered = 0;
		error = drv_rpi4_firmware_property(TEST_TAG, values, 1U, 1U, &answered);
		CHECK(error == 0);
		CHECK(answered == 4U);
		CHECK(values[0] == ((result->index << 24) | request) + 1U);
		result->answers++;
	}

	return NULL;
}

/* Plays the VideoCore: answers the value in the buffer plus one. */
static void
answer(
	uint32_t request)
{
	CHECK((request & 0xfU) == 8U);
	CHECK((request & 0x3ffffff0U) == (uint32_t)BUFFER_PHYSICAL);
	CHECK(buffer[2] == TEST_TAG);
	CHECK(buffer[3] == 4U);

	/* Takes a little time, so another CPU has room to interfere. */
	sched_yield();

	buffer[1] = 0x80000000U;
	buffer[4] = 0x80000000U | 4U;
	buffer[5] = buffer[5] + 1U;
	pending = request;
	pending_count = 1;
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

	offset = (unsigned)((const volatile uint8_t *)address - (const volatile uint8_t *)mailbox_registers);
	if (offset == 0x38U)
		return 0;
	if (offset == 0x18U)
		return pending_count == 0 ? 0x40000000U : 0;
	if (offset == 0x00U) {
		CHECK(pending_count == 1U);
		pending_count = 0;
		return pending;
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
	CHECK(pending_count == 0);
	answer(value);
}

int
kern_device_map(
	uint64_t address,
	size_t size,
	unsigned attributes,
	void **mapped)
{
	struct timespec pause;

	CHECK(address == MAILBOX_BASE);
	CHECK(size == 0x40U);
	CHECK(attributes == KERN_DEVICE_UNCACHED);
	__atomic_fetch_add(&mappings, 1U, __ATOMIC_SEQ_CST);

	/* Holds the preparation open, so the other CPUs arrive during it. */
	pause.tv_sec = 0;
	pause.tv_nsec = 2000000L;
	nanosleep(&pause, NULL);

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
	unsigned previous;
	struct timespec pause;

	CHECK(size == 4096U);
	CHECK(alignment >= 16U);
	CHECK(max_address < 0x40000000ULL);
	CHECK(boundary == 0);
	previous = __atomic_fetch_add(&allocations, 1U, __ATOMIC_SEQ_CST);

	/* The retry scenario fails the first preparation after the others have arrived. */
	if (fail_first_allocation && previous == 0) {
		pause.tv_sec = 0;
		pause.tv_nsec = 2000000L;
		nanosleep(&pause, NULL);
		return ENOMEM;
	}

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
	kern_physaddr_t address)
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
}

void
kern_dcache_invalidate_range(
	const void *address,
	size_t size)
{
	CHECK(address == buffer);
	CHECK(size >= 16U);
}

void
kern_usleep_range(
	unsigned min_us,
	unsigned max_us)
{
	CHECK(min_us <= max_us);
	sched_yield();
}

bool
kern_irq_disable(void)
{
	return true;
}

void
kern_irq_enable(void)
{
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

/*
 * The kernel's lock-word claim and spin hint, which the kernel core defines
 * with the HAL.  The host's exchange stands in for the port's sequence.
 */
int
atomic_raw_try_acquire_zero(
	volatile unsigned *value)
{
	return __atomic_exchange_n(value, 1U, __ATOMIC_ACQUIRE) == 0U;
}

/* The host has no spin instruction to give. */
void
atomic_spin_hint(
	void)
{
}
