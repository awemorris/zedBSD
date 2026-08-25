/* Focused xHCI ring/context arithmetic fixture for ws004-p004. */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

static unsigned
scratchpads(uint32_t hcs2)
{
	return (((hcs2 >> 27) & 31U) << 5) | ((hcs2 >> 21) & 31U);
}

static unsigned
normal_trbs(uint64_t address, size_t length)
{
	unsigned count = 0;
	if (length == 0)
		return 1;
	while (length != 0) {
		size_t chunk = 0x10000U - (size_t)(address & 0xffffU);
		if (chunk > length)
			chunk = length;
		assert(chunk != 0 && chunk <= 0x10000U);
		assert((address & ~0xffffULL) ==
		       ((address + chunk - 1U) & ~0xffffULL));
		address += chunk;
		length -= chunk;
		count++;
	}
	return count;
}

static unsigned
interval(unsigned speed, unsigned value)
{
	unsigned result, microframes;
	if (speed >= 3U) {
		result = value ? value - 1U : 0;
		return result > 15U ? 15U : result;
	}
	microframes = (value ? value : 1U) * 8U;
	for (result = 0; (1U << result) < microframes && result < 15U; result++)
		;
	return result;
}

static unsigned
actual_length(unsigned completion, size_t requested, size_t residual)
{
	assert(completion == 1U || completion == 13U);
	return residual < requested ? (unsigned)(requested - residual) : 0;
}

static unsigned
link_control(unsigned cycle, unsigned preceding_control)
{
	return (6U << 10) | 0x2U | (preceding_control & 0x10U) |
	       (cycle ? 1U : 0U);
}

static int
event_matches(unsigned first, unsigned count, unsigned event_index)
{
	unsigned current = first;
	for (unsigned n = 0; n < count; n++) {
		if (current == event_index)
			return 1;
		current++;
		if (current == 255U)
			current = 0;
	}
	return 0;
}

static int
completion_succeeds(unsigned completion, int input)
{
	return completion == 1U || (completion == 13U && input);
}

static unsigned
root_speed_flag(unsigned xhci_speed)
{
	return xhci_speed == 2U	  ? 0x200U
	       : xhci_speed == 3U ? 0x400U
	       : xhci_speed >= 4U ? 0x800U
				  : 0;
}

static int
cancel_retains_dma(int stop_result)
{
	/* A failed Stop Endpoint cannot prove that DMA fetching ceased. */
	return stop_result != 0;
}

int
main(void)
{
	assert(scratchpads(0) == 0);
	assert(scratchpads((1U << 27) | (3U << 21)) == 35U);
	assert(normal_trbs(0x10000U, 0) == 1U);
	assert(normal_trbs(0x1fff0U, 32U) == 2U);
	assert(normal_trbs(0x20000U, 0x20000U) == 2U);
	assert(interval(3U, 1U) == 0U);
	assert(interval(3U, 16U) == 15U);
	assert(interval(2U, 1U) == 3U);
	assert(actual_length(1U, 4096U, 0) == 4096U);
	assert(actual_length(13U, 64U, 8U) == 56U);
	assert((link_control(1U, 0x10U) & 0x13U) == 0x13U);
	assert((link_control(0U, 0) & 0x13U) == 0x2U);
	assert(event_matches(254U, 2U, 254U));
	assert(event_matches(254U, 2U, 0U));
	assert(!event_matches(254U, 2U, 255U));
	assert(completion_succeeds(1U, 0));
	assert(completion_succeeds(13U, 1));
	assert(!completion_succeeds(13U, 0));
	assert(root_speed_flag(2U) == 0x200U);
	assert(root_speed_flag(3U) == 0x400U);
	assert(root_speed_flag(4U) == 0x800U);
	assert(cancel_retains_dma(42));
	assert(!cancel_retains_dma(0));
	puts("xHCI model test: PASS");
	return 0;
}
