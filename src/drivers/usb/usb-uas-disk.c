/*
 * USB Attached SCSI disk class, independent of Bulk-Only Transport.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 */
#include <drivers/usb.h>
#include <drivers/usb-uas.h>
#include <drivers/usb-storage-scsi.h>
#include <kern/disk.h>
#include <kern/partition.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <hal/hal.h>
#include <errno.h>
#include <string.h>

/* A completed probe description, separate from the published disk state. */
struct uas_media {
	uint8_t inquiry[36];
	enum drv_usb_scsi_flush_policy policy;
	uint64_t blocks;
	uint32_t block_size;
	int write_protected;
};

/* Interface owns this object until disk retirement and checked URB stop.
 * lock serializes commands; control_lock serializes detach/quiesce. */
struct uas_disk {
	struct drv_usb_device *device;
	struct drv_usb_endpoint *pipes[4];
	uint8_t inquiry[36];
	struct drv_usb_uas_transport transport;
	struct disk *disk;
	struct mutex lock;
	struct mutex control_lock;
	enum drv_usb_scsi_flush_policy policy;
	uint64_t blocks;
	uint32_t block_size;
	int write_protected;
	int flush_error;
	int retired;
	int reset_probe;
	int removable;
	int media_pending;
	int partitions_pending;
	int media_recovery_attempted;
	struct thread *control_worker;
	volatile unsigned control_ready;
	volatile unsigned control_stopping;
};

static int uas_attach(struct drv_usb_interface *, const struct drv_usb_id *);
static int uas_detach(struct drv_usb_interface *, unsigned);
static int uas_quiesce(struct drv_usb_interface *);
static int uas_submit(struct disk *, struct bio *);
static int uas_ioctl(struct disk *, unsigned long, void *);
static int uas_probe(struct uas_disk *, struct uas_media *, int);
static int uas_reset_recover(struct uas_disk *);
static int uas_publish_media(struct uas_disk *, const struct uas_media *);
static int uas_command(struct uas_disk *, const void *, size_t, void *, size_t,
    enum drv_usb_uas_direction, struct drv_usb_uas_result *, struct drv_usb_scsi_sense *);
static int uas_control_step(struct uas_disk *);
static int uas_media_transport_recover(struct uas_disk *);
static void uas_control_worker(void *);
static int uas_control_stop(struct uas_disk *);
static uint32_t uas_be32(const uint8_t *);

/* Static class matching excludes BOT. Attach checks speed before allocating. */
static const struct drv_usb_id uas_ids[] = {
	{ .match_flags = DRV_USB_ID_IF_CLASS | DRV_USB_ID_IF_SUBCLASS | DRV_USB_ID_IF_PROTOCOL,
	  .interface_class = 8, .interface_subclass = 6, .interface_protocol = 0x62 }
};
/* Published only after capacity and persistence policy have been established. */
static const struct disk_ops uas_disk_ops = { .submit = uas_submit, .ioctl = uas_ioctl };
/* Registry retains this static class table for the life of the USB subsystem. */
static struct drv_usb_driver uas_driver = {
	.name = "usb-uas", .ids = uas_ids, .id_count = 1,
	.attach = uas_attach, .detach = uas_detach, .quiesce = uas_quiesce
};

int
drv_usb_uas_driver_register(void)
{
	return drv_usb_driver_register(&uas_driver);
}

static uint32_t
uas_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	    ((uint32_t)p[2] << 8) | p[3];
}

static int
uas_command(struct uas_disk *owner, const void *cdb, size_t cdb_length,
    void *buffer, size_t length, enum drv_usb_uas_direction direction,
    struct drv_usb_uas_result *result, struct drv_usb_scsi_sense *sense)
{
	struct drv_usb_scsi_sense decoded;
	enum drv_usb_scsi_recovery action;
	int owned_reset;
	int error;

	memset(&decoded, 0, sizeof(decoded));
	if (sense != NULL)
		memset(sense, 0, sizeof(*sense));
	if (owner->disk != NULL) {
		error = disk_media_status(owner->disk);
		if (error != 0)
			return error;
	}
	error = drv_usb_uas_transport_execute(&owner->transport, 0, cdb,
	    cdb_length, direction, buffer, length, 5000, result);
	if (error != 0) {
		/* A reset cannot prove that a removable LUN still contains this medium. */
		if (owner->removable && owner->transport.stopped) {
			owner->media_pending = 1;
			if (owner->disk != NULL)
				disk_media_revoke(owner->disk);
		}
		return error;
	}
	if (result->status == 0)
		return 0;
	if (result->status == 2)
		(void)drv_usb_scsi_parse_sense(result->sense, result->sense_length, &decoded);
	if (sense != NULL)
		*sense = decoded;
	action = drv_usb_scsi_recovery_action(&decoded);
	if (action == DRV_USB_SCSI_RECOVERY_MEDIA ||
	    action == DRV_USB_SCSI_RECOVERY_ABSENT)
		owner->media_pending = 1;
	owned_reset = owner->reset_probe && cdb_length != 0 &&
	    ((const uint8_t *)cdb)[0] == 0 && action == DRV_USB_SCSI_RECOVERY_RESET;
	if (owner->disk != NULL && action != DRV_USB_SCSI_RECOVERY_NONE && !owned_reset)
		disk_media_revoke(owner->disk);
	return EIO;
}

static int
uas_probe(struct uas_disk *owner, struct uas_media *media, int recovering)
{
	uint8_t inquiry_cdb[6] = { 0x12, 0, 0, 0, 36, 0 };
	uint8_t ready_cdb[6] = { 0 };
	uint8_t capacity_cdb[10] = { 0x25 };
	uint8_t capacity16_cdb[16] = { 0x9e, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32 };
	uint8_t mode_cdb[6] = { 0x1a, 8, 8, 0, 192, 0 };
	uint8_t sync_cdb[10] = { 0x35 };
	uint8_t buffer[192];
	struct drv_usb_uas_result result;
	struct drv_usb_scsi_sense sense;
	struct drv_usb_scsi_cache_info cache;
	struct uas_media candidate;
	uint64_t last;
	unsigned attempt;
	int error;

	memset(&candidate, 0, sizeof(candidate));
	error = uas_command(owner, inquiry_cdb, sizeof(inquiry_cdb), buffer, 36,
	    DRV_USB_UAS_READ, &result, &sense);
	if (error != 0)
		return error;
	if (result.transferred < 36 || (buffer[0] & 0x1f) != 0)
		return ENODEV;
	memcpy(candidate.inquiry, buffer, sizeof(candidate.inquiry));
	/* Device capability remains available even when no medium is inserted. */
	owner->removable = (buffer[1] & 0x80) != 0;
	for (attempt = 0; attempt < 3; attempt++) {
		error = uas_command(owner, ready_cdb, sizeof(ready_cdb), NULL, 0,
		    DRV_USB_UAS_NO_DATA, &result, &sense);
		if (error == 0)
			break;
		/* Only initial UNIT ATTENTION allows readiness retry, never writes. */
		if (!sense.valid || sense.key != 6 || owner->transport.stopped)
			return error;
		if (recovering && drv_usb_scsi_recovery_action(&sense) != DRV_USB_SCSI_RECOVERY_RESET)
			return error;
	}
	if (error != 0)
		return error;
	error = uas_command(owner, capacity_cdb, sizeof(capacity_cdb), buffer, 8,
	    DRV_USB_UAS_READ, &result, &sense);
	if (error != 0)
		return error;
	if (result.transferred != 8)
		return EIO;
	last = uas_be32(buffer);
	candidate.block_size = uas_be32(buffer + 4);
	if (last == UINT32_MAX) {
		error = uas_command(owner, capacity16_cdb, sizeof(capacity16_cdb), buffer, 32,
		    DRV_USB_UAS_READ, &result, &sense);
		if (error != 0)
			return error;
		if (result.transferred != 32)
			return EIO;
		last = ((uint64_t)uas_be32(buffer) << 32) | uas_be32(buffer + 4);
		candidate.block_size = uas_be32(buffer + 8);
	}
	if (last == UINT64_MAX || candidate.block_size == 0 ||
	    candidate.block_size > owner->transport.capacity)
		return EOVERFLOW;
	candidate.blocks = last + 1;
	memset(&cache, 0, sizeof(cache));
	error = uas_command(owner, mode_cdb, sizeof(mode_cdb), buffer, sizeof(buffer),
	    DRV_USB_UAS_READ, &result, &sense);
	if (error == 0)
		(void)drv_usb_scsi_parse_mode_sense6_cache(buffer, result.transferred, &cache);
	if (owner->transport.stopped ||
	    drv_usb_scsi_recovery_action(&sense) != DRV_USB_SCSI_RECOVERY_NONE)
		return error;
	candidate.write_protected = cache.write_protected;
	error = uas_command(owner, sync_cdb, sizeof(sync_cdb), NULL, 0,
	    DRV_USB_UAS_NO_DATA, &result, &sense);
	if (owner->transport.stopped ||
	    drv_usb_scsi_recovery_action(&sense) != DRV_USB_SCSI_RECOVERY_NONE)
		return error;
	candidate.policy = drv_usb_scsi_select_flush_policy(&cache, error == 0, &sense);
	*media = candidate;
	return 0;
}

/* Called with the disk lock held; teardown waits for this same owner. */
static int
uas_reset_recover(struct uas_disk *owner)
{
	struct uas_media media;
	size_t capacity;
	int error;

	if (owner->transport.recovery_attempted || owner->transport.failed_tag == 0)
		return EIO;
	owner->transport.recovery_attempted = 1;
	/* A removable SCSI medium needs generation retirement, not geometry matching. */
	if ((owner->inquiry[1] & 0x80) != 0)
		return EOPNOTSUPP;
	capacity = owner->transport.capacity;
	error = drv_usb_uas_transport_stop(&owner->transport);
	if (error != 0)
		return error;
	error = drv_usb_device_reset(owner->device);
	if (error != 0)
		return error;
	error = drv_usb_uas_transport_init(&owner->transport, owner->device,
	    owner->pipes, capacity);
	if (error != 0)
		goto failed;
	owner->reset_probe = 1;
	error = uas_probe(owner, &media, 1);
	owner->reset_probe = 0;
	if (error != 0)
		goto failed;
	if (memcmp(media.inquiry, owner->inquiry, sizeof(media.inquiry)) != 0 ||
	    media.blocks != owner->blocks || media.block_size != owner->block_size ||
	    media.policy != owner->policy || media.write_protected != owner->write_protected) {
		error = ENODEV;
		goto failed;
	}
	/* Physical identity was checked by USB reset; the fixed LUN is unchanged. */
	return 0;

failed:
	if (owner->disk != NULL)
		disk_media_revoke(owner->disk);
	owner->transport.stopped = 1;
	owner->transport.recovery_attempted = 1;
	return error;
}

static int
uas_submit(struct disk *disk, struct bio *bio)
{
	struct uas_disk *owner;
	struct drv_usb_uas_result result;
	struct drv_usb_scsi_sense sense;
	uint8_t cdb[16];
	uint64_t lba;
	uint32_t count;
	size_t expected;
	unsigned i;
	int error;

	owner = disk->d_data;
	expected = 0;
	memset(cdb, 0, sizeof(cdb));
	mutex_lock(&owner->lock);
	if (owner->disk != NULL) {
		error = disk_media_status(owner->disk);
		if (error != 0)
			goto done;
	}
	if (owner->transport.stopped) {
		if (owner->transport.super_speed)
			error = uas_reset_recover(owner);
		else
			error = drv_usb_uas_transport_recover(&owner->transport, 5000);
		if (error != 0)
			goto done;
	}
	if (bio->b_op == BIO_FLUSH) {
		error = owner->flush_error;
		if (error != 0)
			goto done;
		if (drv_usb_scsi_flush_policy_uses_sync_cache(owner->policy)) {
			cdb[0] = 0x35;
			error = uas_command(owner, cdb, 10, NULL, 0,
			    DRV_USB_UAS_NO_DATA, &result, &sense);
			drv_usb_scsi_record_flush_result(owner->policy, error, &owner->flush_error);
		} else if (!drv_usb_scsi_flush_policy_allows_write(owner->policy)) {
			error = EOPNOTSUPP;
		}
		goto done;
	}
	if (bio->b_op != BIO_READ && bio->b_op != BIO_WRITE) {
		error = EOPNOTSUPP;
		goto done;
	}
	if (bio->b_op == BIO_WRITE && (owner->write_protected ||
	    !drv_usb_scsi_flush_policy_allows_write(owner->policy) || owner->flush_error)) {
		error = owner->flush_error ? owner->flush_error : EROFS;
		goto done;
	}
	lba = bio->b_mapped_block;
	count = bio->b_block_count;
	if (count == 0 || lba >= owner->blocks || count > owner->blocks - lba ||
	    count > owner->transport.capacity / owner->block_size) {
		error = EINVAL;
		goto done;
	}
	expected = (size_t)count * owner->block_size;
	/* READ/WRITE(16) also covers media beyond the READ CAPACITY(10) limit. */
	cdb[0] = bio->b_op == BIO_WRITE ? 0x8a : 0x88;
	if (bio->b_op == BIO_WRITE && owner->policy == DRV_USB_SCSI_FLUSH_FUA)
		cdb[1] = 8;
	for (i = 0; i < 8; i++)
		cdb[2 + i] = (uint8_t)(lba >> ((7 - i) * 8));
	for (i = 0; i < 4; i++)
		cdb[10 + i] = (uint8_t)(count >> ((3 - i) * 8));
	error = uas_command(owner, cdb, sizeof(cdb), bio->b_data, expected,
	    bio->b_op == BIO_READ ? DRV_USB_UAS_READ : DRV_USB_UAS_WRITE, &result, &sense);
	if (error == 0 && result.transferred != expected)
		error = EIO;
	/* A later successful abort or flush cannot certify an uncertain write. */
	if (error != 0 && bio->b_op == BIO_WRITE && owner->flush_error == 0)
		owner->flush_error = error;

done:
	mutex_unlock(&owner->lock);
	bio_complete(bio, error, error == 0 ? expected : 0);
	return 0;
}

/* Synthetic CHS geometry for partition consumers; capacity remains 64-bit. */
static int
uas_ioctl(struct disk *disk, unsigned long request, void *argument)
{
	struct disk_geometry *geometry;
	uint64_t cylinders;

	geometry = argument;
	if (request != DISK_IOCTL_GET_GEOMETRY || geometry == NULL)
		return EOPNOTSUPP;
	geometry->heads = 255U;
	geometry->sectors_per_track = 63U;
	cylinders = disk->d_block_count / (255U * 63U);
	geometry->cylinders = cylinders > UINT32_MAX ? UINT32_MAX : (uint32_t)cylinders;
	return 0;
}

/* Disk mutex prevents admitted BIOs from observing partial publication. */
static int
uas_publish_media(struct uas_disk *owner, const struct uas_media *media)
{
	struct disk *disk;
	int error;

	if (owner->disk != NULL)
		return EBUSY;
	if (media == NULL || media->blocks == 0 || media->block_size == 0 ||
	    media->block_size > owner->transport.capacity)
		return EINVAL;
	disk = disk_alloc();
	if (disk == NULL)
		return ENOSPC;
	error = disk_alloc_sd_name(disk);
	if (error != 0)
		goto failed;
	disk->d_flags = DISK_REMOVABLE;
	if (drv_usb_scsi_flush_policy_requires_read_only(media->policy, media->write_protected))
		disk->d_flags |= DISK_READ_ONLY;
	if (drv_usb_scsi_flush_policy_allows_write(media->policy))
		disk->d_flags |= DISK_FLUSH_PROOF;
	disk->d_block_size = media->block_size;
	disk->d_block_count = media->blocks;
	disk->d_max_transfer_blocks = owner->transport.capacity / media->block_size;
	disk->d_ops = &uas_disk_ops;
	disk->d_data = owner;
	error = disk_create(disk);
	if (error != 0)
		goto failed;

	memcpy(owner->inquiry, media->inquiry, sizeof(owner->inquiry));
	owner->blocks = media->blocks;
	owner->block_size = media->block_size;
	owner->policy = media->policy;
	owner->write_protected = media->write_protected;
	owner->flush_error = 0;
	owner->retired = 0;
	owner->media_pending = 0;
	owner->partitions_pending = 1;
	owner->media_recovery_attempted = 0;
	owner->disk = disk;
	hal_printf("usb-uas: %s blocks=%llu block-size=%u policy=%u %s\n",
	    disk->d_name, (unsigned long long)owner->blocks, owner->block_size,
	    (unsigned)owner->policy, owner->transport.super_speed ? "super-speed" : "high-speed");
	return 0;

failed:
	(void)disk_destroy(disk);
	return error;
}

/* Reset a closed transport only after complete old-medium retirement. */
static int
uas_media_transport_recover(struct uas_disk *owner)
{
	size_t capacity;
	int error;

	if (owner->disk != NULL)
		return EBUSY;
	if (owner->media_recovery_attempted)
		return EIO;
	owner->media_recovery_attempted = 1;
	capacity = owner->transport.capacity;
	error = drv_usb_uas_transport_stop(&owner->transport);
	if (error != 0)
		return error;
	error = drv_usb_device_reset(owner->device);
	if (error != 0)
		return error;
	error = drv_usb_uas_transport_init(&owner->transport, owner->device,
	    owner->pipes, capacity);
	if (error != 0)
		owner->transport.stopped = 1;
	return error;
}

/* Caller owns control_lock; old references must retire before fresh CDBs. */
static int
uas_control_step(struct uas_disk *owner)
{
	uint8_t ready[6] = { 0 };
	struct drv_usb_uas_result result;
	struct uas_media media;
	int error;

	if (drv_usb_device_state(owner->device) != DRV_USB_STATE_CONFIGURED ||
	    drv_usb_device_is_tearing_down(owner->device))
		return ENODEV;
	mutex_lock(&owner->lock);
	if (owner->disk != NULL && !owner->media_pending) {
		error = uas_command(owner, ready, sizeof(ready), NULL, 0,
		    DRV_USB_UAS_NO_DATA, &result, NULL);
		goto done;
	}
	if (!owner->media_pending) {
		error = EIO;
		goto done;
	}
	if (owner->disk != NULL) {
		if (!owner->retired) {
			error = partition_retire_media(owner->disk);
			if (error != 0)
				goto done;
			owner->retired = 1;
		}
		error = disk_destroy(owner->disk);
		if (error != 0)
			goto done;
		owner->disk = NULL;
		owner->retired = 0;
	}
	if (owner->transport.stopped) {
		error = uas_media_transport_recover(owner);
		if (error != 0)
			goto done;
	}
	error = uas_probe(owner, &media, 0);
	if (error == 0)
		error = uas_publish_media(owner, &media);

done:
	mutex_unlock(&owner->lock);
	/* Reload issues BIOs: control ownership protects lifetime, not command lock. */
	if (error == 0 && owner->partitions_pending) {
		/* The reload API requires one administrative open. */
		error = disk_open(owner->disk);
		if (error != 0)
			return error;
		error = partition_reload(owner->disk);
		disk_close(owner->disk);
		if (error == 0 || error == EINVAL || error == EOPNOTSUPP)
			owner->partitions_pending = 0;
		/* A whole-disk filesystem remains usable without a partition table. */
		if (error == EINVAL || error == EOPNOTSUPP)
			error = 0;
	}
	return error;
}

static void
uas_control_worker(void *argument)
{
	struct uas_disk *owner;

	owner = argument;
	while (!atomic_raw_load_acquire(&owner->control_ready)) {
		if (atomic_raw_load_acquire(&owner->control_stopping))
			return;
		kernel_wait_task();
	}
	while (!atomic_raw_load_acquire(&owner->control_stopping)) {
		mutex_lock(&owner->control_lock);
		if (!atomic_raw_load_acquire(&owner->control_stopping))
			(void)uas_control_step(owner);
		mutex_unlock(&owner->control_lock);
		if (!atomic_raw_load_acquire(&owner->control_stopping))
			sched_sleep(sched_ticks() + 100U);
	}
}

/* No mutex may be held while joining the worker that needs those mutexes. */
static int
uas_control_stop(struct uas_disk *owner)
{
	struct thread *worker;
	uint64_t deadline;
	int error;

	worker = owner->control_worker;
	if (worker == NULL)
		return 0;
	atomic_raw_store_release(&owner->control_stopping, 1U);
	kernel_notify_task(worker->task);
	deadline = sched_ticks() + 2000U;
	while (atomic_raw_load_acquire((volatile unsigned *)&worker->state) != THREAD_ZOMBIE) {
		if (sched_ticks() >= deadline)
			return EBUSY;
		sched_yield();
	}
	error = thread_wait(worker, NULL);
	if (error == 0)
		owner->control_worker = NULL;
	return error;
}

static int
uas_attach(struct drv_usb_interface *interface, const struct drv_usb_id *id)
{
	struct drv_usb_device *device;
	struct drv_usb_endpoint *pipes[4];
	struct drv_usb_endpoint *endpoint;
	const struct drv_usb_interface_descriptor *descriptor;
	struct drv_usb_uas_capabilities capabilities;
	struct uas_media media;
	struct uas_disk *owner;
	const void *raw;
	size_t raw_length;
	unsigned i;
	unsigned j;
	int error;

	(void)id;
	device = drv_usb_interface_device(interface);
	if (drv_usb_device_speed(device) != DRV_USB_SPEED_HIGH &&
	    drv_usb_device_speed(device) != DRV_USB_SPEED_SUPER)
		return ENODEV;
	descriptor = drv_usb_interface_descriptor(interface);
	raw = drv_usb_configuration_raw_descriptors(drv_usb_device_active_configuration(device), &raw_length);
	error = drv_usb_uas_decode_configuration(raw, raw_length,
	    descriptor->interface_number, descriptor->alternate_setting,
	    drv_usb_device_speed(device) == DRV_USB_SPEED_SUPER ?
	    DRV_USB_UAS_SUPER_SPEED : DRV_USB_UAS_HIGH_SPEED, &capabilities);
	if (error != 0)
		return error;
	memset(pipes, 0, sizeof(pipes));
	for (i = 0; i < drv_usb_interface_endpoint_count(interface); i++) {
		endpoint = drv_usb_interface_endpoint(interface, i);
		for (j = 0; j < 4; j++) {
			if (drv_usb_endpoint_descriptor(endpoint)->address == capabilities.pipes[j].address)
				pipes[j] = endpoint;
		}
	}
	owner = hal_malloc(sizeof(*owner));
	if (owner == NULL)
		return ENOMEM;
	memset(owner, 0, sizeof(*owner));
	owner->device = device;
	for (i = 0; i < 4; i++)
		owner->pipes[i] = pipes[i];
	(void)mutex_init(&owner->lock, LOCK_RANK_DISK, "usb-uas");
	(void)mutex_init(&owner->control_lock, LOCK_RANK_DEVICE, "usb-uas control");
	error = drv_usb_uas_transport_init(&owner->transport, device, pipes,
	    DRV_USB_TRANSFER_RESERVE_MAX_SIZE);
	if (error != 0)
		goto fail;
	error = uas_probe(owner, &media, 0);
	if (error != 0 && (!owner->removable || !owner->media_pending ||
	    owner->transport.stopped))
		goto fail;
	/* Allocate the monitor before any disk can escape into the registry. */
	if (owner->removable) {
		int start_error;

		start_error = kthread_create(uas_control_worker, owner,
		    SCHED_PRIORITY_DEFAULT, &owner->control_worker);
		if (start_error != 0) {
			error = start_error;
			goto fail;
		}
		thread_start(owner->control_worker);
	}
	if (error == 0) {
		mutex_lock(&owner->lock);
		error = uas_publish_media(owner, &media);
		mutex_unlock(&owner->lock);
		if (error != 0)
			goto fail;
	}
	(void)drv_usb_interface_set_driver_data(interface, owner);
	atomic_raw_store_release(&owner->control_ready, 1U);
	if (owner->control_worker != NULL)
		kernel_notify_task(owner->control_worker->task);
	return 0;

fail:
	/* USB can retain DMA after a failed wait: retain a class binding for stop. */
	if (uas_control_stop(owner) != 0 ||
	    drv_usb_uas_transport_stop(&owner->transport) != 0) {
		(void)drv_usb_interface_set_driver_data(interface, owner);
		hal_printf("usb-uas: attach error=%d; owner retained for stop\n", error);
		return 0;
	}
	hal_free(owner);
	return error;
}

static int
uas_quiesce(struct drv_usb_interface *interface)
{
	struct uas_disk *owner;
	int error;

	owner = drv_usb_interface_driver_data(interface);
	if (owner == NULL)
		return 0;
	error = uas_control_stop(owner);
	if (error != 0)
		return error;
	mutex_lock(&owner->control_lock);
	mutex_lock(&owner->lock);
	error = drv_usb_uas_transport_stop(&owner->transport);
	mutex_unlock(&owner->lock);
	mutex_unlock(&owner->control_lock);
	return error;
}

static int
uas_detach(struct drv_usb_interface *interface, unsigned flags)
{
	struct uas_disk *owner;
	int error;

	(void)flags;
	owner = drv_usb_interface_driver_data(interface);
	if (owner == NULL)
		return 0;
	error = uas_control_stop(owner);
	if (error != 0)
		return error;
	mutex_lock(&owner->control_lock);
	if (owner->disk != NULL) {
		/* Lost media cannot satisfy a flush during normal idle retirement. */
		if (drv_usb_device_is_tearing_down(owner->device))
			disk_media_revoke(owner->disk);
		if (!owner->retired) {
			error = disk_media_status(owner->disk) != 0 ?
			    partition_retire_media(owner->disk) : disk_gone_if_idle(owner->disk);
			if (error != 0)
				goto done;
			owner->retired = 1;
		}
		error = disk_destroy(owner->disk);
		if (error != 0)
			goto done;
		owner->disk = NULL;
	}
	mutex_lock(&owner->lock);
	error = drv_usb_uas_transport_stop(&owner->transport);
	mutex_unlock(&owner->lock);

done:
	mutex_unlock(&owner->control_lock);
	if (error == 0) {
		(void)drv_usb_interface_set_driver_data(interface, NULL);
		hal_free(owner);
	}
	return error;
}
