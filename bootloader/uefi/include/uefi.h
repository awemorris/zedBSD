/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The minimal UEFI declarations used by the zedBSD loaders (x64 and
 * riscv64).
 *
 * Only the services and fields the loader uses are typed; every other table
 * slot keeps its place as an untyped pointer.
 */

#ifndef KERN_BOOTLOADER_UEFI_H
#define KERN_BOOTLOADER_UEFI_H

#include <stddef.h>
#include <stdint.h>

/*
 * The firmware's calling convention: the Microsoft one on x64, and the
 * processor's own on RISC-V, where UEFI uses the standard calling
 * convention unchanged.
 */
#if defined(__x86_64__)
#define EFIAPI __attribute__((ms_abi))
#else
#define EFIAPI
#endif

/* KeyShiftState: the other bits are meaningful only with this one. */
#define EFI_SHIFT_STATE_VALID 0x80000000U
#define EFI_RIGHT_SHIFT_PRESSED 0x00000001U
#define EFI_LEFT_SHIFT_PRESSED 0x00000002U
#define EFI_RIGHT_CONTROL_PRESSED 0x00000004U
#define EFI_LEFT_CONTROL_PRESSED 0x00000008U

/* KeyToggleState: valid, and modifier keys alone reported as events. */
#define EFI_TOGGLE_STATE_VALID 0x80U
#define EFI_KEY_STATE_EXPOSED 0x40U

/* The status codes the loader tells apart; an error has the top bit set. */
#define EFI_SUCCESS ((EFI_STATUS)0)
#define EFI_LOAD_ERROR (0x8000000000000001ULL)
#define EFI_INVALID_PARAMETER (0x8000000000000002ULL)
#define EFI_UNSUPPORTED (0x8000000000000003ULL)
#define EFI_BAD_BUFFER_SIZE (0x8000000000000004ULL)
#define EFI_BUFFER_TOO_SMALL (0x8000000000000005ULL)
#define EFI_NOT_READY (0x8000000000000006ULL)
#define EFI_DEVICE_ERROR (0x8000000000000007ULL)
#define EFI_OUT_OF_RESOURCES (0x8000000000000009ULL)
#define EFI_NO_MEDIA (0x800000000000000cULL)
#define EFI_MEDIA_CHANGED (0x800000000000000dULL)
#define EFI_NOT_FOUND (0x800000000000000eULL)
#define EFI_ACCESS_DENIED (0x800000000000000fULL)
#define EFI_ERROR(status) (((status) & 0x8000000000000000ULL) != 0)

/* The file open mode and attribute the loader uses. */
#define EFI_FILE_MODE_READ 0x0000000000000001ULL
#define EFI_FILE_DIRECTORY 0x0000000000000010ULL

/* How AllocatePages chooses the pages. */
enum efi_allocate_type {
	AllocateAnyPages,
	AllocateMaxAddress,
	AllocateAddress,
	MaxAllocateType
};

/* The memory types of the firmware's memory map and allocations. */
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

/* How LocateHandleBuffer chooses the handles. */
enum efi_locate_search_type {
	AllHandles,
	ByRegisterNotify,
	ByProtocol
};

/* The UEFI specification's scalar types. */
typedef uint8_t BOOLEAN;
typedef uint16_t CHAR16;
typedef int16_t INT16;
typedef uint64_t EFI_STATUS;
typedef void *EFI_HANDLE;
typedef void *EFI_EVENT;
typedef uint64_t EFI_PHYSICAL_ADDRESS;
typedef uint64_t EFI_VIRTUAL_ADDRESS;
typedef uintptr_t UINTN;
typedef uint32_t UINT32;
typedef uint64_t UINT64;

/* A protocol or table identifier. */
typedef struct {
	uint32_t Data1;
	uint16_t Data2;
	uint16_t Data3;
	uint8_t Data4[8];
} EFI_GUID;

/* The header every UEFI table starts with. */
typedef struct {
	UINT64 Signature;
	UINT32 Revision;
	UINT32 HeaderSize;
	UINT32 CRC32;
	UINT32 Reserved;
} EFI_TABLE_HEADER;

/* One entry of the system table's configuration tables, such as ACPI's. */
typedef struct {
	EFI_GUID VendorGuid;
	void *VendorTable;
} EFI_CONFIGURATION_TABLE;

/* The attribute of a range the firmware keeps using at runtime. */
#define EFI_MEMORY_RUNTIME 0x8000000000000000ULL

/* One range of the firmware's memory map. */
typedef struct {
	UINT32 Type;
	UINT32 Pad;
	EFI_PHYSICAL_ADDRESS PhysicalStart;
	EFI_VIRTUAL_ADDRESS VirtualStart;
	UINT64 NumberOfPages;
	UINT64 Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* A time stamp of a file. */
typedef struct {
	uint16_t Year;
	uint8_t Month;
	uint8_t Day;
	uint8_t Hour;
	uint8_t Minute;
	uint8_t Second;
	uint8_t Pad1;
	UINT32 Nanosecond;
	INT16 TimeZone;
	uint8_t Daylight;
	uint8_t Pad2;
} EFI_TIME;

/* The information GetInfo reports for a file. */
typedef struct {
	UINT64 Size;
	UINT64 FileSize;
	UINT64 PhysicalSize;
	EFI_TIME CreateTime;
	EFI_TIME LastAccessTime;
	EFI_TIME ModificationTime;
	UINT64 Attribute;
	CHAR16 FileName[1];
} EFI_FILE_INFO;

/* The protocol structures the function types below refer to. */
struct efi_simple_text_output_protocol;
struct efi_boot_services;
struct efi_system_table;
struct efi_file_protocol;
struct efi_graphics_output_protocol;
struct efi_block_io_protocol;

/* The header of one device path node. */
typedef struct {
	uint8_t Type;
	uint8_t SubType;
	uint8_t Length[2];
} EFI_DEVICE_PATH_PROTOCOL;

/* Writes a string to a text console. */
typedef EFI_STATUS(EFIAPI *EFI_TEXT_STRING)(
    struct efi_simple_text_output_protocol *, const CHAR16 *);

/* The text console the loader writes its messages to. */
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

/* One key event as the console input reports it: a scan code or a character. */
typedef struct {
	uint16_t ScanCode;
	CHAR16 UnicodeChar;
} EFI_INPUT_KEY;

/* The modifier and toggle keys' state that came with one key event. */
typedef struct {
	UINT32 KeyShiftState;
	uint8_t KeyToggleState;
} EFI_KEY_STATE;

/* One key event with the modifier state (the extended console input). */
typedef struct {
	EFI_INPUT_KEY Key;
	EFI_KEY_STATE KeyState;
} EFI_KEY_DATA;

/* The extended console input, which its own function pointers refer to. */
struct efi_simple_text_input_ex_protocol;

/* The extended console input: key events with the modifier keys' state. */
typedef struct efi_simple_text_input_ex_protocol {
	EFI_STATUS (EFIAPI *Reset)(struct efi_simple_text_input_ex_protocol *,
	    BOOLEAN);
	EFI_STATUS (EFIAPI *ReadKeyStrokeEx)(
	    struct efi_simple_text_input_ex_protocol *, EFI_KEY_DATA *);
	EFI_EVENT WaitForKeyEx;
	EFI_STATUS (EFIAPI *SetState)(struct efi_simple_text_input_ex_protocol *,
	    uint8_t *);
	void *RegisterKeyNotify;
	void *UnregisterKeyNotify;
} EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL;

/* The boot services the loader calls. */
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
typedef EFI_STATUS(EFIAPI *EFI_LOCATE_HANDLE_BUFFER)(
	int, const EFI_GUID *, void *, UINTN *, EFI_HANDLE **);
typedef EFI_STATUS(EFIAPI *EFI_CONNECT_CONTROLLER)(EFI_HANDLE, EFI_HANDLE *,
						   void *, BOOLEAN);

/*
 * The runtime service that moves the firmware's runtime services to the
 * virtual addresses its memory descriptors name.
 */
typedef EFI_STATUS(EFIAPI *EFI_SET_VIRTUAL_ADDRESS_MAP)(
	UINTN, UINTN, UINT32, EFI_MEMORY_DESCRIPTOR *);

/*
 * The runtime services table, up to the one service the loader calls.
 *
 * It stays valid after ExitBootServices, at its physical address until
 * SetVirtualAddressMap has been called.
 */
typedef struct efi_runtime_services {
	EFI_TABLE_HEADER Hdr;
	void *GetTime;
	void *SetTime;
	void *GetWakeupTime;
	void *SetWakeupTime;
	EFI_SET_VIRTUAL_ADDRESS_MAP SetVirtualAddressMap;
} EFI_RUNTIME_SERVICES;

/* The boot services table, valid until ExitBootServices. */
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
	EFI_CONNECT_CONTROLLER ConnectController;
	void *DisconnectController;
	void *OpenProtocol;
	void *CloseProtocol;
	void *OpenProtocolInformation;
	void *ProtocolsPerHandle;
	EFI_LOCATE_HANDLE_BUFFER LocateHandleBuffer;
	EFI_LOCATE_PROTOCOL LocateProtocol;
	void *InstallMultipleProtocolInterfaces;
	void *UninstallMultipleProtocolInterfaces;
	void *CalculateCrc32;
	void *CopyMem;
	void *SetMem;
	void *CreateEventEx;
} EFI_BOOT_SERVICES;

/* The system table the firmware passes to the loader. */
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
	EFI_RUNTIME_SERVICES *RuntimeServices;
	EFI_BOOT_SERVICES *BootServices;
	UINTN NumberOfTableEntries;
	EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* The loader's own image: where it was loaded from. */
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

/* The file services the loader calls. */
typedef EFI_STATUS(EFIAPI *EFI_FILE_OPEN)(struct efi_file_protocol *,
					  struct efi_file_protocol **,
					  const CHAR16 *, UINT64, UINT64);
typedef EFI_STATUS(EFIAPI *EFI_FILE_CLOSE)(struct efi_file_protocol *);
typedef EFI_STATUS(EFIAPI *EFI_FILE_READ)(struct efi_file_protocol *, UINTN *,
					  void *);
typedef EFI_STATUS(EFIAPI *EFI_FILE_SET_POSITION)(struct efi_file_protocol *,
						  UINT64);
typedef EFI_STATUS(EFIAPI *EFI_FILE_GET_INFO)(struct efi_file_protocol *,
					      const EFI_GUID *, UINTN *, void *);

/* An open file or directory of a volume. */
typedef struct efi_file_protocol {
	UINT64 Revision;
	EFI_FILE_OPEN Open;
	EFI_FILE_CLOSE Close;
	void *Delete;
	EFI_FILE_READ Read;
	void *Write;
	void *GetPosition;
	EFI_FILE_SET_POSITION SetPosition;
	EFI_FILE_GET_INFO GetInfo;
	void *SetInfo;
	void *Flush;
	void *OpenEx;
	void *ReadEx;
	void *WriteEx;
	void *FlushEx;
} EFI_FILE_PROTOCOL;

/* Opens a volume's root directory. */
typedef EFI_STATUS(EFIAPI *EFI_OPEN_VOLUME)(void *, EFI_FILE_PROTOCOL **);

/* A volume with a file system the firmware reads. */
typedef struct {
	UINT64 Revision;
	EFI_OPEN_VOLUME OpenVolume;
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

/* The medium behind a block device. */
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

/* Reads blocks of a block device. */
typedef EFI_STATUS(EFIAPI *EFI_BLOCK_READ)(struct efi_block_io_protocol *,
					   UINT32, UINT64, UINTN, void *);

/* A block device the loader reads a volume's boot sector from. */
typedef struct efi_block_io_protocol {
	UINT64 Revision;
	EFI_BLOCK_IO_MEDIA *Media;
	void *Reset;
	EFI_BLOCK_READ ReadBlocks;
	void *WriteBlocks;
	void *FlushBlocks;
} EFI_BLOCK_IO_PROTOCOL;

/* The layout of a graphics mode's pixels. */
enum efi_graphics_pixel_format {
	PixelRedGreenBlueReserved8BitPerColor,
	PixelBlueGreenRedReserved8BitPerColor,
	PixelBitMask,
	PixelBltOnly,
	PixelFormatMax
};

/* The channel masks of a PixelBitMask mode. */
typedef struct {
	UINT32 RedMask;
	UINT32 GreenMask;
	UINT32 BlueMask;
	UINT32 ReservedMask;
} EFI_PIXEL_BITMASK;

/* The description of one graphics mode. */
typedef struct {
	UINT32 Version;
	UINT32 HorizontalResolution;
	UINT32 VerticalResolution;
	UINT32 PixelFormat;
	EFI_PIXEL_BITMASK PixelInformation;
	UINT32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

/* The active graphics mode and its framebuffer. */
typedef struct {
	UINT32 MaxMode;
	UINT32 Mode;
	EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
	UINTN SizeOfInfo;
	EFI_PHYSICAL_ADDRESS FrameBufferBase;
	UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

/* The graphics output the loader picks a mode and a framebuffer from. */
typedef struct efi_graphics_output_protocol {
	EFI_STATUS (EFIAPI *QueryMode)(struct efi_graphics_output_protocol *,
	    UINT32, UINTN *, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **);
	EFI_STATUS (EFIAPI *SetMode)(struct efi_graphics_output_protocol *, UINT32);
	void *Blt;
	EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/*
 * The identifiers of the protocols and tables the loader asks the firmware
 * for.  Each translation unit that includes this header has its own copy;
 * they are constants and never change.
 */

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

static const EFI_GUID EFI_DEVICE_PATH_PROTOCOL_GUID = {
    0x09576e91,
    0x6d3f,
    0x11d2,
    {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};

static const EFI_GUID EFI_FILE_INFO_ID = {
    0x09576e92,
    0x6d3f,
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

/*
 * The protocol a RISC-V firmware reports the boot hart's id through: the
 * application's entry has no register for it.
 */
struct riscv_efi_boot_protocol;
typedef EFI_STATUS(EFIAPI *RISCV_EFI_GET_BOOT_HARTID)(
    struct riscv_efi_boot_protocol *, UINTN *);
typedef struct riscv_efi_boot_protocol {
	UINT64 Revision;
	RISCV_EFI_GET_BOOT_HARTID GetBootHartId;
} RISCV_EFI_BOOT_PROTOCOL;

/*
 * The RISC-V boot protocol, and the configuration table that holds the
 * flattened device tree a firmware passes on a board without ACPI.
 *
 * Values referenced: riscv-non-isa/riscv-uefi boot_protocol.adoc
 * (RISCV_EFI_BOOT_PROTOCOL_GUID), Linux 6.8.12 include/linux/efi.h
 * (RISCV_EFI_BOOT_PROTOCOL_GUID, DEVICE_TREE_GUID).
 */
static const EFI_GUID RISCV_EFI_BOOT_PROTOCOL_GUID = {
    0xccd15fec,
    0x6f73,
    0x4eec,
    {0x83, 0x95, 0x3e, 0x69, 0xe4, 0xb9, 0x40, 0xbf}};

static const EFI_GUID EFI_DEVICE_TREE_GUID = {
    0xb1b621d5,
    0xf19c,
    0x41a5,
    {0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0}};

static const EFI_GUID EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID = {
    0xdd9e7534,
    0x7762,
    0x4698,
    {0x8c, 0x14, 0xf5, 0x85, 0x17, 0xa6, 0x25, 0xaa}};

#endif
