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

#define EFI_SUCCESS ((EFI_STATUS)0)
#define EFI_LOAD_ERROR (0x8000000000000001ULL)
#define EFI_INVALID_PARAMETER (0x8000000000000002ULL)
#define EFI_UNSUPPORTED (0x8000000000000003ULL)
#define EFI_BUFFER_TOO_SMALL (0x8000000000000005ULL)
#define EFI_NOT_FOUND (0x800000000000000eULL)
#define EFI_ERROR(status) (((status) & 0x8000000000000000ULL) != 0)

#define EFI_FILE_MODE_READ 0x0000000000000001ULL

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
	EFI_GUID VendorGuid;
	void *VendorTable;
} EFI_CONFIGURATION_TABLE;

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
struct efi_graphics_output_protocol;
struct efi_block_io_protocol;

typedef EFI_STATUS(EFIAPI *EFI_TEXT_STRING)(
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

typedef EFI_STATUS(EFIAPI *EFI_ALLOCATE_PAGES)(int, int, UINTN,
					       EFI_PHYSICAL_ADDRESS *);
typedef EFI_STATUS(EFIAPI *EFI_FREE_PAGES)(EFI_PHYSICAL_ADDRESS, UINTN);
typedef EFI_STATUS(EFIAPI *EFI_GET_MEMORY_MAP)(UINTN *, EFI_MEMORY_DESCRIPTOR *,
					       UINTN *, UINTN *, UINT32 *);
typedef EFI_STATUS(EFIAPI *EFI_ALLOCATE_POOL)(int, UINTN, void **);
typedef EFI_STATUS(EFIAPI *EFI_FREE_POOL)(void *);
typedef EFI_STATUS(EFIAPI *EFI_HANDLE_PROTOCOL)(EFI_HANDLE, const EFI_GUID *,
						void **);
typedef EFI_STATUS(EFIAPI *EFI_EXIT_BOOT_SERVICES)(EFI_HANDLE, UINTN);
typedef EFI_STATUS(EFIAPI *EFI_LOCATE_PROTOCOL)(const EFI_GUID *, void *,
						void **);

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
	EFI_LOCATE_PROTOCOL LocateProtocol;
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
	EFI_CONFIGURATION_TABLE *ConfigurationTable;
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

typedef EFI_STATUS(EFIAPI *EFI_FILE_OPEN)(struct efi_file_protocol *,
					  struct efi_file_protocol **,
					  const CHAR16 *, UINT64, UINT64);
typedef EFI_STATUS(EFIAPI *EFI_FILE_CLOSE)(struct efi_file_protocol *);
typedef EFI_STATUS(EFIAPI *EFI_FILE_READ)(struct efi_file_protocol *, UINTN *,
					  void *);
typedef EFI_STATUS(EFIAPI *EFI_FILE_SET_POSITION)(struct efi_file_protocol *,
						  UINT64);

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

typedef EFI_STATUS(EFIAPI *EFI_OPEN_VOLUME)(void *, EFI_FILE_PROTOCOL **);
typedef struct {
	UINT64 Revision;
	EFI_OPEN_VOLUME OpenVolume;
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef struct {
	UINT32 MediaId;
	BOOLEAN RemovableMedia;
	BOOLEAN MediaPresent;
	BOOLEAN LogicalPartition;
	BOOLEAN ReadOnly;
	BOOLEAN WriteCaching;
	UINT32 BlockSize;
	UINT32 IoAlign;
	UINT64 LastBlock;
} EFI_BLOCK_IO_MEDIA;

typedef EFI_STATUS(EFIAPI *EFI_BLOCK_READ)(struct efi_block_io_protocol *,
					   UINT32, UINT64, UINTN, void *);

typedef struct efi_block_io_protocol {
	UINT64 Revision;
	EFI_BLOCK_IO_MEDIA *Media;
	void *Reset;
	EFI_BLOCK_READ ReadBlocks;
	void *WriteBlocks;
	void *FlushBlocks;
} EFI_BLOCK_IO_PROTOCOL;

enum efi_graphics_pixel_format {
	PixelRedGreenBlueReserved8BitPerColor,
	PixelBlueGreenRedReserved8BitPerColor,
	PixelBitMask,
	PixelBltOnly,
	PixelFormatMax
};

typedef struct {
	UINT32 RedMask;
	UINT32 GreenMask;
	UINT32 BlueMask;
	UINT32 ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
	UINT32 Version;
	UINT32 HorizontalResolution;
	UINT32 VerticalResolution;
	UINT32 PixelFormat;
	EFI_PIXEL_BITMASK PixelInformation;
	UINT32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
	UINT32 MaxMode;
	UINT32 Mode;
	EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
	UINTN SizeOfInfo;
	EFI_PHYSICAL_ADDRESS FrameBufferBase;
	UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct efi_graphics_output_protocol {
	void *QueryMode;
	void *SetMode;
	void *Blt;
	EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

static const EFI_GUID EFI_LOADED_IMAGE_PROTOCOL_GUID = {
    0x5b1b31a1,
    0x9562,
    0x11d2,
    {0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static const EFI_GUID EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID = {
    0x964e5b22,
    0x6459,
    0x11d2,
    {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static const EFI_GUID EFI_BLOCK_IO_PROTOCOL_GUID = {
    0x964e5b21,
    0x6459,
    0x11d2,
    {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static const EFI_GUID EFI_ACPI_TABLE_GUID = {
    0xeb9d2d30,
    0x2d88,
    0x11d3,
    {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}};
static const EFI_GUID EFI_ACPI_20_TABLE_GUID = {
    0x8868e871,
    0xe4f1,
    0x11d3,
    {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}};
static const EFI_GUID EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID = {
    0x9042a9de,
    0x23dc,
    0x4a38,
    {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}};

#endif
