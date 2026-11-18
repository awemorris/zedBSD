/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Raspberry Pi 4 onboard Wi-Fi: the probe, its publishing thread, and the
 * SDIO bus the BCM43455 driver runs on.
 *
 * The boot probe only reads the device tree and starts one thread, so the
 * boot does not wait on the chip.  The thread first waits for the root
 * file system and reads the firmware package's files; without them it
 * ends having touched no hardware.  Then it prepares the mailbox, reads
 * the controller's clock, routes the SDIO pins, starts the host and adds
 * it to the SD interrupt line, and hands the chip to the BCM43455 driver,
 * which powers it through WL_ON (on the firmware's GPIO expander),
 * enumerates it, learns its identity and publishes it as wlanN.  A
 * published chip keeps the host, the interrupt slot and the power line
 * for as long as the kernel runs; a chip that is not published gives them
 * back.
 *
 * Stopping: drv_rpi4_wifi_stop() sets stopping under the lifecycle mutex
 * and wakes the thread, then lets the mutex go before it waits for the
 * thread to finish.  The thread never takes the lifecycle mutex; every one
 * of its sleeps looks at stopping, and it gives back what it took (the
 * interrupt slot, the controller, the power line) before it reports
 * itself finished.
 */

#include "drivers/platform/rpi4/rpi4-wifi.h"
#include "drivers/platform/rpi4/rpi4-firmware.h"
#include "drivers/platform/rpi-common/rpi-gpio.h"
#include "drivers/platform/rpi-common/rpi-mmc-irq.h"
#include "drivers/platform/rpi-common/rpi-sdio.h"
#include "drivers/wifi/bcm43455/bcm43455.h"
#include <drivers/generic/fdt.h>
#include <kern/atomic.h>
#include <kern/clock.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/pmem.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <kern/waitq.h>
#include <uapi/errno.h>

/*
 * How much of memory at the device tree's address may be read, the same
 * bound the platform uses.
 */
#define WIFI_FDT_READ_LIMIT		(2U * 1024U * 1024U)

/* The firmware's clock rate tag and its clock of the legacy EMMC controller. */
#define FIRMWARE_TAG_GET_CLOCK_RATE	0x00030002U
#define FIRMWARE_CLOCK_EMMC		1U
#define FIRMWARE_CLOCK_WORDS		2U
#define FIRMWARE_CLOCK_ANSWER_BYTES	8U

/* How long the thread waits for the root file system, and how often it looks. */
#define ROOT_WAIT_MS			300000U
#define ROOT_POLL_MS			1000U

/* How long WL_ON stays low for the chip's reset, and how long it settles high. */
#define POWER_LOW_MS			10U
#define POWER_SETTLE_MS			10U

/*
 * The block sizes of function 1 (the chip's backplane) and function 2
 * (its frames), and how long function 1 may take to report ready.
 * Function 2 is enabled only once the chip runs its firmware.
 */
#define FUNCTION1_BLOCK_SIZE		64U
#define FUNCTION2_BLOCK_SIZE		512U
#define FUNCTION1_READY_MS		100U

/* The chip's functions. */
#define FUNCTION_BACKPLANE		1U
#define FUNCTION_FRAMES			2U

/*
 * The steps of the probe.
 *
 * NONE: not probed.  CLAIMED: one caller is probing.  RUNNING: the thread
 * was started and the instance is filled.  REFUSED: the probe found no
 * chip or failed.  Only the caller that moves NONE to CLAIMED probes.
 */
#define PROBE_NONE			0U
#define PROBE_CLAIMED			1U
#define PROBE_RUNNING			2U
#define PROBE_REFUSED			3U

/*
 * The onboard Wi-Fi: its description, its controller and card, the
 * BCM43455 device on them, and the state its thread and
 * drv_rpi4_wifi_stop() share.
 *
 * There is one per board and it lives for as long as the kernel runs.
 * The probe fills it before it starts the thread; afterwards the thread
 * owns everything but the fields under state_lock (stopping, running and
 * the two queues), which drv_rpi4_wifi_stop() changes too.  Once the
 * device is published, the BCM43455 driver owns the host and the card
 * through the bus operations.  lifecycle_lock serializes the publication
 * against drv_rpi4_wifi_stop(); the thread takes it only around the
 * publication, and drv_rpi4_wifi_stop() lets it go before it waits.  The
 * progress flags record what an unpublished chip must give back.
 */
struct rpi4_wifi {
	struct drv_fdt fdt;
	struct drv_rpi_sdio_slot slot;
	struct drv_rpi_sdio_host host;
	struct drv_rpi_sdio_card card;
	struct drv_rpi_sdio_waiter waiter;
	void *registers;
	uint32_t base_clock_hz;
	struct bcm43455_device *device;
	unsigned published;
	unsigned card_reported;

	unsigned host_ready;
	unsigned irq_added;
	unsigned power_touched;

	struct mutex lifecycle_lock;
	struct spinlock state_lock;
	struct wait_queue wake;
	struct wait_queue finished;
	unsigned stopping;
	unsigned running;
};

/*
 * How far the probe has come: one of PROBE_*.
 *
 * drv_rpi4_wifi_stop() looks at wifi_board only once this reads RUNNING
 * with an acquire load, which orders its reads after the probe's filling.
 */
static volatile unsigned wifi_probe_state;

/* The board's one onboard Wi-Fi; see struct rpi4_wifi for who owns which field. */
static struct rpi4_wifi wifi_board;

static void wifi_thread(void *argument);
static int publish(struct rpi4_wifi *wifi);
static int wait_for_root(struct rpi4_wifi *wifi);
static int bring_up(struct rpi4_wifi *wifi);
static int read_controller_clock(struct rpi4_wifi *wifi);
static void report_power_line(void);
static int route_pins(struct rpi4_wifi *wifi);
static int pulse_power(struct rpi4_wifi *wifi);
static int prepare_functions(struct rpi4_wifi *wifi);
static void report_card(struct rpi4_wifi *wifi);
static void give_back(struct rpi4_wifi *wifi);
static int stopping_requested(struct rpi4_wifi *wifi);
static int wifi_sleep(void *context, unsigned milliseconds);
static int bus_read_byte(void *context, unsigned function, uint32_t address, uint8_t *value);
static int bus_write_byte(void *context, unsigned function, uint32_t address, uint8_t value);
static int bus_read(void *context, unsigned function, uint32_t address, int increment, uint8_t *buffer, size_t length);
static int bus_write(void *context, unsigned function, uint32_t address, int increment, uint8_t *buffer, size_t length);
static unsigned bus_block_size(void *context, unsigned function);
static int bus_enable_function(void *context, unsigned function, unsigned timeout_ms);
static int bus_disable_function(void *context, unsigned function);
static int bus_interrupt_wait(void *context, uint64_t deadline);
static void bus_interrupt_enable(void *context, int enable);
static void bus_interrupt_rearm(void *context);
static void bus_interrupt_kick(void *context);
static int bus_power_up(void *context);
static void bus_power_down(void *context);

/* The SDIO bus as the BCM43455 driver sees it: the card layer and the host. */
static const struct bcm43455_bus_ops wifi_bus_operations = {
	bus_read_byte,
	bus_write_byte,
	bus_read,
	bus_write,
	bus_block_size,
	bus_enable_function,
	bus_disable_function,
	wifi_sleep,
	bus_interrupt_wait,
	bus_interrupt_enable,
	bus_interrupt_rearm,
	bus_interrupt_kick,
	bus_power_up,
	bus_power_down
};

/*
 * Looks for the onboard Wi-Fi and starts its thread.
 */
int
drv_rpi4_wifi_probe(
	uint64_t fdt_phys,
	uintptr_t sd_base)
{
	struct rpi4_wifi *wifi;
	struct thread *thread;
	const void *blob;
	unsigned expected;
	unsigned long enabled;
	int claimed;
	int error;

	/* Probes once: a second call finds the first one's result. */
	expected = PROBE_NONE;
	claimed = atomic_raw_compare_exchange(&wifi_probe_state, &expected, PROBE_CLAIMED);
	if (!claimed)
		return EALREADY;
	wifi = &wifi_board;

	/* Finds the device tree, which stays in the direct map for as long as the kernel runs. */
	if (fdt_phys == 0) {
		atomic_raw_store_release(&wifi_probe_state, PROBE_REFUSED);
		return ENODEV;
	}

	/* Maps the tree's address. */
	blob = kern_pmem_to_kernel((kern_physaddr_t)fdt_phys);
	if (blob == NULL) {
		atomic_raw_store_release(&wifi_probe_state, PROBE_REFUSED);
		return ENODEV;
	}

	/* Opens the tree, checking its header. */
	error = drv_fdt_open(&wifi->fdt, blob, WIFI_FDT_READ_LIMIT);
	if (error != 0) {
		atomic_raw_store_release(&wifi_probe_state, PROBE_REFUSED);
		return error;
	}

	/* Finds the chip's slot; QEMU, a disabled slot or the SD card's controller is refused here. */
	error = drv_rpi_sdio_describe_slot(&wifi->fdt, sd_base, &wifi->slot);
	if (error != 0) {
		kern_logf("rpi4-wifi: no onboard Wi-Fi to bring up (%d)\n", error);
		atomic_raw_store_release(&wifi_probe_state, PROBE_REFUSED);
		return error;
	}

	/* Prepares the state the thread and the stop share. */
	error = mutex_init(&wifi->lifecycle_lock, LOCK_RANK_DEVICE, "rpi4 wifi lifecycle");
	if (error != 0) {
		atomic_raw_store_release(&wifi_probe_state, PROBE_REFUSED);
		return error;
	}
	spin_init(&wifi->state_lock, LOCK_RANK_DEVICE, "rpi4 wifi state");
	waitq_init(&wifi->wake, "rpi4 wifi wake");
	waitq_init(&wifi->finished, "rpi4 wifi finished");
	wifi->stopping = 0;
	wifi->running = 0;
	wifi->host_ready = 0;
	wifi->irq_added = 0;
	wifi->power_touched = 0;
	wifi->registers = NULL;
	wifi->base_clock_hz = 0;
	wifi->device = NULL;
	wifi->published = 0;
	wifi->card_reported = 0;
	wifi->waiter.sleep = wifi_sleep;
	wifi->waiter.context = wifi;

	/* Creates the thread that does everything that touches the hardware. */
	error = kthread_create(wifi_thread, wifi, SCHED_PRIORITY_DEFAULT, &thread);
	if (error != 0) {
		kern_logf("rpi4-wifi: thread not created (%d)\n", error);
		atomic_raw_store_release(&wifi_probe_state, PROBE_REFUSED);
		return error;
	}

	/*
	 * running tells drv_rpi4_wifi_stop() that there is a thread to stop
	 * and to wait for; the thread clears it last.
	 */
	enabled = spin_lock_irqsave(&wifi->state_lock);

	wifi->running = 1;

	spin_unlock_irqrestore(&wifi->state_lock, enabled);

	/* RUNNING publishes the filled instance to drv_rpi4_wifi_stop(). */
	atomic_raw_store_release(&wifi_probe_state, PROBE_RUNNING);

	/* Starts the thread; nobody joins it, its retirement releases it. */
	thread->detached = 1;
	thread_start(thread);

	/* Succeeded: the thread brings the chip up in the background. */
	return 0;
}

/*
 * Stops the publishing thread and waits until it has ended.
 */
void
drv_rpi4_wifi_stop(void)
{
	struct rpi4_wifi *wifi;
	unsigned long enabled;
	uint64_t observed;
	unsigned state;

	/* Returns at once when no thread was ever started. */
	state = atomic_raw_load_acquire(&wifi_probe_state);
	if (state != PROBE_RUNNING)
		return;
	wifi = &wifi_board;

	/*
	 * Asks the thread to stop under the lifecycle mutex, then lets the
	 * mutex go before waiting, so a caller that needs it while the thread
	 * finishes is not held up behind this wait.
	 */
	mutex_lock(&wifi->lifecycle_lock);

	enabled = spin_lock_irqsave(&wifi->state_lock);

	if (wifi->running != 0) {
		/* stopping makes every sleep of the thread return ECANCELED. */
		wifi->stopping = 1;
		waitq_wake_all(&wifi->wake);
	}

	spin_unlock_irqrestore(&wifi->state_lock, enabled);

	mutex_unlock(&wifi->lifecycle_lock);

	/* Waits for the thread to give everything back and report itself finished. */
	enabled = spin_lock_irqsave(&wifi->state_lock);

	while (wifi->running != 0) {
		observed = waitq_sequence(&wifi->finished);
		(void)waitq_sleep(&wifi->finished, &wifi->state_lock, observed, 0, 0);
	}

	spin_unlock_irqrestore(&wifi->state_lock, enabled);
}

/* Publishes the chip, or gives everything back when it cannot be. */
static void
wifi_thread(
	void *argument)
{
	struct rpi4_wifi *wifi;
	unsigned long enabled;
	int error;

	wifi = argument;

	/* Brings the chip up and publishes it; a missing package was logged already. */
	error = publish(wifi);
	if (error == ECANCELED)
		kern_logf("rpi4-wifi: bring-up stopped on request\n");
	else if (error != 0 && error != ENOENT)
		kern_logf("rpi4-wifi: onboard Wi-Fi not published (%d)\n", error);

	/*
	 * An unpublished chip leaves the device, the interrupt line, the
	 * controller and its power as they would be without a driver.
	 */
	if (!wifi->published) {
		if (wifi->device != NULL) {
			bcm43455_device_destroy(wifi->device);
			wifi->device = NULL;
		}
		give_back(wifi);
	}

	/* Reports the thread finished to a waiting drv_rpi4_wifi_stop(). */
	enabled = spin_lock_irqsave(&wifi->state_lock);

	wifi->running = 0;
	waitq_wake_all(&wifi->finished);

	spin_unlock_irqrestore(&wifi->state_lock, enabled);
}

/*
 * Waits for the root, reads the files, prepares the host, and has the
 * BCM43455 driver probe and publish the chip.  Hardware is touched only
 * once the files are in memory.
 */
static int
publish(
	struct rpi4_wifi *wifi)
{
	struct bcm43455_files files;
	int error;

	/* Refuses to start when a stop came first. */
	error = stopping_requested(wifi);
	if (error != 0)
		return error;

	/* Waits for the root file system, where the firmware package puts its files. */
	error = wait_for_root(wifi);
	if (error != 0)
		return error;

	/* Reads the files with no lock held; without the package there is no Wi-Fi and nothing is touched. */
	error = bcm43455_files_load(&files);
	if (error == ENOENT) {
		kern_logf("rpi4-wifi: package bcm43455-firmware not installed; no onboard Wi-Fi\n");
		return error;
	}
	if (error != 0)
		return error;

	/* Makes the device, which takes the files. */
	error = bcm43455_device_create(&wifi_bus_operations, wifi, &files, &wifi->device);
	if (error != 0) {
		bcm43455_files_release(&files);
		return error;
	}

	/* Prepares the mailbox, the pins and the host. */
	error = bring_up(wifi);
	if (error != 0)
		return error;

	/* Has the driver bring the chip up once, learn its identity and power it down. */
	error = bcm43455_device_probe(wifi->device);
	if (error != 0)
		return error;

	/* Publishes it, unless a stop came meanwhile. */
	mutex_lock(&wifi->lifecycle_lock);

	error = stopping_requested(wifi);
	if (error == 0)
		error = bcm43455_device_publish(wifi->device);
	if (error == 0)
		wifi->published = 1;

	mutex_unlock(&wifi->lifecycle_lock);

	/* Reports why the chip is not published. */
	if (error != 0)
		return error;

	/* Succeeded: wlanN exists and owns the chip from here on. */
	return 0;
}

/*
 * Waits for the root file system in one-second steps, up to five minutes;
 * a stop ends the wait.
 */
static int
wait_for_root(
	struct rpi4_wifi *wifi)
{
	unsigned waited;
	int ready;
	int error;

	/* Looks for the root until it is there or the bound is spent. */
	for (waited = 0;
	     ;
	     waited += ROOT_POLL_MS) {
		/* Stops as soon as the root is mounted. */
		ready = bcm43455_files_root_ready();
		if (ready)
			break;

		/* The bound is spent without a root. */
		if (waited >= ROOT_WAIT_MS) {
			kern_logf("rpi4-wifi: no root file system after %u s\n", ROOT_WAIT_MS / 1000U);
			return ETIMEDOUT;
		}

		/* Sleeps one step; a stop ends the wait. */
		error = wifi_sleep(wifi, ROOT_POLL_MS);
		if (error != 0)
			return error;
	}

	/* Succeeded: the files can be looked for. */
	return 0;
}

/*
 * Prepares the shared blocks and starts the host; the chip stays
 * unpowered until the driver's power_up.
 */
static int
bring_up(
	struct rpi4_wifi *wifi)
{
	int error;

	/* Prepares the mailbox; a call after the platform's does nothing. */
	error = drv_rpi4_firmware_init(&wifi->fdt);
	if (error != 0) {
		kern_logf("rpi4-wifi: mailbox unavailable (%d)\n", error);
		return error;
	}

	/* Reads the clock the firmware feeds the controller. */
	error = read_controller_clock(wifi);
	if (error != 0)
		return error;

	/* Records how the firmware left the chip's power line. */
	report_power_line();

	/* Routes GPIO 34 to 39 to the controller. */
	error = route_pins(wifi);
	if (error != 0)
		return error;

	/* Maps the controller's registers. */
	error = kern_device_map(
		wifi->slot.registers,
		DRV_RPI_SDIO_HOST_REGISTER_SPAN,
		KERN_DEVICE_UNCACHED,
		&wifi->registers);
	if (error != 0) {
		kern_logf("rpi4-wifi: controller registers not mapped (%d)\n", error);
		return error;
	}

	/* Resets the controller and starts the 400 kHz bus. */
	error = drv_rpi_sdio_host_init(&wifi->host, wifi->registers, wifi->base_clock_hz);
	if (error != 0)
		return error;
	wifi->host_ready = 1;

	/* Joins the SD interrupt line; the card's interrupt stays disarmed until the chip's driver arms it. */
	error = drv_rpi_mmc_irq_add(wifi->slot.irq, drv_rpi_sdio_host_interrupt, &wifi->host);
	if (error != 0) {
		kern_logf("rpi4-wifi: SD interrupt line %d not joined (%d)\n", wifi->slot.irq, error);
		return error;
	}
	wifi->irq_added = 1;

	/* Succeeded: the host can reach the chip once it is powered. */
	return 0;
}

/*
 * Reads the rate of the clock the firmware feeds the legacy EMMC
 * controller, which the controller's capabilities do not state reliably.
 */
static int
read_controller_clock(
	struct rpi4_wifi *wifi)
{
	uint32_t values[FIRMWARE_CLOCK_WORDS];
	uint32_t answered;
	int error;

	/* Asks for the EMMC clock's rate. */
	values[0] = FIRMWARE_CLOCK_EMMC;
	values[1] = 0;
	answered = 0;
	error = drv_rpi4_firmware_property(
		FIRMWARE_TAG_GET_CLOCK_RATE,
		values,
		FIRMWARE_CLOCK_WORDS,
		FIRMWARE_CLOCK_WORDS,
		&answered);
	if (error != 0) {
		kern_logf("rpi4-wifi: EMMC clock rate not read (%d)\n", error);
		return error;
	}

	/* Refuses an answer without the rate, about another clock, or a stopped clock. */
	if (answered < FIRMWARE_CLOCK_ANSWER_BYTES || values[0] != FIRMWARE_CLOCK_EMMC || values[1] == 0) {
		kern_logf(
			"rpi4-wifi: EMMC clock answer unusable (%u bytes, clock %u, %u Hz)\n",
			answered,
			values[0],
			values[1]);
		return EIO;
	}

	/* Succeeded: the host divides this clock. */
	wifi->base_clock_hz = values[1];
	kern_logf("rpi4-wifi: EMMC clock %u Hz\n", wifi->base_clock_hz);
	return 0;
}

/*
 * Logs the level and direction the firmware left WL_ON at, which decides
 * whether the power pulse is needed at all (still unknown on a board).
 */
static void
report_power_line(void)
{
	enum drv_rpi4_firmware_gpio_direction direction;
	int level;
	int level_error;
	int direction_error;

	/* Reads the level and the direction; either may be refused by an older firmware. */
	level = 0;
	level_error = drv_rpi4_firmware_get_gpio(DRV_RPI4_FIRMWARE_GPIO_WL_ON, &level);
	direction = DRV_RPI4_FIRMWARE_GPIO_INPUT;
	direction_error = drv_rpi4_firmware_get_gpio_direction(DRV_RPI4_FIRMWARE_GPIO_WL_ON, &direction);

	/* Records both answers, or why they are missing. */
	kern_logf(
		"rpi4-wifi: WL_ON was level %d (%d), direction %s (%d)\n",
		level,
		level_error,
		direction == DRV_RPI4_FIRMWARE_GPIO_OUTPUT ? "output" : "input",
		direction_error);
}

/*
 * Gives each pin of the slot its pull and the controller's function, as
 * the device tree's pin group says, logging what each pin was before.
 */
static int
route_pins(
	struct rpi4_wifi *wifi)
{
	enum drv_rpi_gpio_function before;
	unsigned pin;
	unsigned i;
	int error;

	/* Routes the pins one by one. */
	for (i = 0; i < wifi->slot.pin_count; i++) {
		pin = wifi->slot.pins[i];

		/* Reads what the firmware left the pin as. */
		error = drv_rpi_gpio_get_function(pin, &before);
		if (error != 0) {
			kern_logf("rpi4-wifi: GPIO %u not readable (%d)\n", pin, error);
			return error;
		}

		/* Sets the pull before the function, so the line never floats as the bus. */
		error = drv_rpi_gpio_set_pull(pin, wifi->slot.pulls[i]);
		if (error != 0)
			return error;

		/* Hands the pin to the bus controller as its function. */
		error = drv_rpi_gpio_set_function(pin, wifi->slot.function);
		if (error != 0)
			return error;

		/* Records the change for the first bring-up on a board. */
		kern_logf(
			"rpi4-wifi: GPIO %u function %u -> %u, pull %u\n",
			pin,
			(unsigned)before,
			(unsigned)wifi->slot.function,
			(unsigned)wifi->slot.pulls[i]);
	}

	/* Succeeded: the bus reaches the chip. */
	return 0;
}

/*
 * Drives WL_ON low and high again, which resets the chip, and lets it
 * settle before the first command.
 */
static int
pulse_power(
	struct rpi4_wifi *wifi)
{
	int error;

	/* Holds the chip in reset; from here the chip must be powered down when the thread ends. */
	wifi->power_touched = 1;
	error = drv_rpi4_firmware_set_gpio(DRV_RPI4_FIRMWARE_GPIO_WL_ON, 0);
	if (error != 0) {
		kern_logf("rpi4-wifi: WL_ON not driven low (%d)\n", error);
		return error;
	}

	/* Keeps the reset long enough for the chip's regulators to drain. */
	error = wifi_sleep(wifi, POWER_LOW_MS);
	if (error != 0)
		return error;

	/* Releases the reset. */
	error = drv_rpi4_firmware_set_gpio(DRV_RPI4_FIRMWARE_GPIO_WL_ON, 1);
	if (error != 0) {
		kern_logf("rpi4-wifi: WL_ON not driven high (%d)\n", error);
		return error;
	}

	/* Lets the chip come up before CMD5, which retries for a second anyway. */
	error = wifi_sleep(wifi, POWER_SETTLE_MS);
	if (error != 0)
		return error;

	/* Succeeded: the chip is out of reset. */
	return 0;
}

/*
 * Enables the backplane function and sets the block sizes of both
 * functions.  The frame function needs the chip's firmware running and is
 * enabled by the chip's driver.
 */
static int
prepare_functions(
	struct rpi4_wifi *wifi)
{
	int error;

	/* Refuses a chip with fewer functions than the BCM43455's two. */
	if (wifi->card.ocr.function_count < FUNCTION_FRAMES) {
		kern_logf("rpi4-wifi: card has %u functions, not 2\n", wifi->card.ocr.function_count);
		return ENODEV;
	}

	/* Enables the backplane, through which the chip's memory is reached. */
	error = drv_rpi_sdio_enable_function(&wifi->card, FUNCTION_BACKPLANE, &wifi->waiter, FUNCTION1_READY_MS);
	if (error != 0) {
		kern_logf("rpi4-wifi: F1 not ready (%d)\n", error);
		return error;
	}

	/* Sets the block sizes CMD53 uses for each function. */
	error = drv_rpi_sdio_set_block_size(&wifi->card, FUNCTION_BACKPLANE, FUNCTION1_BLOCK_SIZE);
	if (error != 0) {
		kern_logf("rpi4-wifi: F1 block size not set (%d)\n", error);
		return error;
	}

	/* Sets the block size CMD53 uses for the frames. */
	error = drv_rpi_sdio_set_block_size(&wifi->card, FUNCTION_FRAMES, FUNCTION2_BLOCK_SIZE);
	if (error != 0) {
		kern_logf("rpi4-wifi: F2 block size not set (%d)\n", error);
		return error;
	}

	/* Records the functions' state. */
	kern_logf(
		"rpi4-wifi: F1 enabled; block sizes F1 %u, F2 %u\n",
		wifi->card.block_size[FUNCTION_BACKPLANE],
		wifi->card.block_size[FUNCTION_FRAMES]);

	/* Succeeded: the backplane can be reached. */
	return 0;
}

/* Logs what enumeration learned of the card. */
static void
report_card(
	struct rpi4_wifi *wifi)
{
	struct drv_rpi_sdio_card *card;

	card = &wifi->card;

	/* The answer to CMD5 and the address. */
	kern_logf(
		"rpi4-wifi: SDIO card: %u functions, OCR %06x, memory %u, RCA %04x\n",
		card->ocr.function_count,
		card->ocr.voltages,
		card->ocr.memory_present,
		card->rca);

	/* The common registers and the CIS. */
	kern_logf(
		"rpi4-wifi: CCCR %u, SDIO %u, capability %02x, CIS at %06x: manufacturer %04x (%u), card %04x, F0 block %u, speed %02x\n",
		card->cccr_revision,
		card->sdio_revision,
		card->capability,
		card->cis_pointer,
		card->cis.manufacturer,
		card->cis.has_manufacturer,
		card->cis.card,
		card->cis.function0_block_max,
		card->cis.speed_code);

	/* The bus the card now runs on. */
	kern_logf(
		"rpi4-wifi: bus %u lines at %u Hz\n",
		wifi->host.bus_width,
		wifi->host.card_clock_hz);
}

/*
 * Gives back what the thread took for a chip that is not published: the
 * card's interrupt and the slot on the line, the controller, and the
 * chip's power.
 */
static void
give_back(
	struct rpi4_wifi *wifi)
{
	int error;

	/* Stops the controller, which keeps the card's interrupt off the line. */
	if (wifi->host_ready != 0) {
		drv_rpi_sdio_host_stop(&wifi->host);
		wifi->host_ready = 0;
	}

	/* Leaves the line once the controller can no longer raise it. */
	if (wifi->irq_added != 0) {
		error = drv_rpi_mmc_irq_remove(wifi->slot.irq, drv_rpi_sdio_host_interrupt, &wifi->host);
		if (error != 0)
			kern_logf("rpi4-wifi: SD interrupt line not left (%d)\n", error);
		wifi->irq_added = 0;
	}

	/* Powers the chip down; untouched power is left as the firmware set it. */
	if (wifi->power_touched != 0) {
		error = drv_rpi4_firmware_set_gpio(DRV_RPI4_FIRMWARE_GPIO_WL_ON, 0);
		if (error != 0)
			kern_logf("rpi4-wifi: WL_ON not driven low at the end (%d)\n", error);
		else
			kern_logf("rpi4-wifi: chip powered down\n");
		wifi->power_touched = 0;
	}
}

/* Reports ECANCELED once a stop has been asked for. */
static int
stopping_requested(
	struct rpi4_wifi *wifi)
{
	unsigned long enabled;
	unsigned stopping;

	/* Samples the request. */
	enabled = spin_lock_irqsave(&wifi->state_lock);

	stopping = wifi->stopping;

	spin_unlock_irqrestore(&wifi->state_lock, enabled);

	/* Reports a stop that came. */
	if (stopping != 0)
		return ECANCELED;

	/* Succeeded: the thread may go on. */
	return 0;
}

/*
 * Sleeps for the given milliseconds, or less when a stop comes.
 *
 * The card layer's waiter: it returns ECANCELED for a stop, which ends
 * whatever the thread was doing.
 */
static int
wifi_sleep(
	void *context,
	unsigned milliseconds)
{
	struct rpi4_wifi *wifi;
	unsigned long enabled;
	uint64_t deadline;
	uint64_t observed;
	int error;

	wifi = context;

	/* The scheduler tick at which the sleep ends. */
	deadline = sched_ticks() + kern_ms_to_ticks(milliseconds);

	/* Sleeps until the deadline, or until drv_rpi4_wifi_stop() sets stopping. */
	error = 0;
	enabled = spin_lock_irqsave(&wifi->state_lock);

	for (;;) {
		/* Ends the sleep for a stop. */
		if (wifi->stopping != 0) {
			error = ECANCELED;
			break;
		}

		/* Sleeps on the stop's wakeup; EAGAIN means one came meanwhile. */
		observed = waitq_sequence(&wifi->wake);
		error = waitq_sleep(&wifi->wake, &wifi->state_lock, observed, deadline, 0);
		if (error == ETIMEDOUT) {
			error = 0;
			break;
		}
	}

	spin_unlock_irqrestore(&wifi->state_lock, enabled);

	/* Reports a stop. */
	if (error != 0)
		return error;

	/* Succeeded: the time has passed. */
	return 0;
}

/* CMD52 read of one byte of a function. */
static int
bus_read_byte(
	void *context,
	unsigned function,
	uint32_t address,
	uint8_t *value)
{
	struct rpi4_wifi *wifi;
	int error;

	wifi = context;

	/* Reads the byte through the card layer. */
	error = drv_rpi_sdio_read_byte(&wifi->card, function, address, value);
	if (error != 0)
		return error;

	/* Succeeded: value holds the byte. */
	return 0;
}

/* CMD52 write of one byte of a function. */
static int
bus_write_byte(
	void *context,
	unsigned function,
	uint32_t address,
	uint8_t value)
{
	struct rpi4_wifi *wifi;
	int error;

	wifi = context;

	/* Writes the byte through the card layer. */
	error = drv_rpi_sdio_write_byte(&wifi->card, function, address, value);
	if (error != 0)
		return error;

	/* Succeeded: the byte is written. */
	return 0;
}

/* CMD53 read, with an incrementing or a fixed address. */
static int
bus_read(
	void *context,
	unsigned function,
	uint32_t address,
	int increment,
	uint8_t *buffer,
	size_t length)
{
	struct rpi4_wifi *wifi;
	int error;

	wifi = context;

	/* Reads the bytes through the card layer. */
	error = drv_rpi_sdio_transfer(&wifi->card, DRV_RPI_SDIO_DATA_READ, function, address, increment, buffer, length);
	if (error != 0)
		return error;

	/* Succeeded: buffer holds the bytes. */
	return 0;
}

/* CMD53 write, with an incrementing or a fixed address. */
static int
bus_write(
	void *context,
	unsigned function,
	uint32_t address,
	int increment,
	uint8_t *buffer,
	size_t length)
{
	struct rpi4_wifi *wifi;
	int error;

	wifi = context;

	/* Writes the bytes through the card layer. */
	error = drv_rpi_sdio_transfer(&wifi->card, DRV_RPI_SDIO_DATA_WRITE, function, address, increment, buffer, length);
	if (error != 0)
		return error;

	/* Succeeded: the bytes are written. */
	return 0;
}

/* The block size set for a function, zero for none. */
static unsigned
bus_block_size(
	void *context,
	unsigned function)
{
	struct rpi4_wifi *wifi;

	wifi = context;

	/* A function past the card's has no block size. */
	if (function > DRV_RPI_SDIO_FUNCTION_MAX)
		return 0;

	/* Reports the size enumeration and power_up set. */
	return wifi->card.block_size[function];
}

/* Enables a function and waits for it to report ready. */
static int
bus_enable_function(
	void *context,
	unsigned function,
	unsigned timeout_ms)
{
	struct rpi4_wifi *wifi;
	int error;

	wifi = context;

	/* Enables it through the card layer, which sleeps through the thread's waiter. */
	error = drv_rpi_sdio_enable_function(&wifi->card, function, &wifi->waiter, timeout_ms);
	if (error != 0)
		return error;

	/* Succeeded: the function is ready. */
	return 0;
}

/* Disables a function. */
static int
bus_disable_function(
	void *context,
	unsigned function)
{
	struct rpi4_wifi *wifi;
	int error;

	wifi = context;

	/* Disables it through the card layer. */
	error = drv_rpi_sdio_disable_function(&wifi->card, function);
	if (error != 0)
		return error;

	/* Succeeded: the function is off. */
	return 0;
}

/* Waits for the card's interrupt, a kick or the deadline. */
static int
bus_interrupt_wait(
	void *context,
	uint64_t deadline)
{
	struct rpi4_wifi *wifi;
	int waited;

	wifi = context;

	/* Waits on the host's card interrupt. */
	waited = drv_rpi_sdio_host_card_irq_wait(&wifi->host, deadline);
	if (waited != 0)
		return waited;

	/* Succeeded: the card interrupted, and its interrupt is disarmed. */
	return 0;
}

/* Lets the card's interrupt reach the CPU, or keeps it away. */
static void
bus_interrupt_enable(
	void *context,
	int enable)
{
	struct rpi4_wifi *wifi;

	wifi = context;

	/* Applies it in the host. */
	drv_rpi_sdio_host_card_irq_enable(&wifi->host, enable);
}

/* Arms the card's interrupt again after it was served. */
static void
bus_interrupt_rearm(
	void *context)
{
	struct rpi4_wifi *wifi;

	wifi = context;

	/* Rearms it in the host. */
	drv_rpi_sdio_host_card_irq_rearm(&wifi->host);
}

/* Ends the bus thread's wait without an interrupt. */
static void
bus_interrupt_kick(
	void *context)
{
	struct rpi4_wifi *wifi;

	wifi = context;

	/* Kicks the host's wait. */
	drv_rpi_sdio_host_card_irq_kick(&wifi->host);
}

/*
 * Powers the chip from reset and enumerates it: the bus back to one line
 * at 400 kHz, the WL_ON pulse, CMD5 to CMD7 and the move to four lines at
 * 25 MHz, function 1 enabled and both block sizes set.
 */
static int
bus_power_up(
	void *context)
{
	struct rpi4_wifi *wifi;
	int error;

	wifi = context;

	/* Returns the bus to the enumeration settings, which a previous run left fast and wide. */
	error = drv_rpi_sdio_host_set_bus_width(&wifi->host, 1);
	if (error != 0)
		return error;

	/* Returns the bus to the enumeration clock. */
	error = drv_rpi_sdio_host_set_clock(&wifi->host, DRV_RPI_SDIO_HOST_IDENTIFY_HZ);
	if (error != 0)
		return error;

	/* Resets the chip through its power line. */
	error = pulse_power(wifi);
	if (error != 0)
		return error;

	/* Enumerates the SDIO interface up to a selected card on the fast bus; the first time is logged. */
	error = drv_rpi_sdio_enumerate(&wifi->card, &wifi->host, &wifi->waiter);
	if (error != 0)
		return error;
	if (!wifi->card_reported) {
		report_card(wifi);
		wifi->card_reported = 1;
	}

	/* Enables the backplane function and sets both functions' block sizes. */
	error = prepare_functions(wifi);
	if (error != 0)
		return error;

	/* Succeeded: the chip's backplane can be reached. */
	return 0;
}

/* Cuts the chip's power and keeps its interrupt away. */
static void
bus_power_down(
	void *context)
{
	struct rpi4_wifi *wifi;
	int error;

	wifi = context;

	/* Keeps the card's interrupt off the line. */
	drv_rpi_sdio_host_card_irq_enable(&wifi->host, 0);

	/* Holds the chip in reset. */
	error = drv_rpi4_firmware_set_gpio(DRV_RPI4_FIRMWARE_GPIO_WL_ON, 0);
	if (error != 0)
		kern_logf("rpi4-wifi: WL_ON not driven low (%d)\n", error);
}
