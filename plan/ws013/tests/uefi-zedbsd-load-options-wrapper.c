/* Inject a valid parameter string before entering the production loader. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "bootloader/uefi/include/uefi.h"

extern EFI_STATUS EFIAPI zedbsd_loader_main(EFI_HANDLE, EFI_SYSTEM_TABLE *);

static CHAR16 ignored_options[] = {
	'b', 'o', 'o', 't', '0', '=', 'U', 'U', 'I', 'D', '=',
	'D', 'E', 'A', 'D', '-', 'B', 'E', 'E', 'F', ' ',
	'i', 'n', 'i', 't', '=', '/', 'b', 'i', 'n', '/', 'f', 'a', 'l', 's', 'e',
	0
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
	status = system->BootServices->HandleProtocol(image,
	    &EFI_LOADED_IMAGE_PROTOCOL_GUID, (void **)&loaded);
	if (EFI_ERROR(status) || loaded == NULL)
		return EFI_ERROR(status) ? status : EFI_INVALID_PARAMETER;

	loaded->LoadOptions = ignored_options;
	loaded->LoadOptionsSize = sizeof(ignored_options);
	debug_string("WS013 LoadOptions injected: "
	    "boot0=UUID=DEAD-BEEF init=/bin/false\n");
	return zedbsd_loader_main(image, system);
}
