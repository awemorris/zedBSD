/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The S905X5M's glue for the ODROID-C5's Ethernet port.
 *
 * The port is a DesignWare Ethernet QoS controller wired by RGMII to a
 * gigabit PHY outside the SoC.  The SoC's glue registers in front of the
 * controller select RGMII, make the 125 MHz transmit clock from the 1 GHz
 * fixed clock and add the 2 ns transmit delay the device tree asks for;
 * the MDIO multiplexer is switched to the external bus, the PHY is taken
 * through its reset pin, and the PHY's own RGMII delays are turned off
 * (the glue gives the transmit delay, and an "rgmii" board gives the
 * receive one), as Linux does for an RTL8211F.  The DesignWare driver then
 * resets the controller, brings the link up and publishes en0.
 *
 * The bootloader leaves the glue set for the SoC's internal 100 Mbit/s
 * PHY, so everything here is written again rather than taken as found.
 */

#include "drivers/platform/s905x5m/s905x5m-ethernet.h"
#include "drivers/platform/s905x5m/s905x5m-soc.h"

#include <drivers/ethernet/dw-eqos.h>
#include <drivers/ethernet/mii-phy.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

/*
 * Where the controller, its glue registers and the MDIO multiplexer are,
 * and how much of each is mapped.
 *
 * Values referenced: Hardkernel common_drivers odroids7d-6.12.y @34daba9
 * arch/arm64/boot/dts/amlogic/meson-s7d.dtsi (ethernet@fe368000: reg
 * 0xfe368000/0x12ec and 0xfe364000/8; mdio-multiplexer@360000 in the apb4
 * bus at 0xfe000000).
 */
#define S905X5M_ETHERNET_REGISTERS	0xfe368000ULL
#define S905X5M_ETHERNET_REGISTER_BYTES	0x2000U
#define S905X5M_ETHERNET_GLUE		0xfe364000ULL
#define S905X5M_ETHERNET_MUX		0xfe360000ULL
#define S905X5M_ETHERNET_BLOCK_BYTES	0x1000U

/*
 * The bits of the glue's first register: the PHY interface, the source,
 * divider and enable of the RGMII transmit clock, the transmit delay, the
 * receive timing adjustment, the inverted RMII clock, and the transmit
 * and PHY reference clock generator.  The second register holds the
 * calibration value, zero for an external PHY.
 *
 * Values referenced: Hardkernel common_drivers odroids7d-6.12.y @34daba9
 * drivers/net/ethernet/stmicro/stmmac/dwmac-meson8b.c (PRG_ETH0,
 * PRG_ETH0_EXT_PHY_MODE_MASK, PRG_ETH0_EXT_RGMII_MODE,
 * PRG_ETH0_CLK_M250_SEL_MASK, PRG_ETH0_TXDLY_MASK,
 * PRG_ETH0_CLK_M250_DIV_SHIFT, PRG_ETH0_RGMII_TX_CLK_EN,
 * PRG_ETH0_INVERTED_RMII_CLK, PRG_ETH0_TX_AND_PHY_REF_CLK,
 * PRG_ETH0_ADJ_ENABLE, PRG_ETH0_ADJ_SETUP, PRG_ETH0_ADJ_DELAY,
 * PRG_ETH0_ADJ_SKEW, PRG_ETH1; aml_custom_setting: cali_val into PRG_ETH1
 * for internal_phy 2).
 */
#define S905X5M_GLUE_MODE		0x0U
#define S905X5M_GLUE_CALIBRATION	0x4U
#define S905X5M_GLUE_PHY_MODE_MASK	0x00000007U
#define S905X5M_GLUE_PHY_MODE_RGMII	0x00000001U
#define S905X5M_GLUE_CLOCK_SOURCE	0x00000010U
#define S905X5M_GLUE_TX_DELAY_SHIFT	5U
#define S905X5M_GLUE_TX_DELAY_MASK	0x00000060U
#define S905X5M_GLUE_CLOCK_DIVIDER_SHIFT 7U
#define S905X5M_GLUE_CLOCK_DIVIDER_MASK	0x00000380U
#define S905X5M_GLUE_TX_CLOCK_ENABLE	0x00000400U
#define S905X5M_GLUE_INVERTED_RMII	0x00000800U
#define S905X5M_GLUE_REFERENCE_CLOCK	0x00001000U
#define S905X5M_GLUE_RX_ADJUST_MASK	0x01ffe000U

/*
 * The divider that makes the 125 MHz transmit clock: the 1 GHz fixed
 * clock divided by 4, then by the fixed 2 behind the divider.
 *
 * Values referenced: Hardkernel common_drivers odroids7d-6.12.y @34daba9
 * drivers/net/ethernet/stmicro/stmmac/dwmac-meson8b.c
 * (meson8b_init_rgmii_tx_clk: m250_sel from clkin0, m250_div, fixed_div2,
 * rgmii_tx_en; meson8b_init_prg_eth: 125 MHz) and
 * arch/arm64/boot/dts/amlogic/meson-s7d.dtsi (ethernet: clkin0 is
 * fclk_div2).
 */
#define S905X5M_GLUE_CLOCK_DIVIDER	4U

/*
 * The MDIO multiplexer's control register, whose zero selects the bus to
 * the external PHY.
 *
 * Values referenced: Hardkernel common_drivers odroids7d-6.12.y @34daba9
 * drivers/net/mdio/mdio-mux-meson-g12a.c (ETH_PHY_CNTL2,
 * g12a_enable_external_mdio).
 */
#define S905X5M_MUX_CONTROL		0x88U

/*
 * The controller's peripheral gates.
 *
 * Values referenced: Hardkernel common_drivers odroids7d-6.12.y @34daba9
 * drivers/clk/meson/s7d.c (sys_ethphy: CLKCTRL_SYS_CLK_EN0_REG0 bit 4;
 * sys_eth: CLKCTRL_SYS_CLK_EN0_REG1 bit 3).
 */
#define S905X5M_ETHERNET_GATE0_BIT	4U
#define S905X5M_ETHERNET_GATE1_BIT	3U

/*
 * The port's pins: the RGMII lines and MDIO on GPIOZ_0..12 in function 6,
 * MDC on GPIOH_11 in function 4, without pulls at 4 mA.
 *
 * Values referenced: Hardkernel common_drivers odroids7d-6.12.y @34daba9
 * drivers/gpio/pinctrl/pinctrl-meson-s7d.c (eth_rgmii_rx_clk .. eth_mdio
 * on GPIOZ_0..12 function 6, eth_mdc on GPIOH_11 function 4) and
 * arch/arm64/boot/dts/amlogic/s7d_s905x5m_odroidc5.dts (eth, eth-rgmii:
 * bias-disable, drive-strength-microamp 4000).
 */
#define S905X5M_ETHERNET_FIRST_PIN	(DRV_S905X5M_GPIO_Z + 0U)
#define S905X5M_ETHERNET_PIN_COUNT	13U
#define S905X5M_ETHERNET_PIN_FUNCTION	6U
#define S905X5M_ETHERNET_MDC_PIN	(DRV_S905X5M_GPIO_H + 11U)
#define S905X5M_ETHERNET_MDC_FUNCTION	4U
#define S905X5M_ETHERNET_PIN_MICROAMPS	4000U

/*
 * The PHY's reset when the device tree's PHY node cannot be read: GPIOH_9,
 * active low and open drain, held 10 ms and then given 80 ms.
 *
 * Values referenced: Hardkernel common_drivers odroids7d-6.12.y @34daba9
 * arch/arm64/boot/dts/amlogic/s7d_s905x5m_odroidc5.dts (ethernet-phy@0:
 * reset-gpios GPIOH_9 with flags 7, reset-assert-us 10000,
 * reset-deassert-us 80000).
 */
#define S905X5M_PHY_RESET_PIN		(DRV_S905X5M_GPIO_H + 9U)
#define S905X5M_PHY_RESET_FLAGS		(DRV_S905X5M_GPIO_ACTIVE_LOW | DRV_S905X5M_GPIO_OPEN_DRAIN)
#define S905X5M_PHY_ASSERT_US		10000U
#define S905X5M_PHY_DEASSERT_US		80000U

/* The transmit delay the glue adds when the device tree names none, in nanoseconds. */
#define S905X5M_DEFAULT_TX_DELAY_NS	2U

/*
 * The RTL8211F: its identity, its page select register, and the page
 * whose registers hold the RGMII transmit and receive delays.
 *
 * Values referenced: Linux v7.3 drivers/net/phy/realtek/realtek_main.c
 * (PHY_ID_MATCH_EXACT(0x001cc916), RTL821x_PAGE_SELECT,
 * RTL8211F_RGMII_PAGE, RTL8211F_TXCR, RTL8211F_TX_DELAY, RTL8211F_RXCR,
 * RTL8211F_RX_DELAY).
 */
#define RTL8211F_IDENTITY		0x001cc916U
#define RTL8211F_PAGE_SELECT		0x1fU
#define RTL8211F_RGMII_PAGE		0x0d08U
#define RTL8211F_TX_CONTROL		0x11U
#define RTL8211F_TX_DELAY		0x0100U
#define RTL8211F_RX_CONTROL		0x15U
#define RTL8211F_RX_DELAY		0x0008U

/* The FIFO sizes when the device tree names none, in bytes. */
#define S905X5M_ETHERNET_TX_FIFO	0x800U
#define S905X5M_ETHERNET_RX_FIFO	0x1000U

/* The interface the port is published as. */
#define S905X5M_ETHERNET_NAME		"en0"

/*
 * The port's PHY as the device tree describes it.
 *
 * Filled by describe_phy() before the hardware is touched and read by the
 * attach only.
 */
struct s905x5m_phy {
	unsigned address;
	struct drv_s905x5m_gpio reset;
	uint32_t assert_us;
	uint32_t deassert_us;
};

/*
 * Whether the port was published.
 *
 * Set once by drv_s905x5m_ethernet_init() and read by the refresh; zero
 * means the boot goes on without the port.
 */
static int s905x5m_ethernet_attached;

static void switch_pins_on(void);
static void describe_phy(uint32_t node, struct s905x5m_phy *phy);
static int program_glue(uint32_t node);
static int select_external_mdio(void);
static void reset_phy(const struct s905x5m_phy *phy);
static int configure_phy(void *context, const struct drv_mii_bus *bus, unsigned phy, uint32_t identity);
static int modify_rtl8211f(const struct drv_mii_bus *bus, unsigned phy, unsigned reg, uint16_t clear, const char *what);

/*
 * Brings up the MAC the device tree names, its glue and the PHY, and
 * publishes the interface.
 */
int
drv_s905x5m_ethernet_init(
	uint64_t fdt_phys)
{
	struct drv_dw_eqos_config config;
	struct drv_s905x5m_interrupt interrupt;
	struct s905x5m_phy phy;
	uint32_t node;
	void *mapped;
	int error;

	/* Opens the device tree and maps the shared blocks. */
	error = drv_s905x5m_soc_init(fdt_phys);
	if (error != 0)
		return -error;

	/* Finds the port's controller. */
	error = drv_s905x5m_find_node("amlogic,meson-axg-dwmac", S905X5M_ETHERNET_REGISTERS, &node);
	if (error != 0) {
		kern_logf("s905x5m-ethernet: no controller at %llx in the device tree\n",
			  (unsigned long long)S905X5M_ETHERNET_REGISTERS);
		return -ENODEV;
	}

	/* Finds the controller's interrupt. */
	error = drv_s905x5m_node_interrupt(node, 0, &interrupt);
	if (error != 0) {
		kern_logf("s905x5m-ethernet: no interrupt (%d)\n", error);
		return -error;
	}

	/* Reads the PHY's address and reset before touching the hardware. */
	describe_phy(node, &phy);

	/* Switches the controller's gates and pins on. */
	drv_s905x5m_clock_update(DRV_S905X5M_CLOCK_GATES0,
				 1U << S905X5M_ETHERNET_GATE0_BIT,
				 1U << S905X5M_ETHERNET_GATE0_BIT);
	drv_s905x5m_clock_update(DRV_S905X5M_CLOCK_GATES1,
				 1U << S905X5M_ETHERNET_GATE1_BIT,
				 1U << S905X5M_ETHERNET_GATE1_BIT);
	switch_pins_on();

	/* Sets the glue for RGMII with its transmit clock and delay. */
	error = program_glue(node);
	if (error != 0)
		return -error;

	/* Routes the MDIO bus to the external PHY. */
	error = select_external_mdio();
	if (error != 0)
		return -error;

	/* Takes the PHY through its reset, after which it drives the receive clock. */
	reset_phy(&phy);

	/* Maps the controller's registers. */
	error = kern_device_map(S905X5M_ETHERNET_REGISTERS, S905X5M_ETHERNET_REGISTER_BYTES, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0)
		return -error;

	/* Describes the controller to the DesignWare driver. */
	kern_memset(&config, 0, sizeof(config));
	config.name = S905X5M_ETHERNET_NAME;
	config.registers = mapped;
	config.irq = interrupt.irq;
	config.dma_coherent = drv_fdt_dma_coherent(drv_s905x5m_fdt(), node, 0);
	config.csr_clock_hz = 0;
	config.phy_address = phy.address;
	config.tx_fifo_bytes = drv_s905x5m_node_number(node, "tx-fifo-depth", S905X5M_ETHERNET_TX_FIFO);
	config.rx_fifo_bytes = drv_s905x5m_node_number(node, "rx-fifo-depth", S905X5M_ETHERNET_RX_FIFO);
	config.has_mac = 0;
	config.set_speed = NULL;
	config.context = NULL;
	config.configure_phy = configure_phy;
	config.phy_context = NULL;

	/* Resets the controller, brings the PHY up and publishes the interface. */
	error = drv_dw_eqos_attach(&config);
	if (error != 0)
		return -error;

	/* Succeeded: the port is en0. */
	s905x5m_ethernet_attached = 1;
	return 0;
}

/*
 * Starts the link's service once interrupts are on.
 *
 * The DesignWare driver runs its link service from the attach, so the
 * refresh only reports a port that is missing.
 */
void
drv_s905x5m_ethernet_refresh(
	void)
{
	/* Leaves a board without the port alone. */
	if (!s905x5m_ethernet_attached)
		kern_logf("s905x5m-ethernet: no port to serve\n");
}

/* Gives the RGMII, MDIO and MDC pins their Ethernet functions, without pulls at 4 mA. */
static void
switch_pins_on(
	void)
{
	unsigned pin;

	/* Gives GPIOZ_0..12 the RGMII and MDIO function. */
	for (pin = S905X5M_ETHERNET_FIRST_PIN; pin < S905X5M_ETHERNET_FIRST_PIN + S905X5M_ETHERNET_PIN_COUNT; pin++) {
		/* Selects the function, turns the pull off and sets the drive. */
		(void)drv_s905x5m_pin_function(pin, S905X5M_ETHERNET_PIN_FUNCTION);
		(void)drv_s905x5m_pin_pull(pin, DRV_S905X5M_PULL_NONE);
		(void)drv_s905x5m_pin_drive(pin, S905X5M_ETHERNET_PIN_MICROAMPS);
	}

	/* Gives GPIOH_11 the MDC function. */
	(void)drv_s905x5m_pin_function(S905X5M_ETHERNET_MDC_PIN, S905X5M_ETHERNET_MDC_FUNCTION);
	(void)drv_s905x5m_pin_pull(S905X5M_ETHERNET_MDC_PIN, DRV_S905X5M_PULL_NONE);
	(void)drv_s905x5m_pin_drive(S905X5M_ETHERNET_MDC_PIN, S905X5M_ETHERNET_PIN_MICROAMPS);
}

/*
 * Reads the PHY's address, its reset pin and its reset times from the
 * node phy-handle names; what the tree does not say is the ODROID-C5's.
 */
static void
describe_phy(
	uint32_t node,
	struct s905x5m_phy *phy)
{
	const uint8_t *value;
	uint32_t length;
	uint32_t phandle;
	uint32_t phy_node;
	int error;

	/* Starts from the ODROID-C5's PHY. */
	phy->address = 0;
	phy->reset.pin = S905X5M_PHY_RESET_PIN;
	phy->reset.flags = S905X5M_PHY_RESET_FLAGS;
	phy->assert_us = S905X5M_PHY_ASSERT_US;
	phy->deassert_us = S905X5M_PHY_DEASSERT_US;

	/* Finds the PHY's node; without it the defaults stand. */
	error = drv_fdt_property(drv_s905x5m_fdt(), node, "phy-handle", &value, &length);
	if (error != 0)
		return;
	if (length < 4U)
		return;
	phandle = (uint32_t)drv_fdt_cells_value(value, 0, 1U);
	error = drv_fdt_find_phandle(drv_s905x5m_fdt(), phandle, &phy_node);
	if (error != 0)
		return;

	/* Reads the PHY's address on the bus. */
	phy->address = drv_s905x5m_node_number(phy_node, "reg", 0);

	/* Reads the reset pin and its times. */
	(void)drv_s905x5m_node_gpio(phy_node, "reset-gpios", &phy->reset);
	phy->assert_us = drv_s905x5m_node_number(phy_node, "reset-assert-us", S905X5M_PHY_ASSERT_US);
	phy->deassert_us = drv_s905x5m_node_number(phy_node, "reset-deassert-us", S905X5M_PHY_DEASSERT_US);
}

/*
 * Sets the glue for an external RGMII PHY: the interface, the 125 MHz
 * transmit clock from the 1 GHz fixed clock, the transmit delay, no
 * receive adjustment, and the clock generator on; the calibration
 * register is cleared.  Writes both registers before and after.
 *
 * Procedure referenced: Hardkernel common_drivers odroids7d-6.12.y
 * @34daba9 drivers/net/ethernet/stmicro/stmmac/dwmac-meson8b.c
 * (meson_axg_set_phy_mode, meson8b_init_rgmii_delays,
 * meson8b_init_prg_eth, aml_custom_setting).
 */
static int
program_glue(
	uint32_t node)
{
	volatile uint8_t *glue;
	uint32_t delay_ns;
	uint32_t before0;
	uint32_t before1;
	uint32_t after0;
	uint32_t after1;
	uint32_t mode;
	void *mapped;
	int error;

	/* Maps the glue's registers. */
	error = kern_device_map(S905X5M_ETHERNET_GLUE, S905X5M_ETHERNET_BLOCK_BYTES, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0)
		return error;
	glue = mapped;

	/* Reads what the bootloader left. */
	before0 = kern_mmio_read32(glue + S905X5M_GLUE_MODE);
	before1 = kern_mmio_read32(glue + S905X5M_GLUE_CALIBRATION);

	/* Reads the transmit delay the board asks for; the field counts 2 ns steps. */
	delay_ns = drv_s905x5m_node_number(node, "amlogic,tx-delay-ns", S905X5M_DEFAULT_TX_DELAY_NS);

	/* Builds the mode register from what it was, changing every field this port uses. */
	mode = before0;
	mode &= ~S905X5M_GLUE_PHY_MODE_MASK;
	mode |= S905X5M_GLUE_PHY_MODE_RGMII;
	mode &= ~S905X5M_GLUE_CLOCK_SOURCE;
	mode &= ~S905X5M_GLUE_CLOCK_DIVIDER_MASK;
	mode |= S905X5M_GLUE_CLOCK_DIVIDER << S905X5M_GLUE_CLOCK_DIVIDER_SHIFT;
	mode &= ~S905X5M_GLUE_TX_DELAY_MASK;
	mode |= ((delay_ns / 2U) << S905X5M_GLUE_TX_DELAY_SHIFT) & S905X5M_GLUE_TX_DELAY_MASK;
	mode &= ~S905X5M_GLUE_RX_ADJUST_MASK;
	mode &= ~S905X5M_GLUE_INVERTED_RMII;
	mode |= S905X5M_GLUE_TX_CLOCK_ENABLE;
	mode |= S905X5M_GLUE_REFERENCE_CLOCK;

	/* Writes the mode, then clears the calibration an external PHY does not use. */
	kern_mmio_write32(glue + S905X5M_GLUE_MODE, mode);
	kern_mmio_write32(glue + S905X5M_GLUE_CALIBRATION, 0);

	/* Reads the registers back and writes them before and after. */
	after0 = kern_mmio_read32(glue + S905X5M_GLUE_MODE);
	after1 = kern_mmio_read32(glue + S905X5M_GLUE_CALIBRATION);
	kern_logf("s905x5m-ethernet: glue %08x %08x -> %08x %08x (tx delay %u ns)\n",
		  before0,
		  before1,
		  after0,
		  after1,
		  delay_ns);

	/* Succeeded: the glue runs RGMII. */
	return 0;
}

/* Switches the MDIO multiplexer to the bus of the external PHY. */
static int
select_external_mdio(
	void)
{
	volatile uint8_t *mux;
	uint32_t before;
	uint32_t after;
	void *mapped;
	int error;

	/* Maps the multiplexer's registers. */
	error = kern_device_map(S905X5M_ETHERNET_MUX, S905X5M_ETHERNET_BLOCK_BYTES, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0)
		return error;
	mux = mapped;

	/* Selects the external bus, which also detaches the internal PHY from it. */
	before = kern_mmio_read32(mux + S905X5M_MUX_CONTROL);
	kern_mmio_write32(mux + S905X5M_MUX_CONTROL, 0);

	/* Reads the register back and writes it before and after. */
	after = kern_mmio_read32(mux + S905X5M_MUX_CONTROL);
	kern_logf("s905x5m-ethernet: MDIO multiplexer %08x -> %08x (external bus)\n",
		  before,
		  after);

	/* Succeeded: the MAC's MDIO reaches the external PHY. */
	return 0;
}

/*
 * Holds the PHY in reset and releases it.
 *
 * An active-low pin is driven low to assert; an open-drain pin is
 * released by turning it into an input, which the board's pull-up takes
 * high.
 */
static void
reset_phy(
	const struct s905x5m_phy *phy)
{
	int asserted;
	int released;
	int active_low;
	int open_drain;

	/* Reads how the pin asserts and how it lets go. */
	active_low = 0;
	if ((phy->reset.flags & DRV_S905X5M_GPIO_ACTIVE_LOW) != 0)
		active_low = 1;
	open_drain = 0;
	if ((phy->reset.flags & DRV_S905X5M_GPIO_OPEN_DRAIN) != 0)
		open_drain = 1;

	/* Makes the pin a GPIO. */
	(void)drv_s905x5m_pin_function(phy->reset.pin, 0);

	/* Asserts the reset and holds it: an active-low pin asserts low. */
	asserted = 1;
	released = 0;
	if (active_low) {
		asserted = 0;
		released = 1;
	}
	(void)drv_s905x5m_gpio_output(phy->reset.pin, asserted);
	drv_s905x5m_delay_us(phy->assert_us);

	/* Releases the reset: an open-drain pin floats, any other drives the other level. */
	if (open_drain) {
		(void)drv_s905x5m_pin_pull(phy->reset.pin, DRV_S905X5M_PULL_UP);
		(void)drv_s905x5m_gpio_input(phy->reset.pin);
	} else {
		(void)drv_s905x5m_gpio_output(phy->reset.pin, released);
	}

	/* Gives the PHY its time to come out of reset. */
	drv_s905x5m_delay_us(phy->deassert_us);
	kern_logf("s905x5m-ethernet: PHY reset on pin %u (flags %x), held %u us, waited %u us\n",
		  phy->reset.pin,
		  phy->reset.flags,
		  phy->assert_us,
		  phy->deassert_us);
}

/*
 * Turns an RTL8211F's own RGMII delays off after its reset; another PHY
 * is left as it is, with its identity in the log.
 *
 * Procedure referenced: Linux v7.3 drivers/net/phy/realtek/realtek_main.c
 * (rtl8211f_config_rgmii_delay: both delays cleared for "rgmii", through
 * the paged registers with the page put back).
 */
static int
configure_phy(
	void *context,
	const struct drv_mii_bus *bus,
	unsigned phy,
	uint32_t identity)
{
	int error;

	(void)context;

	/* Leaves a PHY the glue does not know alone. */
	if (identity != RTL8211F_IDENTITY) {
		kern_logf("s905x5m-ethernet: PHY %08x is not an RTL8211F; its delays are left as they are\n", identity);
		return 0;
	}

	/* Turns the PHY's transmit delay off: the glue adds it. */
	error = modify_rtl8211f(bus, phy, RTL8211F_TX_CONTROL, RTL8211F_TX_DELAY, "tx");
	if (error != 0)
		return error;

	/* Turns the PHY's receive delay off, as an "rgmii" board asks. */
	error = modify_rtl8211f(bus, phy, RTL8211F_RX_CONTROL, RTL8211F_RX_DELAY, "rx");
	if (error != 0)
		return error;

	/* Succeeded: the PHY adds no delay of its own. */
	return 0;
}

/*
 * Clears bits of one register of the RTL8211F's RGMII page and puts the
 * page that was selected back.
 */
static int
modify_rtl8211f(
	const struct drv_mii_bus *bus,
	unsigned phy,
	unsigned reg,
	uint16_t clear,
	const char *what)
{
	uint16_t old_page;
	uint16_t before;
	uint16_t after;
	int error;

	/* Reads the page the PHY has selected. */
	error = bus->read(bus->context, phy, RTL8211F_PAGE_SELECT, &old_page);
	if (error != 0)
		return error;

	/* Selects the RGMII page. */
	error = bus->write(bus->context, phy, RTL8211F_PAGE_SELECT, RTL8211F_RGMII_PAGE);
	if (error != 0)
		return error;

	/* Reads the register, clears the bits and writes it back. */
	before = 0;
	after = 0;
	error = bus->read(bus->context, phy, reg, &before);
	if (error == 0) {
		after = (uint16_t)(before & ~clear);
		error = bus->write(bus->context, phy, reg, after);
	}

	/* Puts the old page back, even after a failed access. */
	(void)bus->write(bus->context, phy, RTL8211F_PAGE_SELECT, old_page);

	/* Reports a failed access of the register. */
	if (error != 0)
		return error;

	/* Writes the register before and after. */
	kern_logf("s905x5m-ethernet: RTL8211F %s delay register %04x -> %04x\n", what, before, after);

	/* Succeeded: the bits are clear. */
	return 0;
}
