/*
 * USB Mass Storage Bulk-Only Transport and minimal SCSI disk driver
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#include <drivers/usb-storage.h>
#include <drivers/usb-storage-scsi.h>
#include <drivers/usb.h>
#include <errno.h>
#include <hal/hal.h>
#include <kern/disk.h>
#include <kern/lock.h>
#include <string.h>

#define USB_MASS_STORAGE_CLASS 0x08U
#define USB_MASS_STORAGE_SCSI 0x06U
#define USB_MASS_STORAGE_BULK_ONLY 0x50U
#define USB_MASS_STORAGE_RESET 0xffU
#define USB_MASS_STORAGE_GET_MAX_LUN 0xfeU
#define USB_ENDPOINT_HALT 0U

#define BOT_CBW_SIGNATURE 0x43425355U
#define BOT_CSW_SIGNATURE 0x53425355U
#define BOT_DIRECTION_IN 0x80U
#define BOT_TIMEOUT_MS 5000U

#define SCSI_TEST_UNIT_READY 0x00U
#define SCSI_REQUEST_SENSE 0x03U
#define SCSI_INQUIRY 0x12U
#define SCSI_MODE_SENSE_6 0x1aU
#define SCSI_READ_CAPACITY_10 0x25U
#define SCSI_READ_10 0x28U
#define SCSI_WRITE_10 0x2aU
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

struct usb_storage {
	struct drv_usb_interface *interface;
	struct drv_usb_device *device;
	struct drv_usb_endpoint *bulk_in;
	struct drv_usb_endpoint *bulk_out;
	struct disk *disk;
	struct mutex lock;
	uint32_t next_tag;
	uint32_t block_size;
	uint64_t block_count;
	uint8_t lun;
	uint8_t write_protected;
};

static uint32_t get_le32(const uint8_t value[4])
{
	return (uint32_t)value[0] | ((uint32_t)value[1] << 8) |
	    ((uint32_t)value[2] << 16) | ((uint32_t)value[3] << 24);
}
static void put_le32(uint8_t value[4], uint32_t number)
{
	value[0] = (uint8_t)number;
	value[1] = (uint8_t)(number >> 8);
	value[2] = (uint8_t)(number >> 16);
	value[3] = (uint8_t)(number >> 24);
}

static uint32_t get_be32(const uint8_t value[4])
{
	return ((uint32_t)value[0] << 24) | ((uint32_t)value[1] << 16) |
	    ((uint32_t)value[2] << 8) | value[3];
}

static void put_be32(uint8_t value[4], uint32_t number)
{
	value[0] = (uint8_t)(number >> 24);
	value[1] = (uint8_t)(number >> 16);
	value[2] = (uint8_t)(number >> 8);
	value[3] = (uint8_t)number;
}

static void put_be16(uint8_t value[2], uint16_t number)
{
	value[0] = (uint8_t)(number >> 8);
	value[1] = (uint8_t)number;
}

static int clear_halt(struct usb_storage *storage,
	struct drv_usb_endpoint *endpoint)
{
	size_t actual = 0;
	return drv_usb_control(storage->device,
	    DRV_USB_DIR_OUT | DRV_USB_REQUEST_STANDARD | DRV_USB_RECIP_ENDPOINT,
	    1U, USB_ENDPOINT_HALT, drv_usb_endpoint_address(endpoint), NULL, 0,
	    1000U, &actual);
}

static int bot_reset(struct usb_storage *storage)
{
	size_t actual = 0;
	int error;
	error = drv_usb_control(storage->device,
	    DRV_USB_DIR_OUT | DRV_USB_REQUEST_CLASS | DRV_USB_RECIP_INTERFACE,
	    USB_MASS_STORAGE_RESET, 0, drv_usb_interface_number(storage->interface),
	    NULL, 0, 1000U, &actual);
	if (error != 0)
		return error;
	(void)drv_usb_endpoint_set_hcd_data(storage->bulk_in, 0, 0);
	(void)drv_usb_endpoint_set_hcd_data(storage->bulk_out, 0, 0);
	error = clear_halt(storage, storage->bulk_in);
	if (error == 0)
		error = clear_halt(storage, storage->bulk_out);
	return error;
}

static int bot_command_locked(struct usb_storage *storage, const void *cdb,
	size_t cdb_length, void *buffer, size_t length, int input)
{
	struct bot_cbw cbw;
	struct bot_csw csw;
	uint32_t tag;
	size_t actual;
	int error;

	if (cdb == NULL || cdb_length == 0 || cdb_length > sizeof(cbw.command) ||
	    length > UINT32_MAX)
		return EINVAL;
	memset(&cbw, 0, sizeof(cbw));
	memset(&csw, 0, sizeof(csw));
	put_le32(cbw.signature, BOT_CBW_SIGNATURE);
	tag = ++storage->next_tag;
	if (tag == 0)
		tag = ++storage->next_tag;
	put_le32(cbw.tag, tag);
	put_le32(cbw.transfer_length, (uint32_t)length);
	cbw.flags = input ? BOT_DIRECTION_IN : 0;
	cbw.lun = storage->lun;
	cbw.command_length = (uint8_t)cdb_length;
	memcpy(cbw.command, cdb, cdb_length);

	error = drv_usb_bulk(storage->device, storage->bulk_out, &cbw,
	    sizeof(cbw), BOT_TIMEOUT_MS, &actual);
	if (error != 0 || actual != sizeof(cbw)) {
		hal_printf("usb-storage: BOT CBW error=%d actual=%u expected=%u\n",
		    error, (unsigned)actual, (unsigned)sizeof(cbw));
		goto transport_error;
	}
	if (length != 0) {
		struct drv_usb_endpoint *endpoint = input ? storage->bulk_in :
		    storage->bulk_out;
		error = drv_usb_bulk(storage->device, endpoint, buffer, length,
		    BOT_TIMEOUT_MS, &actual);
		if (error != 0 || (!input && actual != length)) {
			hal_printf("usb-storage: BOT data dir=%s error=%d actual=%u "
			    "expected=%u\n", input ? "in" : "out", error,
			    (unsigned)actual, (unsigned)length);
			goto transport_error;
		}
	}
	error = drv_usb_bulk(storage->device, storage->bulk_in, &csw,
	    sizeof(csw), BOT_TIMEOUT_MS, &actual);
	if (error != 0 || actual != sizeof(csw) ||
	    get_le32(csw.signature) != BOT_CSW_SIGNATURE ||
	    get_le32(csw.tag) != tag || csw.status > 2U) {
		hal_printf("usb-storage: BOT CSW error=%d actual=%u status=%u "
		    "tag=%u expected-tag=%u\n", error, (unsigned)actual,
		    (unsigned)csw.status, get_le32(csw.tag), tag);
		goto transport_error;
	}
	if (csw.status == 0)
		return 0;
	if (csw.status == 1) {
		hal_printf("usb-storage: BOT check-condition residue=%u\n",
		    get_le32(csw.residue));
		return EIO;
	}
	hal_printf("usb-storage: BOT phase-error residue=%u\n",
	    get_le32(csw.residue));
transport_error:
	(void)bot_reset(storage);
	return error != 0 ? error : EIO;
}

static int
request_sense_locked(struct usb_storage *storage,
	struct drv_usb_scsi_sense *decoded)
{
	uint8_t command[6] = { SCSI_REQUEST_SENSE, 0, 0, 0, 18, 0 };
	uint8_t sense[18];
	int error;

	memset(sense, 0, sizeof(sense));
	if (decoded != NULL)
		memset(decoded, 0, sizeof(*decoded));
	error = bot_command_locked(storage, command, sizeof(command), sense,
	    sizeof(sense), 1);
	if (error == 0 && decoded != NULL &&
	    !drv_usb_scsi_parse_sense(sense, sizeof(sense), decoded))
		return EIO;
	return error;
}

static int
bot_command_sense(struct usb_storage *storage, const void *cdb,
	size_t cdb_length, void *buffer, size_t length, int input,
	struct drv_usb_scsi_sense *sense)
{
	int error;

	if (sense != NULL)
		memset(sense, 0, sizeof(*sense));
	mutex_lock(&storage->lock);
	error = bot_command_locked(storage, cdb, cdb_length, buffer, length,
	    input);
	if (error != 0 && sense != NULL)
		(void)request_sense_locked(storage, sense);
	mutex_unlock(&storage->lock);
	return error;
}

static int bot_command(struct usb_storage *storage, const void *cdb,
	size_t cdb_length, void *buffer, size_t length, int input)
{
	return bot_command_sense(storage, cdb, cdb_length, buffer, length,
	    input, NULL);
}

static int scsi_probe(struct usb_storage *storage)
{
	uint8_t ready_command[6] = { SCSI_TEST_UNIT_READY };
	uint8_t inquiry_command[6] = { SCSI_INQUIRY, 0, 0, 0, 36, 0 };
	uint8_t mode_command[6] = { SCSI_MODE_SENSE_6, 0x08, 0x3f, 0, 4, 0 };
	uint8_t capacity_command[10] = { SCSI_READ_CAPACITY_10 };
	uint8_t inquiry[36], mode[4], capacity[8];
	uint32_t last_block, block_size;
	int error;
	unsigned attempt;

	error = bot_command(storage, inquiry_command, sizeof(inquiry_command),
	    inquiry, sizeof(inquiry), 1);
	if (error != 0)
		return error;
	if ((inquiry[0] & 0x1fU) != 0U)
		return ENODEV;
	for (attempt = 0; attempt < 3U; attempt++) {
		struct drv_usb_scsi_sense sense;

		error = bot_command_sense(storage, ready_command,
		    sizeof(ready_command), NULL, 0, 0, &sense);
		if (error == 0)
			break;
	}
	if (error != 0)
		return error;
	error = bot_command_sense(storage, capacity_command,
	    sizeof(capacity_command), capacity, sizeof(capacity), 1, NULL);
	if (error != 0)
		return error;
	last_block = get_be32(capacity);
	block_size = get_be32(capacity + 4);
	if (last_block == UINT32_MAX || block_size == 0)
		return EOVERFLOW;
	storage->block_size = block_size;
	storage->block_count = (uint64_t)last_block + 1U;
	memset(mode, 0, sizeof(mode));
	if (bot_command(storage, mode_command, sizeof(mode_command), mode,
	    sizeof(mode), 1) == 0)
		storage->write_protected =
		    drv_usb_scsi_mode_sense6_read_only(mode, sizeof(mode));
	return 0;
}

static int storage_submit(struct disk *disk, struct bio *bio)
{
	struct usb_storage *storage = disk->d_data;
	struct drv_usb_scsi_sense sense;
	uint8_t command[10] = { 0 };
	uint8_t opcode = 0;
	int error;

	if (bio->b_op == BIO_FLUSH) {
		opcode = command[0] = SCSI_SYNCHRONIZE_CACHE_10;
		error = bot_command_sense(storage, command, sizeof(command), NULL,
		    0, 0, &sense);
	} else if (bio->b_mapped_block > UINT32_MAX ||
	    bio->b_block_count == 0 || bio->b_block_count > UINT16_MAX) {
		error = EOVERFLOW;
	} else {
		opcode = command[0] =
		    bio->b_op == BIO_READ ? SCSI_READ_10 : SCSI_WRITE_10;
		put_be32(command + 2, (uint32_t)bio->b_mapped_block);
		put_be16(command + 7, (uint16_t)bio->b_block_count);
		error = bot_command_sense(storage, command, sizeof(command),
		    bio->b_data,
		    (size_t)bio->b_block_count * disk->d_block_size,
		    bio->b_op == BIO_READ, &sense);
	}
	if (error != 0 && opcode != 0) {
		if (sense.valid)
			hal_printf("usb-storage: %s op=%02x lba=%u blocks=%u "
			    "error=%d sense=%02x/%02x/%02x\n",
			    disk->d_name, opcode, (uint32_t)bio->b_mapped_block,
			    bio->b_block_count, error, sense.key, sense.asc,
			    sense.ascq);
		else
			hal_printf("usb-storage: %s op=%02x lba=%u blocks=%u "
			    "error=%d sense=unavailable\n",
			    disk->d_name, opcode, (uint32_t)bio->b_mapped_block,
			    bio->b_block_count, error);
	}
	bio_complete(bio, error, error == 0 && bio->b_op != BIO_FLUSH ?
	    (size_t)bio->b_block_count * disk->d_block_size : 0);
	return 0;
}

static int storage_ioctl(struct disk *disk, unsigned long request,
	void *argument)
{
	struct disk_geometry *geometry = argument;
	uint64_t cylinders;
	if (request != DISK_IOCTL_GET_GEOMETRY || geometry == NULL)
		return EOPNOTSUPP;
	geometry->heads = 255U;
	geometry->sectors_per_track = 63U;
	cylinders = disk->d_block_count /
	    ((uint64_t)geometry->heads * geometry->sectors_per_track);
	geometry->cylinders =
	    cylinders > UINT32_MAX ? UINT32_MAX : (uint32_t)cylinders;
	return 0;
}

static const struct disk_ops storage_disk_ops = {
	.submit = storage_submit,
	.ioctl = storage_ioctl
};

static int storage_attach(struct drv_usb_interface *interface,
	const struct drv_usb_id *id)
{
	struct usb_storage *storage;
	struct disk *disk;
	uint8_t maximum_lun = 0;
	size_t actual = 0;
	int error;
	(void)id;

	storage = hal_malloc(sizeof(*storage));
	if (storage == NULL)
		return ENOMEM;
	memset(storage, 0, sizeof(*storage));
	storage->interface = interface;
	storage->device = drv_usb_interface_device(interface);
	storage->bulk_in = drv_usb_interface_find_endpoint(interface,
	    DRV_USB_TRANSFER_BULK, DRV_USB_DIR_IN, NULL);
	storage->bulk_out = drv_usb_interface_find_endpoint(interface,
	    DRV_USB_TRANSFER_BULK, DRV_USB_DIR_OUT, NULL);
	if (storage->bulk_in == NULL || storage->bulk_out == NULL) {
		hal_free(storage);
		return ENODEV;
	}
	(void)mutex_init(&storage->lock, LOCK_RANK_DISK, "usb-storage");
	if (drv_usb_control(storage->device,
	    DRV_USB_DIR_IN | DRV_USB_REQUEST_CLASS | DRV_USB_RECIP_INTERFACE,
	    USB_MASS_STORAGE_GET_MAX_LUN, 0,
	    drv_usb_interface_number(interface), &maximum_lun, 1, 1000U,
	    &actual) == 0 && actual == 1 && maximum_lun != 0)
		hal_printf("usb-storage: only LUN 0 of %u is supported\n",
		    (unsigned)maximum_lun + 1U);
	error = scsi_probe(storage);
	if (error != 0) {
		hal_free(storage);
		return error;
	}
	disk = disk_alloc();
	if (disk == NULL) {
		hal_free(storage);
		return ENOSPC;
	}
	error = disk_alloc_sd_name(disk);
	if (error != 0) {
		(void)disk_destroy(disk);
		hal_free(storage);
		return error;
	}
	disk->d_flags = DISK_REMOVABLE |
	    (storage->write_protected ? DISK_READ_ONLY : 0);
	disk->d_block_size = storage->block_size;
	disk->d_block_count = storage->block_count;
	disk->d_max_transfer_blocks = 16U;
	disk->d_ops = &storage_disk_ops;
	disk->d_data = storage;
	storage->disk = disk;
	error = disk_create(disk);
	if (error != 0) {
		(void)disk_destroy(disk);
		hal_free(storage);
		return error;
	}
	(void)drv_usb_interface_set_driver_data(interface, storage);
	hal_printf("usb-storage: %s blocks=%u block-size=%u%s\n", disk->d_name,
	    (uint32_t)disk->d_block_count, disk->d_block_size,
	    storage->write_protected ? " read-only" : "");
	return 0;
}

static int storage_detach(struct drv_usb_interface *interface, unsigned flags)
{
	struct usb_storage *storage = drv_usb_interface_driver_data(interface);
	int error;
	(void)flags;
	if (storage == NULL)
		return 0;
	error = disk_gone_if_idle(storage->disk);
	if (error != 0)
		return error;
	error = disk_destroy(storage->disk);
	if (error != 0)
		return error;
	hal_free(storage);
	return 0;
}

static const struct drv_usb_id storage_ids[] = {{
	.match_flags = DRV_USB_ID_IF_CLASS | DRV_USB_ID_IF_SUBCLASS |
	    DRV_USB_ID_IF_PROTOCOL,
	.interface_class = USB_MASS_STORAGE_CLASS,
	.interface_subclass = USB_MASS_STORAGE_SCSI,
	.interface_protocol = USB_MASS_STORAGE_BULK_ONLY
}};

static struct drv_usb_driver storage_driver = {
	.name = "usb-storage",
	.ids = storage_ids,
	.id_count = sizeof(storage_ids) / sizeof(storage_ids[0]),
	.attach = storage_attach,
	.detach = storage_detach
};

int drv_usb_storage_driver_register(void)
{
	return drv_usb_driver_register(&storage_driver);
}
