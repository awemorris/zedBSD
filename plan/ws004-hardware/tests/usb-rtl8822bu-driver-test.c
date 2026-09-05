/*
 * RTL8822BU exact USB/register/lifecycle fixture
 * SPDX-License-Identifier: Zlib
 */

#include <drivers/usb.h>
#include <kern/net/net-device.h>
#include <kern/net/wlan.h>

#include <assert.h>
#include <errno.h>
#define sched_yield host_sched_yield
#include <pthread.h>
#undef sched_yield
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct drv_usb_device {
	struct drv_usb_device_descriptor descriptor;
	enum drv_usb_speed speed;
	unsigned hcd_capabilities;
};

struct drv_usb_endpoint {
	struct drv_usb_endpoint_descriptor descriptor;
	enum drv_usb_transfer_type type;
};

struct drv_usb_host_interface {
	struct drv_usb_endpoint *endpoints[5];
	unsigned endpoint_count;
};

struct drv_usb_interface {
	struct drv_usb_device *device;
	struct drv_usb_interface_descriptor descriptor;
	struct drv_usb_host_interface alternate;
	void *driver_data;
};

struct drv_usb_urb {
	struct drv_usb_device *device;
	struct drv_usb_endpoint *endpoint;
	void *buffer;
	size_t length;
	size_t actual;
	enum drv_usb_urb_status status;
	drv_usb_urb_callback_t callback;
	void *callback_argument;
};

struct wlan_station {
	unsigned marker;
};

#define RTL8822B_HOST_TEST 1
#define RTL8822B_TESTING 1
#include "../../../src/drivers/rtl8822b.c"

static int __attribute__((unused)) fixture_firmware_load(
	struct rtl8822b_firmware_blob *firmware);
static void __attribute__((unused)) fixture_firmware_release(
	struct rtl8822b_firmware_blob *firmware);
static int test_firmware_walk(const struct rtl8822b_firmware_view *view,
	rtl8822b_firmware_chunk_fn callback, void *context);

#define RTL8822BU_FIRMWARE_LOAD fixture_firmware_load
#define RTL8822BU_FIRMWARE_RELEASE fixture_firmware_release
#define RTL8822BU_FIRMWARE_WALK test_firmware_walk
#include "../../../src/drivers/usb-rtl8822bu.c"

#define FIXTURE_SEC_COMMAND       0x0670U
#define FIXTURE_SEC_WRITE         0x0674U
#define FIXTURE_RCR               0x0608U
#define FIXTURE_FIFO_PAGE_HIGH    0x0230U
#define FIXTURE_FIFO_PAGE_LOW     0x0234U
#define FIXTURE_FIFO_PAGE_NORMAL  0x0238U
#define FIXTURE_FIFO_PAGE_EXTRA   0x023cU
#define FIXTURE_CAM_WRITE_ENABLE  0x00010000U
#define FIXTURE_CAM_POLLING       0x80000000U

static uint8_t fake_registers[UINT16_MAX + 1U];
static uint8_t fake_efuse[RTL8822B_EFUSE_PHYSICAL_SIZE];
static uint32_t fake_cam[RTL8822B_CAM_ENTRY_COUNT][8];
static struct net_device fake_net_device;
static struct wlan_station fake_station;
static unsigned control_calls;
static unsigned control_fail_at;
static uint16_t control_fail_address;
static unsigned control_fail_address_remaining;
static unsigned sec_command_write_fail_skip;
static unsigned sec_command_write_fail_enabled;
static unsigned control_short_at;
static unsigned control_stall_at;
static unsigned control_stall_remaining;
static unsigned access_off_count;
static unsigned allocations;
static unsigned allocation_calls;
static unsigned allocation_fail_at;
static unsigned net_created;
static unsigned net_gone;
static unsigned net_gone_calls;
static int net_create_error;
static int net_gone_error;
static unsigned station_attached;
static unsigned station_detached;
static unsigned station_detach_calls;
static unsigned station_open_calls;
static unsigned station_close_calls;
static unsigned link_loss_calls;
static uint64_t link_loss_generation;
static int link_loss_reason;
static int link_loss_error;
static struct rtl8822bu_adapter *link_loss_cleanup_adapter;
static uint64_t link_loss_cleanup_pairwise_generation;
static uint64_t link_loss_cleanup_group_generation;
static uint8_t link_loss_cleanup_group_index;
static unsigned scan_ready_calls;
static uint64_t scan_ready_generation;
static uint32_t scan_ready_step;
static unsigned lifecycle_sequence;
static unsigned net_gone_sequence;
static unsigned station_detach_sequence;
static int station_attach_error;
static int station_detach_error;
static int station_open_error;
static int station_close_error;
static unsigned station_close_busy_count;
static struct rtl8822bu_adapter *station_close_cleanup_adapter;
static uint64_t station_close_cleanup_generation;
static uint64_t station_close_cleanup_pairwise_generation;
static uint64_t station_close_cleanup_group_generation;
static uint8_t station_close_cleanup_group_index;
static int scan_ready_error;
static unsigned open_during_create;
static int open_during_create_error;
static unsigned callback_during_gone;
static int callback_during_gone_error;
static unsigned open_count_during_gone;
static unsigned close_calls_during_gone;
static unsigned defer_net_release;
static void (*deferred_release)(void *);
static void *deferred_driver_data;
static unsigned carrier_block_once;
static unsigned carrier_block_entered;
static unsigned carrier_block_release;
static unsigned mutex_contentions;
static unsigned threaded_test_active;
static unsigned net_callbacks_active;
static unsigned net_join_waiting;
static unsigned ioctl_block_once;
static unsigned ioctl_block_entered;
static unsigned ioctl_block_release;
static unsigned report_block_once;
static unsigned report_block_entered;
static unsigned report_block_release;
static unsigned rx_submit_block_once;
static unsigned rx_submit_block_entered;
static unsigned rx_submit_block_release;
static unsigned rx_status_block_once;
static unsigned rx_status_block_entered;
static unsigned rx_status_block_release;
static unsigned rx_start_post_access_complete;
static unsigned require_rx_post_before_free;
static unsigned bulk_calls;
static unsigned bulk_fail_at;
static unsigned bulk_short_at;
static unsigned bulk_stall_at;
static unsigned bulk_stall_remaining;
static unsigned firmware_chunk_count;
static unsigned firmware_checksum_fail;
static unsigned firmware_ready_suppressed;
static unsigned firmware_ready_forbidden_bit;
static unsigned beacon_completion_suppressed;
static unsigned firmware_descriptors_checked;
static unsigned management_descriptors_checked;
static unsigned scan_probe_descriptors_checked;
static unsigned deauthentication_descriptors_checked;
static unsigned station_descriptors_checked;
static unsigned data_descriptors_checked;
static uint8_t last_tx_sequence;
static unsigned cam_writes;
static unsigned firmware_load_calls;
static unsigned firmware_release_calls;
static int firmware_load_error;
static unsigned urb_submit_calls;
static int urb_submit_error;
static unsigned urb_cancel_calls;
static unsigned urb_drain_calls;
static unsigned clear_halt_calls;
static int clear_halt_error;
static unsigned poll_schedule_calls;
static unsigned scan_report_calls;
static uint64_t scan_report_generation;
static uint8_t scan_report_channel;
static int32_t scan_report_rssi;
static unsigned frame_report_calls;
static struct wlan_radio_rx_frame last_frame_report;
static unsigned tx_report_calls;
static uint64_t tx_report_generation;
static uint64_t tx_report_cookie;
static int tx_report_acknowledged;
static int tx_report_error;
static int tx_report_return_error;
static unsigned station_transmit_calls;
static int station_transmit_error;
static unsigned station_transmit_block_once;
static unsigned station_transmit_block_entered;
static unsigned station_transmit_block_release;
static unsigned queue_read_block_once;
static unsigned queue_read_block_entered;
static unsigned queue_read_block_release;
static unsigned storage_progress;
static unsigned transport_forced_absent;
static unsigned transport_io_after_absence;
static unsigned route_data_to_normal_endpoint;
static unsigned device_old_rx_payload_pending;
static unsigned old_rx_payload_deliveries;
static unsigned rx_generation_flushes;
static uint64_t fake_clock_ticks;
static struct drv_usb_urb *persistent_rx_urb;
static void (*yield_hook)(void);
static unsigned yield_calls;

static void wait_for_atomic_nonzero(unsigned *);

struct key_delete_thread_context {
	struct rtl8822bu_adapter *adapter;
	uint64_t generation;
	uint64_t key_generation;
	uint64_t deadline;
	int result;
	unsigned done;
};

static void *
run_key_delete_thread(void *argument)
{
	struct key_delete_thread_context *context = argument;

	context->result = rtl8822bu_key_delete(context->adapter,
	    context->generation, WLAN_RADIO_KEY_PAIRWISE, 0U,
	    context->key_generation, context->deadline);
	__atomic_store_n(&context->done, 1U, __ATOMIC_RELEASE);
	return NULL;
}

enum fixture_thread_role {
	FIXTURE_ROLE_NONE,
	FIXTURE_ROLE_START,
	FIXTURE_ROLE_STOP,
	FIXTURE_ROLE_DETACH
};

static _Thread_local enum fixture_thread_role fixture_thread_role;

static uint16_t
fake_le16(const uint8_t *bytes)
{
	return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t
fake_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	    ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void
fake_store16(uint16_t address, uint16_t value)
{
	fake_registers[address] = (uint8_t)value;
	fake_registers[address + 1U] = (uint8_t)(value >> 8);
}

static void
fake_store32(uint16_t address, uint32_t value)
{
	fake_registers[address] = (uint8_t)value;
	fake_registers[address + 1U] = (uint8_t)(value >> 8);
	fake_registers[address + 2U] = (uint8_t)(value >> 16);
	fake_registers[address + 3U] = (uint8_t)(value >> 24);
}

static void
fake_efuse_block(size_t *offset, unsigned block, const uint8_t bytes[8])
{
	assert(offset != NULL && bytes != NULL && block < 96U);
	assert(*offset <= sizeof(fake_efuse) - 10U);
	fake_efuse[(*offset)++] = (uint8_t)(((block & 7U) << 5) | 0x0fU);
	fake_efuse[(*offset)++] = (uint8_t)(((block & 0x78U) << 1) | 0U);
	memcpy(fake_efuse + *offset, bytes, 8U);
	*offset += 8U;
}

static void
fake_efuse_make_board(void)
{
	static const uint8_t block2[8] = {
		32U, 33U, 34U, 35U, 36U, 37U, 30U, 31U
	};
	static const uint8_t block3[8] = {
		32U, 33U, 34U, 0xaeU, 0xffU, 0xffU, 0xffU, 0xffU
	};
	static const uint8_t block4[8] = {
		0xffU, 0xffU, 24U, 25U, 26U, 27U, 28U, 29U
	};
	static const uint8_t block5[8] = {
		30U, 31U, 32U, 33U, 34U, 35U, 36U, 37U
	};
	static const uint8_t block6[8] = {
		0xafU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU
	};
	static const uint8_t block7[8] = {
		0xffU, 0xffU, 28U, 29U, 30U, 31U, 32U, 33U
	};
	static const uint8_t block8[8] = {
		26U, 27U, 28U, 29U, 30U, 0xb3U, 0xffU, 0xffU
	};
	static const uint8_t block9[8] = {
		0xffU, 0xffU, 0xffU, 0xffU, 20U, 21U, 22U, 23U
	};
	static const uint8_t block10[8] = {
		24U, 25U, 26U, 27U, 28U, 29U, 30U, 31U
	};
	static const uint8_t block11[8] = {
		32U, 33U, 0xb2U, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU
	};
	static const uint8_t block23[8] = {
		0x7f, 0x22, 0x19, 0xff, 0xff, 0xff, 0xff, 0xff
	};
	static const uint8_t block24[8] = {
		0xff, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
	};
	static const uint8_t block25[8] = {
		0xff, 0xff, 0x02, 'J', 'P', 0xff, 0xff, 0xff
	};
	static const uint8_t block32[8] = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02
	};
	static const uint8_t block33[8] = {
		0x11, 0x22, 0x33, 0x44, 0x55, 0xff, 0xff, 0xff
	};
	size_t offset = 0U;

	memset(fake_efuse, 0xff, sizeof(fake_efuse));
	fake_efuse_block(&offset, 2U, block2);
	fake_efuse_block(&offset, 3U, block3);
	fake_efuse_block(&offset, 4U, block4);
	fake_efuse_block(&offset, 5U, block5);
	fake_efuse_block(&offset, 6U, block6);
	fake_efuse_block(&offset, 7U, block7);
	fake_efuse_block(&offset, 8U, block8);
	fake_efuse_block(&offset, 9U, block9);
	fake_efuse_block(&offset, 10U, block10);
	fake_efuse_block(&offset, 11U, block11);
	fake_efuse_block(&offset, 23U, block23);
	fake_efuse_block(&offset, 24U, block24);
	fake_efuse_block(&offset, 25U, block25);
	fake_efuse_block(&offset, 32U, block32);
	fake_efuse_block(&offset, 33U, block33);
}

static void
fake_transport_reset(void)
{
	assert(deferred_release == NULL && deferred_driver_data == NULL);
	memset(fake_registers, 0, sizeof(fake_registers));
	memset(fake_cam, 0, sizeof(fake_cam));
	fake_efuse_make_board();
	control_calls = 0U;
	control_fail_at = 0U;
	control_fail_address = 0U;
	control_fail_address_remaining = 0U;
	sec_command_write_fail_skip = 0U;
	sec_command_write_fail_enabled = 0U;
	control_short_at = 0U;
	control_stall_at = 0U;
	control_stall_remaining = 0U;
	access_off_count = 0U;
	bulk_calls = 0U;
	bulk_fail_at = 0U;
	bulk_short_at = 0U;
	bulk_stall_at = 0U;
	bulk_stall_remaining = 0U;
	firmware_chunk_count = 0U;
	firmware_checksum_fail = 0U;
	firmware_ready_suppressed = 0U;
	firmware_ready_forbidden_bit = 0U;
	beacon_completion_suppressed = 0U;
	firmware_descriptors_checked = 0U;
	management_descriptors_checked = 0U;
	scan_probe_descriptors_checked = 0U;
	deauthentication_descriptors_checked = 0U;
	station_descriptors_checked = 0U;
	data_descriptors_checked = 0U;
	last_tx_sequence = 0U;
	cam_writes = 0U;
	firmware_load_calls = 0U;
	firmware_release_calls = 0U;
	firmware_load_error = 0;
	urb_submit_error = 0;
	urb_submit_calls = 0U;
	urb_cancel_calls = 0U;
	urb_drain_calls = 0U;
	clear_halt_calls = 0U;
	clear_halt_error = 0;
	poll_schedule_calls = 0U;
	fake_clock_ticks = 1U;
	net_created = 0U;
	net_gone = 0U;
	net_gone_calls = 0U;
	net_create_error = 0;
	net_gone_error = 0;
	station_attached = 0U;
	station_detached = 0U;
	station_detach_calls = 0U;
	station_open_calls = 0U;
	station_close_calls = 0U;
	link_loss_calls = 0U;
	link_loss_generation = 0U;
	link_loss_reason = 0;
	link_loss_error = 0;
	link_loss_cleanup_adapter = NULL;
	link_loss_cleanup_pairwise_generation = 0U;
	link_loss_cleanup_group_generation = 0U;
	link_loss_cleanup_group_index = 0U;
	scan_ready_calls = 0U;
	scan_ready_generation = 0U;
	scan_ready_step = 0U;
	station_attach_error = 0;
	station_detach_error = 0;
	station_open_error = 0;
	station_close_error = 0;
	station_close_busy_count = 0U;
	station_close_cleanup_adapter = NULL;
	station_close_cleanup_generation = 0U;
	station_close_cleanup_pairwise_generation = 0U;
	station_close_cleanup_group_generation = 0U;
	station_close_cleanup_group_index = 0U;
	scan_ready_error = 0;
	frame_report_calls = 0U;
	memset(&last_frame_report, 0, sizeof(last_frame_report));
	tx_report_calls = 0U;
	tx_report_generation = 0U;
	tx_report_cookie = 0U;
	tx_report_acknowledged = 0;
	tx_report_error = 0;
	tx_report_return_error = 0;
	station_transmit_calls = 0U;
	station_transmit_error = 0;
	station_transmit_block_once = 0U;
	station_transmit_block_entered = 0U;
	station_transmit_block_release = 0U;
	queue_read_block_once = 0U;
	queue_read_block_entered = 0U;
	queue_read_block_release = 0U;
	storage_progress = 0U;
	transport_forced_absent = 0U;
	transport_io_after_absence = 0U;
	route_data_to_normal_endpoint = 0U;
	device_old_rx_payload_pending = 0U;
	old_rx_payload_deliveries = 0U;
	rx_generation_flushes = 0U;
	open_count_during_gone = 0U;
	close_calls_during_gone = 0U;
	defer_net_release = 0U;
	carrier_block_once = 0U;
	carrier_block_entered = 0U;
	carrier_block_release = 0U;
	mutex_contentions = 0U;
	threaded_test_active = 0U;
	net_callbacks_active = 0U;
	net_join_waiting = 0U;
	ioctl_block_once = 0U;
	ioctl_block_entered = 0U;
	ioctl_block_release = 0U;
	report_block_once = 0U;
	report_block_entered = 0U;
	report_block_release = 0U;
	rx_submit_block_once = 0U;
	rx_submit_block_entered = 0U;
	rx_submit_block_release = 0U;
	rx_status_block_once = 0U;
	rx_status_block_entered = 0U;
	rx_status_block_release = 0U;
	rx_start_post_access_complete = 0U;
	require_rx_post_before_free = 0U;
	fixture_thread_role = FIXTURE_ROLE_NONE;
	persistent_rx_urb = NULL;
	fake_store16(RTL8822BU_REG_SYS_FUNC_EN, 0x0440U);
	fake_store16(RTL8822BU_REG_SYS_CLKR, 0x4080U);
	fake_store32(RTL8822BU_REG_LDO_EFUSE_CTRL, 0x80000355U);
	/* RTL8822BU is a two-path part; exercise both EFUSE power records. */
	fake_store32(RTL8822BU_REG_SYS_CFG1, 0x08800000U);
	fake_registers[RTL8822BU_REG_RSV_CTRL + 1U] =
	    RTL8822BU_WCPU_IO_ENABLE | 0xa0U;
	/* The exact-radio core starts from a genuinely powered-off device. */
	fake_registers[RTL8822BU_REG_CR] = 0xeaU;
	fake_registers[RTL8822BU_REG_CR + 1U] = 0x22U;
	fake_registers[RTL8822BU_REG_TXDMA_PQ_MAP + 1U] = 0x55U;
	fake_store16(RTL8822BU_REG_FIFOPAGE_CTRL_2, 0x0123U);
	fake_store16(RTL8822BU_REG_FIFOPAGE_INFO_1, 0x4567U);
	fake_store32(RTL8822BU_REG_RQPN_CTRL_2, 0x12345678U);
	fake_registers[RTL8822BU_REG_BCN_CTRL] = 0xa5U;
	fake_store32(RTL8822BU_REG_H2CQ_CSR, 0x10203040U);
	fake_registers[RTL8822BU_REG_CPU_DMEM_CON + 2U] = 0x81U;
	fake_store16(RTL8822BU_REG_MCUFW_CTRL, 0x3000U);
	fake_store32(RTL8822B_RF_DIRECT_A + 0x18U * 4U, 0x00000c01U);
	fake_store32(RTL8822B_RF_DIRECT_B + 0x18U * 4U, 0x00000c01U);
	fake_store32(0x0c50U, 0x20U);
	fake_store32(0x0e50U, 0x20U);
}

void *
hal_malloc(size_t size)
{
	void *result;

	allocation_calls++;
	if (allocation_calls == allocation_fail_at) {
		allocation_fail_at = 0U;
		return NULL;
	}
	result = malloc(size);

	if (result != NULL)
		allocations++;
	return result;
}

void
hal_free(void *pointer)
{
	if (pointer == NULL)
		return;
	assert(allocations != 0U);
	allocations--;
	free(pointer);
}

int
hal_printf(const char *format, ...)
{
	(void)format;
	return 0;
}

bool
hal_irq_disable(void)
{
	return true;
}

void
hal_irq_enable(void)
{
}

void
sched_yield(void)
{
	static const struct timespec pause = { 0, 1000 };

	(void)__atomic_add_fetch(&yield_calls, 1U, __ATOMIC_RELAXED);
	(void)__atomic_add_fetch(&fake_clock_ticks, 1U, __ATOMIC_RELAXED);
	if (yield_hook != NULL)
		yield_hook();
	if (__atomic_load_n(&threaded_test_active, __ATOMIC_RELAXED))
		(void)nanosleep(&pause, NULL);
}

uint64_t
clock_ticks(void)
{
	return __atomic_load_n(&fake_clock_ticks, __ATOMIC_RELAXED);
}

void
spin_init(struct spinlock *lock, enum lock_rank rank, const char *name)
{
	memset(lock, 0, sizeof(*lock));
	lock->rank = rank;
	lock->name = name;
}

unsigned long
spin_lock_irqsave(struct spinlock *lock)
{
	unsigned expected;

	assert(lock != NULL);
	for (;;) {
		expected = 0U;
		if (__atomic_compare_exchange_n(&lock->held.value, &expected, 1U,
		    0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
			return 1U;
		sched_yield();
	}
}

void
spin_unlock_irqrestore(struct spinlock *lock, unsigned long enabled)
{
	assert(lock != NULL && __atomic_exchange_n(&lock->held.value, 0U,
	    __ATOMIC_RELEASE) == 1U);
	(void)enabled;
}

int
mutex_init(struct mutex *mutex, enum lock_rank rank, const char *name)
{
	memset(mutex, 0, sizeof(*mutex));
	mutex->guard.rank = rank;
	mutex->guard.name = name;
	return 0;
}

void
mutex_lock(struct mutex *mutex)
{
	unsigned expected;

	assert(mutex != NULL);
	for (;;) {
		expected = 0U;
		if (__atomic_compare_exchange_n(&mutex->locked, &expected, 1U, 0,
		    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
			return;
		(void)__atomic_add_fetch(&mutex_contentions, 1U, __ATOMIC_RELAXED);
		sched_yield();
	}
}

void
mutex_unlock(struct mutex *mutex)
{
	assert(mutex != NULL &&
	    __atomic_exchange_n(&mutex->locked, 0U, __ATOMIC_RELEASE) == 1U);
}

struct drv_usb_device *
drv_usb_interface_device(const struct drv_usb_interface *interface)
{
	return interface == NULL ? NULL : interface->device;
}

const struct drv_usb_device_descriptor *
drv_usb_device_descriptor(const struct drv_usb_device *device)
{
	return device == NULL ? NULL : &device->descriptor;
}

enum drv_usb_speed
drv_usb_device_speed(const struct drv_usb_device *device)
{
	return device == NULL ? DRV_USB_SPEED_UNKNOWN : device->speed;
}

unsigned
drv_usb_device_hcd_capabilities(const struct drv_usb_device *device)
{
	return device == NULL ? 0U : device->hcd_capabilities;
}

const struct drv_usb_interface_descriptor *
drv_usb_interface_descriptor(const struct drv_usb_interface *interface)
{
	return interface == NULL ? NULL : &interface->descriptor;
}

unsigned
drv_usb_interface_alternate_count(const struct drv_usb_interface *interface)
{
	return interface == NULL ? 0U : 1U;
}

const struct drv_usb_host_interface *
drv_usb_interface_active_alternate(const struct drv_usb_interface *interface)
{
	return interface == NULL ? NULL : &interface->alternate;
}

unsigned
drv_usb_host_interface_endpoint_count(
	const struct drv_usb_host_interface *alternate)
{
	return alternate == NULL ? 0U : alternate->endpoint_count;
}

struct drv_usb_endpoint *
drv_usb_host_interface_endpoint(const struct drv_usb_host_interface *alternate,
	unsigned index)
{
	return alternate == NULL || index >= alternate->endpoint_count ? NULL :
	    alternate->endpoints[index];
}

const struct drv_usb_endpoint_descriptor *
drv_usb_endpoint_descriptor(const struct drv_usb_endpoint *endpoint)
{
	return endpoint == NULL ? NULL : &endpoint->descriptor;
}

enum drv_usb_transfer_type
drv_usb_endpoint_type(const struct drv_usb_endpoint *endpoint)
{
	return endpoint == NULL ? DRV_USB_TRANSFER_CONTROL : endpoint->type;
}

bool
drv_usb_endpoint_is_input(const struct drv_usb_endpoint *endpoint)
{
	return endpoint != NULL &&
	    (endpoint->descriptor.address & DRV_USB_DIR_IN) != 0U;
}

int
drv_usb_control(struct drv_usb_device *device, uint8_t request_type,
	uint8_t request, uint16_t value, uint16_t index, void *buffer,
	size_t length, unsigned timeout_ms, size_t *actual)
{
	uint32_t control;
	unsigned address;
	unsigned call;
	int write = (request_type & DRV_USB_DIR_IN) == 0U;
	int command_write_fail = 0;

	if (transport_forced_absent) {
		transport_io_after_absence++;
		return ENODEV;
	}
	call = ++control_calls;
	assert(device != NULL);
	assert(request == RTL8822BU_VENDOR_REQUEST);
	assert(index == 0U);
	assert(timeout_ms == RTL8822BU_REGISTER_TIMEOUT_MS);
	assert(length == 1U || length == 2U || length == 4U);
	assert((request_type & (DRV_USB_REQUEST_VENDOR | DRV_USB_RECIP_DEVICE)) ==
	    (DRV_USB_REQUEST_VENDOR | DRV_USB_RECIP_DEVICE));
	if (!write && value == FIXTURE_FIFO_PAGE_HIGH &&
	    __atomic_exchange_n(&queue_read_block_once, 0U,
	    __ATOMIC_ACQ_REL) != 0U) {
		__atomic_store_n(&queue_read_block_entered, 1U,
		    __ATOMIC_RELEASE);
		while (__atomic_load_n(&queue_read_block_release,
		    __ATOMIC_ACQUIRE) == 0U)
			sched_yield();
	}
	if (write && value == FIXTURE_SEC_COMMAND &&
	    sec_command_write_fail_enabled) {
		if (sec_command_write_fail_skip == 0U) {
			command_write_fail = 1;
			sec_command_write_fail_enabled = 0U;
		} else {
			sec_command_write_fail_skip--;
		}
	}
	if (control_stall_remaining != 0U && call >= control_stall_at) {
		control_stall_remaining--;
		if (actual != NULL)
			*actual = 0U;
		return EPIPE;
	}
	if (call == control_fail_at || command_write_fail ||
	    (control_fail_address != 0U && value == control_fail_address)) {
		control_fail_at = 0U;
		if (control_fail_address != 0U &&
		    value == control_fail_address &&
		    control_fail_address_remaining > 1U) {
			control_fail_address_remaining--;
		} else {
			control_fail_address = 0U;
			control_fail_address_remaining = 0U;
		}
		if (actual != NULL)
			*actual = 0U;
		return ETIMEDOUT;
	}
	if (write) {
		if (value == RTL8822BU_REG_FIFOPAGE_CTRL_2 && length == 2U) {
			uint16_t old = fake_le16(fake_registers + value);
			uint16_t incoming = fake_le16(buffer);
			uint16_t stored = (uint16_t)((incoming &
			    ~RTL8822BU_BEACON_VALID) |
			    (old & RTL8822BU_BEACON_VALID));

			/* BIT_BCN_VALID_V1 is write-one-to-clear. */
			if ((incoming & RTL8822BU_BEACON_VALID) != 0U)
				stored &= (uint16_t)~RTL8822BU_BEACON_VALID;
			fake_store16(value, stored);
			} else {
				memcpy(fake_registers + value, buffer, length);
			}
			if (value == RTL8822B_REG_RX_PACKET_NUMBER && length == 4U &&
			    (fake_le32(buffer) & RTL8822B_RX_RELEASE_ENABLE) != 0U) {
				fake_store32(value, fake_le32(buffer) |
				    RTL8822B_RXDMA_IDLE);
				device_old_rx_payload_pending = 0U;
				rx_generation_flushes++;
			}
		/* The radio start transaction reserves pages in each priority queue.
		 * The default fixture models an already drained MAC by mirroring the
		 * hardware's available counter; individual tests may then force a
		 * mismatch to exercise the bounded retry path. */
		if (length == 2U && (value == FIXTURE_FIFO_PAGE_HIGH ||
		    value == FIXTURE_FIFO_PAGE_LOW ||
		    value == FIXTURE_FIFO_PAGE_NORMAL ||
		    value == FIXTURE_FIFO_PAGE_EXTRA))
			fake_store16((uint16_t)(value + 2U),
			    fake_le16(buffer));
		if (value == FIXTURE_SEC_COMMAND && length == 4U) {
			uint32_t command = fake_le32(buffer);
			unsigned cam_address = command & 0xffU;

			if ((command & (FIXTURE_CAM_WRITE_ENABLE |
			    FIXTURE_CAM_POLLING)) == (FIXTURE_CAM_WRITE_ENABLE |
			    FIXTURE_CAM_POLLING)) {
				assert((cam_address >> 3) <
				    RTL8822B_CAM_ENTRY_COUNT);
				fake_cam[cam_address >> 3][cam_address & 7U] =
				    fake_le32(fake_registers + FIXTURE_SEC_WRITE);
				cam_writes++;
			}
			command &= ~FIXTURE_CAM_POLLING;
			fake_store32(FIXTURE_SEC_COMMAND, command);
		}
		/* A SIPI RF write is observed through the direct-read window.  Model
		 * that hardware alias so the core's bounded RF LUT/readback checks test
		 * the USB transport rather than a permanently-zero byte array. */
		if ((value == RTL8822B_RF_SIPI_A ||
		    value == RTL8822B_RF_SIPI_B) && length == 4U) {
			control = fake_le32(buffer);
			address = ((control >> 20) & 0xffU) * 4U +
			    (value == RTL8822B_RF_SIPI_A ? RTL8822B_RF_DIRECT_A :
			    RTL8822B_RF_DIRECT_B);
			fake_store32((uint16_t)address,
			    control & RTL8822B_RF_VALUE_MASK);
		}
		if (value == RTL8822BU_REG_EFUSE_ACCESS && length == 1U &&
		    *(const uint8_t *)buffer == RTL8822BU_EFUSE_ACCESS_OFF)
			access_off_count++;
		if (value == RTL8822BU_REG_DDMA_CH0CTRL && length == 4U) {
			uint32_t ddma = fake_le32(buffer);

			if ((ddma & RTL8822BU_DDMA_OWN) != 0U) {
				uint32_t destination = fake_le32(fake_registers +
				    RTL8822BU_REG_DDMA_CH0DA);
				uint32_t wire_source = fake_le32(fake_registers +
				    RTL8822BU_REG_DDMA_CH0SA);
				uint32_t transfer_length = ddma &
				    RTL8822BU_DDMA_LENGTH_MASK;

				assert(wire_source == RTL8822BU_TX_BUFFER_OCP +
				    RTL8822B_FIRMWARE_TX_DESCRIPTOR_SIZE);
				assert(transfer_length != 0U &&
				    transfer_length <= RTL8822B_FIRMWARE_CHUNK_MAX);
				assert((ddma & RTL8822BU_DDMA_CHECKSUM_ENABLE) != 0U);
				if (destination == RTL8822B_FIRMWARE_DMEM_ADDRESS ||
				    destination == RTL8822B_FIRMWARE_IMEM_ADDRESS)
					assert((ddma &
					    RTL8822BU_DDMA_CHECKSUM_CONTINUE) == 0U);
				else
					assert((ddma &
					    RTL8822BU_DDMA_CHECKSUM_CONTINUE) != 0U);
				firmware_chunk_count++;
				ddma &= ~RTL8822BU_DDMA_OWN;
				if (firmware_checksum_fail)
					ddma |= RTL8822BU_DDMA_CHECKSUM_ERROR;
				fake_store32(value, ddma);
			}
		}
		if (value == RTL8822BU_REG_SYS_FUNC_EN + 1U && length == 1U &&
		    (*(const uint8_t *)buffer & RTL8822BU_WCPU_ENABLE) != 0U &&
		    !firmware_ready_suppressed &&
		    (fake_le16(fake_registers + RTL8822BU_REG_MCUFW_CTRL) &
		    RTL8822BU_MCUFW_DOWNLOAD_READY) != 0U) {
			uint16_t firmware = fake_le16(fake_registers +
			    RTL8822BU_REG_MCUFW_CTRL);

			firmware |= RTL8822BU_MCUFW_INIT_READY;
			if (firmware_ready_forbidden_bit)
				firmware |= 0x0002U;
			fake_store16(RTL8822BU_REG_MCUFW_CTRL, firmware);
		}
	} else {
		/* Model the self-clearing card-enable/disable handshake bits rather
		 * than spending the entire deterministic deadline in a fake poll. */
		if (value == 0x0006U && length == 1U)
			fake_registers[value] |= 0x02U;
		if (value == 0x0005U && length == 1U)
			fake_registers[value] &= (uint8_t)~0x03U;
		if (value == RTL8822B_REG_AUTO_LLT && length == 1U)
			fake_registers[value] &= (uint8_t)~0x01U;
		if (value == RTL8822BU_REG_EFUSE_CTRL && length == 4U) {
			control = fake_le32(fake_registers + value);
			address = (control & RTL8822BU_EFUSE_ADDRESS_MASK) >>
			    RTL8822BU_EFUSE_ADDRESS_SHIFT;
			assert(address < sizeof(fake_efuse));
			control &= ~0xffU;
			control |= fake_efuse[address];
			control |= RTL8822BU_EFUSE_READY;
			fake_store32(value, control);
		}
		memcpy(buffer, fake_registers + value, length);
	}
	if (actual != NULL)
		*actual = call == control_short_at ? length - 1U : length;
	return 0;
}

int
drv_usb_interface_set_driver_data(struct drv_usb_interface *interface,
	void *data)
{
	if (interface == NULL)
		return EINVAL;
	interface->driver_data = data;
	return 0;
}

void *
drv_usb_interface_driver_data(const struct drv_usb_interface *interface)
{
	return interface == NULL ? NULL : interface->driver_data;
}

struct drv_usb_urb *
drv_usb_urb_alloc(struct drv_usb_device *device,
	struct drv_usb_endpoint *endpoint, unsigned iso_count)
{
	struct drv_usb_urb *urb;

	assert(iso_count == 0U);
	urb = hal_malloc(sizeof(*urb));
	if (urb != NULL) {
		memset(urb, 0, sizeof(*urb));
		urb->device = device;
		urb->endpoint = endpoint;
		urb->status = DRV_USB_URB_IDLE;
	}
	return urb;
}

void
drv_usb_urb_free(struct drv_usb_urb *urb)
{
	assert(urb == NULL || __atomic_load_n(&urb->status,
	    __ATOMIC_ACQUIRE) != DRV_USB_URB_PENDING);
	if (urb != NULL && urb->endpoint != NULL &&
	    urb->endpoint->descriptor.address == RTL8822BU_BULK_IN_ADDRESS &&
	    __atomic_load_n(&require_rx_post_before_free,
	    __ATOMIC_ACQUIRE) != 0U)
		assert(__atomic_load_n(&rx_start_post_access_complete,
		    __ATOMIC_ACQUIRE) != 0U);
	hal_free(urb);
}

int
drv_usb_urb_setup(struct drv_usb_urb *urb, void *buffer, size_t length,
	unsigned flags, unsigned timeout, drv_usb_urb_callback_t callback,
	void *argument)
{
	assert(flags == 0U && timeout == 0U);
	if (urb == NULL || buffer == NULL || length == 0U ||
	    __atomic_load_n(&urb->status,
	    __ATOMIC_ACQUIRE) == DRV_USB_URB_PENDING)
		return EINVAL;
	urb->buffer = buffer;
	urb->length = length;
	urb->actual = 0U;
	urb->callback = callback;
	urb->callback_argument = argument;
	__atomic_store_n(&urb->status, DRV_USB_URB_IDLE, __ATOMIC_RELEASE);
	return 0;
}

int
drv_usb_urb_submit(struct drv_usb_urb *urb)
{
	if (urb == NULL || urb->buffer == NULL || urb->length == 0U ||
	    __atomic_load_n(&urb->status,
	    __ATOMIC_ACQUIRE) == DRV_USB_URB_PENDING)
		return EINVAL;
	if (transport_forced_absent) {
		transport_io_after_absence++;
		return ENODEV;
	}
	urb_submit_calls++;
	if (urb_submit_error != 0) {
		int error = urb_submit_error;

		urb_submit_error = 0;
		return error;
	}
	__atomic_store_n(&urb->status, DRV_USB_URB_PENDING, __ATOMIC_RELEASE);
	if (urb->endpoint->descriptor.address == RTL8822BU_BULK_IN_ADDRESS) {
		if (persistent_rx_urb == NULL)
			persistent_rx_urb = urb;
		assert(persistent_rx_urb == urb);
		if (__atomic_exchange_n(&rx_submit_block_once, 0U,
		    __ATOMIC_ACQ_REL) != 0U) {
			__atomic_store_n(&rx_submit_block_entered, 1U,
			    __ATOMIC_RELEASE);
				while (__atomic_load_n(&rx_submit_block_release,
				    __ATOMIC_ACQUIRE) == 0U)
					sched_yield();
			}
			/* Without the device-side RX release barrier this completion would be
			 * delivered through the newly armed URB and lose its old CAM generation. */
			if (device_old_rx_payload_pending) {
				drv_usb_urb_callback_t callback = urb->callback;
				void *argument = urb->callback_argument;

				device_old_rx_payload_pending = 0U;
				old_rx_payload_deliveries++;
				((uint8_t *)urb->buffer)[0] = 0xffU;
				__atomic_store_n(&urb->actual, 1U, __ATOMIC_RELAXED);
				__atomic_store_n(&urb->status, DRV_USB_URB_COMPLETE,
				    __ATOMIC_RELEASE);
				if (callback != NULL)
					callback(urb, argument);
			}
	} else {
		storage_progress++;
	}
	return 0;
}

int
drv_usb_urb_cancel(struct drv_usb_urb *urb)
{
	drv_usb_urb_callback_t callback;
	void *argument;
	enum drv_usb_urb_status expected = DRV_USB_URB_PENDING;

	if (urb == NULL || !__atomic_compare_exchange_n(&urb->status, &expected,
	    DRV_USB_URB_CANCELLED, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		return EINVAL;
	urb_cancel_calls++;
	callback = urb->callback;
	argument = urb->callback_argument;
	if (callback != NULL)
		callback(urb, argument);
	if (fixture_thread_role == FIXTURE_ROLE_START)
		__atomic_store_n(&rx_start_post_access_complete, 1U,
		    __ATOMIC_RELEASE);
	return 0;
}

int
drv_usb_urb_drain(struct drv_usb_urb *urb, unsigned timeout)
{
	assert(timeout == RTL8822BU_RX_DRAIN_TIMEOUT_MS);
	urb_drain_calls++;
	if (urb == NULL)
		return EINVAL;
	return __atomic_load_n(&urb->status,
	    __ATOMIC_ACQUIRE) == DRV_USB_URB_PENDING ? ETIMEDOUT : 0;
}

enum drv_usb_urb_status
drv_usb_urb_status(const struct drv_usb_urb *urb)
{
	if (fixture_thread_role == FIXTURE_ROLE_START &&
	    __atomic_exchange_n(&rx_status_block_once, 0U,
	    __ATOMIC_ACQ_REL) != 0U) {
		__atomic_store_n(&rx_status_block_entered, 1U, __ATOMIC_RELEASE);
		while (__atomic_load_n(&rx_status_block_release,
		    __ATOMIC_ACQUIRE) == 0U)
			sched_yield();
	}
	return urb == NULL ? DRV_USB_URB_IO_ERROR : __atomic_load_n(
	    &urb->status, __ATOMIC_ACQUIRE);
}

size_t
drv_usb_urb_actual_length(const struct drv_usb_urb *urb)
{
	return urb == NULL || __atomic_load_n(&urb->status,
	    __ATOMIC_ACQUIRE) == DRV_USB_URB_PENDING ? 0U :
	    __atomic_load_n(&urb->actual, __ATOMIC_ACQUIRE);
}

void *
drv_usb_urb_buffer(const struct drv_usb_urb *urb)
{
	return urb == NULL ? NULL : urb->buffer;
}

static void
fake_urb_complete(struct drv_usb_urb *urb, enum drv_usb_urb_status status,
	size_t actual)
{
	drv_usb_urb_callback_t callback;
	void *argument;

	assert(urb != NULL && __atomic_load_n(&urb->status,
	    __ATOMIC_ACQUIRE) == DRV_USB_URB_PENDING);
	assert(actual <= urb->length);
	__atomic_store_n(&urb->actual, actual, __ATOMIC_RELAXED);
	__atomic_store_n(&urb->status, status, __ATOMIC_RELEASE);
	callback = urb->callback;
	argument = urb->callback_argument;
	if (callback != NULL)
		callback(urb, argument);
}

int
drv_usb_endpoint_clear_halt(struct drv_usb_endpoint *endpoint)
{
	int error;

	if (transport_forced_absent) {
		transport_io_after_absence++;
		return ENODEV;
	}
	assert(endpoint != NULL);
	assert(endpoint->descriptor.address == RTL8822BU_BULK_IN_ADDRESS ||
	    endpoint->descriptor.address == RTL8822BU_BULK_OUT_HIGH_ADDRESS ||
	    endpoint->descriptor.address == RTL8822BU_BULK_OUT_NORMAL_ADDRESS ||
	    endpoint->descriptor.address == RTL8822BU_BULK_OUT_LOW_ADDRESS);
	clear_halt_calls++;
	error = clear_halt_error;
	clear_halt_error = 0;
	return error;
}

int
drv_usb_bulk(struct drv_usb_device *device, struct drv_usb_endpoint *endpoint,
	void *buffer, size_t length, unsigned timeout, size_t *actual)
{
	const uint8_t *bytes = buffer;
	uint16_t checksum = 0U;
	uint32_t qsel;
	unsigned word;
	unsigned call;
	int stalled;

	if (transport_forced_absent) {
		transport_io_after_absence++;
		return ENODEV;
	}
	call = ++bulk_calls;
	assert(device != NULL && endpoint != NULL && buffer != NULL);
	stalled = bulk_stall_remaining != 0U && call >= bulk_stall_at;
	if (stalled)
		bulk_stall_remaining--;
	qsel = (fake_le32(bytes + 4U) >> 8) & 0x1fU;
	if (qsel == 18U || qsel == 0U) {
		assert(endpoint->descriptor.address ==
		    (qsel == 18U ? RTL8822BU_BULK_OUT_HIGH_ADDRESS :
		    (route_data_to_normal_endpoint ?
		    RTL8822BU_BULK_OUT_NORMAL_ADDRESS :
		    RTL8822BU_BULK_OUT_LOW_ADDRESS)));
		assert(timeout != 0U);
		assert(length > RTL8822B_DATA_TX_DESCRIPTOR_SIZE);
		assert((fake_le32(bytes) & 0xffffU) ==
		    length - RTL8822B_DATA_TX_DESCRIPTOR_SIZE);
		assert(((fake_le32(bytes) >> 16) & 0xffU) ==
		    RTL8822B_DATA_TX_DESCRIPTOR_SIZE);
		for (word = 0U; word < 16U; word++)
			checksum ^= fake_le16(bytes + word * 2U);
		assert(checksum == 0U);
		if ((fake_le32(bytes + 8U) & (1U << 19)) != 0U) {
			last_tx_sequence = (uint8_t)(fake_le32(bytes + 24U) &
			    0xfcU);
			if (qsel == 18U) {
				/* The fixture BSS is channel 6.  Management traffic
				 * must use the 2.4 GHz 1 Mbps basic rate; EAPOL is a
				 * data frame and retains the separate data policy. */
				if ((fake_le16(bytes +
				    RTL8822B_DATA_TX_DESCRIPTOR_SIZE) & 0x000cU) == 0U)
					assert((fake_le32(bytes + 16U) & 0x7fU) == 0U);
				if ((fake_le16(bytes +
				    RTL8822B_DATA_TX_DESCRIPTOR_SIZE) & 0x00fcU) ==
				    0x0040U)
					scan_probe_descriptors_checked++;
				station_descriptors_checked++;
			} else {
				data_descriptors_checked++;
			}
		} else {
			const uint8_t *frame = bytes +
			    RTL8822B_MANAGEMENT_TX_DESCRIPTOR_SIZE;

			assert(qsel == 18U);
			management_descriptors_checked++;
			if ((frame[0] & 0xfcU) == 0xc0U) {
				static const uint8_t local[6] = {
					0x02U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U
				};

				assert(length == RTL8822B_MANAGEMENT_TX_DESCRIPTOR_SIZE +
				    26U);
				assert(memcmp(frame + 4U, frame + 16U, 6U) == 0);
				assert(memcmp(frame + 10U, local, sizeof(local)) == 0);
				assert(fake_le16(frame + 24U) == 3U);
				deauthentication_descriptors_checked++;
			}
		}
		if (actual != NULL)
			*actual = call == bulk_short_at ? length - 1U : length;
		if (stalled)
			return EPIPE;
		return call == bulk_fail_at ? ETIMEDOUT : 0;
	}
	assert(endpoint->descriptor.address ==
	    RTL8822BU_BULK_OUT_HIGH_ADDRESS);
	assert(timeout == RTL8822BU_FIRMWARE_TRANSFER_TIMEOUT_MS);
	assert(length > RTL8822B_FIRMWARE_TX_DESCRIPTOR_SIZE);
	assert((fake_le32(bytes) & 0xffffU) ==
	    length - RTL8822B_FIRMWARE_TX_DESCRIPTOR_SIZE);
	assert(((fake_le32(bytes) >> 16) & 0xffU) ==
	    RTL8822B_FIRMWARE_TX_DESCRIPTOR_SIZE);
	assert((fake_le32(bytes) & (1U << 26)) != 0U);
	assert(((fake_le32(bytes + 4U) >> 8) & 0x1fU) == 16U);
	for (word = 0U; word < 16U; word++)
		checksum ^= fake_le16(bytes + word * 2U);
	assert(checksum == 0U);
	firmware_descriptors_checked++;
	if (actual != NULL)
		*actual = call == bulk_short_at ? length - 1U : length;
	if (call == bulk_fail_at)
		return ETIMEDOUT;
	if (stalled)
		return EPIPE;
	if (call != bulk_short_at && !beacon_completion_suppressed) {
		uint16_t page = fake_le16(fake_registers +
		    RTL8822BU_REG_FIFOPAGE_CTRL_2);

		fake_store16(RTL8822BU_REG_FIFOPAGE_CTRL_2,
		    page | RTL8822BU_BEACON_VALID);
	}
	return 0;
}

int
drv_usb_driver_register(struct drv_usb_driver *driver)
{
	return driver == &rtl8822bu_driver ? 0 : EINVAL;
}

struct net_device *
net_device_alloc(void)
{
	memset(&fake_net_device, 0, sizeof(fake_net_device));
	return &fake_net_device;
}

int
net_device_create(struct net_device *device)
{
	struct rtl8822bu_adapter *adapter = device->driver_data;
	unsigned before;

	assert(device == &fake_net_device);
	assert(adapter != NULL && __atomic_load_n(
	    &adapter->lifecycle_lock.locked, __ATOMIC_RELAXED));
	if (net_create_error != 0)
		return net_create_error;
	if (net_created)
		return EEXIST;
	net_created = 1U;
	if (open_during_create) {
		before = control_calls;
		open_during_create_error = device->ops->open(device);
		assert(control_calls == before);
	}
	return 0;
}

int
net_device_gone(struct net_device *device)
{
	struct rtl8822bu_adapter *adapter = device->driver_data;

	assert(device == &fake_net_device);
	assert(adapter != NULL && !__atomic_load_n(
	    &adapter->lifecycle_lock.locked, __ATOMIC_RELAXED));
	net_gone_calls++;
	if (net_gone_error != 0)
		return net_gone_error;
	net_gone = 1U;
	net_gone_sequence = ++lifecycle_sequence;
	while (__atomic_load_n(&net_callbacks_active,
	    __ATOMIC_ACQUIRE) != 0U) {
		__atomic_store_n(&net_join_waiting, 1U, __ATOMIC_RELEASE);
		sched_yield();
	}
	if (open_count_during_gone != 0U) {
		device->ops->close(device);
		open_count_during_gone--;
		close_calls_during_gone++;
	}
	if (callback_during_gone)
		callback_during_gone_error = device->ops->ioctl(device, 0U, NULL);
	return 0;
}

void
net_device_destroy(struct net_device *device)
{
	void (*release)(void *);
	void *driver_data;

	assert(device == &fake_net_device);
	release = device->ops->release;
	driver_data = device->driver_data;
	device->driver_data = NULL;
	if (defer_net_release) {
		assert(deferred_release == NULL && deferred_driver_data == NULL);
		deferred_release = release;
		deferred_driver_data = driver_data;
		return;
	}
	if (release != NULL)
		release(driver_data);
}

static void
fake_net_release_deferred(void)
{
	void (*release)(void *) = deferred_release;
	void *driver_data = deferred_driver_data;

	deferred_release = NULL;
	deferred_driver_data = NULL;
	if (release != NULL)
		release(driver_data);
}

int
net_device_set_carrier(struct net_device *device, int carrier)
{
	assert(device == &fake_net_device);
	if (__atomic_exchange_n(&carrier_block_once, 0U,
	    __ATOMIC_ACQ_REL) != 0U) {
		__atomic_store_n(&carrier_block_entered, 1U, __ATOMIC_RELEASE);
		while (__atomic_load_n(&carrier_block_release,
		    __ATOMIC_ACQUIRE) == 0U)
			sched_yield();
	}
	device->carrier = carrier != 0;
	return 0;
}

void
net_device_schedule_poll(struct net_device *device)
{
	assert(device == &fake_net_device);
	poll_schedule_calls++;
}

void
packet_buf_free(struct packet_buf *packet)
{
	free(packet);
}

int
wlan_station_attach(struct net_device *device,
	const struct wlan_radio_ops *ops, void *radio_context,
	const struct wlan_scan_profile *profile, struct wlan_station **result)
{
	assert(device == &fake_net_device);
	assert(ops == &rtl8822bu_radio_ops);
	assert(radio_context != NULL);
	assert(__atomic_load_n(&((struct rtl8822bu_adapter *)radio_context)->
	    lifecycle_lock.locked, __ATOMIC_RELAXED));
	assert(profile != NULL && profile->channel_count == 15U);
	for (unsigned index = 0U; index < profile->channel_count; index++) {
		uint32_t channel = index < 11U ? index + 1U :
		    36U + (index - 11U) * 4U;
		uint32_t frequency = channel <= 11U ? 2407U + channel * 5U :
		    5000U + channel * 5U;

		assert(profile->channels[index].channel == channel);
		assert(profile->channels[index].center_frequency_mhz == frequency);
		assert(profile->channels[index].flags ==
		    WLAN_SCAN_CHANNEL_ACTIVE_ALLOWED);
	}
	if (station_attach_error != 0)
		return station_attach_error;
	fake_station.marker = 0x8822U;
	*result = &fake_station;
	station_attached = 1U;
	return 0;
}

int
wlan_station_open(struct wlan_station *station)
{
	assert(station == &fake_station);
	station_open_calls++;
	return station_open_error;
}

int
wlan_station_close(struct wlan_station *station)
{
	struct rtl8822bu_adapter *adapter;
	uint64_t deadline;
	int error;

	assert(station == &fake_station);
	station_close_calls++;
	if (station_close_busy_count != 0U) {
		station_close_busy_count--;
		return EBUSY;
	}
	adapter = station_close_cleanup_adapter;
	if (adapter != NULL) {
		assert(adapter->ready && adapter->detaching);
		deadline = clock_ticks() + 100U;
		error = rtl8822bu_key_delete(adapter,
		    station_close_cleanup_generation, WLAN_RADIO_KEY_GROUP,
		    station_close_cleanup_group_index,
		    station_close_cleanup_group_generation, deadline);
		if (error == 0)
			error = rtl8822bu_key_delete(adapter,
			    station_close_cleanup_generation,
			    WLAN_RADIO_KEY_PAIRWISE, 0U,
			    station_close_cleanup_pairwise_generation, deadline);
		if (error == 0)
			error = rtl8822bu_association_clear(adapter,
			    station_close_cleanup_generation, deadline);
		if (error == 0)
			error = rtl8822bu_disconnect(adapter,
			    station_close_cleanup_generation);
		if (error == 0)
			station_close_cleanup_adapter = NULL;
		return error;
	}
	return station_close_error;
}

int
wlan_station_report_link_loss(struct wlan_station *station,
	uint64_t generation, int error)
{
	struct rtl8822bu_adapter *adapter;
	uint64_t deadline;
	int cleanup_error;

	assert(station == &fake_station);
	assert(generation != 0U && error > 0);
	link_loss_calls++;
	link_loss_generation = generation;
	link_loss_reason = error;
	if (link_loss_error != 0)
		return link_loss_error;
	adapter = link_loss_cleanup_adapter;
	if (adapter == NULL)
		return 0;
	deadline = clock_ticks() + 100U;
	cleanup_error = rtl8822bu_key_delete(adapter, generation,
	    WLAN_RADIO_KEY_GROUP, link_loss_cleanup_group_index,
	    link_loss_cleanup_group_generation, deadline);
	if (cleanup_error == 0)
		cleanup_error = rtl8822bu_key_delete(adapter, generation,
		    WLAN_RADIO_KEY_PAIRWISE, 0U,
		    link_loss_cleanup_pairwise_generation, deadline);
	if (cleanup_error == 0)
		cleanup_error = rtl8822bu_association_clear(adapter, generation,
		    deadline);
	if (cleanup_error == 0)
		cleanup_error = rtl8822bu_disconnect(adapter, generation);
	if (cleanup_error == 0)
		link_loss_cleanup_adapter = NULL;
	return cleanup_error;
}

int
wlan_station_detach(struct wlan_station *station)
{
	assert(station == &fake_station);
	station_detach_calls++;
	station_detached = 1U;
	station_detach_sequence = ++lifecycle_sequence;
	return station_detach_error;
}

int
wlan_station_ioctl(struct net_device *device, unsigned long request,
	void *argument)
{
	assert(device == &fake_net_device);
	(void)request;
	(void)argument;
	if (__atomic_exchange_n(&ioctl_block_once, 0U,
	    __ATOMIC_ACQ_REL) != 0U) {
		__atomic_store_n(&ioctl_block_entered, 1U, __ATOMIC_RELEASE);
		while (__atomic_load_n(&ioctl_block_release,
		    __ATOMIC_ACQUIRE) == 0U)
			sched_yield();
	}
	return 77;
}

int
wlan_station_report_scan_frame(struct wlan_station *station,
	uint64_t generation, const uint8_t *frame, size_t length,
	int32_t rssi_dbm, uint8_t channel_hint)
{
	assert(station == &fake_station);
	assert(frame != NULL && length >= 24U);
	assert((frame[0] & 0xf0U) == 0x80U || (frame[0] & 0xf0U) == 0x50U);
	if (__atomic_exchange_n(&report_block_once, 0U,
	    __ATOMIC_ACQ_REL) != 0U) {
		__atomic_store_n(&report_block_entered, 1U, __ATOMIC_RELEASE);
		while (__atomic_load_n(&report_block_release,
		    __ATOMIC_ACQUIRE) == 0U)
			sched_yield();
	}
	scan_report_calls++;
	scan_report_generation = generation;
	scan_report_channel = channel_hint;
	scan_report_rssi = rssi_dbm;
	return 0;
}

int
wlan_station_report_frame(struct wlan_station *station,
	const struct wlan_radio_rx_frame *report)
{
	assert(station == &fake_station && report != NULL);
	assert(report->frame != NULL && report->length >= 2U);
	frame_report_calls++;
	last_frame_report = *report;
	return 0;
}

int
wlan_station_report_tx_complete(struct wlan_station *station,
	uint64_t generation, uint64_t cookie, int acknowledged, int error)
{
	assert(station == &fake_station);
	tx_report_calls++;
	tx_report_generation = generation;
	tx_report_cookie = cookie;
	tx_report_acknowledged = acknowledged;
	tx_report_error = error;
	return tx_report_return_error;
}

int
wlan_station_transmit(struct wlan_station *station, struct packet_buf *packet)
{
	assert(station == &fake_station && packet != NULL);
	station_transmit_calls++;
	if (__atomic_exchange_n(&station_transmit_block_once, 0U,
	    __ATOMIC_ACQ_REL) != 0U) {
		__atomic_store_n(&station_transmit_block_entered, 1U,
		    __ATOMIC_RELEASE);
		while (__atomic_load_n(&station_transmit_block_release,
		    __ATOMIC_ACQUIRE) == 0U)
			sched_yield();
	}
	packet_buf_free(packet);
	return station_transmit_error;
}

int
wlan_station_report_scan_channel_ready(struct wlan_station *station,
	uint64_t generation, uint32_t step_index)
{
	assert(station == &fake_station);
	scan_ready_calls++;
	scan_ready_generation = generation;
	scan_ready_step = step_index;
	return scan_ready_error;
}

static void
make_exact_interface(struct drv_usb_device *device,
	struct drv_usb_interface *interface, struct drv_usb_endpoint endpoints[5])
{
	static const uint8_t addresses[5] = { 0x84, 0x05, 0x06, 0x87, 0x08 };
	unsigned index;

	memset(device, 0, sizeof(*device));
	memset(interface, 0, sizeof(*interface));
	memset(endpoints, 0, sizeof(*endpoints) * 5U);
	device->descriptor.usb_release = RTL8822BU_USB_RELEASE;
	device->descriptor.vendor = RTL8822BU_VENDOR_ID;
	device->descriptor.product = RTL8822BU_PRODUCT_ID;
	device->descriptor.device_release = RTL8822BU_DEVICE_RELEASE;
	device->descriptor.endpoint0_max_packet_size = 64U;
	device->descriptor.configuration_count = 1U;
	device->speed = DRV_USB_SPEED_HIGH;
	device->hcd_capabilities = DRV_USB_HCD_CAP_CONCURRENT_URBS;
	interface->device = device;
	interface->descriptor.interface_number = 0U;
	interface->descriptor.alternate_setting = 0U;
	interface->descriptor.endpoint_count = 5U;
	interface->descriptor.interface_class = RTL8822BU_INTERFACE_CLASS;
	interface->descriptor.interface_subclass = RTL8822BU_INTERFACE_SUBCLASS;
	interface->descriptor.interface_protocol = RTL8822BU_INTERFACE_PROTOCOL;
	interface->alternate.endpoint_count = 5U;
	for (index = 0U; index < 5U; index++) {
		int interrupt = addresses[index] == 0x87U;

		endpoints[index].descriptor.address = addresses[index];
		endpoints[index].descriptor.attributes = interrupt ? 3U : 2U;
		endpoints[index].descriptor.maximum_packet_size = interrupt ?
		    64U : 512U;
		endpoints[index].descriptor.interval = interrupt ? 3U : 0U;
		endpoints[index].type = interrupt ? DRV_USB_TRANSFER_INTERRUPT :
		    DRV_USB_TRANSFER_BULK;
		interface->alternate.endpoints[index] = &endpoints[index];
	}
}

static void
fixture_put_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

static void
fixture_put_le32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

static void
make_test_firmware(uint8_t *firmware)
{
	memset(firmware, 0x5a, RTL8822B_FIRMWARE_SIZE);
	memset(firmware, 0, RTL8822B_FIRMWARE_HEADER_SIZE);
	fixture_put_le16(firmware, 0x8822U);
	fixture_put_le16(firmware + 4U, 30U);
	firmware[6] = 20U;
	firmware[24] = 0x08U;
	fixture_put_le16(firmware + 28U, 14U);
	fixture_put_le32(firmware + 32U, 0x80200000U);
	fixture_put_le32(firmware + 36U, RTL8822B_FIRMWARE_DMEM_SIZE);
	fixture_put_le32(firmware + 48U, RTL8822B_FIRMWARE_IMEM_SIZE);
	fixture_put_le32(firmware + 60U, 0x80000000U);
}

static uint8_t test_firmware_digest[32];

static int
test_firmware_walk(const struct rtl8822b_firmware_view *view,
	rtl8822b_firmware_chunk_fn callback, void *context)
{
	return rtl8822b_test_firmware_walk(view, test_firmware_digest,
	    callback, context);
}

static int
fixture_firmware_load(struct rtl8822b_firmware_blob *firmware)
{
	uint8_t *bytes;
	int error;

	firmware_load_calls++;
	if (firmware == NULL)
		return EINVAL;
	if (firmware_load_error != 0)
		return firmware_load_error;
	if (firmware->bytes != NULL || firmware->size != 0U)
		return EBUSY;
	bytes = hal_malloc(RTL8822B_FIRMWARE_SIZE);
	if (bytes == NULL)
		return ENOMEM;
	make_test_firmware(bytes);
	error = rtl8822b_sha256(bytes, RTL8822B_FIRMWARE_SIZE,
	    test_firmware_digest);
	if (error == 0)
		error = rtl8822b_test_firmware_validate(bytes,
		    RTL8822B_FIRMWARE_SIZE, test_firmware_digest, &firmware->view);
	if (error != 0) {
		memset(bytes, 0, RTL8822B_FIRMWARE_SIZE);
		hal_free(bytes);
		memset(firmware, 0, sizeof(*firmware));
		return error;
	}
	firmware->bytes = bytes;
	firmware->size = RTL8822B_FIRMWARE_SIZE;
	return 0;
}

static void
fixture_firmware_release(struct rtl8822b_firmware_blob *firmware)
{
	if (firmware == NULL)
		return;
	if (firmware->bytes != NULL) {
		memset(firmware->bytes, 0, firmware->size);
		hal_free(firmware->bytes);
		firmware_release_calls++;
	}
	memset(firmware, 0, sizeof(*firmware));
}

struct firmware_register_snapshot {
	uint8_t txdma_map_high;
	uint8_t cr;
	uint8_t cr_high;
	uint8_t beacon;
	uint8_t sys_function_high;
	uint8_t reserved_high;
	uint8_t system_clock_high;
	uint8_t cpu_dmem_high;
	uint16_t fifo_info;
	uint16_t fifo_control;
	uint16_t mcufw;
	uint32_t h2c;
	uint32_t rqpn;
};

static void
firmware_snapshot(struct firmware_register_snapshot *snapshot)
{
	snapshot->txdma_map_high =
	    fake_registers[RTL8822BU_REG_TXDMA_PQ_MAP + 1U];
	snapshot->cr = fake_registers[RTL8822BU_REG_CR];
	snapshot->cr_high = fake_registers[RTL8822BU_REG_CR + 1U];
	snapshot->beacon = fake_registers[RTL8822BU_REG_BCN_CTRL];
	snapshot->sys_function_high =
	    fake_registers[RTL8822BU_REG_SYS_FUNC_EN + 1U];
	snapshot->reserved_high = fake_registers[RTL8822BU_REG_RSV_CTRL + 1U];
	snapshot->system_clock_high = fake_registers[RTL8822BU_REG_SYS_CLKR + 1U];
	snapshot->cpu_dmem_high =
	    fake_registers[RTL8822BU_REG_CPU_DMEM_CON + 2U];
	snapshot->fifo_info = fake_le16(fake_registers +
	    RTL8822BU_REG_FIFOPAGE_INFO_1);
	snapshot->fifo_control = fake_le16(fake_registers +
	    RTL8822BU_REG_FIFOPAGE_CTRL_2);
	snapshot->mcufw = fake_le16(fake_registers + RTL8822BU_REG_MCUFW_CTRL);
	snapshot->h2c = fake_le32(fake_registers + RTL8822BU_REG_H2CQ_CSR);
	snapshot->rqpn = fake_le32(fake_registers + RTL8822BU_REG_RQPN_CTRL_2);
}

static void
assert_firmware_transient_restored(
	const struct firmware_register_snapshot *snapshot)
{
	assert(fake_registers[RTL8822BU_REG_TXDMA_PQ_MAP + 1U] ==
	    snapshot->txdma_map_high);
	assert(fake_registers[RTL8822BU_REG_CR] == snapshot->cr);
	assert(fake_registers[RTL8822BU_REG_CR + 1U] == snapshot->cr_high);
	assert(fake_registers[RTL8822BU_REG_BCN_CTRL] == snapshot->beacon);
	assert(fake_le16(fake_registers + RTL8822BU_REG_FIFOPAGE_INFO_1) ==
	    snapshot->fifo_info);
	assert(fake_le16(fake_registers + RTL8822BU_REG_FIFOPAGE_CTRL_2) ==
	    snapshot->fifo_control);
	assert(fake_le32(fake_registers + RTL8822BU_REG_H2CQ_CSR) ==
	    snapshot->h2c);
	assert(fake_le32(fake_registers + RTL8822BU_REG_RQPN_CTRL_2) ==
	    snapshot->rqpn);
}

static void
assert_firmware_failure_restored(
	const struct firmware_register_snapshot *snapshot)
{
	assert_firmware_transient_restored(snapshot);
	assert(fake_registers[RTL8822BU_REG_SYS_FUNC_EN + 1U] ==
	    snapshot->sys_function_high);
	assert(fake_registers[RTL8822BU_REG_RSV_CTRL + 1U] ==
	    snapshot->reserved_high);
	assert(fake_registers[RTL8822BU_REG_SYS_CLKR + 1U] ==
	    snapshot->system_clock_high);
	assert(fake_registers[RTL8822BU_REG_CPU_DMEM_CON + 2U] ==
	    snapshot->cpu_dmem_high);
	assert(fake_le16(fake_registers + RTL8822BU_REG_MCUFW_CTRL) ==
	    snapshot->mcufw);
}

static void
test_firmware_transport(void)
{
	struct firmware_register_snapshot snapshot;
	struct drv_usb_device device;
	struct drv_usb_interface interface;
	struct drv_usb_endpoint endpoints[5];
	struct rtl8822bu_adapter adapter;
	struct rtl8822b_firmware_view view;
	uint8_t *firmware = malloc(RTL8822B_FIRMWARE_SIZE);
	int error;

	assert(firmware != NULL);
	make_test_firmware(firmware);
	assert(rtl8822b_sha256(firmware, RTL8822B_FIRMWARE_SIZE,
	    test_firmware_digest) == 0);
	assert(rtl8822b_test_firmware_validate(firmware,
	    RTL8822B_FIRMWARE_SIZE, test_firmware_digest, &view) == 0);
	make_exact_interface(&device, &interface, endpoints);
	memset(&adapter, 0, sizeof(adapter));
	adapter.usb_device = &device;
	adapter.bulk_out_high = &endpoints[1];

	fake_transport_reset();
	firmware_snapshot(&snapshot);
	error = rtl8822bu_firmware_download_model(&adapter, &view,
	    test_firmware_walk);
	assert(error == 0);
	assert(firmware_chunk_count == 40U);
	assert(bulk_calls == 40U && firmware_descriptors_checked == 40U);
	assert((fake_le16(fake_registers + RTL8822BU_REG_MCUFW_CTRL) &
	    RTL8822BU_MCUFW_READY_MASK) == RTL8822BU_MCUFW_READY);
	assert_firmware_transient_restored(&snapshot);
	assert(allocations == 0U);

	fake_transport_reset();
	firmware_snapshot(&snapshot);
	bulk_short_at = 2U;
	assert(rtl8822bu_firmware_download_model(&adapter, &view,
	    test_firmware_walk) == EIO);
	assert(bulk_calls == 2U && firmware_chunk_count == 1U);
	assert_firmware_failure_restored(&snapshot);
	assert(allocations == 0U);

	fake_transport_reset();
	firmware_snapshot(&snapshot);
	bulk_fail_at = 1U;
	assert(rtl8822bu_firmware_download_model(&adapter, &view,
	    test_firmware_walk) == ETIMEDOUT);
	assert(bulk_calls == 1U && firmware_chunk_count == 0U);
	assert_firmware_failure_restored(&snapshot);
	assert(allocations == 0U);

	fake_transport_reset();
	firmware_snapshot(&snapshot);
	beacon_completion_suppressed = 1U;
	assert(rtl8822bu_firmware_download_model(&adapter, &view,
	    test_firmware_walk) == ETIMEDOUT);
	/* The W1C clear cannot satisfy its own completion poll. */
	assert(bulk_calls == 1U && firmware_chunk_count == 0U);
	assert_firmware_failure_restored(&snapshot);
	assert(allocations == 0U);

	fake_transport_reset();
	firmware_snapshot(&snapshot);
	/* Thirteen reads save state.  Fail after several prepare writes so every
	 * already-mutated register must still be restored in reverse order. */
	control_fail_at = 20U;
	assert(rtl8822bu_firmware_download_model(&adapter, &view,
	    test_firmware_walk) == ETIMEDOUT);
	assert(bulk_calls == 0U && firmware_chunk_count == 0U);
	assert_firmware_failure_restored(&snapshot);
	assert(allocations == 0U);

	fake_transport_reset();
	firmware_snapshot(&snapshot);
	firmware_checksum_fail = 1U;
	assert(rtl8822bu_firmware_download_model(&adapter, &view,
	    test_firmware_walk) == EILSEQ);
	assert(firmware_chunk_count == 3U);
	assert_firmware_failure_restored(&snapshot);
	assert(allocations == 0U);

	fake_transport_reset();
	firmware_snapshot(&snapshot);
	firmware_ready_suppressed = 1U;
	assert(rtl8822bu_firmware_download_model(&adapter, &view,
	    test_firmware_walk) == ETIMEDOUT);
	assert(firmware_chunk_count == 40U);
	assert_firmware_failure_restored(&snapshot);
	assert(allocations == 0U);

	fake_transport_reset();
	firmware_snapshot(&snapshot);
	firmware_ready_forbidden_bit = 1U;
	assert(rtl8822bu_firmware_download_model(&adapter, &view,
	    test_firmware_walk) == ETIMEDOUT);
	/* FW_READY ignores only CPU_CLK_SEL (bits 12..13).  Any other unexpected
	 * latched bit, here bit 1, rejects readiness. */
	assert(firmware_chunk_count == 40U);
	assert_firmware_failure_restored(&snapshot);
	assert(allocations == 0U);

	fake_transport_reset();
	firmware_snapshot(&snapshot);
	allocation_calls = 0U;
	allocation_fail_at = 1U;
	assert(rtl8822bu_firmware_download_model(&adapter, &view,
	    test_firmware_walk) == ENOMEM);
	assert(control_calls == 0U && bulk_calls == 0U && allocations == 0U);
	assert_firmware_failure_restored(&snapshot);
	memset(firmware, 0, RTL8822B_FIRMWARE_SIZE);
	free(firmware);
}

static size_t
make_beacon_aggregate(uint8_t *buffer, uint8_t subtype)
{
	uint8_t *phy = buffer + RTL8822B_RX_DESCRIPTOR_SIZE;
	uint8_t *frame = phy + RTL8822B_RX_PHY_INFO_SIZE;
	static const uint8_t source[6] = { 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee };
	size_t frame_length = 42U;
	size_t packet_length = frame_length + RTL8822B_RX_FCS_SIZE;
	size_t aggregate_length = RTL8822B_RX_DESCRIPTOR_SIZE +
	    RTL8822B_RX_PHY_INFO_SIZE + packet_length;
	uint32_t word0;

	aggregate_length = (aggregate_length + 7U) & ~(size_t)7U;
	memset(buffer, 0, aggregate_length);
	word0 = (uint32_t)packet_length | (4U << 16) | 0x04000000U;
	fixture_put_le32(buffer, word0);
	fixture_put_le32(buffer + 12U, 1U);
	phy[0] = 1U;
	phy[1] = 50U;
	phy[2] = 45U;
	frame[0] = subtype;
	frame[1] = 0U;
	memset(frame + 4U, 0xff, 6U);
	memcpy(frame + 10U, source, 6U);
	memcpy(frame + 16U, source, 6U);
	frame[36U] = 0U;
	frame[37U] = 4U;
	memcpy(frame + 38U, "q056", 4U);
	return aggregate_length;
}

static struct rtl8822bu_adapter *retiring_poll_adapter;
static struct rtl8822bu_adapter *serialized_connect_adapter;

static void
release_serialized_radio_operation(void)
{
	struct rtl8822bu_adapter *adapter = serialized_connect_adapter;

	assert(adapter != NULL);
	assert(adapter->radio_operations_active == 1U);
	adapter->radio_operations_active = 0U;
	serialized_connect_adapter = NULL;
	yield_hook = NULL;
}

static void
retire_fake_poll(void)
{
	struct rtl8822bu_adapter *adapter = retiring_poll_adapter;

	assert(adapter != NULL);
	/* Forward admission is closed by detaching while checked station cleanup
	 * keeps the driver's inverse callbacks available until retirement joins. */
	assert(adapter->ready && adapter->detaching && adapter->closing &&
	    adapter->stopping);
	assert(adapter->polls_active == 1U);
	adapter->polls_active = 0U;
	retiring_poll_adapter = NULL;
	yield_hook = NULL;
}

static void
test_exact_match(void)
{
	struct drv_usb_device device;
	struct drv_usb_interface interface;
	struct drv_usb_endpoint endpoints[5];
	struct rtl8822bu_binding binding;

	make_exact_interface(&device, &interface, endpoints);
	assert(rtl8822bu_binding_parse(&interface, &binding));
	assert(rtl8822bu_match(&interface, &rtl8822bu_ids[0]) == 200);
	device.descriptor.product++;
	assert(!rtl8822bu_binding_parse(&interface, &binding));
	device.descriptor.product--;
	device.descriptor.device_release++;
	assert(!rtl8822bu_binding_parse(&interface, &binding));
	device.descriptor.device_release--;
	device.descriptor.usb_release--;
	assert(!rtl8822bu_binding_parse(&interface, &binding));
	device.descriptor.usb_release++;
	device.speed = DRV_USB_SPEED_SUPER;
	assert(!rtl8822bu_binding_parse(&interface, &binding));
	device.speed = DRV_USB_SPEED_HIGH;
	endpoints[0].descriptor.maximum_packet_size = 64U;
	assert(!rtl8822bu_binding_parse(&interface, &binding));
	endpoints[0].descriptor.maximum_packet_size = 512U;
	endpoints[3].descriptor.interval = 4U;
	assert(!rtl8822bu_binding_parse(&interface, &binding));
	endpoints[3].descriptor.interval = 3U;
	device.hcd_capabilities = 0U;
	assert(!rtl8822bu_binding_parse(&interface, &binding));
	device.hcd_capabilities = DRV_USB_HCD_CAP_CONCURRENT_URBS;
	endpoints[4].descriptor.address = 0x09U;
	assert(!rtl8822bu_binding_parse(&interface, &binding));
	endpoints[4].descriptor.address = RTL8822BU_BULK_OUT_LOW_ADDRESS;
	/* Interrupt-IN is advertised by the retail adapter but unused: all C2H
	 * and CCX reports arrive on bulk-IN.  Four bulk endpoints therefore form
	 * the complete producer/fault contract. */
	interface.descriptor.endpoint_count = 4U;
	interface.alternate.endpoint_count = 4U;
	interface.alternate.endpoints[3] = &endpoints[4];
	assert(rtl8822bu_binding_parse(&interface, &binding));
}

static void
test_register_transport(void)
{
	struct drv_usb_device device;
	struct rtl8822bu_adapter adapter;
	uint16_t value16 = 0U;
	uint32_t value32 = 0U;

	memset(&device, 0, sizeof(device));
	memset(&adapter, 0, sizeof(adapter));
	adapter.usb_device = &device;
	fake_transport_reset();
	assert(rtl8822bu_write16(&adapter, 0x1234U, 0xabcdU) == 0);
	assert(fake_registers[0x1234] == 0xcdU);
	assert(fake_registers[0x1235] == 0xabU);
	assert(rtl8822bu_read16(&adapter, 0x1234U, &value16) == 0);
	assert(value16 == 0xabcdU);
	assert(rtl8822bu_write32(&adapter, 0x2000U, 0x78563412U) == 0);
	assert(rtl8822bu_read32(&adapter, 0x2000U, &value32) == 0);
	assert(value32 == 0x78563412U);
	control_short_at = control_calls + 1U;
	value16 = 0x1111U;
	assert(rtl8822bu_read16(&adapter, 0x1234U, &value16) == EIO);
	assert(value16 == 0x1111U);
	control_fail_at = control_calls + 1U;
	assert(rtl8822bu_write8(&adapter, 0x1234U, 1U) == ETIMEDOUT);
}

static void
test_radio_delay_resolution(void)
{
	uint64_t before;

	fake_transport_reset();
	before = clock_ticks();
	assert(rtl8822bu_radio_delay_us(NULL, 1U, before + 10U) == 0);
	assert(rtl8822bu_radio_delay_us(NULL, 50U, before + 10U) == 0);
	/* Sub-tick table delays are never expanded into scheduler ticks. */
	assert(clock_ticks() == before);
	assert(rtl8822bu_radio_delay_us(NULL, 5000U, before + 10U) == 0);
	assert(clock_ticks() == before + 1U);
	before = clock_ticks();
	assert(rtl8822bu_radio_delay_us(NULL, 50000U, before + 10U) == 0);
	assert(clock_ticks() == before + 5U);
	before = clock_ticks();
	assert(rtl8822bu_radio_delay_us(NULL, 50000U, before + 5U) ==
	    ETIMEDOUT);
	assert(clock_ticks() == before);
}

static void
test_efuse_cleanup(void)
{
	struct drv_usb_device device;
	struct rtl8822bu_adapter adapter;
	uint8_t physical[RTL8822B_EFUSE_PHYSICAL_SIZE];
	unsigned index;

	memset(&device, 0, sizeof(device));
	memset(&adapter, 0, sizeof(adapter));
	adapter.usb_device = &device;
	fake_transport_reset();
	for (index = 0U; index < sizeof(fake_efuse); index++)
		fake_efuse[index] = (uint8_t)(index ^ (index >> 8));
	assert(rtl8822bu_efuse_physical_read(&adapter, physical) == 0);
	assert(memcmp(physical, fake_efuse, sizeof(physical)) == 0);
	assert(fake_le16(fake_registers + RTL8822BU_REG_SYS_FUNC_EN) ==
	    0x0440U);
	assert(fake_le16(fake_registers + RTL8822BU_REG_SYS_CLKR) == 0x4080U);
	assert(fake_le32(fake_registers + RTL8822BU_REG_LDO_EFUSE_CTRL) ==
	    0x80000355U);
	assert(fake_registers[RTL8822BU_REG_EFUSE_ACCESS] == 0U);
	assert(access_off_count == 1U);

	fake_transport_reset();
	control_fail_at = 8U;
	assert(rtl8822bu_efuse_physical_read(&adapter, physical) == ETIMEDOUT);
	assert(fake_registers[RTL8822BU_REG_EFUSE_ACCESS] == 0U);
	assert(access_off_count == 1U);
}

static void
test_attach_open_detach(void)
{
	struct drv_usb_device device;
	struct drv_usb_interface interface;
	struct drv_usb_endpoint endpoints[5];
	struct rtl8822bu_adapter *adapter;

	fake_transport_reset();
	net_created = 0U;
	net_gone = 0U;
	station_attached = 0U;
	station_detached = 0U;
	lifecycle_sequence = 0U;
	net_gone_sequence = 0U;
	station_detach_sequence = 0U;
	station_attach_error = 0;
	station_detach_error = 0;
	open_during_create = 1U;
	open_during_create_error = 0;
	callback_during_gone = 1U;
	callback_during_gone_error = 0;
	open_count_during_gone = 1U;
	close_calls_during_gone = 0U;
	make_exact_interface(&device, &interface, endpoints);
	assert(rtl8822bu_attach(&interface, &rtl8822bu_ids[0]) == 0);
	adapter = interface.driver_data;
	assert(adapter != NULL && adapter->ready);
	assert(adapter->board.tx_power_2g[0].cck_base[0] == 32U);
	assert(adapter->board.tx_power_2g[0].bw40_base[0] == 30U);
	assert(adapter->board.tx_power_2g[0].ofdm_diff == -2);
	assert(adapter->board.tx_power_2g[1].cck_base[0] == 28U);
	assert(adapter->board.tx_power_2g[1].bw40_base[0] == 26U);
	assert(adapter->board.tx_power_2g[1].ofdm_diff == 3);
	assert(adapter->board.tx_power_5g[0].bw40_base[0] == 24U);
	assert(adapter->board.tx_power_5g[0].bw40_base[1] == 25U);
	assert(adapter->board.tx_power_5g[0].ofdm_diff == -1);
	assert(adapter->board.tx_power_5g[1].bw40_base[0] == 20U);
	assert(adapter->board.tx_power_5g[1].bw40_base[1] == 21U);
	assert(adapter->board.tx_power_5g[1].ofdm_diff == 2);
	assert(net_created && station_attached);
	assert(open_during_create_error == ENODEV);
	assert(strcmp(fake_net_device.name, "wlan0") == 0);
	assert(fake_net_device.capabilities == NET_DEVICE_CAP_WLAN);
	assert(memcmp(fake_net_device.hwaddr,
	    (const uint8_t[]){ 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 }, 6U) == 0);
	assert(fake_net_device.carrier == 0U);
	control_calls = 0U;
	bulk_calls = 0U;
	assert(fake_net_device.ops->open(&fake_net_device) == 0);
	assert(control_calls != 0U && bulk_calls != 0U);
	assert(firmware_load_calls == 1U && firmware_release_calls == 1U);
	assert(station_open_calls == 1U);
	assert(adapter->firmware_running && adapter->radio_running &&
	    adapter->opened && adapter->rx_urb->status == DRV_USB_URB_PENDING);
	assert(fake_net_device.ops->ioctl(&fake_net_device, 0U, NULL) == 77);
	assert(rtl8822bu_detach(&interface, 0U) == 0);
	assert(net_gone && station_detached);
	assert(close_calls_during_gone == 1U);
	assert(callback_during_gone_error == ENODEV);
	assert(net_gone_sequence < station_detach_sequence);
	assert(interface.driver_data == NULL);
	assert(allocations == 0U);
	assert(drv_usb_rtl8822bu_driver_register() == 0);
	open_during_create = 0U;
	callback_during_gone = 0U;
}

static void
assert_open_failure_stopped(struct rtl8822bu_adapter *adapter,
	unsigned baseline_allocations)
{
	assert(adapter != NULL);
	assert(!adapter->opened && !adapter->firmware_running &&
	    !adapter->radio_running && !adapter->quarantined);
	assert(adapter->radio.state == RTL8822B_RADIO_OFF);
	assert(adapter->rx_urb->status != DRV_USB_URB_PENDING);
	assert(allocations == baseline_allocations);
}

static void
test_first_open_failure_unwind(void)
{
	struct drv_usb_device device;
	struct drv_usb_interface interface;
	struct drv_usb_endpoint endpoints[5];
	struct rtl8822bu_adapter *adapter;
	unsigned baseline;
	int expected;
	unsigned failure;

	for (failure = 0U; failure < 8U; failure++) {
		fake_transport_reset();
		make_exact_interface(&device, &interface, endpoints);
		assert(rtl8822bu_attach(&interface, &rtl8822bu_ids[0]) == 0);
		adapter = interface.driver_data;
		baseline = allocations;
		expected = EIO;
		if (failure == 0U) {
			control_fail_address = 0x004aU;
			expected = ETIMEDOUT;
		} else if (failure == 1U) {
			firmware_load_error = ENOENT;
			expected = ENOENT;
		} else if (failure == 2U) {
			bulk_fail_at = 1U;
			expected = ETIMEDOUT;
		} else if (failure == 3U) {
			/* TXAGC is programmed after firmware and the imported tables. */
			control_fail_address = RTL8822B_TXAGC_A;
			expected = ETIMEDOUT;
		} else if (failure == 4U) {
			urb_submit_error = EIO;
		} else if (failure == 5U) {
			station_open_error = EIO;
		} else if (failure == 6U) {
			bulk_short_at = 1U;
		} else {
			bulk_stall_at = 1U;
			bulk_stall_remaining = 2U;
			expected = EPIPE;
		}
		assert(fake_net_device.ops->open(&fake_net_device) == expected);
		assert(firmware_load_calls == (failure == 0U ? 0U : 1U));
		assert(firmware_release_calls == (failure <= 1U ? 0U : 1U));
		assert_open_failure_stopped(adapter, baseline);
		if (failure == 5U)
			assert(urb_cancel_calls == 1U);
		/* A checked unwind clears attempt-local endpoint/recovery state.  The
		 * same published interface can retry from a cold ownership graph. */
		firmware_load_error = 0;
		urb_submit_error = 0;
		station_open_error = 0;
		bulk_short_at = 0U;
		bulk_stall_remaining = 0U;
		assert(!adapter->recovery_pending &&
		    adapter->control_error_streak == 0U &&
		    adapter->tx_high_error_streak == 0U);
		assert(fake_net_device.ops->open(&fake_net_device) == 0);
		fake_net_device.ops->close(&fake_net_device);
		assert(rtl8822bu_detach(&interface, 0U) == 0);
		assert(interface.driver_data == NULL && allocations == 0U);
	}
}

static size_t
make_wildcard_probe(uint8_t frame[26])
{
	memset(frame, 0, 26U);
	frame[0] = 0x40U;
	memset(frame + 4U, 0xff, 6U);
	frame[10] = 0x02U;
	frame[11] = 0x11U;
	frame[12] = 0x22U;
	frame[13] = 0x33U;
	frame[14] = 0x44U;
	frame[15] = 0x55U;
	memset(frame + 16U, 0xff, 6U);
	/* Empty SSID information element. */
	frame[24] = 0U;
	frame[25] = 0U;
	return 26U;
}

static size_t
make_directed_probe(uint8_t frame[64], const uint8_t *ssid,
	uint8_t ssid_length)
{
	static const uint8_t rates[4] = { 0x82U, 0x84U, 0x8bU, 0x96U };
	size_t offset;

	assert(ssid != NULL && ssid_length != 0U && ssid_length <= 32U);
	memset(frame, 0, 64U);
	frame[0] = 0x40U;
	memset(frame + 4U, 0xff, 6U);
	memcpy(frame + 10U,
	    (const uint8_t[]){ 0x02U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U },
	    6U);
	memset(frame + 16U, 0xff, 6U);
	frame[24U] = 0U;
	frame[25U] = ssid_length;
	memcpy(frame + 26U, ssid, ssid_length);
	offset = 26U + ssid_length;
	frame[offset++] = 1U;
	frame[offset++] = sizeof(rates);
	memcpy(frame + offset, rates, sizeof(rates));
	return offset + sizeof(rates);
}

static void
make_connection_bss(struct wlan_bss_record *bss)
{
	memset(bss, 0, sizeof(*bss));
	memcpy(bss->ssid, "fixture", 7U);
	bss->ssid_length = 7U;
	memcpy(bss->bssid,
	    (const uint8_t[]){ 0x02U, 0xaaU, 0xbbU, 0xccU, 0xddU, 0xeeU },
	    6U);
	bss->channel = 6U;
	bss->center_frequency_mhz = 2437U;
	bss->security = WLAN_SECURITY_PRIVACY | WLAN_SECURITY_WPA2 |
	    WLAN_SECURITY_CCMP | WLAN_SECURITY_PSK;
}

static size_t
make_station_frame(uint8_t *frame, enum wlan_radio_frame_class class,
	const struct wlan_bss_record *bss, int encrypted, uint8_t key_index,
	uint64_t packet_number)
{
	size_t offset;
	uint16_t frame_control;

	memset(frame, 0, 96U);
	if (class == WLAN_RADIO_FRAME_MANAGEMENT) {
		frame[0] = 0xb0U;
		memcpy(frame + 4U, bss->bssid, 6U);
		memcpy(frame + 10U, fake_net_device.hwaddr, 6U);
		memcpy(frame + 16U, bss->bssid, 6U);
		return 30U;
	}
	frame_control = 0x0108U | (encrypted ? 0x4000U : 0U);
	frame[0] = (uint8_t)frame_control;
	frame[1] = (uint8_t)(frame_control >> 8);
	memcpy(frame + 4U, bss->bssid, 6U);
	memcpy(frame + 10U, fake_net_device.hwaddr, 6U);
	memcpy(frame + 16U,
	    (const uint8_t[]){ 0x02U, 1U, 2U, 3U, 4U, 5U }, 6U);
	offset = 24U;
	if (encrypted) {
		frame[offset++] = (uint8_t)packet_number;
		frame[offset++] = (uint8_t)(packet_number >> 8);
		frame[offset++] = 0U;
		frame[offset++] = (uint8_t)(0x20U | (key_index << 6));
		frame[offset++] = (uint8_t)(packet_number >> 16);
		frame[offset++] = (uint8_t)(packet_number >> 24);
		frame[offset++] = (uint8_t)(packet_number >> 32);
		frame[offset++] = (uint8_t)(packet_number >> 40);
	}
	memcpy(frame + offset,
	    (const uint8_t[]){ 0xaaU, 0xaaU, 0x03U, 0U, 0U, 0U,
	    class == WLAN_RADIO_FRAME_EAPOL ? 0x88U : 0x08U,
	    class == WLAN_RADIO_FRAME_EAPOL ? 0x8eU : 0x00U }, 8U);
	return offset + 8U;
}

static size_t
make_ap_frame(uint8_t *frame, const struct wlan_bss_record *bss, int group,
	uint8_t key_index, uint64_t packet_number, int eapol)
{
	size_t offset = 24U;
	uint16_t frame_control = 0x0208U | (packet_number != 0U ? 0x4000U : 0U);

	memset(frame, 0, 96U);
	frame[0] = (uint8_t)frame_control;
	frame[1] = (uint8_t)(frame_control >> 8);
	memcpy(frame + 4U, group ?
	    (const uint8_t[]){ 0x01U, 0U, 0x5eU, 0U, 0U, 1U } :
	    fake_net_device.hwaddr, 6U);
	memcpy(frame + 10U, bss->bssid, 6U);
	memcpy(frame + 16U,
	    (const uint8_t[]){ 0x02U, 6U, 7U, 8U, 9U, 10U }, 6U);
	if (packet_number != 0U) {
		frame[offset++] = (uint8_t)packet_number;
		frame[offset++] = (uint8_t)(packet_number >> 8);
		frame[offset++] = 0U;
		frame[offset++] = (uint8_t)(0x20U | (key_index << 6));
		frame[offset++] = (uint8_t)(packet_number >> 16);
		frame[offset++] = (uint8_t)(packet_number >> 24);
		frame[offset++] = (uint8_t)(packet_number >> 32);
		frame[offset++] = (uint8_t)(packet_number >> 40);
	}
	memcpy(frame + offset,
	    (const uint8_t[]){ 0xaaU, 0xaaU, 0x03U, 0U, 0U, 0U,
	    eapol ? 0x88U : 0x08U, eapol ? 0x8eU : 0x00U }, 8U);
	return offset + 8U;
}

static void
make_ccx_report(struct rtl8822b_rx_packet *packet, uint8_t payload[9],
	uint8_t sequence, uint8_t status)
{
	memset(payload, 0, 9U);
	payload[0] = RTL8822BU_C2H_CCX_TX_REPORT_ID;
	payload[8] = sequence;
	payload[2] = status;
	memset(packet, 0, sizeof(*packet));
	packet->kind = RTL8822B_RX_C2H;
	packet->payload = payload;
	packet->payload_length = 9U;
	packet->c2h_id = RTL8822BU_C2H_CCX_TX_REPORT_ID;
}

static void
make_ccx_extended_report(struct rtl8822b_rx_packet *packet,
	uint8_t payload[12], uint8_t sequence, uint8_t status)
{
	memset(payload, 0, 12U);
	payload[0] = RTL8822BU_C2H_EXTENDED_ID;
	payload[2] = RTL8822BU_C2H_EXTENDED_CCX_REPORT_ID;
	payload[10] = sequence;
	payload[11] = status;
	memset(packet, 0, sizeof(*packet));
	packet->kind = RTL8822B_RX_C2H;
	packet->payload = payload;
	packet->payload_length = 12U;
	packet->c2h_id = RTL8822BU_C2H_EXTENDED_ID;
}

static void
test_secure_station_hardware_contract(void)
{
	static const uint8_t broadcast[6] = {
		0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU
	};
	struct drv_usb_device device;
	struct drv_usb_interface interface;
	struct drv_usb_endpoint endpoints[5];
	struct rtl8822bu_adapter *adapter;
	struct wlan_bss_record bss;
	struct wlan_radio_key_request key_request;
	struct wlan_radio_tx_request tx_request;
	struct rtl8822b_rx_packet rx_packet;
	struct rtl8822bu_rx_report_context rx_context;
	uint8_t frame[96];
	uint8_t c2h[12];
	uint64_t deadline;
	size_t frame_length;
	unsigned writes;

	fake_transport_reset();
	make_exact_interface(&device, &interface, endpoints);
	assert(rtl8822bu_attach(&interface, &rtl8822bu_ids[0]) == 0);
	adapter = interface.driver_data;
	assert(fake_net_device.ops->open(&fake_net_device) == 0);
	assert(fake_net_device.ops->transmit(&fake_net_device,
	    malloc(sizeof(struct packet_buf))) == 0);
	assert(station_transmit_calls == 1U &&
	    adapter->radio_operations_active == 0U);
	station_transmit_error = EHOSTUNREACH;
	assert(fake_net_device.ops->transmit(&fake_net_device,
	    malloc(sizeof(struct packet_buf))) == EHOSTUNREACH);
	assert(station_transmit_calls == 2U &&
	    adapter->radio_operations_active == 0U);
	station_transmit_error = 0;
	make_connection_bss(&bss);
	deadline = clock_ticks() + 100U;
	writes = cam_writes;
	/* A receive/poll operation can briefly overlap the completed scan.  The
	 * fresh connect owns its generation and waits inside the same deadline;
	 * it must not leak EBUSY to the WPA engine or clear the BSS snapshot. */
	adapter->radio_operations_active = 1U;
	serialized_connect_adapter = adapter;
	yield_calls = 0U;
	yield_hook = release_serialized_radio_operation;
	assert(rtl8822bu_connect_start(adapter, 900U, &bss, deadline) == 0);
	assert(yield_calls == 1U && serialized_connect_adapter == NULL);
	assert(adapter->connection_generation == 900U &&
	    adapter->connection_prepared && adapter->security_enabled &&
	    adapter->connection_channel == 6U &&
	    rtl8822bu_mac_equal(adapter->connection_bssid, bss.bssid) &&
	    cam_writes == writes);
	assert(rtl8822bu_connect_start(adapter, 900U, &bss, deadline) == 0);
	assert(rtl8822bu_connect_start(adapter, 901U, &bss, deadline) == EBUSY);
	assert(rtl8822bu_scan_channel_start(adapter, 901U, 0U, 1U,
	    deadline) == EBUSY);
	assert(rtl8822bu_association_set(adapter, 899U, bss.bssid, 0x123U,
	    deadline) == ESTALE);
	assert(rtl8822bu_association_set(adapter, 900U, bss.bssid, 0x123U,
	    deadline) == 0);
	assert(adapter->association_active && adapter->association_aid == 0x123U);
	assert(fake_le32(fake_registers + 0x0618U) == 0xccbbaa02U);
	assert(fake_le16(fake_registers + 0x061cU) == 0xeeddU);
	assert((fake_le16(fake_registers + 0x06a8U) & 0x07ffU) == 0x123U);

	memset(&key_request, 0, sizeof(key_request));
	key_request.generation = 900U;
	key_request.key_generation = 77U;
	key_request.deadline_ticks = deadline;
	key_request.kind = WLAN_RADIO_KEY_PAIRWISE;
	memcpy(key_request.address, bss.bssid, 6U);
	memset(key_request.key, 0x11, sizeof(key_request.key));
	assert(rtl8822bu_key_install(adapter, &key_request) == 0);
	assert(adapter->pairwise_key_installed &&
	    adapter->pairwise_key_generation == 77U);
	assert((fake_cam[RTL8822B_CAM_PAIRWISE_SLOT][0] & 0x00008000U) != 0U);
	writes = cam_writes;
	assert(rtl8822bu_key_install(adapter, &key_request) == EALREADY);
	assert(cam_writes == writes);
	key_request.kind = WLAN_RADIO_KEY_GROUP;
	key_request.key_index = 2U;
	key_request.receive_packet_number = 4U;
	memcpy(key_request.address, broadcast, 6U);
	memset(key_request.key, 0x22, sizeof(key_request.key));
	assert(rtl8822bu_key_install(adapter, &key_request) == 0);
	assert((adapter->group_key_mask & (1U << 2)) != 0U &&
	    adapter->group_key_generation[2] == 77U);
	assert((fake_cam[2][0] & (0x00008000U | 0x40U)) ==
	    (0x00008000U | 0x40U));

	memset(&tx_request, 0, sizeof(tx_request));
	tx_request.generation = 900U;
	tx_request.cookie = 0x123456789abcdef0ULL;
	tx_request.deadline_ticks = deadline;
	tx_request.frame_class = WLAN_RADIO_FRAME_MANAGEMENT;
	tx_request.frame = frame;
	tx_request.length = make_station_frame(frame,
	    WLAN_RADIO_FRAME_MANAGEMENT, &bss, 0, 0U, 0U);
	assert(rtl8822bu_frame_transmit(adapter, &tx_request) == 0);
	assert(station_descriptors_checked == 1U);
	make_ccx_report(&rx_packet, c2h, last_tx_sequence, 0U);
	memset(&rx_context, 0, sizeof(rx_context));
	rx_context.adapter = adapter;
	rx_packet.payload_length = 8U;
	assert(rtl8822bu_rx_report(&rx_context, &rx_packet) == EILSEQ);
	assert(tx_report_calls == 0U);
	make_ccx_extended_report(&rx_packet, c2h, last_tx_sequence, 0U);
	c2h[2] = 0x7fU;
	assert(rtl8822bu_rx_report(&rx_context, &rx_packet) == 0);
	assert(tx_report_calls == 0U);
	make_ccx_report(&rx_packet, c2h, last_tx_sequence, 0U);
	assert(rtl8822bu_rx_report(&rx_context, &rx_packet) == 0);
	assert(tx_report_calls == 1U && tx_report_generation == 900U &&
	    tx_report_cookie == 0x123456789abcdef0ULL &&
	    tx_report_acknowledged && tx_report_error == 0);
	/* A duplicate completion is stale and cannot complete another cookie. */
	assert(rtl8822bu_rx_report(&rx_context, &rx_packet) == 0);
	assert(tx_report_calls == 1U);

	tx_request.cookie = 2U;
	tx_request.frame_class = WLAN_RADIO_FRAME_EAPOL;
	tx_request.frame = frame;
	tx_request.length = make_station_frame(frame, WLAN_RADIO_FRAME_EAPOL,
	    &bss, 0, 0U, 0U);
	assert(rtl8822bu_frame_transmit(adapter, &tx_request) == 0);
	assert(station_descriptors_checked == 2U);
	make_ccx_extended_report(&rx_packet, c2h, last_tx_sequence, 0U);
	assert(rtl8822bu_rx_report(&rx_context, &rx_packet) == 0);
	assert(tx_report_calls == 2U && tx_report_cookie == 2U);

	tx_request.cookie = 3U;
	tx_request.key_generation = 77U;
	tx_request.packet_number = 9U;
	tx_request.encrypted = 1U;
	tx_request.frame_class = WLAN_RADIO_FRAME_DATA;
	tx_request.frame = frame;
	tx_request.length = make_station_frame(frame, WLAN_RADIO_FRAME_DATA,
	    &bss, 1, 0U, 9U);
	assert(rtl8822bu_frame_transmit(adapter, &tx_request) == 0);
	assert(data_descriptors_checked == 1U);
	make_ccx_report(&rx_packet, c2h, last_tx_sequence, 0x40U);
	assert(rtl8822bu_rx_report(&rx_context, &rx_packet) == 0);
	assert(tx_report_calls == 3U && tx_report_cookie == 3U &&
	    !tx_report_acknowledged && tx_report_error == EIO);

	/* One lost firmware report remains below the finite stall threshold
	 * and is reclaimed before the next reservation.  The dedicated scan-stall
	 * fixture below proves that the third consecutive expiry reloads firmware. */
	memset(&tx_request, 0, sizeof(tx_request));
	tx_request.generation = 900U;
	tx_request.deadline_ticks = clock_ticks() + 2U;
	tx_request.frame_class = WLAN_RADIO_FRAME_MANAGEMENT;
	tx_request.frame = frame;
	tx_request.length = make_station_frame(frame,
	    WLAN_RADIO_FRAME_MANAGEMENT, &bss, 0, 0U, 0U);
	for (writes = 0U; writes < 1U; writes++) {
		tx_request.cookie = 1000U + writes;
		assert(rtl8822bu_frame_transmit(adapter, &tx_request) == 0);
	}
	tx_request.cookie = 2000U;
	fake_clock_ticks = tx_request.deadline_ticks +
	    RTL8822BU_TX_REPORT_RETIRE_TICKS;
	tx_request.deadline_ticks = clock_ticks() + 100U;
	assert(rtl8822bu_frame_transmit(adapter, &tx_request) == 0);
	make_ccx_report(&rx_packet, c2h, last_tx_sequence, 0U);
	assert(rtl8822bu_rx_report(&rx_context, &rx_packet) == 0);
	assert(tx_report_calls == 4U && tx_report_cookie == 2000U &&
	    tx_report_acknowledged && tx_report_error == 0);

	frame_length = make_ap_frame(frame, &bss, 0, 0U, 0U, 1);
	memset(&rx_packet, 0, sizeof(rx_packet));
	rx_packet.kind = RTL8822B_RX_FRAME;
	rx_packet.payload = frame;
	rx_packet.payload_length = frame_length;
	rx_packet.rssi_dbm = -42;
	assert(rtl8822bu_rx_report(&rx_context, &rx_packet) == 0);
	assert(frame_report_calls == 1U &&
	    last_frame_report.generation == 900U &&
	    last_frame_report.cipher == WLAN_RADIO_CIPHER_NONE &&
	    !last_frame_report.decrypted);

	frame_length = make_ap_frame(frame, &bss, 0, 0U, 10U, 0);
	rx_packet.payload_length = frame_length;
	rx_packet.encryption_type = RTL8822BU_RX_ENCRYPTION_AES;
	assert(rtl8822bu_rx_report(&rx_context, &rx_packet) == 0);
	assert(frame_report_calls == 2U && last_frame_report.decrypted &&
	    last_frame_report.key_generation == 77U &&
	    last_frame_report.packet_number == 10U);
	frame_length = make_ap_frame(frame, &bss, 0, 1U, 10U, 0);
	rx_packet.payload_length = frame_length;
	assert(rtl8822bu_rx_report(&rx_context, &rx_packet) == EACCES);
	assert(frame_report_calls == 2U);
	frame_length = make_ap_frame(frame, &bss, 0, 0U, 10U, 0);
	rx_packet.payload_length = frame_length;
	rx_packet.icv_error = 1U;
	assert(rtl8822bu_rx_report(&rx_context, &rx_packet) == EACCES);
	assert(frame_report_calls == 2U);
	rx_packet.icv_error = 0U;
	frame_length = make_ap_frame(frame, &bss, 1, 2U, 11U, 0);
	rx_packet.payload_length = frame_length;
	assert(rtl8822bu_rx_report(&rx_context, &rx_packet) == 0);
	assert(frame_report_calls == 3U && last_frame_report.key_index == 2U &&
	    last_frame_report.packet_number == 11U);

	/* CAM deletion is a nonblocking checked barrier.  A data frame whose USB
	 * write completed still owns its key until CCX completion or the bounded
	 * retention window expires; the worker may poll/retry without deadlock. */
	memset(&tx_request, 0, sizeof(tx_request));
	tx_request.generation = 900U;
	tx_request.cookie = 3000U;
	tx_request.deadline_ticks = clock_ticks() + 2U;
	tx_request.key_generation = 77U;
	tx_request.packet_number = 12U;
	tx_request.encrypted = 1U;
	tx_request.frame_class = WLAN_RADIO_FRAME_DATA;
	tx_request.frame = frame;
	tx_request.length = make_station_frame(frame, WLAN_RADIO_FRAME_DATA,
	    &bss, 1, 0U, 12U);
	assert(rtl8822bu_frame_transmit(adapter, &tx_request) == 0);
	assert(rtl8822bu_key_delete(adapter, 900U, WLAN_RADIO_KEY_PAIRWISE, 0U,
	    77U, deadline) == EBUSY);
	fake_clock_ticks = tx_request.deadline_ticks +
	    RTL8822BU_TX_REPORT_RETIRE_TICKS;
	deadline = clock_ticks() + 100U;

	assert(rtl8822bu_key_delete(adapter, 900U, WLAN_RADIO_KEY_GROUP, 2U,
	    78U, deadline) == ESTALE);
	assert(rtl8822bu_key_delete(adapter, 900U, WLAN_RADIO_KEY_GROUP, 2U,
	    77U, deadline) == 0);
	assert(rtl8822bu_key_delete(adapter, 900U, WLAN_RADIO_KEY_PAIRWISE, 0U,
	    77U, deadline) == 0);
	assert(rtl8822bu_association_clear(adapter, 900U, deadline) == 0);
	assert(rtl8822bu_disconnect(adapter, 900U) == 0);
	assert(adapter->connection_generation == 0U &&
	    !adapter->connection_prepared && !adapter->association_active &&
	    !adapter->pairwise_key_installed && adapter->group_key_mask == 0U);

	/* A command failure leaves the partially written slot invalid and keeps
	 * the radio usable when both the core and wrapper rollback clear succeed. */
	assert(rtl8822bu_connect_start(adapter, 901U, &bss, deadline) == 0);
	assert(rtl8822bu_association_set(adapter, 901U, bss.bssid, 1U,
	    deadline) == 0);
	memset(&key_request, 0, sizeof(key_request));
	key_request.generation = 901U;
	key_request.key_generation = 78U;
	key_request.deadline_ticks = deadline;
	key_request.kind = WLAN_RADIO_KEY_PAIRWISE;
	memcpy(key_request.address, bss.bssid, 6U);
	memset(key_request.key, 0x33, sizeof(key_request.key));
	control_fail_address = FIXTURE_SEC_COMMAND;
	assert(rtl8822bu_key_install(adapter, &key_request) == ETIMEDOUT);
	assert(!adapter->pairwise_key_installed &&
	    fake_cam[RTL8822B_CAM_PAIRWISE_SLOT][0] == 0U &&
	    !adapter->quarantined);
	assert(rtl8822bu_association_clear(adapter, 901U, deadline) == 0);
	assert(rtl8822bu_disconnect(adapter, 901U) == 0);

	/* A failed association rollback is not allowed to make its inverse
	 * unreachable.  Quarantine blocks forward work, while the matching clear
	 * and final disconnect remain checked recovery barriers. */
	assert(rtl8822bu_connect_start(adapter, 902U, &bss, deadline) == 0);
	control_fail_address = FIXTURE_RCR;
	control_fail_address_remaining = 3U;
	assert(rtl8822bu_association_set(adapter, 902U, bss.bssid, 2U,
	    deadline) == ETIMEDOUT);
	assert(adapter->quarantined && adapter->association_uncertain &&
	    !adapter->association_active);
	assert(rtl8822bu_association_clear(adapter, 902U, deadline) == 0);
	assert(!adapter->association_uncertain);
	assert(rtl8822bu_disconnect(adapter, 902U) == 0);
	assert(!adapter->quarantined);

	/* Likewise, a CAM program whose internal and wrapper rollback both fail
	 * retains the slot generation.  The exact inverse can clear that slot even
	 * though ordinary radio operations are quarantined. */
	assert(rtl8822bu_connect_start(adapter, 903U, &bss, deadline) == 0);
	assert(rtl8822bu_association_set(adapter, 903U, bss.bssid, 3U,
	    deadline) == 0);
	memset(&key_request, 0, sizeof(key_request));
	key_request.generation = 903U;
	key_request.key_generation = 79U;
	key_request.deadline_ticks = deadline;
	key_request.kind = WLAN_RADIO_KEY_PAIRWISE;
	memcpy(key_request.address, bss.bssid, 6U);
	memset(key_request.key, 0x44, sizeof(key_request.key));
	control_fail_address = FIXTURE_SEC_COMMAND;
	control_fail_address_remaining = 3U;
	assert(rtl8822bu_key_install(adapter, &key_request) == ETIMEDOUT);
	assert(adapter->quarantined && !adapter->pairwise_key_installed &&
	    adapter->pairwise_key_generation == 79U &&
	    (adapter->cam_uncertain_mask &
	    (1U << RTL8822B_CAM_PAIRWISE_SLOT)) != 0U);
	assert(rtl8822bu_association_clear(adapter, 903U, deadline) == EBUSY);
	assert(rtl8822bu_key_delete(adapter, 903U, WLAN_RADIO_KEY_PAIRWISE,
	    0U, 79U, deadline) == 0);
	assert(adapter->pairwise_key_generation == 0U &&
	    (adapter->cam_uncertain_mask &
	    (1U << RTL8822B_CAM_PAIRWISE_SLOT)) == 0U);
	assert(rtl8822bu_association_clear(adapter, 903U, deadline) == 0);
	assert(rtl8822bu_disconnect(adapter, 903U) == 0);
	assert(!adapter->quarantined);
	fake_net_device.ops->close(&fake_net_device);
	assert(rtl8822bu_detach(&interface, 0U) == 0);
	assert(interface.driver_data == NULL && allocations == 0U);
}

static void
test_tx_quiesce_queue_barrier(void)
{
	struct drv_usb_device device;
	struct drv_usb_interface interface;
	struct drv_usb_endpoint endpoints[5];
	struct rtl8822bu_adapter *adapter;
	struct wlan_bss_record bss;
	struct wlan_radio_key_request key_request;
	struct wlan_radio_tx_request tx_request;
	struct key_delete_thread_context delete_context = {0};
	pthread_t delete_thread;
	struct packet_buf *packet;
	uint8_t frame[96];
	uint64_t deadline;
	uint16_t reserved;
	unsigned writes, transmit_calls;

	fake_transport_reset();
	make_exact_interface(&device, &interface, endpoints);
	assert(rtl8822bu_attach(&interface, &rtl8822bu_ids[0]) == 0);
	adapter = interface.driver_data;
	assert(fake_net_device.ops->open(&fake_net_device) == 0);
	make_connection_bss(&bss);
	deadline = clock_ticks() + 100U;
	assert(rtl8822bu_connect_start(adapter, 910U, &bss, deadline) == 0);
	assert(rtl8822bu_association_set(adapter, 910U, bss.bssid, 7U,
	    deadline) == 0);
	memset(&key_request, 0, sizeof(key_request));
	key_request.generation = 910U;
	key_request.key_generation = 77U;
	key_request.deadline_ticks = deadline;
	key_request.kind = WLAN_RADIO_KEY_PAIRWISE;
	memcpy(key_request.address, bss.bssid, sizeof(key_request.address));
	memset(key_request.key, 0x5a, sizeof(key_request.key));
	assert(rtl8822bu_key_install(adapter, &key_request) == 0);

	/* A complete but non-empty counter snapshot is a retry state.  CAM is not
	 * touched and admission stays closed until the same inverse observes a
	 * drained queue. */
	reserved = fake_le16(fake_registers + FIXTURE_FIFO_PAGE_LOW);
	assert(reserved != 0U);
	fake_store16(FIXTURE_FIFO_PAGE_LOW + 2U,
	    (uint16_t)(reserved - 1U));
	writes = cam_writes;
	assert(rtl8822bu_key_delete(adapter, 910U,
	    WLAN_RADIO_KEY_PAIRWISE, 0U, 77U, deadline) == EBUSY);
	assert(adapter->tx_quiescing && !adapter->quarantined &&
	    adapter->pairwise_key_installed && cam_writes == writes);
	memset(&tx_request, 0, sizeof(tx_request));
	tx_request.generation = 910U;
	tx_request.cookie = 0x1111U;
	tx_request.deadline_ticks = deadline;
	tx_request.frame_class = WLAN_RADIO_FRAME_MANAGEMENT;
	tx_request.frame = frame;
	tx_request.length = make_station_frame(frame,
	    WLAN_RADIO_FRAME_MANAGEMENT, &bss, 0, 0U, 0U);
	assert(rtl8822bu_frame_transmit(adapter, &tx_request) == EBUSY);
	fake_store16(FIXTURE_FIFO_PAGE_LOW + 2U, reserved);
	assert(rtl8822bu_key_delete(adapter, 910U,
	    WLAN_RADIO_KEY_PAIRWISE, 0U, 77U, deadline) == 0);
	assert(!adapter->tx_quiescing && !adapter->pairwise_key_installed);

	/* Hold the first hardware counter read after the transition atomically
	 * closes admission.  Both the private radio callback and outer net-device
	 * entry must reject a racing frame before CAM mutation begins. */
	key_request.key_generation = 78U;
	assert(rtl8822bu_key_install(adapter, &key_request) == 0);
	delete_context.adapter = adapter;
	delete_context.generation = 910U;
	delete_context.key_generation = 78U;
	delete_context.deadline = UINT64_MAX;
	__atomic_store_n(&threaded_test_active, 1U, __ATOMIC_RELEASE);
	__atomic_store_n(&queue_read_block_once, 1U, __ATOMIC_RELEASE);
	assert(pthread_create(&delete_thread, NULL, run_key_delete_thread,
	    &delete_context) == 0);
	wait_for_atomic_nonzero(&queue_read_block_entered);
	assert(adapter->tx_quiescing && adapter->radio_operations_active == 1U);
	writes = cam_writes;
	tx_request.cookie++;
	assert(rtl8822bu_frame_transmit(adapter, &tx_request) == EBUSY);
	transmit_calls = station_transmit_calls;
	packet = malloc(sizeof(*packet));
	assert(packet != NULL);
	assert(fake_net_device.ops->transmit(&fake_net_device, packet) == EBUSY);
	assert(station_transmit_calls == transmit_calls && cam_writes == writes);
	__atomic_store_n(&queue_read_block_release, 1U, __ATOMIC_RELEASE);
	assert(pthread_join(delete_thread, NULL) == 0);
	__atomic_store_n(&threaded_test_active, 0U, __ATOMIC_RELEASE);
	assert(delete_context.result == 0 &&
	    __atomic_load_n(&delete_context.done, __ATOMIC_ACQUIRE) != 0U);
	assert(!adapter->tx_quiescing && !adapter->pairwise_key_installed);

	/* An incomplete queue snapshot is not EBUSY: retain quarantine and the
	 * gate, then let the exact inverse retry prove drain and clear the slot. */
	deadline = clock_ticks() + 100U;
	key_request.deadline_ticks = deadline;
	key_request.key_generation = 79U;
	assert(rtl8822bu_key_install(adapter, &key_request) == 0);
	writes = cam_writes;
	control_fail_address = FIXTURE_FIFO_PAGE_NORMAL + 2U;
	assert(rtl8822bu_key_delete(adapter, 910U,
	    WLAN_RADIO_KEY_PAIRWISE, 0U, 79U, deadline) == ETIMEDOUT);
	assert(adapter->tx_quiescing && adapter->quarantined &&
	    (adapter->cam_uncertain_mask &
	    (1U << RTL8822B_CAM_PAIRWISE_SLOT)) != 0U &&
	    cam_writes == writes);
	assert(rtl8822bu_key_delete(adapter, 910U,
	    WLAN_RADIO_KEY_PAIRWISE, 0U, 79U, deadline) == 0);
	assert(rtl8822bu_association_clear(adapter, 910U, deadline) == 0);
	assert(rtl8822bu_disconnect(adapter, 910U) == 0);
	assert(!adapter->tx_quiescing && !adapter->quarantined);
	fake_net_device.ops->close(&fake_net_device);
	assert(rtl8822bu_detach(&interface, 0U) == 0);
	assert(interface.driver_data == NULL && allocations == 0U);
}

static void
test_cam_rekey_generation_activation(void)
{
	static const uint8_t broadcast[6] = {
		0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU
	};
	struct drv_usb_device device;
	struct drv_usb_interface interface;
	struct drv_usb_endpoint endpoints[5];
	struct rtl8822bu_adapter *adapter;
	struct wlan_bss_record bss;
	struct wlan_radio_key_request request;
	uint64_t deadline;
	unsigned writes;

	fake_transport_reset();
	make_exact_interface(&device, &interface, endpoints);
	assert(rtl8822bu_attach(&interface, &rtl8822bu_ids[0]) == 0);
	adapter = interface.driver_data;
	assert(fake_net_device.ops->open(&fake_net_device) == 0);
	make_connection_bss(&bss);
	deadline = clock_ticks() + 100U;
	assert(rtl8822bu_connect_start(adapter, 920U, &bss, deadline) == 0);
	assert(rtl8822bu_association_set(adapter, 920U, bss.bssid, 8U,
	    deadline) == 0);

	memset(&request, 0, sizeof(request));
	request.generation = 920U;
	request.deadline_ticks = deadline;
	request.kind = WLAN_RADIO_KEY_PAIRWISE;
	request.key_generation = 77U;
	memcpy(request.address, bss.bssid, sizeof(request.address));
	memset(request.key, 0x17, sizeof(request.key));
	assert(rtl8822bu_key_install(adapter, &request) == 0);
	request.kind = WLAN_RADIO_KEY_GROUP;
	request.key_index = 2U;
	memcpy(request.address, broadcast, sizeof(request.address));
	memset(request.key, 0x27, sizeof(request.key));
	assert(rtl8822bu_key_install(adapter, &request) == 0);
	assert((fake_cam[RTL8822B_CAM_PAIRWISE_SLOT][0] & 0x8000U) != 0U);
	assert((fake_cam[2][0] & 0x8000U) != 0U);

	/* Replacement material is inert until the common WPA engine reports the
	 * matching M4 acknowledgement.  In particular, duplicate BSSID entries
	 * never become simultaneously live and CAM lookup order is irrelevant. */
	request.kind = WLAN_RADIO_KEY_PAIRWISE;
	request.key_index = 0U;
	request.key_generation = 78U;
	memcpy(request.address, bss.bssid, sizeof(request.address));
	memset(request.key, 0x18, sizeof(request.key));
	assert(rtl8822bu_key_install(adapter, &request) == 0);
	assert(adapter->pairwise_staged_installed &&
	    adapter->pairwise_staged_generation == 78U);
	assert(fake_cam[RTL8822BU_PAIRWISE_STAGING_SLOT][0] == 0U);
	writes = cam_writes;
	assert(rtl8822bu_key_install(adapter, &request) == EALREADY);
	assert(cam_writes == writes);
	request.kind = WLAN_RADIO_KEY_GROUP;
	request.key_index = 2U;
	request.key_generation = 78U;
	memcpy(request.address, broadcast, sizeof(request.address));
	memset(request.key, 0x28, sizeof(request.key));
	assert(rtl8822bu_key_install(adapter, &request) == 0);
	assert((adapter->group_staged_mask & (1U << 2)) != 0U &&
	    adapter->group_staged_generation[2] == 78U);
	assert(fake_cam[RTL8822BU_GROUP_STAGING_SLOT_BASE + 2U][0] == 0U);
	assert((fake_cam[RTL8822B_CAM_PAIRWISE_SLOT][0] & 0x8000U) != 0U);
	assert((fake_cam[2][0] & 0x8000U) != 0U);
	device_old_rx_payload_pending = 1U;

	/* Activation never waits for an old RX producer from inside the common
	 * callback.  It returns EBUSY with both replacements still invalid and the
	 * old key generation still live, then succeeds after poll retirement. */
	adapter->polls_active = 1U;
	writes = cam_writes;
	assert(rtl8822bu_keys_activate(adapter, 920U, 78U, 78U,
	    deadline) == EBUSY);
	assert(cam_writes == writes &&
	    (fake_cam[RTL8822B_CAM_PAIRWISE_SLOT][0] & 0x8000U) != 0U &&
	    (fake_cam[2][0] & 0x8000U) != 0U &&
	    fake_cam[RTL8822BU_PAIRWISE_STAGING_SLOT][0] == 0U &&
	    fake_cam[RTL8822BU_GROUP_STAGING_SLOT_BASE + 2U][0] == 0U);
	assert(device_old_rx_payload_pending && rx_generation_flushes == 0U);
	adapter->polls_active = 0U;

	assert(rtl8822bu_keys_activate(adapter, 920U, 78U, 78U,
	    deadline) == 0);
	assert(fake_cam[RTL8822B_CAM_PAIRWISE_SLOT][0] == 0U);
	assert(fake_cam[2][0] == 0U);
	assert((fake_cam[RTL8822BU_PAIRWISE_STAGING_SLOT][0] & 0x8000U) != 0U);
	assert((fake_cam[RTL8822BU_GROUP_STAGING_SLOT_BASE + 2U][0] &
	    0x8000U) != 0U);
	assert(adapter->pairwise_key_generation == 78U &&
	    adapter->pairwise_retired_valid &&
	    adapter->pairwise_retired_generation == 77U);
	assert(adapter->group_key_generation[2] == 78U &&
	    (adapter->group_retired_mask & (1U << 2)) != 0U &&
	    adapter->group_retired_generation[2] == 77U);
	assert(!device_old_rx_payload_pending && rx_generation_flushes == 1U &&
	    old_rx_payload_deliveries == 0U);
	writes = cam_writes;
	assert(rtl8822bu_keys_activate(adapter, 920U, 78U, 78U,
	    deadline) == 0);
	assert(cam_writes == writes);

	/* Old-generation deletes consume software tombstones only and cannot
	 * clear the newly active physical slots. */
	assert(rtl8822bu_key_delete(adapter, 920U, WLAN_RADIO_KEY_GROUP, 2U,
	    77U, deadline) == 0);
	assert(rtl8822bu_key_delete(adapter, 920U, WLAN_RADIO_KEY_PAIRWISE, 0U,
	    77U, deadline) == 0);
	assert(cam_writes == writes);
	assert((fake_cam[adapter->pairwise_key_slot][0] & 0x8000U) != 0U);
	assert((fake_cam[adapter->group_key_slot[2]][0] & 0x8000U) != 0U);
	assert(rtl8822bu_key_delete(adapter, 920U, WLAN_RADIO_KEY_GROUP, 2U,
	    78U, deadline) == 0);
	assert(rtl8822bu_key_delete(adapter, 920U, WLAN_RADIO_KEY_PAIRWISE, 0U,
	    78U, deadline) == 0);
	/* Explicit teardown attempts one bounded deauthentication while association
	 * identity still exists.  A transport timeout is best-effort and cannot
	 * prevent the local association/disconnect inverses from completing. */
	bulk_fail_at = bulk_calls + 1U;
	writes = deauthentication_descriptors_checked;
	assert(rtl8822bu_association_clear(adapter, 920U, deadline) == 0);
	assert(deauthentication_descriptors_checked == writes + 1U);
	bulk_fail_at = 0U;
	assert(rtl8822bu_disconnect(adapter, 920U) == 0);
	assert(deauthentication_descriptors_checked == writes + 1U);
	fake_net_device.ops->close(&fake_net_device);
	assert(rtl8822bu_detach(&interface, 0U) == 0);
	assert(interface.driver_data == NULL && allocations == 0U);
}

static void
test_cam_activation_failure_quarantine(void)
{
	static const uint8_t broadcast[6] = {
		0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU
	};
	struct drv_usb_device device;
	struct drv_usb_interface interface;
	struct drv_usb_endpoint endpoints[5];
	struct rtl8822bu_adapter *adapter;
	struct wlan_bss_record bss;
	struct wlan_radio_key_request request;
	uint64_t deadline, generation;
	unsigned failure;

	/* Fail the first old-entry invalidation and, separately, the replacement
	 * valid-word publication after both old entries were invalidated. */
	for (failure = 0U; failure < 2U; failure++) {
		fake_transport_reset();
		make_exact_interface(&device, &interface, endpoints);
		assert(rtl8822bu_attach(&interface, &rtl8822bu_ids[0]) == 0);
		adapter = interface.driver_data;
		assert(fake_net_device.ops->open(&fake_net_device) == 0);
		make_connection_bss(&bss);
		deadline = clock_ticks() + 100U;
		generation = 930U + failure;
		assert(rtl8822bu_connect_start(adapter, generation, &bss,
		    deadline) == 0);
		assert(rtl8822bu_association_set(adapter, generation, bss.bssid,
		    9U, deadline) == 0);
		memset(&request, 0, sizeof(request));
		request.generation = generation;
		request.deadline_ticks = deadline;
		request.kind = WLAN_RADIO_KEY_PAIRWISE;
		request.key_generation = 77U;
		memcpy(request.address, bss.bssid, sizeof(request.address));
		memset(request.key, 0x37, sizeof(request.key));
		assert(rtl8822bu_key_install(adapter, &request) == 0);
		request.kind = WLAN_RADIO_KEY_GROUP;
		request.key_index = 2U;
		memcpy(request.address, broadcast, sizeof(request.address));
		memset(request.key, 0x47, sizeof(request.key));
		assert(rtl8822bu_key_install(adapter, &request) == 0);
		request.kind = WLAN_RADIO_KEY_PAIRWISE;
		request.key_index = 0U;
		request.key_generation = 78U;
		memcpy(request.address, bss.bssid, sizeof(request.address));
		memset(request.key, 0x38, sizeof(request.key));
		assert(rtl8822bu_key_install(adapter, &request) == 0);
		request.kind = WLAN_RADIO_KEY_GROUP;
		request.key_index = 2U;
		memcpy(request.address, broadcast, sizeof(request.address));
		memset(request.key, 0x48, sizeof(request.key));
		assert(rtl8822bu_key_install(adapter, &request) == 0);

		sec_command_write_fail_enabled = 1U;
		sec_command_write_fail_skip = failure == 0U ? 0U : 2U;
		assert(rtl8822bu_keys_activate(adapter, generation, 78U, 78U,
		    deadline) == ETIMEDOUT);
		assert(adapter->quarantined && adapter->tx_quiescing &&
		    adapter->rx_generation_barrier);
		/* Failure can leave old entries either live or already absent, but staged
		 * replacements are always rolled back to invalid word zero. */
		assert(fake_cam[RTL8822BU_PAIRWISE_STAGING_SLOT][0] == 0U);
		assert(fake_cam[RTL8822BU_GROUP_STAGING_SLOT_BASE + 2U][0] == 0U);

		assert(rtl8822bu_key_delete(adapter, generation,
		    WLAN_RADIO_KEY_GROUP, 2U, 78U, deadline) == 0);
		assert(rtl8822bu_key_delete(adapter, generation,
		    WLAN_RADIO_KEY_PAIRWISE, 0U, 78U, deadline) == 0);
		assert(rtl8822bu_key_delete(adapter, generation,
		    WLAN_RADIO_KEY_GROUP, 2U, 77U, deadline) == 0);
		assert(rtl8822bu_key_delete(adapter, generation,
		    WLAN_RADIO_KEY_PAIRWISE, 0U, 77U, deadline) == 0);
		assert(rtl8822bu_association_clear(adapter, generation,
		    deadline) == 0);
		assert(rtl8822bu_disconnect(adapter, generation) == 0);
		fake_net_device.ops->close(&fake_net_device);
		assert(rtl8822bu_detach(&interface, 0U) == 0);
		assert(interface.driver_data == NULL && allocations == 0U);
	}
}

static void
test_active_connection_terminal_cleanup(void)
{
	static const uint8_t broadcast[6] = {
		0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU
	};
	struct drv_usb_device device;
	struct drv_usb_interface interface;
	struct drv_usb_endpoint endpoints[5];
	struct rtl8822bu_adapter *adapter;
	struct wlan_bss_record bss;
	struct wlan_radio_key_request request;
	uint64_t deadline, generation;
	unsigned shutdown;

	for (shutdown = 0U; shutdown < 2U; shutdown++) {
		fake_transport_reset();
		make_exact_interface(&device, &interface, endpoints);
		assert(rtl8822bu_attach(&interface, &rtl8822bu_ids[0]) == 0);
		adapter = interface.driver_data;
		assert(fake_net_device.ops->open(&fake_net_device) == 0);
		make_connection_bss(&bss);
		generation = 960U + shutdown;
		deadline = clock_ticks() + 100U;
		assert(rtl8822bu_connect_start(adapter, generation, &bss,
		    deadline) == 0);
		assert(rtl8822bu_association_set(adapter, generation, bss.bssid,
		    10U, deadline) == 0);
		memset(&request, 0, sizeof(request));
		request.generation = generation;
		request.key_generation = 88U;
		request.deadline_ticks = deadline;
		request.kind = WLAN_RADIO_KEY_PAIRWISE;
		memcpy(request.address, bss.bssid, sizeof(request.address));
		memset(request.key, 0x58, sizeof(request.key));
		assert(rtl8822bu_key_install(adapter, &request) == 0);
		request.kind = WLAN_RADIO_KEY_GROUP;
		request.key_index = 1U;
		memcpy(request.address, broadcast, sizeof(request.address));
		memset(request.key, 0x68, sizeof(request.key));
		assert(rtl8822bu_key_install(adapter, &request) == 0);

		station_close_cleanup_adapter = adapter;
		station_close_cleanup_generation = generation;
		station_close_cleanup_pairwise_generation = 88U;
		station_close_cleanup_group_generation = 88U;
		station_close_cleanup_group_index = 1U;
		if (shutdown) {
			rtl8822bu_shutdown(&interface);
			assert(!adapter->ready && adapter->detaching &&
			    !adapter->opened && adapter->radio.state ==
			    RTL8822B_RADIO_OFF);
			assert(rtl8822bu_detach(&interface, 0U) == 0);
		} else {
			open_count_during_gone = 1U;
			assert(rtl8822bu_detach(&interface, 0U) == 0);
		}
		assert(station_close_cleanup_adapter == NULL &&
		    deauthentication_descriptors_checked == 1U);
		for (unsigned slot = 0U; slot < RTL8822BU_CAM_OWNED_SLOT_COUNT;
		    slot++)
			assert(fake_cam[slot][0] == 0U);
		assert(interface.driver_data == NULL && allocations == 0U);
	}
}

static void
test_force_unplug_and_fresh_reinsert(void)
{
	static const uint8_t broadcast[6] = {
		0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU
	};
	struct drv_usb_device device;
	struct drv_usb_interface interface;
	struct drv_usb_endpoint endpoints[5];
	struct rtl8822bu_adapter *adapter;
	struct wlan_bss_record bss;
	struct wlan_radio_key_request request;
	uint64_t deadline;
	unsigned control_before, bulk_before;

	fake_transport_reset();
	make_exact_interface(&device, &interface, endpoints);
	assert(rtl8822bu_attach(&interface, &rtl8822bu_ids[0]) == 0);
	adapter = interface.driver_data;
	assert(fake_net_device.ops->open(&fake_net_device) == 0);
	make_connection_bss(&bss);
	deadline = clock_ticks() + 100U;
	assert(rtl8822bu_connect_start(adapter, 980U, &bss, deadline) == 0);
	assert(rtl8822bu_association_set(adapter, 980U, bss.bssid, 12U,
	    deadline) == 0);
	memset(&request, 0, sizeof(request));
	request.generation = 980U;
	request.key_generation = 98U;
	request.deadline_ticks = deadline;
	request.kind = WLAN_RADIO_KEY_PAIRWISE;
	memcpy(request.address, bss.bssid, sizeof(request.address));
	memset(request.key, 0x98, sizeof(request.key));
	assert(rtl8822bu_key_install(adapter, &request) == 0);
	request.kind = WLAN_RADIO_KEY_GROUP;
	request.key_index = 1U;
	memcpy(request.address, broadcast, sizeof(request.address));
	memset(request.key, 0xa8, sizeof(request.key));
	assert(rtl8822bu_key_install(adapter, &request) == 0);

	station_close_cleanup_adapter = adapter;
	station_close_cleanup_generation = 980U;
	station_close_cleanup_pairwise_generation = 98U;
	station_close_cleanup_group_generation = 98U;
	station_close_cleanup_group_index = 1U;
	open_count_during_gone = 1U;
	defer_net_release = 1U;
	control_before = control_calls;
	bulk_before = bulk_calls;
	/* Model device_begin_disconnect(): from here on every new transfer and
	 * clear-halt request for this physical generation is permanently ENODEV. */
	transport_forced_absent = 1U;
	assert(rtl8822bu_detach(&interface,
	    DRV_USB_DETACH_FORCE | DRV_USB_DETACH_QUIET) == 0);
	assert(transport_io_after_absence == 0U);
	assert(control_calls == control_before && bulk_calls == bulk_before);
	assert(station_close_cleanup_adapter == NULL);
	assert(interface.driver_data == NULL && net_gone && station_detached);
	assert(adapter->transport_absent && !adapter->ready &&
	    !adapter->opened && !adapter->radio_running &&
	    !adapter->firmware_running && adapter->rx_urb == NULL &&
	    adapter->rx_buffer == NULL);
	assert(rtl8822bu_bytes_zero(&adapter->radio, sizeof(adapter->radio)));
	assert(adapter->connection_generation == 0U &&
	    adapter->pairwise_key_generation == 0U &&
	    adapter->pairwise_staged_generation == 0U &&
	    adapter->group_key_mask == 0U && adapter->group_staged_mask == 0U &&
	    adapter->group_retired_mask == 0U &&
	    adapter->cam_uncertain_mask == 0U &&
	    adapter->rx_inflight_generation == 0U &&
	    adapter->rx_submit_generation == 0U &&
	    adapter->recovery_pending == 0U &&
	    adapter->control_error_streak == 0U &&
	    adapter->tx_high_error_streak == 0U &&
	    adapter->tx_normal_error_streak == 0U &&
	    adapter->tx_low_error_streak == 0U);
	assert(deauthentication_descriptors_checked == 0U);
	fake_net_release_deferred();
	defer_net_release = 0U;
	assert(allocations == 0U);

	/* A reinsert owns a new USB/driver generation and starts with none of the
	 * absent generation's gates, keys, URBs, or error streaks. */
	fake_transport_reset();
	make_exact_interface(&device, &interface, endpoints);
	assert(rtl8822bu_attach(&interface, &rtl8822bu_ids[0]) == 0);
	adapter = interface.driver_data;
	assert(!adapter->transport_absent && adapter->ready &&
	    adapter->connection_generation == 0U && adapter->rx_urb != NULL);
	assert(fake_net_device.ops->open(&fake_net_device) == 0);
	fake_net_device.ops->close(&fake_net_device);
	assert(rtl8822bu_detach(&interface, 0U) == 0);
	assert(interface.driver_data == NULL && allocations == 0U);
}

static void
test_first_open_software_scan(void)
{
	struct drv_usb_device device;
	struct drv_usb_interface interface;
	struct drv_usb_endpoint endpoints[5];
	struct rtl8822bu_adapter *adapter;
	uint8_t probe[26];
	uint8_t directed_probe[64];
	uint64_t deadline;
	size_t aggregate_length;
	unsigned reports, errors;

	fake_transport_reset();
	make_exact_interface(&device, &interface, endpoints);
	assert(rtl8822bu_attach(&interface, &rtl8822bu_ids[0]) == 0);
	adapter = interface.driver_data;
	assert(fake_net_device.ops->open(&fake_net_device) == 0);
	deadline = clock_ticks() + 50U;
	assert(rtl8822bu_scan_channel_start(adapter, 42U, 5U, 6U,
	    deadline) == 0);
	assert(scan_ready_calls == 1U && scan_ready_generation == 42U &&
	    scan_ready_step == 5U);
	assert(adapter->scan_generation == 42U && adapter->scan_channel == 6U);
	assert(rtl8822bu_management_transmit(adapter, 42U, probe,
	    make_wildcard_probe(probe), deadline) == 0);
	assert(scan_probe_descriptors_checked == 1U);
	/* Byte-for-byte common reconnect Probe Request contract: one directed
	 * SSID IE plus the four basic rates reaches production RTL validation,
	 * descriptor creation, and the high-priority bulk OUT endpoint. */
	assert(rtl8822bu_management_transmit(adapter, 42U, directed_probe,
	    make_directed_probe(directed_probe,
	    (const uint8_t *)"fixture", 7U), deadline) == 0);
	assert(scan_probe_descriptors_checked == 2U);
	bulk_short_at = bulk_calls + 1U;
	assert(rtl8822bu_management_transmit(adapter, 42U, probe,
	    26U, deadline) == EIO);
	bulk_short_at = 0U;
	bulk_fail_at = bulk_calls + 1U;
	assert(rtl8822bu_management_transmit(adapter, 42U, probe,
	    26U, deadline) == ETIMEDOUT);
	bulk_fail_at = 0U;
	assert(rtl8822bu_management_transmit(adapter, 42U, probe,
	    26U, clock_ticks()) == ETIMEDOUT);

	aggregate_length = make_beacon_aggregate(adapter->rx_buffer, 0x80U);
	fake_urb_complete(adapter->rx_urb, DRV_USB_URB_COMPLETE,
	    aggregate_length);
	assert(rtl8822bu_poll_receive(&fake_net_device, 1U) == 1U);
	assert(scan_report_calls == 1U && scan_report_generation == 42U &&
	    scan_report_channel == 6U);
	assert(rtl8822bu_scan_stop(adapter, 42U) == 0);
	assert(adapter->scan_generation == 0U && adapter->scan_channel == 0U);

	/* Late management RX after the synchronous stop barrier is ignored and is
	 * not misclassified as a malformed aggregate. */
	reports = scan_report_calls;
	errors = fake_net_device.rx_errors;
	aggregate_length = make_beacon_aggregate(adapter->rx_buffer, 0x80U);
	fake_urb_complete(adapter->rx_urb, DRV_USB_URB_COMPLETE,
	    aggregate_length);
	assert(rtl8822bu_poll_receive(&fake_net_device, 1U) == 1U);
	assert(scan_report_calls == reports && fake_net_device.rx_errors == errors);
	assert(rtl8822bu_management_transmit(adapter, 42U, probe,
	    sizeof(probe), deadline) == ENETDOWN);
	fake_net_device.ops->close(&fake_net_device);
	assert(station_close_calls == 1U && !adapter->opened &&
	    adapter->radio.state == RTL8822B_RADIO_OFF);
	assert(rtl8822bu_detach(&interface, 0U) == 0);
	assert(interface.driver_data == NULL && allocations == 0U);
}

static void
test_w52_scan_and_connect(void)
{
	struct drv_usb_device device;
	struct drv_usb_interface interface;
	struct drv_usb_endpoint endpoints[5];
	struct rtl8822bu_adapter *adapter;
	struct wlan_bss_record bss;
	uint64_t deadline;

	fake_transport_reset();
	make_exact_interface(&device, &interface, endpoints);
	assert(rtl8822bu_attach(&interface, &rtl8822bu_ids[0]) == 0);
	adapter = interface.driver_data;
	assert(fake_net_device.ops->open(&fake_net_device) == 0);
	deadline = clock_ticks() + 100U;
	assert(rtl8822bu_scan_channel_start(adapter, 48U, 14U, 48U,
	    deadline) == 0);
	assert(adapter->scan_generation == 48U && adapter->scan_channel == 48U &&
	    adapter->radio.channel == 48U);
	assert(rtl8822bu_scan_stop(adapter, 48U) == 0);
	assert(rtl8822bu_scan_channel_start(adapter, 49U, 0U, 1U,
	    deadline) == 0);
	assert(adapter->scan_generation == 49U && adapter->scan_channel == 1U &&
	    adapter->radio.channel == 1U);
	assert(rtl8822bu_scan_stop(adapter, 49U) == 0);

	make_connection_bss(&bss);
	bss.channel = 44U;
	bss.center_frequency_mhz = 5220U;
	assert(rtl8822bu_connect_start(adapter, 50U, &bss, deadline) == 0);
	assert(adapter->connection_prepared &&
	    adapter->connection_generation == 50U &&
	    adapter->connection_channel == 44U && adapter->radio.channel == 44U);
	assert(rtl8822bu_disconnect(adapter, 50U) == 0);
	bss.channel = 52U;
	bss.center_frequency_mhz = 5260U;
	assert(rtl8822bu_connect_start(adapter, 51U, &bss, deadline) == EINVAL);
	fake_net_device.ops->close(&fake_net_device);
	assert(rtl8822bu_detach(&interface, 0U) == 0);
	assert(interface.driver_data == NULL && allocations == 0U);
}

static void
test_scan_report_stall_recovery(void)
{
	struct drv_usb_device device;
	struct drv_usb_interface interface;
	struct drv_usb_endpoint endpoints[5];
	struct rtl8822bu_adapter *adapter;
	struct rtl8822b_rx_packet report_packet;
	struct rtl8822bu_rx_report_context report_context;
	uint8_t probe[64];
	uint8_t report_bytes[9];
	uint8_t sequences[3];
	uint64_t deadline;
	unsigned firmware_before, reports_before, index;

	fake_transport_reset();
	make_exact_interface(&device, &interface, endpoints);
	assert(rtl8822bu_attach(&interface, &rtl8822bu_ids[0]) == 0);
	adapter = interface.driver_data;
	assert(fake_net_device.ops->open(&fake_net_device) == 0);
	memset(&report_context, 0, sizeof(report_context));
	report_context.adapter = adapter;

	/* A silent WCPU can keep accepting high bulk OUT while never returning
	 * CCX completion on bulk IN.  Each expired correlation becomes a
	 * non-reusable tombstone in this hardware generation; the third arms a
	 * finite device-local firmware reload. */
	for (index = 0U; index < 3U; index++) {
		deadline = clock_ticks() + 10U;
		assert(rtl8822bu_scan_channel_start(adapter, 1000U + index,
		    0U, 6U, deadline) == 0);
		assert(rtl8822bu_management_transmit(adapter, 1000U + index,
		    probe, make_directed_probe(probe,
		    (const uint8_t *)"fixture", 7U), deadline) == 0);
		sequences[index] = last_tx_sequence;
		assert(index == 0U || sequences[index] != sequences[index - 1U]);
		assert(rtl8822bu_scan_stop(adapter, 1000U + index) == 0);
		fake_clock_ticks += RTL8822BU_TX_REPORT_RETIRE_TICKS + 11U;
		if (index == 1U) {
			/* A late report for the expired first sequence is rejected as
			 * stale and cannot complete the second probe's cookie. */
			reports_before = tx_report_calls;
			make_ccx_report(&report_packet, report_bytes, sequences[0], 0U);
			assert(rtl8822bu_rx_report(&report_context,
			    &report_packet) == 0);
			assert(tx_report_calls == reports_before);
		}
	}
	deadline = clock_ticks() + 10U;
	assert(rtl8822bu_scan_channel_start(adapter, 1003U, 0U, 6U,
	    deadline) == 0);
	assert(rtl8822bu_management_transmit(adapter, 1003U, probe,
	    make_directed_probe(probe, (const uint8_t *)"fixture", 7U),
	    deadline) == ENETDOWN);
	assert(adapter->recovery_pending && adapter->tx_quiescing &&
	    adapter->tx_report_tombstone_count == 3U &&
	    adapter->recovery_error == ETIMEDOUT);
	firmware_before = firmware_load_calls;
	assert(rtl8822bu_poll_receive(&fake_net_device, 1U) == 1U);
	assert(firmware_load_calls == firmware_before + 1U &&
	    !adapter->recovery_pending && !adapter->quarantined &&
	    adapter->tx_report_tombstone_count == 0U &&
	    adapter->tx_report_error_streak == 0U && adapter->opened);

	/* Only that hardware reset boundary permits sequence reuse.  One later
	 * expiry remains cumulative as a tombstone, while a valid fresh report
	 * resets the consecutive-error streak and common ESTALE is benign. */
	deadline = clock_ticks() + 10U;
	assert(rtl8822bu_scan_channel_start(adapter, 1100U, 0U, 6U,
	    deadline) == 0);
	assert(rtl8822bu_management_transmit(adapter, 1100U, probe,
	    make_directed_probe(probe, (const uint8_t *)"fixture", 7U),
	    deadline) == 0);
	assert(last_tx_sequence == sequences[0]);
	assert(rtl8822bu_scan_stop(adapter, 1100U) == 0);
	fake_clock_ticks += RTL8822BU_TX_REPORT_RETIRE_TICKS + 11U;
	deadline = clock_ticks() + 10U;
	assert(rtl8822bu_scan_channel_start(adapter, 1101U, 0U, 6U,
	    deadline) == 0);
	assert(rtl8822bu_management_transmit(adapter, 1101U, probe,
	    make_directed_probe(probe, (const uint8_t *)"fixture", 7U),
	    deadline) == 0);
	assert(last_tx_sequence != sequences[0] &&
	    adapter->tx_report_tombstone_count == 1U &&
	    adapter->tx_report_error_streak == 1U);
	make_ccx_report(&report_packet, report_bytes, last_tx_sequence, 0U);
	tx_report_return_error = ESTALE;
	assert(rtl8822bu_rx_report(&report_context, &report_packet) == 0);
	tx_report_return_error = 0;
	assert(adapter->tx_report_error_streak == 0U &&
	    adapter->tx_report_tombstone_count == 1U);
	assert(rtl8822bu_scan_stop(adapter, 1101U) == 0);
	fake_net_device.ops->close(&fake_net_device);
	assert(rtl8822bu_detach(&interface, 0U) == 0);
	assert(interface.driver_data == NULL && allocations == 0U);
}

static void
test_attempted_tx_report_tombstones(void)
{
	struct drv_usb_device device;
	struct drv_usb_interface interface;
	struct drv_usb_endpoint endpoints[5];
	struct rtl8822bu_adapter *adapter;
	struct rtl8822b_rx_packet report_packet;
	struct rtl8822bu_rx_report_context report_context;
	uint8_t report_bytes[9];
	uint8_t probe[64];
	uint8_t short_sequence, timeout_sequence, live_sequence;
	uint64_t deadline;
	unsigned reports_before;

	fake_transport_reset();
	make_exact_interface(&device, &interface, endpoints);
	assert(rtl8822bu_attach(&interface, &rtl8822bu_ids[0]) == 0);
	adapter = interface.driver_data;
	assert(fake_net_device.ops->open(&fake_net_device) == 0);
	deadline = clock_ticks() + 100U;
	assert(rtl8822bu_scan_channel_start(adapter, 1200U, 0U, 6U,
	    deadline) == 0);
	bulk_short_at = bulk_calls + 1U;
	assert(rtl8822bu_management_transmit(adapter, 1200U, probe,
	    make_directed_probe(probe, (const uint8_t *)"fixture", 7U),
	    deadline) == EIO);
	short_sequence = last_tx_sequence;
	bulk_short_at = 0U;
	bulk_fail_at = bulk_calls + 1U;
	assert(rtl8822bu_management_transmit(adapter, 1200U, probe,
	    make_directed_probe(probe, (const uint8_t *)"fixture", 7U),
	    deadline) == ETIMEDOUT);
	timeout_sequence = last_tx_sequence;
	bulk_fail_at = 0U;
	assert(short_sequence != timeout_sequence &&
	    adapter->tx_report_tombstone_count == 2U);
	assert(rtl8822bu_management_transmit(adapter, 1200U, probe,
	    make_directed_probe(probe, (const uint8_t *)"fixture", 7U),
	    deadline) == 0);
	live_sequence = last_tx_sequence;
	assert(live_sequence != short_sequence &&
	    live_sequence != timeout_sequence);
	memset(&report_context, 0, sizeof(report_context));
	report_context.adapter = adapter;
	reports_before = tx_report_calls;
	make_ccx_report(&report_packet, report_bytes, short_sequence, 0U);
	assert(rtl8822bu_rx_report(&report_context, &report_packet) == 0);
	make_ccx_report(&report_packet, report_bytes, timeout_sequence, 0U);
	assert(rtl8822bu_rx_report(&report_context, &report_packet) == 0);
	assert(tx_report_calls == reports_before);
	make_ccx_report(&report_packet, report_bytes, live_sequence, 0U);
	assert(rtl8822bu_rx_report(&report_context, &report_packet) == 0);
	assert(tx_report_calls == reports_before + 1U);
	assert(rtl8822bu_scan_stop(adapter, 1200U) == 0);
	fake_net_device.ops->close(&fake_net_device);
	assert(rtl8822bu_detach(&interface, 0U) == 0);
	assert(interface.driver_data == NULL && allocations == 0U);
}

static void
test_scan_channel_transport_failure_quarantine(void)
{
	struct drv_usb_device device;
	struct drv_usb_interface interface;
	struct drv_usb_endpoint endpoints[5];
	struct rtl8822bu_adapter *adapter;
	uint8_t probe[26];
	uint64_t deadline;
	size_t aggregate_length;
	unsigned reports, schedules, submits;

	fake_transport_reset();
	make_exact_interface(&device, &interface, endpoints);
	assert(rtl8822bu_attach(&interface, &rtl8822bu_ids[0]) == 0);
	adapter = interface.driver_data;
	assert(fake_net_device.ops->open(&fake_net_device) == 0);
	deadline = clock_ticks() + 50U;

	/* Wrapper validation never reaches the core and must leave a healthy
	 * radio usable rather than turning a caller error into quarantine. */
	assert(rtl8822bu_scan_channel_start(adapter, 0U, 0U, 1U,
	    deadline) == EINVAL);
	assert(rtl8822bu_scan_channel_start(adapter, 1U, 0U, 12U,
	    deadline) == EINVAL);
	assert(adapter->radio.state == RTL8822B_RADIO_STARTED);
	assert(adapter->firmware_running && adapter->radio_running &&
	    adapter->opened && !adapter->quarantined);

	/* A transport failure after channel programming starts makes the core
	 * emergency-off and clear itself.  The USB wrapper must mirror that state
	 * while retaining ownership of the pending RX URB for checked detach. */
	control_fail_address = RTL8822B_REG_TX_PAUSE;
	assert(rtl8822bu_scan_channel_start(adapter, 43U, 6U, 7U,
	    deadline) == ETIMEDOUT);
	assert(adapter->radio.state == RTL8822B_RADIO_OFF);
	assert(!adapter->firmware_running && !adapter->radio_running &&
	    adapter->opened && adapter->quarantined);
	assert(adapter->scan_generation == 0U && adapter->scan_channel == 0U);
	assert(adapter->rx_urb->status == DRV_USB_URB_PENDING);
	assert(rtl8822bu_management_transmit(adapter, 43U, probe,
	    make_wildcard_probe(probe), deadline) == ENETDOWN);

	/* A completion already owned by the controller is allowed to retire, but
	 * quarantine suppresses both publication and RX rearm. */
	schedules = poll_schedule_calls;
	submits = urb_submit_calls;
	reports = scan_report_calls;
	aggregate_length = make_beacon_aggregate(adapter->rx_buffer, 0x80U);
	fake_urb_complete(adapter->rx_urb, DRV_USB_URB_COMPLETE,
	    aggregate_length);
	assert(poll_schedule_calls == schedules);
	assert(rtl8822bu_poll_receive(&fake_net_device, 1U) == 0U);
	assert(urb_submit_calls == submits && scan_report_calls == reports);
	assert(rtl8822bu_detach(&interface, 0U) == 0);
	assert(interface.driver_data == NULL && allocations == 0U);
}

static void
test_close_finite_station_join(void)
{
	struct drv_usb_device device;
	struct drv_usb_interface interface;
	struct drv_usb_endpoint endpoints[5];
	struct rtl8822bu_adapter *adapter;
	uint64_t before;

	/* A short-lived common callback retires while close holds only the driver
	 * lifecycle mutex; no adapter/common spin lock blocks its progress. */
	fake_transport_reset();
	make_exact_interface(&device, &interface, endpoints);
	assert(rtl8822bu_attach(&interface, &rtl8822bu_ids[0]) == 0);
	adapter = interface.driver_data;
	assert(fake_net_device.ops->open(&fake_net_device) == 0);
	station_close_busy_count = 3U;
	fake_net_device.ops->close(&fake_net_device);
	assert(station_close_calls == 4U && station_close_busy_count == 0U);
	assert(!adapter->opened && !adapter->quarantined &&
	    adapter->radio.state == RTL8822B_RADIO_OFF);
	assert(rtl8822bu_detach(&interface, 0U) == 0);
	assert(interface.driver_data == NULL && allocations == 0U);

	/* A producer that misses the finite void-close bound leaves RX/DMA and the
	 * radio owned, closes further admission, and is recovered by checked
	 * detach after the common callback becomes joinable. */
	fake_transport_reset();
	make_exact_interface(&device, &interface, endpoints);
	assert(rtl8822bu_attach(&interface, &rtl8822bu_ids[0]) == 0);
	adapter = interface.driver_data;
	assert(fake_net_device.ops->open(&fake_net_device) == 0);
	station_close_error = EBUSY;
	before = clock_ticks();
	fake_net_device.ops->close(&fake_net_device);
	assert(clock_ticks() >= before + RTL8822BU_STATION_CLOSE_TIMEOUT_TICKS);
	assert(adapter->quarantined && adapter->opened &&
	    adapter->radio.state == RTL8822B_RADIO_STARTED &&
	    adapter->rx_urb->status == DRV_USB_URB_PENDING);
	station_close_error = 0;
	assert(rtl8822bu_detach(&interface, 0U) == 0);
	assert(interface.driver_data == NULL && allocations == 0U);
}

static void
test_rx_poll_rearm_and_lifecycle(void)
{
	struct drv_usb_device device;
	struct drv_usb_interface interface;
	struct drv_usb_endpoint endpoints[5];
	struct drv_usb_endpoint storage_endpoint;
	struct drv_usb_urb storage_urb;
	struct rtl8822bu_adapter *adapter;
	uint8_t storage_byte = 0U;
	size_t aggregate_length;
	unsigned submits;

	fake_transport_reset();
	net_created = 0U;
	net_gone = 0U;
	station_attached = 0U;
	station_detached = 0U;
	station_attach_error = 0;
	station_detach_error = 0;
	open_during_create = 0U;
	callback_during_gone = 0U;
	urb_submit_calls = 0U;
	urb_cancel_calls = 0U;
	urb_drain_calls = 0U;
	poll_schedule_calls = 0U;
	scan_report_calls = 0U;
	storage_progress = 0U;
	persistent_rx_urb = NULL;
	make_exact_interface(&device, &interface, endpoints);
	assert(rtl8822bu_attach(&interface, &rtl8822bu_ids[0]) == 0);
	adapter = interface.driver_data;
	assert(adapter != NULL);
	assert(rtl8822bu_rx_start(adapter, 77U, 6U) == 0);
	assert(adapter->rx_urb == persistent_rx_urb);
	assert(adapter->rx_urb->status == DRV_USB_URB_PENDING);

	/* A different endpoint remains independently admissible while the
	 * persistent WLAN request is owned by the fake HCD. */
	memset(&storage_endpoint, 0, sizeof(storage_endpoint));
	storage_endpoint.descriptor.address = 0x82U;
	storage_endpoint.type = DRV_USB_TRANSFER_BULK;
	memset(&storage_urb, 0, sizeof(storage_urb));
	storage_urb.device = &device;
	storage_urb.endpoint = &storage_endpoint;
	storage_urb.status = DRV_USB_URB_IDLE;
	assert(drv_usb_urb_setup(&storage_urb, &storage_byte,
	    sizeof(storage_byte), 0U, 0U, NULL, NULL) == 0);
	assert(drv_usb_urb_submit(&storage_urb) == 0);
	assert(storage_progress == 1U);
	fake_urb_complete(&storage_urb, DRV_USB_URB_COMPLETE, 1U);

	aggregate_length = make_beacon_aggregate(adapter->rx_buffer, 0x80U);
	fake_urb_complete(adapter->rx_urb, DRV_USB_URB_COMPLETE,
	    aggregate_length);
	assert(poll_schedule_calls == 1U);
	assert(rtl8822bu_poll_receive(&fake_net_device, 1U) == 1U);
	assert(scan_report_calls == 1U);
	assert(scan_report_generation == 77U && scan_report_channel == 6U);
	assert(scan_report_rssi == -60);
	assert(adapter->rx_urb == persistent_rx_urb);
	assert(adapter->rx_urb->status == DRV_USB_URB_PENDING);
	submits = urb_submit_calls;
	/* A recoverable terminal error is consumed in poll context and rearms
	 * the same object; USB completion itself never parses or reports. */
	fake_urb_complete(adapter->rx_urb, DRV_USB_URB_STALL, 0U);
	assert(scan_report_calls == 1U);
	assert(rtl8822bu_poll_receive(&fake_net_device, 1U) == 1U);
	assert(urb_submit_calls == submits + 1U);
	assert(clear_halt_calls == 1U);
	assert(adapter->rx_urb == persistent_rx_urb &&
	    adapter->rx_urb->status == DRV_USB_URB_PENDING);
	assert(fake_net_device.rx_errors == 1U);

	aggregate_length = make_beacon_aggregate(adapter->rx_buffer, 0x50U);
	fake_urb_complete(adapter->rx_urb, DRV_USB_URB_COMPLETE,
	    aggregate_length);
	assert(rtl8822bu_poll_receive(&fake_net_device, 1U) == 1U);
	assert(scan_report_calls == 2U);
	assert(adapter->rx_urb == persistent_rx_urb &&
	    adapter->rx_urb->status == DRV_USB_URB_PENDING);

	/* Shutdown closes the admission gate before cancellation.  The cancel
	 * callback therefore cannot schedule a late poll or rearm.  A fake
	 * already-admitted poll retires only after stop observes and joins it. */
	submits = poll_schedule_calls;
	adapter->polls_active = 1U;
	retiring_poll_adapter = adapter;
	yield_calls = 0U;
	yield_hook = retire_fake_poll;
	rtl8822bu_shutdown(&interface);
	assert(yield_calls != 0U && retiring_poll_adapter == NULL);
	assert(urb_cancel_calls == 1U && urb_drain_calls != 0U);
	assert(poll_schedule_calls == submits);
	assert(!adapter->opened && adapter->rx_urb->status ==
	    DRV_USB_URB_CANCELLED);
	assert(rtl8822bu_detach(&interface, 0U) == 0);
	assert(interface.driver_data == NULL);
	assert(allocations == 0U);

	/* A disconnect completion is terminal and is never retried before the
	 * USB detach gate catches up. */
	fake_transport_reset();
	net_created = 0U;
	net_gone = 0U;
	station_attached = 0U;
	station_detached = 0U;
	urb_submit_calls = 0U;
	urb_cancel_calls = 0U;
	urb_drain_calls = 0U;
	persistent_rx_urb = NULL;
	make_exact_interface(&device, &interface, endpoints);
	assert(rtl8822bu_attach(&interface, &rtl8822bu_ids[0]) == 0);
	adapter = interface.driver_data;
	assert(rtl8822bu_rx_start(adapter, 88U, 11U) == 0);
	submits = urb_submit_calls;
	fake_urb_complete(adapter->rx_urb, DRV_USB_URB_DISCONNECTED, 0U);
	assert(rtl8822bu_poll_receive(&fake_net_device, 1U) == 1U);
	assert(urb_submit_calls == submits && adapter->quarantined);
	assert(adapter->rx_urb->status == DRV_USB_URB_DISCONNECTED);
	assert(rtl8822bu_detach(&interface, 0U) == 0);
	assert(interface.driver_data == NULL);
	assert(allocations == 0U);
}

static void
test_endpoint_local_recovery_and_reopen(void)
{
	struct drv_usb_device device;
	struct drv_usb_interface interface;
	struct drv_usb_endpoint endpoints[5];
	struct drv_usb_endpoint storage_endpoint;
	struct drv_usb_urb storage_urb;
	struct rtl8822bu_adapter *adapter;
	struct wlan_bss_record bss;
	uint8_t storage_byte = 0U;
	uint64_t deadline;
	unsigned firmware_before;
	unsigned index;

	fake_transport_reset();
	make_exact_interface(&device, &interface, endpoints);
	assert(rtl8822bu_attach(&interface, &rtl8822bu_ids[0]) == 0);
	adapter = interface.driver_data;
	assert(fake_net_device.ops->open(&fake_net_device) == 0);
	assert(adapter->opened && adapter->radio_running &&
	    adapter->rx_urb->status == DRV_USB_URB_PENDING);

	/* Keep an unrelated storage request owned by its own endpoint throughout
	 * WLAN recovery.  A driver-local recovery must never reset or complete it. */
	memset(&storage_endpoint, 0, sizeof(storage_endpoint));
	storage_endpoint.descriptor.address = 0x82U;
	storage_endpoint.type = DRV_USB_TRANSFER_BULK;
	memset(&storage_urb, 0, sizeof(storage_urb));
	storage_urb.device = &device;
	storage_urb.endpoint = &storage_endpoint;
	storage_urb.status = DRV_USB_URB_IDLE;
	assert(drv_usb_urb_setup(&storage_urb, &storage_byte,
	    sizeof(storage_byte), 0U, 0U, NULL, NULL) == 0);
	assert(drv_usb_urb_submit(&storage_urb) == 0);
	/* A USB completion can be transport-successful while carrying a malformed
	 * RTL aggregate.  Payload parse success, not COMPLETE alone, owns the RX
	 * streak reset; repeated malformed payloads promote the same finite local
	 * recovery while the sibling storage request remains untouched. */
	firmware_before = firmware_load_calls;
	for (index = 0U; index < RTL8822BU_RX_RECOVERY_LIMIT; index++) {
		adapter->rx_buffer[0] = 0xffU;
		fake_urb_complete(adapter->rx_urb, DRV_USB_URB_COMPLETE, 1U);
		assert(rtl8822bu_poll_receive(&fake_net_device, 1U) == 1U);
		assert(storage_urb.status == DRV_USB_URB_PENDING);
	}
	assert(firmware_load_calls == firmware_before + 1U &&
	    adapter->rx_error_streak == 0U && !adapter->recovery_pending &&
	    adapter->rx_urb->status == DRV_USB_URB_PENDING);
	firmware_before = firmware_load_calls;
	for (index = 0U; index < RTL8822BU_RX_RECOVERY_LIMIT; index++) {
		fake_urb_complete(adapter->rx_urb, DRV_USB_URB_TIMEOUT, 0U);
		assert(rtl8822bu_poll_receive(&fake_net_device, 1U) == 1U);
		assert(storage_urb.status == DRV_USB_URB_PENDING);
	}
	/* The bounded third timeout closes carrier, retires the old graph, reloads
	 * the pinned firmware profile and leaves a fresh station/RX generation. */
	assert(firmware_load_calls == firmware_before + 1U);
	assert(station_close_calls == 0U && station_open_calls == 1U &&
	    link_loss_calls == 0U);
	assert(adapter->opened && adapter->radio_running &&
	    !adapter->quarantined && !adapter->recovery_pending &&
	    adapter->rx_error_streak == 0U &&
	    adapter->rx_urb->status == DRV_USB_URB_PENDING);
	assert(clear_halt_calls == 0U);
	/* The fresh hardware graph accepts normal scan/connect work immediately;
	 * no explicit down/up cycle is required after the local transaction. */
	deadline = clock_ticks() + 100U;
	assert(rtl8822bu_scan_channel_start(adapter, 940U, 0U, 6U,
	    deadline) == 0);
	assert(rtl8822bu_scan_stop(adapter, 940U) == 0);
	make_connection_bss(&bss);
	assert(rtl8822bu_connect_start(adapter, 940U, &bss, deadline) == 0);
	assert(rtl8822bu_disconnect(adapter, 940U) == 0);
	fake_urb_complete(&storage_urb, DRV_USB_URB_COMPLETE, 1U);

	/* IFF_DOWN after recovery proves the same checked inverse, and a new open
	 * loads firmware from scratch rather than reviving the retired generation. */
	firmware_before = firmware_load_calls;
	fake_net_device.ops->close(&fake_net_device);
	assert(!adapter->opened && adapter->radio.state == RTL8822B_RADIO_OFF);
	assert(fake_net_device.ops->open(&fake_net_device) == 0);
	assert(firmware_load_calls == firmware_before + 1U && adapter->opened);
	fake_net_device.ops->close(&fake_net_device);
	assert(rtl8822bu_detach(&interface, 0U) == 0);
	assert(interface.driver_data == NULL && allocations == 0U);

	/* A clear-halt failure is not retried globally.  It immediately takes the
	 * same local firmware transaction while preserving all sibling devices. */
	fake_transport_reset();
	make_exact_interface(&device, &interface, endpoints);
	assert(rtl8822bu_attach(&interface, &rtl8822bu_ids[0]) == 0);
	adapter = interface.driver_data;
	assert(fake_net_device.ops->open(&fake_net_device) == 0);
	make_connection_bss(&bss);
	deadline = clock_ticks() + 100U;
	assert(rtl8822bu_connect_start(adapter, 941U, &bss, deadline) == 0);
	firmware_before = firmware_load_calls;
	link_loss_error = EALREADY;
	clear_halt_error = EIO;
	fake_urb_complete(adapter->rx_urb, DRV_USB_URB_STALL, 0U);
	assert(rtl8822bu_poll_receive(&fake_net_device, 1U) == 1U);
	/* EALREADY/EBUSY mean common checked cleanup still owns state.  Hardware
	 * must remain live and untouched until a later worker retry returns zero. */
	assert(clear_halt_calls == 1U && firmware_load_calls == firmware_before);
	assert(link_loss_calls == 1U && link_loss_generation == 941U &&
	    link_loss_reason != 0);
	assert(adapter->recovery_pending && !adapter->recovery_active &&
	    adapter->opened && adapter->radio.state == RTL8822B_RADIO_STARTED);
	link_loss_error = EBUSY;
	assert(rtl8822bu_poll_receive(&fake_net_device, 1U) == 1U);
	assert(link_loss_calls == 2U && firmware_load_calls == firmware_before &&
	    adapter->recovery_pending && adapter->radio.state ==
	    RTL8822B_RADIO_STARTED);
	link_loss_error = 0;
	assert(rtl8822bu_poll_receive(&fake_net_device, 1U) == 1U);
	assert(link_loss_calls == 3U &&
	    firmware_load_calls == firmware_before + 1U);
	assert(adapter->opened && !adapter->quarantined &&
	    adapter->rx_urb->status == DRV_USB_URB_PENDING);
	assert(rtl8822bu_connect_start(adapter, 942U, &bss, deadline) == 0);
	assert(rtl8822bu_disconnect(adapter, 942U) == 0);
	fake_net_device.ops->close(&fake_net_device);
	assert(rtl8822bu_detach(&interface, 0U) == 0);
	assert(interface.driver_data == NULL && allocations == 0U);
}

static void
test_sync_endpoint_fault_recovery(void)
{
	struct drv_usb_device device;
	struct drv_usb_interface interface;
	struct drv_usb_endpoint endpoints[5];
	struct rtl8822bu_adapter *adapter;
	struct wlan_bss_record bss;
	struct wlan_radio_key_request key_request;
	struct wlan_radio_tx_request tx_request;
	uint8_t frame[96];
	uint8_t normal_wire[RTL8822B_DATA_TX_DESCRIPTOR_SIZE + sizeof(frame) + 1U];
	uint8_t probe[26];
	uint8_t value;
	uint64_t deadline;
	size_t actual, frame_length, normal_wire_length;
	unsigned before, firmware_before, index;

	fake_transport_reset();
	make_exact_interface(&device, &interface, endpoints);
	assert(rtl8822bu_attach(&interface, &rtl8822bu_ids[0]) == 0);
	adapter = interface.driver_data;
	assert(fake_net_device.ops->open(&fake_net_device) == 0);

	/* EP0 and bulk OUT each retry one local STALL.  A successful retry resets
	 * only that endpoint's streak and never resets the device/controller. */
	before = control_calls;
	control_stall_at = control_calls + 1U;
	control_stall_remaining = 1U;
	assert(rtl8822bu_read8(adapter, RTL8822BU_REG_CR, &value) == 0);
	assert(control_calls == before + 2U && adapter->control_error_streak == 0U);
	for (index = 0U; index < RTL8822BU_RX_RECOVERY_LIMIT; index++) {
		if (index == 1U) {
			control_short_at = control_calls + 1U;
			assert(rtl8822bu_read8(adapter, RTL8822BU_REG_CR,
			    &value) == EIO);
		} else {
			control_fail_at = control_calls + 1U;
			assert(rtl8822bu_read8(adapter, RTL8822BU_REG_CR,
			    &value) == ETIMEDOUT);
		}
	}
	assert(adapter->recovery_pending &&
	    adapter->control_error_streak == RTL8822BU_RX_RECOVERY_LIMIT);
	firmware_before = firmware_load_calls;
	assert(rtl8822bu_poll_receive(&fake_net_device, 1U) == 1U);
	assert(firmware_load_calls == firmware_before + 1U &&
	    !adapter->recovery_pending && adapter->control_error_streak == 0U);

	deadline = clock_ticks() + 100U;
	assert(rtl8822bu_scan_channel_start(adapter, 970U, 0U, 6U,
	    deadline) == 0);
	make_wildcard_probe(probe);
	before = clear_halt_calls;
	bulk_stall_at = bulk_calls + 1U;
	bulk_stall_remaining = 1U;
	assert(rtl8822bu_management_transmit(adapter, 970U, probe,
	    sizeof(probe), deadline) == 0);
	assert(clear_halt_calls == before + 1U &&
	    adapter->tx_high_error_streak == 0U);
	for (index = 0U; index < RTL8822BU_RX_RECOVERY_LIMIT; index++) {
		if (index == 1U) {
			bulk_short_at = bulk_calls + 1U;
			assert(rtl8822bu_management_transmit(adapter, 970U, probe,
			    sizeof(probe), deadline) == EIO);
		} else {
			bulk_fail_at = bulk_calls + 1U;
			assert(rtl8822bu_management_transmit(adapter, 970U, probe,
			    sizeof(probe), deadline) == ETIMEDOUT);
		}
	}
	bulk_short_at = 0U;
	assert(adapter->recovery_pending &&
	    adapter->tx_high_error_streak == RTL8822BU_RX_RECOVERY_LIMIT);
	firmware_before = firmware_load_calls;
	assert(rtl8822bu_poll_receive(&fake_net_device, 1U) == 1U);
	assert(firmware_load_calls == firmware_before + 1U &&
	    adapter->tx_high_error_streak == 0U);

	/* Queue 0 normally routes through the low pipe in the minimum profile,
	 * but the RTL USB interface owns a distinct normal OUT endpoint and streak.
	 * Exercise that production wrapper directly so a future queue mapping cannot
	 * inherit untested STALL/short-transfer behavior. */
	make_connection_bss(&bss);
	frame_length = make_station_frame(frame, WLAN_RADIO_FRAME_DATA, &bss,
	    0, 0U, 0U);
	assert(rtl8822b_data_frame_prepare(&adapter->radio, normal_wire,
	    sizeof(normal_wire), frame, frame_length, 0, 0U, 0U,
	    &normal_wire_length) == 0);
	route_data_to_normal_endpoint = 1U;
	before = clear_halt_calls;
	bulk_stall_at = bulk_calls + 1U;
	bulk_stall_remaining = 1U;
	assert(rtl8822bu_bulk_transfer(adapter, adapter->bulk_out_normal,
	    normal_wire, normal_wire_length, 20U, &actual) == 0);
	assert(actual == normal_wire_length && clear_halt_calls == before + 1U &&
	    adapter->tx_normal_error_streak == 0U);
	for (index = 0U; index < RTL8822BU_RX_RECOVERY_LIMIT; index++) {
		if (index == 1U) {
			bulk_short_at = bulk_calls + 1U;
			assert(rtl8822bu_bulk_transfer(adapter,
			    adapter->bulk_out_normal, normal_wire,
			    normal_wire_length, 20U, &actual) == EIO);
		} else {
			bulk_fail_at = bulk_calls + 1U;
			assert(rtl8822bu_bulk_transfer(adapter,
			    adapter->bulk_out_normal, normal_wire,
			    normal_wire_length, 20U, &actual) == ETIMEDOUT);
		}
	}
	bulk_short_at = 0U;
	route_data_to_normal_endpoint = 0U;
	assert(adapter->recovery_pending &&
	    adapter->tx_normal_error_streak == RTL8822BU_RX_RECOVERY_LIMIT);
	firmware_before = firmware_load_calls;
	assert(rtl8822bu_poll_receive(&fake_net_device, 1U) == 1U);
	assert(firmware_load_calls == firmware_before + 1U &&
	    adapter->tx_normal_error_streak == 0U);
	memset(normal_wire, 0, sizeof(normal_wire));

	/* The low data pipe owns an independent streak and recovery decision. */
	make_connection_bss(&bss);
	deadline = clock_ticks() + 100U;
	assert(rtl8822bu_connect_start(adapter, 971U, &bss, deadline) == 0);
	assert(rtl8822bu_association_set(adapter, 971U, bss.bssid, 11U,
	    deadline) == 0);
	memset(&key_request, 0, sizeof(key_request));
	key_request.generation = 971U;
	key_request.key_generation = 89U;
	key_request.deadline_ticks = deadline;
	key_request.kind = WLAN_RADIO_KEY_PAIRWISE;
	memcpy(key_request.address, bss.bssid, sizeof(key_request.address));
	memset(key_request.key, 0x79, sizeof(key_request.key));
	assert(rtl8822bu_key_install(adapter, &key_request) == 0);
	memset(&tx_request, 0, sizeof(tx_request));
	tx_request.generation = 971U;
	tx_request.deadline_ticks = deadline;
	tx_request.key_generation = 89U;
	tx_request.packet_number = 1U;
	tx_request.encrypted = 1U;
	tx_request.frame_class = WLAN_RADIO_FRAME_DATA;
	tx_request.frame = frame;
	tx_request.length = make_station_frame(frame, WLAN_RADIO_FRAME_DATA,
	    &bss, 1, 0U, tx_request.packet_number);
	before = clear_halt_calls;
	bulk_stall_at = bulk_calls + 1U;
	bulk_stall_remaining = 1U;
	tx_request.cookie = 0x6fffU;
	assert(rtl8822bu_frame_transmit(adapter, &tx_request) == 0);
	assert(clear_halt_calls == before + 1U &&
	    adapter->tx_low_error_streak == 0U);
	/* Retire the successful correlation before exercising ambiguous faults. */
	{
		struct rtl8822b_rx_packet report;
		struct rtl8822bu_rx_report_context context;
		uint8_t bytes[9];

		make_ccx_report(&report, bytes, last_tx_sequence, 0U);
		memset(&context, 0, sizeof(context));
		context.adapter = adapter;
		assert(rtl8822bu_rx_report(&context, &report) == 0);
	}
	for (index = 0U; index < RTL8822BU_RX_RECOVERY_LIMIT; index++) {
		tx_request.cookie = 0x7000U + index;
		if (index == 1U) {
			bulk_short_at = bulk_calls + 1U;
			assert(rtl8822bu_frame_transmit(adapter,
			    &tx_request) == EIO);
		} else {
			bulk_fail_at = bulk_calls + 1U;
			assert(rtl8822bu_frame_transmit(adapter,
			    &tx_request) == ETIMEDOUT);
		}
	}
	bulk_short_at = 0U;
	assert(adapter->recovery_pending &&
	    adapter->tx_low_error_streak == RTL8822BU_RX_RECOVERY_LIMIT);
	firmware_before = firmware_load_calls;
	assert(rtl8822bu_poll_receive(&fake_net_device, 1U) == 1U);
	assert(link_loss_calls == 1U && link_loss_generation == 971U &&
	    firmware_load_calls == firmware_before + 1U &&
	    adapter->tx_low_error_streak == 0U);
	fake_net_device.ops->close(&fake_net_device);
	assert(rtl8822bu_detach(&interface, 0U) == 0);
	assert(interface.driver_data == NULL && allocations == 0U);
}

static void
test_runtime_firmware_restart_failure(void)
{
	struct drv_usb_device device;
	struct drv_usb_interface interface;
	struct drv_usb_endpoint endpoints[5];
	struct drv_usb_endpoint storage_endpoint;
	struct drv_usb_urb storage_urb;
	struct rtl8822bu_adapter *adapter;
	uint8_t storage_byte = 0U;
	unsigned index, loads;

	fake_transport_reset();
	make_exact_interface(&device, &interface, endpoints);
	assert(rtl8822bu_attach(&interface, &rtl8822bu_ids[0]) == 0);
	adapter = interface.driver_data;
	assert(fake_net_device.ops->open(&fake_net_device) == 0);
	memset(&storage_endpoint, 0, sizeof(storage_endpoint));
	storage_endpoint.descriptor.address = 0x82U;
	storage_endpoint.type = DRV_USB_TRANSFER_BULK;
	memset(&storage_urb, 0, sizeof(storage_urb));
	storage_urb.device = &device;
	storage_urb.endpoint = &storage_endpoint;
	storage_urb.status = DRV_USB_URB_IDLE;
	assert(drv_usb_urb_setup(&storage_urb, &storage_byte,
	    sizeof(storage_byte), 0U, 0U, NULL, NULL) == 0);
	assert(drv_usb_urb_submit(&storage_urb) == 0);
	loads = firmware_load_calls;
	for (index = 0U; index < RTL8822BU_RX_RECOVERY_LIMIT; index++) {
		if (index + 1U == RTL8822BU_RX_RECOVERY_LIMIT)
			firmware_load_error = EIO;
		fake_urb_complete(adapter->rx_urb, DRV_USB_URB_TIMEOUT, 0U);
		assert(rtl8822bu_poll_receive(&fake_net_device, 1U) == 1U);
		assert(storage_urb.status == DRV_USB_URB_PENDING);
	}
	/* The failed firmware restart is bounded and fail-closed.  It neither
	 * resets the controller nor consumes the unrelated storage transaction. */
	assert(firmware_load_calls == loads + 1U && adapter->quarantined &&
	    !adapter->opened && !adapter->radio_running &&
	    adapter->radio.state == RTL8822B_RADIO_OFF &&
	    adapter->rx_urb->status != DRV_USB_URB_PENDING);
	firmware_load_error = 0;
	/* Explicit down/up is the finite retry boundary after restart itself
	 * failed; the same published interface must remain recoverable. */
	fake_net_device.ops->close(&fake_net_device);
	assert(fake_net_device.ops->open(&fake_net_device) == 0);
	assert(adapter->opened && !adapter->quarantined &&
	    adapter->rx_urb->status == DRV_USB_URB_PENDING);
	fake_urb_complete(&storage_urb, DRV_USB_URB_COMPLETE, 1U);
	fake_net_device.ops->close(&fake_net_device);
	assert(rtl8822bu_detach(&interface, 0U) == 0);
	assert(interface.driver_data == NULL && allocations == 0U);
}

static void
test_hundred_lifecycle_recovery_iterations(void)
{
	static const uint8_t broadcast[6] = {
		0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU
	};
	struct drv_usb_device device;
	struct drv_usb_interface interface;
	struct drv_usb_endpoint endpoints[5];
	struct drv_usb_endpoint storage_endpoint;
	struct drv_usb_urb storage_urb;
	struct rtl8822bu_adapter *adapter;
	struct rtl8822b_rx_packet report_packet;
	struct rtl8822bu_rx_report_context report_context;
	struct wlan_bss_record bss;
	struct wlan_radio_key_request key_request;
	uint8_t report_bytes[9];
	uint8_t probe[64];
	uint8_t storage_byte = 0U;
	uint8_t stale_sequence, live_sequence;
	uint64_t connection_generation, deadline, generation, key_generation;
	unsigned cycle, index, reports_before, slot;

	for (cycle = 0U; cycle < 100U; cycle++) {
		fake_transport_reset();
		make_exact_interface(&device, &interface, endpoints);
		assert(rtl8822bu_attach(&interface, &rtl8822bu_ids[0]) == 0);
		adapter = interface.driver_data;
		assert(fake_net_device.ops->open(&fake_net_device) == 0);
		memset(&storage_endpoint, 0, sizeof(storage_endpoint));
		storage_endpoint.descriptor.address = 0x82U;
		storage_endpoint.type = DRV_USB_TRANSFER_BULK;
		memset(&storage_urb, 0, sizeof(storage_urb));
		storage_urb.device = &device;
		storage_urb.endpoint = &storage_endpoint;
		storage_urb.status = DRV_USB_URB_IDLE;
		assert(drv_usb_urb_setup(&storage_urb, &storage_byte,
		    sizeof(storage_byte), 0U, 0U, NULL, NULL) == 0);
		assert(drv_usb_urb_submit(&storage_urb) == 0 &&
		    storage_progress == 1U);

		/* One ambiguous high-OUT completion becomes a CCX tombstone.  A
		 * following request uses a different sequence, and duplicate late old
		 * reports cannot complete its cookie. */
		generation = 2000U + cycle;
		deadline = clock_ticks() + 100U;
		assert(rtl8822bu_scan_channel_start(adapter, generation, 0U, 6U,
		    deadline) == 0);
		bulk_short_at = bulk_calls + 1U;
		assert(rtl8822bu_management_transmit(adapter, generation, probe,
		    make_directed_probe(probe, (const uint8_t *)"fixture", 7U),
		    deadline) == EIO);
		stale_sequence = last_tx_sequence;
		bulk_short_at = 0U;
		assert(rtl8822bu_management_transmit(adapter, generation, probe,
		    make_directed_probe(probe, (const uint8_t *)"fixture", 7U),
		    deadline) == 0);
		live_sequence = last_tx_sequence;
		assert(live_sequence != stale_sequence);
		memset(&report_context, 0, sizeof(report_context));
		report_context.adapter = adapter;
		reports_before = tx_report_calls;
		make_ccx_report(&report_packet, report_bytes, stale_sequence, 0U);
		assert(rtl8822bu_rx_report(&report_context, &report_packet) == 0);
		assert(rtl8822bu_rx_report(&report_context, &report_packet) == 0);
		assert(tx_report_calls == reports_before);
		make_ccx_report(&report_packet, report_bytes, live_sequence, 0U);
		assert(rtl8822bu_rx_report(&report_context, &report_packet) == 0);
		assert(tx_report_calls == reports_before + 1U);
		assert(rtl8822bu_scan_stop(adapter, generation) == 0);

		/* Build a complete live station graph before injecting the transport
		 * fault.  The link-loss callback below emulates the common station
		 * engine's checked inverse, so every cycle crosses association, pairwise
		 * and group-key ledgers rather than testing only an empty radio. */
		make_connection_bss(&bss);
		connection_generation = 3000U + cycle;
		key_generation = 4000U + cycle;
		deadline = clock_ticks() + 100U;
		assert(rtl8822bu_connect_start(adapter, connection_generation, &bss,
		    deadline) == 0);
		assert(rtl8822bu_association_set(adapter, connection_generation,
		    bss.bssid, (uint16_t)(cycle + 1U), deadline) == 0);
		memset(&key_request, 0, sizeof(key_request));
		key_request.generation = connection_generation;
		key_request.key_generation = key_generation;
		key_request.deadline_ticks = deadline;
		key_request.kind = WLAN_RADIO_KEY_PAIRWISE;
		memcpy(key_request.address, bss.bssid,
		    sizeof(key_request.address));
		memset(key_request.key, (int)(0x20U + cycle),
		    sizeof(key_request.key));
		assert(rtl8822bu_key_install(adapter, &key_request) == 0);
		key_request.kind = WLAN_RADIO_KEY_GROUP;
		key_request.key_index = 2U;
		key_request.receive_packet_number = cycle + 1U;
		memcpy(key_request.address, broadcast,
		    sizeof(key_request.address));
		memset(key_request.key, (int)(0x80U + cycle),
		    sizeof(key_request.key));
		assert(rtl8822bu_key_install(adapter, &key_request) == 0);
		assert(adapter->association_active &&
		    adapter->pairwise_key_installed &&
		    (adapter->group_key_mask & (1U << 2)) != 0U);
		link_loss_cleanup_adapter = adapter;
		link_loss_cleanup_pairwise_generation = key_generation;
		link_loss_cleanup_group_generation = key_generation;
		link_loss_cleanup_group_index = 2U;

		for (index = 0U; index < RTL8822BU_RX_RECOVERY_LIMIT; index++) {
			fake_urb_complete(adapter->rx_urb, DRV_USB_URB_TIMEOUT, 0U);
			assert(rtl8822bu_poll_receive(&fake_net_device, 1U) == 1U);
			assert(storage_urb.status == DRV_USB_URB_PENDING);
			if (index == 1U) {
				/* A sibling mass-storage completion and its next request make
				 * real progress while WLAN is between the first fault and the
				 * recovery threshold.  The replacement remains pending across
				 * the device-local firmware transaction. */
				fake_urb_complete(&storage_urb,
				    DRV_USB_URB_COMPLETE, 1U);
				assert(storage_urb.status == DRV_USB_URB_COMPLETE);
				assert(drv_usb_urb_setup(&storage_urb, &storage_byte,
				    sizeof(storage_byte), 0U, 0U, NULL, NULL) == 0);
				assert(drv_usb_urb_submit(&storage_urb) == 0 &&
				    storage_progress == 2U);
			}
		}
		assert(adapter->opened && !adapter->recovery_pending &&
		    !adapter->quarantined && adapter->rx_error_streak == 0U &&
		    adapter->control_error_streak == 0U &&
		    adapter->tx_high_error_streak == 0U &&
		    adapter->tx_normal_error_streak == 0U &&
		    adapter->tx_low_error_streak == 0U &&
		    adapter->tx_report_error_streak == 0U &&
		    adapter->tx_report_tombstone_count == 0U &&
		    adapter->connection_generation == 0U &&
		    adapter->rx_urb->status == DRV_USB_URB_PENDING);
		assert(link_loss_calls == 1U &&
		    link_loss_generation == connection_generation &&
		    link_loss_cleanup_adapter == NULL &&
		    !adapter->association_active &&
		    !adapter->pairwise_key_installed &&
		    adapter->group_key_mask == 0U &&
		    adapter->pairwise_key_generation == 0U &&
		    adapter->group_key_generation[2] == 0U);
		for (slot = 0U; slot < RTL8822BU_TX_REPORT_COUNT; slot++)
			assert(!adapter->tx_reports[slot].active &&
			    !adapter->tx_reports[slot].tombstone);
		for (slot = 0U; slot < RTL8822BU_CAM_OWNED_SLOT_COUNT; slot++)
			assert(fake_cam[slot][0] == 0U);
		/* Completion and resubmission also advance immediately after WLAN
		 * recovery; neither the endpoint-local reset nor common checked
		 * inverse owns this sibling URB. */
		fake_urb_complete(&storage_urb, DRV_USB_URB_COMPLETE, 1U);
		assert(drv_usb_urb_setup(&storage_urb, &storage_byte,
		    sizeof(storage_byte), 0U, 0U, NULL, NULL) == 0);
		assert(drv_usb_urb_submit(&storage_urb) == 0 &&
		    storage_progress == 3U);
		fake_urb_complete(&storage_urb, DRV_USB_URB_COMPLETE, 1U);
		fake_net_device.ops->close(&fake_net_device);
		assert(rtl8822bu_detach(&interface, 0U) == 0);
		assert(interface.driver_data == NULL && allocations == 0U);
	}
}

static void
test_attach_failure_unwind(void)
{
	struct drv_usb_device device;
	struct drv_usb_interface interface;
	struct drv_usb_endpoint endpoints[5];

	fake_transport_reset();
	net_created = 0U;
	net_gone = 0U;
	station_attached = 0U;
	station_detached = 0U;
	station_attach_error = EIO;
	make_exact_interface(&device, &interface, endpoints);
	assert(rtl8822bu_attach(&interface, &rtl8822bu_ids[0]) == EIO);
	assert(interface.driver_data == NULL);
	assert(net_gone && !station_attached && !station_detached);
	assert(allocations == 0U);
	station_attach_error = 0;
}

static void
test_network_shutdown_precedes_usb(void)
{
	struct drv_usb_device device;
	struct drv_usb_interface interface;
	struct drv_usb_endpoint endpoints[5];

	fake_transport_reset();
	net_created = 0U;
	net_gone = 0U;
	station_attached = 0U;
	station_detached = 0U;
	station_attach_error = 0;
	station_detach_error = ENODEV;
	make_exact_interface(&device, &interface, endpoints);
	assert(rtl8822bu_attach(&interface, &rtl8822bu_ids[0]) == 0);
	rtl8822bu_shutdown(&interface);
	assert(rtl8822bu_detach(&interface, 0U) == 0);
	assert(interface.driver_data == NULL);
	assert(station_detached && allocations == 0U);
	station_detach_error = 0;
}

static void
test_allocation_failure_unwind(void)
{
	struct drv_usb_device device;
	struct drv_usb_interface interface;
	struct drv_usb_endpoint endpoints[5];
	unsigned failure;

	for (failure = 1U; failure <= 5U; failure++) {
		assert(allocations == 0U);
		allocation_calls = 0U;
		allocation_fail_at = failure;
		fake_transport_reset();
		net_created = 0U;
		station_attach_error = 0;
		make_exact_interface(&device, &interface, endpoints);
		assert(rtl8822bu_attach(&interface, &rtl8822bu_ids[0]) ==
		    ENOMEM);
		assert(interface.driver_data == NULL);
		assert(allocations == 0U);
	}
	allocation_fail_at = 0U;
}

static void
test_attach_rollback_matrix(void)
{
	struct drv_usb_device device;
	struct drv_usb_interface interface;
	struct drv_usb_endpoint endpoints[5];

	/* A board-read error occurs after driver_data, RX buffer, and URB exist. */
	fake_transport_reset();
	control_fail_at = 1U;
	make_exact_interface(&device, &interface, endpoints);
	assert(rtl8822bu_attach(&interface, &rtl8822bu_ids[0]) == ETIMEDOUT);
	assert(interface.driver_data == NULL && !net_created);
	assert(allocations == 0U);

	/* A failed network publication owns no live name and is fully local. */
	fake_transport_reset();
	net_create_error = EIO;
	make_exact_interface(&device, &interface, endpoints);
	assert(rtl8822bu_attach(&interface, &rtl8822bu_ids[0]) == EIO);
	assert(interface.driver_data == NULL && !net_created && !net_gone);
	assert(allocations == 0U);

	/* If rollback itself is temporarily blocked, retain the complete graph
	 * for USB-core retry; never clear interface.driver_data prematurely. */
	fake_transport_reset();
	station_attach_error = EIO;
	net_gone_error = EBUSY;
	make_exact_interface(&device, &interface, endpoints);
	assert(rtl8822bu_attach(&interface, &rtl8822bu_ids[0]) == EBUSY);
	assert(interface.driver_data != NULL && net_gone_calls == 1U);
	net_gone_error = 0;
	assert(rtl8822bu_detach(&interface,
	    DRV_USB_DETACH_ATTACH_FAILED) == 0);
	assert(interface.driver_data == NULL && net_gone_calls == 2U);
	assert(allocations == 0U);
}

static void
test_checked_detach_retry(void)
{
	struct drv_usb_device device;
	struct drv_usb_interface interface;
	struct drv_usb_endpoint endpoints[5];

	fake_transport_reset();
	make_exact_interface(&device, &interface, endpoints);
	assert(rtl8822bu_attach(&interface, &rtl8822bu_ids[0]) == 0);
	station_detach_error = EBUSY;
	assert(rtl8822bu_detach(&interface, 0U) == EBUSY);
	assert(interface.driver_data != NULL);
	assert(net_gone_calls == 1U && station_detach_calls == 1U);
	station_detach_error = 0;
	assert(rtl8822bu_detach(&interface, 0U) == 0);
	/* net_device_gone succeeded before the first station failure and must not
	 * be repeated on retry. */
	assert(net_gone_calls == 1U && station_detach_calls == 2U);
	assert(interface.driver_data == NULL && allocations == 0U);
}

static void
test_usb_resources_retire_before_net_release(void)
{
	struct drv_usb_device device;
	struct drv_usb_interface interface;
	struct drv_usb_endpoint endpoints[5];
	struct rtl8822bu_adapter *adapter;

	fake_transport_reset();
	make_exact_interface(&device, &interface, endpoints);
	assert(rtl8822bu_attach(&interface, &rtl8822bu_ids[0]) == 0);
	adapter = interface.driver_data;
	assert(adapter != NULL && adapter->rx_urb != NULL &&
	    adapter->rx_buffer != NULL);
	defer_net_release = 1U;
	assert(rtl8822bu_detach(&interface, 0U) == 0);
	assert(interface.driver_data == NULL);
	assert(adapter->rx_urb == NULL && adapter->rx_buffer == NULL);
	assert(deferred_driver_data == adapter && allocations == 1U);
	fake_net_release_deferred();
	defer_net_release = 0U;
	assert(allocations == 0U);
}

struct lifecycle_thread_context {
	struct drv_usb_interface *interface;
	struct rtl8822bu_adapter *adapter;
	int result;
	unsigned done;
};

static void *
run_open_thread(void *argument)
{
	struct lifecycle_thread_context *context = argument;

	context->result = fake_net_device.ops->open(&fake_net_device);
	__atomic_store_n(&context->done, 1U, __ATOMIC_RELEASE);
	return NULL;
}

static void *
run_detach_thread(void *argument)
{
	struct lifecycle_thread_context *context = argument;

	fixture_thread_role = FIXTURE_ROLE_DETACH;
	context->result = rtl8822bu_detach(context->interface, 0U);
	__atomic_store_n(&context->done, 1U, __ATOMIC_RELEASE);
	return NULL;
}

static void *
run_close_thread(void *argument)
{
	struct lifecycle_thread_context *context = argument;

	fake_net_device.ops->close(&fake_net_device);
	context->result = 0;
	__atomic_store_n(&context->done, 1U, __ATOMIC_RELEASE);
	return NULL;
}

static void *
run_shutdown_thread(void *argument)
{
	struct lifecycle_thread_context *context = argument;

	rtl8822bu_shutdown(context->interface);
	context->result = 0;
	__atomic_store_n(&context->done, 1U, __ATOMIC_RELEASE);
	return NULL;
}

static void
fake_net_callback_enter(void)
{
	(void)__atomic_add_fetch(&net_callbacks_active, 1U, __ATOMIC_ACQ_REL);
}

static void
fake_net_callback_leave(void)
{
	assert(__atomic_fetch_sub(&net_callbacks_active, 1U,
	    __ATOMIC_ACQ_REL) != 0U);
}

static void *
run_ioctl_thread(void *argument)
{
	struct lifecycle_thread_context *context = argument;

	fake_net_callback_enter();
	context->result = fake_net_device.ops->ioctl(&fake_net_device, 0U, NULL);
	fake_net_callback_leave();
	__atomic_store_n(&context->done, 1U, __ATOMIC_RELEASE);
	return NULL;
}

static void *
run_poll_thread(void *argument)
{
	struct lifecycle_thread_context *context = argument;

	fake_net_callback_enter();
	context->result = (int)fake_net_device.ops->poll_receive(
	    &fake_net_device, 1U);
	fake_net_callback_leave();
	__atomic_store_n(&context->done, 1U, __ATOMIC_RELEASE);
	return NULL;
}

static void *
run_transmit_thread(void *argument)
{
	struct lifecycle_thread_context *context = argument;
	struct packet_buf *packet = malloc(sizeof(*packet));

	assert(packet != NULL);
	context->result = fake_net_device.ops->transmit(&fake_net_device, packet);
	__atomic_store_n(&context->done, 1U, __ATOMIC_RELEASE);
	return NULL;
}

static void *
run_start_thread(void *argument)
{
	struct lifecycle_thread_context *context = argument;

	fixture_thread_role = FIXTURE_ROLE_START;
	context->result = rtl8822bu_rx_start(context->adapter, 99U, 6U);
	__atomic_store_n(&context->done, 1U, __ATOMIC_RELEASE);
	return NULL;
}

static void *
run_stop_thread(void *argument)
{
	struct lifecycle_thread_context *context = argument;

	fixture_thread_role = FIXTURE_ROLE_STOP;
	context->result = rtl8822bu_rx_stop(context->adapter);
	__atomic_store_n(&context->done, 1U, __ATOMIC_RELEASE);
	return NULL;
}

static void
wait_for_atomic_nonzero(unsigned *value)
{
	unsigned count;

	for (count = 0U; count < 1000000U; count++) {
		if (__atomic_load_n(value, __ATOMIC_ACQUIRE) != 0U)
			return;
		sched_yield();
	}
	assert(!"timed out waiting for fixture thread");
}

static void
wait_for_adapter_stopping(struct rtl8822bu_adapter *adapter)
{
	unsigned count;

	for (count = 0U; count < 1000000U; count++) {
		unsigned long enabled = spin_lock_irqsave(&adapter->lock);
		unsigned stopping = adapter->stopping;

		spin_unlock_irqrestore(&adapter->lock, enabled);
		if (stopping)
			return;
		sched_yield();
	}
	assert(!"timed out waiting for RX stop gate");
}

static unsigned
adapter_starts_active(struct rtl8822bu_adapter *adapter)
{
	unsigned long enabled = spin_lock_irqsave(&adapter->lock);
	unsigned active = adapter->starts_active;

	spin_unlock_irqrestore(&adapter->lock, enabled);
	return active;
}

static void
test_open_detach_lifecycle_serialization(void)
{
	struct drv_usb_device device;
	struct drv_usb_interface interface;
	struct drv_usb_endpoint endpoints[5];
	struct lifecycle_thread_context open_context = {0};
	struct lifecycle_thread_context detach_context = {0};
	pthread_t open_thread, detach_thread;

	fake_transport_reset();
	make_exact_interface(&device, &interface, endpoints);
	assert(rtl8822bu_attach(&interface, &rtl8822bu_ids[0]) == 0);
	open_context.interface = &interface;
	detach_context.interface = &interface;
	__atomic_store_n(&threaded_test_active, 1U, __ATOMIC_RELEASE);
	__atomic_store_n(&carrier_block_once, 1U, __ATOMIC_RELEASE);
	assert(pthread_create(&open_thread, NULL, run_open_thread,
	    &open_context) == 0);
	wait_for_atomic_nonzero(&carrier_block_entered);
	assert(pthread_create(&detach_thread, NULL, run_detach_thread,
	    &detach_context) == 0);
	wait_for_atomic_nonzero(&mutex_contentions);
	assert(__atomic_load_n(&detach_context.done, __ATOMIC_ACQUIRE) == 0U);
	__atomic_store_n(&carrier_block_release, 1U, __ATOMIC_RELEASE);
	assert(pthread_join(open_thread, NULL) == 0);
	assert(pthread_join(detach_thread, NULL) == 0);
	__atomic_store_n(&threaded_test_active, 0U, __ATOMIC_RELEASE);
	assert(open_context.result == 0 && detach_context.result == 0);
	assert(interface.driver_data == NULL && allocations == 0U);
}

static void
test_transmit_detach_operation_lease(void)
{
	struct drv_usb_device device;
	struct drv_usb_interface interface;
	struct drv_usb_endpoint endpoints[5];
	struct rtl8822bu_adapter *adapter;
	struct lifecycle_thread_context transmit = {0}, detach = {0};
	pthread_t transmit_thread, detach_thread;

	fake_transport_reset();
	make_exact_interface(&device, &interface, endpoints);
	assert(rtl8822bu_attach(&interface, &rtl8822bu_ids[0]) == 0);
	adapter = interface.driver_data;
	assert(fake_net_device.ops->open(&fake_net_device) == 0);
	detach.interface = &interface;
	__atomic_store_n(&station_transmit_block_once, 1U, __ATOMIC_RELEASE);
	assert(pthread_create(&transmit_thread, NULL, run_transmit_thread,
	    &transmit) == 0);
	wait_for_atomic_nonzero(&station_transmit_block_entered);
	assert(pthread_create(&detach_thread, NULL, run_detach_thread,
	    &detach) == 0);
	wait_for_adapter_stopping(adapter);
	assert(__atomic_load_n(&detach.done, __ATOMIC_ACQUIRE) == 0U);
	__atomic_store_n(&station_transmit_block_release, 1U, __ATOMIC_RELEASE);
	assert(pthread_join(transmit_thread, NULL) == 0);
	assert(pthread_join(detach_thread, NULL) == 0);
	assert(transmit.result == 0 && detach.result == 0);
	assert(interface.driver_data == NULL && allocations == 0U);
}

static void
test_close_and_shutdown_detach_serialization(void)
{
	struct drv_usb_device device;
	struct drv_usb_interface interface;
	struct drv_usb_endpoint endpoints[5];
	struct lifecycle_thread_context operation = {0}, detach = {0};
	pthread_t operation_thread, detach_thread;

	/* close holds lifecycle_lock while changing RX ownership. */
	fake_transport_reset();
	make_exact_interface(&device, &interface, endpoints);
	assert(rtl8822bu_attach(&interface, &rtl8822bu_ids[0]) == 0);
	operation.interface = &interface;
	detach.interface = &interface;
	__atomic_store_n(&threaded_test_active, 1U, __ATOMIC_RELEASE);
	__atomic_store_n(&carrier_block_once, 1U, __ATOMIC_RELEASE);
	assert(pthread_create(&operation_thread, NULL, run_close_thread,
	    &operation) == 0);
	wait_for_atomic_nonzero(&carrier_block_entered);
	assert(pthread_create(&detach_thread, NULL, run_detach_thread,
	    &detach) == 0);
	wait_for_atomic_nonzero(&mutex_contentions);
	assert(__atomic_load_n(&detach.done, __ATOMIC_ACQUIRE) == 0U);
	__atomic_store_n(&carrier_block_release, 1U, __ATOMIC_RELEASE);
	assert(pthread_join(operation_thread, NULL) == 0);
	assert(pthread_join(detach_thread, NULL) == 0);
	__atomic_store_n(&threaded_test_active, 0U, __ATOMIC_RELEASE);
	assert(operation.result == 0 && detach.result == 0);
	assert(interface.driver_data == NULL && allocations == 0U);

	/* shutdown uses the same serialization domain. */
	memset(&operation, 0, sizeof(operation));
	memset(&detach, 0, sizeof(detach));
	fake_transport_reset();
	make_exact_interface(&device, &interface, endpoints);
	assert(rtl8822bu_attach(&interface, &rtl8822bu_ids[0]) == 0);
	operation.interface = &interface;
	detach.interface = &interface;
	__atomic_store_n(&threaded_test_active, 1U, __ATOMIC_RELEASE);
	__atomic_store_n(&carrier_block_once, 1U, __ATOMIC_RELEASE);
	assert(pthread_create(&operation_thread, NULL, run_shutdown_thread,
	    &operation) == 0);
	wait_for_atomic_nonzero(&carrier_block_entered);
	assert(pthread_create(&detach_thread, NULL, run_detach_thread,
	    &detach) == 0);
	wait_for_atomic_nonzero(&mutex_contentions);
	assert(__atomic_load_n(&detach.done, __ATOMIC_ACQUIRE) == 0U);
	__atomic_store_n(&carrier_block_release, 1U, __ATOMIC_RELEASE);
	assert(pthread_join(operation_thread, NULL) == 0);
	assert(pthread_join(detach_thread, NULL) == 0);
	__atomic_store_n(&threaded_test_active, 0U, __ATOMIC_RELEASE);
	assert(operation.result == 0 && detach.result == 0);
	assert(interface.driver_data == NULL && allocations == 0U);
}

static void
test_ioctl_and_poll_detach_join(void)
{
	struct drv_usb_device device;
	struct drv_usb_interface interface;
	struct drv_usb_endpoint endpoints[5];
	struct rtl8822bu_adapter *adapter;
	struct lifecycle_thread_context operation = {0}, detach = {0};
	pthread_t operation_thread, detach_thread;
	size_t aggregate_length;

	/* net_device_gone joins an already-admitted ioctl before station retire. */
	fake_transport_reset();
	make_exact_interface(&device, &interface, endpoints);
	assert(rtl8822bu_attach(&interface, &rtl8822bu_ids[0]) == 0);
	operation.interface = &interface;
	detach.interface = &interface;
	__atomic_store_n(&threaded_test_active, 1U, __ATOMIC_RELEASE);
	__atomic_store_n(&ioctl_block_once, 1U, __ATOMIC_RELEASE);
	assert(pthread_create(&operation_thread, NULL, run_ioctl_thread,
	    &operation) == 0);
	wait_for_atomic_nonzero(&ioctl_block_entered);
	assert(pthread_create(&detach_thread, NULL, run_detach_thread,
	    &detach) == 0);
	wait_for_atomic_nonzero(&net_join_waiting);
	assert(__atomic_load_n(&detach.done, __ATOMIC_ACQUIRE) == 0U);
	assert(station_detach_calls == 0U);
	__atomic_store_n(&ioctl_block_release, 1U, __ATOMIC_RELEASE);
	assert(pthread_join(operation_thread, NULL) == 0);
	assert(pthread_join(detach_thread, NULL) == 0);
	__atomic_store_n(&threaded_test_active, 0U, __ATOMIC_RELEASE);
	assert(operation.result == 77 && detach.result == 0);
	assert(interface.driver_data == NULL && allocations == 0U);

	/* The same terminal join covers an admitted poll while its station report
	 * is deliberately held. */
	memset(&operation, 0, sizeof(operation));
	memset(&detach, 0, sizeof(detach));
	fake_transport_reset();
	make_exact_interface(&device, &interface, endpoints);
	assert(rtl8822bu_attach(&interface, &rtl8822bu_ids[0]) == 0);
	adapter = interface.driver_data;
	assert(rtl8822bu_rx_start(adapter, 101U, 6U) == 0);
	aggregate_length = make_beacon_aggregate(adapter->rx_buffer, 0x80U);
	fake_urb_complete(adapter->rx_urb, DRV_USB_URB_COMPLETE,
	    aggregate_length);
	operation.interface = &interface;
	operation.adapter = adapter;
	detach.interface = &interface;
	__atomic_store_n(&threaded_test_active, 1U, __ATOMIC_RELEASE);
	__atomic_store_n(&report_block_once, 1U, __ATOMIC_RELEASE);
	assert(pthread_create(&operation_thread, NULL, run_poll_thread,
	    &operation) == 0);
	wait_for_atomic_nonzero(&report_block_entered);
	assert(pthread_create(&detach_thread, NULL, run_detach_thread,
	    &detach) == 0);
	wait_for_atomic_nonzero(&net_join_waiting);
	assert(__atomic_load_n(&detach.done, __ATOMIC_ACQUIRE) == 0U);
	assert(adapter_starts_active(adapter) == 0U);
	assert(station_detach_calls == 0U);
	__atomic_store_n(&report_block_release, 1U, __ATOMIC_RELEASE);
	assert(pthread_join(operation_thread, NULL) == 0);
	assert(pthread_join(detach_thread, NULL) == 0);
	__atomic_store_n(&threaded_test_active, 0U, __ATOMIC_RELEASE);
	assert(operation.result == 1 && detach.result == 0);
	assert(interface.driver_data == NULL && allocations == 0U);
}

static void
test_rx_start_stop_and_detach_ownership(void)
{
	struct drv_usb_device device;
	struct drv_usb_interface interface;
	struct drv_usb_endpoint endpoints[5];
	struct rtl8822bu_adapter *adapter;
	struct lifecycle_thread_context start = {0}, terminal = {0};
	pthread_t start_thread, terminal_thread;

	/* Hold the submit return and then the start-side status access.  stop must
	 * observe starts_active until cancel has completed. */
	fake_transport_reset();
	make_exact_interface(&device, &interface, endpoints);
	assert(rtl8822bu_attach(&interface, &rtl8822bu_ids[0]) == 0);
	adapter = interface.driver_data;
	start.adapter = adapter;
	terminal.adapter = adapter;
	__atomic_store_n(&threaded_test_active, 1U, __ATOMIC_RELEASE);
	__atomic_store_n(&rx_submit_block_once, 1U, __ATOMIC_RELEASE);
	__atomic_store_n(&rx_status_block_once, 1U, __ATOMIC_RELEASE);
	__atomic_store_n(&require_rx_post_before_free, 1U, __ATOMIC_RELEASE);
	assert(pthread_create(&start_thread, NULL, run_start_thread, &start) == 0);
	wait_for_atomic_nonzero(&rx_submit_block_entered);
	assert(pthread_create(&terminal_thread, NULL, run_stop_thread,
	    &terminal) == 0);
	wait_for_adapter_stopping(adapter);
	__atomic_store_n(&rx_submit_block_release, 1U, __ATOMIC_RELEASE);
	wait_for_atomic_nonzero(&rx_status_block_entered);
	assert(adapter_starts_active(adapter) == 1U);
	assert(__atomic_load_n(&terminal.done, __ATOMIC_ACQUIRE) == 0U);
	__atomic_store_n(&rx_status_block_release, 1U, __ATOMIC_RELEASE);
	assert(pthread_join(start_thread, NULL) == 0);
	assert(pthread_join(terminal_thread, NULL) == 0);
	assert(start.result == ENETDOWN && terminal.result == 0);
	assert(__atomic_load_n(&rx_start_post_access_complete,
	    __ATOMIC_ACQUIRE) != 0U);
	assert(rtl8822bu_detach(&interface, 0U) == 0);
	__atomic_store_n(&threaded_test_active, 0U, __ATOMIC_RELEASE);
	assert(interface.driver_data == NULL && allocations == 0U);

	/* Repeat with full detach as the terminal owner.  It must neither clear
	 * driver_data nor free the URB while start is held in post-submit status. */
	memset(&start, 0, sizeof(start));
	memset(&terminal, 0, sizeof(terminal));
	fake_transport_reset();
	make_exact_interface(&device, &interface, endpoints);
	assert(rtl8822bu_attach(&interface, &rtl8822bu_ids[0]) == 0);
	adapter = interface.driver_data;
	start.adapter = adapter;
	terminal.adapter = adapter;
	terminal.interface = &interface;
	__atomic_store_n(&threaded_test_active, 1U, __ATOMIC_RELEASE);
	__atomic_store_n(&rx_submit_block_once, 1U, __ATOMIC_RELEASE);
	__atomic_store_n(&rx_status_block_once, 1U, __ATOMIC_RELEASE);
	__atomic_store_n(&require_rx_post_before_free, 1U, __ATOMIC_RELEASE);
	open_count_during_gone = 1U;
	assert(pthread_create(&start_thread, NULL, run_start_thread, &start) == 0);
	wait_for_atomic_nonzero(&rx_submit_block_entered);
	assert(pthread_create(&terminal_thread, NULL, run_detach_thread,
	    &terminal) == 0);
	wait_for_adapter_stopping(adapter);
	__atomic_store_n(&rx_submit_block_release, 1U, __ATOMIC_RELEASE);
	wait_for_atomic_nonzero(&rx_status_block_entered);
	assert(adapter_starts_active(adapter) == 1U);
	assert(__atomic_load_n(&terminal.done, __ATOMIC_ACQUIRE) == 0U);
	assert(interface.driver_data == adapter && adapter->rx_urb != NULL);
	__atomic_store_n(&rx_status_block_release, 1U, __ATOMIC_RELEASE);
	assert(pthread_join(start_thread, NULL) == 0);
	assert(pthread_join(terminal_thread, NULL) == 0);
	__atomic_store_n(&threaded_test_active, 0U, __ATOMIC_RELEASE);
	assert(start.result == ENETDOWN && terminal.result == 0);
	assert(interface.driver_data == NULL && allocations == 0U);
}

int
main(void)
{
	test_exact_match();
	test_register_transport();
	test_radio_delay_resolution();
	test_efuse_cleanup();
	test_firmware_transport();
	test_attach_open_detach();
	test_first_open_failure_unwind();
	test_first_open_software_scan();
	test_w52_scan_and_connect();
	test_scan_report_stall_recovery();
	test_attempted_tx_report_tombstones();
	test_secure_station_hardware_contract();
	test_cam_rekey_generation_activation();
	test_cam_activation_failure_quarantine();
	test_active_connection_terminal_cleanup();
	test_force_unplug_and_fresh_reinsert();
	test_tx_quiesce_queue_barrier();
	test_scan_channel_transport_failure_quarantine();
	test_close_finite_station_join();
	test_rx_poll_rearm_and_lifecycle();
	test_endpoint_local_recovery_and_reopen();
	test_sync_endpoint_fault_recovery();
	test_runtime_firmware_restart_failure();
	test_hundred_lifecycle_recovery_iterations();
	test_attach_failure_unwind();
	test_network_shutdown_precedes_usb();
	test_allocation_failure_unwind();
	test_attach_rollback_matrix();
	test_checked_detach_retry();
	test_usb_resources_retire_before_net_release();
	test_open_detach_lifecycle_serialization();
	test_transmit_detach_operation_lease();
	test_close_and_shutdown_detach_serialization();
	test_ioctl_and_poll_detach_join();
	test_rx_start_stop_and_detach_ownership();
	puts("usb rtl8822bu driver: PASS");
	return 0;
}
