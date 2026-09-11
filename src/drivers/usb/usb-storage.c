/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * USB Mass Storage Bulk-Only Transport and minimal SCSI disk driver
 */

#include <drivers/usb-storage.h>
#include <drivers/usb-storage-bot.h>
#include <drivers/usb-storage-scsi.h>
#include <drivers/usb.h>
#include <errno.h>
#include <hal/hal.h>
#include <kern/disk.h>
#include <kern/partition.h>
#include <kern/io-stats.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <string.h>

#define USB_MASS_STORAGE_CLASS 0x08U
#define USB_MASS_STORAGE_SCSI 0x06U
#define USB_MASS_STORAGE_BULK_ONLY 0x50U
#define USB_MASS_STORAGE_RESET 0xffU
#define USB_MASS_STORAGE_GET_MAX_LUN 0xfeU

#define BOT_CBW_SIGNATURE 0x43425355U
#define BOT_CSW_SIGNATURE 0x53425355U
#define BOT_DIRECTION_IN 0x80U
#define BOT_TIMEOUT_MS 5000U

#define SCSI_TEST_UNIT_READY 0x00U
#define SCSI_REQUEST_SENSE 0x03U
#define SCSI_INQUIRY 0x12U
#define SCSI_READ_CAPACITY_10 0x25U
#define SCSI_READ_10 0x28U
#define SCSI_SYNCHRONIZE_CACHE_10 0x35U

struct bot_cbw {
	uint8_t signature[4];
	uint8_t tag[4];
	uint8_t transfer_length[4];
	uint8_t flags;
	uint8_t lun;
	uint8_t command_length;
	uint8_t command[16];
} __attribute__((packed));

struct bot_csw {
	uint8_t signature[4];
	uint8_t tag[4];
	uint8_t residue[4];
	uint8_t status;
} __attribute__((packed));

struct storage_read_checkpoint {
	uint32_t lba;
	uint16_t blocks;
};

enum storage_media_state {
	STORAGE_ONLINE,
	STORAGE_RECONFIGURE,
	STORAGE_REVALIDATE,
	STORAGE_ABSENT,
	STORAGE_FAILED
};

struct usb_storage {
	struct drv_usb_interface *interface;
	struct drv_usb_device *device;
	struct drv_usb_endpoint *bulk_in;
	struct drv_usb_endpoint *bulk_out;
	struct drv_usb_urb *control_urb;
	struct drv_usb_urb *bulk_in_urb;
	struct drv_usb_urb *bulk_out_urb;
	struct disk *disk;
	struct mutex lock;
	struct mutex control_lock;
	struct thread *control_worker;
	volatile unsigned control_ready;
	volatile unsigned control_stopping;
	uint32_t next_tag;
	uint32_t block_size;
	size_t transfer_size;
	uint64_t block_count;
	uint8_t lun;
	uint8_t write_protected;
	uint8_t cache_known;
	uint8_t write_cache_enabled;
	uint8_t dpofua;
	int flush_error;
	int transport_error;
	enum storage_media_state media_state;
	struct drv_usb_scsi_sense last_sense;
	unsigned media_retired;
	unsigned partitions_pending;
	uint64_t command_deadline;
	enum drv_usb_scsi_flush_policy flush_policy;
#ifdef ZEDBSD_TEST_CHECKPOINTS
	unsigned checkpoint_read_sequence;
#endif
};

/*
 * Forward declaration
 */
static uint32_t get_le32(const uint8_t value[4]);
static void put_le32(uint8_t value[4], uint32_t number);
static uint32_t get_be32(const uint8_t value[4]);
static uint16_t get_be16(const uint8_t value[2]);
static void put_be32(uint8_t value[4], uint32_t number);
static void put_be16(uint8_t value[2], uint16_t number);
static unsigned storage_timeout(struct usb_storage *storage, unsigned timeout);
static int storage_urb_transfer(struct usb_storage *storage, struct drv_usb_urb *urb, void *buffer, size_t length, unsigned timeout, size_t *actual, const struct storage_read_checkpoint *checkpoint);
static int storage_bulk(struct usb_storage *storage, struct drv_usb_endpoint *endpoint, void *buffer, size_t length, unsigned timeout, size_t *actual, const struct storage_read_checkpoint *checkpoint);
static int storage_control(struct usb_storage *storage, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index, void *buffer, size_t length, unsigned timeout, size_t *actual);
static int storage_urbs_alloc(struct usb_storage *storage);
static int storage_transfer_reserve(struct usb_storage *storage);
static void storage_urbs_free(struct usb_storage *storage);
static int bot_reset(struct usb_storage *storage);
static int bot_command_locked(struct usb_storage *storage, const void *cdb, size_t cdb_length, void *buffer, size_t length, int input, size_t *transferred, int *command_failed, int report_command_failed);
static int request_sense_locked(struct usb_storage *storage, struct drv_usb_scsi_sense *decoded);
static int storage_reconfigure_locked(struct usb_storage *storage);
static int bot_command_sense_locked(struct usb_storage *storage, const void *cdb, size_t cdb_length, void *buffer, size_t length, int input, struct drv_usb_scsi_sense *sense, size_t *transferred, int report_command_failed);
static int bot_command_sense_report(struct usb_storage *storage, const void *cdb, size_t cdb_length, void *buffer, size_t length, int input, struct drv_usb_scsi_sense *sense, size_t *transferred, int report_command_failed);
static int bot_command_sense(struct usb_storage *storage, const void *cdb, size_t cdb_length, void *buffer, size_t length, int input, struct drv_usb_scsi_sense *sense, size_t *transferred);
static int bot_command(struct usb_storage *storage, const void *cdb, size_t cdb_length, void *buffer, size_t length, int input, size_t *transferred);
static const char *flush_policy_name(enum drv_usb_scsi_flush_policy policy);
static void scsi_configure_flush_policy(struct usb_storage *storage);
static int scsi_probe(struct usb_storage *storage, int *medium_absent);
static int storage_submit(struct disk *disk, struct bio *bio);
static int storage_ioctl(struct disk *disk, unsigned long request, void *argument);
static int storage_publish_disk(struct usb_storage *storage);
static int storage_refresh_partitions(struct usb_storage *storage);
static int storage_control_step(struct usb_storage *storage);
static void storage_control_worker(void *argument);
static int storage_control_start(struct usb_storage *storage);
static int storage_control_stop(struct usb_storage *storage);
static int storage_attach(struct drv_usb_interface *interface, const struct drv_usb_id *id);
static int storage_detach(struct drv_usb_interface *interface, unsigned flags);
static int storage_quiesce(struct drv_usb_interface *interface);

/* Device operation and registration tables. */
static const struct disk_ops storage_disk_ops = {
	.submit = storage_submit,
	.ioctl = storage_ioctl
};

static const struct drv_usb_id storage_ids[] = {
	{
		.match_flags = DRV_USB_ID_IF_CLASS | DRV_USB_ID_IF_SUBCLASS | DRV_USB_ID_IF_PROTOCOL,
		.interface_class = USB_MASS_STORAGE_CLASS,
		.interface_subclass = USB_MASS_STORAGE_SCSI,
		.interface_protocol = USB_MASS_STORAGE_BULK_ONLY
	}
};

static struct drv_usb_driver storage_driver = {
	.name = "usb-storage",
	.ids = storage_ids,
	.id_count = sizeof(storage_ids) / sizeof(storage_ids[0]),
	.attach = storage_attach,
	.quiesce = storage_quiesce,
	.detach = storage_detach
};

/*
 * Registers this driver with the USB subsystem.
 */
int
drv_usb_storage_driver_register(
	void)
{
	int error;

	/* Obtains the drv usb driver register result. */
	error = drv_usb_driver_register(&storage_driver);

	/* Returns the computed result. */
	return error;
}

/* Reads a 32-bit field, least significant byte first. */
static uint32_t
get_le32(
	const uint8_t value[4])
{
	/* Returns the computed result. */
	return (uint32_t)value[0] | ((uint32_t)value[1] << 8) |
	       ((uint32_t)value[2] << 16) | ((uint32_t)value[3] << 24);
}

/* Writes a 32-bit field, least significant byte first. */
static void
put_le32(
	uint8_t value[4],
	uint32_t number)
{
	value[0] = (uint8_t)number;
	value[1] = (uint8_t)(number >> 8);
	value[2] = (uint8_t)(number >> 16);
	value[3] = (uint8_t)(number >> 24);
}

/* Reads a 32-bit field, most significant byte first. */
static uint32_t
get_be32(
	const uint8_t value[4])
{
	/* Returns the computed result. */
	return ((uint32_t)value[0] << 24) | ((uint32_t)value[1] << 16) |
	       ((uint32_t)value[2] << 8) | value[3];
}

/* Reads a 16-bit field, most significant byte first. */
static uint16_t
get_be16(
	const uint8_t value[2])
{
	/* Returns the computed result. */
	return (uint16_t)((uint16_t)value[0] << 8) | value[1];
}

/* Writes a 32-bit field, most significant byte first. */
static void
put_be32(
	uint8_t value[4],
	uint32_t number)
{
	value[0] = (uint8_t)(number >> 24);
	value[1] = (uint8_t)(number >> 16);
	value[2] = (uint8_t)(number >> 8);
	value[3] = (uint8_t)number;
}

/* Writes a 16-bit field, most significant byte first. */
static void
put_be16(
	uint8_t value[2],
	uint16_t number)
{
	value[0] = (uint8_t)(number >> 8);
	value[1] = (uint8_t)number;
}

/* One command budget includes its recovery and REQUEST SENSE. Ownership retirement has its own bounded grace in the USB core. */
static unsigned
storage_timeout(
	struct usb_storage *storage,
	unsigned timeout)
{
	uint64_t now, remaining;

	/* Handles the storage condition. */
	if (storage->command_deadline == 0)
		return timeout;

	/* Handles the now condition. */
	now = sched_ticks();
	if (now >= storage->command_deadline)
		return 0;
	remaining = (storage->command_deadline - now) * 10U;

	/* Returns the computed result. */
	return remaining < timeout ? (unsigned)remaining : timeout;
}

/* USB storage can back swap.  Allocate its synchronous URBs while the device is attached rather than while reclaim is trying to create a free page. The storage mutex serializes every reuse of these three endpoint-specific objects. */
static int
storage_urb_transfer(
	struct usb_storage *storage,
	struct drv_usb_urb *urb,
	void *buffer,
	size_t length,
	unsigned timeout,
	size_t *actual,
	const struct storage_read_checkpoint *checkpoint)
{
	unsigned flags = length <= DRV_USB_URB_RECLAIM_SAFE_MAX_SIZE
				 ? DRV_USB_URB_RECLAIM_SAFE
				 : 0;
	int error;
#ifdef ZEDBSD_TEST_CHECKPOINTS
	unsigned checkpoint_sequence = 0;

#else
	(void)checkpoint;
#endif

	/* Handles the actual availability. */
	if (actual != NULL)
		*actual = 0;

	/* Handles the timeout condition. */
	timeout = storage_timeout(storage, timeout);
	if (timeout == 0)
		return ETIMEDOUT;

	/* Checks the operation status. */
	error = drv_usb_urb_setup(urb, buffer, length, flags, timeout, NULL,
				  NULL);
	if (error == 0)
		error = drv_usb_urb_submit(urb);
#ifdef ZEDBSD_TEST_CHECKPOINTS

	/*
	 * This marker is intentionally emitted only after the HCD accepted the
	 * READ(10) data URB and while the USB core still publishes it as
	 * pending. HW-T25 waits for it before injecting the concurrent HID
	 * completion.
	 */
	if (error == 0 && checkpoint != NULL &&
	    drv_usb_urb_status(urb) == DRV_USB_URB_PENDING) {
		checkpoint_sequence = ++storage->checkpoint_read_sequence;
		hal_printf(
			"usb-storage-checkpoint: accepted disk=%s "
			"generation=%u "
			"usb%u device=%u lba=%u blocks=%u bytes=%u "
			"status=pending\n",
			storage->disk != NULL ? storage->disk->d_name
					      : "unpublished",
			checkpoint_sequence,
			drv_usb_bus_number(drv_usb_device_bus(storage->device)),
			drv_usb_device_address(storage->device),
			checkpoint->lba, (unsigned)checkpoint->blocks,
			(unsigned)length);
	}

#endif

	/* Checks the operation status. */
	if (error == 0)
		error = drv_usb_urb_wait_reusable(urb);
#ifdef ZEDBSD_TEST_CHECKPOINTS

	/* Handles the checkpoint sequence condition. */
	if (checkpoint_sequence != 0) {
		hal_printf(
			"usb-storage-checkpoint: completed disk=%s "
			"generation=%u "
			"usb%u device=%u lba=%u blocks=%u bytes=%u status=%u "
			"actual=%u error=%d\n",
			storage->disk != NULL ? storage->disk->d_name
					      : "unpublished",
			checkpoint_sequence,
			drv_usb_bus_number(drv_usb_device_bus(storage->device)),
			drv_usb_device_address(storage->device),
			checkpoint->lba, (unsigned)checkpoint->blocks,
			(unsigned)length, (unsigned)drv_usb_urb_status(urb),
			(unsigned)drv_usb_urb_actual_length(urb), error);
	}

#endif

	/* Handles the actual availability. */
	if (actual != NULL)
		*actual = drv_usb_urb_actual_length(urb);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Runs one bulk transfer against the device. */
static int
storage_bulk(
	struct usb_storage *storage,
	struct drv_usb_endpoint *endpoint,
	void *buffer,
	size_t length,
	unsigned timeout,
	size_t *actual,
	const struct storage_read_checkpoint *checkpoint)
{
	int error;
	struct drv_usb_urb *urb;

	/* Handles the endpoint condition. */
	if (endpoint == storage->bulk_in)
		urb = storage->bulk_in_urb;
	else if (endpoint == storage->bulk_out)
		urb = storage->bulk_out_urb;
	else {
		/* Failed. */
		return EINVAL;
	}

	/* Obtains the storage urb transfer result. */
	error = storage_urb_transfer(storage, urb, buffer, length,
					       timeout, actual, checkpoint);

	/* Returns the computed result. */
	return error;
}

/* Runs one control transfer against the device. */
static int
storage_control(
	struct usb_storage *storage,
	uint8_t request_type,
	uint8_t request,
	uint16_t value,
	uint16_t index,
	void *buffer,
	size_t length,
	unsigned timeout,
	size_t *actual)
{
	uint64_t now;
	uint64_t ticks;
	struct drv_usb_control_request control = {request_type, request, value,
						  index, (uint16_t)length};
	unsigned flags = length <= DRV_USB_URB_RECLAIM_SAFE_MAX_SIZE
				 ? DRV_USB_URB_RECLAIM_SAFE
				 : 0;
	uint64_t deadline = 0;
	int error;

	/* Handles the timeout condition. */
	timeout = storage_timeout(storage, timeout);
	if (timeout == 0)
		return ETIMEDOUT;

	/* Checks the current data length. */
	if (length > UINT16_MAX)
		return EINVAL;

	/* Handles the actual availability. */
	if (actual != NULL)
		*actual = 0;
	/* Handles the timeout condition. */
	if (timeout != 0) {
		now = sched_ticks();
		ticks = ((uint64_t)timeout + 9U) / 10U;

		deadline = UINT64_MAX - now < ticks ? UINT64_MAX : now + ticks;
	}

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Checks the operation status. */
		error = drv_usb_urb_setup_control_flags(
			storage->control_urb, &control, buffer, length, flags,
			timeout, NULL, NULL);
		if (error == 0)
			error = drv_usb_urb_submit(storage->control_urb);
		if (error != EBUSY || deadline == 0)
			break;

		/* Checks the sched ticks result. */
		if (sched_ticks() >= deadline) {
			error = ETIMEDOUT;
			break;
		}

		sched_yield();
	}

	/* Checks the operation status. */
	if (error == 0)
		error = drv_usb_urb_wait_reusable(storage->control_urb);

	/* Handles the actual availability. */
	if (actual != NULL)
		*actual = drv_usb_urb_actual_length(storage->control_urb);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Takes the transfers this device needs. */
static int
storage_urbs_alloc(
	struct usb_storage *storage)
{
	storage->control_urb = drv_usb_urb_alloc(storage->device, NULL, 0);
	storage->bulk_in_urb =
		drv_usb_urb_alloc(storage->device, storage->bulk_in, 0);
	storage->bulk_out_urb =
		drv_usb_urb_alloc(storage->device, storage->bulk_out, 0);

	/* Checks the drv usb urb reserve sync result. */
	if (storage->control_urb != NULL && storage->bulk_in_urb != NULL &&
	    storage->bulk_out_urb != NULL &&
	    drv_usb_urb_reserve_sync(storage->control_urb,
				     DRV_USB_URB_RECLAIM_SAFE_MAX_SIZE) == 0 &&
	    drv_usb_urb_reserve_sync(storage->bulk_in_urb,
				     DRV_USB_URB_RECLAIM_SAFE_MAX_SIZE) == 0 &&
	    drv_usb_urb_reserve_sync(storage->bulk_out_urb,
				     DRV_USB_URB_RECLAIM_SAFE_MAX_SIZE) == 0) {
		/* Succeeded. */
		return 0;
	}
	drv_usb_urb_free(storage->bulk_out_urb);
	drv_usb_urb_free(storage->bulk_in_urb);
	drv_usb_urb_free(storage->control_urb);
	storage->bulk_out_urb = NULL;
	storage->bulk_in_urb = NULL;
	storage->control_urb = NULL;

	/* Failed. */
	return ENOMEM;
}

/* Reserves ordinary transfers separately from the controller's reclaim reserve. */
static int
storage_transfer_reserve(
	struct usb_storage *storage)
{
	int error;

	/*
	 * Keeps the established byte limit on controllers without the paired
	 * callbacks.
	 */
	storage->transfer_size = DRV_USB_URB_RECLAIM_SAFE_MAX_SIZE;

	/* Checks the drv usb device hcd capabilities result. */
	if (!(drv_usb_device_hcd_capabilities(storage->device) &
	      DRV_USB_HCD_CAP_TRANSFER_RESERVE)) {
		/* Succeeded. */
		return 0;
	}

	/*
	 * Leaves unusually large logical sectors on their existing one-block
	 * path.
	 */
	if (storage->block_size > DRV_USB_TRANSFER_RESERVE_MAX_SIZE)
		return 0;

	/* Checks the operation status. */
	error = drv_usb_urb_reserve_transfer(storage->control_urb,
					     DRV_USB_URB_RECLAIM_SAFE_MAX_SIZE);
	if (error != 0)
		return error;

	/* Checks the operation status. */
	error = drv_usb_urb_reserve_transfer(storage->bulk_in_urb,
					     DRV_USB_TRANSFER_RESERVE_MAX_SIZE);
	if (error != 0)
		return error;

	/* Checks the operation status. */
	error = drv_usb_urb_reserve_transfer(storage->bulk_out_urb,
					     DRV_USB_TRANSFER_RESERVE_MAX_SIZE);
	if (error != 0)
		return error;
	storage->transfer_size = DRV_USB_TRANSFER_RESERVE_MAX_SIZE;

	/*
	 * Publishes the effective limit only after every reservation is ready.
	 */
	return 0;
}

/* Gives them back. */
static void
storage_urbs_free(
	struct usb_storage *storage)
{
	drv_usb_urb_free(storage->bulk_out_urb);
	drv_usb_urb_free(storage->bulk_in_urb);
	drv_usb_urb_free(storage->control_urb);
	storage->bulk_out_urb = NULL;
	storage->bulk_in_urb = NULL;
	storage->control_urb = NULL;
}

/* Resets the device the way the transport defines. */
static int
bot_reset(
	struct usb_storage *storage)
{
	size_t actual = 0;
	int error;

	/* Drops what was cached and resets the interface. */
	disk_persistence_forget(storage->disk);

	/* Checks the operation status. */
	error = storage_control(storage,
				DRV_USB_DIR_OUT | DRV_USB_REQUEST_CLASS |
					DRV_USB_RECIP_INTERFACE,
				USB_MASS_STORAGE_RESET, 0,
				drv_usb_interface_number(storage->interface),
				NULL, 0, 1000U, &actual);
	if (error != 0)
		return error;

	/* Checks the operation status. */
	error = drv_usb_endpoint_clear_halt(storage->bulk_in);
	if (error == 0)
		error = drv_usb_endpoint_clear_halt(storage->bulk_out);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Runs one command, with the device lock held. */
static int
bot_command_locked(
	struct usb_storage *storage,
	const void *cdb,
	size_t cdb_length,
	void *buffer,
	size_t length,
	int input,
	size_t *transferred,
	int *command_failed,
	int report_command_failed)
{
	int halt_error;
	struct drv_usb_endpoint *endpoint;
	int data_stalled;
	struct bot_cbw cbw;
	struct bot_csw csw;
	struct storage_read_checkpoint read_checkpoint;
	const struct storage_read_checkpoint *checkpoint = NULL;
	enum drv_usb_bot_csw_result csw_result;
	uint32_t residue, tag;
	size_t actual, data_actual = 0, processed;
	int error;

	storage->transport_error = 0;

	/* Handles the transferred availability. */
	if (transferred != NULL)
		*transferred = 0;
	/* Checks the operation status. */
	if (command_failed != NULL)
		*command_failed = 0;
	/* Handles the cdb availability. */
	if (cdb == NULL || cdb_length == 0 ||
	    cdb_length > sizeof(cbw.command) || length > UINT32_MAX) {
		/* Failed. */
		return EINVAL;
	}
	memset(&cbw, 0, sizeof(cbw));
	memset(&csw, 0, sizeof(csw));
	put_le32(cbw.signature, BOT_CBW_SIGNATURE);

	/* Handles the tag condition. */
	tag = ++storage->next_tag;
	if (tag == 0)
		tag = ++storage->next_tag;
	put_le32(cbw.tag, tag);
	put_le32(cbw.transfer_length, (uint32_t)length);
	cbw.flags = input ? BOT_DIRECTION_IN : 0;
	cbw.lun = storage->lun;
	cbw.command_length = (uint8_t)cdb_length;
	memcpy(cbw.command, cdb, cdb_length);

	/* Validates the current input. */
	if (input != 0 && ((const uint8_t *)cdb)[0] == SCSI_READ_10 &&
	    cdb_length >= 10U) {
		read_checkpoint.lba = get_be32((const uint8_t *)cdb + 2);
		read_checkpoint.blocks = get_be16((const uint8_t *)cdb + 7);
		checkpoint = &read_checkpoint;
	}

	/* Counts wire attempts, including retries and failed CBWs. */
	io_stats_record(
		cbw.command[0] == SCSI_READ_10
			? IO_USB_READ10
			: (cbw.command[0] == 0x2aU
				   ? IO_USB_WRITE10
				   : (cbw.command[0] ==
						      SCSI_SYNCHRONIZE_CACHE_10
					      ? IO_USB_SYNC_CACHE
					      : IO_USB_OTHER)),
		length);
	actual = 0;

	/* Checks the operation status. */
	error = storage_bulk(storage, storage->bulk_out, &cbw, sizeof(cbw),
			     BOT_TIMEOUT_MS, &actual, NULL);
	if (error != 0 || actual != sizeof(cbw)) {
		hal_printf(
			"usb-storage: BOT CBW error=%d actual=%u expected=%u\n",
			error, (unsigned)actual, (unsigned)sizeof(cbw));
		goto transport_error;
	}

	/* Checks the current data length. */
	if (length != 0) {
		endpoint = input ? storage->bulk_in : storage->bulk_out;
		data_stalled = 0;

		actual = 0;
		error = storage_bulk(storage, endpoint, buffer, length,
				     BOT_TIMEOUT_MS, &actual, checkpoint);
		data_actual = actual;

		/* Checks the operation status. */
		if (error == EPIPE) {
			/* Checks the operation status. */
			halt_error = drv_usb_endpoint_clear_halt(endpoint);
			if (halt_error != 0) {
				error = halt_error;
			} else {
				data_stalled = 1;
				error = 0;
			}
		}
		if (error != 0 ||
		    (data_stalled == 0 && !input && actual != length)) {
			hal_printf("usb-storage: BOT data dir=%s error=%d "
				   "actual=%u "
				   "expected=%u\n",
				   input ? "in" : "out", error,
				   (unsigned)actual, (unsigned)length);
			goto transport_error;
		}
	}

	actual = 0;

	/* Checks the operation status. */
	error = storage_bulk(storage, storage->bulk_in, &csw, sizeof(csw),
			     BOT_TIMEOUT_MS, &actual, NULL);
	if (error == EPIPE) {
		/* Checks the operation status. */
		error = drv_usb_endpoint_clear_halt(storage->bulk_in);
		if (error == 0) {
			error = storage_bulk(storage, storage->bulk_in, &csw,
					     sizeof(csw), BOT_TIMEOUT_MS,
					     &actual, NULL);
		}
	}
	if (error != 0 || actual != sizeof(csw) ||
	    get_le32(csw.signature) != BOT_CSW_SIGNATURE ||
	    get_le32(csw.tag) != tag) {
		hal_printf("usb-storage: BOT CSW error=%d actual=%u status=%u "
			   "tag=%u expected-tag=%u\n",
			   error, (unsigned)actual, (unsigned)csw.status,
			   get_le32(csw.tag), tag);
		goto transport_error;
	}

	/* Handles the csw result condition. */
	csw_result = drv_usb_bot_classify_csw_status(csw.status);
	if (csw_result == DRV_USB_BOT_CSW_INVALID) {
		hal_printf("usb-storage: BOT invalid CSW status=%u\n",
			   (unsigned)csw.status);
		goto transport_error;
	}

	/* Handles the uint64 t condition. */
	residue = get_le32(csw.residue);
	if ((uint64_t)residue > length) {
		hal_printf("usb-storage: BOT residue=%u exceeds transfer=%u\n",
			   residue, (unsigned)length);
		goto transport_error;
	}

	/* Checks the drv usb bot processed length result. */
	if (csw_result == DRV_USB_BOT_CSW_GOOD &&
	    !drv_usb_bot_processed_length(length, data_actual, residue, input,
					  &processed)) {
		hal_printf(
			"usb-storage: BOT data length=%u actual=%u residue=%u "
			"direction=%s\n",
			(unsigned)length, (unsigned)data_actual, residue,
			input ? "in" : "out");
		goto transport_error;
	}

	/* Handles the csw result condition. */
	if (csw_result == DRV_USB_BOT_CSW_GOOD) {
		/* Handles the transferred availability. */
		if (transferred != NULL)
			*transferred = processed;
		/* Succeeded. */
		return 0;
	}

	/* Handles the drv usb bot csw requests sense condition. */
	if (drv_usb_bot_csw_requests_sense(csw_result)) {
		/* Checks the operation status. */
		if (report_command_failed != 0) {
			hal_printf(
				"usb-storage: BOT check-condition residue=%u\n",
				residue);
		}

		/* Checks the operation status. */
		if (command_failed != NULL)
			*command_failed = 1;
		/* Failed. */
		return EIO;
	}

	hal_printf("usb-storage: BOT phase-error residue=%u\n", residue);
transport_error:
	storage->transport_error = 1;

	/* Returns the computed result. */
	return error != 0 ? error : EIO;
}

/* Asks the device why its last command failed. */
static int
request_sense_locked(
	struct usb_storage *storage,
	struct drv_usb_scsi_sense *decoded)
{
	uint8_t command[6] = {SCSI_REQUEST_SENSE, 0, 0, 0, 18, 0};
	uint8_t sense[18];
	size_t actual = 0;
	int error;

	memset(sense, 0, sizeof(sense));

	/* Handles the decoded availability. */
	if (decoded != NULL)
		memset(decoded, 0, sizeof(*decoded));

	/* Checks the operation status. */
	error = bot_command_locked(storage, command, sizeof(command), sense,
				   sizeof(sense), 1, &actual, NULL, 1);
	if (error == 0 && decoded != NULL &&
	    !drv_usb_scsi_parse_sense(sense, actual, decoded)) {
		/* Failed. */
		return EIO;
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Refreshes policy within the original command's lock and deadline. */
static int
storage_reconfigure_locked(
	struct usb_storage *storage)
{
	struct drv_usb_scsi_cache_info cache;
	struct drv_usb_scsi_sense sense;
	enum drv_usb_scsi_flush_policy policy, previous;
	uint8_t mode_command[6], sync_command[10], mode[64];
	size_t actual;
	int error, failed;

	/* Asks the device again how its cache behaves. */
	previous = storage->flush_policy;
	storage->media_state = STORAGE_RECONFIGURE;
	disk_persistence_forget(storage->disk);
	memset(&cache, 0, sizeof(cache));
	memset(mode, 0, sizeof(mode));
	memset(&sense, 0, sizeof(sense));
	(void)drv_usb_scsi_make_mode_sense6_cache_cdb(
		mode_command, sizeof(mode_command), sizeof(mode));

	/* Checks the operation status. */
	error = bot_command_locked(storage, mode_command, sizeof(mode_command),
				   mode, sizeof(mode), 1, &actual, &failed, 1);
	if (error != 0)
		goto fail;
	(void)drv_usb_scsi_parse_mode_sense6_cache(mode, actual, &cache);

	/* Handles the cache condition. */
	if (!cache.header_valid) {
		error = EIO;
		goto fail;
	}

	/*
	 * Flush the old policy's accepted writes before publishing a new
	 * policy.
	 */
	memset(sync_command, 0, sizeof(sync_command));
	sync_command[0] = SCSI_SYNCHRONIZE_CACHE_10;

	/* Checks the operation status. */
	error = bot_command_locked(storage, sync_command, sizeof(sync_command),
				   NULL, 0, 0, NULL, &failed, 1);
	if (error != 0 && failed &&
	    request_sense_locked(storage, &sense) == 0) {
		storage->last_sense = sense;
	}

	policy = drv_usb_scsi_select_flush_policy(&cache, error == 0, &sense);

	/*
	 * A newly advertised FUA/cache-disabled policy cannot prove older
	 * writes.
	 */
	if (error != 0 && (!drv_usb_scsi_sense_is_invalid_opcode(&sense) ||
			   (previous != DRV_USB_SCSI_FLUSH_FUA &&
			    previous != DRV_USB_SCSI_FLUSH_WRITE_THROUGH)))
		goto fail;

	/* Checks the drv usb scsi flush policy allows write result. */
	if (!drv_usb_scsi_flush_policy_allows_write(policy)) {
		error = EOPNOTSUPP;
		goto fail;
	}

	storage->write_protected = cache.write_protected;
	storage->cache_known = cache.cache_valid;
	storage->write_cache_enabled = cache.write_cache_enabled;
	storage->dpofua = cache.dpofua;
	storage->flush_policy = policy;

	/* Handles the disk availability. */
	if (storage->disk != NULL && cache.write_protected)
		storage->disk->d_flags |= DISK_READ_ONLY;
	storage->media_state = STORAGE_ONLINE;

	/* Succeeded. */
	return 0;
fail:
	storage->media_state = STORAGE_FAILED;
	disk_media_revoke(storage->disk);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Runs one command and reads its sense data if it failed. */
static int
bot_command_sense_locked(
	struct usb_storage *storage,
	const void *cdb,
	size_t cdb_length,
	void *buffer,
	size_t length,
	int input,
	struct drv_usb_scsi_sense *sense,
	size_t *transferred,
	int report_command_failed)
{
	struct drv_usb_scsi_sense local_sense;
	enum drv_usb_scsi_recovery action;
	unsigned reset_done = 0, ua_retried = 0, mode_retried = 0;
	uint8_t retry_cdb[16];
	int command_failed, error;
	uint64_t now = sched_ticks();

	/* Handles the cdb availability. */
	if (cdb == NULL || cdb_length == 0 || cdb_length > sizeof(retry_cdb))
		return EINVAL;
	memcpy(retry_cdb, cdb, cdb_length);
	cdb = retry_cdb;

	storage->command_deadline = now + (3U * BOT_TIMEOUT_MS + 9U) / 10U;

	/* Handles the sense availability. */
	if (sense == NULL)
		sense = &local_sense;
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		memset(sense, 0, sizeof(*sense));
		command_failed = 0;

		/* Checks the operation status. */
		error = bot_command_locked(
			storage, cdb, cdb_length, buffer, length, input,
			transferred, &command_failed, report_command_failed);
		if (error == 0)
			break;

		/* Checks the sched ticks result. */
		if (sched_ticks() >= storage->command_deadline)
			break;

		/*
		 * This storage object retains the original USB device. The core
		 * closes admission on disconnect; a replacement binds a
		 * different object and cannot inherit this operation's reset
		 * authorization.
		 */
		if (drv_usb_device_state(storage->device) !=
		    DRV_USB_STATE_CONFIGURED)
			break;

		/* Checks the operation status. */
		if (storage->transport_error != 0) {
			/* Checks the bot reset result. */
			if (reset_done != 0 || bot_reset(storage) != 0)
				break;
			reset_done = 1;
			continue;
		}

		/* Checks the operation status. */
		if (command_failed == 0 ||
		    request_sense_locked(storage, sense) != 0)
			break;
		storage->last_sense = *sense;

		/* Handles the disk availability. */
		action = drv_usb_scsi_recovery_action(sense);
		if (action == DRV_USB_SCSI_RECOVERY_MODE &&
		    storage->disk != NULL && mode_retried == 0) {
			mode_retried = 1;

			/* Checks the operation status. */
			error = storage_reconfigure_locked(storage);
			if (error != 0)
				break;

			/* Handles the retry cdb condition. */
			if (retry_cdb[0] == 0x2aU) {
				/* Handles the storage condition. */
				if (storage->write_protected) {
					error = EROFS;
					break;
				}

				retry_cdb[1] &= ~0x08U;

				/* Checks the drv usb scsi flush policy uses fua result. */
				if (drv_usb_scsi_flush_policy_uses_fua(
					    storage->flush_policy))
					retry_cdb[1] |= 0x08U;
			}

			/* Checks the drv usb scsi flush policy uses sync cache result. */
			if (retry_cdb[0] == SCSI_SYNCHRONIZE_CACHE_10 &&
			    !drv_usb_scsi_flush_policy_uses_sync_cache(
				    storage->flush_policy)) {
				error = 0;
				break;
			}

			continue;
		}

		/*
		 * ASCQ 00 only: do not interpret arbitrary reset/medium-change
		 * indications as proof that our class reset caused them.
		 */
		if (reset_done == 0 || ua_retried != 0 ||
		    action != DRV_USB_SCSI_RECOVERY_RESET) {
			/*
			 * Initial readiness may consume power-on attention
			 * before any disk or cached medium identity exists. Its
			 * bounded probe retries still require success before
			 * publication. Later commands must retain media-change
			 * failures.
			 */
			if (storage->disk != NULL &&
			    action != DRV_USB_SCSI_RECOVERY_NONE) {
				/* Handles the action condition. */
				if (action == DRV_USB_SCSI_RECOVERY_MODE) {
					storage->media_state = STORAGE_FAILED;
					disk_media_revoke(storage->disk);
				} else {
					storage->media_state =
						action == DRV_USB_SCSI_RECOVERY_MEDIA
							? STORAGE_REVALIDATE
						: action == DRV_USB_SCSI_RECOVERY_ABSENT
							? STORAGE_ABSENT
							: STORAGE_FAILED;
					disk_media_revoke(storage->disk);
				}
			}

			break;
		}

		ua_retried = 1;
	}

	storage->command_deadline = 0;

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Runs one command and reports the sense data it gave. */
static int
bot_command_sense_report(
	struct usb_storage *storage,
	const void *cdb,
	size_t cdb_length,
	void *buffer,
	size_t length,
	int input,
	struct drv_usb_scsi_sense *sense,
	size_t *transferred,
	int report_command_failed)
{
	int error;

	mutex_lock(&storage->lock);

	error = bot_command_sense_locked(storage, cdb, cdb_length, buffer,
					 length, input, sense, transferred,
					 report_command_failed);

	mutex_unlock(&storage->lock);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Runs one command and keeps the sense data it gave. */
static int
bot_command_sense(
	struct usb_storage *storage,
	const void *cdb,
	size_t cdb_length,
	void *buffer,
	size_t length,
	int input,
	struct drv_usb_scsi_sense *sense,
	size_t *transferred)
{
	int error;

	/* Obtains the bot command sense report result. */
	error =
		bot_command_sense_report(storage, cdb, cdb_length, buffer,
					 length, input, sense, transferred, 1);

	/* Returns the computed result. */
	return error;
}

/* Runs one command against the device. */
static int
bot_command(
	struct usb_storage *storage,
	const void *cdb,
	size_t cdb_length,
	void *buffer,
	size_t length,
	int input,
	size_t *transferred)
{
	int error;

	/* Obtains the bot command sense result. */
	error = bot_command_sense(storage, cdb, cdb_length, buffer,
					    length, input, NULL, transferred);

	/* Returns the computed result. */
	return error;
}

/* Reports the name of the cache policy in force. */
static const char *
flush_policy_name(
	enum drv_usb_scsi_flush_policy policy)
{
	/* Dispatch the selected operation case. */
	switch (policy) {
	case DRV_USB_SCSI_FLUSH_SYNC_CACHE:
		/* Returns the computed result. */
		return "sync-cache";
	case DRV_USB_SCSI_FLUSH_WRITE_THROUGH:
		/* Returns the computed result. */
		return "write-through";
	case DRV_USB_SCSI_FLUSH_FUA:
		/* Returns the computed result. */
		return "fua";
	default:
		/* Returns the computed result. */
		return "unsafe/unknown";
	}
}

/* Decides how this device's write cache must be flushed. */
static void
scsi_configure_flush_policy(
	struct usb_storage *storage)
{
	struct drv_usb_scsi_cache_info cache;
	struct drv_usb_scsi_sense sense;
	uint8_t mode_command[6] = {0};
	uint8_t sync_command[10] = {SCSI_SYNCHRONIZE_CACHE_10};
	uint8_t mode[64];
	size_t actual = 0;
	int error;

	memset(&cache, 0, sizeof(cache));
	memset(mode, 0, sizeof(mode));

	/* Checks the drv usb scsi make mode sense6 cache cdb result. */
	if (!drv_usb_scsi_make_mode_sense6_cache_cdb(
		    mode_command, sizeof(mode_command), sizeof(mode))) {
		/* Returns the computed result. */
		return;
	}

	/* Checks the operation status. */
	error = bot_command_sense(storage, mode_command, sizeof(mode_command),
				  mode, sizeof(mode), 1, &sense, &actual);
	if (error == 0) {
		(void)drv_usb_scsi_parse_mode_sense6_cache(mode, actual,
							   &cache);
	}

	/* Handles the cache condition. */
	if (cache.header_valid != 0)
		storage->write_protected = cache.write_protected;
	storage->cache_known = cache.cache_valid;
	storage->write_cache_enabled = cache.write_cache_enabled;
	storage->dpofua = cache.dpofua;

	/*
	 * A protected medium cannot accept volatile writes and needs no flush.
	 */
	if (storage->write_protected != 0) {
		storage->flush_policy = DRV_USB_SCSI_FLUSH_WRITE_THROUGH;

		/* Returns the computed result. */
		return;
	}

	/* Handles the cache condition. */
	if (cache.cache_valid != 0 && cache.write_cache_enabled == 0) {
		storage->flush_policy =
			drv_usb_scsi_select_flush_policy(&cache, 0, NULL);

		/* Returns the computed result. */
		return;
	}

	/*
	 * Probe before publishing the disk.  If this command is unsupported,
	 * every later WRITE must already use the selected fallback policy.
	 */
	error = bot_command_sense(storage, sync_command, sizeof(sync_command),
				  NULL, 0, 0, &sense, NULL);
	storage->flush_policy =
		drv_usb_scsi_select_flush_policy(&cache, error == 0, &sense);
	if (error == 0)
		return;

	/* Handles the sense condition. */
	if (sense.valid) {
		hal_printf("usb-storage: flush preflight error=%d "
			   "sense=%02x/%02x/%02x policy=%s\n",
			   error, sense.key, sense.asc, sense.ascq,
			   flush_policy_name(storage->flush_policy));
	} else {
		hal_printf("usb-storage: flush preflight error=%d "
			   "sense=unavailable policy=%s\n",
			   error, flush_policy_name(storage->flush_policy));
	}
}

/* Reads the geometry the device reports and publishes it. */
static int
scsi_probe(
	struct usb_storage *storage,
	int *medium_absent)
{
	uint8_t ready_command[6] = {SCSI_TEST_UNIT_READY};
	uint8_t inquiry_command[6] = {SCSI_INQUIRY, 0, 0, 0, 36, 0};
	uint8_t capacity_command[10] = {SCSI_READ_CAPACITY_10};
	uint8_t inquiry[36], capacity[8];
	uint32_t last_block, block_size;
	size_t actual;
	int error, removable;
	unsigned attempt;
	struct drv_usb_scsi_sense sense;

	/* Handles the medium absent availability. */
	if (medium_absent != NULL)
		*medium_absent = 0;

	memset(inquiry, 0, sizeof(inquiry));

	/* Checks the operation status. */
	error = bot_command(storage, inquiry_command, sizeof(inquiry_command),
			    inquiry, sizeof(inquiry), 1, &actual);
	if (error != 0)
		return error;

	/* Handles the actual condition. */
	if (actual < 1U || (inquiry[0] & 0x1fU) != 0U)
		return ENODEV;
	removable = actual >= 2U && (inquiry[1] & 0x80U) != 0;
	/* Process each element required by the operation. */
	for (attempt = 0; attempt < 3U; attempt++) {
		/* Checks the operation status. */
		error = bot_command_sense_report(storage, ready_command,
						 sizeof(ready_command), NULL, 0,
						 0, &sense, NULL, 0);
		if (error == 0)
			break;

		/* Checks the drv usb scsi sense is medium absent result. */
		if (removable != 0 &&
		    drv_usb_scsi_sense_is_medium_absent(&sense)) {
			/* Handles the medium absent availability. */
			if (medium_absent != NULL)
				*medium_absent = 1;
			/* Failed. */
			return error;
		}
	}

	/* Checks the operation status. */
	if (error != 0) {
		/* Handles the sense condition. */
		if (sense.valid) {
			hal_printf("usb-storage: LUN %u not ready error=%d "
				   "sense=%02x/%02x/%02x\n",
				   storage->lun, error, sense.key, sense.asc,
				   sense.ascq);
		} else {
			hal_printf("usb-storage: LUN %u not ready error=%d "
				   "sense=unavailable\n",
				   storage->lun, error);
		}

		/* Failed. */
		return error;
	}

	memset(capacity, 0, sizeof(capacity));

	/* Checks the operation status. */
	error = bot_command_sense(storage, capacity_command,
				  sizeof(capacity_command), capacity,
				  sizeof(capacity), 1, NULL, &actual);
	if (error != 0)
		return error;

	/* Handles the actual condition. */
	if (actual < sizeof(capacity))
		return EIO;
	last_block = get_be32(capacity);

	/* Handles the last block condition. */
	block_size = get_be32(capacity + 4);
	if (last_block == UINT32_MAX || block_size == 0)
		return EOVERFLOW;
	storage->block_size = block_size;
	storage->block_count = (uint64_t)last_block + 1U;
	scsi_configure_flush_policy(storage);

	/* Succeeded. */
	return 0;
}

/* Serves one block request against this device. */
static int
storage_submit(
	struct disk *disk,
	struct bio *bio)
{
	struct usb_storage *storage = disk->d_data;
	struct drv_usb_scsi_sense sense;
	uint8_t command[10] = {0};
	uint8_t opcode = 0;
	size_t actual = 0;
	size_t expected = 0;
	int error;

	memset(&sense, 0, sizeof(sense));
	mutex_lock(&storage->lock);

	/* Handles the storage condition. */
	if (storage->media_state != STORAGE_ONLINE) {
		error = EIO;
	} else if (storage->flush_error != 0 &&
		   (bio->b_op == BIO_WRITE || bio->b_op == BIO_FLUSH)) {
		error = storage->flush_error;
	} else if (bio->b_op == BIO_FLUSH) {
		/* Checks the drv usb scsi flush policy uses sync cache result. */
		if (drv_usb_scsi_flush_policy_uses_sync_cache(
			    storage->flush_policy)) {
			opcode = command[0] = SCSI_SYNCHRONIZE_CACHE_10;
			error = bot_command_sense_locked(storage, command,
							 sizeof(command), NULL,
							 0, 0, &sense, NULL, 1);
			drv_usb_scsi_record_flush_result(storage->flush_policy,
							 error,
							 &storage->flush_error);
		} else if (drv_usb_scsi_flush_policy_allows_write(
				   storage->flush_policy)) {
			error = 0;
		} else {
			error = EOPNOTSUPP;
		}
	} else if (bio->b_op != BIO_READ && bio->b_op != BIO_WRITE) {
		error = EOPNOTSUPP;
	} else if (bio->b_mapped_block > UINT32_MAX ||
		   bio->b_block_count == 0 || bio->b_block_count > UINT16_MAX) {
		error = EOVERFLOW;
	} else {
		/* Handles the bio condition. */
		if (bio->b_op == BIO_WRITE && storage->write_protected != 0) {
			error = EROFS;
			goto out;
		}

		/* Checks the drv usb scsi make rw10 cdb result. */
		if (!drv_usb_scsi_make_rw10_cdb(command, sizeof(command),
						bio->b_op == BIO_WRITE,
						storage->flush_policy)) {
			error = bio->b_op == BIO_WRITE ? EROFS : EIO;
			goto out;
		}

		opcode = command[0];
		put_be32(command + 2, (uint32_t)bio->b_mapped_block);
		put_be16(command + 7, (uint16_t)bio->b_block_count);
		expected = (size_t)bio->b_block_count * disk->d_block_size;

		/* Checks the operation status. */
		error = bot_command_sense_locked(
			storage, command, sizeof(command), bio->b_data,
			expected, bio->b_op == BIO_READ, &sense, &actual, 1);
		if (error == 0 && actual != expected)
			error = EIO;
	}

out:

	mutex_unlock(&storage->lock);

	/* Checks the operation status. */
	if (error != 0 && (opcode != 0 || bio->b_op == BIO_FLUSH)) {
		/* Handles the bio condition. */
		if (bio->b_op == BIO_FLUSH && sense.valid) {
			hal_printf("usb-storage: %s flush policy=%s error=%d "
				   "sense=%02x/%02x/%02x\n",
				   disk->d_name,
				   flush_policy_name(storage->flush_policy),
				   error, sense.key, sense.asc, sense.ascq);
		} else if (bio->b_op == BIO_FLUSH) {
			hal_printf("usb-storage: %s flush policy=%s error=%d "
				   "sense=unavailable\n",
				   disk->d_name,
				   flush_policy_name(storage->flush_policy),
				   error);
		} else if (sense.valid) {
			hal_printf("usb-storage: %s op=%02x lba=%u blocks=%u "
				   "error=%d sense=%02x/%02x/%02x\n",
				   disk->d_name, opcode,
				   (uint32_t)bio->b_mapped_block,
				   bio->b_block_count, error, sense.key,
				   sense.asc, sense.ascq);
		} else {
			hal_printf("usb-storage: %s op=%02x lba=%u blocks=%u "
				   "error=%d sense=unavailable\n",
				   disk->d_name, opcode,
				   (uint32_t)bio->b_mapped_block,
				   bio->b_block_count, error);
		}
	}

	bio_complete(bio, error,
		     error == 0 && bio->b_op != BIO_FLUSH ? expected : 0);

	/* Succeeded. */
	return 0;
}

/* Serves one control request against this device. */
static int
storage_ioctl(
	struct disk *disk,
	unsigned long request,
	void *argument)
{
	struct disk_geometry *geometry = argument;
	uint64_t cylinders;

	/* Handles the geometry availability. */
	if (request != DISK_IOCTL_GET_GEOMETRY || geometry == NULL)
		return EOPNOTSUPP;
	geometry->heads = 255U;
	geometry->sectors_per_track = 63U;
	cylinders = disk->d_block_count /
		    ((uint64_t)geometry->heads * geometry->sectors_per_track);
	geometry->cylinders =
		cylinders > UINT32_MAX ? UINT32_MAX : (uint32_t)cylinders;

	/* Succeeded. */
	return 0;
}

/* Publishes a probed medium without consuming the class/control owner on error. */
static int
storage_publish_disk(
	struct usb_storage *storage)
{
	struct disk *disk;
	int error;

	/* Handles the disk availability. */
	if (storage->disk != NULL)
		return EBUSY;

	/* Builds normal core/HCD reserves before publishing a usable disk. */

	/* Checks the operation status. */
	error = storage_transfer_reserve(storage);
	if (error != 0) {
		return error;
	}

	/*
	 * READ CAPACITY may describe 4KiB (or larger) logical sectors. Bound
	 * BIOs by bytes, not the historical sixteen 512-byte blocks.
	 */
	if (storage->block_size > DRV_USB_URB_RECLAIM_SAFE_MAX_SIZE) {
		/* Checks the operation status. */
		error = drv_usb_urb_reserve_sync(storage->bulk_in_urb,
						 storage->block_size);
		if (error == 0) {
			error = drv_usb_urb_reserve_sync(storage->bulk_out_urb,
							 storage->block_size);
		}
		if (error != 0) {
			return error;
		}
	}

	/* Handles the disk availability. */
	disk = disk_alloc();
	if (disk == NULL) {
		return ENOSPC;
	}

	/* Checks the operation status. */
	error = disk_alloc_sd_name(disk);
	if (error != 0) {
		(void)disk_destroy(disk);

		/* Failed. */
		return error;
	}

	disk->d_flags =
		DISK_REMOVABLE |
		(drv_usb_scsi_flush_policy_requires_read_only(
			 storage->flush_policy, storage->write_protected)
			 ? DISK_READ_ONLY
			 : 0);

	/*
	 * Cache-disabled or working SYNCHRONIZE CACHE policy establishes
	 * persistence.
	 */
	if (drv_usb_scsi_flush_policy_allows_write(storage->flush_policy))
		disk->d_flags |= DISK_FLUSH_PROOF;
	disk->d_block_size = storage->block_size;
	disk->d_block_count = storage->block_count;
	disk->d_max_transfer_blocks =
		storage->transfer_size / storage->block_size;

	/* Handles the disk condition. */
	if (disk->d_max_transfer_blocks == 0)
		disk->d_max_transfer_blocks = 1;
	disk->d_ops = &storage_disk_ops;
	disk->d_data = storage;
	storage->disk = disk;

	/* Checks the operation status. */
	error = disk_create(disk);
	if (error != 0) {
		storage->disk = NULL;
		(void)disk_destroy(disk);

		/* Failed. */
		return error;
	}

	hal_printf("usb-storage: %s blocks=%u block-size=%u cache=%s "
		   "dpofua=%s flush=%s%s\n",
		   disk->d_name, (uint32_t)disk->d_block_count,
		   disk->d_block_size,
		   storage->cache_known == 0	       ? "unknown"
		   : storage->write_cache_enabled != 0 ? "write-back"
						       : "disabled",
		   storage->dpofua != 0 ? "yes" : "no",
		   flush_policy_name(storage->flush_policy),
		   (disk->d_flags & DISK_READ_ONLY) != 0 ? " read-only" : "");

	/* Succeeded. */
	return 0;
}

/* Publishes replacement partitions outside the recursive command mutex. */
static int
storage_refresh_partitions(
	struct usb_storage *storage)
{
	int error;

	/*
	 * Retries temporary ownership/allocation failures on a later control
	 * pass.
	 */

	/* Checks the operation status. */
	/* Reload requires exactly one administrative open, outside command lock. */
	error = disk_open(storage->disk);
	if (error != 0)
		return error;
	error = partition_reload(storage->disk);
	disk_close(storage->disk);
	if (error == 0 || error == EINVAL || error == EOPNOTSUPP)
		storage->partitions_pending = 0;

	/*
	 * Leaves a whole-disk filesystem usable when it has no partition table.
	 */
	if (error == EINVAL || error == EOPNOTSUPP)
		return 0;

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Executes one bounded pass under control ownership, never from a submitted BIO. */
static int
storage_control_step(
	struct usb_storage *storage)
{
	uint8_t ready[6] = {SCSI_TEST_UNIT_READY};
	int error, absent;

	/* Checks the drv usb device state result. */
	if (drv_usb_device_state(storage->device) != DRV_USB_STATE_CONFIGURED)
		return ENODEV;
	mutex_lock(&storage->lock);

	/* Handles the storage condition. */
	if (storage->media_state == STORAGE_ONLINE) {
		error = bot_command_sense_locked(storage, ready, sizeof(ready),
						 NULL, 0, 0, NULL, NULL, 0);
		mutex_unlock(&storage->lock);
		if (error == 0 && storage->partitions_pending)
			error = storage_refresh_partitions(storage);

		/* Failed. */
		return error;
	}

	/* Handles the storage condition. */
	if (storage->media_state != STORAGE_REVALIDATE &&
	    storage->media_state != STORAGE_ABSENT) {
		mutex_unlock(&storage->lock);

		/* Failed. */
		return EIO;
	}

	/* Handles the disk availability. */
	if (storage->disk != NULL) {
		/* Handles the storage condition. */
		if (!storage->media_retired) {
			/* Checks the operation status. */
			error = partition_retire_media(storage->disk);
			if (error != 0) {
				mutex_unlock(&storage->lock);

				/* Failed. */
				return error;
			}

			storage->media_retired = 1;
		}

		/* Checks the operation status. */
		error = disk_destroy(storage->disk);
		if (error != 0) {
			mutex_unlock(&storage->lock);

			/* Failed. */
			return error;
		}

		storage->disk = NULL;
		storage->media_retired = 0;
	}

	/*
	 * Fresh policy is permitted only after complete old-identity
	 * retirement.
	 */
	storage->flush_error = 0;
	storage->transport_error = 0;
	storage->write_protected = 0;
	storage->cache_known = 0;
	storage->write_cache_enabled = 0;
	storage->dpofua = 0;
	storage->flush_policy = DRV_USB_SCSI_FLUSH_UNSAFE;
	storage->block_size = 0;
	storage->block_count = 0;
	storage->partitions_pending = 0;
	storage_urbs_free(storage);
	error = storage_urbs_alloc(storage);

	mutex_unlock(&storage->lock);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	absent = 0;
	error = scsi_probe(storage, &absent);
	mutex_lock(&storage->lock);
	if (error != 0) {
		storage->media_state =
			absent ? STORAGE_ABSENT : STORAGE_REVALIDATE;
	} else {
		storage->media_state = STORAGE_ONLINE;

		/* Checks the operation status. */
		error = storage_publish_disk(storage);
		if (error != 0)
			storage->media_state = STORAGE_REVALIDATE;
		else
			storage->partitions_pending = 1;
	}

	mutex_unlock(&storage->lock);

	/* Checks the operation status. */
	if (error == 0 && storage->partitions_pending)
		error = storage_refresh_partitions(storage);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Polls readiness outside submitted BIOs, with class lifetime held until join. */
static void
storage_control_worker(
	void *argument)
{
	struct usb_storage *storage = argument;

	/* Continue while the operation condition remains true. */
	while (!atomic_raw_load_acquire(&storage->control_ready)) {
		/* Checks the atomic raw load acquire result. */
		if (atomic_raw_load_acquire(&storage->control_stopping))
			return;
		kernel_wait_task();
	}
	while (!atomic_raw_load_acquire(&storage->control_stopping)) {
		mutex_lock(&storage->control_lock);

		/* Checks the atomic raw load acquire result. */
		if (!atomic_raw_load_acquire(&storage->control_stopping))
			(void)storage_control_step(storage);
		mutex_unlock(&storage->control_lock);

		/* Checks the atomic raw load acquire result. */
		if (!atomic_raw_load_acquire(&storage->control_stopping))
			sched_sleep(sched_ticks() + 100U);
	}
}

/* Allocates before disk publication and waits for the attach-ready handshake. */
static int
storage_control_start(
	struct usb_storage *storage)
{
	int error;

	/* Checks the operation status. */
	error = kthread_create(storage_control_worker, storage,
			       SCHED_PRIORITY_DEFAULT,
			       &storage->control_worker);
	if (error == 0)
		thread_start(storage->control_worker);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Never waits with a command/control mutex held; failed joins retain ownership. */
static int
storage_control_stop(
	struct usb_storage *storage)
{
	struct thread *worker;
	uint64_t deadline;
	int error;

	/* Handles the worker availability. */
	worker = storage->control_worker;
	if (worker == NULL)
		return 0;
	atomic_raw_store_release(&storage->control_stopping, 1U);
	kernel_notify_task(worker->task);
	deadline = sched_ticks() + 2000U;
	/* Continue while the operation condition remains true. */
	while (atomic_raw_load_acquire((volatile unsigned *)&worker->state) !=
	       THREAD_ZOMBIE) {
		/* Checks the sched ticks result. */
		if (sched_ticks() >= deadline)
			return EBUSY;
		sched_yield();
	}

	/* Checks the operation status. */
	error = thread_wait(worker, NULL);
	if (error == 0)
		storage->control_worker = NULL;

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Binds this driver to an interface the bus has matched. */
static int
storage_attach(
	struct drv_usb_interface *interface,
	const struct drv_usb_id *id)
{
	struct usb_storage *storage;
	uint8_t maximum_lun = 0;
	size_t actual = 0;
	int error, medium_absent = 0;

	(void)id;

	/* Handles the storage availability. */
	storage = kernel_alloc(sizeof(*storage));
	if (storage == NULL)
		return ENOMEM;
	memset(storage, 0, sizeof(*storage));
	storage->interface = interface;
	storage->device = drv_usb_interface_device(interface);
	storage->bulk_in = drv_usb_interface_find_endpoint(
		interface, DRV_USB_TRANSFER_BULK, DRV_USB_DIR_IN, NULL);
	storage->bulk_out = drv_usb_interface_find_endpoint(
		interface, DRV_USB_TRANSFER_BULK, DRV_USB_DIR_OUT, NULL);

	/* Handles the bulk in availability. */
	if (storage->bulk_in == NULL || storage->bulk_out == NULL) {
		kernel_free(storage);

		/* Failed. */
		return ENODEV;
	}

	(void)mutex_init(&storage->lock, LOCK_RANK_DISK, "usb-storage");
	(void)mutex_init(&storage->control_lock, LOCK_RANK_DEVICE,
			 "usb-storage control");

	/* Checks the operation status. */
	error = storage_urbs_alloc(storage);
	if (error != 0) {
		kernel_free(storage);

		/* Failed. */
		return error;
	}

	/* Checks the storage control result. */
	if (storage_control(storage,
			    DRV_USB_DIR_IN | DRV_USB_REQUEST_CLASS |
				    DRV_USB_RECIP_INTERFACE,
			    USB_MASS_STORAGE_GET_MAX_LUN, 0,
			    drv_usb_interface_number(interface), &maximum_lun,
			    1, 1000U, &actual) == 0 &&
	    actual == 1 && maximum_lun != 0) {
		hal_printf("usb-storage: only LUN 0 of %u is supported\n",
			   (unsigned)maximum_lun + 1U);
	}

	/* Checks the operation status. */
	error = scsi_probe(storage, &medium_absent);
	if (error != 0 && !medium_absent)
		goto fail;

	/* Handles the medium absent condition. */
	if (medium_absent)
		storage->media_state = STORAGE_ABSENT;

	/* Checks the operation status. */
	error = storage_control_start(storage);
	if (error != 0)
		goto fail;

	/* Handles the medium absent condition. */
	if (!medium_absent) {
		/* Checks the operation status. */
		error = storage_publish_disk(storage);
		if (error != 0)
			goto fail;
	}

	(void)drv_usb_interface_set_driver_data(interface, storage);
	atomic_raw_store_release(&storage->control_ready, 1U);
	kernel_notify_task(storage->control_worker->task);

	/* Handles the medium absent condition. */
	if (medium_absent) {
		hal_printf("usb-storage: LUN %u has no medium; reader attached "
			   "without a disk\n",
			   storage->lun);
	}

	/* Succeeded. */
	return 0;
fail:

	/* Checks the storage control stop result. */
	if (storage_control_stop(storage) != 0) {
		/*
		 * Retain a class owner so a later detach can finish the failed
		 * join.
		 */
		storage->media_state = STORAGE_FAILED;
		(void)drv_usb_interface_set_driver_data(interface, storage);
		hal_printf("usb-storage: attach error=%d; control owner "
			   "retained for stop\n",
			   error);

		/* Succeeded. */
		return 0;
	}

	storage_urbs_free(storage);
	kernel_free(storage);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Stops terminal activity without destroying root/swap-referenced media. */
static int
storage_quiesce(
	struct drv_usb_interface *interface)
{
	struct usb_storage *storage;
	int error;

	/* A failed attach may not have published any class owner. */
	storage = drv_usb_interface_driver_data(interface);
	if (storage == NULL)
		return 0;

	/* Excludes media revalidation and waits for the current BOT command. */
	mutex_lock(&storage->control_lock);
	atomic_raw_store_release(&storage->control_stopping, 1U);
	mutex_lock(&storage->lock);
	storage->media_state = STORAGE_FAILED;
	mutex_unlock(&storage->lock);
	mutex_unlock(&storage->control_lock);

	/* Joins without either mutex held; failure keeps the worker and URBs. */
	error = storage_control_stop(storage);
	return error;
}

/* Gives that interface up and everything held for it. */
static int
storage_detach(
	struct drv_usb_interface *interface,
	unsigned flags)
{
	struct usb_storage *storage = drv_usb_interface_driver_data(interface);
	int error;

	(void)flags;

	/* Handles the storage availability. */
	if (storage == NULL)
		return 0;
	mutex_lock(&storage->control_lock);

	/* Handles the disk availability. */
	if (storage->disk != NULL) {
		/* Handles the storage condition. */
		if (!storage->media_retired) {
			/* Checks the operation status. */
			error = disk_media_status(storage->disk) != 0
					? partition_retire_media(storage->disk)
					: disk_gone_if_idle(storage->disk);
			if (error != 0)
				goto unlock;
			storage->media_retired = 1;
		}

		/* Checks the operation status. */
		error = disk_destroy(storage->disk);
		if (error != 0)
			goto unlock;
		storage->disk = NULL;
	}

	atomic_raw_store_release(&storage->control_stopping, 1U);

	mutex_unlock(&storage->control_lock);

	/* Checks the operation status. */
	error = storage_control_stop(storage);
	if (error != 0)
		return error;
	storage_urbs_free(storage);
	kernel_free(storage);

	/* Succeeded. */
	return 0;
unlock:

	mutex_unlock(&storage->control_lock);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * USB Mass Storage Class
 */
