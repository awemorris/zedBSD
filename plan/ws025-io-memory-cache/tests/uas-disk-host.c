/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <assert.h>
#include <stdio.h>
#include <sys/types.h>
typedef int tid_t;
#define sigset_t zedbsd_sigset_t
#include "../../../src/drivers/usb/usb-uas-disk.c"

static unsigned commands, recoveries;
static int retire_busy, destroy_busy;
static int media_revoked, lock_held;
static unsigned expected_blocks = 200;
static int reload_error;
static unsigned reload_calls;
static int admin_open, open_error;
int disk_open(struct disk *d)
{ assert(d && !lock_held && !admin_open); if (open_error) return open_error; admin_open = 1; return 0; }
void disk_close(struct disk *d)
{ assert(d && !lock_held && admin_open); admin_open = 0; }
int partition_reload(struct disk *d)
{ assert(d && !lock_held && admin_open); reload_calls++; return reload_error; }
enum drv_usb_device_state drv_usb_device_state(const struct drv_usb_device *d)
{ (void)d; return DRV_USB_STATE_CONFIGURED; }
int drv_usb_device_is_tearing_down(const struct drv_usb_device *d)
{ (void)d; return 0; }
int partition_retire_media(struct disk *d)
{ assert(d && lock_held && media_revoked); return retire_busy ? EBUSY : 0; }
static unsigned probe_failure;
static int probing;
static int sense_enabled;
static unsigned sense_key, sense_asc, sense_ascq;
static unsigned reset_stage, reset_failure;
static struct disk published_disk;
static unsigned publication_failure, publication_destroyed;
static int command_error, completion_error;
static size_t completion_bytes, short_bytes;

void mutex_lock(struct mutex *m) { (void)m; assert(!lock_held); lock_held = 1; }
void mutex_unlock(struct mutex *m) { (void)m; assert(lock_held); lock_held = 0; }
void bio_complete(struct bio *bio, int error, size_t bytes)
{ (void)bio; assert(!lock_held); completion_error = error; completion_bytes = bytes; }
int drv_usb_uas_transport_execute(struct drv_usb_uas_transport *t, unsigned lun,
    const void *cdb, size_t cdb_length, enum drv_usb_uas_direction direction,
    void *buffer, size_t length, unsigned timeout, struct drv_usb_uas_result *result)
{
	(void)lun; (void)cdb; (void)cdb_length; (void)direction; (void)buffer;
	assert(timeout && lock_held); commands++;
	memset(result, 0, sizeof(*result));
	if (sense_enabled) {
		result->status = 2;
		result->sense_length = 18;
		result->sense[0] = 0x70;
		result->sense[2] = sense_key;
		result->sense[7] = 10;
		result->sense[12] = sense_asc;
		result->sense[13] = sense_ascq;
		return 0;
	}
	if (probing) {
		if (commands == probe_failure) {
			t->stopped = 1;
			return ETIMEDOUT;
		}
		if (length != 0)
			memset(buffer, 0, length);
		if (((const unsigned char *)cdb)[0] == 0x25) {
			unsigned char *bytes = buffer;
			bytes[3] = 99;
			bytes[6] = 2;
		}
		result->transferred = length;
		return 0;
	}

	if (command_error) { t->stopped = 1; return command_error; }
	result->transferred = length - short_bytes;
	return 0;
}
int drv_usb_uas_transport_recover(struct drv_usb_uas_transport *t, unsigned timeout)
{ assert(lock_held && timeout); recoveries++; t->stopped = 0; return 0; }

int drv_usb_device_reset(struct drv_usb_device *d)
{ (void)d; assert(reset_stage == 1); reset_stage++; return reset_failure == 2 ? EIO : 0; }
int drv_usb_uas_transport_stop(struct drv_usb_uas_transport *t)
{ assert(reset_stage == 0); reset_stage++; t->stopped = 1; t->failed_tag = 0; return reset_failure == 1 ? EIO : 0; }
int drv_usb_uas_transport_init(struct drv_usb_uas_transport *t, struct drv_usb_device *d,
    struct drv_usb_endpoint *p[4], size_t capacity)
{ (void)d; (void)p; assert(capacity == 65536 && reset_stage == 2);
  reset_stage++; memset(t, 0, sizeof(*t)); t->capacity = capacity; t->super_speed = 1;
  return reset_failure == 3 ? ENOMEM : 0; }

void disk_media_revoke(struct disk *d) { assert(d); media_revoked = 1; }
int disk_media_status(const struct disk *d) { assert(d); return media_revoked ? ENODEV : 0; }

int hal_printf(const char *format, ...) { (void)format; return 0; }
struct disk *disk_alloc(void)
{ assert(lock_held); memset(&published_disk, 0, sizeof(published_disk)); return publication_failure == 1 ? NULL : &published_disk; }
int disk_alloc_sd_name(struct disk *d)
{ assert(d == &published_disk && lock_held); return publication_failure == 2 ? ENOSPC : 0; }
int disk_create(struct disk *d)
{ assert(d == &published_disk && lock_held && d->d_block_size == 512 && d->d_block_count == expected_blocks); return publication_failure == 3 ? EIO : 0; }
int disk_destroy(struct disk *d)
{ assert(d == &published_disk && lock_held); publication_destroyed++; return destroy_busy ? EBUSY : 0; }

int main(void)
{
	struct uas_disk owner;
	struct disk disk;
	struct bio bio;
	unsigned scenario;

	for (scenario = 0; scenario < 3; scenario++) {
		memset(&owner, 0, sizeof(owner)); memset(&disk, 0, sizeof(disk));
		memset(&bio, 0, sizeof(bio));
		commands = recoveries = short_bytes = 0;
		owner.policy = DRV_USB_SCSI_FLUSH_SYNC_CACHE;
		owner.block_size = 512; owner.blocks = 1000; owner.transport.capacity = 65536;
		disk.d_data = &owner; bio.b_block_count = 1;
		bio.b_op = scenario == 0 ? BIO_READ : BIO_WRITE;
		command_error = scenario == 2 ? 0 : ETIMEDOUT;
		short_bytes = scenario == 2 ? 1 : 0;
		assert(uas_submit(&disk, &bio) == 0);
		assert(completion_error != 0 && completion_bytes == 0 && commands == 1);
		command_error = 0; short_bytes = 0;
		bio.b_op = BIO_READ;
		assert(uas_submit(&disk, &bio) == 0);
		assert(completion_error == 0 && completion_bytes == 512 && commands == 2);
		assert(recoveries == (scenario == 2 ? 0 : 1));
		bio.b_op = BIO_FLUSH;
		assert(uas_submit(&disk, &bio) == 0);
		if (scenario == 0) {
			assert(completion_error == 0 && commands == 3);
		} else {
			assert(completion_error != 0 && commands == 2);
			bio.b_op = BIO_WRITE;
			assert(uas_submit(&disk, &bio) == 0);
			assert(completion_error != 0 && commands == 2);
		}
	}
	/* Probe failures cannot publish a partial capacity or clear write errors. */
	for (probe_failure = 0; probe_failure <= 5; probe_failure++) {
		struct uas_media media;
		struct uas_media before;
		int error;

		memset(&owner, 0, sizeof(owner));
		owner.blocks = 1234;
		owner.block_size = 4096;
		owner.policy = DRV_USB_SCSI_FLUSH_SYNC_CACHE;
		owner.write_protected = 1;
		owner.flush_error = EIO;
		owner.transport.capacity = 65536;
		memset(&media, 0x5a, sizeof(media));
		memcpy(&before, &media, sizeof(before));
		commands = 0;
		probing = 1;
		mutex_lock(&owner.lock);
		error = uas_probe(&owner, &media, 0);
		mutex_unlock(&owner.lock);
		probing = 0;
		assert(owner.blocks == 1234 && owner.block_size == 4096);
		assert(owner.write_protected == 1 && owner.flush_error == EIO);
		assert(owner.policy == DRV_USB_SCSI_FLUSH_SYNC_CACHE);
		if (probe_failure != 0) {
			assert(error != 0);
			assert(memcmp(&media, &before, sizeof(media)) == 0);
		} else {
			assert(error == 0 && media.blocks == 100 && media.block_size == 512);
			assert(commands == 5);
		}
	}
	for (reset_failure = 0; reset_failure <= 5; reset_failure++) {
		int error;

		memset(&owner, 0, sizeof(owner));
		owner.blocks = reset_failure == 5 ? 101 : 100;
		owner.block_size = 512;
		owner.policy = DRV_USB_SCSI_FLUSH_SYNC_CACHE;
		owner.flush_error = EIO;
		owner.transport.capacity = 65536;
		owner.transport.super_speed = 1;
		owner.transport.stopped = 1;
		owner.transport.failed_tag = 1;
		reset_stage = commands = 0;
		probe_failure = reset_failure == 4 ? 3 : 0;
		probing = 1;
		mutex_lock(&owner.lock);
		error = uas_reset_recover(&owner);
		mutex_unlock(&owner.lock);
		probing = 0;
		assert(owner.flush_error == EIO);
		assert(owner.blocks == (reset_failure == 5 ? 101U : 100U));
		if (reset_failure == 0) {
			assert(error == 0 && !owner.transport.stopped && reset_stage == 3);
		} else {
			assert(error != 0 && owner.transport.stopped);
			assert(uas_reset_recover(&owner) == EIO);
			assert(reset_stage == (reset_failure < 3 ? reset_failure : 3));
		}
	}
	/* Current media/policy attention closes both old and already queued I/O. */
	for (scenario = 0; scenario < 7; scenario++) {
		struct drv_usb_uas_result result;
		unsigned char cdb[6] = {0};
		unsigned before_commands;

		memset(&owner, 0, sizeof(owner));
		memset(&disk, 0, sizeof(disk));
		memset(&bio, 0, sizeof(bio));
		owner.disk = &disk; disk.d_data = &owner;
		owner.flush_error = EIO;
		media_revoked = 0; sense_enabled = 1;
		sense_key = scenario == 1 ? 2 : (scenario == 6 ? 5 : 6);
		sense_asc = scenario == 0 ? 0x28 : scenario == 1 ? 0x3a : scenario == 2 ? 0x2a : 0x29;
		sense_ascq = scenario == 2 ? 1 : 0;
		owner.reset_probe = scenario == 4 || scenario == 5;
		if (scenario == 5) cdb[0] = 0x12;
		mutex_lock(&owner.lock);
		assert(uas_command(&owner, cdb, sizeof(cdb), NULL, 0,
		    DRV_USB_UAS_NO_DATA, &result, NULL) == EIO);
		mutex_unlock(&owner.lock);
		assert(media_revoked == (scenario != 4 && scenario != 6));
		assert(owner.flush_error == EIO);
		if (media_revoked) {
			before_commands = commands;
			assert(uas_submit(&disk, &bio) == 0);
			assert(commands == before_commands && completion_error == ENODEV && completion_bytes == 0);
		}
	}
	sense_enabled = 0;
	for (publication_failure = 0; publication_failure <= 3; publication_failure++) {
		struct uas_media media;
		int error;
		memset(&owner, 0, sizeof(owner));
		memset(&media, 0, sizeof(media));
		owner.blocks = 1234; owner.block_size = 4096; owner.flush_error = EIO;
		owner.transport.capacity = 65536;
		media.blocks = 200; media.block_size = 512;
		media.policy = DRV_USB_SCSI_FLUSH_SYNC_CACHE;
		publication_destroyed = 0;
		mutex_lock(&owner.lock);
		error = uas_publish_media(&owner, &media);
		if (publication_failure == 0) {
			assert(error == 0 && owner.disk == &published_disk);
			assert(owner.blocks == 200 && owner.block_size == 512 && owner.flush_error == 0);
			assert(uas_publish_media(&owner, &media) == EBUSY);
		} else {
			assert(error != 0 && owner.disk == NULL);
			assert(owner.blocks == 1234 && owner.block_size == 4096 && owner.flush_error == EIO);
			assert(publication_destroyed == (publication_failure != 1));
		}
		mutex_unlock(&owner.lock);
	}
	/* A medium change cannot probe or publish while any old reference survives. */
	memset(&owner, 0, sizeof(owner));
	owner.disk = &published_disk; owner.transport.capacity = 65536;
	owner.removable = 1; owner.flush_error = EIO;
	media_revoked = 0; sense_enabled = 1;
	sense_key = 6; sense_asc = 0x28; sense_ascq = 0;
	commands = 0;
	assert(uas_control_step(&owner) == EIO);
	assert(media_revoked && owner.media_pending && commands == 1);
	sense_enabled = 0; probing = 1; probe_failure = 0;
	retire_busy = 1;
	assert(uas_control_step(&owner) == EBUSY);
	assert(commands == 1 && owner.disk && owner.flush_error == EIO);
	retire_busy = 0; destroy_busy = 1;
	assert(uas_control_step(&owner) == EBUSY);
	assert(commands == 1 && owner.retired && owner.disk && owner.flush_error == EIO);
	destroy_busy = 0; publication_failure = 1;
	assert(uas_control_step(&owner) == ENOSPC);
	assert(owner.disk == NULL && owner.media_pending && owner.flush_error == EIO);
	publication_failure = 0; expected_blocks = 100;
	assert(uas_control_step(&owner) == 0);
	assert(owner.disk == &published_disk && !owner.media_pending && !owner.flush_error);
	probing = 0;
	media_revoked = 0;
	assert(reload_calls == 1 && !owner.partitions_pending);
	owner.partitions_pending = 1; reload_error = EBUSY;
	assert(uas_control_step(&owner) == EBUSY && owner.partitions_pending);
	reload_error = EINVAL;
	assert(uas_control_step(&owner) == 0 && !owner.partitions_pending);
	assert(reload_calls == 3 && !admin_open);
	owner.partitions_pending = 1; open_error = EBUSY;
	assert(uas_control_step(&owner) == EBUSY && owner.partitions_pending);
	assert(reload_calls == 3 && !admin_open);
	open_error = 0;
	owner.partitions_pending = 1; reload_error = EOPNOTSUPP;
	assert(uas_control_step(&owner) == 0 && !owner.partitions_pending);
	{
		struct disk_geometry geometry;
		published_disk.d_block_count = UINT64_MAX;
		assert(uas_ioctl(&published_disk, DISK_IOCTL_GET_GEOMETRY, &geometry) == 0);
		assert(geometry.cylinders == UINT32_MAX && geometry.heads == 255 && geometry.sectors_per_track == 63);
		assert(uas_ioctl(&published_disk, DISK_IOCTL_GET_GEOMETRY, NULL) == EOPNOTSUPP);
	}
	/* Reset cannot touch a live old identity; each failed barrier is attempted once. */
	for (reset_failure = 0; reset_failure <= 3; reset_failure++) {
		memset(&owner, 0, sizeof(owner));
		owner.disk = &published_disk;
		owner.transport.capacity = 65536; owner.transport.stopped = 1;
		owner.flush_error = EIO;
		reset_stage = 0;
		assert(uas_media_transport_recover(&owner) == EBUSY && !reset_stage);
		owner.disk = NULL;
		assert((uas_media_transport_recover(&owner) == 0) == (reset_failure == 0));
		assert(reset_stage == (reset_failure == 1 ? 1U : reset_failure == 2 ? 2U : 3U));
		assert(owner.flush_error == EIO && owner.media_recovery_attempted);
		assert(uas_media_transport_recover(&owner) == EIO);
		if (reset_failure) assert(owner.transport.stopped);
	}
	memset(&owner, 0, sizeof(owner));
	owner.removable = 1; owner.disk = &published_disk;
	media_revoked = 0; command_error = ETIMEDOUT;
	{
		struct drv_usb_uas_result result;
		unsigned char cdb[6] = {0};
		mutex_lock(&owner.lock);
		assert(uas_command(&owner, cdb, 6, NULL, 0, DRV_USB_UAS_NO_DATA, &result, NULL) == ETIMEDOUT);
		mutex_unlock(&owner.lock);
		assert(media_revoked && owner.media_pending);
	}
	puts("UAS disk recovery and sticky uncertain write: PASS");
	return 0;
}
