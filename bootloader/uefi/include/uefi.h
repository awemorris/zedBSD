/* Minimal UEFI x64 declarations used by the zedBSD loader. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_BOOTLOADER_UEFI_H
#define ZEDBSD_BOOTLOADER_UEFI_H

#include <stddef.h>
#include <stdint.h>

#define EFIAPI __attribute__((ms_abi))

typedef uint8_t BOOLEAN;
typedef uint16_t CHAR16;
typedef uint64_t EFI_STATUS;
typedef void *EFI_HANDLE;
typedef void *EFI_EVENT;
typedef uint64_t EFI_PHYSICAL_ADDRESS;
typedef uint64_t EFI_VIRTUAL_ADDRESS;
typedef uintptr_t UINTN;
typedef uint32_t UINT32;
typedef uint64_t UINT64;

#define EFI_SUCCESS             ((EFI_STATUS)0)
#define EFI_LOAD_ERROR          (0x8000000000000001ULL)
#define EFI_INVALID_PARAMETER   (0x8000000000000002ULL)
#define EFI_UNSUPPORTED         (0x8000000000000003ULL)
#define EFI_BUFFER_TOO_SMALL    (0x8000000000000005ULL)
#define EFI_NOT_FOUND           (0x800000000000000eULL)
#define EFI_ERROR(status)       (((status) & 0x8000000000000000ULL) != 0)

#define EFI_FILE_MODE_READ      0x0000000000000001ULL

enum efi_allocate_type {
	AllocateAnyPages,
	AllocateMaxAddress,
	AllocateAddress,
	MaxAllocateType
};

enum efi_memory_type {
	EfiReservedMemoryType,
	EfiLoaderCode,
	EfiLoaderData,
	EfiBootServicesCode,
	EfiBootServicesData,
	EfiRuntimeServicesCode,
	EfiRuntimeServicesData,
	EfiConventionalMemory,
	EfiUnusableMemory,
	EfiACPIReclaimMemory,
	EfiACPIMemoryNVS,
	EfiMemoryMappedIO,
	EfiMemoryMappedIOPortSpace,
	EfiPalCode,
	EfiPersistentMemory,
	EfiUnacceptedMemoryType,
	EfiMaxMemoryType
};

typedef struct {
	uint32_t Data1;
	uint16_t Data2;
	uint16_t Data3;
	uint8_t Data4[8];
} EFI_GUID;

typedef struct {
	UINT64 Signature;
	UINT32 Revision;
	UINT32 HeaderSize;
	UINT32 CRC32;
	UINT32 Reserved;
} EFI_TABLE_HEADER;

typedef struct {
	UINT32 Type;
	UINT32 Pad;
	EFI_PHYSICAL_ADDRESS PhysicalStart;
	EFI_VIRTUAL_ADDRESS VirtualStart;
	UINT64 NumberOfPages;
	UINT64 Attribute;
} EFI_MEMORY_DESCRIPTOR;

struct efi_simple_text_output_protocol;
struct efi_boot_services;
struct efi_system_table;
struct efi_file_protocol;

typedef EFI_STATUS (EFIAPI *EFI_TEXT_STRING)(
	struct efi_simple_text_output_protocol *, const CHAR16 *);

typedef struct efi_simple_text_output_protocol {
	void *Reset;
	EFI_TEXT_STRING OutputString;
	void *TestString;
	void *QueryMode;
	void *SetMode;
	void *SetAttribute;
	void *ClearScreen;
	void *SetCursorPosition;
	void *EnableCursor;
	void *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_PAGES)(int, int, UINTN,
	EFI_PHYSICAL_ADDRESS *);
typedef EFI_STATUS (EFIAPI *EFI_FREE_PAGES)(EFI_PHYSICAL_ADDRESS, UINTN);
typedef EFI_STATUS (EFIAPI *EFI_GET_MEMORY_MAP)(UINTN *,
	EFI_MEMORY_DESCRIPTOR *, UINTN *, UINTN *, UINT32 *);
typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_POOL)(int, UINTN, void **);
typedef EFI_STATUS (EFIAPI *EFI_FREE_POOL)(void *);
typedef EFI_STATUS (EFIAPI *EFI_HANDLE_PROTOCOL)(EFI_HANDLE,
	const EFI_GUID *, void **);
typedef EFI_STATUS (EFIAPI *EFI_EXIT_BOOT_SERVICES)(EFI_HANDLE, UINTN);

typedef struct efi_boot_services {
	EFI_TABLE_HEADER Hdr;
	void *RaiseTPL;
	void *RestoreTPL;
	EFI_ALLOCATE_PAGES AllocatePages;
	EFI_FREE_PAGES FreePages;
	EFI_GET_MEMORY_MAP GetMemoryMap;
	EFI_ALLOCATE_POOL AllocatePool;
	EFI_FREE_POOL FreePool;
	void *CreateEvent;
	void *SetTimer;
	void *WaitForEvent;
	void *SignalEvent;
	void *CloseEvent;
	void *CheckEvent;
	void *InstallProtocolInterface;
	void *ReinstallProtocolInterface;
	void *UninstallProtocolInterface;
	EFI_HANDLE_PROTOCOL HandleProtocol;
	void *Reserved;
	void *RegisterProtocolNotify;
	void *LocateHandle;
	void *LocateDevicePath;
	void *InstallConfigurationTable;
	void *LoadImage;
	void *StartImage;
	void *Exit;
	void *UnloadImage;
	EFI_EXIT_BOOT_SERVICES ExitBootServices;
	void *GetNextMonotonicCount;
	void *Stall;
	void *SetWatchdogTimer;
	void *ConnectController;
	void *DisconnectController;
	void *OpenProtocol;
	void *CloseProtocol;
	void *OpenProtocolInformation;
	void *ProtocolsPerHandle;
	void *LocateHandleBuffer;
	void *LocateProtocol;
	void *InstallMultipleProtocolInterfaces;
	void *UninstallMultipleProtocolInterfaces;
	void *CalculateCrc32;
	void *CopyMem;
	void *SetMem;
	void *CreateEventEx;
} EFI_BOOT_SERVICES;

typedef struct efi_system_table {
	EFI_TABLE_HEADER Hdr;
	CHAR16 *FirmwareVendor;
	UINT32 FirmwareRevision;
	EFI_HANDLE ConsoleInHandle;
	void *ConIn;
	EFI_HANDLE ConsoleOutHandle;
	EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
	EFI_HANDLE StandardErrorHandle;
	EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
	void *RuntimeServices;
	EFI_BOOT_SERVICES *BootServices;
	UINTN NumberOfTableEntries;
	void *ConfigurationTable;
} EFI_SYSTEM_TABLE;

typedef struct {
	UINT32 Revision;
	EFI_HANDLE ParentHandle;
	EFI_SYSTEM_TABLE *SystemTable;
	EFI_HANDLE DeviceHandle;
	void *FilePath;
	void *Reserved;
	UINT32 LoadOptionsSize;
	void *LoadOptions;
	void *ImageBase;
	UINT64 ImageSize;
	int ImageCodeType;
	int ImageDataType;
	void *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_FILE_OPEN)(struct efi_file_protocol *,
	struct efi_file_protocol **, const CHAR16 *, UINT64, UINT64);
typedef EFI_STATUS (EFIAPI *EFI_FILE_CLOSE)(struct efi_file_protocol *);
typedef EFI_STATUS (EFIAPI *EFI_FILE_READ)(struct efi_file_protocol *,
	UINTN *, void *);
typedef EFI_STATUS (EFIAPI *EFI_FILE_SET_POSITION)(
	struct efi_file_protocol *, UINT64);

typedef struct efi_file_protocol {
	UINT64 Revision;
	EFI_FILE_OPEN Open;
	EFI_FILE_CLOSE Close;
	void *Delete;
	EFI_FILE_READ Read;
	void *Write;
	void *GetPosition;
	EFI_FILE_SET_POSITION SetPosition;
	void *GetInfo;
	void *SetInfo;
	void *Flush;
	void *OpenEx;
	void *ReadEx;
	void *WriteEx;
	void *FlushEx;
} EFI_FILE_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_OPEN_VOLUME)(void *, EFI_FILE_PROTOCOL **);
typedef struct {
	UINT64 Revision;
	EFI_OPEN_VOLUME OpenVolume;
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

static const EFI_GUID EFI_LOADED_IMAGE_PROTOCOL_GUID = {
	0x5b1b31a1, 0x9562, 0x11d2,
	{ 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b }
};
static const EFI_GUID EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID = {
	0x964e5b22, 0x6459, 0x11d2,
	{ 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b }
};

#endif
