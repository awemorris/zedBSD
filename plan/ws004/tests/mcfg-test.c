#include "src/hal/amd64/bsp-pcat/acpi.h"
#include <hal/hal.h>

#include <assert.h>
#include <stdint.h>
#include <string.h>

struct fixture {
	uint8_t header[44];
	uint8_t entries[32];
};

static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

static void
checksum(struct fixture *fixture, unsigned length)
{
	uint8_t sum = 0;
	unsigned index;
	fixture->header[9] = 0;
	for (index = 0; index < length; index++)
		sum = (uint8_t)(sum + ((uint8_t *)fixture)[index]);
	fixture->header[9] = (uint8_t)(0U - sum);
}

static void
initialize(struct fixture *fixture, unsigned count)
{
	unsigned length = 44U + count * 16U;
	memset(fixture, 0, sizeof(*fixture));
	memcpy(fixture->header, "MCFG", 4);
	put32(fixture->header + 4, length);
	fixture->header[8] = 1;
	put64(fixture->entries, 0xb0000000ULL);
	put16(fixture->entries + 8, 0);
	fixture->entries[10] = 0;
	fixture->entries[11] = 127;
	if (count == 2) {
		put64(fixture->entries + 16, 0xc0000000ULL);
		put16(fixture->entries + 24, 0);
		fixture->entries[26] = 128;
		fixture->entries[27] = 255;
	}
	checksum(fixture, length);
}

int
main(void)
{
	struct amd64_acpi_ecam regions[AMD64_ECAM_MAX];
	struct fixture fixture;
	unsigned count = 99;
	initialize(&fixture, 2);
	assert(amd64_acpi_parse_mcfg(&fixture, sizeof(fixture), regions,
	    &count) == HAL_OK);
	assert(count == 2 && regions[0].address == (paddr_t)0xb0000000U);
	assert(regions[1].start_bus == 128 && regions[1].end_bus == 255);
	fixture.entries[26] = 127;
	checksum(&fixture, 76);
	assert(amd64_acpi_parse_mcfg(&fixture, sizeof(fixture), regions,
	    &count) == HAL_ERR_INVALID);
	initialize(&fixture, 1);
	fixture.header[9]++;
	assert(amd64_acpi_parse_mcfg(&fixture, sizeof(fixture), regions,
	    &count) == HAL_ERR_INVALID);
	initialize(&fixture, 1);
	put64(fixture.entries, 0xb0001000ULL);
	checksum(&fixture, 60);
	assert(amd64_acpi_parse_mcfg(&fixture, sizeof(fixture), regions,
	    &count) == HAL_ERR_INVALID);
	initialize(&fixture, 1);
	assert(amd64_acpi_parse_mcfg(&fixture, 50, regions, &count) ==
	    HAL_ERR_INVALID);
	return 0;
}
