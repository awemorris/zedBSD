/* USB storage SCSI response regression fixture. */
#include <drivers/usb-storage-scsi.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

int
main(void)
{
	struct drv_usb_scsi_sense sense;
	uint8_t fixed[18] = {0x70, 0, 0x07, 0, 0, 0, 0, 10};
	uint8_t descriptor[8] = {0x72, 0x06, 0x29, 0x00};
	uint8_t mode[4] = {3, 0, 0x80, 0};
	uint8_t invalid[3] = {0x50, 0, 0};

	fixed[12] = 0x27;
	fixed[13] = 0x00;
	memset(&sense, 0, sizeof(sense));
	assert(drv_usb_scsi_parse_sense(fixed, sizeof(fixed), &sense));
	assert(sense.valid && sense.key == 0x07 && sense.asc == 0x27 &&
	       sense.ascq == 0x00);

	assert(
	    drv_usb_scsi_parse_sense(descriptor, sizeof(descriptor), &sense));
	assert(sense.valid && sense.key == 0x06 && sense.asc == 0x29 &&
	       sense.ascq == 0x00);
	assert(!drv_usb_scsi_parse_sense(invalid, sizeof(invalid), &sense));
	assert(!sense.valid);

	assert(drv_usb_scsi_mode_sense6_read_only(mode, sizeof(mode)));
	mode[2] = 0;
	assert(!drv_usb_scsi_mode_sense6_read_only(mode, sizeof(mode)));
	assert(!drv_usb_scsi_mode_sense6_read_only(mode, 2));

	puts("usb-storage SCSI model: PASS");
	return 0;
}
