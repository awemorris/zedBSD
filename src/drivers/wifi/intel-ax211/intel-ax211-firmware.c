/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Intel AX211 exact firmware-file loader.
 */

#include "intel-ax211-firmware.h"
#include <kern/kcrt.h>

#include <uapi/errno.h>
#include <uapi/fcntl.h>

#include "kern/file.h"
#include "kern/kmem.h"
#include "kern/vfs.h"

/*
 * The running state of one SHA-256 computation.
 *
 * Procedure referenced: FIPS 180-4, 6.2 (the SHA-256 hash computation).
 */
struct ax211_sha256_context {
	uint32_t state[8];
	uint64_t length;
	uint8_t block[64];
	size_t used;
};

/*
 * The 64 round constants of SHA-256.
 *
 * The loader hashes each firmware file before it parses it.
 *
 * Values referenced: FIPS 180-4, 4.2.2 (the first 32 bits of the fractional
 * parts of the cube roots of the first 64 primes).
 */
static const uint32_t ax211_sha256_constants[64] = {
	0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
	0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
	0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
	0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
	0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
	0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
	0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
	0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
	0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
	0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
	0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
	0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
	0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

/*
 * The SHA-256 digests of the pinned firmware image and PNVM file.
 *
 * The digests were measured by zedBSD on the two files named in
 * intel-ax211-internal.h; a file with another digest is not loaded.
 *
 * Values referenced: linux-firmware 20260410 (commit
 * dc85ccedc9c973682fbcf4d628ca61174bcc3120)
 * intel/iwlwifi/iwlwifi-so-a0-gf-a0-89.ucode and
 * intel/iwlwifi/iwlwifi-so-a0-gf-a0.pnvm.
 */
static const uint8_t ax211_ucode_digest[32] = {
	0xc5U, 0x69U, 0xc4U, 0xb0U, 0xffU, 0xe2U, 0x05U, 0x4aU,
	0x1cU, 0xedU, 0xd5U, 0xafU, 0xfcU, 0xcfU, 0xf2U, 0xdaU,
	0x85U, 0x15U, 0x32U, 0x5eU, 0xebU, 0x23U, 0xf7U, 0x88U,
	0xc7U, 0xabU, 0xe9U, 0x46U, 0x3dU, 0x1aU, 0x15U, 0x14U};

/* The SHA-256 digest of the pinned PNVM file, measured as above. */
static const uint8_t ax211_pnvm_digest[32] = {
	0xefU, 0xa9U, 0x72U, 0x6dU, 0x4aU, 0x9dU, 0x44U, 0xb8U,
	0x3fU, 0xc9U, 0xa1U, 0x4cU, 0xedU, 0xcfU, 0x30U, 0x6aU,
	0x4eU, 0x43U, 0x9eU, 0x9dU, 0xe9U, 0x19U, 0x80U, 0x2eU,
	0xb9U, 0xe9U, 0x2dU, 0xf4U, 0xecU, 0x03U, 0x2bU, 0x2aU};

static uint32_t ax211_rotate_right(uint32_t value, unsigned amount);
static uint32_t ax211_get_be32(const uint8_t *bytes);
static void ax211_put_be32(uint8_t *bytes, uint32_t value);
static void ax211_sha256_transform(struct ax211_sha256_context *context, const uint8_t block[64]);
static void ax211_sha256_init(struct ax211_sha256_context *context);
static int ax211_sha256_update(struct ax211_sha256_context *context, const uint8_t *bytes, size_t length);
static void ax211_sha256_final(struct ax211_sha256_context *context, uint8_t digest[32]);
static int ax211_sha256(const void *data, size_t length, uint8_t digest[32]);
static int ax211_digest_matches(const uint8_t *bytes, size_t length, const uint8_t expected[32]);
static void ax211_release_bytes(uint8_t **bytes, size_t exact_size);
static int ax211_files_state(const struct intel_ax211_firmware_files *files, int *owned);
static int ax211_read_exact_file(const char *path, size_t exact_size, const uint8_t digest[32], uint8_t **bytes);
static int ax211_parse_ucode(const uint8_t *bytes, size_t length, struct intel_ax211_firmware_manifest *manifest);
static int ax211_inspect_pnvm(const uint8_t *bytes, size_t length, struct intel_ax211_pnvm_inventory *inventory);

/*
 * Frees the two files and returns the set to its empty state.
 */
void
drv_intel_ax211_firmware_files_release(
	struct intel_ax211_firmware_files *files)
{
	/* Nothing to release without a set. */
	if (files == NULL)
		return;

	/* Clears and frees both buffers, then the bookkeeping. */
	ax211_release_bytes(&files->ucode_bytes, INTEL_AX211_FIRMWARE_SIZE);
	ax211_release_bytes(&files->pnvm_bytes, INTEL_AX211_PNVM_FILE_SIZE);
	kern_memset(files, 0, sizeof(*files));
}

/*
 * Loads, verifies and parses the pinned firmware image and PNVM file.
 *
 * The files are read into a candidate set first; the caller's set is replaced
 * only when both files were read, matched their digests and parsed.
 */
int
drv_intel_ax211_firmware_files_load(
	struct intel_ax211_firmware_files *files)
{
	struct intel_ax211_firmware_files candidate;
	int owned;
	int error;
	int parsed;

	/* Learns whether the caller's set holds an earlier load. */
	error = ax211_files_state(files, &owned);
	if (error != 0)
		return error;

	/* Reads the firmware image and checks its digest. */
	kern_memset(&candidate, 0, sizeof(candidate));
	error = ax211_read_exact_file(
		INTEL_AX211_FIRMWARE_VFS_PATH, INTEL_AX211_FIRMWARE_SIZE,
		ax211_ucode_digest, &candidate.ucode_bytes);

	/* Parses the image; a malformed image is an illegal byte sequence. */
	if (error == 0) {
		candidate.ucode_size = INTEL_AX211_FIRMWARE_SIZE;
		parsed = ax211_parse_ucode(
			candidate.ucode_bytes, candidate.ucode_size,
			&candidate.ucode_manifest);
		if (parsed != INTEL_AX211_SUCCESS)
			error = EILSEQ;
	}

	/* Reads the PNVM file and checks its digest. */
	if (error == 0) {
		error = ax211_read_exact_file(
			INTEL_AX211_PNVM_VFS_PATH, INTEL_AX211_PNVM_FILE_SIZE,
			ax211_pnvm_digest, &candidate.pnvm_bytes);
		if (error == 0)
			candidate.pnvm_size = INTEL_AX211_PNVM_FILE_SIZE;
	}

	/*
	 * Inspects the PNVM file; a malformed file is an illegal byte sequence.
	 */
	if (error == 0) {
		parsed = ax211_inspect_pnvm(candidate.pnvm_bytes, candidate.pnvm_size,
					    &candidate.pnvm_inventory);
		if (parsed != INTEL_AX211_SUCCESS)
			error = EILSEQ;
	}

	/* Discards the candidate and reports why it could not be loaded. */
	if (error != 0) {
		drv_intel_ax211_firmware_files_release(&candidate);
		return error;
	}

	/* Replaces an earlier load with the candidate. */
	if (owned)
		drv_intel_ax211_firmware_files_release(files);
	*files = candidate;

	/* Succeeded: the caller's set holds the verified files. */
	return 0;
}

#ifdef INTEL_AX211_FIRMWARE_LOADER_HOST_TEST
/*
 * Computes the SHA-256 digest of a buffer for the host test.
 */
int
drv_intel_ax211_firmware_loader_test_sha256(
	const void *data,
	size_t length,
	uint8_t digest[32])
{
	int error;

	/* Computes the digest. */
	error = ax211_sha256(data, length, digest);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}
#endif

/* Rotates a 32-bit word right by the given number of bits. */
static uint32_t
ax211_rotate_right(
	uint32_t value,
	unsigned amount)
{
	/*
	 * Combines the bits shifted out on the right with the bits shifted in
	 * on the left.
	 */
	return (value >> amount) | (value << (32U - amount));
}

/* Reads a 32-bit big-endian value. */
static uint32_t
ax211_get_be32(
	const uint8_t *bytes)
{
	/* Assembles the word from its four bytes, most significant first. */
	return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
	       ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

/* Stores a 32-bit value in big-endian byte order. */
static void
ax211_put_be32(
	uint8_t *bytes,
	uint32_t value)
{
	bytes[0] = (uint8_t)(value >> 24);
	bytes[1] = (uint8_t)(value >> 16);
	bytes[2] = (uint8_t)(value >> 8);
	bytes[3] = (uint8_t)value;
}

/* Mixes one 64-byte block into the hash state. */
static void
ax211_sha256_transform(
	struct ax211_sha256_context *context,
	const uint8_t block[64])
{
	uint32_t schedule_sigma0;
	uint32_t schedule_sigma1;
	uint32_t sum0;
	uint32_t sum1;
	uint32_t choose;
	uint32_t majority;
	uint32_t temporary1;
	uint32_t temporary2;
	uint32_t schedule[64];
	uint32_t a, b, c, d, e, f, g, h;
	unsigned index;

	/* Takes the first 16 words of the message schedule from the block. */
	for (index = 0; index < 16U; index++)
		schedule[index] = ax211_get_be32(block + index * 4U);

	/* Extends the schedule to 64 words. */
	for (; index < 64U; index++) {
		schedule_sigma0 = ax211_rotate_right(schedule[index - 15U], 7U) ^
				  ax211_rotate_right(schedule[index - 15U], 18U) ^
				  (schedule[index - 15U] >> 3);
		schedule_sigma1 = ax211_rotate_right(schedule[index - 2U], 17U) ^
				  ax211_rotate_right(schedule[index - 2U], 19U) ^
				  (schedule[index - 2U] >> 10);
		schedule[index] = schedule[index - 16U] + schedule_sigma0 +
				  schedule[index - 7U] + schedule_sigma1;
	}

	/* Starts the eight working variables from the hash state. */
	a = context->state[0];
	b = context->state[1];
	c = context->state[2];
	d = context->state[3];
	e = context->state[4];
	f = context->state[5];
	g = context->state[6];
	h = context->state[7];

	/* Runs the 64 rounds of the compression function. */
	for (index = 0; index < 64U; index++) {
		sum1 = ax211_rotate_right(e, 6U) ^
		       ax211_rotate_right(e, 11U) ^
		       ax211_rotate_right(e, 25U);
		choose = (e & f) ^ (~e & g);
		temporary1 = h + sum1 + choose +
			     ax211_sha256_constants[index] +
			     schedule[index];
		sum0 = ax211_rotate_right(a, 2U) ^
		       ax211_rotate_right(a, 13U) ^
		       ax211_rotate_right(a, 22U);
		majority = (a & b) ^ (a & c) ^ (b & c);
		temporary2 = sum0 + majority;
		h = g;
		g = f;
		f = e;
		e = d + temporary1;
		d = c;
		c = b;
		b = a;
		a = temporary1 + temporary2;
	}

	/* Adds the working variables into the hash state. */
	context->state[0] += a;
	context->state[1] += b;
	context->state[2] += c;
	context->state[3] += d;
	context->state[4] += e;
	context->state[5] += f;
	context->state[6] += g;
	context->state[7] += h;

	/* Clears the schedule, which holds message words. */
	drv_intel_ax211_scrub(schedule, sizeof(schedule));
}

/* Starts a SHA-256 computation from the initial hash values. */
static void
ax211_sha256_init(
	struct ax211_sha256_context *context)
{
	/* The initial hash values of SHA-256 (FIPS 180-4, 5.3.3). */
	static const uint32_t initial[8] = {
		0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
		0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};

	/* Clears the state and loads the initial hash values. */
	kern_memset(context, 0, sizeof(*context));
	kern_memcpy(context->state, initial, sizeof(initial));
}

/* Adds bytes to a SHA-256 computation. */
static int
ax211_sha256_update(
	struct ax211_sha256_context *context,
	const uint8_t *bytes,
	size_t length)
{
	size_t available;
	size_t amount;
	size_t remaining = length;

	/* Refuses a missing buffer of nonzero length. */
	if (length != 0U && bytes == NULL)
		return EINVAL;

	/* Refuses a total length whose bit count overflows 64 bits. */
	if ((uint64_t)length > UINT64_MAX / 8U - context->length)
		return EOVERFLOW;

	/*
	 * Counts the bytes, then fills the block buffer and hashes each full
	 * block.
	 */
	context->length += (uint64_t)length;
	while (remaining != 0U) {
		available = sizeof(context->block) - context->used;
		amount = remaining < available ? remaining : available;
		kern_memcpy(context->block + context->used, bytes, amount);
		context->used += amount;
		bytes += amount;
		remaining -= amount;

		/* Hashes the block once it is full. */
		if (context->used == sizeof(context->block)) {
			ax211_sha256_transform(context, context->block);
			context->used = 0U;
		}
	}

	/* Succeeded: the bytes are part of the computation. */
	return 0;
}

/* Pads the last block, hashes it and writes the digest. */
static void
ax211_sha256_final(
	struct ax211_sha256_context *context,
	uint8_t digest[32])
{
	uint64_t bit_length = context->length * 8U;
	unsigned index;

	/* Appends the one bit that ends the message. */
	context->block[context->used++] = 0x80U;

	/*
	 * Hashes the block first when the length no longer fits behind the end
	 * bit.
	 */
	if (context->used > 56U) {
		kern_memset(context->block + context->used, 0,
		       sizeof(context->block) - context->used);
		ax211_sha256_transform(context, context->block);
		context->used = 0U;
	}

	/*
	 * Zero-fills up to the length field and stores the bit length
	 * big-endian.
	 */
	kern_memset(context->block + context->used, 0, 56U - context->used);
	for (index = 0; index < 8U; index++) {
		context->block[63U - index] =
			(uint8_t)(bit_length >> (index * 8U));
	}

	/* Hashes the last block. */
	ax211_sha256_transform(context, context->block);

	/* Writes the digest big-endian and clears the state. */
	for (index = 0; index < 8U; index++)
		ax211_put_be32(digest + index * 4U, context->state[index]);
	drv_intel_ax211_scrub(context, sizeof(*context));
}

/* Computes the SHA-256 digest of a buffer. */
static int
ax211_sha256(
	const void *data,
	size_t length,
	uint8_t digest[32])
{
	struct ax211_sha256_context context;
	int error;

	/*
	 * Refuses a missing digest buffer, or missing data of nonzero length.
	 */
	if (digest == NULL || (data == NULL && length != 0U))
		return EINVAL;

	/* Starts the computation. */
	ax211_sha256_init(&context);

	/*
	 * Hashes the data, and finishes on success or clears the state on
	 * failure.
	 */
	error = ax211_sha256_update(&context, data, length);
	if (error == 0) {
		ax211_sha256_final(&context, digest);
	} else {
		drv_intel_ax211_scrub(&context, sizeof(context));
	}

	/* Reports why the data could not be hashed. */
	if (error != 0)
		return error;

	/* Succeeded: the digest is written. */
	return 0;
}

/*
 * Checks a buffer against its expected SHA-256 digest: 0 on a match, EILSEQ
 * otherwise.
 */
static int
ax211_digest_matches(
	const uint8_t *bytes,
	size_t length,
	const uint8_t expected[32])
{
	uint8_t actual[32];
	uint8_t difference = 0U;
	unsigned index;
	int error;

	/* Computes the digest of the buffer. */
	error = ax211_sha256(bytes, length, actual);
	if (error != 0)
		return error;

	/*
	 * Compares every byte, so the time taken does not depend on where they
	 * differ.
	 */
	for (index = 0; index < sizeof(actual); index++)
		difference |= (uint8_t)(actual[index] ^ expected[index]);

	/* Clears the computed digest. */
	drv_intel_ax211_scrub(actual, sizeof(actual));

	/* Refuses a buffer whose digest differs. */
	if (difference != 0U)
		return EILSEQ;

	/* Succeeded: the buffer is the expected file. */
	return 0;
}

/* Clears and frees a file buffer and forgets it. */
static void
ax211_release_bytes(
	uint8_t **bytes,
	size_t exact_size)
{
	/* Only a buffer that is held is freed. */
	if (*bytes != NULL) {
		drv_intel_ax211_scrub(*bytes, exact_size);
		kern_free(*bytes);
		*bytes = NULL;
	}
}

/*
 * Learns whether a file set holds an earlier load: 0 and *owned set, or EINVAL.
 */
static int
ax211_files_state(
	const struct intel_ax211_firmware_files *files,
	int *owned)
{
	int ucode_present;
	int pnvm_present;

	/* Refuses missing arguments. */
	if (files == NULL || owned == NULL)
		return EINVAL;

	/* Notes which of the two buffers are held. */
	ucode_present = 0;
	if (files->ucode_bytes != NULL)
		ucode_present = 1;
	pnvm_present = 0;
	if (files->pnvm_bytes != NULL)
		pnvm_present = 1;

	/* An empty set owns nothing, and its sizes must be zero too. */
	if (!ucode_present && !pnvm_present) {
		*owned = 0;
		if (files->ucode_size != 0U || files->pnvm_size != 0U)
			return EINVAL;
		return 0;
	}

	/*
	 * Refuses a set that holds one file only, or files of the wrong size.
	 */
	if (!ucode_present ||
	    !pnvm_present ||
	    files->ucode_size != INTEL_AX211_FIRMWARE_SIZE ||
	    files->pnvm_size != INTEL_AX211_PNVM_FILE_SIZE) {
		return EINVAL;
	}

	/* Succeeded: the set owns an earlier load. */
	*owned = 1;
	return 0;
}

/* Reads a file of an exact size and checks its digest. */
static int
ax211_read_exact_file(
	const char *path,
	size_t exact_size,
	const uint8_t digest[32],
	uint8_t **bytes)
{
	ssize_t count;
	struct file_content_lease lease;
	struct file *file = NULL;
	uint8_t *result = NULL;
	size_t offset = 0U;
	int lease_active = 0;
	int error;
	int close_error;

	/* Refuses missing arguments. */
	if (path == NULL || digest == NULL || bytes == NULL)
		return EINVAL;

	/* Opens the file and takes a lease on its content. */
	kern_memset(&lease, 0, sizeof(lease));
	error = file_openat(&kern_cwdinfo, path, O_RDONLY | O_NOFOLLOW, 0,
			    &file);
	if (error == 0) {
		error = file_content_lease_begin(file, &lease);
		if (error == 0)
			lease_active = 1;
	}

	/* Refuses a file of another size. */
	if (error == 0 &&
	    (lease.size < 0 || (uint64_t)lease.size != (uint64_t)exact_size))
		error = EINVAL;

	/* Allocates the buffer for the whole file. */
	if (error == 0) {
		result = kern_malloc(exact_size);
		if (result == NULL)
			error = ENOMEM;
	}

	/* Reads the file until the buffer is full. */
	while (error == 0 && offset < exact_size) {
		count = file_content_lease_pread(&lease, result + offset,
						 exact_size - offset,
						 (off_t)offset);

		/*
		 * A failed read, an empty read or an overlong read ends the
		 * loop.
		 */
		if (count < 0)
			error = (int)-count;
		else if (count == 0 || (size_t)count > exact_size - offset)
			error = EIO;
		else
			offset += (size_t)count;
	}

	/* Ends the lease. */
	if (lease_active)
		file_content_lease_end(&lease);

	/*
	 * Closes the file; a close error counts only when nothing failed
	 * before.
	 */
	if (file != NULL) {
		close_error = file_close(file);
		if (error == 0 && close_error != 0)
			error = close_error;
	}

	/* Checks the digest of what was read. */
	if (error == 0)
		error = ax211_digest_matches(result, exact_size, digest);

	/* Frees the buffer on failure, or hands it to the caller. */
	if (error != 0)
		ax211_release_bytes(&result, exact_size);
	else
		*bytes = result;

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Parses the firmware image, through the host test's parser in the host build.
 */
static int
ax211_parse_ucode(
	const uint8_t *bytes,
	size_t length,
	struct intel_ax211_firmware_manifest *manifest)
{
	int error;

#ifdef INTEL_AX211_FIRMWARE_LOADER_HOST_TEST

	/* Parses the image with the host test's parser. */
	error = intel_ax211_firmware_loader_host_parse(bytes, length, manifest);

	/* Reports the parser's result. */
	return error;

#else

	/* Parses the image. */
	error = drv_intel_ax211_firmware_parse(bytes, length, manifest);

	/* Reports the parser's result. */
	return error;

#endif
}

/* Inspects the PNVM file, through the host test's code in the host build. */
static int
ax211_inspect_pnvm(
	const uint8_t *bytes,
	size_t length,
	struct intel_ax211_pnvm_inventory *inventory)
{
	int error;

#ifdef INTEL_AX211_FIRMWARE_LOADER_HOST_TEST

	/* Inspects the file with the host test's code. */
	error = intel_ax211_firmware_loader_host_inspect_pnvm(
		bytes, length, inventory);

	/* Reports the inspection's result. */
	return error;

#else

	/* Inspects the file. */
	error = drv_intel_ax211_pnvm_inspect(bytes, length, inventory);

	/* Reports the inspection's result. */
	return error;

#endif
}
