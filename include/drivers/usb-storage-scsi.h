/*
 * USB storage SCSI response helpers
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#ifndef ZEDBSD_DRIVERS_USB_STORAGE_SCSI_H
#define ZEDBSD_DRIVERS_USB_STORAGE_SCSI_H

#include <stddef.h>
#include <stdint.h>

struct drv_usb_scsi_sense {
	uint8_t key;
	uint8_t asc;
	uint8_t ascq;
	uint8_t valid;
	uint8_t response_code;
};

/*
 * Independently validated portions of a MODE SENSE(6) response.  A valid
 * four-byte header can establish write protection even when the descriptor
 * or mode-page area is malformed.  Cache state is usable only when
 * cache_valid is set.
 */
struct drv_usb_scsi_cache_info {
	uint8_t header_valid;
	uint8_t write_protected;
	uint8_t dpofua;
	uint8_t cache_valid;
	uint8_t write_cache_enabled;
};

enum drv_usb_scsi_flush_policy {
	DRV_USB_SCSI_FLUSH_UNSAFE = 0,
	DRV_USB_SCSI_FLUSH_SYNC_CACHE,
	DRV_USB_SCSI_FLUSH_WRITE_THROUGH,
	DRV_USB_SCSI_FLUSH_FUA
};

static inline int
drv_usb_scsi_parse_sense(const void *buffer, size_t length,
			 struct drv_usb_scsi_sense *sense)
{
	const uint8_t *bytes = buffer;
	uint8_t response;

	if (sense == NULL)
		return 0;
	sense->key = 0;
	sense->asc = 0;
	sense->ascq = 0;
	sense->valid = 0;
	sense->response_code = 0;
	if (bytes == NULL || length == 0)
		return 0;
	response = bytes[0] & 0x7fU;
	if ((response == 0x70U || response == 0x71U) && length >= 14U) {
		/* ASC/ASCQ are present only when Additional Sense Length >= 6. */
		if (bytes[7] < 6U)
			return 0;
		sense->key = bytes[2] & 0x0fU;
		sense->asc = bytes[12];
		sense->ascq = bytes[13];
	} else if ((response == 0x72U || response == 0x73U) && length >= 8U) {
		sense->key = bytes[1] & 0x0fU;
		sense->asc = bytes[2];
		sense->ascq = bytes[3];
	} else {
		return 0;
	}
	sense->valid = 1;
	sense->response_code = response;
	return 1;
}

static inline int
drv_usb_scsi_sense_is_invalid_opcode(
	const struct drv_usb_scsi_sense *sense)
{
	return sense != NULL && sense->valid != 0 &&
	    (sense->response_code == 0x70U ||
		sense->response_code == 0x72U) &&
	    sense->key == 0x05U && sense->asc == 0x20U &&
	    sense->ascq == 0x00U;
}

/*
 * A current NOT READY / MEDIUM NOT PRESENT is the ordinary state of an empty
 * removable reader. ASCQ refines the reason (tray open, unloaded, and so on),
 * but every value under ASC 3Ah means that no block medium can be published
 * yet. Deferred responses (71h/73h) describe an earlier command and therefore
 * cannot establish why this TEST UNIT READY failed.
 */
static inline int
drv_usb_scsi_sense_is_medium_absent(
	const struct drv_usb_scsi_sense *sense)
{
	return sense != NULL && sense->valid != 0 &&
	    (sense->response_code == 0x70U ||
	     sense->response_code == 0x72U) &&
	    sense->key == 0x02U && sense->asc == 0x3aU;
}

/*
 * Parse the current-values MODE SENSE(6) parameter list for Caching page 08h.
 * The returned boolean says whether cache_valid is set.  Declared bytes past
 * the actual transfer, invalid block/page lengths, a subpage-form caching
 * page, or duplicate caching pages make cache state unknown.
 */
static inline int
drv_usb_scsi_parse_mode_sense6_cache(const void *buffer, size_t actual,
	struct drv_usb_scsi_cache_info *info)
{
	const uint8_t *bytes = buffer;
	size_t declared, offset;
	uint8_t cache_found = 0;
	uint8_t write_cache_enabled = 0;

	if (info == NULL)
		return 0;
	info->header_valid = 0;
	info->write_protected = 0;
	info->dpofua = 0;
	info->cache_valid = 0;
	info->write_cache_enabled = 0;
	if (bytes == NULL || actual < 4U)
		return 0;
	declared = (size_t)bytes[0] + 1U;
	if (declared < 4U)
		return 0;
	info->header_valid = 1;
	info->write_protected = (bytes[2] & 0x80U) != 0;
	info->dpofua = (bytes[2] & 0x10U) != 0;
	if (declared > actual)
		return 0;
	offset = 4U + (size_t)bytes[3];
	if ((bytes[3] & 7U) != 0 || offset > declared)
		return 0;
	while (offset < declared) {
		size_t page_length;
		uint8_t page_code;
		uint8_t subpage;

		if (declared - offset < 2U)
			return 0;
		page_code = bytes[offset] & 0x3fU;
		subpage = bytes[offset] & 0x40U;
		if (subpage != 0) {
			if (declared - offset < 4U)
				return 0;
			page_length = 4U +
			    ((size_t)bytes[offset + 2U] << 8) +
			    (size_t)bytes[offset + 3U];
			/* Page 08h must use the ordinary page format here. */
			if (page_code == 0x08U)
				return 0;
		} else {
			page_length = 2U + (size_t)bytes[offset + 1U];
		}
		if (page_length > declared - offset)
			return 0;
		if (subpage == 0 && page_code == 0x08U) {
			if (cache_found != 0 || page_length < 3U)
				return 0;
			cache_found = 1;
			write_cache_enabled =
			    (bytes[offset + 2U] & 0x04U) != 0;
		}
		offset += page_length;
	}
	if (cache_found == 0)
		return 0;
	info->cache_valid = 1;
	info->write_cache_enabled = write_cache_enabled;
	return 1;
}

static inline enum drv_usb_scsi_flush_policy
drv_usb_scsi_flush_policy_after_unsupported(
	const struct drv_usb_scsi_cache_info *info)
{
	if (info == NULL || info->cache_valid == 0)
		return DRV_USB_SCSI_FLUSH_UNSAFE;
	if (info->write_cache_enabled == 0)
		return DRV_USB_SCSI_FLUSH_WRITE_THROUGH;
	if (info->dpofua != 0)
		return DRV_USB_SCSI_FLUSH_FUA;
	return DRV_USB_SCSI_FLUSH_UNSAFE;
}

/*
 * Select the immutable policy before publishing the disk.  A positively
 * disabled cache is already durable and does not need an opcode probe.
 * Otherwise, a successful SYNCHRONIZE CACHE probe is preferred, and only an
 * exact invalid-opcode response may select an advertised fallback.
 */
static inline enum drv_usb_scsi_flush_policy
drv_usb_scsi_select_flush_policy(const struct drv_usb_scsi_cache_info *info,
	int sync_succeeded, const struct drv_usb_scsi_sense *sync_sense)
{
	if (info != NULL && info->cache_valid != 0 &&
	    info->write_cache_enabled == 0)
		return DRV_USB_SCSI_FLUSH_WRITE_THROUGH;
	if (sync_succeeded != 0)
		return DRV_USB_SCSI_FLUSH_SYNC_CACHE;
	if (drv_usb_scsi_sense_is_invalid_opcode(sync_sense))
		return drv_usb_scsi_flush_policy_after_unsupported(info);
	return DRV_USB_SCSI_FLUSH_UNSAFE;
}

static inline int
drv_usb_scsi_flush_policy_allows_write(
	enum drv_usb_scsi_flush_policy policy)
{
	return policy == DRV_USB_SCSI_FLUSH_SYNC_CACHE ||
	    policy == DRV_USB_SCSI_FLUSH_WRITE_THROUGH ||
	    policy == DRV_USB_SCSI_FLUSH_FUA;
}

static inline int
drv_usb_scsi_flush_policy_requires_read_only(
	enum drv_usb_scsi_flush_policy policy, int write_protected)
{
	return write_protected != 0 ||
	    !drv_usb_scsi_flush_policy_allows_write(policy);
}

static inline int
drv_usb_scsi_flush_policy_uses_fua(enum drv_usb_scsi_flush_policy policy)
{
	return policy == DRV_USB_SCSI_FLUSH_FUA;
}

static inline int
drv_usb_scsi_flush_policy_uses_sync_cache(
	enum drv_usb_scsi_flush_policy policy)
{
	return policy == DRV_USB_SCSI_FLUSH_SYNC_CACHE;
}

static inline int
drv_usb_scsi_make_mode_sense6_cache_cdb(void *cdb, size_t length,
	uint8_t allocation_length)
{
	uint8_t *bytes = cdb;

	if (bytes == NULL || length < 6U || allocation_length < 4U)
		return 0;
	bytes[0] = 0x1aU;
	bytes[1] = 0x08U; /* Disable block descriptors. */
	bytes[2] = 0x08U; /* Current values, Caching mode page. */
	bytes[3] = 0;
	bytes[4] = allocation_length;
	bytes[5] = 0;
	return 1;
}

static inline void
drv_usb_scsi_record_flush_result(enum drv_usb_scsi_flush_policy policy,
	int error, int *sticky_error)
{
	if (sticky_error != NULL && *sticky_error == 0 &&
	    policy == DRV_USB_SCSI_FLUSH_SYNC_CACHE && error != 0)
		*sticky_error = error;
}

static inline int
drv_usb_scsi_write10_set_fua(void *cdb, size_t length)
{
	uint8_t *bytes = cdb;

	if (bytes == NULL || length < 10U || bytes[0] != 0x2aU)
		return 0;
	bytes[1] |= 0x08U;
	return 1;
}

static inline int
drv_usb_scsi_make_rw10_cdb(void *cdb, size_t length, int write,
	enum drv_usb_scsi_flush_policy policy)
{
	uint8_t *bytes = cdb;
	size_t i;

	if (bytes == NULL || length < 10U ||
	    (write != 0 && !drv_usb_scsi_flush_policy_allows_write(policy)))
		return 0;
	for (i = 0; i < 10U; i++)
		bytes[i] = 0;
	bytes[0] = write != 0 ? 0x2aU : 0x28U;
	if (write != 0 && drv_usb_scsi_flush_policy_uses_fua(policy))
		return drv_usb_scsi_write10_set_fua(bytes, 10U);
	return 1;
}

static inline int
drv_usb_scsi_mode_sense6_read_only(const void *buffer, size_t length)
{
	const uint8_t *bytes = buffer;

	return bytes != NULL && length >= 3U && (bytes[2] & 0x80U) != 0;
}

#endif
