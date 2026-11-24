/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include <hal/hal.h>
#include "asm.h"
#include "bsp.h"
#include "defs.h"
#include "watch.h"

/*
 * ARMv8.5-RNG system register encoding and ID_AA64ISAR0_EL1 field position.
 *
 * Values referenced: Arm Architecture Reference Manual for A-profile
 * architecture (DDI 0487), ID_AA64ISAR0_EL1 (RNDR field at bits 60-63)
 * and RNDR (S3_3_C2_C4_0, NZCV.Z clear on success).
 */
#define ARM64_ISAR0_RNDR_SHIFT		60U
#define ARM64_RNDR_RETRIES		10U

/*
 * Broadcom RNG200 register offsets and control/status bits.
 *
 * Values referenced: Linux v6.12 drivers/char/hw_random/iproc-rng200.c
 * (register offsets 0x00, 0x04, 0x08, 0x18, 0x20, 0x24 and bit masks).
 */
#define RNG200_CTRL_OFFSET		0x00U
#define RNG200_CTRL_RBGEN_MASK		0x00001fffU
#define RNG200_CTRL_RBGEN_ENABLE	0x00000001U
#define RNG200_SOFT_RESET_OFFSET	0x04U
#define RNG200_SOFT_RESET_BIT		0x00000001U
#define RNG200_RBG_SOFT_RESET_OFFSET	0x08U
#define RNG200_RBG_SOFT_RESET_BIT	0x00000001U
#define RNG200_INT_STATUS_OFFSET	0x18U
#define RNG200_INT_MASTER_FAIL		0x80000000U
#define RNG200_INT_NIST_FAIL		0x00000020U
#define RNG200_FIFO_DATA_OFFSET		0x20U
#define RNG200_FIFO_COUNT_OFFSET	0x24U
#define RNG200_FIFO_COUNT_MASK		0x000000ffU
#define RNG200_POLL_LIMIT		100000U

static int fdt_seed_consumed;

static void *(*alloc_fn)(size_t);
static void (*free_fn)(void *);

int
hal_strlen(
	const char *s)
{
	int n = 0;
	while (s[n] != '\0')
		n++;
	return n;
}

void *
hal_memset(
	void *p,
	int c,
	size_t n)
{
	uint8_t *d = p;
	while (n-- != 0)
		*d++ = (uint8_t)c;
	return p;
}

void *
hal_memset16(
	uint16_t *p,
	uint16_t c,
	size_t n)
{
	uint16_t *d = p;
	while (n-- != 0)
		*d++ = c;
	return p;
}

void *
hal_memset32(
	uint32_t *p,
	uint32_t c,
	size_t n)
{
	uint32_t *d = p;
	while (n-- != 0)
		*d++ = c;
	return p;
}

void *
hal_memcpy(
	void *d0,
	const void *s0,
	size_t n)
{
	uint8_t *d = d0;
	const uint8_t *s = s0;
	while (n-- != 0)
		*d++ = *s++;
	return d0;
}

void hal_set_allocator(void *(*a)(size_t), void (*f)(void *))
{
	if (a == NULL || f == NULL || alloc_fn != NULL || free_fn != NULL)
		HAL_FATAL("hal_set_allocator must be called exactly once");
	alloc_fn = a; free_fn = f;
}

void *
hal_malloc(
	size_t n)
{
	if (alloc_fn == NULL)
		HAL_FATAL("allocator unset");
	return alloc_fn(n);
}

void
hal_free(
	void *p)
{
	if (free_fn == NULL)
		HAL_FATAL("allocator unset");
	free_fn(p);
}

int
hal_putchar(
	int c)
{
	hal_putc(c);
	return c;
}

/* Writes one terminated string through the early console. */
static void
cons_puts(
	const char *string)
{
	/* Ignores a missing input string. */
	if (string == NULL)
		return;

	/* Emits every byte in order. */
	while (*string != '\0')
		hal_putc((unsigned char)*string++);
}

int
hal_puts(
	const char *s)
{
	cons_puts(s);
	return 0;
}

static void
put_u64(
	uint64_t value,
	unsigned base,
	int width)
{
	char b[24]; int n = 0;

	do {
		unsigned d = (unsigned)(value % base);
		b[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
		value /= base;
	} while (value != 0);

	while (width-- > n)
		hal_putc('0');

	while (n != 0)
		hal_putc(b[--n]);
}

int hal_printf(const char *fmt, ...)
{
	__builtin_va_list ap;

	__builtin_va_start(ap, fmt);

	while (*fmt != '\0') {
		int width = 0;
		int long_arg = 0;

		if (*fmt != '%') {
			hal_putc(*fmt++);
			continue;
		}

		fmt++;

		if (*fmt == '0')
			fmt++;

		while (*fmt >= '0' && *fmt <= '9')
			width = width * 10 + (*fmt++ - '0');

		if (*fmt == 'l') {
			long_arg = 1;
			fmt++;
			if (*fmt == 'l') fmt++;
		}

		switch (*fmt++) {
		case 'c':
			hal_putc(__builtin_va_arg(ap, int));
			break;
		case 's': {
			const char *s = __builtin_va_arg(ap, const char *);
			cons_puts(s ? s : "(null)");
			break;
		}
		case 'u':
			put_u64(long_arg ?
				__builtin_va_arg(ap, uint64_t) :
				__builtin_va_arg(ap, uint32_t),
				10, width);
			break;
		case 'x':
			put_u64(long_arg ?
				__builtin_va_arg(ap, uint64_t) :
				__builtin_va_arg(ap, uint32_t),
				16, width);
			break;
		case 'p':
			put_u64((uintptr_t)__builtin_va_arg(ap, void *), 16, 16);
			break;
		case '%':
			hal_putc('%');
			break;
		default:
			hal_putc('?');
			break;
		}
	}

	__builtin_va_end(ap);

	return 0;
}

/*
 * Stops the calling CPU on a failed assertion.
 *
 * The console may have nowhere to print on a board whose screen a display
 * driver owns, so the stall watch draws the stop on the screen as well.
 */
void
hal_assert(
	const char *f,
	int l,
	const char *e)
{
	/* Prints the failed assertion where the console can. */
	hal_printf("assert: %s:%u: %s\n", f, (uint32_t)l, e);

	/* Draws it where a photograph of the screen reads it. */
	arm64_watch_fatal(f, l, e);

	/* Halts with interrupts off for good. */
	arm64_irq_mask();
	for (;;)
		arm64_wfe();
}

/*
 * Stops the calling CPU on a fatal error.
 *
 * As for hal_assert(), the stall watch draws the stop on the screen.
 */
void
hal_fatal(
	const char *f,
	int l,
	const char *s)
{
	/* Prints the site and the message where the console can. */
	hal_printf("fatal: %s:%u: %s\n", f, (uint32_t)l, s);

	/* Draws them where a photograph of the screen reads them. */
	arm64_watch_fatal(f, l, s);

	/* Halts with interrupts off for good. */
	arm64_irq_mask();
	for (;;)
		arm64_wfe();
}

/*
 * Fills a buffer using the ARMv8.5 RNDR instruction when implemented.
 *
 * Procedure referenced: Arm Architecture Reference Manual for A-profile
 * architecture (DDI 0487), ID_AA64ISAR0_EL1.RNDR feature check and RNDR
 * NZCV.Z retry loop.
 */
static bool
fill_from_rndr(
	uint8_t *bytes,
	size_t size)
{
	uint64_t isar0;
	uint64_t value;
	uint32_t ready;
	unsigned attempt;
	size_t chunk;

	/* Checks ID_AA64ISAR0_EL1.RNDR (bits 60-63) for hardware support. */
	__asm__ volatile("mrs %0, id_aa64isar0_el1" : "=r"(isar0));
	if (((isar0 >> ARM64_ISAR0_RNDR_SHIFT) & 0xfU) == 0U)
		return false;

	/* Reads 64-bit words from RNDR (S3_3_C2_C4_0) with bounded retries. */
	while (size != 0U) {
		value = 0;
		ready = 0;
		for (attempt = 0; attempt < ARM64_RNDR_RETRIES && ready == 0U; attempt++) {
			__asm__ volatile(
			    "mrs %0, S3_3_C2_C4_0\n\t"
			    "cset %w1, ne"
			    : "=r"(value), "=r"(ready)
			    :
			    : "cc");
		}
		if (ready == 0U)
			return false;

		chunk = size < sizeof(value) ? size : sizeof(value);
		hal_memcpy(bytes, &value, chunk);
		bytes += chunk;
		size -= chunk;
	}

	/* Succeeded. */
	return true;
}

/*
 * Restarts the Broadcom RNG200 block after a health or NIST failure.
 *
 * Procedure referenced: Linux v6.12 drivers/char/hw_random/iproc-rng200.c
 * (iproc_rng200_enable_set and iproc_rng200_restart).
 */
static void
rng200_restart(
	volatile uint8_t *base)
{
	uint32_t value;

	/* Disables the generator and clears all pending status bits. */
	value = hal_mmio_read32(base + RNG200_CTRL_OFFSET);
	value &= ~RNG200_CTRL_RBGEN_MASK;
	hal_mmio_write32(base + RNG200_CTRL_OFFSET, value);
	hal_mmio_write32(base + RNG200_INT_STATUS_OFFSET, 0xffffffffU);

	/* Pulses both RBG and RNG soft resets, then re-enables RBGEN. */
	value = hal_mmio_read32(base + RNG200_RBG_SOFT_RESET_OFFSET);
	hal_mmio_write32(base + RNG200_RBG_SOFT_RESET_OFFSET, value | RNG200_RBG_SOFT_RESET_BIT);
	value = hal_mmio_read32(base + RNG200_SOFT_RESET_OFFSET);
	hal_mmio_write32(base + RNG200_SOFT_RESET_OFFSET, value | RNG200_SOFT_RESET_BIT);

	value = hal_mmio_read32(base + RNG200_SOFT_RESET_OFFSET);
	hal_mmio_write32(base + RNG200_SOFT_RESET_OFFSET, value & ~RNG200_SOFT_RESET_BIT);
	value = hal_mmio_read32(base + RNG200_RBG_SOFT_RESET_OFFSET);
	hal_mmio_write32(base + RNG200_RBG_SOFT_RESET_OFFSET, value & ~RNG200_RBG_SOFT_RESET_BIT);

	value = hal_mmio_read32(base + RNG200_CTRL_OFFSET);
	value &= ~RNG200_CTRL_RBGEN_MASK;
	value |= RNG200_CTRL_RBGEN_ENABLE;
	hal_mmio_write32(base + RNG200_CTRL_OFFSET, value);
}

/*
 * Fills a buffer from the Broadcom RNG200 peripheral when mapped.
 *
 * Procedure referenced: Linux v6.12 drivers/char/hw_random/iproc-rng200.c
 * (iproc_rng200_init and iproc_rng200_read).
 */
static bool
fill_from_rng200(
	uint8_t *bytes,
	size_t size)
{
	const struct arm64_fdt_info *info;
	volatile uint8_t *base;
	uint64_t irq_state;
	uint32_t ctrl;
	uint32_t status;
	uint32_t word;
	unsigned resets;
	unsigned polls;
	size_t chunk;

	info = arm64_boot_info();
	if (info == NULL || info->rng_base == 0ULL)
		return false;

	base = (volatile uint8_t *)(ARM64_DIRECT_BASE + info->rng_base);
	irq_state = arm64_irq_save();

	/* Ensures the random bit generator is enabled. */
	ctrl = hal_mmio_read32(base + RNG200_CTRL_OFFSET);
	if ((ctrl & RNG200_CTRL_RBGEN_MASK) == 0U) {
		ctrl |= RNG200_CTRL_RBGEN_ENABLE;
		hal_mmio_write32(base + RNG200_CTRL_OFFSET, ctrl);
	}

	resets = 0U;
	polls = 0U;
	while (size != 0U) {
		status = hal_mmio_read32(base + RNG200_INT_STATUS_OFFSET);
		if ((status & (RNG200_INT_MASTER_FAIL | RNG200_INT_NIST_FAIL)) != 0U) {
			if (resets >= 1U) {
				arm64_irq_restore(irq_state);
				return false;
			}
			rng200_restart(base);
			resets++;
			continue;
		}

		if ((hal_mmio_read32(base + RNG200_FIFO_COUNT_OFFSET) & RNG200_FIFO_COUNT_MASK) == 0U) {
			polls++;
			if (polls >= RNG200_POLL_LIMIT) {
				arm64_irq_restore(irq_state);
				return false;
			}
			continue;
		}

		word = hal_mmio_read32(base + RNG200_FIFO_DATA_OFFSET);
		chunk = size < sizeof(word) ? size : sizeof(word);
		hal_memcpy(bytes, &word, chunk);
		bytes += chunk;
		size -= chunk;
		polls = 0U;
	}

	arm64_irq_restore(irq_state);

	/* Succeeded. */
	return true;
}

/*
 * Consumes a one-time boot seed from /chosen/rng-seed when large enough.
 */
static bool
fill_from_fdt_seed(
	uint8_t *bytes,
	size_t size)
{
	const struct arm64_fdt_info *info;
	uintptr_t fdt_phys;
	const uint8_t *seed;
	uint64_t irq_state;

	info = arm64_boot_info();
	fdt_phys = arm64_boot_fdt_phys();
	if (info == NULL || fdt_phys == 0U)
		return false;
	if (info->rng_seed_offset == 0U || info->rng_seed_length < size)
		return false;

	irq_state = arm64_irq_save();
	if (fdt_seed_consumed != 0) {
		arm64_irq_restore(irq_state);
		return false;
	}
	fdt_seed_consumed = 1;
	arm64_irq_restore(irq_state);

	seed = (const uint8_t *)(ARM64_DIRECT_BASE + fdt_phys + info->rng_seed_offset);
	hal_memcpy(bytes, seed, size);

	/* Succeeded. */
	return true;
}

/*
 * Fills a buffer from an architectural, board, or boot entropy source.
 */
bool
hal_entropy_fill(
	void *buffer,
	size_t size)
{
	uint8_t *bytes;

	if (size == 0U)
		return true;
	if (buffer == NULL)
		return false;

	bytes = buffer;
	if (fill_from_rndr(bytes, size))
		return true;
	if (fill_from_rng200(bytes, size))
		return true;
	if (fill_from_fdt_seed(bytes, size))
		return true;

	/* No entropy source is available on this platform. */
	return false;
}
