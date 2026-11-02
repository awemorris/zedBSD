/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The constant-time checks of the helpers of cryc.h: the comparison, the
 * masks, the selection and the byte-order readers and writers, with their
 * secret inputs marked so that memcheck reports any branch or address
 * that depends on them.
 */

#include "check.h"
#include "ct.h"

#include "userland/base/libcrypto-compat/cryc.h"

/* Runs the checks. */
int
main(void)
{
	uint8_t left[96];
	uint8_t right[96];
	uint8_t out[96];
	uint8_t bytes[8];
	uint32_t first;
	uint32_t second;
	uint32_t mask;
	uint32_t result;
	uint32_t expected;
	uint8_t expected_byte;
	uint64_t wide;
	int equal;
	size_t index;
	int round;

	/* Fills the inputs with values the program chose (defined), as a stand-in for key material. */
	for (index = 0; index < sizeof(left); index++) {
		left[index] = (uint8_t)(index * 7U + 1U);
		right[index] = (uint8_t)(index * 7U + 1U);
	}

	/* The comparison: secret bytes, equal and then different; only the answer is public. */
	CT_SECRET(left, sizeof(left));
	CT_SECRET(right, sizeof(right));
	equal = cryc_ct_equal(left, right, sizeof(left));
	CT_PUBLIC(&equal, sizeof(equal));
	CHECK(equal == 1);
	CT_PUBLIC(left, sizeof(left));
	CT_PUBLIC(right, sizeof(right));
	right[50] = (uint8_t)(right[50] ^ 1U);
	CT_SECRET(left, sizeof(left));
	CT_SECRET(right, sizeof(right));
	equal = cryc_ct_equal(left, right, sizeof(left));
	CT_PUBLIC(&equal, sizeof(equal));
	CHECK(equal == 0);
	CT_PUBLIC(left, sizeof(left));
	CT_PUBLIC(right, sizeof(right));

	/* The masks with secret operands. */
	for (round = 0; round < 8; round++) {
		/* The operands of the zero mask: equal in even rounds (public until marked secret). */
		first = 0x01020304U * (uint32_t)(round + 1);
		second = first;
		if (round % 2 != 0)
			second = first + 5U;
		CT_SECRET(&first, sizeof(first));
		CT_SECRET(&second, sizeof(second));
		result = cryc_ct_mask_zero32(first);
		CT_PUBLIC(&result, sizeof(result));
		CHECK(result == 0);

		/* The equality mask: all ones when the operands are equal. */
		first = 0x01020304U * (uint32_t)(round + 1);
		second = first;
		if (round % 2 != 0)
			second = first + 5U;
		CT_SECRET(&first, sizeof(first));
		CT_SECRET(&second, sizeof(second));
		result = cryc_ct_mask_eq32(first, second);
		CT_PUBLIC(&result, sizeof(result));
		expected = 0U;
		if (round % 2 == 0)
			expected = 0xffffffffU;
		CHECK(result == expected);

		/* The less-than mask: all ones when the first operand is smaller. */
		first = 0x01020304U * (uint32_t)(round + 1);
		second = first;
		if (round % 2 != 0)
			second = first + 5U;
		CT_SECRET(&first, sizeof(first));
		CT_SECRET(&second, sizeof(second));
		result = cryc_ct_mask_lt32(first, second);
		CT_PUBLIC(&result, sizeof(result));
		expected = 0xffffffffU;
		if (round % 2 == 0)
			expected = 0U;
		CHECK(result == expected);
	}

	/* The selection and the conditional copy with a secret mask and secret inputs. */
	for (round = 0; round < 2; round++) {
		/* The mask is all ones in the first round (public until marked secret). */
		mask = 0U;
		if (round == 0)
			mask = 0xffffffffU;

		/* The selection of one of two secret buffers. */
		CT_SECRET(&mask, sizeof(mask));
		CT_SECRET(left, sizeof(left));
		CT_SECRET(right, sizeof(right));
		cryc_ct_select_bytes(out, left, right, sizeof(out), mask);
		CT_PUBLIC(out, sizeof(out));
		CT_PUBLIC(left, sizeof(left));
		CT_PUBLIC(right, sizeof(right));
		CT_PUBLIC(&mask, sizeof(mask));
		expected_byte = right[3];
		if (round == 0)
			expected_byte = left[3];
		CHECK(out[3] == expected_byte);

		/* The conditional copy of one secret buffer over another. */
		memcpy(out, right, sizeof(out));
		CT_SECRET(&mask, sizeof(mask));
		CT_SECRET(left, sizeof(left));
		CT_SECRET(out, sizeof(out));
		cryc_ct_copy_if(out, left, sizeof(out), mask);
		CT_PUBLIC(out, sizeof(out));
		CT_PUBLIC(left, sizeof(left));
		CT_PUBLIC(&mask, sizeof(mask));
		expected_byte = right[3];
		if (round == 0)
			expected_byte = left[3];
		CHECK(out[3] == expected_byte);
	}

	/* Reading and writing secret integers: no branch on their bytes. */
	wide = CRYC_U64(0x01020304, 0x05060708);
	CT_SECRET(&wide, sizeof(wide));
	cryc_store_be64(bytes, wide);
	CT_PUBLIC(bytes, sizeof(bytes));
	CT_PUBLIC(&wide, sizeof(wide));
	CHECK(bytes[0] == 0x01 && bytes[7] == 0x08);
	CT_SECRET(bytes, sizeof(bytes));
	wide = cryc_load_be64(bytes);
	first = cryc_load_be32(bytes);
	second = cryc_load_le32(bytes + 4);
	CT_PUBLIC(&wide, sizeof(wide));
	CT_PUBLIC(&first, sizeof(first));
	CT_PUBLIC(&second, sizeof(second));
	CHECK(wide == CRYC_U64(0x01020304, 0x05060708));
	CHECK(first == 0x01020304U && second == 0x08070605U);
	CT_PUBLIC(bytes, sizeof(bytes));

	/* The result. */
	return check_finish("ct-util");
}
