/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Intel AX211 private MMIO implementation.
 *
 * Powers the device up and down, takes ownership of it, reads its station
 * address and publishes the boot context and the image loader.
 */

#include "intel-ax211-mmio.h"
#include <kern/kcrt.h>

#include <stddef.h>
#include <stdint.h>

/*
 * The host-visible control registers (the CSR window at the start of BAR0)
 * that power the device up, hand it to the driver and start its firmware.
 *
 * Every register here is 32 bits wide and is written with a read-modify-write
 * of the bits named below, except the address and length registers, which are
 * written whole.
 *
 * - FUNCTION_CONTROL (0x000) carries the ownership handshake, the prepare
 *   request, the power-event enable, the host-wake-from-L1A permission and
 *   the self-boot enable.
 * - RESTART_CONTROL (0x020) carries the whole-device reset and the bus
 *   master stop request and its acknowledgment.
 * - POWER_CONTROL (0x024) carries the active-state request, the keep-awake
 *   request and the clock and sleep indications.
 * - LINK_STATE_CONTROL (0x03c) and LINK_WORKAROUNDS (0x100) tune the PCI
 *   Express link power states (L0s and L1A).
 * - HOST_ANNOUNCE (0x088) is the mailbox in which the driver says that an
 *   operating system driver is present.
 * - LATENCY_TOLERANCE (0x0d4) holds the latency tolerance the device
 *   reports while it boots.
 * - BOOT_CONTEXT_ADDRESS (0x118), LOADER_IMAGE_ADDRESS (0x120) and
 *   LOADER_IMAGE_LENGTH (0x128) tell the device where the boot context and
 *   the image loader are.  The two address registers are 64 bits wide as two
 *   words, and drv_intel_ax211_mmio_publish_gen3() writes the low word first.
 * - DMA_WAIT_TUNING (0x240) holds the DMA engine's wait threshold.  It is a
 *   debug register whose upper half is set to the maximum during power-up to
 *   avoid a hardware error under stress; this driver does so in apm_init.
 * - LINK_POWER_OVERRIDE (0x250) keeps the link awake while bit 31 is set.
 *   The prepare fallback sets it before its first retry; the stop sequence
 *   sets it and clears it again 1 ms later.
 * - STATION_ADDRESS_* (0x380 to 0x38c) hold the station address twice: the
 *   OEM strap copy and the fused (one-time programmable) copy.  Each copy is
 *   two words; the first gives address bytes 0 to 3 from its most
 *   significant byte down, the second gives bytes 4 and 5 from bits 15:0.
 *
 * Values referenced: Linux v6.17 drivers/net/wireless/intel/iwlwifi/iwl-csr.h
 * (all offsets) and
 * drivers/net/wireless/intel/iwlwifi/pcie/iwl-context-info-v2.h (the boot
 * context and loader image registers).
 *
 * Procedure referenced: Linux v6.17
 * drivers/net/wireless/intel/iwlwifi/pcie/gen1_2/trans.c (iwl_pcie_apm_init,
 * the DMA wait threshold).
 */
#define AX211_CSR_FUNCTION_CONTROL		0x000U
#define AX211_CSR_RESTART_CONTROL		0x020U
#define AX211_CSR_POWER_CONTROL			0x024U
#define AX211_CSR_LINK_STATE_CONTROL		0x03cU
#define AX211_CSR_HOST_ANNOUNCE			0x088U
#define AX211_CSR_LATENCY_TOLERANCE		0x0d4U
#define AX211_CSR_LINK_WORKAROUNDS		0x100U
#define AX211_CSR_BOOT_CONTEXT_ADDRESS		0x118U
#define AX211_CSR_LOADER_IMAGE_ADDRESS		0x120U
#define AX211_CSR_LOADER_IMAGE_LENGTH		0x128U
#define AX211_CSR_DMA_WAIT_TUNING		0x240U
#define AX211_CSR_LINK_POWER_OVERRIDE		0x250U
#define AX211_CSR_STATION_ADDRESS_BASE		0x380U
#define AX211_CSR_STATION_ADDRESS_FUSE_FIRST	(AX211_CSR_STATION_ADDRESS_BASE + 0x00U)
#define AX211_CSR_STATION_ADDRESS_FUSE_SECOND   (AX211_CSR_STATION_ADDRESS_BASE + 0x04U)
#define AX211_CSR_STATION_ADDRESS_STRAP_FIRST   (AX211_CSR_STATION_ADDRESS_BASE + 0x08U)
#define AX211_CSR_STATION_ADDRESS_STRAP_SECOND  (AX211_CSR_STATION_ADDRESS_BASE + 0x0cU)

/*
 * The bits of those registers, grouped by register.
 *
 * The three ownership bits of FUNCTION_CONTROL are bit 22, bit 27 and bit 28.
 * They are named here after what the driver uses them for: the ownership
 * handshake, the prepare request and the power-event enable.
 *
 * What the driver found about each group:
 * - FUNCTION_CONTROL.  Writing OWNERSHIP_REQUEST asks for the device; the
 *   device grants it by keeping the bit set when read back, within 50 us.
 *   When it does not, the prepare fallback sets PREPARE_REQUEST and asks
 *   again (drv_intel_ax211_mmio_take_ownership()).  HOST_WAKE_FROM_L1A lets
 *   management traffic wake the link and is set in apm_init.  SELF_BOOT is
 *   set last in publish_gen3, after the boot context and loader image are
 *   visible: from then on the device fetches the loader image by itself.
 *   PREPARE_REQUEST and POWER_EVENT_ENABLE are set together during stop.
 * - HOST_ANNOUNCE.  DRIVER_PRESENT (bit 5) is written once ownership has
 *   been granted.
 * - RESTART_CONTROL.  WHOLE_DEVICE resets the device, and the driver waits
 *   5 ms after it before the next access.  BUS_MASTER_STOP_REQUEST asks the
 *   DMA engines to stop, and BUS_MASTER_STOPPED reads back set when they
 *   have; the driver waits 100 us for it and treats a timeout as a warning.
 * - POWER_CONTROL.  ACTIVE_STATE moves the device from its unpowered D0U
 *   state to D0A; CLOCK_STABLE reads set when the MAC clock runs, which is
 *   needed before any periphery register is touched.  KEEP_AWAKE_REQUEST
 *   holds the device awake for register access; the hold is granted when
 *   CLOCK_STABLE is set and ENTERING_SLEEP is clear.
 * - LINK_STATE_CONTROL.  NO_L0S disables L0s without touching L1.
 * - LINK_WORKAROUNDS.  L1A_WAKE keeps the link from entering L0s while it
 *   wakes from L1A for received traffic.
 * - DMA_WAIT_TUNING.  MAXIMUM sets the upper half of the register, the FH
 *   (DMA engine) wait threshold, to its largest value.
 *
 * Values referenced: Linux v6.17 drivers/net/wireless/intel/iwlwifi/iwl-csr.h
 * (CSR_HW_IF_CONFIG_REG_PCI_OWN_SET, CSR_HW_IF_CONFIG_REG_WAKE_ME,
 * CSR_HW_IF_CONFIG_REG_WAKE_ME_PCIE_OWNER_EN and the bits of the other
 * registers), drivers/net/wireless/intel/iwlwifi/pcie/iwl-context-info-v2.h
 * (the self-boot bit) and OpenBSD 0f464d413c50 sys/dev/pci/if_iwxreg.h (the
 * names of the ownership bits).
 *
 * Procedure referenced: Linux v6.17
 * drivers/net/wireless/intel/iwlwifi/pcie/gen1_2/trans.c
 * (iwl_pcie_apm_stop_master) and OpenBSD 0f464d413c50 sys/dev/pci/if_iwx.c
 * (iwx_apm_stop), the wait for the bus master stop.
 */
#define AX211_FUNCTION_HOST_WAKE_FROM_L1A	0x00080000U
#define AX211_FUNCTION_OWNERSHIP_REQUEST	0x00400000U
#define AX211_FUNCTION_PREPARE_REQUEST		0x08000000U
#define AX211_FUNCTION_POWER_EVENT_ENABLE	0x10000000U
#define AX211_HOST_ANNOUNCE_DRIVER_PRESENT	0x00000020U
#define AX211_RESTART_WHOLE_DEVICE		0x00000080U
#define AX211_RESTART_BUS_MASTER_STOPPED	0x00000100U
#define AX211_RESTART_BUS_MASTER_STOP_REQUEST	0x00000200U
#define AX211_LINK_POWER_OVERRIDE_HOLD_AWAKE	0x80000000U
#define AX211_POWER_CLOCK_STABLE		0x00000001U
#define AX211_POWER_ACTIVE_STATE		0x00000004U
#define AX211_POWER_KEEP_AWAKE_REQUEST		0x00000008U
#define AX211_POWER_ENTERING_SLEEP		0x00000010U
#define AX211_LINK_STATE_NO_L0S			0x00000002U
#define AX211_LINK_WORKAROUND_L1A_WAKE		0x00800000U
#define AX211_DMA_WAIT_TUNING_MAXIMUM		0xffff0000U
#define AX211_FUNCTION_SELF_BOOT		0x00000002U

/*
 * The latency tolerance the device reports while it boots.
 *
 * A value of about 250 us is written on a discrete (not integrated)
 * AX210-family device to work around latency problems while the device boots;
 * the firmware replaces it later with a smaller value.  The register has two
 * halves, the no-snoop requirement in bits 31:16 and the snoop requirement in
 * bits 15:0; in each half the top bit says the requirement is valid, a 3-bit
 * scale follows (bits 28:26 and 12:10; scale 2 is units of 1024 ns) and the
 * low 10 bits are the value.  The snoop half written here is valid, scale 2,
 * value 250, about 256 us.  The no-snoop half is written as 0x82fa: valid, and
 * with the scale bit placed at bit 25, which lies in the value field, so by
 * that layout it reads as scale 0 and value 762 (762 ns) rather than the 256
 * us of the snoop half.  The device boots with this value on the test
 * machines; it is recorded here as found, not corrected (ws218-p002).
 *
 * Values referenced: Linux v6.17 drivers/net/wireless/intel/iwlwifi/iwl-csr.h
 * (CSR_LTR_LONG_VAL_AD, the register layout).
 *
 * Procedure referenced: Linux v6.17
 * drivers/net/wireless/intel/iwlwifi/pcie/gen1_2/trans-gen2.c
 * (iwl_pcie_set_ltr).
 */
#define AX211_LATENCY_NO_SNOOP_REQUIRED 0x80000000U
#define AX211_LATENCY_NO_SNOOP_UNIT_MICROSECOND 0x02000000U
#define AX211_LATENCY_NO_SNOOP_250 0x00fa0000U
#define AX211_LATENCY_SNOOP_REQUIRED 0x00008000U
#define AX211_LATENCY_SNOOP_UNIT_MICROSECOND 0x00000800U
#define AX211_LATENCY_SNOOP_250 0x000000faU
#define AX211_LATENCY_BOOT_VALUE                                               \
	(AX211_LATENCY_NO_SNOOP_REQUIRED |                                     \
	 AX211_LATENCY_NO_SNOOP_UNIT_MICROSECOND |                             \
	 AX211_LATENCY_NO_SNOOP_250 | AX211_LATENCY_SNOOP_REQUIRED |           \
	 AX211_LATENCY_SNOOP_UNIT_MICROSECOND | AX211_LATENCY_SNOOP_250)

/*
 * The periphery register that starts the upper CPU, and the bound of the
 * periphery address space.
 *
 * Periphery registers are reached through an indirect window
 * (intel-ax211-pci-mmio.c), and the upper CPU's registers lie at the upper-CPU
 * base (0x300000 on this family) plus their offset.  Writing 1 to
 * UPPER_CPU_START at that base is the last step of publish_gen3; the firmware
 * boot then runs on its own and reports ALIVE.  Periphery addresses are 24
 * bits wide.
 *
 * Values referenced: Linux v6.17 drivers/net/wireless/intel/iwlwifi/iwl-prph.h
 * (the upper CPU start register and the periphery address bound).
 */
#define AX211_PRPH_UPPER_CPU_START 0xa05c44U
#define AX211_PRPH_MAX_ADDRESS 0x00ffffffU

/*
 * The timing of the power and ownership sequences.
 *
 * The device's documentation of these delays is not public.  The ownership
 * handshake is polled every 10 us for 50 us.  The prepare fallback retries 10
 * times, polling every 200 us within one 150 ms budget shared by all retries,
 * with 25 ms between retries.  A whole-device reset needs 5 ms; the MAC clock
 * is awaited for at most 25 ms; a keep-awake request is given 2 us before
 * polling and 150 ms in all; the stop sequence waits 1 ms after its prepare
 * request, 5 ms after releasing the link override and at most 100 us for the
 * bus master stop.
 *
 * Values referenced: Linux v6.17
 * drivers/net/wireless/intel/iwlwifi/pcie/gen1_2/trans.c
 * (iwl_pcie_set_hw_ready, iwl_pcie_prepare_card_hw, iwl_pcie_apm_init,
 * iwl_pcie_apm_stop) and OpenBSD 0f464d413c50 sys/dev/pci/if_iwx.c
 * (iwx_set_hw_ready, iwx_prepare_card_hw, iwx_apm_init, iwx_apm_stop), the
 * delays of the same sequences.  The shared budget of the prepare retries
 * follows iwx_prepare_card_hw.
 */
#define AX211_OWNERSHIP_ACK_TIMEOUT_US 50U
#define AX211_OWNERSHIP_ACK_POLL_US 10U
#define AX211_PREPARE_SETTLE_US 1000U
#define AX211_PREPARE_RETRY_POLL_US 200U
#define AX211_PREPARE_TOTAL_POLL_US 150000U
#define AX211_PREPARE_RETRY_DELAY_US 25000U
#define AX211_PREPARE_RETRY_COUNT 10U
#define AX211_SW_RESET_DELAY_US 5000U
#define AX211_APM_CLOCK_TIMEOUT_US 25000U
#define AX211_NIC_REQUEST_SETTLE_US 2U
#define AX211_NIC_OWNERSHIP_TIMEOUT_US 150000U
#define AX211_STOP_PREPARE_DELAY_US 1000U
#define AX211_STOP_LINK_DELAY_US 5000U
#define AX211_STOP_MASTER_TIMEOUT_US 100U
#define AX211_GENERAL_POLL_US 10U

/* The length of an 802.11 station address in bytes. */
#define AX211_MAC_ADDRESS_SIZE 6U

static int ax211_profile_valid(const struct intel_ax211_mmio_profile *profile);
static int ax211_mmio_valid(const struct intel_ax211_mmio *mmio);
static int ax211_request_ownership(struct intel_ax211_mmio *mmio);
static int ax211_csr_set_bits(struct intel_ax211_mmio *mmio, uint32_t offset, uint32_t bits);
static int ax211_csr_read(struct intel_ax211_mmio *mmio, uint32_t offset, uint32_t *value);
static int ax211_csr_write(struct intel_ax211_mmio *mmio, uint32_t offset, uint32_t value);
static int ax211_poll_csr(struct intel_ax211_mmio *mmio, uint32_t offset, uint32_t expected, uint32_t mask, uint32_t timeout_us, uint32_t step_us, enum intel_ax211_mmio_wait wait);
static int ax211_clock(struct intel_ax211_mmio *mmio, uint64_t *time_us);
static int ax211_delay(struct intel_ax211_mmio *mmio, uint32_t duration_us);
static int ax211_prepare_fallback(struct intel_ax211_mmio *mmio);
static int ax211_csr_clear_bits(struct intel_ax211_mmio *mmio, uint32_t offset, uint32_t bits);
static void ax211_remember_error(int *first_error, int result);
static void ax211_scrub(void *memory, size_t length);
static int ax211_read_mac_words(struct intel_ax211_mmio *mmio, uint32_t first_offset, uint32_t second_offset, uint8_t address[AX211_MAC_ADDRESS_SIZE]);
static void ax211_decode_mac_words(uint32_t first, uint32_t second, uint8_t address[AX211_MAC_ADDRESS_SIZE]);
static int ax211_mac_valid(const uint8_t address[AX211_MAC_ADDRESS_SIZE]);
static int ax211_publish_address(struct intel_ax211_mmio *mmio, uint32_t offset, uint64_t address);

/*
 * Initializes one exact-device MMIO state without touching hardware.
 */
int
drv_intel_ax211_mmio_init(
	struct intel_ax211_mmio *mmio,
	const struct intel_ax211_mmio_ops *ops,
	void *argument,
	const struct intel_ax211_mmio_profile *profile)
{
	int valid;

	/* Refuses missing arguments. */
	if (mmio == NULL || ops == NULL || profile == NULL)
		return INTEL_AX211_MMIO_INVALID;

	/* Refuses a backend without register access. */
	if (ops->csr_read32 == NULL || ops->csr_write32 == NULL)
		return INTEL_AX211_MMIO_INVALID;

	/* Refuses a backend without periphery access. */
	if (ops->prph_read32 == NULL || ops->prph_write32 == NULL)
		return INTEL_AX211_MMIO_INVALID;

	/* Refuses a backend without a delay or a clock. */
	if (ops->delay_us == NULL || ops->clock_us == NULL)
		return INTEL_AX211_MMIO_INVALID;

	/*
	 * Refuses a device other than the one the sequences were established
	 * for.
	 */
	valid = ax211_profile_valid(profile);
	if (!valid)
		return INTEL_AX211_MMIO_INVALID;

	/* Initializes a private, unowned device state. */
	kern_memset(mmio, 0, sizeof(*mmio));
	mmio->ops = ops;
	mmio->argument = argument;
	mmio->profile = *profile;

	/* Succeeded: the state is ready for the ownership handshake. */
	return INTEL_AX211_MMIO_OK;
}

/*
 * Acquires PCI ownership through the bounded AX210-family prepare sequence.
 */
int
drv_intel_ax211_mmio_take_ownership(
	struct intel_ax211_mmio *mmio)
{
	int valid;
	int result;

	/* Refuses an uninitialized state. */
	valid = ax211_mmio_valid(mmio);
	if (!valid)
		return INTEL_AX211_MMIO_INVALID;

	/* Refuses a handshake while the device is held awake. */
	if (mmio->wake_hold_depth != 0U)
		return INTEL_AX211_MMIO_ORDER;

	/* Starts each attempt from an unpublished state. */
	mmio->prepared = 0;
	mmio->reset_done = 0;
	mmio->apm_ready = 0;
	mmio->master_disable_timed_out = 0;

	/* Tries the ordinary ownership handshake first. */
	result = ax211_request_ownership(mmio);
	if (result == INTEL_AX211_MMIO_OK) {
		mmio->prepared = 1;
		return INTEL_AX211_MMIO_OK;
	}

	/* Reports any failure other than a timeout. */
	if (result != INTEL_AX211_MMIO_TIMEOUT)
		return result;

	/* Runs the finite wake-and-retry fallback. */
	result = ax211_prepare_fallback(mmio);
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Succeeded: the driver owns the device. */
	mmio->prepared = 1;
	return INTEL_AX211_MMIO_OK;
}

/*
 * Applies the AX210-family software reset and its required five-ms delay.
 */
int
drv_intel_ax211_mmio_sw_reset(
	struct intel_ax211_mmio *mmio)
{
	int valid;
	int result;

	/* Refuses an uninitialized state. */
	valid = ax211_mmio_valid(mmio);
	if (!valid)
		return INTEL_AX211_MMIO_INVALID;

	/*
	 * Refuses a reset before ownership or while the device is held awake.
	 */
	if (!mmio->prepared || mmio->wake_hold_depth != 0U)
		return INTEL_AX211_MMIO_ORDER;

	/* Invalidates every post-reset readiness assertion. */
	mmio->reset_done = 0;
	mmio->apm_ready = 0;

	/* Requests the AX210-family CSR reset. */
	result = ax211_csr_set_bits(mmio, AX211_CSR_RESTART_CONTROL, AX211_RESTART_WHOLE_DEVICE);
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Observes the documented reset settling time. */
	result = ax211_delay(mmio, AX211_SW_RESET_DELAY_US);
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Succeeded: the device is reset. */
	mmio->reset_done = 1;
	return INTEL_AX211_MMIO_OK;
}

/*
 * Powers the MAC clock through the bounded AX210-family APM sequence.
 */
int
drv_intel_ax211_mmio_apm_init(
	struct intel_ax211_mmio *mmio)
{
	int valid;
	int result;

	/* Refuses an uninitialized state. */
	valid = ax211_mmio_valid(mmio);
	if (!valid)
		return INTEL_AX211_MMIO_INVALID;

	/* Refuses a power-up before ownership and reset. */
	if (!mmio->prepared || !mmio->reset_done)
		return INTEL_AX211_MMIO_ORDER;

	/* Refuses a power-up while the device is held awake. */
	if (mmio->wake_hold_depth != 0U)
		return INTEL_AX211_MMIO_ORDER;

	/* Keeps L0s from racing L1A receive wakeup. */
	mmio->apm_ready = 0;
	result = ax211_csr_set_bits(mmio,
				    AX211_CSR_LINK_WORKAROUNDS,
				    AX211_LINK_WORKAROUND_L1A_WAKE);
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Raises the DMA engine (FH) wait threshold to its maximum. */
	result = ax211_csr_set_bits(mmio, AX211_CSR_DMA_WAIT_TUNING,
				    AX211_DMA_WAIT_TUNING_MAXIMUM);
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Permits management traffic to wake the PCIe link. */
	result = ax211_csr_set_bits(mmio,
				    AX211_CSR_FUNCTION_CONTROL,
				    AX211_FUNCTION_HOST_WAKE_FROM_L1A);
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Disables unsupported L0s without disabling L1. */
	result = ax211_csr_set_bits(mmio,
				    AX211_CSR_LINK_STATE_CONTROL,
				    AX211_LINK_STATE_NO_L0S);
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Moves the adapter from D0U into its active power state. */
	result = ax211_csr_set_bits(mmio,
				    AX211_CSR_POWER_CONTROL,
				    AX211_POWER_ACTIVE_STATE);
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Waits only for the clock indication required by PRPH access. */
	result = ax211_poll_csr(mmio,
				AX211_CSR_POWER_CONTROL,
				AX211_POWER_CLOCK_STABLE,
				AX211_POWER_CLOCK_STABLE,
				AX211_APM_CLOCK_TIMEOUT_US,
				AX211_GENERAL_POLL_US,
				INTEL_AX211_MMIO_WAIT_APM_CLOCK);
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Succeeded: the MAC clock runs. */
	mmio->apm_ready = 1;
	return INTEL_AX211_MMIO_OK;
}

/*
 * Quiesces AX210-family bus-master activity and finishes with a software reset.
 *
 * Every stage runs even after an earlier one failed; the first failure is
 * reported.
 */
int
drv_intel_ax211_mmio_stop(
	struct intel_ax211_mmio *mmio)
{
	int first_error;
	int valid;
	int result;

	/* Refuses an uninitialized state. */
	valid = ax211_mmio_valid(mmio);
	if (!valid)
		return INTEL_AX211_MMIO_INVALID;

	/* Makes every failure fail closed to later MMIO/PRPH users. */
	mmio->wake_hold_depth = 0U;
	mmio->prepared = 0;
	mmio->reset_done = 0;
	mmio->apm_ready = 0;
	first_error = INTEL_AX211_MMIO_OK;

	/* Releases a redundant or leaked request to keep the MAC awake. */
	result = ax211_csr_clear_bits(mmio, AX211_CSR_POWER_CONTROL,
				      AX211_POWER_KEEP_AWAKE_REQUEST);
	ax211_remember_error(&first_error, result);

	/*
	 * Holds the link awake and asks the device to prepare for the stop, the
	 * link-power preamble of a device older than the BZ family.
	 *
	 * Procedure referenced: OpenBSD 0f464d413c50 sys/dev/pci/if_iwx.c
	 * (iwx_apm_stop).
	 */
	result = ax211_csr_set_bits(mmio,
				    AX211_CSR_LINK_POWER_OVERRIDE,
				    AX211_LINK_POWER_OVERRIDE_HOLD_AWAKE);
	ax211_remember_error(&first_error, result);
	result = ax211_csr_set_bits(mmio,
				    AX211_CSR_FUNCTION_CONTROL,
				    AX211_FUNCTION_PREPARE_REQUEST | AX211_FUNCTION_POWER_EVENT_ENABLE);
	ax211_remember_error(&first_error, result);
	result = ax211_delay(mmio, AX211_STOP_PREPARE_DELAY_US);
	ax211_remember_error(&first_error, result);
	result = ax211_csr_clear_bits(mmio,
				      AX211_CSR_LINK_POWER_OVERRIDE,
				      AX211_LINK_POWER_OVERRIDE_HOLD_AWAKE);
	ax211_remember_error(&first_error, result);
	result = ax211_delay(mmio, AX211_STOP_LINK_DELAY_US);
	ax211_remember_error(&first_error, result);

	/* Stops bus-master DMA and exposes the finite 100-us deadline. */
	result = ax211_csr_set_bits(mmio,
				    AX211_CSR_RESTART_CONTROL,
				    AX211_RESTART_BUS_MASTER_STOP_REQUEST);
	ax211_remember_error(&first_error, result);
	result = ax211_poll_csr(mmio,
				AX211_CSR_RESTART_CONTROL,
				AX211_RESTART_BUS_MASTER_STOPPED,
				AX211_RESTART_BUS_MASTER_STOPPED,
				AX211_STOP_MASTER_TIMEOUT_US,
				AX211_GENERAL_POLL_US,
				INTEL_AX211_MMIO_WAIT_MASTER_DISABLED);

	/*
	 * A missed 100-us indication deadline is only a warning, and the
	 * mandatory software reset continues.  The PCI owner has already
	 * disabled bus mastering before a DMA-owning stop reaches here; keep
	 * all actual MMIO, clock, and reset failures fatal.
	 *
	 * Procedure referenced: Linux v6.17
	 * drivers/net/wireless/intel/iwlwifi/pcie/gen1_2/trans.c
	 * (iwl_pcie_apm_stop_master) and OpenBSD 0f464d413c50
	 * sys/dev/pci/if_iwx.c (iwx_apm_stop).
	 */
	if (result == INTEL_AX211_MMIO_TIMEOUT)
		mmio->master_disable_timed_out = 1;
	else
		ax211_remember_error(&first_error, result);

	/* Returns the adapter to D0U even when an earlier stop stage failed. */
	result = ax211_csr_clear_bits(mmio,
				      AX211_CSR_POWER_CONTROL,
				      AX211_POWER_ACTIVE_STATE);
	ax211_remember_error(&first_error, result);

	/*
	 * Invalidates the stopped firmware generation with the required reset.
	 */
	result = ax211_csr_set_bits(mmio, AX211_CSR_RESTART_CONTROL, AX211_RESTART_WHOLE_DEVICE);
	ax211_remember_error(&first_error, result);
	result = ax211_delay(mmio, AX211_SW_RESET_DELAY_US);
	ax211_remember_error(&first_error, result);

	/* Reports the first stage that failed. */
	if (first_error != INTEL_AX211_MMIO_OK)
		return first_error;

	/* Succeeded: the device is stopped and reset. */
	return INTEL_AX211_MMIO_OK;
}

/*
 * Acquires nested NIC ownership with a 150-ms outer bound.
 */
int
drv_intel_ax211_mmio_wake_hold(
	struct intel_ax211_mmio *mmio)
{
	int clear_result;
	int valid;
	int result;

	/* Refuses an uninitialized state. */
	valid = ax211_mmio_valid(mmio);
	if (!valid)
		return INTEL_AX211_MMIO_INVALID;

	/* Refuses a hold before the MAC clock runs. */
	if (!mmio->apm_ready)
		return INTEL_AX211_MMIO_ORDER;

	/* Reuses an ownership scope already held by this state. */
	if (mmio->wake_hold_depth != 0U) {
		/* Refuses a depth that would wrap around. */
		if (mmio->wake_hold_depth == (unsigned int)-1)
			return INTEL_AX211_MMIO_INVALID;

		/* Nests the hold. */
		mmio->wake_hold_depth++;
		return INTEL_AX211_MMIO_OK;
	}

	/* Requests access. */
	result = ax211_csr_set_bits(mmio,
				    AX211_CSR_POWER_CONTROL,
				    AX211_POWER_KEEP_AWAKE_REQUEST);
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/*
	 * Gives the power controller time to react; a failure withdraws the
	 * request.
	 */
	result = ax211_delay(mmio, AX211_NIC_REQUEST_SETTLE_US);
	if (result != INTEL_AX211_MMIO_OK) {
		/* A failed withdrawal leaves the clock state unknown. */
		clear_result = ax211_csr_clear_bits(mmio,
						    AX211_CSR_POWER_CONTROL,
						    AX211_POWER_KEEP_AWAKE_REQUEST);
		if (clear_result != INTEL_AX211_MMIO_OK) {
			mmio->apm_ready = 0;
			return clear_result;
		}

		/* Reports the failed delay. */
		return result;
	}

	/* Requires a running clock and a device which is not going to sleep. */
	result = ax211_poll_csr(mmio,
				AX211_CSR_POWER_CONTROL,
				AX211_POWER_CLOCK_STABLE,
				AX211_POWER_CLOCK_STABLE | AX211_POWER_ENTERING_SLEEP,
				AX211_NIC_OWNERSHIP_TIMEOUT_US,
				AX211_GENERAL_POLL_US,
				INTEL_AX211_MMIO_WAIT_NIC_OWNERSHIP);
	if (result != INTEL_AX211_MMIO_OK) {
		/* A failed withdrawal leaves the clock state unknown. */
		clear_result = ax211_csr_clear_bits(mmio,
						    AX211_CSR_POWER_CONTROL,
						    AX211_POWER_KEEP_AWAKE_REQUEST);
		if (clear_result != INTEL_AX211_MMIO_OK) {
			mmio->apm_ready = 0;
			return clear_result;
		}

		/* Reports the failed wait. */
		return result;
	}

	/* Succeeded: the device stays awake until the hold is released. */
	mmio->wake_hold_depth = 1U;
	return INTEL_AX211_MMIO_OK;
}

/*
 * Releases one nested NIC ownership reference.
 */
int
drv_intel_ax211_mmio_wake_release(
	struct intel_ax211_mmio *mmio)
{
	int valid;
	int result;

	/* Refuses an uninitialized state. */
	valid = ax211_mmio_valid(mmio);
	if (!valid)
		return INTEL_AX211_MMIO_INVALID;

	/* Refuses a release without a hold. */
	if (mmio->wake_hold_depth == 0U)
		return INTEL_AX211_MMIO_NOT_OWNER;

	/* Retains hardware ownership for an outer caller. */
	mmio->wake_hold_depth--;
	if (mmio->wake_hold_depth != 0U)
		return INTEL_AX211_MMIO_OK;

	/* Drops the hardware request at the outermost boundary. */
	result = ax211_csr_clear_bits(mmio,
				      AX211_CSR_POWER_CONTROL,
				      AX211_POWER_KEEP_AWAKE_REQUEST);
	if (result != INTEL_AX211_MMIO_OK) {
		mmio->apm_ready = 0;
		return result;
	}

	/* Succeeded: the device may sleep again. */
	return INTEL_AX211_MMIO_OK;
}

/*
 * Reads the fused station address transactionally under NIC ownership.
 */
int
drv_intel_ax211_mmio_read_mac(
	struct intel_ax211_mmio *mmio,
	uint8_t mac_address[AX211_MAC_ADDRESS_SIZE])
{
	uint8_t candidate[AX211_MAC_ADDRESS_SIZE];
	int result;
	int unlock_result;
	int valid;

	/* Refuses an uninitialized state. */
	valid = ax211_mmio_valid(mmio);
	if (!valid)
		return INTEL_AX211_MMIO_INVALID;

	/* Refuses a missing output. */
	if (mac_address == NULL)
		return INTEL_AX211_MMIO_INVALID;

	/* Holds the device awake for the reads. */
	kern_memset(candidate, 0, sizeof(candidate));
	result = drv_intel_ax211_mmio_wake_hold(mmio);
	if (result != INTEL_AX211_MMIO_OK) {
		ax211_scrub(candidate, sizeof(candidate));
		return result;
	}

	/* Prefers the OEM strap value when it is a usable unicast address. */
	result = ax211_read_mac_words(mmio,
				      AX211_CSR_STATION_ADDRESS_STRAP_FIRST,
				      AX211_CSR_STATION_ADDRESS_STRAP_SECOND,
				      candidate);

	/* Falls back to the fused value when the strap value is not usable. */
	if (result == INTEL_AX211_MMIO_OK) {
		/* Reads the fused words in place of an unusable strap value. */
		valid = ax211_mac_valid(candidate);
		if (!valid) {
			ax211_scrub(candidate, sizeof(candidate));
			result = ax211_read_mac_words(mmio,
						      AX211_CSR_STATION_ADDRESS_FUSE_FIRST,
						      AX211_CSR_STATION_ADDRESS_FUSE_SECOND,
						      candidate);
		}
	}

	/* Refuses a fused value that is not usable either. */
	if (result == INTEL_AX211_MMIO_OK) {
		/* An unusable address is invalid. */
		valid = ax211_mac_valid(candidate);
		if (!valid)
			result = INTEL_AX211_MMIO_INVALID;
	}

	/*
	 * Never publishes an address until its ownership reference is released;
	 * a failed release replaces any other result.
	 */
	unlock_result = drv_intel_ax211_mmio_wake_release(mmio);
	if (unlock_result != INTEL_AX211_MMIO_OK)
		result = unlock_result;

	/* Publishes the address on success and clears the temporary. */
	if (result == INTEL_AX211_MMIO_OK)
		kern_memcpy(mac_address, candidate, sizeof(candidate));
	ax211_scrub(candidate, sizeof(candidate));

	/* Reports why no address was published. */
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Succeeded: the station address is published. */
	return INTEL_AX211_MMIO_OK;
}

/*
 * Reads one AX210-family PRPH register while NIC ownership is held.
 */
int
drv_intel_ax211_mmio_prph_read32(
	struct intel_ax211_mmio *mmio,
	uint32_t address,
	uint32_t *value)
{
	int valid;
	int error;

	/* Refuses an uninitialized state. */
	valid = ax211_mmio_valid(mmio);
	if (!valid)
		return INTEL_AX211_MMIO_INVALID;

	/* Refuses a missing output. */
	if (value == NULL)
		return INTEL_AX211_MMIO_INVALID;

	/* Refuses an access while the device is not held awake. */
	if (mmio->wake_hold_depth == 0U)
		return INTEL_AX211_MMIO_NOT_OWNER;

	/* Refuses an address beyond the periphery space. */
	if (address > AX211_PRPH_MAX_ADDRESS)
		return INTEL_AX211_MMIO_INVALID;

	/* Reads the register through the backend. */
	error = mmio->ops->prph_read32(mmio->argument, address, value);
	if (error != 0)
		return INTEL_AX211_MMIO_IO;

	/* Succeeded. */
	return INTEL_AX211_MMIO_OK;
}

/*
 * Writes one AX210-family PRPH register while NIC ownership is held.
 */
int
drv_intel_ax211_mmio_prph_write32(
	struct intel_ax211_mmio *mmio,
	uint32_t address,
	uint32_t value)
{
	int valid;
	int error;

	/* Refuses an uninitialized state. */
	valid = ax211_mmio_valid(mmio);
	if (!valid)
		return INTEL_AX211_MMIO_INVALID;

	/* Refuses an access while the device is not held awake. */
	if (mmio->wake_hold_depth == 0U)
		return INTEL_AX211_MMIO_NOT_OWNER;

	/* Refuses an address beyond the periphery space. */
	if (address > AX211_PRPH_MAX_ADDRESS)
		return INTEL_AX211_MMIO_INVALID;

	/* Writes the register through the backend. */
	error = mmio->ops->prph_write32(mmio->argument, address, value);
	if (error != 0)
		return INTEL_AX211_MMIO_IO;

	/* Succeeded. */
	return INTEL_AX211_MMIO_OK;
}

/*
 * Publishes preallocated Gen3 context and IML addresses, then starts UMAC.
 */
int
drv_intel_ax211_mmio_publish_gen3(
	struct intel_ax211_mmio *mmio,
	const struct intel_ax211_mmio_boot *boot)
{
	uint32_t boot_control;
	int result;
	int unlock_result;
	int valid;

	/* Refuses an uninitialized state. */
	valid = ax211_mmio_valid(mmio);
	if (!valid)
		return INTEL_AX211_MMIO_INVALID;

	/* Refuses missing boot inputs. */
	if (boot == NULL)
		return INTEL_AX211_MMIO_INVALID;

	/* Refuses a start before power-up or while the device is held awake. */
	if (!mmio->apm_ready || mmio->wake_hold_depth != 0U)
		return INTEL_AX211_MMIO_ORDER;

	/* Refuses unset addresses. */
	if (boot->context_address == 0U || boot->iml_address == 0U)
		return INTEL_AX211_MMIO_INVALID;

	/* Refuses a loader of another size. */
	if (boot->iml_size != INTEL_AX211_MMIO_LOADER_IMAGE_SIZE)
		return INTEL_AX211_MMIO_INVALID;

	/*
	 * Publishes the context physical address low word before its high word.
	 */
	result = ax211_publish_address(mmio,
				       AX211_CSR_BOOT_CONTEXT_ADDRESS,
				       boot->context_address);
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Publishes the IML physical address before its byte length. */
	result = ax211_publish_address(mmio,
				       AX211_CSR_LOADER_IMAGE_ADDRESS,
				       boot->iml_address);
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Publishes the loader's length. */
	result = ax211_csr_write(mmio, AX211_CSR_LOADER_IMAGE_LENGTH, boot->iml_size);
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/*
	 * Enables automatic function boot only after every boot input is
	 * visible.
	 */
	result = ax211_csr_read(mmio, AX211_CSR_FUNCTION_CONTROL, &boot_control);
	if (result != INTEL_AX211_MMIO_OK)
		return result;
	boot_control |= AX211_FUNCTION_SELF_BOOT;
	result = ax211_csr_write(mmio, AX211_CSR_FUNCTION_CONTROL, boot_control);
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Acquires ownership before the LTR bootstrap and UMAC PRPH write. */
	result = drv_intel_ax211_mmio_wake_hold(mmio);
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Applies the non-integrated SO/GF bootstrap latency contract. */
	result = ax211_csr_write(mmio, AX211_CSR_LATENCY_TOLERANCE,
				 AX211_LATENCY_BOOT_VALUE);
	if (result != INTEL_AX211_MMIO_OK) {
		(void)drv_intel_ax211_mmio_wake_release(mmio);
		return result;
	}

	/* Starts UMAC through the supplied, validated PRPH aperture offset. */
	result = drv_intel_ax211_mmio_prph_write32(mmio,
						   mmio->profile.upper_cpu_prph_base + AX211_PRPH_UPPER_CPU_START,
						   1U);
	unlock_result = drv_intel_ax211_mmio_wake_release(mmio);
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Reports a failed release of the hold. */
	if (unlock_result != INTEL_AX211_MMIO_OK)
		return unlock_result;

	/* Succeeded: the firmware boots on its own from here. */
	return INTEL_AX211_MMIO_OK;
}

/*
 * Reports whether a profile is the So or SoF MAC with a single-band Garfield
 * Peak radio the transport was established for: 1 when it is, 0 when not.
 */
static int
ax211_profile_valid(
	const struct intel_ax211_mmio_profile *profile)
{
	/* Rejects a missing profile. */
	if (profile == NULL)
		return 0;

	/* Rejects another MAC type. */
	if (profile->mac_type != INTEL_AX211_MMIO_MAC_SO &&
	    profile->mac_type != INTEL_AX211_MMIO_MAC_SOF) {
		return 0;
	}

	/* Rejects another radio. */
	if (profile->rf_type != INTEL_AX211_MMIO_RF_GF)
		return 0;

	/*
	 * Rejects a radio that runs two bands at once and an integrated part.
	 */
	if (profile->concurrent_bands != 0U || profile->integrated != 0U)
		return 0;

	/* Rejects another periphery base of the upper CPU. */
	if (profile->upper_cpu_prph_base != INTEL_AX211_MMIO_UPPER_CPU_PRPH_BASE)
		return 0;

	/* The profile is the expected one. */
	return 1;
}

/*
 * Reports whether a state was initialized through the exact profile gate: 1
 * when it was, 0 when not.
 */
static int
ax211_mmio_valid(
	const struct intel_ax211_mmio *mmio)
{
	int valid;

	/* Rejects a missing state and missing operations. */
	if (mmio == NULL || mmio->ops == NULL)
		return 0;

	/* Rejects a backend without register access. */
	if (mmio->ops->csr_read32 == NULL || mmio->ops->csr_write32 == NULL)
		return 0;

	/* Rejects a backend without periphery access. */
	if (mmio->ops->prph_read32 == NULL || mmio->ops->prph_write32 == NULL)
		return 0;

	/* Rejects a backend without a delay or a clock. */
	if (mmio->ops->delay_us == NULL || mmio->ops->clock_us == NULL)
		return 0;

	/* Rejects a profile other than the expected one. */
	valid = ax211_profile_valid(&mmio->profile);
	if (!valid)
		return 0;

	/* The state is usable. */
	return 1;
}

/* Requests NIC readiness and announces OS_ALIVE only after acceptance. */
static int
ax211_request_ownership(
	struct intel_ax211_mmio *mmio)
{
	int result;

	/* Requests ownership. */
	result = ax211_csr_set_bits(mmio, AX211_CSR_FUNCTION_CONTROL,
				    AX211_FUNCTION_OWNERSHIP_REQUEST);
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Waits for the device to keep the request bit set. */
	result = ax211_poll_csr(
		mmio, AX211_CSR_FUNCTION_CONTROL, AX211_FUNCTION_OWNERSHIP_REQUEST,
		AX211_FUNCTION_OWNERSHIP_REQUEST, AX211_OWNERSHIP_ACK_TIMEOUT_US,
		AX211_OWNERSHIP_ACK_POLL_US, INTEL_AX211_MMIO_WAIT_HW_READY);
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Announces the driver. */
	result = ax211_csr_write(mmio, AX211_CSR_HOST_ANNOUNCE,
				 AX211_HOST_ANNOUNCE_DRIVER_PRESENT);
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Succeeded: the driver owns the device. */
	return INTEL_AX211_MMIO_OK;
}

/* Sets CSR bits through a checked read-modify-write operation. */
static int
ax211_csr_set_bits(
	struct intel_ax211_mmio *mmio,
	uint32_t offset,
	uint32_t bits)
{
	uint32_t value;
	int result;

	/* Reads the register. */
	result = ax211_csr_read(mmio, offset, &value);
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Writes it back with the bits set. */
	value |= bits;
	result = ax211_csr_write(mmio, offset, value);
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Succeeded. */
	return INTEL_AX211_MMIO_OK;
}

/* Reads one CSR and converts a backend failure into a private result. */
static int
ax211_csr_read(
	struct intel_ax211_mmio *mmio,
	uint32_t offset,
	uint32_t *value)
{
	int error;

	/* Reads through the backend. */
	error = mmio->ops->csr_read32(mmio->argument, offset, value);
	if (error != 0)
		return INTEL_AX211_MMIO_IO;

	/* Succeeded. */
	return INTEL_AX211_MMIO_OK;
}

/* Writes one CSR and converts a backend failure into a private result. */
static int
ax211_csr_write(
	struct intel_ax211_mmio *mmio,
	uint32_t offset,
	uint32_t value)
{
	int error;

	/* Writes through the backend. */
	error = mmio->ops->csr_write32(mmio->argument, offset, value);
	if (error != 0)
		return INTEL_AX211_MMIO_IO;

	/* Succeeded. */
	return INTEL_AX211_MMIO_OK;
}

/* Polls one CSR against an observable finite deadline. */
static int
ax211_poll_csr(
	struct intel_ax211_mmio *mmio,
	uint32_t offset,
	uint32_t expected,
	uint32_t mask,
	uint32_t timeout_us,
	uint32_t step_us,
	enum intel_ax211_mmio_wait wait)
{
	uint64_t start;
	uint64_t deadline;
	uint64_t previous;
	uint64_t current;
	uint32_t value;
	uint32_t iteration;
	uint32_t maximum_iterations;
	int result;

	/* Refuses a zero step or timeout. */
	if (step_us == 0U || timeout_us == 0U)
		return INTEL_AX211_MMIO_INVALID;

	/* Creates a checked absolute deadline. */
	result = ax211_clock(mmio, &start);
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Refuses a deadline that overflows the clock. */
	if ((uint64_t)timeout_us > UINT64_MAX - start)
		return INTEL_AX211_MMIO_CLOCK;

	/* Lets the backend trace the wait. */
	deadline = start + (uint64_t)timeout_us;
	if (mmio->ops->trace_deadline != NULL) {
		mmio->ops->trace_deadline(mmio->argument,
					  wait,
					  start,
					  deadline);
	}

	/* Polls with both absolute-time and iteration bounds. */
	previous = start;
	maximum_iterations = timeout_us / step_us + 2U;
	for (iteration = 0U; iteration < maximum_iterations; iteration++) {
		/* Reads the register. */
		result = ax211_csr_read(mmio, offset, &value);
		if (result != INTEL_AX211_MMIO_OK)
			return result;

		/* The masked bits have the expected value. */
		if ((value & mask) == (expected & mask))
			return INTEL_AX211_MMIO_OK;

		/* Reads the clock. */
		result = ax211_clock(mmio, &current);
		if (result != INTEL_AX211_MMIO_OK)
			return result;

		/* Refuses a clock that went back. */
		if (current < previous)
			return INTEL_AX211_MMIO_CLOCK;

		/* The deadline has passed. */
		if (current >= deadline)
			return INTEL_AX211_MMIO_TIMEOUT;

		/* Waits one step. */
		result = ax211_delay(mmio, step_us);
		if (result != INTEL_AX211_MMIO_OK)
			return result;

		/* Reads the clock again. */
		result = ax211_clock(mmio, &current);
		if (result != INTEL_AX211_MMIO_OK)
			return result;

		/* Refuses a clock that did not advance over the step. */
		if (current <= previous)
			return INTEL_AX211_MMIO_CLOCK;

		/* Remembers the time for the next step. */
		previous = current;
	}

	/* The iteration bound ran out. */
	return INTEL_AX211_MMIO_TIMEOUT;
}

/* Reads the monotonic backend clock. */
static int
ax211_clock(
	struct intel_ax211_mmio *mmio,
	uint64_t *time_us)
{
	int error;

	/* Reads through the backend. */
	error = mmio->ops->clock_us(mmio->argument, time_us);
	if (error != 0)
		return INTEL_AX211_MMIO_CLOCK;

	/* Succeeded. */
	return INTEL_AX211_MMIO_OK;
}

/* Delays through the checked backend clock source. */
static int
ax211_delay(
	struct intel_ax211_mmio *mmio,
	uint32_t duration_us)
{
	int error;

	/* Waits through the backend. */
	error = mmio->ops->delay_us(mmio->argument, duration_us);
	if (error != 0)
		return INTEL_AX211_MMIO_IO;

	/* Succeeded. */
	return INTEL_AX211_MMIO_OK;
}

/*
 * Runs the finite link-power fallback of the ownership request.
 *
 * Procedure referenced: OpenBSD 0f464d413c50 sys/dev/pci/if_iwx.c
 * (iwx_prepare_card_hw).
 */
static int
ax211_prepare_fallback(
	struct intel_ax211_mmio *mmio)
{
	uint32_t retry;
	uint32_t polled_us;
	int result;

	/* Keeps the PCIe link awake while requesting ownership. */
	result = ax211_csr_set_bits(mmio,
				    AX211_CSR_LINK_POWER_OVERRIDE,
				    AX211_LINK_POWER_OVERRIDE_HOLD_AWAKE);
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Lets the link settle. */
	result = ax211_delay(mmio, AX211_PREPARE_SETTLE_US);
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Retries within one cumulative 150-ms polling budget. */
	polled_us = 0U;
	for (retry = 0U; retry < AX211_PREPARE_RETRY_COUNT; retry++) {
		/* Asks the device to prepare. */
		result = ax211_csr_set_bits(mmio,
					    AX211_CSR_FUNCTION_CONTROL,
					    AX211_FUNCTION_PREPARE_REQUEST);
		if (result != INTEL_AX211_MMIO_OK)
			return result;

		/* Rechecks readiness at 200-us fallback intervals. */
		do {
			/* Tries the handshake; a timeout keeps polling. */
			result = ax211_request_ownership(mmio);
			if (result == INTEL_AX211_MMIO_OK)
				return INTEL_AX211_MMIO_OK;
			if (result != INTEL_AX211_MMIO_TIMEOUT)
				return result;

			/* Waits one interval of the budget. */
			result = ax211_delay(mmio, AX211_PREPARE_RETRY_POLL_US);
			if (result != INTEL_AX211_MMIO_OK)
				return result;

			/* Counts the interval against the budget. */
			polled_us += AX211_PREPARE_RETRY_POLL_US;
		} while (polled_us < AX211_PREPARE_TOTAL_POLL_US);

		/* Separates hardware-prepare attempts by 25 ms. */
		result = ax211_delay(mmio, AX211_PREPARE_RETRY_DELAY_US);
		if (result != INTEL_AX211_MMIO_OK)
			return result;
	}

	/* Every attempt timed out. */
	return INTEL_AX211_MMIO_TIMEOUT;
}

/* Clears CSR bits through a checked read-modify-write operation. */
static int
ax211_csr_clear_bits(
	struct intel_ax211_mmio *mmio,
	uint32_t offset,
	uint32_t bits)
{
	uint32_t value;
	int result;

	/* Reads the register. */
	result = ax211_csr_read(mmio, offset, &value);
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Writes it back with the bits cleared. */
	value &= ~bits;
	result = ax211_csr_write(mmio, offset, value);
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Succeeded. */
	return INTEL_AX211_MMIO_OK;
}

/* Retains the first stop failure while later cleanup stages still run. */
static void
ax211_remember_error(
	int *first_error,
	int result)
{
	/* Keeps only the first failure. */
	if (*first_error == INTEL_AX211_MMIO_OK &&
	    result != INTEL_AX211_MMIO_OK)
		*first_error = result;
}

/* Erases private identity temporaries through an observable byte loop. */
static void
ax211_scrub(
	void *memory,
	size_t length)
{
	volatile uint8_t *byte;

	/* Clears every byte through a volatile pointer. */
	byte = memory;
	while (length != 0U) {
		*byte++ = 0U;
		length--;
	}
}

/* Reads and decodes one two-word AX210-family MAC-address source. */
static int
ax211_read_mac_words(
	struct intel_ax211_mmio *mmio,
	uint32_t first_offset,
	uint32_t second_offset,
	uint8_t address[AX211_MAC_ADDRESS_SIZE])
{
	uint32_t first;
	uint32_t second;
	int result;

	/* Reads both words; the address is decoded only when both were read. */
	first = 0U;
	second = 0U;
	result = ax211_csr_read(mmio, first_offset, &first);
	if (result == INTEL_AX211_MMIO_OK)
		result = ax211_csr_read(mmio, second_offset, &second);
	if (result == INTEL_AX211_MMIO_OK)
		ax211_decode_mac_words(first, second, address);

	/* Clears the raw words. */
	ax211_scrub(&first, sizeof(first));
	ax211_scrub(&second, sizeof(second));

	/* Reports a failed read. */
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Succeeded: the address is decoded. */
	return INTEL_AX211_MMIO_OK;
}

/*
 * Decodes the two station address words without relying on host endianness.
 *
 * Procedure referenced: OpenBSD 0f464d413c50 sys/dev/pci/if_iwx.c
 * (iwx_flip_hw_address).
 */
static void
ax211_decode_mac_words(
	uint32_t first,
	uint32_t second,
	uint8_t address[AX211_MAC_ADDRESS_SIZE])
{
	address[0] = (uint8_t)(first >> 24);
	address[1] = (uint8_t)(first >> 16);
	address[2] = (uint8_t)(first >> 8);
	address[3] = (uint8_t)first;
	address[4] = (uint8_t)(second >> 8);
	address[5] = (uint8_t)second;
}

/*
 * Rejects the reserved placeholder address and the zero, broadcast and
 * multicast values.
 *
 * Value referenced: OpenBSD 0f464d413c50 sys/dev/pci/if_iwx.c
 * (iwx_is_valid_mac_addr, the reserved address).
 */
static int
ax211_mac_valid(
	const uint8_t address[AX211_MAC_ADDRESS_SIZE])
{
	/* The addresses a station address must not be. */
	static const uint8_t reserved[AX211_MAC_ADDRESS_SIZE] = {
		0x02U, 0xccU, 0xaaU, 0xffU, 0xeeU, 0x00U};
	static const uint8_t zero[AX211_MAC_ADDRESS_SIZE] = {0U, 0U, 0U,
							     0U, 0U, 0U};
	static const uint8_t broadcast[AX211_MAC_ADDRESS_SIZE] = {
		0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU};
	int compared;

	/* Rejects a group address. */
	if ((address[0] & 1U) != 0U)
		return 0;

	/* Rejects the reserved placeholder. */
	compared = kern_memcmp(address, reserved, sizeof(reserved));
	if (compared == 0)
		return 0;

	/* Rejects the zero address. */
	compared = kern_memcmp(address, zero, sizeof(zero));
	if (compared == 0)
		return 0;

	/* Rejects the broadcast address. */
	compared = kern_memcmp(address, broadcast, sizeof(broadcast));
	if (compared == 0)
		return 0;

	/* The address is usable. */
	return 1;
}

/* Writes a 64-bit DMA address as ordered low and high CSR words. */
static int
ax211_publish_address(
	struct intel_ax211_mmio *mmio,
	uint32_t offset,
	uint64_t address)
{
	int result;

	/* Writes the low word. */
	result = ax211_csr_write(mmio, offset, (uint32_t)address);
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Writes the high word. */
	result = ax211_csr_write(mmio, offset + 4U, (uint32_t)(address >> 32));
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Succeeded. */
	return INTEL_AX211_MMIO_OK;
}
