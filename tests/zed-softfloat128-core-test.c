/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <stdint.h>
#include "src/softfloat/zed-softfloat128.h"
#ifndef UINT64_C
#define UINT64_C(value) value##ULL
#endif
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
static int same(struct zsf128 a,struct zsf128 b)
{ return a.high==b.high&&a.low==b.low; }
int main(void)
{
	struct zsf128 one={UINT64_C(0x3fff000000000000),0};
	struct zsf128 one_half={UINT64_C(0x3fff800000000000),0};
	struct zsf128 two={UINT64_C(0x4000000000000000),0};
	struct zsf128 three={UINT64_C(0x4000800000000000),0};
	int unordered;
	CHECK(same(zsf128_add(one,one),two));
	CHECK(same(zsf128_sub(two,one),one));
	CHECK(same(zsf128_mul(one_half,two),three));
	CHECK(same(zsf128_div(three,two),one_half));
	CHECK(same(zsf64_to_128(UINT64_C(0x3ff8000000000000)),one_half));
	CHECK(zsf128_to_64(one_half)==UINT64_C(0x3ff8000000000000));
	CHECK(same(zsf_i64_to_128(-1),(struct zsf128){UINT64_C(0xbfff000000000000),0}));
	CHECK(zsf128_compare(one,two,&unordered)<0&&!unordered);
	return 0;
}
