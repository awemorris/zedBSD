/*
 * TP-Link Archer T3U Nano RTL8822BU USB WLAN substrate
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include <drivers/usb-rtl8822bu.h>
#include <drivers/usb.h>
#include <errno.h>
#include <hal/hal.h>
#include <kern/clock.h>
#include <kern/lock.h>
#include <kern/net/net-device.h>
#include <kern/net/packet-buf.h>
#include <kern/net/wlan.h>
#include <kern/sched.h>
#include <string.h>

#include "rtl8822b-internal.h"

/* The host fixture substitutes an immutable in-memory lease.  Production
 * always uses the fixed-path VFS loader and the validated core walker. */
#ifndef RTL8822BU_FIRMWARE_LOAD
#define RTL8822BU_FIRMWARE_LOAD rtl8822b_firmware_load
#endif
#ifndef RTL8822BU_FIRMWARE_RELEASE
#define RTL8822BU_FIRMWARE_RELEASE rtl8822b_firmware_release
#endif
#ifndef RTL8822BU_FIRMWARE_WALK
#define RTL8822BU_FIRMWARE_WALK rtl8822b_firmware_walk
#endif

#define RTL8822BU_VENDOR_ID                         0x2357U
#define RTL8822BU_PRODUCT_ID                        0x012eU
#define RTL8822BU_DEVICE_RELEASE                    0x0210U
#define RTL8822BU_USB_RELEASE                       0x0210U
#define RTL8822BU_INTERFACE_CLASS                     0xffU
#define RTL8822BU_INTERFACE_SUBCLASS                  0xffU
#define RTL8822BU_INTERFACE_PROTOCOL                  0xffU

#define RTL8822BU_BULK_IN_ADDRESS                     0x84U
#define RTL8822BU_BULK_OUT_HIGH_ADDRESS               0x05U
#define RTL8822BU_BULK_OUT_NORMAL_ADDRESS             0x06U
#define RTL8822BU_BULK_OUT_LOW_ADDRESS                0x08U
#define RTL8822BU_INTERRUPT_IN_ADDRESS                 0x87U
#define RTL8822BU_BULK_MAX_PACKET_SIZE                  512U
#define RTL8822BU_INTERRUPT_MAX_PACKET_SIZE              64U
#define RTL8822BU_INTERRUPT_INTERVAL                      3U

#define RTL8822BU_VENDOR_REQUEST                         0x05U
#define RTL8822BU_REGISTER_TIMEOUT_MS                      20U
#define RTL8822BU_EFUSE_POLL_MAX                         100U
#define RTL8822BU_FIRMWARE_TRANSFER_TIMEOUT_MS             50U
#define RTL8822BU_FIRMWARE_POLL_MAX                       1000U
#define RTL8822BU_RX_DRAIN_TIMEOUT_MS                       50U
#define RTL8822BU_RX_RECOVERY_LIMIT                           3U
#define RTL8822BU_RECOVERY_CLEANUP_RETRY_LIMIT               32U
/*
 * The bounded RFE/cut model needs at most 3245 control transfers plus
 * 412326 us of explicit table/RF delay.  Fifteen seconds retains a finite
 * failure bound without assuming an unrealistically low host USB latency.
 */
#define RTL8822BU_RADIO_OPEN_TIMEOUT_TICKS      (15U * KERN_CLOCK_HZ)
#define RTL8822BU_RADIO_STOP_TIMEOUT_TICKS       (1U * KERN_CLOCK_HZ)
#define RTL8822BU_STATION_CLOSE_TIMEOUT_TICKS     (1U * KERN_CLOCK_HZ)
#define RTL8822BU_MICROSECONDS_PER_SECOND                 1000000ULL
#define RTL8822BU_RELAXATIONS_PER_MICROSECOND                  128U

#define RTL8822BU_REG_SYS_FUNC_EN                      0x0002U
#define RTL8822BU_REG_SYS_CLKR                         0x0008U
#define RTL8822BU_REG_EFUSE_CTRL                       0x0030U
#define RTL8822BU_REG_LDO_EFUSE_CTRL                   0x0034U
#define RTL8822BU_REG_EFUSE_ACCESS                     0x00cfU
#define RTL8822BU_REG_SYS_CFG1                         0x00f0U
#define RTL8822BU_REG_RSV_CTRL                         0x001cU
#define RTL8822BU_REG_CR                               0x0100U
#define RTL8822BU_REG_TXDMA_PQ_MAP                     0x010cU
#define RTL8822BU_REG_FIFOPAGE_CTRL_2                  0x0204U
#define RTL8822BU_REG_TXDMA_STATUS                     0x0210U
#define RTL8822BU_REG_RQPN_CTRL_2                      0x022cU
#define RTL8822BU_REG_FIFOPAGE_INFO_1                  0x0230U
#define RTL8822BU_REG_BCN_CTRL                         0x0550U
#define RTL8822BU_REG_CPU_DMEM_CON                     0x1080U
#define RTL8822BU_REG_H2CQ_CSR                         0x1330U
#define RTL8822BU_REG_DDMA_CH0SA                       0x1200U
#define RTL8822BU_REG_DDMA_CH0DA                       0x1204U
#define RTL8822BU_REG_DDMA_CH0CTRL                     0x1208U
#define RTL8822BU_REG_MCUFW_CTRL                       0x0080U
#define RTL8822BU_EFUSE_ACCESS_ON                        0x69U
#define RTL8822BU_EFUSE_ACCESS_OFF                       0x00U
#define RTL8822BU_SYS_FUNC_EFUSE_ENABLE                0x1000U
#define RTL8822BU_SYS_CLK_EFUSE_ENABLE                 0x0022U
#define RTL8822BU_EFUSE_BANK_MASK                  0x00000300U
#define RTL8822BU_EFUSE_LDO25_ENABLE               0x80000000U
#define RTL8822BU_EFUSE_ADDRESS_MASK               0x0003ff00U
#define RTL8822BU_EFUSE_ADDRESS_SHIFT                       8U
#define RTL8822BU_EFUSE_READY                      0x80000000U

#define RTL8822BU_WCPU_ENABLE                              0x04U
#define RTL8822BU_WCPU_IO_ENABLE                           0x01U
#define RTL8822BU_TXDMA_HIGH_QUEUE                         0xc0U
#define RTL8822BU_CR_FIRMWARE_TXDMA                        0x05U
#define RTL8822BU_CR_ENABLE_SW_BEACON                      0x01U
#define RTL8822BU_H2CQ_FULL                           0x80000000U
#define RTL8822BU_LOAD_RQPN                           0x80000000U
#define RTL8822BU_DISABLE_TSF_UPDATE                       0x10U
#define RTL8822BU_ENABLE_BEACON                            0x08U
#define RTL8822BU_BEACON_VALID                         0x8000U
#define RTL8822BU_BEACON_PAGE_MASK                     0x0fffU
#define RTL8822BU_TXDMA_PAGE_OVERFLOW                      0x04U

#define RTL8822BU_DDMA_OWN                          0x80000000U
#define RTL8822BU_DDMA_CHECKSUM_ENABLE              0x20000000U
#define RTL8822BU_DDMA_CHECKSUM_ERROR               0x08000000U
#define RTL8822BU_DDMA_RESET_CHECKSUM               0x02000000U
#define RTL8822BU_DDMA_CHECKSUM_CONTINUE            0x01000000U
#define RTL8822BU_DDMA_LENGTH_MASK                  0x0003ffffU
#define RTL8822BU_TX_BUFFER_OCP                     0x18780000U

#define RTL8822BU_MCUFW_INIT_READY                       0x8000U
#define RTL8822BU_MCUFW_DOWNLOAD_READY                   0x4000U
#define RTL8822BU_MCUFW_DMEM_CHECKSUM_OK                 0x0040U
#define RTL8822BU_MCUFW_DMEM_DOWNLOAD_OK                 0x0020U
#define RTL8822BU_MCUFW_IMEM_CHECKSUM_OK                 0x0010U
#define RTL8822BU_MCUFW_IMEM_DOWNLOAD_OK                 0x0008U
#define RTL8822BU_MCUFW_DOWNLOAD_ENABLE                  0x0001U
#define RTL8822BU_MCUFW_CPU_CLOCK_MASK                   0x3000U
#define RTL8822BU_MCUFW_READY_MASK                       0xcfffU
#define RTL8822BU_MCUFW_READY                            0xc078U

#define RTL8822BU_MTU                                  1500U
#define RTL8822BU_RX_BUFFER_SIZE RTL8822B_RX_AGGREGATE_MAX
#define RTL8822BU_SECURITY_TIMEOUT_TICKS       (1U * KERN_CLOCK_HZ)
#define RTL8822BU_GROUP_KEY_COUNT                         4U
#define RTL8822BU_PAIRWISE_STAGING_SLOT                   5U
#define RTL8822BU_GROUP_STAGING_SLOT_BASE                 8U
#define RTL8822BU_CAM_OWNED_SLOT_COUNT                   12U
#define RTL8822BU_TX_REPORT_COUNT                        64U
#define RTL8822BU_TX_REPORT_SEQUENCE_STEP                 4U
#define RTL8822BU_TX_REPORT_RETIRE_TICKS \
	((KERN_CLOCK_HZ + 1U) / 2U)
#define RTL8822BU_C2H_CCX_TX_REPORT_ID                  0x03U
#define RTL8822BU_C2H_EXTENDED_ID                       0xffU
#define RTL8822BU_C2H_EXTENDED_CCX_REPORT_ID            0x0fU
#define RTL8822BU_RX_ENCRYPTION_AES                        4U

enum rtl8822bu_key_role {
	RTL8822BU_KEY_PAIRWISE = 1,
	RTL8822BU_KEY_GROUP = 2
};

enum rtl8822bu_frame_class {
	RTL8822BU_FRAME_MANAGEMENT = 1,
	RTL8822BU_FRAME_EAPOL = 2,
	RTL8822BU_FRAME_DATA = 3
};

enum rtl8822bu_rx_class {
	RTL8822BU_RX_IGNORE = 0,
	RTL8822BU_RX_SCAN = 1,
	RTL8822BU_RX_MANAGEMENT = 2,
	RTL8822BU_RX_EAPOL = 3,
	RTL8822BU_RX_DATA = 4,
	RTL8822BU_RX_TX_REPORT = 5
};

struct rtl8822bu_tx_report_slot {
	uint64_t cookie;
	uint64_t connection_generation;
	uint64_t key_generation;
	uint64_t deadline_ticks;
	uint64_t retire_deadline_ticks;
	uint8_t active;
	uint8_t tombstone;
};

struct rtl8822bu_rx_private {
	enum rtl8822bu_rx_class class;
	uint64_t connection_generation;
	uint64_t key_generation;
	uint64_t packet_number;
	uint64_t cookie;
	int tx_error;
	uint8_t channel;
	uint8_t key_index;
	uint8_t encryption_type;
	uint8_t software_decrypted;
	uint8_t icv_error;
};

struct rtl8822bu_binding {
	struct drv_usb_device *device;
	struct drv_usb_endpoint *bulk_in;
	struct drv_usb_endpoint *bulk_out_high;
	struct drv_usb_endpoint *bulk_out_normal;
	struct drv_usb_endpoint *bulk_out_low;
	unsigned interrupt_in_present;
};

struct rtl8822bu_adapter;

/* The USB core publishes exactly one terminal callback per submission and a
 * successful drain joins that callback before setup may reuse the URB.  The
 * alternating contexts tag the currently published submission for local
 * bookkeeping; they do not claim tolerance for callbacks which violate that
 * core exact-once/drain contract. */
struct rtl8822bu_rx_completion_context {
	struct rtl8822bu_adapter *adapter;
	uint64_t generation;
};

struct rtl8822bu_adapter {
	struct drv_usb_device *usb_device;
	struct drv_usb_interface *interface;
	struct drv_usb_endpoint *bulk_in;
	struct drv_usb_endpoint *bulk_out_high;
	struct drv_usb_endpoint *bulk_out_normal;
	struct drv_usb_endpoint *bulk_out_low;
	struct drv_usb_urb *rx_urb;
	uint8_t *rx_buffer;
	struct net_device *net_device;
	struct wlan_station *station;
	struct rtl8822bu_board_info board;
	struct rtl8822b_radio radio;
	struct mutex lifecycle_lock;
	struct spinlock lock;
	unsigned ready;
	unsigned net_live;
	unsigned station_attached;
	unsigned detaching;
	/* Set only for a physical USB-absence boundary.  The USB core closes its
	 * submit/binding gate before FORCE detach, so checked software inverses
	 * must retire local ownership without attempting impossible register I/O. */
	unsigned transport_absent;
	unsigned firmware_running;
	unsigned radio_running;
	unsigned opened;
	unsigned closing;
	unsigned stopping;
	unsigned quarantined;
	unsigned starts_active;
	unsigned polls_active;
	unsigned rx_ready;
	unsigned rx_rearm;
	unsigned rx_rearm_active;
	unsigned rx_generation_barrier;
	unsigned rx_error_streak;
	unsigned recovery_pending;
	unsigned recovery_active;
	unsigned recovery_cleanup_attempts;
	int recovery_error;
	unsigned control_error_streak;
	unsigned tx_high_error_streak;
	unsigned tx_normal_error_streak;
	unsigned tx_low_error_streak;
	unsigned tx_report_error_streak;
	unsigned tx_report_tombstone_count;
	uint64_t rx_submit_generation;
	uint64_t rx_inflight_generation;
	uint64_t rx_completed_generation;
	struct rtl8822bu_rx_completion_context rx_completion[2];
	unsigned radio_operations_active;
	/* Set under lock before any CAM/BSSID transition.  TX admission and the
	 * operation lease are checked/changed by the same lock, closing the race
	 * between the final admitted USB transfer and the hardware queue drain. */
	unsigned tx_quiescing;
	unsigned connection_preparing;
	unsigned connection_prepared;
	unsigned association_active;
	unsigned association_uncertain;
	unsigned security_enabled;
	unsigned pairwise_key_installed;
	uint8_t group_key_mask;
	uint8_t group_staged_mask;
	uint8_t group_retired_mask;
	uint8_t pairwise_key_slot;
	uint8_t pairwise_staged_slot;
	uint8_t pairwise_retired_slot;
	uint8_t pairwise_staged_installed;
	uint8_t pairwise_retired_valid;
	uint32_t cam_uncertain_mask;
	uint8_t connection_channel;
	uint16_t association_aid;
	uint8_t connection_bssid[6];
	uint64_t connection_generation;
	uint64_t deauthentication_attempted_generation;
	uint64_t pairwise_key_generation;
	uint64_t pairwise_staged_generation;
	uint64_t pairwise_retired_generation;
	uint64_t group_key_generation[RTL8822BU_GROUP_KEY_COUNT];
	uint64_t group_staged_generation[RTL8822BU_GROUP_KEY_COUNT];
	uint64_t group_retired_generation[RTL8822BU_GROUP_KEY_COUNT];
	uint8_t group_key_slot[RTL8822BU_GROUP_KEY_COUNT];
	uint8_t group_staged_slot[RTL8822BU_GROUP_KEY_COUNT];
	uint8_t group_retired_slot[RTL8822BU_GROUP_KEY_COUNT];
	struct rtl8822bu_tx_report_slot
	    tx_reports[RTL8822BU_TX_REPORT_COUNT];
	uint8_t tx_report_next;
	uint64_t scan_generation;
	uint32_t scan_channel;
};

typedef int (*rtl8822bu_firmware_walk_fn)(
	const struct rtl8822b_firmware_view *,
	rtl8822b_firmware_chunk_fn, void *);

static int rtl8822bu_read8(struct rtl8822bu_adapter *, uint16_t, uint8_t *);
static int rtl8822bu_read16(struct rtl8822bu_adapter *, uint16_t, uint16_t *);
static int rtl8822bu_read32(struct rtl8822bu_adapter *, uint16_t, uint32_t *);
static int rtl8822bu_write8(struct rtl8822bu_adapter *, uint16_t, uint8_t);
static int rtl8822bu_write16(struct rtl8822bu_adapter *, uint16_t, uint16_t);
static int rtl8822bu_write32(struct rtl8822bu_adapter *, uint16_t, uint32_t);
static int rtl8822bu_security_hardware_clear(
	struct rtl8822bu_adapter *, uint64_t);
static int rtl8822bu_hardware_start_locked(struct rtl8822bu_adapter *);
static void rtl8822bu_runtime_recover(struct rtl8822bu_adapter *);
static int rtl8822bu_rx_generation_pause(struct rtl8822bu_adapter *,
	uint64_t);
static int rtl8822bu_rx_generation_resume(struct rtl8822bu_adapter *,
	uint64_t);
static void rtl8822bu_deauthenticate_best_effort(
	struct rtl8822bu_adapter *, const uint8_t [6], uint64_t);
static void rtl8822bu_sync_endpoint_result(struct rtl8822bu_adapter *,
	struct drv_usb_endpoint *, int);
static int rtl8822bu_bulk_transfer(struct rtl8822bu_adapter *,
	struct drv_usb_endpoint *, void *, size_t, unsigned, size_t *);
static int rtl8822bu_tx_report_generation_active_locked(
	const struct rtl8822bu_adapter *, uint64_t, uint64_t, int);
static void rtl8822bu_tx_report_reap_locked(
	struct rtl8822bu_adapter *, uint64_t);

static int
rtl8822bu_radio_read(void *context, uint16_t address, unsigned width,
	uint32_t *value)
{
	struct rtl8822bu_adapter *adapter = context;
	uint16_t value16;
	uint8_t value8;
	int error;

	if (adapter == NULL || value == NULL)
		return EINVAL;
	if (width == 1U) {
		error = rtl8822bu_read8(adapter, address, &value8);
		if (error == 0)
			*value = value8;
		return error;
	}
	if (width == 2U) {
		error = rtl8822bu_read16(adapter, address, &value16);
		if (error == 0)
			*value = value16;
		return error;
	}
	if (width == 4U)
		return rtl8822bu_read32(adapter, address, value);
	return EINVAL;
}

static int
rtl8822bu_radio_write(void *context, uint16_t address, unsigned width,
	uint32_t value)
{
	struct rtl8822bu_adapter *adapter = context;

	if (adapter == NULL)
		return EINVAL;
	if (width == 1U && value <= UINT8_MAX)
		return rtl8822bu_write8(adapter, address, (uint8_t)value);
	if (width == 2U && value <= UINT16_MAX)
		return rtl8822bu_write16(adapter, address, (uint16_t)value);
	if (width == 4U)
		return rtl8822bu_write32(adapter, address, value);
	return EINVAL;
}

static uint64_t
rtl8822bu_radio_now(void *context)
{
	(void)context;
	return clock_ticks();
}

static void
rtl8822bu_radio_yield(void *context)
{
	(void)context;
	sched_yield();
}

static int
rtl8822bu_radio_delay_us(void *context, uint32_t microseconds,
	uint64_t deadline_ticks)
{
	uint64_t now, target, ticks, scaled;
	uint32_t remaining, batch, spin;

	(void)context;
	now = clock_ticks();
	if (now >= deadline_ticks)
		return ETIMEDOUT;
	if (microseconds == 0U)
		return 0;
	/* Long requests are representable by the monotonic kernel clock.  Short
	 * MAC/RF-table delays are deliberately not rounded to a 10-ms tick: doing
	 * so for every 1/5/13-us table entry makes initialization take minutes.
	 * hal_atomic_relax() keeps the bounded sub-tick path architecture-neutral. */
	scaled = (uint64_t)microseconds * KERN_CLOCK_HZ;
	/* One millisecond and longer is rare (power transition or an explicit
	 * table pseudo-op) and needs a guaranteed minimum, so round it once to the
	 * coarse clock.  Only the dense <=100-us RF-write delays use relax. */
	if (microseconds >= 1000U) {
		ticks = (scaled + RTL8822BU_MICROSECONDS_PER_SECOND - 1U) /
		    RTL8822BU_MICROSECONDS_PER_SECOND;
		if (ticks > UINT64_MAX - now)
			return EOVERFLOW;
		target = now + ticks;
		if (target >= deadline_ticks)
			return ETIMEDOUT;
		while (clock_ticks() < target) {
			if (clock_ticks() >= deadline_ticks)
				return ETIMEDOUT;
			sched_yield();
		}
		return 0;
	}
	remaining = microseconds;
	while (remaining != 0U) {
		batch = remaining > 50U ? 50U : remaining;
		for (spin = 0U;
		    spin < batch * RTL8822BU_RELAXATIONS_PER_MICROSECOND; spin++)
			hal_atomic_relax();
		remaining -= batch;
		now = clock_ticks();
		if (now >= deadline_ticks)
			return ETIMEDOUT;
	}
	return 0;
}

static int
rtl8822bu_deadline_after(uint64_t delta, uint64_t *deadline)
{
	uint64_t now = clock_ticks();

	if (deadline == NULL || now > UINT64_MAX - delta)
		return EOVERFLOW;
	*deadline = now + delta;
	return 0;
}

static uint16_t
rtl8822bu_load_le16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static uint32_t
rtl8822bu_load_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	    ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void
rtl8822bu_store_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

static void
rtl8822bu_store_le32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

static int
rtl8822bu_mac_equal(const uint8_t left[6], const uint8_t right[6])
{
	uint8_t difference = 0U;
	unsigned index;

	for (index = 0U; index < 6U; index++)
		difference |= left[index] ^ right[index];
	return difference == 0U;
}

static int
rtl8822bu_bytes_zero(const void *pointer, size_t length)
{
	const uint8_t *bytes = pointer;
	uint8_t value = 0U;

	while (length-- != 0U)
		value |= *bytes++;
	return value == 0U;
}

static int
rtl8822bu_unicast_address(const uint8_t address[6])
{
	static const uint8_t zero[6];

	return address != NULL && (address[0] & 1U) == 0U &&
	    !rtl8822bu_mac_equal(address, zero);
}

static void
rtl8822bu_connection_state_clear_locked(struct rtl8822bu_adapter *adapter)
{
	unsigned index;

	adapter->connection_preparing = 0U;
	adapter->connection_prepared = 0U;
	adapter->association_active = 0U;
	adapter->association_uncertain = 0U;
	adapter->security_enabled = 0U;
	adapter->pairwise_key_installed = 0U;
	adapter->group_key_mask = 0U;
	adapter->group_staged_mask = 0U;
	adapter->group_retired_mask = 0U;
	adapter->pairwise_key_slot = RTL8822B_CAM_PAIRWISE_SLOT;
	adapter->pairwise_staged_slot = RTL8822BU_PAIRWISE_STAGING_SLOT;
	adapter->pairwise_retired_slot = 0U;
	adapter->pairwise_staged_installed = 0U;
	adapter->pairwise_retired_valid = 0U;
	adapter->cam_uncertain_mask = 0U;
	adapter->connection_channel = 0U;
	adapter->association_aid = 0U;
	adapter->connection_generation = 0U;
	adapter->deauthentication_attempted_generation = 0U;
	adapter->pairwise_key_generation = 0U;
	adapter->pairwise_staged_generation = 0U;
	adapter->pairwise_retired_generation = 0U;
	memset(adapter->connection_bssid, 0,
	    sizeof(adapter->connection_bssid));
	memset(adapter->group_key_generation, 0,
	    sizeof(adapter->group_key_generation));
	memset(adapter->group_staged_generation, 0,
	    sizeof(adapter->group_staged_generation));
	memset(adapter->group_retired_generation, 0,
	    sizeof(adapter->group_retired_generation));
	for (index = 0U; index < RTL8822BU_GROUP_KEY_COUNT; index++) {
		adapter->group_key_slot[index] = (uint8_t)index;
		adapter->group_staged_slot[index] = (uint8_t)
		    (RTL8822BU_GROUP_STAGING_SLOT_BASE + index);
		adapter->group_retired_slot[index] = 0U;
	}
	adapter->tx_quiescing = 0U;
}

static void
rtl8822bu_tx_quiesce_result_locked(struct rtl8822bu_adapter *adapter,
	int error, int absence_known)
{
	if (error == 0 || absence_known) {
		adapter->tx_quiescing = 0U;
		return;
	}
	if (error == EBUSY)
		/* Preserve closed admission while the worker retries the drain.  Opening
		 * it here would allow another direct frame to refill the MAC queue. */
		return;
	/* A failed transport or a partially issued hardware mutation leaves the
	 * admission gate closed.  Teardown inverses deliberately remain callable
	 * while quarantined and are the only way to prove absence. */
	adapter->tx_quiescing = 1U;
	adapter->quarantined = 1U;
}

static void
rtl8822bu_operation_leave(struct rtl8822bu_adapter *adapter)
{
	unsigned long enabled = spin_lock_irqsave(&adapter->lock);
	int schedule;

	if (adapter->radio_operations_active == 0U)
		__builtin_trap();
	adapter->radio_operations_active--;
	schedule = adapter->radio_operations_active == 0U &&
	    adapter->recovery_pending && !adapter->recovery_active &&
	    adapter->ready && !adapter->detaching && adapter->opened &&
	    !adapter->closing &&
	    !adapter->stopping && adapter->net_device != NULL;
	spin_unlock_irqrestore(&adapter->lock, enabled);
	if (schedule)
		net_device_schedule_poll(adapter->net_device);
}

static void
rtl8822bu_radio_transport_init(struct rtl8822bu_adapter *adapter,
	struct rtl8822b_radio_transport *transport)
{
	memset(transport, 0, sizeof(*transport));
	transport->context = adapter;
	transport->read = rtl8822bu_radio_read;
	transport->write = rtl8822bu_radio_write;
	transport->now_ticks = rtl8822bu_radio_now;
	transport->delay_us = rtl8822bu_radio_delay_us;
	transport->yield = rtl8822bu_radio_yield;
}

static int
rtl8822bu_endpoint_accept(struct rtl8822bu_binding *binding,
	struct drv_usb_endpoint *endpoint)
{
	const struct drv_usb_endpoint_descriptor *descriptor;
	struct drv_usb_endpoint **slot;
	enum drv_usb_transfer_type type;
	uint16_t packet_size;
	uint8_t interval;

	if (endpoint == NULL)
		return 0;
	descriptor = drv_usb_endpoint_descriptor(endpoint);
	if (descriptor == NULL)
		return 0;
	/* RTL8822BU advertises interrupt-IN 0x87, but this minimum driver receives
	 * all C2H/TX reports through bulk-IN 0x84 and never submits an interrupt
	 * URB.  Validate it when present without making an unused producer part of
	 * the binding/lifecycle contract. */
	if (descriptor->address == RTL8822BU_INTERRUPT_IN_ADDRESS) {
		if (binding->interrupt_in_present ||
		    drv_usb_endpoint_type(endpoint) != DRV_USB_TRANSFER_INTERRUPT ||
		    descriptor->attributes != 3U ||
		    descriptor->maximum_packet_size !=
		    RTL8822BU_INTERRUPT_MAX_PACKET_SIZE ||
		    descriptor->interval != RTL8822BU_INTERRUPT_INTERVAL ||
		    !drv_usb_endpoint_is_input(endpoint))
			return 0;
		binding->interrupt_in_present = 1U;
		return 1;
	}
	slot = NULL;
	type = DRV_USB_TRANSFER_BULK;
	packet_size = RTL8822BU_BULK_MAX_PACKET_SIZE;
	interval = 0U;
	switch (descriptor->address) {
	case RTL8822BU_BULK_IN_ADDRESS:
		slot = &binding->bulk_in;
		break;
	case RTL8822BU_BULK_OUT_HIGH_ADDRESS:
		slot = &binding->bulk_out_high;
		break;
	case RTL8822BU_BULK_OUT_NORMAL_ADDRESS:
		slot = &binding->bulk_out_normal;
		break;
	case RTL8822BU_BULK_OUT_LOW_ADDRESS:
		slot = &binding->bulk_out_low;
		break;
	default:
		return 0;
	}
	if (*slot != NULL || drv_usb_endpoint_type(endpoint) != type ||
	    descriptor->attributes != (type == DRV_USB_TRANSFER_BULK ? 2U : 3U) ||
	    descriptor->maximum_packet_size != packet_size ||
	    descriptor->interval != interval)
		return 0;
	if ((descriptor->address == RTL8822BU_BULK_IN_ADDRESS) !=
	    drv_usb_endpoint_is_input(endpoint))
		return 0;
	*slot = endpoint;
	return 1;
}

static int
rtl8822bu_binding_parse(struct drv_usb_interface *interface,
	struct rtl8822bu_binding *binding)
{
	const struct drv_usb_device_descriptor *device_descriptor;
	const struct drv_usb_interface_descriptor *interface_descriptor;
	const struct drv_usb_host_interface *alternate;
	struct drv_usb_device *device;
	unsigned endpoint_count;
	unsigned index;

	if (interface == NULL || binding == NULL)
		return 0;
	memset(binding, 0, sizeof(*binding));
	device = drv_usb_interface_device(interface);
	device_descriptor = drv_usb_device_descriptor(device);
	interface_descriptor = drv_usb_interface_descriptor(interface);
	if (device == NULL || device_descriptor == NULL ||
	    interface_descriptor == NULL ||
	    device_descriptor->usb_release != RTL8822BU_USB_RELEASE ||
	    device_descriptor->vendor != RTL8822BU_VENDOR_ID ||
	    device_descriptor->product != RTL8822BU_PRODUCT_ID ||
	    device_descriptor->device_release != RTL8822BU_DEVICE_RELEASE ||
	    device_descriptor->device_class != 0U ||
	    device_descriptor->device_subclass != 0U ||
	    device_descriptor->device_protocol != 0U ||
	    device_descriptor->endpoint0_max_packet_size != 64U ||
	    device_descriptor->configuration_count != 1U ||
	    drv_usb_device_speed(device) != DRV_USB_SPEED_HIGH ||
	    interface_descriptor->interface_number != 0U ||
	    interface_descriptor->alternate_setting != 0U ||
	    (interface_descriptor->endpoint_count < 4U ||
	    interface_descriptor->endpoint_count > 5U) ||
	    interface_descriptor->interface_class !=
	    RTL8822BU_INTERFACE_CLASS ||
	    interface_descriptor->interface_subclass !=
	    RTL8822BU_INTERFACE_SUBCLASS ||
	    interface_descriptor->interface_protocol !=
	    RTL8822BU_INTERFACE_PROTOCOL ||
	    drv_usb_interface_alternate_count(interface) != 1U ||
	    (drv_usb_device_hcd_capabilities(device) &
	    DRV_USB_HCD_CAP_CONCURRENT_URBS) == 0U)
		return 0;
	alternate = drv_usb_interface_active_alternate(interface);
	endpoint_count = interface_descriptor->endpoint_count;
	if (alternate == NULL ||
	    drv_usb_host_interface_endpoint_count(alternate) != endpoint_count)
		return 0;
	for (index = 0U; index < endpoint_count; index++)
		if (!rtl8822bu_endpoint_accept(binding,
		    drv_usb_host_interface_endpoint(alternate, index)))
			return 0;
	if (binding->bulk_in == NULL || binding->bulk_out_high == NULL ||
	    binding->bulk_out_normal == NULL || binding->bulk_out_low == NULL)
		return 0;
	binding->device = device;
	return 1;
}

static int
rtl8822bu_register_transfer(struct rtl8822bu_adapter *adapter,
	uint16_t reg, void *bytes, size_t width, int write)
{
	size_t actual = 0U;
	uint8_t request_type;
	int error;

	if (adapter == NULL || adapter->usb_device == NULL || bytes == NULL ||
	    (width != 1U && width != 2U && width != 4U))
		return EINVAL;
	request_type = (write ? DRV_USB_DIR_OUT : DRV_USB_DIR_IN) |
	    DRV_USB_REQUEST_VENDOR | DRV_USB_RECIP_DEVICE;
	error = drv_usb_control(adapter->usb_device, request_type,
	    RTL8822BU_VENDOR_REQUEST, reg, 0U, bytes, width,
	    RTL8822BU_REGISTER_TIMEOUT_MS, &actual);
	/* EP0 has implicit host-side STALL recovery in the USB core.  Reissuing
	 * this idempotent vendor register transaction once is the endpoint-local
	 * retry; no device/controller reset is permitted. */
	if (error == EPIPE) {
		actual = 0U;
		error = drv_usb_control(adapter->usb_device, request_type,
		    RTL8822BU_VENDOR_REQUEST, reg, 0U, bytes, width,
		    RTL8822BU_REGISTER_TIMEOUT_MS, &actual);
	}
	if (error == 0 && actual != width)
		error = EIO;
	rtl8822bu_sync_endpoint_result(adapter, NULL, error);
	if (error != 0)
		return error;
	return 0;
}

static int
rtl8822bu_read8(struct rtl8822bu_adapter *adapter, uint16_t reg,
	uint8_t *result)
{
	uint8_t bytes[1];
	int error;

	if (result == NULL)
		return EINVAL;
	error = rtl8822bu_register_transfer(adapter, reg, bytes, sizeof(bytes), 0);
	if (error == 0)
		*result = bytes[0];
	return error;
}

static int
rtl8822bu_read16(struct rtl8822bu_adapter *adapter, uint16_t reg,
	uint16_t *result)
{
	uint8_t bytes[2];
	int error;

	if (result == NULL)
		return EINVAL;
	error = rtl8822bu_register_transfer(adapter, reg, bytes, sizeof(bytes), 0);
	if (error == 0)
		*result = (uint16_t)((uint16_t)bytes[0] |
		    ((uint16_t)bytes[1] << 8));
	return error;
}

static int
rtl8822bu_read32(struct rtl8822bu_adapter *adapter, uint16_t reg,
	uint32_t *result)
{
	uint8_t bytes[4];
	int error;

	if (result == NULL)
		return EINVAL;
	error = rtl8822bu_register_transfer(adapter, reg, bytes, sizeof(bytes), 0);
	if (error == 0)
		*result = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
		    ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
	return error;
}

static int
rtl8822bu_write8(struct rtl8822bu_adapter *adapter, uint16_t reg,
	uint8_t value)
{
	uint8_t bytes[1] = { value };

	return rtl8822bu_register_transfer(adapter, reg, bytes, sizeof(bytes), 1);
}

static int
rtl8822bu_write16(struct rtl8822bu_adapter *adapter, uint16_t reg,
	uint16_t value)
{
	uint8_t bytes[2] = { (uint8_t)value, (uint8_t)(value >> 8) };

	return rtl8822bu_register_transfer(adapter, reg, bytes, sizeof(bytes), 1);
}

static int
rtl8822bu_write32(struct rtl8822bu_adapter *adapter, uint16_t reg,
	uint32_t value)
{
	uint8_t bytes[4] = {
		(uint8_t)value,
		(uint8_t)(value >> 8),
		(uint8_t)(value >> 16),
		(uint8_t)(value >> 24)
	};

	return rtl8822bu_register_transfer(adapter, reg, bytes, sizeof(bytes), 1);
}

static void
rtl8822bu_record_cleanup_error(int *error, int cleanup_error)
{
	if (*error == 0 && cleanup_error != 0)
		*error = cleanup_error;
}

static int
rtl8822bu_efuse_physical_read(struct rtl8822bu_adapter *adapter,
	uint8_t physical[RTL8822B_EFUSE_PHYSICAL_SIZE])
{
	uint16_t original_sys_func = 0U, original_sys_clk = 0U;
	uint32_t original_ldo = 0U, control = 0U;
	uint8_t data;
	unsigned address, poll;
	int error = 0, cleanup_error;
	int sys_func_saved = 0, sys_clk_saved = 0, ldo_saved = 0;

	if (adapter == NULL || physical == NULL)
		return EINVAL;
	error = rtl8822bu_write8(adapter, RTL8822BU_REG_EFUSE_ACCESS,
	    RTL8822BU_EFUSE_ACCESS_ON);
	if (error != 0)
		goto out;
	error = rtl8822bu_read16(adapter, RTL8822BU_REG_SYS_FUNC_EN,
	    &original_sys_func);
	if (error != 0)
		goto out;
	sys_func_saved = 1;
	error = rtl8822bu_write16(adapter, RTL8822BU_REG_SYS_FUNC_EN,
	    original_sys_func | RTL8822BU_SYS_FUNC_EFUSE_ENABLE);
	if (error != 0)
		goto out;
	error = rtl8822bu_read16(adapter, RTL8822BU_REG_SYS_CLKR,
	    &original_sys_clk);
	if (error != 0)
		goto out;
	sys_clk_saved = 1;
	error = rtl8822bu_write16(adapter, RTL8822BU_REG_SYS_CLKR,
	    original_sys_clk | RTL8822BU_SYS_CLK_EFUSE_ENABLE);
	if (error != 0)
		goto out;
	error = rtl8822bu_read32(adapter, RTL8822BU_REG_LDO_EFUSE_CTRL,
	    &original_ldo);
	if (error != 0)
		goto out;
	ldo_saved = 1;
	error = rtl8822bu_write32(adapter, RTL8822BU_REG_LDO_EFUSE_CTRL,
	    original_ldo & ~(RTL8822BU_EFUSE_BANK_MASK |
	    RTL8822BU_EFUSE_LDO25_ENABLE));
	if (error != 0)
		goto out;
	error = rtl8822bu_read32(adapter, RTL8822BU_REG_EFUSE_CTRL, &control);
	if (error != 0)
		goto out;
	for (address = 0U; address < RTL8822B_EFUSE_PHYSICAL_SIZE; address++) {
		control &= ~RTL8822BU_EFUSE_ADDRESS_MASK;
		control |= address << RTL8822BU_EFUSE_ADDRESS_SHIFT;
		control &= ~RTL8822BU_EFUSE_READY;
		error = rtl8822bu_write32(adapter, RTL8822BU_REG_EFUSE_CTRL,
		    control);
		if (error != 0)
			goto out;
		for (poll = 0U; poll < RTL8822BU_EFUSE_POLL_MAX; poll++) {
			error = rtl8822bu_read32(adapter,
			    RTL8822BU_REG_EFUSE_CTRL, &control);
			if (error != 0)
				goto out;
			if ((control & RTL8822BU_EFUSE_READY) != 0U)
				break;
		}
		if (poll == RTL8822BU_EFUSE_POLL_MAX) {
			error = ETIMEDOUT;
			goto out;
		}
		error = rtl8822bu_read8(adapter, RTL8822BU_REG_EFUSE_CTRL,
		    &data);
		if (error != 0)
			goto out;
		physical[address] = data;
	}

out:
	if (ldo_saved) {
		cleanup_error = rtl8822bu_write32(adapter,
		    RTL8822BU_REG_LDO_EFUSE_CTRL, original_ldo);
		rtl8822bu_record_cleanup_error(&error, cleanup_error);
	}
	if (sys_clk_saved) {
		cleanup_error = rtl8822bu_write16(adapter,
		    RTL8822BU_REG_SYS_CLKR, original_sys_clk);
		rtl8822bu_record_cleanup_error(&error, cleanup_error);
	}
	if (sys_func_saved) {
		cleanup_error = rtl8822bu_write16(adapter,
		    RTL8822BU_REG_SYS_FUNC_EN, original_sys_func);
		rtl8822bu_record_cleanup_error(&error, cleanup_error);
	}
	cleanup_error = rtl8822bu_write8(adapter, RTL8822BU_REG_EFUSE_ACCESS,
	    RTL8822BU_EFUSE_ACCESS_OFF);
	rtl8822bu_record_cleanup_error(&error, cleanup_error);
	return error;
}

static int
rtl8822bu_board_read(struct rtl8822bu_adapter *adapter,
	struct rtl8822bu_board_info *board)
{
	uint8_t *physical, *logical;
	uint32_t sys_cfg1;
	int error;

	if (adapter == NULL || board == NULL)
		return EINVAL;
	physical = hal_malloc(RTL8822B_EFUSE_PHYSICAL_SIZE);
	logical = hal_malloc(RTL8822B_EFUSE_LOGICAL_SIZE);
	if (physical == NULL || logical == NULL) {
		hal_free(logical);
		hal_free(physical);
		return ENOMEM;
	}
	error = rtl8822bu_read32(adapter, RTL8822BU_REG_SYS_CFG1, &sys_cfg1);
	if (error == 0)
		error = rtl8822bu_efuse_physical_read(adapter, physical);
	if (error == 0)
		error = rtl8822b_efuse_decode(physical,
		    RTL8822B_EFUSE_PHYSICAL_SIZE, logical,
		    RTL8822B_EFUSE_LOGICAL_SIZE);
	if (error == 0)
		error = rtl8822bu_board_parse(logical,
		    RTL8822B_EFUSE_LOGICAL_SIZE, sys_cfg1, board);
	memset(logical, 0, RTL8822B_EFUSE_LOGICAL_SIZE);
	memset(physical, 0, RTL8822B_EFUSE_PHYSICAL_SIZE);
	hal_free(logical);
	hal_free(physical);
	return error;
}

struct rtl8822bu_firmware_saved_registers {
	uint8_t txdma_map_high;
	uint8_t cr;
	uint8_t cr_high;
	uint8_t beacon_control;
	uint8_t sys_func_high;
	uint8_t reserved_control_high;
	uint8_t sys_clock_high;
	uint8_t cpu_dmem_high;
	uint16_t fifo_page_info;
	uint16_t fifo_page_control;
	uint16_t mcufw_control;
	uint32_t h2c_queue;
	uint32_t rqpn_control;
};

struct rtl8822bu_firmware_transfer {
	struct rtl8822bu_adapter *adapter;
	const struct rtl8822b_firmware_view *view;
	uint8_t *wire_buffer;
	enum rtl8822b_firmware_segment segment;
	uint32_t next_destination;
	size_t next_file_offset;
	unsigned segment_open;
	unsigned dmem_complete;
	unsigned imem_complete;
};

static int
rtl8822bu_wait32(struct rtl8822bu_adapter *adapter, uint16_t reg,
	uint32_t mask, uint32_t expected)
{
	uint32_t value;
	unsigned poll;
	int error;

	for (poll = 0U; poll < RTL8822BU_FIRMWARE_POLL_MAX; poll++) {
		error = rtl8822bu_read32(adapter, reg, &value);
		if (error != 0)
			return error;
		if ((value & mask) == expected)
			return 0;
		sched_yield();
	}
	return ETIMEDOUT;
}

static int
rtl8822bu_wait16(struct rtl8822bu_adapter *adapter, uint16_t reg,
	uint16_t mask, uint16_t expected)
{
	uint16_t value;
	unsigned poll;
	int error;

	for (poll = 0U; poll < RTL8822BU_FIRMWARE_POLL_MAX; poll++) {
		error = rtl8822bu_read16(adapter, reg, &value);
		if (error != 0)
			return error;
		if ((value & mask) == expected)
			return 0;
		sched_yield();
	}
	return ETIMEDOUT;
}

static int
rtl8822bu_firmware_save(struct rtl8822bu_adapter *adapter,
	struct rtl8822bu_firmware_saved_registers *saved)
{
	int error;

	memset(saved, 0, sizeof(*saved));
	error = rtl8822bu_read8(adapter, RTL8822BU_REG_TXDMA_PQ_MAP + 1U,
	    &saved->txdma_map_high);
	if (error == 0)
		error = rtl8822bu_read8(adapter, RTL8822BU_REG_CR, &saved->cr);
	if (error == 0)
		error = rtl8822bu_read8(adapter, RTL8822BU_REG_CR + 1U,
		    &saved->cr_high);
	if (error == 0)
		error = rtl8822bu_read32(adapter, RTL8822BU_REG_H2CQ_CSR,
		    &saved->h2c_queue);
	if (error == 0)
		error = rtl8822bu_read16(adapter, RTL8822BU_REG_FIFOPAGE_INFO_1,
		    &saved->fifo_page_info);
	if (error == 0)
		error = rtl8822bu_read32(adapter, RTL8822BU_REG_RQPN_CTRL_2,
		    &saved->rqpn_control);
	if (error == 0)
		error = rtl8822bu_read8(adapter, RTL8822BU_REG_BCN_CTRL,
		    &saved->beacon_control);
	if (error == 0)
		error = rtl8822bu_read16(adapter, RTL8822BU_REG_FIFOPAGE_CTRL_2,
		    &saved->fifo_page_control);
	if (error == 0)
		error = rtl8822bu_read8(adapter, RTL8822BU_REG_SYS_FUNC_EN + 1U,
		    &saved->sys_func_high);
	if (error == 0)
		error = rtl8822bu_read8(adapter, RTL8822BU_REG_RSV_CTRL + 1U,
		    &saved->reserved_control_high);
	if (error == 0)
		error = rtl8822bu_read8(adapter, RTL8822BU_REG_SYS_CLKR + 1U,
		    &saved->sys_clock_high);
	if (error == 0)
		error = rtl8822bu_read8(adapter, RTL8822BU_REG_CPU_DMEM_CON + 2U,
		    &saved->cpu_dmem_high);
	if (error == 0)
		error = rtl8822bu_read16(adapter, RTL8822BU_REG_MCUFW_CTRL,
		    &saved->mcufw_control);
	return error;
}

static int
rtl8822bu_firmware_restore_transport(struct rtl8822bu_adapter *adapter,
	const struct rtl8822bu_firmware_saved_registers *saved)
{
	int error = 0, cleanup_error;

	cleanup_error = rtl8822bu_write16(adapter,
	    RTL8822BU_REG_FIFOPAGE_CTRL_2,
	    saved->fifo_page_control | RTL8822BU_BEACON_VALID);
	rtl8822bu_record_cleanup_error(&error, cleanup_error);
	cleanup_error = rtl8822bu_write8(adapter, RTL8822BU_REG_BCN_CTRL,
	    saved->beacon_control);
	rtl8822bu_record_cleanup_error(&error, cleanup_error);
	cleanup_error = rtl8822bu_write32(adapter, RTL8822BU_REG_RQPN_CTRL_2,
	    saved->rqpn_control);
	rtl8822bu_record_cleanup_error(&error, cleanup_error);
	cleanup_error = rtl8822bu_write16(adapter,
	    RTL8822BU_REG_FIFOPAGE_INFO_1, saved->fifo_page_info);
	rtl8822bu_record_cleanup_error(&error, cleanup_error);
	cleanup_error = rtl8822bu_write32(adapter, RTL8822BU_REG_H2CQ_CSR,
	    saved->h2c_queue);
	rtl8822bu_record_cleanup_error(&error, cleanup_error);
	cleanup_error = rtl8822bu_write8(adapter, RTL8822BU_REG_CR + 1U,
	    saved->cr_high);
	rtl8822bu_record_cleanup_error(&error, cleanup_error);
	cleanup_error = rtl8822bu_write8(adapter, RTL8822BU_REG_CR, saved->cr);
	rtl8822bu_record_cleanup_error(&error, cleanup_error);
	cleanup_error = rtl8822bu_write8(adapter,
	    RTL8822BU_REG_TXDMA_PQ_MAP + 1U, saved->txdma_map_high);
	rtl8822bu_record_cleanup_error(&error, cleanup_error);
	return error;
}

static int
rtl8822bu_firmware_restore_failure(struct rtl8822bu_adapter *adapter,
	const struct rtl8822bu_firmware_saved_registers *saved, int error)
{
	int cleanup_error;

	cleanup_error = rtl8822bu_firmware_restore_transport(adapter, saved);
	rtl8822bu_record_cleanup_error(&error, cleanup_error);
	cleanup_error = rtl8822bu_write16(adapter, RTL8822BU_REG_MCUFW_CTRL,
	    saved->mcufw_control);
	rtl8822bu_record_cleanup_error(&error, cleanup_error);
	cleanup_error = rtl8822bu_write8(adapter,
	    RTL8822BU_REG_CPU_DMEM_CON + 2U, saved->cpu_dmem_high);
	rtl8822bu_record_cleanup_error(&error, cleanup_error);
	cleanup_error = rtl8822bu_write8(adapter, RTL8822BU_REG_SYS_CLKR + 1U,
	    saved->sys_clock_high);
	rtl8822bu_record_cleanup_error(&error, cleanup_error);
	cleanup_error = rtl8822bu_write8(adapter, RTL8822BU_REG_RSV_CTRL + 1U,
	    saved->reserved_control_high);
	rtl8822bu_record_cleanup_error(&error, cleanup_error);
	cleanup_error = rtl8822bu_write8(adapter,
	    RTL8822BU_REG_SYS_FUNC_EN + 1U, saved->sys_func_high);
	rtl8822bu_record_cleanup_error(&error, cleanup_error);
	return error;
}

static int
rtl8822bu_firmware_prepare(struct rtl8822bu_adapter *adapter,
	const struct rtl8822bu_firmware_saved_registers *saved)
{
	int error;

	error = rtl8822bu_write8(adapter, RTL8822BU_REG_SYS_FUNC_EN + 1U,
	    saved->sys_func_high & ~RTL8822BU_WCPU_ENABLE);
	if (error == 0)
		error = rtl8822bu_write8(adapter, RTL8822BU_REG_RSV_CTRL + 1U,
		    saved->reserved_control_high & ~RTL8822BU_WCPU_IO_ENABLE);
	if (error == 0)
		error = rtl8822bu_write8(adapter,
		    RTL8822BU_REG_TXDMA_PQ_MAP + 1U,
		    RTL8822BU_TXDMA_HIGH_QUEUE);
	if (error == 0)
		error = rtl8822bu_write8(adapter, RTL8822BU_REG_CR,
		    RTL8822BU_CR_FIRMWARE_TXDMA);
	if (error == 0)
		error = rtl8822bu_write8(adapter, RTL8822BU_REG_CR + 1U,
		    saved->cr_high | RTL8822BU_CR_ENABLE_SW_BEACON);
	if (error == 0)
		error = rtl8822bu_write32(adapter, RTL8822BU_REG_H2CQ_CSR,
		    RTL8822BU_H2CQ_FULL);
	if (error == 0)
		error = rtl8822bu_write16(adapter,
		    RTL8822BU_REG_FIFOPAGE_INFO_1, 0x0200U);
	if (error == 0)
		error = rtl8822bu_write32(adapter, RTL8822BU_REG_RQPN_CTRL_2,
		    saved->rqpn_control | RTL8822BU_LOAD_RQPN);
	if (error == 0)
		error = rtl8822bu_write8(adapter, RTL8822BU_REG_BCN_CTRL,
		    (saved->beacon_control & ~RTL8822BU_ENABLE_BEACON) |
		    RTL8822BU_DISABLE_TSF_UPDATE);
	if (error == 0)
		error = rtl8822bu_write8(adapter,
		    RTL8822BU_REG_CPU_DMEM_CON + 2U,
		    saved->cpu_dmem_high & ~1U);
	if (error == 0)
		error = rtl8822bu_write8(adapter, RTL8822BU_REG_SYS_CLKR + 1U,
		    saved->sys_clock_high & ~0x40U);
	if (error == 0)
		error = rtl8822bu_write8(adapter,
		    RTL8822BU_REG_CPU_DMEM_CON + 2U,
		    saved->cpu_dmem_high | 1U);
	if (error == 0)
		error = rtl8822bu_write8(adapter, RTL8822BU_REG_SYS_CLKR + 1U,
		    saved->sys_clock_high | 0x40U);
	if (error == 0)
		error = rtl8822bu_write16(adapter, RTL8822BU_REG_MCUFW_CTRL,
		    (saved->mcufw_control & 0x3800U) |
		    RTL8822BU_MCUFW_DOWNLOAD_ENABLE);
	return error;
}

static int
rtl8822bu_firmware_reserved_page(struct rtl8822bu_firmware_transfer *transfer,
	const struct rtl8822b_firmware_chunk *chunk)
{
	struct rtl8822bu_adapter *adapter = transfer->adapter;
	size_t total, actual = 0U;
	uint16_t page_control;
	int error;

	error = rtl8822b_firmware_tx_descriptor(transfer->wire_buffer,
	    chunk->wire_payload_length);
	if (error != 0)
		return error;
	memcpy(transfer->wire_buffer + RTL8822B_FIRMWARE_TX_DESCRIPTOR_SIZE,
	    transfer->view->bytes + chunk->file_offset, chunk->length);
	if (chunk->wire_payload_length > chunk->length)
		transfer->wire_buffer[RTL8822B_FIRMWARE_TX_DESCRIPTOR_SIZE +
		    chunk->length] = 0U;
	page_control = RTL8822BU_BEACON_VALID;
	error = rtl8822bu_write16(adapter, RTL8822BU_REG_FIFOPAGE_CTRL_2,
	    page_control);
	if (error != 0)
		return error;
	total = RTL8822B_FIRMWARE_TX_DESCRIPTOR_SIZE +
	    chunk->wire_payload_length;
	error = rtl8822bu_bulk_transfer(adapter, adapter->bulk_out_high,
	    transfer->wire_buffer, total,
	    RTL8822BU_FIRMWARE_TRANSFER_TIMEOUT_MS, &actual);
	if (error != 0)
		return error;
	if (actual != total)
		return EIO;
	return rtl8822bu_wait16(adapter, RTL8822BU_REG_FIFOPAGE_CTRL_2,
	    RTL8822BU_BEACON_VALID, RTL8822BU_BEACON_VALID);
}

static int
rtl8822bu_firmware_ddma(struct rtl8822bu_firmware_transfer *transfer,
	const struct rtl8822b_firmware_chunk *chunk)
{
	struct rtl8822bu_adapter *adapter = transfer->adapter;
	uint32_t control;
	int error;

	error = rtl8822bu_wait32(adapter, RTL8822BU_REG_DDMA_CH0CTRL,
	    RTL8822BU_DDMA_OWN, 0U);
	if (error != 0)
		return error;
	control = RTL8822BU_DDMA_OWN | RTL8822BU_DDMA_CHECKSUM_ENABLE |
	    chunk->length;
	if (chunk->checksum_continue)
		control |= RTL8822BU_DDMA_CHECKSUM_CONTINUE;
	error = rtl8822bu_write32(adapter, RTL8822BU_REG_DDMA_CH0SA,
	    RTL8822BU_TX_BUFFER_OCP + RTL8822B_FIRMWARE_TX_DESCRIPTOR_SIZE);
	if (error == 0)
		error = rtl8822bu_write32(adapter, RTL8822BU_REG_DDMA_CH0DA,
		    chunk->destination);
	if (error == 0)
		error = rtl8822bu_write32(adapter, RTL8822BU_REG_DDMA_CH0CTRL,
		    control);
	if (error != 0)
		return error;
	return rtl8822bu_wait32(adapter, RTL8822BU_REG_DDMA_CH0CTRL,
	    RTL8822BU_DDMA_OWN, 0U);
}

static int
rtl8822bu_firmware_chunk(void *context,
	const struct rtl8822b_firmware_chunk *chunk)
{
	struct rtl8822bu_firmware_transfer *transfer = context;
	struct rtl8822bu_adapter *adapter = transfer->adapter;
	uint32_t ddma_control;
	uint16_t firmware_control;
	int error;

	if (chunk == NULL || chunk->length == 0U ||
	    chunk->length > RTL8822B_FIRMWARE_CHUNK_MAX ||
	    (chunk->segment != RTL8822B_FIRMWARE_SEGMENT_DMEM &&
	    chunk->segment != RTL8822B_FIRMWARE_SEGMENT_IMEM) ||
	    chunk->wire_payload_length != chunk->length +
	    (((chunk->length + RTL8822B_FIRMWARE_TX_DESCRIPTOR_SIZE) %
	    RTL8822BU_BULK_MAX_PACKET_SIZE) == 0U) ||
	    chunk->file_offset > transfer->view->size ||
	    chunk->length > transfer->view->size - chunk->file_offset ||
	    chunk->destination > UINT32_MAX - chunk->length)
		return EINVAL;
	if (chunk->first) {
		if (transfer->segment_open || chunk->checksum_continue)
			return EINVAL;
		transfer->segment = chunk->segment;
		transfer->next_file_offset = chunk->file_offset;
		transfer->next_destination = chunk->destination;
		transfer->segment_open = 1U;
		error = rtl8822bu_wait32(adapter, RTL8822BU_REG_DDMA_CH0CTRL,
		    RTL8822BU_DDMA_OWN, 0U);
		if (error == 0)
			error = rtl8822bu_read32(adapter,
			    RTL8822BU_REG_DDMA_CH0CTRL,
			    &ddma_control);
		if (error == 0)
			error = rtl8822bu_write32(adapter,
			    RTL8822BU_REG_DDMA_CH0CTRL,
			    ddma_control | RTL8822BU_DDMA_RESET_CHECKSUM);
		if (error != 0)
			return error;
	} else if (!transfer->segment_open ||
	    transfer->segment != chunk->segment || !chunk->checksum_continue) {
		return EINVAL;
	}
	if (chunk->file_offset != transfer->next_file_offset ||
	    chunk->destination != transfer->next_destination)
		return EINVAL;
	error = rtl8822bu_firmware_reserved_page(transfer, chunk);
	if (error == 0)
		error = rtl8822bu_firmware_ddma(transfer, chunk);
	if (error != 0)
		return error;
	transfer->next_file_offset += chunk->length;
	transfer->next_destination += chunk->length;
	if (!chunk->last)
		return 0;
	error = rtl8822bu_read32(adapter, RTL8822BU_REG_DDMA_CH0CTRL,
	    &ddma_control);
	if (error != 0)
		return error;
	if ((ddma_control & RTL8822BU_DDMA_CHECKSUM_ERROR) != 0U)
		return EILSEQ;
	error = rtl8822bu_read16(adapter, RTL8822BU_REG_MCUFW_CTRL,
	    &firmware_control);
	if (error != 0)
		return error;
	if (chunk->segment == RTL8822B_FIRMWARE_SEGMENT_DMEM) {
		firmware_control |= RTL8822BU_MCUFW_DMEM_DOWNLOAD_OK |
		    RTL8822BU_MCUFW_DMEM_CHECKSUM_OK;
		transfer->dmem_complete = 1U;
	} else if (chunk->segment == RTL8822B_FIRMWARE_SEGMENT_IMEM) {
		firmware_control |= RTL8822BU_MCUFW_IMEM_DOWNLOAD_OK |
		    RTL8822BU_MCUFW_IMEM_CHECKSUM_OK;
		transfer->imem_complete = 1U;
	} else {
		return EINVAL;
	}
	error = rtl8822bu_write16(adapter, RTL8822BU_REG_MCUFW_CTRL,
	    firmware_control);
	if (error == 0)
		transfer->segment_open = 0U;
	return error;
}

static int
rtl8822bu_firmware_download_model(struct rtl8822bu_adapter *adapter,
	const struct rtl8822b_firmware_view *view,
	rtl8822bu_firmware_walk_fn walk)
{
	struct rtl8822bu_firmware_saved_registers saved;
	struct rtl8822bu_firmware_transfer transfer;
	uint16_t firmware_control;
	int error, cleanup_error;

	if (adapter == NULL || view == NULL || view->bytes == NULL || walk == NULL)
		return EINVAL;
	memset(&transfer, 0, sizeof(transfer));
	transfer.adapter = adapter;
	transfer.view = view;
	transfer.wire_buffer = hal_malloc(RTL8822B_FIRMWARE_TX_DESCRIPTOR_SIZE +
	    RTL8822B_FIRMWARE_CHUNK_MAX + 1U);
	if (transfer.wire_buffer == NULL)
		return ENOMEM;
	error = rtl8822bu_firmware_save(adapter, &saved);
	if (error != 0)
		goto out;
	error = rtl8822bu_firmware_prepare(adapter, &saved);
	if (error != 0)
		goto fail_restore;
	error = walk(view, rtl8822bu_firmware_chunk, &transfer);
	if (error == 0 && (transfer.segment_open || !transfer.dmem_complete ||
	    !transfer.imem_complete))
		error = EINVAL;
	if (error != 0)
		goto fail_restore;
	error = rtl8822bu_firmware_restore_transport(adapter, &saved);
	if (error != 0)
		goto fail_state;
	error = rtl8822bu_write32(adapter, RTL8822BU_REG_TXDMA_STATUS,
	    RTL8822BU_TXDMA_PAGE_OVERFLOW);
	if (error != 0)
		goto fail_state;
	error = rtl8822bu_read16(adapter, RTL8822BU_REG_MCUFW_CTRL,
	    &firmware_control);
	if (error != 0)
		goto fail_state;
	if ((firmware_control & (RTL8822BU_MCUFW_DMEM_CHECKSUM_OK |
	    RTL8822BU_MCUFW_IMEM_CHECKSUM_OK)) !=
	    (RTL8822BU_MCUFW_DMEM_CHECKSUM_OK |
	    RTL8822BU_MCUFW_IMEM_CHECKSUM_OK)) {
		error = EILSEQ;
		goto fail_state;
	}
	firmware_control |= RTL8822BU_MCUFW_DOWNLOAD_READY;
	firmware_control &= ~RTL8822BU_MCUFW_DOWNLOAD_ENABLE;
	error = rtl8822bu_write16(adapter, RTL8822BU_REG_MCUFW_CTRL,
	    firmware_control);
	if (error == 0)
		error = rtl8822bu_write8(adapter, RTL8822BU_REG_RSV_CTRL + 1U,
		    saved.reserved_control_high | RTL8822BU_WCPU_IO_ENABLE);
	if (error == 0)
		error = rtl8822bu_write8(adapter,
		    RTL8822BU_REG_SYS_FUNC_EN + 1U,
		    saved.sys_func_high | RTL8822BU_WCPU_ENABLE);
	if (error == 0)
		error = rtl8822bu_wait16(adapter, RTL8822BU_REG_MCUFW_CTRL,
		    RTL8822BU_MCUFW_READY_MASK, RTL8822BU_MCUFW_READY);
	if (error == 0)
		goto out;

fail_state:
	cleanup_error = rtl8822bu_write16(adapter, RTL8822BU_REG_MCUFW_CTRL,
	    saved.mcufw_control);
	rtl8822bu_record_cleanup_error(&error, cleanup_error);
	cleanup_error = rtl8822bu_write8(adapter,
	    RTL8822BU_REG_CPU_DMEM_CON + 2U, saved.cpu_dmem_high);
	rtl8822bu_record_cleanup_error(&error, cleanup_error);
	cleanup_error = rtl8822bu_write8(adapter, RTL8822BU_REG_SYS_CLKR + 1U,
	    saved.sys_clock_high);
	rtl8822bu_record_cleanup_error(&error, cleanup_error);
	cleanup_error = rtl8822bu_write8(adapter, RTL8822BU_REG_RSV_CTRL + 1U,
	    saved.reserved_control_high);
	rtl8822bu_record_cleanup_error(&error, cleanup_error);
	cleanup_error = rtl8822bu_write8(adapter,
	    RTL8822BU_REG_SYS_FUNC_EN + 1U, saved.sys_func_high);
	rtl8822bu_record_cleanup_error(&error, cleanup_error);
	goto out;

fail_restore:
	error = rtl8822bu_firmware_restore_failure(adapter, &saved, error);
out:
	memset(transfer.wire_buffer, 0,
	    RTL8822B_FIRMWARE_TX_DESCRIPTOR_SIZE +
	    RTL8822B_FIRMWARE_CHUNK_MAX + 1U);
	hal_free(transfer.wire_buffer);
	memset(&transfer, 0, sizeof(transfer));
	memset(&saved, 0, sizeof(saved));
	return error;
}

static int __attribute__((unused))
rtl8822bu_firmware_download(struct rtl8822bu_adapter *adapter,
	const struct rtl8822b_firmware_view *view)
{
	return rtl8822bu_firmware_download_model(adapter, view,
	    RTL8822BU_FIRMWARE_WALK);
}

static int
rtl8822bu_ready_station(struct rtl8822bu_adapter *adapter,
	struct wlan_station **station)
{
	unsigned long enabled;
	int ready;

	enabled = spin_lock_irqsave(&adapter->lock);
	ready = adapter->ready && !adapter->detaching &&
	    adapter->station != NULL;
	if (station != NULL)
		*station = ready ? adapter->station : NULL;
	spin_unlock_irqrestore(&adapter->lock, enabled);
	return ready;
}

static int
rtl8822bu_urb_status_error(enum drv_usb_urb_status status)
{
	if (status == DRV_USB_URB_COMPLETE)
		return 0;
	if (status == DRV_USB_URB_TIMEOUT)
		return ETIMEDOUT;
	if (status == DRV_USB_URB_STALL)
		return EPIPE;
	if (status == DRV_USB_URB_DISCONNECTED)
		return ENODEV;
	return EIO;
}

static unsigned *
rtl8822bu_sync_error_streak_locked(struct rtl8822bu_adapter *adapter,
	const struct drv_usb_endpoint *endpoint)
{
	if (endpoint == NULL)
		return &adapter->control_error_streak;
	if (endpoint == adapter->bulk_out_high)
		return &adapter->tx_high_error_streak;
	if (endpoint == adapter->bulk_out_normal)
		return &adapter->tx_normal_error_streak;
	return &adapter->tx_low_error_streak;
}

static void
rtl8822bu_sync_endpoint_result(struct rtl8822bu_adapter *adapter,
	struct drv_usb_endpoint *endpoint, int error)
{
	unsigned long enabled;
	unsigned *streak;
	int schedule = 0;

	if (adapter == NULL)
		return;
	enabled = spin_lock_irqsave(&adapter->lock);
	streak = rtl8822bu_sync_error_streak_locked(adapter, endpoint);
	if (error == 0) {
		*streak = 0U;
	} else if (error == ENODEV) {
		adapter->quarantined = 1U;
	} else {
		if (*streak != UINT_MAX)
			(*streak)++;
		if (*streak >= RTL8822BU_RX_RECOVERY_LIMIT || error == EPIPE) {
			adapter->recovery_pending = 1U;
			adapter->recovery_error = error;
			adapter->tx_quiescing = 1U;
			schedule = adapter->ready && !adapter->detaching &&
			    adapter->opened &&
			    !adapter->closing && !adapter->stopping &&
			    adapter->radio_operations_active == 0U &&
			    adapter->net_device != NULL;
		}
	}
	spin_unlock_irqrestore(&adapter->lock, enabled);
	if (schedule)
		net_device_schedule_poll(adapter->net_device);
}

/* Synchronous bulk STALL means that request was rejected by the endpoint and
 * is therefore safe to retry once after a local CLEAR_FEATURE(ENDPOINT_HALT).
 * Timeouts and short transfers are not retried: completion may be ambiguous,
 * so the caller reports failure and the bounded endpoint streak eventually
 * promotes the device to local firmware recovery. */
static int
rtl8822bu_bulk_transfer(struct rtl8822bu_adapter *adapter,
	struct drv_usb_endpoint *endpoint, void *buffer, size_t length,
	unsigned timeout_ms, size_t *actual)
{
	int error;
	int clear_error;

	if (actual != NULL)
		*actual = 0U;
	error = drv_usb_bulk(adapter->usb_device, endpoint, buffer, length,
	    timeout_ms, actual);
	if (error == EPIPE) {
		clear_error = drv_usb_endpoint_clear_halt(endpoint);
		if (clear_error == 0) {
			if (actual != NULL)
				*actual = 0U;
			error = drv_usb_bulk(adapter->usb_device, endpoint, buffer,
			    length, timeout_ms, actual);
		} else {
			error = clear_error;
		}
	}
	if (error == 0 && actual != NULL && *actual != length)
		error = EIO;
	rtl8822bu_sync_endpoint_result(adapter, endpoint, error);
	return error;
}

static void
rtl8822bu_rx_completion(struct drv_usb_urb *urb, void *argument)
{
	struct rtl8822bu_rx_completion_context *completion = argument;
	struct rtl8822bu_adapter *adapter;
	unsigned long enabled;
	int schedule = 0;

	if (completion == NULL || completion->adapter == NULL)
		return;
	adapter = completion->adapter;
	if (urb != adapter->rx_urb)
		return;
	enabled = spin_lock_irqsave(&adapter->lock);
	if (completion->generation != 0U &&
	    completion->generation == adapter->rx_inflight_generation &&
	    adapter->ready && !adapter->detaching && adapter->opened &&
	    !adapter->closing &&
	    !adapter->rx_generation_barrier &&
	    !adapter->stopping && (!adapter->recovery_pending ||
	    adapter->recovery_active) &&
	    !adapter->quarantined) {
		adapter->rx_completed_generation = completion->generation;
		adapter->rx_ready = 1U;
		schedule = 1;
	}
	spin_unlock_irqrestore(&adapter->lock, enabled);
	if (schedule)
		net_device_schedule_poll(adapter->net_device);
}

static int
rtl8822bu_rx_submit(struct rtl8822bu_adapter *adapter, int close_on_error)
{
	struct rtl8822bu_rx_completion_context *completion;
	unsigned long enabled;
	uint64_t generation;
	int cancel = 0, error;

	enabled = spin_lock_irqsave(&adapter->lock);
	if (!adapter->ready || adapter->detaching || !adapter->opened ||
	    adapter->closing ||
	    adapter->rx_generation_barrier ||
	    adapter->stopping || (adapter->recovery_pending &&
	    !adapter->recovery_active) ||
	    adapter->quarantined) {
		spin_unlock_irqrestore(&adapter->lock, enabled);
		return ENETDOWN;
	}
	generation = adapter->rx_submit_generation + 1U;
	if (generation == 0U)
		generation = 1U;
	adapter->rx_submit_generation = generation;
	adapter->rx_inflight_generation = generation;
	adapter->rx_completed_generation = 0U;
	completion = &adapter->rx_completion[generation & 1U];
	completion->adapter = adapter;
	completion->generation = generation;
	adapter->starts_active++;
	spin_unlock_irqrestore(&adapter->lock, enabled);
	error = drv_usb_urb_setup(adapter->rx_urb, adapter->rx_buffer,
	    RTL8822BU_RX_BUFFER_SIZE, 0U, 0U, rtl8822bu_rx_completion,
	    completion);
	if (error == 0)
		error = drv_usb_urb_submit(adapter->rx_urb);
	enabled = spin_lock_irqsave(&adapter->lock);
	if (error == 0 && (!adapter->ready || adapter->detaching ||
	    !adapter->opened ||
	    adapter->closing || adapter->stopping ||
	    (adapter->recovery_pending && !adapter->recovery_active))) {
		error = ENETDOWN;
		cancel = 1;
	}
	spin_unlock_irqrestore(&adapter->lock, enabled);
	/* starts_active remains owned through the last URB dereference.  stop and
	 * detach may close admission concurrently, but cannot free this object
	 * until status/cancel has completed outside the spin lock. */
	if (cancel &&
	    drv_usb_urb_status(adapter->rx_urb) == DRV_USB_URB_PENDING)
		(void)drv_usb_urb_cancel(adapter->rx_urb);
	enabled = spin_lock_irqsave(&adapter->lock);
	if (adapter->starts_active == 0U)
		__builtin_trap();
	if (error != 0 && close_on_error)
		adapter->opened = 0U;
	if (error != 0 && adapter->rx_inflight_generation == generation) {
		adapter->rx_inflight_generation = 0U;
		adapter->rx_completed_generation = 0U;
	}
	adapter->starts_active--;
	spin_unlock_irqrestore(&adapter->lock, enabled);
	return error;
}

static int
rtl8822bu_poll_enter(struct rtl8822bu_adapter *adapter)
{
	unsigned long enabled = spin_lock_irqsave(&adapter->lock);
	int admitted = adapter->ready && !adapter->detaching &&
	    adapter->opened && !adapter->closing &&
	    !adapter->rx_generation_barrier &&
	    !adapter->stopping && !adapter->recovery_pending &&
	    !adapter->quarantined;

	if (admitted)
		adapter->polls_active++;
	spin_unlock_irqrestore(&adapter->lock, enabled);
	return admitted;
}

static void
rtl8822bu_poll_exit(struct rtl8822bu_adapter *adapter)
{
	unsigned long enabled = spin_lock_irqsave(&adapter->lock);

	if (adapter->polls_active == 0U)
		__builtin_trap();
	adapter->polls_active--;
	spin_unlock_irqrestore(&adapter->lock, enabled);
}

static void
rtl8822bu_wait_activity(struct rtl8822bu_adapter *adapter)
{
	for (;;) {
		unsigned long enabled = spin_lock_irqsave(&adapter->lock);
		unsigned active = adapter->starts_active + adapter->polls_active +
		    adapter->radio_operations_active;

		spin_unlock_irqrestore(&adapter->lock, enabled);
		if (active == 0U)
			return;
		sched_yield();
	}
}

static int
rtl8822bu_rx_stop(struct rtl8822bu_adapter *adapter)
{
	enum drv_usb_urb_status status;
	unsigned long enabled;
	int error;

	if (adapter == NULL || adapter->rx_urb == NULL)
		return 0;
	enabled = spin_lock_irqsave(&adapter->lock);
	adapter->closing = 1U;
	adapter->stopping = 1U;
	adapter->opened = 0U;
	adapter->rx_ready = 0U;
	adapter->rx_rearm = 0U;
	adapter->rx_inflight_generation = 0U;
	adapter->rx_completed_generation = 0U;
	adapter->rx_generation_barrier = 0U;
	spin_unlock_irqrestore(&adapter->lock, enabled);
	rtl8822bu_wait_activity(adapter);
	status = drv_usb_urb_status(adapter->rx_urb);
	if (status == DRV_USB_URB_PENDING)
		(void)drv_usb_urb_cancel(adapter->rx_urb);
	error = drv_usb_urb_drain(adapter->rx_urb,
	    RTL8822BU_RX_DRAIN_TIMEOUT_MS);
	enabled = spin_lock_irqsave(&adapter->lock);
	adapter->stopping = 0U;
	adapter->rx_rearm_active = 0U;
	if (error != 0)
		adapter->quarantined = 1U;
	else {
		adapter->closing = 0U;
		adapter->rx_error_streak = 0U;
	}
	spin_unlock_irqrestore(&adapter->lock, enabled);
	return error;
}

/* Retire the complete old RX producer generation before a CAM generation is
 * published.  A poll/start already admitted by the old generation is never
 * waited from inside a radio callback: the common worker receives EBUSY and
 * retries after that bounded producer retires. */
static int
rtl8822bu_rx_generation_pause(struct rtl8822bu_adapter *adapter,
	uint64_t deadline)
{
	enum drv_usb_urb_status status;
	unsigned long enabled;
	int error;

	if (adapter == NULL || adapter->rx_urb == NULL)
		return ENODEV;
	enabled = spin_lock_irqsave(&adapter->lock);
	if (!adapter->ready || adapter->detaching || !adapter->opened ||
	    adapter->closing ||
	    adapter->stopping || adapter->recovery_pending ||
	    adapter->quarantined) {
		error = adapter->ready ? ENETDOWN : ENODEV;
	} else if (adapter->rx_generation_barrier ||
	    adapter->starts_active != 0U || adapter->polls_active != 0U) {
		error = EBUSY;
	} else {
		adapter->rx_generation_barrier = 1U;
		adapter->rx_ready = 0U;
		adapter->rx_rearm = 0U;
		adapter->rx_inflight_generation = 0U;
		adapter->rx_completed_generation = 0U;
		error = 0;
	}
	spin_unlock_irqrestore(&adapter->lock, enabled);
	if (error != 0)
		return error;
	/* Stop MAC/HCI RX and release the device-side RXDMA/USB FIFO generation
	 * before cancelling the host URB.  Host drain alone cannot prove that an
	 * old-key frame was not already queued inside the device. */
	error = rtl8822b_radio_rx_generation_pause(&adapter->radio, deadline);
	if (error != 0) {
		enabled = spin_lock_irqsave(&adapter->lock);
		adapter->quarantined = 1U;
		spin_unlock_irqrestore(&adapter->lock, enabled);
		return error;
	}
	status = drv_usb_urb_status(adapter->rx_urb);
	if (status == DRV_USB_URB_PENDING)
		(void)drv_usb_urb_cancel(adapter->rx_urb);
	error = drv_usb_urb_drain(adapter->rx_urb,
	    RTL8822BU_RX_DRAIN_TIMEOUT_MS);
	if (error != 0) {
		enabled = spin_lock_irqsave(&adapter->lock);
		adapter->quarantined = 1U;
		spin_unlock_irqrestore(&adapter->lock, enabled);
	}
	return error;
}

static int
rtl8822bu_rx_generation_resume(struct rtl8822bu_adapter *adapter,
	uint64_t deadline)
{
	enum drv_usb_urb_status status;
	unsigned long enabled;
	int error;

	enabled = spin_lock_irqsave(&adapter->lock);
	if (!adapter->rx_generation_barrier) {
		error = EINVAL;
	} else if (!adapter->ready || adapter->detaching ||
	    !adapter->opened || adapter->closing ||
	    adapter->stopping || adapter->recovery_pending ||
	    adapter->quarantined) {
		error = adapter->ready ? ENETDOWN : ENODEV;
	} else {
		adapter->rx_generation_barrier = 0U;
		error = 0;
	}
	spin_unlock_irqrestore(&adapter->lock, enabled);
	if (error == 0)
		error = rtl8822bu_rx_submit(adapter, 0);
	if (error == 0)
		error = rtl8822b_radio_rx_generation_resume(&adapter->radio,
		    deadline);
	if (error != 0) {
		enabled = spin_lock_irqsave(&adapter->lock);
		adapter->rx_generation_barrier = 1U;
		adapter->rx_ready = 0U;
		adapter->rx_rearm = 0U;
		adapter->rx_inflight_generation = 0U;
		adapter->rx_completed_generation = 0U;
		adapter->quarantined = 1U;
		spin_unlock_irqrestore(&adapter->lock, enabled);
		status = drv_usb_urb_status(adapter->rx_urb);
		if (status == DRV_USB_URB_PENDING)
			(void)drv_usb_urb_cancel(adapter->rx_urb);
		(void)drv_usb_urb_drain(adapter->rx_urb,
		    RTL8822BU_RX_DRAIN_TIMEOUT_MS);
	}
	return error;
}

static int
rtl8822bu_rx_start(struct rtl8822bu_adapter *adapter, uint64_t generation,
	uint32_t channel)
{
	unsigned long enabled;

	if (adapter == NULL || channel == 0U || channel > UINT8_MAX)
		return EINVAL;
	enabled = spin_lock_irqsave(&adapter->lock);
	if (!adapter->ready || adapter->detaching || adapter->opened ||
	    adapter->closing ||
	    adapter->stopping || adapter->quarantined) {
		spin_unlock_irqrestore(&adapter->lock, enabled);
		return adapter->ready ? EBUSY : ENODEV;
	}
	adapter->opened = 1U;
	adapter->scan_generation = generation;
	adapter->scan_channel = channel;
	spin_unlock_irqrestore(&adapter->lock, enabled);
	return rtl8822bu_rx_submit(adapter, 1);
}

/* lifecycle_lock is held.  The common WLAN station is quiesced before this
 * helper runs, so no channel/TX callback can race the checked hardware stop. */
static int
rtl8822bu_hardware_stop_locked(struct rtl8822bu_adapter *adapter)
{
	uint64_t deadline;
	uint64_t now;
	unsigned long enabled;
	int error, security_error = 0, stop_error;

	error = rtl8822bu_rx_stop(adapter);
	enabled = spin_lock_irqsave(&adapter->lock);
	if (adapter->transport_absent) {
		/* FORCE detach arrives after the USB core has permanently closed
		 * transfers for this device generation.  A drained host producer plus
		 * physical absence is a stronger hardware-absence proof than any
		 * register inverse; scrub the complete local radio/CAM ledger without
		 * touching EP0 or an OUT endpoint. */
		if (error == 0) {
			memset(&adapter->radio, 0, sizeof(adapter->radio));
			adapter->radio_running = 0U;
			adapter->firmware_running = 0U;
			rtl8822bu_connection_state_clear_locked(adapter);
			memset(adapter->tx_reports, 0,
			    sizeof(adapter->tx_reports));
			adapter->tx_report_next = 0U;
			adapter->quarantined = 0U;
			adapter->recovery_pending = 0U;
			adapter->recovery_active = 0U;
			adapter->recovery_error = 0;
			adapter->recovery_cleanup_attempts = 0U;
			adapter->rx_error_streak = 0U;
			adapter->control_error_streak = 0U;
			adapter->tx_high_error_streak = 0U;
			adapter->tx_normal_error_streak = 0U;
			adapter->tx_low_error_streak = 0U;
			adapter->tx_report_error_streak = 0U;
			adapter->tx_report_tombstone_count = 0U;
			adapter->rx_submit_generation = 0U;
			adapter->rx_inflight_generation = 0U;
			adapter->rx_completed_generation = 0U;
			memset(adapter->rx_completion, 0,
			    sizeof(adapter->rx_completion));
			adapter->rx_generation_barrier = 0U;
			adapter->scan_generation = 0U;
			adapter->scan_channel = 0U;
		} else {
			adapter->quarantined = 1U;
		}
		spin_unlock_irqrestore(&adapter->lock, enabled);
		return error;
	}
	/* opened=0 already closes every normal TX path.  Keep an explicit gate
	 * across the queue snapshot and any best-effort CAM/BSSID cleanup so this
	 * ordering remains local even if stop admission changes later. */
	adapter->tx_quiescing = 1U;
	spin_unlock_irqrestore(&adapter->lock, enabled);
	if (rtl8822bu_deadline_after(RTL8822BU_RADIO_STOP_TIMEOUT_TICKS,
	    &deadline) != 0) {
		stop_error = EOVERFLOW;
	} else {
		now = clock_ticks();
		enabled = spin_lock_irqsave(&adapter->lock);
		rtl8822bu_tx_report_reap_locked(adapter, now);
		if (adapter->connection_generation != 0U &&
		    adapter->radio.state == RTL8822B_RADIO_STARTED) {
			security_error = rtl8822bu_tx_report_generation_active_locked(
			    adapter, adapter->connection_generation, 0U, 0) ?
			    EBUSY : EINPROGRESS;
		}
		spin_unlock_irqrestore(&adapter->lock, enabled);
		if (security_error == EINPROGRESS)
			security_error = rtl8822bu_security_hardware_clear(adapter,
			    deadline);
		stop_error = rtl8822b_radio_stop(&adapter->radio, deadline);
	}
	enabled = spin_lock_irqsave(&adapter->lock);
	if (stop_error == 0) {
		/* A checked radio stop is the final absence barrier for BSSID/CAM
		 * state, even when an earlier best-effort per-entry clear failed. */
		adapter->radio_running = 0U;
		adapter->firmware_running = 0U;
		rtl8822bu_connection_state_clear_locked(adapter);
		memset(adapter->tx_reports, 0, sizeof(adapter->tx_reports));
		adapter->tx_report_next = 0U;
		adapter->tx_quiescing = 0U;
		adapter->quarantined = 0U;
		adapter->recovery_pending = 0U;
		adapter->recovery_error = 0;
		adapter->recovery_cleanup_attempts = 0U;
		adapter->rx_error_streak = 0U;
		adapter->control_error_streak = 0U;
		adapter->tx_high_error_streak = 0U;
		adapter->tx_normal_error_streak = 0U;
		adapter->tx_low_error_streak = 0U;
		adapter->tx_report_error_streak = 0U;
		adapter->tx_report_tombstone_count = 0U;
	} else {
		adapter->tx_quiescing = 1U;
		adapter->quarantined = 1U;
	}
	adapter->scan_generation = 0U;
	adapter->scan_channel = 0U;
	spin_unlock_irqrestore(&adapter->lock, enabled);
	/* A successful terminal stop supersedes an intermediate CAM-clear error;
	 * it verified a stronger reset boundary. */
	if (stop_error != 0)
		rtl8822bu_record_cleanup_error(&error, security_error);
	rtl8822bu_record_cleanup_error(&error, stop_error);
	return error;
}

static int
rtl8822bu_tx_report_generation_active_locked(
	const struct rtl8822bu_adapter *adapter, uint64_t generation,
	uint64_t key_generation, int match_key_generation)
{
	unsigned index;

	for (index = 0U; index < RTL8822BU_TX_REPORT_COUNT; index++)
		if (adapter->tx_reports[index].active &&
		    adapter->tx_reports[index].connection_generation == generation &&
		    (!match_key_generation ||
		    adapter->tx_reports[index].key_generation == key_generation))
			return 1;
	return 0;
}

static void
rtl8822bu_tx_report_reap_locked(struct rtl8822bu_adapter *adapter,
	uint64_t now)
{
	unsigned expired = 0U;
	unsigned index;

	for (index = 0U; index < RTL8822BU_TX_REPORT_COUNT; index++)
		if (adapter->tx_reports[index].active &&
		    now >= adapter->tx_reports[index].retire_deadline_ticks) {
			memset(&adapter->tx_reports[index], 0,
			    sizeof(adapter->tx_reports[index]));
			adapter->tx_reports[index].tombstone = 1U;
			expired++;
		}
	while (expired-- != 0U) {
		if (adapter->tx_report_error_streak != UINT_MAX)
			adapter->tx_report_error_streak++;
		if (adapter->tx_report_tombstone_count != UINT_MAX)
			adapter->tx_report_tombstone_count++;
	}
	if (adapter->tx_report_tombstone_count >= RTL8822BU_RX_RECOVERY_LIMIT &&
	    adapter->ready && !adapter->detaching && adapter->opened &&
	    !adapter->closing && !adapter->stopping && adapter->radio_running) {
		/* USB bulk completion only proves host-to-device delivery.  Missing
		 * bounded CCX reports are the observable firmware-stall boundary. */
		adapter->recovery_pending = 1U;
		adapter->recovery_error = ETIMEDOUT;
		adapter->tx_quiescing = 1U;
	}
}

static int
rtl8822bu_tx_report_reserve_locked(struct rtl8822bu_adapter *adapter,
	uint64_t generation, uint64_t key_generation, uint64_t cookie,
	uint64_t now, uint64_t deadline, uint8_t *sequence)
{
	uint64_t retire_deadline;
	unsigned count;
	unsigned index;

	/* A missing firmware report must not consume one of the finite 6-bit
	 * CCX sequence slots forever.  Retain it for at least the Linux rtw88
	 * 500-ms report window as a bounded MAC-queue/key-use drain, even when
	 * the common transaction has a shorter deadline. */
	rtl8822bu_tx_report_reap_locked(adapter, now);
	if (adapter->recovery_pending)
		return ENETDOWN;
	retire_deadline = now > UINT64_MAX -
	    RTL8822BU_TX_REPORT_RETIRE_TICKS ? UINT64_MAX :
	    now + RTL8822BU_TX_REPORT_RETIRE_TICKS;
	if (retire_deadline < deadline)
		retire_deadline = deadline;
	for (count = 0U; count < RTL8822BU_TX_REPORT_COUNT; count++) {
		index = (adapter->tx_report_next + count) %
		    RTL8822BU_TX_REPORT_COUNT;
		if (adapter->tx_reports[index].active ||
		    adapter->tx_reports[index].tombstone)
			continue;
		adapter->tx_reports[index].active = 1U;
		adapter->tx_reports[index].cookie = cookie;
		adapter->tx_reports[index].connection_generation = generation;
		adapter->tx_reports[index].key_generation = key_generation;
		adapter->tx_reports[index].deadline_ticks = deadline;
		adapter->tx_reports[index].retire_deadline_ticks = retire_deadline;
		adapter->tx_report_next = (uint8_t)((index + 1U) %
		    RTL8822BU_TX_REPORT_COUNT);
		*sequence = (uint8_t)(index * RTL8822BU_TX_REPORT_SEQUENCE_STEP);
		return 0;
	}
	adapter->recovery_pending = 1U;
	adapter->recovery_error = ENOSPC;
	adapter->tx_quiescing = 1U;
	return ENOSPC;
}

static void
rtl8822bu_tx_report_release(struct rtl8822bu_adapter *adapter,
	uint8_t sequence)
{
	unsigned long enabled;
	unsigned index = sequence / RTL8822BU_TX_REPORT_SEQUENCE_STEP;

	if ((sequence & (RTL8822BU_TX_REPORT_SEQUENCE_STEP - 1U)) != 0U ||
	    index >= RTL8822BU_TX_REPORT_COUNT)
		return;
	enabled = spin_lock_irqsave(&adapter->lock);
	memset(&adapter->tx_reports[index], 0,
	    sizeof(adapter->tx_reports[index]));
	spin_unlock_irqrestore(&adapter->lock, enabled);
}

static void
rtl8822bu_tx_report_abandon_attempted(struct rtl8822bu_adapter *adapter,
	uint8_t sequence, int error)
{
	unsigned long enabled;
	unsigned index = sequence / RTL8822BU_TX_REPORT_SEQUENCE_STEP;

	if ((sequence & (RTL8822BU_TX_REPORT_SEQUENCE_STEP - 1U)) != 0U ||
	    index >= RTL8822BU_TX_REPORT_COUNT)
		return;
	enabled = spin_lock_irqsave(&adapter->lock);
	if (adapter->tx_reports[index].active) {
		/* Once bulk OUT was attempted, timeout/short/STALL cannot prove that
		 * firmware did not retain the CCX sequence.  Never recycle it until a
		 * checked hardware reset makes every late report impossible. */
		memset(&adapter->tx_reports[index], 0,
		    sizeof(adapter->tx_reports[index]));
		adapter->tx_reports[index].tombstone = 1U;
		if (adapter->tx_report_error_streak != UINT_MAX)
			adapter->tx_report_error_streak++;
		if (adapter->tx_report_tombstone_count != UINT_MAX)
			adapter->tx_report_tombstone_count++;
		if (adapter->tx_report_tombstone_count >=
		    RTL8822BU_RX_RECOVERY_LIMIT && adapter->ready &&
		    !adapter->detaching && adapter->opened && !adapter->closing &&
		    !adapter->stopping && adapter->radio_running) {
			adapter->recovery_pending = 1U;
			adapter->recovery_error = error != 0 ? error : EIO;
			adapter->tx_quiescing = 1U;
		}
	}
	spin_unlock_irqrestore(&adapter->lock, enabled);
}

static int
rtl8822bu_c2h_tx_report_decode(const struct rtl8822b_rx_packet *packet,
	uint8_t *sequence, int *tx_error)
{
	uint8_t status;

	if (packet == NULL || sequence == NULL || tx_error == NULL ||
	    packet->kind != RTL8822B_RX_C2H || packet->payload == NULL)
		return EINVAL;
	if (packet->payload_length == 0U ||
	    packet->payload[0] != packet->c2h_id)
		return EILSEQ;
	if (packet->c2h_id == RTL8822BU_C2H_CCX_TX_REPORT_ID) {
		if (packet->payload_length < 9U)
			return EILSEQ;
		status = packet->payload[2U] & 0xc0U;
		*sequence = packet->payload[8U] & 0xfcU;
	} else if (packet->c2h_id == RTL8822BU_C2H_EXTENDED_ID) {
		if (packet->payload_length < 12U || packet->payload[2U] !=
		    RTL8822BU_C2H_EXTENDED_CCX_REPORT_ID)
			return packet->payload_length < 3U ? EILSEQ : ENOENT;
		status = packet->payload[11U] & 0xc0U;
		*sequence = packet->payload[10U] & 0xfcU;
	} else {
		return ENOENT;
	}
	*tx_error = status == 0U ? 0 : EIO;
	return 0;
}

static int
rtl8822bu_tx_report_complete(struct rtl8822bu_adapter *adapter,
	const struct rtl8822b_rx_packet *packet,
	struct rtl8822bu_rx_private *result)
{
	struct rtl8822bu_tx_report_slot pending;
	unsigned long enabled;
	unsigned index;
	uint64_t now;
	uint8_t sequence;
	int error;
	int tx_error;

	error = rtl8822bu_c2h_tx_report_decode(packet, &sequence, &tx_error);
	if (error != 0)
		return error;
	index = sequence / RTL8822BU_TX_REPORT_SEQUENCE_STEP;
	if (index >= RTL8822BU_TX_REPORT_COUNT)
		return EILSEQ;
	now = clock_ticks();
	enabled = spin_lock_irqsave(&adapter->lock);
	pending = adapter->tx_reports[index];
	if (!pending.active) {
		spin_unlock_irqrestore(&adapter->lock, enabled);
		return ESTALE;
	}
	memset(&adapter->tx_reports[index], 0,
	    sizeof(adapter->tx_reports[index]));
	if (now < pending.deadline_ticks && tx_error == 0)
		adapter->tx_report_error_streak = 0U;
	spin_unlock_irqrestore(&adapter->lock, enabled);
	memset(result, 0, sizeof(*result));
	result->class = RTL8822BU_RX_TX_REPORT;
	result->connection_generation = pending.connection_generation;
	result->key_generation = pending.key_generation;
	result->cookie = pending.cookie;
	/* A late success cannot resurrect a common transaction which has already
	 * crossed the deadline used to allocate this correlation slot. */
	result->tx_error = now >= pending.deadline_ticks ? ETIMEDOUT : tx_error;
	return 0;
}

static int
rtl8822bu_tx_descriptor_set_priority(uint8_t *wire, size_t wire_length,
	enum rtl8822bu_frame_class class, uint8_t channel)
{
	uint32_t word1;
	uint32_t word4;
	uint16_t checksum = 0U;
	unsigned index;

	if (wire == NULL || wire_length < RTL8822B_DATA_TX_DESCRIPTOR_SIZE)
		return EINVAL;
	if (class == RTL8822BU_FRAME_DATA)
		return 0;
	if (class != RTL8822BU_FRAME_MANAGEMENT &&
	    class != RTL8822BU_FRAME_EAPOL)
		return EINVAL;
	word1 = rtl8822bu_load_le32(wire + 4U);
	word1 &= ~((uint32_t)0x1fU << 8);
	word1 |= 18U << 8;
	word1 &= ~((uint32_t)0x1fU << 16);
	word1 |= 8U << 16;
	rtl8822bu_store_le32(wire + 4U, word1);
	/* The common descriptor starts at 6 Mbps.  On 2.4 GHz, authentication
	 * and association management frames must use the 1 Mbps basic rate just
	 * like the already working active-scan path and upstream rtw88. */
	if (class == RTL8822BU_FRAME_MANAGEMENT && channel <= 14U) {
		word4 = rtl8822bu_load_le32(wire + 16U);
		word4 &= ~0x7fU;
		rtl8822bu_store_le32(wire + 16U, word4);
	}
	rtl8822bu_store_le16(wire + 28U, 0U);
	for (index = 0U; index < 16U; index++)
		checksum ^= rtl8822bu_load_le16(wire + index * 2U);
	rtl8822bu_store_le16(wire + 28U, checksum);
	return 0;
}

static int
rtl8822bu_tx_descriptor_request_report(uint8_t *wire, size_t wire_length,
	uint8_t sequence)
{
	uint32_t word2;
	uint32_t word6;
	uint16_t checksum = 0U;
	unsigned index;

	if (wire == NULL || wire_length < RTL8822B_DATA_TX_DESCRIPTOR_SIZE ||
	    (sequence & (RTL8822BU_TX_REPORT_SEQUENCE_STEP - 1U)) != 0U)
		return EINVAL;
	word2 = rtl8822bu_load_le32(wire + 8U) | (1U << 19);
	word6 = rtl8822bu_load_le32(wire + 24U);
	word6 = (word6 & ~0x0fffU) | sequence;
	rtl8822bu_store_le32(wire + 8U, word2);
	rtl8822bu_store_le32(wire + 24U, word6);
	rtl8822bu_store_le16(wire + 28U, 0U);
	for (index = 0U; index < 16U; index++)
		checksum ^= rtl8822bu_load_le16(wire + index * 2U);
	rtl8822bu_store_le16(wire + 28U, checksum);
	return 0;
}

static int
rtl8822bu_frame_transmit_private(struct rtl8822bu_adapter *adapter,
	uint64_t generation, enum rtl8822bu_frame_class class,
	const uint8_t *frame, size_t length, int encrypted, uint8_t key_index,
	uint64_t key_generation, uint64_t packet_number, uint64_t cookie,
	uint64_t deadline)
{
	struct drv_usb_endpoint *endpoint;
	uint8_t *wire;
	unsigned long enabled;
	uint64_t now;
	uint64_t milliseconds;
	uint64_t remaining;
	size_t capacity;
	size_t wire_length = 0U;
	size_t actual = 0U;
	uint8_t sequence = 0U;
	uint8_t channel = 0U;
	int transfer_attempted = 0;
	int error;

	if (adapter == NULL || generation == 0U || cookie == 0U || frame == NULL ||
	    length < 24U || length > RTL8822B_DATA_MPDU_MAX ||
	    (class != RTL8822BU_FRAME_MANAGEMENT &&
	    class != RTL8822BU_FRAME_EAPOL && class != RTL8822BU_FRAME_DATA) ||
	    (encrypted != 0 && encrypted != 1) || key_index >= 4U)
		return EINVAL;
	now = clock_ticks();
	if ((class == RTL8822BU_FRAME_MANAGEMENT && encrypted) ||
	    (class == RTL8822BU_FRAME_DATA && !encrypted) ||
	    (!encrypted && (key_generation != 0U || packet_number != 0U)) ||
	    (encrypted && (key_generation == 0U || packet_number == 0U ||
	    packet_number > 0x0000ffffffffffffULL)))
		return EINVAL;
	if ((class == RTL8822BU_FRAME_MANAGEMENT &&
	    (rtl8822bu_load_le16(frame) & 0x000cU) != 0U) ||
	    (class != RTL8822BU_FRAME_MANAGEMENT &&
	    (rtl8822bu_load_le16(frame) & 0x000cU) != 0x0008U))
		return EINVAL;
	if (encrypted) {
		uint64_t header_packet_number;

		if (length < 32U ||
		    (rtl8822bu_load_le16(frame) & 0x4000U) == 0U ||
		    frame[26U] != 0U || (frame[27U] & 0x3fU) != 0x20U ||
		    frame[27U] >> 6 != key_index)
			return EINVAL;
		header_packet_number = (uint64_t)frame[24U] |
		    ((uint64_t)frame[25U] << 8) |
		    ((uint64_t)frame[28U] << 16) |
		    ((uint64_t)frame[29U] << 24) |
		    ((uint64_t)frame[30U] << 32) |
		    ((uint64_t)frame[31U] << 40);
		if (header_packet_number != packet_number)
			return EINVAL;
	} else if ((rtl8822bu_load_le16(frame) & 0x4000U) != 0U) {
		return EINVAL;
	}
	enabled = spin_lock_irqsave(&adapter->lock);
	if (!adapter->ready || adapter->detaching || !adapter->opened ||
	    adapter->closing ||
	    adapter->stopping || adapter->recovery_pending ||
	    adapter->quarantined ||
	    !adapter->radio_running) {
		error = adapter->ready ? ENETDOWN : ENODEV;
	} else if (adapter->tx_quiescing) {
		error = EBUSY;
	} else if (!adapter->connection_prepared ||
	    adapter->connection_generation != generation) {
		error = ESTALE;
	} else if (class != RTL8822BU_FRAME_MANAGEMENT &&
	    !adapter->association_active) {
		error = ENOTCONN;
	} else if (encrypted && (!adapter->pairwise_key_installed ||
	    adapter->pairwise_key_generation != key_generation)) {
		error = ESTALE;
	} else {
		error = rtl8822bu_tx_report_reserve_locked(adapter, generation,
		    key_generation, cookie, now, deadline, &sequence);
		if (error == 0) {
			channel = adapter->connection_channel;
			adapter->radio_operations_active++;
		}
	}
	spin_unlock_irqrestore(&adapter->lock, enabled);
	if (error != 0)
		return error;
	capacity = RTL8822B_DATA_TX_DESCRIPTOR_SIZE + length + 1U;
	wire = hal_malloc(capacity);
	if (wire == NULL) {
		error = ENOMEM;
		goto out_release;
	}
	error = rtl8822b_data_frame_prepare(&adapter->radio, wire, capacity,
	    frame, length, encrypted, 0U, sequence, &wire_length);
	if (error == 0)
		error = rtl8822bu_tx_descriptor_set_priority(wire, wire_length,
		    class, channel);
	now = clock_ticks();
	if (error == 0 && now >= deadline)
		error = ETIMEDOUT;
	if (error == 0) {
		remaining = deadline - now;
		milliseconds = remaining > UINT64_MAX / 1000ULL ? UINT64_MAX :
		    (remaining * 1000ULL) / KERN_CLOCK_HZ;
		if (milliseconds == 0U)
			milliseconds = 1U;
		if (milliseconds > UINT32_MAX)
			milliseconds = UINT32_MAX;
		/* The frozen three-OUT 0xf5a0 map sends QSEL/TID0 (BE) through
		 * the low DMA pipe; management QSEL 18 uses the high pipe. */
		endpoint = class == RTL8822BU_FRAME_DATA ?
		    adapter->bulk_out_low : adapter->bulk_out_high;
		transfer_attempted = 1;
		error = rtl8822bu_bulk_transfer(adapter, endpoint, wire,
		    wire_length, (unsigned)milliseconds, &actual);
		if (error == 0 && actual != wire_length)
			error = EIO;
	}
	memset(wire, 0, capacity);
	hal_free(wire);
out_release:
	if (error != 0) {
		if (transfer_attempted)
			rtl8822bu_tx_report_abandon_attempted(adapter, sequence,
			    error);
		else
			rtl8822bu_tx_report_release(adapter, sequence);
	}
	rtl8822bu_operation_leave(adapter);
	return error;
}

static int
rtl8822bu_frame_transmit(void *context,
	const struct wlan_radio_tx_request *request)
{
	enum rtl8822bu_frame_class class;

	if (context == NULL || request == NULL ||
	    !rtl8822bu_bytes_zero(request->reserved,
	    sizeof(request->reserved)))
		return EINVAL;
	if (request->frame_class == WLAN_RADIO_FRAME_MANAGEMENT)
		class = RTL8822BU_FRAME_MANAGEMENT;
	else if (request->frame_class == WLAN_RADIO_FRAME_EAPOL)
		class = RTL8822BU_FRAME_EAPOL;
	else if (request->frame_class == WLAN_RADIO_FRAME_DATA)
		class = RTL8822BU_FRAME_DATA;
	else
		return EINVAL;
	return rtl8822bu_frame_transmit_private(context, request->generation,
	    class, request->frame, request->length, request->encrypted,
	    request->key_index, request->key_generation,
	    request->packet_number, request->cookie,
	    request->deadline_ticks);
}

static int
rtl8822bu_rx_classify(struct rtl8822bu_adapter *adapter,
	const struct rtl8822b_rx_packet *packet,
	struct rtl8822bu_rx_private *result)
{
	static const uint8_t llc_eapol[8] = {
		0xaaU, 0xaaU, 0x03U, 0U, 0U, 0U, 0x88U, 0x8eU
	};
	unsigned long enabled;
	uint64_t connection_generation;
	uint64_t key_generation = 0U;
	uint64_t pairwise_key_generation;
	uint64_t group_key_generation[RTL8822BU_GROUP_KEY_COUNT];
	uint64_t packet_number = 0U;
	uint16_t frame_control;
	size_t llc_offset;
	uint8_t group_key_mask;
	uint8_t key_index = 0U;
	uint8_t connection_channel;
	unsigned association_active;
	unsigned pairwise_key_installed;
	int group;

	if (adapter == NULL || packet == NULL || result == NULL)
		return EINVAL;
	memset(result, 0, sizeof(*result));
	if (packet->kind == RTL8822B_RX_C2H)
		return rtl8822bu_tx_report_complete(adapter, packet, result);
	if (packet->kind != RTL8822B_RX_FRAME || packet->payload == NULL ||
	    packet->payload_length < 2U)
		return EINVAL;
	frame_control = rtl8822bu_load_le16(packet->payload);
	enabled = spin_lock_irqsave(&adapter->lock);
	connection_generation = adapter->connection_generation;
	connection_channel = adapter->connection_channel;
	association_active = adapter->association_active;
	pairwise_key_installed = adapter->pairwise_key_installed;
	pairwise_key_generation = adapter->pairwise_key_generation;
	group_key_mask = adapter->group_key_mask;
	memcpy(group_key_generation, adapter->group_key_generation,
	    sizeof(group_key_generation));
	if ((frame_control & 0x000cU) == 0U) {
		uint16_t subtype = frame_control & 0x00f0U;

		/* q058 has no 802.11w/PMF contract.  Never pass an encrypted or
		 * integrity-failed management frame to the WPA2 state machine as if it
		 * were an authenticated clear management transaction. */
		if ((frame_control & 0x4000U) != 0U ||
		    packet->encryption_type != 0U || packet->icv_error) {
			spin_unlock_irqrestore(&adapter->lock, enabled);
			return EACCES;
		}
		if ((subtype == 0x0080U || subtype == 0x0050U) &&
		    adapter->scan_generation != 0U)
			result->class = RTL8822BU_RX_SCAN;
		else if (connection_generation != 0U)
			result->class = RTL8822BU_RX_MANAGEMENT;
		spin_unlock_irqrestore(&adapter->lock, enabled);
		result->connection_generation = connection_generation;
		result->channel = connection_channel;
		return 0;
	}
	spin_unlock_irqrestore(&adapter->lock, enabled);
	if ((frame_control & 0x000cU) != 0x0008U ||
	    packet->payload_length < 24U || connection_generation == 0U ||
	    !association_active)
		return 0;
	llc_offset = 24U;
	if ((frame_control & 0x4000U) != 0U) {
		if (packet->payload_length < 32U || packet->encryption_type !=
		    RTL8822BU_RX_ENCRYPTION_AES || packet->software_decrypted ||
		    packet->icv_error)
			return EACCES;
		key_index = packet->payload[27U] >> 6;
		if (packet->payload[26U] != 0U ||
		    (packet->payload[27U] & 0x3fU) != 0x20U)
			return EILSEQ;
		packet_number = (uint64_t)packet->payload[24U] |
		    ((uint64_t)packet->payload[25U] << 8) |
		    ((uint64_t)packet->payload[28U] << 16) |
		    ((uint64_t)packet->payload[29U] << 24) |
		    ((uint64_t)packet->payload[30U] << 32) |
		    ((uint64_t)packet->payload[31U] << 40);
		if (packet_number == 0U)
			return EILSEQ;
		group = (packet->payload[4U] & 1U) != 0U;
		if (group) {
			if ((group_key_mask & (1U << key_index)) == 0U)
				return EACCES;
			key_generation = group_key_generation[key_index];
		} else {
			if (key_index != 0U || !pairwise_key_installed)
				return EACCES;
			key_generation = pairwise_key_generation;
		}
		if (key_generation == 0U)
			return EACCES;
		llc_offset += 8U;
	} else if (packet->encryption_type != 0U || packet->icv_error) {
		return EACCES;
	} else {
		key_generation = 0U;
	}
	result->class = packet->payload_length >= llc_offset +
	    sizeof(llc_eapol) && memcmp(packet->payload + llc_offset,
	    llc_eapol, sizeof(llc_eapol)) == 0 ? RTL8822BU_RX_EAPOL :
	    RTL8822BU_RX_DATA;
	result->connection_generation = connection_generation;
	result->channel = connection_channel;
	result->key_generation = key_generation;
	result->packet_number = packet_number;
	result->key_index = key_index;
	result->encryption_type = packet->encryption_type;
	result->software_decrypted = packet->software_decrypted;
	result->icv_error = packet->icv_error;
	return 0;
}

struct rtl8822bu_rx_report_context {
	struct rtl8822bu_adapter *adapter;
	unsigned reported;
};

static int
rtl8822bu_rx_report(void *context, const struct rtl8822b_rx_packet *packet)
{
	struct rtl8822bu_rx_report_context *report = context;
	struct rtl8822bu_adapter *adapter = report->adapter;
	struct rtl8822bu_rx_private classified;
	struct wlan_radio_rx_frame frame_report;
	struct wlan_station *station;
	unsigned long enabled;
	uint64_t generation;
	uint32_t channel;
	uint16_t frame_control;
	uint16_t subtype;
	int error;

	error = rtl8822bu_rx_classify(adapter, packet, &classified);
	/* Unknown firmware notifications and reports for a transaction already
	 * retired by disconnect are benign.  A malformed known CCX report is not. */
	if (error == ENOENT || error == ESTALE)
		return 0;
	if (error != 0)
		return error;
	enabled = spin_lock_irqsave(&adapter->lock);
	station = adapter->station;
	spin_unlock_irqrestore(&adapter->lock, enabled);
	if (station == NULL || classified.class == RTL8822BU_RX_IGNORE)
		return 0;
	if (classified.class == RTL8822BU_RX_TX_REPORT) {
		error = wlan_station_report_tx_complete(station,
		    classified.connection_generation, classified.cookie,
		    classified.tx_error == 0, classified.tx_error);
		return error == ESTALE ? 0 : error;
	}
	if (classified.class != RTL8822BU_RX_SCAN) {
		memset(&frame_report, 0, sizeof(frame_report));
		frame_report.generation = classified.connection_generation;
		frame_report.key_generation = classified.key_generation;
		frame_report.packet_number = classified.packet_number;
		frame_report.frame = packet->payload;
		frame_report.length = packet->payload_length;
		frame_report.rssi_dbm = packet->rssi_dbm;
		frame_report.channel = classified.channel;
		frame_report.cipher = classified.encryption_type ==
		    RTL8822BU_RX_ENCRYPTION_AES ? WLAN_RADIO_CIPHER_CCMP :
		    WLAN_RADIO_CIPHER_NONE;
		frame_report.decrypted = classified.encryption_type ==
		    RTL8822BU_RX_ENCRYPTION_AES &&
		    !classified.software_decrypted && !classified.icv_error;
		frame_report.key_index = classified.key_index;
		frame_report.integrity_error = classified.icv_error;
		error = wlan_station_report_frame(station, &frame_report);
		return error == ESTALE ? 0 : error;
	}
	frame_control = (uint16_t)((uint16_t)packet->payload[0] |
	    ((uint16_t)packet->payload[1] << 8));
	if ((frame_control & 0x000cU) != 0U)
		return 0;
	subtype = frame_control & 0x00f0U;
	if (subtype != 0x0080U && subtype != 0x0050U)
		return 0;
	enabled = spin_lock_irqsave(&adapter->lock);
	generation = adapter->scan_generation;
	channel = adapter->scan_channel;
	spin_unlock_irqrestore(&adapter->lock, enabled);
	/* Management traffic outside a current software-scan dwell is normal.
	 * It is not a malformed RX aggregate and must not inflate rx_errors. */
	if (station == NULL || generation == 0U || channel == 0U)
		return 0;
	error = wlan_station_report_scan_frame(station, generation,
	    packet->payload, packet->payload_length, packet->rssi_dbm,
	    (uint8_t)channel);
	if (error == ESTALE)
		return 0;
	if (error == 0)
		report->reported++;
	return error;
}

static int
rtl8822bu_rx_has_work(struct rtl8822bu_adapter *adapter)
{
	unsigned long enabled = spin_lock_irqsave(&adapter->lock);
	int pending = adapter->ready && !adapter->detaching &&
	    adapter->opened && !adapter->closing &&
	    !adapter->stopping && !adapter->recovery_pending &&
	    !adapter->quarantined &&
	    (adapter->rx_ready || (adapter->rx_rearm &&
	    !adapter->rx_rearm_active));

	spin_unlock_irqrestore(&adapter->lock, enabled);
	return pending;
}

static int
rtl8822bu_scan_channel_start(void *context, uint64_t generation,
	uint32_t step_index, uint32_t channel, uint64_t deadline)
{
	struct rtl8822bu_adapter *adapter = context;
	struct wlan_station *station;
	unsigned long enabled;
	int error;

	if (adapter == NULL || generation == 0U || step_index >= 11U ||
	    channel == 0U || channel > 11U)
		return EINVAL;
	if (clock_ticks() >= deadline)
		return ETIMEDOUT;
	enabled = spin_lock_irqsave(&adapter->lock);
	if (!adapter->ready || adapter->detaching || !adapter->opened ||
	    adapter->closing ||
	    adapter->stopping || adapter->recovery_pending ||
	    adapter->quarantined ||
	    !adapter->radio_running || adapter->station == NULL) {
		error = adapter->ready ? ENETDOWN : ENODEV;
		spin_unlock_irqrestore(&adapter->lock, enabled);
		return error;
	}
	if (adapter->connection_generation != 0U || adapter->tx_quiescing ||
	    adapter->radio_operations_active != 0U) {
		spin_unlock_irqrestore(&adapter->lock, enabled);
		return EBUSY;
	}
	station = adapter->station;
	adapter->radio_operations_active++;
	spin_unlock_irqrestore(&adapter->lock, enabled);
	error = rtl8822b_radio_set_channel(&adapter->radio, (uint8_t)channel,
	    deadline);
	if (error != 0) {
		/* Invalid caller input is rejected above without touching hardware.
		 * Once the core has entered channel programming, however, a failed
		 * transaction fail-closes the radio and clears its state.  Mirror that
		 * terminal hardware state here so no later callback can observe stale
		 * firmware/radio-running flags or keep admitting RX/TX work.  opened is
		 * intentionally retained until close/detach drains the outstanding URB. */
		enabled = spin_lock_irqsave(&adapter->lock);
		if (adapter->radio.state == RTL8822B_RADIO_OFF) {
			adapter->firmware_running = 0U;
			adapter->radio_running = 0U;
			adapter->scan_generation = 0U;
			adapter->scan_channel = 0U;
			adapter->quarantined = 1U;
		}
		spin_unlock_irqrestore(&adapter->lock, enabled);
		rtl8822bu_operation_leave(adapter);
		return error;
	}
	enabled = spin_lock_irqsave(&adapter->lock);
	if (!adapter->ready || adapter->detaching || !adapter->opened ||
	    adapter->closing ||
	    adapter->stopping || adapter->recovery_pending ||
	    adapter->quarantined ||
	    !adapter->radio_running || adapter->station != station) {
		error = adapter->ready ? ENETDOWN : ENODEV;
	} else {
		adapter->scan_generation = generation;
		adapter->scan_channel = channel;
		error = 0;
	}
	spin_unlock_irqrestore(&adapter->lock, enabled);
	if (error != 0) {
		rtl8822bu_operation_leave(adapter);
		return error;
	}
	error = wlan_station_report_scan_channel_ready(station, generation,
	    step_index);
	rtl8822bu_operation_leave(adapter);
	return error == ESTALE ? 0 : error;
}

static int
rtl8822bu_scan_stop(void *context, uint64_t generation)
{
	struct rtl8822bu_adapter *adapter = context;
	unsigned long enabled;

	if (adapter == NULL)
		return ENODEV;
	enabled = spin_lock_irqsave(&adapter->lock);
	if (generation == 0U || adapter->scan_generation == generation) {
		adapter->scan_generation = 0U;
		adapter->scan_channel = 0U;
	}
	spin_unlock_irqrestore(&adapter->lock, enabled);
	return 0;
}

static int
rtl8822bu_security_hardware_clear(struct rtl8822bu_adapter *adapter,
	uint64_t deadline)
{
	unsigned long enabled;
	unsigned slot;
	int error = 0;
	int cleanup_error;

	/* USB bulk completion only retires the host transfer.  The MAC may still
	 * own pages for that frame, so CAM/BSSID mutation is forbidden until every
	 * hardware priority queue reports reserved == available. */
	cleanup_error = rtl8822b_tx_queues_empty(&adapter->radio, deadline);
	if (cleanup_error != 0)
		return cleanup_error;

	/* Clear the complete q058/q060 allocation, not merely the software bitmap.
	 * This makes a warm firmware/device handoff fail closed even if the prior
	 * host disappeared between a CAM write and its software commit. */
	cleanup_error = rtl8822b_security_clear_association(&adapter->radio,
	    deadline);
	enabled = spin_lock_irqsave(&adapter->lock);
	adapter->association_uncertain = cleanup_error != 0;
	if (cleanup_error == 0) {
		adapter->association_active = 0U;
		adapter->association_aid = 0U;
	}
	spin_unlock_irqrestore(&adapter->lock, enabled);
	rtl8822bu_record_cleanup_error(&error, cleanup_error);
	for (slot = 0U; slot < RTL8822BU_CAM_OWNED_SLOT_COUNT; slot++) {
		cleanup_error = rtl8822b_cam_clear(&adapter->radio, (uint8_t)slot,
		    deadline);
		enabled = spin_lock_irqsave(&adapter->lock);
		if (cleanup_error == 0)
			adapter->cam_uncertain_mask &= ~(1U << slot);
		else
			adapter->cam_uncertain_mask |= 1U << slot;
		spin_unlock_irqrestore(&adapter->lock, enabled);
		rtl8822bu_record_cleanup_error(&error, cleanup_error);
	}
	enabled = spin_lock_irqsave(&adapter->lock);
	if (error == 0) {
		adapter->pairwise_key_installed = 0U;
		adapter->pairwise_key_generation = 0U;
		adapter->pairwise_staged_installed = 0U;
		adapter->pairwise_staged_generation = 0U;
		adapter->pairwise_retired_valid = 0U;
		adapter->pairwise_retired_generation = 0U;
		adapter->pairwise_key_slot = RTL8822B_CAM_PAIRWISE_SLOT;
		adapter->pairwise_staged_slot = RTL8822BU_PAIRWISE_STAGING_SLOT;
		adapter->group_key_mask = 0U;
		adapter->group_staged_mask = 0U;
		adapter->group_retired_mask = 0U;
		memset(adapter->group_key_generation, 0,
		    sizeof(adapter->group_key_generation));
		memset(adapter->group_staged_generation, 0,
		    sizeof(adapter->group_staged_generation));
		memset(adapter->group_retired_generation, 0,
		    sizeof(adapter->group_retired_generation));
		for (slot = 0U; slot < RTL8822BU_GROUP_KEY_COUNT; slot++) {
			adapter->group_key_slot[slot] = (uint8_t)slot;
			adapter->group_staged_slot[slot] = (uint8_t)
			    (RTL8822BU_GROUP_STAGING_SLOT_BASE + slot);
			adapter->group_retired_slot[slot] = 0U;
		}
	}
	spin_unlock_irqrestore(&adapter->lock, enabled);
	return error;
}

static int
rtl8822bu_connect_start(void *context, uint64_t generation,
	const struct wlan_bss_record *bss, uint64_t deadline)
{
	struct rtl8822bu_adapter *adapter = context;
	unsigned long enabled;
	uint64_t cleanup_deadline;
	int absence_known = 0;
	int quarantine = 0;
	int error;
	int cleanup_error = 0;

	if (adapter == NULL || generation == 0U || bss == NULL ||
	    bss->channel == 0U || bss->channel > 11U ||
	    !rtl8822bu_unicast_address(bss->bssid))
		return EINVAL;
	if (clock_ticks() >= deadline)
		return ETIMEDOUT;
	enabled = spin_lock_irqsave(&adapter->lock);
	if (!adapter->ready || adapter->detaching || !adapter->opened ||
	    adapter->closing ||
	    adapter->stopping || adapter->recovery_pending ||
	    adapter->quarantined ||
	    !adapter->radio_running) {
		error = adapter->ready ? ENETDOWN : ENODEV;
	} else if (adapter->connection_generation != 0U) {
		/* A retry of the identical completed preparation is idempotent.  A
		 * queue-drain retry of the identical preparation keeps admission
		 * closed and resumes the transaction.  A new generation must first
		 * pass the checked disconnect barrier. */
		if (adapter->connection_prepared &&
		    adapter->connection_generation == generation &&
		    adapter->connection_channel == bss->channel &&
		    rtl8822bu_mac_equal(adapter->connection_bssid, bss->bssid)) {
			error = 0;
		} else if (adapter->connection_preparing &&
		    adapter->tx_quiescing &&
		    adapter->connection_generation == generation &&
		    adapter->connection_channel == bss->channel &&
		    rtl8822bu_mac_equal(adapter->connection_bssid, bss->bssid)) {
			if (adapter->radio_operations_active != 0U) {
				error = EBUSY;
			} else {
				adapter->radio_operations_active++;
				error = EINPROGRESS;
			}
		} else {
			error = EBUSY;
		}
	} else {
		adapter->tx_quiescing = 1U;
		adapter->connection_preparing = 1U;
		adapter->connection_generation = generation;
		adapter->connection_channel = bss->channel;
		memcpy(adapter->connection_bssid, bss->bssid,
		    sizeof(adapter->connection_bssid));
		adapter->scan_generation = 0U;
		adapter->scan_channel = 0U;
		if (adapter->radio_operations_active != 0U) {
			error = EBUSY;
		} else {
			adapter->radio_operations_active++;
			error = EINPROGRESS;
		}
	}
	spin_unlock_irqrestore(&adapter->lock, enabled);
	if (error != EINPROGRESS)
		return error;

	error = rtl8822b_radio_set_channel(&adapter->radio, bss->channel,
	    deadline);
	if (error == 0)
		error = rtl8822b_security_enable(&adapter->radio, deadline);
	if (error == 0)
		error = rtl8822bu_security_hardware_clear(adapter, deadline);
	enabled = spin_lock_irqsave(&adapter->lock);
	if (error == 0 && adapter->ready && !adapter->detaching &&
	    adapter->opened &&
	    !adapter->closing && !adapter->stopping &&
	    !adapter->recovery_pending && !adapter->quarantined &&
	    adapter->radio_running &&
	    adapter->connection_generation == generation) {
		adapter->connection_preparing = 0U;
		adapter->connection_prepared = 1U;
		adapter->security_enabled = 1U;
		adapter->tx_quiescing = 0U;
	} else if (error == EBUSY) {
		/* The MAC still owns queued pages.  Preserve both the transaction and
		 * closed admission so an identical bounded retry can make progress. */
	} else {
		if (error == 0)
			error = adapter->ready ? ENETDOWN : ENODEV;
		adapter->connection_preparing = 0U;
		if (adapter->radio.state == RTL8822B_RADIO_OFF) {
			adapter->firmware_running = 0U;
			adapter->radio_running = 0U;
			absence_known = 1;
			quarantine = 1;
		}
	}
	spin_unlock_irqrestore(&adapter->lock, enabled);
	if (error != 0 && error != EBUSY &&
	    adapter->radio.state == RTL8822B_RADIO_STARTED) {
		cleanup_error = rtl8822bu_deadline_after(
		    RTL8822BU_SECURITY_TIMEOUT_TICKS, &cleanup_deadline);
		if (cleanup_error == 0)
			cleanup_error = rtl8822bu_security_hardware_clear(adapter,
			    cleanup_deadline);
		rtl8822bu_record_cleanup_error(&error, cleanup_error);
		if (cleanup_error != 0)
			quarantine = 1;
		else
			absence_known = 1;
	}
	if (error != 0 && error != EBUSY) {
		enabled = spin_lock_irqsave(&adapter->lock);
		if (absence_known) {
			rtl8822bu_connection_state_clear_locked(adapter);
			adapter->tx_quiescing = 0U;
		}
		if (quarantine)
			/* A failed preparation remains unavailable.  When absence is not
			 * known, its generation and uncertainty remain available to the
			 * matching disconnect/radio-stop cleanup barrier. */
			adapter->quarantined = 1U;
		if (!absence_known && !quarantine)
			rtl8822bu_tx_quiesce_result_locked(adapter, error, 0);
		spin_unlock_irqrestore(&adapter->lock, enabled);
	}
	rtl8822bu_operation_leave(adapter);
	return error;
}

static int
rtl8822bu_association_set(void *context, uint64_t generation,
	const uint8_t bssid[6], uint16_t aid, uint64_t deadline)
{
	struct rtl8822bu_adapter *adapter = context;
	unsigned long enabled;
	uint64_t cleanup_deadline;
	int cleanup_error;
	int error;

	if (adapter == NULL || generation == 0U ||
	    !rtl8822bu_unicast_address(bssid) || aid == 0U || aid > 0x07ffU)
		return EINVAL;
	if (clock_ticks() >= deadline)
		return ETIMEDOUT;
	enabled = spin_lock_irqsave(&adapter->lock);
	if (!adapter->ready || adapter->detaching || !adapter->opened ||
	    adapter->closing ||
	    adapter->stopping || adapter->recovery_pending ||
	    adapter->quarantined ||
	    !adapter->radio_running) {
		error = adapter->ready ? ENETDOWN : ENODEV;
	} else if (!adapter->connection_prepared ||
	    adapter->connection_generation != generation ||
	    !rtl8822bu_mac_equal(adapter->connection_bssid, bssid)) {
		error = ESTALE;
	} else if (adapter->association_uncertain) {
		error = EBUSY;
	} else if (adapter->association_active) {
		error = adapter->association_aid == aid ? 0 : EBUSY;
	} else if (adapter->tx_quiescing) {
		/* A queue-drain retry owns the tentative AID while the admission
		 * gate remains closed. */
		if (adapter->association_aid == aid) {
			if (adapter->radio_operations_active != 0U) {
				error = EBUSY;
			} else {
				adapter->radio_operations_active++;
				error = EINPROGRESS;
			}
		} else {
			error = EBUSY;
		}
	} else {
		adapter->tx_quiescing = 1U;
		adapter->association_aid = aid;
		if (adapter->radio_operations_active != 0U) {
			error = EBUSY;
		} else {
			adapter->radio_operations_active++;
			error = EINPROGRESS;
		}
	}
	spin_unlock_irqrestore(&adapter->lock, enabled);
	if (error != EINPROGRESS)
		return error;
	error = rtl8822b_tx_queues_empty(&adapter->radio, deadline);
	if (error != 0) {
		enabled = spin_lock_irqsave(&adapter->lock);
		if (error != EBUSY) {
			adapter->association_uncertain = 1U;
			adapter->quarantined = 1U;
		}
		rtl8822bu_tx_quiesce_result_locked(adapter, error, 0);
		spin_unlock_irqrestore(&adapter->lock, enabled);
		rtl8822bu_operation_leave(adapter);
		return error;
	}
	enabled = spin_lock_irqsave(&adapter->lock);
	adapter->association_uncertain = 1U;
	spin_unlock_irqrestore(&adapter->lock, enabled);
	error = rtl8822b_security_set_association(&adapter->radio, bssid, aid,
	    deadline);
	enabled = spin_lock_irqsave(&adapter->lock);
	if (error == 0 && adapter->connection_generation == generation &&
	    adapter->connection_prepared && !adapter->recovery_pending &&
	    !adapter->quarantined) {
		adapter->association_active = 1U;
		adapter->association_uncertain = 0U;
		adapter->association_aid = aid;
		adapter->tx_quiescing = 0U;
	} else {
		if (error == 0)
			error = ESTALE;
	}
	spin_unlock_irqrestore(&adapter->lock, enabled);
	if (error != 0) {
		cleanup_error = rtl8822bu_deadline_after(
		    RTL8822BU_SECURITY_TIMEOUT_TICKS, &cleanup_deadline);
		if (cleanup_error == 0)
			cleanup_error = rtl8822b_security_clear_association(
			    &adapter->radio, cleanup_deadline);
		enabled = spin_lock_irqsave(&adapter->lock);
		if (cleanup_error == 0) {
			adapter->association_active = 0U;
			adapter->association_uncertain = 0U;
			adapter->association_aid = 0U;
			adapter->tx_quiescing = 0U;
		} else {
			adapter->association_uncertain = 1U;
			adapter->tx_quiescing = 1U;
			adapter->quarantined = 1U;
		}
		spin_unlock_irqrestore(&adapter->lock, enabled);
		rtl8822bu_record_cleanup_error(&error, cleanup_error);
	}
	rtl8822bu_operation_leave(adapter);
	return error;
}

static int
rtl8822bu_association_clear(void *context, uint64_t generation,
	uint64_t deadline)
{
	struct rtl8822bu_adapter *adapter = context;
	uint8_t bssid[6];
	unsigned long enabled;
	uint64_t now;
	int send_deauthentication = 0;
	int error;

	if (adapter == NULL || generation == 0U)
		return EINVAL;
	now = clock_ticks();
	enabled = spin_lock_irqsave(&adapter->lock);
	rtl8822bu_tx_report_reap_locked(adapter, now);
	if (adapter->connection_generation != generation) {
		error = ESTALE;
	} else if (adapter->transport_absent) {
		/* Physical absence proves the device-side association no longer
		 * exists.  Preserve common inverse ordering: keys must first retire
		 * their exact local ledgers, then association identity can disappear. */
		if (adapter->pairwise_key_installed ||
		    adapter->pairwise_staged_installed ||
		    adapter->pairwise_staged_generation != 0U ||
		    adapter->group_key_mask != 0U ||
		    adapter->group_staged_mask != 0U ||
		    adapter->cam_uncertain_mask != 0U) {
			error = EBUSY;
		} else {
			adapter->association_active = 0U;
			adapter->association_uncertain = 0U;
			adapter->association_aid = 0U;
			error = 0;
		}
	} else if (now >= deadline) {
		error = ETIMEDOUT;
	} else if (!adapter->ready || !adapter->opened ||
	    adapter->closing ||
	    adapter->stopping ||
	    !adapter->radio_running) {
		error = adapter->ready ? ENETDOWN : ENODEV;
	} else if (adapter->pairwise_key_installed ||
	    adapter->pairwise_staged_installed ||
	    adapter->pairwise_staged_generation != 0U ||
	    adapter->group_key_mask != 0U ||
	    adapter->group_staged_mask != 0U ||
	    adapter->cam_uncertain_mask != 0U) {
		adapter->tx_quiescing = 1U;
		error = EBUSY;
	} else if (rtl8822bu_tx_report_generation_active_locked(adapter,
	    generation, 0U, 0)) {
		adapter->tx_quiescing = 1U;
		error = EBUSY;
	} else if (!adapter->association_active &&
	    !adapter->association_uncertain) {
		error = 0;
	} else if (adapter->radio_operations_active != 0U) {
		adapter->tx_quiescing = 1U;
		error = EBUSY;
	} else {
		adapter->radio_operations_active++;
		adapter->tx_quiescing = 1U;
		if (adapter->association_active &&
		    adapter->deauthentication_attempted_generation != generation) {
			adapter->deauthentication_attempted_generation = generation;
			memcpy(bssid, adapter->connection_bssid, sizeof(bssid));
			send_deauthentication = 1;
		}
		error = EINPROGRESS;
	}
	spin_unlock_irqrestore(&adapter->lock, enabled);
	if (error != EINPROGRESS)
		return error;
	error = rtl8822b_tx_queues_empty(&adapter->radio, deadline);
	if (error != 0) {
		enabled = spin_lock_irqsave(&adapter->lock);
		if (error != EBUSY) {
			adapter->association_uncertain = 1U;
			adapter->quarantined = 1U;
		}
		rtl8822bu_tx_quiesce_result_locked(adapter, error, 0);
		spin_unlock_irqrestore(&adapter->lock, enabled);
		rtl8822bu_operation_leave(adapter);
		return error;
	}
	/* The common teardown order retires keys before association state and calls
	 * disconnect last.  Send the optional class-2 deauthentication while the
	 * association identity is still retained.  Transport failure is deliberately
	 * ignored and can never delay the checked local state inverse below. */
	if (send_deauthentication)
		rtl8822bu_deauthenticate_best_effort(adapter, bssid, deadline);
	error = rtl8822b_security_clear_association(&adapter->radio, deadline);
	enabled = spin_lock_irqsave(&adapter->lock);
	if (error == 0) {
		adapter->association_active = 0U;
		adapter->association_uncertain = 0U;
		adapter->association_aid = 0U;
		adapter->tx_quiescing = 0U;
		if (adapter->connection_generation != generation)
			error = ESTALE;
	} else {
		adapter->association_uncertain = 1U;
		adapter->tx_quiescing = 1U;
		adapter->quarantined = 1U;
	}
	spin_unlock_irqrestore(&adapter->lock, enabled);
	rtl8822bu_operation_leave(adapter);
	return error;
}

static int
rtl8822bu_key_install_checked(struct rtl8822bu_adapter *adapter,
	uint64_t generation, enum rtl8822bu_key_role role, uint8_t key_index,
	const uint8_t address[6], const uint8_t key[16],
	uint64_t key_generation, uint64_t deadline)
{
	static const uint8_t broadcast[6] = {
		0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU
	};
	const uint8_t *cam_address;
	unsigned long enabled;
	uint64_t cleanup_deadline;
	uint64_t active_generation;
	uint8_t slot;
	int installed;
	int replacement;
	int error;
	int rollback_error;

	if (adapter == NULL || generation == 0U || key_generation == 0U ||
	    address == NULL || key == NULL || key_index >=
	    RTL8822BU_GROUP_KEY_COUNT || (role != RTL8822BU_KEY_PAIRWISE &&
	    role != RTL8822BU_KEY_GROUP))
		return EINVAL;
	if (clock_ticks() >= deadline)
		return ETIMEDOUT;
	if (role == RTL8822BU_KEY_PAIRWISE) {
		if (key_index != 0U)
			return EINVAL;
		cam_address = address;
	} else {
		if ((address[0] & 1U) == 0U)
			return EINVAL;
		cam_address = broadcast;
	}
	enabled = spin_lock_irqsave(&adapter->lock);
	if (!adapter->ready || adapter->detaching || !adapter->opened ||
	    adapter->closing ||
	    adapter->stopping || adapter->recovery_pending ||
	    adapter->quarantined ||
	    !adapter->radio_running) {
		error = adapter->ready ? ENETDOWN : ENODEV;
	} else if (role == RTL8822BU_KEY_PAIRWISE &&
	    !rtl8822bu_mac_equal(address, adapter->connection_bssid)) {
		error = EINVAL;
	} else if (!adapter->connection_prepared ||
	    !adapter->association_active ||
	    adapter->connection_generation != generation) {
		error = ESTALE;
	} else {
		installed = role == RTL8822BU_KEY_PAIRWISE ?
		    adapter->pairwise_key_installed : adapter->group_key_mask != 0U;
		active_generation = role == RTL8822BU_KEY_PAIRWISE ?
		    adapter->pairwise_key_generation :
		    adapter->group_key_generation[key_index];
		replacement = installed;
		if (role == RTL8822BU_KEY_PAIRWISE) {
			if (adapter->pairwise_staged_generation != 0U) {
				if (adapter->pairwise_staged_generation != key_generation)
					error = EBUSY;
				else if (adapter->pairwise_staged_installed)
					error = EALREADY;
				else
					error = EINPROGRESS;
			} else if (installed) {
				if (active_generation == key_generation) {
					error = EALREADY;
				} else if (adapter->pairwise_retired_valid) {
					error = EBUSY;
				} else {
					adapter->pairwise_staged_slot =
					    adapter->pairwise_key_slot ==
					    RTL8822B_CAM_PAIRWISE_SLOT ?
					    RTL8822BU_PAIRWISE_STAGING_SLOT :
					    RTL8822B_CAM_PAIRWISE_SLOT;
					adapter->pairwise_staged_generation =
					    key_generation;
					error = EINPROGRESS;
				}
			} else if (active_generation == 0U) {
				adapter->pairwise_key_slot =
				    RTL8822B_CAM_PAIRWISE_SLOT;
				adapter->pairwise_key_generation = key_generation;
				error = EINPROGRESS;
			} else {
				error = active_generation == key_generation &&
				    adapter->tx_quiescing ? EINPROGRESS : EALREADY;
			}
			slot = replacement ? adapter->pairwise_staged_slot :
			    adapter->pairwise_key_slot;
		} else {
			if (adapter->group_staged_generation[key_index] != 0U) {
				if (adapter->group_staged_generation[key_index] !=
				    key_generation)
					error = EBUSY;
				else if ((adapter->group_staged_mask &
				    (1U << key_index)) != 0U)
					error = EALREADY;
				else
					error = EINPROGRESS;
			} else if (replacement) {
				if ((adapter->group_key_mask & (1U << key_index)) != 0U &&
				    active_generation == key_generation) {
					error = EALREADY;
				} else if ((adapter->group_retired_mask &
				    (1U << key_index)) != 0U) {
					error = EBUSY;
				} else {
					adapter->group_staged_slot[key_index] =
					    (adapter->group_key_mask &
					    (1U << key_index)) != 0U ?
					    (adapter->group_key_slot[key_index] == key_index ?
					    (uint8_t)(RTL8822BU_GROUP_STAGING_SLOT_BASE +
					    key_index) : key_index) : key_index;
					adapter->group_staged_generation[key_index] =
					    key_generation;
					error = EINPROGRESS;
				}
			} else if (active_generation == 0U) {
				adapter->group_key_slot[key_index] = key_index;
				adapter->group_key_generation[key_index] = key_generation;
				error = EINPROGRESS;
			} else {
				error = active_generation == key_generation &&
				    adapter->tx_quiescing ? EINPROGRESS : EALREADY;
			}
			slot = replacement ? adapter->group_staged_slot[key_index] :
			    adapter->group_key_slot[key_index];
		}
		if (error == EINPROGRESS &&
		    (adapter->cam_uncertain_mask & (1U << slot)) != 0U) {
			error = EIO;
			adapter->quarantined = 1U;
		}
		if (error == EINPROGRESS && replacement &&
		    role == RTL8822BU_KEY_PAIRWISE &&
		    rtl8822bu_tx_report_generation_active_locked(adapter,
		    generation, active_generation, 1)) {
			adapter->tx_quiescing = 1U;
			error = EBUSY;
		} else if (error == EINPROGRESS &&
		    adapter->radio_operations_active != 0U) {
			adapter->tx_quiescing = 1U;
			error = EBUSY;
		} else if (error == EINPROGRESS) {
			adapter->tx_quiescing = 1U;
			adapter->radio_operations_active++;
		}
	}
	spin_unlock_irqrestore(&adapter->lock, enabled);
	if (error != EINPROGRESS)
		return error;
	error = rtl8822b_tx_queues_empty(&adapter->radio, deadline);
	if (error != 0) {
		enabled = spin_lock_irqsave(&adapter->lock);
		if (error != EBUSY) {
			adapter->cam_uncertain_mask |= 1U << slot;
			adapter->quarantined = 1U;
		}
		rtl8822bu_tx_quiesce_result_locked(adapter, error, 0);
		spin_unlock_irqrestore(&adapter->lock, enabled);
		rtl8822bu_operation_leave(adapter);
		return error;
	}
	enabled = spin_lock_irqsave(&adapter->lock);
	adapter->cam_uncertain_mask |= 1U << slot;
	spin_unlock_irqrestore(&adapter->lock, enabled);
	if (replacement)
		error = rtl8822b_cam_stage_ccmp(&adapter->radio, slot, key_index,
		    role == RTL8822BU_KEY_GROUP, cam_address, key, deadline);
	else
		error = rtl8822b_cam_program_ccmp(&adapter->radio, slot, key_index,
		    role == RTL8822BU_KEY_GROUP, cam_address, key, deadline);
	if (error == 0) {
		enabled = spin_lock_irqsave(&adapter->lock);
		if (adapter->connection_generation != generation ||
		    !adapter->association_active || adapter->recovery_pending ||
		    adapter->quarantined) {
			error = ESTALE;
		} else if (role == RTL8822BU_KEY_PAIRWISE) {
			if (replacement)
				adapter->pairwise_staged_installed = 1U;
			else
				adapter->pairwise_key_installed = 1U;
			adapter->cam_uncertain_mask &= ~(1U << slot);
			adapter->tx_quiescing = 0U;
		} else {
			if (replacement)
				adapter->group_staged_mask |=
				    (uint8_t)(1U << key_index);
			else
				adapter->group_key_mask |=
				    (uint8_t)(1U << key_index);
			adapter->cam_uncertain_mask &= ~(1U << slot);
			adapter->tx_quiescing = 0U;
		}
		spin_unlock_irqrestore(&adapter->lock, enabled);
	}
	if (error != 0) {
		rollback_error = rtl8822bu_deadline_after(
		    RTL8822BU_SECURITY_TIMEOUT_TICKS, &cleanup_deadline);
		if (rollback_error == 0)
			rollback_error = rtl8822b_cam_clear(&adapter->radio, slot,
			    cleanup_deadline);
		enabled = spin_lock_irqsave(&adapter->lock);
		if (rollback_error == 0) {
			adapter->cam_uncertain_mask &= ~(1U << slot);
			if (role == RTL8822BU_KEY_PAIRWISE) {
				if (replacement) {
					adapter->pairwise_staged_installed = 0U;
					adapter->pairwise_staged_generation = 0U;
				} else {
					adapter->pairwise_key_installed = 0U;
					adapter->pairwise_key_generation = 0U;
				}
			} else {
				if (replacement) {
					adapter->group_staged_mask &=
					    (uint8_t)~(1U << key_index);
					adapter->group_staged_generation[key_index] = 0U;
				} else {
					adapter->group_key_mask &=
					    (uint8_t)~(1U << key_index);
					adapter->group_key_generation[key_index] = 0U;
				}
			}
			adapter->tx_quiescing = 0U;
		} else {
			adapter->cam_uncertain_mask |= 1U << slot;
			adapter->tx_quiescing = 1U;
			adapter->quarantined = 1U;
		}
		spin_unlock_irqrestore(&adapter->lock, enabled);
		rtl8822bu_record_cleanup_error(&error, rollback_error);
	}
	rtl8822bu_operation_leave(adapter);
	return error;
}

static int
rtl8822bu_keys_activate_checked(struct rtl8822bu_adapter *adapter,
	uint64_t generation, uint64_t pairwise_generation,
	uint64_t group_generation, uint64_t deadline)
{
	static const uint8_t broadcast[6] = {
		0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU
	};
	unsigned long enabled;
	uint64_t now;
	uint64_t old_pairwise_generation = 0U;
	uint64_t old_group_generation = 0U;
	uint8_t old_pairwise_slot = 0U;
	uint8_t new_pairwise_slot = 0U;
	uint8_t old_group_slot = 0U;
	uint8_t new_group_slot = 0U;
	int pairwise_staged = 0;
	int group_staged_index = -1;
	int group_active_index = -1;
	int group_collision = 0;
	int rx_paused = 0;
	int new_pairwise_cleared = 0;
	int new_group_cleared = 0;
	int index;
	int error;
	int cleanup_error;

	if (adapter == NULL || generation == 0U ||
	    pairwise_generation == 0U || group_generation == 0U)
		return EINVAL;
	now = clock_ticks();
	if (now >= deadline)
		return ETIMEDOUT;
	enabled = spin_lock_irqsave(&adapter->lock);
	rtl8822bu_tx_report_reap_locked(adapter, now);
	for (index = 0; index < (int)RTL8822BU_GROUP_KEY_COUNT; index++) {
		if ((adapter->group_staged_mask & (1U << index)) != 0U &&
		    adapter->group_staged_generation[index] == group_generation) {
			if (group_staged_index >= 0)
				group_staged_index = -2;
			else
				group_staged_index = index;
		}
		if ((adapter->group_key_mask & (1U << index)) != 0U &&
		    adapter->group_key_generation[index] == group_generation) {
			if (group_active_index >= 0)
				group_active_index = -2;
			else
				group_active_index = index;
		}
	}
	pairwise_staged = adapter->pairwise_staged_installed &&
	    adapter->pairwise_staged_generation == pairwise_generation;
	if (!adapter->ready || adapter->detaching || !adapter->opened ||
	    adapter->closing ||
	    adapter->stopping || adapter->recovery_pending ||
	    adapter->quarantined || !adapter->radio_running) {
		error = adapter->ready ? ENETDOWN : ENODEV;
	} else if (!adapter->connection_prepared ||
	    !adapter->association_active ||
	    adapter->connection_generation != generation) {
		error = ESTALE;
	} else if (group_staged_index == -2 || group_active_index == -2) {
		error = EIO;
	} else if (!pairwise_staged &&
	    (!adapter->pairwise_key_installed ||
	    adapter->pairwise_key_generation != pairwise_generation)) {
		error = ESTALE;
	} else if (group_staged_index < 0 && group_active_index < 0) {
		error = ESTALE;
	} else if (!pairwise_staged && group_staged_index < 0) {
		/* The common layer may repeat activation after the hardware switch but
		 * before both old-generation tombstones have been retired. */
		error = 0;
	} else if (pairwise_staged && adapter->pairwise_retired_valid) {
		error = EBUSY;
	} else if (group_staged_index >= 0 &&
	    (adapter->group_retired_mask &
	    (1U << group_staged_index)) != 0U) {
		error = EBUSY;
	} else {
		if (pairwise_staged) {
			old_pairwise_slot = adapter->pairwise_key_slot;
			old_pairwise_generation =
			    adapter->pairwise_key_generation;
			new_pairwise_slot = adapter->pairwise_staged_slot;
		}
		if (group_staged_index >= 0) {
			new_group_slot =
			    adapter->group_staged_slot[group_staged_index];
			if ((adapter->group_key_mask &
			    (1U << group_staged_index)) != 0U) {
				group_collision = 1;
				old_group_slot =
				    adapter->group_key_slot[group_staged_index];
				old_group_generation = adapter->
				    group_key_generation[group_staged_index];
			}
		}
		adapter->tx_quiescing = 1U;
		if (pairwise_staged &&
		    rtl8822bu_tx_report_generation_active_locked(adapter,
		    generation, old_pairwise_generation, 1)) {
			error = EBUSY;
		} else if (adapter->radio_operations_active != 0U) {
			error = EBUSY;
		} else {
			adapter->radio_operations_active++;
			error = EINPROGRESS;
		}
	}
	spin_unlock_irqrestore(&adapter->lock, enabled);
	if (error != EINPROGRESS)
		return error;

	error = rtl8822b_tx_queues_empty(&adapter->radio, deadline);
	if (error != 0)
		goto out_error;
	error = rtl8822bu_rx_generation_pause(adapter, deadline);
	if (error != 0)
		goto out_error;
	rx_paused = 1;
	enabled = spin_lock_irqsave(&adapter->lock);
	if (pairwise_staged) {
		adapter->cam_uncertain_mask |= 1U << old_pairwise_slot;
		adapter->cam_uncertain_mask |= 1U << new_pairwise_slot;
	}
	if (group_staged_index >= 0) {
		adapter->cam_uncertain_mask |= 1U << new_group_slot;
		if (group_collision)
			adapter->cam_uncertain_mask |= 1U << old_group_slot;
	}
	spin_unlock_irqrestore(&adapter->lock, enabled);

	/* A matching old entry is invalidated before its staged replacement can
	 * become live.  The interval between those writes is intentionally
	 * fail-closed; no CAM lookup-order assumption is made. */
	if (pairwise_staged)
		error = rtl8822b_cam_clear(&adapter->radio, old_pairwise_slot,
		    deadline);
	if (error == 0 && group_collision)
		error = rtl8822b_cam_clear(&adapter->radio, old_group_slot,
		    deadline);
	if (error == 0 && pairwise_staged)
		error = rtl8822b_cam_activate_ccmp(&adapter->radio,
		    new_pairwise_slot, 0U, 0, adapter->connection_bssid, deadline);
	if (error == 0 && group_staged_index >= 0)
		error = rtl8822b_cam_activate_ccmp(&adapter->radio, new_group_slot,
		    (uint8_t)group_staged_index, 1, broadcast, deadline);
	if (error != 0)
		goto rollback_new;

	enabled = spin_lock_irqsave(&adapter->lock);
	if (!adapter->ready || adapter->detaching || !adapter->opened ||
	    adapter->closing ||
	    adapter->stopping || adapter->recovery_pending ||
	    adapter->quarantined || !adapter->association_active ||
	    adapter->connection_generation != generation ||
	    (pairwise_staged && (!adapter->pairwise_staged_installed ||
	    adapter->pairwise_staged_generation != pairwise_generation)) ||
	    (group_staged_index >= 0 &&
	    ((adapter->group_staged_mask &
	    (1U << group_staged_index)) == 0U ||
	    adapter->group_staged_generation[group_staged_index] !=
	    group_generation))) {
		error = ESTALE;
		adapter->quarantined = 1U;
		adapter->tx_quiescing = 1U;
	} else {
		if (pairwise_staged) {
			adapter->pairwise_retired_valid = 1U;
			adapter->pairwise_retired_slot = old_pairwise_slot;
			adapter->pairwise_retired_generation =
			    old_pairwise_generation;
			adapter->pairwise_key_slot = new_pairwise_slot;
			adapter->pairwise_key_generation = pairwise_generation;
			adapter->pairwise_key_installed = 1U;
			adapter->pairwise_staged_installed = 0U;
			adapter->pairwise_staged_generation = 0U;
			adapter->pairwise_staged_slot = old_pairwise_slot;
			adapter->cam_uncertain_mask &= ~(1U << old_pairwise_slot);
			adapter->cam_uncertain_mask &= ~(1U << new_pairwise_slot);
		}
		if (group_staged_index >= 0) {
			if (group_collision) {
				adapter->group_retired_mask |=
				    (uint8_t)(1U << group_staged_index);
				adapter->group_retired_slot[group_staged_index] =
				    old_group_slot;
				adapter->group_retired_generation[group_staged_index] =
				    old_group_generation;
				adapter->cam_uncertain_mask &= ~(1U << old_group_slot);
			}
			adapter->group_key_mask |=
			    (uint8_t)(1U << group_staged_index);
			adapter->group_key_slot[group_staged_index] = new_group_slot;
			adapter->group_key_generation[group_staged_index] =
			    group_generation;
			adapter->group_staged_mask &=
			    (uint8_t)~(1U << group_staged_index);
			adapter->group_staged_generation[group_staged_index] = 0U;
			adapter->group_staged_slot[group_staged_index] =
			    group_collision ? old_group_slot :
			    (uint8_t)(RTL8822BU_GROUP_STAGING_SLOT_BASE +
			    group_staged_index);
			adapter->cam_uncertain_mask &= ~(1U << new_group_slot);
		}
		/* RX is still cancelled here.  Publish the driver generations first,
		 * then arm a fresh URB before reopening TX admission. */
		adapter->tx_quiescing = 1U;
	}
	spin_unlock_irqrestore(&adapter->lock, enabled);
	if (error == 0) {
		error = rtl8822bu_rx_generation_resume(adapter, deadline);
		if (error == 0) {
			enabled = spin_lock_irqsave(&adapter->lock);
			adapter->tx_quiescing = 0U;
			spin_unlock_irqrestore(&adapter->lock, enabled);
		}
		rtl8822bu_operation_leave(adapter);
		return error;
	}

rollback_new:
	/* Never try to resurrect an old generation without retaining its key.
	 * Instead invalidate every possibly published replacement and quarantine
	 * the connection until the checked disconnect path clears all CAM slots. */
	if (pairwise_staged) {
		cleanup_error = rtl8822b_cam_clear(&adapter->radio,
		    new_pairwise_slot, deadline);
		new_pairwise_cleared = cleanup_error == 0;
		rtl8822bu_record_cleanup_error(&error, cleanup_error);
	}
	if (group_staged_index >= 0) {
		cleanup_error = rtl8822b_cam_clear(&adapter->radio,
		    new_group_slot, deadline);
		new_group_cleared = cleanup_error == 0;
		rtl8822bu_record_cleanup_error(&error, cleanup_error);
	}
	enabled = spin_lock_irqsave(&adapter->lock);
	if (new_pairwise_cleared)
		adapter->cam_uncertain_mask &= ~(1U << new_pairwise_slot);
	if (new_group_cleared)
		adapter->cam_uncertain_mask &= ~(1U << new_group_slot);
	/* Once device RX has been paused, an activation failure remains fail-closed.
	 * Checked key/association inverses are still callable while quarantined;
	 * only the terminal hardware-stop transaction clears this barrier. */
	if (rx_paused)
		adapter->rx_generation_barrier = 1U;
	adapter->tx_quiescing = 1U;
	adapter->quarantined = 1U;
	spin_unlock_irqrestore(&adapter->lock, enabled);
	rtl8822bu_operation_leave(adapter);
	return error;

out_error:
	enabled = spin_lock_irqsave(&adapter->lock);
	if (error != EBUSY) {
		adapter->quarantined = 1U;
		adapter->tx_quiescing = 1U;
	}
	spin_unlock_irqrestore(&adapter->lock, enabled);
	rtl8822bu_operation_leave(adapter);
	return error;
}

static int
rtl8822bu_key_delete_checked(struct rtl8822bu_adapter *adapter,
	uint64_t generation, enum rtl8822bu_key_role role, uint8_t key_index,
	uint64_t key_generation, uint64_t deadline)
{
	enum rtl8822bu_key_location {
		RTL8822BU_KEY_ABSENT,
		RTL8822BU_KEY_ACTIVE,
		RTL8822BU_KEY_STAGED,
		RTL8822BU_KEY_RETIRED
	};
	unsigned long enabled;
	uint64_t now;
	uint8_t slot = 0U;
	enum rtl8822bu_key_location location = RTL8822BU_KEY_ABSENT;
	int present = 0;
	int known_generation = 0;
	int uncertain;
	int error;

	if (adapter == NULL || generation == 0U || key_generation == 0U ||
	    key_index >= RTL8822BU_GROUP_KEY_COUNT ||
	    (role != RTL8822BU_KEY_PAIRWISE && role != RTL8822BU_KEY_GROUP))
		return EINVAL;
	now = clock_ticks();
	if (role == RTL8822BU_KEY_PAIRWISE && key_index != 0U)
		return EINVAL;
	enabled = spin_lock_irqsave(&adapter->lock);
	rtl8822bu_tx_report_reap_locked(adapter, now);
	if (role == RTL8822BU_KEY_PAIRWISE) {
		known_generation = adapter->pairwise_key_generation != 0U ||
		    adapter->pairwise_staged_generation != 0U ||
		    adapter->pairwise_retired_valid;
		if (adapter->pairwise_key_generation == key_generation) {
			location = RTL8822BU_KEY_ACTIVE;
			slot = adapter->pairwise_key_slot;
			present = adapter->pairwise_key_installed;
		} else if (adapter->pairwise_staged_generation ==
		    key_generation) {
			location = RTL8822BU_KEY_STAGED;
			slot = adapter->pairwise_staged_slot;
			present = adapter->pairwise_staged_installed;
		} else if (adapter->pairwise_retired_valid &&
		    adapter->pairwise_retired_generation == key_generation) {
			location = RTL8822BU_KEY_RETIRED;
			slot = adapter->pairwise_retired_slot;
		}
	} else {
		known_generation = adapter->group_key_generation[key_index] != 0U ||
		    adapter->group_staged_generation[key_index] != 0U ||
		    (adapter->group_retired_mask & (1U << key_index)) != 0U;
		if (adapter->group_key_generation[key_index] == key_generation) {
			location = RTL8822BU_KEY_ACTIVE;
			slot = adapter->group_key_slot[key_index];
			present = (adapter->group_key_mask &
			    (1U << key_index)) != 0U;
		} else if (adapter->group_staged_generation[key_index] ==
		    key_generation) {
			location = RTL8822BU_KEY_STAGED;
			slot = adapter->group_staged_slot[key_index];
			present = (adapter->group_staged_mask &
			    (1U << key_index)) != 0U;
		} else if ((adapter->group_retired_mask &
		    (1U << key_index)) != 0U &&
		    adapter->group_retired_generation[key_index] ==
		    key_generation) {
			location = RTL8822BU_KEY_RETIRED;
			slot = adapter->group_retired_slot[key_index];
		}
	}
	uncertain = location != RTL8822BU_KEY_ABSENT &&
	    (adapter->cam_uncertain_mask & (1U << slot)) != 0U;
	if (adapter->connection_generation != generation) {
		error = ESTALE;
	} else if (adapter->transport_absent) {
		/* A FORCE detach is itself proof that every hardware CAM entry is
		 * absent.  Retire only the exact software ledger requested by the
		 * common engine; do not issue register I/O after the USB submit gate
		 * has closed. */
		if (location == RTL8822BU_KEY_ABSENT) {
			error = known_generation ? ESTALE : 0;
		} else {
			adapter->cam_uncertain_mask &= ~(1U << slot);
			if (role == RTL8822BU_KEY_PAIRWISE) {
				if (location == RTL8822BU_KEY_ACTIVE) {
					adapter->pairwise_key_installed = 0U;
					adapter->pairwise_key_generation = 0U;
				} else if (location == RTL8822BU_KEY_STAGED) {
					adapter->pairwise_staged_installed = 0U;
					adapter->pairwise_staged_generation = 0U;
				} else {
					adapter->pairwise_retired_valid = 0U;
					adapter->pairwise_retired_generation = 0U;
					adapter->pairwise_retired_slot = 0U;
				}
			} else if (location == RTL8822BU_KEY_ACTIVE) {
				adapter->group_key_mask &=
				    (uint8_t)~(1U << key_index);
				adapter->group_key_generation[key_index] = 0U;
			} else if (location == RTL8822BU_KEY_STAGED) {
				adapter->group_staged_mask &=
				    (uint8_t)~(1U << key_index);
				adapter->group_staged_generation[key_index] = 0U;
			} else {
				adapter->group_retired_mask &=
				    (uint8_t)~(1U << key_index);
				adapter->group_retired_generation[key_index] = 0U;
				adapter->group_retired_slot[key_index] = 0U;
			}
			error = 0;
		}
	} else if (now >= deadline) {
		error = ETIMEDOUT;
	} else if (!adapter->ready || !adapter->opened ||
	    adapter->closing ||
	    adapter->stopping ||
	    !adapter->radio_running) {
		error = adapter->ready ? ENETDOWN : ENODEV;
	} else if (location == RTL8822BU_KEY_ABSENT) {
		error = known_generation ? ESTALE : 0;
	} else if (location == RTL8822BU_KEY_RETIRED) {
		/* Activation already cleared the colliding hardware entry.  Retain a
		 * software tombstone until the common layer retires the old generation,
		 * then make that exact inverse idempotent without touching the new key. */
		if (role == RTL8822BU_KEY_PAIRWISE) {
			adapter->pairwise_retired_valid = 0U;
			adapter->pairwise_retired_generation = 0U;
			adapter->pairwise_retired_slot = 0U;
		} else {
			adapter->group_retired_mask &=
			    (uint8_t)~(1U << key_index);
			adapter->group_retired_generation[key_index] = 0U;
			adapter->group_retired_slot[key_index] = 0U;
		}
		error = 0;
	} else if (!present && !uncertain) {
		/* A queue/operation retry may reserve a generation before any CAM
		 * write.  Cancellation must retire that reservation as well. */
		if (role == RTL8822BU_KEY_PAIRWISE) {
			if (location == RTL8822BU_KEY_ACTIVE)
				adapter->pairwise_key_generation = 0U;
			else
				adapter->pairwise_staged_generation = 0U;
		} else if (location == RTL8822BU_KEY_ACTIVE) {
			adapter->group_key_generation[key_index] = 0U;
		} else {
			adapter->group_staged_generation[key_index] = 0U;
		}
		error = 0;
	} else if (location == RTL8822BU_KEY_ACTIVE &&
	    rtl8822bu_tx_report_generation_active_locked(adapter,
	    generation, key_generation, 1)) {
		adapter->tx_quiescing = 1U;
		error = EBUSY;
	} else if (adapter->radio_operations_active != 0U) {
		adapter->tx_quiescing = 1U;
		error = EBUSY;
	} else {
		adapter->tx_quiescing = 1U;
		adapter->radio_operations_active++;
		error = EINPROGRESS;
	}
	spin_unlock_irqrestore(&adapter->lock, enabled);
	if (error != EINPROGRESS)
		return error;
	error = rtl8822b_tx_queues_empty(&adapter->radio, deadline);
	if (error != 0) {
		enabled = spin_lock_irqsave(&adapter->lock);
		if (error != EBUSY) {
			adapter->cam_uncertain_mask |= 1U << slot;
			adapter->quarantined = 1U;
		}
		rtl8822bu_tx_quiesce_result_locked(adapter, error, 0);
		spin_unlock_irqrestore(&adapter->lock, enabled);
		rtl8822bu_operation_leave(adapter);
		return error;
	}
	error = rtl8822b_cam_clear(&adapter->radio, slot, deadline);
	enabled = spin_lock_irqsave(&adapter->lock);
	if (error == 0) {
		adapter->cam_uncertain_mask &= ~(1U << slot);
		if (role == RTL8822BU_KEY_PAIRWISE) {
			if (location == RTL8822BU_KEY_ACTIVE) {
				adapter->pairwise_key_installed = 0U;
				adapter->pairwise_key_generation = 0U;
			} else {
				adapter->pairwise_staged_installed = 0U;
				adapter->pairwise_staged_generation = 0U;
			}
		} else {
			if (location == RTL8822BU_KEY_ACTIVE) {
				adapter->group_key_mask &=
				    (uint8_t)~(1U << key_index);
				adapter->group_key_generation[key_index] = 0U;
			} else {
				adapter->group_staged_mask &=
				    (uint8_t)~(1U << key_index);
				adapter->group_staged_generation[key_index] = 0U;
			}
		}
		adapter->tx_quiescing = 0U;
		if (adapter->connection_generation != generation)
			error = ESTALE;
	} else {
		adapter->cam_uncertain_mask |= 1U << slot;
		adapter->tx_quiescing = 1U;
		adapter->quarantined = 1U;
	}
	spin_unlock_irqrestore(&adapter->lock, enabled);
	rtl8822bu_operation_leave(adapter);
	return error;
}

static void
rtl8822bu_deauthenticate_best_effort(struct rtl8822bu_adapter *adapter,
	const uint8_t bssid[6], uint64_t deadline)
{
	uint8_t wire[RTL8822B_MANAGEMENT_TX_DESCRIPTOR_SIZE + 26U + 1U];
	uint64_t now, remaining, milliseconds;
	size_t actual = 0U, wire_length = 0U;
	int error;

	if (adapter == NULL || bssid == NULL ||
	    adapter->transport_absent ||
	    adapter->radio.state != RTL8822B_RADIO_STARTED)
		return;
	error = rtl8822b_radio_deauthentication_prepare(&adapter->radio, wire,
	    sizeof(wire), bssid, adapter->board.mac_address, 3U, &wire_length);
	now = clock_ticks();
	if (error != 0 || now >= deadline)
		goto out;
	remaining = deadline - now;
	milliseconds = remaining > UINT64_MAX / 1000ULL ? UINT64_MAX :
	    (remaining * 1000ULL) / KERN_CLOCK_HZ;
	if (milliseconds == 0U)
		milliseconds = 1U;
	if (milliseconds > RTL8822BU_REGISTER_TIMEOUT_MS)
		milliseconds = RTL8822BU_REGISTER_TIMEOUT_MS;
	error = drv_usb_bulk(adapter->usb_device, adapter->bulk_out_high, wire,
	    wire_length, (unsigned)milliseconds, &actual);
	if (error == EPIPE &&
	    drv_usb_endpoint_clear_halt(adapter->bulk_out_high) == 0) {
		actual = 0U;
		(void)drv_usb_bulk(adapter->usb_device, adapter->bulk_out_high,
		    wire, wire_length, (unsigned)milliseconds, &actual);
	}
out:
	memset(wire, 0, sizeof(wire));
}

static int
rtl8822bu_disconnect(void *context, uint64_t generation)
{
	struct rtl8822bu_adapter *adapter = context;
	uint8_t bssid[6];
	unsigned long enabled;
	uint64_t deadline;
	uint64_t now;
	int send_deauthentication = 0;
	int error;

	if (adapter == NULL)
		return ENODEV;
	now = clock_ticks();
	enabled = spin_lock_irqsave(&adapter->lock);
	rtl8822bu_tx_report_reap_locked(adapter, now);
	if (adapter->connection_generation == 0U &&
	    !adapter->association_active && !adapter->association_uncertain &&
	    adapter->cam_uncertain_mask == 0U &&
	    !adapter->pairwise_key_installed &&
	    !adapter->pairwise_staged_installed &&
	    adapter->pairwise_staged_generation == 0U &&
	    adapter->group_key_mask == 0U &&
	    adapter->group_staged_mask == 0U) {
		error = 0;
	} else if (generation != 0U &&
	    adapter->connection_generation != generation) {
		error = ESTALE;
	} else if (adapter->transport_absent) {
		if (adapter->pairwise_key_installed ||
		    adapter->pairwise_staged_installed ||
		    adapter->pairwise_staged_generation != 0U ||
		    adapter->pairwise_retired_valid ||
		    adapter->group_key_mask != 0U ||
		    adapter->group_staged_mask != 0U ||
		    adapter->group_retired_mask != 0U) {
			error = EBUSY;
		} else {
			rtl8822bu_connection_state_clear_locked(adapter);
			error = 0;
		}
	} else if (rtl8822bu_tx_report_generation_active_locked(adapter,
	    adapter->connection_generation, 0U, 0)) {
		adapter->tx_quiescing = 1U;
		error = EBUSY;
	} else if (adapter->radio_operations_active != 0U) {
		adapter->tx_quiescing = 1U;
		error = EBUSY;
	} else {
		adapter->tx_quiescing = 1U;
		adapter->radio_operations_active++;
		if (adapter->association_active &&
		    adapter->deauthentication_attempted_generation !=
		    adapter->connection_generation) {
			adapter->deauthentication_attempted_generation =
			    adapter->connection_generation;
			memcpy(bssid, adapter->connection_bssid, sizeof(bssid));
			send_deauthentication = 1;
		}
		error = EINPROGRESS;
	}
	adapter->scan_generation = 0U;
	adapter->scan_channel = 0U;
	spin_unlock_irqrestore(&adapter->lock, enabled);
	if (error != EINPROGRESS)
		return error;
	error = rtl8822bu_deadline_after(RTL8822BU_SECURITY_TIMEOUT_TICKS,
	    &deadline);
	if (error == 0 && send_deauthentication)
		rtl8822bu_deauthenticate_best_effort(adapter, bssid, deadline);
	if (error == 0 && adapter->radio.state == RTL8822B_RADIO_STARTED)
		error = rtl8822bu_security_hardware_clear(adapter, deadline);
	enabled = spin_lock_irqsave(&adapter->lock);
	if (error == 0) {
		rtl8822bu_connection_state_clear_locked(adapter);
		if (adapter->radio.state == RTL8822B_RADIO_STARTED &&
		    adapter->ready && adapter->opened && !adapter->closing &&
		    !adapter->stopping)
			adapter->quarantined = 0U;
	} else if (error != EBUSY) {
		adapter->tx_quiescing = 1U;
		adapter->quarantined = 1U;
	}
	spin_unlock_irqrestore(&adapter->lock, enabled);
	rtl8822bu_operation_leave(adapter);
	return error;
}

static int
rtl8822bu_management_transmit(void *context, uint64_t generation,
	const uint8_t *frame, size_t length, uint64_t deadline)
{
	struct rtl8822bu_adapter *adapter = context;
	uint8_t *wire;
	unsigned long enabled;
	uint64_t now, remaining, milliseconds;
	uint32_t channel;
	size_t wire_length = 0U, actual = 0U;
	uint8_t sequence = 0U;
	int report_reserved = 0;
	int transfer_attempted = 0;
	int error;

	if (adapter == NULL || generation == 0U || frame == NULL ||
	    length == 0U || length > RTL8822B_MANAGEMENT_MPDU_MAX)
		return EINVAL;
	now = clock_ticks();
	if (now >= deadline)
		return ETIMEDOUT;
	enabled = spin_lock_irqsave(&adapter->lock);
	if (!adapter->ready || adapter->detaching || !adapter->opened ||
	    adapter->closing ||
	    adapter->stopping || adapter->recovery_pending ||
	    adapter->quarantined ||
	    !adapter->radio_running || adapter->scan_generation != generation) {
		error = adapter->ready ? ENETDOWN : ENODEV;
		spin_unlock_irqrestore(&adapter->lock, enabled);
		return error;
	}
	if (adapter->tx_quiescing) {
		spin_unlock_irqrestore(&adapter->lock, enabled);
		return EBUSY;
	}
	channel = adapter->scan_channel;
	adapter->radio_operations_active++;
	error = rtl8822bu_tx_report_reserve_locked(adapter, generation, 0U,
	    generation, now, deadline, &sequence);
	if (error == 0)
		report_reserved = 1;
	spin_unlock_irqrestore(&adapter->lock, enabled);
	if (error != 0)
		goto out_operation;
	/* The core permits only its calibrated, worldwide-bounded legacy-rate
	 * 2.4-GHz profile; every broader transmit mode remains closed. */
	if (!rtl8822b_radio_active_scan_allowed(&adapter->radio,
	    (uint8_t)channel)) {
		error = EPERM;
		goto out_operation;
	}
	wire = hal_malloc(RTL8822B_MANAGEMENT_TX_DESCRIPTOR_SIZE + length + 1U);
	if (wire == NULL) {
		error = ENOMEM;
		goto out_operation;
	}
	error = rtl8822b_radio_management_frame_prepare(&adapter->radio, wire,
	    RTL8822B_MANAGEMENT_TX_DESCRIPTOR_SIZE + length + 1U, frame, length,
	    &wire_length);
	if (error == 0)
		error = rtl8822bu_tx_descriptor_request_report(wire, wire_length,
		    sequence);
	now = clock_ticks();
	if (error == 0 && now >= deadline)
		error = ETIMEDOUT;
	if (error == 0) {
		remaining = deadline - now;
		milliseconds = remaining > UINT64_MAX / 1000ULL ? UINT64_MAX :
		    (remaining * 1000ULL) / KERN_CLOCK_HZ;
		if (milliseconds == 0U)
			milliseconds = 1U;
		if (milliseconds > UINT32_MAX)
			milliseconds = UINT32_MAX;
		/* The frozen three-OUT RQPN map routes the management QSEL to the
		 * high-priority DMA pipe, endpoint 0x05. */
		transfer_attempted = 1;
		error = rtl8822bu_bulk_transfer(adapter,
		    adapter->bulk_out_high, wire, wire_length,
		    (unsigned)milliseconds, &actual);
		if (error == 0 && actual != wire_length)
			error = EIO;
	}
	memset(wire, 0,
	    RTL8822B_MANAGEMENT_TX_DESCRIPTOR_SIZE + length + 1U);
	hal_free(wire);
out_operation:
	if (error != 0 && report_reserved) {
		if (transfer_attempted)
			rtl8822bu_tx_report_abandon_attempted(adapter, sequence,
			    error);
		else
			rtl8822bu_tx_report_release(adapter, sequence);
	}
	rtl8822bu_operation_leave(adapter);
	return error;
}

static int
rtl8822bu_key_install(void *context,
	const struct wlan_radio_key_request *request)
{
	struct rtl8822bu_adapter *adapter = context;
	enum rtl8822bu_key_role role;

	if (adapter == NULL || request == NULL || request->key_index >=
	    RTL8822BU_GROUP_KEY_COUNT || request->receive_packet_number >
	    0x0000ffffffffffffULL || (request->kind !=
	    WLAN_RADIO_KEY_PAIRWISE && request->kind != WLAN_RADIO_KEY_GROUP))
		return EINVAL;
	role = request->kind == WLAN_RADIO_KEY_PAIRWISE ?
	    RTL8822BU_KEY_PAIRWISE : RTL8822BU_KEY_GROUP;
	return rtl8822bu_key_install_checked(adapter, request->generation, role,
	    request->key_index, request->address, request->key,
	    request->key_generation, request->deadline_ticks);
}

static int
rtl8822bu_key_delete(void *context, uint64_t generation,
	enum wlan_radio_key_kind kind, uint8_t key_index,
	uint64_t key_generation, uint64_t deadline)
{
	struct rtl8822bu_adapter *adapter = context;
	enum rtl8822bu_key_role role;

	if (adapter == NULL || key_index >= RTL8822BU_GROUP_KEY_COUNT ||
	    (kind != WLAN_RADIO_KEY_PAIRWISE && kind != WLAN_RADIO_KEY_GROUP))
		return EINVAL;
	role = kind == WLAN_RADIO_KEY_PAIRWISE ? RTL8822BU_KEY_PAIRWISE :
	    RTL8822BU_KEY_GROUP;
	return rtl8822bu_key_delete_checked(adapter, generation, role,
	    key_index, key_generation, deadline);
}

static int
rtl8822bu_keys_activate(void *context, uint64_t generation,
	uint64_t pairwise_key_generation, uint64_t group_key_generation,
	uint64_t deadline)
{
	return rtl8822bu_keys_activate_checked(context, generation,
	    pairwise_key_generation, group_key_generation, deadline);
}

static int
rtl8822bu_quiesce(void *context)
{
	struct rtl8822bu_adapter *adapter = context;
	unsigned long enabled;
	int error;

	if (adapter == NULL)
		return ENODEV;
	error = rtl8822bu_disconnect(adapter, 0U);
	enabled = spin_lock_irqsave(&adapter->lock);
	adapter->scan_generation = 0U;
	adapter->scan_channel = 0U;
	spin_unlock_irqrestore(&adapter->lock, enabled);
	return error;
}

static const struct wlan_radio_ops rtl8822bu_radio_ops = {
	.scan_channel_start = rtl8822bu_scan_channel_start,
	.scan_stop = rtl8822bu_scan_stop,
	.connect_start = rtl8822bu_connect_start,
	.disconnect = rtl8822bu_disconnect,
	.management_transmit = rtl8822bu_management_transmit,
	.association_set = rtl8822bu_association_set,
	.association_clear = rtl8822bu_association_clear,
	.frame_transmit = rtl8822bu_frame_transmit,
	.key_install = rtl8822bu_key_install,
	.key_delete = rtl8822bu_key_delete,
	.keys_activate = rtl8822bu_keys_activate,
	.quiesce = rtl8822bu_quiesce
};

static int
rtl8822bu_hardware_start_locked(struct rtl8822bu_adapter *adapter)
{
	struct rtl8822b_firmware_blob firmware;
	struct rtl8822b_radio_transport transport;
	unsigned long enabled;
	uint64_t deadline;
	int error, cleanup_error;

	memset(&firmware, 0, sizeof(firmware));
	if (!rtl8822bu_ready_station(adapter, NULL)) {
		error = ENODEV;
		goto done;
	}
	enabled = spin_lock_irqsave(&adapter->lock);
	if (adapter->opened || adapter->closing || adapter->stopping ||
	    adapter->quarantined || adapter->radio_running ||
	    adapter->firmware_running)
		error = adapter->quarantined ? EIO : EBUSY;
	else
		error = 0;
	spin_unlock_irqrestore(&adapter->lock, enabled);
	if (error != 0)
		goto done;
	error = rtl8822bu_deadline_after(RTL8822BU_RADIO_OPEN_TIMEOUT_TICKS,
	    &deadline);
	if (error != 0)
		goto done;
	rtl8822bu_radio_transport_init(adapter, &transport);
	error = rtl8822b_radio_power_on(&adapter->radio, &transport,
	    &adapter->board, deadline);
	if (error != 0)
		goto fail_hardware;
	error = RTL8822BU_FIRMWARE_LOAD(&firmware);
	if (error != 0)
		goto fail_hardware;
	error = rtl8822bu_firmware_download(adapter, &firmware.view);
	if (error != 0)
		goto fail_hardware;
	enabled = spin_lock_irqsave(&adapter->lock);
	adapter->firmware_running = 1U;
	spin_unlock_irqrestore(&adapter->lock, enabled);
	error = rtl8822b_radio_start(&adapter->radio, deadline);
	if (error != 0)
		goto fail_hardware;
	enabled = spin_lock_irqsave(&adapter->lock);
	adapter->radio_running = 1U;
	spin_unlock_irqrestore(&adapter->lock, enabled);
	error = rtl8822bu_rx_start(adapter, 0U, 1U);
	if (error != 0)
		goto fail_hardware;
	RTL8822BU_FIRMWARE_RELEASE(&firmware);
	return 0;

fail_hardware:
	/* The staging lease remains owned until RX cancellation and the checked
	 * core reset have completed, so every failure follows one reverse-order
	 * transaction boundary. */
	cleanup_error = rtl8822bu_hardware_stop_locked(adapter);
	rtl8822bu_record_cleanup_error(&error, cleanup_error);
	RTL8822BU_FIRMWARE_RELEASE(&firmware);
done:
	return error;
}

static int
rtl8822bu_open(struct net_device *device)
{
	struct rtl8822bu_adapter *adapter = device->driver_data;
	int error, cleanup_error;

	/* net_device_create() makes the name visible before station attachment.
	 * Do not recurse into lifecycle_lock from that publication callback. */
	if (!rtl8822bu_ready_station(adapter, NULL))
		return ENODEV;
	mutex_lock(&adapter->lifecycle_lock);
	error = net_device_set_carrier(device, 0);
	if (error == 0)
		error = rtl8822bu_hardware_start_locked(adapter);
	if (error == 0)
		error = wlan_station_open(adapter->station);
	if (error != 0) {
		cleanup_error = rtl8822bu_hardware_stop_locked(adapter);
		rtl8822bu_record_cleanup_error(&error, cleanup_error);
	}
	mutex_unlock(&adapter->lifecycle_lock);
	return error;
}

static int
rtl8822bu_station_close_wait(struct wlan_station *station)
{
	uint64_t deadline;
	int error;

	if (station == NULL)
		return ENODEV;
	error = rtl8822bu_deadline_after(
	    RTL8822BU_STATION_CLOSE_TIMEOUT_TICKS, &deadline);
	if (error != 0)
		return error;
	for (;;) {
		error = wlan_station_close(station);
		if (error != EBUSY)
			return error;
		if (clock_ticks() >= deadline)
			return ETIMEDOUT;
		/* No adapter spin or common-WLAN lock is retained here.  An already
		 * admitted channel/TX callback can therefore retire before retry. */
		sched_yield();
	}
}

/*
 * Runtime USB recovery is a device-local lifecycle transaction.  The caller
 * has already retired the terminal RX completion and left poll context, so
 * hardware_stop can join every remaining producer without waiting on itself.
 * Common WLAN link-loss cleanup retires the controlled port, CAM generations,
 * and packet-number state while retaining the selected BSS and PMK.  The
 * device is then restarted locally; the common finite reconnect worker owns
 * the fresh handshake.  Explicit IFF_DOWN/remove still use station_close and
 * deliberately erase that reconnect intent.
 */
static void
rtl8822bu_runtime_recover(struct rtl8822bu_adapter *adapter)
{
	struct wlan_station *station;
	unsigned long enabled;
	uint64_t generation;
	int reason;
	int error = 0;
	int cleanup_error;
	int hardware_stopped = 0;
	int schedule = 0;

	if (adapter == NULL)
		return;
	mutex_lock(&adapter->lifecycle_lock);
	enabled = spin_lock_irqsave(&adapter->lock);
	if (!adapter->ready || adapter->detaching || !adapter->opened ||
	    !adapter->recovery_pending || adapter->recovery_active) {
		spin_unlock_irqrestore(&adapter->lock, enabled);
		mutex_unlock(&adapter->lifecycle_lock);
		return;
	}
	adapter->recovery_active = 1U;
	station = adapter->station;
	generation = adapter->connection_generation;
	reason = adapter->recovery_error != 0 ? adapter->recovery_error : EIO;
	spin_unlock_irqrestore(&adapter->lock, enabled);
	if (adapter->net_device != NULL)
		(void)net_device_set_carrier(adapter->net_device, 0);
	if (generation != 0U) {
		error = wlan_station_report_link_loss(station, generation, reason);
	}
	if (error != 0) {
		/* The common station still owns at least one controlled-port/key/PN
		 * inverse.  Never reset the device underneath that ownership: doing so
		 * would leave subsequent checked deletes unable to prove absence.  A
		 * bounded series of poll-worker retries leaves inverse callbacks usable;
		 * after the bound, explicit down/remove may still finish cleanup. */
		enabled = spin_lock_irqsave(&adapter->lock);
		if (adapter->recovery_cleanup_attempts != UINT_MAX)
			adapter->recovery_cleanup_attempts++;
		adapter->recovery_active = 0U;
		adapter->recovery_pending = 1U;
		adapter->recovery_error = reason;
		adapter->tx_quiescing = 1U;
		if (adapter->recovery_cleanup_attempts <
		    RTL8822BU_RECOVERY_CLEANUP_RETRY_LIMIT) {
			schedule = adapter->ready && !adapter->detaching &&
			    adapter->opened && adapter->net_device != NULL;
		} else {
			adapter->quarantined = 1U;
		}
		spin_unlock_irqrestore(&adapter->lock, enabled);
		mutex_unlock(&adapter->lifecycle_lock);
		if (schedule)
			net_device_schedule_poll(adapter->net_device);
		return;
	}
	if (error == 0) {
		error = rtl8822bu_hardware_stop_locked(adapter);
		hardware_stopped = error == 0;
	}
	if (error == 0)
		error = rtl8822bu_hardware_start_locked(adapter);
	if (error != 0 && hardware_stopped) {
		/* If start crossed any acquisition boundary, use the same checked
		 * reverse transaction.  Failure preserves quarantine and ownership. */
		cleanup_error = rtl8822bu_hardware_stop_locked(adapter);
		rtl8822bu_record_cleanup_error(&error, cleanup_error);
	}
	enabled = spin_lock_irqsave(&adapter->lock);
	adapter->recovery_pending = 0U;
	adapter->recovery_active = 0U;
	adapter->recovery_cleanup_attempts = 0U;
	adapter->recovery_error = 0;
	adapter->rx_error_streak = 0U;
	if (error != 0)
		adapter->quarantined = 1U;
	spin_unlock_irqrestore(&adapter->lock, enabled);
	mutex_unlock(&adapter->lifecycle_lock);
}

static void
rtl8822bu_close(struct net_device *device)
{
	struct rtl8822bu_adapter *adapter = device->driver_data;
	struct wlan_station *station;
	unsigned long enabled;
	int error;

	mutex_lock(&adapter->lifecycle_lock);
	(void)net_device_set_carrier(device, 0);
	enabled = spin_lock_irqsave(&adapter->lock);
	station = adapter->station;
	spin_unlock_irqrestore(&adapter->lock, enabled);
	error = rtl8822bu_station_close_wait(station);
	if (error == 0 || error == ENODEV)
		(void)rtl8822bu_hardware_stop_locked(adapter);
	else {
		enabled = spin_lock_irqsave(&adapter->lock);
		adapter->quarantined = 1U;
		spin_unlock_irqrestore(&adapter->lock, enabled);
	}
	mutex_unlock(&adapter->lifecycle_lock);
}

static int
rtl8822bu_transmit(struct net_device *device, struct packet_buf *packet)
{
	struct rtl8822bu_adapter *adapter = device->driver_data;
	struct wlan_station *station;
	unsigned long enabled;
	int error;

	if (packet == NULL)
		return EINVAL;
	enabled = spin_lock_irqsave(&adapter->lock);
	if (!adapter->ready || adapter->detaching || adapter->station == NULL ||
	    !adapter->opened ||
	    adapter->closing || adapter->stopping ||
	    adapter->recovery_pending || adapter->quarantined) {
		error = adapter->ready ? ENETDOWN : ENODEV;
		station = NULL;
	} else if (adapter->tx_quiescing) {
		error = EBUSY;
		station = NULL;
	} else {
		station = adapter->station;
		adapter->radio_operations_active++;
		error = 0;
	}
	spin_unlock_irqrestore(&adapter->lock, enabled);
	if (station == NULL) {
		packet_buf_free(packet);
		return error;
	}
	/* wlan_station_transmit() owns packet on every return path.  Keeping the
	 * Ethernet-to-802.11/CCMP conversion in the common station layer leaves
	 * this USB backend responsible only for the exact radio transaction.  The
	 * outer operation lease also pins station until hardware-stop has joined a
	 * transmit admitted immediately before net-device removal. */
	error = wlan_station_transmit(station, packet);
	rtl8822bu_operation_leave(adapter);
	return error;
}

static unsigned
rtl8822bu_poll_receive(struct net_device *device, unsigned budget)
{
	struct rtl8822bu_adapter *adapter = device->driver_data;
	struct rtl8822bu_rx_report_context report;
	enum drv_usb_urb_status status = DRV_USB_URB_IO_ERROR;
	unsigned long enabled;
	uint64_t completed_generation = 0U;
	size_t length, packet_count = 0U;
	int clear_error = 0, error = 0, recover = 0;
	int take_ready = 0, take_rearm = 0;

	if (budget == 0U)
		return 0U;
	enabled = spin_lock_irqsave(&adapter->lock);
	recover = adapter->ready && !adapter->detaching && adapter->opened &&
	    !adapter->closing &&
	    !adapter->stopping && adapter->recovery_pending &&
	    !adapter->recovery_active;
	spin_unlock_irqrestore(&adapter->lock, enabled);
	if (recover) {
		rtl8822bu_runtime_recover(adapter);
		return 1U;
	}
	if (!rtl8822bu_poll_enter(adapter))
		return 0U;
	enabled = spin_lock_irqsave(&adapter->lock);
	if (adapter->rx_ready) {
		adapter->rx_ready = 0U;
		completed_generation = adapter->rx_completed_generation;
		take_ready = 1;
	}
	spin_unlock_irqrestore(&adapter->lock, enabled);
	if (take_ready) {
		error = drv_usb_urb_drain(adapter->rx_urb,
		    RTL8822BU_RX_DRAIN_TIMEOUT_MS);
		if (error == 0) {
			status = drv_usb_urb_status(adapter->rx_urb);
			error = rtl8822bu_urb_status_error(status);
		}
		enabled = spin_lock_irqsave(&adapter->lock);
		if (completed_generation == 0U ||
		    completed_generation != adapter->rx_inflight_generation) {
			/* The callback belongs to a retired submission.  It must not
			 * consume or rearm the current request. */
			error = ESTALE;
		} else {
			adapter->rx_inflight_generation = 0U;
			adapter->rx_completed_generation = 0U;
		}
		spin_unlock_irqrestore(&adapter->lock, enabled);
		length = drv_usb_urb_actual_length(adapter->rx_urb);
		memset(&report, 0, sizeof(report));
		report.adapter = adapter;
		if (error == 0)
			error = rtl8822b_rx_aggregate_walk(adapter->rx_buffer,
			    length, rtl8822bu_rx_report, &report, &packet_count);
		if (error != 0 && error != ESTALE) {
			device->rx_errors++;
		}
		/* A halted pipe is recovered locally.  Never reset the USB device or
		 * controller: unrelated storage owns independent endpoints and URBs. */
		if (status == DRV_USB_URB_STALL)
			clear_error = drv_usb_endpoint_clear_halt(adapter->bulk_in);
		enabled = spin_lock_irqsave(&adapter->lock);
		if (status == DRV_USB_URB_COMPLETE && error == 0) {
			adapter->rx_error_streak = 0U;
		} else if (error == ENODEV || status == DRV_USB_URB_DISCONNECTED) {
			adapter->quarantined = 1U;
		} else if (error != ESTALE) {
			adapter->rx_error_streak++;
			if (clear_error != 0 || adapter->rx_error_streak >=
			    RTL8822BU_RX_RECOVERY_LIMIT) {
				adapter->recovery_pending = 1U;
				adapter->recovery_error = clear_error != 0 ?
				    clear_error : (error != 0 ? error : EIO);
				adapter->tx_quiescing = 1U;
				recover = 1;
			}
		}
		if (adapter->ready && adapter->opened && !adapter->closing &&
		    !adapter->stopping && !adapter->recovery_pending &&
		    !adapter->quarantined &&
		    error != ESTALE)
			adapter->rx_rearm = 1U;
		spin_unlock_irqrestore(&adapter->lock, enabled);
	}
	enabled = spin_lock_irqsave(&adapter->lock);
	if (adapter->rx_rearm && !adapter->rx_rearm_active) {
		adapter->rx_rearm = 0U;
		adapter->rx_rearm_active = 1U;
		take_rearm = 1;
	}
	spin_unlock_irqrestore(&adapter->lock, enabled);
	if (take_rearm) {
		int rearm_error = rtl8822bu_rx_submit(adapter, 0);

		enabled = spin_lock_irqsave(&adapter->lock);
		adapter->rx_rearm_active = 0U;
		if (rearm_error != 0 && adapter->ready && adapter->opened &&
		    !adapter->closing && !adapter->stopping) {
			adapter->quarantined = 1U;
			device->rx_errors++;
		}
		spin_unlock_irqrestore(&adapter->lock, enabled);
	}
	if (!recover && rtl8822bu_rx_has_work(adapter))
		net_device_schedule_poll(device);
	rtl8822bu_poll_exit(adapter);
	if (recover) {
		(void)net_device_set_carrier(device, 0);
		rtl8822bu_runtime_recover(adapter);
	}
	return take_ready || take_rearm ? 1U : 0U;
}

static int
rtl8822bu_ioctl(struct net_device *device, unsigned long request,
	void *argument)
{
	struct rtl8822bu_adapter *adapter = device->driver_data;
	struct wlan_station *station;
	unsigned long enabled;
	int blocked;

	if (!rtl8822bu_ready_station(adapter, &station))
		return ENODEV;
	enabled = spin_lock_irqsave(&adapter->lock);
	blocked = adapter->recovery_pending || adapter->recovery_active ||
	    adapter->quarantined;
	spin_unlock_irqrestore(&adapter->lock, enabled);
	if (blocked)
		return ENETDOWN;
	return wlan_station_ioctl(device, request, argument);
}

static void
rtl8822bu_release(void *driver_data)
{
	struct rtl8822bu_adapter *adapter = driver_data;

	if (adapter == NULL)
		return;
	memset(adapter, 0, sizeof(*adapter));
	hal_free(adapter);
}

static void
rtl8822bu_usb_resources_free(struct rtl8822bu_adapter *adapter)
{
	if (adapter == NULL)
		return;
	drv_usb_urb_free(adapter->rx_urb);
	adapter->rx_urb = NULL;
	if (adapter->rx_buffer != NULL) {
		memset(adapter->rx_buffer, 0, RTL8822BU_RX_BUFFER_SIZE);
		hal_free(adapter->rx_buffer);
		adapter->rx_buffer = NULL;
	}
}

static const struct net_device_ops rtl8822bu_net_ops = {
	.open = rtl8822bu_open,
	.close = rtl8822bu_close,
	.transmit = rtl8822bu_transmit,
	.poll_receive = rtl8822bu_poll_receive,
	.ioctl = rtl8822bu_ioctl,
	.release = rtl8822bu_release
};

static void
rtl8822bu_scan_profile(struct wlan_scan_profile *profile)
{
	unsigned channel;

	memset(profile, 0, sizeof(*profile));
	/* The initial table set and channel programming cover 2.4-GHz channels
	 * 1--11.  The core permits its calibrated, worldwide-bounded legacy-rate
	 * TXAGC profile; HT, VHT, and 5-GHz transmission remain disabled. */
	profile->channel_count = 11U;
	for (channel = 1U; channel <= profile->channel_count; channel++) {
		profile->channels[channel - 1U].channel = channel;
		profile->channels[channel - 1U].center_frequency_mhz =
		    2407U + channel * 5U;
		profile->channels[channel - 1U].flags =
		    WLAN_SCAN_CHANNEL_ACTIVE_ALLOWED;
	}
}

static int
rtl8822bu_net_device_create(struct rtl8822bu_adapter *adapter)
{
	struct wlan_scan_profile profile;
	struct net_device *device;
	struct wlan_station *station = NULL;
	unsigned long enabled;
	unsigned index, detaching;
	int error = ENOSPC;

	device = net_device_alloc();
	if (device == NULL)
		return ENOSPC;
	adapter->net_device = device;
	device->flags = NET_DEVICE_BROADCAST | NET_DEVICE_MULTICAST;
	device->mtu = RTL8822BU_MTU;
	memcpy(device->hwaddr, adapter->board.mac_address, 6U);
	device->hwaddr_len = 6U;
	device->capabilities = NET_DEVICE_CAP_WLAN;
	device->ops = &rtl8822bu_net_ops;
	device->driver_data = adapter;
	for (index = 0U; index < NET_DEVICE_MAX; index++) {
		memcpy(device->name, "wlan", 4U);
		device->name[4] = (char)('0' + index);
		device->name[5] = '\0';
		error = net_device_create(device);
		if (error != EEXIST)
			break;
	}
	if (error != 0) {
		device->driver_data = NULL;
		adapter->net_device = NULL;
		net_device_destroy(device);
		return error;
	}
	enabled = spin_lock_irqsave(&adapter->lock);
	adapter->net_live = 1U;
	spin_unlock_irqrestore(&adapter->lock, enabled);
	rtl8822bu_scan_profile(&profile);
	error = wlan_station_attach(device, &rtl8822bu_radio_ops, adapter,
	    &profile, &station);
	memset(&profile, 0, sizeof(profile));
	if (error != 0)
		return error;
	enabled = spin_lock_irqsave(&adapter->lock);
	adapter->station = station;
	adapter->station_attached = 1U;
	detaching = adapter->detaching;
	adapter->ready = !detaching;
	spin_unlock_irqrestore(&adapter->lock, enabled);
	if (detaching)
		return ENODEV;
	return 0;
}

/*
 * This is the sole terminal owner of the published graph.  lifecycle_lock is
 * held across net_device_create() through wlan_station_attach(), and by close
 * and detach, so none of those paths can observe or retire a half-published
 * station.  A checked failure leaves the graph intact and ready=0 so the USB
 * core may retry detach without a dangling driver_data pointer.
 */
static int
rtl8822bu_teardown(struct drv_usb_interface *interface,
	struct rtl8822bu_adapter *adapter, unsigned flags)
{
	struct net_device *device;
	struct wlan_station *station;
	unsigned long enabled;
	unsigned net_live;
	int error;

	mutex_lock(&adapter->lifecycle_lock);
	enabled = spin_lock_irqsave(&adapter->lock);
	adapter->detaching = 1U;
	if ((flags & DRV_USB_DETACH_FORCE) != 0U)
		adapter->transport_absent = 1U;
	device = adapter->net_device;
	net_live = adapter->net_live;
	spin_unlock_irqrestore(&adapter->lock, enabled);
	mutex_unlock(&adapter->lifecycle_lock);
	if (device != NULL)
		(void)net_device_set_carrier(device, 0);
	/* net_device_gone() may synchronously call ops->close().  Do not hold the
	 * lifecycle mutex across that callback edge.  detaching/ready already
	 * closes admission while close takes the mutex and drains the URB. */
	if (device != NULL && net_live) {
		error = net_device_gone(device);
		if (error != 0)
			return error;
		mutex_lock(&adapter->lifecycle_lock);
		enabled = spin_lock_irqsave(&adapter->lock);
		adapter->net_live = 0U;
		spin_unlock_irqrestore(&adapter->lock, enabled);
		mutex_unlock(&adapter->lifecycle_lock);
	}
	mutex_lock(&adapter->lifecycle_lock);
	enabled = spin_lock_irqsave(&adapter->lock);
	station = adapter->station;
	spin_unlock_irqrestore(&adapter->lock, enabled);
	if (station != NULL && adapter->station_attached) {
		error = wlan_station_close(station);
		if (error != 0 && error != ENODEV) {
			mutex_unlock(&adapter->lifecycle_lock);
			return error;
		}
	}
	error = rtl8822bu_hardware_stop_locked(adapter);
	if (error != 0) {
		mutex_unlock(&adapter->lifecycle_lock);
		return error;
	}
	enabled = spin_lock_irqsave(&adapter->lock);
	adapter->ready = 0U;
	spin_unlock_irqrestore(&adapter->lock, enabled);
	if (station != NULL && adapter->station_attached) {
		error = wlan_station_detach(station);
		/* Global network shutdown may already have joined this station. */
		if (error != 0 && error != ENODEV) {
			mutex_unlock(&adapter->lifecycle_lock);
			return error;
		}
		enabled = spin_lock_irqsave(&adapter->lock);
		adapter->station = NULL;
		adapter->station_attached = 0U;
		spin_unlock_irqrestore(&adapter->lock, enabled);
	}
	/* These USB-owned objects must not be held until the last external network
	 * reference releases the adapter: the URB itself retains USB device state. */
	rtl8822bu_usb_resources_free(adapter);
	error = drv_usb_interface_set_driver_data(interface, NULL);
	if (error != 0) {
		mutex_unlock(&adapter->lifecycle_lock);
		return error;
	}
	enabled = spin_lock_irqsave(&adapter->lock);
	adapter->net_device = NULL;
	spin_unlock_irqrestore(&adapter->lock, enabled);
	/* No callback can enter after net_device_gone().  Release the lifecycle
	 * lock before net_device_destroy() invokes the adapter release callback. */
	mutex_unlock(&adapter->lifecycle_lock);
	if (device != NULL)
		net_device_destroy(device);
	else
		rtl8822bu_release(adapter);
	return 0;
}

static int
rtl8822bu_match(struct drv_usb_interface *interface,
	const struct drv_usb_id *id)
{
	struct rtl8822bu_binding binding;

	(void)id;
	return rtl8822bu_binding_parse(interface, &binding) ? 200 : 0;
}

static int
rtl8822bu_attach(struct drv_usb_interface *interface,
	const struct drv_usb_id *id)
{
	struct rtl8822bu_binding binding;
	struct rtl8822bu_adapter *adapter;
	int error;

	(void)id;
	if (!rtl8822bu_binding_parse(interface, &binding))
		return ENODEV;
	adapter = hal_malloc(sizeof(*adapter));
	if (adapter == NULL)
		return ENOMEM;
	memset(adapter, 0, sizeof(*adapter));
	rtl8822bu_connection_state_clear_locked(adapter);
	adapter->usb_device = binding.device;
	adapter->interface = interface;
	adapter->bulk_in = binding.bulk_in;
	adapter->bulk_out_high = binding.bulk_out_high;
	adapter->bulk_out_normal = binding.bulk_out_normal;
	adapter->bulk_out_low = binding.bulk_out_low;
	spin_init(&adapter->lock, LOCK_RANK_DEVICE, "usb-rtl8822bu");
	error = mutex_init(&adapter->lifecycle_lock, LOCK_RANK_DEVICE,
	    "usb-rtl8822bu lifecycle");
	if (error != 0) {
		hal_free(adapter);
		return error;
	}
	error = drv_usb_interface_set_driver_data(interface, adapter);
	if (error != 0) {
		hal_free(adapter);
		return error;
	}
	mutex_lock(&adapter->lifecycle_lock);
	adapter->rx_buffer = hal_malloc(RTL8822BU_RX_BUFFER_SIZE);
	if (adapter->rx_buffer == NULL) {
		error = ENOMEM;
		goto fail_locked;
	}
	adapter->rx_urb = drv_usb_urb_alloc(adapter->usb_device,
	    adapter->bulk_in, 0U);
	if (adapter->rx_urb == NULL) {
		error = ENOMEM;
		goto fail_locked;
	}
	error = rtl8822bu_board_read(adapter, &adapter->board);
	if (error != 0)
		goto fail_locked;
	error = rtl8822bu_net_device_create(adapter);
	if (error != 0)
		goto fail_locked;
	mutex_unlock(&adapter->lifecycle_lock);
	hal_printf("usb-rtl8822bu: %s mac=%02x:%02x:%02x:%02x:%02x:%02x "
	    "cut=%u rfe=%u pre-radio\n", adapter->net_device->name,
	    adapter->board.mac_address[0], adapter->board.mac_address[1],
	    adapter->board.mac_address[2], adapter->board.mac_address[3],
	    adapter->board.mac_address[4], adapter->board.mac_address[5],
	    adapter->board.chip.cut, adapter->board.rfe_option);
	return 0;

fail_locked:
	mutex_unlock(&adapter->lifecycle_lock);
	{
		int cleanup_error = rtl8822bu_teardown(interface, adapter,
		    DRV_USB_DETACH_ATTACH_FAILED);

		return cleanup_error != 0 ? cleanup_error : error;
	}
}

static int
rtl8822bu_detach(struct drv_usb_interface *interface, unsigned flags)
{
	struct rtl8822bu_adapter *adapter;

	adapter = drv_usb_interface_driver_data(interface);
	if (adapter == NULL)
		return 0;
	return rtl8822bu_teardown(interface, adapter, flags);
}

static void
rtl8822bu_shutdown(struct drv_usb_interface *interface)
{
	struct rtl8822bu_adapter *adapter =
	    drv_usb_interface_driver_data(interface);
	struct wlan_station *station;
	unsigned long enabled;
	int error;

	if (adapter == NULL)
		return;
	mutex_lock(&adapter->lifecycle_lock);
	enabled = spin_lock_irqsave(&adapter->lock);
	adapter->detaching = 1U;
	station = adapter->station;
	spin_unlock_irqrestore(&adapter->lock, enabled);
	if (adapter->net_device != NULL)
		(void)net_device_set_carrier(adapter->net_device, 0);
	error = rtl8822bu_station_close_wait(station);
	if (error == 0 || error == ENODEV) {
		error = rtl8822bu_hardware_stop_locked(adapter);
		if (error == 0) {
			enabled = spin_lock_irqsave(&adapter->lock);
			adapter->ready = 0U;
			spin_unlock_irqrestore(&adapter->lock, enabled);
		}
	}
	else {
		enabled = spin_lock_irqsave(&adapter->lock);
		adapter->quarantined = 1U;
		spin_unlock_irqrestore(&adapter->lock, enabled);
	}
	mutex_unlock(&adapter->lifecycle_lock);
}

static const struct drv_usb_id rtl8822bu_ids[] = {{
	.match_flags = DRV_USB_ID_VENDOR | DRV_USB_ID_PRODUCT |
	    DRV_USB_ID_RELEASE_RANGE | DRV_USB_ID_IF_CLASS |
	    DRV_USB_ID_IF_SUBCLASS | DRV_USB_ID_IF_PROTOCOL |
	    DRV_USB_ID_IF_NUMBER,
	.vendor = RTL8822BU_VENDOR_ID,
	.product = RTL8822BU_PRODUCT_ID,
	.release_minimum = RTL8822BU_DEVICE_RELEASE,
	.release_maximum = RTL8822BU_DEVICE_RELEASE,
	.interface_class = RTL8822BU_INTERFACE_CLASS,
	.interface_subclass = RTL8822BU_INTERFACE_SUBCLASS,
	.interface_protocol = RTL8822BU_INTERFACE_PROTOCOL,
	.interface_number = 0U
}};

static struct drv_usb_driver rtl8822bu_driver = {
	.name = "usb-rtl8822bu",
	.ids = rtl8822bu_ids,
	.id_count = sizeof(rtl8822bu_ids) / sizeof(rtl8822bu_ids[0]),
	.match = rtl8822bu_match,
	.attach = rtl8822bu_attach,
	.detach = rtl8822bu_detach,
	.shutdown = rtl8822bu_shutdown
};

int
drv_usb_rtl8822bu_driver_register(void)
{
	return drv_usb_driver_register(&rtl8822bu_driver);
}
