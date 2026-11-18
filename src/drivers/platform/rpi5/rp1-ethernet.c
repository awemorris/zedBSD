/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Raspberry Pi 5's Ethernet port: RP1's Cadence GEM, handed to the
 * Cadence GEM driver, and the board's BCM54213PE PHY on the GEM's MDIO bus.
 *
 * The glue finds the controller and its PHY in RP1's device tree, makes
 * sure the controller's clocks run, maps the registers, takes the board's
 * address from the tree (or lets the driver take the one the firmware left
 * in the controller's filters), and gives the driver RP1's DMA window, the
 * PHY's reset line and register setup, and RP1's interrupt path.  RP1's
 * DMA addresses are above 4 GiB, so the descriptors are 64-bit.
 */

#include <drivers/platform/rpi5/rp1.h>
#include <drivers/platform/rpi5/rp1-ethernet.h>
#include <drivers/platform/rpi5/rp1-gpio.h>
#include <drivers/platform/rpi5/rp1-irq.h>
#include <drivers/ethernet/bcm54213pe.h>
#include <drivers/ethernet/cadence-gem.h>
#include <drivers/ethernet/mii-phy.h>
#include <drivers/generic/dma.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <uapi/errno.h>

/*
 * RP1's clock controller: the Ethernet and timestamp clocks' control
 * registers and their enable bit, the system clock's number in the
 * clocks' binding, and the system clock's rate when the tree gives none.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y drivers/clk/clk-rp1.c
 * (CLK_ETH_CTRL, CLK_ETH_TSU_CTRL, CLK_CTRL_ENABLE) and
 * include/dt-bindings/clock/rp1.h (RP1_CLK_SYS); the rate is the tree's
 * assigned-clock-rates entry for it.
 */
#define RP1_CLK_ETH_CTRL		0x00064U
#define RP1_CLK_ETH_TSU_CTRL		0x00134U
#define RP1_CLK_CTRL_ENABLE		(1U << 11)
#define RP1_CLK_SYS			12U
#define RP1_CLK_SYS_DEFAULT_HZ		200000000U

/* Marks a parameter that the driver's hook interface passes but this board does not need. */
#define UNUSED_PARAMETER(parameter)	((void)(parameter))

/* The largest DMA allocation the driver makes: one ring's 256 slots of 2 KiB. */
#define RP1_GEM_DMA_SEGMENT_BYTES	(256U * 2048U)

/* How long the PHY settles after its reset, and the longest reset pulse the tree may ask for. */
#define RP1_GEM_PHY_RESET_SETTLE_MS	1U
#define RP1_GEM_PHY_RESET_MAX_MS	1000U

/*
 * The Pi 5's one Ethernet port as the glue knows it.
 *
 * It is filled once at boot by drv_rp1_ethernet_init() and kept for the
 * kernel's life: registers is set once the controller is mapped, which
 * refuses a second attachment; bus is the driver's MDIO bus, set when the
 * driver asks for the PHY's setup; gem is the attached controller, NULL
 * until the attachment has succeeded.
 */
struct rp1_ethernet {
	uint32_t node;
	volatile uint8_t *registers;
	unsigned phy_address;
	struct drv_bcm54213pe phy;
	const struct drv_mii_bus *bus;
	struct drv_cadence_gem *gem;
};

/* The Pi 5's one Ethernet port; attachment keeps it until the kernel stops. */
static struct rp1_ethernet board_ethernet;

static int describe_controller(struct rp1_ethernet *ethernet);
static int describe_phy(struct rp1_ethernet *ethernet);
static void read_tree_mac(struct rp1_ethernet *ethernet, uint8_t mac[6]);
static void enable_clocks(void);
static unsigned clock_rate(unsigned clock);
static int create_dma(struct drv_dma_device **dma);
static int reset_phy(void *context);
static int setup_phy(void *context, const struct drv_mii_bus *bus, unsigned phy, uint32_t *identity);
static int read_link(void *context, const struct drv_mii_bus *bus, unsigned phy, struct drv_mii_link *link);
static int establish_irq(void *context, void (*handler)(void *argument), void *argument);
static int phy_read(void *context, unsigned reg, uint16_t *data);
static int phy_write(void *context, unsigned reg, uint16_t data);
static int delay_milliseconds(unsigned milliseconds);

/*
 * Starts the controller and the PHY and publishes the interface.
 */
int
drv_rp1_ethernet_init(
	void)
{
	struct rp1_ethernet *ethernet;
	struct drv_cadence_gem_config config;
	const char *stage;
	uint64_t register_size;
	unsigned rate;
	int error;

	/* Refuses a second attachment. */
	ethernet = &board_ethernet;
	if (ethernet->registers != NULL)
		return EALREADY;

	/* Finds the controller and its PHY in the tree. */
	stage = "device tree";
	error = describe_controller(ethernet);
	if (error != 0)
		goto fail;

	/* Makes sure the controller's clocks run. */
	enable_clocks();

	/* Maps the registers. */
	stage = "registers";
	error = drv_rp1_map(ethernet->node, 0, &ethernet->registers, &register_size);
	if (error != 0)
		goto fail;

	/* Takes the board's address from the tree, when the firmware wrote one. */
	kern_memset(&config, 0, sizeof(config));
	read_tree_mac(ethernet, config.mac);

	/* Creates the DMA device behind RP1's window onto memory. */
	stage = "DMA buffers";
	error = create_dma(&config.dma);
	if (error != 0)
		goto fail;

	/* Finds the system clock the MDIO clock divides. */
	rate = clock_rate(RP1_CLK_SYS);
	if (rate == 0)
		rate = RP1_CLK_SYS_DEFAULT_HZ;

	/*
	 * Describes the controller: en0, 64-bit descriptors, the PHY at the
	 * tree's address, and the address in the filters when the tree has
	 * none.
	 */
	config.name = "en0";
	config.registers = ethernet->registers;
	config.descriptor_width = DRV_CADENCE_GEM_DESCRIPTOR_64;
	config.pclk_hz = rate;
	config.phy_address = ethernet->phy_address;
	config.phy_scan = 0;
	config.flags = DRV_CADENCE_GEM_FLAG_MAC_FROM_FILTERS;

	/* Gives the driver the board's steps: the PHY's reset and setup, its link, the interrupt. */
	config.context = ethernet;
	config.reset_phy = reset_phy;
	config.setup_phy = setup_phy;
	config.read_link = read_link;
	config.set_tx_clock = NULL;
	config.establish_irq = establish_irq;

	/* Has the driver reset the controller, start the PHY and publish the interface. */
	error = drv_cadence_gem_attach(&config, &ethernet->gem);
	if (error != 0)
		return error;

	/* Succeeded: networkd can open the interface. */
	return 0;

fail:
	/* Reports where the attachment stopped; what was taken is kept, as nothing reaches it. */
	kern_logf("gem: initialization failed at %s (%d)\n", stage, error);
	return error;
}

/*
 * Starts the periodic service of the link and of missed interrupts.
 */
void
drv_rp1_ethernet_refresh(
	void)
{
	/* Leaves a port that did not attach alone. */
	if (board_ethernet.gem == NULL)
		return;

	/* Starts the service of the attached interface. */
	drv_cadence_gem_start_service(board_ethernet.gem);
}

/*
 * Finds the GEM among RP1's peripherals and its PHY.
 */
static int
describe_controller(
	struct rp1_ethernet *ethernet)
{
	const struct drv_fdt *fdt;
	uint32_t node;
	bool enabled;
	int error;

	/* Finds the controller. */
	error = drv_rp1_find_child("cdns,macb", DRV_FDT_NO_NODE, &node);
	if (error != 0) {
		kern_logf("gem: no Ethernet controller in the device tree\n");
		return ENODEV;
	}

	/* Leaves a controller the tree turned off alone. */
	fdt = drv_rp1_fdt();
	enabled = drv_fdt_node_enabled(fdt, node);
	if (!enabled)
		return ENODEV;

	/* Keeps the controller's node. */
	ethernet->node = node;

	/* Finds the PHY and its delays. */
	error = describe_phy(ethernet);
	if (error != 0)
		return error;

	/* Succeeded: the controller and its PHY are known. */
	return 0;
}

/*
 * Reads the PHY's address and the RGMII delays the PHY must insert.
 */
static int
describe_phy(
	struct rp1_ethernet *ethernet)
{
	const struct drv_fdt *fdt;
	const uint8_t *property;
	uint32_t length;
	uint32_t phandle;
	uint32_t phy;
	int comparison;
	int error;

	/* Reads the interface-mode string. */
	fdt = drv_rp1_fdt();
	error = drv_fdt_property(fdt, ethernet->node, "phy-mode", &property, &length);
	if (error != 0)
		return error;

	/* Requires the string to be terminated. */
	if (length == 0 || property[length - 1U] != 0)
		return EINVAL;

	/* Refuses another mode: the Pi 5 has the PHY insert both delays. */
	comparison = kern_strcmp((const char *)property, "rgmii-id");
	if (comparison != 0) {
		kern_logf("gem: PHY mode %s not supported\n", (const char *)property);
		return ENOTSUP;
	}

	/* The PHY inserts both delays. */
	ethernet->phy.rx_delay = 1;
	ethernet->phy.tx_delay = 1;

	/* Reads the phy-handle, which is one cell. */
	error = drv_fdt_property(fdt, ethernet->node, "phy-handle", &property, &length);
	if (error != 0)
		return error;
	if (length != 4U)
		return EINVAL;

	/* Follows the phy-handle to the PHY node. */
	phandle = (uint32_t)drv_fdt_cells_value(property, 0, 1U);
	error = drv_fdt_find_phandle(fdt, phandle, &phy);
	if (error != 0)
		return error;

	/* Reads the PHY's clause-22 address, which is one cell. */
	error = drv_fdt_property(fdt, phy, "reg", &property, &length);
	if (error != 0)
		return error;
	if (length != 4U)
		return EINVAL;

	/* Keeps the address, which MDIO's five bits can name. */
	ethernet->phy_address = (unsigned)drv_fdt_cells_value(property, 0, 1U);
	if (ethernet->phy_address > 31U)
		return EINVAL;

	/* Succeeded: MDIO can reach the PHY. */
	return 0;
}

/*
 * Copies the board's Ethernet address the firmware wrote into the tree;
 * without one the address stays all zeros and the driver looks in the
 * controller's filters.
 */
static void
read_tree_mac(
	struct rp1_ethernet *ethernet,
	uint8_t mac[6])
{
	const struct drv_fdt *fdt;
	const uint8_t *property;
	uint32_t length;
	int error;

	/* Reads the tree's address. */
	fdt = drv_rp1_fdt();
	error = drv_fdt_property(fdt, ethernet->node, "local-mac-address", &property, &length);
	if (error == 0 && length == 6U)
		kern_memcpy(mac, property, 6U);
}

/*
 * Turns RP1's Ethernet and timestamp clocks on when the firmware left them
 * off, without changing their sources or dividers.
 */
static void
enable_clocks(
	void)
{
	volatile uint8_t *clocks;
	uint64_t size;
	uint32_t node;
	uint32_t control;
	int error;

	/* Finds RP1's clock controller; without one the clocks stay as they are. */
	error = drv_rp1_find_child("raspberrypi,rp1-clocks", DRV_FDT_NO_NODE, &node);
	if (error != 0)
		return;

	/* Maps the clock controller's registers. */
	error = drv_rp1_map(node, 0, &clocks, &size);
	if (error != 0)
		return;

	/* Refuses a window too small to hold the two control registers. */
	if (size < RP1_CLK_ETH_TSU_CTRL + 4U)
		return;

	/* Turns the Ethernet clock on. */
	control = kern_mmio_read32(clocks + RP1_CLK_ETH_CTRL);
	if ((control & RP1_CLK_CTRL_ENABLE) == 0) {
		kern_logf("gem: Ethernet clock was off (%08x)\n", control);
		kern_mmio_write32(clocks + RP1_CLK_ETH_CTRL, control | RP1_CLK_CTRL_ENABLE);
	}

	/* Turns the timestamp clock on. */
	control = kern_mmio_read32(clocks + RP1_CLK_ETH_TSU_CTRL);
	if ((control & RP1_CLK_CTRL_ENABLE) == 0) {
		kern_logf("gem: timestamp clock was off (%08x)\n", control);
		kern_mmio_write32(clocks + RP1_CLK_ETH_TSU_CTRL, control | RP1_CLK_CTRL_ENABLE);
	}
}

/*
 * Reads a clock's rate from the clock controller's assigned-clock-rates;
 * zero when the tree does not assign one.
 */
static unsigned
clock_rate(
	unsigned clock)
{
	const struct drv_fdt *fdt;
	const uint8_t *clocks;
	const uint8_t *rates;
	uint64_t assigned;
	uint32_t clocks_length;
	uint32_t rates_length;
	uint32_t node;
	unsigned entry;
	unsigned rate;
	int found;
	int error;

	/* Finds the clock controller. */
	fdt = drv_rp1_fdt();
	error = drv_rp1_find_child("raspberrypi,rp1-clocks", DRV_FDT_NO_NODE, &node);
	if (error != 0)
		return 0;

	/* Reads the clocks the tree assigns. */
	error = drv_fdt_property(fdt, node, "assigned-clocks", &clocks, &clocks_length);
	if (error != 0)
		return 0;

	/* Reads the rates assigned to them. */
	error = drv_fdt_property(fdt, node, "assigned-clock-rates", &rates, &rates_length);
	if (error != 0)
		return 0;

	/* Each assignment is a phandle and a clock number, with one rate. */
	found = 0;
	for (entry = 0; (entry + 1U) * 8U <= clocks_length && (entry + 1U) * 4U <= rates_length; entry++) {
		/* Stops at the clock's assignment. */
		assigned = drv_fdt_cells_value(clocks, entry * 2U + 1U, 1U);
		if (assigned == clock) {
			found = 1;
			break;
		}
	}

	/* No assignment for the clock. */
	if (!found)
		return 0;

	/* Succeeded: the clock's assigned rate. */
	rate = (unsigned)drv_fdt_cells_value(rates, entry, 1U);
	return rate;
}

/*
 * Creates the controller's DMA device: 64-bit addresses through RP1's
 * window onto memory, coherent when RP1 says so.
 */
static int
create_dma(
	struct drv_dma_device **dma)
{
	struct drv_dma_constraints constraints;
	uint64_t bus_offset;
	uint64_t physical_limit;
	int coherent;
	int error;

	/* Reads the addresses RP1's peripherals use for memory. */
	error = drv_rp1_dma_window(&bus_offset, &physical_limit, &coherent);
	if (error != 0)
		return error;

	/* Describes the controller's DMA. */
	kern_memset(&constraints, 0, sizeof(constraints));
	constraints.address_bits = 64U;
	constraints.max_segment_size = RP1_GEM_DMA_SEGMENT_BYTES;
	constraints.segment_boundary = 0;
	constraints.coherent = coherent;

	/* Creates the device behind the window. */
	error = drv_dma_device_create_window(&constraints, bus_offset, physical_limit, dma);
	if (error != 0)
		return error;

	/* Succeeded: the driver allocates its rings and slots from it. */
	return 0;
}

/*
 * Pulses the PHY's reset line, when the tree names one, for its duration.
 *
 * Procedure referenced: OpenBSD sys/dev/fdt/if_cad.c rev 1.17
 * (cad_attach(): assert the reset for the duration plus a millisecond,
 * release it, wait a millisecond).
 */
static int
reset_phy(
	void *context)
{
	struct rp1_ethernet *ethernet;
	const struct drv_fdt *fdt;
	const uint8_t *property;
	uint32_t length;
	unsigned pin;
	unsigned active_low;
	unsigned duration;
	int error;

	/* A PHY without a reset line needs no pulse. */
	ethernet = context;
	fdt = drv_rp1_fdt();
	error = drv_fdt_property(fdt, ethernet->node, "phy-reset-gpios", &property, &length);
	if (error != 0)
		return 0;

	/* Refuses a reset line that is not a controller, a pin and its flags. */
	if (length != 12U)
		return EINVAL;

	/* Reads the pin and which level asserts the reset. */
	pin = (unsigned)drv_fdt_cells_value(property, 1U, 1U);
	active_low = (unsigned)drv_fdt_cells_value(property, 2U, 1U) & 1U;

	/* Reads the pulse's length, a millisecond by default. */
	duration = drv_fdt_node_cells(fdt, ethernet->node, "phy-reset-duration", 1U);
	if (duration > RP1_GEM_PHY_RESET_MAX_MS)
		duration = 1U;

	/* Brings up the GPIO controller the reset line is on. */
	error = drv_rp1_gpio_init();
	if (error != 0)
		return error;

	/* Asserts the reset line. */
	error = drv_rp1_gpio_set_output(pin, active_low == 0);
	if (error != 0)
		return error;

	/* Holds the reset for the pulse's length and a millisecond more. */
	error = delay_milliseconds(duration + 1U);
	if (error != 0)
		return error;

	/* Releases the reset line. */
	error = drv_rp1_gpio_set_output(pin, active_low != 0);
	if (error != 0)
		return error;

	/* Lets the PHY come out of reset. */
	error = delay_milliseconds(RP1_GEM_PHY_RESET_SETTLE_MS);
	if (error != 0)
		return error;

	/* Succeeded: the PHY answers on MDIO. */
	return 0;
}

/*
 * Starts the BCM54213PE over the driver's MDIO bus, with both RGMII delays,
 * and reports the identity it read.
 */
static int
setup_phy(
	void *context,
	const struct drv_mii_bus *bus,
	unsigned phy,
	uint32_t *identity)
{
	struct rp1_ethernet *ethernet;
	int error;

	UNUSED_PARAMETER(phy);

	/* Points the PHY driver at the controller's bus, at the tree's address. */
	ethernet = context;
	ethernet->bus = bus;
	ethernet->phy.context = ethernet;
	ethernet->phy.read = phy_read;
	ethernet->phy.write = phy_write;

	/* Resets the PHY, sets its delays and starts autonegotiation. */
	error = drv_bcm54213pe_initialize(&ethernet->phy, identity);
	if (error != 0)
		return error;

	/* Succeeded: the PHY negotiates with its link partner. */
	return 0;
}

/*
 * Reads the BCM54213PE's negotiated speed; it only advertises full duplex.
 */
static int
read_link(
	void *context,
	const struct drv_mii_bus *bus,
	unsigned phy,
	struct drv_mii_link *link)
{
	struct rp1_ethernet *ethernet;
	unsigned speed;
	int error;

	UNUSED_PARAMETER(bus);
	UNUSED_PARAMETER(phy);

	/* Reads the speed through the PHY driver. */
	ethernet = context;
	error = drv_bcm54213pe_read_link(&ethernet->phy, &speed);
	if (error != 0)
		return error;

	/* Succeeded: the speed, at full duplex. */
	link->speed_mbps = speed;
	link->full_duplex = 1;
	return 0;
}

/*
 * Routes the controller's interrupt through RP1's interrupt controller.
 */
static int
establish_irq(
	void *context,
	void (*handler)(void *argument),
	void *argument)
{
	struct rp1_ethernet *ethernet;
	unsigned vector;
	unsigned trigger;
	int error;

	/* Finds RP1's interrupt path. */
	ethernet = context;
	error = drv_rp1_irq_init();
	if (error != 0)
		return error;

	/* Reads the controller's vector. */
	error = drv_rp1_irq_of(ethernet->node, 0, &vector, &trigger);
	if (error != 0)
		return error;

	/* Routes it to the driver. */
	error = drv_rp1_irq_establish(vector, trigger, handler, argument);
	if (error != 0)
		return error;

	/* Succeeded: the controller's interrupt reaches the driver. */
	return 0;
}

/* Reads one PHY register over the driver's MDIO bus. */
static int
phy_read(
	void *context,
	unsigned reg,
	uint16_t *data)
{
	struct rp1_ethernet *ethernet;
	int error;

	/* Runs the read at the PHY's address. */
	ethernet = context;
	error = ethernet->bus->read(ethernet->bus->context, ethernet->phy_address, reg, data);
	if (error != 0)
		return error;

	/* Succeeded: the register's value. */
	return 0;
}

/* Writes one PHY register over the driver's MDIO bus. */
static int
phy_write(
	void *context,
	unsigned reg,
	uint16_t data)
{
	struct rp1_ethernet *ethernet;
	int error;

	/* Runs the write at the PHY's address. */
	ethernet = context;
	error = ethernet->bus->write(ethernet->bus->context, ethernet->phy_address, reg, data);
	if (error != 0)
		return error;

	/* Succeeded: the PHY holds the value. */
	return 0;
}

/* Waits a number of milliseconds in thread context. */
static int
delay_milliseconds(
	unsigned milliseconds)
{
	unsigned step;

	/* Waits a millisecond at a time. */
	for (step = 0; step < milliseconds; step++)
		kern_usleep_range(1000U, 1000U);

	/* Succeeded: the time has passed. */
	return 0;
}
