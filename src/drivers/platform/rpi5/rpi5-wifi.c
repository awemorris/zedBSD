/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Raspberry Pi 5 onboard Wi-Fi: the probe, its publishing thread, and the
 * SDIO bus the BCM43455 driver runs on.
 *
 * The chip sits on the BCM2712's second SD controller (sdio2), an SD Host
 * Controller 3.0 that the SDIO host of the Pi 4 drives once the
 * controller's own configuration block forces the card present and
 * selects the SD pins, and its
 * power is the wl-on regulator, a GPIO of the gio block.  The boot probe
 * only reads the device tree and starts one thread, so the boot does not
 * wait on the chip.  The thread first waits for the root file system and
 * reads the firmware package's files; without them it ends having touched
 * no hardware.  Then it routes the SDIO and power pins, starts the host on
 * the controller's own interrupt line, and hands the chip to the BCM43455
 * driver, which powers it through WL_ON, enumerates it, learns its
 * identity and publishes it as wlanN.  A published chip keeps the host,
 * the interrupt slot and the power line for as long as the kernel runs; a
 * chip that is not published gives them back.
 *
 * Stopping: drv_rpi5_wifi_stop() sets stopping under the lifecycle mutex
 * and wakes the thread, then lets the mutex go before it waits for the
 * thread to finish.  The thread never takes the lifecycle mutex; every one
 * of its sleeps looks at stopping, and it gives back what it took before
 * it reports itself finished.
 */

#include "drivers/platform/rpi5/rpi5-wifi.h"
#include "drivers/platform/rpi5/rpi5-gio.h"
#include "drivers/platform/rpi5/rpi5-pinctrl.h"
#include "drivers/platform/rpi5/rpi5-serror.h"
#include "drivers/platform/rpi4/rpi4-mmc-irq.h"
#include "drivers/platform/rpi4/rpi4-sdio.h"
#include "drivers/wifi/bcm43455/bcm43455.h"
#include <drivers/generic/fdt.h>
#include <kern/atomic.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
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

/* The GIC's first shared line and the cells of its interrupt specifier. */
#define GIC_SPI_BASE			32U
#define GIC_INTERRUPT_CELLS		3U
#define GIC_KIND_SPI			0U

/* How long the thread waits for the root file system, and how often it looks. */
#define ROOT_WAIT_MS			300000U
#define ROOT_POLL_MS			1000U

/* How long WL_ON stays off for the chip's reset, and its settling time when the tree gives none. */
#define POWER_LOW_MS			10U
#define POWER_SETTLE_DEFAULT_MS		10U
#define POWER_SETTLE_MAX_MS		1000U

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
 * The length of an Ethernet address, and the octets that number one
 * board's interfaces: the Raspberry Pi gives the Wi-Fi the address after
 * the Ethernet's.
 */
#define ADDRESS_LENGTH			6U
#define ADDRESS_SERIAL_FIRST		3U

/*
 * The BCM2712 SD controller's own configuration block (the node's second
 * reg, "cfg"): the card detect the controller sees, which a non-removable
 * card forces to present (a controller that sees no card keeps the bus
 * unpowered and unclocked), and the pins the controller drives, the SD
 * ones rather than the eMMC ones.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y
 * drivers/mmc/host/sdhci-brcmstb.c (SDIO_CFG_CTRL with its
 * SDCD_N_TEST_EN and SDCD_N_TEST_LEV bits, SDIO_CFG_SD_PIN_SEL with its
 * SD and MMC selectors, and their use in sdhci_brcmstb_cfginit_2712() and
 * sdhci_bcm2712_set_clock()).
 */
#define SDIO_CFG_CTRL			0x00U
#define SDIO_CFG_CTRL_CD_TEST_ENABLE	(1U << 31)
#define SDIO_CFG_CTRL_CD_TEST_LEVEL	(1U << 30)
#define SDIO_CFG_SD_PIN_SEL		0x44U
#define SDIO_CFG_SD_PIN_SEL_MASK	0x3U
#define SDIO_CFG_SD_PIN_SEL_SD		0x2U
#define SDIO_CFG_REGISTER_SPAN		0x48U

/* The host's present state register, read for the bring-up's log, and its command line's level. */
#define SDHCI_PRESENT_STATE		0x24U
#define SDHCI_PRESENT_CMD_LEVEL		(1U << 24)
#define SDHCI_HOST_CONTROL		0x28U
#define SDHCI_CLOCK_CONTROL		0x2cU

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
 * drv_rpi5_wifi_stop() share.
 *
 * There is one per board and it lives for as long as the kernel runs.
 * The probe fills it before it starts the thread; afterwards the thread
 * owns everything but the fields under state_lock (stopping, running and
 * the two queues), which drv_rpi5_wifi_stop() changes too.  Once the
 * device is published, the BCM43455 driver owns the host and the card
 * through the bus operations.  lifecycle_lock serializes the publication
 * against drv_rpi5_wifi_stop(); the thread takes it only around the
 * publication, and drv_rpi5_wifi_stop() lets it go before it waits.  The
 * progress flags record what an unpublished chip must give back.
 */
struct rpi5_wifi {
	struct drv_fdt fdt;
	uint32_t chip_node;
	uint32_t controller_node;
	uint32_t regulator_node;
	uint32_t gio_node;
	uint64_t registers_phys;
	uint64_t cfg_phys;
	int irq;
	uint32_t base_clock_hz;
	unsigned power_pin;
	unsigned power_active_high;
	unsigned power_settle_ms;
	uint8_t mac[ADDRESS_LENGTH];
	unsigned has_mac;
	struct drv_rpi5_gio gio;
	struct drv_rpi4_sdio_host host;
	struct drv_rpi4_sdio_card card;
	struct drv_rpi4_sdio_waiter waiter;
	void *registers;
	void *cfg_registers;
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
 * drv_rpi5_wifi_stop() looks at wifi_board only once this reads RUNNING
 * with an acquire load, which orders its reads after the probe's filling.
 */
static volatile unsigned wifi_probe_state;

/* The board's one onboard Wi-Fi; see struct rpi5_wifi for who owns which field. */
static struct rpi5_wifi wifi_board;

static int describe(struct rpi5_wifi *wifi);
static int describe_interrupt(struct rpi5_wifi *wifi);
static int describe_clock(struct rpi5_wifi *wifi);
static int describe_power(struct rpi5_wifi *wifi);
static void describe_address(struct rpi5_wifi *wifi);
static int read_tree_address(struct rpi5_wifi *wifi, uint32_t node, uint8_t mac[ADDRESS_LENGTH]);
static void wifi_thread(void *argument);
static int publish(struct rpi5_wifi *wifi);
static int wait_for_root(struct rpi5_wifi *wifi);
static int bring_up(struct rpi5_wifi *wifi);
static void report_power_line(struct rpi5_wifi *wifi);
static int configure_controller(struct rpi5_wifi *wifi);
static void report_host(struct rpi5_wifi *wifi, const char *when);
static void report_pin_selections(struct rpi5_wifi *wifi);
static int route_pins(struct rpi5_wifi *wifi);
static int set_power(struct rpi5_wifi *wifi, int on);
static int pulse_power(struct rpi5_wifi *wifi);
static int prepare_functions(struct rpi5_wifi *wifi);
static void report_card(struct rpi5_wifi *wifi);
static void give_back(struct rpi5_wifi *wifi);
static int stopping_requested(struct rpi5_wifi *wifi);
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
drv_rpi5_wifi_probe(
	uint64_t fdt_phys)
{
	struct rpi5_wifi *wifi;
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

	/* Finds the chip, its controller and its power line. */
	error = describe(wifi);
	if (error != 0) {
		kern_logf("rpi5-wifi: no onboard Wi-Fi to bring up (%d)\n", error);
		atomic_raw_store_release(&wifi_probe_state, PROBE_REFUSED);
		return error;
	}

	/* Prepares the state the thread and the stop share. */
	error = mutex_init(&wifi->lifecycle_lock, LOCK_RANK_DEVICE, "rpi5 wifi lifecycle");
	if (error != 0) {
		atomic_raw_store_release(&wifi_probe_state, PROBE_REFUSED);
		return error;
	}

	/* Prepares the state's lock and wait queues. */
	spin_init(&wifi->state_lock, LOCK_RANK_DEVICE, "rpi5 wifi state");
	waitq_init(&wifi->wake, "rpi5 wifi wake");
	waitq_init(&wifi->finished, "rpi5 wifi finished");
	wifi->stopping = 0;
	wifi->running = 0;
	wifi->host_ready = 0;
	wifi->irq_added = 0;
	wifi->power_touched = 0;
	wifi->registers = NULL;
	wifi->device = NULL;
	wifi->published = 0;
	wifi->card_reported = 0;
	wifi->waiter.sleep = wifi_sleep;
	wifi->waiter.context = wifi;

	/* Creates the thread that does everything that touches the hardware. */
	error = kthread_create(wifi_thread, wifi, SCHED_PRIORITY_DEFAULT, &thread);
	if (error != 0) {
		kern_logf("rpi5-wifi: thread not created (%d)\n", error);
		atomic_raw_store_release(&wifi_probe_state, PROBE_REFUSED);
		return error;
	}

	/*
	 * running tells drv_rpi5_wifi_stop() that there is a thread to stop
	 * and to wait for; the thread clears it last.
	 */
	enabled = spin_lock_irqsave(&wifi->state_lock);

	wifi->running = 1;

	spin_unlock_irqrestore(&wifi->state_lock, enabled);

	/* RUNNING publishes the filled instance to drv_rpi5_wifi_stop(). */
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
drv_rpi5_wifi_stop(void)
{
	struct rpi5_wifi *wifi;
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

/*
 * Finds the chip's node, its SD controller (the chip's parent), the
 * controller's interrupt and clock, and the chip's power regulator.
 */
static int
describe(
	struct rpi5_wifi *wifi)
{
	uint64_t size;
	unsigned index;
	bool enabled;
	int error;

	/* Finds the chip. */
	error = drv_fdt_find_compatible(&wifi->fdt, "brcm,bcm4329-fmac", DRV_FDT_NO_NODE, &wifi->chip_node);
	if (error != 0)
		return ENODEV;

	/* Its parent is the SD controller, which must be the BCM2712's and enabled. */
	error = drv_fdt_parent(&wifi->fdt, wifi->chip_node, &wifi->controller_node);
	if (error != 0)
		return error;
	error = drv_fdt_string_index(&wifi->fdt, wifi->controller_node, "compatible", "brcm,bcm2712-sdhci", &index);
	if (error != 0)
		return ENODEV;
	enabled = drv_fdt_node_enabled(&wifi->fdt, wifi->controller_node);
	if (!enabled)
		return ENODEV;

	/* Finds the controller's registers. */
	error = drv_fdt_reg(&wifi->fdt, wifi->controller_node, 0, &wifi->registers_phys, &size);
	if (error != 0)
		return error;
	if (size < DRV_RPI4_SDIO_HOST_REGISTER_SPAN)
		return ERANGE;

	/* Finds the controller's configuration block; without it the card cannot be forced present. */
	error = drv_fdt_reg(&wifi->fdt, wifi->controller_node, 1U, &wifi->cfg_phys, &size);
	if (error != 0)
		return error;
	if (size < SDIO_CFG_REGISTER_SPAN)
		return ERANGE;

	/* Finds its interrupt line and its clock. */
	error = describe_interrupt(wifi);
	if (error != 0)
		return error;
	error = describe_clock(wifi);
	if (error != 0)
		return error;

	/* Finds the chip's power line. */
	error = describe_power(wifi);
	if (error != 0)
		return error;

	/* Finds the board's address for the chip, whose OTP carries none. */
	describe_address(wifi);

	/* Reports what was found. */
	kern_logf("rpi5-wifi: SDIO host at %llx, line %d, %u Hz; WL_ON gio %u\n",
		  (unsigned long long)wifi->registers_phys,
		  wifi->irq,
		  wifi->base_clock_hz,
		  wifi->power_pin);

	/* Succeeded: the chip can be brought up. */
	return 0;
}

/* Reads the controller's interrupt: a shared GIC line. */
static int
describe_interrupt(
	struct rpi5_wifi *wifi)
{
	const uint8_t *value;
	uint32_t length;
	uint64_t kind;
	uint64_t line;
	int error;

	/* Reads the first specifier. */
	error = drv_fdt_property(&wifi->fdt, wifi->controller_node, "interrupts", &value, &length);
	if (error != 0)
		return error;
	if (length < GIC_INTERRUPT_CELLS * 4U)
		return EINVAL;

	/* Accepts only a shared line. */
	kind = drv_fdt_cells_value(value, 0, 1U);
	if (kind != GIC_KIND_SPI)
		return ENOTSUP;
	line = drv_fdt_cells_value(value, 1U, 1U);
	if (line > 987U)
		return EINVAL;

	/* Succeeded: the line's number after the private ones. */
	wifi->irq = (int)(line + GIC_SPI_BASE);
	return 0;
}

/* Reads the rate of the fixed clock the controller divides. */
static int
describe_clock(
	struct rpi5_wifi *wifi)
{
	const uint8_t *value;
	uint32_t length;
	uint32_t phandle;
	uint32_t clock;
	int error;

	/* Follows the controller's clock to its node. */
	error = drv_fdt_property(&wifi->fdt, wifi->controller_node, "clocks", &value, &length);
	if (error != 0)
		return error;
	if (length < 4U)
		return EINVAL;
	phandle = (uint32_t)drv_fdt_cells_value(value, 0, 1U);
	error = drv_fdt_find_phandle(&wifi->fdt, phandle, &clock);
	if (error != 0)
		return error;

	/* Reads its rate; a clock without one cannot be divided. */
	wifi->base_clock_hz = drv_fdt_node_cells(&wifi->fdt, clock, "clock-frequency", 0);
	if (wifi->base_clock_hz == 0)
		return ENOTSUP;

	/* Succeeded: the host divides this clock. */
	return 0;
}

/*
 * Reads the chip's power line from the controller's vmmc-supply: a fixed
 * regulator whose gpio is a pin of a brcmstb GPIO block, its polarity and
 * its start-up delay.
 */
static int
describe_power(
	struct rpi5_wifi *wifi)
{
	const uint8_t *value;
	uint32_t length;
	uint32_t phandle;
	uint32_t delay_us;
	int error;

	/* Follows vmmc-supply to the regulator. */
	error = drv_fdt_property(&wifi->fdt, wifi->controller_node, "vmmc-supply", &value, &length);
	if (error != 0)
		return error;
	if (length < 4U)
		return EINVAL;
	phandle = (uint32_t)drv_fdt_cells_value(value, 0, 1U);
	error = drv_fdt_find_phandle(&wifi->fdt, phandle, &wifi->regulator_node);
	if (error != 0)
		return error;

	/* Reads the regulator's GPIO: the block's phandle, the pin and its flags. */
	error = drv_fdt_property(&wifi->fdt, wifi->regulator_node, "gpio", &value, &length);
	if (error != 0)
		return error;
	if (length != 12U)
		return EINVAL;
	phandle = (uint32_t)drv_fdt_cells_value(value, 0, 1U);
	error = drv_fdt_find_phandle(&wifi->fdt, phandle, &wifi->gio_node);
	if (error != 0)
		return error;
	wifi->power_pin = (unsigned)drv_fdt_cells_value(value, 1U, 1U);

	/* A fixed regulator is enabled by a low level unless it says active high. */
	error = drv_fdt_property(&wifi->fdt, wifi->regulator_node, "enable-active-high", &value, &length);
	wifi->power_active_high = 0;
	if (error == 0)
		wifi->power_active_high = 1;

	/* Reads how long the chip's power takes to settle. */
	delay_us = drv_fdt_node_cells(&wifi->fdt, wifi->regulator_node, "startup-delay-us", POWER_SETTLE_DEFAULT_MS * 1000U);
	wifi->power_settle_ms = (delay_us + 999U) / 1000U;
	if (wifi->power_settle_ms > POWER_SETTLE_MAX_MS)
		wifi->power_settle_ms = POWER_SETTLE_MAX_MS;

	/* Succeeded: the power line is known. */
	return 0;
}

/*
 * Finds the board's Wi-Fi address.
 *
 * The chip's OTP carries no address on the Pi 5: the boot firmware puts
 * it in the local-mac-address of the chip's node (the tree's wifi0
 * alias), which Linux's brcmfmac gives the chip.  A tree that left it
 * zero gets the address after the Ethernet's local-mac-address, the
 * numbering of the board's interfaces.  Without either the chip keeps its
 * own, and the BCM43455 driver refuses the NVRAM's placeholder.
 */
static void
describe_address(
	struct rpi5_wifi *wifi)
{
	uint8_t ethernet[ADDRESS_LENGTH];
	uint32_t ethernet_node;
	uint32_t serial;
	unsigned index;
	int error;

	/* Takes the chip node's own address. */
	wifi->has_mac = 0;
	error = read_tree_address(wifi, wifi->chip_node, wifi->mac);
	if (error == 0) {
		wifi->has_mac = 1;
		kern_logf("rpi5-wifi: address %02x:%02x:%02x:%02x:%02x:%02x from the tree's Wi-Fi node\n",
			  wifi->mac[0],
			  wifi->mac[1],
			  wifi->mac[2],
			  wifi->mac[3],
			  wifi->mac[4],
			  wifi->mac[5]);
		return;
	}

	/* Finds the Ethernet's node. */
	error = drv_fdt_find_compatible(&wifi->fdt, "cdns,macb", DRV_FDT_NO_NODE, &ethernet_node);
	if (error != 0) {
		kern_logf("rpi5-wifi: no address in the tree's Wi-Fi node and no Ethernet node; the chip keeps its own\n");
		return;
	}

	/* Reads the Ethernet's address. */
	error = read_tree_address(wifi, ethernet_node, ethernet);
	if (error != 0) {
		kern_logf("rpi5-wifi: no address in the tree's Wi-Fi or Ethernet node; the chip keeps its own\n");
		return;
	}

	/* Reads the serial part of the Ethernet's address, its last three octets. */
	serial = 0;
	for (index = ADDRESS_SERIAL_FIRST; index < ADDRESS_LENGTH; index++)
		serial = (serial << 8) | ethernet[index];

	/* Numbers the Wi-Fi after the Ethernet within the same serial part. */
	serial = (serial + 1U) & 0xffffffU;
	kern_memcpy(wifi->mac, ethernet, ADDRESS_SERIAL_FIRST);

	/* Writes the serial part back, its last octet last. */
	for (index = ADDRESS_LENGTH; index > ADDRESS_SERIAL_FIRST; index--) {
		wifi->mac[index - 1U] = (uint8_t)serial;
		serial >>= 8;
	}

	/* Reports the derived address. */
	wifi->has_mac = 1;
	kern_logf("rpi5-wifi: address %02x:%02x:%02x:%02x:%02x:%02x after the Ethernet's (the tree's Wi-Fi node has none)\n",
		  wifi->mac[0],
		  wifi->mac[1],
		  wifi->mac[2],
		  wifi->mac[3],
		  wifi->mac[4],
		  wifi->mac[5]);
}

/*
 * Reads a node's local-mac-address into mac.  Returns ENOENT for a node
 * without one, and EINVAL for one of the wrong length, of all zeros (the
 * tree's own, which the firmware did not fill) or multicast.
 */
static int
read_tree_address(
	struct rpi5_wifi *wifi,
	uint32_t node,
	uint8_t mac[ADDRESS_LENGTH])
{
	const uint8_t *value;
	uint32_t length;
	unsigned nonzero;
	unsigned index;
	int error;

	/* Reads the property. */
	error = drv_fdt_property(&wifi->fdt, node, "local-mac-address", &value, &length);
	if (error != 0)
		return ENOENT;

	/* Refuses a property that is not one address. */
	if (length != ADDRESS_LENGTH)
		return EINVAL;

	/* Looks for a nonzero octet. */
	nonzero = 0;
	for (index = 0; index < ADDRESS_LENGTH; index++)
		nonzero |= value[index];

	/* Refuses the tree's unfilled zeros. */
	if (nonzero == 0)
		return EINVAL;

	/* Refuses a multicast address. */
	if ((value[0] & 0x01U) != 0)
		return EINVAL;

	/* Succeeded: the node names the address. */
	kern_memcpy(mac, value, ADDRESS_LENGTH);
	return 0;
}

/* Publishes the chip, or gives everything back when it cannot be. */
static void
wifi_thread(
	void *argument)
{
	struct rpi5_wifi *wifi;
	unsigned long enabled;
	int error;

	wifi = argument;

	/* Brings the chip up and publishes it; a missing package was logged already. */
	error = publish(wifi);
	if (error == ECANCELED) {
		kern_logf("rpi5-wifi: bring-up stopped on request\n");
	} else if (error != 0 && error != ENOENT) {
		kern_logf("rpi5-wifi: onboard Wi-Fi not published (%d)\n", error);
	}

	/*
	 * An unpublished chip leaves the device, the interrupt line, the
	 * controller and its power as they would be without a driver.
	 */
	if (!wifi->published) {
		if (wifi->device != NULL) {
			bcm43455_device_destroy(wifi->device);
			wifi->device = NULL;
		}

		/* Gives back the controller, the line and the power. */
		give_back(wifi);
	}

	/* Reports the thread finished to a waiting drv_rpi5_wifi_stop(). */
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
	struct rpi5_wifi *wifi)
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
		kern_logf("rpi5-wifi: package bcm43455-firmware not installed; no onboard Wi-Fi\n");
		return error;
	}

	/* Reports a package that could not be read. */
	if (error != 0)
		return error;

	/* Makes the device, which takes the files. */
	error = bcm43455_device_create(&wifi_bus_operations, wifi, &files, &wifi->device);
	if (error != 0) {
		bcm43455_files_release(&files);
		return error;
	}

	/* Gives the chip the board's address, which every start sets. */
	if (wifi->has_mac) {
		error = bcm43455_device_set_address(wifi->device, wifi->mac);
		if (error != 0)
			return error;
	}

	/* Prepares the pins, the power line and the host. */
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
	struct rpi5_wifi *wifi)
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
			kern_logf("rpi5-wifi: no root file system after %u s\n", ROOT_WAIT_MS / 1000U);
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
 * Routes the pins, opens the power line's GPIO block and starts the host;
 * the chip stays unpowered until the driver's power_up.
 */
static int
bring_up(
	struct rpi5_wifi *wifi)
{
	int error;

	/* Names the reading of the files (SD card accesses) if it left an SError. */
	(void)drv_rpi5_serror_check("rpi5-wifi: reading the firmware files");

	/* Opens the GPIO block of the power line. */
	error = drv_rpi5_gio_open(&wifi->fdt, wifi->gio_node, &wifi->gio);
	if (error != 0) {
		kern_logf("rpi5-wifi: WL_ON's GPIO block unavailable (%d)\n", error);
		return error;
	}

	/* Records how the firmware left the chip's power line. */
	report_power_line(wifi);
	(void)drv_rpi5_serror_check("rpi5-wifi: reading WL_ON");

	/* Routes the SDIO pins to the controller and WL_ON to its GPIO. */
	error = route_pins(wifi);
	(void)drv_rpi5_serror_check("rpi5-wifi: routing the pins");
	if (error != 0)
		return error;

	/* Maps the controller's registers. */
	error = kern_device_map(
		wifi->registers_phys,
		DRV_RPI4_SDIO_HOST_REGISTER_SPAN,
		KERN_DEVICE_UNCACHED,
		&wifi->registers);
	if (error != 0) {
		kern_logf("rpi5-wifi: controller registers not mapped (%d)\n", error);
		return error;
	}

	/* Forces the card present and selects the SD pins, before the host powers the bus. */
	error = configure_controller(wifi);
	(void)drv_rpi5_serror_check("rpi5-wifi: the controller configuration");
	if (error != 0)
		return error;

	/* Resets the controller and starts the 400 kHz bus. */
	error = drv_rpi4_sdio_host_init(&wifi->host, wifi->registers, wifi->base_clock_hz);
	(void)drv_rpi5_serror_check("rpi5-wifi: starting the host");
	if (error != 0)
		return error;
	wifi->host_ready = 1;
	report_host(wifi, "started");

	/* Joins the controller's interrupt line; the card's interrupt stays disarmed until the chip's driver arms it. */
	error = drv_rpi4_mmc_irq_add(wifi->irq, drv_rpi4_sdio_host_interrupt, &wifi->host);
	if (error != 0) {
		kern_logf("rpi5-wifi: SD interrupt line %d not joined (%d)\n", wifi->irq, error);
		return error;
	}

	/* Records the slot to give back. */
	wifi->irq_added = 1;

	/* Succeeded: the host can reach the chip once it is powered. */
	return 0;
}

/*
 * Forces the card present in the controller's configuration block and
 * makes the controller drive the SD pins, logging what the firmware left.
 */
static int
configure_controller(
	struct rpi5_wifi *wifi)
{
	volatile uint8_t *cfg;
	uint32_t control;
	uint32_t pins;
	int error;

	/* Maps the block. */
	error = kern_device_map(wifi->cfg_phys, SDIO_CFG_REGISTER_SPAN, KERN_DEVICE_UNCACHED, &wifi->cfg_registers);
	if (error != 0) {
		kern_logf("rpi5-wifi: controller configuration not mapped (%d)\n", error);
		return error;
	}
	cfg = wifi->cfg_registers;

	/* Reads what the firmware left. */
	control = kern_mmio_read32(cfg + SDIO_CFG_CTRL);
	pins = kern_mmio_read32(cfg + SDIO_CFG_SD_PIN_SEL);

	/* Forces the card detect to present: the test enable on, the test level low. */
	kern_mmio_write32(cfg + SDIO_CFG_CTRL,
			  (control & ~SDIO_CFG_CTRL_CD_TEST_LEVEL) | SDIO_CFG_CTRL_CD_TEST_ENABLE);

	/* Selects the SD pins. */
	kern_mmio_write32(cfg + SDIO_CFG_SD_PIN_SEL,
			  (pins & ~SDIO_CFG_SD_PIN_SEL_MASK) | SDIO_CFG_SD_PIN_SEL_SD);

	/* Records the change. */
	kern_logf("rpi5-wifi: controller configuration %08x pins %08x, now %08x pins %08x\n",
		  control,
		  pins,
		  kern_mmio_read32(cfg + SDIO_CFG_CTRL),
		  kern_mmio_read32(cfg + SDIO_CFG_SD_PIN_SEL));

	/* Succeeded: the controller sees the card and reaches its pins. */
	return 0;
}

/*
 * Logs the host's present state (bit 16 the card inserted, bits 20 to 23
 * the data lines' levels, bit 24 the command line's), its power and its
 * clock.
 */
static void
report_host(
	struct rpi5_wifi *wifi,
	const char *when)
{
	volatile uint8_t *registers;

	/* Reads the registers through the host's mapping. */
	registers = wifi->registers;
	kern_logf("rpi5-wifi: host %s: present %08x, control %08x, clock %08x\n",
		  when,
		  kern_mmio_read32(registers + SDHCI_PRESENT_STATE),
		  kern_mmio_read32(registers + SDHCI_HOST_CONTROL),
		  kern_mmio_read32(registers + SDHCI_CLOCK_CONTROL));
}

/*
 * Logs the lines' levels under each of the controller's pin selections
 * when the command line reads low under the SD one; a powered chip and the
 * pull-up hold it high on the pins the controller drives.  The controller
 * takes a selection when its clock starts, so the clock is restarted after
 * each.  The SD selection is restored.
 */
static void
report_pin_selections(
	struct rpi5_wifi *wifi)
{
	volatile uint8_t *cfg;
	volatile uint8_t *registers;
	uint32_t pins;
	uint32_t present[SDIO_CFG_SD_PIN_SEL_MASK + 1U];
	unsigned selection;

	registers = wifi->registers;
	cfg = wifi->cfg_registers;

	/* Nothing to learn when the command line is high. */
	if ((kern_mmio_read32(registers + SDHCI_PRESENT_STATE) & SDHCI_PRESENT_CMD_LEVEL) != 0)
		return;

	/* Reads the lines under each selection. */
	pins = kern_mmio_read32(cfg + SDIO_CFG_SD_PIN_SEL) & ~SDIO_CFG_SD_PIN_SEL_MASK;
	for (selection = 0; selection <= SDIO_CFG_SD_PIN_SEL_MASK; selection++) {
		kern_mmio_write32(cfg + SDIO_CFG_SD_PIN_SEL, pins | selection);
		(void)drv_rpi4_sdio_host_set_clock(&wifi->host, DRV_RPI4_SDIO_HOST_IDENTIFY_HZ);
		present[selection] = kern_mmio_read32(registers + SDHCI_PRESENT_STATE);
	}

	/* Restores the SD pins. */
	kern_mmio_write32(cfg + SDIO_CFG_SD_PIN_SEL, pins | SDIO_CFG_SD_PIN_SEL_SD);
	(void)drv_rpi4_sdio_host_set_clock(&wifi->host, DRV_RPI4_SDIO_HOST_IDENTIFY_HZ);

	/* Records the levels. */
	kern_logf("rpi5-wifi: command line low; present by pin selection 0-3: %08x %08x %08x %08x\n",
		  present[0],
		  present[1],
		  present[2],
		  present[3]);
}

/*
 * Logs WL_ON's level and direction: as the firmware left it, and as the
 * chip's power-up left it.
 */
static void
report_power_line(
	struct rpi5_wifi *wifi)
{
	uint32_t data;
	uint32_t direction;
	int level;
	int output;
	int error;

	/* Reads the pin. */
	level = 0;
	output = 0;
	error = drv_rpi5_gio_get(&wifi->gio, wifi->power_pin, &level, &output);

	/* Records the answer, or why it is missing. */
	kern_logf("rpi5-wifi: WL_ON level %d, %s (%d)\n",
		  level,
		  output ? "output" : "input",
		  error);

	/*
	 * Records the levels of the whole bank, which holds the SDIO lines
	 * (GPIO 30 to 35) too, as the pads see them.
	 */
	data = 0;
	direction = 0;
	error = drv_rpi5_gio_get_bank(&wifi->gio, wifi->power_pin, &data, &direction);
	if (error == 0)
		kern_logf("rpi5-wifi: GPIO bank levels %08x, inputs %08x\n", data, direction);
}

/*
 * Gives the controller's pins their function and pulls, and the power
 * line its GPIO function, as the device tree's pin groups say.
 */
static int
route_pins(
	struct rpi5_wifi *wifi)
{
	int error;

	/* Finds the pin controller and records how the firmware left it. */
	error = drv_rpi5_pinctrl_init(&wifi->fdt);
	if (error != 0)
		return error;
	drv_rpi5_pinctrl_report("before");

	/* Routes the SDIO pins. */
	error = drv_rpi5_pinctrl_apply_default(&wifi->fdt, wifi->controller_node);
	if (error != 0) {
		kern_logf("rpi5-wifi: SDIO pins not routed (%d)\n", error);
		return error;
	}

	/* Routes the power line. */
	error = drv_rpi5_pinctrl_apply_default(&wifi->fdt, wifi->regulator_node);
	if (error != 0) {
		kern_logf("rpi5-wifi: WL_ON pin not routed (%d)\n", error);
		return error;
	}

	/* Succeeded: the bus and the power line reach the chip. */
	drv_rpi5_pinctrl_report("after");
	kern_logf("rpi5-wifi: SDIO and WL_ON pins routed\n");
	return 0;
}

/* Drives WL_ON to power the chip on or off, by the regulator's polarity. */
static int
set_power(
	struct rpi5_wifi *wifi,
	int on)
{
	int level;
	int error;

	/* The level that means on is the regulator's enable level. */
	level = 0;
	if (on != 0 && wifi->power_active_high)
		level = 1;
	if (on == 0 && !wifi->power_active_high)
		level = 1;

	/* Drives the pin. */
	error = drv_rpi5_gio_set_output(&wifi->gio, wifi->power_pin, level);
	if (error != 0)
		return error;

	/* Succeeded: the chip's power follows. */
	return 0;
}

/*
 * Turns WL_ON off and on again, which resets the chip, and lets it settle
 * for the regulator's start-up delay before the first command.
 */
static int
pulse_power(
	struct rpi5_wifi *wifi)
{
	int error;

	/* Holds the chip in reset; from here the chip must be powered down when the thread ends. */
	wifi->power_touched = 1;
	error = set_power(wifi, 0);
	if (error != 0) {
		kern_logf("rpi5-wifi: WL_ON not turned off (%d)\n", error);
		return error;
	}

	/* Keeps the reset long enough for the chip's regulators to drain. */
	error = wifi_sleep(wifi, POWER_LOW_MS);
	if (error != 0)
		return error;

	/* Releases the reset. */
	error = set_power(wifi, 1);
	if (error != 0) {
		kern_logf("rpi5-wifi: WL_ON not turned on (%d)\n", error);
		return error;
	}

	/* Lets the chip come up before CMD5. */
	error = wifi_sleep(wifi, wifi->power_settle_ms);
	if (error != 0)
		return error;

	/* Records the power line and the bus as CMD5 finds them. */
	report_power_line(wifi);
	report_host(wifi, "before CMD5");
	report_pin_selections(wifi);
	(void)drv_rpi5_serror_check("rpi5-wifi: powering the chip");

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
	struct rpi5_wifi *wifi)
{
	int error;

	/* Refuses a chip with fewer functions than the BCM43455's two. */
	if (wifi->card.ocr.function_count < FUNCTION_FRAMES) {
		kern_logf("rpi5-wifi: card has %u functions, not 2\n", wifi->card.ocr.function_count);
		return ENODEV;
	}

	/* Enables the backplane, through which the chip's memory is reached. */
	error = drv_rpi4_sdio_enable_function(&wifi->card, FUNCTION_BACKPLANE, &wifi->waiter, FUNCTION1_READY_MS);
	if (error != 0) {
		kern_logf("rpi5-wifi: F1 not ready (%d)\n", error);
		return error;
	}

	/* Sets the block size CMD53 uses for the backplane. */
	error = drv_rpi4_sdio_set_block_size(&wifi->card, FUNCTION_BACKPLANE, FUNCTION1_BLOCK_SIZE);
	if (error != 0) {
		kern_logf("rpi5-wifi: F1 block size not set (%d)\n", error);
		return error;
	}

	/* Sets the block size CMD53 uses for the frames. */
	error = drv_rpi4_sdio_set_block_size(&wifi->card, FUNCTION_FRAMES, FUNCTION2_BLOCK_SIZE);
	if (error != 0) {
		kern_logf("rpi5-wifi: F2 block size not set (%d)\n", error);
		return error;
	}

	/* Records the functions' state. */
	kern_logf(
		"rpi5-wifi: F1 enabled; block sizes F1 %u, F2 %u\n",
		wifi->card.block_size[FUNCTION_BACKPLANE],
		wifi->card.block_size[FUNCTION_FRAMES]);

	/* Succeeded: the backplane can be reached. */
	return 0;
}

/* Logs what enumeration learned of the card. */
static void
report_card(
	struct rpi5_wifi *wifi)
{
	struct drv_rpi4_sdio_card *card;

	card = &wifi->card;

	/* The answer to CMD5 and the address. */
	kern_logf(
		"rpi5-wifi: SDIO card: %u functions, OCR %06x, memory %u, RCA %04x\n",
		card->ocr.function_count,
		card->ocr.voltages,
		card->ocr.memory_present,
		card->rca);

	/* The common registers and the CIS. */
	kern_logf(
		"rpi5-wifi: CCCR %u, SDIO %u, capability %02x, CIS at %06x: manufacturer %04x (%u), card %04x, F0 block %u, speed %02x\n",
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
		"rpi5-wifi: bus %u lines at %u Hz\n",
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
	struct rpi5_wifi *wifi)
{
	int error;

	/* Stops the controller, which keeps the card's interrupt off the line. */
	if (wifi->host_ready != 0) {
		drv_rpi4_sdio_host_stop(&wifi->host);
		wifi->host_ready = 0;
	}

	/* Leaves the line once the controller can no longer raise it. */
	if (wifi->irq_added != 0) {
		error = drv_rpi4_mmc_irq_remove(wifi->irq, drv_rpi4_sdio_host_interrupt, &wifi->host);
		if (error != 0)
			kern_logf("rpi5-wifi: SD interrupt line not left (%d)\n", error);
		wifi->irq_added = 0;
	}

	/* Powers the chip down; untouched power is left as the firmware set it. */
	if (wifi->power_touched != 0) {
		error = set_power(wifi, 0);
		if (error != 0) {
			kern_logf("rpi5-wifi: WL_ON not turned off at the end (%d)\n", error);
		} else {
			kern_logf("rpi5-wifi: chip powered down\n");
		}

		/* Records the chip's power as given back. */
		wifi->power_touched = 0;
	}

	/* Names this step if giving back raised an SError. */
	(void)drv_rpi5_serror_check("rpi5-wifi: giving the controller back");
}

/* Reports ECANCELED once a stop has been asked for. */
static int
stopping_requested(
	struct rpi5_wifi *wifi)
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
	struct rpi5_wifi *wifi;
	unsigned long enabled;
	uint64_t deadline;
	uint64_t observed;
	int error;

	wifi = context;

	/* The scheduler tick at which the sleep ends. */
	deadline = sched_ticks() + kern_ms_to_ticks(milliseconds);

	/* Sleeps until the deadline, or until drv_rpi5_wifi_stop() sets stopping. */
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
	struct rpi5_wifi *wifi;
	int error;

	wifi = context;

	/* Reads the byte through the card layer. */
	error = drv_rpi4_sdio_read_byte(&wifi->card, function, address, value);
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
	struct rpi5_wifi *wifi;
	int error;

	wifi = context;

	/* Writes the byte through the card layer. */
	error = drv_rpi4_sdio_write_byte(&wifi->card, function, address, value);
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
	struct rpi5_wifi *wifi;
	int error;

	wifi = context;

	/* Reads the bytes through the card layer. */
	error = drv_rpi4_sdio_transfer(&wifi->card, DRV_RPI4_SDIO_DATA_READ, function, address, increment, buffer, length);
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
	struct rpi5_wifi *wifi;
	int error;

	wifi = context;

	/* Writes the bytes through the card layer. */
	error = drv_rpi4_sdio_transfer(&wifi->card, DRV_RPI4_SDIO_DATA_WRITE, function, address, increment, buffer, length);
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
	struct rpi5_wifi *wifi;

	wifi = context;

	/* A function past the card's has no block size. */
	if (function > DRV_RPI4_SDIO_FUNCTION_MAX)
		return 0;

	/* Succeeded: the size enumeration and power_up set. */
	return wifi->card.block_size[function];
}

/* Enables a function and waits for it to report ready. */
static int
bus_enable_function(
	void *context,
	unsigned function,
	unsigned timeout_ms)
{
	struct rpi5_wifi *wifi;
	int error;

	wifi = context;

	/* Enables it through the card layer, which sleeps through the thread's waiter. */
	error = drv_rpi4_sdio_enable_function(&wifi->card, function, &wifi->waiter, timeout_ms);
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
	struct rpi5_wifi *wifi;
	int error;

	wifi = context;

	/* Disables it through the card layer. */
	error = drv_rpi4_sdio_disable_function(&wifi->card, function);
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
	struct rpi5_wifi *wifi;
	int waited;

	wifi = context;

	/* Waits on the host's card interrupt. */
	waited = drv_rpi4_sdio_host_card_irq_wait(&wifi->host, deadline);
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
	struct rpi5_wifi *wifi;

	wifi = context;

	/* Applies it in the host. */
	drv_rpi4_sdio_host_card_irq_enable(&wifi->host, enable);
}

/* Arms the card's interrupt again after it was served. */
static void
bus_interrupt_rearm(
	void *context)
{
	struct rpi5_wifi *wifi;

	wifi = context;

	/* Rearms it in the host. */
	drv_rpi4_sdio_host_card_irq_rearm(&wifi->host);
}

/* Ends the bus thread's wait without an interrupt. */
static void
bus_interrupt_kick(
	void *context)
{
	struct rpi5_wifi *wifi;

	wifi = context;

	/* Kicks the host's wait. */
	drv_rpi4_sdio_host_card_irq_kick(&wifi->host);
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
	struct rpi5_wifi *wifi;
	int error;

	wifi = context;

	/* Returns the bus to one line, which a previous run left wide. */
	error = drv_rpi4_sdio_host_set_bus_width(&wifi->host, 1);
	if (error != 0)
		return error;

	/* Returns the bus to the enumeration clock, which a previous run left fast. */
	error = drv_rpi4_sdio_host_set_clock(&wifi->host, DRV_RPI4_SDIO_HOST_IDENTIFY_HZ);
	if (error != 0)
		return error;

	/* Resets the chip through its power line. */
	error = pulse_power(wifi);
	if (error != 0)
		return error;

	/* Enumerates the SDIO interface up to a selected card on the fast bus. */
	error = drv_rpi4_sdio_enumerate(&wifi->card, &wifi->host, &wifi->waiter);
	(void)drv_rpi5_serror_check("rpi5-wifi: enumerating the card");
	if (error != 0)
		return error;

	/* Logs the card the first time. */
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
	struct rpi5_wifi *wifi;
	int error;

	wifi = context;

	/* Keeps the card's interrupt off the line. */
	drv_rpi4_sdio_host_card_irq_enable(&wifi->host, 0);

	/* Holds the chip in reset. */
	error = set_power(wifi, 0);
	if (error != 0)
		kern_logf("rpi5-wifi: WL_ON not turned off (%d)\n", error);

	/* Names this step if powering down raised an SError. */
	(void)drv_rpi5_serror_check("rpi5-wifi: powering the chip down");
}
