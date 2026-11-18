/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Intel AX211 private core contract.
 *
 * The values of the device and firmware interface that the components of the
 * driver share, with what the driver established about each of them on AX211
 * hardware: where the value is used, what it means to the device or to the
 * firmware, the order the device requires, and how the firmware behaved.
 */

#ifndef KERN_DRIVERS_INTEL_AX211_INTERNAL_H
#define KERN_DRIVERS_INTEL_AX211_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

/*
 * The PCI identity of the one AX211 module this driver binds.
 *
 * The read-only hardware intake of the Latitude test machines (ws004 HW-T37)
 * found the CNVio2 function at 8086:51f0 with subsystem 8086:4090 and
 * revision 01.  The driver attaches only on an exact match of all five
 * values, so a different AX2xx part is left alone instead of being driven
 * with this firmware.
 *
 * Values referenced: the PCI ID database (vendor 8086, device 51f0).
 */
#define INTEL_AX211_PCI_VENDOR_ID 0x8086U
#define INTEL_AX211_PCI_DEVICE_ID 0x51f0U
#define INTEL_AX211_PCI_SUBVENDOR_ID 0x8086U
#define INTEL_AX211_PCI_SUBDEVICE_ID 0x4090U
#define INTEL_AX211_PCI_REVISION 0x01U

/*
 * The silicon and radio identity the firmware image is built for.
 *
 * The MAC type is the 12-bit field at bits 15:4 of the silicon revision
 * register and is 0x37 on So and 0x43 on SoF; the radio kind is the 12-bit
 * field at bits 23:12 of the radio identity register and is 0x10d on Garfield
 * Peak.  The same pair of codes reappears inside the PNVM file as the
 * hardware-kind record, and drv_intel_ax211_pnvm_select() accepts a PNVM
 * section only when that record carries this pair.  CONCURRENT_BANDS is zero
 * because this module has one LMAC and cannot run two bands at once, so every
 * command that takes an LMAC number is sent with zero.  The CRF, CNV, WFPM and
 * raw radio identity words are the values of the exact unit this driver
 * targets; the driver does not read them, and the host tests pin them so that
 * the identity record stays complete.
 *
 * Values referenced: Linux v6.17
 * drivers/net/wireless/intel/iwlwifi/iwl-config.h (the MAC and RF type codes
 * of the So family and of the Garfield Peak radio) and
 * drivers/net/wireless/intel/iwlwifi/iwl-csr.h (the fields they are read
 * from).
 */
#define INTEL_AX211_SILICON_SO 0x37U
#define INTEL_AX211_SILICON_SOF 0x43U
#define INTEL_AX211_CRF_ID 0x401410U
#define INTEL_AX211_CNV_ID 0x80400U
#define INTEL_AX211_WFPM_ID 0x80000020U
#define INTEL_AX211_RAW_RF_ID 0x2010d000U
#define INTEL_AX211_RADIO_GF 0x10dU
#define INTEL_AX211_RADIO_CONCURRENT_BANDS 0U
#define INTEL_AX211_RADIO_JACKET 1U

/*
 * The one firmware image the driver accepts.
 *
 * The size, the SHA-256 and the version record were measured on the file by
 * zedBSD.  The driver refuses any other image: the command versions, the
 * command layouts and the boot order in this driver were established against
 * API 89 of this build, and nothing guarantees that another build keeps them.
 *
 * Value referenced: linux-firmware 20260410 (commit
 * dc85ccedc9c973682fbcf4d628ca61174bcc3120)
 * intel/iwlwifi/iwlwifi-so-a0-gf-a0-89.ucode.
 */
#define INTEL_AX211_FIRMWARE_PATH	"intel/iwlwifi/iwlwifi-so-a0-gf-a0-89.ucode"
#define INTEL_AX211_FIRMWARE_SIZE	1736748U
#define INTEL_AX211_FIRMWARE_SHA256     "c569c4b0ffe2054a1cedd5affccff2da8515325eeb23f788c7abe9463d1a1514"
#define INTEL_AX211_FIRMWARE_VERSION	"89.735b75a4.0"
#define INTEL_AX211_FIRMWARE_API	89U
#define INTEL_AX211_FIRMWARE_MINOR	0x735b75a4U
#define INTEL_AX211_FIRMWARE_SERIAL	0U

/*
 * The platform data (PNVM) file that belongs to that image.
 *
 * The size and the SHA-256 were measured on the file by zedBSD.  The file
 * holds radio tuning sections for several boards; the driver copies only the
 * sections of the board that the firmware names in its ALIVE notification.
 *
 * Value referenced: linux-firmware 20260410 (commit
 * dc85ccedc9c973682fbcf4d628ca61174bcc3120)
 * intel/iwlwifi/iwlwifi-so-a0-gf-a0.pnvm.
 */
#define INTEL_AX211_PNVM_PATH		"intel/iwlwifi/iwlwifi-so-a0-gf-a0.pnvm"
#define INTEL_AX211_PNVM_FILE_SIZE	55176U
#define INTEL_AX211_PNVM_SHA256         "efa9726d4a9d44b83fc9a14cedcf306a4e439e9de919802eb9e92df4ec032b2a"

/*
 * The release tag and the commit of the repository the two files above were
 * taken from.
 */
#define INTEL_AX211_LINUX_FIRMWARE_TAG		"20260410"
#define INTEL_AX211_LINUX_FIRMWARE_COMMIT	"dc85ccedc9c973682fbcf4d628ca61174bcc3120"

/*
 * The framing of the firmware file and of the PNVM file.
 *
 * The firmware file starts with an 88-byte header: a zero word, the magic word
 * 0x0a4c5749 ("IWL\n" in little-endian order), a 64-byte human-readable build
 * name, the header version at byte 72 and the build number at byte 76.
 * Records follow, each an 8-byte header (type and length, both little-endian
 * words) and a body padded to four bytes.  The PNVM file is the same record
 * stream without the 88-byte header.  The section limits are zedBSD's bounds.
 * The host test of the pinned files found 60 runtime entries in the image (15
 * LMAC, 17 UMAC and 26 paged sections plus the two marks below, none longer
 * than 32 KiB) and 8 sections for 4 boards in the PNVM file.
 *
 * Values referenced: Linux v6.17 drivers/net/wireless/intel/iwlwifi/fw/file.h
 * (struct iwl_tlv_ucode_header, struct iwl_ucode_tlv).
 */
#define INTEL_AX211_FIRMWARE_FILE_MAGIC		0x0a4c5749U
#define INTEL_AX211_FIRMWARE_FILE_HEADER_SIZE	88U
#define INTEL_AX211_FIRMWARE_RECORD_HEADER_SIZE	8U
#define INTEL_AX211_FIRMWARE_SECTION_LIMIT	69U
#define INTEL_AX211_PNVM_SECTION_LIMIT		64U

/*
 * The command ring and the receive ring, as the device sees them.
 *
 * The device is told the size of each ring as a code, not a count: the command
 * ring of 256 entries is announced as 5 (log2 of the count minus 3, because
 * that ring is counted in groups of eight), the receive ring of 512 entries as
 * 9 (log2 of the count).  A receive transfer descriptor is 16 bytes (buffer id
 * at byte 0, buffer address at byte 8); a receive completion descriptor is 32
 * bytes and the device writes the buffer id back at byte 4 and its flags at
 * byte 6.
 *
 * Values referenced: Linux v6.17 drivers/net/wireless/intel/iwlwifi/iwl-fh.h
 * (RX_QUEUE_CB_SIZE, TFD_QUEUE_CB_SIZE, struct iwl_rx_transfer_desc, struct
 * iwl_rx_completion_desc) and
 * drivers/net/wireless/intel/iwlwifi/pcie/iwl-context-info-v2.h.
 */
#define INTEL_AX211_COMMAND_RING_SIZE			256U
#define INTEL_AX211_RX_RING_SIZE			512U
#define INTEL_AX211_COMMAND_RING_CB_SIZE		5U
#define INTEL_AX211_RX_RING_CB_SIZE			9U
#define INTEL_AX211_MAX_COMMAND_PAYLOAD			4088U
#define INTEL_AX211_BOOT_CONTEXT_SIZE			104U
#define INTEL_AX211_RX_TRANSFER_DESCRIPTOR_SIZE		16U
#define INTEL_AX211_RX_COMPLETION_DESCRIPTOR_SIZE	32U

/*
 * The transfer descriptor (TFD) of the Gen3 transmit queues.
 *
 * A descriptor is 256 bytes: a 16-bit buffer count, then up to 25 buffer
 * entries of 10 bytes (a 16-bit length and a 64-bit address).  A buffer may
 * not exceed 4092 bytes.  The command ring and every data ring use this one
 * layout.
 *
 * Values referenced: Linux v6.17 drivers/net/wireless/intel/iwlwifi/iwl-fh.h
 * (struct iwl_tfh_tfd, struct iwl_tfh_tb).
 */
#define INTEL_AX211_TRANSFER_DESCRIPTOR_SIZE		256U
#define INTEL_AX211_TRANSFER_DESCRIPTOR_BUFFERS		25U
#define INTEL_AX211_TRANSFER_BUFFER_LENGTH_MAX		4092U

/*
 * The headers in front of a command and of a firmware event.
 *
 * The narrow header is 4 bytes: opcode, flags, ring index, ring number.  The
 * wide header adds a command family byte in place of the flags, a 16-bit
 * payload length at byte 4, a reserved byte and the layout version at byte 7.
 * An event starts with a 32-bit word whose low 14 bits are the frame length
 * and whose bits 21:16 name the receive queue, followed by a narrow header;
 * the payload therefore starts at byte 8.  The staging capacity is zedBSD's
 * own bound for one copied event.
 *
 * Values referenced: Linux v6.17
 * drivers/net/wireless/intel/iwlwifi/fw/api/cmdhdr.h (struct iwl_cmd_header,
 * struct iwl_cmd_header_wide, struct iwl_rx_packet).
 */
#define INTEL_AX211_NARROW_COMMAND_HEADER_SIZE		4U
#define INTEL_AX211_WIDE_COMMAND_HEADER_SIZE		8U
#define INTEL_AX211_EVENT_HEADER_SIZE			8U
#define INTEL_AX211_STAGING_CAPACITY			4096U

/*
 * The destination words that mark section boundaries instead of naming a
 * load address.
 *
 * In the runtime section list of the firmware file, a section whose
 * destination is 0xffffcccc separates the lower-MAC (LMAC) sections from the
 * upper-MAC (UMAC) sections, and one whose destination is 0xaaaabbbb separates
 * the UMAC sections from the paged sections.  Both marks carry a body of
 * exactly one zero word, and the firmware loader in this file refuses an image
 * that lacks either mark or repeats it.  In the PNVM file, a section with
 * destination 0xddddeeee closes a board's data and is not copied.
 *
 * Values referenced: Linux v6.17 drivers/net/wireless/intel/iwlwifi/fw/file.h
 * (the first two marks) and drivers/net/wireless/intel/iwlwifi/fw/pnvm.c (the
 * PNVM reader, the third mark).
 */
#define INTEL_AX211_UPPER_CPU_IMAGE_MARK		0xffffccccU
#define INTEL_AX211_PAGED_IMAGE_MARK			0xaaaabbbbU
#define INTEL_AX211_PNVM_IMAGE_MARK			0xddddeeeeU

/* The results of the shared parsers, encoders and ring bookkeeping. */
enum intel_ax211_result {
	INTEL_AX211_SUCCESS = 0,
	INTEL_AX211_INVALID = 1,
	INTEL_AX211_TRUNCATED = 2,
	INTEL_AX211_OVERFLOW = 3,
	INTEL_AX211_DUPLICATE = 4,
	INTEL_AX211_UNSUPPORTED = 5,
	INTEL_AX211_MISSING = 6,
	INTEL_AX211_IDENTITY_MISMATCH = 7,
	INTEL_AX211_FULL = 8,
	INTEL_AX211_STALE = 9
};

/* The PCI identity of a device, compared with the one supported AX211. */
struct intel_ax211_identity {
	uint16_t vendor;
	uint16_t device;
	uint16_t subvendor;
	uint16_t subdevice;
	uint8_t revision;
};

/*
 * One image section: the device address it loads to, and where it lies in the
 * file.
 */
struct intel_ax211_section {
	uint32_t destination;
	size_t file_offset;
	size_t length;
};

/*
 * What the firmware file parser found: the image's version, API and radio
 * profile, the loader image, the command-version table, the runtime sections
 * of each CPU, and the API and capability flags.
 */
struct intel_ax211_firmware_manifest {
	uint32_t header_version;
	uint32_t build;
	uint32_t api_major;
	uint32_t api_minor;
	uint32_t api_serial;
	uint32_t radio_profile;
	uint32_t cpu_count;
	size_t iml_offset;
	size_t iml_length;
	size_t command_versions_offset;
	size_t command_versions_length;
	struct intel_ax211_section runtime[INTEL_AX211_FIRMWARE_SECTION_LIMIT];
	size_t runtime_count;
	size_t lmac_count;
	size_t umac_count;
	size_t paging_count;
	uint32_t api_changes[4];
	uint32_t capabilities[5];
};

/*
 * The three words that name the board a firmware instance runs on.
 *
 * The firmware reports these words in its ALIVE notification; the PNVM file
 * opens each board's data with a 12-byte record carrying the same three words.
 * The driver copies the PNVM sections of the one board whose words are equal
 * to the reported ones, so the signature is the key that ties a running
 * firmware to its radio tuning data.
 *
 * Values referenced: Linux v6.17
 * drivers/net/wireless/intel/iwlwifi/fw/api/alive.h (the SKU words of the
 * ALIVE notification) and drivers/net/wireless/intel/iwlwifi/fw/file.h (the
 * PNVM SKU record, type 64).
 */
struct intel_ax211_board_signature {
	uint32_t data[3];
};

/*
 * The PNVM data selected for one board: its signature, version and hardware
 * type, and the sections to copy.
 */
struct intel_ax211_pnvm_manifest {
	struct intel_ax211_board_signature sku;
	uint32_t version;
	uint16_t mac_type;
	uint16_t rf_id;
	struct intel_ax211_section section[INTEL_AX211_PNVM_SECTION_LIMIT];
	size_t section_count;
	size_t total_length;
};

/* What a PNVM file holds, counted for inspection. */
struct intel_ax211_pnvm_inventory {
	size_t sku_count;
	size_t version_count;
	size_t hardware_type_count;
	size_t supported_hardware_type_count;
	size_t section_count;
	size_t total_section_length;
};

/*
 * The boot context: the one structure the device reads to find the host's
 * rings when the firmware starts.
 *
 * The field order and widths are the device's DMA layout.
 *
 * drv_intel_ax211_boot_context_encode() writes these fields as 104
 * little-endian bytes into a coherent DMA page, and the address of that page
 * goes into the boot context register before the upper CPU is started.  The
 * device reads the page when the firmware starts and then works from the
 * rings it names.  drv_intel_ax211_dma_prepare_boot() writes a new page for
 * every boot attempt instead of reusing an old one.  Byte offsets on the
 * wire, in field order: version 0, (a 16-bit length in words, always 26,
 * written by the encoder at 2), config 4, status page 8, the four index
 * arrays 16, 24, 32 and 40, the two index slot counts 48 and 50, the two
 * ring addresses 52 and 60, the two size codes 68 and 70, the doorbells 72
 * and 74, the vectors 76 and 78, the four entry header and footer sizes 80
 * to 83, the ring control flags 84, the status page vector 86, the scratch
 * area address 88 and its size 96; bytes 100 to 103 are reserved and zero.
 *
 * How the fields are used on the AX211, as found by this driver:
 * - version and config are zero; the API 89 firmware boots with them so.
 * - status_page_address is a 4 KiB page the firmware owns for its own
 *   bookkeeping.  The driver puts the two tail-index arrays inside it, at
 *   2048 and 3072 bytes from its start (half and three quarters of the page).
 * - completion_head_array_address is a two-byte DMA word in which the device
 *   publishes how far it has filled the receive completion ring.  The
 *   transport reads it as a little-endian 16-bit index to learn which
 *   completions are new.
 * - transfer_head_array_address and the two index slot counts stay zero: this
 *   driver uses no per-queue head array.
 * - command_ring_address is the command TFD ring and response_ring_address
 *   is the receive completion ring; command responses and notifications all
 *   arrive through receive completions.
 * - the size codes must be 5 for the 256-entry command ring and 9 for the
 *   512-entry receive ring, and the encoder refuses any other value.
 * - doorbells, vectors, entry header and footer sizes, ring control flags and
 *   the status page vector are zero.  The API 89 firmware boots with them so,
 *   and every interrupt then arrives on MSI-X vector 0, the only vector the
 *   transport routes.
 * - scratch_area_address and scratch_area_size name the 1660-byte scratch
 *   area in which the driver tells the firmware where the PNVM sections, the
 *   receive buffers and the firmware's own DRAM copies are.
 *
 * Values referenced: Linux v6.17
 * drivers/net/wireless/intel/iwlwifi/pcie/iwl-context-info-v2.h (struct
 * iwl_context_info_v2) and OpenBSD 0f464d413c50 sys/dev/pci/if_iwxreg.h
 * (struct iwx_context_info_gen3).
 */
struct intel_ax211_boot_context {
	uint16_t version;
	uint32_t config;
	uint64_t status_page_address;
	uint64_t completion_head_array_address;
	uint64_t transfer_tail_array_address;
	uint64_t completion_tail_array_address;
	uint64_t transfer_head_array_address;
	uint16_t completion_index_slots;
	uint16_t transfer_index_slots;
	uint64_t command_ring_address;
	uint64_t response_ring_address;
	uint16_t command_ring_size_code;
	uint16_t response_ring_size_code;
	uint16_t command_ring_doorbell;
	uint16_t response_ring_doorbell;
	uint16_t command_ring_vector;
	uint16_t response_ring_vector;
	uint8_t command_entry_header_words;
	uint8_t command_entry_footer_words;
	uint8_t response_entry_header_words;
	uint8_t response_entry_footer_words;
	uint16_t ring_control_flags;
	uint16_t status_page_vector;
	uint64_t scratch_area_address;
	uint32_t scratch_area_size;
};

/* A firmware command: its opcode, family and version. */
struct intel_ax211_command_id {
	uint8_t opcode;
	uint8_t group;
	uint8_t version;
};

/* One transfer buffer of a TFD: its device address and length. */
struct intel_ax211_tfd_buffer {
	uint64_t address;
	uint16_t length;
};

/*
 * One decoded receive event: its command and flags, the command slot it
 * answers, its receive queue, and where its payload lies.
 */
struct intel_ax211_event {
	struct intel_ax211_command_id command;
	uint8_t flags;
	uint8_t index;
	uint8_t queue;
	uint8_t rx_queue;
	size_t payload_offset;
	size_t payload_length;
};

/*
 * The host bookkeeping of a command ring: its capacity, the next slot to
 * reserve, the oldest outstanding slot, the slots in use, and its queue.
 */
struct intel_ax211_ring {
	uint16_t capacity;
	uint16_t head;
	uint16_t tail;
	uint16_t used;
	uint8_t queue;
};

/* One reserved ring slot: its queue and index. */
struct intel_ax211_ring_token {
	uint8_t queue;
	uint8_t index;
};

/* A fixed buffer holding one copy of a command's bytes. */
struct intel_ax211_staging {
	uint8_t bytes[INTEL_AX211_STAGING_CAPACITY];
	size_t length;
};

int drv_intel_ax211_identity_matches(
	const struct intel_ax211_identity *identity);
int drv_intel_ax211_mac_type_supported(uint16_t mac_type);
int drv_intel_ax211_firmware_parse(
	const uint8_t *bytes, size_t length,
	struct intel_ax211_firmware_manifest *manifest);
int drv_intel_ax211_sku_equal(const struct intel_ax211_board_signature *left,
			      const struct intel_ax211_board_signature *right);
int drv_intel_ax211_pnvm_select(const uint8_t *bytes, size_t length,
			       const struct intel_ax211_board_signature *sku,
			       uint16_t mac_type, uint16_t rf_id,
			       struct intel_ax211_pnvm_manifest *manifest);
int drv_intel_ax211_pnvm_inspect(const uint8_t *bytes, size_t length,
				 struct intel_ax211_pnvm_inventory *inventory);

int drv_intel_ax211_boot_context_encode(
	uint8_t output[104],
	const struct intel_ax211_boot_context *context);
int drv_intel_ax211_rx_transfer_descriptor_encode(uint8_t output[16],
						  uint16_t buffer_id,
						  uint64_t address);
int drv_intel_ax211_rx_completion_descriptor_decode(const uint8_t input[32],
						    uint16_t *buffer_id,
						    uint8_t *flags);
int drv_intel_ax211_tfd_encode(uint8_t output[256],
			       const struct intel_ax211_tfd_buffer *buffers,
			       size_t buffer_count);
int drv_intel_ax211_narrow_command_encode(
	uint8_t output[4], uint8_t opcode, uint8_t flags,
	const struct intel_ax211_ring_token *token);
int drv_intel_ax211_wide_command_encode(
	uint8_t output[8], const struct intel_ax211_command_id *command,
	uint16_t payload_length, const struct intel_ax211_ring_token *token);
int drv_intel_ax211_event_decode(const uint8_t *bytes, size_t length,
				 struct intel_ax211_event *event);

int drv_intel_ax211_ring_init(struct intel_ax211_ring *ring, uint8_t queue,
			      uint16_t capacity);
int drv_intel_ax211_ring_reserve(struct intel_ax211_ring *ring,
				 struct intel_ax211_ring_token *token);
int drv_intel_ax211_ring_complete(struct intel_ax211_ring *ring,
				  const struct intel_ax211_ring_token *token);
size_t drv_intel_ax211_ring_available(const struct intel_ax211_ring *ring);

void drv_intel_ax211_scrub(void *memory, size_t length);
int drv_intel_ax211_staging_set(struct intel_ax211_staging *staging,
				const void *data, size_t length);
void drv_intel_ax211_staging_clear(struct intel_ax211_staging *staging);

#endif
