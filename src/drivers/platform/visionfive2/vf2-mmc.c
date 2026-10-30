/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The JH7110's glue for the SD card slot's DesignWare controller.
 *
 * The slot is SDIO1.  Its node in the device tree names the bus clock
 * (biu), the card interface clock (ciu) and its rate, the reset, the pin
 * group and the card detect pin; the glue switches them on in that order
 * and hands the mapped controller to the DesignWare driver, which talks
 * to the card.
 *
 * The card runs at high speed at most, which needs no tuning, so the
 * controller's sample phase starts as the firmware set it, as Linux leaves
 * it; the glue writes it to the boot log with the system control block's
 * field the node's starfive,sysreg names, and hands the DesignWare driver
 * a hook that sets it, which the driver uses when a read at high speed
 * fails with a data CRC error.  The controller has no drive phase that
 * Linux sets for the slot, so the glue sets none.
 *
 * The card clock is held to the default speed's 25 MHz although the slot
 * and the card allow high speed: the DesignWare driver moves the data by
 * programmed I/O, and the U74 cores do not drain the 128-byte FIFO as fast
 * as a 4-bit bus at 49.5 MHz fills it.  The full FIFO stops the card clock
 * in the middle of a block, and those reads failed their data CRC whatever
 * the sample phase (measured on the board, a phase sweep read the failed
 * run at every phase).
 */

#include <drivers/platform/visionfive2/vf2-mmc.h>

#include <drivers/mmc/dw-mmc.h>
#include <drivers/platform/visionfive2/vf2-crg.h>
#include <drivers/platform/visionfive2/vf2-pinctrl.h>
#include <drivers/platform/visionfive2/vf2-syscon.h>
#include <kern/device-io.h>
#include <kern/klog.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

/*
 * Where the SD card slot's controller (SDIO1) is.
 *
 * Value referenced: Linux v6.12 arch/riscv/boot/dts/starfive/jh7110.dtsi
 * (mmc1, reg).
 */
#define VF2_MMC_SD_REGISTERS		0x16020000ULL

/*
 * The controller's register that holds the sample phase, and the phase's
 * five bits in it.
 *
 * Values referenced: Linux v6.12 drivers/mmc/host/dw_mmc.h
 * (SDMMC_UHS_REG_EXT) and drivers/mmc/host/dw_mmc-starfive.c
 * (STARFIVE_SMPL_PHASE).
 */
#define VF2_MMC_PHASE_REGISTER		0x108U
#define VF2_MMC_PHASE_SHIFT		16U
#define VF2_MMC_PHASE_MASK		0x1fU

/*
 * How many sample phases the controller has: the field's five bits.
 *
 * Value referenced: Linux v6.12 drivers/mmc/host/dw_mmc-starfive.c
 * (MAX_DELAY_CHAIN).
 */
#define VF2_MMC_PHASE_COUNT		32U

/*
 * The fastest card clock the slot is run at: the default speed, which the
 * driver's programmed I/O keeps up with (see the file's comment).
 */
#define VF2_MMC_MAX_HZ			25000000U

/* The unit number of the slot's card, which names its disk mmcblk0. */
#define VF2_MMC_UNIT			0U

/*
 * The slot's card detect pin, the context of card_present().
 *
 * Filled by drv_vf2_mmc_attach() before the controller is handed over and
 * kept for the life of the system.
 */
static struct drv_vf2_gpio vf2_mmc_card_detect;

static int find_slot(const struct drv_fdt *fdt, uint32_t *node, uint64_t *address, uint64_t *size);
static int switch_slot_on(const struct drv_fdt *fdt, uint32_t node, uint64_t *clock_rate);
static int prepare_card_detect(const struct drv_fdt *fdt, uint32_t node, int *has_detect);
static int card_present(void *context, int *present);
static uint32_t read_number(const struct drv_fdt *fdt, uint32_t node, const char *name, uint32_t fallback);
static int has_property(const struct drv_fdt *fdt, uint32_t node, const char *name);
static uint32_t describe_sample_phase(const struct drv_fdt *fdt, uint32_t node, volatile uint8_t *registers);
static void set_sample_phase(void *context, uint32_t phase);

/*
 * Brings up the SD card slot and publishes its card.
 */
int
drv_vf2_mmc_attach(
	const struct drv_fdt *fdt,
	struct disk **disk)
{
	struct drv_dw_mmc_config config;
	uint64_t clock_rate;
	uint32_t phase;
	uint64_t address;
	uint64_t size;
	uint32_t node;
	void *mapped;
	int has_detect;
	int error;

	/* Finds the slot's controller. */
	error = find_slot(fdt, &node, &address, &size);
	if (error != 0)
		return error;

	/* Maps the controller's registers as device memory. */
	error = kern_device_map(address, (size_t)size, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0)
		return error;

	/* Switches the slot's pins, clocks and reset on. */
	error = switch_slot_on(fdt, node, &clock_rate);
	if (error != 0)
		return error;

	/* Makes the card detect pin an input, when the slot has one. */
	error = prepare_card_detect(fdt, node, &has_detect);
	if (error != 0)
		return error;

	/* Writes the sample phase the firmware left to the boot log. */
	phase = describe_sample_phase(fdt, node, mapped);

	/* Describes the controller and the slot to the DesignWare driver. */
	config.registers = mapped;
	config.clock_rate = clock_rate;
	config.fifo_depth = read_number(fdt, node, "fifo-depth", 0);
	config.bus_width = read_number(fdt, node, "bus-width", 1U);
	config.max_frequency = read_number(fdt, node, "max-frequency", 0);
	config.high_speed = has_property(fdt, node, "cap-sd-highspeed");

	/* Holds the card clock to what the programmed I/O keeps up with. */
	if (config.max_frequency == 0 || config.max_frequency > VF2_MMC_MAX_HZ)
		config.max_frequency = VF2_MMC_MAX_HZ;
	config.power_on_delay_ms = read_number(fdt, node, "post-power-on-delay-ms", 0);
	config.sample_phase = phase;
	config.sample_phase_count = VF2_MMC_PHASE_COUNT;
	config.set_sample_phase = set_sample_phase;
	config.phase_context = mapped;
	config.card_present = NULL;
	config.context = NULL;
	if (has_detect) {
		config.card_present = card_present;
		config.context = &vf2_mmc_card_detect;
	}

	/* Names the slot's disk mmcblk0. */
	config.unit = VF2_MMC_UNIT;

	/* Brings the card up and publishes it. */
	error = drv_dw_mmc_attach(&config, disk);
	if (error != 0)
		return error;

	/* Succeeded: the slot's card is a disk. */
	return 0;
}

/* Finds the enabled controller of the JH7110 whose registers are the SD slot's. */
static int
find_slot(
	const struct drv_fdt *fdt,
	uint32_t *node,
	uint64_t *address,
	uint64_t *size)
{
	uint32_t candidate;
	bool enabled;
	int error;

	/* Walks the JH7110's SD/MMC controllers. */
	candidate = DRV_FDT_NO_NODE;
	for (;;) {
		/* Moves to the next controller; none left means the tree has no slot. */
		error = drv_fdt_find_compatible(fdt, "starfive,jh7110-mmc", candidate, &candidate);
		if (error != 0)
			return ENOENT;

		/* Skips a controller the tree disables. */
		enabled = drv_fdt_node_enabled(fdt, candidate);
		if (!enabled)
			continue;

		/* Skips a controller whose registers cannot be read. */
		error = drv_fdt_reg(fdt, candidate, 0, address, size);
		if (error != 0)
			continue;

		/* Stops at the slot's controller. */
		if (*address == VF2_MMC_SD_REGISTERS)
			break;
	}

	/* Succeeded: the slot's node and registers. */
	*node = candidate;
	return 0;
}

/*
 * Applies the slot's pin group, sets its card interface clock's rate,
 * switches its clocks on and releases its reset, and reports the card
 * interface clock's rate.
 *
 * Procedure referenced: OpenBSD sys/dev/fdt/dwmmc.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (dwmmc_attach).
 */
static int
switch_slot_on(
	const struct drv_fdt *fdt,
	uint32_t node,
	uint64_t *clock_rate)
{
	struct drv_vf2_clock bus_clock;
	struct drv_vf2_clock card_clock;
	struct drv_vf2_reset reset;
	unsigned changed;
	int error;

	/* Applies the slot's pin group; a node without one keeps the firmware's pins. */
	error = drv_vf2_pinctrl_apply_default(fdt, node, &changed);
	if (error != 0 && error != ENOENT)
		return error;

	/* Finds the bus clock. */
	error = drv_vf2_clock_lookup(fdt, node, "biu", &bus_clock);
	if (error != 0)
		return error;

	/* Finds the card interface clock. */
	error = drv_vf2_clock_lookup(fdt, node, "ciu", &card_clock);
	if (error != 0)
		return error;

	/* Sets the card interface clock to the rate the tree assigns, 50 MHz on the board. */
	error = drv_vf2_clock_apply_assigned(fdt, node);
	if (error != 0 && error != ENOENT)
		return error;

	/* Switches the bus clock on. */
	error = drv_vf2_clock_enable(&bus_clock);
	if (error != 0)
		return error;

	/* Switches the card interface clock on. */
	error = drv_vf2_clock_enable(&card_clock);
	if (error != 0)
		return error;

	/* Finds the controller's reset. */
	error = drv_vf2_reset_lookup(fdt, node, "reset", &reset);
	if (error != 0)
		return error;

	/* Releases the controller's reset. */
	error = drv_vf2_reset_deassert(&reset);
	if (error != 0)
		return error;

	/* Reads the rate the card clock is divided from. */
	error = drv_vf2_clock_get_rate(&card_clock, clock_rate);
	if (error != 0)
		return error;

	/* Succeeded: the controller runs. */
	return 0;
}

/* Finds the slot's card detect pin and makes it an input; a slot without one has none. */
static int
prepare_card_detect(
	const struct drv_fdt *fdt,
	uint32_t node,
	int *has_detect)
{
	int error;

	/* Finds the pin; a node without cd-gpios has a card always in. */
	*has_detect = 0;
	error = drv_vf2_gpio_lookup(fdt, node, "cd-gpios", &vf2_mmc_card_detect);
	if (error == ENOENT)
		return 0;
	if (error != 0)
		return error;

	/* Makes the pin an input. */
	error = drv_vf2_gpio_configure_input(&vf2_mmc_card_detect);
	if (error != 0)
		return error;

	/* Succeeded: the slot reports whether a card is in. */
	*has_detect = 1;
	return 0;
}

/* Reports whether a card is in the slot, from its card detect pin. */
static int
card_present(
	void *context,
	int *present)
{
	const struct drv_vf2_gpio *pin;
	int error;

	/* Reads the pin's active state, which is a card in the slot. */
	pin = context;
	error = drv_vf2_gpio_read(pin, present);
	if (error != 0)
		return error;

	/* Succeeded: whether a card is in. */
	return 0;
}

/* Reads a one-cell number property, or the fallback when the node does not have it. */
static uint32_t
read_number(
	const struct drv_fdt *fdt,
	uint32_t node,
	const char *name,
	uint32_t fallback)
{
	const uint8_t *value;
	uint32_t length;
	uint64_t number;
	int error;

	/* Finds the property; a missing or short one gives the fallback. */
	error = drv_fdt_property(fdt, node, name, &value, &length);
	if (error != 0)
		return fallback;
	if (length < 4U)
		return fallback;

	/* Decodes its first cell. */
	number = drv_fdt_cells_value(value, 0, 1U);

	/* Reports the number. */
	return (uint32_t)number;
}

/* Reports whether a node has a property, which for a flag means the flag is set. */
static int
has_property(
	const struct drv_fdt *fdt,
	uint32_t node,
	const char *name)
{
	const uint8_t *value;
	uint32_t length;
	int error;

	/* Looks the property up. */
	error = drv_fdt_property(fdt, node, name, &value, &length);
	if (error != 0)
		return 0;

	/* The node has it. */
	return 1;
}

/*
 * Writes the controller's sample phase and the field of the system
 * control block that starfive,sysreg names to the boot log, and reports
 * the phase.
 *
 * The property's three arguments are the field's offset, shift and mask.
 *
 * Values referenced: Linux v6.12 arch/riscv/boot/dts/starfive/jh7110.dtsi
 * (mmc1, starfive,sysreg = <&sys_syscon 0x9c 0x1 0x3e>).
 */
static uint32_t
describe_sample_phase(
	const struct drv_fdt *fdt,
	uint32_t node,
	volatile uint8_t *registers)
{
	enum drv_vf2_syscon syscon;
	uint32_t arguments[3];
	uint32_t phase;
	uint32_t field;
	unsigned count;
	int error;

	/* Reads the controller's sample phase. */
	phase = kern_mmio_read32(registers + VF2_MMC_PHASE_REGISTER);
	phase = (phase >> VF2_MMC_PHASE_SHIFT) & VF2_MMC_PHASE_MASK;

	/* Finds the system control block's field; a node without it logs the phase alone. */
	error = drv_vf2_syscon_lookup(fdt, node, "starfive,sysreg", &syscon, arguments, 3U, &count);
	if (error != 0 ||
	    count < 3U ||
	    arguments[1] >= 32U) {
		kern_logf("vf2-mmc: sample phase %u\n", phase);
		return phase;
	}

	/* Reads the field. */
	error = drv_vf2_syscon_read(syscon, arguments[0], &field);
	if (error != 0) {
		kern_logf("vf2-mmc: sample phase %u, sysreg unreadable (%d)\n", phase, error);
		return phase;
	}

	/* Takes the phase's bits out of the field. */
	field = (field & arguments[2]) >> arguments[1];

	/* Succeeded: logs both. */
	kern_logf("vf2-mmc: sample phase %u, sysreg %x field %x\n", phase, arguments[0], field);
	return phase;
}

/*
 * Sets the controller's sample phase, keeping the register's other bits;
 * the DesignWare driver waits for it to settle.
 *
 * Procedure referenced: Linux v6.12 drivers/mmc/host/dw_mmc-starfive.c
 * (dw_mci_starfive_set_sample_phase).
 */
static void
set_sample_phase(
	void *context,
	uint32_t phase)
{
	volatile uint8_t *registers;
	uint32_t value;

	/* Replaces the phase's five bits of the register. */
	registers = context;
	value = kern_mmio_read32(registers + VF2_MMC_PHASE_REGISTER);
	value &= ~(VF2_MMC_PHASE_MASK << VF2_MMC_PHASE_SHIFT);
	value |= (phase & VF2_MMC_PHASE_MASK) << VF2_MMC_PHASE_SHIFT;
	kern_mmio_write32(registers + VF2_MMC_PHASE_REGISTER, value);
}
