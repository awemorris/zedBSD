/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The JH7110's glue for the VisionFive 2's two Ethernet ports.
 *
 * Each port is a DesignWare Ethernet QoS controller (GMAC0 for the first,
 * GMAC1 for the second) wired to an RGMII PHY on its own MDIO bus.  Its
 * node in the device tree names the controller's clocks (register and bus
 * interfaces, time stamps, the transmit clock and its source), its two
 * resets, the field of a system control block (AON for the first, SYS for
 * the second) that selects the PHY interface, and the PHY with the delays
 * and drive strengths the board needs.  The glue switches the clocks on,
 * selects RGMII while the controller is held in reset, releases it, and
 * describes everything to the DesignWare driver.
 *
 * A board whose transmit clock comes back from the RGMII receive side
 * (starfive,tx-use-rgmii-clk) needs nothing at a speed change.  On any
 * other board the glue sets the controller's transmit clock source to the
 * new speed's rate; the source reaches 125 MHz only, so such a board runs
 * at gigabit alone.
 */

#include <drivers/platform/visionfive2/vf2-ethernet.h>

#include <drivers/ethernet/dw-eqos.h>
#include <drivers/platform/visionfive2/vf2-crg.h>
#include <drivers/platform/visionfive2/vf2-pinctrl.h>
#include <drivers/platform/visionfive2/vf2-syscon.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

#include <stdbool.h>

/*
 * The bytes of one Ethernet controller's register block.
 *
 * Value referenced: Linux v6.12 arch/riscv/boot/dts/starfive/jh7110.dtsi
 * (gmac0 and gmac1, reg).
 */
#define VF2_ETHERNET_REGISTER_BYTES	0x10000U

/*
 * The values of the PHY interface field in the system control block.
 *
 * Values referenced: OpenBSD sys/dev/fdt/if_dwqe_fdt.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (JH7110_PHY_INTF_RGMII,
 * the three-bit field written by dwqe_setup_jh7110).
 */
#define VF2_ETHERNET_INTERFACE_RGMII	1U
#define VF2_ETHERNET_INTERFACE_MASK	7U

/*
 * The PHY's RGMII delay when the board names none, in picoseconds.
 *
 * Value referenced: Linux v6.12 drivers/net/phy/motorcomm.c
 * (ytphy_rgmii_clk_delay_config, the default YT8521_RC1R_RGMII_1_950_NS).
 */
#define VF2_ETHERNET_DEFAULT_DELAY_PS	1950U

/* How long the controller stays in reset and settles after it, in microseconds. */
#define VF2_ETHERNET_RESET_PAUSE_US	5000U

/*
 * One port of the board: where its controller's registers are, the
 * interface it is published as, and the SYS clock generator's transmit
 * clock source whose rate follows the link on a board without
 * starfive,tx-use-rgmii-clk.
 */
struct vf2_ethernet_port {
	uint64_t registers;
	const char *name;
	uint32_t tx_source;
};

/*
 * The board's ports, in drv_vf2_ethernet_attach() order.
 *
 * Values referenced: Linux v6.12 arch/riscv/boot/dts/starfive/jh7110.dtsi
 * (gmac0 and gmac1, reg); Linux v6.12
 * include/dt-bindings/clock/starfive,jh7110-crg.h
 * (JH7110_SYSCLK_GMAC0_GTXCLK 108, JH7110_SYSCLK_GMAC1_GTXCLK 100).
 */
static const struct vf2_ethernet_port vf2_ethernet_ports[DRV_VF2_ETHERNET_PORTS] = {
	{ 0x16030000ULL, "en0", 108U },
	{ 0x16040000ULL, "en1", 100U }
};

/* One RGMII variant of phy-mode and the directions in which the PHY adds the delay. */
struct vf2_ethernet_mode {
	const char *name;
	int rx_delay;
	int tx_delay;
};

/*
 * The RGMII variants of phy-mode.
 *
 * Values referenced: OpenBSD sys/dev/fdt/if_dwqe_fdt.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (dwqe_fdt_attach, the
 * phy-mode strings and the delays each one asks the PHY for).
 */
static const struct vf2_ethernet_mode vf2_ethernet_modes[] = {
	{ "rgmii", 0, 0 },
	{ "rgmii-id", 1, 1 },
	{ "rgmii-rxid", 1, 0 },
	{ "rgmii-txid", 0, 1 }
};

/*
 * Each port's transmit clock source, the context of set_tx_rate().
 *
 * Filled by drv_vf2_ethernet_attach() before the port's controller is
 * handed over and kept for the life of the system; the board attaches the
 * ports one after the other on the boot CPU, so nothing else writes them.
 */
static struct drv_vf2_clock vf2_ethernet_tx_sources[DRV_VF2_ETHERNET_PORTS];

static int find_port(const struct drv_fdt *fdt, uint64_t registers, uint32_t *node, uint64_t *address, uint64_t *size);
static int switch_port_on(const struct drv_fdt *fdt, uint32_t node, uint64_t *csr_rate);
static int select_interface(const struct drv_fdt *fdt, uint32_t node);
static int describe_phy(const struct drv_fdt *fdt, uint32_t node, struct drv_dw_eqos_config *config);
static void describe_dma(const struct drv_fdt *fdt, uint32_t node, struct drv_dw_eqos_config *config);
static int describe_interrupt(const struct drv_fdt *fdt, uint32_t node, unsigned *irq);
static void describe_mac(const struct drv_fdt *fdt, uint32_t node, struct drv_dw_eqos_config *config);
static int set_tx_rate(void *context, unsigned speed_mbps);
static int read_mode(const struct drv_fdt *fdt, uint32_t node, int *rx_delay, int *tx_delay);
static uint32_t read_number(const struct drv_fdt *fdt, uint32_t node, const char *name, uint32_t fallback);
static int has_property(const struct drv_fdt *fdt, uint32_t node, const char *name);
static void pause_microseconds(uint64_t microseconds);

/*
 * Brings up one Ethernet port and publishes it as its interface.
 */
int
drv_vf2_ethernet_attach(
	const struct drv_fdt *fdt,
	unsigned port,
	int dma_default_coherent)
{
	struct drv_dw_eqos_config config;
	const struct vf2_ethernet_port *description;
	uint64_t address;
	uint64_t size;
	uint64_t csr_rate;
	uint32_t node;
	void *mapped;
	int rgmii_clock;
	int error;

	/* Refuses a port the board does not have. */
	if (port >= DRV_VF2_ETHERNET_PORTS)
		return EINVAL;
	description = &vf2_ethernet_ports[port];

	/* Finds the port's controller. */
	error = find_port(fdt, description->registers, &node, &address, &size);
	if (error != 0)
		return error;

	/* Starts the controller's description empty. */
	kern_memset(&config, 0, sizeof(config));

	/* Reads the PHY's address and settings before touching the hardware. */
	error = describe_phy(fdt, node, &config);
	if (error != 0)
		return error;

	/* Finds the controller's interrupt. */
	error = describe_interrupt(fdt, node, &config.irq);
	if (error != 0)
		return error;

	/* Maps the controller's registers as device memory. */
	error = kern_device_map(address, VF2_ETHERNET_REGISTER_BYTES, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0)
		return error;

	/* Switches the port's pins and clocks on and releases it from reset in RGMII mode. */
	error = switch_port_on(fdt, node, &csr_rate);
	if (error != 0)
		return error;

	/* Describes the controller to the DesignWare driver. */
	config.name = description->name;
	config.registers = mapped;
	config.csr_clock_hz = csr_rate;
	config.dma_coherent = drv_fdt_dma_coherent(fdt, node, dma_default_coherent);
	describe_dma(fdt, node, &config);
	describe_mac(fdt, node, &config);

	/* Lets the driver move the transmit clock unless the board takes it from the PHY. */
	config.set_speed = NULL;
	config.context = NULL;
	rgmii_clock = has_property(fdt, node, "starfive,tx-use-rgmii-clk");
	if (!rgmii_clock) {
		vf2_ethernet_tx_sources[port].crg = DRV_VF2_CRG_SYS;
		vf2_ethernet_tx_sources[port].index = description->tx_source;
		config.set_speed = set_tx_rate;
		config.context = &vf2_ethernet_tx_sources[port];
	}

	/* Resets the controller, brings the PHY up and publishes the interface. */
	error = drv_dw_eqos_attach(&config);
	if (error != 0)
		return error;

	/* Succeeded: the port is its interface. */
	return 0;
}

/* Finds the enabled controller of the JH7110 whose registers are at the given address. */
static int
find_port(
	const struct drv_fdt *fdt,
	uint64_t registers,
	uint32_t *node,
	uint64_t *address,
	uint64_t *size)
{
	uint32_t candidate;
	bool enabled;
	int error;

	/* Walks the JH7110's Ethernet controllers. */
	candidate = DRV_FDT_NO_NODE;
	for (;;) {
		/* Moves to the next controller; none left means the tree has no port. */
		error = drv_fdt_find_compatible(fdt, "starfive,jh7110-dwmac", candidate, &candidate);
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

		/* Stops at the port's controller. */
		if (*address == registers)
			break;
	}

	/* Refuses a register block smaller than the controller's. */
	if (*size < VF2_ETHERNET_REGISTER_BYTES)
		return EINVAL;

	/* Succeeded: the port's node and registers. */
	*node = candidate;
	return 0;
}

/*
 * Applies the port's pins and assigned clocks, switches its clocks on,
 * selects RGMII while it is held in reset, releases it, and reports the
 * register clock's rate (zero when it cannot be computed).
 *
 * Procedure referenced: OpenBSD sys/dev/fdt/if_dwqe_fdt.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (dwqe_fdt_attach,
 * dwqe_setup_jh7110).
 */
static int
switch_port_on(
	const struct drv_fdt *fdt,
	uint32_t node,
	uint64_t *csr_rate)
{
	static const char *const clock_names[] = {
		"stmmaceth",
		"pclk",
		"ptp_ref",
		"tx",
		"gtx"
	};
	struct drv_vf2_clock clock;
	struct drv_vf2_reset bus_reset;
	struct drv_vf2_reset register_reset;
	unsigned changed;
	unsigned index;
	int error;

	/* Applies the port's pin group; a node without one keeps the firmware's pins. */
	error = drv_vf2_pinctrl_apply_default(fdt, node, &changed);
	if (error != 0 && error != ENOENT)
		return error;

	/* Moves the transmit clock to the source the board assigns. */
	error = drv_vf2_clock_apply_assigned_parents(fdt, node);
	if (error != 0 && error != ENOENT)
		return error;

	/* Sets the rates the board assigns. */
	error = drv_vf2_clock_apply_assigned(fdt, node);
	if (error != 0 && error != ENOENT)
		return error;

	/* Switches every clock of the controller on. */
	for (index = 0; index < sizeof(clock_names) / sizeof(clock_names[0]); index++) {
		/* Finds the clock. */
		error = drv_vf2_clock_lookup(fdt, node, clock_names[index], &clock);
		if (error != 0)
			return error;

		/* Opens its gates. */
		error = drv_vf2_clock_enable(&clock);
		if (error != 0)
			return error;
	}

	/* Finds the bus interface's reset. */
	error = drv_vf2_reset_lookup(fdt, node, "stmmaceth", &bus_reset);
	if (error != 0)
		return error;

	/* Finds the register interface's reset. */
	error = drv_vf2_reset_lookup(fdt, node, "ahb", &register_reset);
	if (error != 0)
		return error;

	/* Holds the bus interface in reset, whatever the firmware left it in. */
	error = drv_vf2_reset_assert(&bus_reset);
	if (error != 0)
		return error;

	/* Holds the register interface in reset too. */
	error = drv_vf2_reset_assert(&register_reset);
	if (error != 0)
		return error;

	/* Selects the PHY interface while the controller cannot run. */
	error = select_interface(fdt, node);
	if (error != 0)
		return error;

	/* Releases the register interface from reset after the reset has lasted long enough. */
	pause_microseconds(VF2_ETHERNET_RESET_PAUSE_US);
	error = drv_vf2_reset_deassert(&register_reset);
	if (error != 0)
		return error;

	/* Releases the bus interface. */
	error = drv_vf2_reset_deassert(&bus_reset);
	if (error != 0)
		return error;

	/* Lets it settle before its registers are used. */
	pause_microseconds(VF2_ETHERNET_RESET_PAUSE_US);

	/*
	 * Reads the bus clock's rate, which the MDIO clock is divided from; the
	 * first clock is it.  A rate that cannot be computed is zero.
	 */
	*csr_rate = 0;
	error = drv_vf2_clock_lookup(fdt, node, clock_names[0], &clock);
	if (error == 0) {
		/* Computes the rate of the clock found. */
		error = drv_vf2_clock_get_rate(&clock, csr_rate);
		if (error != 0)
			*csr_rate = 0;
	}

	/* Writes the rate to the boot log, zero when it could not be computed. */
	kern_logf("vf2-ethernet: bus clock %llu Hz (%d)\n", (unsigned long long)*csr_rate, error);

	/* Succeeded: the controller runs in RGMII mode. */
	return 0;
}

/*
 * Writes the PHY interface field the node's starfive,syscon names, for
 * RGMII in any of its delay variants.
 *
 * The property's two arguments are the field's offset and shift.
 *
 * Values referenced: Linux v6.12 arch/riscv/boot/dts/starfive/jh7110.dtsi
 * (gmac0, starfive,syscon = <&aon_syscon 0xc 0x12>; gmac1,
 * starfive,syscon = <&sys_syscon 0x90 0x2>).
 * Procedure referenced: OpenBSD sys/dev/fdt/if_dwqe_fdt.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (dwqe_setup_jh7110).
 */
static int
select_interface(
	const struct drv_fdt *fdt,
	uint32_t node)
{
	enum drv_vf2_syscon syscon;
	uint32_t arguments[2];
	unsigned count;
	int rx_delay;
	int tx_delay;
	int error;

	/* Refuses an interface other than RGMII, which the glue does not drive. */
	error = read_mode(fdt, node, &rx_delay, &tx_delay);
	if (error != 0)
		return error;

	/* Finds the field. */
	error = drv_vf2_syscon_lookup(fdt, node, "starfive,syscon", &syscon, arguments, 2U, &count);
	if (error != 0)
		return error;

	/* Refuses a field without its offset and shift, or one that does not fit the register. */
	if (count < 2U || arguments[1] > 29U)
		return EINVAL;

	/* Writes RGMII into the three-bit field. */
	error = drv_vf2_syscon_update(syscon,
				      arguments[0],
				      VF2_ETHERNET_INTERFACE_MASK << arguments[1],
				      VF2_ETHERNET_INTERFACE_RGMII << arguments[1]);
	if (error != 0)
		return error;

	/* Succeeded: the controller talks RGMII. */
	return 0;
}

/*
 * Finds the PHY the node's phy-handle names: its address on the MDIO bus,
 * the delays the interface mode asks it for, and its Motorcomm settings.
 */
static int
describe_phy(
	const struct drv_fdt *fdt,
	uint32_t node,
	struct drv_dw_eqos_config *config)
{
	struct drv_yt8531_config *phy;
	const uint8_t *value;
	uint32_t length;
	uint32_t phandle;
	uint32_t phy_node;
	int error;

	/* Reads which directions the PHY delays. */
	phy = &config->yt8531;
	error = read_mode(fdt, node, &phy->rx_delay, &phy->tx_delay);
	if (error != 0)
		return error;

	/* Reads the phandle of the PHY's node. */
	error = drv_fdt_property(fdt, node, "phy-handle", &value, &length);
	if (error != 0)
		return error;
	if (length != 4U)
		return EINVAL;

	/* Resolves the PHY's node. */
	phandle = (uint32_t)drv_fdt_cells_value(value, 0, 1U);
	error = drv_fdt_find_phandle(fdt, phandle, &phy_node);
	if (error != 0)
		return error;

	/* Reads the PHY's address on the bus. */
	error = drv_fdt_property(fdt, phy_node, "reg", &value, &length);
	if (error != 0)
		return error;
	if (length != 4U)
		return EINVAL;

	/* Refuses an address beyond the bus's 32. */
	config->phy_address = (unsigned)drv_fdt_cells_value(value, 0, 1U);
	if (config->phy_address > 31U)
		return EINVAL;

	/* Reads the delays and the receive pins' drive strengths. */
	phy->rx_delay_ps = read_number(fdt, phy_node, "rx-internal-delay-ps", VF2_ETHERNET_DEFAULT_DELAY_PS);
	phy->tx_delay_ps = read_number(fdt, phy_node, "tx-internal-delay-ps", VF2_ETHERNET_DEFAULT_DELAY_PS);
	phy->rx_clock_drive_ua = read_number(fdt, phy_node, "motorcomm,rx-clk-drv-microamp", 0);
	phy->rx_data_drive_ua = read_number(fdt, phy_node, "motorcomm,rx-data-drv-microamp", 0);

	/* Reads whether the transmit clock is inverted at each speed. */
	phy->tx_clock_adjust = has_property(fdt, phy_node, "motorcomm,tx-clk-adj-enabled");
	phy->tx_clock_inverted_10 = has_property(fdt, phy_node, "motorcomm,tx-clk-10-inverted");
	phy->tx_clock_inverted_100 = has_property(fdt, phy_node, "motorcomm,tx-clk-100-inverted");
	phy->tx_clock_inverted_1000 = has_property(fdt, phy_node, "motorcomm,tx-clk-1000-inverted");

	/* Succeeded: the PHY is described. */
	return 0;
}

/*
 * Reads the DMA's FIFO sizes, burst lengths and AXI settings.
 *
 * Procedure referenced: OpenBSD sys/dev/fdt/if_dwqe_fdt.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (dwqe_fdt_attach, the
 * snps,* properties and snps,axi-config).
 */
static void
describe_dma(
	const struct drv_fdt *fdt,
	uint32_t node,
	struct drv_dw_eqos_config *config)
{
	const uint8_t *value;
	uint32_t length;
	uint32_t burst;
	uint32_t phandle;
	uint32_t axi;
	unsigned index;
	int no_burst_x8;
	int error;

	/* Reads the FIFO sizes; zero lets the driver use the controller's. */
	config->tx_fifo_bytes = read_number(fdt, node, "tx-fifo-depth", 0);
	config->rx_fifo_bytes = read_number(fdt, node, "rx-fifo-depth", 0);

	/* Reads the burst lengths: one for both directions, each overridable. */
	burst = read_number(fdt, node, "snps,pbl", 8U);
	config->tx_burst = read_number(fdt, node, "snps,txpbl", burst);
	config->rx_burst = read_number(fdt, node, "snps,rxpbl", burst);

	/* Multiplies the burst lengths by eight unless the node says not to. */
	no_burst_x8 = has_property(fdt, node, "snps,no-pbl-x8");
	config->burst_x8 = 1;
	if (no_burst_x8)
		config->burst_x8 = 0;

	/* Reads the burst kinds and whether the queues pass frames on a threshold. */
	config->fixed_burst = has_property(fdt, node, "snps,fixed-burst");
	config->mixed_burst = has_property(fdt, node, "snps,mixed-burst");
	config->address_aligned = has_property(fdt, node, "snps,aal");
	config->threshold_mode = has_property(fdt, node, "snps,force_thresh_dma_mode");

	/* Finds the AXI settings' node; a controller without one keeps the defaults. */
	config->axi_present = 0;
	error = drv_fdt_property(fdt, node, "snps,axi-config", &value, &length);
	if (error != 0 || length != 4U)
		return;

	/* Resolves the node; a phandle that names none keeps the defaults. */
	phandle = (uint32_t)drv_fdt_cells_value(value, 0, 1U);
	error = drv_fdt_find_phandle(fdt, phandle, &axi);
	if (error != 0)
		return;

	/* Reads the outstanding request limits and low-power idle. */
	config->axi_present = 1;
	config->axi_lpi = has_property(fdt, axi, "snps,lpi_en");
	config->axi_write_limit = read_number(fdt, axi, "snps,wr_osr_lmt", 1U);
	config->axi_read_limit = read_number(fdt, axi, "snps,rd_osr_lmt", 1U);

	/* Reads the burst lengths the bus allows. */
	error = drv_fdt_property(fdt, axi, "snps,blen", &value, &length);
	if (error != 0)
		return;

	/* Keeps each length the property lists, up to the driver's table. */
	for (index = 0; index < DRV_DW_EQOS_BURST_LENGTHS && (index + 1U) * 4U <= length; index++)
		config->axi_burst_lengths[index] = (uint32_t)drv_fdt_cells_value(value, index, 1U);
}

/*
 * Finds the controller's main interrupt, the one interrupt-names calls
 * macirq, as a PLIC source.
 */
static int
describe_interrupt(
	const struct drv_fdt *fdt,
	uint32_t node,
	unsigned *irq)
{
	const uint8_t *value;
	uint32_t length;
	unsigned position;
	int error;

	/* Finds the interrupt's position; a node without names has it first. */
	error = drv_fdt_string_index(fdt, node, "interrupt-names", "macirq", &position);
	if (error != 0)
		position = 0;

	/* Reads the interrupts, one cell each on the PLIC. */
	error = drv_fdt_property(fdt, node, "interrupts", &value, &length);
	if (error != 0)
		return error;
	if ((position + 1U) * 4U > length)
		return EINVAL;

	/* Succeeded: the source number. */
	*irq = (unsigned)drv_fdt_cells_value(value, position, 1U);
	return 0;
}

/*
 * Reads the station address the firmware put in the tree, preferring an
 * explicit mac-address to the local-mac-address U-Boot fills in.
 */
static void
describe_mac(
	const struct drv_fdt *fdt,
	uint32_t node,
	struct drv_dw_eqos_config *config)
{
	const uint8_t *value;
	uint32_t length;
	int error;

	/* Reads the address; a tree without one leaves the driver to find one. */
	config->has_mac = 0;
	error = drv_fdt_property(fdt, node, "mac-address", &value, &length);
	if (error != 0)
		error = drv_fdt_property(fdt, node, "local-mac-address", &value, &length);
	if (error != 0 || length != sizeof(config->mac))
		return;

	/* Keeps it in network byte order. */
	kern_memcpy(config->mac, value, sizeof(config->mac));
	config->has_mac = 1;
}

/*
 * Sets the transmit clock source to the rate RGMII needs at a link speed.
 *
 * The source divides PLL0 by 1 to 15, so only the gigabit rate is in
 * reach; a slower speed reports the divider's ERANGE.
 *
 * Procedure referenced: OpenBSD sys/dev/fdt/if_dwqe_fdt.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3
 * (dwqe_mii_statchg_jh7110_task).
 */
static int
set_tx_rate(
	void *context,
	unsigned speed_mbps)
{
	const struct drv_vf2_clock *source;
	uint64_t rate;
	int error;

	/* Picks the RGMII transmit clock of the speed. */
	source = context;
	switch (speed_mbps) {
	case 1000U:
		rate = 125000000ULL;
		break;
	case 100U:
		rate = 25000000ULL;
		break;
	case 10U:
		rate = 2500000ULL;
		break;
	default:
		return EINVAL;
	}

	/* Sets the source's divider. */
	error = drv_vf2_clock_set_rate(source, rate);
	if (error != 0)
		return error;

	/* Succeeded: the transmit clock runs at the speed's rate. */
	return 0;
}

/*
 * Reads phy-mode and reports which directions the PHY delays; anything
 * but RGMII is refused.
 */
static int
read_mode(
	const struct drv_fdt *fdt,
	uint32_t node,
	int *rx_delay,
	int *tx_delay)
{
	const uint8_t *value;
	const char *mode;
	uint32_t length;
	unsigned index;
	int comparison;
	int error;

	/* Reads the mode, which must be a terminated string. */
	error = drv_fdt_property(fdt, node, "phy-mode", &value, &length);
	if (error != 0)
		return error;
	if (length == 0 || value[length - 1U] != 0)
		return EINVAL;
	mode = (const char *)value;

	/* Looks the mode up among the RGMII variants. */
	for (index = 0; index < sizeof(vf2_ethernet_modes) / sizeof(vf2_ethernet_modes[0]); index++) {
		/* Stops at the variant the tree names. */
		comparison = kern_strcmp(mode, vf2_ethernet_modes[index].name);
		if (comparison == 0)
			break;
	}

	/* Refuses any other interface, such as the RMII of the second port of an early board. */
	if (index == sizeof(vf2_ethernet_modes) / sizeof(vf2_ethernet_modes[0])) {
		kern_logf("vf2-ethernet: phy-mode %s unsupported\n", mode);
		return ENOTSUP;
	}

	/* Succeeded: the delays the PHY adds in this variant. */
	*rx_delay = vf2_ethernet_modes[index].rx_delay;
	*tx_delay = vf2_ethernet_modes[index].tx_delay;
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

/* Spins for at least the given number of microseconds; without a counter it does not wait. */
static void
pause_microseconds(
	uint64_t microseconds)
{
	uint64_t frequency;
	uint64_t start;
	uint64_t now;
	bool counted;

	/* Reads where the pause starts. */
	counted = kern_rtc_read_counter(&start, &frequency);
	if (!counted || frequency == 0)
		return;

	/* Spins until the counter has moved far enough. */
	for (;;) {
		/* A counter that stops being readable ends the pause. */
		counted = kern_rtc_read_counter(&now, &frequency);
		if (!counted)
			return;

		/* The pause is over once the time has passed. */
		if (now - start >= frequency * microseconds / 1000000ULL)
			return;
	}
}
