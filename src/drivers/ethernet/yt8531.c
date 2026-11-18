/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Motorcomm YT8531 gigabit PHY's own settings.
 *
 * The PHY keeps its settings in extended registers, reached through two
 * standard-sized ones: the first takes the extended register's number,
 * the second reads or writes it.  Every access puts the number register
 * back as it was.
 *
 * The receive delay is a step of 150 ps from 0 to 2250 ps, with a fixed
 * 1900 ps that can be added on top; the transmit delay is the same steps
 * without the fixed part.  A board value that no step matches falls back
 * to the PHY's 1950 ps.
 */

#include <drivers/ethernet/yt8531.h>
#include <kern/klog.h>
#include <uapi/errno.h>

/*
 * The registers that reach the extended ones.
 *
 * Values referenced: Linux v6.12 drivers/net/phy/motorcomm.c
 * (YTPHY_PAGE_SELECT, YTPHY_PAGE_DATA); OpenBSD sys/dev/mii/ytphy.c,
 * master 859883f17cb8e20088fece5398547e4264ad30b3 (YT8511_REG_ADDR,
 * YT8511_REG_DATA).
 */
#define YT_EXTENDED_NUMBER		0x1eU
#define YT_EXTENDED_DATA		0x1fU

/*
 * The extended registers and their fields.
 *
 * Values referenced: Linux v6.12 drivers/net/phy/motorcomm.c
 * (YT8521_CHIP_CONFIG_REG, YT8521_CCR_RXC_DLY_EN,
 * YT8531_RGMII_LDO_VOL_MASK, YT8531_LDO_VOL_3V3, YT8531_LDO_VOL_1V8,
 * YT8521_RGMII_CONFIG1_REG, YT8521_RC1R_TX_CLK_SEL_INVERTED,
 * YT8521_RC1R_RX_DELAY_MASK, YT8521_RC1R_GE_TX_DELAY_MASK,
 * YTPHY_PAD_DRIVE_STRENGTH_REG, YT8531_RGMII_RXC_DS_MASK,
 * YT8531_RGMII_RXD_DS_HI_MASK, YT8531_RGMII_RXD_DS_LOW_MASK,
 * YT8531_RGMII_RX_DS_DEFAULT).
 */
#define YT_CHIP_CONFIG			0xa001U
#define YT_CHIP_RX_FIXED_DELAY		0x0100U
#define YT_CHIP_LDO_SHIFT		4U
#define YT_CHIP_LDO_MASK		0x3U
#define YT_LDO_3V3			0U
#define YT_LDO_2V5			1U

#define YT_RGMII_CONFIG			0xa003U
#define YT_RGMII_TX_CLOCK_INVERTED	0x4000U
#define YT_RGMII_RX_DELAY_SHIFT		10U
#define YT_RGMII_RX_DELAY_MASK		0x3c00U
#define YT_RGMII_TX_DELAY_SHIFT		0U
#define YT_RGMII_TX_DELAY_MASK		0x000fU

#define YT_PAD_DRIVE			0xa010U
#define YT_PAD_RX_CLOCK_SHIFT		13U
#define YT_PAD_RX_CLOCK_MASK		0xe000U
#define YT_PAD_RX_DATA_HIGH_SHIFT	12U
#define YT_PAD_RX_DATA_HIGH_MASK	0x1000U
#define YT_PAD_RX_DATA_LOW_SHIFT	4U
#define YT_PAD_RX_DATA_LOW_MASK		0x0030U
#define YT_PAD_DEFAULT_STRENGTH		3U

/*
 * The delay steps.
 *
 * Values referenced: Linux v6.12 drivers/net/phy/motorcomm.c
 * (ytphy_rgmii_delays, YT8521_CCR_RXC_DLY_1_900_NS,
 * YT8521_RC1R_RGMII_1_950_NS).
 */
#define YT_DELAY_STEP_PS		150U
#define YT_DELAY_STEPS			16U
#define YT_FIXED_RX_DELAY_PS		1900U
#define YT_DEFAULT_DELAY_STEP		13U

/* The receive pins' supply voltages the drive table covers. */
enum yt_supply {
	YT_SUPPLY_3V3 = 0,
	YT_SUPPLY_1V8 = 1
};

/*
 * One drive strength setting: the supply it applies at, the current the
 * board names in microamps, and the field's value.
 */
struct yt_drive {
	enum yt_supply supply;
	uint32_t microamps;
	uint16_t strength;
};

/*
 * The receive pins' drive strengths at each supply.
 *
 * Values referenced: Linux v6.12 drivers/net/phy/motorcomm.c
 * (yt8531_ldo_vol); OpenBSD sys/dev/mii/ytphy.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (ytphy_yt8531_ds_map).
 */
static const struct yt_drive yt_drives[] = {
	{ YT_SUPPLY_1V8, 1200U, 0 },
	{ YT_SUPPLY_1V8, 2100U, 1 },
	{ YT_SUPPLY_1V8, 2700U, 2 },
	{ YT_SUPPLY_1V8, 2910U, 3 },
	{ YT_SUPPLY_1V8, 3110U, 4 },
	{ YT_SUPPLY_1V8, 3600U, 5 },
	{ YT_SUPPLY_1V8, 3970U, 6 },
	{ YT_SUPPLY_1V8, 4350U, 7 },
	{ YT_SUPPLY_3V3, 3070U, 0 },
	{ YT_SUPPLY_3V3, 4080U, 1 },
	{ YT_SUPPLY_3V3, 4370U, 2 },
	{ YT_SUPPLY_3V3, 4680U, 3 },
	{ YT_SUPPLY_3V3, 5020U, 4 },
	{ YT_SUPPLY_3V3, 5450U, 5 },
	{ YT_SUPPLY_3V3, 5740U, 6 },
	{ YT_SUPPLY_3V3, 6140U, 7 }
};

static int read_extended(const struct drv_mii_bus *bus, unsigned phy, uint16_t number, uint16_t *value);
static int modify_extended(const struct drv_mii_bus *bus, unsigned phy, uint16_t number, uint16_t mask, uint16_t bits);
static void rx_delay_setting(const struct drv_yt8531_config *config, uint16_t *step, int *fixed);
static uint16_t tx_delay_setting(const struct drv_yt8531_config *config);
static uint16_t drive_setting(uint16_t chip_config, uint32_t microamps, const char *pins);

/*
 * Writes a board's RGMII delays and receive drive strengths to a YT8531.
 *
 * Procedure referenced: Linux v6.12 drivers/net/phy/motorcomm.c
 * (yt8531_config_init, ytphy_rgmii_clk_delay_config, yt8531_set_ds);
 * OpenBSD sys/dev/mii/ytphy.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (ytphy_yt8521_init,
 * ytphy_yt8531_init).
 */
int
drv_yt8531_configure(
	const struct drv_mii_bus *bus,
	unsigned phy,
	const struct drv_yt8531_config *config)
{
	uint16_t chip_config;
	uint16_t rx_step;
	uint16_t tx_step;
	uint16_t clock_strength;
	uint16_t data_strength;
	uint16_t bits;
	int fixed;
	int error;

	/* Refuses a missing bus or board description. */
	if (bus == NULL || config == NULL || phy > 31U)
		return EINVAL;

	/* Turns the board's delays into the PHY's steps. */
	rx_delay_setting(config, &rx_step, &fixed);
	tx_step = tx_delay_setting(config);

	/* Adds or removes the fixed receive delay. */
	bits = 0;
	if (fixed)
		bits = YT_CHIP_RX_FIXED_DELAY;
	error = modify_extended(bus, phy, YT_CHIP_CONFIG, YT_CHIP_RX_FIXED_DELAY, bits);
	if (error != 0)
		return error;

	/* Writes the receive and transmit delay steps, leaving the 100 Mbps transmit delay. */
	bits = (uint16_t)(rx_step << YT_RGMII_RX_DELAY_SHIFT);
	bits |= (uint16_t)(tx_step << YT_RGMII_TX_DELAY_SHIFT);
	error = modify_extended(bus,
				phy,
				YT_RGMII_CONFIG,
				YT_RGMII_RX_DELAY_MASK | YT_RGMII_TX_DELAY_MASK,
				bits);
	if (error != 0)
		return error;

	/* Reads the supply voltage the receive pins run at, which the drive table depends on. */
	error = read_extended(bus, phy, YT_CHIP_CONFIG, &chip_config);
	if (error != 0)
		return error;

	/* Picks the receive clock's and data pins' drive strengths. */
	clock_strength = drive_setting(chip_config, config->rx_clock_drive_ua, "clock");
	data_strength = drive_setting(chip_config, config->rx_data_drive_ua, "data");

	/* Writes them; the data strength's top bit lives apart from its two low ones. */
	bits = (uint16_t)(clock_strength << YT_PAD_RX_CLOCK_SHIFT);
	bits |= (uint16_t)(((data_strength >> 2) & 1U) << YT_PAD_RX_DATA_HIGH_SHIFT);
	bits |= (uint16_t)((data_strength & 3U) << YT_PAD_RX_DATA_LOW_SHIFT);
	error = modify_extended(bus,
				phy,
				YT_PAD_DRIVE,
				YT_PAD_RX_CLOCK_MASK | YT_PAD_RX_DATA_HIGH_MASK | YT_PAD_RX_DATA_LOW_MASK,
				bits);
	if (error != 0)
		return error;

	/* Writes what the PHY was set to the boot log, for checking against the board. */
	kern_logf("yt8531: rx delay step %u%s, tx delay step %u, drive clock %u data %u\n",
		  (unsigned)rx_step,
		  fixed ? " + 1900 ps" : "",
		  (unsigned)tx_step,
		  (unsigned)clock_strength,
		  (unsigned)data_strength);

	/* Succeeded: the PHY delays and drives the RGMII as the board needs. */
	return 0;
}

/*
 * Inverts the transmit clock or not, as the board says for the speed the
 * link came up at.
 *
 * A board that does not ask for the adjustment leaves the PHY alone.
 *
 * Procedure referenced: Linux v6.12 drivers/net/phy/motorcomm.c
 * (yt8531_link_change_notify); OpenBSD sys/dev/mii/ytphy.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (ytphy_yt8521_update).
 */
int
drv_yt8531_set_speed(
	const struct drv_mii_bus *bus,
	unsigned phy,
	const struct drv_yt8531_config *config,
	unsigned speed_mbps)
{
	uint16_t bits;
	int inverted;
	int error;

	/* Refuses a missing bus or board description. */
	if (bus == NULL || config == NULL || phy > 31U)
		return EINVAL;

	/* A board without the adjustment keeps the PHY's clock as it is. */
	if (!config->tx_clock_adjust)
		return 0;

	/* Picks the board's inversion for the speed. */
	switch (speed_mbps) {
	case 1000U:
		inverted = config->tx_clock_inverted_1000;
		break;
	case 100U:
		inverted = config->tx_clock_inverted_100;
		break;
	case 10U:
		inverted = config->tx_clock_inverted_10;
		break;
	default:
		return EINVAL;
	}

	/* Sets or clears the inversion. */
	bits = 0;
	if (inverted)
		bits = YT_RGMII_TX_CLOCK_INVERTED;
	error = modify_extended(bus, phy, YT_RGMII_CONFIG, YT_RGMII_TX_CLOCK_INVERTED, bits);
	if (error != 0)
		return error;

	/* Succeeded: the transmit clock suits the speed. */
	return 0;
}

/* Reads one extended register, putting the number register back afterwards. */
static int
read_extended(
	const struct drv_mii_bus *bus,
	unsigned phy,
	uint16_t number,
	uint16_t *value)
{
	uint16_t saved;
	int error;

	/* Keeps the number register's contents. */
	error = bus->read(bus->context, phy, YT_EXTENDED_NUMBER, &saved);
	if (error != 0)
		return error;

	/* Selects the extended register. */
	error = bus->write(bus->context, phy, YT_EXTENDED_NUMBER, number);
	if (error != 0)
		return error;

	/* Reads it. */
	error = bus->read(bus->context, phy, YT_EXTENDED_DATA, value);
	if (error != 0)
		return error;

	/* Puts the number register back. */
	error = bus->write(bus->context, phy, YT_EXTENDED_NUMBER, saved);
	if (error != 0)
		return error;

	/* Succeeded: the extended register's value. */
	return 0;
}

/* Replaces some bits of one extended register, putting the number register back afterwards. */
static int
modify_extended(
	const struct drv_mii_bus *bus,
	unsigned phy,
	uint16_t number,
	uint16_t mask,
	uint16_t bits)
{
	uint16_t saved;
	uint16_t value;
	int error;

	/* Keeps the number register's contents. */
	error = bus->read(bus->context, phy, YT_EXTENDED_NUMBER, &saved);
	if (error != 0)
		return error;

	/* Selects the extended register. */
	error = bus->write(bus->context, phy, YT_EXTENDED_NUMBER, number);
	if (error != 0)
		return error;

	/* Reads it. */
	error = bus->read(bus->context, phy, YT_EXTENDED_DATA, &value);
	if (error != 0)
		return error;

	/* Writes it back with the masked bits replaced. */
	value = (uint16_t)((value & ~mask) | (bits & mask));
	error = bus->write(bus->context, phy, YT_EXTENDED_DATA, value);
	if (error != 0)
		return error;

	/* Puts the number register back. */
	error = bus->write(bus->context, phy, YT_EXTENDED_NUMBER, saved);
	if (error != 0)
		return error;

	/* Succeeded: the bits are written. */
	return 0;
}

/*
 * Turns the board's receive delay into a step and whether the fixed
 * 1900 ps goes on top.
 *
 * A step alone is preferred; the fixed part is used only for a value no
 * step reaches by itself.  No delay is asked for when the interface mode
 * leaves the receive clock to the MAC.
 */
static void
rx_delay_setting(
	const struct drv_yt8531_config *config,
	uint16_t *step,
	int *fixed)
{
	uint32_t delay;

	/* The MAC delays the receive clock itself: the PHY adds nothing. */
	*fixed = 0;
	if (!config->rx_delay) {
		*step = 0;
		return;
	}

	/* A whole number of steps within the range needs no fixed part. */
	delay = config->rx_delay_ps;
	if (delay % YT_DELAY_STEP_PS == 0 && delay / YT_DELAY_STEP_PS < YT_DELAY_STEPS) {
		*step = (uint16_t)(delay / YT_DELAY_STEP_PS);
		return;
	}

	/* A value past the fixed part that is a whole number of steps above it uses it. */
	if (delay >= YT_FIXED_RX_DELAY_PS) {
		delay -= YT_FIXED_RX_DELAY_PS;
		if (delay % YT_DELAY_STEP_PS == 0 && delay / YT_DELAY_STEP_PS < YT_DELAY_STEPS) {
			*step = (uint16_t)(delay / YT_DELAY_STEP_PS);
			*fixed = 1;
			return;
		}
	}

	/* Anything else gets the PHY's own 1950 ps. */
	kern_logf("yt8531: rx delay %u ps unsupported, using 1950 ps\n", (unsigned)config->rx_delay_ps);
	*step = YT_DEFAULT_DELAY_STEP;
}

/* Turns the board's transmit delay into a step; no delay when the MAC delays it. */
static uint16_t
tx_delay_setting(
	const struct drv_yt8531_config *config)
{
	uint32_t delay;

	/* The MAC delays the transmit clock itself: the PHY adds nothing. */
	if (!config->tx_delay)
		return 0;

	/* Reports a whole number of steps within the range. */
	delay = config->tx_delay_ps;
	if (delay % YT_DELAY_STEP_PS == 0 && delay / YT_DELAY_STEP_PS < YT_DELAY_STEPS)
		return (uint16_t)(delay / YT_DELAY_STEP_PS);

	/* Reports the PHY's own 1950 ps for anything else. */
	kern_logf("yt8531: tx delay %u ps unsupported, using 1950 ps\n", (unsigned)delay);
	return YT_DEFAULT_DELAY_STEP;
}

/*
 * Finds the drive strength for a current at the pins' supply, or the
 * PHY's default when the board names none or one the table lacks.
 */
static uint16_t
drive_setting(
	uint16_t chip_config,
	uint32_t microamps,
	const char *pins)
{
	enum yt_supply supply;
	unsigned ldo;
	unsigned index;

	/* A board that names no current keeps the default. */
	if (microamps == 0)
		return YT_PAD_DEFAULT_STRENGTH;

	/* Decodes the supply; the table has no entries for 2.5 V. */
	ldo = (chip_config >> YT_CHIP_LDO_SHIFT) & YT_CHIP_LDO_MASK;
	if (ldo == YT_LDO_2V5) {
		kern_logf("yt8531: no rx %s drive table at 2.5 V, using the default\n", pins);
		return YT_PAD_DEFAULT_STRENGTH;
	}

	/* The remaining encodings are 3.3 V and two spellings of 1.8 V. */
	supply = YT_SUPPLY_1V8;
	if (ldo == YT_LDO_3V3)
		supply = YT_SUPPLY_3V3;

	/* Looks the current up at the supply. */
	for (index = 0; index < sizeof(yt_drives) / sizeof(yt_drives[0]); index++) {
		/* Reports the entry that matches both. */
		if (yt_drives[index].supply == supply && yt_drives[index].microamps == microamps)
			return yt_drives[index].strength;
	}

	/* Reports the default for a current the table lacks. */
	kern_logf("yt8531: rx %s drive %u uA unsupported, using the default\n", pins, (unsigned)microamps);
	return YT_PAD_DEFAULT_STRENGTH;
}
