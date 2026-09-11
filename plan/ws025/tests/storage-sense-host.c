/* Production response classifier, including every non-whitelisted ASCQ. */
#define main retained_scsi_main
#include "../../ws004/tests/usb-storage-scsi-test.c"
#undef main

int
main(void)
{
	struct drv_usb_scsi_sense sense;
	enum drv_usb_scsi_recovery expected;
	unsigned response, ascq;

	assert(retained_scsi_main() == 0);
	assert(drv_usb_scsi_recovery_action(NULL) == DRV_USB_SCSI_RECOVERY_NONE);
	memset(&sense, 0, sizeof(sense));
	assert(drv_usb_scsi_recovery_action(&sense) == DRV_USB_SCSI_RECOVERY_NONE);
	sense.valid = 1;
	for (response = 0x70; response <= 0x73; response++) {
		sense.response_code = response;
		sense.key = 6;
		for (ascq = 0; ascq < 256; ascq++) {
			sense.ascq = ascq;
			sense.asc = 0x29;
			expected = ascq == 0 && !(response & 1) ?
			    DRV_USB_SCSI_RECOVERY_RESET : DRV_USB_SCSI_RECOVERY_FAILED;
			assert(drv_usb_scsi_recovery_action(&sense) == expected);
			sense.asc = 0x3a;
			expected = ascq == 0 && !(response & 1) ?
			    DRV_USB_SCSI_RECOVERY_ABSENT : DRV_USB_SCSI_RECOVERY_FAILED;
			assert(drv_usb_scsi_recovery_action(&sense) == expected);
			sense.asc = 0x28;
			expected = ascq == 0 && !(response & 1) ?
			    DRV_USB_SCSI_RECOVERY_MEDIA : DRV_USB_SCSI_RECOVERY_FAILED;
			assert(drv_usb_scsi_recovery_action(&sense) == expected);
			sense.asc = 0x2a;
			expected = DRV_USB_SCSI_RECOVERY_FAILED;
			if (!(response & 1) && ascq == 1)
				expected = DRV_USB_SCSI_RECOVERY_MODE;
			if (!(response & 1) && ascq == 9)
				expected = DRV_USB_SCSI_RECOVERY_MEDIA;
			assert(drv_usb_scsi_recovery_action(&sense) == expected);
		}
		sense.key = 2;
		sense.asc = 0x3a;
		for (ascq = 0; ascq < 256; ascq++) {
			sense.ascq = ascq;
			expected = response & 1 ? DRV_USB_SCSI_RECOVERY_FAILED :
			    DRV_USB_SCSI_RECOVERY_ABSENT;
			assert(drv_usb_scsi_recovery_action(&sense) == expected);
		}
		sense.key = 5;
		assert(drv_usb_scsi_recovery_action(&sense) == DRV_USB_SCSI_RECOVERY_NONE);
	}
	puts("SCSI recovery classifier PASS: 5120 current/deferred/ASCQ combinations");
	return 0;
}
