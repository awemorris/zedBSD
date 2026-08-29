/* BR-T48: inject a complete EFI_LOAD_OPTION before entering the real loader. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "bootloader/uefi/include/uefi.h"

extern EFI_STATUS EFIAPI zedbsd_loader_main(EFI_HANDLE, EFI_SYSTEM_TABLE *);

/*
 * A packed EFI_LOAD_OPTION with:
 *
 *   Attributes         LOAD_OPTION_ACTIVE
 *   Description        L"z"
 *   FilePathList       an empty MEDIA_FILEPATH node, then END_ENTIRE
 *   OptionalData       zero bytes
 *
 * This is deliberately the whole descriptor rather than OptionalData.  Some
 * physical firmware exposes this compatibility form through LoadedImage's
 * LoadOptions.  Its final bytes are 7f ff 04 00, so treating the complete
 * binary descriptor as a mandatory NUL-terminated CHAR16 string reproduces
 * the Dell `missing-nul` boot failure.
 */
static const uint8_t complete_load_option[] = {
	0x01, 0x00, 0x00, 0x00, /* LOAD_OPTION_ACTIVE */
	0x0a, 0x00,             /* FilePathListLength */
	'z',  0x00, 0x00, 0x00, /* Description: L"z" */
	0x04, 0x04, 0x06, 0x00, 0x00, 0x00, /* empty MEDIA_FILEPATH */
	0x7f, 0xff, 0x04, 0x00, /* END_ENTIRE */
};

static void
debug_string(const char *text)
{
	while (*text != '\0') {
		__asm__ volatile("outb %0,%1"
		    :
		    : "a"((uint8_t)*text++), "Nd"((uint16_t)0xe9));
	}
}

EFI_STATUS EFIAPI
efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *system)
{
	EFI_LOADED_IMAGE_PROTOCOL *loaded;
	EFI_STATUS status;

	if (system == NULL || system->BootServices == NULL ||
	    system->BootServices->HandleProtocol == NULL)
		return EFI_INVALID_PARAMETER;
	status = system->BootServices->HandleProtocol(
	    image, &EFI_LOADED_IMAGE_PROTOCOL_GUID, (void **)&loaded);
	if (EFI_ERROR(status) || loaded == NULL)
		return EFI_ERROR(status) ? status : EFI_INVALID_PARAMETER;

	loaded->LoadOptions = (void *)complete_load_option;
	loaded->LoadOptionsSize = sizeof(complete_load_option);
	debug_string("BR-T48 EFI_LOAD_OPTION injected optional-data=0\n");
	return zedbsd_loader_main(image, system);
}
