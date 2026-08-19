/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

extern int versioned_value(void);
extern int versioned_value_v1_reference(void);

__asm__(".symver versioned_value_v1_reference,versioned_value@ZEDBSD_1.0");

int
versionuse_value(void)
{
	return versioned_value() + versioned_value_v1_reference();
}
