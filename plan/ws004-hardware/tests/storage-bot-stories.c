/* Actual usb-storage command engine; wire and scheduler are deterministic.
 * SPDX-License-Identifier: Zlib */
#define main no_media_regression_main
#include "usb-storage-no-media-test.c"
#undef main

static unsigned stage, resets, commands, opcode_commands, csw_reads;
static unsigned cbw_fail, cbw_short, csw_stalls, bad_tag, bad_residue;
static unsigned ua_left, ua_after_reset, disconnect_on_failure;
static unsigned char ua_key, ua_asc, ua_ascq, opcode;
static unsigned char medium[512], tag[4];
static unsigned check_condition, data_size;
static unsigned fault_opcode;

static int wire(struct drv_usb_urb *u)
{
	unsigned char *bytes = u->buffer;
	u->actual = 0;
	if (u->endpoint == NULL) {
		CHECK(u->control.request == USB_MASS_STORAGE_RESET);
		resets++;
		stage = 0;
		ua_left = ua_after_reset;
		return 0;
	}
	if (stage == 0) {
		CHECK(u->endpoint == &fixture_bulk_out && u->length == 31);
		opcode = bytes[15];
		commands++;
		if (opcode != SCSI_REQUEST_SENSE)
			opcode_commands++;
		memcpy(tag, bytes + 4, 4);
		data_size = get_le32(bytes + 8);
		if (cbw_fail && (fault_opcode == 0 || opcode == fault_opcode)) {
			cbw_fail--;
			device_disconnected = disconnect_on_failure;
			return EIO;
		}
		if (cbw_short) {
			cbw_short--;
			u->actual = 30;
			return 0;
		}
		check_condition = ua_left != 0 && opcode != SCSI_REQUEST_SENSE;
		stage = data_size ? 1 : 2;
		u->actual = 31;
		return 0;
	}
	if (stage == 1) {
		CHECK(u->length == data_size);
		if (opcode == SCSI_REQUEST_SENSE) {
			memset(bytes, 0, u->length);
			bytes[0] = 0x70; bytes[2] = ua_key; bytes[7] = 10;
			bytes[12] = ua_asc; bytes[13] = ua_ascq;
			CHECK(ua_left != 0);
			ua_left--;
		} else if (!check_condition) {
			CHECK(u->length == sizeof(medium));
			if (opcode == SCSI_READ_10)
				memcpy(bytes, medium, sizeof(medium));
			else {
				CHECK(opcode == 0x2aU);
				memcpy(medium, bytes, sizeof(medium));
			}
		}
		u->actual = u->length;
		stage = 2;
		return 0;
	}
	CHECK(stage == 2 && u->endpoint == &fixture_bulk_in && u->length == 13);
	csw_reads++;
	if (csw_stalls) { csw_stalls--; return EPIPE; }
	memset(bytes, 0, 13);
	put_le32(bytes, BOT_CSW_SIGNATURE);
	memcpy(bytes + 4, tag, 4);
	if (bad_tag) { bytes[4] ^= 1; bad_tag--; }
	if (bad_residue) { put_le32(bytes + 8, data_size + 1); bad_residue--; }
	bytes[12] = check_condition ? 1 : 0;
	u->actual = 13;
	stage = 0;
	return 0;
}

static void run_bio(struct usb_storage *s, unsigned operation,
	unsigned char *buffer, int expected)
{
	struct bio bio = {0};
	bio.b_op = operation; bio.b_block_count = 1; bio.b_data = buffer;
	CHECK(storage_submit(s->disk, &bio) == 0);
	CHECK(bio_error == expected);
	CHECK(s->lock.locked == 0);
	if (expected == 0 && operation != BIO_FLUSH)
		CHECK(bio_bytes == 512);
}

int main(void)
{
	static const unsigned ids[] = {1,2,3,4,9,10,11,12,13,14,15,16,17,18};
	for (unsigned cell = 0; cell < sizeof(ids) / sizeof(ids[0]); cell++) {
		unsigned id = ids[cell];
		struct drv_usb_interface interface;
		struct usb_storage s = {0};
		struct disk disk = {0};
		unsigned char buffer[512];
		fixture_reset(&interface, 0, 0x70);
		stage = resets = commands = opcode_commands = csw_reads = 0;
		cbw_fail = cbw_short = csw_stalls = bad_tag = bad_residue = 0;
		ua_left = ua_after_reset = disconnect_on_failure = device_disconnected = 0;
		fault_opcode = 0; ua_key = 6; ua_asc = 0x29; ua_ascq = 0;
		memset(medium, 0x24, sizeof(medium)); memset(buffer, 0x73, sizeof(buffer));
		s.device = &fixture_device; s.interface = &interface;
		s.bulk_in = &fixture_bulk_in; s.bulk_out = &fixture_bulk_out;
		s.disk = &disk; disk.d_data = &s; disk.d_block_size = 512;
		s.flush_policy = DRV_USB_SCSI_FLUSH_SYNC_CACHE;
		CHECK(storage_urbs_alloc(&s) == 0); transfer_override = wire;
		if (id == 2) csw_stalls = 1;
		if (id == 3) csw_stalls = 2;
		if (id == 4) cbw_short = 1;
		if (id == 9 || id == 11 || id == 12 || id == 13) {
			cbw_fail = 1; ua_after_reset = id == 13 ? 2 : 1;
		}
		if (id == 10) ua_left = 1;
		if (id == 11) ua_asc = 0x28;
		if (id == 12) { ua_key = 2; ua_asc = 0x3a; }
		if (id == 14 || id == 15) {
			cbw_fail = id == 15 ? 2 : 1; fault_opcode = SCSI_SYNCHRONIZE_CACHE_10;
		}
		if (id == 16) bad_tag = 1;
		if (id == 17) bad_residue = 1;
		if (id == 18) { cbw_fail = 1; disconnect_on_failure = 1; }
		if (id == 14 || id == 15) {
			run_bio(&s, BIO_FLUSH, NULL, id == 15 ? EIO : 0);
			CHECK((s.flush_error != 0) == (id == 15));
			if (id == 15) {
				unsigned before = commands;
				run_bio(&s, BIO_WRITE, buffer, EIO);
				CHECK(commands == before);
				stage = 0;
				run_bio(&s, BIO_READ, buffer, 0);
			}
		} else {
			int expected = (id == 10 || id == 11 || id == 12 || id == 13 || id == 18) ? EIO : 0;
			run_bio(&s, BIO_READ, buffer, expected);
			if (expected == 0) CHECK(memcmp(buffer, medium, 512) == 0);
			if (id == 1) {
				memset(buffer, 0x73, 512);
				run_bio(&s, BIO_WRITE, buffer, 0);
				run_bio(&s, BIO_FLUSH, NULL, 0);
				memset(buffer, 0, 512); run_bio(&s, BIO_READ, buffer, 0);
				CHECK(buffer[0] == 0x73 && buffer[511] == 0x73);
			}
			if (id == 11 || id == 12) run_bio(&s, BIO_WRITE, buffer, EIO);
		}
		CHECK(resets <= 1 && opcode_commands <= 4);
		if (id == 2) CHECK(resets == 0 && opcode_commands == 1 && csw_reads == 2);
		if (id == 9 || id == 13) CHECK(resets == 1 && opcode_commands == 3);
		if (id == 10 || id == 18) CHECK(resets == 0 && opcode_commands == 1);
		transfer_override = NULL; storage_urbs_free(&s);
		CHECK(live_urbs == 0 && live_allocations == 0);
		printf("S%02u PASS production BOT recovery and post-operation lock/ownership\n", id);
	}
	return 0;
}
