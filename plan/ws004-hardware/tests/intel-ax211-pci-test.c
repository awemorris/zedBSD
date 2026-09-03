/*
 * Intel AX211 persistent PCI/CNVio2 lifecycle fixture
 * SPDX-License-Identifier: Zlib
 */

#include <drivers/pci.h>

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum fixture_failure {
	FIXTURE_FAIL_NONE,
	FIXTURE_FAIL_CLAIM,
	FIXTURE_FAIL_BAR,
	FIXTURE_FAIL_SAVE,
	FIXTURE_FAIL_MASTER,
	FIXTURE_FAIL_MAP,
	FIXTURE_FAIL_ENABLE,
	FIXTURE_FAIL_COMMAND_READ,
	FIXTURE_FAIL_BAR_RESTORE,
	FIXTURE_FAIL_RESTORE,
	FIXTURE_FAIL_IRQ_ALLOCATE,
	FIXTURE_FAIL_IRQ_WRONG_TYPE,
	FIXTURE_FAIL_IRQ_ESTABLISH,
	FIXTURE_FAIL_IRQ_DRAIN,
	FIXTURE_FAIL_BOOT_RUN,
	FIXTURE_FAIL_RUNTIME_RUN,
	FIXTURE_FAIL_SCAN_INIT
};

struct drv_pci_device {
	uint16_t vendor;
	uint16_t product;
	uint16_t subvendor;
	uint16_t subproduct;
	uint32_t class_code;
	uint8_t revision;
	struct drv_pci_address address;
	struct drv_pci_bar bar;
	uint16_t command;
	uint16_t saved_command;
	uint32_t registers[0x4000U / sizeof(uint32_t)];
	enum drv_pci_bar_type mapping_type;
	size_t mapping_size;
	enum fixture_failure failure;
	unsigned claimed;
	unsigned mapped;
	unsigned map_flags;
	unsigned map_reassigns_bar;
	unsigned fail_bar_restore;
	unsigned fail_state_restore;
	unsigned bar_reads;
	unsigned memory_enabled_without_master;
	unsigned command_read_without_master;
	unsigned irq_allocated;
	unsigned irq_established;
	unsigned irq_freed;
	unsigned irq_disestablished;
	unsigned poll_scheduled;
	drv_pci_irq_handler_t irq_handler;
	void *irq_argument;
	void *irq_cookie;
	void *driver_data;
	char events[256];
	size_t event_count;
};

#include "../../../src/drivers/pci-intel-ax211.c"

#define FIXTURE_TX_QUEUE 0x0107U
#define FIXTURE_TX_EVENT_QUEUE \
	((uint8_t)(FIXTURE_TX_QUEUE & 0x1fU))
#define FIXTURE_TX_EVENT_GROUP INTEL_AX211_PROTOCOL_GROUP_LEGACY

static struct drv_pci_driver *registered_driver;
static struct drv_pci_device *active_device;
static struct ax211_pci_controller *allocated_controller;
static struct intel_ax211_mmio *initialized_mmio;
static struct net_device *published_device;
static struct wlan_station *published_station;
static struct wlan_scan_profile published_profile;
static uint8_t strap_mac[6];
static uint8_t otp_mac[6];
static unsigned strap_valid;
static unsigned otp_valid;
static unsigned controller_frees;
static unsigned net_creates;
static unsigned net_destroys;
static unsigned station_attaches;
static unsigned station_detaches;
static unsigned station_opens;
static unsigned station_closes;
static unsigned scan_profile_updates;
static unsigned carrier_updates;
static unsigned lifecycle_locks;
static unsigned lifecycle_unlocks;
static unsigned radio_quiesce_callbacks;
static unsigned fail_controller_alloc;
static unsigned fail_net_alloc;
static unsigned fail_station_attach;
static unsigned fail_station_open;
static unsigned fail_net_gone;
static unsigned fail_station_close;
static unsigned fail_driver_data_clear;
static char log_buffer[1024];
static size_t log_length;
static unsigned read_barrier_count;
static unsigned write_barrier_count;
static unsigned transport_bind_count;
static unsigned transport_replenish_count;
static unsigned transport_rearm_count;
static unsigned transport_disable_count;
static unsigned transport_activate_count;
static unsigned boot_run_count;
static unsigned runtime_run_count;
static unsigned runtime_stop_count;
static unsigned scan_init_count;
static unsigned scan_begin_count;
static unsigned scan_start_ack_count;
static unsigned scan_abort_count;
static unsigned scan_abort_ack_count;
static unsigned scan_notification_count;
static unsigned scan_ready_reports;
static unsigned scan_frame_reports;
static unsigned scan_error_reports;
static unsigned rx_decode_count;
static unsigned command_submit_count;
static unsigned command_complete_count;
static unsigned tx_completion_reports;
static unsigned rx_frame_reports;
static unsigned link_loss_reports;
static unsigned dma_allocations;
static unsigned dma_frees;
static uint64_t reported_tx_cookie;
static uint64_t reported_rx_generation;
static uint64_t reported_link_loss_generation;
static int reported_link_loss_error;
static uint64_t scan_report_generation;
static uint32_t scan_report_step;
static uint8_t scan_report_channel;
static int32_t scan_report_rssi;
static int scan_report_error;
static uint16_t fixture_rx_frame_control;
static uint8_t fixture_rx_cipher;
static uint8_t fixture_rx_decrypted;
static uint8_t fixture_rx_key_index;
static uint64_t fixture_rx_packet_number;
static int fixture_rx_decode_result;
static int fixture_scan_expire_result;
static int fixture_scan_notification_result;
static unsigned coherent_dma = 1U;
static unsigned char fixture_dma_token;
static uint8_t fixture_rx_bytes[INTEL_AX211_BOOT_EVENT_CAPACITY];
static size_t fixture_rx_length;
static unsigned fixture_rx_ready;
static uint32_t fixture_hw_causes;
static uint64_t fixture_clock;
static unsigned fixture_command_timeout;
static unsigned fixture_key_add_timeout_once;
static unsigned fixture_command_malformed_queue;
static unsigned fixture_command_async_tx_before_response;
static unsigned fixture_pending_command_response;
static uint8_t fixture_pending_response_bytes[INTEL_AX211_BOOT_EVENT_CAPACITY];
static size_t fixture_pending_response_length;
static uint8_t fixture_async_tx_response[48];
static uint8_t fixture_async_tx_index;
static unsigned fixture_common_gate_held;
static unsigned fixture_key_add_count;
static unsigned fixture_key_remove_count;
static int fixture_frame_report_result;
static unsigned fixture_mcast_command_order;
static unsigned fixture_power_command_order;
static unsigned fixture_power_status;
static int fixture_power_response_length_delta;
static unsigned fixture_station_close_busy_retries;
static unsigned fixture_release_operation_on_yield;
static unsigned fixture_yield_count;
static unsigned fixture_poll_overlap;
static unsigned fixture_nested_poll_count;
static unsigned fixture_tx_kick_failure;
static unsigned fixture_command_fatal_mac;
static uint32_t fixture_sram[96];
static uint32_t fixture_sram_cursor;

#define FIXTURE_SRAM_BASE 0x00400000U
#define FIXTURE_UMAC_ERROR_ADDRESS (FIXTURE_SRAM_BASE + 0x100U)

struct wlan_station {
	const struct wlan_radio_ops *ops;
	void *radio_context;
	unsigned live;
};

static void fixture_event(struct drv_pci_device *device, char event);
static void fixture_reset(struct drv_pci_device *device);
static void test_registration_and_exact_match(void);
static void test_duplicate_registration_retains_controller(void);
static void test_attach_persists_until_refresh(void);
static void test_strap_publication_and_reverse_detach(void);
static void test_otp_fallback_and_retry(void);
static void test_partial_publication_retry(void);
static void test_checked_detach_retry(void);
static void test_attach_restore_failure_quarantine(void);
static void test_attach_failure_unwind(void);
static void test_open_failure_unwind(void);
static void test_checked_runtime_drain_retry(void);
static void test_common_up_down_up_lifecycle(void);
static void test_transmit_lease_detach_join(void);
static void test_malformed_rx_is_dropped(void);
static void test_failed_key_add_scrubs_plaintext(void);
static void test_tx_kick_schedules_recovery(void);
static void test_recovery_join_retries_without_free(void);
static void test_notification_layout_versions(void);
static void test_receive_copy_replenish_and_poll_boundary(void);
static void test_scan_session_and_rx_poll_integration(void);
static void test_scan_generation_resets_full_bss_cache(void);
static void test_association_key_tx_rx_disconnect_sequence(void);
static void test_association_failure_unwind_and_tx_timeout(void);
static void fixture_rx_event(uint8_t opcode, uint8_t group, uint8_t queue,
	const uint8_t *payload, size_t payload_length);
static void fixture_seed_bss(struct ax211_pci_controller *controller,
	const uint8_t bssid[6]);

static void
fixture_event(
	struct drv_pci_device *device,
	char event)
{
	assert(device->event_count + 1U < sizeof(device->events));
	device->events[device->event_count++] = event;
	device->events[device->event_count] = '\0';
}

static void
fixture_reset(
	struct drv_pci_device *device)
{
	static const uint8_t default_strap[6] = {
		0x02U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U
	};
	static const uint8_t default_otp[6] = {
		0x02U, 0xaaU, 0xbbU, 0xccU, 0xddU, 0xeeU
	};

	memset(device, 0, sizeof(*device));
	device->vendor = 0x8086U;
	device->product = 0x51f0U;
	device->subvendor = 0x8086U;
	device->subproduct = 0x4090U;
	device->class_code = 0x028000U;
	device->revision = 0x01U;
	device->address.segment = 0U;
	device->address.bus = 0U;
	device->address.device = 20U;
	device->address.function = 3U;
	device->bar.index = 0U;
	device->bar.type = DRV_PCI_BAR_MEMORY64;
	device->bar.bus_address = 0xf0810000U;
	device->bar.size = 0x4000U;
	device->command = 0x0005U;
	device->mapping_type = DRV_PCI_BAR_MEMORY64;
	device->mapping_size = 0x4000U;
	device->registers[0x028U / sizeof(uint32_t)] = 0x00000370U;
	device->registers[0x09cU / sizeof(uint32_t)] = 0x2010d000U;
	active_device = device;
	allocated_controller = NULL;
	initialized_mmio = NULL;
	published_device = NULL;
	published_station = NULL;
	memset(&published_profile, 0, sizeof(published_profile));
	memcpy(strap_mac, default_strap, sizeof(strap_mac));
	memcpy(otp_mac, default_otp, sizeof(otp_mac));
	strap_valid = 1U;
	otp_valid = 1U;
	controller_frees = 0U;
	net_creates = 0U;
	net_destroys = 0U;
	station_attaches = 0U;
	station_detaches = 0U;
	station_opens = 0U;
	station_closes = 0U;
	scan_profile_updates = 0U;
	carrier_updates = 0U;
	lifecycle_locks = 0U;
	lifecycle_unlocks = 0U;
	radio_quiesce_callbacks = 0U;
	fail_controller_alloc = 0U;
	fail_net_alloc = 0U;
	fail_station_attach = 0U;
	fail_station_open = 0U;
	fail_net_gone = 0U;
	fail_station_close = 0U;
	fail_driver_data_clear = 0U;
	memset(log_buffer, 0, sizeof(log_buffer));
	log_length = 0U;
	read_barrier_count = 0U;
	write_barrier_count = 0U;
	transport_bind_count = 0U;
	transport_replenish_count = 0U;
	transport_rearm_count = 0U;
	transport_disable_count = 0U;
	transport_activate_count = 0U;
	boot_run_count = 0U;
	runtime_run_count = 0U;
	runtime_stop_count = 0U;
	scan_init_count = 0U;
	scan_begin_count = 0U;
	scan_start_ack_count = 0U;
	scan_abort_count = 0U;
	scan_abort_ack_count = 0U;
	scan_notification_count = 0U;
	scan_ready_reports = 0U;
	scan_frame_reports = 0U;
	scan_error_reports = 0U;
	rx_decode_count = 0U;
	command_submit_count = 0U;
	command_complete_count = 0U;
	tx_completion_reports = 0U;
	rx_frame_reports = 0U;
	link_loss_reports = 0U;
	dma_allocations = 0U;
	dma_frees = 0U;
	reported_tx_cookie = 0U;
	reported_rx_generation = 0U;
	reported_link_loss_generation = 0U;
	reported_link_loss_error = 0;
	scan_report_generation = 0U;
	scan_report_step = 0U;
	scan_report_channel = 0U;
	scan_report_rssi = 0;
	scan_report_error = 0;
	fixture_rx_frame_control = 0x0080U;
	fixture_rx_cipher = INTEL_AX211_RX_CIPHER_NONE;
	fixture_rx_decrypted = 0U;
	fixture_rx_key_index = 0U;
	fixture_rx_packet_number = 0U;
	fixture_rx_decode_result = INTEL_AX211_RX_OK;
	fixture_scan_expire_result = INTEL_AX211_SCAN_SESSION_OK;
	fixture_scan_notification_result = INTEL_AX211_SCAN_SESSION_COMPLETE;
	coherent_dma = 1U;
	memset(fixture_rx_bytes, 0, sizeof(fixture_rx_bytes));
	fixture_rx_length = 0U;
	fixture_rx_ready = 0U;
	fixture_hw_causes = 0U;
	fixture_clock = 1U;
	fixture_command_timeout = 0U;
	fixture_key_add_timeout_once = 0U;
	fixture_command_malformed_queue = 0U;
	fixture_command_async_tx_before_response = 0U;
	fixture_pending_command_response = 0U;
	memset(fixture_pending_response_bytes, 0,
	    sizeof(fixture_pending_response_bytes));
	fixture_pending_response_length = 0U;
	memset(fixture_async_tx_response, 0, sizeof(fixture_async_tx_response));
	fixture_async_tx_index = 0U;
	fixture_common_gate_held = 0U;
	fixture_key_add_count = 0U;
	fixture_key_remove_count = 0U;
	fixture_frame_report_result = 0;
	fixture_mcast_command_order = 0U;
	fixture_power_command_order = 0U;
	fixture_power_status = 0U;
	fixture_power_response_length_delta = 0;
	fixture_station_close_busy_retries = 0U;
	fixture_release_operation_on_yield = 0U;
	fixture_yield_count = 0U;
	fixture_poll_overlap = 0U;
	fixture_nested_poll_count = 0U;
	fixture_tx_kick_failure = 0U;
	fixture_command_fatal_mac = 0U;
	memset(fixture_sram, 0, sizeof(fixture_sram));
	fixture_sram_cursor = 0U;
	ax211_registry_initialized = 0U;
	ax211_controllers = NULL;
	ax211_refresh_epoch = 0U;
	registered_driver = NULL;
}

uint16_t
drv_pci_device_vendor(
	const struct drv_pci_device *device)
{
	return device->vendor;
}

uint16_t
drv_pci_device_product(
	const struct drv_pci_device *device)
{
	return device->product;
}

uint16_t
drv_pci_device_subvendor(
	const struct drv_pci_device *device)
{
	return device->subvendor;
}

uint16_t
drv_pci_device_subproduct(
	const struct drv_pci_device *device)
{
	return device->subproduct;
}

uint32_t
drv_pci_device_class(
	const struct drv_pci_device *device)
{
	return device->class_code;
}

uint8_t
drv_pci_device_revision(
	const struct drv_pci_device *device)
{
	return device->revision;
}

void
drv_pci_device_address(
	const struct drv_pci_device *device,
	struct drv_pci_address *address)
{
	*address = device->address;
}

int
drv_pci_device_claim_bar(
	struct drv_pci_device *device,
	unsigned index)
{
	assert(index == 0U);
	fixture_event(device, 'C');
	if (device->failure == FIXTURE_FAIL_CLAIM)
		return EBUSY;
	assert(device->claimed == 0U);
	device->claimed = 1U;
	return 0;
}

void
drv_pci_device_release_bar(
	struct drv_pci_device *device,
	unsigned index)
{
	assert(index == 0U);
	assert(device->claimed != 0U);
	fixture_event(device, 'L');
	device->claimed = 0U;
}

int
drv_pci_device_bar(
	const struct drv_pci_device *constant_device,
	unsigned index,
	struct drv_pci_bar *bar)
{
	struct drv_pci_device *device;

	device = (struct drv_pci_device *)constant_device;
	assert(index == 0U);
	assert(device->claimed != 0U);
	fixture_event(device, device->bar_reads == 0U ? 'B' : 'b');
	device->bar_reads++;
	if (device->failure == FIXTURE_FAIL_BAR)
		return EIO;
	*bar = device->bar;
	return 0;
}

int
drv_pci_device_save_enable_state(
	struct drv_pci_device *device,
	struct drv_pci_enable_state *state)
{
	fixture_event(device, 'S');
	if (device->failure == FIXTURE_FAIL_SAVE)
		return EIO;
	device->saved_command = device->command;
	state->private_data[0] = device->command;
	state->private_data[1] = 1U;
	return 0;
}

int
drv_pci_device_restore_enable_state(
	struct drv_pci_device *device,
	struct drv_pci_enable_state *state)
{
	fixture_event(device, 'T');
	if (device->failure == FIXTURE_FAIL_RESTORE ||
	    device->fail_state_restore)
		return EIO;
	assert(state->private_data[1] != 0U);
	device->command = (uint16_t)state->private_data[0];
	state->private_data[0] = 0U;
	state->private_data[1] = 0U;
	return 0;
}

int
drv_pci_device_set_bus_master(
	struct drv_pci_device *device,
	bool enabled)
{
	fixture_event(device, enabled ? 'q' : 'Q');
	if (enabled)
		device->command |= 0x0004U;
	else
		device->command &= (uint16_t)~0x0004U;
	if (device->failure == FIXTURE_FAIL_MASTER)
		return EIO;
	return 0;
}

int
drv_pci_device_map_bar(
	struct drv_pci_device *device,
	unsigned index,
	unsigned flags,
	struct drv_pci_mapping *mapping)
{
	assert(index == 0U);
	assert(device->claimed != 0U);
	assert((device->command & 0x0004U) == 0U);
	fixture_event(device, 'M');
	device->map_flags = flags;
	if (device->map_reassigns_bar)
		device->bar.bus_address = 0xf0820000U;
	if (device->failure == FIXTURE_FAIL_MAP)
		return EIO;
	mapping->address = device->registers;
	mapping->size = device->mapping_size;
	mapping->type = device->mapping_type;
	device->mapped = 1U;
	return 0;
}

int
drv_pci_device_assign_bar(
	struct drv_pci_device *device,
	unsigned index,
	uint64_t address)
{
	assert(index == 0U);
	assert(device->claimed != 0U);
	assert((device->command & 0x0004U) == 0U);
	fixture_event(device, 'A');
	if (device->failure == FIXTURE_FAIL_BAR_RESTORE ||
	    device->fail_bar_restore)
		return EIO;
	device->bar.bus_address = address;
	return 0;
}

void
drv_pci_device_unmap_bar(
	struct drv_pci_device *device,
	struct drv_pci_mapping *mapping)
{
	assert(device->mapped != 0U);
	assert(mapping->address == device->registers);
	fixture_event(device, 'U');
	device->mapped = 0U;
	mapping->address = NULL;
}

int
drv_pci_device_enable_memory(
	struct drv_pci_device *device)
{
	fixture_event(device, 'E');
	if ((device->command & 0x0004U) == 0U)
		device->memory_enabled_without_master++;
	if (device->failure == FIXTURE_FAIL_ENABLE)
		return EIO;
	device->command |= 0x0002U;
	return 0;
}

int
drv_pci_device_config_read16(
	struct drv_pci_device *device,
	unsigned offset,
	uint16_t *value)
{
	assert(offset == 0x04U);
	fixture_event(device, 'R');
	if ((device->command & 0x0004U) == 0U)
		device->command_read_without_master++;
	if (device->failure == FIXTURE_FAIL_COMMAND_READ)
		return EIO;
	*value = device->command;
	return 0;
}

int
drv_pci_driver_register(
	struct drv_pci_driver *driver)
{
	if (registered_driver == driver)
		return EEXIST;
	registered_driver = driver;
	return 0;
}

void
hal_io_rmb(void)
{
	read_barrier_count++;
}

void
hal_io_mb(void)
{
	read_barrier_count++;
	write_barrier_count++;
}

int
hal_printf(
	const char *format,
	...)
{
	va_list arguments;
	int written;

	assert(log_length < sizeof(log_buffer));
	va_start(arguments, format);
	written = vsnprintf(log_buffer + log_length,
	    sizeof(log_buffer) - log_length, format, arguments);
	va_end(arguments);
	assert(written >= 0);
	assert((size_t)written < sizeof(log_buffer) - log_length);
	log_length += (size_t)written;
	return written;
}

void
spin_init(
	struct spinlock *lock,
	enum lock_rank rank,
	const char *name)
{
	memset(lock, 0, sizeof(*lock));
	lock->rank = rank;
	lock->name = name;
}

unsigned long
spin_lock_irqsave(
	struct spinlock *lock)
{
	assert(lock->held.value == 0U);
	lock->held.value = 1U;
	return 1UL;
}

void
spin_unlock_irqrestore(
	struct spinlock *lock,
	unsigned long enabled)
{
	assert(lock->held.value == 1U);
	assert(enabled == 1UL);
	lock->held.value = 0U;
}

int
mutex_init(
	struct mutex *mutex,
	enum lock_rank rank,
	const char *name)
{
	assert(mutex != NULL);
	memset(mutex, 0, sizeof(*mutex));
	mutex->guard.rank = rank;
	mutex->guard.name = name;
	return 0;
}

void
mutex_lock(
	struct mutex *mutex)
{
	assert(mutex != NULL);
	assert(mutex->locked == 0U);
	mutex->locked = 1U;
	lifecycle_locks++;
}

void
mutex_unlock(
	struct mutex *mutex)
{
	assert(mutex != NULL);
	assert(mutex->locked == 1U);
	mutex->locked = 0U;
	lifecycle_unlocks++;
}

void *
hal_malloc(
	size_t size)
{
	void *memory;

	assert(size == sizeof(struct ax211_pci_controller));
	if (fail_controller_alloc)
		return NULL;
	memory = calloc(1U, size);
	assert(memory != NULL);
	allocated_controller = memory;
	return memory;
}

void
hal_free(
	void *memory)
{
	const uint8_t *bytes;
	size_t index;

	assert(memory == allocated_controller);
	bytes = memory;
	for (index = 0U; index < sizeof(struct ax211_pci_controller); index++)
		assert(bytes[index] == 0U);
	fixture_event(active_device, 'F');
	controller_frees++;
	allocated_controller = NULL;
	free(memory);
}

void *
drv_pci_device_driver_data(
	const struct drv_pci_device *device)
{
	return device->driver_data;
}

int
drv_pci_device_set_driver_data(
	struct drv_pci_device *device,
	void *data)
{
	fixture_event(device, data == NULL ? 'd' : 'D');
	if (data == NULL && fail_driver_data_clear)
		return EIO;
	device->driver_data = data;
	return 0;
}

int
intel_ax211_pci_mmio_backend_init(
	struct intel_ax211_pci_mmio_backend *backend,
	void *registers,
	size_t mapping_size)
{
	fixture_event(active_device, 'K');
	memset(backend, 0, sizeof(*backend));
	backend->registers = registers;
	backend->mapping_size = mapping_size;
	return 0;
}

static int
fixture_delay_us(
	void *argument,
	uint32_t duration)
{
	(void)argument;
	fixture_clock += duration;
	return 0;
}

static int
fixture_clock_us(
	void *argument,
	uint64_t *time_us)
{
	(void)argument;
	assert(time_us != NULL);
	*time_us = fixture_clock;
	return 0;
}

static int
fixture_csr_write32(
	void *argument,
	uint32_t offset,
	uint32_t value)
{
	struct intel_ax211_pci_mmio_backend *backend = argument;

	assert(backend == &allocated_controller->backend);
	if (offset == AX211_HBUS_TARG_MEM_RADDR) {
		fixture_sram_cursor = value;
		return 0;
	}
	assert(offset == INTEL_AX211_TX_RING_WRITE_POINTER_REGISTER);
	assert((value >> 16) == FIXTURE_TX_QUEUE);
	return fixture_tx_kick_failure ? -1 : 0;
}

static int
fixture_csr_read32(
	void *argument,
	uint32_t offset,
	uint32_t *value)
{
	struct intel_ax211_pci_mmio_backend *backend = argument;
	size_t index;

	assert(backend == &allocated_controller->backend);
	assert(offset == AX211_HBUS_TARG_MEM_RDAT);
	assert(value != NULL);
	assert(fixture_sram_cursor >= FIXTURE_SRAM_BASE);
	assert((fixture_sram_cursor & 3U) == 0U);
	index = (fixture_sram_cursor - FIXTURE_SRAM_BASE) / sizeof(uint32_t);
	assert(index < sizeof(fixture_sram) / sizeof(fixture_sram[0U]));
	*value = fixture_sram[index];
	fixture_sram_cursor += sizeof(uint32_t);
	return 0;
}

static const struct intel_ax211_mmio_ops fixture_mmio_ops = {
	.csr_read32 = fixture_csr_read32,
	.csr_write32 = fixture_csr_write32,
	.delay_us = fixture_delay_us,
	.clock_us = fixture_clock_us
};

const struct intel_ax211_mmio_ops *
intel_ax211_pci_mmio_ops(void)
{
	return &fixture_mmio_ops;
}

int
intel_ax211_mmio_init(
	struct intel_ax211_mmio *mmio,
	const struct intel_ax211_mmio_ops *ops,
	void *argument,
	const struct intel_ax211_mmio_profile *profile)
{
	struct ax211_pci_controller *controller;

	fixture_event(active_device, 'I');
	controller = allocated_controller;
	assert(argument == &controller->backend);
	assert(ops == &fixture_mmio_ops);
	assert(profile->mac_type == INTEL_AX211_MMIO_MAC_SO ||
	    profile->mac_type == INTEL_AX211_MMIO_MAC_SOF);
	assert(profile->rf_type == INTEL_AX211_MMIO_RF_GF);
	memset(mmio, 0, sizeof(*mmio));
	mmio->ops = ops;
	mmio->argument = argument;
	mmio->profile = *profile;
	initialized_mmio = mmio;
	return INTEL_AX211_MMIO_OK;
}

int
intel_ax211_mmio_prepare_card_hw(
	struct intel_ax211_mmio *mmio)
{
	assert(mmio == initialized_mmio);
	fixture_event(active_device, 'P');
	mmio->prepared = 1;
	return INTEL_AX211_MMIO_OK;
}

int
intel_ax211_mmio_sw_reset(
	struct intel_ax211_mmio *mmio)
{
	assert(mmio == initialized_mmio);
	assert(mmio->prepared);
	fixture_event(active_device, 'Z');
	mmio->reset_done = 1;
	return INTEL_AX211_MMIO_OK;
}

int
intel_ax211_mmio_apm_init(
	struct intel_ax211_mmio *mmio)
{
	assert(mmio == initialized_mmio);
	assert(mmio->reset_done);
	fixture_event(active_device, 'A');
	mmio->apm_ready = 1;
	return INTEL_AX211_MMIO_OK;
}

int
intel_ax211_mmio_read_mac(
	struct intel_ax211_mmio *mmio,
	uint8_t mac_address[6])
{
	assert(mmio == initialized_mmio);
	assert(mmio->apm_ready);
	fixture_event(active_device, 'H');
	if (strap_valid)
		memcpy(mac_address, strap_mac, 6U);
	else if (otp_valid)
		memcpy(mac_address, otp_mac, 6U);
	else
		return INTEL_AX211_MMIO_IO;
	return INTEL_AX211_MMIO_OK;
}

int
intel_ax211_mmio_stop(
	struct intel_ax211_mmio *mmio)
{
	assert(mmio == initialized_mmio);
	fixture_event(active_device, 'O');
	mmio->prepared = 0;
	mmio->reset_done = 0;
	mmio->apm_ready = 0;
	return INTEL_AX211_MMIO_OK;
}

struct net_device *
net_device_alloc(void)
{
	if (fail_net_alloc)
		return NULL;
	return calloc(1U, sizeof(struct net_device));
}

int
net_device_create(
	struct net_device *device)
{
	assert(device != NULL);
	assert(allocated_controller->lifecycle_lock.locked == 1U);
	assert(published_device == NULL);
	fixture_event(active_device, 'N');
	published_device = device;
	net_creates++;
	return 0;
}

int
net_device_set_carrier(
	struct net_device *device,
	int carrier)
{
	assert(device != NULL);
	device->carrier = carrier != 0;
	carrier_updates++;
	return 0;
}

int
net_device_gone(
	struct net_device *device)
{
	assert(device == published_device);
	assert(allocated_controller->lifecycle_lock.locked == 0U);
	fixture_event(active_device, 'G');
	if (fail_net_gone)
		return EBUSY;
	published_device = NULL;
	return 0;
}

void
net_device_destroy(
	struct net_device *device)
{
	void *driver_data;

	assert(device != NULL);
	assert(device != published_device);
	if (device->driver_data != NULL)
		assert(allocated_controller->lifecycle_lock.locked == 0U);
	fixture_event(active_device, 'V');
	driver_data = device->driver_data;
	device->driver_data = NULL;
	net_destroys++;
	if (driver_data != NULL && device->ops != NULL &&
	    device->ops->release != NULL)
		device->ops->release(driver_data);
	memset(device, 0, sizeof(*device));
	free(device);
}

int
wlan_station_attach(
	struct net_device *device,
	const struct wlan_radio_ops *ops,
	void *radio_context,
	const struct wlan_scan_profile *profile,
	struct wlan_station **result)
{
	unsigned index;

	assert(device == published_device);
	assert(allocated_controller->lifecycle_lock.locked == 1U);
	assert(ops == &ax211_radio_ops);
	assert(radio_context == allocated_controller);
	assert(profile->channel_count == AX211_PASSIVE_CHANNEL_COUNT);
	for (index = 0U; index < profile->channel_count; index++) {
		assert(profile->channels[index].channel == index + 1U);
		assert(profile->channels[index].center_frequency_mhz ==
		    2412U + index * 5U);
		assert(profile->channels[index].flags == 0U);
	}
	fixture_event(active_device, 'W');
	station_attaches++;
	published_profile = *profile;
	if (fail_station_attach)
		return EIO;
	published_station = calloc(1U, sizeof(*published_station));
	assert(published_station != NULL);
	published_station->ops = ops;
	published_station->radio_context = radio_context;
	published_station->live = 0U;
	*result = published_station;
	return 0;
}

int
wlan_station_scan_profile_update(
	struct wlan_station *station,
	const struct wlan_scan_profile *profile)
{
	assert(station == published_station);
	assert(allocated_controller->lifecycle_lock.locked == 1U);
	assert(station->live == 0U);
	assert(profile != NULL && profile->channel_count == 11U);
	assert(profile->channels[0U].channel == 1U);
	assert(profile->channels[0U].center_frequency_mhz == 2412U);
	assert(profile->channels[10U].channel == 36U);
	assert(profile->channels[10U].center_frequency_mhz == 5180U);
	published_profile = *profile;
	scan_profile_updates++;
	return 0;
}

int
wlan_station_open(
	struct wlan_station *station)
{
	assert(station == published_station);
	assert(allocated_controller->lifecycle_lock.locked == 1U);
	assert(allocated_controller->runtime_active == 1U);
	assert(allocated_controller->active_dma != NULL);
	station_opens++;
	if (fail_station_open)
		return EIO;
	station->live = 1U;
	return 0;
}

int
wlan_station_close(
	struct wlan_station *station)
{
	assert(station == published_station);
	assert(allocated_controller->lifecycle_lock.locked == 0U);
	station_closes++;
	if (fixture_station_close_busy_retries != 0U) {
		fixture_station_close_busy_retries--;
		return EBUSY;
	}
	if (fail_station_close)
		return EBUSY;
	fixture_event(active_device, 'X');
	station->live = 0U;
	return 0;
}

int
wlan_station_detach(
	struct wlan_station *station)
{
	assert(station == published_station);
	assert(allocated_controller->lifecycle_lock.locked == 0U);
	assert(station->live == 0U);
	assert(station->ops != NULL);
	assert(station->ops->quiesce != NULL);
	radio_quiesce_callbacks++;
	if (station->ops->quiesce(station->radio_context) != 0)
		return EIO;
	assert(allocated_controller->lifecycle_lock.locked == 0U);
	fixture_event(active_device, 'Y');
	station_detaches++;
	published_station = NULL;
	free(station);
	return 0;
}

int
wlan_station_ioctl(
	struct net_device *device,
	unsigned long request,
	void *argument)
{
	(void)device;
	(void)request;
	(void)argument;
	return ENOTSUP;
}

void
packet_buf_free(
	struct packet_buf *packet)
{
	free(packet);
}

uint64_t
clock_ticks(void)
{
	return fixture_clock / (1000000U / KERN_CLOCK_HZ);
}

void
sched_yield(void)
{
	fixture_yield_count++;
	fixture_clock += 1000000U / KERN_CLOCK_HZ;
	if (fixture_release_operation_on_yield) {
		fixture_release_operation_on_yield = 0U;
		assert(allocated_controller != NULL);
		assert(published_station != NULL);
		mutex_lock(&allocated_controller->lifecycle_lock);
		ax211_pci_operation_leave_locked(allocated_controller);
		mutex_unlock(&allocated_controller->lifecycle_lock);
	}
}

int
drv_dma_alloc_coherent(
	struct drv_dma_device *device,
	size_t size,
	size_t alignment,
	struct drv_dma_buffer *buffer)
{
	void *memory;
	size_t host_alignment;

	assert(device == (struct drv_dma_device *)&fixture_dma_token);
	assert(size != 0U && alignment != 0U && buffer != NULL);
	host_alignment = alignment < sizeof(void *) ? sizeof(void *) : alignment;
	memory = NULL;
	if (posix_memalign(&memory, host_alignment, size) != 0)
		return ENOMEM;
	memset(memory, 0, size);
	memset(buffer, 0, sizeof(*buffer));
	buffer->address = memory;
	buffer->device_address = (uint64_t)(uintptr_t)memory;
	buffer->size = size;
	dma_allocations++;
	return 0;
}

void
drv_dma_free_coherent(
	struct drv_dma_device *device,
	struct drv_dma_buffer *buffer)
{
	assert(device == (struct drv_dma_device *)&fixture_dma_token);
	assert(buffer != NULL && buffer->address != NULL && buffer->size != 0U);
	free(buffer->address);
	memset(buffer, 0, sizeof(*buffer));
	dma_frees++;
}

struct drv_dma_device *
drv_pci_device_dma(
	struct drv_pci_device *device)
{
	(void)device;
	return (struct drv_dma_device *)&fixture_dma_token;
}

int
drv_dma_device_is_coherent(
	const struct drv_dma_device *device)
{
	return device == (const struct drv_dma_device *)&fixture_dma_token &&
	    coherent_dma;
}

int
drv_pci_device_find_capability(
	struct drv_pci_device *device,
	uint8_t identity,
	unsigned *result)
{
	(void)device;
	assert(identity == 0x10U);
	assert(result != NULL);
	return ENOENT;
}

int
drv_pci_device_allocate_irqs(
	struct drv_pci_device *device,
	unsigned flags,
	unsigned minimum,
	unsigned maximum,
	struct drv_pci_irq *irq,
	unsigned *count)
{
	assert(flags == DRV_PCI_IRQ_ALLOW_MSIX);
	assert(minimum == 1U && maximum == 1U);
	assert(irq != NULL && count != NULL);
	fixture_event(device, 'J');
	if (device->failure == FIXTURE_FAIL_IRQ_ALLOCATE)
		return ENOSPC;
	memset(irq, 0, sizeof(*irq));
	irq->type = device->failure == FIXTURE_FAIL_IRQ_WRONG_TYPE ?
	    DRV_PCI_IRQ_MSI : DRV_PCI_IRQ_MSIX;
	irq->vector = 64U;
	*count = 1U;
	device->irq_allocated = 1U;
	return 0;
}

void
drv_pci_device_free_irqs(
	struct drv_pci_device *device,
	struct drv_pci_irq *irq,
	unsigned count)
{
	assert(irq != NULL && (irq->type == DRV_PCI_IRQ_MSIX ||
	    (device->failure == FIXTURE_FAIL_IRQ_WRONG_TYPE &&
	    irq->type == DRV_PCI_IRQ_MSI)));
	assert(count == 1U && device->irq_allocated);
	assert(!device->irq_established);
	fixture_event(device, 'j');
	device->irq_allocated = 0U;
	device->irq_freed++;
}

int
drv_pci_device_establish_irq(
	struct drv_pci_device *device,
	const struct drv_pci_irq *irq,
	drv_pci_irq_handler_t handler,
	void *argument,
	const char *name,
	void **result)
{
	assert(irq != NULL && irq->type == DRV_PCI_IRQ_MSIX);
	assert(handler != NULL && argument == allocated_controller);
	assert(strcmp(name, "intel-ax211") == 0);
	assert(result != NULL && device->irq_allocated);
	fixture_event(device, 'K');
	if (device->failure == FIXTURE_FAIL_IRQ_ESTABLISH)
		return EIO;
	device->irq_handler = handler;
	device->irq_argument = argument;
	device->irq_cookie = device;
	device->irq_established = 1U;
	*result = device->irq_cookie;
	return 0;
}

int
drv_pci_device_disestablish_irq_checked(
	struct drv_pci_device *device,
	void *cookie)
{
	assert(cookie == device->irq_cookie);
	assert(device->irq_established);
	fixture_event(device, 'k');
	if (device->failure == FIXTURE_FAIL_IRQ_DRAIN)
		return EBUSY;
	device->irq_handler = NULL;
	device->irq_argument = NULL;
	device->irq_cookie = NULL;
	device->irq_established = 0U;
	device->irq_disestablished++;
	return 0;
}

void
net_device_schedule_poll(
	struct net_device *device)
{
	assert(device == published_device);
	active_device->poll_scheduled++;
}

void
hal_io_wmb(void)
{
	write_barrier_count++;
}

static const struct intel_ax211_transport_ops fixture_transport_ops;

int
intel_ax211_transport_backend_init(
	struct intel_ax211_transport_backend *backend,
	struct intel_ax211_mmio *mmio,
	struct intel_ax211_pci_mmio_backend *pci_mmio,
	struct intel_ax211_dma_resources *dma)
{
	assert(backend != NULL && mmio == initialized_mmio);
	assert(pci_mmio == &allocated_controller->backend);
	assert(dma != NULL && dma->device == drv_pci_device_dma(active_device));
	memset(backend, 0, sizeof(*backend));
	backend->mmio = mmio;
	backend->pci_mmio = pci_mmio;
	backend->dma = dma;
	backend->initialized = 1U;
	return INTEL_AX211_TRANSPORT_BACKEND_OK;
}

int
intel_ax211_transport_backend_ring_memory(
	const struct intel_ax211_transport_backend *backend,
	struct intel_ax211_transport_ring_memory *memory)
{
	assert(backend != NULL && backend->initialized);
	assert(memory != NULL);
	memset(memory, 0, sizeof(*memory));
	return INTEL_AX211_TRANSPORT_BACKEND_OK;
}

const struct intel_ax211_transport_ops *
intel_ax211_transport_backend_ops(void)
{
	return &fixture_transport_ops;
}

int
intel_ax211_transport_init(
	struct intel_ax211_transport *transport,
	const struct intel_ax211_transport_ops *ops,
	void *argument,
	const struct intel_ax211_mmio_profile *profile,
	const struct intel_ax211_transport_ring_memory *memory)
{
	(void)memory;
	assert(transport == &allocated_controller->transport);
	assert(ops == &fixture_transport_ops);
	assert(argument == &allocated_controller->transport_backend);
	assert(profile == &allocated_controller->mmio.profile);
	memset(transport, 0, sizeof(*transport));
	transport->ops = ops;
	transport->argument = argument;
	transport->profile = *profile;
	transport_bind_count++;
	return INTEL_AX211_TRANSPORT_OK;
}

int
intel_ax211_transport_interrupt_claim(
	struct intel_ax211_transport *transport,
	struct intel_ax211_transport_causes *causes)
{
	assert(transport == &allocated_controller->transport);
	assert(causes != NULL);
	memset(causes, 0, sizeof(*causes));
	causes->hardware = fixture_hw_causes;
	causes->raw_hardware = fixture_hw_causes;
	fixture_hw_causes = 0U;
	return INTEL_AX211_TRANSPORT_OK;
}

int
intel_ax211_transport_activate_rx(
	struct intel_ax211_transport *transport)
{
	assert(transport == &allocated_controller->transport);
	assert(!transport->rx_active);
	transport->rx_active = 1U;
	transport_activate_count++;
	return INTEL_AX211_TRANSPORT_OK;
}

int
intel_ax211_transport_rx_next(
	struct intel_ax211_transport *transport,
	struct intel_ax211_transport_rx_completion *completion)
{
	assert(transport == &allocated_controller->transport);
	assert(completion != NULL);
	if (!transport->rx_active)
		return INTEL_AX211_TRANSPORT_ORDER;
	if (!fixture_rx_ready)
		return INTEL_AX211_TRANSPORT_STALE;
	memset(completion, 0, sizeof(*completion));
	return INTEL_AX211_TRANSPORT_OK;
}

int
intel_ax211_transport_rx_replenish(
	struct intel_ax211_transport *transport,
	uint64_t device_address)
{
	assert(transport == &allocated_controller->transport);
	assert(device_address == UINT64_C(0x1000));
	assert(fixture_rx_ready);
	if (fixture_pending_command_response) {
		memcpy(fixture_rx_bytes, fixture_pending_response_bytes,
		    fixture_pending_response_length);
		fixture_rx_length = fixture_pending_response_length;
		fixture_rx_ready = 1U;
		fixture_pending_command_response = 0U;
		memset(fixture_pending_response_bytes, 0,
		    sizeof(fixture_pending_response_bytes));
		fixture_pending_response_length = 0U;
	} else {
		fixture_rx_ready = 0U;
		memset(fixture_rx_bytes, 0xa5, sizeof(fixture_rx_bytes));
	}
	transport_replenish_count++;
	return INTEL_AX211_TRANSPORT_OK;
}

int
intel_ax211_transport_interrupt_rearm(
	struct intel_ax211_transport *transport)
{
	assert(transport == &allocated_controller->transport);
	transport_rearm_count++;
	return INTEL_AX211_TRANSPORT_OK;
}

int
intel_ax211_transport_disable_interrupts(
	struct intel_ax211_transport *transport)
{
	assert(transport == &allocated_controller->transport);
	transport->interrupts_enabled = 0U;
	transport_disable_count++;
	return INTEL_AX211_TRANSPORT_OK;
}

int
intel_ax211_mmio_nic_lock(
	struct intel_ax211_mmio *mmio)
{
	assert(mmio == initialized_mmio);
	mmio->nic_lock_depth++;
	return INTEL_AX211_MMIO_OK;
}

int
intel_ax211_mmio_nic_unlock(
	struct intel_ax211_mmio *mmio)
{
	assert(mmio == initialized_mmio && mmio->nic_lock_depth != 0U);
	mmio->nic_lock_depth--;
	return INTEL_AX211_MMIO_OK;
}

int
intel_ax211_mmio_prph_write32(
	struct intel_ax211_mmio *mmio,
	uint32_t address,
	uint32_t value)
{
	assert(mmio == initialized_mmio && mmio->nic_lock_depth != 0U);
	assert(address == UINT32_C(0xd05c04));
	assert(value == (1U << 20));
	return INTEL_AX211_MMIO_OK;
}

int
intel_ax211_event_decode(
	const uint8_t *bytes,
	size_t length,
	struct intel_ax211_event *event)
{
	size_t frame_length;

	if (bytes == NULL || event == NULL || length < 8U)
		return INTEL_AX211_TRUNCATED;
	frame_length = ax211_pci_get_le32(bytes) & 0x3fffU;
	if (frame_length < 4U || frame_length + 4U != length)
		return INTEL_AX211_INVALID;
	memset(event, 0, sizeof(*event));
	event->command.opcode = bytes[4U];
	event->flags = bytes[5U];
	event->index = bytes[6U];
	event->queue = bytes[7U];
	event->payload_offset = 8U;
	event->payload_length = frame_length - 4U;
	return INTEL_AX211_OK;
}

int
intel_ax211_protocol_command_table_validate_api89(
	const struct intel_ax211_protocol_command_table *table)
{
	return table != NULL && table->bytes != NULL && table->count ==
	    INTEL_AX211_PROTOCOL_API89_COMMAND_COUNT ?
	    INTEL_AX211_PROTOCOL_OK : INTEL_AX211_PROTOCOL_INVALID;
}

int
intel_ax211_protocol_command_version_lookup(
	const struct intel_ax211_protocol_command_table *table,
	uint8_t group,
	uint8_t opcode,
	struct intel_ax211_protocol_command_version *version)
{
	uint8_t command_version;
	uint8_t notification_version;

	if (intel_ax211_protocol_command_table_validate_api89(table) !=
	    INTEL_AX211_PROTOCOL_OK || version == NULL)
		return INTEL_AX211_PROTOCOL_INVALID;
	command_version = 0U;
	notification_version = 0U;
	if (group == INTEL_AX211_ASSOC_GROUP_LONG &&
	    opcode == INTEL_AX211_ASSOC_PHY_CONTEXT_OPCODE)
		command_version = INTEL_AX211_ASSOC_PHY_CONTEXT_VERSION;
	else if (group == INTEL_AX211_ASSOC_GROUP_MAC_CONFIG &&
	    opcode == INTEL_AX211_ASSOC_MAC_CONFIG_OPCODE)
		command_version = INTEL_AX211_ASSOC_MAC_CONFIG_VERSION;
	else if (group == INTEL_AX211_ASSOC_GROUP_MAC_CONFIG &&
	    opcode == INTEL_AX211_ASSOC_LINK_CONFIG_OPCODE)
		command_version = INTEL_AX211_ASSOC_LINK_CONFIG_VERSION;
	else if (group == INTEL_AX211_ASSOC_GROUP_DATA_PATH &&
	    opcode == INTEL_AX211_ASSOC_RLC_CONFIG_OPCODE)
		command_version = INTEL_AX211_ASSOC_RLC_CONFIG_VERSION;
	else if (group == INTEL_AX211_ASSOC_GROUP_DATA_PATH &&
	    opcode == INTEL_AX211_ASSOC_QUEUE_CONFIG_OPCODE) {
		command_version = INTEL_AX211_ASSOC_QUEUE_CONFIG_VERSION;
		notification_version = INTEL_AX211_ASSOC_QUEUE_RESPONSE_VERSION;
	} else if (group == INTEL_AX211_ASSOC_GROUP_MAC_CONFIG &&
	    opcode == INTEL_AX211_ASSOC_SESSION_PROTECTION_OPCODE)
		command_version = INTEL_AX211_ASSOC_SESSION_PROTECTION_VERSION;
	else if (group == INTEL_AX211_ASSOC_GROUP_MAC_CONFIG &&
	    opcode == INTEL_AX211_ASSOC_SESSION_NOTIFICATION_OPCODE) {
		command_version = INTEL_AX211_PROTOCOL_UNKNOWN_VERSION;
		notification_version =
		    INTEL_AX211_ASSOC_SESSION_NOTIFICATION_API89_VERSION;
	} else if (group == INTEL_AX211_ASSOC_GROUP_LONG &&
	    opcode == INTEL_AX211_ASSOC_MCAST_FILTER_OPCODE)
		command_version = INTEL_AX211_ASSOC_MCAST_FILTER_VERSION;
	else if (group == INTEL_AX211_ASSOC_GROUP_LONG &&
	    opcode == INTEL_AX211_ASSOC_MAC_POWER_OPCODE)
		command_version = INTEL_AX211_ASSOC_MAC_POWER_VERSION;
	else if (group == INTEL_AX211_KEY_GROUP &&
	    opcode == INTEL_AX211_KEY_OPCODE)
		command_version = INTEL_AX211_KEY_COMMAND_VERSION;
	else if (group == INTEL_AX211_TX_GROUP &&
	    opcode == INTEL_AX211_TX_OPCODE) {
		command_version = INTEL_AX211_TX_COMMAND_VERSION;
		notification_version = INTEL_AX211_TX_NOTIFICATION_VERSION;
	} else
		return INTEL_AX211_PROTOCOL_MISSING;
	memset(version, 0, sizeof(*version));
	version->group = group;
	version->opcode = opcode;
	version->command_version = command_version;
	version->notification_version = notification_version;
	return INTEL_AX211_PROTOCOL_OK;
}

int
intel_ax211_protocol_command_response_validate(
	const struct intel_ax211_protocol_message *message,
	const struct intel_ax211_protocol_pending_command *pending)
{
	if (message == NULL || pending == NULL || message->group != pending->group ||
	    message->opcode != pending->opcode ||
	    message->version != pending->response_version ||
	    message->queue != pending->queue || message->index != pending->index ||
	    message->generation != pending->generation ||
	    (message->flags & INTEL_AX211_PROTOCOL_COMMAND_FAILED_MASK) != 0U)
		return INTEL_AX211_PROTOCOL_STALE;
	if (message->payload_length < pending->minimum_response_length)
		return INTEL_AX211_PROTOCOL_TRUNCATED;
	if (message->payload_length > pending->maximum_response_length)
		return INTEL_AX211_PROTOCOL_OVERSIZED;
	return INTEL_AX211_PROTOCOL_OK;
}

size_t
intel_ax211_command_pending_count(
	const struct intel_ax211_command_transaction *transaction)
{
	return transaction == NULL ? 0U : transaction->pending_count;
}

int
intel_ax211_command_submit(
	struct intel_ax211_command_transaction *transaction,
	const struct intel_ax211_command_request *request,
	uint64_t now,
	uint64_t timeout,
	struct intel_ax211_command_handle *handle)
{
	struct intel_ax211_command_entry *entry;
	uint8_t response[INTEL_AX211_ASSOC_RESPONSE_MAX];
	size_t response_length;
	int suppress_response;

	assert(transaction == &allocated_controller->runtime_start.commands);
	assert(request != NULL && handle != NULL && now != 0U && timeout != 0U);
	if (!transaction->initialized || transaction->poisoned)
		return INTEL_AX211_COMMAND_POISONED;
	assert(transaction->pending_count == 0U);
	memset(handle, 0, sizeof(*handle));
	handle->token.queue = 1U;
	handle->token.index = 3U;
	handle->generation = ++transaction->next_generation;
	handle->hardware_epoch = transaction->hardware_epoch;
	entry = &transaction->entry[handle->token.index];
	memset(entry, 0, sizeof(*entry));
	entry->active = 1U;
	entry->logical_generation = handle->generation;
	entry->deadline = now + timeout;
	entry->pending.group = request->command.group;
	entry->pending.opcode = request->command.opcode;
	entry->pending.response_version = request->response_version;
	entry->pending.queue = handle->token.queue;
	entry->pending.index = handle->token.index;
	entry->pending.generation = transaction->hardware_epoch;
	entry->pending.minimum_response_length =
	    request->minimum_response_length;
	entry->pending.maximum_response_length =
	    request->maximum_response_length;
	transaction->pending_count = 1U;
	command_submit_count++;
	suppress_response = fixture_command_timeout != 0U;
	if (request->command.group == INTEL_AX211_ASSOC_GROUP_MAC_CONFIG &&
	    request->command.opcode == INTEL_AX211_ASSOC_MAC_CONFIG_OPCODE) {
		const uint8_t *payload = request->payload;
		uint32_t action;

		assert(request->command.version == 0U);
		assert(payload != NULL && request->payload_length ==
		    INTEL_AX211_ASSOC_MAC_CONFIG_SIZE);
		assert(ax211_pci_get_le32(payload) == INTEL_AX211_ASSOC_MAC_ID);
		action = ax211_pci_get_le32(payload + 4U);
		assert(action >= 1U && action <= 3U);
		if (action != 3U) {
			assert(ax211_pci_get_le32(payload + 8U) == 5U);
			assert(ax211_pci_get_le32(payload + 20U) == 0x0cU);
			assert(ax211_pci_get_le32(payload + 32U) == 1U);
		}
		if (fixture_command_fatal_mac && action == 1U) {
			fixture_command_fatal_mac = 0U;
			fixture_hw_causes =
			    INTEL_AX211_TRANSPORT_HW_CAUSE_SW_ERROR;
			suppress_response = 1;
		}
	}
	if (request->command.group == INTEL_AX211_ASSOC_GROUP_MAC_CONFIG &&
	    request->command.opcode == INTEL_AX211_ASSOC_LINK_CONFIG_OPCODE) {
		const uint8_t *payload = request->payload;
		uint32_t action;
		uint32_t mask;

		assert(request->command.version == 0U);
		assert(payload != NULL && request->payload_length ==
		    INTEL_AX211_ASSOC_LINK_CONFIG_SIZE);
		action = ax211_pci_get_le32(payload);
		assert(action >= 1U && action <= 3U);
		assert(ax211_pci_get_le32(payload + 4U) ==
		    INTEL_AX211_ASSOC_LINK_ID);
		if (action == 1U || action == 3U)
			assert(ax211_pci_get_le32(payload + 12U) == UINT32_MAX);
		else {
			assert(ax211_pci_get_le32(payload + 12U) == 0U);
			mask = ax211_pci_get_le32(payload + 24U);
			assert(mask == 0U || mask == 0x01U || mask == 0x03U ||
			    mask == 0x1aU);
			assert(ax211_pci_get_le32(payload + 28U) <= 1U);
		}
	}
	if (request->command.group == INTEL_AX211_ASSOC_GROUP_MAC_CONFIG &&
	    request->command.opcode == INTEL_AX211_ASSOC_STATION_CONFIG_OPCODE) {
		const uint8_t *payload = request->payload;

		assert(request->command.version == 0U);
		assert(payload != NULL && request->payload_length ==
		    INTEL_AX211_ASSOC_STATION_CONFIG_SIZE);
		assert(ax211_pci_get_le32(payload) ==
		    INTEL_AX211_ASSOC_STATION_ID);
		assert(ax211_pci_get_le32(payload + 4U) ==
		    INTEL_AX211_ASSOC_LINK_ID);
		assert(ax211_pci_get_le32(payload + 24U) == 0U);
	}
	if (request->command.group == INTEL_AX211_ASSOC_GROUP_MAC_CONFIG &&
	    request->command.opcode == INTEL_AX211_ASSOC_STATION_REMOVE_OPCODE) {
		assert(request->command.version == 0U);
		assert(request->payload != NULL && request->payload_length ==
		    INTEL_AX211_ASSOC_REMOVE_STATION_SIZE);
		assert(ax211_pci_get_le32(request->payload) ==
		    INTEL_AX211_ASSOC_STATION_ID);
	}
	if (request->command.group == INTEL_AX211_ASSOC_GROUP_DATA_PATH &&
	    request->command.opcode == INTEL_AX211_ASSOC_RLC_CONFIG_OPCODE) {
		assert(request->command.version ==
		    INTEL_AX211_ASSOC_RLC_CONFIG_VERSION);
		assert(request->payload != NULL && request->payload_length ==
		    INTEL_AX211_ASSOC_RLC_CONFIG_SIZE);
		/* This fixture advertises RX chain A only. */
		assert(ax211_pci_get_le32(request->payload + 4U) == 0x1402U);
	}
	if (request->command.group == INTEL_AX211_ASSOC_GROUP_LEGACY &&
	    request->command.opcode == INTEL_AX211_ASSOC_MCAST_FILTER_OPCODE) {
		const uint8_t *payload = request->payload;

		assert(request->command.version ==
		    INTEL_AX211_ASSOC_MCAST_FILTER_VERSION);
		assert(payload != NULL && request->payload_length ==
		    INTEL_AX211_ASSOC_MCAST_FILTER_SIZE);
		assert(payload[0U] == 1U && payload[1U] == 0U &&
		    payload[2U] == 0U && payload[3U] == 1U);
		fixture_mcast_command_order = command_submit_count;
	} else if (request->command.group == INTEL_AX211_ASSOC_GROUP_LEGACY &&
	    request->command.opcode == INTEL_AX211_ASSOC_MAC_POWER_OPCODE) {
		const uint8_t *payload = request->payload;
		size_t index;

		assert(request->command.version == INTEL_AX211_ASSOC_MAC_POWER_VERSION);
		assert(payload != NULL && request->payload_length ==
		    INTEL_AX211_ASSOC_MAC_POWER_SIZE);
		for (index = 0U; index < 6U; index++)
			assert(payload[index] == 0U);
		assert(payload[6U] == 25U && payload[7U] == 0U);
		for (index = 8U; index < INTEL_AX211_ASSOC_MAC_POWER_SIZE; index++)
			assert(payload[index] == 0U);
		fixture_power_command_order = command_submit_count;
	}
	if (request->command.group == INTEL_AX211_KEY_GROUP &&
	    request->command.opcode == INTEL_AX211_KEY_OPCODE &&
	    request->payload != NULL && request->payload_length >= 4U) {
		assert(request->command.version == INTEL_AX211_KEY_WIRE_VERSION);
		assert(request->payload_length == INTEL_AX211_KEY_COMMAND_SIZE);
		if (((const uint8_t *)request->payload)[0U] == 1U) {
			fixture_key_add_count++;
			if (fixture_key_add_timeout_once) {
				fixture_key_add_timeout_once = 0U;
				suppress_response = 1;
			}
		} else if (((const uint8_t *)request->payload)[0U] == 3U)
			fixture_key_remove_count++;
	}
	if (!suppress_response) {
		memset(response, 0, sizeof(response));
		response_length = request->maximum_response_length;
		if (request->command.group == INTEL_AX211_ASSOC_GROUP_LEGACY &&
		    request->command.opcode == INTEL_AX211_ASSOC_MAC_POWER_OPCODE) {
			response_length = (size_t)((int)
			    INTEL_AX211_ASSOC_MAC_POWER_RESPONSE_SIZE +
			    fixture_power_response_length_delta);
			response[0U] = (uint8_t)fixture_power_status;
		}
		if (response_length == INTEL_AX211_ASSOC_QUEUE_RESPONSE_SIZE) {
			uint16_t queue;

			queue = fixture_command_malformed_queue ? 0U :
			    FIXTURE_TX_QUEUE;
			response[0U] = (uint8_t)queue;
			response[1U] = (uint8_t)(queue >> 8);
		}
		fixture_rx_event(request->command.opcode, request->command.group,
		    handle->token.queue, response, response_length);
		if (fixture_command_async_tx_before_response) {
			memcpy(fixture_pending_response_bytes, fixture_rx_bytes,
			    fixture_rx_length);
			fixture_pending_response_length = fixture_rx_length;
			fixture_pending_command_response = 1U;
			fixture_rx_event(INTEL_AX211_TX_OPCODE, FIXTURE_TX_EVENT_GROUP,
			    FIXTURE_TX_EVENT_QUEUE, fixture_async_tx_response,
			    sizeof(fixture_async_tx_response));
			fixture_rx_bytes[6U] = fixture_async_tx_index;
			fixture_command_async_tx_before_response = 0U;
		}
	}
	return INTEL_AX211_COMMAND_OK;
}

int
intel_ax211_command_complete(
	struct intel_ax211_command_transaction *transaction,
	const uint8_t *event_bytes,
	size_t event_length,
	uint32_t hardware_epoch,
	void *response,
	size_t response_capacity,
	size_t *response_length)
{
	struct intel_ax211_command_entry *entry;
	struct intel_ax211_event event;

	assert(transaction == &allocated_controller->runtime_start.commands);
	assert(event_bytes != NULL && response_length != NULL);
	if (hardware_epoch != transaction->hardware_epoch ||
	    intel_ax211_event_decode(event_bytes, event_length, &event) !=
	    INTEL_AX211_OK)
		return INTEL_AX211_COMMAND_STALE;
	entry = &transaction->entry[event.index];
	if (!entry->active || event.queue != entry->pending.queue ||
	    event.command.opcode != entry->pending.opcode ||
	    (event.flags & (uint8_t)~INTEL_AX211_PROTOCOL_COMMAND_FAILED_MASK) !=
	    entry->pending.group)
		return INTEL_AX211_COMMAND_STALE;
	if (event.payload_length > response_capacity)
		return INTEL_AX211_COMMAND_BUFFER_TOO_SMALL;
	if (event.payload_length != 0U)
		memcpy(response, event_bytes + event.payload_offset,
		    event.payload_length);
	*response_length = event.payload_length;
	entry->active = 0U;
	transaction->pending_count = 0U;
	command_complete_count++;
	return INTEL_AX211_COMMAND_OK;
}

int
intel_ax211_command_cancel(
	struct intel_ax211_command_transaction *transaction,
	const struct intel_ax211_command_handle *handle)
{
	assert(transaction != NULL && handle != NULL);
	transaction->entry[handle->token.index].active = 0U;
	transaction->pending_count = 0U;
	transaction->poisoned = 1U;
	return INTEL_AX211_COMMAND_OK;
}

int
intel_ax211_boot_init(
	struct intel_ax211_boot *boot,
	const struct intel_ax211_boot_ops *ops,
	void *argument,
	struct drv_dma_device *dma_device,
	struct intel_ax211_mmio *mmio,
	struct intel_ax211_transport *transport,
	uint16_t hardware_revision,
	uint16_t rf_type,
	uint32_t generation_seed)
{
	assert(boot != NULL && ops == &ax211_runtime_start_ops.boot);
	assert(argument == allocated_controller);
	assert(dma_device == drv_pci_device_dma(active_device));
	assert(mmio == initialized_mmio && transport == &allocated_controller->transport);
	assert(hardware_revision == (uint16_t)allocated_controller->hardware_revision);
	assert(rf_type == 0x10dU && generation_seed != 0U);
	memset(boot, 0, sizeof(*boot));
	boot->ops = ops;
	boot->argument = argument;
	boot->dma_device = dma_device;
	boot->mmio = mmio;
	boot->transport = transport;
	boot->hardware_revision = hardware_revision;
	boot->rf_type = rf_type;
	boot->generation = generation_seed;
	boot->state = INTEL_AX211_BOOT_STATE_IDLE;
	return INTEL_AX211_BOOT_OK;
}

int
intel_ax211_boot_run(
	struct intel_ax211_boot *boot,
	struct intel_ax211_protocol_nvm *nvm)
{
	int result;

	assert(boot != NULL && nvm != NULL);
	boot_run_count++;
	boot->generation++;
	boot->dma.device = boot->dma_device;
	boot->dma.boot_prepared = 1U;
	boot->dma.rx_buffer_count = INTEL_AX211_RX_RING_SIZE;
	result = boot->ops->receive_epoch_begin(boot->argument,
	    boot->generation);
	if (result == 0)
		result = boot->ops->transport_bind(boot->argument, &boot->dma,
		    boot->mmio, boot->transport, boot->generation);
	if (result == 0) {
		boot->transport->msix_configured = 1U;
		boot->transport->interrupts_enabled = 1U;
		result = boot->ops->interrupt_drain(boot->argument);
	}
	if (result != 0 || active_device->failure == FIXTURE_FAIL_BOOT_RUN) {
		boot->state = INTEL_AX211_BOOT_STATE_IDLE;
		return INTEL_AX211_BOOT_IO;
	}
	memset(boot->command_version_bytes, 0,
	    sizeof(boot->command_version_bytes));
	boot->command_table_valid = 1U;
	boot->state = INTEL_AX211_BOOT_STATE_COMPLETE;
	memset(nvm, 0, sizeof(*nvm));
	nvm->band_24_enabled = 1U;
	nvm->tx_chain_mask = 1U;
	nvm->rx_chain_mask = 1U;
	return INTEL_AX211_BOOT_OK;
}

int
intel_ax211_boot_cleanup(
	struct intel_ax211_boot *boot)
{
	assert(boot != NULL);
	boot->state = INTEL_AX211_BOOT_STATE_IDLE;
	return INTEL_AX211_BOOT_OK;
}

int
intel_ax211_boot_command_table(
	const struct intel_ax211_boot *boot,
	struct intel_ax211_protocol_command_table *table)
{
	assert(boot != NULL && table != NULL);
	if (boot->state != INTEL_AX211_BOOT_STATE_COMPLETE)
		return INTEL_AX211_BOOT_INVALID;
	table->bytes = boot->command_version_bytes;
	table->count = INTEL_AX211_PROTOCOL_API89_COMMAND_COUNT;
	return INTEL_AX211_BOOT_OK;
}

int
intel_ax211_runtime_start_init(
	struct intel_ax211_runtime_start *session,
	const struct intel_ax211_runtime_start_ops *ops,
	void *argument,
	struct drv_dma_device *dma_device,
	struct intel_ax211_mmio *mmio,
	struct intel_ax211_transport *transport,
	uint16_t hardware_revision,
	uint16_t rf_type,
	const struct intel_ax211_protocol_command_table *command_table,
	const struct intel_ax211_protocol_nvm *nvm,
	int ltr_enabled,
	uint32_t generation_seed)
{
	(void)command_table;
	(void)nvm;
	(void)ltr_enabled;
	assert(session != NULL && ops == &ax211_runtime_start_ops);
	assert(argument == allocated_controller);
	assert(dma_device == drv_pci_device_dma(active_device));
	assert(mmio == initialized_mmio && transport == &allocated_controller->transport);
	assert(hardware_revision == (uint16_t)allocated_controller->hardware_revision);
	assert(rf_type == 0x10dU && generation_seed != 0U);
	memset(session, 0, sizeof(*session));
	session->ops = ops;
	session->argument = argument;
	session->dma_device = dma_device;
	session->mmio = mmio;
	session->transport = transport;
	session->generation = generation_seed;
	session->state = INTEL_AX211_RUNTIME_START_STATE_IDLE;
	return INTEL_AX211_RUNTIME_START_OK;
}

int
intel_ax211_runtime_start_run(
	struct intel_ax211_runtime_start *session)
{
	int result;

	assert(session != NULL);
	runtime_run_count++;
	session->generation++;
	session->dma.device = session->dma_device;
	session->dma.boot_prepared = 1U;
	session->dma.rx_buffer_count = INTEL_AX211_RX_RING_SIZE;
	session->dma.rx_buffer[0U].address = fixture_rx_bytes;
	session->dma.rx_buffer[0U].size = sizeof(fixture_rx_bytes);
	session->dma.rx_buffer[0U].device_address = UINT64_C(0x1000);
	result = session->ops->boot.receive_epoch_begin(session->argument,
	    session->generation);
	if (result == 0)
		result = session->ops->boot.transport_bind(session->argument,
		    &session->dma, session->mmio, session->transport,
		    session->generation);
	if (result == 0) {
		session->transport->msix_configured = 1U;
		session->transport->interrupts_enabled = 1U;
		session->transport->rx_active = 1U;
	}
	if (result != 0 || active_device->failure == FIXTURE_FAIL_RUNTIME_RUN) {
		if (result == 0)
			(void)session->ops->boot.interrupt_drain(session->argument);
		session->state = INTEL_AX211_RUNTIME_START_STATE_IDLE;
		return INTEL_AX211_RUNTIME_START_IO;
	}
	memset(&session->commands, 0, sizeof(session->commands));
	session->commands.transport = session->transport;
	session->commands.max_pending = INTEL_AX211_COMMAND_MAX_PENDING;
	session->commands.next_generation = 1U;
	session->commands.initialized = 1U;
	session->commands.hardware_epoch = session->generation;
	session->commands_initialized = 1U;
	memset(session->command_version_bytes, 0x5a,
	    sizeof(session->command_version_bytes));
	memset(&session->nvm, 0, sizeof(session->nvm));
	session->nvm.band_24_enabled = 1U;
	session->nvm.tx_chain_mask = 1U;
	session->nvm.rx_chain_mask = 1U;
	memset(&session->mcc, 0, sizeof(session->mcc));
	session->mcc.channel_count = AX211_PASSIVE_CHANNEL_COUNT;
	session->mcc_valid = 1U;
	session->state = INTEL_AX211_RUNTIME_START_STATE_RUNNING;
	return INTEL_AX211_RUNTIME_START_OK;
}

int
intel_ax211_runtime_start_stop(
	struct intel_ax211_runtime_start *session)
{
	int result;

	assert(session != NULL);
	runtime_stop_count++;
	result = session->ops->boot.interrupt_drain(session->argument);
	if (result != 0) {
		session->state = INTEL_AX211_RUNTIME_START_STATE_STOP_REQUIRED;
		return INTEL_AX211_RUNTIME_START_STOP_REQUIRED;
	}
	session->state = INTEL_AX211_RUNTIME_START_STATE_IDLE;
	return INTEL_AX211_RUNTIME_START_OK;
}

int
intel_ax211_runtime_start_cleanup(
	struct intel_ax211_runtime_start *session)
{
	assert(session != NULL);
	return intel_ax211_runtime_start_stop(session);
}

int
intel_ax211_runtime_start_mcc(
	const struct intel_ax211_runtime_start *session,
	struct intel_ax211_runtime_mcc *mcc)
{
	assert(session != NULL && mcc != NULL);
	if (session->state != INTEL_AX211_RUNTIME_START_STATE_RUNNING ||
	    !session->mcc_valid)
		return INTEL_AX211_RUNTIME_START_INVALID;
	*mcc = session->mcc;
	return INTEL_AX211_RUNTIME_START_OK;
}

int
intel_ax211_rx_api89_validate(
	const struct intel_ax211_protocol_command_table *table)
{
	assert(table != NULL);
	assert(table->bytes == allocated_controller->runtime_start.
	    command_version_bytes);
	assert(table->count == INTEL_AX211_PROTOCOL_API89_COMMAND_COUNT);
	return INTEL_AX211_RX_OK;
}

int
intel_ax211_scan_session_init(
	struct intel_ax211_scan_session *session,
	struct intel_ax211_command_transaction *commands,
	const struct intel_ax211_protocol_command_table *command_table,
	const struct intel_ax211_protocol_nvm *nvm,
	const struct intel_ax211_runtime_mcc *mcc,
	const uint8_t station_address[6],
	uint32_t hardware_epoch)
{
	assert(session != NULL && commands != NULL && command_table != NULL);
	assert(nvm != NULL && mcc != NULL && station_address != NULL);
	assert(commands == &allocated_controller->runtime_start.commands);
	assert(command_table->count == INTEL_AX211_PROTOCOL_API89_COMMAND_COUNT);
	assert(nvm == &allocated_controller->runtime_start.nvm);
	assert(memcmp(station_address, published_device->hwaddr, 6U) == 0);
	assert(hardware_epoch == allocated_controller->hardware_epoch);
	scan_init_count++;
	if (active_device->failure == FIXTURE_FAIL_SCAN_INIT)
		return INTEL_AX211_SCAN_SESSION_UNSUPPORTED;
	memset(session, 0, sizeof(*session));
	session->commands = commands;
	session->hardware_epoch = hardware_epoch;
	session->initialized = 1U;
	session->phase = INTEL_AX211_SCAN_SESSION_IDLE;
	session->full_profile.channel_width_mhz =
	    INTEL_AX211_SCAN_CHANNEL_WIDTH_MHZ;
	session->full_profile.channel_count = 11U;
	for (size_t index = 0U; index < 10U; index++)
		session->full_profile.channel[index] = (uint8_t)(index + 1U);
	session->full_profile.channel[10U] = 36U;
	return INTEL_AX211_SCAN_SESSION_OK;
}

int
intel_ax211_scan_session_begin_channel(
	struct intel_ax211_scan_session *session,
	uint64_t common_generation,
	uint8_t channel,
	uint64_t now_us)
{
	assert(session != NULL && session->initialized);
	assert(common_generation != 0U && channel != 0U && now_us != 0U);
	scan_begin_count++;
	if (session->phase != INTEL_AX211_SCAN_SESSION_IDLE &&
	    session->phase != INTEL_AX211_SCAN_SESSION_TERMINAL)
		return INTEL_AX211_SCAN_SESSION_BUSY;
	session->common_generation = common_generation;
	session->channel = channel;
	session->command_deadline = now_us +
	    INTEL_AX211_SCAN_SESSION_COMMAND_TIMEOUT_US;
	session->phase = INTEL_AX211_SCAN_SESSION_WAIT_START_ACK;
	session->terminal_result = INTEL_AX211_SCAN_SESSION_OK;
	return INTEL_AX211_SCAN_SESSION_OK;
}

int
intel_ax211_scan_session_start_ack(
	struct intel_ax211_scan_session *session,
	const uint8_t *event_bytes,
	size_t event_length,
	uint32_t hardware_epoch,
	uint64_t now_us)
{
	assert(session != NULL && event_bytes != NULL && event_length >= 8U);
	assert(hardware_epoch == session->hardware_epoch && now_us != 0U);
	assert((event_bytes[7U] & 0x80U) == 0U);
	scan_start_ack_count++;
	if (session->phase != INTEL_AX211_SCAN_SESSION_WAIT_START_ACK)
		return INTEL_AX211_SCAN_SESSION_OUT_OF_ORDER;
	session->phase = INTEL_AX211_SCAN_SESSION_RUNNING;
	return INTEL_AX211_SCAN_SESSION_OK;
}

int
intel_ax211_scan_session_notification(
	struct intel_ax211_scan_session *session,
	const struct intel_ax211_protocol_message *message,
	uint64_t now_us,
	struct intel_ax211_scan_session_event *event)
{
	assert(session != NULL && message != NULL && event != NULL);
	assert(message->generation == session->hardware_epoch && now_us != 0U);
	assert(message->group == INTEL_AX211_SCAN_GROUP_LEGACY);
	assert(message->opcode == INTEL_AX211_SCAN_COMPLETE_OPCODE ||
	    message->opcode == INTEL_AX211_SCAN_ITERATION_COMPLETE_OPCODE);
	scan_notification_count++;
	if (session->phase != INTEL_AX211_SCAN_SESSION_RUNNING &&
	    session->phase != INTEL_AX211_SCAN_SESSION_WAIT_ABORT_COMPLETE)
		return INTEL_AX211_SCAN_SESSION_OUT_OF_ORDER;
	memset(event, 0, sizeof(*event));
	event->common_generation = session->common_generation;
	event->channel = session->channel;
	session->phase = INTEL_AX211_SCAN_SESSION_TERMINAL;
	session->terminal_result = (uint8_t)fixture_scan_notification_result;
	return fixture_scan_notification_result;
}

int
intel_ax211_scan_session_abort(
	struct intel_ax211_scan_session *session,
	uint64_t common_generation,
	uint64_t now_us)
{
	assert(session != NULL && common_generation != 0U && now_us != 0U);
	scan_abort_count++;
	if (common_generation != session->common_generation)
		return INTEL_AX211_SCAN_SESSION_STALE;
	if (session->phase != INTEL_AX211_SCAN_SESSION_RUNNING)
		return INTEL_AX211_SCAN_SESSION_OUT_OF_ORDER;
	session->phase = INTEL_AX211_SCAN_SESSION_WAIT_ABORT_ACK;
	session->command_deadline = now_us +
	    INTEL_AX211_SCAN_SESSION_ABORT_TIMEOUT_US;
	return INTEL_AX211_SCAN_SESSION_OK;
}

int
intel_ax211_scan_session_abort_ack(
	struct intel_ax211_scan_session *session,
	const uint8_t *event_bytes,
	size_t event_length,
	uint32_t hardware_epoch,
	uint64_t now_us)
{
	assert(session != NULL && event_bytes != NULL && event_length >= 8U);
	assert(hardware_epoch == session->hardware_epoch && now_us != 0U);
	assert((event_bytes[7U] & 0x80U) == 0U);
	scan_abort_ack_count++;
	if (session->phase != INTEL_AX211_SCAN_SESSION_WAIT_ABORT_ACK)
		return INTEL_AX211_SCAN_SESSION_OUT_OF_ORDER;
	session->phase = INTEL_AX211_SCAN_SESSION_TERMINAL;
	session->terminal_result = INTEL_AX211_SCAN_SESSION_ABORTED;
	return INTEL_AX211_SCAN_SESSION_ABORTED;
}

int
intel_ax211_scan_session_expire(
	struct intel_ax211_scan_session *session,
	uint64_t now_us)
{
	assert(session != NULL && session->initialized && now_us != 0U);
	if (fixture_scan_expire_result != INTEL_AX211_SCAN_SESSION_OK) {
		session->phase = INTEL_AX211_SCAN_SESSION_TERMINAL;
		session->terminal_result = (uint8_t)fixture_scan_expire_result;
	}
	return fixture_scan_expire_result;
}

int
intel_ax211_rx_mpdu_decode(
	const struct intel_ax211_protocol_message *message,
	uint32_t generation,
	uint8_t *output,
	size_t output_capacity,
	struct intel_ax211_rx_mpdu *mpdu)
{
	assert(message != NULL && output != NULL && mpdu != NULL);
	assert(message->group == INTEL_AX211_RX_MPDU_GROUP);
	assert(message->opcode == INTEL_AX211_RX_MPDU_OPCODE);
	assert(message->version == INTEL_AX211_RX_MPDU_NOTIFICATION_VERSION);
	static const uint8_t bssid[6] = {
		0x02U, 0x66U, 0x77U, 0x88U, 0x99U, 0xaaU
	};
	size_t frame_length;

	assert(message->generation == generation && output_capacity >= 48U);
	assert(message->payload != fixture_rx_bytes + 8U);
	assert(fixture_rx_bytes[8U] == 0xa5U);
	rx_decode_count++;
	if (fixture_rx_decode_result != INTEL_AX211_RX_OK)
		return fixture_rx_decode_result;
	frame_length = fixture_rx_frame_control == 0x0080U ? 48U : 24U;
	memset(output, 0, frame_length);
	output[0U] = (uint8_t)fixture_rx_frame_control;
	output[1U] = (uint8_t)(fixture_rx_frame_control >> 8);
	if (fixture_rx_frame_control == 0x0080U) {
		memset(output + 4U, 0xff, 6U);
		memcpy(output + 10U, bssid, sizeof(bssid));
		memcpy(output + 16U, bssid, sizeof(bssid));
		output[24U] = 0x11U;
		output[32U] = 100U;
		output[34U] = 0x20U;
		output[36U] = 0U;
		output[37U] = 1U;
		output[38U] = 'x';
		output[39U] = 3U;
		output[40U] = 1U;
		output[41U] = allocated_controller->scan_session.channel;
		output[42U] = 5U;
		output[43U] = 4U;
		output[44U] = 0U;
		output[45U] = 1U;
		output[46U] = 0U;
		output[47U] = 0U;
	}
	memset(mpdu, 0, sizeof(*mpdu));
	mpdu->frame = output;
	mpdu->length = frame_length;
	mpdu->rssi_dbm = -47;
	mpdu->channel = allocated_controller->scan_session.channel;
	mpdu->tsf = UINT64_C(0x1122334455667788);
	mpdu->gp2_on_air_rise = 1234U;
	mpdu->tsf_valid = 1U;
	mpdu->cipher = fixture_rx_cipher;
	mpdu->decrypted = fixture_rx_decrypted;
	mpdu->key_index = fixture_rx_key_index;
	mpdu->packet_number = fixture_rx_packet_number;
	return INTEL_AX211_RX_OK;
}

int
wlan_station_report_scan_channel_ready(
	struct wlan_station *station,
	uint64_t generation,
	uint32_t step_index)
{
	assert(station == published_station);
	assert(allocated_controller->lifecycle_lock.locked == 1U);
	scan_ready_reports++;
	scan_report_generation = generation;
	scan_report_step = step_index;
	return 0;
}

int
wlan_station_report_scan_frame(
	struct wlan_station *station,
	uint64_t generation,
	const uint8_t *frame,
	size_t length,
	int32_t rssi_dbm,
	uint8_t channel_hint)
{
	assert(station == published_station);
	assert(allocated_controller->lifecycle_lock.locked == 1U);
	assert(frame != NULL && length == 48U);
	scan_frame_reports++;
	scan_report_generation = generation;
	scan_report_channel = channel_hint;
	scan_report_rssi = rssi_dbm;
	return 0;
}

int
wlan_station_report_scan_error(
	struct wlan_station *station,
	uint64_t generation,
	int error)
{
	assert(station == published_station);
	assert(allocated_controller->lifecycle_lock.locked == 1U);
	assert(error != 0);
	scan_error_reports++;
	scan_report_generation = generation;
	scan_report_error = error;
	return 0;
}

int
wlan_station_report_tx_complete(
	struct wlan_station *station,
	uint64_t generation,
	uint64_t cookie,
	int acknowledged,
	int error)
{
	assert(station == published_station);
	assert(allocated_controller->lifecycle_lock.locked == 0U);
	assert(fixture_common_gate_held == 0U);
	assert(generation == allocated_controller->connection_generation);
	assert(acknowledged == 0 || acknowledged == 1);
	assert((acknowledged && error == 0) || (!acknowledged && error != 0));
	tx_completion_reports++;
	reported_tx_cookie = cookie;
	return 0;
}

int
wlan_station_report_frame(
	struct wlan_station *station,
	const struct wlan_radio_rx_frame *report)
{
	static const uint8_t nested_payload[] = { 0x7eU };
	uint8_t frame0;
	uint8_t frame1;

	assert(station == published_station && report != NULL);
	assert(allocated_controller->lifecycle_lock.locked == 0U);
	assert(report->frame != NULL && report->length != 0U);
	frame0 = report->frame[0U];
	frame1 = report->frame[1U];
	if (fixture_poll_overlap) {
		fixture_poll_overlap = 0U;
		fixture_rx_event(0x7fU, 0U, 0x80U, nested_payload,
		    sizeof(nested_payload));
		assert(published_device->ops->poll_receive(published_device, 1U) ==
		    0U);
		assert(report->frame[0U] == frame0 && report->frame[1U] == frame1);
		fixture_nested_poll_count++;
	}
	rx_frame_reports++;
	reported_rx_generation = report->generation;
	return fixture_frame_report_result;
}

int
wlan_station_report_link_loss(
	struct wlan_station *station,
	uint64_t generation,
	int error)
{
	int result;

	assert(station == published_station);
	assert(allocated_controller->lifecycle_lock.locked == 0U);
	assert(generation != 0U && error > 0);
	link_loss_reports++;
	reported_link_loss_generation = generation;
	reported_link_loss_error = error;
	/* Model common's controlled retirement re-entering the private radio
	 * boundary while the poll owner keeps only its station lifetime pin. */
	result = station->ops->disconnect(station->radio_context, generation);
	return result == ENOTCONN ? 0 : result;
}

int
wlan_station_transmit(
	struct wlan_station *station,
	struct packet_buf *packet)
{
	assert(station == published_station && packet != NULL);
	packet_buf_free(packet);
	return 0;
}


static void
test_registration_and_exact_match(void)
{
	struct drv_pci_device device;

	fixture_reset(&device);
	registered_driver = NULL;
	assert(drv_pci_intel_ax211_driver_register() == 0);
	assert(registered_driver == &ax211_pci_driver);
	assert(registered_driver->id_count == 1U);
	assert(registered_driver->ids[0].vendor == 0x8086U);
	assert(registered_driver->ids[0].device == 0x51f0U);
	assert(registered_driver->ids[0].subvendor == 0x8086U);
	assert(registered_driver->ids[0].subdevice == 0x4090U);
	assert(registered_driver->ids[0].class_code == 0x028000U);
	assert(registered_driver->ids[0].class_mask == 0xffffffU);
	assert(ax211_pci_match(&device, &ax211_pci_ids[0]) ==
	    DRV_PCI_MATCH_EXACT);
	device.vendor++;
	assert(ax211_pci_match(&device, &ax211_pci_ids[0]) ==
	    DRV_PCI_MATCH_NONE);
	fixture_reset(&device);
	device.product++;
	assert(ax211_pci_match(&device, &ax211_pci_ids[0]) ==
	    DRV_PCI_MATCH_NONE);
	fixture_reset(&device);
	device.subvendor++;
	assert(ax211_pci_match(&device, &ax211_pci_ids[0]) ==
	    DRV_PCI_MATCH_NONE);
	fixture_reset(&device);
	device.subproduct++;
	assert(ax211_pci_match(&device, &ax211_pci_ids[0]) ==
	    DRV_PCI_MATCH_NONE);
	fixture_reset(&device);
	device.revision++;
	assert(ax211_pci_match(&device, &ax211_pci_ids[0]) ==
	    DRV_PCI_MATCH_NONE);
	fixture_reset(&device);
	device.class_code++;
	assert(ax211_pci_match(&device, &ax211_pci_ids[0]) ==
	    DRV_PCI_MATCH_NONE);
}

static void
test_duplicate_registration_retains_controller(void)
{
	struct ax211_pci_controller *controller;
	struct drv_pci_device device;
	size_t before;

	fixture_reset(&device);
	assert(drv_pci_intel_ax211_driver_register() == 0);
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == 0);
	controller = device.driver_data;
	assert(controller != NULL);
	ax211_refresh_epoch = 41U;
	before = device.event_count;
	assert(drv_pci_intel_ax211_driver_register() == EEXIST);
	assert(device.event_count == before);
	assert(ax211_controllers == controller);
	assert(ax211_refresh_epoch == 41U);
	assert(controller->listed == 1U);
	assert(controller->ready == 1U);
	drv_pci_intel_ax211_devices_ready();
	assert(published_device != NULL);
	assert(ax211_pci_detach(&device, 0U) == 0);
}

static void
test_attach_persists_until_refresh(void)
{
	struct drv_pci_device device;
	struct ax211_pci_controller *controller;

	fixture_reset(&device);
	assert(drv_pci_intel_ax211_driver_register() == 0);
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == 0);
	controller = device.driver_data;
	assert(controller != NULL);
	assert(controller == allocated_controller);
	assert(strcmp(device.events, "CBSQMEQRKID") == 0);
	assert(device.claimed == 1U);
	assert(device.mapped == 1U);
	assert((device.command & 0x0002U) != 0U);
	assert((device.command & 0x0004U) == 0U);
	assert(device.map_flags == (DRV_PCI_MAP_READ | DRV_PCI_MAP_WRITE |
	    DRV_PCI_MAP_NOCACHE));
	assert(controller->backend.registers ==
	    (volatile uint8_t *)device.registers);
	assert(initialized_mmio == &controller->mmio);
	assert(published_device == NULL);
	assert(published_station == NULL);
	assert(read_barrier_count == 2U);
	assert(ax211_pci_detach(&device, 0U) == 0);
	assert(strcmp(device.events, "CBSQMEQRKIDUbTLdF") == 0);
	assert(device.driver_data == NULL);
	assert(device.claimed == 0U);
	assert(device.mapped == 0U);
	assert(device.command == 0x0005U);
	assert(controller_frees == 1U);
}

static void
test_strap_publication_and_reverse_detach(void)
{
	struct drv_pci_device device;
	const struct net_device_ops *operations;
	size_t before;

	fixture_reset(&device);
	device.map_reassigns_bar = 1U;
	assert(drv_pci_intel_ax211_driver_register() == 0);
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == 0);
	drv_pci_intel_ax211_devices_ready();
	assert(strstr(device.events, "PZAHONW") != NULL);
	assert(net_creates == 1U);
	assert(station_attaches == 1U);
	assert(published_device != NULL);
	assert(published_station != NULL);
	assert(memcmp(published_device->hwaddr, strap_mac, 6U) == 0);
	assert(published_device->hwaddr_len == 6U);
	assert(strcmp(published_device->name, "wlan0") == 0);
	assert(published_device->capabilities == NET_DEVICE_CAP_WLAN);
	assert((published_device->flags & NET_DEVICE_UP) == 0U);
	assert(published_device->carrier == 0U);
	operations = published_device->ops;
	assert(operations == &ax211_net_ops);
	assert(operations->open(published_device) == 0);
	assert(allocated_controller->runtime_active == 1U);
	assert(allocated_controller->scan_initialized == 1U);
	assert(allocated_controller->irq_established == 1U);
	assert(device.irq_established == 1U);
	assert(boot_run_count == 1U);
	assert(runtime_run_count == 1U);
	assert(scan_init_count == 1U);
	assert(transport_bind_count == 2U);
	assert(carrier_updates != 0U);
	before = device.event_count;
	drv_pci_intel_ax211_devices_ready();
	assert(device.event_count == before);
	assert(net_creates == 1U);
	assert(station_attaches == 1U);
	assert(ax211_pci_detach(&device, 0U) == 0);
	assert(strstr(device.events, "qJKkjQqJK") != NULL);
	assert(device.bar.bus_address == 0xf0810000U);
	assert(device.command == 0x0005U);
	assert(station_detaches == 1U);
	assert(radio_quiesce_callbacks == 1U);
	assert(net_destroys == 1U);
	assert(controller_frees == 1U);
	assert(strstr(log_buffer, "02:11:22:33:44:55") == NULL);
	assert(strstr(log_buffer, "02:aa:bb:cc:dd:ee") == NULL);
}

static void
test_otp_fallback_and_retry(void)
{
	struct drv_pci_device device;

	fixture_reset(&device);
	strap_valid = 0U;
	assert(drv_pci_intel_ax211_driver_register() == 0);
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == 0);
	drv_pci_intel_ax211_devices_ready();
	assert(published_device != NULL);
	assert(memcmp(published_device->hwaddr, otp_mac, 6U) == 0);
	assert(ax211_pci_detach(&device, 0U) == 0);

	fixture_reset(&device);
	strap_valid = 0U;
	otp_valid = 0U;
	assert(drv_pci_intel_ax211_driver_register() == 0);
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == 0);
	drv_pci_intel_ax211_devices_ready();
	assert(published_device == NULL);
	assert(station_attaches == 0U);
	assert(strstr(log_buffer, "publication failed") != NULL);
	otp_valid = 1U;
	drv_pci_intel_ax211_devices_ready();
	assert(published_device != NULL);
	assert(memcmp(published_device->hwaddr, otp_mac, 6U) == 0);
	assert(net_creates == 1U);
	assert(ax211_pci_detach(&device, 0U) == 0);
}

static void
test_partial_publication_retry(void)
{
	struct ax211_pci_controller *controller;
	struct drv_pci_device device;

	fixture_reset(&device);
	fail_station_attach = 1U;
	fail_net_gone = 1U;
	assert(drv_pci_intel_ax211_driver_register() == 0);
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == 0);
	controller = device.driver_data;
	drv_pci_intel_ax211_devices_ready();
	assert(net_creates == 1U);
	assert(net_destroys == 0U);
	assert(published_device != NULL);
	assert(published_station == NULL);
	assert(controller->net_device == published_device);
	assert(controller->net_live == 1U);
	assert(published_device->driver_data == controller);
	assert(controller->listed == 1U);
	fail_station_attach = 0U;
	fail_net_gone = 0U;
	drv_pci_intel_ax211_devices_ready();
	assert(net_creates == 2U);
	assert(net_destroys == 1U);
	assert(published_device != NULL);
	assert(published_station != NULL);
	assert(ax211_pci_detach(&device, 0U) == 0);
	assert(net_destroys == 2U);
	assert(controller_frees == 1U);
}

static void
test_checked_detach_retry(void)
{
	struct drv_pci_device device;
	size_t before;

	fixture_reset(&device);
	assert(drv_pci_intel_ax211_driver_register() == 0);
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == 0);
	drv_pci_intel_ax211_devices_ready();
	fail_station_close = 1U;
	assert(ax211_pci_detach(&device, 0U) == ETIMEDOUT);
	assert(controller_frees == 0U);
	assert(device.driver_data != NULL);
	before = device.event_count;
	drv_pci_intel_ax211_devices_ready();
	assert(device.event_count == before);
	fail_station_close = 0U;
	fail_driver_data_clear = 1U;
	assert(ax211_pci_detach(&device, 0U) == EIO);
	assert(controller_frees == 0U);
	assert(device.driver_data != NULL);
	assert(device.claimed == 0U);
	assert(device.mapped == 0U);
	fail_driver_data_clear = 0U;
	assert(ax211_pci_detach(&device, 0U) == 0);
	assert(controller_frees == 1U);
	assert(device.driver_data == NULL);
}

static void
test_attach_restore_failure_quarantine(void)
{
	struct ax211_pci_controller *controller;
	struct drv_pci_device device;
	size_t before;

	fixture_reset(&device);
	device.map_reassigns_bar = 1U;
	device.fail_bar_restore = 1U;
	device.registers[0x028U / sizeof(uint32_t)] = 0x00000440U;
	assert(drv_pci_intel_ax211_driver_register() == 0);
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == 0);
	controller = device.driver_data;
	assert(controller != NULL);
	assert(controller->quarantined == 1U);
	assert(controller->ready == 0U);
	assert(controller->listed == 1U);
	assert(device.claimed == 1U);
	assert(device.mapped == 0U);
	assert((device.command & 0x0004U) == 0U);
	assert(device.bar.bus_address == 0xf0820000U);
	before = device.event_count;
	drv_pci_intel_ax211_devices_ready();
	assert(device.event_count == before);
	assert(published_device == NULL);
	device.fail_bar_restore = 0U;
	assert(ax211_pci_detach(&device, 0U) == 0);
	assert(device.bar.bus_address == 0xf0810000U);
	assert(device.command == 0x0005U);
	assert(device.claimed == 0U);
	assert(controller_frees == 1U);

	fixture_reset(&device);
	device.fail_state_restore = 1U;
	device.registers[0x028U / sizeof(uint32_t)] = 0x00000440U;
	assert(drv_pci_intel_ax211_driver_register() == 0);
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == 0);
	controller = device.driver_data;
	assert(controller != NULL);
	assert(controller->quarantined == 1U);
	assert(controller->state_saved == 1U);
	assert(controller->bar_claimed == 1U);
	assert((device.command & 0x0004U) == 0U);
	before = device.event_count;
	drv_pci_intel_ax211_devices_ready();
	assert(device.event_count == before);
	device.fail_state_restore = 0U;
	assert(ax211_pci_detach(&device, 0U) == 0);
	assert(device.command == 0x0005U);
	assert(device.claimed == 0U);
	assert(controller_frees == 1U);
	assert(strstr(log_buffer, "02:11:22:33:44:55") == NULL);
}

static void
test_attach_failure_unwind(void)
{
	static const enum fixture_failure failures[] = {
		FIXTURE_FAIL_CLAIM,
		FIXTURE_FAIL_BAR,
		FIXTURE_FAIL_SAVE,
		FIXTURE_FAIL_MASTER,
		FIXTURE_FAIL_MAP,
		FIXTURE_FAIL_ENABLE,
		FIXTURE_FAIL_COMMAND_READ
	};
	struct drv_pci_device device;
	size_t index;

	fixture_reset(&device);
	fail_controller_alloc = 1U;
	assert(drv_pci_intel_ax211_driver_register() == 0);
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == ENOMEM);
	assert(device.event_count == 0U);

	for (index = 0U; index < sizeof(failures) / sizeof(failures[0]);
	    index++) {
		fixture_reset(&device);
		device.failure = failures[index];
		assert(drv_pci_intel_ax211_driver_register() == 0);
		assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) != 0);
		assert(device.claimed == 0U);
		assert(device.mapped == 0U);
		assert(device.driver_data == NULL);
		assert(controller_frees == 1U);
	}

	fixture_reset(&device);
	device.registers[0x028U / sizeof(uint32_t)] = 0x00000440U;
	assert(drv_pci_intel_ax211_driver_register() == 0);
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == ENODEV);
	assert(device.claimed == 0U);
	assert(device.mapped == 0U);
	assert(controller_frees == 1U);

	fixture_reset(&device);
	device.mapping_size = 0x3fffU;
	assert(drv_pci_intel_ax211_driver_register() == 0);
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == EIO);
	assert(device.claimed == 0U);
	assert(device.mapped == 0U);
	assert(controller_frees == 1U);
}

static void
fixture_rx_event(
	uint8_t opcode,
	uint8_t group,
	uint8_t queue,
	const uint8_t *payload,
	size_t payload_length)
{
	size_t frame_length;

	assert(payload_length <= sizeof(fixture_rx_bytes) - 8U);
	assert(payload != NULL || payload_length == 0U);
	memset(fixture_rx_bytes, 0, sizeof(fixture_rx_bytes));
	frame_length = 4U + payload_length;
	fixture_rx_bytes[0U] = (uint8_t)frame_length;
	fixture_rx_bytes[1U] = (uint8_t)(frame_length >> 8);
	fixture_rx_bytes[4U] = opcode;
	fixture_rx_bytes[5U] = group;
	fixture_rx_bytes[6U] = 3U;
	fixture_rx_bytes[7U] = queue;
	if (payload_length != 0U)
		memcpy(fixture_rx_bytes + 8U, payload, payload_length);
	fixture_rx_length = frame_length + 4U;
	fixture_rx_ready = 1U;
}

static void
fixture_seed_bss(
	struct ax211_pci_controller *controller,
	const uint8_t bssid[6])
{
	struct intel_ax211_bss_entry entry;

	assert(controller != NULL && bssid != NULL);
	assert(intel_ax211_bss_cache_init(&controller->bss_published_cache,
	    controller->hardware_epoch) == INTEL_AX211_BSS_OK);
	controller->bss_published_initialized = 1U;
	controller->bss_published_generation = 1U;
	memset(&entry, 0, sizeof(entry));
	memcpy(entry.bssid, bssid, sizeof(entry.bssid));
	entry.observation_generation = 1U;
	entry.frame_timestamp = 1U;
	entry.receive_tsf = 1U;
	entry.hardware_epoch = controller->hardware_epoch;
	entry.gp2_on_air_rise = 1U;
	entry.last_seen_ticks = 1U;
	entry.beacon_interval_tu = 100U;
	entry.capability = 0x20U;
	entry.channel = 1U;
	entry.dtim_period = 1U;
	entry.tim_valid = 1U;
	entry.wmm_present = 1U;
	entry.receive_tsf_valid = 1U;
	entry.source = INTEL_AX211_BSS_SOURCE_BEACON;
	entry.valid = 1U;
	assert(intel_ax211_bss_cache_observe(&controller->bss_published_cache,
	    &entry) ==
	    INTEL_AX211_BSS_OK);
}

static void
test_open_failure_unwind(void)
{
	static const enum fixture_failure failures[] = {
		FIXTURE_FAIL_IRQ_ALLOCATE,
		FIXTURE_FAIL_IRQ_WRONG_TYPE,
		FIXTURE_FAIL_IRQ_ESTABLISH,
		FIXTURE_FAIL_BOOT_RUN,
		FIXTURE_FAIL_RUNTIME_RUN,
		FIXTURE_FAIL_SCAN_INIT
	};
	struct ax211_pci_controller *controller;
	struct drv_pci_device device;
	size_t index;

	for (index = 0U; index < sizeof(failures) / sizeof(failures[0]);
	    index++) {
		fixture_reset(&device);
		assert(drv_pci_intel_ax211_driver_register() == 0);
		assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == 0);
		drv_pci_intel_ax211_devices_ready();
		controller = device.driver_data;
		assert(controller != NULL && published_device != NULL);
		device.failure = failures[index];
		assert(published_device->ops->open(published_device) != 0);
		assert(controller->runtime_active == 0U);
		assert(controller->irq_established == 0U);
		assert(controller->irq_allocated == 0U);
		assert(controller->active_dma == NULL);
		assert(controller->receive_enabled == 0U);
		assert(device.irq_established == 0U);
		assert(device.irq_allocated == 0U);
		assert((device.command & 0x0004U) == 0U);
		device.failure = FIXTURE_FAIL_NONE;
		assert(ax211_pci_detach(&device, 0U) == 0);
		assert(controller_frees == 1U);
	}

	fixture_reset(&device);
	assert(drv_pci_intel_ax211_driver_register() == 0);
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == 0);
	drv_pci_intel_ax211_devices_ready();
	coherent_dma = 0U;
	assert(published_device->ops->open(published_device) == EIO);
	assert(boot_run_count == 0U);
	assert(device.irq_allocated == 0U);
	coherent_dma = 1U;
	assert(ax211_pci_detach(&device, 0U) == 0);

	/* Common admission is the last open edge; reject it by reversing all
	 * already-live firmware, IRQ, and DMA ownership. */
	fixture_reset(&device);
	assert(drv_pci_intel_ax211_driver_register() == 0);
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == 0);
	drv_pci_intel_ax211_devices_ready();
	controller = device.driver_data;
	fail_station_open = 1U;
	assert(published_device->ops->open(published_device) == EIO);
	assert(station_opens == 1U);
	assert(controller->runtime_active == 0U);
	assert(controller->operation_admission_open == 0U);
	assert(controller->active_dma == NULL);
	assert(controller->irq_established == 0U);
	assert(dma_allocations == dma_frees);
	fail_station_open = 0U;
	assert(ax211_pci_detach(&device, 0U) == 0);
}

static void
test_checked_runtime_drain_retry(void)
{
	struct ax211_pci_controller *controller;
	struct drv_pci_device device;

	fixture_reset(&device);
	assert(drv_pci_intel_ax211_driver_register() == 0);
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == 0);
	drv_pci_intel_ax211_devices_ready();
	controller = device.driver_data;
	assert(published_device->ops->open(published_device) == 0);
	assert(controller->runtime_active == 1U);
	device.failure = FIXTURE_FAIL_IRQ_DRAIN;
	published_device->ops->close(published_device);
	assert(controller->quarantined == 1U);
	assert(controller->runtime_start.state ==
	    INTEL_AX211_RUNTIME_START_STATE_STOP_REQUIRED);
	assert(controller->irq_established == 1U);
	assert(controller->active_dma != NULL);
	assert(device.irq_established == 1U);
	device.failure = FIXTURE_FAIL_NONE;
	assert(ax211_pci_detach(&device, 0U) == 0);
	assert(device.irq_disestablished == 2U);
	assert(device.irq_freed == 2U);
	assert(runtime_stop_count == 2U);
	assert(controller_frees == 1U);
}

static void
test_common_up_down_up_lifecycle(void)
{
	struct ax211_pci_controller *controller;
	struct drv_pci_device device;

	fixture_reset(&device);
	assert(drv_pci_intel_ax211_driver_register() == 0);
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == 0);
	drv_pci_intel_ax211_devices_ready();
	controller = device.driver_data;
	assert(published_device->ops->open(published_device) == 0);
	assert(station_opens == 1U && published_station->live == 1U);
	assert(controller->runtime_active == 1U);
	assert(controller->operation_admission_open == 1U);
	fixture_station_close_busy_retries = 2U;
	published_device->ops->close(published_device);
	assert(fixture_station_close_busy_retries == 0U);
	assert(fixture_yield_count == 2U);
	assert(station_closes == 3U && published_station->live == 0U);
	assert(controller->runtime_active == 0U);
	assert(controller->operation_admission_open == 0U);
	assert(controller->active_dma == NULL);
	assert(dma_allocations == dma_frees);
	assert(published_device->ops->open(published_device) == 0);
	assert(station_opens == 2U && published_station->live == 1U);
	assert(controller->runtime_active == 1U);
	assert(controller->operation_admission_open == 1U);
	published_device->ops->close(published_device);
	assert(station_closes == 4U && published_station->live == 0U);
	assert(controller->runtime_active == 0U);
	assert(dma_allocations == dma_frees);
	assert(ax211_pci_detach(&device, 0U) == 0);
}

static void
test_transmit_lease_detach_join(void)
{
	struct ax211_pci_controller *controller;
	struct wlan_station *station;
	struct drv_pci_device device;

	fixture_reset(&device);
	assert(drv_pci_intel_ax211_driver_register() == 0);
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == 0);
	drv_pci_intel_ax211_devices_ready();
	controller = device.driver_data;
	assert(published_device->ops->open(published_device) == 0);
	station = NULL;
	mutex_lock(&controller->lifecycle_lock);
	assert(ax211_pci_operation_enter_locked(controller, &station) == 0);
	assert(station == published_station && controller->operations_active == 1U);
	mutex_unlock(&controller->lifecycle_lock);
	fixture_release_operation_on_yield = 1U;
	assert(ax211_pci_detach(&device, 0U) == 0);
	assert(fixture_release_operation_on_yield == 0U);
	assert(fixture_yield_count != 0U);
	assert(station_detaches == 1U);
	assert(controller_frees == 1U);
}

static void
test_malformed_rx_is_dropped(void)
{
	static const uint8_t bssid[6] = {
		0x02U, 0x66U, 0x77U, 0x88U, 0x99U, 0xaaU
	};
	static const uint8_t payload[] = { 0x5aU };
	struct ax211_pci_controller *controller;
	struct wlan_bss_record bss;
	struct drv_pci_device device;
	uint64_t deadline;

	fixture_reset(&device);
	assert(drv_pci_intel_ax211_driver_register() == 0);
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == 0);
	drv_pci_intel_ax211_devices_ready();
	controller = device.driver_data;
	assert(published_device->ops->open(published_device) == 0);
	fixture_seed_bss(controller, bssid);
	memset(&bss, 0, sizeof(bss));
	memcpy(bss.bssid, bssid, sizeof(bssid));
	bss.channel = 1U;
	deadline = clock_ticks() + 100U;
	assert(published_station->ops->connect_start(controller, 71U, &bss,
	    deadline) == 0);
	assert(published_station->ops->association_set(controller, 71U, bssid,
	    1U, deadline) == 0);
	assert(net_device_set_carrier(published_device, 1) == 0);
	fixture_rx_decode_result = INTEL_AX211_RX_TRUNCATED;
	fixture_rx_event(INTEL_AX211_RX_MPDU_OPCODE, INTEL_AX211_RX_MPDU_GROUP,
	    0x80U, payload, sizeof(payload));
	assert(published_device->ops->poll_receive(published_device, 1U) == 1U);
	assert(controller->poll_active == 0U);
	assert(controller->runtime_active == 1U);
	assert(controller->operation_admission_open == 1U);
	assert(controller->quarantined == 0U);
	assert(link_loss_reports == 0U);
	assert(published_device->carrier == 1U);
	assert(controller->active_dma != NULL);
	assert(controller->irq_established == 1U);
	assert(device.irq_established == 1U);
	assert((device.command & AX211_PCI_COMMAND_MASTER) != 0U);
	fixture_rx_decode_result = INTEL_AX211_RX_OK;
	assert(ax211_pci_detach(&device, 0U) == 0);
	assert(dma_allocations == dma_frees);
}

static void
test_failed_key_add_scrubs_plaintext(void)
{
	static const uint8_t bssid[6] = {
		0x02U, 0x66U, 0x77U, 0x88U, 0x99U, 0xaaU
	};
	struct ax211_pci_controller *controller;
	struct wlan_radio_key_request key;
	struct wlan_bss_record bss;
	struct drv_pci_device device;
	uint64_t deadline;
	size_t index;

	fixture_reset(&device);
	assert(drv_pci_intel_ax211_driver_register() == 0);
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == 0);
	drv_pci_intel_ax211_devices_ready();
	controller = device.driver_data;
	assert(published_device->ops->open(published_device) == 0);
	fixture_seed_bss(controller, bssid);
	memset(&bss, 0, sizeof(bss));
	memcpy(bss.bssid, bssid, sizeof(bssid));
	bss.channel = 1U;
	deadline = clock_ticks() + 100U;
	assert(published_station->ops->connect_start(controller, 81U, &bss,
	    deadline) == 0);
	assert(published_station->ops->association_set(controller, 81U, bssid,
	    1U, deadline) == 0);

	memset(&key, 0, sizeof(key));
	key.generation = 81U;
	key.key_generation = 82U;
	key.deadline_ticks = clock_ticks() + 1U;
	key.kind = WLAN_RADIO_KEY_PAIRWISE;
	memcpy(key.address, bssid, sizeof(bssid));
	memset(key.key, 0x9a, sizeof(key.key));
	fixture_key_add_timeout_once = 1U;
	device.failure = FIXTURE_FAIL_IRQ_DRAIN;
	assert(published_station->ops->key_install(controller, &key) != 0);
	assert(fixture_key_add_timeout_once == 0U);
	assert(controller->quarantined == 1U);
	assert(controller->runtime_active == 0U);
	assert(controller->active_dma != NULL);
	assert(controller->staged_pairwise_key.valid == 1U);
	assert(controller->staged_pairwise_key.programmed == 1U);
	for (index = 0U;
	    index < sizeof(controller->staged_pairwise_key.request.key);
	    index++)
		assert(controller->staged_pairwise_key.request.key[index] == 0U);
	device.failure = FIXTURE_FAIL_NONE;
	assert(ax211_pci_detach(&device, 0U) == 0);
}

static void
test_tx_kick_schedules_recovery(void)
{
	static const uint8_t bssid[6] = {
		0x02U, 0x66U, 0x77U, 0x88U, 0x99U, 0xaaU
	};
	static const uint8_t payload[] = { 0x5aU };
	struct ax211_pci_controller *controller;
	struct wlan_radio_tx_request transmit;
	struct wlan_bss_record bss;
	struct drv_pci_device device;
	uint8_t frame[24];
	uint64_t deadline;
	unsigned decode_count;

	fixture_reset(&device);
	assert(drv_pci_intel_ax211_driver_register() == 0);
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == 0);
	drv_pci_intel_ax211_devices_ready();
	controller = device.driver_data;
	assert(published_device->ops->open(published_device) == 0);
	fixture_seed_bss(controller, bssid);
	memset(&bss, 0, sizeof(bss));
	memcpy(bss.bssid, bssid, sizeof(bssid));
	bss.channel = 1U;
	deadline = clock_ticks() + 100U;
	assert(published_station->ops->connect_start(controller, 91U, &bss,
	    deadline) == 0);
	assert(published_station->ops->association_set(controller, 91U, bssid,
	    1U, deadline) == 0);
	assert(net_device_set_carrier(published_device, 1) == 0);

	memset(frame, 0, sizeof(frame));
	frame[0U] = 0xb0U;
	memset(&transmit, 0, sizeof(transmit));
	transmit.generation = 91U;
	transmit.cookie = 92U;
	transmit.deadline_ticks = deadline;
	transmit.frame_class = WLAN_RADIO_FRAME_MANAGEMENT;
	transmit.frame = frame;
	transmit.length = sizeof(frame);
	fixture_tx_kick_failure = 1U;
	assert(published_station->ops->frame_transmit(controller, &transmit) ==
	    EIO);
	fixture_tx_kick_failure = 0U;
	assert(controller->recovery_pending == 1U);
	assert(controller->operation_admission_open == 0U);
	assert(controller->runtime_active == 1U);
	assert(device.poll_scheduled != 0U);
	/* A latched fatal edge is handled before any later RX/TX notification.
	 * Dispatching one could re-enter common state while its runtime is already
	 * known to be poisoned. */
	decode_count = rx_decode_count;
	fixture_rx_event(INTEL_AX211_RX_MPDU_OPCODE, INTEL_AX211_RX_MPDU_GROUP,
	    0x80U, payload, sizeof(payload));
	assert(published_device->ops->poll_receive(published_device, 1U) == 0U);
	assert(rx_decode_count == decode_count);
	assert(link_loss_reports == 1U);
	assert(reported_link_loss_generation == 91U);
	assert(reported_link_loss_error == EIO);
	assert(published_device->carrier == 0U);
	assert(controller->runtime_active == 0U);
	assert(controller->quarantined == 1U);
	assert(controller->active_dma == NULL);
	assert(dma_allocations == dma_frees);
	assert(ax211_pci_detach(&device, 0U) == 0);
}

static void
test_recovery_join_retries_without_free(void)
{
	static const uint8_t bssid[6] = {
		0x02U, 0x66U, 0x77U, 0x88U, 0x99U, 0xaaU
	};
	struct ax211_pci_controller *controller;
	struct wlan_radio_tx_request transmit;
	struct wlan_station *station;
	struct wlan_bss_record bss;
	struct drv_pci_device device;
	uint8_t frame[24];
	uint64_t deadline;
	unsigned frees;

	fixture_reset(&device);
	assert(drv_pci_intel_ax211_driver_register() == 0);
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == 0);
	drv_pci_intel_ax211_devices_ready();
	controller = device.driver_data;
	assert(published_device->ops->open(published_device) == 0);
	fixture_seed_bss(controller, bssid);
	memset(&bss, 0, sizeof(bss));
	memcpy(bss.bssid, bssid, sizeof(bssid));
	bss.channel = 1U;
	deadline = clock_ticks() + 100U;
	assert(published_station->ops->connect_start(controller, 93U, &bss,
	    deadline) == 0);
	assert(published_station->ops->association_set(controller, 93U, bssid,
	    1U, deadline) == 0);
	assert(net_device_set_carrier(published_device, 1) == 0);

	/* Model a common caller that copied station state before the fatal TX
	 * edge.  Recovery may lower carrier, but must not reset or free DMA until
	 * this lease has actually retired. */
	station = NULL;
	mutex_lock(&controller->lifecycle_lock);
	assert(ax211_pci_operation_enter_locked(controller, &station) == 0);
	mutex_unlock(&controller->lifecycle_lock);
	assert(station == published_station);
	memset(frame, 0, sizeof(frame));
	frame[0U] = 0xb0U;
	memset(&transmit, 0, sizeof(transmit));
	transmit.generation = 93U;
	transmit.cookie = 94U;
	transmit.deadline_ticks = deadline;
	transmit.frame_class = WLAN_RADIO_FRAME_MANAGEMENT;
	transmit.frame = frame;
	transmit.length = sizeof(frame);
	fixture_tx_kick_failure = 1U;
	assert(published_station->ops->frame_transmit(controller, &transmit) ==
	    EIO);
	fixture_tx_kick_failure = 0U;
	frees = dma_frees;
	assert(published_device->ops->poll_receive(published_device, 1U) == 0U);
	assert(controller->recovery_pending == 1U);
	assert(controller->runtime_active == 1U);
	assert(controller->active_dma != NULL);
	assert(controller->irq_established == 1U);
	assert(dma_frees == frees);
	assert(link_loss_reports == 0U);
	assert(published_device->carrier == 0U);

	mutex_lock(&controller->lifecycle_lock);
	ax211_pci_operation_leave_locked(controller);
	mutex_unlock(&controller->lifecycle_lock);
	assert(published_device->ops->poll_receive(published_device, 1U) == 0U);
	assert(link_loss_reports == 1U);
	assert(reported_link_loss_generation == 93U);
	assert(controller->recovery_pending == 0U);
	assert(controller->runtime_active == 0U);
	assert(controller->active_dma == NULL);
	assert(controller->quarantined == 1U);
	assert(dma_allocations == dma_frees);
	assert(ax211_pci_detach(&device, 0U) == 0);
}

static void
test_notification_layout_versions(void)
{
	uint8_t event[INTEL_AX211_EVENT_HEADER_SIZE];

	memset(event, 0, sizeof(event));
	event[4U] = INTEL_AX211_PROTOCOL_ALIVE_OPCODE;
	event[5U] = INTEL_AX211_PROTOCOL_GROUP_LEGACY;
	event[7U] = 0x80U;
	assert(ax211_pci_notification_version(event, sizeof(event)) ==
	    INTEL_AX211_PROTOCOL_ALIVE_VERSION);
	event[4U] = INTEL_AX211_PROTOCOL_PNVM_INIT_COMPLETE_OPCODE;
	event[5U] = INTEL_AX211_PROTOCOL_GROUP_REGULATORY_NVM;
	assert(ax211_pci_notification_version(event, sizeof(event)) ==
	    INTEL_AX211_PROTOCOL_PNVM_INIT_COMPLETE_VERSION);
	event[4U] = INTEL_AX211_PROTOCOL_INIT_COMPLETE_OPCODE;
	event[5U] = INTEL_AX211_PROTOCOL_GROUP_LEGACY;
	assert(ax211_pci_notification_version(event, sizeof(event)) ==
	    INTEL_AX211_PROTOCOL_UNKNOWN_VERSION);
	event[4U] = 0xc1U;
	event[5U] = 0U;
	assert(ax211_pci_notification_version(event, sizeof(event)) == 5U);
	event[4U] = 0xc8U;
	event[5U] = 1U;
	assert(ax211_pci_notification_version(event, sizeof(event)) == 6U);
	event[4U] = 0x0fU;
	event[5U] = 0U;
	assert(ax211_pci_notification_version(event, sizeof(event)) == 1U);
	event[4U] = 0xb5U;
	assert(ax211_pci_notification_version(event, sizeof(event)) == 1U);
	event[4U] = 0x7fU;
	assert(ax211_pci_notification_version(event, sizeof(event)) ==
	    INTEL_AX211_PROTOCOL_UNKNOWN_VERSION);
	event[7U] = 0U;
	assert(ax211_pci_notification_version(event, sizeof(event)) == 0U);
	event[4U] = INTEL_AX211_TX_OPCODE;
	event[5U] = FIXTURE_TX_EVENT_GROUP;
	event[7U] = FIXTURE_TX_EVENT_QUEUE;
	assert(ax211_pci_notification_version(event, sizeof(event)) ==
	    INTEL_AX211_TX_NOTIFICATION_VERSION);
}

static void
test_receive_copy_replenish_and_poll_boundary(void)
{
	static const uint8_t payload[] = { 1U, 2U, 3U, 4U };
	struct ax211_pci_controller *controller;
	struct intel_ax211_boot_received_event event;
	struct intel_ax211_protocol_alive alive;
	struct drv_pci_device device;
	uint8_t copy[32];

	fixture_reset(&device);
	assert(drv_pci_intel_ax211_driver_register() == 0);
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == 0);
	drv_pci_intel_ax211_devices_ready();
	controller = device.driver_data;
	assert(published_device->ops->open(published_device) == 0);
	assert(controller->active_dma != NULL);
	controller->active_dma->pnvm_prepared = 1U;
	controller->active_dma->pnvm_count = 1U;
	controller->active_dma->pnvm_table.address = fixture_rx_bytes;
	controller->active_dma->pnvm_table.device_address = UINT64_C(0x2000);
	assert(ax211_pci_publish_pnvm(controller, controller->active_dma) == 0);
	assert(write_barrier_count == 1U);
	assert(controller->mmio.nic_lock_depth == 0U);
	memset(&alive, 0, sizeof(alive));
	alive.status = INTEL_AX211_PROTOCOL_ALIVE_STATUS_OK;
	assert(ax211_pci_post_alive(controller, &alive) == 0);
	assert(controller->mmio.nic_lock_depth == 0U);
	assert(device.irq_handler != NULL);
	assert(device.irq_handler(device.irq_argument) == 1);
	assert(device.poll_scheduled == 1U);
	controller->transport.rx_active = 0U;
	assert(controller->transport.rx_active == 0U);
	memset(copy, 0, sizeof(copy));
	memset(&event, 0, sizeof(event));
	assert(ax211_pci_receive_event(controller, fixture_clock, copy,
	    sizeof(copy), &event) == INTEL_AX211_BOOT_RECEIVE_TIMEOUT);
	assert(controller->transport.rx_active == 0U);
	assert(transport_activate_count == 0U);
	fixture_rx_event(INTEL_AX211_PROTOCOL_ALIVE_OPCODE,
	    INTEL_AX211_PROTOCOL_GROUP_LEGACY, 0x80U, payload,
	    sizeof(payload));
	fixture_hw_causes = INTEL_AX211_TRANSPORT_HW_CAUSE_ALIVE;
	memset(copy, 0, sizeof(copy));
	memset(&event, 0, sizeof(event));
	assert(ax211_pci_receive_event(controller, fixture_clock, copy,
	    sizeof(copy), &event) == INTEL_AX211_BOOT_RECEIVE_OK);
	assert(event.length == fixture_rx_length);
	assert(event.generation == controller->hardware_epoch);
	assert(event.notification_version ==
	    INTEL_AX211_PROTOCOL_ALIVE_VERSION);
	assert(controller->transport.rx_active == 1U);
	assert(transport_activate_count == 1U);
	assert(memcmp(copy + 8U, payload, sizeof(payload)) == 0);
	assert(fixture_rx_bytes[8U] == 0xa5U);
	assert(transport_replenish_count == 1U);
	assert(transport_rearm_count == 2U);

	memset(fixture_rx_bytes, 0, sizeof(fixture_rx_bytes));
	fixture_rx_bytes[0U] = 0xffU;
	fixture_rx_bytes[1U] = 0x3fU;
	fixture_rx_ready = 1U;
	fixture_hw_causes = INTEL_AX211_TRANSPORT_HW_CAUSE_ALIVE;
	memset(&event, 0, sizeof(event));
	assert(ax211_pci_receive_event(controller, fixture_clock, copy,
	    sizeof(copy), &event) == INTEL_AX211_BOOT_RECEIVE_IO);
	assert(transport_replenish_count == 2U);
	assert(transport_activate_count == 1U);

	controller->command_hw_causes = 0U;
	controller->command_raw_hw_causes = 0U;
	fixture_hw_causes = INTEL_AX211_TRANSPORT_HW_CAUSE_SW_ERROR_V2;
	memset(&event, 0, sizeof(event));
	assert(ax211_pci_receive_event(controller, fixture_clock, copy,
	    sizeof(copy), &event) == INTEL_AX211_BOOT_RECEIVE_IO);
	assert(controller->command_hw_causes ==
	    INTEL_AX211_TRANSPORT_HW_CAUSE_SW_ERROR_V2);
	assert(controller->command_raw_hw_causes ==
	    INTEL_AX211_TRANSPORT_HW_CAUSE_SW_ERROR_V2);
	assert(strstr(log_buffer, "fatal firmware interrupt") != NULL);

	fixture_rx_event(0x7fU, 0U, 0x80U, payload, sizeof(payload));
	assert(published_device->ops->poll_receive(published_device, 1U) == 1U);
	assert(transport_replenish_count == 3U);
	assert(controller->runtime_event[0U] == 0U);
	published_device->ops->close(published_device);
	assert(controller->runtime_active == 0U);
	assert(device.irq_established == 0U);
	assert(ax211_pci_detach(&device, 0U) == 0);
}

static void
test_scan_session_and_rx_poll_integration(void)
{
	static const uint8_t payload[] = { 0x5aU };
	struct ax211_pci_controller *controller;
	struct drv_pci_device device;
	uint64_t common_generation;

	fixture_reset(&device);
	assert(drv_pci_intel_ax211_driver_register() == 0);
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == 0);
	drv_pci_intel_ax211_devices_ready();
	controller = device.driver_data;
	assert(published_device->ops->open(published_device) == 0);
	assert(controller->scan_initialized == 1U);
	assert(controller->scan_session.hardware_epoch ==
	    controller->hardware_epoch);
	common_generation = UINT64_C(0x100000005);
	assert(published_station->ops->scan_channel_start(controller,
	    common_generation, 0U, 1U, 100U) == 0);
	assert(scan_begin_count == 1U);
	assert(controller->scan_session.common_generation == common_generation);
	assert(controller->scan_session.hardware_epoch !=
	    (uint32_t)common_generation);

	fixture_rx_event(INTEL_AX211_SCAN_REQUEST_OPCODE,
	    INTEL_AX211_SCAN_GROUP_LONG, 1U, NULL, 0U);
	assert(scan_ready_reports == 0U);
	assert(published_device->ops->poll_receive(published_device, 1U) == 1U);
	assert(scan_start_ack_count == 1U);
	assert(scan_ready_reports == 1U);
	assert(scan_report_generation == common_generation);
	assert(scan_report_step == 0U);
	assert(controller->scan_session.phase ==
	    INTEL_AX211_SCAN_SESSION_RUNNING);

	fixture_rx_event(INTEL_AX211_RX_MPDU_OPCODE,
	    INTEL_AX211_RX_MPDU_GROUP, 0x80U, payload, sizeof(payload));
	assert(published_device->ops->poll_receive(published_device, 1U) == 1U);
	assert(rx_decode_count == 1U);
	assert(scan_frame_reports == 1U);
	assert(scan_report_generation == common_generation);
	assert(scan_report_channel == 1U);
	assert(scan_report_rssi == -47);
	assert(controller->runtime_frame[0U] == 0U);

	fixture_rx_frame_control = 0x0008U;
	fixture_rx_event(INTEL_AX211_RX_MPDU_OPCODE,
	    INTEL_AX211_RX_MPDU_GROUP, 0x80U, payload, sizeof(payload));
	assert(published_device->ops->poll_receive(published_device, 1U) == 1U);
	assert(rx_decode_count == 2U);
	assert(scan_frame_reports == 1U);

	fixture_rx_event(INTEL_AX211_SCAN_ITERATION_COMPLETE_OPCODE,
	    INTEL_AX211_SCAN_GROUP_LEGACY, 0x80U, payload, sizeof(payload));
	assert(published_device->ops->poll_receive(published_device, 1U) == 1U);
	assert(scan_notification_count == 1U);
	assert(controller->scan_session.phase ==
	    INTEL_AX211_SCAN_SESSION_TERMINAL);
	assert(published_station->ops->scan_stop(controller,
	    common_generation) == 0);

	fixture_rx_frame_control = 0x0080U;
	assert(published_station->ops->scan_channel_start(controller,
	    common_generation, 1U, 2U, 100U) == 0);
	fixture_rx_event(INTEL_AX211_SCAN_REQUEST_OPCODE,
	    INTEL_AX211_SCAN_GROUP_LONG, 1U, NULL, 0U);
	assert(published_device->ops->poll_receive(published_device, 1U) == 1U);
	assert(published_station->ops->scan_stop(controller,
	    common_generation) == EBUSY);
	assert(scan_abort_count == 1U);
	assert(controller->scan_session.phase ==
	    INTEL_AX211_SCAN_SESSION_WAIT_ABORT_ACK);
	fixture_rx_event(INTEL_AX211_SCAN_ABORT_OPCODE,
	    INTEL_AX211_SCAN_GROUP_LONG, 1U, NULL, 0U);
	assert(published_device->ops->poll_receive(published_device, 1U) == 1U);
	assert(scan_abort_ack_count == 1U);
	assert(published_station->ops->scan_stop(controller,
	    common_generation) == 0);

	assert(published_station->ops->scan_channel_start(controller,
	    common_generation, 2U, 3U, 100U) == 0);
	fixture_rx_event(INTEL_AX211_SCAN_REQUEST_OPCODE,
	    INTEL_AX211_SCAN_GROUP_LONG, 1U, NULL, 0U);
	assert(published_device->ops->poll_receive(published_device, 1U) == 1U);
	fixture_scan_notification_result = INTEL_AX211_SCAN_SESSION_FAILED;
	fixture_rx_event(INTEL_AX211_SCAN_COMPLETE_OPCODE,
	    INTEL_AX211_SCAN_GROUP_LEGACY, 0x80U, payload, sizeof(payload));
	assert(published_device->ops->poll_receive(published_device, 1U) == 1U);
	assert(scan_error_reports == 1U);
	assert(scan_report_generation == common_generation);
	assert(scan_report_error == EIO);
	assert(published_station->ops->scan_stop(controller,
	    common_generation) == EIO);
	assert(controller->runtime_active == 0U);
	assert(controller->quarantined == 1U);
	assert(published_station->ops->scan_stop(controller,
	    common_generation) == 0);
	assert(ax211_pci_detach(&device, 0U) == 0);
}

static void
test_association_key_tx_rx_disconnect_sequence(void)
{
	static const uint8_t bssid[6] = {
		0x02U, 0x66U, 0x77U, 0x88U, 0x99U, 0xaaU
	};
	static const uint8_t payload[] = { 0x5aU };
	struct ax211_pci_controller *controller;
	struct ax211_pci_staged_key empty_staged_key;
	struct wlan_radio_key_request key;
	struct wlan_radio_tx_request transmit;
	struct wlan_bss_record bss;
	struct drv_pci_device device;
	uint8_t frame[32];
	uint8_t session_event[INTEL_AX211_ASSOC_SESSION_NOTIFICATION_SIZE];
	uint8_t tx_response[48];
	uint8_t tx_index;
	uint64_t connection_generation;
	uint64_t scan_generation;
	uint64_t deadline;
	size_t key_byte;
	uint16_t reconnect_sequence;
	unsigned completion_count;
	unsigned poll_scheduled;

	fixture_reset(&device);
	assert(drv_pci_intel_ax211_driver_register() == 0);
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == 0);
	drv_pci_intel_ax211_devices_ready();
	controller = device.driver_data;
	assert(published_device->ops->open(published_device) == 0);
	assert(scan_profile_updates == 1U);
	assert(published_profile.channels[10U].channel == 36U);
	scan_generation = UINT64_C(0x100000029);
	deadline = clock_ticks() + 100U;
	assert(published_station->ops->scan_channel_start(controller,
	    scan_generation, AX211_PASSIVE_CHANNEL_COUNT - 1U, 36U,
	    deadline) == 0);
	fixture_rx_event(INTEL_AX211_SCAN_REQUEST_OPCODE,
	    INTEL_AX211_SCAN_GROUP_LONG, 1U, NULL, 0U);
	assert(published_device->ops->poll_receive(published_device, 1U) == 1U);
	fixture_rx_event(INTEL_AX211_RX_MPDU_OPCODE, INTEL_AX211_RX_MPDU_GROUP,
	    0x80U, payload, sizeof(payload));
	assert(published_device->ops->poll_receive(published_device, 1U) == 1U);
	assert(controller->bss_staging_cache.count == 1U);
	fixture_rx_event(INTEL_AX211_SCAN_COMPLETE_OPCODE,
	    INTEL_AX211_SCAN_GROUP_LEGACY, 0x80U, payload, sizeof(payload));
	assert(published_device->ops->poll_receive(published_device, 1U) == 1U);
	assert(published_station->ops->scan_stop(controller, scan_generation) == 0);
	assert(controller->bss_published_cache.count == 1U);

	memset(&bss, 0, sizeof(bss));
	memcpy(bss.bssid, bssid, sizeof(bssid));
	bss.channel = 36U;
	connection_generation = UINT64_C(0x20000002a);
	assert(published_station->ops->connect_start(controller,
	    connection_generation, &bss, deadline) == 0);
	assert(controller->association.phase ==
	    INTEL_AX211_ASSOC_PHASE_AUTH_READY);
	assert(controller->association.profile.channel == 36U);
	assert(controller->association.profile.cck_ack_rates == 1U);
	/* The synthetic AP capability omits SHORT_SLOT_TIME; 5 GHz still forces
	 * short-slot in the Intel MAC context while keeping short preamble off. */
	assert((controller->selected_metadata.capability & 0x0400U) == 0U);
	assert(controller->association.profile.short_preamble == 0U);
	assert(controller->association.profile.short_slot == 1U);
	assert(controller->tx_ring.enabled == 1U);

	memset(frame, 0, sizeof(frame));
	frame[0U] = 0xb0U;
	memset(&transmit, 0, sizeof(transmit));
	transmit.generation = connection_generation;
	transmit.cookie = UINT64_MAX;
	transmit.deadline_ticks = deadline;
	transmit.frame_class = WLAN_RADIO_FRAME_MANAGEMENT;
	transmit.frame = frame;
	transmit.length = 24U;
	assert(published_station->ops->frame_transmit(controller, &transmit) == 0);
	tx_index = (uint8_t)(controller->tx_ring.write_sequence - 1U);
	assert(ax211_pci_get_le32(controller->tx_ring.slot[tx_index].
	    command.address + 20U) == 0x00004100U);
	assert(controller->tx_ring.pending_count == 1U);
	memset(tx_response, 0, sizeof(tx_response));
	tx_response[0U] = 1U;
	tx_response[30U] = 24U;
	tx_response[36U] = (uint8_t)FIXTURE_TX_QUEUE;
	tx_response[37U] = (uint8_t)(FIXTURE_TX_QUEUE >> 8);
	tx_response[40U] = 1U;
	tx_response[44U] = 1U;
	fixture_rx_event(INTEL_AX211_TX_OPCODE, FIXTURE_TX_EVENT_GROUP,
	    FIXTURE_TX_EVENT_QUEUE, tx_response, sizeof(tx_response));
	fixture_rx_bytes[6U] = 0U;
	assert(published_device->ops->poll_receive(published_device, 1U) == 1U);
	assert(tx_completion_reports == 1U && reported_tx_cookie == UINT64_MAX);

	memset(session_event, 0, sizeof(session_event));
	session_event[4U] = 1U;
	fixture_rx_event(INTEL_AX211_ASSOC_SESSION_NOTIFICATION_OPCODE,
	    INTEL_AX211_ASSOC_GROUP_MAC_CONFIG, 0x80U, session_event,
	    sizeof(session_event));
	assert(published_device->ops->poll_receive(published_device, 1U) == 1U);
	assert(controller->association.session_ended == 1U);
	assert(published_station->ops->association_set(controller,
	    connection_generation, bssid, 1U, deadline) == 0);
	assert(controller->association.phase == INTEL_AX211_ASSOC_PHASE_ASSOCIATED);
	assert(fixture_mcast_command_order != 0U);
	assert(fixture_power_command_order == fixture_mcast_command_order + 1U);

	/* Reject hostile key indices before either private array is addressed. */
	memset(&key, 0, sizeof(key));
	key.generation = connection_generation;
	key.key_generation = 99U;
	key.deadline_ticks = deadline;
	key.kind = WLAN_RADIO_KEY_GROUP;
	key.key_index = INTEL_AX211_KEY_INDEX_LIMIT;
	memset(key.address, 0xff, sizeof(key.address));
	memset(key.key, 0x0f, sizeof(key.key));
	assert(published_station->ops->key_install(controller, &key) == EINVAL);
	key.kind = WLAN_RADIO_KEY_PAIRWISE;
	key.key_index = 1U;
	memcpy(key.address, bssid, sizeof(bssid));
	assert(published_station->ops->key_install(controller, &key) == EINVAL);
	assert(published_station->ops->key_delete(controller,
	    connection_generation, WLAN_RADIO_KEY_GROUP,
	    INTEL_AX211_KEY_INDEX_LIMIT, 99U, deadline) == EINVAL);
	assert(published_station->ops->key_delete(controller,
	    connection_generation, WLAN_RADIO_KEY_PAIRWISE, 1U, 99U,
	    deadline) == EINVAL);
	assert(fixture_key_add_count == 0U && fixture_key_remove_count == 0U);

	/* A TX completion crossing a synchronous key command is copied, not
	 * reported through common while its station-control gate is held. */
	memset(frame, 0, sizeof(frame));
	frame[0U] = 0xb0U;
	memset(&transmit, 0, sizeof(transmit));
	transmit.generation = connection_generation;
	transmit.cookie = UINT64_C(0x123456789abcdef0);
	transmit.deadline_ticks = deadline;
	transmit.frame_class = WLAN_RADIO_FRAME_MANAGEMENT;
	transmit.frame = frame;
	transmit.length = 24U;
	assert(published_station->ops->frame_transmit(controller, &transmit) == 0);
	memset(fixture_async_tx_response, 0,
	    sizeof(fixture_async_tx_response));
	fixture_async_tx_response[0U] = 1U;
	fixture_async_tx_response[30U] = 24U;
	fixture_async_tx_response[36U] = (uint8_t)FIXTURE_TX_QUEUE;
	fixture_async_tx_response[37U] = (uint8_t)(FIXTURE_TX_QUEUE >> 8);
	fixture_async_tx_response[40U] = 1U;
	fixture_async_tx_response[44U] = 2U;
	fixture_async_tx_index = 1U;
	fixture_command_async_tx_before_response = 1U;
	fixture_common_gate_held = 1U;

	memset(&key, 0, sizeof(key));
	key.generation = connection_generation;
	key.key_generation = 100U;
	key.deadline_ticks = deadline;
	key.kind = WLAN_RADIO_KEY_PAIRWISE;
	memcpy(key.address, bssid, sizeof(bssid));
	memset(key.key, 0x11, sizeof(key.key));
	assert(published_station->ops->key_install(controller, &key) == 0);
	assert(controller->staged_pairwise_key.valid == 1U);
	assert(controller->staged_pairwise_key.programmed == 1U);
	for (key_byte = 0U;
	    key_byte < sizeof(controller->staged_pairwise_key.request.key);
	    key_byte++)
		assert(controller->staged_pairwise_key.request.key[key_byte] == 0U);
	/* Once firmware owns the key, duplicate detection uses only public
	 * metadata; plaintext is never retained for a byte comparison. */
	assert(published_station->ops->key_install(controller, &key) == 0);
	assert(fixture_key_add_count == 1U);
	assert(tx_completion_reports == 1U);
	assert(controller->deferred_event_count == 1U);
	fixture_common_gate_held = 0U;
	assert(published_device->ops->poll_receive(published_device, 1U) == 1U);
	assert(tx_completion_reports == 2U && reported_tx_cookie ==
	    UINT64_C(0x123456789abcdef0));
	assert(controller->deferred_event_count == 0U);
	key.kind = WLAN_RADIO_KEY_GROUP;
	key.key_index = 1U;
	memset(key.address, 0xff, sizeof(key.address));
	memset(key.key, 0x22, sizeof(key.key));
	assert(published_station->ops->key_install(controller, &key) == 0);
	assert(fixture_key_add_count == 2U);
	assert(controller->keys.active_pairwise == 100U);
	assert(controller->keys.active_group[1U] == 100U);

	memset(frame, 0, sizeof(frame));
	frame[0U] = 0x08U;
	frame[1U] = 0x40U;
	frame[24U] = 1U;
	frame[27U] = 0x20U;
	memset(&transmit, 0, sizeof(transmit));
	transmit.generation = connection_generation;
	transmit.cookie = UINT64_C(0x8000000000000001);
	transmit.deadline_ticks = deadline;
	transmit.key_generation = 100U;
	transmit.packet_number = 1U;
	transmit.frame_class = WLAN_RADIO_FRAME_DATA;
	transmit.encrypted = 1U;
	transmit.frame = frame;
	transmit.length = sizeof(frame);
	assert(published_station->ops->frame_transmit(controller, &transmit) == 0);
	memset(tx_response, 0, sizeof(tx_response));
	tx_response[0U] = 1U;
	tx_response[30U] = 24U;
	tx_response[36U] = (uint8_t)FIXTURE_TX_QUEUE;
	tx_response[37U] = (uint8_t)(FIXTURE_TX_QUEUE >> 8);
	tx_response[40U] = 1U;
	tx_response[44U] = 3U;
	fixture_rx_event(INTEL_AX211_TX_OPCODE, FIXTURE_TX_EVENT_GROUP,
	    FIXTURE_TX_EVENT_QUEUE, tx_response, sizeof(tx_response));
	fixture_rx_bytes[6U] = 2U;
	assert(published_device->ops->poll_receive(published_device, 1U) == 1U);
	assert(tx_completion_reports == 3U && reported_tx_cookie ==
	    UINT64_C(0x8000000000000001));

	/* Replacement secrets stay private until common reports M4 ACK. */
	memset(&key, 0, sizeof(key));
	key.generation = connection_generation;
	key.key_generation = 200U;
	key.deadline_ticks = deadline;
	key.kind = WLAN_RADIO_KEY_PAIRWISE;
	memcpy(key.address, bssid, sizeof(bssid));
	memset(key.key, 0x33, sizeof(key.key));
	assert(published_station->ops->key_install(controller, &key) == 0);
	key.kind = WLAN_RADIO_KEY_GROUP;
	key.key_index = 2U;
	memset(key.address, 0xff, sizeof(key.address));
	memset(key.key, 0x44, sizeof(key.key));
	assert(published_station->ops->key_install(controller, &key) == 0);
	assert(fixture_key_add_count == 2U);
	assert(controller->keys.active_pairwise == 100U);
	assert(controller->staged_pairwise_key.valid == 1U);
	assert(controller->staged_group_key[2U].valid == 1U);

	frame[24U] = 2U;
	transmit.cookie = UINT64_C(0x7fffffffffffffff);
	transmit.key_generation = 100U;
	transmit.packet_number = 2U;
	assert(published_station->ops->frame_transmit(controller, &transmit) == 0);
	memset(tx_response, 0, sizeof(tx_response));
	tx_response[0U] = 1U;
	tx_response[30U] = 24U;
	tx_response[36U] = (uint8_t)FIXTURE_TX_QUEUE;
	tx_response[37U] = (uint8_t)(FIXTURE_TX_QUEUE >> 8);
	tx_response[40U] = 1U;
	tx_response[44U] = 4U;
	fixture_rx_event(INTEL_AX211_TX_OPCODE, FIXTURE_TX_EVENT_GROUP,
	    FIXTURE_TX_EVENT_QUEUE, tx_response, sizeof(tx_response));
	fixture_rx_bytes[6U] = 3U;
	assert(published_device->ops->poll_receive(published_device, 1U) == 1U);
	assert(published_station->ops->keys_activate(controller,
	    connection_generation, 200U, 200U, deadline) == 0);
	assert(fixture_key_add_count == 4U);
	assert(controller->keys.active_pairwise == 200U);
	assert(controller->keys.active_group[2U] == 200U);
	assert(controller->staged_pairwise_key.valid == 0U);
	assert(controller->staged_group_key[2U].valid == 0U);

	frame[24U] = 3U;
	transmit.cookie = 77U;
	transmit.key_generation = 100U;
	transmit.packet_number = 3U;
	assert(published_station->ops->frame_transmit(controller, &transmit) ==
	    ESTALE);
	transmit.key_generation = 200U;
	assert(published_station->ops->frame_transmit(controller, &transmit) == 0);
	memset(tx_response, 0, sizeof(tx_response));
	tx_response[0U] = 1U;
	tx_response[30U] = 24U;
	tx_response[36U] = (uint8_t)FIXTURE_TX_QUEUE;
	tx_response[37U] = (uint8_t)(FIXTURE_TX_QUEUE >> 8);
	tx_response[40U] = 1U;
	tx_response[44U] = 5U;
	fixture_rx_event(INTEL_AX211_TX_OPCODE, FIXTURE_TX_EVENT_GROUP,
	    FIXTURE_TX_EVENT_QUEUE, tx_response, sizeof(tx_response));
	fixture_rx_bytes[6U] = 4U;
	assert(published_device->ops->poll_receive(published_device, 1U) == 1U);

	/* Deletes for overwritten tuples must not remove their replacements. */
	assert(published_station->ops->key_delete(controller,
	    connection_generation, WLAN_RADIO_KEY_PAIRWISE, 0U, 100U,
	    deadline) == 0);
	assert(fixture_key_remove_count == 0U);
	assert(published_station->ops->key_delete(controller,
	    connection_generation, WLAN_RADIO_KEY_GROUP, 1U, 100U,
	    deadline) == 0);
	assert(fixture_key_remove_count == 1U);
	assert(controller->keys.active_pairwise == 200U);

	/* A same-index GTK replacement overwrites the old tuple.  Its later
	 * logical delete must not issue a firmware REMOVE for the new key. */
	memset(&key, 0, sizeof(key));
	key.generation = connection_generation;
	key.key_generation = 250U;
	key.deadline_ticks = deadline;
	key.kind = WLAN_RADIO_KEY_GROUP;
	key.key_index = 2U;
	memset(key.address, 0xff, sizeof(key.address));
	memset(key.key, 0x66, sizeof(key.key));
	assert(published_station->ops->key_install(controller, &key) == 0);
	assert(fixture_key_add_count == 4U);
	assert(published_station->ops->keys_activate(controller,
	    connection_generation, 200U, 250U, deadline) == 0);
	assert(fixture_key_add_count == 5U);
	assert(controller->keys.active_group[2U] == 250U);
	assert(published_station->ops->key_delete(controller,
	    connection_generation, WLAN_RADIO_KEY_GROUP, 2U, 200U,
	    deadline) == 0);
	assert(fixture_key_remove_count == 1U);

	/* A different-index GTK replacement leaves a distinct old tuple which
	 * must be removed only after the replacement has been activated. */
	memset(&key, 0, sizeof(key));
	key.generation = connection_generation;
	key.key_generation = 275U;
	key.deadline_ticks = deadline;
	key.kind = WLAN_RADIO_KEY_GROUP;
	key.key_index = 3U;
	memset(key.address, 0xff, sizeof(key.address));
	memset(key.key, 0x77, sizeof(key.key));
	assert(published_station->ops->key_install(controller, &key) == 0);
	assert(fixture_key_add_count == 5U);
	assert(published_station->ops->keys_activate(controller,
	    connection_generation, 200U, 275U, deadline) == 0);
	assert(fixture_key_add_count == 6U);
	assert(controller->keys.active_group[3U] == 275U);
	assert(published_station->ops->key_delete(controller,
	    connection_generation, WLAN_RADIO_KEY_GROUP, 2U, 250U,
	    deadline) == 0);
	assert(fixture_key_remove_count == 2U);

	/* Removing an unprogrammed staged key preserves the active generation. */
	memset(&key, 0, sizeof(key));
	key.generation = connection_generation;
	key.key_generation = 300U;
	key.deadline_ticks = deadline;
	key.kind = WLAN_RADIO_KEY_GROUP;
	key.key_index = 1U;
	memset(key.address, 0xff, sizeof(key.address));
	memset(key.key, 0x55, sizeof(key.key));
	assert(published_station->ops->key_install(controller, &key) == 0);
	assert(fixture_key_add_count == 6U);
	assert(published_station->ops->key_delete(controller,
	    connection_generation, WLAN_RADIO_KEY_GROUP, 1U, 300U,
	    deadline) == 0);
	assert(fixture_key_remove_count == 2U);
	assert(controller->keys.active_pairwise == 200U);

	/* A still-staged replacement is secret controller state.  Disconnect
	 * must scrub it without removing the active firmware tuple. */
	key.key_generation = 400U;
	memset(key.key, 0x88, sizeof(key.key));
	assert(published_station->ops->key_install(controller, &key) == 0);
	assert(controller->staged_group_key[1U].valid == 1U);
	assert(fixture_key_add_count == 6U);

	fixture_rx_frame_control = 0x4008U;
	fixture_rx_cipher = INTEL_AX211_RX_CIPHER_CCMP;
	fixture_rx_decrypted = 1U;
	fixture_rx_packet_number = 4U;
	poll_scheduled = device.poll_scheduled;
	fixture_poll_overlap = 1U;
	fixture_rx_event(INTEL_AX211_RX_MPDU_OPCODE, INTEL_AX211_RX_MPDU_GROUP,
	    0x80U, payload, sizeof(payload));
	assert(published_device->ops->poll_receive(published_device, 1U) == 1U);
	assert(fixture_nested_poll_count == 1U);
	assert(controller->poll_active == 0U);
	assert(device.poll_scheduled > poll_scheduled);
	assert(fixture_rx_ready == 1U);
	/* The nested caller only requested rescheduling.  The outer owner kept
	 * its copied frame stable and leaves the newly queued event for a later
	 * poll invocation. */
	assert(published_device->ops->poll_receive(published_device, 1U) == 1U);
	assert(fixture_rx_ready == 0U);
	assert(rx_frame_reports == 1U && reported_rx_generation ==
	    connection_generation);
	/* Common may reject a valid private RX envelope as a foreign, replayed,
	 * malformed, or unsupported air frame.  That is a drop, not hardware
	 * quarantine. */
	fixture_frame_report_result = EINVAL;
	fixture_rx_packet_number = 5U;
	fixture_rx_event(INTEL_AX211_RX_MPDU_OPCODE, INTEL_AX211_RX_MPDU_GROUP,
	    0x80U, payload, sizeof(payload));
	assert(published_device->ops->poll_receive(published_device, 1U) == 1U);
	assert(rx_frame_reports == 2U);
	assert(controller->runtime_active == 1U && controller->quarantined == 0U);
	fixture_frame_report_result = 0;

	assert(published_station->ops->disconnect(controller,
	    connection_generation) == 0);
	memset(&empty_staged_key, 0, sizeof(empty_staged_key));
	assert(memcmp(&controller->staged_group_key[1U], &empty_staged_key,
	    sizeof(empty_staged_key)) == 0);
	assert(fixture_key_remove_count == 4U);
	assert(controller->connection_generation == 0U);
	assert(controller->tx_ring.enabled == 0U);
	/* A completion already in the device event ring may arrive after the
	 * queue-remove ACK.  It is a stale tombstone, not live-DMA corruption. */
	fixture_rx_event(INTEL_AX211_TX_OPCODE, FIXTURE_TX_EVENT_GROUP,
	    FIXTURE_TX_EVENT_QUEUE, tx_response, sizeof(tx_response));
	assert(published_device->ops->poll_receive(published_device, 1U) == 1U);
	assert(controller->runtime_active == 1U);
	assert(controller->quarantined == 0U);

	/* A reconnect starts from the write pointer returned by the new
	 * firmware-owned queue rather than from a host-selected sequence. */
	connection_generation = UINT64_C(0x20000002b);
	deadline = clock_ticks() + 100U;
	assert(published_station->ops->connect_start(controller,
	    connection_generation, &bss, deadline) == 0);
	reconnect_sequence = controller->tx_ring.write_sequence;
	assert(reconnect_sequence == controller->tx_ring.read_sequence);
	memset(frame, 0, sizeof(frame));
	frame[0U] = 0xb0U;
	memset(&transmit, 0, sizeof(transmit));
	transmit.generation = connection_generation;
	transmit.cookie = UINT64_C(0x20000002c);
	transmit.deadline_ticks = deadline;
	transmit.frame_class = WLAN_RADIO_FRAME_MANAGEMENT;
	transmit.frame = frame;
	transmit.length = 24U;
	assert(published_station->ops->frame_transmit(controller, &transmit) == 0);
	assert(controller->tx_ring.pending_count == 1U);
	completion_count = tx_completion_reports;
	memset(tx_response, 0, sizeof(tx_response));
	tx_response[0U] = 1U;
	tx_response[30U] = 24U;
	tx_response[36U] = (uint8_t)FIXTURE_TX_QUEUE;
	tx_response[37U] = (uint8_t)(FIXTURE_TX_QUEUE >> 8);
	tx_response[40U] = 1U;
	tx_response[44U] = (uint8_t)(reconnect_sequence + 1U);
	tx_response[45U] = (uint8_t)((reconnect_sequence + 1U) >> 8);
	fixture_rx_event(INTEL_AX211_TX_OPCODE, FIXTURE_TX_EVENT_GROUP,
	    FIXTURE_TX_EVENT_QUEUE, tx_response, sizeof(tx_response));
	fixture_rx_bytes[6U] = (uint8_t)reconnect_sequence;
	assert(published_device->ops->poll_receive(published_device, 1U) == 1U);
	assert(controller->tx_ring.pending_count == 0U);
	assert(tx_completion_reports == completion_count + 1U);
	assert(reported_tx_cookie == UINT64_C(0x20000002c));
	assert(published_station->ops->disconnect(controller,
	    connection_generation) == 0);
	published_device->ops->close(published_device);
	assert(dma_allocations == dma_frees);
	assert(ax211_pci_detach(&device, 0U) == 0);
}

static void
test_scan_generation_resets_full_bss_cache(void)
{
	static const uint8_t selected_bssid[6] = {
		0x02U, 0xdeU, 0xadU, 0xbeU, 0xefU, 0x80U
	};
	struct ax211_pci_controller *controller;
	struct intel_ax211_bss_entry entry;
	struct wlan_bss_record bss;
	struct drv_pci_device device;
	uint64_t deadline;
	uint64_t first_generation;
	uint64_t second_generation;
	uint64_t aborted_generation;
	uint64_t connection_generation;
	unsigned index;

	fixture_reset(&device);
	assert(drv_pci_intel_ax211_driver_register() == 0);
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == 0);
	drv_pci_intel_ax211_devices_ready();
	controller = device.driver_data;
	assert(published_device->ops->open(published_device) == 0);
	deadline = clock_ticks() + 100U;
	first_generation = UINT64_C(0x300000001);
	assert(published_station->ops->scan_channel_start(controller,
	    first_generation, AX211_PASSIVE_CHANNEL_COUNT - 1U, 1U,
	    deadline) == 0);
	fixture_rx_event(INTEL_AX211_SCAN_REQUEST_OPCODE,
	    INTEL_AX211_SCAN_GROUP_LONG, 1U, NULL, 0U);
	assert(published_device->ops->poll_receive(published_device, 1U) == 1U);
	for (index = 0U; index < INTEL_AX211_BSS_CACHE_LIMIT; index++) {
		memset(&entry, 0, sizeof(entry));
		entry.bssid[0U] = 0x02U;
		entry.bssid[1U] = 0x10U;
		entry.bssid[5U] = (uint8_t)index;
		entry.observation_generation = first_generation;
		entry.frame_timestamp = (uint64_t)index + 1U;
		entry.receive_tsf = (uint64_t)index + 1U;
		entry.hardware_epoch = controller->hardware_epoch;
		entry.gp2_on_air_rise = index + 1U;
		entry.last_seen_ticks = index + 1U;
		entry.beacon_interval_tu = 100U;
		entry.capability = 0x20U;
		entry.channel = 1U;
		entry.dtim_period = 1U;
		entry.tim_valid = 1U;
		entry.receive_tsf_valid = 1U;
		entry.source = INTEL_AX211_BSS_SOURCE_BEACON;
		entry.valid = 1U;
		assert(intel_ax211_bss_cache_observe(
		    &controller->bss_staging_cache,
		    &entry) == INTEL_AX211_BSS_OK);
	}
	assert(controller->bss_staging_cache.count ==
	    INTEL_AX211_BSS_CACHE_LIMIT);
	fixture_rx_event(INTEL_AX211_SCAN_COMPLETE_OPCODE,
	    INTEL_AX211_SCAN_GROUP_LEGACY, 0x80U, NULL, 0U);
	assert(published_device->ops->poll_receive(published_device, 1U) == 1U);
	assert(published_station->ops->scan_stop(controller,
	    first_generation) == 0);
	assert(controller->bss_published_cache.count ==
	    INTEL_AX211_BSS_CACHE_LIMIT);

	second_generation = UINT64_C(0x300000002);
	assert(published_station->ops->scan_channel_start(controller,
	    second_generation, 0U, 1U,
	    deadline) == 0);
	assert(controller->bss_staging_cache.count == 0U);
	assert(controller->bss_published_cache.count ==
	    INTEL_AX211_BSS_CACHE_LIMIT);
	fixture_rx_event(INTEL_AX211_SCAN_REQUEST_OPCODE,
	    INTEL_AX211_SCAN_GROUP_LONG, 1U, NULL, 0U);
	assert(published_device->ops->poll_receive(published_device, 1U) == 1U);
	memset(&entry, 0, sizeof(entry));
	memcpy(entry.bssid, selected_bssid, sizeof(entry.bssid));
	entry.observation_generation = second_generation;
	entry.frame_timestamp = 1000U;
	entry.receive_tsf = 1000U;
	entry.hardware_epoch = controller->hardware_epoch;
	entry.gp2_on_air_rise = 1000U;
	entry.last_seen_ticks = 1000U;
	entry.beacon_interval_tu = 100U;
	entry.capability = 0x20U;
	entry.channel = 1U;
	entry.dtim_period = 1U;
	entry.tim_valid = 1U;
	entry.receive_tsf_valid = 1U;
	entry.source = INTEL_AX211_BSS_SOURCE_BEACON;
	entry.valid = 1U;
	assert(intel_ax211_bss_cache_observe(&controller->bss_staging_cache,
	    &entry) ==
	    INTEL_AX211_BSS_OK);
	assert(controller->bss_staging_cache.count == 1U);
	fixture_rx_event(INTEL_AX211_SCAN_COMPLETE_OPCODE,
	    INTEL_AX211_SCAN_GROUP_LEGACY, 0x80U, NULL, 0U);
	assert(published_device->ops->poll_receive(published_device, 1U) == 1U);
	assert(published_station->ops->scan_stop(controller,
	    second_generation) == 0);
	assert(controller->bss_staging_cache.count == 1U);
	assert(controller->bss_staging_initialized == 1U);
	assert(controller->bss_published_generation == first_generation);
	/* Complete the same generation on its final 5-GHz channel.  The BSS
	 * found on channel 1 must survive and remain association metadata. */
	assert(published_station->ops->scan_channel_start(controller,
	    second_generation, AX211_PASSIVE_CHANNEL_COUNT - 1U, 36U,
	    deadline) == 0);
	fixture_rx_event(INTEL_AX211_SCAN_REQUEST_OPCODE,
	    INTEL_AX211_SCAN_GROUP_LONG, 1U, NULL, 0U);
	assert(published_device->ops->poll_receive(published_device, 1U) == 1U);
	fixture_rx_event(INTEL_AX211_SCAN_COMPLETE_OPCODE,
	    INTEL_AX211_SCAN_GROUP_LEGACY, 0x80U, NULL, 0U);
	assert(published_device->ops->poll_receive(published_device, 1U) == 1U);
	assert(published_station->ops->scan_stop(controller,
	    second_generation) == 0);
	assert(controller->bss_published_cache.count == 1U);
	assert(controller->bss_published_generation == second_generation);

	/* A later cancelled partial scan must not overwrite the last snapshot. */
	aborted_generation = UINT64_C(0x300000004);
	assert(published_station->ops->scan_channel_start(controller,
	    aborted_generation, 0U, 1U, deadline) == 0);
	fixture_rx_event(INTEL_AX211_SCAN_REQUEST_OPCODE,
	    INTEL_AX211_SCAN_GROUP_LONG, 1U, NULL, 0U);
	assert(published_device->ops->poll_receive(published_device, 1U) == 1U);
	entry.observation_generation = aborted_generation;
	entry.frame_timestamp = 2000U;
	entry.receive_tsf = 2000U;
	entry.gp2_on_air_rise = 2000U;
	entry.last_seen_ticks = 2000U;
	entry.beacon_interval_tu = 200U;
	assert(intel_ax211_bss_cache_observe(&controller->bss_staging_cache,
	    &entry) == INTEL_AX211_BSS_OK);
	assert(published_station->ops->scan_stop(controller,
	    aborted_generation) == EBUSY);
	fixture_rx_event(INTEL_AX211_SCAN_ABORT_OPCODE,
	    INTEL_AX211_SCAN_GROUP_LONG, 1U, NULL, 0U);
	assert(published_device->ops->poll_receive(published_device, 1U) == 1U);
	assert(published_station->ops->scan_stop(controller,
	    aborted_generation) == 0);
	assert(controller->bss_staging_initialized == 0U);
	assert(controller->bss_published_generation == second_generation);
	memset(&entry, 0, sizeof(entry));
	assert(intel_ax211_bss_cache_lookup(&controller->bss_published_cache,
	    selected_bssid, 1U, controller->hardware_epoch, &entry) ==
	    INTEL_AX211_BSS_OK);
	assert(entry.observation_generation == second_generation);
	assert(entry.beacon_interval_tu == 100U);

	memset(&bss, 0, sizeof(bss));
	memcpy(bss.bssid, selected_bssid, sizeof(bss.bssid));
	bss.channel = 1U;
	connection_generation = UINT64_C(0x300000003);
	assert(published_station->ops->connect_start(controller,
	    connection_generation, &bss, deadline) == 0);
	assert(controller->selected_bss.observation_generation ==
	    second_generation);
	assert(published_station->ops->disconnect(controller,
	    connection_generation) == 0);
	published_device->ops->close(published_device);
	assert(dma_allocations == dma_frees);
	assert(ax211_pci_detach(&device, 0U) == 0);
}

static void
test_association_failure_unwind_and_tx_timeout(void)
{
	static const uint8_t bssid[6] = {
		0x02U, 0x66U, 0x77U, 0x88U, 0x99U, 0xaaU
	};
	struct ax211_pci_controller *controller;
	struct wlan_radio_tx_request transmit;
	struct wlan_bss_record bss;
	struct drv_pci_device device;
	uint8_t frame[24];
	uint64_t deadline;

	memset(&bss, 0, sizeof(bss));
	memcpy(bss.bssid, bssid, sizeof(bssid));
	bss.channel = 1U;

	fixture_reset(&device);
	assert(drv_pci_intel_ax211_driver_register() == 0);
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == 0);
	drv_pci_intel_ax211_devices_ready();
	controller = device.driver_data;
	assert(published_device->ops->open(published_device) == 0);
	fixture_seed_bss(controller, bssid);
	fixture_command_malformed_queue = 1U;
	deadline = clock_ticks() + 100U;
	assert(published_station->ops->connect_start(controller, 41U, &bss,
	    deadline) == EIO);
	assert(controller->runtime_active == 1U);
	assert(controller->quarantined == 0U);
	assert(controller->tx_ring.enabled == 0U);
	assert(ax211_pci_detach(&device, 0U) == 0);
	assert(dma_allocations == dma_frees);

	/* A firmware fatal edge is diagnosed from the ALIVE SRAM tables before
	 * command cancellation releases NIC ownership. */
	fixture_reset(&device);
	assert(drv_pci_intel_ax211_driver_register() == 0);
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == 0);
	drv_pci_intel_ax211_devices_ready();
	controller = device.driver_data;
	assert(published_device->ops->open(published_device) == 0);
	fixture_seed_bss(controller, bssid);
	controller->runtime_start.alive_accepted = 1U;
	controller->runtime_start.alive.status =
	    INTEL_AX211_PROTOCOL_ALIVE_STATUS_OK;
	controller->runtime_start.alive.lmac[0].error_event_table =
	    FIXTURE_SRAM_BASE;
	controller->runtime_start.alive.umac.error_info =
	    FIXTURE_UMAC_ERROR_ADDRESS | AX211_FW_ADDR_CACHE_CONTROL;
	fixture_sram[0U] = 1U;
	fixture_sram[1U] = 0x38U;
	fixture_sram[7U] = 0x11111111U;
	fixture_sram[8U] = 0x22222222U;
	fixture_sram[9U] = 0x33333333U;
	fixture_sram[23U] = 0x00030128U;
	fixture_sram[29U] = 0x00000028U;
	fixture_sram[64U + 0U] = 1U;
	fixture_sram[64U + 1U] = 0x35U;
	fixture_sram[64U + 6U] = 0x44444444U;
	fixture_sram[64U + 7U] = 0x55555555U;
	fixture_sram[64U + 8U] = 0x66666666U;
	fixture_sram[64U + 13U] = 0x00030128U;
	fixture_sram[64U + 14U] = 0x77777777U;
	fixture_command_fatal_mac = 1U;
	deadline = clock_ticks() + 100U;
	assert(published_station->ops->connect_start(controller, 42U, &bss,
	    deadline) == EIO);
	assert(strstr(log_buffer, "LMAC firmware error") != NULL);
	assert(strstr(log_buffer, "id=00000038") != NULL);
	assert(strstr(log_buffer, "hcmd=00030128 last=00000028") != NULL);
	assert(strstr(log_buffer, "UMAC firmware error") != NULL);
	assert(controller->mmio.nic_lock_depth == 0U);
	assert(controller->runtime_active == 0U);
	assert(controller->quarantined == 1U);
	assert(dma_allocations == dma_frees);
	assert(ax211_pci_detach(&device, 0U) == 0);

	fixture_reset(&device);
	assert(drv_pci_intel_ax211_driver_register() == 0);
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == 0);
	drv_pci_intel_ax211_devices_ready();
	controller = device.driver_data;
	assert(published_device->ops->open(published_device) == 0);
	fixture_seed_bss(controller, bssid);
	fixture_command_timeout = 1U;
	deadline = clock_ticks() + 1U;
	assert(published_station->ops->connect_start(controller, 42U, &bss,
	    deadline) != 0);
	assert(controller->runtime_active == 0U);
	assert(controller->quarantined == 1U);
	assert(dma_allocations == dma_frees);
	fixture_command_timeout = 0U;
	assert(ax211_pci_detach(&device, 0U) == 0);

	/* A rejected MAC power table is an association transaction failure. */
	fixture_reset(&device);
	assert(drv_pci_intel_ax211_driver_register() == 0);
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == 0);
	drv_pci_intel_ax211_devices_ready();
	controller = device.driver_data;
	assert(published_device->ops->open(published_device) == 0);
	fixture_seed_bss(controller, bssid);
	deadline = clock_ticks() + 100U;
	assert(published_station->ops->connect_start(controller, 43U, &bss,
	    deadline) == 0);
	fixture_power_status = 1U;
	assert(published_station->ops->association_set(controller, 43U, bssid,
	    1U, deadline) == EIO);
	assert(fixture_mcast_command_order != 0U);
	assert(fixture_power_command_order == fixture_mcast_command_order + 1U);
	assert(controller->runtime_active == 0U);
	assert(controller->quarantined == 1U);
	assert(dma_allocations == dma_frees);
	assert(ax211_pci_detach(&device, 0U) == 0);

	/* A truncated generic MAC power response fails closed as well. */
	fixture_reset(&device);
	assert(drv_pci_intel_ax211_driver_register() == 0);
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == 0);
	drv_pci_intel_ax211_devices_ready();
	controller = device.driver_data;
	assert(published_device->ops->open(published_device) == 0);
	fixture_seed_bss(controller, bssid);
	deadline = clock_ticks() + 100U;
	assert(published_station->ops->connect_start(controller, 44U, &bss,
	    deadline) == 0);
	fixture_power_response_length_delta = -1;
	assert(published_station->ops->association_set(controller, 44U, bssid,
	    1U, deadline) == EIO);
	assert(fixture_mcast_command_order != 0U);
	assert(fixture_power_command_order == fixture_mcast_command_order + 1U);
	assert(controller->runtime_active == 0U);
	assert(controller->quarantined == 1U);
	assert(dma_allocations == dma_frees);
	assert(ax211_pci_detach(&device, 0U) == 0);

	fixture_reset(&device);
	assert(drv_pci_intel_ax211_driver_register() == 0);
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == 0);
	drv_pci_intel_ax211_devices_ready();
	controller = device.driver_data;
	assert(published_device->ops->open(published_device) == 0);
	fixture_seed_bss(controller, bssid);
	deadline = clock_ticks() + 100U;
	assert(published_station->ops->connect_start(controller, 45U, &bss,
	    deadline) == 0);
	/* WMM scan capability must not enable QoS before common negotiates it. */
	assert(controller->association.profile.qos == 0U);
	memset(frame, 0, sizeof(frame));
	frame[0U] = 0xb0U;
	memset(&transmit, 0, sizeof(transmit));
	transmit.generation = 45U;
	transmit.cookie = 99U;
	transmit.deadline_ticks = clock_ticks() + 1U;
	transmit.frame_class = WLAN_RADIO_FRAME_MANAGEMENT;
	transmit.frame = frame;
	transmit.length = sizeof(frame);
	assert(published_station->ops->frame_transmit(controller, &transmit) == 0);
	fixture_clock += 20000U;
	assert(published_device->ops->poll_receive(published_device, 1U) == 0U);
	assert(tx_completion_reports == 1U && reported_tx_cookie == 99U);
	assert(controller->runtime_active == 0U);
	assert(controller->quarantined == 1U);
	assert(dma_allocations == dma_frees);
	assert(ax211_pci_detach(&device, 0U) == 0);
}

int
main(void)
{
	test_registration_and_exact_match();
	test_duplicate_registration_retains_controller();
	test_attach_persists_until_refresh();
	test_strap_publication_and_reverse_detach();
	test_otp_fallback_and_retry();
	test_partial_publication_retry();
	test_checked_detach_retry();
	test_attach_restore_failure_quarantine();
	test_attach_failure_unwind();
	test_open_failure_unwind();
	test_checked_runtime_drain_retry();
	test_common_up_down_up_lifecycle();
	test_transmit_lease_detach_join();
	test_malformed_rx_is_dropped();
	test_failed_key_add_scrubs_plaintext();
	test_tx_kick_schedules_recovery();
	test_recovery_join_retries_without_free();
	test_notification_layout_versions();
	test_receive_copy_replenish_and_poll_boundary();
	test_scan_session_and_rx_poll_integration();
	test_scan_generation_resets_full_bss_cache();
	test_association_key_tx_rx_disconnect_sequence();
	test_association_failure_unwind_and_tx_timeout();
	puts("intel ax211 persistent PCI lifecycle fixture: PASS");
	return 0;
}
