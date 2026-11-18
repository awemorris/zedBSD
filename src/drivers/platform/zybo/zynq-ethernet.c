/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Zynq-7000's glue for its first Ethernet controller, GEM0, a Cadence
 * GEM, and the ZYBO's RTL8211E PHY on GEM0's MDIO bus.
 *
 * The first stage loader (U-Boot SPL's ps7_init) has routed GEM0's RGMII
 * and MDIO pins and started its clocks.  The glue switches on a clock it
 * finds off, maps the controller, gives the driver a DMA device for the
 * processor's physical addresses (the PS's masters do not snoop the
 * processor's caches, so the driver's memory is uncached), the bus clock
 * cpu_1x that the MDIO clock is divided from, the board's address, the
 * PHY's setup and the transmit clock that follows the link's speed, and
 * the controller's GIC interrupt.
 *
 * The board's PHY answers at MDIO address 0; the emulator's xilinx-zynq-a9
 * machine puts a PHY of another make at address 7, which the driver finds
 * by scanning.  The board has no address of its own anywhere the
 * processor can read: it comes from the boot parameter mac0=, else from
 * the device tree, else it is a fixed locally administered one.
 */

#include <drivers/platform/zybo/zynq-ethernet.h>

#include <stddef.h>
#include <stdint.h>

#include <drivers/ethernet/cadence-gem.h>
#include <drivers/ethernet/mii-phy.h>
#include <drivers/ethernet/rtl8211e.h>
#include <drivers/generic/dma.h>
#include <drivers/generic/fdt.h>
#include <drivers/platform/zybo/zynq-slcr.h>
#include <kern/boot.h>
#include <kern/device-io.h>
#include <kern/irq.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/platform.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

/*
 * Where GEM0's registers are, how many bytes of them are mapped, and its
 * GIC interrupt (shared peripheral interrupt 22).
 *
 * Values referenced: Linux v6.12 arch/arm/boot/dts/xilinx/zynq-7000.dtsi
 * (gem0, reg 0xe000b000 0x1000, interrupts 0 22 4).
 */
#define ZYNQ_GEM0_PHYS			0xe000b000ULL
#define ZYNQ_GEM0_BYTES			0x1000U
#define ZYNQ_GEM0_IRQ			54

/*
 * The address the board's PHY answers at on GEM0's MDIO bus.
 *
 * Value referenced: Linux v6.12 arch/arm/boot/dts/xilinx/zynq-zybo.dts
 * (ethernet-phy@0, reg 0).
 */
#define ZYNQ_GEM0_PHY_ADDRESS		0U

/* The largest DMA allocation the driver makes: one ring's 256 slots of 2 KiB. */
#define ZYNQ_GEM_DMA_SEGMENT_BYTES	(256U * 2048U)

/* The bits of a physical address the PS's masters drive. */
#define ZYNQ_GEM_DMA_ADDRESS_BITS	32U

/*
 * GEM0's bus clock gate in APER_CLK_CTRL.
 *
 * Value referenced: Linux v6.12 drivers/clk/zynq/clkc.c (gem0_aper, bit 6
 * of APER_CLK_CTRL).
 */
#define ZYNQ_APER_GEM0			0x00000040U

/*
 * The fields of GEM0_RCLK_CTRL and GEM0_CLK_CTRL: the receive and transmit
 * clock gates, the PLL the transmit clock comes from (the I/O PLL for 0
 * and 1, the ARM PLL for 2 and the DDR PLL for 3), the selection of a
 * transmit clock from the programmable logic (EMIO) instead, and the two
 * dividers applied one after the other.
 *
 * Values referenced: Linux v6.12 drivers/clk/zynq/clkc.c (gem0_mux over
 * the periph parents io, io, arm and ddr in bits 5:4 of GEM0_CLK_CTRL,
 * gem0_emio_mux in bit 6, gem0_div0 in bits 13:8 and gem0_div1 in bits
 * 25:20, one-based, the gate in bit 0); U-Boot v2025.07
 * board/xilinx/zynq/zynq-zybo/ps7_init_gpl.c (0xf8000138: the receive
 * clock's gate in bit 0, its source in bit 4).
 */
#define ZYNQ_GEM_RCLK_CLKACT		0x00000001U
#define ZYNQ_GEM_CLK_CLKACT		0x00000001U
#define ZYNQ_GEM_CLK_SRCSEL_SHIFT	4U
#define ZYNQ_GEM_CLK_SRCSEL_MASK	0x3U
#define ZYNQ_GEM_CLK_SRCSEL_ARM		2U
#define ZYNQ_GEM_CLK_SRCSEL_DDR		3U
#define ZYNQ_GEM_CLK_EMIO		0x00000040U
#define ZYNQ_GEM_CLK_DIVISOR0_SHIFT	8U
#define ZYNQ_GEM_CLK_DIVISOR1_SHIFT	20U
#define ZYNQ_GEM_CLK_DIVISOR_MASK	0x3fU
#define ZYNQ_GEM_CLK_DIVISOR_MAX	63U

/*
 * The transmit clock RGMII needs at each speed: 125 MHz at 1000 Mbit/s,
 * 25 MHz at 100 and 2.5 MHz at 10.
 *
 * Values referenced: Linux v6.12 drivers/net/ethernet/cadence/macb_main.c
 * (macb_set_tx_clk(): 125000000, 25000000, 2500000).
 */
#define ZYNQ_GEM_TX_CLOCK_1000_HZ	125000000ULL
#define ZYNQ_GEM_TX_CLOCK_100_HZ	25000000ULL
#define ZYNQ_GEM_TX_CLOCK_10_HZ		2500000ULL

/*
 * The bus clock cpu_1x taken when the SLCR gives none: the ZYBO's, the ARM
 * PLL's 1300 MHz divided by 2 and by 6.
 *
 * Value referenced: U-Boot v2025.07
 * board/xilinx/zynq/zynq-zybo/ps7_init_gpl.c (0xf8000100 ARM_PLL_CTRL
 * FDIV 26, 0xf8000120 ARM_CLK_CTRL DIVISOR 2, 0xf80001c4 CLK_621_TRUE 1).
 */
#define ZYNQ_CPU_1X_FALLBACK_HZ		108333333ULL

/*
 * The address the board takes when nothing names one: locally
 * administered (the second-lowest bit of the first octet set, the lowest,
 * multicast, clear), the rest "ZYBO0" in ASCII.  A fixed address keeps the
 * DHCP lease and the clients' known hosts the same from boot to boot.
 */
#define ZYNQ_GEM_DEFAULT_MAC_0		0x02U
#define ZYNQ_GEM_DEFAULT_MAC_1		0x5aU
#define ZYNQ_GEM_DEFAULT_MAC_2		0x59U
#define ZYNQ_GEM_DEFAULT_MAC_3		0x42U
#define ZYNQ_GEM_DEFAULT_MAC_4		0x4fU
#define ZYNQ_GEM_DEFAULT_MAC_5		0x30U

/* How long mac0='s value is: six pairs of hex digits and five colons. */
#define ZYNQ_GEM_MAC_TEXT_LENGTH	17U

/* The length of a device tree's local-mac-address. */
#define ZYNQ_GEM_MAC_BYTES		6U

/*
 * How much of memory at the device tree's address may be read.  The
 * hardware layer bounds the tree it accepts the same way.
 */
#define ZYNQ_FDT_READ_LIMIT		(2U * 1024U * 1024U)

/* Marks a parameter that the driver's hook interface passes but this board does not need. */
#define UNUSED_PARAMETER(parameter)	((void)(parameter))

/*
 * The ZYBO's one Ethernet port as the glue knows it.
 *
 * It is filled once at boot by drv_zynq_ethernet_init() and kept for the
 * kernel's life: registers is set once the controller is mapped, which
 * refuses a second attachment; handler and handler_argument are the
 * driver's interrupt handler, set before the interrupt is unmasked and
 * read by the interrupt; gem is the attached controller, NULL until the
 * attachment has succeeded.
 */
struct zynq_ethernet {
	volatile uint8_t *registers;
	void (*handler)(void *argument);
	void *handler_argument;
	struct drv_cadence_gem *gem;
};

/* The ZYBO's one Ethernet port; attachment keeps it until the kernel stops. */
static struct zynq_ethernet board_ethernet;

/*
 * The boot parameters, parsed here from the loader's line: the platform's
 * devices are found before the kernel parses them for itself.  Filled
 * once by choose_mac() during the attachment and not read afterwards.
 */
static struct kern_boot_parameters boot_line_parameters;

static int switch_clocks_on(void);
static uint64_t bus_clock_rate(void);
static int create_dma(struct drv_dma_device **dma);
static void choose_mac(uint8_t mac[6]);
static int mac_from_parameters(uint8_t mac[6]);
static int mac_from_tree(uint8_t mac[6]);
static int parse_mac(const char *text, uint8_t mac[6]);
static int hex_digit(char character, unsigned *value);
static int setup_phy(void *context, const struct drv_mii_bus *bus, unsigned phy, uint32_t *identity);
static void set_tx_clock(void *context, unsigned speed_mbps);
static uint64_t tx_source_rate(uint32_t control);
static int establish_irq(void *context, void (*handler)(void *argument), void *argument);
static void interrupt(int irq, kern_irq_ack_t acknowledge, void *argument);

/*
 * Starts GEM0 and the board's PHY and publishes the interface en0.
 *
 * Returns 0, EALREADY on a second call, or the errno of the step that
 * failed; the boot goes on without the network.
 */
int
drv_zynq_ethernet_init(
	void)
{
	struct zynq_ethernet *ethernet;
	struct drv_cadence_gem_config config;
	const char *stage;
	void *mapped;
	int error;

	/* Refuses a second attachment. */
	ethernet = &board_ethernet;
	if (ethernet->registers != NULL)
		return EALREADY;

	/* Maps the SLCR the controller's clocks are read and set in. */
	stage = "SLCR";
	error = drv_zynq_slcr_init();
	if (error != 0)
		goto fail;

	/* Switches on a clock of the controller the first stage loader left off. */
	stage = "clocks";
	error = switch_clocks_on();
	if (error != 0)
		goto fail;

	/* Maps the controller's registers as device memory. */
	stage = "registers";
	error = kern_device_map(ZYNQ_GEM0_PHYS, ZYNQ_GEM0_BYTES, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0)
		goto fail;
	ethernet->registers = mapped;

	/* Creates the DMA device the driver's rings and slots come from. */
	kern_memset(&config, 0, sizeof(config));
	stage = "DMA buffers";
	error = create_dma(&config.dma);
	if (error != 0)
		goto fail;

	/* Takes the board's address. */
	choose_mac(config.mac);

	/*
	 * Describes the controller: en0, 32-bit descriptors, the MDIO clock
	 * from cpu_1x, the PHY at address 0 or wherever one answers, the
	 * receive restart this controller needs, and RGMII.
	 */
	config.name = "en0";
	config.registers = ethernet->registers;
	config.descriptor_width = DRV_CADENCE_GEM_DESCRIPTOR_32;
	config.pclk_hz = bus_clock_rate();
	config.phy_address = ZYNQ_GEM0_PHY_ADDRESS;
	config.phy_scan = 1;
	config.flags = DRV_CADENCE_GEM_FLAG_RESTART_RX_ON_USED_BIT | DRV_CADENCE_GEM_FLAG_USRIO_RGMII;

	/* Gives the driver the board's steps: the PHY's setup, the transmit clock, the interrupt. */
	config.context = ethernet;
	config.reset_phy = NULL;
	config.setup_phy = setup_phy;
	config.read_link = NULL;
	config.set_tx_clock = set_tx_clock;
	config.establish_irq = establish_irq;

	/* Has the driver reset the controller, start the PHY and publish the interface. */
	error = drv_cadence_gem_attach(&config, &ethernet->gem);
	if (error != 0)
		return error;

	/* Succeeded: networkd can open the interface. */
	return 0;

fail:
	/* Reports where the attachment stopped; what was taken is kept, as nothing reaches it. */
	kern_logf("zynq-ethernet: initialization failed at %s (%d)\n", stage, error);
	return error;
}

/*
 * Starts the periodic service of the link and of missed interrupts, once
 * interrupts are enabled.
 */
void
drv_zynq_ethernet_refresh(
	void)
{
	/* Leaves a port that did not attach alone. */
	if (board_ethernet.gem == NULL)
		return;

	/* Starts the service of the attached interface. */
	drv_cadence_gem_start_service(board_ethernet.gem);
}

/*
 * Switches GEM0's bus clock, receive clock and transmit clock on when any
 * of them is off.
 *
 * On the board the first stage loader started all three, and the
 * emulator's reset values have them on; a write is made only for a clock
 * found off.  The transmit clock's rate is set when the link comes up.
 */
static int
switch_clocks_on(
	void)
{
	uint32_t gates;
	uint32_t control;
	int error;

	/* Reads the bus clock gates. */
	error = drv_zynq_slcr_read(DRV_ZYNQ_SLCR_APER_CLK_CTRL, &gates);
	if (error != 0)
		return error;

	/* Opens GEM0's bus clock gate when it is closed. */
	if ((gates & ZYNQ_APER_GEM0) == 0) {
		error = drv_zynq_slcr_modify(DRV_ZYNQ_SLCR_APER_CLK_CTRL, ZYNQ_APER_GEM0, ZYNQ_APER_GEM0);
		if (error != 0)
			return error;

		/* Writes the change to the boot log. */
		kern_logf("zynq-ethernet: switched the bus clock on\n");
	}

	/* Reads the receive clock's control. */
	error = drv_zynq_slcr_read(DRV_ZYNQ_SLCR_GEM0_RCLK_CTRL, &control);
	if (error != 0)
		return error;

	/* Starts the receive clock when it is stopped. */
	if ((control & ZYNQ_GEM_RCLK_CLKACT) == 0) {
		error = drv_zynq_slcr_modify(DRV_ZYNQ_SLCR_GEM0_RCLK_CTRL, ZYNQ_GEM_RCLK_CLKACT, ZYNQ_GEM_RCLK_CLKACT);
		if (error != 0)
			return error;

		/* Writes the change to the boot log. */
		kern_logf("zynq-ethernet: switched the receive clock on\n");
	}

	/* Reads the transmit clock's control. */
	error = drv_zynq_slcr_read(DRV_ZYNQ_SLCR_GEM0_CLK_CTRL, &control);
	if (error != 0)
		return error;

	/* Starts the transmit clock when it is stopped. */
	if ((control & ZYNQ_GEM_CLK_CLKACT) == 0) {
		error = drv_zynq_slcr_modify(DRV_ZYNQ_SLCR_GEM0_CLK_CTRL, ZYNQ_GEM_CLK_CLKACT, ZYNQ_GEM_CLK_CLKACT);
		if (error != 0)
			return error;

		/* Writes the change to the boot log. */
		kern_logf("zynq-ethernet: switched the transmit clock on\n");
	}

	/* Succeeded: the three clocks run. */
	return 0;
}

/*
 * Finds the bus clock cpu_1x, which the controller's register interface
 * runs on and its MDIO clock is divided from.
 *
 * A clock tree that gives no rate leaves the ZYBO's, with a warning in the
 * boot log.
 */
static uint64_t
bus_clock_rate(
	void)
{
	uint64_t rate;

	/* Reads the rate from the clock tree. */
	rate = drv_zynq_slcr_cpu_1x_hz();
	if (rate == 0) {
		kern_logf("zynq-ethernet: bus clock unknown, taking %llu Hz\n",
			  (unsigned long long)ZYNQ_CPU_1X_FALLBACK_HZ);
		return ZYNQ_CPU_1X_FALLBACK_HZ;
	}

	/* Writes the rate the MDIO clock will be divided from to the boot log. */
	kern_logf("zynq-ethernet: bus clock %llu Hz\n", (unsigned long long)rate);

	/* Succeeded: the bus clock's rate. */
	return rate;
}

/*
 * Creates the controller's DMA device: 32-bit physical addresses, and
 * memory the controller does not see through the processor's caches.
 */
static int
create_dma(
	struct drv_dma_device **dma)
{
	struct drv_dma_constraints constraints;
	int error;

	/* Describes the controller's DMA. */
	kern_memset(&constraints, 0, sizeof(constraints));
	constraints.address_bits = ZYNQ_GEM_DMA_ADDRESS_BITS;
	constraints.max_segment_size = ZYNQ_GEM_DMA_SEGMENT_BYTES;
	constraints.segment_boundary = 0;
	constraints.coherent = 0;

	/* Creates the device on the processor's physical addresses. */
	error = drv_dma_device_create(&constraints, dma);
	if (error != 0)
		return error;

	/* Succeeded: the driver allocates its rings and slots from it. */
	return 0;
}

/*
 * Takes the board's address: mac0= from the boot parameters, else the
 * device tree's local-mac-address of GEM0, else the fixed one.
 */
static void
choose_mac(
	uint8_t mac[6])
{
	int found;

	/* Takes the address the boot parameters name. */
	found = mac_from_parameters(mac);
	if (found) {
		kern_logf("zynq-ethernet: address from mac0=\n");
		return;
	}

	/* Takes the address a loader wrote into the device tree. */
	found = mac_from_tree(mac);
	if (found) {
		kern_logf("zynq-ethernet: address from the device tree\n");
		return;
	}

	/* Takes the fixed address, and says so: two boards on one network would clash. */
	mac[0] = ZYNQ_GEM_DEFAULT_MAC_0;
	mac[1] = ZYNQ_GEM_DEFAULT_MAC_1;
	mac[2] = ZYNQ_GEM_DEFAULT_MAC_2;
	mac[3] = ZYNQ_GEM_DEFAULT_MAC_3;
	mac[4] = ZYNQ_GEM_DEFAULT_MAC_4;
	mac[5] = ZYNQ_GEM_DEFAULT_MAC_5;
	kern_logf("zynq-ethernet: no address given; taking the fixed one (mac0= sets another)\n");
}

/*
 * Reads mac0= from the loader's boot parameter line.
 *
 * Returns nonzero with the address in mac, or zero when the line names
 * none or names one that is not a unicast address, which is logged.
 */
static int
mac_from_parameters(
	uint8_t mac[6])
{
	const char *line;
	const char *value;
	int error;

	/* Without a boot line there is no mac0=. */
	line = kern_boot_handoff("boot.command-line");
	if (line == NULL)
		return 0;

	/* Parses the line; a line the parser refuses is reported by the boot itself. */
	error = kern_boot_parameters_parse(&boot_line_parameters, line, KERN_BOOT_PARAMETERS_STORAGE_SIZE);
	if (error != 0)
		return 0;

	/* Finds mac0=. */
	value = kern_boot_parameters_value(&boot_line_parameters, KERN_BOOT_PARAMETER_MAC0);
	if (value == NULL)
		return 0;

	/* Reads the address; a malformed one is passed over with a warning. */
	error = parse_mac(value, mac);
	if (error != 0) {
		kern_logf("zynq-ethernet: mac0=%s is not a unicast address written xx:xx:xx:xx:xx:xx\n", value);
		return 0;
	}

	/* Succeeded: the boot parameters name the address. */
	return 1;
}

/*
 * Reads GEM0's local-mac-address from the device tree the loader passed,
 * as a loader that knows the board's address writes it.
 *
 * Returns nonzero with the address in mac, or zero when the tree has none.
 */
static int
mac_from_tree(
	uint8_t mac[6])
{
	const uint64_t *fdt_range;
	const uint8_t *property;
	struct drv_fdt fdt;
	const void *blob;
	uint64_t address;
	uint64_t size;
	uint32_t length;
	uint32_t node;
	size_t available;
	unsigned index;
	int error;

	/* Finds the device tree's physical range the hardware layer kept. */
	fdt_range = kern_boot_handoff("armv7.fdt");
	if (fdt_range == NULL)
		return 0;

	/* Finds the tree in the direct map. */
	blob = kern_pmem_to_kernel((kern_physaddr_t)fdt_range[0]);
	if (blob == NULL)
		return 0;

	/* Reads no further than the tree, nor than the hardware layer's bound. */
	available = ZYNQ_FDT_READ_LIMIT;
	if (fdt_range[1] != 0 && fdt_range[1] < available)
		available = (size_t)fdt_range[1];

	/* Refuses a tree whose header does not check out. */
	error = drv_fdt_open(&fdt, blob, available);
	if (error != 0)
		return 0;

	/* Looks at every Cadence GEM node for GEM0's. */
	node = DRV_FDT_NO_NODE;
	for (;;) {
		/* Finds the next GEM node; none left means no address. */
		error = drv_fdt_find_compatible(&fdt, "cdns,gem", node, &node);
		if (error != 0)
			return 0;

		/* Passes over a node that is not GEM0. */
		error = drv_fdt_reg(&fdt, node, 0, &address, &size);
		if (error != 0)
			continue;
		if (address != ZYNQ_GEM0_PHYS)
			continue;

		/* GEM0 without an address leaves the choice to the fixed one. */
		error = drv_fdt_property(&fdt, node, "local-mac-address", &property, &length);
		if (error != 0)
			return 0;
		if (length != ZYNQ_GEM_MAC_BYTES)
			return 0;
		break;
	}

	/* Refuses a multicast address, which no station has. */
	if ((property[0] & 1U) != 0)
		return 0;

	/* Copies the address. */
	for (index = 0; index < ZYNQ_GEM_MAC_BYTES; index++)
		mac[index] = property[index];

	/* Succeeded: the tree names the address. */
	return 1;
}

/*
 * Reads an address written as six pairs of hex digits separated by
 * colons, such as 02:5a:59:42:4f:30.
 *
 * Returns 0, or EINVAL for other text, for a multicast address and for
 * all zeros.
 */
static int
parse_mac(
	const char *text,
	uint8_t mac[6])
{
	unsigned high;
	unsigned low;
	unsigned nonzero;
	unsigned index;
	size_t length;
	int error;

	/* Requires exactly six pairs and five colons. */
	length = kern_strlen(text);
	if (length != ZYNQ_GEM_MAC_TEXT_LENGTH)
		return EINVAL;

	/* Reads each pair and the colon after it. */
	nonzero = 0;
	for (index = 0; index < ZYNQ_GEM_MAC_BYTES; index++) {
		/* Reads the pair's first digit. */
		error = hex_digit(text[index * 3U], &high);
		if (error != 0)
			return error;

		/* Reads its second digit. */
		error = hex_digit(text[index * 3U + 1U], &low);
		if (error != 0)
			return error;

		/* Requires a colon between two pairs. */
		if (index + 1U < ZYNQ_GEM_MAC_BYTES && text[index * 3U + 2U] != ':')
			return EINVAL;

		/* Keeps the octet. */
		mac[index] = (uint8_t)((high << 4) | low);
		nonzero |= mac[index];
	}

	/* Refuses all zeros, which names no station. */
	if (nonzero == 0)
		return EINVAL;

	/* Refuses a multicast address. */
	if ((mac[0] & 1U) != 0)
		return EINVAL;

	/* Succeeded: the address is in mac. */
	return 0;
}

/* Reads one hex digit, in either case. */
static int
hex_digit(
	char character,
	unsigned *value)
{
	/* A decimal digit. */
	if (character >= '0' && character <= '9') {
		*value = (unsigned)(character - '0');
		return 0;
	}

	/* A lower-case letter digit. */
	if (character >= 'a' && character <= 'f') {
		*value = (unsigned)(character - 'a') + 10U;
		return 0;
	}

	/* An upper-case letter digit. */
	if (character >= 'A' && character <= 'F') {
		*value = (unsigned)(character - 'A') + 10U;
		return 0;
	}

	/* Anything else is not a digit. */
	return EINVAL;
}

/*
 * Resets the PHY, sets the RTL8211E's RGMII delays, and starts
 * autonegotiation with gigabit offered.
 *
 * The ZYBO wires the PHY as rgmii-id, the delays in the PHY; they are set
 * in its register rather than left to its straps.  The emulator's PHY is
 * another make and needs no delays.
 */
static int
setup_phy(
	void *context,
	const struct drv_mii_bus *bus,
	unsigned phy,
	uint32_t *identity)
{
	int error;

	UNUSED_PARAMETER(context);

	/* Resets the PHY to its defaults. */
	error = drv_mii_phy_reset(bus, phy);
	if (error != 0)
		return error;

	/* Has the RTL8211E delay both RGMII clocks. */
	if ((*identity & DRV_RTL8211E_IDENTITY_MASK) == (DRV_RTL8211E_IDENTITY & DRV_RTL8211E_IDENTITY_MASK)) {
		error = drv_rtl8211e_set_rgmii_delays(bus, phy, 1, 1);
		if (error != 0)
			return error;
	}

	/* Starts autonegotiation; the controller runs gigabit at full duplex. */
	error = drv_mii_phy_start_autonegotiation(bus, phy, 1);
	if (error != 0)
		return error;

	/* Succeeded: the PHY negotiates with its link partner. */
	return 0;
}

/*
 * Sets GEM0's transmit clock for the link's speed: the dividers that
 * bring the clock's PLL closest to 125, 25 or 2.5 MHz, the second divider
 * as small as an exact rate allows.  A transmit clock from the
 * programmable logic is left alone.
 *
 * Called with the driver's lock held and interrupts off.
 *
 * Procedure referenced: Linux v6.12 drivers/net/ethernet/cadence/macb_main.c
 * (macb_set_tx_clk(): the rate for the speed) and drivers/clk/clk-divider.c
 * (clk_divider_bestdiv(): the outer divider tried from the smallest, the
 * first exact rate taken).
 */
static void
set_tx_clock(
	void *context,
	unsigned speed_mbps)
{
	uint64_t target;
	uint64_t parent;
	uint64_t rate;
	uint64_t best_rate;
	uint64_t difference;
	uint64_t best_difference;
	uint32_t control;
	uint32_t divisor0;
	uint32_t divisor1;
	uint32_t best_divisor0;
	uint32_t best_divisor1;
	uint32_t mask;
	uint32_t bits;
	int error;

	UNUSED_PARAMETER(context);

	/* Takes the rate the speed needs; another speed keeps the clock. */
	if (speed_mbps == 1000U) {
		target = ZYNQ_GEM_TX_CLOCK_1000_HZ;
	} else if (speed_mbps == 100U) {
		target = ZYNQ_GEM_TX_CLOCK_100_HZ;
	} else if (speed_mbps == 10U) {
		target = ZYNQ_GEM_TX_CLOCK_10_HZ;
	} else {
		return;
	}

	/* Reads the transmit clock's control; without it the clock stays as it is. */
	error = drv_zynq_slcr_read(DRV_ZYNQ_SLCR_GEM0_CLK_CTRL, &control);
	if (error != 0)
		return;

	/* A clock from the programmable logic is the logic's to set. */
	if ((control & ZYNQ_GEM_CLK_EMIO) != 0)
		return;

	/* Finds the PLL the clock comes from; a PLL that gives no rate leaves it. */
	parent = tx_source_rate(control);
	if (parent == 0) {
		kern_logf("zynq-ethernet: transmit clock's PLL unknown (control %x)\n", control);
		return;
	}

	/* Tries every outer divider with the inner one that comes nearest. */
	best_divisor0 = 1;
	best_divisor1 = 1;
	best_rate = 0;
	best_difference = UINT64_MAX;
	for (divisor1 = 1; divisor1 <= ZYNQ_GEM_CLK_DIVISOR_MAX; divisor1++) {
		/* The inner divider rounded to the nearest, within its field. */
		divisor0 = (uint32_t)((parent + target * divisor1 / 2U) / (target * divisor1));
		if (divisor0 == 0)
			divisor0 = 1;
		if (divisor0 > ZYNQ_GEM_CLK_DIVISOR_MAX)
			continue;

		/* How far the pair misses the rate. */
		rate = parent / divisor0 / divisor1;
		difference = target - rate;
		if (rate > target)
			difference = rate - target;

		/* Keeps the nearest pair, the smaller outer divider on a tie. */
		if (difference < best_difference) {
			best_difference = difference;
			best_rate = rate;
			best_divisor0 = divisor0;
			best_divisor1 = divisor1;
		}

		/* Stops at an exact rate. */
		if (difference == 0)
			break;
	}

	/* Writes both dividers. */
	mask = ZYNQ_GEM_CLK_DIVISOR_MASK << ZYNQ_GEM_CLK_DIVISOR0_SHIFT;
	mask |= ZYNQ_GEM_CLK_DIVISOR_MASK << ZYNQ_GEM_CLK_DIVISOR1_SHIFT;
	bits = best_divisor0 << ZYNQ_GEM_CLK_DIVISOR0_SHIFT;
	bits |= best_divisor1 << ZYNQ_GEM_CLK_DIVISOR1_SHIFT;
	error = drv_zynq_slcr_modify(DRV_ZYNQ_SLCR_GEM0_CLK_CTRL, mask, bits);
	if (error != 0) {
		kern_logf("zynq-ethernet: transmit clock not set (%d)\n", error);
		return;
	}

	/* Writes the clock the link now runs on to the boot log. */
	kern_logf("zynq-ethernet: transmit clock %llu Hz for %u Mbps (pll %llu Hz / %u / %u)\n",
		  (unsigned long long)best_rate,
		  speed_mbps,
		  (unsigned long long)parent,
		  (unsigned)best_divisor0,
		  (unsigned)best_divisor1);
}

/* Reports the rate of the PLL a GEM clock control selects, or 0 when unknown. */
static uint64_t
tx_source_rate(
	uint32_t control)
{
	uint64_t parent;
	uint32_t source;

	/* Reads the PLL the selection names. */
	source = (control >> ZYNQ_GEM_CLK_SRCSEL_SHIFT) & ZYNQ_GEM_CLK_SRCSEL_MASK;
	if (source == ZYNQ_GEM_CLK_SRCSEL_ARM) {
		parent = drv_zynq_slcr_pll_hz(DRV_ZYNQ_SLCR_ARM_PLL_CTRL);
	} else if (source == ZYNQ_GEM_CLK_SRCSEL_DDR) {
		parent = drv_zynq_slcr_pll_hz(DRV_ZYNQ_SLCR_DDR_PLL_CTRL);
	} else {
		parent = drv_zynq_slcr_pll_hz(DRV_ZYNQ_SLCR_IO_PLL_CTRL);
	}

	/* Succeeded: the PLL's rate, 0 when the SLCR gave none. */
	return parent;
}

/*
 * Routes GEM0's interrupt from the GIC to the driver's handler and lets
 * it through; the controller's own sources stay off until the interface
 * opens.
 */
static int
establish_irq(
	void *context,
	void (*handler)(void *argument),
	void *argument)
{
	struct zynq_ethernet *ethernet;
	int error;

	/* Keeps the driver's handler before the interrupt can arrive. */
	ethernet = context;
	ethernet->handler = handler;
	ethernet->handler_argument = argument;

	/* Installs the adapter on the controller's GIC interrupt, a level one. */
	error = kern_irq_register(ZYNQ_GEM0_IRQ, interrupt, ethernet);
	if (error != 0)
		return error;

	/* Lets the interrupt through to the adapter. */
	kern_irq_unmask(ZYNQ_GEM0_IRQ);

	/* Succeeded: the controller's interrupt reaches the driver. */
	return 0;
}

/*
 * Runs the driver's handler for GEM0's interrupt and retires the
 * acknowledgement; the handler has cleared what the controller raised.
 */
static void
interrupt(
	int irq,
	kern_irq_ack_t acknowledge,
	void *argument)
{
	struct zynq_ethernet *ethernet;

	UNUSED_PARAMETER(irq);

	/* Serves the controller. */
	ethernet = argument;
	ethernet->handler(ethernet->handler_argument);

	/* Gives the interrupt back to the GIC. */
	kern_irq_send_eoi(acknowledge);
}
