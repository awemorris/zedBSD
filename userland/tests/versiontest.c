/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

int
versioned_value_v1(void)
{
	return 41;
}

int
versioned_value_v2(void)
{
	return 42;
}

__asm__(".symver versioned_value_v1,versioned_value@ZEDBSD_1.0");
__asm__(".symver versioned_value_v2,versioned_value@@ZEDBSD_2.0");
