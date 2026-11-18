/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The words of the GPU's interrupt ring.
 *
 * The interrupt controller writes every event of every block as one
 * 32-byte entry into a ring in memory, stores where it wrote last in a
 * word of memory, and signals the CPU with a message.  The driver reads
 * the entries up to that point and tells the controller how far it read
 * through a doorbell.  Nothing here touches the hardware, so the same code
 * runs in the kernel and in the host tests.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-hw.h"

/*
 * The fields of an entry: the client (bits 7:0), the source (15:8), the
 * ring (23:16), the context (27:24) and the media-hub bit (31) of the
 * first word; the timestamp's low word and its high 16 bits; the process
 * address-space ID (bits 15:0 of the fourth word); four data words.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_ih.c (amdgpu_ih_decode_iv_helper).
 */
#define FORMAT_ENTRY_BYTE_MASK		0x000000ffU
#define FORMAT_ENTRY_SOURCE_SHIFT	8U
#define FORMAT_ENTRY_RING_SHIFT		16U
#define FORMAT_ENTRY_CONTEXT_SHIFT	24U
#define FORMAT_ENTRY_CONTEXT_MASK	0x0000000fU
#define FORMAT_ENTRY_MEDIA_HUB		0x80000000U
#define FORMAT_ENTRY_STAMP_HIGH_MASK	0x0000ffffU
#define FORMAT_ENTRY_SPACE_MASK		0x0000ffffU

/*
 * The fields of the ring's control word: the ring enable, the ring size
 * (bits 5:1, log2 of the size in dwords), the timestamp enable, the
 * write-pointer copy to memory, the overflow enable, the interrupt enable,
 * snooping of the CPU's caches, the re-arm on a read-pointer write (for a
 * message interrupt), read-only access, the context (bits 27:24), the
 * address space (bits 30:28, 4 for the GPU's own addresses through
 * context 0) and the overflow clear.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/oss/osssys_5_0_0_sh_mask.h
 * (IH_RB_CNTL__RB_ENABLE, __RB_SIZE, __RB_GPU_TS_ENABLE,
 * __WPTR_WRITEBACK_ENABLE, __WPTR_OVERFLOW_ENABLE, __ENABLE_INTR,
 * __MC_SNOOP, __RPTR_REARM, __MC_RO, __MC_VMID, __MC_SPACE,
 * __WPTR_OVERFLOW_CLEAR) and drivers/gpu/drm/amd/amdgpu/navi10_ih.c
 * (navi10_ih_rb_cntl: space 4 for a ring the GPU reaches through its
 * table).
 */
#define FORMAT_RING_ENABLE		0x00000001U
#define FORMAT_RING_SIZE_SHIFT		1U
#define FORMAT_RING_SIZE_MASK		0x0000003eU
#define FORMAT_RING_SIZE_LARGEST	31U
#define FORMAT_RING_STAMPS		0x00000080U
#define FORMAT_RING_POINTER_COPY	0x00000100U
#define FORMAT_RING_OVERFLOW_ENABLE	0x00010000U
#define FORMAT_RING_INTERRUPT		0x00020000U
#define FORMAT_RING_SNOOP		0x00100000U
#define FORMAT_RING_REARM		0x00200000U
#define FORMAT_RING_READ_ONLY		0x00400000U
#define FORMAT_RING_CONTEXT_MASK	0x0f000000U
#define FORMAT_RING_SPACE_MASK		0x70000000U
#define FORMAT_RING_SPACE_TRANSLATED	0x40000000U
#define FORMAT_RING_OVERFLOW_CLEAR	0x80000000U

/*
 * The write pointer: the overflow bit and the byte offset of the next
 * entry (bits 17:2).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/oss/osssys_5_0_0_sh_mask.h
 * (IH_RB_WPTR__RB_OVERFLOW, IH_RB_WPTR__OFFSET).
 */
#define FORMAT_POINTER_OVERFLOW		0x00000001U
#define FORMAT_POINTER_LOW_BITS		0x00000003U

/*
 * The read-pointer doorbell word of the controller: the doorbell's dword
 * index (bits 25:0) and its enable (bit 28); and the host bridge's
 * doorbell range for the controller: the index in bits 11:2 and the size
 * (bits 20:16, 2).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/oss/osssys_5_0_0_sh_mask.h
 * (IH_DOORBELL_RPTR__OFFSET, IH_DOORBELL_RPTR__ENABLE),
 * drivers/gpu/drm/amd/include/asic_reg/nbio/nbio_2_3_sh_mask.h
 * (BIF_IH_DOORBELL_RANGE__OFFSET, BIF_IH_DOORBELL_RANGE__SIZE) and
 * drivers/gpu/drm/amd/amdgpu/nbio_v2_3.c (nbio_v2_3_ih_doorbell_range).
 */
#define FORMAT_DOORBELL_INDEX_MASK	0x03ffffffU
#define FORMAT_DOORBELL_ENABLE		0x10000000U
#define FORMAT_RANGE_INDEX_SHIFT	2U
#define FORMAT_RANGE_INDEX_MASK		0x00000ffcU
#define FORMAT_RANGE_SIZE_MASK		0x001f0000U
#define FORMAT_RANGE_SIZE_TWO		0x00020000U

/*
 * The host bridge's interrupt control word: the dummy-read override (bit
 * 0, clear so a message interrupt needs no dummy read) and the non-snooped
 * request bit (bit 3, clear for a ring in the CPU's coherent memory).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/nbio/nbio_2_3_sh_mask.h
 * (INTERRUPT_CNTL__IH_DUMMY_RD_OVERRIDE, INTERRUPT_CNTL__IH_REQ_NONSNOOP_EN)
 * and drivers/gpu/drm/amd/amdgpu/nbio_v2_3.c (nbio_v2_3_ih_control).
 */
#define FORMAT_CONTROL_DUMMY_OVERRIDE	0x00000001U
#define FORMAT_CONTROL_NOT_SNOOPED	0x00000008U

/*
 * Decodes one 32-byte entry of the interrupt ring.
 */
void
amdrdna_interrupt_decode(
	const uint32_t words[AMDRDNA_INTERRUPT_ENTRY_BYTES / 4U],
	struct amdrdna_interrupt *interrupt)
{
	unsigned index;

	/* The first word: who raised it, for which ring and context. */
	interrupt->client = (uint8_t)(words[0] & FORMAT_ENTRY_BYTE_MASK);
	interrupt->source = (uint8_t)((words[0] >> FORMAT_ENTRY_SOURCE_SHIFT) & FORMAT_ENTRY_BYTE_MASK);
	interrupt->ring = (uint8_t)((words[0] >> FORMAT_ENTRY_RING_SHIFT) & FORMAT_ENTRY_BYTE_MASK);
	interrupt->context = (uint8_t)((words[0] >> FORMAT_ENTRY_CONTEXT_SHIFT) & FORMAT_ENTRY_CONTEXT_MASK);

	/* Whether the media hub stamped the context. */
	interrupt->from_media_hub = false;
	if ((words[0] & FORMAT_ENTRY_MEDIA_HUB) != 0U)
		interrupt->from_media_hub = true;

	/* The timestamp: 48 bits over the second and third words. */
	interrupt->timestamp = words[1];
	interrupt->timestamp |= (uint64_t)(words[2] & FORMAT_ENTRY_STAMP_HIGH_MASK) << 32;

	/* The process address space the event belongs to. */
	interrupt->space_id = (uint16_t)(words[3] & FORMAT_ENTRY_SPACE_MASK);

	/* The four data words. */
	for (index = 0U; index < 4U; index++)
		interrupt->data[index] = words[4U + index];
}

/*
 * Turns the ring's control word into the one that describes a ring of
 * ring_bytes (a power of two) in the GPU's translated address space, with
 * the write pointer copied to memory, overflows detected and cleared,
 * snooped accesses, and re-arming on a read-pointer write when the CPU is
 * signalled by a message.  The ring and its interrupt stay off; the
 * timestamps stay on.
 *
 * Returns 0 with the word, or EINVAL for a size that is not a power of two
 * of at least one entry or that the size field cannot hold.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/navi10_ih.c (navi10_ih_rb_cntl,
 * navi10_ih_enable_ring, navi10_ih_toggle_ring_interrupts).
 */
int
amdrdna_interrupt_ring_word(
	uint32_t old,
	uint32_t ring_bytes,
	bool message,
	uint32_t *word)
{
	uint32_t dword_count;
	uint32_t order;
	uint32_t built;

	/* The size must be a power of two of at least one entry. */
	if (ring_bytes < AMDRDNA_INTERRUPT_ENTRY_BYTES)
		return EINVAL;
	if ((ring_bytes & (ring_bytes - 1U)) != 0U)
		return EINVAL;

	/* The size field holds log2 of the size in dwords. */
	dword_count = ring_bytes / 4U;
	order = 0U;
	while ((1U << order) < dword_count)
		order++;
	if (order > FORMAT_RING_SIZE_LARGEST)
		return EINVAL;

	/* Clears every field the driver sets. */
	built = old;
	built &= ~FORMAT_RING_ENABLE;
	built &= ~FORMAT_RING_INTERRUPT;
	built &= ~FORMAT_RING_SIZE_MASK;
	built &= ~FORMAT_RING_READ_ONLY;
	built &= ~FORMAT_RING_CONTEXT_MASK;
	built &= ~FORMAT_RING_SPACE_MASK;
	built &= ~FORMAT_RING_REARM;

	/* The ring's place and size, the copied pointer, overflow handling and snooping. */
	built |= order << FORMAT_RING_SIZE_SHIFT;
	built |= FORMAT_RING_SPACE_TRANSLATED;
	built |= FORMAT_RING_STAMPS;
	built |= FORMAT_RING_POINTER_COPY;
	built |= FORMAT_RING_OVERFLOW_ENABLE;
	built |= FORMAT_RING_OVERFLOW_CLEAR;
	built |= FORMAT_RING_SNOOP;

	/* A message interrupt is re-armed by the read-pointer write. */
	if (message)
		built |= FORMAT_RING_REARM;

	/* Hands the word to the caller. */
	*word = built;

	/* Succeeded: the control word. */
	return 0;
}

/*
 * Turns the ring's control word into one with the ring and its interrupt
 * on or off, the timestamps on either way.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/navi10_ih.c
 * (navi10_ih_toggle_ring_interrupts).
 */
uint32_t
amdrdna_interrupt_switch_word(
	uint32_t old,
	bool on)
{
	uint32_t word;

	/* The two switches off, the timestamps on. */
	word = old;
	word &= ~FORMAT_RING_ENABLE;
	word &= ~FORMAT_RING_INTERRUPT;
	word |= FORMAT_RING_STAMPS;

	/* The two switches on when asked. */
	if (on) {
		word |= FORMAT_RING_ENABLE;
		word |= FORMAT_RING_INTERRUPT;
	}

	/* Succeeded: the new control word. */
	return word;
}

/*
 * Turns the ring's control word into one with the overflow clear bit set
 * (to acknowledge an overflow) or cleared (so the next one is seen).
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/navi10_ih.c (navi10_ih_get_wptr).
 */
uint32_t
amdrdna_interrupt_overflow_word(
	uint32_t old,
	bool clear)
{
	uint32_t word;

	/* Sets or clears the bit. */
	word = old & ~FORMAT_RING_OVERFLOW_CLEAR;
	if (clear)
		word |= FORMAT_RING_OVERFLOW_CLEAR;

	/* Succeeded: the new control word. */
	return word;
}

/*
 * Makes the controller's read-pointer doorbell word: the doorbell's dword
 * index, enabled.
 */
uint32_t
amdrdna_interrupt_doorbell_word(
	uint32_t doorbell)
{
	uint32_t word;

	/* The index and the enable bit. */
	word = doorbell & FORMAT_DOORBELL_INDEX_MASK;
	word |= FORMAT_DOORBELL_ENABLE;

	/* Succeeded: the doorbell word. */
	return word;
}

/*
 * Turns the host bridge's doorbell range word for the interrupt controller
 * into one that routes two dwords from the doorbell's index to it.
 */
uint32_t
amdrdna_interrupt_doorbell_range_word(
	uint32_t old,
	uint32_t doorbell)
{
	uint32_t word;

	/* Replaces the index and the size. */
	word = old;
	word &= ~FORMAT_RANGE_INDEX_MASK;
	word &= ~FORMAT_RANGE_SIZE_MASK;
	word |= (doorbell << FORMAT_RANGE_INDEX_SHIFT) & FORMAT_RANGE_INDEX_MASK;
	word |= FORMAT_RANGE_SIZE_TWO;

	/* Succeeded: the new range word. */
	return word;
}

/*
 * Turns the host bridge's interrupt control word into one for a message
 * interrupt and a ring in coherent memory: no dummy-read override, snooped
 * requests.
 */
uint32_t
amdrdna_interrupt_control_word(
	uint32_t old)
{
	uint32_t word;

	/* Both bits cleared. */
	word = old;
	word &= ~FORMAT_CONTROL_DUMMY_OVERRIDE;
	word &= ~FORMAT_CONTROL_NOT_SNOOPED;

	/* Succeeded: the new control word. */
	return word;
}

/*
 * Turns a write-pointer word into the byte offset of the next entry in a
 * ring of ring_bytes (a power of two), and says whether the controller
 * reported an overflow.
 */
uint32_t
amdrdna_interrupt_offset(
	uint32_t pointer,
	uint32_t ring_bytes,
	bool *overflowed)
{
	uint32_t offset;

	/* The overflow bit. */
	*overflowed = false;
	if ((pointer & FORMAT_POINTER_OVERFLOW) != 0U)
		*overflowed = true;

	/* The offset inside the ring, without the two flag bits. */
	offset = pointer & (ring_bytes - 1U);
	offset &= ~FORMAT_POINTER_LOW_BITS;

	/* Succeeded: the byte offset. */
	return offset;
}
