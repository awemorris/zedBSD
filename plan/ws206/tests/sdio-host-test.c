/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host test of the Raspberry Pi 4 SDIO host, the SD interrupt line's
 * dispatcher, the SDIO card layer and the onboard Wi-Fi's thread
 * (ws206-p002b).
 *
 * The production rpi-mmc-irq.c, rpi-sdio-host.c, rpi-sdio.c,
 * rpi4-wifi.c, rpi-gpio.c and rpi4-board.c are compiled unchanged against
 * a model of the legacy EMMC controller's registers with an SDIO card
 * behind them, a model of the GPIO block, stand-ins for the firmware's
 * mailbox helpers, and the firmware's real device tree.
 *
 * Usage: sdio-host-test unit|stop|smp|nopackage|refused FIRMWARE.dtb
 *
 * unit: the clock divider, the answers to CMD5 and CMD52, the CIS walk,
 *       the slot in the tree, the dispatcher's two slots, the host's
 *       procedures and the card's interrupt, enumeration, CMD53, and the
 *       Wi-Fi thread from probe to a published chip (ws206-p004): the
 *       BCM43455 driver is replaced by stand-ins that drive the bus
 *       operations rpi4-wifi.c gives it.
 * stop: the thread is stopped while it waits for a card that never
 *       answers; the stop returns at once and everything is given back.
 * smp:  the dispatcher's slots change while interrupts arrive, and the
 *       card's interrupt fires while the serving thread waits and rearms,
 *       and a kick ends the wait.
 * nopackage: without the firmware package the thread ends having touched
 *       no hardware: no mailbox, no pin, no power line.
 * refused: a chip the driver refuses (its probe fails) gets everything
 *       given back.
 */

#include "drivers/platform/rpi4/rpi4-firmware.h"
#include "drivers/platform/rpi-common/rpi-gpio.h"
#include "drivers/platform/rpi-common/rpi-mmc-irq.h"
#include "drivers/platform/rpi-common/rpi-sdio-host.h"
#include "drivers/platform/rpi-common/rpi-sdio.h"
#include "drivers/platform/rpi4/rpi4-wifi.h"
#include "drivers/wifi/bcm43455/bcm43455.h"
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/irq.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/pmem.h>
#include <kern/thread.h>
#include <kern/waitq.h>
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
			fprintf(stderr, "sdio-host-test: failed at %s:%d: %s\n", \
			    __FILE__, __LINE__, #expression);                   \
			abort();                                              \
		}                                                             \
	} while (0)

/* Where the firmware's tree puts the blocks, as the ARM sees them. */
#define SDHCI_BASE		0xfe300000ULL
#define EMMC2_BASE		0xfe340000ULL
#define GPIO_BASE		0xfe200000ULL
#define FDT_PHYSICAL		0x00200000ULL

/* The SD interrupt line: GIC SPI 126. */
#define MMC_IRQ			158

/* The register spans the drivers map. */
#define SDHCI_SPAN		0x100U
#define GPIO_SPAN		0xf4U

/* The clock the model firmware feeds the controller. */
#define EMMC_CLOCK_HZ		250000000U

/* The controller's registers, written out independently of the driver. */
#define R_BLOCK			0x04U
#define R_ARGUMENT		0x08U
#define R_COMMAND		0x0cU
#define R_RESPONSE		0x10U
#define R_DATA			0x20U
#define R_PRESENT		0x24U
#define R_HOST			0x28U
#define R_CLOCK			0x2cU
#define R_STATUS		0x30U
#define R_STATUS_ENABLE		0x34U
#define R_SIGNAL_ENABLE		0x38U
#define R_CAPABILITIES		0x40U
#define R_VERSION		0xfcU

/* The status bits the model raises. */
#define S_COMMAND		0x00000001U
#define S_TRANSFER		0x00000002U
#define S_WRITE_READY		0x00000010U
#define S_READ_READY		0x00000020U
#define S_CARD			0x00000100U
#define S_ERROR			0x00008000U
#define S_COMMAND_TIMEOUT	0x00010000U

/* The card's spaces: function 0 (CCCR, FBR, CIS) and function 1 (backplane). */
#define F0_SPAN			0x2000U
#define F1_SPAN			0x10000U
#define CIS_ADDRESS		0x1000U

/* The largest transfer the model holds. */
#define TRANSFER_MAX		(DRV_RPI_SDIO_HOST_BLOCK_MAX * 4U)

/*
 * The scheduler's clock and its default priority (kern/sched.h, whose
 * sched_yield() clashes with the host's).
 */
#define TEST_PRIORITY_DEFAULT	8
uint64_t sched_ticks(void);

/* How many times the smp scenario fires and changes. */
#define SMP_INTERRUPTS		20000U
#define SMP_CHANGES		2000U

/*
 * The model SDIO card.
 *
 * powered follows WL_ON.  silent_cmd5 CMD5s after power-up get no answer
 * and busy_cmd5 offers of a voltage answer not ready; absent never
 * answers at all.
 */
struct card_model {
	int powered;
	int absent;
	int silent_cmd5;
	int busy_cmd5;
	int silent_setting;
	int busy_setting;
	int ready;
	int addressed;
	int selected;
	uint8_t f0[F0_SPAN];
	uint8_t f1[F1_SPAN];
};

/*
 * The CMD53 transfer in flight: its direction, where it goes, and how far
 * it has come.
 */
struct transfer_model {
	int active;
	int write;
	unsigned function;
	uint32_t address;
	int increment;
	unsigned block_size;
	unsigned blocks;
	unsigned done;
	uint8_t bytes[TRANSFER_MAX];
};

/* What the model WL_ON line has been driven to, in order. */
struct power_record {
	int states[16];
	unsigned count;
};

/*
 * The model registers and card.  Changed by the driver's register
 * accesses and, in the smp scenario, only under the big lock.
 */
static uint32_t sdhci[SDHCI_SPAN / 4U];
static uint32_t sdhci_status;
static uint32_t gpio_block[GPIO_SPAN / 4U];
static struct card_model card;
static struct transfer_model transfer;
static volatile int card_irq_line;

/* Whether the next controller write must wait for a pause first (R2). */
static int pause_owed;

/* How many times the controller was wholly reset, and its lines. */
static unsigned resets_all;
static unsigned resets_lines;

/* The power bytes written, in order, and how many status reads the handler made. */
static uint32_t power_writes[16];
static unsigned power_write_count;
static unsigned status_reads;

/* The model WL_ON and the firmware stand-ins' counters. */
static struct power_record power;
static unsigned firmware_inits;

/* The kernel interrupt stand-ins: the installed handler and the line's state. */
static kern_irq_handler_t installed_handler;
static void *installed_argument;
static int installed_irq;
static unsigned register_calls;
static int register_fail_next;
static unsigned mask_calls;
static unsigned unmask_calls;
static unsigned eoi_calls;
static unsigned mode_calls;

/* The Wi-Fi thread the stand-in thread_start() runs on a host thread. */
static void (*thread_entry)(void *);
static void *thread_argument;
static struct thread fake_thread;
static pthread_t wifi_pthread;
static int wifi_pthread_started;

/*
 * The big lock that stands for every spinlock, and what each thread holds.
 * Recursive, because the dispatcher's lock is held when a controller's
 * handler takes its own.
 */
static pthread_mutex_t big_lock;
static pthread_cond_t big_wakeup = PTHREAD_COND_INITIALIZER;
static __thread unsigned held_depth;
static __thread const char *held_names[4];
static unsigned nested_dispatch;

/* The kernel mutexes, each a host mutex. */
static struct {
	struct mutex *mutex;
	pthread_mutex_t host;
} mutexes[8];
static unsigned mutex_count;
static pthread_mutex_t mutex_table_lock = PTHREAD_MUTEX_INITIALIZER;

/* When the test began, for the scheduler's clock. */
static struct timespec started;

/* The firmware's device tree, as read and as opened. */
static unsigned char *tree_blob;
static size_t tree_size;
static struct drv_fdt fdt;

/*
 * The BCM43455 driver's stand-ins: whether the package's files are there,
 * what the probe answers, and what the thread asked of them.
 */
static int files_present = 1;
static int probe_answer;
static unsigned files_loaded;
static unsigned devices_made;
static unsigned devices_probed;
static unsigned devices_published;
static unsigned devices_destroyed;
static unsigned probe_cccr;
static const struct bcm43455_bus_ops *bus_ops;
static void *bus_context;
static int fake_device_token;

/* The handlers the dispatcher tests add, and how often each was asked. */
static unsigned asked[3];
static bool answers[3];
static volatile int smp_removed;
static volatile int smp_stop;

static void run_unit(void);
static void run_stop(void);
static void run_smp(void);
static void run_nopackage(void);
static void run_refused(void);
static void check_divisor(void);
static void check_decoders(void);
static void check_cis(void);
static void check_slot(void);
static void check_dispatcher(void);
static void check_host(void);
static void check_enumeration(void);
static void check_thread(void);
static void prepare_model(void);
static void power_card(int on);
static void execute_command(uint32_t value);
static uint32_t read_data_port(void);
static void write_data_port(uint32_t value);
static int model_slow(void);
static uint32_t model_clock_hz(void);
static int count_sleep(void *context, unsigned milliseconds);
static int cancel_sleep(void *context, unsigned milliseconds);
static bool handler_0(void *argument);
static bool handler_1(void *argument);
static bool handler_2(void *argument);
static bool smp_handler(void *argument);
static void *smp_interrupts(void *argument);
static void *smp_waiter(void *argument);
static void *wifi_pthread_main(void *argument);
static void join_wifi(void);
static void prepare_gpio(void);
static void corrupt(unsigned char *blob, size_t size, const char *text, char replacement);
static unsigned char *read_file(const char *path, size_t *size);
static pthread_mutex_t *find_mutex(struct mutex *mutex);

/*
 * Runs one scenario against the device tree named on the command line.
 */
int
main(
	int argc,
	char **argv)
{
	pthread_mutexattr_t attributes;
	int error;

	/* Requires the scenario and the tree. */
	if (argc != 3) {
		fprintf(stderr, "usage: sdio-host-test unit|stop|smp FIRMWARE.dtb\n");
		return 2;
	}

	/* Keeps the drivers' log lines in order with a failure's report. */
	setvbuf(stdout, NULL, _IONBF, 0);

	/* Prepares the big lock and the clock. */
	pthread_mutexattr_init(&attributes);
	pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&big_lock, &attributes);
	clock_gettime(CLOCK_MONOTONIC, &started);

	/* Opens the firmware's tree. */
	tree_blob = read_file(argv[2], &tree_size);
	error = drv_fdt_open(&fdt, tree_blob, tree_size);
	CHECK(error == 0);

	/* Runs the scenario asked for. */
	if (strcmp(argv[1], "unit") == 0) {
		run_unit();
	} else if (strcmp(argv[1], "stop") == 0) {
		run_stop();
	} else if (strcmp(argv[1], "smp") == 0) {
		run_smp();
	} else if (strcmp(argv[1], "nopackage") == 0) {
		run_nopackage();
	} else if (strcmp(argv[1], "refused") == 0) {
		run_refused();
	} else {
		fprintf(stderr, "sdio-host-test: unknown scenario %s\n", argv[1]);
		return 2;
	}

	printf("sdio-host-test %s: PASS\n", argv[1]);
	return 0;
}

/* The single-threaded checks, then the thread from probe to power-down. */
static void
run_unit(void)
{
	check_divisor();
	check_decoders();
	check_cis();
	check_slot();
	check_dispatcher();
	check_host();
	check_enumeration();
	check_thread();
}

/* The divider: specification 3.00's 10 bits, the older powers of two, the refusals. */
static void
check_divisor(void)
{
	unsigned divisor;
	uint32_t actual;

	/* 25 MHz from the base clocks the firmware may give. */
	CHECK(drv_rpi_sdio_host_clock_divisor(250000000U, 25000000U, 2, &divisor, &actual) == 0);
	CHECK(divisor == 5U && actual == 25000000U);
	CHECK(drv_rpi_sdio_host_clock_divisor(200000000U, 25000000U, 2, &divisor, &actual) == 0);
	CHECK(divisor == 4U && actual == 25000000U);
	CHECK(drv_rpi_sdio_host_clock_divisor(100000000U, 25000000U, 2, &divisor, &actual) == 0);
	CHECK(divisor == 2U && actual == 25000000U);

	/* 400 kHz, never above. */
	CHECK(drv_rpi_sdio_host_clock_divisor(250000000U, 400000U, 2, &divisor, &actual) == 0);
	CHECK(divisor == 313U && actual == 399361U);
	CHECK(drv_rpi_sdio_host_clock_divisor(200000000U, 400000U, 2, &divisor, &actual) == 0);
	CHECK(divisor == 250U && actual == 400000U);
	CHECK(drv_rpi_sdio_host_clock_divisor(100000000U, 400000U, 2, &divisor, &actual) == 0);
	CHECK(divisor == 125U && actual == 400000U);

	/* A version 2.00 host rounds up to a power of two, up to 128. */
	CHECK(drv_rpi_sdio_host_clock_divisor(100000000U, 400000U, 1, &divisor, &actual) == 0);
	CHECK(divisor == 128U && actual == 390625U);
	CHECK(drv_rpi_sdio_host_clock_divisor(100000000U, 25000000U, 1, &divisor, &actual) == 0);
	CHECK(divisor == 2U && actual == 25000000U);
	CHECK(drv_rpi_sdio_host_clock_divisor(250000000U, 25000000U, 1, &divisor, &actual) == 0);
	CHECK(divisor == 8U && actual == 15625000U);
	CHECK(drv_rpi_sdio_host_clock_divisor(250000000U, 400000U, 1, &divisor, &actual) == ERANGE);

	/* An undivided clock, a rate too slow for 1023, and missing values. */
	CHECK(drv_rpi_sdio_host_clock_divisor(25000000U, 25000000U, 2, &divisor, &actual) == 0);
	CHECK(divisor == 0 && actual == 25000000U);
	CHECK(drv_rpi_sdio_host_clock_divisor(250000000U, 100000U, 2, &divisor, &actual) == ERANGE);
	CHECK(drv_rpi_sdio_host_clock_divisor(0, 400000U, 2, &divisor, &actual) == EINVAL);
	CHECK(drv_rpi_sdio_host_clock_divisor(250000000U, 0, 2, &divisor, &actual) == EINVAL);
}

/* The answers to CMD5 and CMD52, and the voltage window. */
static void
check_decoders(void)
{
	struct drv_rpi_sdio_ocr ocr;
	uint32_t window;
	uint8_t data;

	/* A ready card with two functions and no memory, from 2.8 to 3.6 V. */
	CHECK(drv_rpi_sdio_decode_ocr(0xa0ff8000U, &ocr) == 0);
	CHECK(ocr.ready == 1 && ocr.function_count == 2 && ocr.memory_present == 0);
	CHECK(ocr.voltages == 0x00ff8000U);
	CHECK(drv_rpi_sdio_voltage_window(ocr.voltages, &window) == 0);
	CHECK(window == 0x00300000U);

	/* A busy combined card with seven functions. */
	CHECK(drv_rpi_sdio_decode_ocr(0x78100000U, &ocr) == 0);
	CHECK(ocr.ready == 0 && ocr.function_count == 7 && ocr.memory_present == 1);
	CHECK(drv_rpi_sdio_voltage_window(ocr.voltages, &window) == 0);
	CHECK(window == 0x00100000U);

	/* A card that cannot run at 3.3 V. */
	CHECK(drv_rpi_sdio_voltage_window(0x0000ff80U, &window) == ENOTSUP);

	/* R5: the data byte, and each error flag refused. */
	CHECK(drv_rpi_sdio_decode_r5(0x00001055U, &data) == 0 && data == 0x55U);
	CHECK(drv_rpi_sdio_decode_r5(0x000020aaU, &data) == 0 && data == 0xaaU);
	CHECK(drv_rpi_sdio_decode_r5(0x00008000U, &data) == EIO);
	CHECK(drv_rpi_sdio_decode_r5(0x00004000U, &data) == EIO);
	CHECK(drv_rpi_sdio_decode_r5(0x00000800U, &data) == EIO);
	CHECK(drv_rpi_sdio_decode_r5(0x00000200U, &data) == EIO);
	CHECK(drv_rpi_sdio_decode_r5(0x00000100U, &data) == EIO);
	CHECK(drv_rpi_sdio_decode_r5(0x00000400U, NULL) == 0);
}

/* The CIS walk: the tuples it reads, padding, the two ends, and truncation. */
static void
check_cis(void)
{
	static const uint8_t chain[] = {
		0x21, 0x02, 0x0c, 0x00,
		0x00,
		0x22, 0x04, 0x00, 0x00, 0x02, 0x32,
		0x22, 0x04, 0x01, 0x40, 0x00, 0x00,
		0x20, 0x04, 0xd0, 0x02, 0x45, 0x43,
		0xff
	};
	static const uint8_t link_end[] = { 0x20, 0x04, 0xd0, 0x02, 0x45, 0x43, 0x80, 0xff, 0x22 };
	static const uint8_t short_manfid[] = { 0x20, 0x02, 0xd0, 0x02, 0xff };
	static const uint8_t truncated[] = { 0x20, 0x04, 0xd0, 0x02 };
	static const uint8_t no_end[] = { 0x21, 0x02, 0x0c, 0x00 };
	struct drv_rpi_sdio_cis cis;

	/* MANFID and function 0's FUNCE are read; function 1's FUNCE is not function 0's. */
	CHECK(drv_rpi_sdio_parse_cis(chain, sizeof(chain), &cis) == 0);
	CHECK(cis.has_manufacturer == 1 && cis.manufacturer == 0x02d0U && cis.card == 0x4345U);
	CHECK(cis.has_function0 == 1 && cis.function0_block_max == 0x0200U && cis.speed_code == 0x32U);

	/* A link of 0xff ends the chain where it stands. */
	CHECK(drv_rpi_sdio_parse_cis(link_end, sizeof(link_end), &cis) == 0);
	CHECK(cis.has_manufacturer == 1 && cis.has_function0 == 0);

	/* A MANFID too short to hold the codes is passed over. */
	CHECK(drv_rpi_sdio_parse_cis(short_manfid, sizeof(short_manfid), &cis) == 0);
	CHECK(cis.has_manufacturer == 0);

	/* A tuple or a chain that runs out of bytes. */
	CHECK(drv_rpi_sdio_parse_cis(truncated, sizeof(truncated), &cis) == EINVAL);
	CHECK(drv_rpi_sdio_parse_cis(no_end, sizeof(no_end), &cis) == EINVAL);
	CHECK(drv_rpi_sdio_parse_cis(NULL, 4, &cis) == EINVAL);
}

/* The slot in the firmware's tree, and the trees that refuse it. */
static void
check_slot(void)
{
	struct drv_rpi_sdio_slot slot;
	struct drv_fdt changed;
	unsigned char *copy;
	unsigned i;

	/* The board's tree with the SD card on eMMC2. */
	CHECK(drv_rpi_sdio_describe_slot(&fdt, (uintptr_t)EMMC2_BASE, &slot) == 0);
	CHECK(slot.registers == SDHCI_BASE);
	CHECK(slot.irq == MMC_IRQ);
	CHECK(slot.pin_count == 6U);
	for (i = 0; i < 6U; i++)
		CHECK(slot.pins[i] == 34U + i);
	CHECK(slot.function == DRV_RPI_GPIO_FUNCTION_ALT3);
	CHECK(slot.pulls[0] == DRV_RPI_GPIO_PULL_NONE);
	for (i = 1; i < 6U; i++)
		CHECK(slot.pulls[i] == DRV_RPI_GPIO_PULL_UP);

	/* The SD card on the same controller (QEMU's fallback) keeps it. */
	CHECK(drv_rpi_sdio_describe_slot(&fdt, (uintptr_t)SDHCI_BASE, &slot) == EBUSY);

	/* A tree without the PCIe controller and GENET is QEMU's. */
	copy = malloc(tree_size);
	CHECK(copy != NULL);
	memcpy(copy, tree_blob, tree_size);
	corrupt(copy, tree_size, "brcm,bcm2711-pcie", 'x');
	corrupt(copy, tree_size, "brcm,bcm2711-genet-v5", 'x');
	CHECK(drv_fdt_open(&changed, copy, tree_size) == 0);
	CHECK(drv_rpi_sdio_describe_slot(&changed, (uintptr_t)EMMC2_BASE, &slot) == ENODEV);

	/* A tree without the chip. */
	memcpy(copy, tree_blob, tree_size);
	corrupt(copy, tree_size, "brcm,bcm4329-fmac", 'x');
	CHECK(drv_fdt_open(&changed, copy, tree_size) == 0);
	CHECK(drv_rpi_sdio_describe_slot(&changed, (uintptr_t)EMMC2_BASE, &slot) == ENODEV);

	/* The slot switched off, as the disable-wifi overlay does. */
	memcpy(copy, tree_blob, tree_size);
	corrupt(copy, tree_size, "mmcnr@7e300000", 0);
	CHECK(drv_fdt_open(&changed, copy, tree_size) == 0);
	CHECK(drv_rpi_sdio_describe_slot(&changed, (uintptr_t)EMMC2_BASE, &slot) == ENODEV);
	free(copy);
}

/* The dispatcher: the installation, the two slots, the refusals and a stuck line. */
static void
check_dispatcher(void)
{
	unsigned i;

	/* A failed installation is retried by the next add. */
	register_fail_next = 1;
	CHECK(drv_rpi_mmc_irq_add(MMC_IRQ, handler_0, &asked[0]) == EIO);
	CHECK(register_calls == 1U && installed_handler == NULL);
	CHECK(drv_rpi_mmc_irq_add(MMC_IRQ, handler_0, &asked[0]) == 0);
	CHECK(register_calls == 2U && installed_handler != NULL && installed_irq == MMC_IRQ);
	CHECK(installed_argument == NULL);
	CHECK(mode_calls == 2U);

	/* The line was masked for the installation and unmasked by the first handler. */
	CHECK(mask_calls == 2U && unmask_calls == 1U);

	/* The second slot, then the refusals. */
	CHECK(drv_rpi_mmc_irq_add(MMC_IRQ, handler_1, &asked[1]) == 0);
	CHECK(drv_rpi_mmc_irq_add(MMC_IRQ, handler_2, &asked[2]) == EBUSY);
	CHECK(drv_rpi_mmc_irq_add(MMC_IRQ, handler_1, &asked[1]) == EEXIST);
	CHECK(drv_rpi_mmc_irq_add(MMC_IRQ + 1, handler_2, &asked[2]) == EINVAL);
	CHECK(drv_rpi_mmc_irq_add(MMC_IRQ, NULL, NULL) == EINVAL);
	CHECK(register_calls == 2U);

	/* An interrupt asks both, even when the first claims it, and is ended once. */
	answers[0] = true;
	answers[1] = false;
	installed_handler(MMC_IRQ, (kern_irq_ack_t)1, installed_argument);
	CHECK(asked[0] == 1U && asked[1] == 1U && eoi_calls == 1U);

	/* A removed handler is not asked again. */
	CHECK(drv_rpi_mmc_irq_remove(MMC_IRQ, handler_0, &asked[0]) == 0);
	installed_handler(MMC_IRQ, (kern_irq_ack_t)1, installed_argument);
	CHECK(asked[0] == 1U && asked[1] == 2U && eoi_calls == 2U);
	CHECK(drv_rpi_mmc_irq_remove(MMC_IRQ, handler_0, &asked[0]) == ENOENT);
	CHECK(drv_rpi_mmc_irq_remove(MMC_IRQ + 1, handler_1, &asked[1]) == EINVAL);

	/* A line nobody claims for a long run is masked; the next add unmasks it. */
	for (i = 0; i < 100000U; i++)
		installed_handler(MMC_IRQ, (kern_irq_ack_t)1, installed_argument);
	CHECK(mask_calls == 3U);
	installed_handler(MMC_IRQ, (kern_irq_ack_t)1, installed_argument);
	CHECK(mask_calls == 3U);
	CHECK(drv_rpi_mmc_irq_add(MMC_IRQ, handler_0, &asked[0]) == 0);
	CHECK(unmask_calls == 2U);

	/* Leaves the line with both slots empty for the Wi-Fi thread. */
	CHECK(drv_rpi_mmc_irq_remove(MMC_IRQ, handler_0, &asked[0]) == 0);
	CHECK(drv_rpi_mmc_irq_remove(MMC_IRQ, handler_1, &asked[1]) == 0);
	installed_handler(MMC_IRQ, (kern_irq_ack_t)1, installed_argument);
	CHECK(asked[0] == 1U);
}

/* The host's procedures on the model: init, commands, errors, the card's interrupt and a reset. */
static void
check_host(void)
{
	struct drv_rpi_sdio_host host;
	struct drv_rpi_sdio_command command;
	uint64_t deadline;
	unsigned reads_before;
	bool mine;

	/* Init: a reset, 3.3 V before power, the 400 kHz divider, the enables. */
	prepare_model();
	CHECK(drv_rpi_sdio_host_init(&host, sdhci, EMMC_CLOCK_HZ) == 0);
	CHECK(resets_all == 1U);
	CHECK(host.specification == 2U);
	CHECK(power_write_count >= 2U);
	CHECK(power_writes[power_write_count - 2U] == 0x0eU);
	CHECK(power_writes[power_write_count - 1U] == 0x0fU);
	CHECK((sdhci[R_CLOCK / 4U] & 0x7U) == 0x7U);
	CHECK(((sdhci[R_CLOCK / 4U] >> 8) & 0xffU) == (313U & 0xffU));
	CHECK(((sdhci[R_CLOCK / 4U] >> 6) & 0x3U) == (313U >> 8));
	CHECK(((sdhci[R_CLOCK / 4U] >> 16) & 0xffU) == 0x0eU);
	CHECK(host.card_clock_hz == 399361U);
	CHECK(sdhci[R_STATUS_ENABLE / 4U] == 0x03ff0033U);
	CHECK(sdhci[R_SIGNAL_ENABLE / 4U] == 0);

	/* A command to a card without power times out, and the lines are reset for the next. */
	power_card(0);
	command.index = 5;
	command.argument = 0;
	command.response = DRV_RPI_SDIO_RESPONSE_SHORT_UNCHECKED;
	command.data = DRV_RPI_SDIO_DATA_NONE;
	command.buffer = NULL;
	command.block_size = 0;
	command.block_count = 0;
	CHECK(drv_rpi_sdio_host_command(&host, &command) == ETIMEDOUT);
	CHECK(resets_lines == 1U);
	CHECK((sdhci_status & 0xffff7fffU) == 0);

	/* A powered card answers CMD5. */
	power_card(1);
	card.silent_cmd5 = 0;
	CHECK(drv_rpi_sdio_host_command(&host, &command) == 0);
	CHECK((command.reply & 0x70000000U) == 0x20000000U);

	/* Refusals before the controller is touched. */
	command.index = 64;
	CHECK(drv_rpi_sdio_host_command(&host, &command) == EINVAL);
	command.index = 53;
	command.data = DRV_RPI_SDIO_DATA_READ;
	CHECK(drv_rpi_sdio_host_command(&host, &command) == EINVAL);
	command.data = DRV_RPI_SDIO_DATA_NONE;

	/* Moves to 25 MHz: no pause owed from here on. */
	CHECK(drv_rpi_sdio_host_set_clock(&host, 25000000U) == 0);
	CHECK(host.card_clock_hz == 25000000U);
	CHECK(((sdhci[R_CLOCK / 4U] >> 8) & 0xffU) == 5U);
	CHECK(model_slow() == 0);

	/* A disarmed controller is not the line's, and its status is not read. */
	card_irq_line = 1;
	reads_before = status_reads;
	mine = drv_rpi_sdio_host_interrupt(&host);
	CHECK(!mine && status_reads == reads_before);

	/* Enabled, the card's interrupt is signalled; the handler disarms it and wakes the waiter. */
	drv_rpi_sdio_host_card_irq_enable(&host, 1);
	CHECK((sdhci[R_STATUS_ENABLE / 4U] & S_CARD) != 0);
	CHECK((sdhci[R_SIGNAL_ENABLE / 4U] & S_CARD) != 0);
	mine = drv_rpi_sdio_host_interrupt(&host);
	CHECK(mine);
	CHECK((sdhci[R_STATUS_ENABLE / 4U] & S_CARD) == 0);
	CHECK((sdhci[R_SIGNAL_ENABLE / 4U] & S_CARD) == 0);
	CHECK(host.card_irq_count == 1U);
	CHECK(drv_rpi_sdio_host_card_irq_wait(&host, sched_ticks() + 1000U) == 0);

	/* Nothing pending: the wait ends at its deadline. */
	deadline = sched_ticks() + 5U;
	CHECK(drv_rpi_sdio_host_card_irq_wait(&host, deadline) == ETIMEDOUT);
	CHECK(sched_ticks() >= deadline);

	/* A kick before the wait ends it at once with EAGAIN, once, and leaves the interrupt disarmed. */
	drv_rpi_sdio_host_card_irq_kick(&host);
	CHECK(drv_rpi_sdio_host_card_irq_wait(&host, sched_ticks() + 1000U) == EAGAIN);
	CHECK(sched_ticks() < deadline + 500U);
	CHECK(drv_rpi_sdio_host_card_irq_wait(&host, sched_ticks() + 5U) == ETIMEDOUT);
	CHECK((sdhci[R_SIGNAL_ENABLE / 4U] & S_CARD) == 0);

	/* A disarmed interrupt stays disarmed through a reset; rearming brings it back. */
	CHECK(drv_rpi_sdio_host_reset(&host) == 0);
	CHECK(resets_all == 2U);
	CHECK(host.card_clock_hz == 25000000U);
	CHECK(sdhci[R_STATUS_ENABLE / 4U] == 0x03ff0033U);
	CHECK(sdhci[R_SIGNAL_ENABLE / 4U] == 0);
	drv_rpi_sdio_host_card_irq_rearm(&host);
	CHECK(sdhci[R_STATUS_ENABLE / 4U] == (0x03ff0033U | S_CARD));
	CHECK(sdhci[R_SIGNAL_ENABLE / 4U] == S_CARD);

	/* An armed interrupt is armed again after a reset, which cleared the enables. */
	CHECK(drv_rpi_sdio_host_reset(&host) == 0);
	CHECK(sdhci[R_STATUS_ENABLE / 4U] == (0x03ff0033U | S_CARD));
	CHECK(sdhci[R_SIGNAL_ENABLE / 4U] == S_CARD);

	/* Disabling forgets the interrupt; stopping leaves no clock and no power. */
	drv_rpi_sdio_host_card_irq_enable(&host, 0);
	CHECK(sdhci[R_SIGNAL_ENABLE / 4U] == 0);
	card_irq_line = 0;
	drv_rpi_sdio_host_stop(&host);
	CHECK((sdhci[R_CLOCK / 4U] & 0xffffU) == 0);
	CHECK(((sdhci[R_HOST / 4U] >> 8) & 0xffU) == 0);
	CHECK(host.card_clock_hz == 0);
}

/* Enumeration on the model card, the functions, the block sizes and CMD53. */
static void
check_enumeration(void)
{
	struct drv_rpi_sdio_host host;
	struct drv_rpi_sdio_card sdio;
	struct drv_rpi_sdio_waiter waiter;
	struct drv_rpi_sdio_waiter canceller;
	unsigned sleeps;
	unsigned budget;
	uint8_t bytes[256];
	uint8_t back[256];
	unsigned i;

	/* A card that is silent twice and busy twice after its power-up. */
	prepare_model();
	CHECK(drv_rpi_sdio_host_init(&host, sdhci, EMMC_CLOCK_HZ) == 0);
	power_card(1);
	sleeps = 0;
	waiter.sleep = count_sleep;
	waiter.context = &sleeps;
	CHECK(drv_rpi_sdio_enumerate(&sdio, &host, &waiter) == 0);
	CHECK(sleeps == 4U);

	/* What enumeration learned. */
	CHECK(sdio.ocr.ready == 1 && sdio.ocr.function_count == 2U);
	CHECK(sdio.rca == 0x0001U);
	CHECK(card.selected == 1);
	CHECK(sdio.cccr_revision == 3U && sdio.sdio_revision == 4U);
	CHECK(sdio.cis_pointer == CIS_ADDRESS);
	CHECK(sdio.cis.has_manufacturer == 1);
	CHECK(sdio.cis.manufacturer == 0x02d0U && sdio.cis.card == 0x4345U);
	CHECK(sdio.cis.function0_block_max == 512U);

	/* The bus: four lines on both sides, 25 MHz. */
	CHECK(card.f0[0x07] == 0x02U);
	CHECK((sdhci[R_HOST / 4U] & 0x02U) != 0);
	CHECK(host.card_clock_hz == 25000000U);

	/* F1 becomes ready; F2 does not without the chip's firmware. */
	CHECK(drv_rpi_sdio_enable_function(&sdio, 1, &waiter, 10) == 0);
	CHECK((card.f0[0x02] & 0x02U) != 0);
	CHECK(drv_rpi_sdio_enable_function(&sdio, 2, &waiter, 5) == ETIMEDOUT);
	CHECK(drv_rpi_sdio_disable_function(&sdio, 2) == 0);
	CHECK((card.f0[0x02] & 0x04U) == 0 && (card.f0[0x02] & 0x02U) != 0);
	CHECK(drv_rpi_sdio_enable_function(&sdio, 3, &waiter, 5) == EINVAL);
	CHECK(drv_rpi_sdio_enable_function(&sdio, 0, &waiter, 5) == EINVAL);

	/* Block sizes, read back from the FBRs. */
	CHECK(drv_rpi_sdio_set_block_size(&sdio, 1, 64) == 0);
	CHECK(card.f0[0x110] == 64U && card.f0[0x111] == 0);
	CHECK(drv_rpi_sdio_set_block_size(&sdio, 2, 512) == 0);
	CHECK(card.f0[0x210] == 0 && card.f0[0x211] == 2U);
	CHECK(sdio.block_size[1] == 64U && sdio.block_size[2] == 512U);
	CHECK(drv_rpi_sdio_set_block_size(&sdio, 1, 0) == EINVAL);
	CHECK(drv_rpi_sdio_set_block_size(&sdio, 1, 4096) == EINVAL);

	/* CMD53 in byte mode, a length that is not a multiple of four. */
	for (i = 0; i < sizeof(bytes); i++)
		bytes[i] = (uint8_t)(i * 7U + 3U);
	CHECK(drv_rpi_sdio_transfer(&sdio, DRV_RPI_SDIO_DATA_WRITE, 1, 0x8000, 1, bytes, 63) == 0);
	CHECK(memcmp(card.f1 + 0x8000, bytes, 63) == 0);
	memset(back, 0, sizeof(back));
	CHECK(drv_rpi_sdio_transfer(&sdio, DRV_RPI_SDIO_DATA_READ, 1, 0x8000, 1, back, 63) == 0);
	CHECK(memcmp(back, bytes, 63) == 0);

	/* CMD53 in block mode, four blocks of 64. */
	CHECK(drv_rpi_sdio_transfer(&sdio, DRV_RPI_SDIO_DATA_WRITE, 1, 0x9000, 1, bytes, 256) == 0);
	CHECK(memcmp(card.f1 + 0x9000, bytes, 256) == 0);
	memset(back, 0, sizeof(back));
	CHECK(drv_rpi_sdio_transfer(&sdio, DRV_RPI_SDIO_DATA_READ, 1, 0x9000, 1, back, 256) == 0);
	CHECK(memcmp(back, bytes, 256) == 0);

	/* Runs that are not whole blocks, functions the card lacks, addresses beyond 17 bits. */
	CHECK(drv_rpi_sdio_transfer(&sdio, DRV_RPI_SDIO_DATA_READ, 1, 0x9000, 1, back, 100) == EINVAL);
	CHECK(drv_rpi_sdio_transfer(&sdio, DRV_RPI_SDIO_DATA_READ, 3, 0x9000, 1, back, 4) == EINVAL);
	CHECK(drv_rpi_sdio_transfer(&sdio, DRV_RPI_SDIO_DATA_READ, 1, 0x20000, 1, back, 4) == EINVAL);

	/* A card that never finishes its power-up times out after a second of waits. */
	prepare_model();
	CHECK(drv_rpi_sdio_host_init(&host, sdhci, EMMC_CLOCK_HZ) == 0);
	card.busy_setting = 1000000;
	power_card(1);
	sleeps = 0;
	CHECK(drv_rpi_sdio_enumerate(&sdio, &host, &waiter) == ETIMEDOUT);
	CHECK(sleeps >= 100U);

	/* A stop during the waits ends enumeration with the stop's error. */
	prepare_model();
	CHECK(drv_rpi_sdio_host_init(&host, sdhci, EMMC_CLOCK_HZ) == 0);
	card.absent = 1;
	budget = 3;
	canceller.sleep = cancel_sleep;
	canceller.context = &budget;
	CHECK(drv_rpi_sdio_enumerate(&sdio, &host, &canceller) == ECANCELED);
}

/*
 * The probe on a board, the thread run to a published chip, and what it
 * kept.  The probe runs once per kernel, so this is the unit scenario's
 * last step.
 */
static void
check_thread(void)
{
	enum drv_rpi_gpio_function function;
	uint32_t pulls;
	unsigned i;

	/* The board as the firmware leaves it: WL_ON high, the GPIO block mapped. */
	prepare_model();
	prepare_gpio();
	power_card(1);
	power.count = 0;
	card.silent_setting = 1;
	card.busy_setting = 1;

	/* The probe starts the thread without touching the hardware. */
	CHECK(drv_rpi4_wifi_probe(FDT_PHYSICAL, (uintptr_t)EMMC2_BASE) == 0);
	CHECK(fake_thread.detached == 1U);
	join_wifi();
	CHECK(drv_rpi4_wifi_probe(FDT_PHYSICAL, (uintptr_t)EMMC2_BASE) == EALREADY);

	/* The files were read before the mailbox, and the driver probed and published the chip. */
	CHECK(files_loaded == 1U && devices_made == 1U);
	CHECK(devices_probed == 1U && devices_published == 1U && devices_destroyed == 0U);
	CHECK(firmware_inits == 1U);

	/* The driver's power_up pulsed WL_ON low and high, its power_down drove it low. */
	CHECK(power.count == 3U);
	CHECK(power.states[0] == 0 && power.states[1] == 1 && power.states[2] == 0);

	/* GPIO 34 to 39 carry the controller's function, CLK without pull, the others pulled up. */
	for (i = 34; i <= 39; i++) {
		CHECK(drv_rpi_gpio_get_function(i, &function) == 0);
		CHECK(function == DRV_RPI_GPIO_FUNCTION_ALT3);
	}
	pulls = gpio_block[(0xe4U + 8U) / 4U];
	CHECK(((pulls >> ((34U - 32U) * 2U)) & 0x3U) == 0);
	for (i = 35; i <= 39; i++)
		CHECK(((pulls >> ((i - 32U) * 2U)) & 0x3U) == 1U);

	/* The card was enumerated, F1 enabled, the block sizes set, and the CCCR read through the bus. */
	CHECK((card.f0[0x02] & 0x02U) != 0);
	CHECK(card.f0[0x110] == 64U && card.f0[0x211] == 2U);
	CHECK(probe_cccr == card.f0[0x00]);
	CHECK(bus_ops->block_size(bus_context, 1) == 64U && bus_ops->block_size(bus_context, 2) == 512U);

	/* The published chip keeps the host: its clock still runs and its bus is powered. */
	CHECK((sdhci[R_CLOCK / 4U] & 0x0004U) != 0);
	CHECK(((sdhci[R_HOST / 4U] >> 8) & 0xffU) != 0);

	/* A stop after the end returns at once and leaves the published chip alone. */
	drv_rpi4_wifi_stop();
	CHECK((sdhci[R_CLOCK / 4U] & 0x0004U) != 0);
}

/*
 * The thread is stopped while it waits for a card that never answers; the
 * stop wakes its sleep, and the thread gives everything back before the
 * stop returns.
 */
static void
run_stop(void)
{
	uint64_t before;
	uint64_t taken;
	struct timespec pause;

	/* A board whose chip never answers CMD5. */
	prepare_model();
	prepare_gpio();
	power_card(1);
	power.count = 0;
	card.absent = 1;

	/* Lets the thread reach its CMD5 retries. */
	CHECK(drv_rpi4_wifi_probe(FDT_PHYSICAL, (uintptr_t)EMMC2_BASE) == 0);
	pause.tv_sec = 0;
	pause.tv_nsec = 100000000L;
	nanosleep(&pause, NULL);

	/* The stop ends the second of retries at once. */
	before = sched_ticks();
	drv_rpi4_wifi_stop();
	taken = sched_ticks() - before;
	printf("  stop took %llu ms\n", (unsigned long long)taken);
	CHECK(taken < 300U);
	join_wifi();

	/* The retries happened, the driver's stand-in powered down, and everything was given back. */
	CHECK(resets_lines > 0);
	CHECK(devices_destroyed == 1U && devices_published == 0U);
	CHECK(power.count == 4U);
	CHECK(power.states[0] == 0 && power.states[1] == 1 && power.states[2] == 0 && power.states[3] == 0);
	CHECK((sdhci[R_CLOCK / 4U] & 0xffffU) == 0);
	CHECK(sdhci[R_SIGNAL_ENABLE / 4U] == 0);
	CHECK(drv_rpi_mmc_irq_remove(MMC_IRQ, drv_rpi_sdio_host_interrupt, NULL) == ENOENT);

	/* A second stop finds no thread. */
	drv_rpi4_wifi_stop();
}

/*
 * Without the firmware package the thread reads no further than the file
 * system: no mailbox, no pin, no controller, no power line.
 */
static void
run_nopackage(void)
{
	enum drv_rpi_gpio_function function;

	/* A board without the package. */
	prepare_model();
	prepare_gpio();
	power_card(1);
	power.count = 0;
	files_present = 0;

	/* The thread ends on its own. */
	CHECK(drv_rpi4_wifi_probe(FDT_PHYSICAL, (uintptr_t)EMMC2_BASE) == 0);
	join_wifi();

	/* Nothing was touched. */
	CHECK(files_loaded == 1U && devices_made == 0U);
	CHECK(firmware_inits == 0U);
	CHECK(power.count == 0U);
	CHECK(register_calls == 0U);
	CHECK(drv_rpi_gpio_get_function(34, &function) == 0);
	CHECK(function == DRV_RPI_GPIO_FUNCTION_INPUT);
	CHECK(sdhci[R_CLOCK / 4U] == 0 && resets_all == 0U);

	drv_rpi4_wifi_stop();
}

/*
 * A chip the driver refuses is given everything back, as if no driver
 * had run.
 */
static void
run_refused(void)
{
	/* A board whose chip the driver's probe refuses, as for a placeholder address. */
	prepare_model();
	prepare_gpio();
	power_card(1);
	power.count = 0;
	probe_answer = EADDRNOTAVAIL;

	CHECK(drv_rpi4_wifi_probe(FDT_PHYSICAL, (uintptr_t)EMMC2_BASE) == 0);
	join_wifi();

	/* The device was destroyed and the hardware given back. */
	CHECK(devices_probed == 1U && devices_published == 0U && devices_destroyed == 1U);
	CHECK(power.count == 4U);
	CHECK(power.states[2] == 0 && power.states[3] == 0);
	CHECK((sdhci[R_CLOCK / 4U] & 0xffffU) == 0);
	CHECK(sdhci[R_SIGNAL_ENABLE / 4U] == 0);
	CHECK(((sdhci[R_HOST / 4U] >> 8) & 0xffU) == 0);

	drv_rpi4_wifi_stop();
}

/*
 * The dispatcher's slots change while interrupts arrive, then the card's
 * interrupt fires while the serving thread waits and rearms.
 */
static void
run_smp(void)
{
	struct drv_rpi_sdio_host host;
	pthread_t interrupts;
	pthread_t waiter;
	unsigned i;

	/* A permanent handler that claims every interrupt keeps the line unmasked. */
	answers[0] = true;
	CHECK(drv_rpi_mmc_irq_add(MMC_IRQ, handler_0, &asked[0]) == 0);

	/* Interrupts arrive on another CPU while this one adds and removes a second handler. */
	smp_stop = 0;
	CHECK(pthread_create(&interrupts, NULL, smp_interrupts, NULL) == 0);
	for (i = 0; i < SMP_CHANGES; i++) {
		__atomic_store_n(&smp_removed, 0, __ATOMIC_SEQ_CST);
		CHECK(drv_rpi_mmc_irq_add(MMC_IRQ, smp_handler, NULL) == 0);
		sched_yield();
		CHECK(drv_rpi_mmc_irq_remove(MMC_IRQ, smp_handler, NULL) == 0);
		__atomic_store_n(&smp_removed, 1, __ATOMIC_SEQ_CST);
	}
	__atomic_store_n(&smp_stop, 1, __ATOMIC_SEQ_CST);
	CHECK(pthread_join(interrupts, NULL) == 0);
	CHECK(drv_rpi_mmc_irq_remove(MMC_IRQ, handler_0, &asked[0]) == 0);

	/* The controller at 25 MHz with its card's interrupt armed on the line. */
	prepare_model();
	CHECK(drv_rpi_sdio_host_init(&host, sdhci, EMMC_CLOCK_HZ) == 0);
	CHECK(drv_rpi_sdio_host_set_clock(&host, 25000000U) == 0);
	CHECK(drv_rpi_mmc_irq_add(MMC_IRQ, drv_rpi_sdio_host_interrupt, &host) == 0);
	drv_rpi_sdio_host_card_irq_enable(&host, 1);

	/* The card interrupts while the serving thread waits, serves and rearms. */
	smp_stop = 0;
	CHECK(pthread_create(&waiter, NULL, smp_waiter, &host) == 0);
	CHECK(pthread_create(&interrupts, NULL, smp_interrupts, &host) == 0);
	CHECK(pthread_join(interrupts, NULL) == 0);
	CHECK(pthread_join(waiter, NULL) == 0);

	/* Every interrupt was claimed once and served once. */
	CHECK(host.card_irq_count == SMP_CHANGES);
	CHECK(drv_rpi_mmc_irq_remove(MMC_IRQ, drv_rpi_sdio_host_interrupt, &host) == 0);
	printf("  nested dispatcher-to-controller locks: %u\n", nested_dispatch);
	CHECK(nested_dispatch > 0);
}

/* Counts served card interrupts in the smp scenario. */
static volatile unsigned smp_served;

/*
 * Plays the interrupt CPU.  Without a host it fires the line for the
 * dispatcher's slot test; with one it raises the card's interrupt, fires,
 * fires again while disarmed, and waits for the serving thread.
 */
static void *
smp_interrupts(
	void *argument)
{
	struct drv_rpi_sdio_host *host;
	unsigned i;

	host = argument;

	/* The slot test: fire until the changes are done. */
	if (host == NULL) {
		for (i = 0; i < SMP_INTERRUPTS; i++) {
			installed_handler(MMC_IRQ, (kern_irq_ack_t)1, installed_argument);
			if (__atomic_load_n(&smp_stop, __ATOMIC_SEQ_CST) != 0)
				break;
		}
		return NULL;
	}

	/* The card test: one interrupt at a time, each served before the next. */
	for (i = 0; i < SMP_CHANGES; i++) {
		while (__atomic_load_n(&smp_served, __ATOMIC_SEQ_CST) < i)
			sched_yield();
		__atomic_store_n(&card_irq_line, 1, __ATOMIC_SEQ_CST);
		installed_handler(MMC_IRQ, (kern_irq_ack_t)1, installed_argument);
		installed_handler(MMC_IRQ, (kern_irq_ack_t)1, installed_argument);
	}
	return NULL;
}

/* Plays the thread serving the card: waits, serves, rearms. */
static void *
smp_waiter(
	void *argument)
{
	struct drv_rpi_sdio_host *host;
	unsigned i;
	int error;

	host = argument;
	for (i = 0; i < SMP_CHANGES; i++) {
		/* Waits for the interrupt; a deadline only means another look. */
		do {
			error = drv_rpi_sdio_host_card_irq_wait(host, sched_ticks() + 1000U);
		} while (error == ETIMEDOUT);
		CHECK(error == 0);

		/* Serves the card, which drops its line, then rearms. */
		__atomic_store_n(&card_irq_line, 0, __ATOMIC_SEQ_CST);
		drv_rpi_sdio_host_card_irq_rearm(host);
		__atomic_store_n(&smp_served, i + 1U, __ATOMIC_SEQ_CST);
	}
	return NULL;
}

/* The second handler of the slot test: never called once removed. */
static bool
smp_handler(
	void *argument)
{
	(void)argument;
	CHECK(__atomic_load_n(&smp_removed, __ATOMIC_SEQ_CST) == 0);
	return false;
}

/* The dispatcher test's handlers: count, and answer as told. */
static bool
handler_0(
	void *argument)
{
	CHECK(argument == &asked[0]);
	__atomic_fetch_add(&asked[0], 1U, __ATOMIC_SEQ_CST);
	return answers[0];
}

static bool
handler_1(
	void *argument)
{
	CHECK(argument == &asked[1]);
	asked[1]++;
	return answers[1];
}

static bool
handler_2(
	void *argument)
{
	CHECK(argument == &asked[2]);
	asked[2]++;
	return answers[2];
}

/* A waiter that only counts. */
static int
count_sleep(
	void *context,
	unsigned milliseconds)
{
	unsigned *count;

	CHECK(milliseconds > 0);
	count = context;
	(*count)++;
	return 0;
}

/* A waiter that reports a stop once its budget is spent. */
static int
cancel_sleep(
	void *context,
	unsigned milliseconds)
{
	unsigned *budget;

	CHECK(milliseconds > 0);
	budget = context;
	CHECK(*budget > 0);
	(*budget)--;
	if (*budget == 0)
		return ECANCELED;
	return 0;
}

/*
 * The model.
 */

/* A fresh controller and an unpowered card with a Broadcom CIS. */
static void
prepare_model(void)
{
	static const uint8_t cis[] = {
		0x21, 0x02, 0x0c, 0x00,
		0x22, 0x04, 0x00, 0x00, 0x02, 0x32,
		0x20, 0x04, 0xd0, 0x02, 0x45, 0x43,
		0xff
	};

	memset(sdhci, 0, sizeof(sdhci));
	sdhci[R_VERSION / 4U] = 0x99020000U;
	sdhci[R_CAPABILITIES / 4U] = 0x00003200U;
	sdhci_status = 0;
	memset(&card, 0, sizeof(card));
	memset(&transfer, 0, sizeof(transfer));
	card.silent_setting = 2;
	card.busy_setting = 2;
	card.f0[0x00] = 0x43U;
	card.f0[0x08] = 0x13U;
	card.f0[0x09] = 0x00U;
	card.f0[0x0a] = 0x10U;
	card.f0[0x0b] = 0x00U;
	memcpy(card.f0 + CIS_ADDRESS, cis, sizeof(cis));
	pause_owed = 0;
	resets_all = 0;
	resets_lines = 0;
	power_write_count = 0;
	card_irq_line = 0;
}

/* Powers the model card up, fresh, or down. */
static void
power_card(
	int on)
{
	/* Down: it forgets everything. */
	if (!on) {
		card.powered = 0;
		card.ready = 0;
		card.addressed = 0;
		card.selected = 0;
		return;
	}

	/* Up from down: a card just out of reset. */
	if (!card.powered) {
		card.powered = 1;
		card.ready = 0;
		card.addressed = 0;
		card.selected = 0;
		card.silent_cmd5 = card.silent_setting;
		card.busy_cmd5 = card.busy_setting;
		card.f0[0x02] = 0;
		card.f0[0x03] = 0;
		card.f0[0x07] = 0;
	}
}

/* The bus clock the clock control register runs, zero when stopped. */
static uint32_t
model_clock_hz(void)
{
	uint32_t clock;
	uint32_t divisor;

	clock = sdhci[R_CLOCK / 4U] & 0xffffU;
	if ((clock & 0x1U) == 0 || (clock & 0x4U) == 0)
		return 0;
	divisor = ((clock >> 8) & 0xffU) | (((clock >> 6) & 0x3U) << 8);
	if (divisor == 0)
		return EMMC_CLOCK_HZ;
	return EMMC_CLOCK_HZ / (2U * divisor);
}

/* Whether a write must be followed by a pause: the bus stopped or at 400 kHz or below. */
static int
model_slow(void)
{
	uint32_t hz;

	hz = model_clock_hz();
	if (hz == 0 || hz <= 400000U)
		return 1;
	return 0;
}

/* Answers a CMD52 on the model card's spaces. */
static uint32_t
io_direct(
	uint32_t argument)
{
	unsigned write;
	unsigned function;
	uint32_t address;
	uint8_t data;

	write = argument >> 31;
	function = (argument >> 28) & 0x7U;
	address = (argument >> 9) & 0x1ffffU;
	data = (uint8_t)(argument & 0xffU);

	/* A function the card does not have. */
	if (function > 2U)
		return 0x00000200U;

	/* Function 0: the CCCR, the FBRs and the CIS; I/O ready follows F1's enable only. */
	if (function == 0) {
		if (address >= F0_SPAN)
			return 0x00000100U;
		if (write) {
			CHECK(address != 0x03U);
			card.f0[address] = data;
			if (address == 0x02U)
				card.f0[0x03] = (uint8_t)(data & 0x02U);
		}
		return 0x00001000U | card.f0[address];
	}

	/* Function 1: the backplane window. */
	if (function == 1) {
		if (address >= F1_SPAN)
			return 0x00000100U;
		if (write)
			card.f1[address] = data;
		return 0x00001000U | card.f1[address];
	}

	/* Function 2 holds nothing before the chip's firmware runs. */
	return 0x00001000U;
}

/* Starts a CMD53 transfer: checks its argument against the block registers and the bus. */
static void
start_transfer(
	uint32_t mode,
	uint32_t argument)
{
	unsigned write;
	unsigned block_mode;
	unsigned count;
	unsigned size;
	unsigned blocks;
	unsigned function_size;
	unsigned i;

	write = argument >> 31;
	block_mode = (argument >> 27) & 0x1U;
	count = argument & 0x1ffU;
	size = sdhci[R_BLOCK / 4U] & 0xfffU;
	blocks = sdhci[R_BLOCK / 4U] >> 16;

	/* The transfer mode agrees with the argument. */
	CHECK((mode & 0x0002U) != 0);
	CHECK(((mode & 0x0010U) != 0) == (write == 0));
	CHECK(((mode & 0x0020U) != 0) == (blocks > 1U));
	CHECK((mode & 0x0001U) == 0);

	/* Block mode moves blocks of the function's size; byte mode one run of count bytes. */
	transfer.function = (argument >> 28) & 0x7U;
	CHECK(transfer.function == 1U);
	if (block_mode) {
		function_size = card.f0[0x100U * transfer.function + 0x10U] | (card.f0[0x100U * transfer.function + 0x11U] << 8);
		CHECK(size == function_size);
		CHECK(count == blocks);
	} else {
		CHECK(blocks == 1U);
		CHECK(size == (count == 0 ? 512U : count));
	}

	/* A four-line card is driven on four lines. */
	if ((card.f0[0x07] & 0x3U) == 0x2U)
		CHECK((sdhci[R_HOST / 4U] & 0x02U) != 0);

	/* Holds the transfer and, for a read, the bytes it returns. */
	transfer.active = 1;
	transfer.write = (int)write;
	transfer.address = (argument >> 9) & 0x1ffffU;
	transfer.increment = (int)((argument >> 26) & 0x1U);
	transfer.block_size = size;
	transfer.blocks = blocks;
	transfer.done = 0;
	CHECK(size * blocks <= TRANSFER_MAX);
	CHECK(transfer.address + size * blocks <= F1_SPAN);
	if (!write) {
		for (i = 0; i < size * blocks; i++)
			transfer.bytes[i] = card.f1[transfer.address + (transfer.increment ? i : 0)];
		sdhci_status |= S_READ_READY;
	} else {
		sdhci_status |= S_WRITE_READY;
	}
}

/* Carries out the command the driver just wrote, as the controller and the card would. */
static void
execute_command(
	uint32_t value)
{
	uint32_t code;
	uint32_t mode;
	uint32_t argument;
	uint32_t reply;
	unsigned index;

	code = value >> 16;
	mode = value & 0xffffU;
	index = (code >> 8) & 0x3fU;
	argument = sdhci[R_ARGUMENT / 4U];
	CHECK(!transfer.active);

	/* A card without power, or absent, does not answer. */
	if (!card.powered || card.absent) {
		sdhci_status |= S_COMMAND_TIMEOUT;
		return;
	}

	/* Answers the command with its response type, checks and data checked. */
	reply = 0;
	switch (index) {
	case 5:
		CHECK((code & 0xffU) == 0x02U);
		CHECK(mode == 0);
		if (card.silent_cmd5 > 0) {
			card.silent_cmd5--;
			sdhci_status |= S_COMMAND_TIMEOUT;
			return;
		}
		if (argument != 0) {
			CHECK((argument & ~0x00300000U) == 0);
			if (card.busy_cmd5 > 0)
				card.busy_cmd5--;
			else
				card.ready = 1;
		}
		reply = 0x20ff8000U;
		if (card.ready)
			reply |= 0x80000000U;
		break;
	case 3:
		CHECK((code & 0xffU) == 0x1aU);
		CHECK(card.ready);
		card.addressed = 1;
		reply = 0x00010000U;
		break;
	case 7:
		CHECK((code & 0xffU) == 0x1bU);
		CHECK(card.addressed);
		CHECK(argument == 0x00010000U);
		card.selected = 1;
		reply = 0x00001e00U;
		sdhci_status |= S_TRANSFER;
		break;
	case 52:
		CHECK((code & 0xffU) == 0x1aU);
		CHECK(mode == 0);
		CHECK(card.selected);
		reply = io_direct(argument);
		break;
	case 53:
		CHECK((code & 0xffU) == 0x3aU);
		CHECK(card.selected);
		start_transfer(mode, argument);
		reply = 0x00001000U;
		break;
	default:
		CHECK(false);
	}

	/* The response is in place and the command complete. */
	sdhci[R_RESPONSE / 4U] = reply;
	sdhci_status |= S_COMMAND;
}

/* Hands the driver the next word of a read, the ready bit cleared first. */
static uint32_t
read_data_port(void)
{
	uint32_t word;
	unsigned in_block;
	unsigned count;
	unsigned i;

	CHECK(transfer.active && !transfer.write);
	in_block = transfer.done % transfer.block_size;
	if (in_block == 0)
		CHECK((sdhci_status & S_READ_READY) == 0);

	/* The block's next bytes, the last word padded. */
	count = transfer.block_size - in_block;
	if (count > 4U)
		count = 4U;
	word = 0;
	for (i = 0; i < count; i++)
		word |= (uint32_t)transfer.bytes[transfer.done + i] << (8U * i);
	transfer.done += count;

	/* A finished block makes room for the next, or ends the transfer. */
	if (transfer.done % transfer.block_size == 0) {
		if (transfer.done == transfer.block_size * transfer.blocks) {
			transfer.active = 0;
			sdhci_status |= S_TRANSFER;
		} else {
			sdhci_status |= S_READ_READY;
		}
	}
	return word;
}

/* Takes the next word of a write, the ready bit cleared first. */
static void
write_data_port(
	uint32_t value)
{
	unsigned in_block;
	unsigned count;
	unsigned i;

	CHECK(transfer.active && transfer.write);
	in_block = transfer.done % transfer.block_size;
	if (in_block == 0)
		CHECK((sdhci_status & S_WRITE_READY) == 0);

	/* The block's next bytes; padding beyond the block is dropped. */
	count = transfer.block_size - in_block;
	if (count > 4U)
		count = 4U;
	for (i = 0; i < count; i++)
		transfer.bytes[transfer.done + i] = (uint8_t)(value >> (8U * i));
	transfer.done += count;

	/* A finished block asks for the next, or commits the transfer. */
	if (transfer.done % transfer.block_size == 0) {
		if (transfer.done == transfer.block_size * transfer.blocks) {
			for (i = 0; i < transfer.done; i++)
				card.f1[transfer.address + (transfer.increment ? i : 0)] = transfer.bytes[i];
			transfer.active = 0;
			sdhci_status |= S_TRANSFER;
		} else {
			sdhci_status |= S_WRITE_READY;
		}
	}
}

/* Maps the model GPIO block for the GPIO helpers the thread uses. */
static void
prepare_gpio(void)
{
	memset(gpio_block, 0, sizeof(gpio_block));
	CHECK(drv_rpi_gpio_init(&fdt) == 0);
}

/*
 * Damages a tree: every occurrence of text gets a new first character, or,
 * with replacement 0, the first "okay" after the node named text (its
 * status, which comes before its children) becomes "dead", which disables
 * it.
 */
static void
corrupt(
	unsigned char *blob,
	size_t size,
	const char *text,
	char replacement)
{
	size_t length;
	size_t i;
	size_t j;
	int found;

	length = strlen(text) + 1U;
	found = 0;
	for (i = 0; i + length <= size; i++) {
		if (memcmp(blob + i, text, length) != 0)
			continue;

		/* A node's name follows its FDT_BEGIN_NODE token; a path in an alias is not the node. */
		if (replacement == 0 && (i == 0 || blob[i - 1U] != 0x01U))
			continue;
		found = 1;
		if (replacement != 0) {
			blob[i] = (unsigned char)replacement;
			continue;
		}
		for (j = i + length; j + 5U <= size; j++) {
			if (memcmp(blob + j, "okay", 5U) == 0) {
				memcpy(blob + j, "dead", 5U);
				return;
			}
		}
		CHECK(false);
	}
	CHECK(found);
}

/* Runs the Wi-Fi thread's entry on a host thread. */
static void *
wifi_pthread_main(
	void *argument)
{
	(void)argument;
	thread_entry(thread_argument);
	return NULL;
}

/* Waits for the Wi-Fi thread's host thread to end. */
static void
join_wifi(void)
{
	CHECK(wifi_pthread_started);
	CHECK(pthread_join(wifi_pthread, NULL) == 0);
	wifi_pthread_started = 0;
}

/* Reads a whole file. */
static unsigned char *
read_file(
	const char *path,
	size_t *size)
{
	FILE *file;
	unsigned char *bytes;
	long length;

	file = fopen(path, "rb");
	CHECK(file != NULL);
	CHECK(fseek(file, 0, SEEK_END) == 0);
	length = ftell(file);
	CHECK(length > 0);
	CHECK(fseek(file, 0, SEEK_SET) == 0);
	bytes = malloc((size_t)length);
	CHECK(bytes != NULL);
	CHECK(fread(bytes, 1, (size_t)length, file) == (size_t)length);
	fclose(file);
	*size = (size_t)length;
	return bytes;
}

/* Finds, or makes, the host mutex that stands for a kernel mutex. */
static pthread_mutex_t *
find_mutex(
	struct mutex *mutex)
{
	pthread_mutex_t *host;
	unsigned i;

	host = NULL;
	pthread_mutex_lock(&mutex_table_lock);
	for (i = 0; i < mutex_count; i++) {
		if (mutexes[i].mutex == mutex)
			host = &mutexes[i].host;
	}
	if (host == NULL) {
		CHECK(mutex_count < 8U);
		mutexes[mutex_count].mutex = mutex;
		pthread_mutex_init(&mutexes[mutex_count].host, NULL);
		host = &mutexes[mutex_count].host;
		mutex_count++;
	}
	pthread_mutex_unlock(&mutex_table_lock);
	return host;
}

/*
 * The kernel services the drivers call.
 */

/* Reads a model register: the GPIO block or the controller. */
uint32_t
kern_mmio_read32(
	const volatile void *address)
{
	const volatile uint8_t *byte;
	uint32_t value;
	unsigned offset;

	byte = address;
	if (byte >= (const volatile uint8_t *)gpio_block && byte < (const volatile uint8_t *)gpio_block + GPIO_SPAN) {
		offset = (unsigned)(byte - (const volatile uint8_t *)gpio_block);
		return gpio_block[offset / 4U];
	}

	CHECK(byte >= (const volatile uint8_t *)sdhci && byte < (const volatile uint8_t *)sdhci + SDHCI_SPAN);
	offset = (unsigned)(byte - (const volatile uint8_t *)sdhci);
	CHECK(offset % 4U == 0);
	switch (offset) {
	case R_STATUS:
		status_reads++;
		value = sdhci_status;
		if (__atomic_load_n(&card_irq_line, __ATOMIC_SEQ_CST) != 0 && (sdhci[R_STATUS_ENABLE / 4U] & S_CARD) != 0)
			value |= S_CARD;
		if ((value >> 16) != 0)
			value |= S_ERROR;
		return value;
	case R_DATA:
		return read_data_port();
	case R_PRESENT:
		return 0;
	default:
		return sdhci[offset / 4U];
	}
}

/* Writes a model register, holding the controller to its pause after a slow write. */
void
kern_mmio_write32(
	volatile void *address,
	uint32_t value)
{
	volatile uint8_t *byte;
	unsigned offset;
	uint32_t reset;

	byte = address;
	if (byte >= (volatile uint8_t *)gpio_block && byte < (volatile uint8_t *)gpio_block + GPIO_SPAN) {
		offset = (unsigned)(byte - (volatile uint8_t *)gpio_block);
		gpio_block[offset / 4U] = value;
		return;
	}

	CHECK(byte >= (volatile uint8_t *)sdhci && byte < (volatile uint8_t *)sdhci + SDHCI_SPAN);
	offset = (unsigned)(byte - (volatile uint8_t *)sdhci);
	CHECK(offset % 4U == 0);

	/* Two writes without the pause the slow bus needs between them. */
	CHECK(!pause_owed);

	switch (offset) {
	case R_CLOCK:
		reset = value >> 24;
		if ((reset & 0x1U) != 0) {
			resets_all++;
			memset(sdhci, 0, R_VERSION);
			sdhci[R_VERSION / 4U] = 0x99020000U;
			sdhci[R_CAPABILITIES / 4U] = 0x00003200U;
			sdhci_status = 0;
			transfer.active = 0;
			break;
		}
		if ((reset & 0x6U) != 0) {
			resets_lines++;
			transfer.active = 0;
		}
		value &= 0x00ffffffU;
		if ((value & 0x1U) != 0)
			value |= 0x2U;
		sdhci[R_CLOCK / 4U] = value;
		break;
	case R_STATUS:
		sdhci_status &= ~value;
		break;
	case R_HOST:
		CHECK(power_write_count < 16U);
		power_writes[power_write_count] = (value >> 8) & 0xffU;
		power_write_count++;
		sdhci[R_HOST / 4U] = value;
		break;
	case R_COMMAND:
		sdhci[R_COMMAND / 4U] = value;
		execute_command(value);
		break;
	case R_DATA:
		write_data_port(value);
		break;
	default:
		sdhci[offset / 4U] = value;
		break;
	}

	/* A slow or stopped bus owes a pause before the next write. */
	pause_owed = model_slow();
}

/* Maps the GPIO block or the controller to the model's registers. */
int
kern_device_map(
	uint64_t address,
	size_t size,
	unsigned attributes,
	void **mapped)
{
	CHECK(attributes == KERN_DEVICE_UNCACHED);
	if (address == GPIO_BASE) {
		CHECK(size == GPIO_SPAN);
		*mapped = gpio_block;
		return 0;
	}
	CHECK(address == SDHCI_BASE);
	CHECK(size == SDHCI_SPAN);
	*mapped = sdhci;
	return 0;
}

/* Finds the device tree. */
void *
kern_pmem_to_kernel(
	hal_physaddr_t address)
{
	CHECK(address == FDT_PHYSICAL);
	return tree_blob;
}

/* No monotonic counter: the host's pause falls back to kern_usleep_range, which the model checks. */
bool
kern_rtc_read_counter(
	uint64_t *counter,
	uint64_t *frequency_hz)
{
	*counter = 0;
	*frequency_hz = 0;
	return false;
}

/* A busy delay: it pays a pause the slow bus is owed, of two of its cycles. */
void
kern_usleep_range(
	unsigned min_us,
	unsigned max_us)
{
	uint32_t hz;
	unsigned needed;

	CHECK(min_us <= max_us);
	if (pause_owed) {
		hz = model_clock_hz();
		needed = 1U;
		if (hz != 0)
			needed = (2000000U + hz - 1U) / hz;
		CHECK(min_us >= needed);
		pause_owed = 0;
	}
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

/* Prepares a spinlock: the drivers' locks are all of the device rank. */
void
spin_init(
	struct spinlock *lock,
	enum lock_rank rank,
	const char *name)
{
	CHECK(rank == LOCK_RANK_DEVICE);
	lock->rank = rank;
	lock->name = name;
	lock->owner_cpu = 0;
	lock->owner_valid = 0;
}

/*
 * Takes a spinlock: the big lock.  The dispatcher's lock may hold a
 * controller's interrupt lock inside it, never the other way round.
 */
unsigned long
spin_lock_irqsave(
	struct spinlock *lock)
{
	unsigned i;

	CHECK(lock->rank == LOCK_RANK_DEVICE);
	CHECK(lock->name != NULL);
	for (i = 0; i < held_depth; i++) {
		CHECK(held_names[i] != lock->name);
		if (strcmp(lock->name, "rpi4 mmc irq") == 0)
			CHECK(strcmp(held_names[i], "rpi4 sdio irq") != 0);
		if (strcmp(lock->name, "rpi4 sdio irq") == 0 && strcmp(held_names[i], "rpi4 mmc irq") == 0)
			nested_dispatch++;
	}
	pthread_mutex_lock(&big_lock);
	CHECK(held_depth < 4U);
	held_names[held_depth] = lock->name;
	held_depth++;
	return 1UL;
}

/* Gives a spinlock back, innermost first. */
void
spin_unlock_irqrestore(
	struct spinlock *lock,
	unsigned long enabled)
{
	CHECK(enabled == 1UL);
	CHECK(held_depth > 0);
	CHECK(held_names[held_depth - 1U] == lock->name);
	held_depth--;
	pthread_mutex_unlock(&big_lock);
}

/* Prepares a kernel mutex as a host mutex. */
int
mutex_init(
	struct mutex *mutex,
	enum lock_rank rank,
	const char *name)
{
	CHECK(rank == LOCK_RANK_DEVICE);
	CHECK(name != NULL);
	(void)find_mutex(mutex);
	return 0;
}

/* Takes a kernel mutex: never under a spinlock, where it could sleep. */
void
mutex_lock(
	struct mutex *mutex)
{
	CHECK(held_depth == 0);
	pthread_mutex_lock(find_mutex(mutex));
}

/* Gives a kernel mutex back. */
void
mutex_unlock(
	struct mutex *mutex)
{
	pthread_mutex_unlock(find_mutex(mutex));
}

/* Prepares an empty wait queue. */
void
waitq_init(
	struct wait_queue *queue,
	const char *name)
{
	queue->head = NULL;
	queue->tail = NULL;
	queue->sequence = 1;
	queue->name = name;
}

/* Reads a queue's wakeup sequence. */
uint64_t
waitq_sequence(
	const struct wait_queue *queue)
{
	return __atomic_load_n(&queue->sequence, __ATOMIC_SEQ_CST);
}

/*
 * Sleeps under the condition lock, which must be the one spinlock held,
 * until a wakeup or the deadline in milliseconds.
 */
int
waitq_sleep(
	struct wait_queue *queue,
	struct spinlock *condition_lock,
	uint64_t observed,
	uint64_t deadline,
	unsigned flags)
{
	struct timespec until;
	uint64_t nanoseconds;

	CHECK(flags == 0);
	CHECK(held_depth == 1U);
	CHECK(held_names[0] == condition_lock->name);
	if (waitq_sequence(queue) != observed)
		return EAGAIN;

	/* Sleeps on the big lock's condition, with or without a deadline. */
	if (deadline == 0) {
		pthread_cond_wait(&big_wakeup, &big_lock);
	} else {
		nanoseconds = (uint64_t)started.tv_nsec + deadline * 1000000ULL;
		until.tv_sec = started.tv_sec + (time_t)(nanoseconds / 1000000000ULL);
		until.tv_nsec = (long)(nanoseconds % 1000000000ULL);
		pthread_cond_timedwait(&big_wakeup, &big_lock, &until);
	}

	if (waitq_sequence(queue) != observed)
		return 0;
	if (deadline != 0 && sched_ticks() >= deadline)
		return ETIMEDOUT;
	return 0;
}

/* Wakes every sleeper of a queue; the waker holds the condition lock. */
void
waitq_wake_all(
	struct wait_queue *queue)
{
	CHECK(held_depth > 0);
	__atomic_fetch_add(&queue->sequence, 1U, __ATOMIC_SEQ_CST);
	pthread_cond_broadcast(&big_wakeup);
}

/* Wakes one sleeper; the host wakes them all and they look again. */
void
waitq_wake_one(
	struct wait_queue *queue)
{
	waitq_wake_all(queue);
}

/* The scheduler's clock: milliseconds since the test began. */
uint64_t
sched_ticks(void)
{
	struct timespec now;
	int64_t nanoseconds;

	clock_gettime(CLOCK_MONOTONIC, &now);
	nanoseconds = (int64_t)(now.tv_sec - started.tv_sec) * 1000000000LL + (now.tv_nsec - started.tv_nsec);
	return (uint64_t)nanoseconds / 1000000ULL;
}

/* Records the Wi-Fi thread instead of making one. */
int
kthread_create(
	void (*entry)(void *),
	void *arg,
	int priority,
	struct thread **result)
{
	CHECK(priority == TEST_PRIORITY_DEFAULT);
	CHECK(thread_entry == NULL);
	thread_entry = entry;
	thread_argument = arg;
	fake_thread.detached = 0;
	*result = &fake_thread;
	return 0;
}

/* Runs the recorded thread on a host thread; it must have been detached. */
void
thread_start(
	struct thread *thread)
{
	CHECK(thread == &fake_thread);
	CHECK(thread->detached == 1U);
	CHECK(pthread_create(&wifi_pthread, NULL, wifi_pthread_main, NULL) == 0);
	wifi_pthread_started = 1;
}

/* Installs the dispatcher; the first attempt may be told to fail. */
int
kern_irq_register(
	int irq,
	kern_irq_handler_t handler,
	void *argument)
{
	register_calls++;
	if (register_fail_next) {
		register_fail_next = 0;
		return EIO;
	}
	CHECK(installed_handler == NULL);
	installed_irq = irq;
	installed_argument = argument;
	installed_handler = handler;
	return 0;
}

/* The SD line is level high. */
int
kern_irq_set_mode(
	int irq,
	unsigned trigger,
	unsigned polarity)
{
	CHECK(irq == MMC_IRQ);
	CHECK(trigger == KERN_IRQ_TRIGGER_LEVEL);
	CHECK(polarity == KERN_IRQ_POLARITY_HIGH);
	mode_calls++;
	return 0;
}

/* Counts the line's masking. */
void
kern_irq_mask(
	int irq)
{
	CHECK(irq == MMC_IRQ);
	mask_calls++;
}

/* Counts the line's unmasking. */
void
kern_irq_unmask(
	int irq)
{
	CHECK(irq == MMC_IRQ);
	unmask_calls++;
}

/* Counts the ended interrupts. */
void
kern_irq_send_eoi(
	kern_irq_ack_t acknowledge)
{
	CHECK(acknowledge == (kern_irq_ack_t)1);
	__atomic_fetch_add(&eoi_calls, 1U, __ATOMIC_SEQ_CST);
}

/* The mailbox needs no preparation in the model. */
int
drv_rpi4_firmware_init(
	const struct drv_fdt *tree)
{
	CHECK(tree != NULL);
	firmware_inits++;
	return 0;
}

/* Answers the one property the thread asks: the EMMC clock's rate. */
int
drv_rpi4_firmware_property(
	uint32_t tag,
	uint32_t *values,
	unsigned request_count,
	unsigned capacity,
	uint32_t *answered)
{
	CHECK(tag == 0x00030002U);
	CHECK(request_count == 2U && capacity == 2U);
	CHECK(values[0] == 1U);
	values[1] = EMMC_CLOCK_HZ;
	*answered = 8U;
	return 0;
}

/* Reads the model WL_ON; the thread never asks for BT_ON. */
int
drv_rpi4_firmware_get_gpio(
	enum drv_rpi4_firmware_gpio line,
	int *state)
{
	CHECK(line == DRV_RPI4_FIRMWARE_GPIO_WL_ON);
	*state = card.powered;
	return 0;
}

/* Drives the model WL_ON, which powers the card; BT_ON is never touched. */
int
drv_rpi4_firmware_set_gpio(
	enum drv_rpi4_firmware_gpio line,
	int state)
{
	CHECK(line == DRV_RPI4_FIRMWARE_GPIO_WL_ON);
	CHECK(state == 0 || state == 1);
	CHECK(power.count < 16U);
	power.states[power.count] = state;
	power.count++;
	power_card(state);
	return 0;
}

/* WL_ON is an output of the expander. */
int
drv_rpi4_firmware_get_gpio_direction(
	enum drv_rpi4_firmware_gpio line,
	enum drv_rpi4_firmware_gpio_direction *direction)
{
	CHECK(line == DRV_RPI4_FIRMWARE_GPIO_WL_ON);
	*direction = DRV_RPI4_FIRMWARE_GPIO_OUTPUT;
	return 0;
}

/*
 * The BCM43455 driver's stand-ins.  They drive the bus operations the
 * way the driver does at its probe: power up, a CMD52, power down.
 */

/* The root is always there. */
int
bcm43455_files_root_ready(void)
{
	return 1;
}

/* Hands out three small buffers, or ENOENT without the package. */
int
bcm43455_files_load(
	struct bcm43455_files *files)
{
	files_loaded++;
	memset(files, 0, sizeof(*files));
	if (!files_present)
		return ENOENT;
	files->firmware = malloc(4);
	files->clm = malloc(4);
	files->nvram = malloc(4);
	CHECK(files->firmware != NULL && files->clm != NULL && files->nvram != NULL);
	files->firmware_length = 4;
	files->clm_length = 4;
	files->nvram_length = 4;
	return 0;
}

/* Frees what bcm43455_files_load() handed out. */
void
bcm43455_files_release(
	struct bcm43455_files *files)
{
	free(files->firmware);
	free(files->clm);
	free(files->nvram);
	memset(files, 0, sizeof(*files));
}

/* Records the bus and takes the files. */
int
bcm43455_device_create(
	const struct bcm43455_bus_ops *ops,
	void *context,
	struct bcm43455_files *files,
	struct bcm43455_device **result)
{
	CHECK(ops != NULL && files != NULL && files->firmware != NULL);
	bus_ops = ops;
	bus_context = context;
	bcm43455_files_release(files);
	devices_made++;
	*result = (struct bcm43455_device *)&fake_device_token;
	return 0;
}

/* Powers the chip up through the bus, reads the CCCR revision, powers it down. */
int
bcm43455_device_probe(
	struct bcm43455_device *device)
{
	uint8_t value;
	int error;

	CHECK(device == (struct bcm43455_device *)&fake_device_token);
	devices_probed++;
	error = bus_ops->power_up(bus_context);
	if (error == 0) {
		CHECK(bus_ops->read_byte(bus_context, 0, 0x00, &value) == 0);
		probe_cccr = value;
	}
	bus_ops->power_down(bus_context);
	if (error != 0)
		return error;
	return probe_answer;
}

/* Publishes nothing, counting the call. */
int
bcm43455_device_publish(
	struct bcm43455_device *device)
{
	CHECK(device == (struct bcm43455_device *)&fake_device_token);
	devices_published++;
	return 0;
}

/* Counts the device's end. */
void
bcm43455_device_destroy(
	struct bcm43455_device *device)
{
	CHECK(device == (struct bcm43455_device *)&fake_device_token);
	devices_destroyed++;
}
