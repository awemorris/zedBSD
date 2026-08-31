/* USB storage SCSI response regression fixture. */
#include <drivers/usb-storage-scsi.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

int
main(void)
{
	struct drv_usb_scsi_cache_info cache;
	struct drv_usb_scsi_sense sense;
	uint8_t fixed[18] = {0x70, 0, 0x07, 0, 0, 0, 0, 10};
	uint8_t descriptor[8] = {0x72, 0x06, 0x29, 0x00};
	uint8_t no_medium_fixed[18] = {0x70, 0, 0x02, 0, 0, 0, 0, 10};
	uint8_t no_medium_descriptor[8] = {0x72, 0x02, 0x3a, 0x02};
	uint8_t mode[4] = {3, 0, 0x80, 0};
	uint8_t caching[7] = {6, 0, 0x10, 0, 0x08, 1, 0x04};
	uint8_t invalid[3] = {0x50, 0, 0};

	fixed[12] = 0x27;
	fixed[13] = 0x00;
	memset(&sense, 0, sizeof(sense));
	assert(drv_usb_scsi_parse_sense(fixed, sizeof(fixed), &sense));
	assert(sense.valid && sense.key == 0x07 && sense.asc == 0x27 &&
	       sense.ascq == 0x00 && sense.response_code == 0x70);

	assert(
	    drv_usb_scsi_parse_sense(descriptor, sizeof(descriptor), &sense));
	assert(sense.valid && sense.key == 0x06 && sense.asc == 0x29 &&
	       sense.ascq == 0x00 && sense.response_code == 0x72);
	assert(!drv_usb_scsi_parse_sense(invalid, sizeof(invalid), &sense));
	assert(!sense.valid && sense.response_code == 0);

	no_medium_fixed[12] = 0x3a;
	assert(drv_usb_scsi_parse_sense(no_medium_fixed,
	    sizeof(no_medium_fixed), &sense));
	assert(drv_usb_scsi_sense_is_medium_absent(&sense));
	assert(drv_usb_scsi_parse_sense(no_medium_descriptor,
	    sizeof(no_medium_descriptor), &sense));
	assert(drv_usb_scsi_sense_is_medium_absent(&sense));

	memset(&sense, 0, sizeof(sense));
	sense.valid = 1;
	sense.response_code = 0x70;
	sense.key = 0x02;
	sense.asc = 0x3a;
	assert(drv_usb_scsi_sense_is_medium_absent(&sense));
	sense.ascq = 0x02;
	assert(drv_usb_scsi_sense_is_medium_absent(&sense));
	sense.key = 0x06;
	assert(!drv_usb_scsi_sense_is_medium_absent(&sense));
	sense.key = 0x02;
	sense.asc = 0x04;
	assert(!drv_usb_scsi_sense_is_medium_absent(&sense));
	sense.asc = 0x3a;
	sense.valid = 0;
	assert(!drv_usb_scsi_sense_is_medium_absent(&sense));
	sense.valid = 1;
	sense.response_code = 0x71;
	assert(!drv_usb_scsi_sense_is_medium_absent(&sense));
	sense.response_code = 0x73;
	assert(!drv_usb_scsi_sense_is_medium_absent(&sense));
	assert(!drv_usb_scsi_sense_is_medium_absent(NULL));

	assert(drv_usb_scsi_mode_sense6_read_only(mode, sizeof(mode)));
	mode[2] = 0;
	assert(!drv_usb_scsi_mode_sense6_read_only(mode, sizeof(mode)));
	assert(!drv_usb_scsi_mode_sense6_read_only(mode, 2));

	assert(drv_usb_scsi_parse_mode_sense6_cache(caching,
	    sizeof(caching), &cache));
	assert(cache.header_valid && !cache.write_protected && cache.dpofua);
	assert(cache.cache_valid && cache.write_cache_enabled);
	caching[6] = 0;
	assert(drv_usb_scsi_parse_mode_sense6_cache(caching,
	    sizeof(caching), &cache));
	assert(!cache.write_cache_enabled);

	puts("usb-storage SCSI model: PASS");
	return 0;
}
