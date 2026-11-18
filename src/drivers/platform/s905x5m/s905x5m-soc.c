/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The S905X5M's shared blocks for its peripheral drivers.
 *
 * The bootloader has switched the clocks the boot needed on and left the
 * pins of the SD card it read in their functions; the drivers still set
 * what they depend on themselves, through these calls, so that a driver
 * does not rely on what the bootloader happened to do.  Every register
 * here is shared by several drivers: each change is a read, a change of
 * the caller's bits and a write, under one lock.
 *
 * The device tree is opened once, from the physical address the HAL
 * handed over, and read in place for the life of the system.
 */

#include <drivers/platform/s905x5m/s905x5m-soc.h>

#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

/*
 * Where the shared blocks are, and how much of each is mapped.
 *
 * Values referenced: Hardkernel common_drivers odroids7d-6.12.y @34daba9
 * arch/arm64/boot/dts/amlogic/meson-s7d.dtsi (clock-controller reg
 * 0xfe000000, reset-controller@2000, pinctrl bank@4000 "mux" 0xfe004000 and
 * "gpio" 0xfe0040c0).
 */
#define S905X5M_CLOCK_BASE		0xfe000000ULL
#define S905X5M_RESET_BASE		0xfe002000ULL
#define S905X5M_PINS_BASE		0xfe004000ULL
#define S905X5M_BLOCK_BYTES		0x1000U

/*
 * The reset controller's two kinds of register: a write of a bit to the
 * pulse words resets the block once, and a level word holds a block in
 * reset while its bit is clear.  Each word serves 32 resets.
 *
 * Values referenced: Linux v7.3 drivers/reset/reset-meson.c
 * (meson_s4_param: reset_offset 0x0, level_offset 0x40) and Hardkernel
 * common_drivers odroids7d-6.12.y @34daba9
 * drivers/usb/phy_meson_usb/phy-meson-aml-usb2-com.c
 * (meson_u2phy_hold_reset: the level bit set takes the block out of reset).
 */
#define S905X5M_RESET_PULSE		0x00U
#define S905X5M_RESET_LEVEL		0x40U

/* Where the GPIO registers start inside the pin block. */
#define S905X5M_GPIO_OFFSET		0xc0U

/*
 * The words of one GPIO bank, counted from the bank's first word: the
 * input levels, the output levels, the directions (a set bit is an
 * input), the pull enables, the pull directions (a set bit pulls up), and
 * the drive strengths (two bits a pin).
 *
 * Values referenced: Hardkernel common_drivers odroids7d-6.12.y @34daba9
 * drivers/gpio/pinctrl/pinctrl-meson-s7d.c (meson_s7d_periphs_banks, the
 * in, out, dir, pullen, pull and ds columns).
 */
#define S905X5M_GPIO_IN			0x0U
#define S905X5M_GPIO_OUT		0x1U
#define S905X5M_GPIO_DIRECTION		0x2U
#define S905X5M_GPIO_PULL_ENABLE	0x3U
#define S905X5M_GPIO_PULL_UP		0x4U
#define S905X5M_GPIO_DRIVE		0x7U

/* The bits of the pin multiplexer a pin's function takes, and how many pins one word holds. */
#define S905X5M_MUX_BITS		4U
#define S905X5M_MUX_PINS_PER_WORD	8U
#define S905X5M_MUX_FUNCTION_MASK	0xfU

/*
 * The drive strength codes of a pin, by the most current each one gives.
 *
 * Values referenced: Linux v7.3 drivers/pinctrl/meson/pinctrl-meson.c
 * (meson_pinconf_set_drive_strength) and pinctrl-meson.h
 * (MESON_PINCONF_DRV_500UA .. MESON_PINCONF_DRV_4000UA).
 */
#define S905X5M_DRIVE_500UA		0U
#define S905X5M_DRIVE_2500UA		1U
#define S905X5M_DRIVE_3000UA		2U
#define S905X5M_DRIVE_4000UA		3U

/* The most bytes of the device tree the drivers read. */
#define S905X5M_FDT_READ_LIMIT		(1024U * 1024U)

/* The cells of one GIC interrupt specifier, and its kinds. */
#define S905X5M_INTERRUPT_CELLS		3U
#define S905X5M_INTERRUPT_SPI		0U
#define S905X5M_INTERRUPT_PPI		1U
#define S905X5M_GIC_SPI_BASE		32U
#define S905X5M_GIC_PPI_BASE		16U
#define S905X5M_INTERRUPT_EDGE_MASK	0x3U

/* The cells of one GPIO specifier: the controller, the pin and its flags. */
#define S905X5M_GPIO_CELLS		3U

/*
 * One GPIO bank: its first pin number in the device tree's numbering, how
 * many pins it has, the word of the pin multiplexer that holds its first
 * pin, and the first of its GPIO words.
 */
struct s905x5m_bank {
	const char *name;
	unsigned first;
	unsigned count;
	uint32_t mux_word;
	uint32_t gpio_word;
};

/*
 * The shared blocks once mapped, and the device tree.
 *
 * Filled by the first drv_s905x5m_soc_init() on the boot thread and kept
 * for the life of the system; ready stays zero until every mapping and the
 * tree are in place.  The lock serializes every read-modify-write of a
 * shared register.
 */
struct s905x5m_soc {
	int ready;
	volatile uint8_t *clock;
	volatile uint8_t *reset;
	volatile uint8_t *pins;
	struct drv_fdt fdt;
	struct spinlock lock;
};

/*
 * The GPIO banks of the peripheral pin controller, in the order of the
 * device tree's pin numbers.
 *
 * Values referenced: Hardkernel common_drivers odroids7d-6.12.y @34daba9
 * drivers/gpio/pinctrl/pinctrl-meson-s7d.c (meson_s7d_periphs_banks: the
 * in column gives each bank's first GPIO word; meson_s7d_periphs_pmx_banks:
 * each bank's first multiplexer word) and
 * include/dt-bindings/gpio/meson-s7d-gpio.h (the pin numbers).
 */
static const struct s905x5m_bank s905x5m_banks[] = {
	{ "E", DRV_S905X5M_GPIO_E, 2U, 0x12U, 0x40U },
	{ "B", DRV_S905X5M_GPIO_B, 14U, 0x00U, 0x60U },
	{ "C", DRV_S905X5M_GPIO_C, 8U, 0x09U, 0x50U },
	{ "D", DRV_S905X5M_GPIO_D, 5U, 0x10U, 0x30U },
	{ "DV", DRV_S905X5M_GPIO_DV, 7U, 0x02U, 0x70U },
	{ "H", DRV_S905X5M_GPIO_H, 12U, 0x0bU, 0x20U },
	{ "X", DRV_S905X5M_GPIO_X, 20U, 0x03U, 0x10U },
	{ "Z", DRV_S905X5M_GPIO_Z, 13U, 0x06U, 0x00U },
	{ "TEST_N", DRV_S905X5M_GPIO_TEST_N, 1U, 0x0fU, 0x80U }
};

/* The number of banks in s905x5m_banks. */
#define S905X5M_BANK_COUNT	(sizeof(s905x5m_banks) / sizeof(s905x5m_banks[0]))

/* The one view of the SoC's shared blocks. */
static struct s905x5m_soc s905x5m_soc;

static int map_block(uint64_t address, volatile uint8_t **mapped);
static const struct s905x5m_bank *find_bank(unsigned pin, unsigned *index);
static void update_word(volatile uint8_t *block, uint32_t offset, uint32_t mask, uint32_t value);
static int update_gpio_bit(unsigned pin, uint32_t word, int set);
static int start_deadline(uint64_t microseconds, uint64_t *deadline);
static int deadline_passed(uint64_t deadline);

/*
 * Maps the shared blocks and opens the device tree at fdt_phys.
 *
 * A later call finds them ready and does nothing.  Returns 0, ENODEV when
 * the tree cannot be reached, or the errno of the mapping that failed.
 */
int
drv_s905x5m_soc_init(
	uint64_t fdt_phys)
{
	const void *blob;
	int error;

	/* A second driver finds the blocks mapped already. */
	if (s905x5m_soc.ready)
		return 0;

	/* Opens the device tree through the direct map. */
	blob = kern_pmem_to_kernel((kern_physaddr_t)fdt_phys);
	if (blob == NULL)
		return ENODEV;
	error = drv_fdt_open(&s905x5m_soc.fdt, blob, S905X5M_FDT_READ_LIMIT);
	if (error != 0)
		return error;

	/* Maps the clock controller. */
	error = map_block(S905X5M_CLOCK_BASE, &s905x5m_soc.clock);
	if (error != 0)
		return error;

	/* Maps the reset controller. */
	error = map_block(S905X5M_RESET_BASE, &s905x5m_soc.reset);
	if (error != 0)
		return error;

	/* Maps the pin multiplexer and the GPIO banks. */
	error = map_block(S905X5M_PINS_BASE, &s905x5m_soc.pins);
	if (error != 0)
		return error;

	/* Prepares the lock of the shared registers. */
	spin_init(&s905x5m_soc.lock, LOCK_RANK_DEVICE, "S905X5M SoC");

	/* Succeeded: the drivers may use the shared blocks. */
	s905x5m_soc.ready = 1;
	return 0;
}

/*
 * Returns the device tree opened by drv_s905x5m_soc_init().
 */
const struct drv_fdt *
drv_s905x5m_fdt(
	void)
{
	/* Succeeded: the tree, which the caller reads only after the init. */
	return &s905x5m_soc.fdt;
}

/*
 * Reads one register of the clock controller.
 */
uint32_t
drv_s905x5m_clock_read(
	uint32_t offset)
{
	uint32_t value;

	/* Reads the register. */
	value = kern_mmio_read32(s905x5m_soc.clock + offset);

	/* Reports the register's value. */
	return value;
}

/*
 * Changes the bits mask of one register of the clock controller to value.
 */
void
drv_s905x5m_clock_update(
	uint32_t offset,
	uint32_t mask,
	uint32_t value)
{
	/* Changes only the caller's bits of the shared register. */
	update_word(s905x5m_soc.clock, offset, mask, value);
}

/*
 * Resets a block once through the reset controller's pulse words.
 */
void
drv_s905x5m_reset_pulse(
	unsigned bit)
{
	uint32_t offset;

	/* Writes the reset's bit alone; the controller ignores the zeros. */
	offset = S905X5M_RESET_PULSE + (bit / 32U) * 4U;
	kern_mmio_write32(s905x5m_soc.reset + offset, 1U << (bit % 32U));
}

/*
 * Holds a block in reset through its level bit.
 */
void
drv_s905x5m_reset_hold(
	unsigned bit)
{
	uint32_t offset;

	/* A clear level bit keeps the block in reset. */
	offset = S905X5M_RESET_LEVEL + (bit / 32U) * 4U;
	update_word(s905x5m_soc.reset, offset, 1U << (bit % 32U), 0);
}

/*
 * Takes a block out of reset through its level bit.
 */
void
drv_s905x5m_reset_release(
	unsigned bit)
{
	uint32_t offset;
	uint32_t mask;

	/* A set level bit lets the block run. */
	offset = S905X5M_RESET_LEVEL + (bit / 32U) * 4U;
	mask = 1U << (bit % 32U);
	update_word(s905x5m_soc.reset, offset, mask, mask);
}

/*
 * Gives a pin one of its functions; function 0 is the GPIO.
 *
 * Returns 0, or EINVAL for a pin or a function the multiplexer does not
 * have.
 */
int
drv_s905x5m_pin_function(
	unsigned pin,
	unsigned function)
{
	const struct s905x5m_bank *bank;
	unsigned index;
	uint32_t offset;
	uint32_t shift;

	/* Refuses a pin no bank has and a function wider than its field. */
	bank = find_bank(pin, &index);
	if (bank == NULL)
		return EINVAL;
	if (function > S905X5M_MUX_FUNCTION_MASK)
		return EINVAL;

	/* Finds the pin's field: eight pins a word from the bank's first word. */
	offset = (bank->mux_word + index / S905X5M_MUX_PINS_PER_WORD) * 4U;
	shift = (index % S905X5M_MUX_PINS_PER_WORD) * S905X5M_MUX_BITS;

	/* Writes the function into the field. */
	update_word(s905x5m_soc.pins,
		    offset,
		    S905X5M_MUX_FUNCTION_MASK << shift,
		    (uint32_t)function << shift);

	/* Succeeded: the pin carries the function. */
	return 0;
}

/*
 * Sets the pull of a pin's input stage.
 *
 * Returns 0, or EINVAL for a pin no bank has.
 */
int
drv_s905x5m_pin_pull(
	unsigned pin,
	enum drv_s905x5m_pull pull)
{
	int error;
	int up;

	/* Turns the pull off for a pin that floats. */
	if (pull == DRV_S905X5M_PULL_NONE) {
		error = update_gpio_bit(pin, S905X5M_GPIO_PULL_ENABLE, 0);
		if (error != 0)
			return error;
		return 0;
	}

	/* A set direction bit pulls the pin up, a clear one down. */
	up = 0;
	if (pull == DRV_S905X5M_PULL_UP)
		up = 1;

	/* Chooses the direction first, so the pin never pulls the wrong way. */
	error = update_gpio_bit(pin, S905X5M_GPIO_PULL_UP, up);
	if (error != 0)
		return error;

	/* Then turns the pull on. */
	error = update_gpio_bit(pin, S905X5M_GPIO_PULL_ENABLE, 1);
	if (error != 0)
		return error;

	/* Succeeded: the pin pulls as asked. */
	return 0;
}

/*
 * Sets the drive strength of a pin to the strongest code that gives at
 * most microamps; anything above 4 mA gets the 4 mA code.
 *
 * Returns 0, or EINVAL for a pin no bank has.
 */
int
drv_s905x5m_pin_drive(
	unsigned pin,
	uint32_t microamps)
{
	const struct s905x5m_bank *bank;
	unsigned index;
	uint32_t code;
	uint32_t bit;
	uint32_t offset;

	/* Refuses a pin no bank has. */
	bank = find_bank(pin, &index);
	if (bank == NULL)
		return EINVAL;

	/* Picks the code of the current, as the device tree's microamps name it. */
	if (microamps <= 500U) {
		code = S905X5M_DRIVE_500UA;
	} else if (microamps <= 2500U) {
		code = S905X5M_DRIVE_2500UA;
	} else if (microamps <= 3000U) {
		code = S905X5M_DRIVE_3000UA;
	} else {
		code = S905X5M_DRIVE_4000UA;
	}

	/* Finds the pin's two bits: sixteen pins a word from the bank's drive word. */
	bit = index * 2U;
	offset = S905X5M_GPIO_OFFSET + (bank->gpio_word + S905X5M_GPIO_DRIVE + bit / 32U) * 4U;
	bit %= 32U;

	/* Writes the code. */
	update_word(s905x5m_soc.pins, offset, 0x3U << bit, code << bit);

	/* Succeeded: the pin drives as asked. */
	return 0;
}

/*
 * Drives a GPIO high or low.
 *
 * The level is written before the direction, so the pin never drives the
 * level it had.  Returns 0, or EINVAL for a pin no bank has.
 */
int
drv_s905x5m_gpio_output(
	unsigned pin,
	int high)
{
	int error;

	/* Sets the level the pin will drive. */
	error = update_gpio_bit(pin, S905X5M_GPIO_OUT, high);
	if (error != 0)
		return error;

	/* Turns the pin into an output. */
	error = update_gpio_bit(pin, S905X5M_GPIO_DIRECTION, 0);
	if (error != 0)
		return error;

	/* Succeeded: the pin drives the level. */
	return 0;
}

/*
 * Stops driving a GPIO, which leaves it an input.
 *
 * Returns 0, or EINVAL for a pin no bank has.
 */
int
drv_s905x5m_gpio_input(
	unsigned pin)
{
	int error;

	/* A set direction bit makes the pin an input. */
	error = update_gpio_bit(pin, S905X5M_GPIO_DIRECTION, 1);
	if (error != 0)
		return error;

	/* Succeeded: the pin is released. */
	return 0;
}

/*
 * Reads the level at a GPIO.
 *
 * Returns 0 with the level, or EINVAL for a pin no bank has.
 */
int
drv_s905x5m_gpio_read(
	unsigned pin,
	int *high)
{
	const struct s905x5m_bank *bank;
	unsigned index;
	uint32_t offset;
	uint32_t levels;

	/* Refuses a pin no bank has. */
	if (high == NULL)
		return EINVAL;
	bank = find_bank(pin, &index);
	if (bank == NULL)
		return EINVAL;

	/* Reads the bank's input levels. */
	offset = S905X5M_GPIO_OFFSET + (bank->gpio_word + S905X5M_GPIO_IN) * 4U;
	levels = kern_mmio_read32(s905x5m_soc.pins + offset);

	/* Succeeded: the pin's level. */
	*high = 0;
	if ((levels & (1U << index)) != 0)
		*high = 1;
	return 0;
}

/*
 * Finds the enabled node of a device by its compatible string and, when
 * address is not zero, by the first address of its reg property.
 *
 * Returns 0 with the node, or ENOENT when the tree has no such device.
 */
int
drv_s905x5m_find_node(
	const char *compatible,
	uint64_t address,
	uint32_t *node)
{
	uint32_t candidate;
	uint64_t found;
	uint64_t size;
	bool enabled;
	int error;

	/* Walks the nodes that carry the compatible string. */
	candidate = DRV_FDT_NO_NODE;
	for (;;) {
		/* Moves to the next one; none left means the tree has no such device. */
		error = drv_fdt_find_compatible(&s905x5m_soc.fdt, compatible, candidate, &candidate);
		if (error != 0)
			return ENOENT;

		/* Skips a node the tree turned off. */
		enabled = drv_fdt_node_enabled(&s905x5m_soc.fdt, candidate);
		if (!enabled)
			continue;

		/* Takes any enabled node when the caller names no address. */
		if (address == 0)
			break;

		/* Takes the node whose registers are at the address. */
		error = drv_fdt_reg(&s905x5m_soc.fdt, candidate, 0, &found, &size);
		if (error != 0)
			continue;
		if (found == address)
			break;
	}

	/* Succeeded: the device's node. */
	*node = candidate;
	return 0;
}

/*
 * Reads one GIC interrupt of a node's interrupts property.
 *
 * Returns 0 with the interrupt, ENOENT when the node has no such entry,
 * or EINVAL for a kind of interrupt the GIC numbering here does not cover.
 */
int
drv_s905x5m_node_interrupt(
	uint32_t node,
	unsigned index,
	struct drv_s905x5m_interrupt *interrupt)
{
	const uint8_t *value;
	uint32_t length;
	uint32_t kind;
	uint32_t number;
	uint32_t flags;
	int error;

	/* Reads the property; an entry is three cells. */
	error = drv_fdt_property(&s905x5m_soc.fdt, node, "interrupts", &value, &length);
	if (error != 0)
		return ENOENT;
	if (length < (index + 1U) * S905X5M_INTERRUPT_CELLS * 4U)
		return ENOENT;

	/* Decodes the kind, the number and the trigger of the entry. */
	kind = (uint32_t)drv_fdt_cells_value(value, index * S905X5M_INTERRUPT_CELLS, 1U);
	number = (uint32_t)drv_fdt_cells_value(value, index * S905X5M_INTERRUPT_CELLS + 1U, 1U);
	flags = (uint32_t)drv_fdt_cells_value(value, index * S905X5M_INTERRUPT_CELLS + 2U, 1U);

	/* Numbers a shared or a private peripheral interrupt as the GIC does. */
	if (kind == S905X5M_INTERRUPT_SPI) {
		interrupt->irq = number + S905X5M_GIC_SPI_BASE;
	} else if (kind == S905X5M_INTERRUPT_PPI) {
		interrupt->irq = number + S905X5M_GIC_PPI_BASE;
	} else {
		return EINVAL;
	}

	/* The two low bits of the flags name an edge trigger. */
	interrupt->edge = 0;
	if ((flags & S905X5M_INTERRUPT_EDGE_MASK) != 0)
		interrupt->edge = 1;

	/* Succeeded: the interrupt as the GIC numbers it. */
	return 0;
}

/*
 * Reads the first GPIO a node's property names, as <controller pin flags>.
 *
 * Returns 0 with the pin and its flags, ENOENT when the node lacks the
 * property, or EINVAL for a pin the SoC does not have.
 */
int
drv_s905x5m_node_gpio(
	uint32_t node,
	const char *name,
	struct drv_s905x5m_gpio *gpio)
{
	const uint8_t *value;
	uint32_t length;
	uint32_t pin;
	int error;

	/* Reads the property; a specifier is three cells. */
	error = drv_fdt_property(&s905x5m_soc.fdt, node, name, &value, &length);
	if (error != 0)
		return ENOENT;
	if (length < S905X5M_GPIO_CELLS * 4U)
		return ENOENT;

	/* Refuses a pin past the last bank. */
	pin = (uint32_t)drv_fdt_cells_value(value, 1U, 1U);
	if (pin >= DRV_S905X5M_GPIO_COUNT)
		return EINVAL;

	/* Succeeded: the pin and its flags. */
	gpio->pin = pin;
	gpio->flags = (uint32_t)drv_fdt_cells_value(value, 2U, 1U);
	return 0;
}

/*
 * Reads a node's one-cell number property, or fallback when the node
 * lacks it.
 */
uint32_t
drv_s905x5m_node_number(
	uint32_t node,
	const char *name,
	uint32_t fallback)
{
	const uint8_t *value;
	uint32_t length;
	uint32_t number;
	int error;

	/* Reads the property; one that is missing or short gives the fallback. */
	error = drv_fdt_property(&s905x5m_soc.fdt, node, name, &value, &length);
	if (error != 0)
		return fallback;
	if (length < 4U)
		return fallback;

	/* Succeeded: the property's first cell. */
	number = (uint32_t)drv_fdt_cells_value(value, 0, 1U);
	return number;
}

/*
 * Reads one address and size pair of a node's reg property as written,
 * in the cell counts of its parent and without any translation.
 *
 * The USB PHY's node sits on a bus without ranges, whose addresses are the
 * CPU's; drv_fdt_reg() refuses such a bus.  Returns 0, or ENOENT when the
 * node has fewer pairs than index.
 */
int
drv_s905x5m_node_raw_reg(
	uint32_t node,
	unsigned index,
	uint64_t *address,
	uint64_t *size)
{
	const uint8_t *value;
	uint32_t parent;
	uint32_t length;
	uint32_t address_cells;
	uint32_t size_cells;
	uint32_t pair_cells;
	int error;

	/* Reads the cell counts the node's bus gives its children. */
	error = drv_fdt_parent(&s905x5m_soc.fdt, node, &parent);
	if (error != 0)
		return error;
	address_cells = drv_fdt_node_cells(&s905x5m_soc.fdt, parent, "#address-cells", 2U);
	size_cells = drv_fdt_node_cells(&s905x5m_soc.fdt, parent, "#size-cells", 1U);
	pair_cells = address_cells + size_cells;

	/* Refuses counts the cell reader cannot hold. */
	if (address_cells == 0 || address_cells > 2U)
		return EINVAL;
	if (size_cells > 2U)
		return EINVAL;

	/* Reads the property; the pair must be there in full. */
	error = drv_fdt_property(&s905x5m_soc.fdt, node, "reg", &value, &length);
	if (error != 0)
		return ENOENT;
	if (length < (index + 1U) * pair_cells * 4U)
		return ENOENT;

	/* Succeeded: the pair as the tree writes it. */
	*address = drv_fdt_cells_value(value, index * pair_cells, address_cells);
	*size = 0;
	if (size_cells != 0)
		*size = drv_fdt_cells_value(value, index * pair_cells + address_cells, size_cells);
	return 0;
}

/*
 * Spins for at least the given number of microseconds.
 *
 * The drivers start before the scheduler runs their threads, so the wait
 * spins on the monotonic counter; without one it ends at once.
 */
void
drv_s905x5m_delay_us(
	uint64_t microseconds)
{
	uint64_t deadline;
	int passed;
	int error;

	/* Sets the end of the pause. */
	error = start_deadline(microseconds, &deadline);
	if (error != 0)
		return;

	/* Spins until the counter passes the end of the pause. */
	for (;;) {
		passed = deadline_passed(deadline);
		if (passed)
			break;
	}
}

/* Maps one shared block uncached. */
static int
map_block(
	uint64_t address,
	volatile uint8_t **mapped)
{
	void *window;
	int error;

	/* Maps the block's registers. */
	error = kern_device_map(address, S905X5M_BLOCK_BYTES, KERN_DEVICE_UNCACHED, &window);
	if (error != 0) {
		kern_logf("s905x5m: block at %llx not mapped (%d)\n", (unsigned long long)address, error);
		return error;
	}

	/* Succeeded: the block's registers. */
	*mapped = window;
	return 0;
}

/* Finds the bank of a pin and the pin's index in it, or NULL for none. */
static const struct s905x5m_bank *
find_bank(
	unsigned pin,
	unsigned *index)
{
	unsigned bank;

	/* Walks the banks for the one whose numbers hold the pin. */
	for (bank = 0; bank < S905X5M_BANK_COUNT; bank++) {
		/* Skips a bank that starts after the pin. */
		if (pin < s905x5m_banks[bank].first)
			continue;

		/* Skips a bank that ends before the pin. */
		if (pin - s905x5m_banks[bank].first >= s905x5m_banks[bank].count)
			continue;

		/* Succeeded: the pin's bank and its place in it. */
		*index = pin - s905x5m_banks[bank].first;
		return &s905x5m_banks[bank];
	}

	/* No bank has the pin. */
	return NULL;
}

/* Changes the bits mask of one shared register to value, under the lock. */
static void
update_word(
	volatile uint8_t *block,
	uint32_t offset,
	uint32_t mask,
	uint32_t value)
{
	unsigned long enabled;
	uint32_t word;

	/* Reads the register, changes the caller's bits and writes it back. */
	enabled = spin_lock_irqsave(&s905x5m_soc.lock);

	word = kern_mmio_read32(block + offset);
	word &= ~mask;
	word |= value & mask;
	kern_mmio_write32(block + offset, word);

	spin_unlock_irqrestore(&s905x5m_soc.lock, enabled);
}

/* Sets or clears a pin's bit in one of its bank's GPIO words. */
static int
update_gpio_bit(
	unsigned pin,
	uint32_t word,
	int set)
{
	const struct s905x5m_bank *bank;
	unsigned index;
	uint32_t offset;
	uint32_t mask;

	/* Refuses a pin no bank has. */
	bank = find_bank(pin, &index);
	if (bank == NULL)
		return EINVAL;

	/* Finds the pin's bit; every bank fits one word. */
	offset = S905X5M_GPIO_OFFSET + (bank->gpio_word + word) * 4U;
	mask = 1U << index;

	/* Writes the bit. */
	if (set) {
		update_word(s905x5m_soc.pins, offset, mask, mask);
	} else {
		update_word(s905x5m_soc.pins, offset, mask, 0);
	}

	/* Succeeded: the bit holds the value. */
	return 0;
}

/* Computes the counter value a wait of the given length ends at. */
static int
start_deadline(
	uint64_t microseconds,
	uint64_t *deadline)
{
	uint64_t frequency;
	uint64_t now;
	bool counted;

	/* Reads the monotonic counter; without one no wait can end. */
	counted = kern_rtc_read_counter(&now, &frequency);
	if (!counted)
		return ENODEV;
	if (frequency == 0)
		return ENODEV;

	/* Succeeded: the counter value the wait ends at. */
	*deadline = now + frequency * microseconds / 1000000ULL;
	return 0;
}

/* Reports whether the monotonic counter has passed a deadline. */
static int
deadline_passed(
	uint64_t deadline)
{
	uint64_t frequency;
	uint64_t now;
	bool counted;

	/* A counter that cannot be read ends the wait rather than spinning forever. */
	counted = kern_rtc_read_counter(&now, &frequency);
	if (!counted)
		return 1;

	/* The deadline has passed once the counter reaches it. */
	if (now >= deadline)
		return 1;

	/* The wait goes on. */
	return 0;
}
