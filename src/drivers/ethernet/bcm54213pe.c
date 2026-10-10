/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * BCM54213PE external gigabit PHY over a serialized clause-22 MDIO bus.
 * The MAC owns bus access and DMA; this module owns PHY reset, RGMII clock
 * skew, autonegotiation advertisement, and negotiated full-duplex speed.
 */

#include <drivers/ethernet/bcm54213pe.h>
#include <kern/clock.h>
#include <uapi/errno.h>
#include <stddef.h>

/* Standard clause-22 controls and BCM54xx vendor shadow registers. */
#define BCM54213_CONTROL 0U
#define BCM54213_STATUS 1U
#define BCM54213_ID_HIGH 2U
#define BCM54213_ID_LOW 3U
#define BCM54213_ADVERTISEMENT 4U
#define BCM54213_GIGABIT 9U
#define BCM54213_AUX_CONTROL 0x18U
#define BCM54213_AUX_STATUS 0x19U
#define BCM54213_CLOCK_SHADOW 0x1cU
#define BCM54213_RESET 0x8000U
#define BCM54213_AUTONEG 0x1200U
#define BCM54213_LINK_READY 0x0024U
#define BCM54213_FAMILY 0x600d84a0U

static int reset_phy(const struct drv_bcm54213pe *phy);
static int configure_delays(const struct drv_bcm54213pe *phy);

/*
 * Initializes the external PHY without waiting for a cable or link partner.
 */
int
drv_bcm54213pe_initialize(
	const struct drv_bcm54213pe *phy,
	uint32_t *identity)
{
	uint16_t high;
	uint16_t low;
	int error;

	/* Requires a serialized MDIO bus and storage for the probed identity. */
	if (phy == NULL || identity == NULL)
		return EINVAL;

	/* Rejects an incomplete bus adapter before issuing PHY transactions. */
	if (phy->read == NULL || phy->write == NULL)
		return EINVAL;

	/* Reads both halves of the PHY identity before touching vendor controls. */
	error = phy->read(phy->context, BCM54213_ID_HIGH, &high);
	if (error != 0)
		return error;

	/* Reads the second identity word as a separately debuggable operation. */
	error = phy->read(phy->context, BCM54213_ID_LOW, &low);
	if (error != 0)
		return error;
	*identity = ((uint32_t)high << 16) | low;

	/* Restricts vendor shadows to the Pi 4's BCM54213 family. */
	if ((*identity & 0xfffffff0U) != BCM54213_FAMILY)
		return ENOTSUP;

	/* Resets the PHY with a fixed deadline rather than extending it per MDIO read. */
	error = reset_phy(phy);
	if (error != 0)
		return error;

	/* Restores the board-requested clock skew after PHY reset. */
	error = configure_delays(phy);
	if (error != 0)
		return error;

	/* Advertises 10/100 full duplex without relying on pause-frame support. */
	error = phy->write(phy->context, BCM54213_ADVERTISEMENT, 0x0141U);
	if (error != 0)
		return error;

	/* Advertises 1000BASE-T full duplex through its separate standard register. */
	error = phy->write(phy->context, BCM54213_GIGABIT, 0x0200U);
	if (error != 0)
		return error;

	/* Starts autonegotiation while allowing the rest of boot to continue. */
	error = phy->write(phy->context, BCM54213_CONTROL, BCM54213_AUTONEG);
	if (error != 0)
		return error;

	/* Succeeded: cable negotiation continues independently of initialization. */
	return 0;
}

/*
 * Reads negotiated full-duplex speed, reporting zero while carrier is absent.
 */
int
drv_bcm54213pe_read_link(
	const struct drv_bcm54213pe *phy,
	unsigned *speed_mbps)
{
	uint16_t status;
	uint16_t auxiliary;
	int error;

	/* Requires an MDIO read callback and storage for the speed. */
	if (phy == NULL || speed_mbps == NULL)
		return EINVAL;

	/* Rejects an incomplete adapter without dereferencing its callback. */
	if (phy->read == NULL)
		return EINVAL;

	/* Clears the standard latched-low carrier bit before sampling current status. */
	*speed_mbps = 0;
	error = phy->read(phy->context, BCM54213_STATUS, &status);
	if (error != 0)
		return error;

	/* Requires current carrier and completed autonegotiation. */
	error = phy->read(phy->context, BCM54213_STATUS, &status);
	if (error != 0)
		return error;

	/* Succeeds with no carrier while a cable is absent or negotiation is pending. */
	if ((status & BCM54213_LINK_READY) != BCM54213_LINK_READY)
		return 0;

	/* Reads the Broadcom highest-common-denominator result after link is stable. */
	error = phy->read(phy->context, BCM54213_AUX_STATUS, &auxiliary);
	if (error != 0)
		return error;

	/* Selects only the full-duplex modes advertised by this PHY implementation. */
	switch (auxiliary & 0x0700U) {
	case 0x0700U:
		*speed_mbps = 1000U;
		break;
	case 0x0500U:
		*speed_mbps = 100U;
		break;
	case 0x0200U:
		*speed_mbps = 10U;
		break;
	default:
		break;
	}

	/* Succeeded: the caller has either a supported speed or no usable carrier. */
	return 0;
}

/* Completes one PHY reset within a fixed half-second wall-time bound. */
static int
reset_phy(
	const struct drv_bcm54213pe *phy)
{
	uint16_t control;
	uint64_t start;
	uint64_t frequency;
	uint64_t now;
	uint64_t current_frequency;
	bool counter_ready;
	unsigned attempts;
	int error;

	/* Establishes a deadline before the reset request can begin. */
	counter_ready = kern_rtc_read_counter(&start, &frequency);
	if (!counter_ready || frequency == 0)
		return EIO;

	/* Asks the PHY to reset its standard and vendor control state. */
	error = phy->write(phy->context, BCM54213_CONTROL, BCM54213_RESET);
	if (error != 0)
		return error;

	/* Caps iterations independently in case an otherwise valid counter stops advancing. */
	for (attempts = 0; attempts < 100000U; attempts++) {
		/* Polls the self-clearing reset bit through the MAC's bounded MDIO accessor. */
		error = phy->read(phy->context, BCM54213_CONTROL, &control);
		if (error != 0)
			return error;

		/* Succeeded: vendor shadows can now be configured. */
		if ((control & BCM54213_RESET) == 0)
			return 0;

		/* Rejects a missing or changed counter rather than waiting on an invalid deadline. */
		counter_ready = kern_rtc_read_counter(&now, &current_frequency);
		if (!counter_ready || current_frequency != frequency)
			return EIO;

		/* Limits PHY reset independently of MDIO command completion time. */
		if (now - start >= frequency / 2U)
			return ETIMEDOUT;
	}

	/* Reports a PHY or counter that never completed reset within the bounds. */
	return ETIMEDOUT;
}

/* Sets receive and transmit RGMII skew through the BCM54xx vendor shadows. */
static int
configure_delays(
	const struct drv_bcm54213pe *phy)
{
	uint16_t data;
	int error;

	/* Selects the miscellaneous auxiliary shadow for receive-clock controls. */
	error = phy->write(phy->context, BCM54213_AUX_CONTROL, 0x7007U);
	if (error != 0)
		return error;

	/* Preserves other auxiliary controls while clearing the receive-delay bit. */
	error = phy->read(phy->context, BCM54213_AUX_CONTROL, &data);
	if (error != 0)
		return error;
	data &= 0x7ff8U;
	data &= ~0x0200U;

	/* Adds receive-clock skew only when the board requests it. */
	if (phy->rx_delay != 0)
		data |= 0x0200U;

	/* Writes the auxiliary shadow with its write-enable and selector bits. */
	error = phy->write(
		phy->context,
		BCM54213_AUX_CONTROL,
		(uint16_t)(data | 0x8007U));
	if (error != 0)
		return error;

	/* Selects the separate transmit-clock shadow. */
	error = phy->write(phy->context, BCM54213_CLOCK_SHADOW, 0x0c00U);
	if (error != 0)
		return error;

	/* Preserves other clock controls before deciding transmit skew. */
	error = phy->read(phy->context, BCM54213_CLOCK_SHADOW, &data);
	if (error != 0)
		return error;
	data &= 0x03ffU;
	data &= ~0x0200U;

	/* Adds transmit-clock skew only when the PHY supplies that board delay. */
	if (phy->tx_delay != 0)
		data |= 0x0200U;

	/* Commits the selected clock-control shadow. */
	error = phy->write(
		phy->context,
		BCM54213_CLOCK_SHADOW,
		(uint16_t)(data | 0x8c00U));
	if (error != 0)
		return error;

	/* Succeeded: both RGMII clocks match the board's delay selection. */
	return 0;
}
