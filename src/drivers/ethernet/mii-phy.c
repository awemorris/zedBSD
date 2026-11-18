/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The standard management registers of an Ethernet PHY.
 *
 * Registers 0 to 10 and 15 are the same in every PHY: the control and
 * status words, the identity, what the PHY and its link partner advertise
 * in autonegotiation, and the gigabit control and status words.  The link
 * is always autonegotiated; the speed and duplex are the best mode both
 * ends advertise.
 */

#include <drivers/ethernet/mii-phy.h>
#include <kern/clock.h>
#include <uapi/errno.h>

#include <stdbool.h>

/*
 * The standard registers and their bits.
 *
 * Values referenced: IEEE Std 802.3-2018, clause 22.2.4 (Table 22-6 and
 * the control, status, identifier and extended status registers),
 * clause 28.2.4.1 (the advertisement and link partner ability
 * registers, Annex 28B base page) and clause 40.5.1.1 (the 1000BASE-T
 * control and status registers).
 */
#define MII_CONTROL			0U
#define MII_STATUS			1U
#define MII_IDENTITY_HIGH		2U
#define MII_IDENTITY_LOW		3U
#define MII_ADVERTISE			4U
#define MII_PARTNER			5U
#define MII_GIGABIT_CONTROL		9U
#define MII_GIGABIT_STATUS		10U
#define MII_EXTENDED_STATUS		15U

#define MII_CONTROL_RESET		0x8000U
#define MII_CONTROL_AUTONEG		0x1000U
#define MII_CONTROL_POWER_DOWN		0x0800U
#define MII_CONTROL_ISOLATE		0x0400U
#define MII_CONTROL_RESTART		0x0200U

#define MII_STATUS_100_FULL		0x4000U
#define MII_STATUS_100_HALF		0x2000U
#define MII_STATUS_10_FULL		0x1000U
#define MII_STATUS_10_HALF		0x0800U
#define MII_STATUS_EXTENDED		0x0100U
#define MII_STATUS_AUTONEG_DONE		0x0020U
#define MII_STATUS_LINK			0x0004U

#define MII_ABILITY_SELECTOR		0x0001U
#define MII_ABILITY_10_HALF		0x0020U
#define MII_ABILITY_10_FULL		0x0040U
#define MII_ABILITY_100_HALF		0x0080U
#define MII_ABILITY_100_FULL		0x0100U
#define MII_ABILITY_MASK		0x01e0U

#define MII_GIGABIT_ADVERTISE_FULL	0x0200U
#define MII_GIGABIT_ADVERTISE_HALF	0x0100U
#define MII_GIGABIT_PARTNER_FULL	0x0800U
#define MII_GIGABIT_PARTNER_HALF	0x0400U

#define MII_EXTENDED_1000T_FULL		0x2000U

/*
 * How long a reset may take before it is given up on, in microseconds.
 *
 * Value referenced: IEEE Std 802.3-2018, clause 22.2.4.1.1 (the reset
 * completes within 0.5 s).
 */
#define MII_RESET_TIMEOUT_US		500000U

/*
 * One ten or hundred megabit mode: the status bit that says the PHY has it
 * and the ability bit that advertises it.
 */
struct mii_mode {
	uint16_t status;
	uint16_t ability;
};

/*
 * The ten and hundred megabit modes, in the order their bits are checked.
 *
 * Values referenced: IEEE Std 802.3-2018, clause 22.2.4.2 (status
 * register bits 1.11 to 1.14) and Annex 28B.2 (base page bits A0 to A3).
 */
static const struct mii_mode mii_modes[] = {
	{ MII_STATUS_10_HALF, MII_ABILITY_10_HALF },
	{ MII_STATUS_10_FULL, MII_ABILITY_10_FULL },
	{ MII_STATUS_100_HALF, MII_ABILITY_100_HALF },
	{ MII_STATUS_100_FULL, MII_ABILITY_100_FULL }
};

static int start_deadline(uint64_t microseconds, uint64_t *deadline);
static int deadline_passed(uint64_t deadline);

/*
 * Reads a PHY's 32-bit identity: the OUI, the model and the revision.
 */
int
drv_mii_phy_read_identity(
	const struct drv_mii_bus *bus,
	unsigned phy,
	uint32_t *identity)
{
	uint16_t high;
	uint16_t low;
	int error;

	/* Refuses a missing bus, result or an address beyond the bus's 32. */
	if (bus == NULL || identity == NULL || phy > 31U)
		return EINVAL;

	/* Reads the identity's upper half. */
	error = bus->read(bus->context, phy, MII_IDENTITY_HIGH, &high);
	if (error != 0)
		return error;

	/* Reads the identity's lower half. */
	error = bus->read(bus->context, phy, MII_IDENTITY_LOW, &low);
	if (error != 0)
		return error;

	/* An address with no PHY behind it reads as all ones on a pulled-up bus. */
	if (high == 0xffffU && low == 0xffffU)
		return ENXIO;

	/* Or as all zeros on a bus that floats low. */
	if (high == 0 && low == 0)
		return ENXIO;

	/* Succeeded: the identity, its upper half first. */
	*identity = ((uint32_t)high << 16) | low;
	return 0;
}

/*
 * Resets a PHY and waits until it reports the reset finished.
 */
int
drv_mii_phy_reset(
	const struct drv_mii_bus *bus,
	unsigned phy)
{
	uint64_t deadline;
	uint16_t control;
	int passed;
	int error;

	/* Refuses a missing bus or an address beyond the bus's 32. */
	if (bus == NULL || phy > 31U)
		return EINVAL;

	/* Sets how long the reset may take. */
	error = start_deadline(MII_RESET_TIMEOUT_US, &deadline);
	if (error != 0)
		return error;

	/* Starts the reset; the bit clears itself when the PHY is back. */
	error = bus->write(bus->context, phy, MII_CONTROL, MII_CONTROL_RESET);
	if (error != 0)
		return error;

	/* Waits for the reset bit to clear. */
	for (;;) {
		/* Reads the control word. */
		error = bus->read(bus->context, phy, MII_CONTROL, &control);
		if (error != 0)
			return error;

		/* The PHY has finished its reset. */
		if ((control & MII_CONTROL_RESET) == 0)
			break;

		/* A PHY that stays in reset is given up on. */
		passed = deadline_passed(deadline);
		if (passed)
			return ETIMEDOUT;
	}

	/* Succeeded: the PHY runs from its defaults. */
	return 0;
}

/*
 * Advertises every speed the PHY can do and starts autonegotiation.
 *
 * Gigabit is advertised, full duplex only, when the caller's MAC can run
 * it and the PHY reports it can.
 */
int
drv_mii_phy_start_autonegotiation(
	const struct drv_mii_bus *bus,
	unsigned phy,
	int gigabit)
{
	uint16_t status;
	uint16_t extended;
	uint16_t advertise;
	uint16_t gigabit_control;
	unsigned index;
	int error;

	/* Refuses a missing bus or an address beyond the bus's 32. */
	if (bus == NULL || phy > 31U)
		return EINVAL;

	/* Reads what the PHY can do. */
	error = bus->read(bus->context, phy, MII_STATUS, &status);
	if (error != 0)
		return error;

	/* Collects each ten and hundred megabit mode the PHY has. */
	advertise = MII_ABILITY_SELECTOR;
	for (index = 0; index < sizeof(mii_modes) / sizeof(mii_modes[0]); index++) {
		/* A mode the PHY lacks is not advertised. */
		if ((status & mii_modes[index].status) != 0)
			advertise |= mii_modes[index].ability;
	}

	/* Advertises them. */
	error = bus->write(bus->context, phy, MII_ADVERTISE, advertise);
	if (error != 0)
		return error;

	/* A PHY without the extended status has no gigabit registers. */
	if ((status & MII_STATUS_EXTENDED) != 0) {
		/* Reads whether the PHY does gigabit over copper. */
		error = bus->read(bus->context, phy, MII_EXTENDED_STATUS, &extended);
		if (error != 0)
			return error;

		/* Advertises full-duplex gigabit when both ends of the MII can run it. */
		gigabit_control = 0;
		if (gigabit && (extended & MII_EXTENDED_1000T_FULL) != 0)
			gigabit_control = MII_GIGABIT_ADVERTISE_FULL;

		/* Writes the gigabit advertisement, none when gigabit is not offered. */
		error = bus->write(bus->context, phy, MII_GIGABIT_CONTROL, gigabit_control);
		if (error != 0)
			return error;
	}

	/* Restarts autonegotiation with the new advertisement, the PHY powered and on the MII. */
	error = bus->write(bus->context, phy, MII_CONTROL, MII_CONTROL_AUTONEG | MII_CONTROL_RESTART);
	if (error != 0)
		return error;

	/* Succeeded: the PHY negotiates with its link partner. */
	return 0;
}

/*
 * Reads the speed and duplex the link settled on, or a zero speed while it
 * is down.
 */
int
drv_mii_phy_read_link(
	const struct drv_mii_bus *bus,
	unsigned phy,
	struct drv_mii_link *link)
{
	uint16_t status;
	uint16_t advertise;
	uint16_t partner;
	uint16_t common;
	uint16_t gigabit_control;
	uint16_t gigabit_status;
	int error;

	/* Refuses a missing bus, result or an address beyond the bus's 32. */
	if (bus == NULL || link == NULL || phy > 31U)
		return EINVAL;

	/* Starts from a link that is down. */
	link->speed_mbps = 0;
	link->full_duplex = 0;

	/* Reads the status word once to clear a link loss it latched. */
	error = bus->read(bus->context, phy, MII_STATUS, &status);
	if (error != 0)
		return error;

	/* Reads it again for the link as it is now. */
	error = bus->read(bus->context, phy, MII_STATUS, &status);
	if (error != 0)
		return error;

	/* A link that is down has no speed. */
	if ((status & MII_STATUS_LINK) == 0)
		return 0;

	/* Nor has one still negotiating. */
	if ((status & MII_STATUS_AUTONEG_DONE) == 0)
		return 0;

	/* A PHY without the extended status cannot have negotiated gigabit. */
	if ((status & MII_STATUS_EXTENDED) != 0) {
		/* Reads what this end advertised for gigabit. */
		error = bus->read(bus->context, phy, MII_GIGABIT_CONTROL, &gigabit_control);
		if (error != 0)
			return error;

		/* Reads what the link partner advertised for gigabit. */
		error = bus->read(bus->context, phy, MII_GIGABIT_STATUS, &gigabit_status);
		if (error != 0)
			return error;

		/* Picks gigabit, full duplex before half, when both ends advertise it. */
		if ((gigabit_control & MII_GIGABIT_ADVERTISE_FULL) != 0 &&
		    (gigabit_status & MII_GIGABIT_PARTNER_FULL) != 0) {
			link->speed_mbps = 1000U;
			link->full_duplex = 1;
		} else if ((gigabit_control & MII_GIGABIT_ADVERTISE_HALF) != 0 &&
			   (gigabit_status & MII_GIGABIT_PARTNER_HALF) != 0) {
			link->speed_mbps = 1000U;
			link->full_duplex = 0;
		}

		/* Succeeded: a gigabit link needs no look at the slower modes. */
		if (link->speed_mbps != 0)
			return 0;
	}

	/* Reads what this end advertised for ten and hundred megabits. */
	error = bus->read(bus->context, phy, MII_ADVERTISE, &advertise);
	if (error != 0)
		return error;

	/* Reads what the link partner advertised. */
	error = bus->read(bus->context, phy, MII_PARTNER, &partner);
	if (error != 0)
		return error;

	/* Picks the fastest mode both ends advertise, full duplex before half. */
	common = advertise & partner & MII_ABILITY_MASK;
	if ((common & MII_ABILITY_100_FULL) != 0) {
		link->speed_mbps = 100U;
		link->full_duplex = 1;
	} else if ((common & MII_ABILITY_100_HALF) != 0) {
		link->speed_mbps = 100U;
		link->full_duplex = 0;
	} else if ((common & MII_ABILITY_10_FULL) != 0) {
		link->speed_mbps = 10U;
		link->full_duplex = 1;
	} else if ((common & MII_ABILITY_10_HALF) != 0) {
		link->speed_mbps = 10U;
		link->full_duplex = 0;
	}

	/* Succeeded: the link's speed, zero when the two ends share no mode. */
	return 0;
}

/* Sets a deadline on the monotonic counter, a number of microseconds away. */
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
	if (!counted || frequency == 0)
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
