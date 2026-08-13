#include "kern/kmem.h"
#include "kern/swap.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fake_swap { uint8_t pages[3][SWAP_PAGE_SIZE]; int fail; };

static void put32(uint8_t *p, uint32_t value)
{
	p[0] = value;
	p[1] = value >> 8;
	p[2] = value >> 16;
	p[3] = value >> 24;
}

static void make_header(uint8_t *header, uint32_t bytes)
{
	memset(header, 0, ZEDBSD_SWAP_HEADER_SIZE);
	memcpy(header, "ZEDSWAP1", 8);
	put32(header + 8, 1);
	put32(header + 12, ZEDBSD_SWAP_HEADER_SIZE);
	put32(header + 16, SWAP_PAGE_SIZE);
	put32(header + 20, bytes);
	put32(header + 24, bytes / SWAP_PAGE_SIZE - 1U);
	put32(header + 28, swap_header_checksum(header));
}

void *kern_calloc(size_t n, size_t s) { return calloc(n, s); }
void kern_free(void *p) { free(p); }

static int read_page(void *data, uint32_t slot, void *page)
{
	struct fake_swap *fake = data;
	if (fake->fail) return EIO;
	memcpy(page, fake->pages[slot], SWAP_PAGE_SIZE);
	return 0;
}

static int write_page(void *data, uint32_t slot, const void *page)
{
	struct fake_swap *fake = data;
	if (fake->fail) return EIO;
	memcpy(fake->pages[slot], page, SWAP_PAGE_SIZE);
	return 0;
}

int main(void)
{
	static const struct swap_backend_ops ops = {
		.read_page = read_page, .write_page = write_page,
	};
	struct swap_backend backend;
	struct fake_swap fake = { 0 };
	uint8_t input[SWAP_PAGE_SIZE], output[SWAP_PAGE_SIZE];
	uint8_t header[ZEDBSD_SWAP_HEADER_SIZE];
	uint32_t slots[4];
	unsigned i;

	make_header(header, ZEDBSD_SWAP_FILE_MIN_BYTES);
	assert(swap_header_validate(header, ZEDBSD_SWAP_FILE_MIN_BYTES) == 0);
	make_header(header, ZEDBSD_SWAP_FILE_MAX_BYTES);
	assert(swap_header_validate(header, ZEDBSD_SWAP_FILE_MAX_BYTES) == 0);
	assert(swap_header_validate(header, ZEDBSD_SWAP_FILE_MIN_BYTES) == EINVAL);
	header[24] ^= 1;
	assert(swap_header_validate(header, ZEDBSD_SWAP_FILE_MAX_BYTES) == EINVAL);

	for (i = 0; i < sizeof(input); i++) input[i] = (uint8_t)(i * 17U);
	swap_init(&backend);
	assert(swap_activate(&backend, &ops, &fake, SWAP_PAGE_SIZE, 3) == 0);
	swap_set_system_backend(&backend);
	assert(swap_system_backend() == &backend && backend.free_slots == 3);
	assert(swap_alloc_slot(&backend, &slots[0]) == 0 && slots[0] == 0);
	assert(swap_alloc_slot(&backend, &slots[1]) == 0 && slots[1] == 1);
	assert(swap_alloc_slot(&backend, &slots[2]) == 0 && slots[2] == 2);
	assert(swap_alloc_slot(&backend, &slots[3]) == ENOSPC);
	assert(swap_write_page(&backend, slots[1], input) == 0);
	memset(output, 0, sizeof(output));
	assert(swap_read_page(&backend, slots[1], output) == 0);
	assert(!memcmp(input, output, sizeof(input)));
	fake.fail = 1;
	assert(swap_read_page(&backend, slots[1], output) == EIO);
	fake.fail = 0;
	swap_free_slot(&backend, slots[1]);
	assert(swap_alloc_slot(&backend, &slots[3]) == 0 && slots[3] == 1);
	assert(swap_shutdown(&backend) == 0);
	assert(swap_system_backend() == NULL && backend.bitmap == NULL);
	puts("zedBSD swap backend host tests: PASS");
	return 0;
}
