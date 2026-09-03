/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: t; tab-width: 8 -*- */

/*
 * zedBSD Intel AX211 PCI/CNVio2 transport
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include <drivers/pci-intel-ax211.h>
#include <drivers/pci.h>

#include "intel-ax211-assoc.h"
#include "intel-ax211-bss.h"
#include "intel-ax211-boot.h"
#include "intel-ax211-key.h"
#include "intel-ax211-mmio.h"
#include "intel-ax211-pci-mmio.h"
#include "intel-ax211-rx.h"
#include "intel-ax211-runtime-start.h"
#include "intel-ax211-scan-session.h"
#include "intel-ax211-transport-backend.h"
#include "intel-ax211-tx-ring.h"

#include <errno.h>
#include <hal/hal.h>
#include <limits.h>
#include <kern/clock.h>
#include <kern/lock.h>
#include <kern/net/net-device.h>
#include <kern/net/packet-buf.h>
#include <kern/net/wlan.h>
#include <kern/sched.h>
#include <stdint.h>
#include <string.h>

#define AX211_PCI_VENDOR		0x8086U
#define AX211_PCI_PRODUCT		0x51f0U
#define AX211_PCI_SUBVENDOR		0x8086U
#define AX211_PCI_SUBPRODUCT		0x4090U
#define AX211_PCI_REVISION		0x01U
#define AX211_PCI_CLASS			0x028000U
#define AX211_PCI_CLASS_MASK		0xffffffU
#define AX211_PCI_COMMAND		0x04U
#define AX211_PCI_COMMAND_MEMORY	0x0002U
#define AX211_PCI_COMMAND_MASTER	0x0004U
#define AX211_BAR0_MINIMUM_SIZE		0x4000U
#define AX211_MTU			1500U
#define AX211_MAC_ADDRESS_SIZE		6U
#define AX211_PASSIVE_CHANNEL_COUNT	11U
#define AX211_RECEIVE_POLL_DELAY_US	50U
#define AX211_DIRECT_EVENT_LIMIT		64U
#define AX211_DEFERRED_EVENT_LIMIT	16U
#define AX211_DIRECT_TIMEOUT_US		1000000U
#define AX211_LIFECYCLE_JOIN_TICKS	(5U * KERN_CLOCK_HZ)
#define AX211_ASSOC_STATION_ID		0U
#define AX211_ASSOC_CCK_ACK_RATES	0x0fU
#define AX211_ASSOC_OFDM_ACK_RATES	0x15U
#define AX211_CAPABILITY_SHORT_PREAMBLE	0x0020U
#define AX211_CAPABILITY_SHORT_SLOT	0x0400U
#define AX211_REPLY_ERROR_OPCODE		0x02U
#define AX211_REPLY_ERROR_SIZE		20U
#define AX211_UREG_DOORBELL_TO_ISR6	0xa05c04U
#define AX211_UREG_DOORBELL_PNVM		(1U << 20)
#define AX211_PCIE_CAPABILITY		0x10U
#define AX211_PCIE_DEVICE_CONTROL2	0x28U
#define AX211_PCIE_DEVICE_CONTROL2_LTR	(1U << 10)
#define AX211_HBUS_TARG_MEM_RADDR	0x40cU
#define AX211_HBUS_TARG_MEM_RDAT		0x41cU
#define AX211_FW_ADDR_CACHE_CONTROL	0xc0000000U
#define AX211_ERROR_LOG_MIN_ADDRESS	0x00400000U
#define AX211_LMAC_ERROR_WORD_COUNT	30U
#define AX211_UMAC_ERROR_WORD_COUNT	15U

struct ax211_pci_staged_key {
	struct intel_ax211_key_request request;
	uint8_t valid;
	uint8_t programmed;
	uint8_t reserved[6];
};

struct ax211_pci_deferred_event {
	struct intel_ax211_boot_received_event received;
	uint8_t bytes[INTEL_AX211_BOOT_EVENT_CAPACITY];
};

/*
 * These are stable device register encodings, not a copied implementation.
 * HW_REV bits 15:4 contain the MAC type. HW_RF_ID bits 23:12 contain the RF
 * type. The frozen P038 target is an SO-or-SOF MAC with a Garfield Peak RF.
 */
#define AX211_CSR_HW_REV			0x028U
#define AX211_CSR_HW_RF_ID		0x09cU
#define AX211_CSR_HW_REV_TYPE_MASK	0x0000fff0U
#define AX211_CSR_HW_REV_TYPE_SHIFT	4U
#define AX211_CSR_HW_REV_TYPE_SO	0x037U
#define AX211_CSR_HW_REV_TYPE_SOF	0x043U
#define AX211_CSR_HW_RF_TYPE_MASK	0x00fff000U
#define AX211_CSR_HW_RF_TYPE_SHIFT	12U
#define AX211_CSR_HW_RF_TYPE_GF		0x10dU
#define AX211_CSR_HW_RF_CDB		0x10000000U

struct ax211_pci_controller {
	struct mutex lifecycle_lock;
	struct spinlock interrupt_lock;
	struct drv_pci_device *device;
	struct drv_pci_mapping mapping;
	struct drv_pci_enable_state enable_state;
	struct drv_pci_bar original_bar;
	struct intel_ax211_pci_mmio_backend backend;
	struct intel_ax211_mmio mmio;
	struct intel_ax211_transport_backend transport_backend;
	struct intel_ax211_transport transport;
	struct intel_ax211_boot boot;
	struct intel_ax211_runtime_start runtime_start;
	struct intel_ax211_scan_session scan_session;
	struct intel_ax211_bss_cache bss_staging_cache;
	struct intel_ax211_bss_cache bss_published_cache;
	struct intel_ax211_bss_entry selected_bss;
	struct intel_ax211_bss_assoc_metadata selected_metadata;
	struct intel_ax211_assoc_state association;
	struct intel_ax211_key_state keys;
	struct ax211_pci_staged_key staged_pairwise_key;
	struct ax211_pci_staged_key staged_group_key[
	    INTEL_AX211_KEY_INDEX_LIMIT];
	struct ax211_pci_deferred_event deferred_event[
	    AX211_DEFERRED_EVENT_LIMIT];
	uint8_t deferred_dispatch_event[INTEL_AX211_BOOT_EVENT_CAPACITY];
	uint64_t retired_pairwise_key_generation;
	uint64_t retired_group_key_generation[INTEL_AX211_KEY_INDEX_LIMIT];
	uint8_t retired_group_key_remove[INTEL_AX211_KEY_INDEX_LIMIT];
	struct intel_ax211_tx_ring tx_ring;
	struct intel_ax211_tx_queue_config tx_queue_config;
	struct intel_ax211_dma_resources *active_dma;
	struct drv_pci_irq irq;
	void *irq_cookie;
	struct net_device *net_device;
	struct wlan_station *station;
	struct ax211_pci_controller *next;
	uint32_t hardware_revision;
	uint32_t radio_identity;
	uint32_t hardware_epoch;
	uint8_t runtime_event[INTEL_AX211_BOOT_EVENT_CAPACITY];
	uint8_t runtime_frame[INTEL_AX211_RX_MPDU_FRAME_MAX];
	uint8_t last_receive_header[INTEL_AX211_EVENT_HEADER_SIZE];
	size_t last_receive_length;
	uint8_t last_receive_version;
	uint32_t command_fh_causes;
	uint32_t command_hw_causes;
	uint32_t command_raw_fh_causes;
	uint32_t command_raw_hw_causes;
	uint8_t tx_report_completion[INTEL_AX211_TX_RING_SLOT_COUNT];
	uint32_t scan_step_index;
	uint64_t bss_staging_generation;
	uint64_t bss_published_generation;
	uint64_t connection_generation;
	uint64_t recovery_generation;
	uint64_t next_management_cookie;
	uint64_t control_deadline_ticks;
	int recovery_error;
	unsigned refresh_epoch;
	unsigned deferred_event_head;
	unsigned deferred_event_count;
	unsigned deferred_event_draining;
	unsigned operations_active;
	unsigned irq_count;
	unsigned irq_allocated;
	unsigned irq_established;
	unsigned receive_enabled;
	unsigned irq_latched;
	unsigned boot_initialized;
	unsigned runtime_initialized;
	unsigned runtime_active;
	unsigned operation_admission_open;
	unsigned recovery_pending;
	unsigned recovery_running;
	unsigned poll_active;
	unsigned poll_reschedule;
	unsigned scan_initialized;
	unsigned bss_staging_initialized;
	unsigned bss_published_initialized;
	unsigned selected_bss_valid;
	unsigned association_initialized;
	unsigned keys_initialized;
	unsigned tx_ring_allocated;
	unsigned bar_claimed;
	unsigned bar_mapped;
	unsigned bar_may_have_moved;
	unsigned original_bar_valid;
	unsigned state_saved;
	unsigned driver_data_set;
	unsigned listed;
	unsigned refresh_busy;
	unsigned detaching;
	unsigned ready;
	unsigned quarantined;
	unsigned net_live;
	unsigned station_attached;
};

static int ax211_pci_match(struct drv_pci_device *device,
	const struct drv_pci_id *identity);
static int ax211_pci_attach(struct drv_pci_device *device,
	const struct drv_pci_id *identity);
static int ax211_pci_detach(struct drv_pci_device *device, unsigned flags);
static int ax211_pci_identity_matches(const struct drv_pci_device *device);
static int ax211_pci_bar_validate(const struct drv_pci_bar *bar);
static uint32_t ax211_pci_read32(
	const struct ax211_pci_controller *controller, unsigned offset);
static int ax211_pci_hardware_validate(uint32_t hardware_revision,
	uint32_t radio_identity);
static int ax211_pci_profile(struct ax211_pci_controller *controller,
	struct intel_ax211_mmio_profile *profile);
static int ax211_pci_acquire(struct ax211_pci_controller *controller);
static int ax211_pci_restore_bar(struct ax211_pci_controller *controller);
static int ax211_pci_release_resources(
	struct ax211_pci_controller *controller, int result,
	int retain_on_failure);
static int ax211_pci_quarantine(struct ax211_pci_controller *controller,
	int failure);
static uint64_t ax211_pci_lifecycle_deadline(void);
static int ax211_pci_station_pin_locked(
	struct ax211_pci_controller *controller,
	struct wlan_station **station);
static int ax211_pci_operation_enter_locked(
	struct ax211_pci_controller *controller,
	struct wlan_station **station);
static void ax211_pci_operation_leave_locked(
	struct ax211_pci_controller *controller);
static int ax211_pci_operations_join_locked(
	struct ax211_pci_controller *controller, uint64_t deadline);
static int ax211_pci_station_close_wait(struct wlan_station *station,
	uint64_t deadline);
static void ax211_pci_recovery_latch_locked(
	struct ax211_pci_controller *controller, int error);
static int ax211_pci_recovery_run_locked(
	struct ax211_pci_controller *controller);
static void ax211_pci_list_add(struct ax211_pci_controller *controller);
static int ax211_pci_list_remove(struct ax211_pci_controller *controller);
static void ax211_pci_list_forget(struct ax211_pci_controller *controller);
static struct ax211_pci_controller *ax211_pci_list_find_device(
	struct drv_pci_device *device);
static struct ax211_pci_controller *ax211_pci_refresh_claim(
	unsigned epoch);
static void ax211_pci_refresh_release(
	struct ax211_pci_controller *controller);
static int ax211_pci_refresh_one(struct ax211_pci_controller *controller);
static int ax211_pci_publish(struct ax211_pci_controller *controller,
	const uint8_t mac_address[AX211_MAC_ADDRESS_SIZE]);
static void ax211_pci_scan_profile(struct wlan_scan_profile *profile);
static int ax211_pci_partial_discard(
	struct ax211_pci_controller *controller);
static int ax211_pci_graph_detach(
	struct ax211_pci_controller *controller);
static int ax211_pci_session_stop(
	struct ax211_pci_controller *controller);
static int ax211_pci_ltr_enabled(
	struct ax211_pci_controller *controller, int *enabled);
static int ax211_pci_irq(void *argument);
static int ax211_pci_receive_epoch_begin(void *argument,
	uint32_t generation);
static int ax211_pci_transport_bind(void *argument,
	struct intel_ax211_dma_resources *dma,
	struct intel_ax211_mmio *mmio,
	struct intel_ax211_transport *transport, uint32_t generation);
static int ax211_pci_receive_event(void *argument, uint64_t deadline_us,
	uint8_t *bytes, size_t capacity,
	struct intel_ax211_boot_received_event *event);
static int ax211_pci_publish_pnvm(void *argument,
	struct intel_ax211_dma_resources *dma);
static int ax211_pci_post_alive(void *argument,
	const struct intel_ax211_protocol_alive *alive);
static int ax211_pci_interrupt_drain(void *argument);
static int ax211_pci_clock_us(void *argument, uint64_t *time_us);
static int ax211_pci_nic_lock(void *argument);
static int ax211_pci_nic_unlock(void *argument);
static int ax211_pci_tx_event(const struct intel_ax211_event *event);
static uint8_t ax211_pci_notification_version(const uint8_t *bytes,
	size_t length);
static int ax211_pci_scan_initialize(
	struct ax211_pci_controller *controller);
static uint32_t ax211_pci_channel_frequency(uint8_t channel);
static int ax211_pci_runtime_scan_profile(
	const struct ax211_pci_controller *controller,
	struct wlan_scan_profile *profile);
static int ax211_pci_scan_channel_present(
	const struct ax211_pci_controller *controller, uint8_t channel);
static void ax211_pci_scan_clear(
	struct ax211_pci_controller *controller);
static void ax211_pci_bss_staging_discard(
	struct ax211_pci_controller *controller);
static int ax211_pci_bss_staging_publish(
	struct ax211_pci_controller *controller, uint64_t generation);
static int ax211_pci_event_message(const uint8_t *bytes, size_t length,
	const struct intel_ax211_boot_received_event *received,
	struct intel_ax211_event *event,
	struct intel_ax211_protocol_message *message);
static int ax211_pci_scan_command_dispatch(
	struct ax211_pci_controller *controller, const uint8_t *bytes,
	size_t length, uint64_t now);
static int ax211_pci_scan_notification_dispatch(
	struct ax211_pci_controller *controller,
	const struct intel_ax211_protocol_message *message, uint64_t now);
static int ax211_pci_rx_dispatch(
	struct ax211_pci_controller *controller,
	const struct intel_ax211_protocol_message *message);
static void ax211_pci_scan_report_error(
	struct ax211_pci_controller *controller, int result);
static int ax211_pci_scan_result_errno(int result);
static int ax211_pci_runtime_event_dispatch(
	struct ax211_pci_controller *controller, const uint8_t *bytes,
	size_t length, const struct intel_ax211_boot_received_event *event);
static int ax211_pci_deferred_event_enqueue(
	struct ax211_pci_controller *controller, const uint8_t *bytes,
	const struct intel_ax211_boot_received_event *received);
static int ax211_pci_deferred_event_drain_one(
	struct ax211_pci_controller *controller);
static void ax211_pci_deferred_event_clear(
	struct ax211_pci_controller *controller);
static uint32_t ax211_pci_get_le32(const uint8_t bytes[4]);
static void ax211_pci_scrub(void *memory, size_t length);
static int ax211_pci_tx_sync_for_device(void *argument,
	const struct drv_dma_buffer *buffer, size_t offset, size_t length);
static int ax211_pci_tx_write32(void *argument, uint32_t offset,
	uint32_t value);
static uint64_t ax211_pci_assoc_clock_us(void *argument);
static int ax211_pci_assoc_exchange(void *argument,
	const struct intel_ax211_assoc_command *command,
	struct intel_ax211_assoc_reply *reply);
static int ax211_pci_direct_command(
	struct ax211_pci_controller *controller,
	const struct intel_ax211_command_request *request,
	uint64_t deadline_ticks, uint8_t *response, size_t response_capacity,
	size_t *response_length,
	struct intel_ax211_protocol_message *completed_message,
	struct intel_ax211_protocol_pending_command *completed_pending);
static int ax211_pci_command_timeout(
	struct ax211_pci_controller *controller, uint64_t deadline_ticks,
	uint64_t now_us, uint64_t *timeout_us, uint64_t *deadline_us);
static int ax211_pci_sram_read_locked(
	struct ax211_pci_controller *controller, uint32_t address,
	uint32_t *words, size_t count);
static void ax211_pci_firmware_error_dump(
	struct ax211_pci_controller *controller);
static int ax211_pci_assoc_result_errno(int result);
static int ax211_pci_tx_ring_result_errno(int result);
static int ax211_pci_key_result_errno(int result);
static int ax211_pci_assoc_profile(
	struct ax211_pci_controller *controller,
	const struct wlan_bss_record *bss, uint64_t connection_generation,
	struct intel_ax211_assoc_profile *profile);
static int ax211_pci_assoc_rollback(
	struct ax211_pci_controller *controller, uint64_t generation);
static int ax211_pci_mcast_filter_configure(
	struct ax211_pci_controller *controller, uint64_t deadline_ticks);
static int ax211_pci_mac_power_configure(
	struct ax211_pci_controller *controller, uint64_t deadline_ticks);
static int ax211_pci_keys_remove_all(
	struct ax211_pci_controller *controller, uint64_t deadline_ticks);
static void ax211_pci_connection_clear(
	struct ax211_pci_controller *controller);
static int ax211_pci_key_command(
	struct ax211_pci_controller *controller, const uint8_t *payload,
	uint64_t deadline_ticks);
static int ax211_pci_staged_key_store(
	struct ax211_pci_staged_key *staged,
	const struct intel_ax211_key_request *request);
static int ax211_pci_staged_key_program(
	struct ax211_pci_controller *controller,
	struct ax211_pci_staged_key *staged, uint64_t deadline_ticks);
static void ax211_pci_staged_key_clear(
	struct ax211_pci_staged_key *staged);
static int ax211_pci_keys_have_active(
	const struct ax211_pci_controller *controller);
static int ax211_pci_key_request_address_valid(
	const struct ax211_pci_controller *controller,
	const struct wlan_radio_key_request *request);
static int ax211_pci_key_fail_closed(
	struct ax211_pci_controller *controller, int error);
static int ax211_pci_tx_submit(
	struct ax211_pci_controller *controller,
	const struct intel_ax211_tx_request *request, uint64_t deadline_ticks,
	int report_completion);
static int ax211_pci_tx_dispatch(
	struct ax211_pci_controller *controller,
	const struct intel_ax211_protocol_message *message);
static int ax211_pci_tx_timeout_check(
	struct ax211_pci_controller *controller, uint64_t now_us);
static int ax211_pci_connection_rx_dispatch(
	struct ax211_pci_controller *controller,
	const struct intel_ax211_rx_mpdu *mpdu, uint16_t frame_control);

static int ax211_net_open(struct net_device *device);
static void ax211_net_close(struct net_device *device);
static int ax211_net_transmit(struct net_device *device,
	struct packet_buf *packet);
static unsigned ax211_net_poll_receive(struct net_device *device,
	unsigned budget);
static int ax211_net_ioctl(struct net_device *device,
	unsigned long request, void *argument);
static void ax211_net_release(void *driver_data);

static int ax211_radio_scan_channel_start(void *context,
	uint64_t generation, uint32_t step_index, uint32_t channel,
	uint64_t deadline);
static int ax211_radio_scan_stop(void *context, uint64_t generation);
static int ax211_radio_connect_start(void *context, uint64_t generation,
	const struct wlan_bss_record *bss, uint64_t deadline);
static int ax211_radio_disconnect(void *context, uint64_t generation);
static int ax211_radio_management_transmit(void *context,
	uint64_t generation, const uint8_t *frame, size_t length,
	uint64_t deadline);
static int ax211_radio_association_set(void *context,
	uint64_t generation, const uint8_t bssid[6], uint16_t aid,
	uint64_t deadline);
static int ax211_radio_association_clear(void *context,
	uint64_t generation, uint64_t deadline);
static int ax211_radio_frame_transmit(void *context,
	const struct wlan_radio_tx_request *request);
static int ax211_radio_key_install(void *context,
	const struct wlan_radio_key_request *request);
static int ax211_radio_key_delete(void *context, uint64_t generation,
	enum wlan_radio_key_kind kind, uint8_t key_index,
	uint64_t key_generation, uint64_t deadline);
static int ax211_radio_keys_activate(void *context, uint64_t generation,
	uint64_t pairwise_key_generation, uint64_t group_key_generation,
	uint64_t deadline);
static int ax211_radio_quiesce(void *context);

static const struct drv_pci_id ax211_pci_ids[] = {
	{
		.vendor = AX211_PCI_VENDOR,
		.device = AX211_PCI_PRODUCT,
		.subvendor = AX211_PCI_SUBVENDOR,
		.subdevice = AX211_PCI_SUBPRODUCT,
		.class_code = AX211_PCI_CLASS,
		.class_mask = AX211_PCI_CLASS_MASK,
	},
};

static struct drv_pci_driver ax211_pci_driver = {
	.name = "intel-ax211",
	.ids = ax211_pci_ids,
	.id_count = sizeof(ax211_pci_ids) / sizeof(ax211_pci_ids[0]),
	.match = ax211_pci_match,
	.attach = ax211_pci_attach,
	.detach = ax211_pci_detach,
};

/* This table is the permanent common-WLAN boundary for the AX211 backend. */
static const struct wlan_radio_ops ax211_radio_ops = {
	.scan_channel_start = ax211_radio_scan_channel_start,
	.scan_stop = ax211_radio_scan_stop,
	.connect_start = ax211_radio_connect_start,
	.disconnect = ax211_radio_disconnect,
	.management_transmit = ax211_radio_management_transmit,
	.association_set = ax211_radio_association_set,
	.association_clear = ax211_radio_association_clear,
	.frame_transmit = ax211_radio_frame_transmit,
	.key_install = ax211_radio_key_install,
	.key_delete = ax211_radio_key_delete,
	.keys_activate = ax211_radio_keys_activate,
	.quiesce = ax211_radio_quiesce
};

static const struct net_device_ops ax211_net_ops = {
	.open = ax211_net_open,
	.close = ax211_net_close,
	.transmit = ax211_net_transmit,
	.poll_receive = ax211_net_poll_receive,
	.ioctl = ax211_net_ioctl,
	.release = ax211_net_release
};

static const struct intel_ax211_runtime_start_ops ax211_runtime_start_ops = {
	.boot = {
		.receive_epoch_begin = ax211_pci_receive_epoch_begin,
		.transport_bind = ax211_pci_transport_bind,
		.receive_event = ax211_pci_receive_event,
		.publish_pnvm = ax211_pci_publish_pnvm,
		.post_alive = ax211_pci_post_alive,
		.interrupt_drain = ax211_pci_interrupt_drain,
		.clock_us = ax211_pci_clock_us
	},
	.nic_lock = ax211_pci_nic_lock,
	.nic_unlock = ax211_pci_nic_unlock
};

static const struct intel_ax211_tx_ring_ops ax211_tx_ring_ops = {
	.sync_for_device = ax211_pci_tx_sync_for_device,
	.write32 = ax211_pci_tx_write32
};

static const struct intel_ax211_assoc_ops ax211_assoc_ops = {
	.clock_us = ax211_pci_assoc_clock_us,
	.exchange = ax211_pci_assoc_exchange
};

static struct ax211_pci_controller *ax211_controllers;
static struct spinlock ax211_registry_lock;
static unsigned ax211_refresh_epoch;
static unsigned ax211_registry_initialized;

/* Registers the exact Intel AX211 PCI transport driver. */
int
drv_pci_intel_ax211_driver_register(void)
{
	int error;

	if (ax211_registry_initialized)
		return drv_pci_driver_register(&ax211_pci_driver);
	spin_init(&ax211_registry_lock, LOCK_RANK_DEVICE,
	    "Intel AX211 registry");
	ax211_controllers = NULL;
	ax211_refresh_epoch = 0U;
	error = drv_pci_driver_register(&ax211_pci_driver);
	if (error == 0)
		ax211_registry_initialized = 1U;
	return error;
}

/* Publishes persistent controllers after platform interrupt bring-up. */
void
drv_pci_intel_ax211_devices_ready(void)
{
	struct ax211_pci_controller *controller;
	unsigned long enabled;
	unsigned epoch;
	int error;

	enabled = spin_lock_irqsave(&ax211_registry_lock);
	ax211_refresh_epoch++;
	if (ax211_refresh_epoch == 0U)
		ax211_refresh_epoch++;
	epoch = ax211_refresh_epoch;
	spin_unlock_irqrestore(&ax211_registry_lock, enabled);
	for (;;) {
		controller = ax211_pci_refresh_claim(epoch);
		if (controller == NULL)
			return;
		error = ax211_pci_refresh_one(controller);
		ax211_pci_refresh_release(controller);
		if (error != 0)
			hal_printf(
			    "intel-ax211: deferred WLAN publication failed (%d)\n",
			    error);
	}
}

/* Matches only the frozen P038 PCI identity. */
static int
ax211_pci_match(
	struct drv_pci_device *device,
	const struct drv_pci_id *identity)
{
	(void)identity;
	return ax211_pci_identity_matches(device) ? DRV_PCI_MATCH_EXACT :
	    DRV_PCI_MATCH_NONE;
}

/* Retains a validated, DMA-disabled controller without waiting or I/O. */
static int
ax211_pci_attach(
	struct drv_pci_device *device,
	const struct drv_pci_id *identity)
{
	struct ax211_pci_controller *controller;
	struct intel_ax211_mmio_profile profile;
	const char *stage;
	int cleanup_error;
	int error;

	(void)identity;
	if (!ax211_pci_identity_matches(device))
		return ENODEV;
	if (drv_pci_device_driver_data(device) != NULL)
		return EBUSY;
	controller = hal_malloc(sizeof(*controller));
	if (controller == NULL)
		return ENOMEM;
	memset(controller, 0, sizeof(*controller));
	memset(&profile, 0, sizeof(profile));
	controller->device = device;
	stage = "lifecycle-lock";
	error = mutex_init(&controller->lifecycle_lock, LOCK_RANK_DEVICE,
	    "Intel AX211 lifecycle");
	if (error == 0) {
		spin_init(&controller->interrupt_lock, LOCK_RANK_DEVICE,
		    "Intel AX211 interrupt");
		controller->hardware_epoch = 1U;
	}
	if (error == 0) {
		stage = "pci-acquire";
		error = ax211_pci_acquire(controller);
	}
	if (error == 0) {
		stage = "mmio-backend";
		error = intel_ax211_pci_mmio_backend_init(&controller->backend,
		    controller->mapping.address, controller->mapping.size);
	}
	if (error == 0) {
		stage = "mmio-profile";
		error = ax211_pci_profile(controller, &profile);
	}
	if (error == 0) {
		stage = "mmio-init";
		error = intel_ax211_mmio_init(&controller->mmio,
		    intel_ax211_pci_mmio_ops(), &controller->backend, &profile);
	}
	ax211_pci_scrub(&profile, sizeof(profile));
	if (error == 0) {
		stage = "driver-data";
		error = drv_pci_device_set_driver_data(device, controller);
		if (error == 0)
			controller->driver_data_set = 1U;
	}
	if (error == 0) {
		controller->ready = 1U;
		ax211_pci_list_add(controller);
		hal_printf(
		    "intel-ax211: controller retained; WLAN publication deferred\n");
		return 0;
	}
	hal_printf("intel-ax211: attach failed stage=%s error=%d "
	    "hw-rev=%08x rf-id=%08x\n", stage, error,
	    controller->hardware_revision, controller->radio_identity);
	cleanup_error = ax211_pci_release_resources(controller, error, 1);
	if (controller->bar_claimed)
		return ax211_pci_quarantine(controller, cleanup_error);
	if (controller->driver_data_set)
		(void)drv_pci_device_set_driver_data(device, NULL);
	ax211_pci_scrub(controller, sizeof(*controller));
	hal_free(controller);
	return cleanup_error;
}

/* Checked detach retires publication before releasing the persistent BAR. */
static int
ax211_pci_detach(
	struct drv_pci_device *device,
	unsigned flags)
{
	struct ax211_pci_controller *controller;
	struct net_device *net_device;
	unsigned net_live;
	int error;

	(void)flags;
	controller = drv_pci_device_driver_data(device);
	if (controller == NULL)
		controller = ax211_pci_list_find_device(device);
	if (controller == NULL || controller->device != device)
		return ENODEV;
	mutex_lock(&controller->lifecycle_lock);
	error = ax211_pci_list_remove(controller);
	if (error != 0) {
		mutex_unlock(&controller->lifecycle_lock);
		return error;
	}
	controller->ready = 0U;
	controller->operation_admission_open = 0U;
	net_device = controller->net_device;
	net_live = controller->net_live;
	mutex_unlock(&controller->lifecycle_lock);
	if (net_device != NULL)
		(void)net_device_set_carrier(net_device, 0);
	if (net_device != NULL && net_live) {
		error = net_device_gone(net_device);
		if (error != 0)
			return error;
	}
	mutex_lock(&controller->lifecycle_lock);
	if (net_live)
		controller->net_live = 0U;
	error = ax211_pci_graph_detach(controller);
	if (error != 0) {
		mutex_unlock(&controller->lifecycle_lock);
		return error;
	}
	error = ax211_pci_session_stop(controller);
	if (error != 0) {
		controller->quarantined = 1U;
		mutex_unlock(&controller->lifecycle_lock);
		return error;
	}
	error = ax211_pci_release_resources(controller, 0, 1);
	if (error != 0) {
		controller->quarantined = 1U;
		mutex_unlock(&controller->lifecycle_lock);
		return error;
	}
	controller->quarantined = 0U;
	if (controller->driver_data_set)
		error = drv_pci_device_set_driver_data(device, NULL);
	else
		error = 0;
	if (error != 0) {
		mutex_unlock(&controller->lifecycle_lock);
		return error;
	}
	controller->driver_data_set = 0U;
	ax211_pci_list_forget(controller);
	controller->device = NULL;
	controller->net_device = NULL;
	mutex_unlock(&controller->lifecycle_lock);
	if (net_device != NULL)
		net_device_destroy(net_device);
	else {
		ax211_pci_scrub(controller, sizeof(*controller));
		hal_free(controller);
	}
	return 0;
}

/* Checks the complete frozen P038 PCI tuple. */
static int
ax211_pci_identity_matches(
	const struct drv_pci_device *device)
{
	if (device == NULL)
		return 0;
	if (drv_pci_device_vendor(device) != AX211_PCI_VENDOR ||
	    drv_pci_device_product(device) != AX211_PCI_PRODUCT)
		return 0;
	if (drv_pci_device_subvendor(device) != AX211_PCI_SUBVENDOR ||
	    drv_pci_device_subproduct(device) != AX211_PCI_SUBPRODUCT)
		return 0;
	if (drv_pci_device_revision(device) != AX211_PCI_REVISION)
		return 0;
	return (drv_pci_device_class(device) & AX211_PCI_CLASS_MASK) ==
	    AX211_PCI_CLASS;
}

/* Validates BAR0 before PCI decode or MMIO is changed. */
static int
ax211_pci_bar_validate(
	const struct drv_pci_bar *bar)
{
	if (bar == NULL)
		return EINVAL;
	if (bar->type != DRV_PCI_BAR_MEMORY32 &&
	    bar->type != DRV_PCI_BAR_MEMORY64)
		return ENODEV;
	return bar->size >= AX211_BAR0_MINIMUM_SIZE ? 0 : ENODEV;
}

/* Reads one naturally aligned identity CSR from retained BAR0. */
static uint32_t
ax211_pci_read32(
	const struct ax211_pci_controller *controller,
	unsigned offset)
{
	volatile uint8_t *registers;
	uint32_t value;

	registers = controller->mapping.address;
	value = *(volatile uint32_t *)(registers + offset);
	hal_io_rmb();
	return value;
}

/* Validates the frozen SO-or-SOF MAC and Garfield Peak RF identities. */
static int
ax211_pci_hardware_validate(
	uint32_t hardware_revision,
	uint32_t radio_identity)
{
	uint32_t mac_type;
	uint32_t radio_type;

	mac_type = (hardware_revision & AX211_CSR_HW_REV_TYPE_MASK) >>
	    AX211_CSR_HW_REV_TYPE_SHIFT;
	radio_type = (radio_identity & AX211_CSR_HW_RF_TYPE_MASK) >>
	    AX211_CSR_HW_RF_TYPE_SHIFT;
	if (mac_type != AX211_CSR_HW_REV_TYPE_SO &&
	    mac_type != AX211_CSR_HW_REV_TYPE_SOF)
		return ENODEV;
	if (radio_type != AX211_CSR_HW_RF_TYPE_GF)
		return ENODEV;
	return (radio_identity & AX211_CSR_HW_RF_CDB) == 0U ? 0 : ENODEV;
}

/* Derives the already validated private MMIO profile. */
static int
ax211_pci_profile(
	struct ax211_pci_controller *controller,
	struct intel_ax211_mmio_profile *profile)
{
	uint32_t mac_type;

	if (controller == NULL || profile == NULL)
		return EINVAL;
	memset(profile, 0, sizeof(*profile));
	mac_type = (controller->hardware_revision &
	    AX211_CSR_HW_REV_TYPE_MASK) >> AX211_CSR_HW_REV_TYPE_SHIFT;
	profile->mac_type = (uint16_t)mac_type;
	profile->rf_type = INTEL_AX211_MMIO_RF_GF;
	profile->umac_prph_offset = INTEL_AX211_MMIO_UMAC_PRPH_OFFSET;
	return 0;
}

/* Acquires the persistent BAR/decode lease with bus mastering disabled. */
static int
ax211_pci_acquire(
	struct ax211_pci_controller *controller)
{
	struct drv_pci_bar bar;
	uint16_t command;
	int error;

	error = drv_pci_device_claim_bar(controller->device, 0U);
	if (error != 0)
		return error;
	controller->bar_claimed = 1U;
	error = drv_pci_device_bar(controller->device, 0U, &bar);
	if (error != 0)
		return error;
	error = ax211_pci_bar_validate(&bar);
	if (error != 0)
		return error;
	controller->original_bar = bar;
	controller->original_bar_valid = 1U;
	error = drv_pci_device_save_enable_state(controller->device,
	    &controller->enable_state);
	if (error != 0)
		return error;
	controller->state_saved = 1U;
	error = drv_pci_device_set_bus_master(controller->device, false);
	if (error != 0)
		return error;
	controller->bar_may_have_moved = 1U;
	error = drv_pci_device_map_bar(controller->device, 0U,
	    DRV_PCI_MAP_READ | DRV_PCI_MAP_WRITE | DRV_PCI_MAP_NOCACHE,
	    &controller->mapping);
	if (error != 0)
		return error;
	controller->bar_mapped = 1U;
	if (controller->mapping.address == NULL ||
	    (controller->mapping.type != DRV_PCI_BAR_MEMORY32 &&
	    controller->mapping.type != DRV_PCI_BAR_MEMORY64) ||
	    controller->mapping.size < AX211_BAR0_MINIMUM_SIZE ||
	    controller->mapping.size > bar.size)
		return EIO;
	error = drv_pci_device_enable_memory(controller->device);
	if (error == 0)
		error = drv_pci_device_set_bus_master(controller->device, false);
	if (error == 0)
		error = drv_pci_device_config_read16(controller->device,
		    AX211_PCI_COMMAND, &command);
	if (error != 0)
		return error;
	if ((command & AX211_PCI_COMMAND_MEMORY) == 0U ||
	    (command & AX211_PCI_COMMAND_MASTER) != 0U)
		return EIO;
	controller->hardware_revision = ax211_pci_read32(controller,
	    AX211_CSR_HW_REV);
	controller->radio_identity = ax211_pci_read32(controller,
	    AX211_CSR_HW_RF_ID);
	return ax211_pci_hardware_validate(controller->hardware_revision,
	    controller->radio_identity);
}

/* Restores BAR0 if the host mapper assigned a temporary MMIO address. */
static int
ax211_pci_restore_bar(
	struct ax211_pci_controller *controller)
{
	struct drv_pci_bar current_bar;
	int error;

	if (!controller->bar_may_have_moved)
		return 0;
	if (!controller->original_bar_valid)
		return EIO;
	error = drv_pci_device_bar(controller->device, 0U, &current_bar);
	if (error != 0)
		return error;
	if (current_bar.bus_address != controller->original_bar.bus_address) {
		error = drv_pci_device_assign_bar(controller->device, 0U,
		    controller->original_bar.bus_address);
		if (error != 0)
			return error;
	}
	controller->bar_may_have_moved = 0U;
	controller->original_bar_valid = 0U;
	return 0;
}

/* Releases the PCI lease in reverse order, optionally retaining for retry. */
static int
ax211_pci_release_resources(
	struct ax211_pci_controller *controller,
	int result,
	int retain_on_failure)
{
	int bar_error;
	int quiesce_error;
	int restore_error;

	bar_error = 0;
	restore_error = 0;
	if (controller->bar_mapped) {
		drv_pci_device_unmap_bar(controller->device,
		    &controller->mapping);
		controller->bar_mapped = 0U;
	}
	bar_error = ax211_pci_restore_bar(controller);
	if (bar_error == 0 && controller->state_saved) {
		restore_error = drv_pci_device_restore_enable_state(
		    controller->device, &controller->enable_state);
		if (restore_error == 0)
			controller->state_saved = 0U;
	}
	if (bar_error != 0 || restore_error != 0) {
		quiesce_error = drv_pci_device_set_bus_master(
		    controller->device, false);
		if (result == 0 && quiesce_error != 0)
			result = quiesce_error;
		if (retain_on_failure)
			return bar_error != 0 ? bar_error : restore_error;
	}
	if (controller->bar_claimed) {
		drv_pci_device_release_bar(controller->device, 0U);
		controller->bar_claimed = 0U;
	}
	if (bar_error != 0)
		return bar_error;
	if (restore_error != 0)
		return restore_error;
	return result;
}

/* Keeps an incompletely restored controller bound for checked detach retry. */
static int
ax211_pci_quarantine(
	struct ax211_pci_controller *controller,
	int failure)
{
	uint16_t command;
	int error;

	controller->ready = 0U;
	controller->quarantined = 1U;
	error = drv_pci_device_set_bus_master(controller->device, false);
	if (error == 0)
		error = drv_pci_device_config_read16(controller->device,
		    AX211_PCI_COMMAND, &command);
	if (error == 0 && (command & AX211_PCI_COMMAND_MASTER) != 0U)
		error = EIO;
	if (error != 0)
		failure = error;
	if (!controller->driver_data_set) {
		error = drv_pci_device_set_driver_data(controller->device,
		    controller);
		if (error == 0)
			controller->driver_data_set = 1U;
	}
	ax211_pci_list_add(controller);
	hal_printf(
	    "intel-ax211: controller restoration quarantined (%d)\n",
	    failure);
	/* Returning success is intentional: the PCI core must bind detach. */
	return 0;
}

/* Publishes one persistent controller to the refresh registry. */
static void
ax211_pci_list_add(
	struct ax211_pci_controller *controller)
{
	unsigned long enabled;

	enabled = spin_lock_irqsave(&ax211_registry_lock);
	controller->next = ax211_controllers;
	ax211_controllers = controller;
	controller->listed = 1U;
	spin_unlock_irqrestore(&ax211_registry_lock, enabled);
}

/* Retires a controller from admission unless refresh still owns it. */
static int
ax211_pci_list_remove(
	struct ax211_pci_controller *controller)
{
	unsigned long enabled;

	enabled = spin_lock_irqsave(&ax211_registry_lock);
	if (controller->refresh_busy) {
		spin_unlock_irqrestore(&ax211_registry_lock, enabled);
		return EBUSY;
	}
	if (!controller->detaching)
		controller->detaching = 1U;
	spin_unlock_irqrestore(&ax211_registry_lock, enabled);
	return 0;
}

/* Forgets a fully restored controller immediately before final release. */
static void
ax211_pci_list_forget(
	struct ax211_pci_controller *controller)
{
	struct ax211_pci_controller **link;
	unsigned long enabled;

	enabled = spin_lock_irqsave(&ax211_registry_lock);
	for (link = &ax211_controllers; *link != NULL;
	    link = &(*link)->next) {
		if (*link != controller)
			continue;
		*link = controller->next;
		controller->next = NULL;
		controller->listed = 0U;
		break;
	}
	spin_unlock_irqrestore(&ax211_registry_lock, enabled);
}

/* Recovers a quarantined controller whose PCI driver-data write failed. */
static struct ax211_pci_controller *
ax211_pci_list_find_device(
	struct drv_pci_device *device)
{
	struct ax211_pci_controller *controller;
	unsigned long enabled;

	enabled = spin_lock_irqsave(&ax211_registry_lock);
	for (controller = ax211_controllers; controller != NULL;
	    controller = controller->next) {
		if (controller->device == device)
			break;
	}
	spin_unlock_irqrestore(&ax211_registry_lock, enabled);
	return controller;
}

/* Claims one unpublished controller exactly once in this refresh epoch. */
static struct ax211_pci_controller *
ax211_pci_refresh_claim(
	unsigned epoch)
{
	struct ax211_pci_controller *controller;
	unsigned long enabled;

	enabled = spin_lock_irqsave(&ax211_registry_lock);
	for (controller = ax211_controllers; controller != NULL;
	    controller = controller->next) {
		if (controller->detaching || controller->refresh_busy ||
		    !controller->ready || controller->quarantined ||
		    controller->station_attached ||
		    controller->refresh_epoch == epoch)
			continue;
		controller->refresh_epoch = epoch;
		controller->refresh_busy = 1U;
		break;
	}
	spin_unlock_irqrestore(&ax211_registry_lock, enabled);
	return controller;
}

/* Releases the registry lease after MMIO and publication have finished. */
static void
ax211_pci_refresh_release(
	struct ax211_pci_controller *controller)
{
	unsigned long enabled;

	enabled = spin_lock_irqsave(&ax211_registry_lock);
	controller->refresh_busy = 0U;
	spin_unlock_irqrestore(&ax211_registry_lock, enabled);
}

/* Reads identity under NIC ownership, stops again, then publishes once. */
static int
ax211_pci_refresh_one(
	struct ax211_pci_controller *controller)
{
	uint8_t mac_address[AX211_MAC_ADDRESS_SIZE];
	int hardware_attempted;
	int error;
	int stop_error;

	memset(mac_address, 0, sizeof(mac_address));
	mutex_lock(&controller->lifecycle_lock);
	hardware_attempted = 0;
	if (controller->detaching || !controller->ready ||
	    controller->quarantined)
		error = ENODEV;
	else
		error = ax211_pci_partial_discard(controller);
	if (error == 0) {
		hardware_attempted = 1;
		error = intel_ax211_mmio_prepare_card_hw(&controller->mmio);
	}
	if (error == INTEL_AX211_MMIO_OK)
		error = intel_ax211_mmio_sw_reset(&controller->mmio);
	if (error == INTEL_AX211_MMIO_OK)
		error = intel_ax211_mmio_apm_init(&controller->mmio);
	if (error == INTEL_AX211_MMIO_OK)
		error = intel_ax211_mmio_read_mac(&controller->mmio,
		    mac_address);
	stop_error = INTEL_AX211_MMIO_OK;
	if (hardware_attempted)
		stop_error = intel_ax211_mmio_stop(&controller->mmio);
	if (error == INTEL_AX211_MMIO_OK &&
	    stop_error != INTEL_AX211_MMIO_OK)
		error = stop_error;
	if (error == INTEL_AX211_MMIO_OK)
		error = ax211_pci_publish(controller, mac_address);
	ax211_pci_scrub(mac_address, sizeof(mac_address));
	mutex_unlock(&controller->lifecycle_lock);
	return error;
}

/* Transactionally publishes an administratively-down WLAN station graph. */
static int
ax211_pci_publish(
	struct ax211_pci_controller *controller,
	const uint8_t mac_address[AX211_MAC_ADDRESS_SIZE])
{
	struct wlan_scan_profile profile;
	struct net_device *device;
	struct wlan_station *station;
	unsigned index;
	int error;
	int gone_error;

	device = net_device_alloc();
	if (device == NULL)
		return ENOSPC;
	device->flags = NET_DEVICE_BROADCAST | NET_DEVICE_MULTICAST;
	device->mtu = AX211_MTU;
	memcpy(device->hwaddr, mac_address, AX211_MAC_ADDRESS_SIZE);
	device->hwaddr_len = AX211_MAC_ADDRESS_SIZE;
	device->capabilities = NET_DEVICE_CAP_WLAN;
	device->ops = &ax211_net_ops;
	device->driver_data = controller;
	error = ENOSPC;
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
		net_device_destroy(device);
		return error;
	}
	controller->net_device = device;
	controller->net_live = 1U;
	station = NULL;
	error = net_device_set_carrier(device, 0);
	if (error == 0) {
		ax211_pci_scan_profile(&profile);
		error = wlan_station_attach(device, &ax211_radio_ops, controller,
		    &profile, &station);
	} else
		memset(&profile, 0, sizeof(profile));
	ax211_pci_scrub(&profile, sizeof(profile));
	if (error == 0 && station == NULL)
		error = EIO;
	if (error != 0) {
		gone_error = ax211_pci_partial_discard(controller);
		if (gone_error != 0)
			return gone_error;
		return error;
	}
	controller->station = station;
	controller->station_attached = 1U;
	return 0;
}

/* Builds the deliberately passive, fixed 2.4-GHz channel 1--11 profile. */
static void
ax211_pci_scan_profile(
	struct wlan_scan_profile *profile)
{
	unsigned channel;

	memset(profile, 0, sizeof(*profile));
	profile->channel_count = AX211_PASSIVE_CHANNEL_COUNT;
	for (channel = 1U; channel <= profile->channel_count; channel++) {
		profile->channels[channel - 1U].channel = channel;
		profile->channels[channel - 1U].center_frequency_mhz =
		    2407U + channel * 5U;
	}
}

/* Retires an unpublished net-device shell before a publication retry. */
static int
ax211_pci_partial_discard(
	struct ax211_pci_controller *controller)
{
	struct net_device *device;
	unsigned net_live;
	int error;

	if (controller->station_attached || controller->station != NULL)
		return 0;
	device = controller->net_device;
	if (device == NULL)
		return 0;
	net_live = controller->net_live;
	mutex_unlock(&controller->lifecycle_lock);
	(void)net_device_set_carrier(device, 0);
	if (net_live) {
		error = net_device_gone(device);
		mutex_lock(&controller->lifecycle_lock);
		if (error != 0)
			return error;
		controller->net_live = 0U;
	} else
		mutex_lock(&controller->lifecycle_lock);
	device->driver_data = NULL;
	controller->net_device = NULL;
	mutex_unlock(&controller->lifecycle_lock);
	net_device_destroy(device);
	mutex_lock(&controller->lifecycle_lock);
	return 0;
}

static uint64_t
ax211_pci_lifecycle_deadline(void)
{
	uint64_t now;

	now = clock_ticks();
	if (now > UINT64_MAX - AX211_LIFECYCLE_JOIN_TICKS)
		return UINT64_MAX;
	return now + AX211_LIFECYCLE_JOIN_TICKS;
}

/* Pins the common station while the lifecycle lock excludes retirement. */
static int
ax211_pci_station_pin_locked(
	struct ax211_pci_controller *controller,
	struct wlan_station **station)
{
	if (controller == NULL || station == NULL)
		return EINVAL;
	*station = NULL;
	if (!controller->station_attached || controller->station == NULL)
		return ENODEV;
	if (controller->operations_active == UINT_MAX)
		return EOVERFLOW;
	controller->operations_active++;
	*station = controller->station;
	return 0;
}

/* Pins the common station across a deliberate lifecycle-lock drop. */
static int
ax211_pci_operation_enter_locked(
	struct ax211_pci_controller *controller,
	struct wlan_station **station)
{
	if (controller == NULL || station == NULL)
		return EINVAL;
	*station = NULL;
	if (!controller->operation_admission_open ||
	    !controller->station_attached || controller->station == NULL)
		return ENODEV;
	return ax211_pci_station_pin_locked(controller, station);
}

static void
ax211_pci_operation_leave_locked(
	struct ax211_pci_controller *controller)
{
	if (controller == NULL || controller->operations_active == 0U)
		__builtin_trap();
	controller->operations_active--;
}

/* Joins callers that already copied the station pointer before retirement. */
static int
ax211_pci_operations_join_locked(
	struct ax211_pci_controller *controller,
	uint64_t deadline)
{
	if (controller == NULL || deadline == 0U)
		return EINVAL;
	while (controller->operations_active != 0U) {
		if (clock_ticks() >= deadline)
			return ETIMEDOUT;
		mutex_unlock(&controller->lifecycle_lock);
		sched_yield();
		mutex_lock(&controller->lifecycle_lock);
	}
	return 0;
}

/* Common close is a checked admission join and may need a bounded retry. */
static int
ax211_pci_station_close_wait(
	struct wlan_station *station,
	uint64_t deadline)
{
	int result;

	if (station == NULL || deadline == 0U)
		return ENODEV;
	for (;;) {
		result = wlan_station_close(station);
		if (result != EBUSY)
			return result;
		if (clock_ticks() >= deadline)
			return ETIMEDOUT;
		sched_yield();
	}
}

/* Records one uncertain live-runtime failure for the poll recovery owner. */
static void
ax211_pci_recovery_latch_locked(
	struct ax211_pci_controller *controller,
	int error)
{
	if (controller == NULL)
		return;
	if (!controller->recovery_pending && !controller->recovery_running) {
		controller->recovery_error = error > 0 ? error : EIO;
		controller->recovery_generation =
		    controller->connection_generation;
		controller->recovery_pending = 1U;
		hal_printf("intel-ax211: recovery latched error=%d generation=%u\n",
		    controller->recovery_error,
		    (unsigned)controller->recovery_generation);
	}
	controller->operation_admission_open = 0U;
	if (controller->net_device != NULL)
		net_device_schedule_poll(controller->net_device);
}

/* Retires common link truth before globally stopping an uncertain epoch. */
static int
ax211_pci_recovery_run_locked(
	struct ax211_pci_controller *controller)
{
	struct net_device *device;
	struct wlan_station *station;
	uint64_t deadline;
	uint64_t generation;
	int carrier_error;
	int join_error;
	int link_error;
	int pin_error;
	int stop_error;
	int failure;

	if (controller == NULL)
		return EINVAL;
	failure = controller->recovery_error > 0 ?
	    controller->recovery_error : EIO;
	generation = controller->recovery_generation != 0U ?
	    controller->recovery_generation : controller->connection_generation;
	controller->operation_admission_open = 0U;
	deadline = ax211_pci_lifecycle_deadline();
	join_error = ax211_pci_operations_join_locked(controller, deadline);
	device = controller->net_device;
	if (join_error != 0) {
		/* The active lease may still be inside common->radio code.  Keep
		 * every DMA owner intact and retry from a later poll rather than
		 * racing a reset or release against that caller. */
		mutex_unlock(&controller->lifecycle_lock);
		carrier_error = device != NULL ?
		    net_device_set_carrier(device, 0) : 0;
		mutex_lock(&controller->lifecycle_lock);
		if (carrier_error != 0 && carrier_error != ENODEV)
			hal_printf(
			    "intel-ax211: recovery carrier-down failed (%d)\n",
			    carrier_error);
		hal_printf("intel-ax211: recovery operation join failed (%d)\n",
		    join_error);
		return join_error;
	}
	controller->recovery_pending = 0U;
	controller->recovery_error = 0;
	controller->recovery_generation = 0U;
	controller->recovery_running = 1U;
	station = NULL;
	pin_error = generation != 0U ?
	    ax211_pci_station_pin_locked(controller, &station) : 0;
	mutex_unlock(&controller->lifecycle_lock);
	carrier_error = device != NULL ? net_device_set_carrier(device, 0) : 0;
	link_error = station != NULL ? wlan_station_report_link_loss(station,
	    generation, failure) : 0;
	mutex_lock(&controller->lifecycle_lock);
	if (station != NULL)
		ax211_pci_operation_leave_locked(controller);
	stop_error = ax211_pci_session_stop(controller);
	controller->recovery_running = 0U;
	controller->quarantined = 1U;
	if (pin_error != 0 && pin_error != ENODEV)
		hal_printf("intel-ax211: recovery station pin failed (%d)\n",
		    pin_error);
	if (carrier_error != 0 && carrier_error != ENODEV)
		hal_printf("intel-ax211: recovery carrier-down failed (%d)\n",
		    carrier_error);
	if (link_error != 0 && link_error != ESTALE && link_error != ENODEV &&
	    link_error != ENOTCONN && link_error != EALREADY)
		hal_printf("intel-ax211: recovery link retirement failed (%d)\n",
		    link_error);
	if (stop_error != 0)
		hal_printf("intel-ax211: fatal poll cleanup failed (%d)\n",
		    stop_error);
	return stop_error != 0 ? stop_error : failure;
}

/* Retires the visible network graph through every checked common barrier. */
static int
ax211_pci_graph_detach(
	struct ax211_pci_controller *controller)
{
	struct wlan_station *station;
	uint64_t deadline;
	int error;

	controller->operation_admission_open = 0U;
	deadline = ax211_pci_lifecycle_deadline();
	error = ax211_pci_operations_join_locked(controller, deadline);
	if (error != 0)
		return error;
	station = controller->station;
	if (station == NULL || !controller->station_attached)
		return 0;
	/* Both joins may synchronously invoke radio callbacks.  Admission is
	 * already closed; drop the hardware lifecycle lock across those edges. */
	mutex_unlock(&controller->lifecycle_lock);
	error = ax211_pci_station_close_wait(station, deadline);
	if (error == 0 || error == ENODEV)
		error = wlan_station_detach(station);
	mutex_lock(&controller->lifecycle_lock);
	if (error != 0 && error != ENODEV)
		return error;
	if (controller->station == station) {
		controller->station = NULL;
		controller->station_attached = 0U;
	}
	return 0;
}

/* Stops every open-generation owner while the lifecycle mutex is held. */
static int
ax211_pci_session_stop(
	struct ax211_pci_controller *controller)
{
	int association_result;
	int coordinator_result;
	int hardware_quiesced;
	int release_result;
	int result;
	unsigned long enabled;

	if (controller == NULL)
		return EINVAL;
	if (controller->runtime_active && controller->connection_generation != 0U)
		hal_printf("intel-ax211: stopping connection generation=%u "
		    "association-phase=%u step=%u\n",
		    (unsigned)controller->connection_generation,
		    controller->association.phase, controller->association.step);
	controller->operation_admission_open = 0U;
	controller->recovery_pending = 0U;
	controller->recovery_error = 0;
	controller->recovery_generation = 0U;
	association_result = 0;
	hardware_quiesced = 0;
	if (controller->association_initialized && controller->runtime_active) {
		controller->control_deadline_ticks =
		    ax211_pci_lifecycle_deadline();
		association_result = ax211_pci_assoc_rollback(controller,
		    controller->connection_generation);
	}
	enabled = spin_lock_irqsave(&controller->interrupt_lock);
	controller->runtime_active = 0U;
	controller->poll_reschedule = 0U;
	spin_unlock_irqrestore(&controller->interrupt_lock, enabled);
	ax211_pci_deferred_event_clear(controller);
	ax211_pci_scan_clear(controller);
	result = 0;
	if (controller->runtime_initialized) {
		coordinator_result = INTEL_AX211_RUNTIME_START_OK;
		if (controller->runtime_start.state ==
		    INTEL_AX211_RUNTIME_START_STATE_RUNNING)
			coordinator_result = intel_ax211_runtime_start_stop(
			    &controller->runtime_start);
		else if (controller->runtime_start.state ==
		    INTEL_AX211_RUNTIME_START_STATE_STOP_REQUIRED ||
		    controller->runtime_start.state ==
		    INTEL_AX211_RUNTIME_START_STATE_STOP_REQUIRED_NO_DMA)
			coordinator_result = intel_ax211_runtime_start_cleanup(
			    &controller->runtime_start);
		if (coordinator_result != INTEL_AX211_RUNTIME_START_OK)
			result = EIO;
		else {
			controller->runtime_initialized = 0U;
			hardware_quiesced = 1;
		}
	}
	if (result == 0 && controller->boot_initialized &&
	    (controller->boot.state == INTEL_AX211_BOOT_STATE_STOP_REQUIRED ||
	    controller->boot.state ==
	    INTEL_AX211_BOOT_STATE_STOP_REQUIRED_NO_DMA)) {
		coordinator_result = intel_ax211_boot_cleanup(&controller->boot);
		if (coordinator_result != INTEL_AX211_BOOT_OK)
			result = EIO;
	}
	if (result == 0 && (controller->irq_established ||
	    controller->irq_allocated)) {
		result = ax211_pci_interrupt_drain(controller);
		if (result == 0)
			hardware_quiesced = 1;
	}
	/* A successful runtime stop includes interrupt drain, controller reset,
	 * and PCI bus-master disable, which is the global queue-1 DMA barrier. */
	if (result == 0 && controller->tx_ring_allocated) {
		release_result = intel_ax211_tx_ring_release(&controller->tx_ring,
		    hardware_quiesced);
		if (release_result != INTEL_AX211_TX_RING_OK)
			result = EIO;
		else
			controller->tx_ring_allocated = 0U;
	}
	if (result == 0) {
		ax211_pci_connection_clear(controller);
		/* A global reset is stronger than a failed per-resource rollback. */
		association_result = 0;
	}
	if (result == 0 && controller->boot_initialized)
		controller->boot_initialized = 0U;
	if (result == 0 && association_result != 0)
		result = association_result;
	return result;
}

/* Reads the PCIe LTR-enable policy without broadening the device match. */
static int
ax211_pci_ltr_enabled(
	struct ax211_pci_controller *controller,
	int *enabled)
{
	uint16_t control;
	unsigned capability;
	int error;

	if (controller == NULL || enabled == NULL)
		return EINVAL;
	*enabled = 0;
	capability = 0U;
	error = drv_pci_device_find_capability(controller->device,
	    AX211_PCIE_CAPABILITY, &capability);
	if (error == ENOENT)
		return 0;
	if (error != 0 || capability > UINT32_MAX -
	    AX211_PCIE_DEVICE_CONTROL2)
		return error != 0 ? error : EIO;
	error = drv_pci_device_config_read16(controller->device,
	    capability + AX211_PCIE_DEVICE_CONTROL2, &control);
	if (error == 0)
		*enabled = (control & AX211_PCIE_DEVICE_CONTROL2_LTR) != 0U;
	return error;
}

/* The exclusive MSI-X handler only latches work for a safe context. */
static int
ax211_pci_irq(
	void *argument)
{
	struct ax211_pci_controller *controller;
	struct net_device *device;
	unsigned long enabled;
	int schedule;

	controller = argument;
	if (controller == NULL)
		return 0;
	enabled = spin_lock_irqsave(&controller->interrupt_lock);
	controller->irq_latched = 1U;
	device = controller->net_device;
	schedule = controller->receive_enabled &&
	    controller->runtime_active && device != NULL;
	spin_unlock_irqrestore(&controller->interrupt_lock, enabled);
	if (schedule)
		net_device_schedule_poll(device);
	return 1;
}

/* Flushes the software latch before admitting one new nonzero epoch. */
static int
ax211_pci_receive_epoch_begin(
	void *argument,
	uint32_t generation)
{
	struct ax211_pci_controller *controller;
	unsigned long enabled;
	int result;

	controller = argument;
	if (controller == NULL || generation == 0U)
		return -1;
	enabled = spin_lock_irqsave(&controller->interrupt_lock);
	controller->receive_enabled = 0U;
	controller->irq_latched = 0U;
	controller->active_dma = NULL;
	memset(controller->last_receive_header, 0,
	    sizeof(controller->last_receive_header));
	controller->last_receive_length = 0U;
	controller->last_receive_version = 0U;
	result = controller->irq_allocated || controller->irq_established ?
	    -1 : 0;
	if (result == 0) {
		controller->hardware_epoch = generation;
		controller->receive_enabled = 1U;
	}
	spin_unlock_irqrestore(&controller->interrupt_lock, enabled);
	return result;
}

/* Binds coherent rings and one exact MSI-X vector transactionally. */
static int
ax211_pci_transport_bind(
	void *argument,
	struct intel_ax211_dma_resources *dma,
	struct intel_ax211_mmio *mmio,
	struct intel_ax211_transport *transport,
	uint32_t generation)
{
	struct ax211_pci_controller *controller;
	struct intel_ax211_transport_ring_memory memory;
	unsigned count;
	unsigned long enabled;
	int bus_master_enabled;
	int result;

	controller = argument;
	if (controller == NULL || dma == NULL || mmio != &controller->mmio ||
	    transport != &controller->transport || generation == 0U ||
	    dma->device == NULL || dma->device !=
	    drv_pci_device_dma(controller->device))
		return -1;
	enabled = spin_lock_irqsave(&controller->interrupt_lock);
	result = !controller->receive_enabled ||
	    controller->hardware_epoch != generation ||
	    controller->irq_allocated || controller->irq_established ? -1 : 0;
	spin_unlock_irqrestore(&controller->interrupt_lock, enabled);
	if (result != 0 || !drv_dma_device_is_coherent(dma->device))
		return -1;

	memset(&memory, 0, sizeof(memory));
	bus_master_enabled = 0;
	result = drv_pci_device_set_bus_master(controller->device, true);
	if (result == 0) {
		bus_master_enabled = 1;
		result = intel_ax211_transport_backend_init(
		    &controller->transport_backend, mmio, &controller->backend,
		    dma);
	}
	if (result == INTEL_AX211_TRANSPORT_BACKEND_OK)
		result = intel_ax211_transport_backend_ring_memory(
		    &controller->transport_backend, &memory);
	if (result == INTEL_AX211_TRANSPORT_BACKEND_OK)
		result = intel_ax211_transport_init(transport,
		    intel_ax211_transport_backend_ops(),
		    &controller->transport_backend, &mmio->profile, &memory);
	if (result == INTEL_AX211_TRANSPORT_OK) {
		count = 0U;
		result = drv_pci_device_allocate_irqs(controller->device,
		    DRV_PCI_IRQ_ALLOW_MSIX, 1U, 1U, &controller->irq, &count);
		if (count != 0U) {
			controller->irq_count = count;
			controller->irq_allocated = 1U;
		}
		if (result == 0 && (count != 1U ||
		    controller->irq.type != DRV_PCI_IRQ_MSIX))
			result = EIO;
	}
	if (result == 0) {
		result = drv_pci_device_establish_irq(controller->device,
		    &controller->irq, ax211_pci_irq, controller, "intel-ax211",
		    &controller->irq_cookie);
		if (result == 0)
			controller->irq_established = 1U;
	}
	if (result == 0) {
		enabled = spin_lock_irqsave(&controller->interrupt_lock);
		controller->active_dma = dma;
		spin_unlock_irqrestore(&controller->interrupt_lock, enabled);
		return 0;
	}
	if (controller->irq_established) {
		if (drv_pci_device_disestablish_irq_checked(controller->device,
		    controller->irq_cookie) == 0) {
			controller->irq_cookie = NULL;
			controller->irq_established = 0U;
		}
	}
	if (!controller->irq_established && controller->irq_allocated) {
		drv_pci_device_free_irqs(controller->device, &controller->irq,
		    controller->irq_count);
		controller->irq_count = 0U;
		controller->irq_allocated = 0U;
	}
	if (bus_master_enabled && drv_pci_device_set_bus_master(
	    controller->device, false) != 0)
		result = EIO;
	enabled = spin_lock_irqsave(&controller->interrupt_lock);
	controller->receive_enabled = 0U;
	controller->irq_latched = 0U;
	controller->active_dma = NULL;
	spin_unlock_irqrestore(&controller->interrupt_lock, enabled);
	return result == 0 ? -1 : result;
}

/* Copies one exact event before returning its DMA buffer to firmware. */
static int
ax211_pci_receive_event(
	void *argument,
	uint64_t deadline_us,
	uint8_t *bytes,
	size_t capacity,
	struct intel_ax211_boot_received_event *event)
{
	struct ax211_pci_controller *controller;
	struct intel_ax211_transport_causes causes;
	struct intel_ax211_transport_rx_completion completion;
	struct drv_dma_buffer *buffer;
	uint64_t now;
	uint64_t remaining;
	uint32_t frame_length;
	uint32_t generation;
	size_t total_length;
	unsigned long enabled;
	int delay_result;
	int receive_result;
	int replenish_result;
	int rearm_result;
	int result;

	controller = argument;
	if (controller == NULL || bytes == NULL || capacity == 0U ||
	    event == NULL)
		return INTEL_AX211_BOOT_RECEIVE_IO;
	memset(event, 0, sizeof(*event));
	receive_result = INTEL_AX211_BOOT_RECEIVE_IO;
	for (;;) {
		enabled = spin_lock_irqsave(&controller->interrupt_lock);
		generation = controller->hardware_epoch;
		result = controller->receive_enabled && generation != 0U &&
		    controller->active_dma != NULL ? 0 : -1;
		controller->irq_latched = 0U;
		spin_unlock_irqrestore(&controller->interrupt_lock, enabled);
		if (result != 0)
			return INTEL_AX211_BOOT_RECEIVE_IO;
		memset(&causes, 0, sizeof(causes));
		result = intel_ax211_transport_interrupt_claim(
		    &controller->transport, &causes);
		if (result != INTEL_AX211_TRANSPORT_OK)
			return INTEL_AX211_BOOT_RECEIVE_IO;
		controller->command_fh_causes |= causes.flow_handler;
		controller->command_hw_causes |= causes.hardware;
		controller->command_raw_fh_causes |= causes.raw_flow_handler;
		controller->command_raw_hw_causes |= causes.raw_hardware;
		if ((causes.flow_handler &
		    INTEL_AX211_TRANSPORT_FH_CAUSE_ERROR) != 0U ||
		    (causes.hardware &
		    INTEL_AX211_TRANSPORT_HW_FATAL_CAUSES) != 0U) {
			hal_printf("intel-ax211: fatal firmware interrupt "
			    "fh=%08x hw=%08x raw-fh=%08x raw-hw=%08x\n",
			    causes.flow_handler, causes.hardware,
			    causes.raw_flow_handler, causes.raw_hardware);
			return INTEL_AX211_BOOT_RECEIVE_IO;
		}
		/*
		 * AX210-generation firmware configures RFH before raising the
		 * hardware-ALIVE cause.  Only then may the host publish its first
		 * receive credit; an earlier doorbell can be lost across that setup.
		 */
		if (!controller->transport.rx_active &&
		    (causes.hardware &
		    INTEL_AX211_TRANSPORT_HW_CAUSE_ALIVE) != 0U) {
			result = intel_ax211_transport_activate_rx(
			    &controller->transport);
			if (result != INTEL_AX211_TRANSPORT_OK)
				return INTEL_AX211_BOOT_RECEIVE_IO;
		}
		memset(&completion, 0, sizeof(completion));
		result = controller->transport.rx_active ?
		    intel_ax211_transport_rx_next(&controller->transport,
		    &completion) : INTEL_AX211_TRANSPORT_STALE;
		if (result == INTEL_AX211_TRANSPORT_OK) {
			if (completion.buffer_id >=
			    controller->active_dma->rx_buffer_count)
				return INTEL_AX211_BOOT_RECEIVE_IO;
			buffer = &controller->active_dma->rx_buffer[
			    completion.buffer_id];
			hal_io_rmb();
			frame_length = buffer->address == NULL || buffer->size < 4U ?
			    0U : ax211_pci_get_le32(buffer->address) & 0x3fffU;
			total_length = (size_t)frame_length + 4U;
			if (frame_length >= 4U &&
			    total_length <= INTEL_AX211_BOOT_EVENT_CAPACITY &&
			    total_length <= buffer->size && total_length <= capacity) {
				memcpy(bytes, buffer->address, total_length);
				receive_result = INTEL_AX211_BOOT_RECEIVE_OK;
			} else
				receive_result = INTEL_AX211_BOOT_RECEIVE_IO;
			replenish_result = intel_ax211_transport_rx_replenish(
			    &controller->transport, buffer->device_address);
			rearm_result = intel_ax211_transport_interrupt_rearm(
			    &controller->transport);
			if (replenish_result != INTEL_AX211_TRANSPORT_OK ||
			    rearm_result != INTEL_AX211_TRANSPORT_OK)
				return INTEL_AX211_BOOT_RECEIVE_IO;
			if (receive_result != INTEL_AX211_BOOT_RECEIVE_OK)
				return receive_result;
			event->length = total_length;
			event->generation = generation;
			event->notification_version =
			    ax211_pci_notification_version(bytes, total_length);
			memcpy(controller->last_receive_header, bytes,
			    sizeof(controller->last_receive_header));
			controller->last_receive_length = total_length;
			controller->last_receive_version =
			    event->notification_version;
			return INTEL_AX211_BOOT_RECEIVE_OK;
		}
		rearm_result = intel_ax211_transport_interrupt_rearm(
		    &controller->transport);
		if (result != INTEL_AX211_TRANSPORT_STALE ||
		    rearm_result != INTEL_AX211_TRANSPORT_OK)
			return INTEL_AX211_BOOT_RECEIVE_IO;
		if (ax211_pci_clock_us(controller, &now) != 0)
			return INTEL_AX211_BOOT_RECEIVE_IO;
		if (now >= deadline_us)
			return INTEL_AX211_BOOT_RECEIVE_TIMEOUT;
		remaining = deadline_us - now;
		delay_result = controller->mmio.ops->delay_us(
		    controller->mmio.argument,
		    (uint32_t)(remaining < AX211_RECEIVE_POLL_DELAY_US ?
		    remaining : AX211_RECEIVE_POLL_DELAY_US));
		if (delay_result != 0)
			return INTEL_AX211_BOOT_RECEIVE_IO;
	}
}

/* Publishes the prepared fragmented PNVM table under NIC ownership. */
static int
ax211_pci_publish_pnvm(
	void *argument,
	struct intel_ax211_dma_resources *dma)
{
	struct ax211_pci_controller *controller;
	int result;
	int unlock_result;

	controller = argument;
	if (controller == NULL || dma == NULL || dma != controller->active_dma ||
	    !dma->pnvm_prepared || dma->pnvm_table.address == NULL ||
	    dma->pnvm_table.device_address == 0U || dma->pnvm_count == 0U)
		return -1;
	hal_io_wmb();
	result = intel_ax211_mmio_nic_lock(&controller->mmio);
	if (result != INTEL_AX211_MMIO_OK)
		return -1;
	result = intel_ax211_mmio_prph_write32(&controller->mmio,
	    controller->mmio.profile.umac_prph_offset +
	    AX211_UREG_DOORBELL_TO_ISR6, AX211_UREG_DOORBELL_PNVM);
	unlock_result = intel_ax211_mmio_nic_unlock(&controller->mmio);
	return result == INTEL_AX211_MMIO_OK &&
	    unlock_result == INTEL_AX211_MMIO_OK ? 0 : -1;
}

/* The MSI-X path needs no ICT reset, but ownership must still be balanced. */
static int
ax211_pci_post_alive(
	void *argument,
	const struct intel_ax211_protocol_alive *alive)
{
	struct ax211_pci_controller *controller;
	int result;
	int unlock_result;

	controller = argument;
	if (controller == NULL || alive == NULL || alive->status !=
	    INTEL_AX211_PROTOCOL_ALIVE_STATUS_OK)
		return -1;
	result = intel_ax211_mmio_nic_lock(&controller->mmio);
	if (result != INTEL_AX211_MMIO_OK)
		return -1;
	unlock_result = intel_ax211_mmio_nic_unlock(&controller->mmio);
	return unlock_result == INTEL_AX211_MMIO_OK ? 0 : -1;
}

/* Masks, disestablishes, and retires the sole MSI-X lifetime in order. */
static int
ax211_pci_interrupt_drain(
	void *argument)
{
	struct ax211_pci_controller *controller;
	unsigned long enabled;
	int disable_result;
	int result;

	controller = argument;
	if (controller == NULL)
		return -1;
	enabled = spin_lock_irqsave(&controller->interrupt_lock);
	controller->receive_enabled = 0U;
	controller->irq_latched = 0U;
	spin_unlock_irqrestore(&controller->interrupt_lock, enabled);
	disable_result = INTEL_AX211_TRANSPORT_OK;
	if (controller->transport.msix_configured &&
	    (controller->irq_established || controller->irq_allocated))
		disable_result = intel_ax211_transport_disable_interrupts(
		    &controller->transport);
	result = 0;
	if (controller->irq_established) {
		result = drv_pci_device_disestablish_irq_checked(
		    controller->device, controller->irq_cookie);
		if (result == 0) {
			controller->irq_cookie = NULL;
			controller->irq_established = 0U;
		}
	}
	if (result == 0 && controller->irq_allocated) {
		drv_pci_device_free_irqs(controller->device, &controller->irq,
		    controller->irq_count);
		controller->irq_count = 0U;
		controller->irq_allocated = 0U;
	}
	if (result == 0 && drv_pci_device_set_bus_master(controller->device,
	    false) != 0)
		result = EIO;
	if (result == 0) {
		enabled = spin_lock_irqsave(&controller->interrupt_lock);
		controller->active_dma = NULL;
		spin_unlock_irqrestore(&controller->interrupt_lock, enabled);
	}
	if (result != 0)
		return -1;
	return disable_result == INTEL_AX211_TRANSPORT_OK ? 0 : -1;
}

static int
ax211_pci_clock_us(
	void *argument,
	uint64_t *time_us)
{
	struct ax211_pci_controller *controller;

	controller = argument;
	if (controller == NULL || time_us == NULL || controller->mmio.ops == NULL ||
	    controller->mmio.ops->clock_us == NULL)
		return -1;
	return controller->mmio.ops->clock_us(controller->mmio.argument,
	    time_us);
}

static int
ax211_pci_nic_lock(
	void *argument)
{
	struct ax211_pci_controller *controller;

	controller = argument;
	return controller != NULL && intel_ax211_mmio_nic_lock(
	    &controller->mmio) == INTEL_AX211_MMIO_OK ? 0 : -1;
}

static int
ax211_pci_nic_unlock(
	void *argument)
{
	struct ax211_pci_controller *controller;

	controller = argument;
	return controller != NULL && intel_ax211_mmio_nic_unlock(
	    &controller->mmio) == INTEL_AX211_MMIO_OK ? 0 : -1;
}

/* TX_CMD uses the narrow data-queue header, whose wire group is legacy. */
static int
ax211_pci_tx_event(
	const struct intel_ax211_event *event)
{
	uint8_t group;

	if (event == NULL || event->command.opcode != INTEL_AX211_TX_OPCODE)
		return 0;
	group = event->flags &
	    (uint8_t)~INTEL_AX211_PROTOCOL_COMMAND_FAILED_MASK;
	return group == INTEL_AX211_PROTOCOL_GROUP_LEGACY ||
	    group == INTEL_AX211_TX_GROUP;
}

/* Pins layouts absent from the RX wire header at the PCI boundary. */
static uint8_t
ax211_pci_notification_version(
	const uint8_t *bytes,
	size_t length)
{
	uint8_t group;
	uint8_t opcode;
	uint8_t queue;

	if (bytes == NULL || length < INTEL_AX211_EVENT_HEADER_SIZE)
		return 0U;
	opcode = bytes[4U];
	group = bytes[5U] &
	    (uint8_t)~INTEL_AX211_PROTOCOL_COMMAND_FAILED_MASK;
	queue = bytes[7U];
	if ((group == INTEL_AX211_PROTOCOL_GROUP_LEGACY ||
	    group == INTEL_AX211_TX_GROUP) && opcode == INTEL_AX211_TX_OPCODE)
		return INTEL_AX211_TX_NOTIFICATION_VERSION;
	if ((queue & 0x80U) == 0U)
		return 0U;
	if (group == INTEL_AX211_PROTOCOL_GROUP_LEGACY &&
	    opcode == INTEL_AX211_PROTOCOL_ALIVE_OPCODE)
		return INTEL_AX211_PROTOCOL_ALIVE_VERSION;
	if (group == INTEL_AX211_PROTOCOL_GROUP_REGULATORY_NVM &&
	    opcode == INTEL_AX211_PROTOCOL_PNVM_INIT_COMPLETE_OPCODE)
		return INTEL_AX211_PROTOCOL_PNVM_INIT_COMPLETE_VERSION;
	if (group == INTEL_AX211_PROTOCOL_GROUP_LEGACY &&
	    opcode == INTEL_AX211_PROTOCOL_INIT_COMPLETE_OPCODE)
		return INTEL_AX211_PROTOCOL_UNKNOWN_VERSION;
	if (group == 0U && opcode == 0xc1U)
		return 5U;
	if (group == INTEL_AX211_ASSOC_GROUP_MAC_CONFIG && opcode ==
	    INTEL_AX211_ASSOC_SESSION_NOTIFICATION_OPCODE)
		return INTEL_AX211_ASSOC_SESSION_NOTIFICATION_LAYOUT_VERSION;
	if (group == 1U && opcode == 0xc8U)
		return 6U;
	if (group == 0U && (opcode == 0x0fU || opcode == 0xb5U))
		return 1U;
	return INTEL_AX211_PROTOCOL_UNKNOWN_VERSION;
}

/* Binds the live command owner to one firmware epoch and copied radio data. */
static int
ax211_pci_scan_initialize(
	struct ax211_pci_controller *controller)
{
	struct intel_ax211_protocol_command_table table;
	struct intel_ax211_runtime_mcc mcc;
	struct wlan_scan_profile profile;
	int result;

	if (controller == NULL || !controller->runtime_initialized ||
	    controller->runtime_start.state !=
	    INTEL_AX211_RUNTIME_START_STATE_RUNNING ||
	    controller->hardware_epoch == 0U || controller->net_device == NULL)
		return EINVAL;
	memset(&table, 0, sizeof(table));
	memset(&mcc, 0, sizeof(mcc));
	table.bytes = controller->runtime_start.command_version_bytes;
	table.count = INTEL_AX211_PROTOCOL_API89_COMMAND_COUNT;
	result = intel_ax211_runtime_start_mcc(&controller->runtime_start, &mcc);
	if (result == INTEL_AX211_RUNTIME_START_OK)
		result = intel_ax211_rx_api89_validate(&table) ==
		    INTEL_AX211_RX_OK ? INTEL_AX211_SCAN_SESSION_OK :
		    INTEL_AX211_SCAN_SESSION_UNSUPPORTED;
	else
		result = INTEL_AX211_SCAN_SESSION_FAILED;
	if (result == INTEL_AX211_SCAN_SESSION_OK)
		result = intel_ax211_scan_session_init(&controller->scan_session,
		    &controller->runtime_start.commands, &table,
		    &controller->runtime_start.nvm, &mcc,
		    controller->net_device->hwaddr, controller->hardware_epoch);
	ax211_pci_scrub(&mcc, sizeof(mcc));
	if (result != INTEL_AX211_SCAN_SESSION_OK)
		return ax211_pci_scan_result_errno(result);
	memset(&profile, 0, sizeof(profile));
	result = ax211_pci_runtime_scan_profile(controller, &profile);
	if (result == 0)
		result = controller->station == NULL ? ENODEV :
		    wlan_station_scan_profile_update(controller->station, &profile);
	ax211_pci_scrub(&profile, sizeof(profile));
	if (result != 0)
		return result;
	controller->scan_step_index = 0U;
	controller->scan_initialized = 1U;
	return 0;
}

static uint32_t
ax211_pci_channel_frequency(uint8_t channel)
{
	if (channel >= 1U && channel <= 13U)
		return 2407U + 5U * (uint32_t)channel;
	if (channel == 14U)
		return 2484U;
	if ((channel >= 36U && channel <= 144U &&
	    (channel - 36U) % 4U == 0U) ||
	    (channel >= 149U && channel <= 181U &&
	    (channel - 149U) % 4U == 0U))
		return 5000U + 5U * (uint32_t)channel;
	return 0U;
}

static int
ax211_pci_runtime_scan_profile(
	const struct ax211_pci_controller *controller,
	struct wlan_scan_profile *profile)
{
	const struct intel_ax211_scan_profile *source;
	size_t index;

	if (controller == NULL || profile == NULL)
		return EINVAL;
	source = &controller->scan_session.full_profile;
	if (source->channel_count == 0U ||
	    source->channel_count > WLAN_SCAN_CHANNEL_MAX)
		return EINVAL;
	memset(profile, 0, sizeof(*profile));
	profile->channel_count = (uint32_t)source->channel_count;
	for (index = 0U; index < source->channel_count; index++) {
		uint32_t frequency = ax211_pci_channel_frequency(
		    source->channel[index]);

		if (frequency == 0U)
			return EINVAL;
		profile->channels[index].channel = source->channel[index];
		profile->channels[index].center_frequency_mhz = frequency;
	}
	return 0;
}

static int
ax211_pci_scan_channel_present(
	const struct ax211_pci_controller *controller,
	uint8_t channel)
{
	size_t index;

	if (controller == NULL || !controller->scan_initialized)
		return 0;
	for (index = 0U;
	    index < controller->scan_session.full_profile.channel_count;
	    index++) {
		if (controller->scan_session.full_profile.channel[index] == channel)
			return 1;
	}
	return 0;
}

/* Retires every common-generation reference before the hardware epoch stops. */
static void
ax211_pci_scan_clear(
	struct ax211_pci_controller *controller)
{
	if (controller == NULL)
		return;
	controller->scan_initialized = 0U;
	controller->scan_step_index = 0U;
	controller->bss_staging_generation = 0U;
	controller->bss_published_generation = 0U;
	controller->bss_staging_initialized = 0U;
	controller->bss_published_initialized = 0U;
	ax211_pci_scrub(&controller->scan_session,
	    sizeof(controller->scan_session));
	ax211_pci_scrub(&controller->bss_staging_cache,
	    sizeof(controller->bss_staging_cache));
	ax211_pci_scrub(&controller->bss_published_cache,
	    sizeof(controller->bss_published_cache));
	ax211_pci_scrub(controller->runtime_frame,
	    sizeof(controller->runtime_frame));
}

/* Drops an unpublished scan generation without touching the last snapshot. */
static void
ax211_pci_bss_staging_discard(
	struct ax211_pci_controller *controller)
{
	if (controller == NULL)
		return;
	controller->bss_staging_generation = 0U;
	controller->bss_staging_initialized = 0U;
	ax211_pci_scrub(&controller->bss_staging_cache,
	    sizeof(controller->bss_staging_cache));
}

/* Atomically replaces the private snapshot after the complete scan succeeds. */
static int
ax211_pci_bss_staging_publish(
	struct ax211_pci_controller *controller,
	uint64_t generation)
{
	if (controller == NULL || generation == 0U ||
	    !controller->bss_staging_initialized)
		return EINVAL;
	if (controller->bss_staging_generation != generation)
		return ESTALE;
	controller->bss_published_cache = controller->bss_staging_cache;
	controller->bss_published_generation = generation;
	controller->bss_published_initialized = 1U;
	ax211_pci_bss_staging_discard(controller);
	return 0;
}

/* Decodes only the already copied, epoch-tagged event envelope. */
static int
ax211_pci_event_message(
	const uint8_t *bytes,
	size_t length,
	const struct intel_ax211_boot_received_event *received,
	struct intel_ax211_event *event,
	struct intel_ax211_protocol_message *message)
{
	if (bytes == NULL || received == NULL || event == NULL ||
	    message == NULL || received->generation == 0U ||
	    received->length != length ||
	    intel_ax211_event_decode(bytes, length, event) != INTEL_AX211_OK ||
	    event->payload_offset > length || event->payload_length !=
	    length - event->payload_offset)
		return EIO;
	memset(message, 0, sizeof(*message));
	message->opcode = event->command.opcode;
	message->group = event->flags &
	    (uint8_t)~INTEL_AX211_PROTOCOL_COMMAND_FAILED_MASK;
	message->version = received->notification_version;
	message->flags = event->flags;
	message->queue = event->queue;
	message->index = event->index;
	message->generation = received->generation;
	message->payload = bytes + event->payload_offset;
	message->payload_length = event->payload_length;
	if (ax211_pci_tx_event(event)) {
		message->group = INTEL_AX211_TX_GROUP;
		message->version = INTEL_AX211_TX_NOTIFICATION_VERSION;
	}
	return 0;
}

/* Delivers the asynchronous request/abort acknowledgement in poll context. */
static int
ax211_pci_scan_command_dispatch(
	struct ax211_pci_controller *controller,
	const uint8_t *bytes,
	size_t length,
	uint64_t now)
{
	uint8_t phase;
	int report_result;
	int result;

	if (!controller->scan_initialized)
		return 0;
	phase = controller->scan_session.phase;
	if (phase == INTEL_AX211_SCAN_SESSION_WAIT_START_ACK) {
		result = intel_ax211_scan_session_start_ack(
		    &controller->scan_session, bytes, length,
		    controller->hardware_epoch, now);
		if (result == INTEL_AX211_SCAN_SESSION_OK) {
			report_result = wlan_station_report_scan_channel_ready(
			    controller->station,
			    controller->scan_session.common_generation,
			    controller->scan_step_index);
			if (report_result != 0 && report_result != ESTALE &&
			    report_result != ENODEV)
				ax211_pci_scan_report_error(controller,
				    INTEL_AX211_SCAN_SESSION_FAILED);
			return 0;
		}
	} else if (phase == INTEL_AX211_SCAN_SESSION_WAIT_ABORT_ACK) {
		result = intel_ax211_scan_session_abort_ack(
		    &controller->scan_session, bytes, length,
		    controller->hardware_epoch, now);
	} else
		return 0;
	if (result == INTEL_AX211_SCAN_SESSION_ABORTED)
		ax211_pci_bss_staging_discard(controller);
	if (result == INTEL_AX211_SCAN_SESSION_OK ||
	    result == INTEL_AX211_SCAN_SESSION_ABORTED ||
	    result == INTEL_AX211_SCAN_SESSION_DUPLICATE ||
	    result == INTEL_AX211_SCAN_SESSION_STALE ||
	    result == INTEL_AX211_SCAN_SESSION_OUT_OF_ORDER)
		return 0;
	hal_printf("intel-ax211: scan command failed phase=%u result=%d "
	    "length=%u opcode=%02x group=%02x index=%02x queue=%02x\n",
	    phase, result, (unsigned)length,
	    length > 4U ? bytes[4U] : 0U,
	    length > 5U ? bytes[5U] : 0U,
	    length > 6U ? bytes[6U] : 0U,
	    length > 7U ? bytes[7U] : 0U);
	ax211_pci_scan_report_error(controller, result);
	return 0;
}

/* Accepts only scan notifications belonging to the current hardware epoch. */
static int
ax211_pci_scan_notification_dispatch(
	struct ax211_pci_controller *controller,
	const struct intel_ax211_protocol_message *message,
	uint64_t now)
{
	struct intel_ax211_scan_session_event reported;
	int result;

	if (!controller->scan_initialized)
		return 0;
	memset(&reported, 0, sizeof(reported));
	result = intel_ax211_scan_session_notification(
	    &controller->scan_session, message, now, &reported);
	if (result == INTEL_AX211_SCAN_SESSION_FAILED &&
	    message->payload != NULL && message->payload_length >= 16U) {
		const uint8_t *payload = message->payload;

		hal_printf("intel-ax211: unexpected scan notification opcode=%02x "
		    "flags=%02x "
		    "length=%u uid=%02x%02x%02x%02x schedule=%u iteration=%u "
		    "status=%u ebs=%u tail=%02x%02x%02x%02x%02x%02x%02x%02x\n",
		    message->opcode, message->flags,
		    (unsigned)message->payload_length,
		    payload[3U], payload[2U], payload[1U], payload[0U],
		    payload[4U], payload[5U], payload[6U], payload[7U],
		    payload[8U], payload[9U], payload[10U], payload[11U],
		    payload[12U], payload[13U], payload[14U], payload[15U]);
	}
	if (result == INTEL_AX211_SCAN_SESSION_ABORTED)
		ax211_pci_bss_staging_discard(controller);
	if (result == INTEL_AX211_SCAN_SESSION_OK ||
	    result == INTEL_AX211_SCAN_SESSION_COMPLETE ||
	    result == INTEL_AX211_SCAN_SESSION_ABORTED ||
	    result == INTEL_AX211_SCAN_SESSION_DUPLICATE ||
	    result == INTEL_AX211_SCAN_SESSION_STALE)
		return 0;
	ax211_pci_scan_report_error(controller, result);
	return 0;
}

/* Normalizes MPDU v5, reports scan frames, and dispatches connection frames. */
static int
ax211_pci_rx_dispatch(
	struct ax211_pci_controller *controller,
	const struct intel_ax211_protocol_message *message)
{
	struct intel_ax211_bss_entry bss_entry;
	struct intel_ax211_rx_mpdu mpdu;
	uint16_t frame_control;
	uint16_t subtype;
	int dispatch_result;
	int result;

	memset(&bss_entry, 0, sizeof(bss_entry));
	memset(&mpdu, 0, sizeof(mpdu));
	ax211_pci_scrub(controller->runtime_frame,
	    sizeof(controller->runtime_frame));
	result = intel_ax211_rx_mpdu_decode(message,
	    controller->hardware_epoch, controller->runtime_frame,
	    sizeof(controller->runtime_frame), &mpdu);
	if (result != INTEL_AX211_RX_OK) {
		/* A malformed or unsupported over-the-air frame is local to this
		 * receive slot.  Dropping it must not quarantine the firmware epoch. */
		ax211_pci_scrub(controller->runtime_frame,
		    sizeof(controller->runtime_frame));
		return 0;
	}
	frame_control = (uint16_t)((uint16_t)mpdu.frame[0U] |
	    ((uint16_t)mpdu.frame[1U] << 8));
	subtype = frame_control & 0x00f0U;
	if (controller->scan_initialized &&
	    controller->scan_session.phase ==
	    INTEL_AX211_SCAN_SESSION_RUNNING &&
	    (frame_control & 0x000cU) == 0U &&
	    (subtype == 0x0080U || subtype == 0x0050U)) {
		if (controller->bss_staging_initialized &&
		    controller->bss_staging_generation ==
		    controller->scan_session.common_generation &&
		    intel_ax211_bss_decode(&mpdu,
		    controller->scan_session.common_generation,
		    controller->hardware_epoch, &bss_entry) ==
		    INTEL_AX211_BSS_OK) {
			/* Use the same monotonic observation point immediately before
			 * both private and common admission decisions. */
			bss_entry.last_seen_ticks = clock_ticks();
			(void)intel_ax211_bss_cache_observe(
			    &controller->bss_staging_cache, &bss_entry);
		}
		(void)wlan_station_report_scan_frame(controller->station,
		    controller->scan_session.common_generation, mpdu.frame,
		    mpdu.length, mpdu.rssi_dbm, mpdu.channel);
	}
	dispatch_result = 0;
	if (controller->connection_generation != 0U)
		dispatch_result = ax211_pci_connection_rx_dispatch(controller,
		    &mpdu, frame_control);
	ax211_pci_scrub(&bss_entry, sizeof(bss_entry));
	ax211_pci_scrub(controller->runtime_frame,
	    sizeof(controller->runtime_frame));
	return dispatch_result;
}

/* Converts private coordinator outcomes to one common scan error latch. */
static void
ax211_pci_scan_report_error(
	struct ax211_pci_controller *controller,
	int result)
{
	int error;

	if (controller == NULL || controller->station == NULL ||
	    !controller->scan_initialized ||
	    controller->scan_session.common_generation == 0U)
		return;
	error = ax211_pci_scan_result_errno(result);
	if (error != 0) {
		hal_printf("intel-ax211: scan generation=%u failed result=%d "
		    "error=%d phase=%u\n",
		    (unsigned)controller->scan_session.common_generation,
		    result, error, controller->scan_session.phase);
		ax211_pci_bss_staging_discard(controller);
		(void)wlan_station_report_scan_error(controller->station,
		    controller->scan_session.common_generation, error);
	}
}

static int
ax211_pci_scan_result_errno(
	int result)
{
	switch (result) {
	case INTEL_AX211_SCAN_SESSION_OK:
	case INTEL_AX211_SCAN_SESSION_COMPLETE:
	case INTEL_AX211_SCAN_SESSION_ABORTED:
		return 0;
	case INTEL_AX211_SCAN_SESSION_INVALID:
		return EINVAL;
	case INTEL_AX211_SCAN_SESSION_UNSUPPORTED:
		return ENOTSUP;
	case INTEL_AX211_SCAN_SESSION_BUSY:
		return EBUSY;
	case INTEL_AX211_SCAN_SESSION_STALE:
	case INTEL_AX211_SCAN_SESSION_DUPLICATE:
	case INTEL_AX211_SCAN_SESSION_OUT_OF_ORDER:
		return ESTALE;
	case INTEL_AX211_SCAN_SESSION_TIMEOUT:
		return ETIMEDOUT;
	case INTEL_AX211_SCAN_SESSION_COMMAND:
	case INTEL_AX211_SCAN_SESSION_FAILED:
	default:
		return EIO;
	}
}

/* Dispatches one recycled-safe copy; no DMA pointer crosses this boundary. */
static int
ax211_pci_runtime_event_dispatch(
	struct ax211_pci_controller *controller,
	const uint8_t *bytes,
	size_t length,
	const struct intel_ax211_boot_received_event *event)
{
	struct intel_ax211_event decoded;
	struct intel_ax211_protocol_message message;
	uint64_t now;
	int result;

	if (controller == NULL || bytes == NULL || event == NULL ||
	    event->generation == 0U || event->generation !=
	    controller->hardware_epoch || event->length != length)
		return EINVAL;
	memset(&decoded, 0, sizeof(decoded));
	memset(&message, 0, sizeof(message));
	result = ax211_pci_event_message(bytes, length, event, &decoded,
	    &message);
	if (result != 0)
		return result;
	if (ax211_pci_clock_us(controller, &now) != 0)
		return EIO;
	if (ax211_pci_tx_event(&decoded))
		return ax211_pci_tx_dispatch(controller, &message);
	if ((decoded.queue & 0x80U) == 0U)
		return ax211_pci_scan_command_dispatch(controller, bytes, length,
		    now);
	if (message.group == INTEL_AX211_SCAN_GROUP_LEGACY &&
	    (message.opcode == INTEL_AX211_SCAN_COMPLETE_OPCODE ||
	    message.opcode == INTEL_AX211_SCAN_ITERATION_COMPLETE_OPCODE))
		return ax211_pci_scan_notification_dispatch(controller, &message,
		    now);
	if (message.group == INTEL_AX211_RX_MPDU_GROUP &&
	    message.opcode == INTEL_AX211_RX_MPDU_OPCODE)
		return ax211_pci_rx_dispatch(controller, &message);
	if (message.group == INTEL_AX211_ASSOC_GROUP_MAC_CONFIG &&
	    message.opcode == INTEL_AX211_ASSOC_SESSION_NOTIFICATION_OPCODE &&
	    controller->association_initialized) {
		result = intel_ax211_assoc_session_event_accept(
		    &controller->association, &message,
		    controller->connection_generation, controller->hardware_epoch);
		if (result == INTEL_AX211_ASSOC_SESSION_EXPIRED ||
		    result == INTEL_AX211_ASSOC_EVENT_IGNORED ||
		    result == INTEL_AX211_ASSOC_DUPLICATE ||
		    result == INTEL_AX211_ASSOC_STALE)
			return 0;
		return EIO;
	}
	return 0;
}

/* Retains an exact post-recycle notification until the common gate unwinds. */
static int
ax211_pci_deferred_event_enqueue(
	struct ax211_pci_controller *controller,
	const uint8_t *bytes,
	const struct intel_ax211_boot_received_event *received)
{
	struct ax211_pci_deferred_event *slot;
	unsigned index;

	if (controller == NULL || bytes == NULL || received == NULL ||
	    received->length == 0U || received->length >
	    INTEL_AX211_BOOT_EVENT_CAPACITY || received->generation == 0U ||
	    received->generation != controller->hardware_epoch)
		return EINVAL;
	if (controller->deferred_event_count >= AX211_DEFERRED_EVENT_LIMIT)
		return ENOBUFS;
	index = (controller->deferred_event_head +
	    controller->deferred_event_count) % AX211_DEFERRED_EVENT_LIMIT;
	slot = &controller->deferred_event[index];
	memset(slot, 0, sizeof(*slot));
	slot->received = *received;
	memcpy(slot->bytes, bytes, received->length);
	controller->deferred_event_count++;
	if (controller->net_device != NULL)
		net_device_schedule_poll(controller->net_device);
	return 0;
}

/* Dispatches one copied notification only from the outer poll safe point. */
static int
ax211_pci_deferred_event_drain_one(
	struct ax211_pci_controller *controller)
{
	struct ax211_pci_deferred_event *slot;
	struct intel_ax211_boot_received_event received;
	int result;

	if (controller == NULL || controller->deferred_event_count == 0U)
		return ENOENT;
	if (controller->deferred_event_draining)
		return EBUSY;
	slot = &controller->deferred_event[controller->deferred_event_head];
	if (slot->received.length == 0U || slot->received.length >
	    sizeof(controller->deferred_dispatch_event))
		return EIO;
	received = slot->received;
	memcpy(controller->deferred_dispatch_event, slot->bytes, received.length);
	ax211_pci_scrub(slot, sizeof(*slot));
	controller->deferred_event_head = (controller->deferred_event_head + 1U) %
	    AX211_DEFERRED_EVENT_LIMIT;
	controller->deferred_event_count--;
	controller->deferred_event_draining = 1U;
	result = ax211_pci_runtime_event_dispatch(controller,
	    controller->deferred_dispatch_event, received.length, &received);
	ax211_pci_scrub(controller->deferred_dispatch_event, received.length);
	controller->deferred_event_draining = 0U;
	return result;
}

/* Scrubs every copied notification when its firmware epoch is retired. */
static void
ax211_pci_deferred_event_clear(
	struct ax211_pci_controller *controller)
{
	if (controller == NULL)
		return;
	controller->deferred_event_head = 0U;
	controller->deferred_event_count = 0U;
	controller->deferred_event_draining = 0U;
	ax211_pci_scrub(controller->deferred_event,
	    sizeof(controller->deferred_event));
	ax211_pci_scrub(controller->deferred_dispatch_event,
	    sizeof(controller->deferred_dispatch_event));
}

static uint32_t
ax211_pci_get_le32(
	const uint8_t bytes[4])
{
	return (uint32_t)bytes[0U] |
	    ((uint32_t)bytes[1U] << 8) |
	    ((uint32_t)bytes[2U] << 16) |
	    ((uint32_t)bytes[3U] << 24);
}

/* Coherent TX allocations need only an ordered visibility boundary. */
static int
ax211_pci_tx_sync_for_device(
	void *argument,
	const struct drv_dma_buffer *buffer,
	size_t offset,
	size_t length)
{
	struct ax211_pci_controller *controller;

	controller = argument;
	if (controller == NULL || buffer == NULL || buffer->address == NULL ||
	    buffer->device_address == 0U || offset > buffer->size ||
	    length > buffer->size - offset || !controller->runtime_initialized ||
	    !drv_dma_device_is_coherent(controller->tx_ring.dma_device))
		return -1;
	hal_io_wmb();
	return 0;
}

/* Rings only the API89 queue-1 write-pointer doorbell. */
static int
ax211_pci_tx_write32(
	void *argument,
	uint32_t offset,
	uint32_t value)
{
	struct ax211_pci_controller *controller;

	controller = argument;
	if (controller == NULL || offset !=
	    INTEL_AX211_TX_RING_WRITE_POINTER_REGISTER ||
	    controller->mmio.ops == NULL ||
	    controller->mmio.ops->csr_write32 == NULL)
		return -1;
	hal_io_wmb();
	return controller->mmio.ops->csr_write32(controller->mmio.argument,
	    offset, value);
}

static uint64_t
ax211_pci_assoc_clock_us(
	void *argument)
{
	uint64_t now;

	if (ax211_pci_clock_us(argument, &now) != 0)
		return UINT64_MAX;
	return now;
}

/* Converts one common-clock deadline into a bounded device-clock interval. */
static int
ax211_pci_command_timeout(
	struct ax211_pci_controller *controller,
	uint64_t deadline_ticks,
	uint64_t now_us,
	uint64_t *timeout_us,
	uint64_t *deadline_us)
{
	uint64_t now_ticks;
	uint64_t remaining_ticks;
	uint64_t remaining_us;

	if (controller == NULL || timeout_us == NULL || deadline_us == NULL)
		return EINVAL;
	remaining_us = AX211_DIRECT_TIMEOUT_US;
	if (deadline_ticks != 0U && deadline_ticks != UINT64_MAX) {
		now_ticks = clock_ticks();
		if (now_ticks >= deadline_ticks)
			return ETIMEDOUT;
		remaining_ticks = deadline_ticks - now_ticks;
		if (remaining_ticks > UINT64_MAX / 1000000U)
			remaining_us = AX211_DIRECT_TIMEOUT_US;
		else {
			remaining_us = remaining_ticks * 1000000U /
			    KERN_CLOCK_HZ;
			if (remaining_us == 0U)
				remaining_us = 1U;
			if (remaining_us > AX211_DIRECT_TIMEOUT_US)
				remaining_us = AX211_DIRECT_TIMEOUT_US;
		}
	}
	if (now_us > UINT64_MAX - remaining_us)
		return EOVERFLOW;
	*timeout_us = remaining_us;
	*deadline_us = now_us + remaining_us;
	return 0;
}

/*
 * Reads a bounded AX210-family SRAM range while an outer command owns the NIC.
 * The HBUS data window advances by one dword after every successful read.
 */
static int
ax211_pci_sram_read_locked(
	struct ax211_pci_controller *controller,
	uint32_t address,
	uint32_t *words,
	size_t count)
{
	size_t index;

	if (controller == NULL || words == NULL || count == 0U ||
	    address < AX211_ERROR_LOG_MIN_ADDRESS || (address & 3U) != 0U ||
	    count - 1U > (UINT32_MAX - address) / sizeof(uint32_t) ||
	    controller->mmio.nic_lock_depth == 0U ||
	    controller->mmio.ops == NULL ||
	    controller->mmio.ops->csr_read32 == NULL ||
	    controller->mmio.ops->csr_write32 == NULL)
		return EINVAL;
	if (controller->mmio.ops->csr_write32(controller->mmio.argument,
	    AX211_HBUS_TARG_MEM_RADDR, address) != 0)
		return EIO;
	hal_io_mb();
	for (index = 0U; index < count; index++) {
		if (controller->mmio.ops->csr_read32(controller->mmio.argument,
		    AX211_HBUS_TARG_MEM_RDAT, &words[index]) != 0) {
			memset(words, 0, count * sizeof(*words));
			return EIO;
		}
	}
	return 0;
}

/*
 * Captures only the stable LMAC/UMAC error-table fields needed to identify a
 * firmware assertion.  The ALIVE pointers are firmware-owned SRAM addresses;
 * cache-control tag bits are not part of the address.
 */
static void
ax211_pci_firmware_error_dump(
	struct ax211_pci_controller *controller)
{
	uint32_t lmac[AX211_LMAC_ERROR_WORD_COUNT];
	uint32_t umac[AX211_UMAC_ERROR_WORD_COUNT];
	uint32_t lmac_address;
	uint32_t umac_address;
	int lmac_result;
	int umac_result;

	if (controller == NULL)
		return;
	if (!controller->runtime_start.alive_accepted ||
	    controller->runtime_start.alive.status !=
	    INTEL_AX211_PROTOCOL_ALIVE_STATUS_OK) {
		hal_printf("intel-ax211: firmware error table unavailable "
		    "alive=%u status=%04x\n",
		    (unsigned)controller->runtime_start.alive_accepted,
		    controller->runtime_start.alive.status);
		return;
	}
	memset(lmac, 0, sizeof(lmac));
	memset(umac, 0, sizeof(umac));
	lmac_address = controller->runtime_start.alive.lmac[0].
	    error_event_table;
	umac_address = controller->runtime_start.alive.umac.error_info &
	    ~AX211_FW_ADDR_CACHE_CONTROL;
	lmac_result = ax211_pci_sram_read_locked(controller, lmac_address,
	    lmac, AX211_LMAC_ERROR_WORD_COUNT);
	umac_result = ax211_pci_sram_read_locked(controller, umac_address,
	    umac, AX211_UMAC_ERROR_WORD_COUNT);
	if (lmac_result == 0) {
		hal_printf("intel-ax211: LMAC firmware error ptr=%08x valid=%08x "
		    "id=%08x data=%08x/%08x/%08x hcmd=%08x last=%08x "
		    "isr=%08x/%08x/%08x/%08x/%08x\n", lmac_address,
		    lmac[0U], lmac[1U], lmac[7U], lmac[8U], lmac[9U],
		    lmac[23U], lmac[29U], lmac[24U], lmac[25U], lmac[26U],
		    lmac[27U], lmac[28U]);
	} else {
		hal_printf("intel-ax211: LMAC firmware error unavailable "
		    "ptr=%08x result=%d\n", lmac_address, lmac_result);
	}
	if (umac_result == 0) {
		hal_printf("intel-ax211: UMAC firmware error ptr=%08x valid=%08x "
		    "id=%08x data=%08x/%08x/%08x hcmd=%08x isr=%08x\n",
		    umac_address, umac[0U], umac[1U], umac[6U], umac[7U],
		    umac[8U], umac[13U], umac[14U]);
	} else {
		hal_printf("intel-ax211: UMAC firmware error unavailable "
		    "ptr=%08x result=%d\n", umac_address, umac_result);
	}
}

/*
 * Performs one exact synchronous command while lifecycle_lock owns the
	 * transport.  Asynchronous notifications are copied to a bounded queue and
	 * dispatched only after the outer common callback has unwound; only the
	 * matching oldest command response retires the slot.
 */
static int
ax211_pci_direct_command(
	struct ax211_pci_controller *controller,
	const struct intel_ax211_command_request *request,
	uint64_t deadline_ticks,
	uint8_t *response,
	size_t response_capacity,
	size_t *response_length,
	struct intel_ax211_protocol_message *completed_message,
	struct intel_ax211_protocol_pending_command *completed_pending)
{
	struct intel_ax211_boot_received_event received;
	struct intel_ax211_command_handle handle;
	struct intel_ax211_command_entry *entry;
	struct intel_ax211_event decoded;
	struct intel_ax211_protocol_pending_command pending;
	uint64_t deadline_us;
	uint64_t now_us;
	uint64_t timeout_us;
	size_t length;
	unsigned index;
	int command_submitted;
	int lock_result;
	int result;
	int unlock_result;

	if (controller == NULL || request == NULL || response_length == NULL ||
	    (response_capacity != 0U && response == NULL) ||
	    !controller->runtime_initialized || !controller->runtime_active ||
	    controller->runtime_start.state !=
	    INTEL_AX211_RUNTIME_START_STATE_RUNNING ||
	    !controller->runtime_start.commands_initialized ||
	    intel_ax211_command_pending_count(
	    &controller->runtime_start.commands) != 0U)
		return EBUSY;
	*response_length = 0U;
	if (completed_message != NULL)
		memset(completed_message, 0, sizeof(*completed_message));
	if (completed_pending != NULL)
		memset(completed_pending, 0, sizeof(*completed_pending));
	memset(&handle, 0, sizeof(handle));
	memset(&pending, 0, sizeof(pending));
	controller->command_fh_causes = 0U;
	controller->command_hw_causes = 0U;
	controller->command_raw_fh_causes = 0U;
	controller->command_raw_hw_causes = 0U;
	result = ax211_pci_clock_us(controller, &now_us);
	if (result == 0)
		result = ax211_pci_command_timeout(controller, deadline_ticks,
		    now_us, &timeout_us, &deadline_us);
	if (result != 0)
		return result;
	lock_result = ax211_pci_nic_lock(controller);
	if (lock_result != 0)
		return EIO;
	command_submitted = 0;
	result = intel_ax211_command_submit(
	    &controller->runtime_start.commands, request, now_us, timeout_us,
	    &handle);
	if (result == INTEL_AX211_COMMAND_OK) {
		command_submitted = 1;
		entry = &controller->runtime_start.commands.entry[
		    handle.token.index];
		pending = entry->pending;
		result = 0;
	} else
		result = EIO;

	index = 0U;
	while (result == 0 && index < AX211_DIRECT_EVENT_LIMIT) {
		memset(&received, 0, sizeof(received));
		result = ax211_pci_receive_event(controller, deadline_us,
		    controller->runtime_event,
		    sizeof(controller->runtime_event), &received);
		if (result == INTEL_AX211_BOOT_RECEIVE_TIMEOUT)
			result = ETIMEDOUT;
		else if (result != INTEL_AX211_BOOT_RECEIVE_OK)
			result = EIO;
		else if (received.generation != controller->hardware_epoch ||
		    received.length == 0U || received.length >
		    sizeof(controller->runtime_event) ||
		    intel_ax211_event_decode(controller->runtime_event,
		    received.length, &decoded) != INTEL_AX211_OK)
			result = EIO;
		else if (ax211_pci_tx_event(&decoded)) {
			result = ax211_pci_deferred_event_enqueue(controller,
			    controller->runtime_event, &received);
			ax211_pci_scrub(controller->runtime_event,
			    received.length);
			index++;
		} else if ((decoded.queue & 0x80U) != 0U) {
			uint8_t event_group;

			event_group = decoded.flags &
			    (uint8_t)~INTEL_AX211_PROTOCOL_COMMAND_FAILED_MASK;
			if (event_group == INTEL_AX211_PROTOCOL_GROUP_LEGACY &&
			    decoded.command.opcode == AX211_REPLY_ERROR_OPCODE) {
				const uint8_t *error_payload;
				uint16_t bad_sequence;

				error_payload = controller->runtime_event +
				    decoded.payload_offset;
				if (decoded.payload_length == AX211_REPLY_ERROR_SIZE) {
					bad_sequence = (uint16_t)error_payload[6U] |
					    ((uint16_t)error_payload[7U] << 8);
					hal_printf("intel-ax211: firmware command error "
					    "type=%08x command=%02x sequence=%04x "
					    "service=%08x\n",
					    ax211_pci_get_le32(error_payload),
					    error_payload[4U], bad_sequence,
					    ax211_pci_get_le32(error_payload + 8U));
				} else
					hal_printf("intel-ax211: malformed firmware "
					    "command error length=%u\n",
					    (unsigned)decoded.payload_length);
			}
			result = ax211_pci_deferred_event_enqueue(controller,
			    controller->runtime_event, &received);
			ax211_pci_scrub(controller->runtime_event,
			    received.length);
			index++;
		} else {
			length = 0U;
			result = intel_ax211_command_complete(
			    &controller->runtime_start.commands,
			    controller->runtime_event, received.length,
			    controller->hardware_epoch, response,
			    response_capacity, &length);
			if (result == INTEL_AX211_COMMAND_OK) {
				*response_length = length;
				if (completed_pending != NULL)
					*completed_pending = pending;
				if (completed_message != NULL) {
					completed_message->opcode =
					    decoded.command.opcode;
					completed_message->group = decoded.flags &
					    (uint8_t)~INTEL_AX211_PROTOCOL_COMMAND_FAILED_MASK;
					completed_message->version =
					    pending.response_version;
					completed_message->flags = decoded.flags;
					completed_message->queue = decoded.queue;
					completed_message->index = decoded.index;
					completed_message->generation =
					    controller->hardware_epoch;
					completed_message->payload = response;
					completed_message->payload_length = length;
				}
				result = 0;
				command_submitted = 0;
			} else {
				hal_printf("intel-ax211: command completion rejected "
				    "request=%02x/%02x response=%02x/%02x "
				    "flags=%02x queue=%02x index=%02x payload=%u "
				    "expected=%u..%u result=%d\n",
				    (unsigned)request->command.group,
				    (unsigned)request->command.opcode,
				    (unsigned)(decoded.flags & (uint8_t)
				    ~INTEL_AX211_PROTOCOL_COMMAND_FAILED_MASK),
				    (unsigned)decoded.command.opcode,
				    (unsigned)decoded.flags, (unsigned)decoded.queue,
				    (unsigned)decoded.index,
				    (unsigned)decoded.payload_length,
				    (unsigned)request->minimum_response_length,
				    (unsigned)request->maximum_response_length,
				    result);
				result = EIO;
			}
			ax211_pci_scrub(controller->runtime_event,
			    received.length);
			break;
		}
	}
	if (result == 0 && index == AX211_DIRECT_EVENT_LIMIT)
		result = ETIMEDOUT;
	if (result != 0 && command_submitted &&
	    ((controller->command_fh_causes &
	    INTEL_AX211_TRANSPORT_FH_CAUSE_ERROR) != 0U ||
	    (controller->command_hw_causes &
	    INTEL_AX211_TRANSPORT_HW_FATAL_CAUSES) != 0U))
		ax211_pci_firmware_error_dump(controller);
	if (result == ETIMEDOUT && command_submitted) {
		uint64_t slot_address;

		slot_address = controller->transport.memory.
		    command_slots_device_address + (uint64_t)handle.token.index *
		    INTEL_AX211_TRANSPORT_COMMAND_SLOT_SIZE;
		hal_printf("intel-ax211: command timeout opcode=%02x group=%02x "
		    "token=%u events=%u reason=%s slot-page=%03x "
		    "fh=%08x hw=%08x raw-fh=%08x raw-hw=%08x "
		    "rx-head=%u rx-tail=%u ring-head=%u ring-tail=%u used=%u\n",
		    (unsigned)request->command.opcode,
		    (unsigned)request->command.group,
		    (unsigned)handle.token.index, index,
		    index == AX211_DIRECT_EVENT_LIMIT ? "event-limit" : "deadline",
		    (unsigned)(slot_address & UINT64_C(0xfff)),
		    controller->command_fh_causes,
		    controller->command_hw_causes,
		    controller->command_raw_fh_causes,
		    controller->command_raw_hw_causes,
		    (unsigned)controller->transport.rx_head,
		    (unsigned)controller->transport.rx_tail,
		    (unsigned)controller->transport.command_ring.head,
		    (unsigned)controller->transport.command_ring.tail,
		    (unsigned)controller->transport.command_ring.used);
	}
	if (result != 0 && command_submitted)
		(void)intel_ax211_command_cancel(
		    &controller->runtime_start.commands, &handle);
	unlock_result = ax211_pci_nic_unlock(controller);
	if (result == 0 && unlock_result != 0)
		result = EIO;
	return result;
}

static int
ax211_pci_assoc_exchange(
	void *argument,
	const struct intel_ax211_assoc_command *command,
	struct intel_ax211_assoc_reply *reply)
{
	struct ax211_pci_controller *controller;
	struct intel_ax211_command_request request;
	struct intel_ax211_protocol_message message;
	struct intel_ax211_protocol_pending_command pending;
	size_t minimum;
	size_t maximum;
	size_t response_length;
	int result;

	controller = argument;
	if (controller == NULL || command == NULL || reply == NULL ||
	    command->common_generation != controller->connection_generation ||
	    command->hardware_epoch != controller->hardware_epoch)
		return INTEL_AX211_ASSOC_INVALID;
	minimum = 0U;
	maximum = 0U;
	if (command->response_kind == INTEL_AX211_ASSOC_RESPONSE_STATUS_ZERO) {
		minimum = 4U;
		maximum = 4U;
	} else if (command->response_kind ==
	    INTEL_AX211_ASSOC_RESPONSE_QUEUE) {
		minimum = INTEL_AX211_ASSOC_QUEUE_RESPONSE_SIZE;
		maximum = INTEL_AX211_ASSOC_QUEUE_RESPONSE_SIZE;
	} else if (command->response_kind ==
	    INTEL_AX211_ASSOC_RESPONSE_IGNORED) {
		maximum = INTEL_AX211_ASSOC_RESPONSE_MAX;
	}
	if (command->step == INTEL_AX211_ASSOC_STEP_QUEUE_ENABLE) {
		result = intel_ax211_tx_ring_queue_add_build(&controller->tx_ring,
		    AX211_ASSOC_STATION_ID, INTEL_AX211_TX_RING_MANAGEMENT_TID,
		    &controller->tx_queue_config);
		if (result != INTEL_AX211_TX_RING_OK || command->payload_length !=
		    sizeof(controller->tx_queue_config.command) ||
		    memcmp(command->payload, controller->tx_queue_config.command,
		    sizeof(controller->tx_queue_config.command)) != 0)
			return INTEL_AX211_ASSOC_INVALID;
	}
	memset(&request, 0, sizeof(request));
	request.command.group = command->group;
	request.command.opcode = command->opcode;
	request.command.version = command->header ==
	    INTEL_AX211_ASSOC_HEADER_WIDE ? command->wire_version :
	    command->layout_version;
	request.payload = command->payload;
	request.payload_length = command->payload_length;
	request.response_version = command->response_version;
	request.minimum_response_length = minimum;
	request.maximum_response_length = maximum;
	memset(&message, 0, sizeof(message));
	memset(&pending, 0, sizeof(pending));
	memset(reply, 0, sizeof(*reply));
	response_length = 0U;
	result = ax211_pci_direct_command(controller, &request,
	    controller->control_deadline_ticks, reply->payload,
	    sizeof(reply->payload), &response_length, &message, &pending);
	if (result != 0) {
		hal_printf("intel-ax211: association exchange failed step=%u "
		    "opcode=%02x group=%02x result=%d\n", command->step,
		    command->opcode, command->group, result);
		return result == ETIMEDOUT ? INTEL_AX211_ASSOC_TIMEOUT :
		    INTEL_AX211_ASSOC_IO;
	}
	reply->step = command->step;
	reply->response_version = command->response_version;
	reply->sequence = command->sequence;
	reply->common_generation = command->common_generation;
	reply->hardware_epoch = command->hardware_epoch;
	reply->payload_length = response_length;
	if (command->step == INTEL_AX211_ASSOC_STEP_QUEUE_ENABLE) {
		result = intel_ax211_tx_ring_queue_add_complete(
		    &controller->tx_ring, &controller->tx_queue_config,
		    controller->hardware_epoch, controller->connection_generation,
		    &message, &pending);
		if (result != INTEL_AX211_TX_RING_OK) {
			uint16_t flags;
			uint16_t queue;
			uint16_t reserved;
			uint16_t write_pointer;

			queue = response_length >= 2U ?
			    (uint16_t)reply->payload[0U] |
			    ((uint16_t)reply->payload[1U] << 8) : 0;
			flags = response_length >= 4U ?
			    (uint16_t)reply->payload[2U] |
			    ((uint16_t)reply->payload[3U] << 8) : 0;
			write_pointer = response_length >= 6U ?
			    (uint16_t)reply->payload[4U] |
			    ((uint16_t)reply->payload[5U] << 8) : 0;
			reserved = response_length >= 8U ?
			    (uint16_t)reply->payload[6U] |
			    ((uint16_t)reply->payload[7U] << 8) : 0;
			hal_printf("intel-ax211: queue add response rejected result=%d "
			    "length=%u qid=%u flags=%04x wp=%u reserved=%04x\n",
			    result, (unsigned)response_length, (unsigned)queue,
			    (unsigned)flags, (unsigned)write_pointer,
			    (unsigned)reserved);
			return INTEL_AX211_ASSOC_FIRMWARE;
		}
	} else if (command->step == INTEL_AX211_ASSOC_STEP_QUEUE_REMOVE) {
		result = intel_ax211_tx_ring_reset(&controller->tx_ring, 1);
		if (result != INTEL_AX211_TX_RING_OK)
			return INTEL_AX211_ASSOC_IO;
	}
	return INTEL_AX211_ASSOC_OK;
}

static int
ax211_pci_assoc_result_errno(
	int result)
{
	switch (result) {
	case INTEL_AX211_ASSOC_OK:
	case INTEL_AX211_ASSOC_AUTH_READY:
	case INTEL_AX211_ASSOC_COMPLETE:
	case INTEL_AX211_ASSOC_ROLLED_BACK:
		return 0;
	case INTEL_AX211_ASSOC_INVALID:
		return EINVAL;
	case INTEL_AX211_ASSOC_UNSUPPORTED:
		return ENOTSUP;
	case INTEL_AX211_ASSOC_PENDING:
		return EBUSY;
	case INTEL_AX211_ASSOC_STALE:
	case INTEL_AX211_ASSOC_DUPLICATE:
	case INTEL_AX211_ASSOC_OUT_OF_ORDER:
		return ESTALE;
	case INTEL_AX211_ASSOC_TIMEOUT:
		return ETIMEDOUT;
	case INTEL_AX211_ASSOC_FIRMWARE:
	case INTEL_AX211_ASSOC_IO:
	case INTEL_AX211_ASSOC_ROLLBACK_FAILED:
	default:
		return EIO;
	}
}

static int
ax211_pci_tx_ring_result_errno(
	int result)
{
	switch (result) {
	case INTEL_AX211_TX_RING_OK:
		return 0;
	case INTEL_AX211_TX_RING_INVALID:
		return EINVAL;
	case INTEL_AX211_TX_RING_UNSUPPORTED:
		return ENOTSUP;
	case INTEL_AX211_TX_RING_NOT_READY:
		return ENETDOWN;
	case INTEL_AX211_TX_RING_FULL:
	case INTEL_AX211_TX_RING_PENDING:
		return EBUSY;
	case INTEL_AX211_TX_RING_STALE:
	case INTEL_AX211_TX_RING_OUT_OF_ORDER:
		return ESTALE;
	case INTEL_AX211_TX_RING_DUPLICATE:
		return EEXIST;
	case INTEL_AX211_TX_RING_TIMEOUT:
		return ETIMEDOUT;
	case INTEL_AX211_TX_RING_NO_MEMORY:
		return ENOMEM;
	case INTEL_AX211_TX_RING_TX_FAILED:
	case INTEL_AX211_TX_RING_IO_ERROR:
	case INTEL_AX211_TX_RING_POISONED:
	case INTEL_AX211_TX_RING_BARRIER_REQUIRED:
	case INTEL_AX211_TX_RING_KICK_FAILED:
	case INTEL_AX211_TX_RING_MALFORMED:
	default:
		return EIO;
	}
}

static int
ax211_pci_key_result_errno(
	int result)
{
	switch (result) {
	case INTEL_AX211_KEY_OK:
	case INTEL_AX211_KEY_DUPLICATE:
		return 0;
	case INTEL_AX211_KEY_INVALID:
		return EINVAL;
	case INTEL_AX211_KEY_UNSUPPORTED:
		return ENOTSUP;
	case INTEL_AX211_KEY_STALE:
		return ESTALE;
	case INTEL_AX211_KEY_MISSING:
	default:
		return ENOENT;
	}
}

/* Copies only scan-derived metadata which belongs to this firmware epoch. */
static int
ax211_pci_assoc_profile(
	struct ax211_pci_controller *controller,
	const struct wlan_bss_record *bss,
	uint64_t connection_generation,
	struct intel_ax211_assoc_profile *profile)
{
	struct intel_ax211_bss_assoc_metadata metadata;
	struct intel_ax211_bss_entry entry;
	struct intel_ax211_tx_queue_config queue;
	int result;

	if (controller == NULL || bss == NULL || profile == NULL ||
	    !controller->bss_published_initialized ||
	    !controller->tx_ring_allocated || connection_generation == 0U ||
	    !ax211_pci_scan_channel_present(controller, bss->channel))
		return EINVAL;
	memset(&entry, 0, sizeof(entry));
	result = intel_ax211_bss_cache_lookup(&controller->bss_published_cache,
	    bss->bssid, bss->channel, controller->hardware_epoch, &entry);
	if (result != INTEL_AX211_BSS_OK)
		return result == INTEL_AX211_BSS_NOT_FOUND ? ENOENT : ESTALE;
	memset(&metadata, 0, sizeof(metadata));
	result = intel_ax211_bss_assoc_metadata(&entry,
	    connection_generation, controller->hardware_epoch, &metadata);
	if (result != INTEL_AX211_BSS_OK)
		return EIO;
	memset(&queue, 0, sizeof(queue));
	result = intel_ax211_tx_ring_queue_add_build(&controller->tx_ring,
	    AX211_ASSOC_STATION_ID, INTEL_AX211_TX_RING_MANAGEMENT_TID, &queue);
	if (result != INTEL_AX211_TX_RING_OK)
		return ax211_pci_tx_ring_result_errno(result);
	memset(profile, 0, sizeof(*profile));
	memcpy(profile->station_address, controller->net_device->hwaddr, 6U);
	memcpy(profile->bssid, metadata.bssid, 6U);
	profile->channel = metadata.channel;
	profile->channel_width_mhz = INTEL_AX211_ASSOC_CHANNEL_WIDTH_MHZ;
	profile->rx_chain_mask = controller->runtime_start.nvm.rx_chain_mask;
	/* Preserve the mandatory 1 Mbps fallback bitmap used by the reference
	 * command ABI even on 5 GHz, where the firmware does not use CCK. */
	profile->cck_ack_rates = profile->channel <= 14U ?
	    AX211_ASSOC_CCK_ACK_RATES : 1U;
	/* Mandatory 5 GHz basic OFDM rates: 6, 12, and 24 Mbit/s.  Do not
	 * advertise every optional OFDM rate as an ACK/basic rate. */
	profile->ofdm_ack_rates = AX211_ASSOC_OFDM_ACK_RATES;
	profile->short_preamble = profile->channel <= 14U &&
	    (metadata.capability & AX211_CAPABILITY_SHORT_PREAMBLE) != 0U;
	/* Short-slot timing is mandatory in the 5 GHz OFDM-only band even when
	 * the AP omits the 2.4-GHz capability bit from its beacon. */
	profile->short_slot = profile->channel > 14U ||
	    (metadata.capability & AX211_CAPABILITY_SHORT_SLOT) != 0U;
	/*
	 * The first p038 common association profile does not negotiate WMM.
	 * Keep data frames non-QoS even when the scanned BSS advertises WMM;
	 * enabling QoS here would make the common 802.11 layer reject its own
	 * data frames before a matching WMM association contract exists.
	 */
	profile->qos = 0U;
	profile->beacon_interval_tu = metadata.beacon_interval_tu;
	profile->dtim_period = metadata.tim_valid != 0U ?
	    metadata.dtim_period : 0U;
	profile->queue_byte_count_address = queue.byte_count_address;
	profile->queue_descriptor_address = queue.tfd_address;
	/* Safe legacy EDCA defaults: BE, BK, VI, VO. */
	profile->edca[0U].ecw_min = 4U;
	profile->edca[0U].ecw_max = 10U;
	profile->edca[0U].aifsn = 3U;
	profile->edca[1U].ecw_min = 4U;
	profile->edca[1U].ecw_max = 10U;
	profile->edca[1U].aifsn = 7U;
	profile->edca[2U].ecw_min = 3U;
	profile->edca[2U].ecw_max = 4U;
	profile->edca[2U].aifsn = 2U;
	profile->edca[2U].txop_32us = 94U;
	profile->edca[3U].ecw_min = 2U;
	profile->edca[3U].ecw_max = 3U;
	profile->edca[3U].aifsn = 2U;
	profile->edca[3U].txop_32us = 47U;
	controller->selected_bss = entry;
	controller->selected_metadata = metadata;
	controller->selected_bss_valid = 1U;
	ax211_pci_scrub(&queue, sizeof(queue));
	ax211_pci_scrub(&metadata, sizeof(metadata));
	ax211_pci_scrub(&entry, sizeof(entry));
	return 0;
}

static void
ax211_pci_connection_clear(
	struct ax211_pci_controller *controller)
{
	uint8_t key_index;

	if (controller == NULL)
		return;
	controller->connection_generation = 0U;
	controller->control_deadline_ticks = 0U;
	controller->selected_bss_valid = 0U;
	controller->association_initialized = 0U;
	controller->keys_initialized = 0U;
	ax211_pci_scrub(&controller->selected_bss,
	    sizeof(controller->selected_bss));
	ax211_pci_scrub(&controller->selected_metadata,
	    sizeof(controller->selected_metadata));
	ax211_pci_scrub(&controller->association,
	    sizeof(controller->association));
	ax211_pci_scrub(&controller->keys, sizeof(controller->keys));
	ax211_pci_staged_key_clear(&controller->staged_pairwise_key);
	for (key_index = 0U; key_index < INTEL_AX211_KEY_INDEX_LIMIT;
	    key_index++)
		ax211_pci_staged_key_clear(
		    &controller->staged_group_key[key_index]);
	controller->retired_pairwise_key_generation = 0U;
	ax211_pci_scrub(controller->retired_group_key_generation,
	    sizeof(controller->retired_group_key_generation));
	ax211_pci_scrub(controller->retired_group_key_remove,
	    sizeof(controller->retired_group_key_remove));
	ax211_pci_scrub(&controller->tx_queue_config,
	    sizeof(controller->tx_queue_config));
	ax211_pci_scrub(controller->tx_report_completion,
	    sizeof(controller->tx_report_completion));
}

/* Performs the association state machine's finite reverse-order teardown. */
static int
ax211_pci_assoc_rollback(
	struct ax211_pci_controller *controller,
	uint64_t generation)
{
	uint64_t now;
	int result;

	if (controller == NULL)
		return EINVAL;
	if (!controller->association_initialized) {
		if (controller->connection_generation != 0U && generation != 0U &&
		    controller->connection_generation != generation)
			return ESTALE;
		ax211_pci_connection_clear(controller);
		return 0;
	}
	if (generation != 0U && generation != controller->connection_generation)
		return ESTALE;
	if (controller->keys_initialized) {
		result = ax211_pci_keys_remove_all(controller,
		    controller->control_deadline_ticks);
		if (result != 0)
			return result;
	}
	if (ax211_pci_clock_us(controller, &now) != 0)
		return EIO;
	result = intel_ax211_assoc_cancel(&controller->association,
	    controller->connection_generation, controller->hardware_epoch, now);
	if (result == INTEL_AX211_ASSOC_PENDING ||
	    result == INTEL_AX211_ASSOC_ROLLED_BACK) {
		result = intel_ax211_assoc_drive(&controller->association,
		    &ax211_assoc_ops, controller);
	}
	if (result != INTEL_AX211_ASSOC_ROLLED_BACK &&
	    result != INTEL_AX211_ASSOC_OK)
		return ax211_pci_assoc_result_errno(result);
	if (controller->tx_ring.enabled || controller->tx_ring.pending_count != 0U)
		return EIO;
	ax211_pci_connection_clear(controller);
	return 0;
}

/* Enables own and broadcast/multicast reception after MAC association. */
static int
ax211_pci_mcast_filter_configure(
	struct ax211_pci_controller *controller,
	uint64_t deadline_ticks)
{
	struct intel_ax211_command_request request;
	uint8_t payload[INTEL_AX211_ASSOC_MCAST_FILTER_SIZE];
	size_t response_length;
	int result;

	if (controller == NULL || !controller->selected_bss_valid)
		return EINVAL;
	memset(payload, 0, sizeof(payload));
	result = intel_ax211_assoc_mcast_filter_encode(
	    controller->selected_metadata.bssid, payload, sizeof(payload));
	if (result != INTEL_AX211_ASSOC_OK) {
		ax211_pci_scrub(payload, sizeof(payload));
		return ax211_pci_assoc_result_errno(result);
	}
	memset(&request, 0, sizeof(request));
	request.command.group = INTEL_AX211_ASSOC_GROUP_LEGACY;
	request.command.opcode = INTEL_AX211_ASSOC_MCAST_FILTER_OPCODE;
	request.command.version = INTEL_AX211_ASSOC_MCAST_FILTER_VERSION;
	request.payload = payload;
	request.payload_length = sizeof(payload);
	request.response_version = 0U;
	request.minimum_response_length = 0U;
	request.maximum_response_length = 0U;
	response_length = 0U;
	result = ax211_pci_direct_command(controller, &request, deadline_ticks,
	    NULL, 0U, &response_length, NULL, NULL);
	if (result == 0 && response_length != 0U)
		result = EIO;
	ax211_pci_scrub(payload, sizeof(payload));
	return result;
}

/* Programs the frozen API89 PM-off client table after multicast admission. */
static int
ax211_pci_mac_power_configure(
	struct ax211_pci_controller *controller,
	uint64_t deadline_ticks)
{
	struct intel_ax211_command_request request;
	uint8_t payload[INTEL_AX211_ASSOC_MAC_POWER_SIZE];
	uint8_t response[INTEL_AX211_ASSOC_MAC_POWER_RESPONSE_SIZE];
	uint32_t beacon_interval_ms;
	size_t response_length;
	int result;

	if (controller == NULL || !controller->selected_bss_valid ||
	    controller->selected_metadata.beacon_interval_tu == 0U)
		return EINVAL;
	/* One TU is 1.024 ms; round up so keep-alive never undershoots the
	 * advertised beacon interval when converting to the codec's ms input. */
	beacon_interval_ms = ((uint32_t)controller->selected_metadata.
	    beacon_interval_tu * 1024U + 999U) / 1000U;
	memset(payload, 0, sizeof(payload));
	memset(response, 0, sizeof(response));
	result = intel_ax211_assoc_mac_power_encode(
	    controller->selected_metadata.dtim_period, beacon_interval_ms,
	    payload, sizeof(payload));
	if (result != INTEL_AX211_ASSOC_OK) {
		ax211_pci_scrub(payload, sizeof(payload));
		return ax211_pci_assoc_result_errno(result);
	}
	memset(&request, 0, sizeof(request));
	request.command.group = INTEL_AX211_ASSOC_GROUP_LEGACY;
	request.command.opcode = INTEL_AX211_ASSOC_MAC_POWER_OPCODE;
	request.command.version = INTEL_AX211_ASSOC_MAC_POWER_VERSION;
	request.payload = payload;
	request.payload_length = sizeof(payload);
	request.response_version = 0U;
	request.minimum_response_length = 0U;
	request.maximum_response_length = sizeof(response);
	response_length = 0U;
	result = ax211_pci_direct_command(controller, &request, deadline_ticks,
	    response, sizeof(response), &response_length, NULL, NULL);
	if (result == 0)
		result = ax211_pci_assoc_result_errno(
		    intel_ax211_assoc_mac_power_response_validate(response,
		    response_length));
	ax211_pci_scrub(response, sizeof(response));
	ax211_pci_scrub(payload, sizeof(payload));
	return result;
}

/* Removes every tuple still reachable by firmware before station teardown. */
static int
ax211_pci_keys_remove_all(
	struct ax211_pci_controller *controller,
	uint64_t deadline_ticks)
{
	uint8_t command[INTEL_AX211_KEY_COMMAND_SIZE];
	uint8_t index;
	uint64_t generation;
	int result;

	if (controller == NULL || !controller->keys_initialized)
		return 0;
	result = 0;
	for (index = 0U; index < INTEL_AX211_KEY_INDEX_LIMIT && result == 0;
	    index++) {
		generation = controller->keys.active_group[index];
		if (generation == 0U && controller->staged_group_key[index].valid &&
		    controller->staged_group_key[index].programmed)
			generation = controller->staged_group_key[index].request.
			    key_generation;
		if (generation == 0U &&
		    controller->retired_group_key_remove[index])
			generation = controller->retired_group_key_generation[index];
		if (generation != 0U) {
			memset(command, 0, sizeof(command));
			result = ax211_pci_key_result_errno(
			    intel_ax211_key_remove_encode(
			    controller->connection_generation, generation,
			    INTEL_AX211_KEY_GROUP_KEY, index, command));
			if (result == 0)
				result = ax211_pci_key_command(controller, command,
				    deadline_ticks);
			intel_ax211_key_command_scrub(command);
		}
	}
	if (result == 0) {
		generation = controller->keys.active_pairwise;
		if (generation == 0U && controller->staged_pairwise_key.valid &&
		    controller->staged_pairwise_key.programmed)
			generation = controller->staged_pairwise_key.request.
			    key_generation;
		if (generation != 0U) {
			memset(command, 0, sizeof(command));
			result = ax211_pci_key_result_errno(
			    intel_ax211_key_remove_encode(
			    controller->connection_generation, generation,
			    INTEL_AX211_KEY_PAIRWISE, 0U, command));
			if (result == 0)
				result = ax211_pci_key_command(controller, command,
				    deadline_ticks);
			intel_ax211_key_command_scrub(command);
		}
	}
	return result;
}

/* Issues one exact API89 key command; caller scrubs its payload. */
static int
ax211_pci_key_command(
	struct ax211_pci_controller *controller,
	const uint8_t *payload,
	uint64_t deadline_ticks)
{
	struct intel_ax211_command_request request;
	uint8_t response[4U];
	size_t response_length;
	int result;

	if (controller == NULL || payload == NULL)
		return EINVAL;
	memset(&request, 0, sizeof(request));
	request.command.group = INTEL_AX211_KEY_GROUP;
	request.command.opcode = INTEL_AX211_KEY_OPCODE;
	/* SEC_KEY layout is v1 in the command table; API89 wide headers use v0. */
	request.command.version = INTEL_AX211_KEY_WIRE_VERSION;
	request.payload = payload;
	request.payload_length = INTEL_AX211_KEY_COMMAND_SIZE;
	request.response_version = INTEL_AX211_KEY_RESPONSE_VERSION;
	request.minimum_response_length = 0U;
	request.maximum_response_length = sizeof(response);
	memset(response, 0, sizeof(response));
	response_length = 0U;
	result = ax211_pci_direct_command(controller, &request,
	    deadline_ticks, response, sizeof(response), &response_length,
	    NULL, NULL);
	ax211_pci_scrub(response, sizeof(response));
	return result;
}

/* Retains one replacement secret only until its checked activation barrier. */
static int
ax211_pci_staged_key_store(
	struct ax211_pci_staged_key *staged,
	const struct intel_ax211_key_request *request)
{
	if (staged == NULL || request == NULL)
		return EINVAL;
	if (staged->valid) {
		if (staged->request.connection_generation ==
		    request->connection_generation &&
		    staged->request.key_generation == request->key_generation &&
		    staged->request.receive_packet_number ==
		    request->receive_packet_number &&
		    staged->request.kind == request->kind &&
		    staged->request.key_index == request->key_index &&
		    (staged->programmed || memcmp(staged->request.key,
		    request->key, INTEL_AX211_KEY_BYTES) == 0))
			return 0;
		return EBUSY;
	}
	staged->request = *request;
	staged->valid = 1U;
	staged->programmed = 0U;
	return 0;
}

/* Marks the key hardware-owned before crossing the uncertain command edge. */
static int
ax211_pci_staged_key_program(
	struct ax211_pci_controller *controller,
	struct ax211_pci_staged_key *staged,
	uint64_t deadline_ticks)
{
	uint8_t command[INTEL_AX211_KEY_COMMAND_SIZE];
	int result;

	if (controller == NULL || staged == NULL || !staged->valid)
		return EINVAL;
	if (staged->programmed)
		return 0;
	memset(command, 0, sizeof(command));
	result = ax211_pci_key_result_errno(
	    intel_ax211_key_add_encode(&staged->request, command));
	if (result == 0) {
		staged->programmed = 1U;
		result = ax211_pci_key_command(controller, command, deadline_ticks);
		/* Once ADD crossed the command transport boundary it is not
		 * retryable: a timeout cannot prove whether firmware installed it.
		 * Erase the controller-owned plaintext even if the subsequent global
		 * fail-closed reset also fails and leaves this object quarantined. */
		ax211_pci_scrub(staged->request.key,
		    sizeof(staged->request.key));
	}
	intel_ax211_key_command_scrub(command);
	return result;
}

static void
ax211_pci_staged_key_clear(
	struct ax211_pci_staged_key *staged)
{
	if (staged != NULL)
		ax211_pci_scrub(staged, sizeof(*staged));
}

static int
ax211_pci_keys_have_active(
	const struct ax211_pci_controller *controller)
{
	uint8_t index;

	if (controller == NULL || !controller->keys_initialized)
		return 0;
	if (controller->keys.active_pairwise != 0U)
		return 1;
	for (index = 0U; index < INTEL_AX211_KEY_INDEX_LIMIT; index++) {
		if (controller->keys.active_group[index] != 0U)
			return 1;
	}
	return 0;
}

static int
ax211_pci_key_request_address_valid(
	const struct ax211_pci_controller *controller,
	const struct wlan_radio_key_request *request)
{
	static const uint8_t broadcast[6] = {
		0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU
	};

	if (controller == NULL || request == NULL ||
	    !controller->selected_bss_valid)
		return 0;
	if (request->kind == WLAN_RADIO_KEY_PAIRWISE)
		return request->key_index == 0U && memcmp(request->address,
		    controller->selected_metadata.bssid, 6U) == 0;
	return request->kind == WLAN_RADIO_KEY_GROUP &&
	    memcmp(request->address, broadcast, sizeof(broadcast)) == 0;
}

/* An uncertain key command is recoverable only after a global DMA reset. */
static int
ax211_pci_key_fail_closed(
	struct ax211_pci_controller *controller,
	int error)
{
	int stop_error;

	if (controller == NULL)
		return error != 0 ? error : EIO;
	hal_printf("intel-ax211: fail-closed key/association path error=%d "
	    "phase=%u step=%u resources=%08x\n", error,
	    controller->association.phase, controller->association.step,
	    controller->association.resources);
	stop_error = ax211_pci_session_stop(controller);
	controller->quarantined = 1U;
	if (stop_error != 0)
		return stop_error;
	return error != 0 ? error : EIO;
}

static int
ax211_pci_tx_submit(
	struct ax211_pci_controller *controller,
	const struct intel_ax211_tx_request *request,
	uint64_t deadline_ticks,
	int report_completion)
{
	struct intel_ax211_tx_ring_handle handle;
	uint64_t deadline_us;
	uint64_t now_us;
	uint64_t timeout_us;
	int result;

	if (controller == NULL || request == NULL ||
	    (report_completion != 0 && report_completion != 1) ||
	    !controller->tx_ring_allocated)
		return EINVAL;
	result = ax211_pci_clock_us(controller, &now_us);
	if (result == 0)
		result = ax211_pci_command_timeout(controller, deadline_ticks,
		    now_us, &timeout_us, &deadline_us);
	if (result != 0)
		return result;
	(void)deadline_us;
	memset(&handle, 0, sizeof(handle));
	result = intel_ax211_tx_ring_submit(&controller->tx_ring, request,
	    now_us, timeout_us, &handle);
	if (result != INTEL_AX211_TX_RING_OK) {
		if (result == INTEL_AX211_TX_RING_KICK_FAILED ||
		    result == INTEL_AX211_TX_RING_POISONED ||
		    result == INTEL_AX211_TX_RING_BARRIER_REQUIRED)
			ax211_pci_recovery_latch_locked(controller, EIO);
		return ax211_pci_tx_ring_result_errno(result);
	}
	controller->tx_report_completion[handle.index] =
	    report_completion != 0 ? 1U : 0U;
	return 0;
}

/* Retires an exact TX response before publishing its common completion. */
static int
ax211_pci_tx_dispatch(
	struct ax211_pci_controller *controller,
	const struct intel_ax211_protocol_message *message)
{
	struct intel_ax211_tx_ring_retired retired;
	struct intel_ax211_protocol_message tx_message;
	struct wlan_station *station;
	unsigned report_completion;
	int report_result;
	int result;

	if (controller == NULL || message == NULL)
		return EINVAL;
	if (!controller->tx_ring_allocated)
		return 0;
	memset(&retired, 0, sizeof(retired));
	tx_message = *message;
	tx_message.queue &= 0x7fU;
	result = intel_ax211_tx_ring_complete(&controller->tx_ring, &tx_message,
	    &retired);
	if (result == INTEL_AX211_TX_RING_DUPLICATE ||
	    result == INTEL_AX211_TX_RING_STALE ||
	    result == INTEL_AX211_TX_RING_NOT_READY)
		return 0;
	if (result != INTEL_AX211_TX_RING_OK &&
	    result != INTEL_AX211_TX_RING_TX_FAILED)
		return EIO;
	report_completion = controller->tx_report_completion[
	    retired.handle.index];
	controller->tx_report_completion[retired.handle.index] = 0U;
	station = NULL;
	if (!report_completion)
		return 0;
	if (result == INTEL_AX211_TX_RING_TX_FAILED)
		hal_printf("intel-ax211: TX failed generation=%u cookie=%u "
		    "acknowledged=%u failure=%u/%u\n",
		    (unsigned)retired.handle.connection_generation,
		    (unsigned)retired.handle.cookie, retired.acknowledged,
		    retired.failure_rts, retired.failure_frame);
	report_result = ax211_pci_operation_enter_locked(controller, &station);
	if (report_result == ENODEV)
		return 0;
	if (report_result != 0)
		return EIO;
	/* Completion can advance WPA and synchronously enter another radio op. */
	mutex_unlock(&controller->lifecycle_lock);
	report_result = wlan_station_report_tx_complete(station,
	    retired.handle.connection_generation, retired.handle.cookie,
	    result == INTEL_AX211_TX_RING_OK ? 1 : 0,
	    result == INTEL_AX211_TX_RING_OK ? 0 : EIO);
	mutex_lock(&controller->lifecycle_lock);
	ax211_pci_operation_leave_locked(controller);
	return report_result == 0 || report_result == ESTALE ||
	    report_result == ENODEV ? 0 : EIO;
}

/* Fails one uncertain oldest TX and resets the complete hardware epoch. */
static int
ax211_pci_tx_timeout_check(
	struct ax211_pci_controller *controller,
	uint64_t now_us)
{
	struct intel_ax211_tx_ring_handle handle;
	struct wlan_station *station;
	unsigned report_completion;
	int report_result;
	int result;

	if (controller == NULL || !controller->tx_ring_allocated ||
	    !controller->tx_ring.enabled || controller->tx_ring.pending_count == 0U)
		return 0;
	memset(&handle, 0, sizeof(handle));
	result = intel_ax211_tx_ring_timeout_oldest(&controller->tx_ring,
	    now_us, &handle);
	if (result == INTEL_AX211_TX_RING_PENDING ||
	    result == INTEL_AX211_TX_RING_NOT_READY)
		return 0;
	if (result != INTEL_AX211_TX_RING_TIMEOUT)
		return EIO;
	hal_printf("intel-ax211: TX completion timeout generation=%u cookie=%u\n",
	    (unsigned)handle.connection_generation, (unsigned)handle.cookie);
	report_completion = controller->tx_report_completion[handle.index];
	controller->tx_report_completion[handle.index] = 0U;
	station = NULL;
	report_result = 0;
	if (report_completion &&
	    ax211_pci_operation_enter_locked(controller, &station) == 0) {
		mutex_unlock(&controller->lifecycle_lock);
		report_result = wlan_station_report_tx_complete(station,
		    handle.connection_generation, handle.cookie, 0, ETIMEDOUT);
		mutex_lock(&controller->lifecycle_lock);
		ax211_pci_operation_leave_locked(controller);
	}
	return report_result == 0 || report_result == ESTALE ||
	    report_result == ENODEV ? ETIMEDOUT : EIO;
}

/* Classifies one copied MPDU against the current connection and key epoch. */
static int
ax211_pci_connection_rx_dispatch(
	struct ax211_pci_controller *controller,
	const struct intel_ax211_rx_mpdu *mpdu,
	uint16_t frame_control)
{
	struct wlan_radio_rx_frame report;
	struct wlan_station *station;
	enum intel_ax211_key_kind kind;
	uint64_t key_generation;
	uint8_t key_index;
	unsigned type;
	int result;

	if (controller == NULL || mpdu == NULL || mpdu->frame == NULL ||
	    controller->connection_generation == 0U)
		return EINVAL;
	type = frame_control & 0x000cU;
	if (type != 0U && type != 0x0008U)
		return 0;
	memset(&report, 0, sizeof(report));
	report.generation = controller->connection_generation;
	report.frame = mpdu->frame;
	report.length = mpdu->length;
	report.rssi_dbm = mpdu->rssi_dbm;
	report.channel = mpdu->channel;
	if (mpdu->cipher == INTEL_AX211_RX_CIPHER_CCMP) {
		if (!controller->keys_initialized || mpdu->length < 10U ||
		    !mpdu->decrypted)
			return 0;
		kind = (mpdu->frame[4U] & 1U) != 0U ?
		    INTEL_AX211_KEY_GROUP_KEY : INTEL_AX211_KEY_PAIRWISE;
		key_index = kind == INTEL_AX211_KEY_PAIRWISE ? 0U :
		    mpdu->key_index;
		result = intel_ax211_key_state_rx_generation(&controller->keys,
		    controller->connection_generation, kind, key_index,
		    controller->hardware_epoch, &key_generation);
		if (result != INTEL_AX211_KEY_OK)
			return 0;
		report.key_generation = key_generation;
		report.packet_number = mpdu->packet_number;
		report.cipher = WLAN_RADIO_CIPHER_CCMP;
		report.decrypted = 1U;
		report.key_index = key_index;
	} else if (mpdu->cipher != INTEL_AX211_RX_CIPHER_NONE)
		return 0;
	station = NULL;
	result = ax211_pci_operation_enter_locked(controller, &station);
	if (result == ENODEV)
		return 0;
	if (result != 0)
		return EIO;
	/* Frame ingestion may advance WPA and synchronously enter a radio op. */
	mutex_unlock(&controller->lifecycle_lock);
	(void)wlan_station_report_frame(station, &report);
	mutex_lock(&controller->lifecycle_lock);
	ax211_pci_operation_leave_locked(controller);
	/* Once the private descriptor and crypto envelope have validated, every
	 * common result is a per-frame policy/drop/control outcome.  It must not
	 * quarantine otherwise healthy DMA or firmware state. */
	return 0;
}

/* Performs a stopped NVM pass followed by one retained runtime pass. */
static int
ax211_net_open(
	struct net_device *device)
{
	struct ax211_pci_controller *controller;
	struct intel_ax211_protocol_command_table table;
	struct intel_ax211_protocol_nvm nvm;
	struct drv_dma_device *dma_device;
	struct wlan_station *station;
	const char *stage;
	unsigned long enabled;
	int boot_result;
	int runtime_result;
	int cleanup_error;
	int error;
	int ltr_enabled;

	if (device == NULL)
		return ENODEV;
	controller = device->driver_data;
	if (controller == NULL)
		return ENODEV;
	mutex_lock(&controller->lifecycle_lock);
	if (controller->detaching || !controller->ready ||
	    controller->quarantined || !controller->station_attached)
		error = ENODEV;
	else if (controller->runtime_active)
		error = 0;
	else if (controller->operations_active != 0U)
		error = EBUSY;
	else {
		controller->operation_admission_open = 0U;
		dma_device = drv_pci_device_dma(controller->device);
		memset(&table, 0, sizeof(table));
		memset(&nvm, 0, sizeof(nvm));
		boot_result = INTEL_AX211_BOOT_OK;
		runtime_result = INTEL_AX211_RUNTIME_START_OK;
		stage = "dma-coherency";
		error = dma_device == NULL ||
		    !drv_dma_device_is_coherent(dma_device) ? EIO : 0;
		if (error == 0) {
			stage = "boot-init";
			error = intel_ax211_boot_init(&controller->boot,
			    &ax211_runtime_start_ops.boot, controller, dma_device,
			    &controller->mmio, &controller->transport,
			    (uint16_t)controller->hardware_revision,
			    AX211_CSR_HW_RF_TYPE_GF, controller->hardware_epoch) ==
			    INTEL_AX211_BOOT_OK ? 0 : EIO;
		}
		if (error == 0) {
			stage = "firmware-boot";
			controller->boot_initialized = 1U;
			boot_result = intel_ax211_boot_run(&controller->boot, &nvm);
			error = boot_result == INTEL_AX211_BOOT_OK ? 0 : EIO;
			controller->hardware_epoch = controller->boot.generation;
			if (boot_result == INTEL_AX211_BOOT_OK &&
			    controller->mmio.master_disable_timed_out)
				hal_printf("intel-ax211: master-disable indication "
				    "timed out; PCI bus master disabled and reset "
				    "completed\n");
			if (error != 0)
				hal_printf("intel-ax211: first boot failed result=%d "
				    "last=%u state=%u generation=%u files=%u dma=%u "
				    "exposed=%u hardware=%u transport=%u alive=%u "
				    "pnvm=%u init=%u commands=%u nvm=%u\n",
				    boot_result, controller->boot.last_error,
				    controller->boot.state, controller->boot.generation,
				    controller->boot.files_loaded,
				    controller->boot.dma_prepared,
				    controller->boot.dma_exposed,
				    controller->boot.hardware_touched,
				    controller->boot.transport_bound,
				    controller->boot.alive_accepted,
				    controller->boot.pnvm_accepted,
				    controller->boot.init_accepted,
				    controller->boot.command_table_valid,
				    controller->boot.nvm_valid);
			if (error != 0 && controller->last_receive_length != 0U)
				hal_printf("intel-ax211: last rx length=%u opcode=%02x "
				    "group=%02x index=%02x queue=%02x version=%u\n",
				    (unsigned)controller->last_receive_length,
				    controller->last_receive_header[4U],
				    controller->last_receive_header[5U],
				    controller->last_receive_header[6U],
				    controller->last_receive_header[7U],
				    controller->last_receive_version);
		}
		if (error == 0) {
			stage = "command-table";
			error = intel_ax211_boot_command_table(&controller->boot,
			    &table) == INTEL_AX211_BOOT_OK ? 0 : EIO;
		}
		ltr_enabled = 0;
		if (error == 0) {
			stage = "ltr";
			error = ax211_pci_ltr_enabled(controller, &ltr_enabled);
		}
		if (error == 0) {
			stage = "runtime-init";
			error = intel_ax211_runtime_start_init(
			    &controller->runtime_start, &ax211_runtime_start_ops,
			    controller, dma_device, &controller->mmio,
			    &controller->transport,
			    (uint16_t)controller->hardware_revision,
			    AX211_CSR_HW_RF_TYPE_GF, &table, &nvm, ltr_enabled,
			    controller->hardware_epoch) ==
			    INTEL_AX211_RUNTIME_START_OK ? 0 : EIO;
		}
		if (error == 0) {
			stage = "runtime-start";
			controller->runtime_initialized = 1U;
			runtime_result = intel_ax211_runtime_start_run(
			    &controller->runtime_start);
			error = runtime_result == INTEL_AX211_RUNTIME_START_OK ?
			    0 : EIO;
			controller->hardware_epoch =
			    controller->runtime_start.generation;
			if (error != 0)
				hal_printf("intel-ax211: runtime start failed result=%d "
				    "last=%u state=%u generation=%u files=%u dma=%u "
				    "exposed=%u hardware=%u transport=%u alive=%u "
				    "pnvm=%u init=%u profile=%u mcc=%u nic=%u\n",
				    runtime_result,
				    controller->runtime_start.last_error,
				    controller->runtime_start.state,
				    controller->runtime_start.generation,
				    controller->runtime_start.files_loaded,
				    controller->runtime_start.dma_prepared,
				    controller->runtime_start.dma_exposed,
				    controller->runtime_start.hardware_touched,
				    controller->runtime_start.transport_bound,
				    controller->runtime_start.alive_accepted,
				    controller->runtime_start.pnvm_accepted,
				    controller->runtime_start.init_accepted,
				    controller->runtime_start.profile_valid,
				    controller->runtime_start.mcc_valid,
				    controller->runtime_start.nic_locked);
			if (error != 0 && controller->last_receive_length != 0U)
				hal_printf("intel-ax211: last runtime rx length=%u "
				    "opcode=%02x group=%02x index=%02x queue=%02x "
				    "version=%u\n",
				    (unsigned)controller->last_receive_length,
				    controller->last_receive_header[4U],
				    controller->last_receive_header[5U],
				    controller->last_receive_header[6U],
				    controller->last_receive_header[7U],
				    controller->last_receive_version);
		}
		if (error == 0) {
			stage = "api89-validation";
			error = intel_ax211_assoc_api89_validate(&table) ==
			    INTEL_AX211_ASSOC_OK &&
			    intel_ax211_key_api89_validate(&table) ==
			    INTEL_AX211_KEY_OK &&
			    intel_ax211_tx_ring_api89_validate(&table) ==
			    INTEL_AX211_TX_RING_OK ? 0 : ENOTSUP;
		}
		if (error == 0) {
			stage = "tx-ring";
			error = intel_ax211_tx_ring_allocate(dma_device,
			    &ax211_tx_ring_ops, controller, &controller->tx_ring) ==
			    INTEL_AX211_TX_RING_OK ? 0 : ENOMEM;
			if (error == 0) {
				controller->tx_ring_allocated = 1U;
				controller->next_management_cookie = UINT64_MAX;
			}
		}
		if (error == 0) {
			stage = "scan-init";
			error = ax211_pci_scan_initialize(controller);
		}
		if (error == 0) {
			enabled = spin_lock_irqsave(&controller->interrupt_lock);
			controller->runtime_active = 1U;
			spin_unlock_irqrestore(&controller->interrupt_lock, enabled);
		}
		station = controller->station;
		if (error == 0) {
			stage = "station-open";
			error = station == NULL ? ENODEV :
			    wlan_station_open(station);
		}
		if (error == 0)
			controller->operation_admission_open = 1U;
		else {
			hal_printf("intel-ax211: open failed stage=%s error=%d\n",
			    stage, error);
			cleanup_error = ax211_pci_session_stop(controller);
			if (cleanup_error != 0) {
				controller->quarantined = 1U;
				error = cleanup_error;
			}
		}
		ax211_pci_scrub(&table, sizeof(table));
		ax211_pci_scrub(&nvm, sizeof(nvm));
	}
	mutex_unlock(&controller->lifecycle_lock);
	return error;
}

static void
ax211_net_close(
	struct net_device *device)
{
	struct ax211_pci_controller *controller;
	struct wlan_station *station;
	uint64_t deadline;
	int stop_error;
	int error;

	if (device == NULL)
		return;
	(void)net_device_set_carrier(device, 0);
	controller = device->driver_data;
	if (controller == NULL)
		return;
	mutex_lock(&controller->lifecycle_lock);
	controller->operation_admission_open = 0U;
	deadline = ax211_pci_lifecycle_deadline();
	error = ax211_pci_operations_join_locked(controller, deadline);
	station = error == 0 && controller->station_attached ?
	    controller->station : NULL;
	mutex_unlock(&controller->lifecycle_lock);
	if (error == 0 && station != NULL)
		error = ax211_pci_station_close_wait(station, deadline);
	mutex_lock(&controller->lifecycle_lock);
	if (error == 0 || error == ENODEV) {
		stop_error = ax211_pci_session_stop(controller);
		if (stop_error != 0)
			error = stop_error;
	}
	if (error != 0 && error != ENODEV) {
		controller->quarantined = 1U;
		hal_printf("intel-ax211: checked close failed (%d)\n", error);
	}
	mutex_unlock(&controller->lifecycle_lock);
}

/* Keeps Ethernet conversion and CCMP framing in the common WLAN station. */
static int
ax211_net_transmit(
	struct net_device *device,
	struct packet_buf *packet)
{
	struct ax211_pci_controller *controller;
	struct wlan_station *station;
	int error;

	if (packet == NULL)
		return EINVAL;
	if (device == NULL) {
		packet_buf_free(packet);
		return ENODEV;
	}
	controller = device->driver_data;
	if (controller == NULL) {
		packet_buf_free(packet);
		return ENODEV;
	}
	mutex_lock(&controller->lifecycle_lock);
	if (!controller->runtime_active || controller->quarantined ||
	    !controller->operation_admission_open ||
	    !controller->station_attached) {
		station = NULL;
		error = controller->ready ? ENETDOWN : ENODEV;
	} else
		error = ax211_pci_operation_enter_locked(controller, &station);
	mutex_unlock(&controller->lifecycle_lock);
	if (station == NULL) {
		packet_buf_free(packet);
		return error;
	}
	error = wlan_station_transmit(station, packet);
	mutex_lock(&controller->lifecycle_lock);
	ax211_pci_operation_leave_locked(controller);
	mutex_unlock(&controller->lifecycle_lock);
	return error;
}

static unsigned
ax211_net_poll_receive(
	struct net_device *device,
	unsigned budget)
{
	struct ax211_pci_controller *controller;
	struct intel_ax211_boot_received_event event;
	uint64_t now;
	unsigned count;
	unsigned reschedule;
	unsigned long enabled;
	int dispatch_result;
	int fatal_error;
	int result;

	if (device == NULL || budget == 0U)
		return 0U;
	controller = device->driver_data;
	if (controller == NULL)
		return 0U;
	count = 0U;
	mutex_lock(&controller->lifecycle_lock);
	if (controller->poll_active) {
		controller->poll_reschedule = 1U;
		mutex_unlock(&controller->lifecycle_lock);
		return 0U;
	}
	if (!controller->runtime_active || controller->quarantined) {
		mutex_unlock(&controller->lifecycle_lock);
		return 0U;
	}
	controller->poll_active = 1U;
	fatal_error = controller->recovery_pending ?
	    controller->recovery_error : 0;
	while (fatal_error == 0 && count < budget && controller->runtime_active &&
	    !controller->quarantined) {
		if (controller->deferred_event_count != 0U) {
			result = ax211_pci_deferred_event_drain_one(controller);
			if (result == EBUSY)
				break;
			if (result != 0) {
				fatal_error = result;
				break;
			}
			count++;
			continue;
		}
		if (ax211_pci_clock_us(controller, &now) != 0) {
			fatal_error = EIO;
			break;
		}
		memset(&event, 0, sizeof(event));
		result = ax211_pci_receive_event(controller, now,
		    controller->runtime_event,
		    sizeof(controller->runtime_event), &event);
		if (result == INTEL_AX211_BOOT_RECEIVE_TIMEOUT)
			break;
		if (result != INTEL_AX211_BOOT_RECEIVE_OK) {
			hal_printf("intel-ax211: runtime receive failed result=%d "
			    "scan-phase=%u pending=%u\n", result,
			    controller->scan_session.phase,
			    (unsigned)intel_ax211_command_pending_count(
			    &controller->runtime_start.commands));
			fatal_error = EIO;
			break;
		}
		dispatch_result = ax211_pci_runtime_event_dispatch(controller,
		    controller->runtime_event, event.length, &event);
		ax211_pci_scrub(controller->runtime_event, event.length);
		if (dispatch_result != 0) {
			hal_printf("intel-ax211: runtime dispatch failed error=%d "
			    "length=%u opcode=%02x group=%02x index=%02x queue=%02x "
			    "version=%u scan-phase=%u\n", dispatch_result,
			    (unsigned)event.length,
			    controller->last_receive_header[4U],
			    controller->last_receive_header[5U],
			    controller->last_receive_header[6U],
			    controller->last_receive_header[7U],
			    controller->last_receive_version,
			    controller->scan_session.phase);
			fatal_error = dispatch_result;
			break;
		}
		count++;
	}
	if (fatal_error == 0 && controller->runtime_active &&
	    !controller->quarantined) {
		result = ax211_pci_clock_us(controller, &now);
		if (result == 0)
			result = ax211_pci_tx_timeout_check(controller, now);
		if (result != 0)
			fatal_error = result;
	}
	if (fatal_error != 0) {
		if (!controller->recovery_pending)
			ax211_pci_recovery_latch_locked(controller, fatal_error);
		(void)ax211_pci_recovery_run_locked(controller);
	}
	enabled = spin_lock_irqsave(&controller->interrupt_lock);
	reschedule = (controller->poll_reschedule ||
	    controller->recovery_pending ||
	    controller->deferred_event_count != 0U || count >= budget ||
	    controller->irq_latched) && controller->receive_enabled &&
	    controller->runtime_active && !controller->quarantined;
	spin_unlock_irqrestore(&controller->interrupt_lock, enabled);
	controller->poll_active = 0U;
	controller->poll_reschedule = 0U;
	mutex_unlock(&controller->lifecycle_lock);
	if (reschedule)
		net_device_schedule_poll(device);
	return count;
}

static int
ax211_net_ioctl(
	struct net_device *device,
	unsigned long request,
	void *argument)
{
	struct ax211_pci_controller *controller;
	int result;

	if (device == NULL)
		return ENODEV;
	controller = device->driver_data;
	if (controller == NULL)
		return ENODEV;
	mutex_lock(&controller->lifecycle_lock);
	if (!controller->station_attached || controller->detaching)
		result = ENODEV;
	else if (!controller->runtime_active || controller->quarantined ||
	    controller->recovery_pending || controller->recovery_running)
		result = ENETDOWN;
	else
		result = 0;
	if (request == SIOCSWLANCONNECT && result != 0)
		hal_printf("intel-ax211: connect ioctl rejected result=%d runtime=%u "
		    "quarantined=%u recovery=%u/%u attached=%u detaching=%u\n",
		    result, controller->runtime_active, controller->quarantined,
		    controller->recovery_pending, controller->recovery_running,
		    controller->station_attached, controller->detaching);
	mutex_unlock(&controller->lifecycle_lock);
	return result == 0 ? wlan_station_ioctl(device, request, argument) :
	    result;
}

/* Frees the controller only after the last removed net-device reference. */
static void
ax211_net_release(
	void *driver_data)
{
	struct ax211_pci_controller *controller;

	controller = driver_data;
	if (controller == NULL)
		return;
	ax211_pci_scrub(controller, sizeof(*controller));
	hal_free(controller);
}

/* Starts one finite asynchronous firmware scan on the selected channel. */
static int
ax211_radio_scan_channel_start(
	void *context,
	uint64_t generation,
	uint32_t step_index,
	uint32_t channel,
	uint64_t deadline)
{
	struct ax211_pci_controller *controller;
	uint64_t now;
	int result;

	controller = context;
	if (controller == NULL)
		return ENODEV;
	if (generation == 0U || channel > UINT8_MAX ||
	    deadline == 0U)
		return EINVAL;

	mutex_lock(&controller->lifecycle_lock);
	if (!controller->ready || controller->detaching ||
	    controller->quarantined || controller->recovery_pending ||
	    controller->recovery_running || !controller->runtime_active ||
	    !controller->scan_initialized || !controller->station_attached ||
	    controller->station == NULL) {
		result = controller->ready ? ENETDOWN : ENODEV;
		mutex_unlock(&controller->lifecycle_lock);
		return result;
	}
	if (step_index >=
	    controller->scan_session.full_profile.channel_count ||
	    !ax211_pci_scan_channel_present(controller, (uint8_t)channel)) {
		mutex_unlock(&controller->lifecycle_lock);
		return EINVAL;
	}

	result = ax211_pci_clock_us(controller, &now);
	/* Common starts a fresh staging set for each scan generation.  Preserve
	 * the published set until this entire generation completes successfully. */
	if (result == 0 && (!controller->bss_staging_initialized ||
	    controller->bss_staging_generation != generation)) {
		ax211_pci_bss_staging_discard(controller);
		result = intel_ax211_bss_cache_init(&controller->bss_staging_cache,
		    controller->hardware_epoch) ==
		    INTEL_AX211_BSS_OK ? 0 : EIO;
		if (result == 0) {
			controller->bss_staging_initialized = 1U;
			controller->bss_staging_generation = generation;
		}
	}
	if (result == 0)
		result = intel_ax211_scan_session_begin_channel(
		    &controller->scan_session, generation, (uint8_t)channel, now);
	else
		result = INTEL_AX211_SCAN_SESSION_FAILED;
	if (result == INTEL_AX211_SCAN_SESSION_OK)
		controller->scan_step_index = step_index;
	else
		ax211_pci_bss_staging_discard(controller);
	mutex_unlock(&controller->lifecycle_lock);
	return ax211_pci_scan_result_errno(result);
}

/* Retires the matching asynchronous scan or requests a bounded retry. */
static int
ax211_radio_scan_stop(
	void *context,
	uint64_t generation)
{
	struct ax211_pci_controller *controller;
	uint64_t now;
	uint8_t phase;
	int cleanup_error;
	int error;
	int result;

	controller = context;
	if (controller == NULL)
		return ENODEV;
	if (generation == 0U)
		return EINVAL;

	mutex_lock(&controller->lifecycle_lock);
	if (controller->recovery_pending && !controller->recovery_running) {
		mutex_unlock(&controller->lifecycle_lock);
		return ENETDOWN;
	}
	if (!controller->runtime_active || !controller->scan_initialized) {
		mutex_unlock(&controller->lifecycle_lock);
		return 0;
	}
	if (controller->scan_session.common_generation == 0U) {
		mutex_unlock(&controller->lifecycle_lock);
		return 0;
	}
	if (controller->scan_session.common_generation != generation) {
		mutex_unlock(&controller->lifecycle_lock);
		return ESTALE;
	}

	phase = controller->scan_session.phase;
	if (phase == INTEL_AX211_SCAN_SESSION_IDLE) {
		mutex_unlock(&controller->lifecycle_lock);
		return 0;
	}
	if (phase == INTEL_AX211_SCAN_SESSION_TERMINAL)
		result = controller->scan_session.terminal_result;
	else {
		error = ax211_pci_clock_us(controller, &now);
		if (error != 0)
			result = INTEL_AX211_SCAN_SESSION_FAILED;
		else
			result = intel_ax211_scan_session_expire(
			    &controller->scan_session, now);
		phase = controller->scan_session.phase;
		if (result == INTEL_AX211_SCAN_SESSION_OK &&
		    phase == INTEL_AX211_SCAN_SESSION_RUNNING)
			result = intel_ax211_scan_session_abort(
			    &controller->scan_session, generation, now);
	}
	if (result == INTEL_AX211_SCAN_SESSION_OK ||
	    result == INTEL_AX211_SCAN_SESSION_BUSY) {
		mutex_unlock(&controller->lifecycle_lock);
		return EBUSY;
	}
	if (result == INTEL_AX211_SCAN_SESSION_COMPLETE) {
		if (controller->scan_step_index + 1U ==
		    controller->scan_session.full_profile.channel_count) {
			if (!controller->bss_staging_initialized &&
			    controller->bss_published_generation == generation)
				error = 0;
			else
				error = ax211_pci_bss_staging_publish(controller,
				    generation);
		} else {
			/* One common generation spans every channel.  Keep private
			 * metadata from earlier channels until the final step publishes
			 * the complete set; association may select any of those BSSes. */
			error = 0;
		}
		mutex_unlock(&controller->lifecycle_lock);
		return error;
	}
	if (result == INTEL_AX211_SCAN_SESSION_ABORTED ||
	    result == INTEL_AX211_SCAN_SESSION_DUPLICATE) {
		ax211_pci_bss_staging_discard(controller);
		mutex_unlock(&controller->lifecycle_lock);
		return 0;
	}

	/* An expired or failed command leaves firmware ownership ambiguous.
	 * Stop the entire epoch so a successful return can never strand a
	 * producer behind the common WLAN barrier. */
	error = ax211_pci_scan_result_errno(result);
	hal_printf("intel-ax211: scan stop generation=%u phase=%u result=%d "
	    "error=%d\n", (unsigned)generation, phase, result, error);
	ax211_pci_bss_staging_discard(controller);
	cleanup_error = ax211_pci_session_stop(controller);
	controller->quarantined = 1U;
	if (cleanup_error != 0)
		error = cleanup_error;
	if (error == 0)
		error = EIO;
	mutex_unlock(&controller->lifecycle_lock);
	return error;
}

static int
ax211_radio_connect_start(
	void *context,
	uint64_t generation,
	const struct wlan_bss_record *bss,
	uint64_t deadline)
{
	struct ax211_pci_controller *controller;
	struct intel_ax211_assoc_profile profile;
	struct intel_ax211_protocol_command_table table;
	uint64_t now;
	int result;

	controller = context;
	if (controller == NULL)
		return ENODEV;
	if (generation == 0U || bss == NULL || deadline == 0U ||
	    clock_ticks() >= deadline)
		return EINVAL;
	mutex_lock(&controller->lifecycle_lock);
	if (!controller->runtime_active || controller->quarantined ||
	    controller->recovery_pending || controller->recovery_running ||
	    !controller->tx_ring_allocated || controller->association_initialized ||
	    controller->connection_generation != 0U) {
		hal_printf("intel-ax211: connect admission rejected runtime=%u "
		    "quarantined=%u recovery=%u/%u tx-ring=%u association=%u "
		    "generation=%u\n", controller->runtime_active,
		    controller->quarantined, controller->recovery_pending,
		    controller->recovery_running, controller->tx_ring_allocated,
		    controller->association_initialized,
		    (unsigned)controller->connection_generation);
		result = controller->runtime_active ? EBUSY : ENETDOWN;
		mutex_unlock(&controller->lifecycle_lock);
		return result;
	}
	memset(&profile, 0, sizeof(profile));
	result = ax211_pci_assoc_profile(controller, bss, generation, &profile);
	memset(&table, 0, sizeof(table));
	if (result == 0) {
		table.bytes = controller->runtime_start.command_version_bytes;
		table.count = INTEL_AX211_PROTOCOL_API89_COMMAND_COUNT;
		result = ax211_pci_clock_us(controller, &now);
	}
	if (result == 0) {
		controller->connection_generation = generation;
		controller->control_deadline_ticks = deadline;
		result = intel_ax211_key_state_init(&controller->keys,
		    controller->hardware_epoch, generation);
		if (result == INTEL_AX211_KEY_OK)
			controller->keys_initialized = 1U;
		else
			result = EIO;
	}
	if (result == 0) {
		result = intel_ax211_assoc_begin(&controller->association, &table,
		    &profile, generation, controller->hardware_epoch, now);
		if (result == INTEL_AX211_ASSOC_OK) {
			controller->association_initialized = 1U;
			result = intel_ax211_assoc_drive(&controller->association,
			    &ax211_assoc_ops, controller);
		}
		if (result != INTEL_AX211_ASSOC_AUTH_READY)
			hal_printf("intel-ax211: association drive stopped result=%d "
			    "phase=%u step=%u failure=%d resources=%08x\n", result,
			    controller->association.phase,
			    controller->association.step,
			    controller->association.failure,
			    controller->association.resources);
		result = ax211_pci_assoc_result_errno(result);
	}
	if (result != 0 && controller->association_initialized) {
		if (controller->association.phase == INTEL_AX211_ASSOC_PHASE_IDLE &&
		    !controller->tx_ring.enabled &&
		    controller->tx_ring.pending_count == 0U)
			ax211_pci_connection_clear(controller);
		else
			result = ax211_pci_key_fail_closed(controller, result);
	} else if (result != 0 && controller->connection_generation != 0U)
		ax211_pci_connection_clear(controller);
	if (result != 0)
		hal_printf("intel-ax211: association start failed result=%d "
		    "phase=%u step=%u failure=%d resources=%08x\n", result,
		    controller->association.phase, controller->association.step,
		    controller->association.failure,
		    controller->association.resources);
	else
		hal_printf("intel-ax211: association hardware ready generation=%u\n",
		    (unsigned)generation);
	ax211_pci_scrub(&profile, sizeof(profile));
	ax211_pci_scrub(&table, sizeof(table));
	mutex_unlock(&controller->lifecycle_lock);
	return result;
}

static int
ax211_radio_disconnect(
	void *context,
	uint64_t generation)
{
	struct ax211_pci_controller *controller;
	int result;

	controller = context;
	if (controller == NULL)
		return ENODEV;
	if (generation == 0U)
		return EINVAL;
	mutex_lock(&controller->lifecycle_lock);
	if (controller->recovery_pending && !controller->recovery_running)
		result = ENETDOWN;
	else {
		controller->control_deadline_ticks =
		    ax211_pci_lifecycle_deadline();
		result = ax211_pci_assoc_rollback(controller, generation);
		if (result != 0)
			hal_printf("intel-ax211: association rollback failed "
			    "generation=%u result=%d phase=%u step=%u failure=%d\n",
			    (unsigned)generation, result,
			    controller->association.phase,
			    controller->association.step,
			    controller->association.failure);
		if (result != 0 && controller->runtime_active)
			result = ax211_pci_key_fail_closed(controller, result);
	}
	mutex_unlock(&controller->lifecycle_lock);
	return result;
}

static int
ax211_radio_management_transmit(
	void *context,
	uint64_t generation,
	const uint8_t *frame,
	size_t length,
	uint64_t deadline)
{
	struct ax211_pci_controller *controller;
	struct intel_ax211_tx_request request;
	unsigned attempt;
	int result;

	controller = context;
	if (controller == NULL)
		return ENODEV;
	if (generation == 0U || frame == NULL || length == 0U ||
	    deadline == 0U)
		return EINVAL;
	mutex_lock(&controller->lifecycle_lock);
	if (!controller->runtime_active || controller->recovery_pending ||
	    controller->recovery_running || !controller->tx_ring.enabled ||
	    controller->connection_generation != generation) {
		hal_printf("intel-ax211: management TX rejected runtime=%u "
		    "recovery=%u/%u ring=%u expected-generation=%u generation=%u\n",
		    controller->runtime_active, controller->recovery_pending,
		    controller->recovery_running, controller->tx_ring.enabled,
		    (unsigned)controller->connection_generation,
		    (unsigned)generation);
		result = ENETDOWN;
	} else {
		memset(&request, 0, sizeof(request));
		request.connection_generation = generation;
		request.frame = frame;
		request.length = length;
		request.frame_class = INTEL_AX211_TX_FRAME_MANAGEMENT;
		request.band_5ghz = controller->selected_metadata.channel > 14U;
		result = EEXIST;
		attempt = 0U;
		while (attempt < INTEL_AX211_TX_RING_SLOT_COUNT &&
		    result == EEXIST) {
			if (controller->next_management_cookie == 0U)
				controller->next_management_cookie = UINT64_MAX;
			request.cookie = controller->next_management_cookie--;
			result = ax211_pci_tx_submit(controller, &request,
			    deadline, 0);
			attempt++;
		}
	}
	mutex_unlock(&controller->lifecycle_lock);
	return result;
}

static int
ax211_radio_association_set(
	void *context,
	uint64_t generation,
	const uint8_t bssid[6],
	uint16_t aid,
	uint64_t deadline)
{
	struct ax211_pci_controller *controller;
	struct intel_ax211_assoc_update update;
	uint64_t now;
	int result;

	controller = context;
	if (controller == NULL)
		return ENODEV;
	if (generation == 0U || bssid == NULL || aid == 0U || aid > 2007U ||
	    deadline == 0U || clock_ticks() >= deadline)
		return EINVAL;
	mutex_lock(&controller->lifecycle_lock);
	if (!controller->runtime_active || controller->recovery_pending ||
	    controller->recovery_running ||
	    !controller->association_initialized ||
	    !controller->selected_bss_valid)
		result = ENETDOWN;
	else if (controller->connection_generation != generation ||
	    memcmp(bssid, controller->selected_metadata.bssid, 6U) != 0)
		result = ESTALE;
	else {
		memset(&update, 0, sizeof(update));
		update.association_id = aid;
		update.dtim_period = controller->selected_metadata.dtim_period;
		update.dtim_count = controller->selected_metadata.dtim_count;
		update.beacon_arrive_time =
		    controller->selected_metadata.beacon_arrive_time;
		update.beacon_tsf = controller->selected_metadata.beacon_tsf;
		result = ax211_pci_clock_us(controller, &now);
		if (result == 0) {
			controller->control_deadline_ticks = deadline;
			result = intel_ax211_assoc_begin_update(
			    &controller->association, &update, generation,
			    controller->hardware_epoch, now);
			if (result == INTEL_AX211_ASSOC_OK)
				result = intel_ax211_assoc_drive(
				    &controller->association, &ax211_assoc_ops,
				    controller);
			result = ax211_pci_assoc_result_errno(result);
			if (result == 0 && controller->association.phase !=
			    INTEL_AX211_ASSOC_PHASE_ASSOCIATED)
				result = EIO;
			if (result == 0) {
				result = ax211_pci_mcast_filter_configure(controller,
				    deadline);
				if (result != 0)
					hal_printf("intel-ax211: post-association "
					    "multicast configuration failed (%d)\n",
					    result);
			}
			if (result == 0) {
				result = ax211_pci_mac_power_configure(controller,
				    deadline);
				if (result != 0)
					hal_printf("intel-ax211: post-association "
					    "power configuration failed (%d)\n", result);
			}
			if (result != 0) {
				result = ax211_pci_key_fail_closed(controller, result);
			}
		}
		ax211_pci_scrub(&update, sizeof(update));
	}
	mutex_unlock(&controller->lifecycle_lock);
	return result;
}

static int
ax211_radio_association_clear(
	void *context,
	uint64_t generation,
	uint64_t deadline)
{
	struct ax211_pci_controller *controller;
	int result;

	controller = context;
	if (controller == NULL)
		return ENODEV;
	if (generation == 0U || deadline == 0U)
		return EINVAL;
	mutex_lock(&controller->lifecycle_lock);
	if (controller->recovery_pending && !controller->recovery_running)
		result = ENETDOWN;
	else {
		controller->control_deadline_ticks = deadline;
		result = ax211_pci_assoc_rollback(controller, generation);
		if (result != 0 && controller->runtime_active)
			result = ax211_pci_key_fail_closed(controller, result);
	}
	mutex_unlock(&controller->lifecycle_lock);
	return result;
}

static int
ax211_radio_frame_transmit(
	void *context,
	const struct wlan_radio_tx_request *request)
{
	struct ax211_pci_controller *controller;
	struct intel_ax211_tx_request tx;
	int result;

	controller = context;
	if (controller == NULL)
		return ENODEV;
	if (request == NULL || request->generation == 0U ||
	    request->cookie == 0U || request->frame == NULL ||
	    request->length == 0U || request->deadline_ticks == 0U)
		return EINVAL;
	mutex_lock(&controller->lifecycle_lock);
	if (!controller->runtime_active || controller->recovery_pending ||
	    controller->recovery_running || !controller->tx_ring.enabled)
		result = ENETDOWN;
	else if (controller->connection_generation != request->generation)
		result = ESTALE;
	else {
		memset(&tx, 0, sizeof(tx));
		tx.connection_generation = request->generation;
		tx.cookie = request->cookie;
		tx.key_generation = request->key_generation;
		tx.packet_number = request->packet_number;
		tx.frame = request->frame;
		tx.length = request->length;
		tx.encrypted = request->encrypted;
		tx.key_index = request->key_index;
		tx.band_5ghz = controller->selected_metadata.channel > 14U;
		if (request->frame_class == WLAN_RADIO_FRAME_MANAGEMENT)
			tx.frame_class = INTEL_AX211_TX_FRAME_MANAGEMENT;
		else if (request->frame_class == WLAN_RADIO_FRAME_EAPOL)
			tx.frame_class = INTEL_AX211_TX_FRAME_EAPOL;
		else if (request->frame_class == WLAN_RADIO_FRAME_DATA)
			tx.frame_class = INTEL_AX211_TX_FRAME_DATA;
		else
			result = EINVAL;
		if (request->frame_class == WLAN_RADIO_FRAME_MANAGEMENT ||
		    request->frame_class == WLAN_RADIO_FRAME_EAPOL ||
		    request->frame_class == WLAN_RADIO_FRAME_DATA) {
			result = 0;
			if (request->encrypted) {
				result = controller->keys_initialized ?
				    intel_ax211_key_state_tx_validate(
				    &controller->keys, request->generation,
				    request->key_generation, request->key_index,
				    request->packet_number,
				    controller->hardware_epoch) :
				    INTEL_AX211_KEY_MISSING;
				result = ax211_pci_key_result_errno(result);
			}
			if (result == 0)
				result = ax211_pci_tx_submit(controller, &tx,
				    request->deadline_ticks, 1);
		}
		ax211_pci_scrub(&tx, sizeof(tx));
	}
	mutex_unlock(&controller->lifecycle_lock);
	return result;
}

static int
ax211_radio_key_install(
	void *context,
	const struct wlan_radio_key_request *request)
{
	struct ax211_pci_controller *controller;
	struct ax211_pci_staged_key *staged;
	struct intel_ax211_key_request key;
	uint8_t index;
	uint64_t group_generation;
	int active_before;
	int state_result;
	int result;

	controller = context;
	if (controller == NULL)
		return ENODEV;
	if (request == NULL || request->generation == 0U ||
	    request->key_generation == 0U || request->deadline_ticks == 0U ||
	    clock_ticks() >= request->deadline_ticks ||
	    (request->kind != WLAN_RADIO_KEY_PAIRWISE &&
	    request->kind != WLAN_RADIO_KEY_GROUP) ||
	    request->key_index >= INTEL_AX211_KEY_INDEX_LIMIT ||
	    (request->kind == WLAN_RADIO_KEY_PAIRWISE &&
	    request->key_index != 0U))
		return EINVAL;
	mutex_lock(&controller->lifecycle_lock);
	memset(&key, 0, sizeof(key));
	if (!controller->runtime_active || controller->recovery_pending ||
	    controller->recovery_running || !controller->keys_initialized)
		result = ENETDOWN;
	else if (controller->connection_generation != request->generation)
		result = ESTALE;
	else if (!ax211_pci_key_request_address_valid(controller, request))
		result = EINVAL;
	else {
		key.connection_generation = request->generation;
		key.key_generation = request->key_generation;
		key.receive_packet_number = request->receive_packet_number;
		key.key_index = request->key_index;
		key.kind = request->kind == WLAN_RADIO_KEY_PAIRWISE ?
		    INTEL_AX211_KEY_PAIRWISE : INTEL_AX211_KEY_GROUP_KEY;
		memcpy(key.key, request->key, sizeof(key.key));
		staged = key.kind == INTEL_AX211_KEY_PAIRWISE ?
		    &controller->staged_pairwise_key :
		    &controller->staged_group_key[key.key_index];
		active_before = ax211_pci_keys_have_active(controller);
		if ((key.kind == INTEL_AX211_KEY_PAIRWISE &&
		    controller->keys.active_pairwise == key.key_generation) ||
		    (key.kind == INTEL_AX211_KEY_GROUP_KEY &&
		    controller->keys.active_group[key.key_index] ==
		    key.key_generation))
			result = 0;
		else {
			result = ax211_pci_staged_key_store(staged, &key);
			state_result = result == 0 ?
			    intel_ax211_key_state_installed(&controller->keys, &key,
			    controller->hardware_epoch) : INTEL_AX211_KEY_INVALID;
			if (result == 0 && state_result != INTEL_AX211_KEY_OK &&
			    state_result != INTEL_AX211_KEY_DUPLICATE)
				result = ax211_pci_key_result_errno(state_result);
			/* Initial keys must be live before common sends its first M4.
			 * Replacement keys remain private until keys_activate(). */
			if (result == 0 && !active_before)
				result = ax211_pci_staged_key_program(controller, staged,
				    request->deadline_ticks);
		}
		/* The initial common handshake installs pairwise then group keys but
		 * has no separate activation callback.  Activate that first complete
		 * pair only after both firmware ACKs. */
		if (result == 0 && !ax211_pci_keys_have_active(controller) &&
		    controller->staged_pairwise_key.valid &&
		    controller->staged_pairwise_key.programmed) {
			group_generation = 0U;
			index = 0U;
			while (index < INTEL_AX211_KEY_INDEX_LIMIT &&
			    group_generation == 0U) {
				if (controller->staged_group_key[index].valid &&
				    controller->staged_group_key[index].programmed)
					group_generation = controller->staged_group_key[
					    index].request.key_generation;
				index++;
			}
			if (group_generation != 0U) {
				result = ax211_pci_key_result_errno(
				    intel_ax211_key_state_activate(&controller->keys,
				    request->generation,
				    controller->staged_pairwise_key.request.key_generation,
				    group_generation, controller->hardware_epoch));
				if (result == 0) {
					ax211_pci_staged_key_clear(
					    &controller->staged_pairwise_key);
					for (index = 0U;
					    index < INTEL_AX211_KEY_INDEX_LIMIT; index++)
						ax211_pci_staged_key_clear(
						    &controller->staged_group_key[index]);
				}
			}
		}
		if (result != 0 && staged != NULL && staged->programmed)
			result = ax211_pci_key_fail_closed(controller, result);
	}
	ax211_pci_scrub(&key, sizeof(key));
	mutex_unlock(&controller->lifecycle_lock);
	return result;
}

static int
ax211_radio_key_delete(
	void *context,
	uint64_t generation,
	enum wlan_radio_key_kind kind,
	uint8_t key_index,
	uint64_t key_generation,
	uint64_t deadline)
{
	struct ax211_pci_controller *controller;
	struct ax211_pci_staged_key *staged;
	enum intel_ax211_key_kind private_kind;
	uint8_t command[INTEL_AX211_KEY_COMMAND_SIZE];
	int active_match;
	int programmed;
	int retired_match;
	int result;

	controller = context;
	if (controller == NULL)
		return ENODEV;
	if (generation == 0U || key_generation == 0U || deadline == 0U ||
	    (kind != WLAN_RADIO_KEY_PAIRWISE &&
	    kind != WLAN_RADIO_KEY_GROUP) ||
	    key_index >= INTEL_AX211_KEY_INDEX_LIMIT ||
	    (kind == WLAN_RADIO_KEY_PAIRWISE && key_index != 0U))
		return EINVAL;
	private_kind = kind == WLAN_RADIO_KEY_PAIRWISE ?
	    INTEL_AX211_KEY_PAIRWISE : INTEL_AX211_KEY_GROUP_KEY;
	memset(command, 0, sizeof(command));
	mutex_lock(&controller->lifecycle_lock);
	if (!controller->runtime_active ||
	    (controller->recovery_pending && !controller->recovery_running) ||
	    !controller->keys_initialized)
		result = ENETDOWN;
	else if (controller->connection_generation != generation)
		result = ESTALE;
	else {
		staged = private_kind == INTEL_AX211_KEY_PAIRWISE ?
		    &controller->staged_pairwise_key :
		    &controller->staged_group_key[key_index];
		programmed = staged->valid &&
		    staged->request.key_generation == key_generation &&
		    staged->programmed;
		active_match = private_kind == INTEL_AX211_KEY_PAIRWISE ?
		    controller->keys.active_pairwise == key_generation :
		    controller->keys.active_group[key_index] == key_generation;
		retired_match = private_kind == INTEL_AX211_KEY_PAIRWISE ?
		    controller->retired_pairwise_key_generation == key_generation :
		    controller->retired_group_key_generation[key_index] ==
		    key_generation;
		result = 0;
		if (programmed || active_match ||
		    (retired_match && private_kind == INTEL_AX211_KEY_GROUP_KEY &&
		    controller->retired_group_key_remove[key_index]))
			result = ax211_pci_key_result_errno(
			    intel_ax211_key_remove_encode(generation, key_generation,
			    private_kind, key_index, command));
		if (result == 0 && (programmed || active_match ||
		    (retired_match && private_kind == INTEL_AX211_KEY_GROUP_KEY &&
		    controller->retired_group_key_remove[key_index])))
			result = ax211_pci_key_command(controller, command, deadline);
		if (result == 0 && ((staged->valid &&
		    staged->request.key_generation == key_generation) || active_match))
			result = ax211_pci_key_result_errno(
			    intel_ax211_key_state_removed(&controller->keys,
			    generation, private_kind, key_index, key_generation,
			    controller->hardware_epoch));
		if (result == 0 && staged->valid &&
		    staged->request.key_generation == key_generation)
			ax211_pci_staged_key_clear(staged);
		if (result == 0 && retired_match) {
			if (private_kind == INTEL_AX211_KEY_PAIRWISE)
				controller->retired_pairwise_key_generation = 0U;
			else {
				controller->retired_group_key_generation[key_index] = 0U;
				controller->retired_group_key_remove[key_index] = 0U;
			}
		}
		if (result != 0 && (programmed || active_match || retired_match))
			result = ax211_pci_key_fail_closed(controller, result);
	}
	intel_ax211_key_command_scrub(command);
	mutex_unlock(&controller->lifecycle_lock);
	return result;
}

static int
ax211_radio_keys_activate(
	void *context,
	uint64_t generation,
	uint64_t pairwise_key_generation,
	uint64_t group_key_generation,
	uint64_t deadline)
{
	struct ax211_pci_controller *controller;
	struct ax211_pci_staged_key *group;
	uint64_t old_group[INTEL_AX211_KEY_INDEX_LIMIT];
	uint64_t old_pairwise;
	uint8_t group_index;
	uint8_t index;
	int command_crossed;
	int result;

	controller = context;
	if (controller == NULL)
		return ENODEV;
	if (generation == 0U || pairwise_key_generation == 0U ||
	    group_key_generation == 0U || deadline == 0U ||
	    clock_ticks() >= deadline)
		return EINVAL;
	mutex_lock(&controller->lifecycle_lock);
	if (!controller->runtime_active || controller->recovery_pending ||
	    controller->recovery_running || !controller->keys_initialized)
		result = ENETDOWN;
	else if (controller->connection_generation != generation)
		result = ESTALE;
	else {
		group = NULL;
		group_index = 0U;
		while (group_index < INTEL_AX211_KEY_INDEX_LIMIT && group == NULL) {
			if (controller->staged_group_key[group_index].valid &&
			    controller->staged_group_key[group_index].request.
			    key_generation == group_key_generation)
				group = &controller->staged_group_key[group_index];
			else
				group_index++;
		}
		old_pairwise = controller->keys.active_pairwise;
		memcpy(old_group, controller->keys.active_group, sizeof(old_group));
		command_crossed = 0;
		result = 0;
		if (group != NULL && !group->programmed) {
			command_crossed = 1;
			result = ax211_pci_staged_key_program(controller, group, deadline);
		}
		if (result == 0 && controller->staged_pairwise_key.valid &&
		    controller->staged_pairwise_key.request.key_generation ==
		    pairwise_key_generation &&
		    !controller->staged_pairwise_key.programmed) {
			command_crossed = 1;
			result = ax211_pci_staged_key_program(controller,
			    &controller->staged_pairwise_key, deadline);
		}
		if (result == 0)
			result = ax211_pci_key_result_errno(
			    intel_ax211_key_state_activate(&controller->keys, generation,
			    pairwise_key_generation, group_key_generation,
			    controller->hardware_epoch));
		if (result == 0) {
			if (old_pairwise != 0U &&
			    old_pairwise != pairwise_key_generation)
				controller->retired_pairwise_key_generation = old_pairwise;
			for (index = 0U; index < INTEL_AX211_KEY_INDEX_LIMIT;
			    index++) {
				if (old_group[index] != 0U &&
				    old_group[index] != group_key_generation) {
					controller->retired_group_key_generation[index] =
					    old_group[index];
					controller->retired_group_key_remove[index] =
					    index != group_index ? 1U : 0U;
				}
			}
			ax211_pci_staged_key_clear(
			    &controller->staged_pairwise_key);
			for (index = 0U; index < INTEL_AX211_KEY_INDEX_LIMIT;
			    index++)
				ax211_pci_staged_key_clear(
				    &controller->staged_group_key[index]);
		} else if (command_crossed)
			result = ax211_pci_key_fail_closed(controller, result);
		ax211_pci_scrub(old_group, sizeof(old_group));
	}
	mutex_unlock(&controller->lifecycle_lock);
	return result;
}

static int
ax211_radio_quiesce(
	void *context)
{
	struct ax211_pci_controller *controller;
	int error;

	controller = context;
	if (controller == NULL)
		return ENODEV;
	mutex_lock(&controller->lifecycle_lock);
	error = ax211_pci_session_stop(controller);
	if (error != 0)
		controller->quarantined = 1U;
	mutex_unlock(&controller->lifecycle_lock);
	return error;
}

/* Erases temporary and terminal controller-owned identity/state bytes. */
static void
ax211_pci_scrub(
	void *memory,
	size_t length)
{
	volatile uint8_t *byte;

	byte = memory;
	while (length != 0U) {
		*byte++ = 0U;
		length--;
	}
}
