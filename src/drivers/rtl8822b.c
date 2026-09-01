/*
 * zedBSD RTL8822B bounded firmware, efuse, and receive codecs
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "rtl8822b-internal.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

#ifndef RTL8822B_HOST_TEST
#include "kern/file.h"
#include "kern/kmem.h"
#include "kern/vfs.h"
#include <fcntl.h>
#endif

#define RTL8822B_FW_SIGNATURE       0x8822U
#define RTL8822B_FW_VERSION             30U
#define RTL8822B_FW_SUBVERSION          20U
#define RTL8822B_FW_SUBINDEX              0U
#define RTL8822B_FW_H2C_FORMAT_VERSION   14U
#define RTL8822B_FW_MEM_USAGE          0x08U
#define RTL8822B_FW_DMEM_HEADER_ADDRESS 0x80200000U
#define RTL8822B_FW_IMEM_HEADER_ADDRESS 0x80000000U

#define RTL8822B_SYS_CFG1_CUT_SHIFT       12U
#define RTL8822B_SYS_CFG1_CUT_MASK        0x0fU
#define RTL8822B_SYS_CFG1_RTL_ID     0x00800000U
#define RTL8822B_SYS_CFG1_RF_2T2R    0x08000000U
#define RTL8822B_CUT_G                       6U

#define RTL8822B_RX_PKT_LENGTH_MASK      0x3fffU
#define RTL8822B_RX_CRC_ERROR            0x4000U
#define RTL8822B_RX_ICV_ERROR            0x8000U
#define RTL8822B_RX_DRV_INFO_SHIFT           16U
#define RTL8822B_RX_DRV_INFO_MASK            0x0fU
#define RTL8822B_RX_SHIFT_SHIFT               24U
#define RTL8822B_RX_SHIFT_MASK                0x03U
#define RTL8822B_RX_PHY_STATUS           0x04000000U
#define RTL8822B_RX_C2H_FLAG             0x10000000U
#define RTL8822B_RX_RATE_MASK                 0x7fU
#define RTL8822B_RX_RATE_MAX                  0x54U
#define RTL8822B_RX_BANDWIDTH_SHIFT              4U
#define RTL8822B_RX_BANDWIDTH_MASK             0x03U

struct rtl8822b_sha256_context {
	uint32_t state[8];
	uint64_t byte_count;
	uint8_t block[64];
	size_t block_length;
};

static const uint32_t sha256_constants[64] = {
	0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
	0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
	0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
	0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
	0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
	0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
	0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
	0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
	0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
	0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
	0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
	0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
	0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
	0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
	0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
	0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

static const uint8_t rtl8822b_firmware_digest[32] = {
	0xa7, 0x2d, 0xa6, 0x90, 0x59, 0x7b, 0xfa, 0x99,
	0xd8, 0xeb, 0x6f, 0xc2, 0xab, 0x09, 0x0d, 0x18,
	0xd8, 0xad, 0x92, 0xac, 0x2b, 0xef, 0xd3, 0x5d,
	0xb1, 0xc9, 0xe3, 0x66, 0x2d, 0x8d, 0x84, 0x18
};

static uint16_t
load_le16(const uint8_t *bytes)
{
	return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t
load_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	    ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void
store_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

static void
store_le32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

static uint32_t
rotate_right(uint32_t value, unsigned shift)
{
	return (value >> shift) | (value << (32U - shift));
}

static void
sha256_transform(struct rtl8822b_sha256_context *context,
	const uint8_t block[64])
{
	uint32_t words[64];
	uint32_t a, b, c, d, e, f, g, h;
	uint32_t choose, majority, sigma0, sigma1, temporary1, temporary2;
	unsigned index;

	for (index = 0; index < 16U; index++) {
		const uint8_t *word = block + index * 4U;

		words[index] = ((uint32_t)word[0] << 24) |
		    ((uint32_t)word[1] << 16) | ((uint32_t)word[2] << 8) |
		    (uint32_t)word[3];
	}
	for (; index < 64U; index++) {
		uint32_t small0, small1;

		small0 = rotate_right(words[index - 15U], 7U) ^
		    rotate_right(words[index - 15U], 18U) ^
		    (words[index - 15U] >> 3);
		small1 = rotate_right(words[index - 2U], 17U) ^
		    rotate_right(words[index - 2U], 19U) ^
		    (words[index - 2U] >> 10);
		words[index] = words[index - 16U] + small0 +
		    words[index - 7U] + small1;
	}

	a = context->state[0];
	b = context->state[1];
	c = context->state[2];
	d = context->state[3];
	e = context->state[4];
	f = context->state[5];
	g = context->state[6];
	h = context->state[7];
	for (index = 0; index < 64U; index++) {
		sigma1 = rotate_right(e, 6U) ^ rotate_right(e, 11U) ^
		    rotate_right(e, 25U);
		choose = (e & f) ^ ((~e) & g);
		temporary1 = h + sigma1 + choose + sha256_constants[index] +
		    words[index];
		sigma0 = rotate_right(a, 2U) ^ rotate_right(a, 13U) ^
		    rotate_right(a, 22U);
		majority = (a & b) ^ (a & c) ^ (b & c);
		temporary2 = sigma0 + majority;
		h = g;
		g = f;
		f = e;
		e = d + temporary1;
		d = c;
		c = b;
		b = a;
		a = temporary1 + temporary2;
	}
	context->state[0] += a;
	context->state[1] += b;
	context->state[2] += c;
	context->state[3] += d;
	context->state[4] += e;
	context->state[5] += f;
	context->state[6] += g;
	context->state[7] += h;
}

static void
sha256_init(struct rtl8822b_sha256_context *context)
{
	static const uint32_t initial_state[8] = {
		0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
		0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
	};

	memcpy(context->state, initial_state, sizeof(initial_state));
	context->byte_count = 0;
	context->block_length = 0;
}

static void
sha256_update(struct rtl8822b_sha256_context *context,
	const uint8_t *data, size_t length)
{
	while (length != 0U) {
		size_t available = sizeof(context->block) - context->block_length;
		size_t copy = length < available ? length : available;

		memcpy(context->block + context->block_length, data, copy);
		context->block_length += copy;
		context->byte_count += copy;
		data += copy;
		length -= copy;
		if (context->block_length == sizeof(context->block)) {
			sha256_transform(context, context->block);
			context->block_length = 0;
		}
	}
}

static void
sha256_final(struct rtl8822b_sha256_context *context, uint8_t digest[32])
{
	uint64_t bit_count = context->byte_count * 8U;
	unsigned index;

	context->block[context->block_length++] = 0x80U;
	if (context->block_length > 56U) {
		memset(context->block + context->block_length, 0,
		    sizeof(context->block) - context->block_length);
		sha256_transform(context, context->block);
		context->block_length = 0;
	}
	memset(context->block + context->block_length, 0,
	    56U - context->block_length);
	for (index = 0; index < 8U; index++)
		context->block[63U - index] = (uint8_t)(bit_count >> (index * 8U));
	sha256_transform(context, context->block);
	for (index = 0; index < 8U; index++) {
		digest[index * 4U] = (uint8_t)(context->state[index] >> 24);
		digest[index * 4U + 1U] =
		    (uint8_t)(context->state[index] >> 16);
		digest[index * 4U + 2U] =
		    (uint8_t)(context->state[index] >> 8);
		digest[index * 4U + 3U] = (uint8_t)context->state[index];
	}
	memset(context, 0, sizeof(*context));
}

int
rtl8822b_sha256(const void *data, size_t length, uint8_t digest[32])
{
	struct rtl8822b_sha256_context context;

	if (digest == NULL || (data == NULL && length != 0U))
		return EINVAL;
#if SIZE_MAX > UINT64_MAX / 8U
	if (length > (size_t)(UINT64_MAX / 8U))
		return EOVERFLOW;
#endif
	sha256_init(&context);
	if (length != 0U)
		sha256_update(&context, data, length);
	sha256_final(&context, digest);
	return 0;
}

static int
firmware_validate_expected(const uint8_t *data, size_t length,
	const uint8_t expected_digest[32], struct rtl8822b_firmware_view *view)
{
	struct rtl8822b_firmware_view result;
	uint8_t actual_digest[32];
	int error;

	if (data == NULL || expected_digest == NULL || view == NULL)
		return EINVAL;
	memset(view, 0, sizeof(*view));
	if (length != RTL8822B_FIRMWARE_SIZE)
		return EINVAL;
	if (load_le16(data) != RTL8822B_FW_SIGNATURE || data[2] != 0U ||
	    data[3] != 0U || load_le16(data + 4U) != RTL8822B_FW_VERSION ||
	    data[6] != RTL8822B_FW_SUBVERSION ||
	    data[7] != RTL8822B_FW_SUBINDEX ||
	    data[24] != RTL8822B_FW_MEM_USAGE ||
	    load_le16(data + 28U) != RTL8822B_FW_H2C_FORMAT_VERSION ||
	    load_le32(data + 32U) != RTL8822B_FW_DMEM_HEADER_ADDRESS ||
	    load_le32(data + 36U) != RTL8822B_FIRMWARE_DMEM_SIZE ||
	    load_le32(data + 48U) != RTL8822B_FIRMWARE_IMEM_SIZE ||
	    load_le32(data + 52U) != 0U ||
	    load_le32(data + 60U) != RTL8822B_FW_IMEM_HEADER_ADDRESS)
		return EINVAL;
	error = rtl8822b_sha256(data, length, actual_digest);
	if (error != 0)
		return error;
	if (memcmp(actual_digest, expected_digest, sizeof(actual_digest)) != 0) {
		memset(actual_digest, 0, sizeof(actual_digest));
		return EILSEQ;
	}
	memset(actual_digest, 0, sizeof(actual_digest));

	result.bytes = data;
	result.size = length;
	result.version = RTL8822B_FW_VERSION;
	result.subversion = RTL8822B_FW_SUBVERSION;
	result.subindex = RTL8822B_FW_SUBINDEX;
	result.dmem_offset = RTL8822B_FIRMWARE_HEADER_SIZE;
	result.dmem_length = RTL8822B_FIRMWARE_DMEM_SIZE +
	    RTL8822B_FIRMWARE_CHECKSUM_SIZE;
	result.dmem_address = RTL8822B_FIRMWARE_DMEM_ADDRESS;
	result.imem_offset = result.dmem_offset + result.dmem_length;
	result.imem_length = RTL8822B_FIRMWARE_IMEM_SIZE +
	    RTL8822B_FIRMWARE_CHECKSUM_SIZE;
	result.imem_address = RTL8822B_FIRMWARE_IMEM_ADDRESS;
	if (result.imem_offset > length || result.imem_length >
	    length - result.imem_offset ||
	    result.imem_offset + result.imem_length != length)
		return EINVAL;
	*view = result;
	return 0;
}

int
rtl8822b_firmware_validate(const uint8_t *data, size_t length,
	struct rtl8822b_firmware_view *view)
{
	return firmware_validate_expected(data, length,
	    rtl8822b_firmware_digest, view);
}

static int
firmware_blob_state(const struct rtl8822b_firmware_blob *firmware,
	int *owned)
{
	if (firmware == NULL || owned == NULL)
		return EINVAL;
	*owned = 0;
	if (firmware->bytes == NULL)
		return firmware->size == 0U ? 0 : EINVAL;
	if (firmware->size != RTL8822B_FIRMWARE_SIZE ||
	    firmware->view.bytes != firmware->bytes ||
	    firmware->view.size != firmware->size)
		return EINVAL;
	*owned = 1;
	return 0;
}

#ifndef RTL8822B_HOST_TEST
static void
firmware_scrub(void *memory, size_t length)
{
	volatile uint8_t *bytes = memory;

	while (length-- != 0U)
		*bytes++ = 0U;
}

void
rtl8822b_firmware_release(struct rtl8822b_firmware_blob *firmware)
{
	if (firmware == NULL)
		return;
	if (firmware->bytes != NULL) {
		firmware_scrub(firmware->bytes, RTL8822B_FIRMWARE_SIZE);
		kern_free(firmware->bytes);
	}
	memset(firmware, 0, sizeof(*firmware));
}

int
rtl8822b_firmware_load(struct rtl8822b_firmware_blob *firmware)
{
	struct file_content_lease lease;
	struct rtl8822b_firmware_blob result;
	struct file *file = NULL;
	ssize_t count;
	size_t offset = 0;
	int replace_owned;
	int close_error;
	int error;

	error = firmware_blob_state(firmware, &replace_owned);
	if (error != 0)
		return error;
	memset(&lease, 0, sizeof(lease));
	memset(&result, 0, sizeof(result));
	error = file_openat(&kern_cwdinfo, RTL8822B_FIRMWARE_PATH,
	    O_RDONLY | O_NOFOLLOW, 0, &file);
	if (error != 0)
		return error;
	error = file_content_lease_begin(file, &lease);
	if (error != 0)
		goto close_file;
	if (lease.size < 0 ||
	    (uint64_t)lease.size != RTL8822B_FIRMWARE_SIZE) {
		error = EINVAL;
		goto end_lease;
	}
	result.bytes = kern_malloc(RTL8822B_FIRMWARE_SIZE);
	if (result.bytes == NULL) {
		error = ENOMEM;
		goto end_lease;
	}
	result.size = RTL8822B_FIRMWARE_SIZE;
	while (offset < result.size) {
		count = file_content_lease_pread(&lease, result.bytes + offset,
		    result.size - offset, (off_t)offset);
		if (count < 0) {
			error = (int)-count;
			goto end_lease;
		}
		if (count == 0 || (size_t)count > result.size - offset) {
			error = EIO;
			goto end_lease;
		}
		offset += (size_t)count;
	}

end_lease:
	file_content_lease_end(&lease);
close_file:
	close_error = file_close(file);
	file = NULL;
	if (error == 0 && close_error != 0)
		error = close_error;
	if (error == 0)
		error = rtl8822b_firmware_validate(result.bytes, result.size,
		    &result.view);
	if (error != 0) {
		rtl8822b_firmware_release(&result);
		return error;
	}
	if (replace_owned)
		rtl8822b_firmware_release(firmware);
	*firmware = result;
	return 0;
}
#endif

#ifdef RTL8822B_TESTING
int
rtl8822b_test_firmware_validate(const uint8_t *data, size_t length,
	const uint8_t expected_digest[32], struct rtl8822b_firmware_view *view)
{
	return firmware_validate_expected(data, length, expected_digest, view);
}

int
rtl8822b_test_firmware_blob_state(
	const struct rtl8822b_firmware_blob *firmware, int *owned)
{
	return firmware_blob_state(firmware, owned);
}
#endif

static int
firmware_walk_segment(const struct rtl8822b_firmware_view *view,
	enum rtl8822b_firmware_segment segment, size_t file_offset,
	size_t segment_length, uint32_t destination,
	rtl8822b_firmware_chunk_fn callback, void *context)
{
	size_t offset = 0;

	while (offset < segment_length) {
		struct rtl8822b_firmware_chunk chunk;
		size_t remaining = segment_length - offset;
		size_t length = remaining < RTL8822B_FIRMWARE_CHUNK_MAX ?
		    remaining : RTL8822B_FIRMWARE_CHUNK_MAX;
		int error;

		if (file_offset > view->size || offset > view->size - file_offset ||
		    length > view->size - file_offset - offset ||
		    offset > UINT32_MAX - destination)
			return EOVERFLOW;
		memset(&chunk, 0, sizeof(chunk));
		chunk.segment = segment;
		chunk.file_offset = file_offset + offset;
		chunk.destination = destination + (uint32_t)offset;
		chunk.length = (uint32_t)length;
		chunk.wire_payload_length = (uint32_t)length;
		if ((length + RTL8822B_FIRMWARE_TX_DESCRIPTOR_SIZE) % 512U == 0U)
			chunk.wire_payload_length++;
		chunk.first = offset == 0U;
		chunk.last = length == remaining;
		chunk.checksum_continue = offset != 0U;
		error = callback(context, &chunk);
		if (error != 0)
			return error;
		offset += length;
	}
	return 0;
}

static int
firmware_view_equal(const struct rtl8822b_firmware_view *left,
	const struct rtl8822b_firmware_view *right)
{
	return left->bytes == right->bytes && left->size == right->size &&
	    left->version == right->version &&
	    left->subversion == right->subversion &&
	    left->subindex == right->subindex &&
	    left->dmem_offset == right->dmem_offset &&
	    left->dmem_length == right->dmem_length &&
	    left->dmem_address == right->dmem_address &&
	    left->imem_offset == right->imem_offset &&
	    left->imem_length == right->imem_length &&
	    left->imem_address == right->imem_address;
}

static int
firmware_walk_expected(const struct rtl8822b_firmware_view *view,
	const uint8_t expected_digest[32],
	rtl8822b_firmware_chunk_fn callback, void *context)
{
	struct rtl8822b_firmware_view validated;
	int error;

	if (view == NULL || expected_digest == NULL || callback == NULL ||
	    view->bytes == NULL)
		return EINVAL;
	error = firmware_validate_expected(view->bytes, view->size,
	    expected_digest, &validated);
	if (error != 0)
		return error;
	if (!firmware_view_equal(view, &validated))
		return EINVAL;
	error = firmware_walk_segment(&validated,
	    RTL8822B_FIRMWARE_SEGMENT_DMEM, validated.dmem_offset,
	    validated.dmem_length, validated.dmem_address,
	    callback, context);
	if (error != 0)
		return error;
	return firmware_walk_segment(&validated,
	    RTL8822B_FIRMWARE_SEGMENT_IMEM, validated.imem_offset,
	    validated.imem_length, validated.imem_address,
	    callback, context);
}

int
rtl8822b_firmware_walk(const struct rtl8822b_firmware_view *view,
	rtl8822b_firmware_chunk_fn callback, void *context)
{
	return firmware_walk_expected(view, rtl8822b_firmware_digest,
	    callback, context);
}

#ifdef RTL8822B_TESTING
int
rtl8822b_test_firmware_walk(const struct rtl8822b_firmware_view *view,
	const uint8_t expected_digest[32],
	rtl8822b_firmware_chunk_fn callback, void *context)
{
	return firmware_walk_expected(view, expected_digest, callback, context);
}
#endif

int
rtl8822b_firmware_tx_descriptor(uint8_t descriptor[48],
	size_t payload_length)
{
	uint16_t checksum = 0;
	unsigned index;

	if (descriptor == NULL || payload_length == 0U ||
	    payload_length > UINT16_MAX)
		return EINVAL;
	memset(descriptor, 0, RTL8822B_FIRMWARE_TX_DESCRIPTOR_SIZE);
	store_le32(descriptor, (uint32_t)payload_length |
	    ((uint32_t)RTL8822B_FIRMWARE_TX_DESCRIPTOR_SIZE << 16) |
	    (1U << 26));
	store_le32(descriptor + 4U, 16U << 8);
	for (index = 0; index < 16U; index++)
		checksum ^= load_le16(descriptor + index * 2U);
	store_le16(descriptor + 28U, checksum);
	return 0;
}

int
rtl8822b_efuse_decode(const uint8_t *physical, size_t physical_length,
	uint8_t *logical, size_t logical_length)
{
	const size_t usable = RTL8822B_EFUSE_PHYSICAL_SIZE -
	    RTL8822B_EFUSE_PROTECTED_SIZE;
	size_t physical_index = 0;

	if (physical == NULL || logical == NULL ||
	    physical_length != RTL8822B_EFUSE_PHYSICAL_SIZE ||
	    logical_length != RTL8822B_EFUSE_LOGICAL_SIZE)
		return EINVAL;
	memset(logical, 0xff, logical_length);
	while (physical_index < usable) {
		uint8_t header1 = physical[physical_index++];
		uint8_t word_enable;
		unsigned block;
		unsigned word;

		if (header1 == 0xffU)
			return 0;
		if ((header1 & 0x1fU) == 0x0fU) {
			uint8_t header2;

			if (physical_index >= usable)
				return EINVAL;
			header2 = physical[physical_index++];
			if (header2 == 0xffU)
				return 0;
			block = ((unsigned)(header2 & 0xf0U) >> 1) |
			    ((unsigned)header1 >> 5);
			word_enable = header2 & 0x0fU;
		} else {
			block = (unsigned)(header1 & 0xf0U) >> 4;
			word_enable = header1 & 0x0fU;
		}
		for (word = 0; word < 4U; word++) {
			size_t logical_index;

			if ((word_enable & (1U << word)) != 0U)
				continue;
			logical_index = (size_t)block * 8U + word * 2U;
			if (physical_index > usable ||
			    usable - physical_index < 2U ||
			    logical_index > logical_length ||
			    logical_length - logical_index < 2U)
				return EINVAL;
			logical[logical_index] = physical[physical_index++];
			logical[logical_index + 1U] =
			    physical[physical_index++];
		}
	}
	return 0;
}

int
rtl8822b_chip_identity_parse(uint32_t sys_cfg1,
	struct rtl8822b_chip_identity *identity)
{
	struct rtl8822b_chip_identity result;
	uint32_t cut;

	if (identity == NULL)
		return EINVAL;
	memset(identity, 0, sizeof(*identity));
	cut = (sys_cfg1 >> RTL8822B_SYS_CFG1_CUT_SHIFT) &
	    RTL8822B_SYS_CFG1_CUT_MASK;
	if (cut > RTL8822B_CUT_G)
		return EOPNOTSUPP;
	memset(&result, 0, sizeof(result));
	result.cut = (uint8_t)cut;
	result.rf_path_count =
	    (sys_cfg1 & RTL8822B_SYS_CFG1_RF_2T2R) != 0U ? 2U : 1U;
	result.mass_production =
	    (sys_cfg1 & RTL8822B_SYS_CFG1_RTL_ID) == 0U ? 1U : 0U;
	*identity = result;
	return 0;
}

static int
mac_address_valid(const uint8_t address[6])
{
	unsigned index;
	int all_zero = 1;
	int all_ff = 1;

	if ((address[0] & 1U) != 0U)
		return 0;
	for (index = 0; index < 6U; index++) {
		if (address[index] != 0U)
			all_zero = 0;
		if (address[index] != 0xffU)
			all_ff = 0;
	}
	return !all_zero && !all_ff;
}

int
rtl8822bu_board_parse(const uint8_t *logical, size_t logical_length,
	uint32_t sys_cfg1, struct rtl8822bu_board_info *board)
{
	struct rtl8822bu_board_info result;
	int error;

	if (logical == NULL || board == NULL ||
	    logical_length != RTL8822B_EFUSE_LOGICAL_SIZE)
		return EINVAL;
	memset(board, 0, sizeof(*board));
	memset(&result, 0, sizeof(result));
	error = rtl8822b_chip_identity_parse(sys_cfg1, &result.chip);
	if (error != 0)
		return error;
	memcpy(result.mac_address, logical + RTL8822BU_EFUSE_MAC_OFFSET,
	    sizeof(result.mac_address));
	if (!mac_address_valid(result.mac_address))
		return EINVAL;
	result.channel_plan = logical[0xb8U];
	result.crystal_cap = logical[0xb9U];
	result.thermal_meter = logical[0xbaU];
	result.rf_board_option = logical[0xc1U];
	result.rfe_option = logical[0xcaU];
	result.country_code[0] = logical[0xcbU];
	result.country_code[1] = logical[0xccU];
	if (result.rfe_option != 2U && result.rfe_option != 3U &&
	    result.rfe_option != 5U)
		return EOPNOTSUPP;
	*board = result;
	return 0;
}

static int32_t
clamp_rssi(int32_t value)
{
	if (value < -120)
		return -120;
	if (value > 0)
		return 0;
	return value;
}

int
rtl8822b_rx_packet_parse(const uint8_t *bytes, size_t length,
	struct rtl8822b_rx_packet *packet)
{
	struct rtl8822b_rx_packet result;
	uint32_t word0, word2, word3, word4;
	size_t packet_length, driver_info_length, shift, payload_offset;
	size_t occupied, aligned;
	int is_c2h;

	if (bytes == NULL || packet == NULL)
		return EINVAL;
	memset(packet, 0, sizeof(*packet));
	if (length < RTL8822B_RX_DESCRIPTOR_SIZE)
		return EINVAL;
	word0 = load_le32(bytes);
	word2 = load_le32(bytes + 8U);
	word3 = load_le32(bytes + 12U);
	word4 = load_le32(bytes + 16U);
	is_c2h = (word2 & RTL8822B_RX_C2H_FLAG) != 0U;
	packet_length = word0 & RTL8822B_RX_PKT_LENGTH_MASK;
	driver_info_length = ((word0 >> RTL8822B_RX_DRV_INFO_SHIFT) &
	    RTL8822B_RX_DRV_INFO_MASK) * 8U;
	shift = (word0 >> RTL8822B_RX_SHIFT_SHIFT) & RTL8822B_RX_SHIFT_MASK;
	if (packet_length == 0U ||
	    (!is_c2h && packet_length > RTL8822B_RX_MPDU_MAX) ||
	    (word0 & (RTL8822B_RX_CRC_ERROR | RTL8822B_RX_ICV_ERROR)) != 0U ||
	    (driver_info_length != 0U &&
	    driver_info_length != RTL8822B_RX_PHY_INFO_SIZE) ||
	    ((word0 & RTL8822B_RX_PHY_STATUS) != 0U &&
	    driver_info_length == 0U) ||
	    (word3 & RTL8822B_RX_RATE_MASK) >= RTL8822B_RX_RATE_MAX)
		return EINVAL;
	if (shift > SIZE_MAX - RTL8822B_RX_DESCRIPTOR_SIZE ||
	    driver_info_length > SIZE_MAX - RTL8822B_RX_DESCRIPTOR_SIZE - shift)
		return EOVERFLOW;
	payload_offset = RTL8822B_RX_DESCRIPTOR_SIZE + shift +
	    driver_info_length;
	if (payload_offset > length || packet_length > length - payload_offset)
		return EINVAL;
	occupied = payload_offset + packet_length;
	if (occupied > SIZE_MAX - 7U)
		return EOVERFLOW;
	aligned = (occupied + 7U) & ~(size_t)7U;
	if (aligned > length) {
		if (occupied != length)
			return EINVAL;
		aligned = occupied;
	}

	memset(&result, 0, sizeof(result));
	result.aggregate_length = aligned;
	result.rate = (uint8_t)(word3 & RTL8822B_RX_RATE_MASK);
	result.bandwidth = (uint8_t)((word4 >>
	    RTL8822B_RX_BANDWIDTH_SHIFT) & RTL8822B_RX_BANDWIDTH_MASK);
	result.tsf_low = load_le32(bytes + 20U);
	result.rssi_dbm = -128;
	if ((word0 & RTL8822B_RX_PHY_STATUS) != 0U) {
		uint8_t page;
		int32_t power_a;

		result.phy_info = bytes + RTL8822B_RX_DESCRIPTOR_SIZE + shift;
		result.phy_info_length = driver_info_length;
		page = result.phy_info[0] & 0x0fU;
		if (page != 0U && page != 1U)
			return EINVAL;
		power_a = (int32_t)result.phy_info[1] - 110;
		result.rssi_dbm = clamp_rssi(power_a);
		if (page == 1U) {
			int32_t power_b = (int32_t)result.phy_info[2] - 110;

			if (power_b > result.rssi_dbm)
				result.rssi_dbm = clamp_rssi(power_b);
		}
	}

	result.payload = bytes + payload_offset;
	if (is_c2h) {
		if (packet_length < 2U)
			return EINVAL;
		result.kind = RTL8822B_RX_C2H;
		result.payload_length = packet_length;
		result.c2h_id = result.payload[0];
		result.c2h_sequence = result.payload[1];
	} else {
		if (packet_length <= RTL8822B_RX_FCS_SIZE)
			return EINVAL;
		result.kind = RTL8822B_RX_FRAME;
		result.payload_length = packet_length - RTL8822B_RX_FCS_SIZE;
	}
	*packet = result;
	return 0;
}

int
rtl8822b_rx_aggregate_walk(const uint8_t *bytes, size_t length,
	rtl8822b_rx_packet_fn callback, void *context, size_t *packet_count)
{
	size_t offset = 0;
	size_t count = 0;

	if (bytes == NULL || callback == NULL || packet_count == NULL ||
	    length == 0U || length > RTL8822B_RX_AGGREGATE_MAX)
		return EINVAL;
	*packet_count = 0;
	while (offset < length) {
		struct rtl8822b_rx_packet packet;
		int error;

		error = rtl8822b_rx_packet_parse(bytes + offset,
		    length - offset, &packet);
		if (error != 0)
			return error;
		if (packet.aggregate_length == 0U ||
		    packet.aggregate_length > length - offset)
			return EINVAL;
		offset += packet.aggregate_length;
	}

	offset = 0;
	while (offset < length) {
		struct rtl8822b_rx_packet packet;
		int error;

		error = rtl8822b_rx_packet_parse(bytes + offset,
		    length - offset, &packet);
		if (error != 0)
			return error;
		if (packet.aggregate_length == 0U ||
		    packet.aggregate_length > length - offset)
			return EINVAL;
		error = callback(context, &packet);
		if (error != 0)
			return error;
		offset += packet.aggregate_length;
		count++;
		*packet_count = count;
	}
	return 0;
}
