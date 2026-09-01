/*
 * zedBSD Intel AX211 exact firmware-file loader
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "intel-ax211-firmware.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include "kern/file.h"
#include "kern/kmem.h"
#include "kern/vfs.h"

struct ax211_sha256_context {
	uint32_t state[8];
	uint64_t length;
	uint8_t block[64];
	size_t used;
};

static const uint32_t ax211_sha256_constants[64] = {
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

static const uint8_t ax211_ucode_digest[32] = {
	0xc5U, 0x69U, 0xc4U, 0xb0U, 0xffU, 0xe2U, 0x05U, 0x4aU,
	0x1cU, 0xedU, 0xd5U, 0xafU, 0xfcU, 0xcfU, 0xf2U, 0xdaU,
	0x85U, 0x15U, 0x32U, 0x5eU, 0xebU, 0x23U, 0xf7U, 0x88U,
	0xc7U, 0xabU, 0xe9U, 0x46U, 0x3dU, 0x1aU, 0x15U, 0x14U
};

static const uint8_t ax211_pnvm_digest[32] = {
	0xefU, 0xa9U, 0x72U, 0x6dU, 0x4aU, 0x9dU, 0x44U, 0xb8U,
	0x3fU, 0xc9U, 0xa1U, 0x4cU, 0xedU, 0xcfU, 0x30U, 0x6aU,
	0x4eU, 0x43U, 0x9eU, 0x9dU, 0xe9U, 0x19U, 0x80U, 0x2eU,
	0xb9U, 0xe9U, 0x2dU, 0xf4U, 0xecU, 0x03U, 0x2bU, 0x2aU
};

static uint32_t
ax211_rotate_right(uint32_t value, unsigned amount)
{
	return (value >> amount) | (value << (32U - amount));
}

static uint32_t
ax211_get_be32(const uint8_t *bytes)
{
	return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
	    ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

static void
ax211_put_be32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)(value >> 24);
	bytes[1] = (uint8_t)(value >> 16);
	bytes[2] = (uint8_t)(value >> 8);
	bytes[3] = (uint8_t)value;
}

static void
ax211_sha256_transform(struct ax211_sha256_context *context,
	const uint8_t block[64])
{
	uint32_t schedule[64];
	uint32_t a, b, c, d, e, f, g, h;
	unsigned index;

	for (index = 0; index < 16U; index++)
		schedule[index] = ax211_get_be32(block + index * 4U);
	for (; index < 64U; index++) {
		uint32_t s0, s1;

		s0 = ax211_rotate_right(schedule[index - 15U], 7U) ^
		    ax211_rotate_right(schedule[index - 15U], 18U) ^
		    (schedule[index - 15U] >> 3);
		s1 = ax211_rotate_right(schedule[index - 2U], 17U) ^
		    ax211_rotate_right(schedule[index - 2U], 19U) ^
		    (schedule[index - 2U] >> 10);
		schedule[index] = schedule[index - 16U] + s0 +
		    schedule[index - 7U] + s1;
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
		uint32_t choose, majority, s0, s1, temporary1, temporary2;

		s1 = ax211_rotate_right(e, 6U) ^ ax211_rotate_right(e, 11U) ^
		    ax211_rotate_right(e, 25U);
		choose = (e & f) ^ (~e & g);
		temporary1 = h + s1 + choose + ax211_sha256_constants[index] +
		    schedule[index];
		s0 = ax211_rotate_right(a, 2U) ^ ax211_rotate_right(a, 13U) ^
		    ax211_rotate_right(a, 22U);
		majority = (a & b) ^ (a & c) ^ (b & c);
		temporary2 = s0 + majority;
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
	intel_ax211_scrub(schedule, sizeof(schedule));
}

static void
ax211_sha256_init(struct ax211_sha256_context *context)
{
	static const uint32_t initial[8] = {
		0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
		0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
	};

	memset(context, 0, sizeof(*context));
	memcpy(context->state, initial, sizeof(initial));
}

static int
ax211_sha256_update(struct ax211_sha256_context *context,
	const uint8_t *bytes, size_t length)
{
	size_t remaining = length;

	if (length != 0U && bytes == NULL)
		return EINVAL;
	if ((uint64_t)length > UINT64_MAX / 8U - context->length)
		return EOVERFLOW;
	context->length += (uint64_t)length;
	while (remaining != 0U) {
		size_t available = sizeof(context->block) - context->used;
		size_t amount = remaining < available ? remaining : available;

		memcpy(context->block + context->used, bytes, amount);
		context->used += amount;
		bytes += amount;
		remaining -= amount;
		if (context->used == sizeof(context->block)) {
			ax211_sha256_transform(context, context->block);
			context->used = 0U;
		}
	}
	return 0;
}

static void
ax211_sha256_final(struct ax211_sha256_context *context, uint8_t digest[32])
{
	uint64_t bit_length = context->length * 8U;
	unsigned index;

	context->block[context->used++] = 0x80U;
	if (context->used > 56U) {
		memset(context->block + context->used, 0,
		    sizeof(context->block) - context->used);
		ax211_sha256_transform(context, context->block);
		context->used = 0U;
	}
	memset(context->block + context->used, 0, 56U - context->used);
	for (index = 0; index < 8U; index++)
		context->block[63U - index] = (uint8_t)(bit_length >> (index * 8U));
	ax211_sha256_transform(context, context->block);
	for (index = 0; index < 8U; index++)
		ax211_put_be32(digest + index * 4U, context->state[index]);
	intel_ax211_scrub(context, sizeof(*context));
}

static int
ax211_sha256(const void *data, size_t length, uint8_t digest[32])
{
	struct ax211_sha256_context context;
	int error;

	if (digest == NULL || (data == NULL && length != 0U))
		return EINVAL;
	ax211_sha256_init(&context);
	error = ax211_sha256_update(&context, data, length);
	if (error == 0)
		ax211_sha256_final(&context, digest);
	else
		intel_ax211_scrub(&context, sizeof(context));
	return error;
}

static int
ax211_digest_matches(const uint8_t *bytes, size_t length,
	const uint8_t expected[32])
{
	uint8_t actual[32];
	uint8_t difference = 0U;
	unsigned index;
	int error;

	error = ax211_sha256(bytes, length, actual);
	if (error != 0)
		return error;
	for (index = 0; index < sizeof(actual); index++)
		difference |= (uint8_t)(actual[index] ^ expected[index]);
	intel_ax211_scrub(actual, sizeof(actual));
	return difference == 0U ? 0 : EILSEQ;
}

static void
ax211_release_bytes(uint8_t **bytes, size_t exact_size)
{
	if (*bytes != NULL) {
		intel_ax211_scrub(*bytes, exact_size);
		kern_free(*bytes);
		*bytes = NULL;
	}
}

void
intel_ax211_firmware_files_release(struct intel_ax211_firmware_files *files)
{
	if (files == NULL)
		return;
	ax211_release_bytes(&files->ucode_bytes, INTEL_AX211_FIRMWARE_SIZE);
	ax211_release_bytes(&files->pnvm_bytes, INTEL_AX211_PNVM_SIZE);
	memset(files, 0, sizeof(*files));
}

static int
ax211_files_state(const struct intel_ax211_firmware_files *files, int *owned)
{
	int ucode_present;
	int pnvm_present;

	if (files == NULL || owned == NULL)
		return EINVAL;
	ucode_present = files->ucode_bytes != NULL;
	pnvm_present = files->pnvm_bytes != NULL;
	if (!ucode_present && !pnvm_present) {
		*owned = 0;
		return files->ucode_size == 0U && files->pnvm_size == 0U ?
		    0 : EINVAL;
	}
	if (!ucode_present || !pnvm_present ||
	    files->ucode_size != INTEL_AX211_FIRMWARE_SIZE ||
	    files->pnvm_size != INTEL_AX211_PNVM_SIZE)
		return EINVAL;
	*owned = 1;
	return 0;
}

static int
ax211_read_exact_file(const char *path, size_t exact_size,
	const uint8_t digest[32], uint8_t **bytes)
{
	struct file_content_lease lease;
	struct file *file = NULL;
	uint8_t *result = NULL;
	size_t offset = 0U;
	int lease_active = 0;
	int error;
	int close_error;

	if (path == NULL || digest == NULL || bytes == NULL)
		return EINVAL;
	memset(&lease, 0, sizeof(lease));
	error = file_openat(&kern_cwdinfo, path, O_RDONLY | O_NOFOLLOW, 0,
	    &file);
	if (error == 0) {
		error = file_content_lease_begin(file, &lease);
		if (error == 0)
			lease_active = 1;
	}
	if (error == 0 && (lease.size < 0 ||
	    (uint64_t)lease.size != (uint64_t)exact_size))
		error = EINVAL;
	if (error == 0) {
		result = kern_malloc(exact_size);
		if (result == NULL)
			error = ENOMEM;
	}
	while (error == 0 && offset < exact_size) {
		ssize_t count;

		count = file_content_lease_pread(&lease, result + offset,
		    exact_size - offset, (off_t)offset);
		if (count < 0)
			error = (int)-count;
		else if (count == 0 || (size_t)count > exact_size - offset)
			error = EIO;
		else
			offset += (size_t)count;
	}
	if (lease_active)
		file_content_lease_end(&lease);
	if (file != NULL) {
		close_error = file_close(file);
		if (error == 0 && close_error != 0)
			error = close_error;
	}
	if (error == 0)
		error = ax211_digest_matches(result, exact_size, digest);
	if (error != 0)
		ax211_release_bytes(&result, exact_size);
	else
		*bytes = result;
	return error;
}

static int
ax211_parse_ucode(const uint8_t *bytes, size_t length,
	struct intel_ax211_firmware_manifest *manifest)
{
#ifdef INTEL_AX211_FIRMWARE_LOADER_HOST_TEST
	return intel_ax211_firmware_loader_host_parse(bytes, length, manifest);
#else
	return intel_ax211_firmware_parse(bytes, length, manifest);
#endif
}

static int
ax211_inspect_pnvm(const uint8_t *bytes, size_t length,
	struct intel_ax211_pnvm_inventory *inventory)
{
#ifdef INTEL_AX211_FIRMWARE_LOADER_HOST_TEST
	return intel_ax211_firmware_loader_host_inspect_pnvm(bytes, length,
	    inventory);
#else
	return intel_ax211_pnvm_inspect(bytes, length, inventory);
#endif
}

int
intel_ax211_firmware_files_load(struct intel_ax211_firmware_files *files)
{
	struct intel_ax211_firmware_files candidate;
	int owned;
	int error;

	error = ax211_files_state(files, &owned);
	if (error != 0)
		return error;
	memset(&candidate, 0, sizeof(candidate));
	error = ax211_read_exact_file(INTEL_AX211_FIRMWARE_VFS_PATH,
	    INTEL_AX211_FIRMWARE_SIZE, ax211_ucode_digest,
	    &candidate.ucode_bytes);
	if (error == 0) {
		candidate.ucode_size = INTEL_AX211_FIRMWARE_SIZE;
		if (ax211_parse_ucode(candidate.ucode_bytes,
		    candidate.ucode_size, &candidate.ucode_manifest) !=
		    INTEL_AX211_OK)
			error = EILSEQ;
	}
	if (error == 0) {
		error = ax211_read_exact_file(INTEL_AX211_PNVM_VFS_PATH,
		    INTEL_AX211_PNVM_SIZE, ax211_pnvm_digest,
		    &candidate.pnvm_bytes);
		if (error == 0)
			candidate.pnvm_size = INTEL_AX211_PNVM_SIZE;
	}
	if (error == 0 && ax211_inspect_pnvm(candidate.pnvm_bytes,
	    candidate.pnvm_size, &candidate.pnvm_inventory) != INTEL_AX211_OK)
		error = EILSEQ;
	if (error != 0) {
		intel_ax211_firmware_files_release(&candidate);
		return error;
	}
	if (owned)
		intel_ax211_firmware_files_release(files);
	*files = candidate;
	return 0;
}

#ifdef INTEL_AX211_FIRMWARE_LOADER_HOST_TEST
int
intel_ax211_firmware_loader_test_sha256(const void *data, size_t length,
	uint8_t digest[32])
{
	return ax211_sha256(data, length, digest);
}
#endif
