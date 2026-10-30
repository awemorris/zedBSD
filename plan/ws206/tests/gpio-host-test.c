/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host test of the Raspberry Pi 4 GPIO helpers and the firmware's GPIO
 * expander helpers (ws206-p002a).
 *
 * The production rpi-gpio.c and rpi4-firmware.c are compiled unchanged
 * against a model of the GPIO block's registers, of the mailbox and of the
 * VideoCore's answers, with the firmware's real device tree.
 *
 * Usage: gpio-host-test unit|smp FIRMWARE.dtb
 *
 * unit: register positions and bits of every pin, the two pull conversions,
 *       the refusals, and the expander helpers' requests and answers.
 * smp:  eight threads map the block at once, then change pins that share
 *       one function register and one pull register; no change is lost
 *       and, under ThreadSanitizer, no register is touched without the lock.
 */

#include "drivers/platform/rpi4/rpi4-firmware.h"
#include "drivers/platform/rpi-common/rpi-gpio.h"
#include <kern/clock.h>
#include <kern/dcache.h>
#include <kern/device-io.h>
#include <kern/irq.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Stops the test at the first expectation that does not hold. */
#define CHECK(expression)                                                     \
	do {                                                                  \
		if (!(expression)) {                                          \
			fprintf(stderr, "gpio-host-test: failed at %s:%d: %s\n", \
			    __FILE__, __LINE__, #expression);                   \
			abort();                                              \
		}                                                             \
	} while (0)

/* Where the firmware's tree puts the blocks, as the ARM sees them. */
#define GPIO_BASE		0xfe200000ULL
#define MAILBOX_BASE		0xfe00b880ULL
#define BUFFER_PHYSICAL		0x00123000ULL

/* The GPIO registers the helpers may touch: up to the last pull register. */
#define GPIO_SPAN		0xf4U
#define GPIO_PULL_OFFSET	0xe4U

/* The firmware's tags and numbering, written out independently of the driver. */
#define TAG_GET_GPIO_STATE	0x00030041U
#define TAG_SET_GPIO_STATE	0x00038041U
#define TAG_GET_GPIO_CONFIG	0x00030043U
#define TAG_SET_GPIO_CONFIG	0x00038043U
#define EXPANDER_BASE		128U

/* How many CPUs the smp scenario plays and how many changes each makes. */
#define CPU_COUNT		8U
#define CHANGES_PER_CPU		4000U

/* How the model VideoCore answers the next GPIO request. */
enum answer_mode {
	ANSWER_NORMAL,
	ANSWER_STATUS_FAILED,
	ANSWER_SHORT,
	ANSWER_EMPTY,
	ANSWER_BAD_DIRECTION,
	ANSWER_SET_LENGTH,
	ANSWER_SETS_IGNORED
};

/* One CPU's part in the smp scenario. */
struct cpu_part {
	unsigned index;
	int init_error;
};

/*
 * The model GPIO block.  Plain words: only a caller holding the driver's
 * lock may touch them, which ThreadSanitizer checks in the smp scenario.
 */
static uint32_t gpio_block[GPIO_SPAN / 4U];

/* The model mailbox, request buffer and the word waiting in mailbox 0. */
static uint32_t mailbox_registers[0x40U / 4U];
static uint32_t buffer[4096U / 4U] __attribute__((aligned(4096)));
static uint32_t pending;
static unsigned pending_count;

/* The levels and directions the model expander holds, for GPIO 128 to 135. */
static uint32_t expander_level[8];
static uint32_t expander_direction[8];

/* How the model answers, and what it last saw. */
static enum answer_mode answer_mode;
static unsigned requests_posted;
static uint32_t last_tag;
static uint32_t last_capacity;
static uint32_t last_values[2];

/* How often the GPIO block was mapped. */
static unsigned gpio_mappings;

/* The lock that stands for the driver's spinlock, and who holds it. */
static pthread_mutex_t fake_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread int holding_lock;

/* The firmware's device tree. */
static struct drv_fdt fdt;

/* Every CPU starts together, so the preparations really overlap. */
static pthread_barrier_t start_line;

static void run_unit(void);
static void run_smp(void);
static void check_conversions(void);
static void check_functions(void);
static void check_pulls(void);
static void check_expander(void);
static void *cpu_main(void *argument);
static void answer(uint32_t request);
static unsigned char *read_file(const char *path, size_t *size);

/*
 * Runs one scenario against the device tree named on the command line.
 */
int
main(
	int argc,
	char **argv)
{
	unsigned char *blob;
	size_t size;
	int error;

	/* Refuses a command line without a scenario and a tree. */
	if (argc != 3) {
		fprintf(stderr, "usage: %s unit|smp FIRMWARE.dtb\n", argv[0]);
		return EXIT_FAILURE;
	}

	/* Opens the firmware's tree. */
	blob = read_file(argv[2], &size);
	error = drv_fdt_open(&fdt, blob, size);
	CHECK(error == 0);

	/* Runs the scenario asked for. */
	if (strcmp(argv[1], "unit") == 0) {
		run_unit();
	} else {
		CHECK(strcmp(argv[1], "smp") == 0);
		run_smp();
	}

	/* Succeeded: every expectation held. */
	printf("gpio-host-test %s: PASS\n", argv[1]);
	return EXIT_SUCCESS;
}

/* Runs the single-threaded checks. */
static void
run_unit(void)
{
	enum drv_rpi_gpio_function function;
	int error;

	/* The conversion from the tree needs no mapping. */
	check_conversions();

	/* Every helper that touches registers refuses to before the block is mapped. */
	error = drv_rpi_gpio_set_function(34U, DRV_RPI_GPIO_FUNCTION_ALT3);
	CHECK(error == ENODEV);
	error = drv_rpi_gpio_get_function(34U, &function);
	CHECK(error == ENODEV);
	error = drv_rpi_gpio_set_pull(35U, DRV_RPI_GPIO_PULL_UP);
	CHECK(error == ENODEV);

	/* Maps the block once; a second call does nothing. */
	error = drv_rpi_gpio_init(&fdt);
	CHECK(error == 0);
	error = drv_rpi_gpio_init(&fdt);
	CHECK(error == 0);
	CHECK(gpio_mappings == 1U);

	/* Checks every pin's function and pull bits. */
	check_functions();
	check_pulls();

	/* Checks the expander helpers before and after the mailbox is ready. */
	check_expander();
}

/* Checks the device tree's pull numbers and the refusal of anything else. */
static void
check_conversions(void)
{
	enum drv_rpi_gpio_pull pull;
	int error;

	/* The tree says 0 for none, 1 for down and 2 for up. */
	error = drv_rpi_gpio_pull_from_dt(0U, &pull);
	CHECK(error == 0);
	CHECK(pull == DRV_RPI_GPIO_PULL_NONE);
	error = drv_rpi_gpio_pull_from_dt(1U, &pull);
	CHECK(error == 0);
	CHECK(pull == DRV_RPI_GPIO_PULL_DOWN);
	error = drv_rpi_gpio_pull_from_dt(2U, &pull);
	CHECK(error == 0);
	CHECK(pull == DRV_RPI_GPIO_PULL_UP);

	/* Any other number, or nowhere to put the answer, is refused. */
	error = drv_rpi_gpio_pull_from_dt(3U, &pull);
	CHECK(error == EINVAL);
	error = drv_rpi_gpio_pull_from_dt(0xffffffffU, &pull);
	CHECK(error == EINVAL);
	error = drv_rpi_gpio_pull_from_dt(0U, NULL);
	CHECK(error == EINVAL);
}

/* Checks that each pin's function lands in its own three bits of GPFSELn. */
static void
check_functions(void)
{
	enum drv_rpi_gpio_function function;
	uint32_t before[GPIO_SPAN / 4U];
	unsigned pin;
	unsigned code;
	unsigned word;
	unsigned shift;
	unsigned index;
	int error;

	/* Sets every function on every pin over a register file of noise. */
	for (pin = 0; pin < DRV_RPI_GPIO_PIN_COUNT; pin++) {
		for (code = 0; code < 8U; code++) {
			/* Fills the block with a pattern the change must keep. */
			for (index = 0; index < GPIO_SPAN / 4U; index++)
				gpio_block[index] = 0x9e3779b9U * (index + pin + code + 1U);
			memcpy(before, gpio_block, sizeof(before));

			/* Changes the pin. */
			error = drv_rpi_gpio_set_function(pin, (enum drv_rpi_gpio_function)code);
			CHECK(error == 0);

			/* Only the pin's three bits of its register changed, and they hold the code. */
			word = pin / 10U;
			shift = 3U * (pin % 10U);
			for (index = 0; index < GPIO_SPAN / 4U; index++) {
				if (index != word) {
					CHECK(gpio_block[index] == before[index]);
				}
			}
			CHECK((gpio_block[word] & ~(7U << shift)) == (before[word] & ~(7U << shift)));
			CHECK(((gpio_block[word] >> shift) & 7U) == code);

			/* The read gives the code back. */
			error = drv_rpi_gpio_get_function(pin, &function);
			CHECK(error == 0);
			CHECK((unsigned)function == code);
		}
	}

	/* Pins and functions the block does not have are refused without a write. */
	memcpy(before, gpio_block, sizeof(before));
	error = drv_rpi_gpio_set_function(DRV_RPI_GPIO_PIN_COUNT, DRV_RPI_GPIO_FUNCTION_OUTPUT);
	CHECK(error == EINVAL);
	error = drv_rpi_gpio_set_function(34U, (enum drv_rpi_gpio_function)8);
	CHECK(error == EINVAL);
	error = drv_rpi_gpio_set_function(34U, (enum drv_rpi_gpio_function)-1);
	CHECK(error == EINVAL);
	error = drv_rpi_gpio_get_function(DRV_RPI_GPIO_PIN_COUNT, &function);
	CHECK(error == EINVAL);
	error = drv_rpi_gpio_get_function(34U, NULL);
	CHECK(error == EINVAL);
	CHECK(memcmp(before, gpio_block, sizeof(before)) == 0);
}

/* Checks that each pin's resistor lands in its own two bits with the register's code. */
static void
check_pulls(void)
{
	static const enum drv_rpi_gpio_pull pulls[3] = {
		DRV_RPI_GPIO_PULL_NONE,
		DRV_RPI_GPIO_PULL_UP,
		DRV_RPI_GPIO_PULL_DOWN
	};

	/* BCM2711 GPIO_PUP_PDN_CNTRL: 00 none, 01 up, 10 down. */
	static const uint32_t codes[3] = { 0x0U, 0x1U, 0x2U };

	uint32_t before[GPIO_SPAN / 4U];
	unsigned pin;
	unsigned choice;
	unsigned word;
	unsigned shift;
	unsigned index;
	int error;

	/* Sets every resistor on every pin over a register file of noise. */
	for (pin = 0; pin < DRV_RPI_GPIO_PIN_COUNT; pin++) {
		for (choice = 0; choice < 3U; choice++) {
			/* Fills the block with a pattern the change must keep. */
			for (index = 0; index < GPIO_SPAN / 4U; index++)
				gpio_block[index] = 0x7f4a7c15U * (index + pin + choice + 3U);
			memcpy(before, gpio_block, sizeof(before));

			/* Changes the pin. */
			error = drv_rpi_gpio_set_pull(pin, pulls[choice]);
			CHECK(error == 0);

			/* Only the pin's two bits of its pull register changed, and they hold the code. */
			word = (GPIO_PULL_OFFSET / 4U) + pin / 16U;
			shift = 2U * (pin % 16U);
			for (index = 0; index < GPIO_SPAN / 4U; index++) {
				if (index != word) {
					CHECK(gpio_block[index] == before[index]);
				}
			}
			CHECK((gpio_block[word] & ~(3U << shift)) == (before[word] & ~(3U << shift)));
			CHECK(((gpio_block[word] >> shift) & 3U) == codes[choice]);
		}
	}

	/* The raw numbers of the tree (0) and outside the enumeration (4) are refused without a write. */
	memcpy(before, gpio_block, sizeof(before));
	error = drv_rpi_gpio_set_pull(35U, (enum drv_rpi_gpio_pull)0);
	CHECK(error == EINVAL);
	error = drv_rpi_gpio_set_pull(35U, (enum drv_rpi_gpio_pull)4);
	CHECK(error == EINVAL);
	error = drv_rpi_gpio_set_pull(DRV_RPI_GPIO_PIN_COUNT, DRV_RPI_GPIO_PULL_UP);
	CHECK(error == EINVAL);
	CHECK(memcmp(before, gpio_block, sizeof(before)) == 0);
}

/* Checks the expander helpers' refusals, requests and answer checks. */
static void
check_expander(void)
{
	enum drv_rpi4_firmware_gpio_direction direction;
	unsigned posted;
	int state;
	int error;

	/* Before the mailbox is ready the helpers pass its ENODEV on. */
	error = drv_rpi4_firmware_get_gpio(DRV_RPI4_FIRMWARE_GPIO_WL_ON, &state);
	CHECK(error == ENODEV);
	error = drv_rpi4_firmware_set_gpio(DRV_RPI4_FIRMWARE_GPIO_WL_ON, 1);
	CHECK(error == ENODEV);
	error = drv_rpi4_firmware_get_gpio_direction(DRV_RPI4_FIRMWARE_GPIO_BT_ON, &direction);
	CHECK(error == ENODEV);
	CHECK(requests_posted == 0);

	/* Prepares the mailbox. */
	error = drv_rpi4_firmware_init(&fdt);
	CHECK(error == 0);

	/* Lines outside the enumeration and other levels are refused without asking. */
	error = drv_rpi4_firmware_get_gpio((enum drv_rpi4_firmware_gpio)2, &state);
	CHECK(error == EINVAL);
	error = drv_rpi4_firmware_get_gpio((enum drv_rpi4_firmware_gpio)6, &state);
	CHECK(error == EINVAL);
	error = drv_rpi4_firmware_get_gpio((enum drv_rpi4_firmware_gpio)-1, &state);
	CHECK(error == EINVAL);
	error = drv_rpi4_firmware_get_gpio((enum drv_rpi4_firmware_gpio)128, &state);
	CHECK(error == EINVAL);
	error = drv_rpi4_firmware_set_gpio((enum drv_rpi4_firmware_gpio)6, 1);
	CHECK(error == EINVAL);
	error = drv_rpi4_firmware_set_gpio(DRV_RPI4_FIRMWARE_GPIO_WL_ON, 2);
	CHECK(error == EINVAL);
	error = drv_rpi4_firmware_set_gpio(DRV_RPI4_FIRMWARE_GPIO_WL_ON, -1);
	CHECK(error == EINVAL);
	error = drv_rpi4_firmware_get_gpio_direction((enum drv_rpi4_firmware_gpio)2, &direction);
	CHECK(error == EINVAL);
	error = drv_rpi4_firmware_get_gpio(DRV_RPI4_FIRMWARE_GPIO_WL_ON, NULL);
	CHECK(error == EINVAL);
	error = drv_rpi4_firmware_get_gpio_direction(DRV_RPI4_FIRMWARE_GPIO_WL_ON, NULL);
	CHECK(error == EINVAL);
	CHECK(requests_posted == 0);

	/* Reading WL_ON asks for GPIO 129 and reports the model's level. */
	expander_level[1] = 1U;
	expander_level[0] = 0;
	answer_mode = ANSWER_NORMAL;
	error = drv_rpi4_firmware_get_gpio(DRV_RPI4_FIRMWARE_GPIO_WL_ON, &state);
	CHECK(error == 0);
	CHECK(state == 1);
	CHECK(last_tag == TAG_GET_GPIO_STATE);
	CHECK(last_values[0] == EXPANDER_BASE + 1U);
	CHECK(last_capacity == 8U);

	/* Reading BT_ON asks for GPIO 128; a level other than 1 still reads as high. */
	expander_level[0] = 5U;
	error = drv_rpi4_firmware_get_gpio(DRV_RPI4_FIRMWARE_GPIO_BT_ON, &state);
	CHECK(error == 0);
	CHECK(state == 1);
	CHECK(last_values[0] == EXPANDER_BASE + 0U);
	expander_level[0] = 0;
	error = drv_rpi4_firmware_get_gpio(DRV_RPI4_FIRMWARE_GPIO_BT_ON, &state);
	CHECK(error == 0);
	CHECK(state == 0);

	/*
	 * Driving WL_ON low while it is an input reads its configuration and
	 * makes it an output at level 0 with SET_GPIO_CONFIG; the firmware
	 * answers a setting tag without the answered bit (BUG-311), which is
	 * taken as an answer.
	 */
	expander_direction[1] = 0;
	error = drv_rpi4_firmware_set_gpio(DRV_RPI4_FIRMWARE_GPIO_WL_ON, 0);
	CHECK(error == 0);
	CHECK(last_tag == TAG_SET_GPIO_CONFIG);
	CHECK(last_values[0] == EXPANDER_BASE + 1U);
	CHECK(last_capacity == 24U);
	CHECK(expander_direction[1] == 1U);
	CHECK(expander_level[1] == 0);

	/* Driving the output high sends GPIO 129 and level 1 with SET_GPIO_STATE. */
	error = drv_rpi4_firmware_set_gpio(DRV_RPI4_FIRMWARE_GPIO_WL_ON, 1);
	CHECK(error == 0);
	CHECK(last_tag == TAG_SET_GPIO_STATE);
	CHECK(last_values[0] == EXPANDER_BASE + 1U);
	CHECK(last_values[1] == 1U);
	CHECK(last_capacity == 8U);
	CHECK(expander_level[1] == 1U);

	/* A firmware that does set the answered bit on a change is taken as well. */
	answer_mode = ANSWER_SET_LENGTH;
	error = drv_rpi4_firmware_set_gpio(DRV_RPI4_FIRMWARE_GPIO_WL_ON, 0);
	CHECK(error == 0);
	CHECK(expander_level[1] == 0);
	answer_mode = ANSWER_NORMAL;

	/* The direction is read with one request word in a five-word buffer. */
	expander_direction[1] = 1U;
	expander_direction[0] = 0;
	error = drv_rpi4_firmware_get_gpio_direction(DRV_RPI4_FIRMWARE_GPIO_WL_ON, &direction);
	CHECK(error == 0);
	CHECK(direction == DRV_RPI4_FIRMWARE_GPIO_OUTPUT);
	CHECK(last_tag == TAG_GET_GPIO_CONFIG);
	CHECK(last_values[0] == EXPANDER_BASE + 1U);
	CHECK(last_capacity == 20U);
	error = drv_rpi4_firmware_get_gpio_direction(DRV_RPI4_FIRMWARE_GPIO_BT_ON, &direction);
	CHECK(error == 0);
	CHECK(direction == DRV_RPI4_FIRMWARE_GPIO_INPUT);

	/* A nonzero status word in the answer is a failure for all three (a change fails at its configuration question). */
	answer_mode = ANSWER_STATUS_FAILED;
	error = drv_rpi4_firmware_get_gpio(DRV_RPI4_FIRMWARE_GPIO_WL_ON, &state);
	CHECK(error == EIO);
	error = drv_rpi4_firmware_set_gpio(DRV_RPI4_FIRMWARE_GPIO_WL_ON, 0);
	CHECK(error == EIO);
	error = drv_rpi4_firmware_get_gpio_direction(DRV_RPI4_FIRMWARE_GPIO_WL_ON, &direction);
	CHECK(error == EIO);

	/* A question answered with the status word alone lacks its answer, also the one a change asks first. */
	answer_mode = ANSWER_SHORT;
	error = drv_rpi4_firmware_get_gpio(DRV_RPI4_FIRMWARE_GPIO_WL_ON, &state);
	CHECK(error == EIO);
	error = drv_rpi4_firmware_get_gpio_direction(DRV_RPI4_FIRMWARE_GPIO_WL_ON, &direction);
	CHECK(error == EIO);
	error = drv_rpi4_firmware_set_gpio(DRV_RPI4_FIRMWARE_GPIO_WL_ON, 1);
	CHECK(error == EIO);

	/* An answer without even the status word is a failure. */
	answer_mode = ANSWER_EMPTY;
	error = drv_rpi4_firmware_set_gpio(DRV_RPI4_FIRMWARE_GPIO_WL_ON, 1);
	CHECK(error == EIO);

	/* A direction the firmware does not define is a failure. */
	answer_mode = ANSWER_BAD_DIRECTION;
	error = drv_rpi4_firmware_get_gpio_direction(DRV_RPI4_FIRMWARE_GPIO_WL_ON, &direction);
	CHECK(error == EIO);

	/* A change the firmware did not handle leaves the GPIO in the status word: a failure, and the level stays. */
	answer_mode = ANSWER_SETS_IGNORED;
	expander_level[1] = 0;
	error = drv_rpi4_firmware_set_gpio(DRV_RPI4_FIRMWARE_GPIO_WL_ON, 1);
	CHECK(error == EIO);
	CHECK(last_tag == TAG_SET_GPIO_STATE);
	CHECK(expander_level[1] == 0);
	answer_mode = ANSWER_NORMAL;

	/* Every accepted call posted its requests: one each, two for a change. */
	posted = requests_posted;
	CHECK(posted == 21U);
}

/* Runs the threads that map the block and change shared registers at once. */
static void
run_smp(void)
{
	pthread_t threads[CPU_COUNT];
	struct cpu_part parts[CPU_COUNT];
	enum drv_rpi_gpio_function function;
	unsigned i;
	unsigned word;
	unsigned shift;
	int error;

	/* Starts every CPU and waits for all of them. */
	error = pthread_barrier_init(&start_line, NULL, CPU_COUNT);
	CHECK(error == 0);
	for (i = 0; i < CPU_COUNT; i++) {
		parts[i].index = i;
		parts[i].init_error = -1;
		error = pthread_create(&threads[i], NULL, cpu_main, &parts[i]);
		CHECK(error == 0);
	}
	for (i = 0; i < CPU_COUNT; i++) {
		error = pthread_join(threads[i], NULL);
		CHECK(error == 0);
	}

	/* Every CPU saw the block mapped, and it was mapped once. */
	for (i = 0; i < CPU_COUNT; i++)
		CHECK(parts[i].init_error == 0);
	CHECK(__atomic_load_n(&gpio_mappings, __ATOMIC_SEQ_CST) == 1U);

	/* Each CPU's last change of its pins survived the others' changes. */
	for (i = 0; i < CPU_COUNT; i++) {
		error = drv_rpi_gpio_get_function(30U + i, &function);
		CHECK(error == 0);
		CHECK(function == (enum drv_rpi_gpio_function)((i + 1U) & 7U));
		word = (GPIO_PULL_OFFSET / 4U) + (32U + 2U * i) / 16U;
		shift = 2U * ((32U + 2U * i) % 16U);
		pthread_mutex_lock(&fake_lock);
		CHECK(((gpio_block[word] >> shift) & 3U) == 0x1U);
		CHECK(((gpio_block[word] >> (shift + 2U)) & 3U) == 0x2U);
		pthread_mutex_unlock(&fake_lock);
	}

	printf("gpio-host-test smp: %u CPUs, %u changes each, %u mapping\n",
	    CPU_COUNT, CHANGES_PER_CPU, gpio_mappings);
}

/*
 * One CPU: maps the block at the same moment as the others, then changes
 * its own pin of GPFSEL3 (pins 30 to 37) and its own two pins of the pull
 * register for pins 32 to 47, reading each change back at once.
 */
static void *
cpu_main(
	void *argument)
{
	static const enum drv_rpi_gpio_pull pulls[3] = {
		DRV_RPI_GPIO_PULL_NONE,
		DRV_RPI_GPIO_PULL_UP,
		DRV_RPI_GPIO_PULL_DOWN
	};
	struct cpu_part *part;
	enum drv_rpi_gpio_function function;
	enum drv_rpi_gpio_function wanted;
	unsigned change;
	unsigned pin;
	int error;

	part = argument;
	pin = 30U + part->index;
	pthread_barrier_wait(&start_line);

	/* Maps the block together with the others. */
	part->init_error = drv_rpi_gpio_init(&fdt);
	if (part->init_error != 0)
		return NULL;

	/* Changes the pins; a lost update shows as a different read-back. */
	for (change = 0; change < CHANGES_PER_CPU; change++) {
		wanted = (enum drv_rpi_gpio_function)((change + part->index) & 7U);
		error = drv_rpi_gpio_set_function(pin, wanted);
		CHECK(error == 0);
		error = drv_rpi_gpio_get_function(pin, &function);
		CHECK(error == 0);
		CHECK(function == wanted);
		error = drv_rpi_gpio_set_pull(32U + 2U * part->index, pulls[change % 3U]);
		CHECK(error == 0);
		error = drv_rpi_gpio_set_pull(33U + 2U * part->index, pulls[(change + 1U) % 3U]);
		CHECK(error == 0);
	}

	/* Leaves a final value the main thread can check. */
	error = drv_rpi_gpio_set_function(pin, (enum drv_rpi_gpio_function)((part->index + 1U) & 7U));
	CHECK(error == 0);
	error = drv_rpi_gpio_set_pull(32U + 2U * part->index, DRV_RPI_GPIO_PULL_UP);
	CHECK(error == 0);
	error = drv_rpi_gpio_set_pull(33U + 2U * part->index, DRV_RPI_GPIO_PULL_DOWN);
	CHECK(error == 0);
	return NULL;
}

/*
 * Plays the VideoCore: answers one GPIO request in the buffer as the model
 * expander and the current answer mode say.
 */
static void
answer(
	uint32_t request)
{
	uint32_t gpio;
	uint32_t tag;
	int setting;

	CHECK((request & 0xfU) == 8U);
	CHECK((request & 0x3ffffff0U) == (uint32_t)BUFFER_PHYSICAL);

	/* Records what was asked. */
	tag = buffer[2];
	gpio = buffer[5];
	requests_posted++;
	last_tag = tag;
	last_capacity = buffer[3];
	last_values[0] = buffer[5];
	last_values[1] = buffer[6];
	CHECK(gpio >= EXPANDER_BASE);
	CHECK(gpio < EXPANDER_BASE + 8U);

	/* Handles the request and writes the answer over the request's words. */
	buffer[1] = 0x80000000U;
	setting = 0;
	if (tag == TAG_GET_GPIO_STATE) {
		CHECK(buffer[3] == 8U);
		buffer[6] = expander_level[gpio - EXPANDER_BASE];
		buffer[4] = 0x80000000U | 8U;
	} else if (tag == TAG_SET_GPIO_STATE) {
		CHECK(buffer[3] == 8U);
		setting = 1;
		if (answer_mode != ANSWER_SETS_IGNORED)
			expander_level[gpio - EXPANDER_BASE] = buffer[6];
	} else if (tag == TAG_SET_GPIO_CONFIG) {
		CHECK(buffer[3] == 24U);
		setting = 1;
		if (answer_mode != ANSWER_SETS_IGNORED) {
			expander_direction[gpio - EXPANDER_BASE] = buffer[6];
			expander_level[gpio - EXPANDER_BASE] = buffer[10];
		}
	} else {
		CHECK(tag == TAG_GET_GPIO_CONFIG);
		CHECK(buffer[3] == 20U);
		buffer[6] = expander_direction[gpio - EXPANDER_BASE];
		buffer[7] = 0;
		buffer[8] = 0;
		buffer[9] = 0;
		buffer[4] = 0x80000000U | 20U;
	}

	/*
	 * The firmware leaves a setting tag's code as the request wrote it,
	 * zero (BUG-311); a change it did not handle also keeps the GPIO in the
	 * first word.
	 */
	if (setting) {
		CHECK(buffer[4] == 0);
		if (answer_mode == ANSWER_SETS_IGNORED) {
			pending = request;
			pending_count = 1;
			return;
		}
		if (answer_mode == ANSWER_SET_LENGTH)
			buffer[4] = 0x80000000U | buffer[3];
	}
	buffer[5] = 0;

	/* Spoils the answer as the mode asks. */
	if (answer_mode == ANSWER_STATUS_FAILED) {
		buffer[5] = 0xffffffffU;
	} else if (answer_mode == ANSWER_SHORT) {
		buffer[4] = 0x80000000U | 4U;
	} else if (answer_mode == ANSWER_EMPTY) {
		buffer[4] = 0x80000000U;
		buffer[5] = gpio;
	} else if (answer_mode == ANSWER_BAD_DIRECTION) {
		buffer[6] = 2U;
	}

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
 * The kernel services the drivers call.
 */

/* Reads a model register: the GPIO block under the lock, or the mailbox. */
uint32_t
kern_mmio_read32(
	const volatile void *address)
{
	const volatile uint8_t *byte;
	unsigned offset;

	byte = address;
	if (byte >= (const volatile uint8_t *)gpio_block && byte < (const volatile uint8_t *)gpio_block + GPIO_SPAN) {
		CHECK(holding_lock);
		offset = (unsigned)(byte - (const volatile uint8_t *)gpio_block);
		return gpio_block[offset / 4U];
	}

	offset = (unsigned)(byte - (const volatile uint8_t *)mailbox_registers);
	if (offset == 0x38U)
		return 0;
	if (offset == 0x18U)
		return pending_count == 0 ? 0x40000000U : 0;
	CHECK(offset == 0x00U);
	CHECK(pending_count == 1U);
	pending_count = 0;
	return pending;
}

/* Writes a model register: the GPIO block under the lock, or the mailbox. */
void
kern_mmio_write32(
	volatile void *address,
	uint32_t value)
{
	volatile uint8_t *byte;
	unsigned offset;

	byte = address;
	if (byte >= (volatile uint8_t *)gpio_block && byte < (volatile uint8_t *)gpio_block + GPIO_SPAN) {
		CHECK(holding_lock);
		offset = (unsigned)(byte - (volatile uint8_t *)gpio_block);
		gpio_block[offset / 4U] = value;
		return;
	}

	offset = (unsigned)(byte - (volatile uint8_t *)mailbox_registers);
	CHECK(offset == 0x20U);
	CHECK(pending_count == 0);
	answer(value);
}

/* Maps the GPIO block or the mailbox to the model's registers. */
int
kern_device_map(
	uint64_t address,
	size_t size,
	unsigned attributes,
	void **mapped)
{
	struct timespec pause;

	CHECK(attributes == KERN_DEVICE_UNCACHED);
	if (address == MAILBOX_BASE) {
		CHECK(size == 0x40U);
		*mapped = mailbox_registers;
		return 0;
	}

	/* The GPIO block is mapped through the last pull register. */
	CHECK(address == GPIO_BASE);
	CHECK(size == GPIO_SPAN);
	__atomic_fetch_add(&gpio_mappings, 1U, __ATOMIC_SEQ_CST);

	/* Holds the preparation open, so the other CPUs arrive during it. */
	pause.tv_sec = 0;
	pause.tv_nsec = 2000000L;
	nanosleep(&pause, NULL);

	*mapped = gpio_block;
	return 0;
}

/* Takes the driver's lock: the one GPIO lock, a leaf of the device rank. */
unsigned long
spin_lock_irqsave(
	struct spinlock *lock)
{
	CHECK(lock->rank == LOCK_RANK_DEVICE);
	CHECK(strcmp(lock->name, "rpi4 gpio") == 0);
	CHECK(!holding_lock);
	pthread_mutex_lock(&fake_lock);
	holding_lock = 1;
	return 1UL;
}

/* Gives the driver's lock back. */
void
spin_unlock_irqrestore(
	struct spinlock *lock,
	unsigned long enabled)
{
	CHECK(lock->rank == LOCK_RANK_DEVICE);
	CHECK(enabled == 1UL);
	CHECK(holding_lock);
	holding_lock = 0;
	pthread_mutex_unlock(&fake_lock);
}

/* Hands out the request buffer below 1 GiB. */
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

/* Never called: the mapping does not fail. */
int
kern_pmem_free(
	struct kern_pmem *run)
{
	(void)run;
	CHECK(false);
	return 0;
}

/* Finds the request buffer. */
void *
kern_pmem_to_kernel(
	hal_physaddr_t address)
{
	CHECK(address == BUFFER_PHYSICAL);
	return buffer;
}

/* The model VideoCore shares the host's caches. */
void
kern_dcache_clean_range(
	const void *address,
	size_t size)
{
	CHECK(address == buffer);
	CHECK(size == buffer[0]);
}

/* The model VideoCore shares the host's caches. */
void
kern_dcache_invalidate_range(
	const void *address,
	size_t size)
{
	CHECK(address == buffer);
	CHECK(size >= 16U);
}

/* Lets another thread run instead of busy waiting. */
void
kern_usleep_range(
	unsigned min_us,
	unsigned max_us)
{
	CHECK(min_us <= max_us);
	sched_yield();
}

/* Prints the drivers' log lines. */
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
