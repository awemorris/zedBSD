/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The words of the video engine (VCN 2.0): the firmware file's header,
 * the commands of its rings, the messages its decoder reads, and the page
 * its firmware shares with the driver.
 *
 * The decode ring carries register writes: each command is a header word
 * naming one of the engine processor's own registers and the value to
 * write, and a write to the processor's command register makes it act
 * on the values written before (start or end a packet, write a fence,
 * raise an interrupt).  The encode rings carry plain commands: a word
 * naming the command and its arguments.  The callers build the words in
 * ordinary memory and copy them where the engine reads them.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-hw.h"
#include "drivers/gpu/amdrdna/amdrdna-regs.h"
#include "drivers/gpu/amdrdna/amdrdna-vcn.h"

/*
 * The largest firmware file the driver takes (1 MiB), and the page the
 * cache window's size is rounded to.
 *
 * Values referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/vcn_v2_0.c
 * (vcn_v2_0_mc_resume: the first cache window spans the file and four
 * bytes more, rounded up to the GPU's page) and amdgpu.h
 * (AMDGPU_GPU_PAGE_SIZE 4096).
 */
#define FORMAT_FILE_LIMIT		(1024U * 1024U)
#define FORMAT_PAGE_BYTES		4096U
#define FORMAT_CACHE_EXTRA_BYTES	4U

/*
 * The version word: bits 23:20 are the encoder's major version in the
 * newer way of naming, and zero in the older one.  The newer way puts the
 * revision in bits 11:0, the encoder's minor version in bits 19:12, the
 * decoder's version in bits 27:24 and the VEP's in bits 31:28; the older
 * way puts the family in bits 7:0, the minor version in bits 15:8 and the
 * major version in bits 31:24.
 *
 * Values referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/amdgpu_vcn.c
 * (amdgpu_vcn_sw_init: the reading of the ucode version).
 */
#define FORMAT_VERSION_ENCODE_MAJOR_SHIFT	20U
#define FORMAT_VERSION_ENCODE_MAJOR_MASK	0xfU
#define FORMAT_VERSION_REVISION_MASK		0xfffU
#define FORMAT_VERSION_ENCODE_MINOR_SHIFT	12U
#define FORMAT_VERSION_ENCODE_MINOR_MASK	0xffU
#define FORMAT_VERSION_DECODE_SHIFT		24U
#define FORMAT_VERSION_DECODE_MASK		0xfU
#define FORMAT_VERSION_VEP_SHIFT		28U
#define FORMAT_VERSION_VEP_MASK			0xfU
#define FORMAT_VERSION_FAMILY_MASK		0xffU
#define FORMAT_VERSION_MINOR_SHIFT		8U
#define FORMAT_VERSION_MINOR_MASK		0xffU
#define FORMAT_VERSION_MAJOR_SHIFT		24U
#define FORMAT_VERSION_MAJOR_MASK		0xffU

/*
 * The file's header is the common one of layout 1.0, which the firmware
 * reader checks; the video engine's file adds nothing to it.
 *
 * Values referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.h
 * (struct common_firmware_header) and amdgpu_vcn.c (amdgpu_vcn_sw_init:
 * only the common header is read).
 */
#define FORMAT_HEADER_MAJOR		1U

/*
 * The decode ring's ring-control word: the ring's size as the base-2
 * logarithm of its bytes (bits 4:0), the block size of 1 (bits 12:8), and
 * the engine told not to fetch, not to update its read pointer copy, and
 * to let the driver write its read pointer, until the ring is set up.
 *
 * Values referenced: Linux 7.1
 * drivers/gpu/drm/amd/include/asic_reg/vcn/vcn_2_0_0_sh_mask.h
 * (UVD_RBC_RB_CNTL__RB_BUFSZ, __RB_BLKSZ, __RB_NO_FETCH, __RB_NO_UPDATE,
 * __RB_RPTR_WR_EN) and drivers/gpu/drm/amd/amdgpu/vcn_v2_0.c
 * (vcn_v2_0_start: the values of the fields).
 */
#define FORMAT_RING_SIZE_MASK		0x0000001fU
#define FORMAT_RING_BLOCK_ONE		0x00000100U
#define FORMAT_RING_NO_FETCH		0x00010000U
#define FORMAT_RING_NO_UPDATE		0x01000000U
#define FORMAT_RING_READ_WRITABLE	0x10000000U

/*
 * The engine processor's own register numbers the decode ring's commands
 * write: the fence's context (its sequence), the two data words, the
 * command, the do-nothing register, and the ninth scratch register; and
 * the registers of an indirect buffer: its address context, its address
 * (low, high) and its length in words.
 *
 * Values referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/vcn_v2_0.c
 * (mmUVD_CONTEXT_ID_INTERNAL_OFFSET, mmUVD_GPCOM_VCPU_DATA0_INTERNAL_OFFSET,
 * mmUVD_GPCOM_VCPU_DATA1_INTERNAL_OFFSET,
 * mmUVD_GPCOM_VCPU_CMD_INTERNAL_OFFSET, mmUVD_NO_OP_INTERNAL_OFFSET,
 * mmUVD_SCRATCH9_INTERNAL_OFFSET, mmUVD_LMI_RBC_IB_VMID_INTERNAL_OFFSET,
 * mmUVD_LMI_RBC_IB_64BIT_BAR_LOW_INTERNAL_OFFSET,
 * mmUVD_LMI_RBC_IB_64BIT_BAR_HIGH_INTERNAL_OFFSET,
 * mmUVD_RBC_IB_SIZE_INTERNAL_OFFSET).
 */
#define FORMAT_REG_FENCE_SEQUENCE	0x01fdU
#define FORMAT_REG_DATA0		0x0504U
#define FORMAT_REG_DATA1		0x0505U
#define FORMAT_REG_COMMAND		0x0503U
#define FORMAT_REG_NOTHING		0x053fU
#define FORMAT_REG_SCRATCH9		0xc01dU
#define FORMAT_REG_IB_CONTEXT		0x01e1U
#define FORMAT_REG_IB_LOW		0x05a7U
#define FORMAT_REG_IB_HIGH		0x05a6U
#define FORMAT_REG_IB_LENGTH		0x01e2U

/*
 * A register write's header word (type 0, one value): the register number
 * in bits 15:0, the count of values minus one (0) in bits 29:16 and the
 * type (0) in bits 31:30.
 *
 * Values referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/soc15d.h
 * (PACKET0).
 */
#define FORMAT_WRITE_REGISTER_MASK	0x0000ffffU

/*
 * The decode commands the driver gives, written to the command register:
 * the driver's mark (bit 31) and the command shifted left by one: write
 * the fence, raise the interrupt, start and end a packet.  A command of 0
 * with the data words holding a message's address hands the message to
 * the firmware.
 *
 * Values referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/amdgpu_vcn.h
 * (VCN_DEC_KMD_CMD, VCN_DEC_CMD_FENCE, VCN_DEC_CMD_TRAP,
 * VCN_DEC_CMD_PACKET_START, VCN_DEC_CMD_PACKET_END) and amdgpu_vcn.c
 * (amdgpu_vcn_dec_send_msg: the command word 0); Mesa 25.0.7
 * src/amd/common/ac_vcn_dec.h (RDECODE_CMD_MSG_BUFFER).
 */
#define FORMAT_DECODE_DRIVER		0x80000000U
#define FORMAT_DECODE_FENCE		0x00000000U
#define FORMAT_DECODE_TRAP		0x00000001U
#define FORMAT_DECODE_PACKET_START	0x0000000aU
#define FORMAT_DECODE_PACKET_END	0x0000000bU
#define FORMAT_DECODE_MESSAGE		0x00000000U

/*
 * The fence's address in the data words: the low 32 bits, and bits 39:32.
 *
 * Values referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/vcn_v2_0.c
 * (vcn_v2_0_dec_ring_emit_fence: upper_32_bits(addr) & 0xff).
 */
#define FORMAT_FENCE_HIGH_MASK		0x000000ffU

/*
 * The encode commands: do nothing, end, run an indirect buffer, write a
 * fence, raise the interrupt.
 *
 * Values referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/amdgpu_vcn.h
 * (VCN_ENC_CMD_NO_OP, VCN_ENC_CMD_END, VCN_ENC_CMD_IB, VCN_ENC_CMD_FENCE,
 * VCN_ENC_CMD_TRAP).
 */
#define FORMAT_ENCODE_NOTHING		0x00000000U
#define FORMAT_ENCODE_END		0x00000001U
#define FORMAT_ENCODE_IB		0x00000002U
#define FORMAT_ENCODE_FENCE		0x00000003U
#define FORMAT_ENCODE_TRAP		0x00000004U

/*
 * A decode message's header: its own size, the message's whole size, the
 * count of index entries, the message's kind, the stream's handle, and
 * the feedback number; then the index entries of four words each (the
 * body's kind, where it starts, its size, and whether it is filled).  The
 * creation body follows the header with the stream's kind, the session's
 * flags, and the stream's width and height in samples.
 *
 * Values referenced: Mesa 25.0.7 src/amd/common/ac_vcn_dec.h (the decode
 * message header, its index entry and the creation body).
 * derived: the fields are 32-bit words in order from byte 0.
 */
#define FORMAT_MESSAGE_HEADER_BYTES_AT	0x00U
#define FORMAT_MESSAGE_TOTAL_BYTES_AT	0x04U
#define FORMAT_MESSAGE_BUFFERS_AT	0x08U
#define FORMAT_MESSAGE_KIND_AT		0x0cU
#define FORMAT_MESSAGE_HANDLE_AT	0x10U
#define FORMAT_MESSAGE_FEEDBACK_AT	0x14U
#define FORMAT_MESSAGE_INDEX_KIND_AT	0x18U
#define FORMAT_MESSAGE_INDEX_START_AT	0x1cU
#define FORMAT_MESSAGE_INDEX_BYTES_AT	0x20U
#define FORMAT_MESSAGE_INDEX_FILLED_AT	0x24U
#define FORMAT_MESSAGE_HEADER_BYTES	0x28U
#define FORMAT_CREATE_STREAM_AT		0x28U
#define FORMAT_CREATE_FLAGS_AT		0x2cU
#define FORMAT_CREATE_WIDTH_AT		0x30U
#define FORMAT_CREATE_HEIGHT_AT		0x34U
#define FORMAT_CREATE_BODY_BYTES	0x10U

/*
 * The message kinds (create a session, destroy it), and the index kind of
 * a creation body.
 *
 * Values referenced: Mesa 25.0.7 src/amd/common/ac_vcn_dec.h
 * (RDECODE_MSG_CREATE, RDECODE_MSG_DESTROY, RDECODE_MESSAGE_CREATE).
 */
#define FORMAT_MESSAGE_CREATE		0x00000000U
#define FORMAT_MESSAGE_DESTROY		0x00000002U
#define FORMAT_INDEX_CREATE		0x00000001U

/*
 * The shared page: the word of the parts the driver uses (bit 8, the
 * several queues), and the byte of each queue's mode, whose bit 0 says
 * the driver resets that queue's ring.
 *
 * Values referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/amdgpu_vcn.h
 * (AMDGPU_VCN_MULTI_QUEUE_FLAG, struct amdgpu_fw_shared,
 * struct amdgpu_fw_shared_multi_queue, FW_QUEUE_RING_RESET).
 * derived: the flags word at 0, 44 bytes of padding, the two ring
 * pointers at 48, one byte of padding at 56, so the queues' modes are the
 * bytes at 57 (decode), 58 (general encode) and 59 (low-latency encode).
 */
#define FORMAT_SHARED_FLAGS_AT		0x00U
#define FORMAT_SHARED_MULTI_QUEUE	0x00000100U
#define FORMAT_SHARED_QUEUES_AT		57U
#define FORMAT_SHARED_QUEUE_RESET	0x01U

/*
 * The host bridge's doorbell range of the engine: the first doorbell's
 * dword index in bits 11:2, the count in bits 20:16.
 *
 * Values referenced: Linux 7.1
 * drivers/gpu/drm/amd/include/asic_reg/nbio/nbio_2_3_sh_mask.h
 * (BIF_MMSCH0_DOORBELL_RANGE__OFFSET_MASK, __SIZE_MASK, __SIZE__SHIFT).
 */
#define FORMAT_DOORBELLS_OFFSET_MASK	0x00000ffcU
#define FORMAT_DOORBELLS_OFFSET_SHIFT	2U
#define FORMAT_DOORBELLS_COUNT_MASK	0x001f0000U
#define FORMAT_DOORBELLS_COUNT_SHIFT	16U

/*
 * The commands that name a buffer to the firmware, written to the
 * command register after the buffer's address in the data words: the
 * kind of the buffer shifted left by one (without the driver's mark).
 * The engine control register starts the decode once every buffer is
 * named.
 *
 * Values referenced: Mesa 25.0.7 src/amd/common/ac_vcn_dec.h
 * (RDECODE_CMD_MSG_BUFFER, RDECODE_CMD_DPB_BUFFER,
 * RDECODE_CMD_DECODING_TARGET_BUFFER, RDECODE_CMD_FEEDBACK_BUFFER,
 * RDECODE_CMD_SESSION_CONTEXT_BUFFER, RDECODE_CMD_BITSTREAM_BUFFER,
 * RDECODE_CMD_IT_SCALING_TABLE_BUFFER, RDECODE_CMD_CONTEXT_BUFFER,
 * RDECODE_VCN2_ENGINE_CNTL) and src/amd/vulkan/radv_video.c (send_cmd,
 * radv_vcn_decode_video: the order of the buffers and the engine's start
 * of 1).
 */
#define FORMAT_BUFFER_MESSAGE		0x000U
#define FORMAT_BUFFER_DPB		0x001U
#define FORMAT_BUFFER_TARGET		0x002U
#define FORMAT_BUFFER_FEEDBACK		0x003U
#define FORMAT_BUFFER_SESSION_CONTEXT	0x005U
#define FORMAT_BUFFER_BITSTREAM		0x100U
#define FORMAT_BUFFER_SCALING		0x204U
#define FORMAT_BUFFER_CONTEXT		0x206U
#define FORMAT_REG_ENGINE_CONTROL	0x0506U
#define FORMAT_ENGINE_START		1U

/*
 * A decode message: the header's two index entries (the decode body's in
 * the header, the codec body's after it), the decode body at byte 56 and
 * the codec body after it; the message kind, and the index kinds of the
 * decode body and of an H.264 body; the H.264 stream kind of the fast
 * path the drivers use.
 *
 * Values referenced: Mesa 25.0.7 src/amd/common/ac_vcn_dec.h (the message
 * header and its index entries, RDECODE_MSG_DECODE,
 * RDECODE_MESSAGE_DECODE, RDECODE_MESSAGE_AVC) and
 * src/amd/vulkan/radv_video.c (rvcn_dec_message_decode: the order of the
 * index entries and the bodies).
 * derived: the header is 40 bytes with the decode body's entry, the codec
 * body's entry 16 more, the decode body 188 bytes.
 */
#define FORMAT_MESSAGE_DECODE		0x00000001U
#define FORMAT_INDEX_DECODE		0x00000002U
#define FORMAT_INDEX_H264		0x00000006U
#define FORMAT_CODEC_INDEX_KIND_AT	0x28U
#define FORMAT_CODEC_INDEX_START_AT	0x2cU
#define FORMAT_CODEC_INDEX_BYTES_AT	0x30U
#define FORMAT_CODEC_INDEX_FILLED_AT	0x34U
#define FORMAT_DECODE_AT		56U
#define FORMAT_DECODE_BYTES		188U
#define FORMAT_CODEC_AT			(FORMAT_DECODE_AT + FORMAT_DECODE_BYTES)

/*
 * The decode body's fields, in bytes from its start: the stream's kind,
 * the decode's flags, the picture's width and height in samples; the
 * bytes of the bitstream, the DPB, the target, the decoder's context and
 * the session context; the DPB's pitch, aligned height, swizzle and array
 * mode; the target's luma and chroma pitches, swizzle, array and field
 * modes, and where its planes start (the top and bottom fields' alike).
 *
 * Values referenced: Mesa 25.0.7 src/amd/common/ac_vcn_dec.h (the decode
 * body).
 * derived: the fields are 32-bit words; the offsets are those of the
 * body's words in its declared order.
 */
#define FORMAT_DECODE_STREAM_AT		0U
#define FORMAT_DECODE_FLAGS_AT		4U
#define FORMAT_DECODE_WIDTH_AT		8U
#define FORMAT_DECODE_HEIGHT_AT		12U
#define FORMAT_DECODE_BITSTREAM_BYTES_AT	16U
#define FORMAT_DECODE_DPB_BYTES_AT	20U
#define FORMAT_DECODE_TARGET_BYTES_AT	24U
#define FORMAT_DECODE_CONTEXT_BYTES_AT	36U
#define FORMAT_DECODE_SESSION_BYTES_AT	40U
#define FORMAT_DECODE_DPB_PITCH_AT	72U
#define FORMAT_DECODE_DPB_HEIGHT_AT	76U
#define FORMAT_DECODE_DPB_SWIZZLE_AT	84U
#define FORMAT_DECODE_DPB_ARRAY_AT	88U
#define FORMAT_DECODE_PITCH_AT		100U
#define FORMAT_DECODE_CHROMA_PITCH_AT	104U
#define FORMAT_DECODE_SWIZZLE_AT	112U
#define FORMAT_DECODE_ARRAY_AT		116U
#define FORMAT_DECODE_FIELD_AT		120U
#define FORMAT_DECODE_LUMA_TOP_AT	136U
#define FORMAT_DECODE_LUMA_BOTTOM_AT	140U
#define FORMAT_DECODE_CHROMA_TOP_AT	144U
#define FORMAT_DECODE_CHROMA_BOTTOM_AT	148U

/*
 * The hidden DPB's layout: its rows on a boundary of 32 samples, its
 * height likewise; linear surfaces (swizzle and array mode 0) for the
 * DPB and the target.
 *
 * Values referenced: Mesa 25.0.7 src/amd/vulkan/radv_video.c
 * (radv_video_get_db_alignment: 32 for H.264, and the DPB's pitch and
 * height from its surface) and src/amd/common/ac_vcn_dec.h
 * (RDECODE_ARRAY_MODE_LINEAR, RDECODE_SW_MODE_LINEAR).
 */
#define FORMAT_DPB_ALIGN		32U
#define FORMAT_LINEAR			0U

/*
 * The feedback buffer's header: its own size and the whole size (the
 * header alone), the count of buffers after it (none), and the status
 * word the firmware writes.
 *
 * Values referenced: Mesa 25.0.7 src/amd/common/ac_vcn_dec.h (the
 * feedback header) and src/amd/vulkan/radv_video.c
 * (rvcn_dec_message_feedback).
 */
#define FORMAT_FEEDBACK_HEADER_BYTES_AT	0x00U
#define FORMAT_FEEDBACK_TOTAL_BYTES_AT	0x04U
#define FORMAT_FEEDBACK_BUFFERS_AT	0x08U
#define FORMAT_FEEDBACK_STATUS_AT	0x10U

struct format_dpg;

static unsigned format_register(uint32_t *words, uint32_t which, uint32_t value);
static void format_put32(uint8_t *bytes, size_t at, uint32_t value);
static unsigned format_buffer(uint32_t *words, uint32_t kind, uint64_t address);
static void format_put16(uint8_t *bytes, size_t at, uint32_t value);
static uint32_t format_get32(const uint8_t *bytes, size_t at);
static int format_message_decode(uint8_t *message, size_t bytes, const struct amdrdna_vcn_frame *frame, uint32_t stream, uint32_t index_kind, uint32_t codec_bytes);
static void format_dpg_put(struct format_dpg *table, uint32_t offset, uint32_t value);

/*
 * Checks a firmware file of the video engine and records what the driver
 * needs of it: the file's size, where the program lies, and its version.
 *
 * Returns 0, or EINVAL for a file larger than the driver takes, a header
 * that is not the common one, or a program outside the file.
 */
int
amdrdna_vcn_firmware_check(
	const uint8_t *bytes,
	size_t length,
	struct amdrdna_vcn_firmware *firmware)
{
	struct amdrdna_firmware_layout layout;
	int error;

	/* Refuses a file larger than the cache window the driver plans for. */
	if (length > FORMAT_FILE_LIMIT)
		return EINVAL;

	/* Reads the common header, which checks the sizes against the file. */
	error = amdrdna_firmware_layout(AMDRDNA_FIRMWARE_WHOLE, bytes, length, &layout);
	if (error != 0)
		return error;

	/* Refuses a layout the engine's file does not have. */
	if (layout.header_major != FORMAT_HEADER_MAJOR)
		return EINVAL;

	/* Refuses a file without a program. */
	if (layout.piece_count != 1U || layout.pieces[0].bytes == 0U)
		return EINVAL;

	/* Records the file and its program. */
	firmware->file_bytes = (uint32_t)length;
	firmware->program_at = layout.pieces[0].offset;
	firmware->program_bytes = layout.pieces[0].bytes;
	firmware->version_word = layout.version;

	/* Reads the version word. */
	amdrdna_vcn_version_read(layout.version, &firmware->version);

	/* Succeeded: the program lies inside the file. */
	return 0;
}

/*
 * Reads the version word of the engine's firmware in whichever of the two
 * ways it is written.
 */
void
amdrdna_vcn_version_read(
	uint32_t word,
	struct amdrdna_vcn_version *version)
{
	unsigned encode_major;

	/* Nothing is known yet. */
	version->named = false;
	version->encode_major = 0U;
	version->encode_minor = 0U;
	version->decode = 0U;
	version->vep = 0U;
	version->revision = 0U;
	version->major = 0U;
	version->minor = 0U;
	version->family = 0U;

	/* A nonzero encoder major version says the word names each part. */
	encode_major = (word >> FORMAT_VERSION_ENCODE_MAJOR_SHIFT) & FORMAT_VERSION_ENCODE_MAJOR_MASK;
	if (encode_major != 0U) {
		version->named = true;
		version->encode_major = encode_major;
		version->encode_minor = (word >> FORMAT_VERSION_ENCODE_MINOR_SHIFT) & FORMAT_VERSION_ENCODE_MINOR_MASK;
		version->decode = (word >> FORMAT_VERSION_DECODE_SHIFT) & FORMAT_VERSION_DECODE_MASK;
		version->vep = (word >> FORMAT_VERSION_VEP_SHIFT) & FORMAT_VERSION_VEP_MASK;
		version->revision = word & FORMAT_VERSION_REVISION_MASK;
		return;
	}

	/* The older word: a major and a minor version and a family. */
	version->family = word & FORMAT_VERSION_FAMILY_MASK;
	version->minor = (word >> FORMAT_VERSION_MINOR_SHIFT) & FORMAT_VERSION_MINOR_MASK;
	version->major = (word >> FORMAT_VERSION_MAJOR_SHIFT) & FORMAT_VERSION_MAJOR_MASK;
}

/*
 * Reports the size of the engine's first cache window, which holds the
 * program the security processor placed: the file's size and four bytes
 * more, rounded up to a page.
 */
uint32_t
amdrdna_vcn_cache_bytes(
	uint32_t file_bytes)
{
	uint32_t bytes;

	/* The file and the extra word, rounded up to the page. */
	bytes = file_bytes + FORMAT_CACHE_EXTRA_BYTES;
	bytes = (bytes + FORMAT_PAGE_BYTES - 1U) & ~(FORMAT_PAGE_BYTES - 1U);

	/* Succeeded: the window's size in bytes. */
	return bytes;
}

/*
 * The engine's own register numbering, which the dynamic power gating's
 * table uses: a register's byte address in the chip's map (its dword
 * index from BAR 5, times four, under 20 bits) inside the video range
 * (0x1fa00, 0x2600 bytes) is counted from the video registers' own start
 * (0), one in the always-on range (0x1f800, 0x600 bytes) from the
 * always-on registers' (0x30000); any other address is kept.
 *
 * Values referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/vcn_v2_0.c
 * (VCN_VID_SOC_ADDRESS_2_0) and amdgpu_vcn.h (VCN_AON_SOC_ADDRESS_2_0,
 * VCN_VID_IP_ADDRESS_2_0, VCN_AON_IP_ADDRESS_2_0, SOC15_DPG_MODE_OFFSET:
 * the video range tried first).
 */
#define FORMAT_DPG_ADDRESS_MASK		0xfffffU
#define FORMAT_DPG_VIDEO_AT		0x1fa00U
#define FORMAT_DPG_VIDEO_BYTES		0x2600U
#define FORMAT_DPG_VIDEO_OWN		0x0U
#define FORMAT_DPG_ALWAYS_AT		0x1f800U
#define FORMAT_DPG_ALWAYS_BYTES		0x600U
#define FORMAT_DPG_ALWAYS_OWN		0x30000U

/* The table while it is written: its words, its capacity, the window of the registers, and whether it overflowed. */
struct format_dpg {
	uint32_t *words;
	unsigned capacity;
	unsigned count;
	uint32_t window;
	bool overflow;
};

/*
 * Reports a register's number in the engine's own numbering, from its
 * dword index from BAR 5.
 */
uint32_t
amdrdna_vcn_dpg_offset(
	uint32_t index)
{
	uint32_t address;

	/* The byte address in the chip's map. */
	address = (index << 2) & FORMAT_DPG_ADDRESS_MASK;

	/* The video range, then the always-on range, then any other. */
	if (address >= FORMAT_DPG_VIDEO_AT && address < FORMAT_DPG_VIDEO_AT + FORMAT_DPG_VIDEO_BYTES)
		address = address - FORMAT_DPG_VIDEO_AT + FORMAT_DPG_VIDEO_OWN;
	else if (address >= FORMAT_DPG_ALWAYS_AT && address < FORMAT_DPG_ALWAYS_AT + FORMAT_DPG_ALWAYS_BYTES)
		address = address - FORMAT_DPG_ALWAYS_AT + FORMAT_DPG_ALWAYS_OWN;

	/* Succeeded: the register's number in dwords. */
	return address >> 2;
}

/*
 * Writes the table of register writes the security processor applies at
 * the start under the dynamic power gating: the clock gates (gating
 * control on, the gates open, the fine gates' gate on and their control
 * in software mode), the processor's control, its interrupt off, the
 * memory interface's settings, the picture cache's mode and
 * multiplexers, the windows (the first two left at 0: the security
 * processor fills them with the placed program and the processor's
 * stack; the third at the context, the uncached one at the shared page),
 * the graphics block's address layout, the register mask and the
 * indirect buffers' register check, the resets released, the memory
 * interface's second control, and the processor's interrupt on.
 *
 * Returns the count of words written (two a register), or 0 when they do
 * not fit in capacity.
 *
 * Procedure referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/vcn_v2_0.c
 * (vcn_v2_0_start_dpg_mode, vcn_v2_0_clock_gating_dpg_mode and
 * vcn_v2_0_mc_resume_dpg_mode with the table, indirect, of a program the
 * security processor loaded).
 */
unsigned
amdrdna_vcn_dpg_table(
	uint32_t *words,
	unsigned capacity,
	const struct amdrdna_vcn_dpg_setup *setup)
{
	struct format_dpg table;

	/* An empty table. */
	table.words = words;
	table.capacity = capacity;
	table.count = 0U;
	table.window = setup->window;
	table.overflow = false;

	/* The clock gates. */
	format_dpg_put(&table, AMDRDNA_VCN_REG_GATE_CONTROL, AMDRDNA_VCN_DPG_GATE_CONTROL);
	format_dpg_put(&table, AMDRDNA_VCN_REG_GATE, 0U);
	format_dpg_put(&table, AMDRDNA_VCN_REG_FINE_GATE, AMDRDNA_VCN_DPG_FINE_GATE);
	format_dpg_put(&table, AMDRDNA_VCN_REG_FINE_GATE_CONTROL, 0U);

	/* The processor's control, its interrupt off, the memory interface. */
	format_dpg_put(&table, AMDRDNA_VCN_REG_PROCESSOR_CONTROL, AMDRDNA_VCN_DPG_PROCESSOR);
	format_dpg_put(&table, AMDRDNA_VCN_REG_INTERRUPT_ENABLE, 0U);
	format_dpg_put(&table, AMDRDNA_VCN_REG_MEMORY_CONTROL, AMDRDNA_VCN_DPG_MEMORY);

	/* The picture cache. */
	format_dpg_put(&table, AMDRDNA_VCN_REG_PICTURE_CONTROL, AMDRDNA_VCN_PICTURE_REPLACE);
	format_dpg_put(&table, AMDRDNA_VCN_REG_PICTURE_MUX_A, AMDRDNA_VCN_PICTURE_MUX_A);
	format_dpg_put(&table, AMDRDNA_VCN_REG_PICTURE_MUX_B, AMDRDNA_VCN_PICTURE_MUX_B);
	format_dpg_put(&table, AMDRDNA_VCN_REG_PICTURE_MUX, AMDRDNA_VCN_PICTURE_MUX);

	/* Window 0, the placed program, and window 1, the stack: the security processor's. */
	format_dpg_put(&table, AMDRDNA_VCN_REG_CACHE0_LOW, 0U);
	format_dpg_put(&table, AMDRDNA_VCN_REG_CACHE0_HIGH, 0U);
	format_dpg_put(&table, AMDRDNA_VCN_REG_CACHE0_OFFSET, 0U);
	format_dpg_put(&table, AMDRDNA_VCN_REG_CACHE0_SIZE, 0U);
	format_dpg_put(&table, AMDRDNA_VCN_REG_CACHE1_LOW, 0U);
	format_dpg_put(&table, AMDRDNA_VCN_REG_CACHE1_HIGH, 0U);
	format_dpg_put(&table, AMDRDNA_VCN_REG_CACHE1_OFFSET, 0U);
	format_dpg_put(&table, AMDRDNA_VCN_REG_CACHE1_SIZE, AMDRDNA_VCN_STACK_BYTES);

	/* Window 2, the context. */
	format_dpg_put(&table, AMDRDNA_VCN_REG_CACHE2_LOW, (uint32_t)setup->context);
	format_dpg_put(&table, AMDRDNA_VCN_REG_CACHE2_HIGH, (uint32_t)(setup->context >> 32));
	format_dpg_put(&table, AMDRDNA_VCN_REG_CACHE2_OFFSET, 0U);
	format_dpg_put(&table, AMDRDNA_VCN_REG_CACHE2_SIZE, AMDRDNA_VCN_CONTEXT_BYTES);

	/* The uncached window, the shared page. */
	format_dpg_put(&table, AMDRDNA_VCN_REG_UNCACHED_LOW, (uint32_t)setup->shared);
	format_dpg_put(&table, AMDRDNA_VCN_REG_UNCACHED_HIGH, (uint32_t)(setup->shared >> 32));
	format_dpg_put(&table, AMDRDNA_VCN_REG_UNCACHED_OFFSET, 0U);
	format_dpg_put(&table, AMDRDNA_VCN_REG_UNCACHED_SIZE, AMDRDNA_VCN_PAGE_BYTES);

	/* The address layout, the register mask and the indirect buffers' check. */
	format_dpg_put(&table, AMDRDNA_VCN_REG_ADDRESS_CONFIG, setup->address_config);
	format_dpg_put(&table, AMDRDNA_VCN_REG_REGISTER_MASK, AMDRDNA_VCN_DPG_REGISTER_MASK);
	format_dpg_put(&table, AMDRDNA_VCN_REG_IB_CHECK, AMDRDNA_VCN_DPG_IB_CHECK);

	/* The resets released, the memory interface's second control, the interrupt on. */
	format_dpg_put(&table, AMDRDNA_VCN_REG_SOFT_RESET, 0U);
	format_dpg_put(&table, AMDRDNA_VCN_REG_MEMORY_CONTROL2, AMDRDNA_VCN_DPG_MEMORY2);
	format_dpg_put(&table, AMDRDNA_VCN_REG_INTERRUPT_ENABLE, AMDRDNA_VCN_INTERRUPT_PROCESSOR);

	/* A table that did not fit is none. */
	if (table.overflow)
		return 0U;

	/* Succeeded: the words written. */
	return table.count;
}

/* Appends one register write to the table, in the engine's own numbering. */
static void
format_dpg_put(
	struct format_dpg *table,
	uint32_t offset,
	uint32_t value)
{
	/* A table already full takes nothing more. */
	if (table->count + 2U > table->capacity) {
		table->overflow = true;
		return;
	}

	/* The register's number, then its value. */
	table->words[table->count] = amdrdna_vcn_dpg_offset(table->window + offset);
	table->words[table->count + 1U] = value;
	table->count += 2U;
}

/*
 * Builds the decode ring's control word for a ring of ring_bytes (a power
 * of two), with the engine held off the ring until it is set up.
 */
uint32_t
amdrdna_vcn_decode_ring_control(
	uint32_t ring_bytes)
{
	uint32_t size;
	uint32_t word;

	/* The base-2 logarithm of the ring's bytes. */
	size = 0U;
	while ((1U << size) < ring_bytes && size < FORMAT_RING_SIZE_MASK)
		size++;

	/* The size, the block, and the engine kept off the ring. */
	word = size & FORMAT_RING_SIZE_MASK;
	word |= FORMAT_RING_BLOCK_ONE;
	word |= FORMAT_RING_NO_FETCH;
	word |= FORMAT_RING_NO_UPDATE;
	word |= FORMAT_RING_READ_WRITABLE;

	/* Succeeded: the control word. */
	return word;
}

/*
 * Reports how many words the driver may still write on a ring of words
 * words (a power of two) whose engine has read up to read and whose
 * driver has written up to write, both in words: one word always stays
 * free, so a full ring is not read as an empty one.
 */
uint32_t
amdrdna_vcn_ring_room(
	uint32_t read,
	uint32_t write,
	uint32_t words)
{
	uint32_t used;

	/* The words written and not yet read. */
	used = (write - read) & (words - 1U);

	/* Succeeded: the free words, one kept back. */
	return words - 1U - used;
}

/* Writes the start of a decode packet (4 words); returns the words written. */
unsigned
amdrdna_vcn_decode_start(
	uint32_t *words)
{
	unsigned count;

	/* The first data word cleared, then the start command. */
	count = format_register(words, FORMAT_REG_DATA0, 0U);
	count += format_register(words + count, FORMAT_REG_COMMAND, FORMAT_DECODE_DRIVER | (FORMAT_DECODE_PACKET_START << 1));

	/* Succeeded: the words written. */
	return count;
}

/* Writes the end of a decode packet (2 words); returns the words written. */
unsigned
amdrdna_vcn_decode_end(
	uint32_t *words)
{
	unsigned count;

	/* The end command. */
	count = format_register(words, FORMAT_REG_COMMAND, FORMAT_DECODE_DRIVER | (FORMAT_DECODE_PACKET_END << 1));

	/* Succeeded: the words written. */
	return count;
}

/*
 * Writes the run of an indirect buffer on the decode ring (8 words): the
 * address context it runs in, its address, and its length in words.
 */
unsigned
amdrdna_vcn_decode_ib(
	uint32_t *words,
	unsigned context,
	uint64_t address,
	uint32_t length)
{
	unsigned count;

	/* The context, the address's two halves, and the length. */
	count = format_register(words, FORMAT_REG_IB_CONTEXT, (uint32_t)context);
	count += format_register(words + count, FORMAT_REG_IB_LOW, (uint32_t)address);
	count += format_register(words + count, FORMAT_REG_IB_HIGH, (uint32_t)(address >> 32));
	count += format_register(words + count, FORMAT_REG_IB_LENGTH, length);

	/* Succeeded: the words written. */
	return count;
}

/*
 * Writes a fence on the decode ring and the interrupt after it (14
 * words): the sequence and the 40-bit address the engine writes it to,
 * the fence command, then the data words cleared and the interrupt
 * command.
 */
unsigned
amdrdna_vcn_decode_fence(
	uint32_t *words,
	uint64_t address,
	uint32_t sequence)
{
	unsigned count;

	/* The sequence and where it goes, then the fence command. */
	count = format_register(words, FORMAT_REG_FENCE_SEQUENCE, sequence);
	count += format_register(words + count, FORMAT_REG_DATA0, (uint32_t)address);
	count += format_register(words + count, FORMAT_REG_DATA1, (uint32_t)(address >> 32) & FORMAT_FENCE_HIGH_MASK);
	count += format_register(words + count, FORMAT_REG_COMMAND, FORMAT_DECODE_DRIVER | (FORMAT_DECODE_FENCE << 1));

	/* The interrupt that tells the driver the fence is written. */
	count += format_register(words + count, FORMAT_REG_DATA0, 0U);
	count += format_register(words + count, FORMAT_REG_DATA1, 0U);
	count += format_register(words + count, FORMAT_REG_COMMAND, FORMAT_DECODE_DRIVER | (FORMAT_DECODE_TRAP << 1));

	/* Succeeded: the words written. */
	return count;
}

/*
 * Writes the decode ring's test (4 words): the start of a packet, then a
 * value into the ninth scratch register, which the CPU reads back.
 */
unsigned
amdrdna_vcn_decode_scratch(
	uint32_t *words,
	uint32_t value)
{
	unsigned count;

	/* The start command, then the scratch write. */
	count = format_register(words, FORMAT_REG_COMMAND, FORMAT_DECODE_DRIVER | (FORMAT_DECODE_PACKET_START << 1));
	count += format_register(words + count, FORMAT_REG_SCRATCH9, value);

	/* Succeeded: the words written. */
	return count;
}

/*
 * Pads a decode submission of count words (an even count) with writes to
 * the do-nothing register up to the ring's step of 16 words; returns the
 * padded count.
 */
unsigned
amdrdna_vcn_decode_pad(
	uint32_t *words,
	unsigned count)
{
	/* One do-nothing write (two words) at a time, until the step is reached. */
	while ((count % AMDRDNA_VCN_DECODE_STEP_WORDS) != 0U)
		count += format_register(words + count, FORMAT_REG_NOTHING, 0U);

	/* Succeeded: the padded count. */
	return count;
}

/*
 * Writes the indirect buffer that hands a message to the firmware (16
 * words): the message's address in the data words, the message command,
 * and do-nothing writes to fill the buffer.
 */
unsigned
amdrdna_vcn_message_ib(
	uint32_t *words,
	uint64_t message)
{
	unsigned count;

	/* The message's address and the command that hands it over. */
	count = format_register(words, FORMAT_REG_DATA0, (uint32_t)message);
	count += format_register(words + count, FORMAT_REG_DATA1, (uint32_t)(message >> 32));
	count += format_register(words + count, FORMAT_REG_COMMAND, FORMAT_DECODE_MESSAGE);

	/* The rest of the buffer does nothing. */
	count = amdrdna_vcn_decode_pad(words, count);

	/* Succeeded: the words written. */
	return count;
}

/*
 * Writes the run of an indirect buffer on an encode ring (5 words): the
 * command, the address context, the address's halves, and the length in
 * words.
 */
unsigned
amdrdna_vcn_encode_ib(
	uint32_t *words,
	unsigned context,
	uint64_t address,
	uint32_t length)
{
	/* The command and its four arguments. */
	words[0] = FORMAT_ENCODE_IB;
	words[1] = (uint32_t)context;
	words[2] = (uint32_t)address;
	words[3] = (uint32_t)(address >> 32);
	words[4] = length;

	/* Succeeded: the words written. */
	return AMDRDNA_VCN_ENCODE_IB_WORDS;
}

/*
 * Writes a fence on an encode ring and the interrupt after it (5 words):
 * the fence command with the address's halves and the 32-bit sequence,
 * then the interrupt command.
 */
unsigned
amdrdna_vcn_encode_fence(
	uint32_t *words,
	uint64_t address,
	uint32_t sequence)
{
	/* The fence, then the interrupt. */
	words[0] = FORMAT_ENCODE_FENCE;
	words[1] = (uint32_t)address;
	words[2] = (uint32_t)(address >> 32);
	words[3] = sequence;
	words[4] = FORMAT_ENCODE_TRAP;

	/* Succeeded: the words written. */
	return AMDRDNA_VCN_ENCODE_FENCE_WORDS;
}

/* Writes the end command of an encode ring (1 word); the ring's test sends it alone. */
unsigned
amdrdna_vcn_encode_end(
	uint32_t *words)
{
	/* The end command. */
	words[0] = FORMAT_ENCODE_END;

	/* Succeeded: the words written. */
	return AMDRDNA_VCN_ENCODE_END_WORDS;
}

/*
 * Pads an encode submission of count words with do-nothing commands up to
 * the ring's step of 64 words; returns the padded count.
 */
unsigned
amdrdna_vcn_encode_pad(
	uint32_t *words,
	unsigned count)
{
	/* One do-nothing word at a time, until the step is reached. */
	while ((count % AMDRDNA_VCN_ENCODE_STEP_WORDS) != 0U) {
		words[count] = FORMAT_ENCODE_NOTHING;
		count++;
	}

	/* Succeeded: the padded count. */
	return count;
}

/*
 * Writes the message that creates a decode session: the header with one
 * index entry for the creation body, then the stream's kind and its size
 * in samples.  The rest of the bytes are zeroed.
 *
 * Returns 0, or EINVAL when bytes cannot hold the message.
 */
int
amdrdna_vcn_message_create(
	uint8_t *message,
	size_t bytes,
	uint32_t handle,
	uint32_t stream,
	uint32_t width,
	uint32_t height)
{
	size_t at;

	/* Refuses a buffer smaller than the message. */
	if (bytes < AMDRDNA_VCN_CREATE_BYTES)
		return EINVAL;

	/* Zeroes the whole buffer, so no stale byte follows the message. */
	for (at = 0U; at < bytes; at++)
		message[at] = 0U;

	/* The header: its size, the whole message's, one body, a creation, the handle. */
	format_put32(message, FORMAT_MESSAGE_HEADER_BYTES_AT, FORMAT_MESSAGE_HEADER_BYTES);
	format_put32(message, FORMAT_MESSAGE_TOTAL_BYTES_AT, AMDRDNA_VCN_CREATE_BYTES);
	format_put32(message, FORMAT_MESSAGE_BUFFERS_AT, 1U);
	format_put32(message, FORMAT_MESSAGE_KIND_AT, FORMAT_MESSAGE_CREATE);
	format_put32(message, FORMAT_MESSAGE_HANDLE_AT, handle);
	format_put32(message, FORMAT_MESSAGE_FEEDBACK_AT, 0U);

	/* The index entry of the creation body, right after the header. */
	format_put32(message, FORMAT_MESSAGE_INDEX_KIND_AT, FORMAT_INDEX_CREATE);
	format_put32(message, FORMAT_MESSAGE_INDEX_START_AT, FORMAT_MESSAGE_HEADER_BYTES);
	format_put32(message, FORMAT_MESSAGE_INDEX_BYTES_AT, FORMAT_CREATE_BODY_BYTES);
	format_put32(message, FORMAT_MESSAGE_INDEX_FILLED_AT, 0U);

	/* The creation body: the stream's kind, no flags, and its size. */
	format_put32(message, FORMAT_CREATE_STREAM_AT, stream);
	format_put32(message, FORMAT_CREATE_FLAGS_AT, 0U);
	format_put32(message, FORMAT_CREATE_WIDTH_AT, width);
	format_put32(message, FORMAT_CREATE_HEIGHT_AT, height);

	/* Succeeded: the message is written. */
	return 0;
}

/*
 * Writes the message that destroys a decode session: the header's own
 * size, a whole size without the index entry, no body, and the handle.
 * The rest of the bytes are zeroed.
 *
 * Returns 0, or EINVAL when bytes cannot hold the message.
 */
int
amdrdna_vcn_message_destroy(
	uint8_t *message,
	size_t bytes,
	uint32_t handle)
{
	size_t at;

	/* Refuses a buffer smaller than the header. */
	if (bytes < FORMAT_MESSAGE_HEADER_BYTES)
		return EINVAL;

	/* Zeroes the whole buffer, so no stale byte follows the message. */
	for (at = 0U; at < bytes; at++)
		message[at] = 0U;

	/* The header alone. */
	format_put32(message, FORMAT_MESSAGE_HEADER_BYTES_AT, FORMAT_MESSAGE_HEADER_BYTES);
	format_put32(message, FORMAT_MESSAGE_TOTAL_BYTES_AT, AMDRDNA_VCN_DESTROY_BYTES);
	format_put32(message, FORMAT_MESSAGE_BUFFERS_AT, 0U);
	format_put32(message, FORMAT_MESSAGE_KIND_AT, FORMAT_MESSAGE_DESTROY);
	format_put32(message, FORMAT_MESSAGE_HANDLE_AT, handle);
	format_put32(message, FORMAT_MESSAGE_FEEDBACK_AT, 0U);

	/* Succeeded: the message is written. */
	return 0;
}

/*
 * Fills the page the firmware shares with the driver: zero, with the
 * flag that says the driver resets the queues one by one.
 */
void
amdrdna_vcn_shared_init(
	uint8_t *shared)
{
	size_t at;

	/* The part the firmware reads, zeroed. */
	for (at = 0U; at < AMDRDNA_VCN_SHARED_BYTES; at++)
		shared[at] = 0U;

	/* The several-queues flag. */
	format_put32(shared, FORMAT_SHARED_FLAGS_AT, FORMAT_SHARED_MULTI_QUEUE);
}

/*
 * Raises or clears the "ring reset" mark of one queue in the shared page;
 * the driver raises it while it sets that queue's ring up.
 */
void
amdrdna_vcn_shared_queue_reset(
	uint8_t *shared,
	unsigned queue,
	bool resetting)
{
	uint8_t mode;

	/* The queue's mode byte. */
	mode = shared[FORMAT_SHARED_QUEUES_AT + queue];

	/* The mark on or off, the other bits as they are. */
	if (resetting) {
		mode |= FORMAT_SHARED_QUEUE_RESET;
	} else {
		mode &= (uint8_t)~FORMAT_SHARED_QUEUE_RESET;
	}

	/* Writes the byte back. */
	shared[FORMAT_SHARED_QUEUES_AT + queue] = mode;
}

/*
 * Builds the host bridge's doorbell range word of the engine from its old
 * value: count doorbells from the dword index first, or none when count
 * is 0 (the first doorbell is then left as it was).
 */
uint32_t
amdrdna_vcn_doorbell_range_word(
	uint32_t old,
	uint32_t first,
	uint32_t count)
{
	uint32_t word;

	/* The count, which alone turns the range off. */
	word = old & ~FORMAT_DOORBELLS_COUNT_MASK;
	word |= (count << FORMAT_DOORBELLS_COUNT_SHIFT) & FORMAT_DOORBELLS_COUNT_MASK;

	/* The first doorbell, only for a range that is on. */
	if (count != 0U) {
		word &= ~FORMAT_DOORBELLS_OFFSET_MASK;
		word |= (first << FORMAT_DOORBELLS_OFFSET_SHIFT) & FORMAT_DOORBELLS_OFFSET_MASK;
	}

	/* Succeeded: the new word. */
	return word;
}

/*
 * Writes the indirect buffer that creates (or destroys) a decode session
 * from a message: the session context and the message named to the
 * firmware, padded to the ring's step; returns the words written.
 */
unsigned
amdrdna_vcn_create_run(
	uint32_t *words,
	uint64_t session_context,
	uint64_t message)
{
	unsigned count;

	/* The session context, then the message that acts on it. */
	count = format_buffer(words, FORMAT_BUFFER_SESSION_CONTEXT, session_context);
	count += format_buffer(words + count, FORMAT_BUFFER_MESSAGE, message);

	/* The rest of the step does nothing. */
	count = amdrdna_vcn_decode_pad(words, count);

	/* Succeeded: the words written. */
	return count;
}

/*
 * Writes the indirect buffer that decodes one picture: each buffer named
 * to the firmware in the order the drivers use, then the engine's start,
 * padded to the ring's step (AMDRDNA_VCN_DECODE_RUN_WORDS); returns the
 * words written.
 */
unsigned
amdrdna_vcn_decode_run(
	uint32_t *words,
	const struct amdrdna_vcn_buffers *buffers)
{
	unsigned count;

	/* The session context and the message, the DPB and the decoder's context. */
	count = format_buffer(words, FORMAT_BUFFER_SESSION_CONTEXT, buffers->session_context);
	count += format_buffer(words + count, FORMAT_BUFFER_MESSAGE, buffers->message);
	count += format_buffer(words + count, FORMAT_BUFFER_DPB, buffers->dpb);
	count += format_buffer(words + count, FORMAT_BUFFER_CONTEXT, buffers->context);

	/* The bitstream, the target, the feedback and the scaling table. */
	count += format_buffer(words + count, FORMAT_BUFFER_BITSTREAM, buffers->bitstream);
	count += format_buffer(words + count, FORMAT_BUFFER_TARGET, buffers->target);
	count += format_buffer(words + count, FORMAT_BUFFER_FEEDBACK, buffers->feedback);
	count += format_buffer(words + count, FORMAT_BUFFER_SCALING, buffers->scaling);

	/* The engine starts on what it was given. */
	count += format_register(words + count, FORMAT_REG_ENGINE_CONTROL, FORMAT_ENGINE_START);

	/* The rest of the step does nothing. */
	count = amdrdna_vcn_decode_pad(words, count);

	/* Succeeded: the words written. */
	return count;
}

/*
 * Fills the feedback buffer's header before a decode: the header's size
 * as the whole size, no buffers after it; the rest is zeroed.
 */
void
amdrdna_vcn_feedback_init(
	uint8_t *feedback)
{
	size_t at;

	/* The whole buffer zeroed. */
	for (at = 0U; at < AMDRDNA_VCN_FEEDBACK_BYTES; at++)
		feedback[at] = 0U;

	/* The header alone. */
	format_put32(feedback, FORMAT_FEEDBACK_HEADER_BYTES_AT, AMDRDNA_VCN_FEEDBACK_HEADER_BYTES);
	format_put32(feedback, FORMAT_FEEDBACK_TOTAL_BYTES_AT, AMDRDNA_VCN_FEEDBACK_HEADER_BYTES);
	format_put32(feedback, FORMAT_FEEDBACK_BUFFERS_AT, 0U);
}

/* Reports the status word the firmware wrote into the feedback buffer's header. */
uint32_t
amdrdna_vcn_feedback_status(
	const uint8_t *feedback)
{
	/* Succeeded: the word. */
	return format_get32(feedback, FORMAT_FEEDBACK_STATUS_AT);
}

/*
 * Writes the header and the decode body of a decode message of a stream
 * kind for a codec body of codec_bytes whose index kind is index_kind, and zeroes the
 * rest of the message: the header names the decode body and the codec
 * body, the decode body the picture's size and buffers.
 *
 * Returns 0, or EINVAL when bytes cannot hold the message.
 */
static int
format_message_decode(
	uint8_t *message,
	size_t bytes,
	const struct amdrdna_vcn_frame *frame,
	uint32_t stream,
	uint32_t index_kind,
	uint32_t codec_bytes)
{
	uint32_t total;
	uint32_t dpb_pitch;
	uint32_t dpb_height;
	size_t at;

	/* Refuses a buffer smaller than the message. */
	total = FORMAT_CODEC_AT + codec_bytes;
	if (bytes < total)
		return EINVAL;

	/* Zeroes the whole buffer, so no stale byte follows the message. */
	for (at = 0U; at < bytes; at++)
		message[at] = 0U;

	/* The header: its size, the whole message's, two bodies, a decode, the handle, the picture's number. */
	format_put32(message, FORMAT_MESSAGE_HEADER_BYTES_AT, FORMAT_MESSAGE_HEADER_BYTES);
	format_put32(message, FORMAT_MESSAGE_TOTAL_BYTES_AT, total);
	format_put32(message, FORMAT_MESSAGE_BUFFERS_AT, 2U);
	format_put32(message, FORMAT_MESSAGE_KIND_AT, FORMAT_MESSAGE_DECODE);
	format_put32(message, FORMAT_MESSAGE_HANDLE_AT, frame->handle);
	format_put32(message, FORMAT_MESSAGE_FEEDBACK_AT, frame->serial);

	/* The decode body's index entry. */
	format_put32(message, FORMAT_MESSAGE_INDEX_KIND_AT, FORMAT_INDEX_DECODE);
	format_put32(message, FORMAT_MESSAGE_INDEX_START_AT, FORMAT_DECODE_AT);
	format_put32(message, FORMAT_MESSAGE_INDEX_BYTES_AT, FORMAT_DECODE_BYTES);
	format_put32(message, FORMAT_MESSAGE_INDEX_FILLED_AT, 0U);

	/* The codec body's index entry. */
	format_put32(message, FORMAT_CODEC_INDEX_KIND_AT, index_kind);
	format_put32(message, FORMAT_CODEC_INDEX_START_AT, FORMAT_CODEC_AT);
	format_put32(message, FORMAT_CODEC_INDEX_BYTES_AT, codec_bytes);
	format_put32(message, FORMAT_CODEC_INDEX_FILLED_AT, 0U);

	/* The stream's kind, no flags, the picture's size. */
	format_put32(message, FORMAT_DECODE_AT + FORMAT_DECODE_STREAM_AT, stream);
	format_put32(message, FORMAT_DECODE_AT + FORMAT_DECODE_FLAGS_AT, 0U);
	format_put32(message, FORMAT_DECODE_AT + FORMAT_DECODE_WIDTH_AT, frame->width);
	format_put32(message, FORMAT_DECODE_AT + FORMAT_DECODE_HEIGHT_AT, frame->height);

	/* The buffers' sizes: the bitstream, the DPB, the target, the decoder's context, the session context. */
	format_put32(message, FORMAT_DECODE_AT + FORMAT_DECODE_BITSTREAM_BYTES_AT, frame->bitstream_bytes);
	format_put32(message, FORMAT_DECODE_AT + FORMAT_DECODE_DPB_BYTES_AT, frame->dpb_bytes);
	format_put32(message, FORMAT_DECODE_AT + FORMAT_DECODE_TARGET_BYTES_AT, frame->target_bytes);
	format_put32(message, FORMAT_DECODE_AT + FORMAT_DECODE_CONTEXT_BYTES_AT, frame->context_bytes);
	format_put32(message, FORMAT_DECODE_AT + FORMAT_DECODE_SESSION_BYTES_AT, AMDRDNA_VCN_SESSION_CONTEXT_BYTES);

	/* The hidden DPB laid out for the session's largest picture, linear. */
	dpb_pitch = (frame->max_width + FORMAT_DPB_ALIGN - 1U) & ~(FORMAT_DPB_ALIGN - 1U);
	dpb_height = (frame->max_height + FORMAT_DPB_ALIGN - 1U) & ~(FORMAT_DPB_ALIGN - 1U);
	format_put32(message, FORMAT_DECODE_AT + FORMAT_DECODE_DPB_PITCH_AT, dpb_pitch);
	format_put32(message, FORMAT_DECODE_AT + FORMAT_DECODE_DPB_HEIGHT_AT, dpb_height);
	format_put32(message, FORMAT_DECODE_AT + FORMAT_DECODE_DPB_SWIZZLE_AT, FORMAT_LINEAR);
	format_put32(message, FORMAT_DECODE_AT + FORMAT_DECODE_DPB_ARRAY_AT, FORMAT_LINEAR);

	/*
	 * The target, linear and progressive: the luma pitch in bytes (one
	 * sample each), the chroma pitch in its two-byte pairs, the luma
	 * plane at its start and the chroma plane after it, the bottom
	 * field's planes where the top field's are.
	 */
	format_put32(message, FORMAT_DECODE_AT + FORMAT_DECODE_PITCH_AT, frame->pitch);
	format_put32(message, FORMAT_DECODE_AT + FORMAT_DECODE_CHROMA_PITCH_AT, frame->pitch / 2U);
	format_put32(message, FORMAT_DECODE_AT + FORMAT_DECODE_SWIZZLE_AT, FORMAT_LINEAR);
	format_put32(message, FORMAT_DECODE_AT + FORMAT_DECODE_ARRAY_AT, FORMAT_LINEAR);
	format_put32(message, FORMAT_DECODE_AT + FORMAT_DECODE_FIELD_AT, 0U);
	format_put32(message, FORMAT_DECODE_AT + FORMAT_DECODE_LUMA_TOP_AT, 0U);
	format_put32(message, FORMAT_DECODE_AT + FORMAT_DECODE_LUMA_BOTTOM_AT, 0U);
	format_put32(message, FORMAT_DECODE_AT + FORMAT_DECODE_CHROMA_TOP_AT, frame->chroma_offset);
	format_put32(message, FORMAT_DECODE_AT + FORMAT_DECODE_CHROMA_BOTTOM_AT, frame->chroma_offset);

	/* Succeeded: the header and the decode body are written. */
	return 0;
}

/* Writes the naming of one buffer to the firmware (6 words): its address in the data words, then its kind. */
static unsigned
format_buffer(
	uint32_t *words,
	uint32_t kind,
	uint64_t address)
{
	unsigned count;

	/* The address's halves, then the command that names the buffer's kind. */
	count = format_register(words, FORMAT_REG_DATA0, (uint32_t)address);
	count += format_register(words + count, FORMAT_REG_DATA1, (uint32_t)(address >> 32));
	count += format_register(words + count, FORMAT_REG_COMMAND, kind << 1);

	/* Succeeded: the words written. */
	return count;
}

/* Writes one register write of the decode ring (2 words); returns the words written. */
static unsigned
format_register(
	uint32_t *words,
	uint32_t which,
	uint32_t value)
{
	/* The header word naming the register, then the value. */
	words[0] = which & FORMAT_WRITE_REGISTER_MASK;
	words[1] = value;

	/* Succeeded: the two words. */
	return 2U;
}

/* Writes a little-endian 32-bit word at a byte offset. */
static void
format_put32(
	uint8_t *bytes,
	size_t at,
	uint32_t value)
{
	/* The four bytes from the least significant. */
	bytes[at] = (uint8_t)value;
	bytes[at + 1U] = (uint8_t)(value >> 8);
	bytes[at + 2U] = (uint8_t)(value >> 16);
	bytes[at + 3U] = (uint8_t)(value >> 24);
}

/* Writes a little-endian 16-bit word at a byte offset. */
static void
format_put16(
	uint8_t *bytes,
	size_t at,
	uint32_t value)
{
	/* The two bytes from the least significant. */
	bytes[at] = (uint8_t)value;
	bytes[at + 1U] = (uint8_t)(value >> 8);
}

/* Reads a little-endian 32-bit word at a byte offset. */
static uint32_t
format_get32(
	const uint8_t *bytes,
	size_t at)
{
	uint32_t value;

	/* The four bytes from the least significant. */
	value = (uint32_t)bytes[at];
	value |= (uint32_t)bytes[at + 1U] << 8;
	value |= (uint32_t)bytes[at + 2U] << 16;
	value |= (uint32_t)bytes[at + 3U] << 24;

	/* Succeeded: the word. */
	return value;
}

/* The H.264 body of a decode message, its scaling lists and the sizes of an H.264 session's buffers. */
#include "drivers/gpu/amdrdna/vcn-h264.inc"

/* The H.265 body of a decode message, its tiles and scaling lists and the sizes of an H.265 session's buffers. */
#include "drivers/gpu/amdrdna/vcn-h265.inc"
