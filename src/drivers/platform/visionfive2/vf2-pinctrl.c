/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The JH7110's pin controllers (SYS and AON) and their GPIO pins.
 *
 * Both controllers keep four pins' fields in each 32-bit register, eight
 * bits apart: the output enable selector of every pin, then the output
 * signal selector of every pin, then, for every input signal of the
 * devices, the pin it listens to.  A level register shows the pins' input
 * levels, and every pin has a pad register of its own with the receiver,
 * drive strength, pull resistors, slew rate and Schmitt trigger.
 *
 * The device tree's GPIOMUX numbers name the selectors directly: an output
 * signal, an output enable and an input signal per pin.  Selector 0 and 1
 * of the output and output enable lists are the constants low and high,
 * so a GPIO pin is an output signal of 0 or 1 with an enable of 0
 * (driving) or 1 (not driving).
 *
 * Every change is a read-modify-write under pinctrl_lock, and a field is
 * written only when it changes, so applying a group the firmware already
 * set up writes nothing.
 */

#include "drivers/platform/visionfive2/vf2-pinctrl.h"
#include <kern/device-io.h>
#include <kern/lock.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

/* How many pins' fields one selector register holds, and their spacing. */
#define PIN_FIELDS_PER_REGISTER	4U
#define PIN_FIELD_SPACING	8U

/*
 * The fields of a pad register.
 *
 * Values referenced:
 * StarFive JH7110 TRM Preliminary V2, section 2.8.7 SYS IOMUX CFG,
 * Table 2-651 (PADCFG_PAD_GPIO0_IE, _DS, _PU, _PD, _SLEW, _SMT).
 */
#define PAD_INPUT_ENABLE	0x01U
#define PAD_DRIVE_SHIFT		1U
#define PAD_DRIVE_MASK		0x06U
#define PAD_PULL_UP		0x08U
#define PAD_PULL_DOWN		0x10U
#define PAD_SLEW		0x20U
#define PAD_SCHMITT		0x40U

/*
 * The fields of a GPIOMUX number in a pinmux property.
 *
 * Values referenced:
 * Linux v6.12 arch/riscv/boot/dts/starfive/jh7110-pinfunc.h (GPIOMUX:
 * input signal in bits 24 to 31, output signal in 16 to 23, output enable
 * in 10 to 15, function in 8 and 9, pin in 0 to 7; GPI_NONE 255).
 */
#define PINMUX_INPUT_SHIFT	24U
#define PINMUX_OUTPUT_SHIFT	16U
#define PINMUX_ENABLE_SHIFT	10U
#define PINMUX_ENABLE_MASK	0x3fU
#define PINMUX_FUNCTION_SHIFT	8U
#define PINMUX_FUNCTION_MASK	0x3U
#define PINMUX_PIN_MASK		0xffU
#define PINMUX_NO_INPUT		0xffU

/* The output and enable selectors that make a pin a plain GPIO. */
#define SELECT_LOW		0U
#define SELECT_HIGH		1U
#define SELECT_DRIVE		0U
#define SELECT_FLOAT		1U

/* An input selector names a pin as its number plus two. */
#define INPUT_PIN_OFFSET	2U

/* The device tree's GPIO flag for an active-low pin. */
#define GPIO_FLAG_ACTIVE_LOW	1U

/* The most pin groups a device's pinctrl-0 may select. */
#define PINCTRL_MAX_GROUPS	8U

/*
 * The register layout of one pin controller.
 *
 * Each selector list starts at its base and holds one field of the given
 * width per pin, or per input signal for the input list.  The level
 * register holds 32 pins per register, and each pin's pad register is
 * four bytes after the previous pin's.
 */
struct pinctrl_layout {
	const char *compatible;
	uint32_t pin_count;
	uint32_t enable_base;
	unsigned enable_bits;
	uint32_t output_base;
	unsigned output_bits;
	uint32_t input_base;
	unsigned input_bits;
	uint32_t input_count;
	uint32_t level_base;
	uint32_t pad_base;
	uint32_t block_size;
};

/*
 * One controller's place in the device tree and in the kernel's address
 * space.
 *
 * Filled by drv_vf2_pinctrl_init() at boot and never changed afterwards.
 * Registers is NULL while the controller is absent.
 */
struct pinctrl_instance {
	uint32_t node;
	volatile uint8_t *registers;
};

/*
 * The pad settings a pin group asks for.
 *
 * Mask selects the pad register bits the group's properties name; value
 * holds what they ask for.  Bits outside the mask stay as they are.
 */
struct pad_request {
	uint32_t mask;
	uint32_t value;
};

/*
 * The two controllers' layouts, in drv_vf2_gpio_bank order.
 *
 * Values referenced:
 * StarFive JH7110 TRM Preliminary V2, section 2.8.7 SYS IOMUX CFG
 * (gpo doen at 0x0, gpo dout at 0x40, gpi at 0x80, gpio_in at 0x118,
 * PADCFG_PAD_GPIO0 at 0x120) and section 2.8.8 AON IOMUX CFG (doen at
 * 0x0, dout at 0x4, gpi at 0x8, gpio_in at 0x2c, PADCFG_PAD_RGPIO0 at
 * 0x34); Linux v6.12 arch/riscv/boot/dts/starfive/jh7110-pinfunc.h (the
 * last GPI_SYS_* and GPI_AON_* numbers).
 */
static const struct pinctrl_layout pinctrl_layouts[DRV_VF2_GPIO_BANK_COUNT] = {
	{ "starfive,jh7110-sys-pinctrl", 64U, 0x0U, 6U, 0x40U, 7U, 0x80U, 7U, 91U,
	  0x118U, 0x120U, 0x10000U },
	{ "starfive,jh7110-aon-pinctrl", 4U, 0x0U, 3U, 0x4U, 4U, 0x8U, 3U, 4U,
	  0x2cU, 0x34U, 0x10000U }
};

/*
 * The mapped controllers, in drv_vf2_gpio_bank order.
 *
 * Written only by drv_vf2_pinctrl_init(), which the board runs once on the
 * boot CPU before any driver attaches; read without a lock afterwards.
 */
static struct pinctrl_instance pinctrl_instances[DRV_VF2_GPIO_BANK_COUNT];

/*
 * Serializes every read-modify-write of a controller register.
 *
 * A leaf lock: nothing else is taken while it is held.  Taken with
 * interrupts held off, so a pin may also be driven from an interrupt.
 */
static struct spinlock pinctrl_lock = {
	{ 0 }, LOCK_RANK_DEVICE, "vf2 pinctrl", 0, 0
};

static int map_controller(const struct drv_fdt *fdt, enum drv_vf2_gpio_bank bank);
static int bank_of(uint32_t node, enum drv_vf2_gpio_bank *bank);
static int apply_group(const struct drv_fdt *fdt, uint32_t group, unsigned *changed);
static int apply_pins(const struct drv_fdt *fdt, enum drv_vf2_gpio_bank bank, uint32_t pins, unsigned *changed);
static int pad_request_of(const struct drv_fdt *fdt, uint32_t pins, struct pad_request *request);
static int has_property(const struct drv_fdt *fdt, uint32_t node, const char *name);
static void update_field(enum drv_vf2_gpio_bank bank, uint32_t base, uint32_t field, unsigned bits, uint32_t value, unsigned *changed);
static void update_pad(enum drv_vf2_gpio_bank bank, uint32_t pin, const struct pad_request *request, unsigned *changed);
static int check_gpio(const struct drv_vf2_gpio *gpio);

/*
 * Finds the two pin controllers in the device tree and maps them.
 */
int
drv_vf2_pinctrl_init(
	const struct drv_fdt *fdt)
{
	unsigned bank;
	int error;

	/* Refuses a missing tree. */
	if (fdt == NULL)
		return EINVAL;

	/* Maps every controller the tree describes; a missing one stays absent. */
	for (bank = 0; bank < DRV_VF2_GPIO_BANK_COUNT; bank++) {
		error = map_controller(fdt, (enum drv_vf2_gpio_bank)bank);
		if (error != 0 && error != ENOENT)
			return error;
	}

	/* Refuses a tree without the SYS controller, which has the board's pins. */
	if (pinctrl_instances[DRV_VF2_GPIO_SYS].registers == NULL)
		return ENODEV;

	/* Succeeded: the controllers the tree describes are mapped. */
	return 0;
}

/*
 * Applies the pin groups a device's pinctrl-0 selects.
 */
int
drv_vf2_pinctrl_apply_default(
	const struct drv_fdt *fdt,
	uint32_t node,
	unsigned *changed)
{
	const uint8_t *value;
	uint32_t length;
	uint32_t group;
	uint32_t phandle;
	unsigned position;
	int error;

	/* Refuses a missing blob or count. */
	if (fdt == NULL || changed == NULL)
		return EINVAL;

	/* Starts the count of changed registers at zero. */
	*changed = 0;

	/* Reads the groups the device selects. */
	error = drv_fdt_property(fdt, node, "pinctrl-0", &value, &length);
	if (error != 0)
		return error;
	if ((length & 3U) != 0 || length / 4U > PINCTRL_MAX_GROUPS)
		return EINVAL;

	/* Applies each group in the order the device lists them. */
	for (position = 0; position < length / 4U; position++) {
		/* Finds the group's node. */
		phandle = (uint32_t)drv_fdt_cells_value(value, position, 1U);
		error = drv_fdt_find_phandle(fdt, phandle, &group);
		if (error != 0)
			return error;

		/* Sets the group's pins up. */
		error = apply_group(fdt, group, changed);
		if (error != 0)
			return error;
	}

	/* Succeeded: every group is in place. */
	return 0;
}

/*
 * Finds the pin a device's *-gpios property names.
 */
int
drv_vf2_gpio_lookup(
	const struct drv_fdt *fdt,
	uint32_t node,
	const char *name,
	struct drv_vf2_gpio *gpio)
{
	const uint8_t *value;
	uint32_t length;
	uint32_t controller;
	uint32_t phandle;
	uint32_t flags;
	enum drv_vf2_gpio_bank bank;
	int error;

	/* Refuses a missing blob, name or result. */
	if (fdt == NULL || name == NULL || gpio == NULL)
		return EINVAL;

	/* Reads the property: the controller, the pin and its flags. */
	error = drv_fdt_property(fdt, node, name, &value, &length);
	if (error != 0)
		return error;
	if (length != 12U)
		return EINVAL;

	/* Finds the controller the phandle names. */
	phandle = (uint32_t)drv_fdt_cells_value(value, 0, 1U);
	error = drv_fdt_find_phandle(fdt, phandle, &controller);
	if (error != 0)
		return error;

	/* Finds which of the mapped controllers it is. */
	error = bank_of(controller, &bank);
	if (error != 0)
		return error;

	/* Fills the pin in, with its active state from the flags. */
	gpio->bank = bank;
	gpio->pin = (uint32_t)drv_fdt_cells_value(value, 1U, 1U);
	flags = (uint32_t)drv_fdt_cells_value(value, 2U, 1U);
	gpio->active_low = 0;
	if ((flags & GPIO_FLAG_ACTIVE_LOW) != 0)
		gpio->active_low = 1;

	/* Refuses a pin the controller does not have. */
	error = check_gpio(gpio);
	if (error != 0)
		return error;

	/* Succeeded: the pin and its active state. */
	return 0;
}

/*
 * Makes a pin an input: its output driver off and its receiver on.
 */
int
drv_vf2_gpio_configure_input(
	const struct drv_vf2_gpio *gpio)
{
	const struct pinctrl_layout *layout;
	struct pad_request request;
	unsigned long irq;
	unsigned changed;
	int error;

	/* Refuses a pin the controllers do not have. */
	error = check_gpio(gpio);
	if (error != 0)
		return error;
	layout = &pinctrl_layouts[gpio->bank];

	/* The receiver and its Schmitt trigger on, the rest of the pad kept. */
	request.mask = PAD_INPUT_ENABLE | PAD_SCHMITT;
	request.value = PAD_INPUT_ENABLE | PAD_SCHMITT;

	/* Turns the receiver on, then stops driving the pin. */
	changed = 0;
	irq = spin_lock_irqsave(&pinctrl_lock);

	update_pad(gpio->bank, gpio->pin, &request, &changed);
	update_field(gpio->bank, layout->enable_base, gpio->pin, layout->enable_bits, SELECT_FLOAT, &changed);

	spin_unlock_irqrestore(&pinctrl_lock, irq);

	/* Succeeded: the pin is an input. */
	return 0;
}

/*
 * Makes a pin an output driving the given active state.
 */
int
drv_vf2_gpio_configure_output(
	const struct drv_vf2_gpio *gpio,
	int active)
{
	const struct pinctrl_layout *layout;
	unsigned long irq;
	unsigned changed;
	uint32_t level;
	int error;

	/* Refuses a pin the controllers do not have. */
	error = check_gpio(gpio);
	if (error != 0)
		return error;
	layout = &pinctrl_layouts[gpio->bank];

	/* Turns the active state into the level on the pin. */
	level = SELECT_LOW;
	if ((active != 0) != (gpio->active_low != 0))
		level = SELECT_HIGH;

	/* Selects the level first, then starts driving the pin. */
	changed = 0;
	irq = spin_lock_irqsave(&pinctrl_lock);

	update_field(gpio->bank, layout->output_base, gpio->pin, layout->output_bits, level, &changed);
	update_field(gpio->bank, layout->enable_base, gpio->pin, layout->enable_bits, SELECT_DRIVE, &changed);

	spin_unlock_irqrestore(&pinctrl_lock, irq);

	/* Succeeded: the pin drives the state. */
	return 0;
}

/*
 * Reads whether a pin is in its active state.
 */
int
drv_vf2_gpio_read(
	const struct drv_vf2_gpio *gpio,
	int *active)
{
	const struct pinctrl_layout *layout;
	volatile uint8_t *registers;
	uint32_t levels;
	uint32_t offset;
	int high;
	int error;

	/* Refuses a missing result or a pin the controllers do not have. */
	if (active == NULL)
		return EINVAL;
	error = check_gpio(gpio);
	if (error != 0)
		return error;
	layout = &pinctrl_layouts[gpio->bank];
	registers = pinctrl_instances[gpio->bank].registers;

	/* Reads the pin's input level. */
	offset = layout->level_base + (gpio->pin / 32U) * 4U;
	levels = kern_mmio_read32((volatile uint32_t *)(registers + offset));
	high = 0;
	if ((levels & (1U << (gpio->pin % 32U))) != 0)
		high = 1;

	/* Succeeded: a high level is active unless the pin is active low. */
	*active = high;
	if (gpio->active_low != 0)
		*active = !high;
	return 0;
}

/*
 * Drives an output pin to its active or inactive state.
 */
int
drv_vf2_gpio_write(
	const struct drv_vf2_gpio *gpio,
	int active)
{
	const struct pinctrl_layout *layout;
	unsigned long irq;
	unsigned changed;
	uint32_t level;
	int error;

	/* Refuses a pin the controllers do not have. */
	error = check_gpio(gpio);
	if (error != 0)
		return error;
	layout = &pinctrl_layouts[gpio->bank];

	/* Turns the active state into the level on the pin. */
	level = SELECT_LOW;
	if ((active != 0) != (gpio->active_low != 0))
		level = SELECT_HIGH;

	/* Selects the level the pin drives. */
	changed = 0;
	irq = spin_lock_irqsave(&pinctrl_lock);

	update_field(gpio->bank, layout->output_base, gpio->pin, layout->output_bits, level, &changed);

	spin_unlock_irqrestore(&pinctrl_lock, irq);

	/* Succeeded: the pin drives the state. */
	return 0;
}

/* Finds and maps one controller, recording where it is. */
static int
map_controller(
	const struct drv_fdt *fdt,
	enum drv_vf2_gpio_bank bank)
{
	const struct pinctrl_layout *layout;
	struct pinctrl_instance *instance;
	uint64_t address;
	uint64_t size;
	uint32_t node;
	void *mapped;
	int error;

	/* Finds the controller's node; a tree without it leaves it absent. */
	layout = &pinctrl_layouts[bank];
	instance = &pinctrl_instances[bank];
	error = drv_fdt_find_compatible(fdt, layout->compatible, DRV_FDT_NO_NODE, &node);
	if (error != 0)
		return ENOENT;

	/* Reads where the registers are; the block must hold every register. */
	error = drv_fdt_reg(fdt, node, 0, &address, &size);
	if (error != 0)
		return error;
	if (size < layout->block_size)
		return EINVAL;

	/* Maps the registers as device memory. */
	error = kern_device_map(address, layout->block_size, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0)
		return error;

	/* Succeeded: records the controller for the other calls. */
	instance->node = node;
	instance->registers = mapped;
	return 0;
}

/* Finds which mapped controller a node is. */
static int
bank_of(
	uint32_t node,
	enum drv_vf2_gpio_bank *bank)
{
	unsigned candidate;

	/* Compares the node with each mapped controller's. */
	for (candidate = 0; candidate < DRV_VF2_GPIO_BANK_COUNT; candidate++) {
		/* A controller counts only once it is mapped. */
		if (pinctrl_instances[candidate].registers == NULL)
			continue;

		/* Stops at the controller the node is. */
		if (pinctrl_instances[candidate].node == node)
			break;
	}

	/* Refuses a node that is no pin controller. */
	if (candidate == DRV_VF2_GPIO_BANK_COUNT)
		return ENODEV;

	/* Succeeded: the controller's bank. */
	*bank = (enum drv_vf2_gpio_bank)candidate;
	return 0;
}

/*
 * Applies one pin group: its own pinmux, if any, and each child's.
 *
 * The JH7110's groups put their pins in child nodes, one per set of pad
 * settings.
 */
static int
apply_group(
	const struct drv_fdt *fdt,
	uint32_t group,
	unsigned *changed)
{
	enum drv_vf2_gpio_bank bank;
	uint32_t controller;
	uint32_t child;
	int present;
	int error;

	/* Finds the controller node the group belongs to. */
	error = drv_fdt_parent(fdt, group, &controller);
	if (error != 0)
		return error;

	/* Finds which of the mapped controllers it is. */
	error = bank_of(controller, &bank);
	if (error != 0)
		return error;

	/* Applies pins the group lists itself. */
	present = has_property(fdt, group, "pinmux");
	if (present) {
		error = apply_pins(fdt, bank, group, changed);
		if (error != 0)
			return error;
	}

	/* Applies the pins of every child that lists some. */
	error = drv_fdt_child(fdt, group, DRV_FDT_NO_NODE, &child);
	while (error == 0) {
		/* A child without pins is skipped. */
		present = has_property(fdt, child, "pinmux");
		if (present) {
			error = apply_pins(fdt, bank, child, changed);
			if (error != 0)
				return error;
		}

		/* Moves on to the next child. */
		error = drv_fdt_child(fdt, group, child, &child);
	}

	/* Reports a walk that stopped on a damaged tree. */
	if (error != ENOENT)
		return error;

	/* Succeeded: the group's pins are in place. */
	return 0;
}

/*
 * Applies one node's pinmux list with the pad settings the node gives.
 *
 * Procedure referenced:
 * OpenBSD sys/dev/fdt/stfpinctrl.c (stfpinctrl_jh7110_config_pin: the
 * output enable, output and pad registers of a pin).
 */
static int
apply_pins(
	const struct drv_fdt *fdt,
	enum drv_vf2_gpio_bank bank,
	uint32_t pins,
	unsigned *changed)
{
	const struct pinctrl_layout *layout;
	struct pad_request request;
	const uint8_t *value;
	uint32_t length;
	uint32_t number;
	uint32_t pin;
	uint32_t output;
	uint32_t enable;
	uint32_t input;
	uint32_t function;
	unsigned position;
	unsigned long irq;
	int error;

	/* Reads the node's pins, refusing a list of part cells. */
	layout = &pinctrl_layouts[bank];
	error = drv_fdt_property(fdt, pins, "pinmux", &value, &length);
	if (error != 0)
		return error;
	if ((length & 3U) != 0)
		return EINVAL;

	/* Reads the pad settings the pins share. */
	error = pad_request_of(fdt, pins, &request);
	if (error != 0)
		return error;

	/* Checks every pin before writing any, so a bad list changes nothing. */
	for (position = 0; position < length / 4U; position++) {
		/* Decodes the selectors of one pin. */
		number = (uint32_t)drv_fdt_cells_value(value, position, 1U);
		pin = number & PINMUX_PIN_MASK;
		output = (number >> PINMUX_OUTPUT_SHIFT) & 0xffU;
		enable = (number >> PINMUX_ENABLE_SHIFT) & PINMUX_ENABLE_MASK;
		input = number >> PINMUX_INPUT_SHIFT;
		function = (number >> PINMUX_FUNCTION_SHIFT) & PINMUX_FUNCTION_MASK;

		/* Refuses a dedicated function, which only GPIOMUX pins do not use. */
		if (function != 0)
			return ENOTSUP;

		/* Refuses a pin or selector the controller does not have. */
		if (pin >= layout->pin_count)
			return EINVAL;
		if (output >= (1U << layout->output_bits))
			return EINVAL;
		if (enable >= (1U << layout->enable_bits))
			return EINVAL;
		if (input != PINMUX_NO_INPUT && input >= layout->input_count)
			return EINVAL;
	}

	/* Writes each pin's selectors and pad. */
	irq = spin_lock_irqsave(&pinctrl_lock);

	for (position = 0; position < length / 4U; position++) {
		/* Decodes the selectors of one pin again. */
		number = (uint32_t)drv_fdt_cells_value(value, position, 1U);
		pin = number & PINMUX_PIN_MASK;
		output = (number >> PINMUX_OUTPUT_SHIFT) & 0xffU;
		enable = (number >> PINMUX_ENABLE_SHIFT) & PINMUX_ENABLE_MASK;
		input = number >> PINMUX_INPUT_SHIFT;

		/* Sets the pad before the pin starts driving or listening. */
		update_pad(bank, pin, &request, changed);
		update_field(bank, layout->output_base, pin, layout->output_bits, output, changed);
		update_field(bank, layout->enable_base, pin, layout->enable_bits, enable, changed);

		/* Points the device's input signal at the pin. */
		if (input != PINMUX_NO_INPUT) {
			update_field(bank,
				     layout->input_base,
				     input,
				     layout->input_bits,
				     pin + INPUT_PIN_OFFSET,
				     changed);
		}
	}

	spin_unlock_irqrestore(&pinctrl_lock, irq);

	/* Succeeded: the node's pins are in place. */
	return 0;
}

/*
 * Collects the pad settings a pin node's generic properties ask for.
 *
 * Values referenced:
 * StarFive JH7110 TRM Preliminary V2, Table 2-651 (PADCFG_PAD_GPIO0_DS:
 * 2, 4, 8 and 12 mA).
 */
static int
pad_request_of(
	const struct drv_fdt *fdt,
	uint32_t pins,
	struct pad_request *request)
{
	const uint8_t *value;
	uint32_t length;
	uint64_t slew;
	uint32_t milliamps;
	uint32_t drive;
	int present;
	int error;

	/* Starts with nothing asked for. */
	request->mask = 0;
	request->value = 0;

	/* Turns both pull resistors off. */
	present = has_property(fdt, pins, "bias-disable");
	if (present)
		request->mask |= PAD_PULL_UP | PAD_PULL_DOWN;

	/* Pulls the pin up. */
	present = has_property(fdt, pins, "bias-pull-up");
	if (present) {
		request->mask |= PAD_PULL_UP | PAD_PULL_DOWN;
		request->value |= PAD_PULL_UP;
	}

	/* Pulls the pin down. */
	present = has_property(fdt, pins, "bias-pull-down");
	if (present) {
		request->mask |= PAD_PULL_UP | PAD_PULL_DOWN;
		request->value |= PAD_PULL_DOWN;
	}

	/* Turns the receiver on or off. */
	present = has_property(fdt, pins, "input-enable");
	if (present) {
		request->mask |= PAD_INPUT_ENABLE;
		request->value |= PAD_INPUT_ENABLE;
	}

	/* A disable clears the receiver's bit. */
	present = has_property(fdt, pins, "input-disable");
	if (present)
		request->mask |= PAD_INPUT_ENABLE;

	/* Turns the Schmitt trigger on or off. */
	present = has_property(fdt, pins, "input-schmitt-enable");
	if (present) {
		request->mask |= PAD_SCHMITT;
		request->value |= PAD_SCHMITT;
	}

	/* A disable clears the trigger's bit. */
	present = has_property(fdt, pins, "input-schmitt-disable");
	if (present)
		request->mask |= PAD_SCHMITT;

	/* Selects the fast or slow slew rate. */
	error = drv_fdt_property(fdt, pins, "slew-rate", &value, &length);
	if (error == 0) {
		/* Refuses a value that is not one cell. */
		if (length != 4U)
			return EINVAL;

		/* A nonzero rate is the fast one. */
		slew = drv_fdt_cells_value(value, 0, 1U);
		request->mask |= PAD_SLEW;
		if (slew != 0)
			request->value |= PAD_SLEW;
	} else if (error != ENOENT) {
		return error;
	}

	/* Reads the drive strength in milliamperes; none leaves it alone. */
	error = drv_fdt_property(fdt, pins, "drive-strength", &value, &length);
	if (error == ENOENT)
		return 0;
	if (error != 0)
		return error;
	if (length != 4U)
		return EINVAL;
	milliamps = (uint32_t)drv_fdt_cells_value(value, 0, 1U);

	/* Turns the strength into the pad's two-bit code. */
	switch (milliamps) {
	case 2:
		drive = 0;
		break;
	case 4:
		drive = 1;
		break;
	case 8:
		drive = 2;
		break;
	case 12:
		drive = 3;
		break;
	default:
		/* The pads have no other strengths. */
		return EINVAL;
	}

	/* Succeeded: the strength joins the settings. */
	request->mask |= PAD_DRIVE_MASK;
	request->value |= drive << PAD_DRIVE_SHIFT;
	return 0;
}

/* Reports whether a node has a property, such as a flag with no value. */
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

	/* Reports that the node has it. */
	return 1;
}

/*
 * Writes one selector field when it differs from the value.
 *
 * The field is the field-th of the list at base; the caller holds
 * pinctrl_lock and has checked the field and the value.
 */
static void
update_field(
	enum drv_vf2_gpio_bank bank,
	uint32_t base,
	uint32_t field,
	unsigned bits,
	uint32_t value,
	unsigned *changed)
{
	volatile uint32_t *address;
	uint32_t contents;
	uint32_t mask;
	unsigned shift;

	/* Finds the register and the field's place in it. */
	address = (volatile uint32_t *)(pinctrl_instances[bank].registers + base +
					(field / PIN_FIELDS_PER_REGISTER) * 4U);
	shift = (field % PIN_FIELDS_PER_REGISTER) * PIN_FIELD_SPACING;
	mask = ((1U << bits) - 1U) << shift;

	/* Leaves a field that already holds the value alone. */
	contents = kern_mmio_read32(address);
	if ((contents & mask) == ((value << shift) & mask))
		return;

	/* Writes the field and counts the change. */
	contents &= ~mask;
	contents |= (value << shift) & mask;
	kern_mmio_write32(address, contents);
	(*changed)++;
}

/*
 * Writes the bits of a pin's pad register that a request names, when they
 * differ.
 *
 * The caller holds pinctrl_lock and has checked the pin.
 */
static void
update_pad(
	enum drv_vf2_gpio_bank bank,
	uint32_t pin,
	const struct pad_request *request,
	unsigned *changed)
{
	volatile uint32_t *address;
	uint32_t contents;

	/* Leaves a pad the request does not touch alone. */
	if (request->mask == 0)
		return;

	/* Leaves a pad that already holds the settings alone. */
	address = (volatile uint32_t *)(pinctrl_instances[bank].registers +
					pinctrl_layouts[bank].pad_base + pin * 4U);
	contents = kern_mmio_read32(address);
	if ((contents & request->mask) == request->value)
		return;

	/* Writes the settings and counts the change. */
	contents &= ~request->mask;
	contents |= request->value;
	kern_mmio_write32(address, contents);
	(*changed)++;
}

/* Refuses a pin of an absent controller or one the controller lacks. */
static int
check_gpio(
	const struct drv_vf2_gpio *gpio)
{
	/* Refuses a missing pin or a controller number outside the two. */
	if (gpio == NULL)
		return EINVAL;
	if ((unsigned)gpio->bank >= DRV_VF2_GPIO_BANK_COUNT)
		return EINVAL;

	/* Refuses a controller the tree did not describe. */
	if (pinctrl_instances[gpio->bank].registers == NULL)
		return ENODEV;

	/* Refuses a pin the controller does not have. */
	if (gpio->pin >= pinctrl_layouts[gpio->bank].pin_count)
		return EINVAL;

	/* Succeeded: the pin exists. */
	return 0;
}
