/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "../src/hal/x86/rtc.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

struct fake_cmos {
	uint8 registers[128];
	int uip_stuck;
};

static uint8
fake_read(uint8 index, void *argument)
{
	struct fake_cmos *cmos = argument;

	if (index == 0x0aU && cmos->uip_stuck)
		return 0x80U;
	return cmos->registers[index];
}

static uint8 bcd(unsigned value)
{ return (uint8)(((value / 10U) << 4) | (value % 10U)); }

static void
set_bcd(struct fake_cmos *cmos, unsigned year, unsigned month, unsigned day,
	unsigned hour, unsigned minute, unsigned second)
{
	memset(cmos, 0, sizeof(*cmos));
	cmos->registers[0x00] = bcd(second);
	cmos->registers[0x02] = bcd(minute);
	cmos->registers[0x04] = bcd(hour);
	cmos->registers[0x07] = bcd(day);
	cmos->registers[0x08] = bcd(month);
	cmos->registers[0x09] = bcd(year % 100U);
	cmos->registers[0x0b] = 0x02U;
}

int
main(void)
{
	struct fake_cmos cmos;
	uint64 seconds;

	set_bcd(&cmos, 2000, 1, 1, 0, 0, 0);
	assert(x86_cmos_rtc_read(fake_read, &cmos, &seconds));
	assert(seconds == 946684800ULL);

	/* Binary, 12-hour, 1:02:03 PM. */
	memset(&cmos, 0, sizeof(cmos));
	cmos.registers[0x00] = 3;
	cmos.registers[0x02] = 2;
	cmos.registers[0x04] = 0x80U | 1U;
	cmos.registers[0x07] = 1;
	cmos.registers[0x08] = 1;
	cmos.registers[0x09] = 0;
	cmos.registers[0x0b] = 0x04U;
	assert(x86_cmos_rtc_read(fake_read, &cmos, &seconds));
	assert(seconds == 946684800ULL + 13U * 3600U + 2U * 60U + 3U);

	set_bcd(&cmos, 2024, 2, 29, 23, 59, 59);
	assert(x86_cmos_rtc_read(fake_read, &cmos, &seconds));
	set_bcd(&cmos, 2023, 2, 29, 0, 0, 0);
	assert(!x86_cmos_rtc_read(fake_read, &cmos, &seconds));
	set_bcd(&cmos, 2026, 13, 1, 0, 0, 0);
	assert(!x86_cmos_rtc_read(fake_read, &cmos, &seconds));
	set_bcd(&cmos, 2026, 8, 16, 12, 34, 60);
	assert(!x86_cmos_rtc_read(fake_read, &cmos, &seconds));
	set_bcd(&cmos, 2026, 8, 16, 12, 34, 56);
	cmos.uip_stuck = 1;
	assert(!x86_cmos_rtc_read(fake_read, &cmos, &seconds));
	assert(!x86_cmos_rtc_read(NULL, &cmos, &seconds));
	assert(!x86_cmos_rtc_read(fake_read, &cmos, NULL));

	puts("zedBSD x86 CMOS RTC decoder tests: PASS");
	return 0;
}
