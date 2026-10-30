/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The S905X5M's glue for the microSD slot of the ODROID-C5: the SD/eMMC
 * controller B.
 *
 * The bootloader read the kernel from the card, so the card is powered at
 * 3.3 V signalling and the controller's clock is on.  The glue still sets
 * what the driver depends on: the controller's gate, its pins, the card's
 * supply and signalling voltage, and the clock selector in front of the
 * controller, which takes the crystal for identification and the default
 * speed and the 1 GHz fixed clock for high speed.  The controller moves
 * data through one bounce buffer below 4 GiB.
 *
 * The card detect pin is read for the log only: the card the bootloader
 * read is tried even when the pin says the slot is empty.
 */

#include "drivers/platform/s905x5m/s905x5m-mmc.h"
#include "drivers/platform/s905x5m/s905x5m-soc.h"
#include "drivers/mmc/meson-mmc.h"

#include <kern/device-io.h>
#include <kern/disk.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

/*
 * The controller of the microSD slot and the size of its register block.
 *
 * Values referenced: Hardkernel common_drivers odroids7d-6.12.y @34daba9
 * arch/arm64/boot/dts/amlogic/meson-s7d.dtsi (sd@fe08a000).
 */
#define S905X5M_MMC_REGISTERS		0xfe08a000ULL
#define S905X5M_MMC_REGISTER_BYTES	0x1000U

/*
 * The controller's peripheral gate, and its clock selector's fields in the
 * SD/eMMC clock register: the source, the divider before the controller
 * and the gate after it.  The source numbers are the crystal (24 MHz) and
 * the fixed clock divided by two (1 GHz).
 *
 * Values referenced: Hardkernel common_drivers odroids7d-6.12.y @34daba9
 * drivers/clk/meson/s7d.c (sys_sd_emmc_b: CLKCTRL_SYS_CLK_EN0_REG0 bit 25;
 * sd_emmc_b_mux: shift 25, mask 0x7, parents xtal and fclk_div2 at 0 and
 * 1; sd_emmc_b_div: shift 16, width 7; sd_emmc_b: gate bit 23).
 */
#define S905X5M_MMC_GATE_BIT		25U
#define S905X5M_MMC_SOURCE_SHIFT	25U
#define S905X5M_MMC_SOURCE_MASK		(0x7U << 25)
#define S905X5M_MMC_SOURCE_CRYSTAL	0U
#define S905X5M_MMC_SOURCE_FIXED_DIV2	1U
#define S905X5M_MMC_DIVIDER_MASK	(0x7fU << 16)
#define S905X5M_MMC_CLOCK_GATE		(1U << 23)
#define S905X5M_MMC_SELECTOR_MASK	(S905X5M_MMC_SOURCE_MASK | S905X5M_MMC_DIVIDER_MASK | S905X5M_MMC_CLOCK_GATE)
#define S905X5M_MMC_CRYSTAL_HZ		24000000ULL
#define S905X5M_MMC_FIXED_DIV2_HZ	1000000000ULL

/*
 * The slot's pins: the four data lines, the clock and the command on
 * GPIOC_0..5 in their SD function, pulled up at 3 mA; the card detect on
 * GPIOC_6 as a GPIO.
 *
 * Values referenced: Hardkernel common_drivers odroids7d-6.12.y @34daba9
 * drivers/gpio/pinctrl/pinctrl-meson-s7d.c (sdcard_d0_c0 .. sdcard_cmd_c5,
 * function 1) and arch/arm64/boot/dts/amlogic/s7d_s905x5m_odroidc5.dts
 * (sdcard_pins: bias-pull-up, drive-strength-microamp 3000; sd@fe08a000:
 * cd-gpios GPIOC_6).
 */
#define S905X5M_MMC_FIRST_PIN		(DRV_S905X5M_GPIO_C + 0U)
#define S905X5M_MMC_PIN_COUNT		6U
#define S905X5M_MMC_PIN_FUNCTION	1U
#define S905X5M_MMC_PIN_MICROAMPS	3000U
#define S905X5M_MMC_DETECT_PIN		(DRV_S905X5M_GPIO_C + 6U)

/*
 * The card's supply switch (GPIOH_6, on when high) and its signalling
 * voltage selector (GPIODV_4, low for 3.3 V), when the device tree's
 * regulators cannot be read.
 *
 * Values referenced: Hardkernel common_drivers odroids7d-6.12.y @34daba9
 * arch/arm64/boot/dts/amlogic/s7d_s905x5m_odroidc5.dts (fixed@sdcard-3v3:
 * gpio GPIOH_6, enable-active-high; gpio@sdcard-1v8_3v3: gpio GPIODV_4,
 * states 1800000 1 3300000 0).
 */
#define S905X5M_MMC_SUPPLY_PIN		(DRV_S905X5M_GPIO_H + 6U)
#define S905X5M_MMC_VOLTAGE_PIN		(DRV_S905X5M_GPIO_DV + 4U)
#define S905X5M_MMC_3V3_MICROVOLTS	3300000U

/* The controller's clock and configuration registers, for the log before the attach. */
#define S905X5M_MMC_CONTROLLER_CLOCK	0x000U
#define S905X5M_MMC_CONTROLLER_CONFIG	0x044U

/* The bounce buffer: 64 KiB, the longest run the driver moves, below 4 GiB. */
#define S905X5M_MMC_BOUNCE_BYTES	(64U * 1024U)
#define S905X5M_MMC_BOUNCE_ALIGN	4096U
#define S905X5M_MMC_BOUNCE_LIMIT	0xffffffffULL

/* The signature at the end of a master boot record. */
#define S905X5M_MMC_MBR_SIGNATURE_LOW	0x55U
#define S905X5M_MMC_MBR_SIGNATURE_HIGH	0xaaU

/* The unit of the slot, which names the disk mmcblk0. */
#define S905X5M_MMC_UNIT		0U

/*
 * The SD card's disk.
 *
 * Set once by drv_s905x5m_mmc_init() when the card is published and kept
 * for the life of the system; NULL means no card was published.
 */
static struct disk *s905x5m_mmc_disk;

/*
 * The bounce buffer the controller moves data through.
 *
 * Allocated once by the first init that reaches the card and never
 * freed; a zero size means none was allocated.
 */
static struct kern_pmem s905x5m_mmc_bounce;

static int select_source(void *context, enum drv_meson_mmc_source source);
static int switch_slot_on(uint32_t node);
static int set_supply(uint32_t node, const char *name, unsigned fallback_pin, int is_voltage);
static int voltage_level(uint32_t regulator, uint32_t microvolts, int *high);
static void log_state(volatile uint8_t *registers);
static void check_boot_record(void);

/*
 * Starts the microSD slot's controller and the card in it.
 */
int
drv_s905x5m_mmc_init(
	uint64_t fdt_phys)
{
	struct drv_meson_mmc_config config;
	const uint8_t *value;
	struct disk *disk;
	uint32_t length;
	uint32_t node;
	void *mapped;
	int error;

	/* Opens the device tree and maps the shared blocks. */
	error = drv_s905x5m_soc_init(fdt_phys);
	if (error != 0)
		return -error;

	/* Finds the slot's controller. */
	error = drv_s905x5m_find_node("amlogic,meson-v8-mmc", S905X5M_MMC_REGISTERS, &node);
	if (error != 0) {
		kern_logf("s905x5m-mmc: no SD controller at %llx in the device tree\n",
			  (unsigned long long)S905X5M_MMC_REGISTERS);
		return -ENODEV;
	}

	/* Maps the controller's registers. */
	error = kern_device_map(S905X5M_MMC_REGISTERS, S905X5M_MMC_REGISTER_BYTES, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0)
		return -error;

	/* Writes what the bootloader left, before anything changes it. */
	log_state(mapped);

	/* Switches the controller's gate, its pins and the card's supplies on. */
	error = switch_slot_on(node);
	if (error != 0)
		return -error;

	/* Allocates the bounce buffer below 4 GiB, once. */
	if (s905x5m_mmc_bounce.size == 0) {
		error = kern_pmem_alloc_limited(S905X5M_MMC_BOUNCE_BYTES,
						S905X5M_MMC_BOUNCE_ALIGN,
						S905X5M_MMC_BOUNCE_LIMIT,
						0,
						&s905x5m_mmc_bounce);
		if (error != 0) {
			kern_logf("s905x5m-mmc: no bounce buffer below 4 GiB (%d)\n", error);
			return -error;
		}
	}

	/* Describes the slot to the driver. */
	kern_memset(&config, 0, sizeof(config));
	config.registers = mapped;
	config.bounce = kern_pmem_to_kernel(s905x5m_mmc_bounce.paddr);
	config.bounce_bus_address = s905x5m_mmc_bounce.paddr;
	config.bounce_bytes = S905X5M_MMC_BOUNCE_BYTES;
	config.slow_rate = S905X5M_MMC_CRYSTAL_HZ;
	config.fast_rate = S905X5M_MMC_FIXED_DIV2_HZ;
	config.select_source = select_source;
	config.bus_width = drv_s905x5m_node_number(node, "bus-width", 1U);
	config.max_frequency = drv_s905x5m_node_number(node, "max-frequency", 0);
	config.card_present = NULL;
	config.context = NULL;
	config.unit = S905X5M_MMC_UNIT;

	/* Allows high speed when the tree says the slot takes it. */
	config.high_speed = 0;
	error = drv_fdt_property(drv_s905x5m_fdt(), node, "cap-sd-highspeed", &value, &length);
	if (error == 0)
		config.high_speed = 1;

	/* Refuses a bounce buffer outside the direct map. */
	if (config.bounce == NULL)
		return -ENOMEM;

	/* Brings the card up and publishes it as mmcblk0. */
	error = drv_meson_mmc_attach(&config, &disk);
	if (error != 0)
		return -error;

	/* Records the disk for the platform. */
	s905x5m_mmc_disk = disk;

	/* Writes whether the card's first sector holds a boot record. */
	check_boot_record();

	/* Succeeded: the card is mmcblk0. */
	return 0;
}

/*
 * Returns the SD card's disk, or NULL before it was published.
 */
struct disk *
drv_s905x5m_mmc_disk(
	void)
{
	/* Succeeded: the disk, or none. */
	return s905x5m_mmc_disk;
}

/*
 * Feeds the controller from the crystal or from the 1 GHz fixed clock,
 * undivided and with its gate on.
 */
static int
select_source(
	void *context,
	enum drv_meson_mmc_source source)
{
	uint32_t selector;

	(void)context;

	/* Picks the selector's source number. */
	selector = S905X5M_MMC_SOURCE_CRYSTAL << S905X5M_MMC_SOURCE_SHIFT;
	if (source == DRV_MESON_MMC_SOURCE_FAST)
		selector = S905X5M_MMC_SOURCE_FIXED_DIV2 << S905X5M_MMC_SOURCE_SHIFT;

	/* Writes the source, a divider of one and the gate on; controller A's half is left alone. */
	drv_s905x5m_clock_update(DRV_S905X5M_CLOCK_SD_EMMC,
				 S905X5M_MMC_SELECTOR_MASK,
				 selector | S905X5M_MMC_CLOCK_GATE);

	/* Succeeded: the controller runs from the source. */
	return 0;
}

/*
 * Switches the controller's peripheral gate on, gives the slot's pins
 * their SD function, makes the card detect a pulled-up input, and turns
 * the card's supply on at 3.3 V signalling.
 */
static int
switch_slot_on(
	uint32_t node)
{
	unsigned pin;
	int error;

	/* Switches the controller's peripheral gate on. */
	drv_s905x5m_clock_update(DRV_S905X5M_CLOCK_GATES0,
				 1U << S905X5M_MMC_GATE_BIT,
				 1U << S905X5M_MMC_GATE_BIT);

	/* Gives the data, clock and command pins their SD function, pulled up at 3 mA. */
	for (pin = S905X5M_MMC_FIRST_PIN; pin < S905X5M_MMC_FIRST_PIN + S905X5M_MMC_PIN_COUNT; pin++) {
		/* Selects the SD function. */
		error = drv_s905x5m_pin_function(pin, S905X5M_MMC_PIN_FUNCTION);
		if (error != 0)
			return error;

		/* Pulls the line up. */
		error = drv_s905x5m_pin_pull(pin, DRV_S905X5M_PULL_UP);
		if (error != 0)
			return error;

		/* Sets the drive strength. */
		error = drv_s905x5m_pin_drive(pin, S905X5M_MMC_PIN_MICROAMPS);
		if (error != 0)
			return error;
	}

	/* Makes the card detect a GPIO input, pulled up. */
	error = drv_s905x5m_pin_function(S905X5M_MMC_DETECT_PIN, 0);
	if (error != 0)
		return error;
	error = drv_s905x5m_pin_pull(S905X5M_MMC_DETECT_PIN, DRV_S905X5M_PULL_UP);
	if (error != 0)
		return error;
	error = drv_s905x5m_gpio_input(S905X5M_MMC_DETECT_PIN);
	if (error != 0)
		return error;

	/* Keeps the signalling at 3.3 V, which the bootloader left. */
	error = set_supply(node, "vqmmc-supply", S905X5M_MMC_VOLTAGE_PIN, 1);
	if (error != 0)
		return error;

	/* Keeps the card's supply on. */
	error = set_supply(node, "vmmc-supply", S905X5M_MMC_SUPPLY_PIN, 0);
	if (error != 0)
		return error;

	/* Succeeded: the slot is powered and its pins are the controller's. */
	return 0;
}

/*
 * Drives the GPIO of one of the card's regulators: the supply switch on,
 * or the signalling selector at 3.3 V.  A regulator the tree does not
 * describe falls back to the ODROID-C5's pin.
 */
static int
set_supply(
	uint32_t node,
	const char *name,
	unsigned fallback_pin,
	int is_voltage)
{
	struct drv_s905x5m_gpio gpio;
	const uint8_t *value;
	uint32_t length;
	uint32_t phandle;
	uint32_t regulator;
	int high;
	int error;

	/* Starts from the board's pin: the switch on is high, 3.3 V is low. */
	gpio.pin = fallback_pin;
	gpio.flags = 0;
	high = 1;
	if (is_voltage)
		high = 0;

	/* Reads the regulator the property names, when it does. */
	regulator = DRV_FDT_NO_NODE;
	error = drv_fdt_property(drv_s905x5m_fdt(), node, name, &value, &length);
	if (error == 0 && length < 4U)
		error = ENOENT;
	if (error == 0) {
		phandle = (uint32_t)drv_fdt_cells_value(value, 0, 1U);
		error = drv_fdt_find_phandle(drv_s905x5m_fdt(), phandle, &regulator);
	}

	/* Takes the regulator's pin and, for the selector, the level of 3.3 V. */
	if (error == 0) {
		/* Reads the regulator's pin. */
		error = drv_s905x5m_node_gpio(regulator, "gpio", &gpio);
		if (error != 0)
			gpio.pin = fallback_pin;

		/* Reads the level the selector takes for 3.3 V. */
		if (is_voltage)
			(void)voltage_level(regulator, S905X5M_MMC_3V3_MICROVOLTS, &high);
	}

	/* Makes the pin a GPIO and drives it. */
	error = drv_s905x5m_pin_function(gpio.pin, 0);
	if (error != 0)
		return error;
	error = drv_s905x5m_gpio_output(gpio.pin, high);
	if (error != 0)
		return error;

	/* Succeeded: the regulator is set. */
	return 0;
}

/*
 * Finds the GPIO level a regulator-gpio's states give a voltage.
 *
 * Leaves high as it was when the regulator lists no such state.
 */
static int
voltage_level(
	uint32_t regulator,
	uint32_t microvolts,
	int *high)
{
	const uint8_t *value;
	uint32_t length;
	uint32_t entry;
	uint32_t voltage;
	uint32_t level;
	int error;

	/* Reads the states: pairs of a voltage and a level. */
	error = drv_fdt_property(drv_s905x5m_fdt(), regulator, "states", &value, &length);
	if (error != 0)
		return error;

	/* Walks the pairs for the voltage. */
	for (entry = 0; (entry + 1U) * 8U <= length; entry++) {
		/* Skips a state of another voltage. */
		voltage = (uint32_t)drv_fdt_cells_value(value, entry * 2U, 1U);
		if (voltage != microvolts)
			continue;

		/* Succeeded: the level of the voltage. */
		level = (uint32_t)drv_fdt_cells_value(value, entry * 2U + 1U, 1U);
		*high = 0;
		if (level != 0)
			*high = 1;
		return 0;
	}

	/* The regulator has no state of the voltage. */
	return ENOENT;
}

/*
 * Writes the clock selector, the controller's clock and configuration
 * registers, and the card detect's level as the bootloader left them.
 */
static void
log_state(
	volatile uint8_t *registers)
{
	uint32_t selector;
	uint32_t clock;
	uint32_t config;
	int level;
	int error;

	/* Reads the selector and the controller's two registers. */
	selector = drv_s905x5m_clock_read(DRV_S905X5M_CLOCK_SD_EMMC);
	clock = kern_mmio_read32(registers + S905X5M_MMC_CONTROLLER_CLOCK);
	config = kern_mmio_read32(registers + S905X5M_MMC_CONTROLLER_CONFIG);

	/* Reads the card detect; a card pulls it low. */
	level = -1;
	error = drv_s905x5m_gpio_read(S905X5M_MMC_DETECT_PIN, &level);
	if (error != 0)
		level = -1;

	/* Writes them in one line. */
	kern_logf("s905x5m-mmc: clock selector %08x, controller clock %08x config %08x, card detect %d (0 is a card)\n",
		  selector,
		  clock,
		  config,
		  level);
}

/*
 * Reads the card's first sector and writes whether it ends with a master
 * boot record's signature.
 */
static void
check_boot_record(
	void)
{
	uint8_t sector[512];
	const char *verdict;
	int error;

	/* Reads sector 0. */
	error = drv_meson_mmc_read(S905X5M_MMC_UNIT, 0, 1U, sector);
	if (error != 0) {
		kern_logf("s905x5m-mmc: sector 0 unreadable (%d)\n", error);
		return;
	}

	/* A boot record ends with 55 aa. */
	verdict = "no boot record";
	if (sector[510] == S905X5M_MMC_MBR_SIGNATURE_LOW && sector[511] == S905X5M_MMC_MBR_SIGNATURE_HIGH)
		verdict = "a boot record";

	/* Writes the signature the sector ends with. */
	kern_logf("s905x5m-mmc: sector 0 ends %02x%02x (%s)\n",
		  sector[510],
		  sector[511],
		  verdict);
}
