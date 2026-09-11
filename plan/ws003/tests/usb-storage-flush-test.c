/* USB-storage cache/flush policy regression fixture (BR-T39). */
#include <drivers/usb-storage-bot.h>
#include <drivers/usb-storage-scsi.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void
expect_unknown(const uint8_t *mode, size_t length, int header_valid,
	int write_protected)
{
	struct drv_usb_scsi_cache_info info;

	memset(&info, 0xa5, sizeof(info));
	assert(!drv_usb_scsi_parse_mode_sense6_cache(mode, length, &info));
	assert(info.header_valid == header_valid);
	assert(info.write_protected == write_protected);
	assert(!info.cache_valid);
	assert(!info.write_cache_enabled);
	assert(drv_usb_scsi_flush_policy_after_unsupported(&info) ==
	    DRV_USB_SCSI_FLUSH_UNSAFE);
}

static void
test_mode_sense_matrix(void)
{
	struct drv_usb_scsi_cache_info info;
	uint8_t mode[20] = {
		19, 0, 0, 8,
		0xde, 0xad, 0xbe, 0xef, 0, 0, 0, 0,
		0x01, 1, 0,
		0x08, 3, 0, 0, 0
	};

	assert(drv_usb_scsi_parse_mode_sense6_cache(mode, sizeof(mode),
	    &info));
	assert(info.header_valid && info.cache_valid);
	assert(!info.write_protected && !info.dpofua);
	assert(!info.write_cache_enabled);
	assert(drv_usb_scsi_flush_policy_after_unsupported(&info) ==
	    DRV_USB_SCSI_FLUSH_WRITE_THROUGH);
	mode[2] = 0x10;
	assert(drv_usb_scsi_parse_mode_sense6_cache(mode, sizeof(mode),
	    &info));
	assert(info.dpofua && !info.write_cache_enabled);
	assert(drv_usb_scsi_flush_policy_after_unsupported(&info) ==
	    DRV_USB_SCSI_FLUSH_WRITE_THROUGH);

	mode[17] = 0x04;
	assert(drv_usb_scsi_parse_mode_sense6_cache(mode, sizeof(mode),
	    &info));
	assert(info.dpofua && info.write_cache_enabled);
	assert(drv_usb_scsi_flush_policy_after_unsupported(&info) ==
	    DRV_USB_SCSI_FLUSH_FUA);

	mode[2] = 0;
	assert(drv_usb_scsi_parse_mode_sense6_cache(mode, sizeof(mode),
	    &info));
	assert(info.write_cache_enabled && !info.dpofua);
	assert(drv_usb_scsi_flush_policy_after_unsupported(&info) ==
	    DRV_USB_SCSI_FLUSH_UNSAFE);

	mode[2] = 0x90;
	assert(drv_usb_scsi_parse_mode_sense6_cache(mode, sizeof(mode),
	    &info));
	assert(info.write_protected && info.dpofua);
}

static void
test_mode_sense_rejection(void)
{
	uint8_t valid[7] = {6, 0, 0x80, 0, 0x08, 1, 0};
	uint8_t absent[7] = {6, 0, 0x80, 0, 0x01, 1, 0};
	uint8_t duplicate[10] = {
		9, 0, 0x80, 0, 0x08, 1, 0, 0x08, 1, 4
	};
	uint8_t bad_descriptor[7] = {6, 0, 0x80, 4, 0x08, 1, 0};
	uint8_t bad_page_length[7] = {6, 0, 0x80, 0, 0x08, 8, 0};
	uint8_t short_page[6] = {5, 0, 0x80, 0, 0x08, 0};
	uint8_t subpage_cache[8] = {7, 0, 0x80, 0, 0x48, 0, 0, 0};
	uint8_t valid_subpage_skip[14] = {
		13, 0, 0, 0,
		0x41, 0, 0, 1, 0,
		0x08, 3, 0, 0, 0
	};
	uint8_t short_header[3] = {2, 0, 0x80};
	uint8_t contradictory_header[4] = {2, 0, 0x80, 0};
	struct drv_usb_scsi_cache_info info;

	/* The transfer is shorter than Mode Data Length declares. */
	valid[0] = 10;
	expect_unknown(valid, sizeof(valid), 1, 1);
	valid[0] = 6;
	expect_unknown(valid, sizeof(valid) - 1U, 1, 1);
	expect_unknown(absent, sizeof(absent), 1, 1);
	expect_unknown(duplicate, sizeof(duplicate), 1, 1);
	expect_unknown(bad_descriptor, sizeof(bad_descriptor), 1, 1);
	expect_unknown(bad_page_length, sizeof(bad_page_length), 1, 1);
	expect_unknown(short_page, sizeof(short_page), 1, 1);
	expect_unknown(subpage_cache, sizeof(subpage_cache), 1, 1);
	assert(drv_usb_scsi_parse_mode_sense6_cache(valid_subpage_skip,
	    sizeof(valid_subpage_skip), &info));
	assert(info.cache_valid && !info.write_cache_enabled);
	expect_unknown(short_header, sizeof(short_header), 0, 0);
	expect_unknown(contradictory_header, sizeof(contradictory_header), 0, 0);
	expect_unknown(NULL, 0, 0, 0);
	assert(!drv_usb_scsi_parse_mode_sense6_cache(valid, sizeof(valid), NULL));
}

static void
test_sense_classifier(void)
{
	struct drv_usb_scsi_sense sense = {
		.key = 0x05,
		.asc = 0x20,
		.ascq = 0x00,
		.valid = 1,
		.response_code = 0x70
	};
	uint8_t truncated_fixed[13] = {0x70, 0, 5, 0, 0, 0, 0, 10};
	uint8_t bad_fixed_length[14] = {0x70, 0, 5, 0, 0, 0, 0, 5};
	uint8_t truncated_descriptor[7] = {0x72, 5, 0x20, 0};
	uint8_t extended_descriptor[8] = {0x72, 5, 0x20, 0, 0, 0, 0, 32};
	uint8_t fixed[18] = {0x70, 0, 5, 0, 0, 0, 0, 10};
	uint8_t descriptor[8] = {0x72, 5, 0x20, 0};

	assert(drv_usb_scsi_sense_is_invalid_opcode(&sense));
	fixed[12] = 0x20;
	assert(drv_usb_scsi_parse_sense(fixed, sizeof(fixed), &sense));
	assert(drv_usb_scsi_sense_is_invalid_opcode(&sense));
	assert(drv_usb_scsi_parse_sense(descriptor, sizeof(descriptor),
	    &sense));
	assert(drv_usb_scsi_sense_is_invalid_opcode(&sense));
	fixed[0] = 0x71;
	assert(drv_usb_scsi_parse_sense(fixed, sizeof(fixed), &sense));
	assert(sense.valid && sense.response_code == 0x71);
	assert(!drv_usb_scsi_sense_is_invalid_opcode(&sense));
	descriptor[0] = 0x73;
	assert(drv_usb_scsi_parse_sense(descriptor, sizeof(descriptor),
	    &sense));
	assert(sense.valid && sense.response_code == 0x73);
	assert(!drv_usb_scsi_sense_is_invalid_opcode(&sense));
	assert(drv_usb_scsi_parse_sense(extended_descriptor,
	    sizeof(extended_descriptor), &sense));
	assert(drv_usb_scsi_sense_is_invalid_opcode(&sense));
	sense.response_code = 0x70;
	sense.valid = 0;
	assert(!drv_usb_scsi_sense_is_invalid_opcode(&sense));
	sense.valid = 1;
	sense.key = 0x06;
	assert(!drv_usb_scsi_sense_is_invalid_opcode(&sense));
	sense.key = 0x05;
	sense.asc = 0x24;
	assert(!drv_usb_scsi_sense_is_invalid_opcode(&sense));
	sense.asc = 0x20;
	sense.ascq = 1;
	assert(!drv_usb_scsi_sense_is_invalid_opcode(&sense));
	sense.ascq = 0;
	sense.response_code = 0x71;
	assert(!drv_usb_scsi_sense_is_invalid_opcode(&sense));
	sense.response_code = 0x73;
	assert(!drv_usb_scsi_sense_is_invalid_opcode(&sense));
	memset(&sense, 0xa5, sizeof(sense));
	assert(!drv_usb_scsi_parse_sense(truncated_fixed,
	    sizeof(truncated_fixed),
	    &sense));
	assert(!sense.valid && sense.response_code == 0);
	memset(&sense, 0xa5, sizeof(sense));
	assert(!drv_usb_scsi_parse_sense(bad_fixed_length,
	    sizeof(bad_fixed_length), &sense));
	assert(!sense.valid && sense.response_code == 0);
	memset(&sense, 0xa5, sizeof(sense));
	assert(!drv_usb_scsi_parse_sense(truncated_descriptor,
	    sizeof(truncated_descriptor), &sense));
	assert(!sense.valid && sense.response_code == 0);
	assert(!drv_usb_scsi_sense_is_invalid_opcode(NULL));
}

static void
test_write10_fua(void)
{
	uint8_t cdb[10] = {0x2a, 0x20};
	uint8_t wrong[10] = {0x28};

	assert(drv_usb_scsi_write10_set_fua(cdb, sizeof(cdb)));
	assert(cdb[1] == 0x28);
	assert(!drv_usb_scsi_write10_set_fua(cdb, 9));
	assert(!drv_usb_scsi_write10_set_fua(wrong, sizeof(wrong)));
	assert(!drv_usb_scsi_write10_set_fua(NULL, sizeof(cdb)));
}

static void
test_policy_selection_and_sticky_failure(void)
{
	struct drv_usb_scsi_cache_info cache = {0};
	struct drv_usb_scsi_sense unsupported = {
		.key = 0x05,
		.asc = 0x20,
		.ascq = 0x00,
		.valid = 1,
		.response_code = 0x70
	};
	struct drv_usb_scsi_sense other = {
		.key = 0x05,
		.asc = 0x24,
		.ascq = 0x00,
		.valid = 1,
		.response_code = 0x70
	};
	enum drv_usb_scsi_flush_policy policy;
	int sticky_error = 0;
	uint8_t mode_cdb[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
	uint8_t write_cdb[10];

	assert(drv_usb_scsi_make_mode_sense6_cache_cdb(mode_cdb,
	    sizeof(mode_cdb), 64));
	assert(mode_cdb[0] == 0x1a && mode_cdb[1] == 0x08 &&
	    mode_cdb[2] == 0x08 && mode_cdb[3] == 0 && mode_cdb[4] == 64 &&
	    mode_cdb[5] == 0);
	assert(!drv_usb_scsi_make_mode_sense6_cache_cdb(mode_cdb, 5, 64));
	assert(!drv_usb_scsi_make_mode_sense6_cache_cdb(mode_cdb,
	    sizeof(mode_cdb), 3));

	cache.cache_valid = 1;
	assert(drv_usb_scsi_select_flush_policy(&cache, 0, NULL) ==
	    DRV_USB_SCSI_FLUSH_WRITE_THROUGH);
	assert(drv_usb_scsi_flush_policy_allows_write(
	    DRV_USB_SCSI_FLUSH_WRITE_THROUGH));
	assert(!drv_usb_scsi_flush_policy_uses_sync_cache(
	    DRV_USB_SCSI_FLUSH_WRITE_THROUGH));
	cache.write_cache_enabled = 1;
	cache.dpofua = 1;
	assert(drv_usb_scsi_select_flush_policy(&cache, 1, NULL) ==
	    DRV_USB_SCSI_FLUSH_SYNC_CACHE);
	assert(drv_usb_scsi_flush_policy_uses_sync_cache(
	    DRV_USB_SCSI_FLUSH_SYNC_CACHE));
	assert(drv_usb_scsi_make_rw10_cdb(write_cdb, sizeof(write_cdb), 1,
	    DRV_USB_SCSI_FLUSH_SYNC_CACHE));
	assert(write_cdb[0] == 0x2a && write_cdb[1] == 0);
	policy = drv_usb_scsi_select_flush_policy(&cache, 0, &unsupported);
	assert(policy == DRV_USB_SCSI_FLUSH_FUA);
	assert(drv_usb_scsi_flush_policy_uses_fua(DRV_USB_SCSI_FLUSH_FUA));
	assert(!drv_usb_scsi_flush_policy_requires_read_only(policy, 0));
	assert(drv_usb_scsi_make_rw10_cdb(write_cdb, sizeof(write_cdb), 1,
	    policy));
	assert(write_cdb[0] == 0x2a && (write_cdb[1] & 0x08) != 0);
	assert(drv_usb_scsi_select_flush_policy(&cache, 0, &other) ==
	    DRV_USB_SCSI_FLUSH_UNSAFE);
	assert(!drv_usb_scsi_flush_policy_allows_write(
	    DRV_USB_SCSI_FLUSH_UNSAFE));
	assert(drv_usb_scsi_flush_policy_requires_read_only(
	    DRV_USB_SCSI_FLUSH_UNSAFE, 0));
	assert(drv_usb_scsi_flush_policy_requires_read_only(
	    DRV_USB_SCSI_FLUSH_SYNC_CACHE, 1));
	assert(!drv_usb_scsi_make_rw10_cdb(write_cdb, sizeof(write_cdb), 1,
	    DRV_USB_SCSI_FLUSH_UNSAFE));
	assert(drv_usb_scsi_make_rw10_cdb(write_cdb, sizeof(write_cdb), 0,
	    DRV_USB_SCSI_FLUSH_UNSAFE));
	assert(write_cdb[0] == 0x28 && write_cdb[1] == 0);
	assert(!drv_usb_scsi_flush_policy_allows_write(
	    (enum drv_usb_scsi_flush_policy)99));
	assert(drv_usb_scsi_select_flush_policy(&cache, 0, NULL) ==
	    DRV_USB_SCSI_FLUSH_UNSAFE);
	cache.dpofua = 0;
	assert(drv_usb_scsi_select_flush_policy(&cache, 0, &unsupported) ==
	    DRV_USB_SCSI_FLUSH_UNSAFE);
	cache.cache_valid = 0;
	assert(drv_usb_scsi_select_flush_policy(&cache, 1, NULL) ==
	    DRV_USB_SCSI_FLUSH_SYNC_CACHE);
	assert(drv_usb_scsi_select_flush_policy(&cache, 0, &unsupported) ==
	    DRV_USB_SCSI_FLUSH_UNSAFE);

	drv_usb_scsi_record_flush_result(DRV_USB_SCSI_FLUSH_WRITE_THROUGH, 5,
	    &sticky_error);
	assert(sticky_error == 0);
	drv_usb_scsi_record_flush_result(DRV_USB_SCSI_FLUSH_FUA, 5,
	    &sticky_error);
	assert(sticky_error == 0);
	drv_usb_scsi_record_flush_result(DRV_USB_SCSI_FLUSH_SYNC_CACHE, 0,
	    &sticky_error);
	assert(sticky_error == 0);
	drv_usb_scsi_record_flush_result(DRV_USB_SCSI_FLUSH_SYNC_CACHE, 42,
	    &sticky_error);
	assert(sticky_error == 42);
	drv_usb_scsi_record_flush_result(DRV_USB_SCSI_FLUSH_SYNC_CACHE, 0,
	    &sticky_error);
	assert(sticky_error == 42);
	drv_usb_scsi_record_flush_result(DRV_USB_SCSI_FLUSH_SYNC_CACHE, 5,
	    &sticky_error);
	assert(sticky_error == 42);
}

static void
test_bot_completion_contract(void)
{
	size_t processed = 99;

	assert(drv_usb_bot_classify_csw_status(0) == DRV_USB_BOT_CSW_GOOD);
	assert(drv_usb_bot_classify_csw_status(1) ==
	    DRV_USB_BOT_CSW_COMMAND_FAILED);
	assert(drv_usb_bot_classify_csw_status(2) ==
	    DRV_USB_BOT_CSW_PHASE_ERROR);
	assert(drv_usb_bot_classify_csw_status(3) ==
	    DRV_USB_BOT_CSW_INVALID);
	assert(!drv_usb_bot_csw_requests_sense(DRV_USB_BOT_CSW_GOOD));
	assert(drv_usb_bot_csw_requests_sense(
	    DRV_USB_BOT_CSW_COMMAND_FAILED));
	assert(!drv_usb_bot_csw_requests_sense(DRV_USB_BOT_CSW_PHASE_ERROR));
	assert(!drv_usb_bot_csw_requests_sense(DRV_USB_BOT_CSW_INVALID));

	assert(drv_usb_bot_processed_length(64, 48, 16, 1, &processed));
	assert(processed == 48);
	assert(!drv_usb_bot_processed_length(64, 64, 16, 1, &processed));
	assert(!drv_usb_bot_processed_length(64, 48, 65, 1, &processed));
	assert(drv_usb_bot_processed_length(64, 64, 16, 0, &processed));
	assert(processed == 48);
	assert(!drv_usb_bot_processed_length(64, 48, 16, 0, &processed));
	assert(drv_usb_bot_processed_length(0, 0, 0, 0, &processed));
	assert(processed == 0);
	assert(!drv_usb_bot_processed_length(0, 0, 1, 0, &processed));
}

int
main(void)
{
	test_mode_sense_matrix();
	test_mode_sense_rejection();
	test_sense_classifier();
	test_write10_fua();
	test_policy_selection_and_sticky_failure();
	test_bot_completion_contract();
	assert(drv_usb_scsi_flush_policy_after_unsupported(NULL) ==
	    DRV_USB_SCSI_FLUSH_UNSAFE);
	puts("BR-T39 usb-storage flush policy: PASS");
	return 0;
}
