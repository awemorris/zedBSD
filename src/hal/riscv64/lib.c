/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv64 HAL's C runtime: memory helpers, the early console and its
 * formatted output, and the stops on a failed check.
 *
 * The early console writes each character to the firmware's console and
 * to the board's screen, until the kernel publishes its own console;
 * from then on every character goes there.
 */

#include <hal/hal.h>
#include "asm.h"
#include "bootinfo.h"
#include "cons.h"
#include "defs.h"
#include "fdt.h"
#include "sbi.h"

/*
 * RISC-V Zkr entropy source CSR encoding and status field values.
 *
 * Values referenced: RISC-V Cryptography Extensions Volume I (Scalar &
 * Entropy Source Instructions), seed CSR (0x015), OPST field (bits 30-31:
 * BIST=0, WAIT=1, ES16=2, DEAD=3) and 16-bit entropy payload (bits 0-15).
 */
#define RISCV64_SEED_CSR_OPST_SHIFT	30U
#define RISCV64_SEED_OPST_ES16		2U
#define RISCV64_SEED_OPST_DEAD		3U
#define RISCV64_SEED_RETRIES		1024U

/*
 * StarFive JH7110 STG clock/reset controller and TRNG register definitions.
 *
 * Values referenced:
 * Linux v6.12 arch/riscv/boot/dts/starfive/jh7110.dtsi (stgcrg@10230000);
 * Linux v6.12 include/dt-bindings/clock/starfive,jh7110-crg.h
 * (JH7110_STGCLK_SEC_AHB=15, JH7110_STGCLK_SEC_MISC_AHB=16);
 * Linux v6.12 include/dt-bindings/reset/starfive,jh7110-crg.h
 * (JH7110_STGRST_SEC_AHB=3);
 * StarFive JH7110 TRM Preliminary V2, Tables 2-277 and 2-278 (STG reset
 * assert 0x74 and status 0x78);
 * Linux v6.12 drivers/char/hw_random/jh7110-trng.c (TRNG register offsets
 * 0x00, 0x04, 0x08, 0x10, 0x14, 0x20 and control/status bits).
 */
#define JH7110_STGCRG_BASE		0x10230000ULL
#define JH7110_STGCRG_SIZE		0x10000ULL
#define JH7110_STGCLK_SEC_AHB_OFF	(15U * 4U)
#define JH7110_STGCLK_SEC_MISC_OFF	(16U * 4U)
#define JH7110_CRG_CLK_ENABLE		0x80000000U
#define JH7110_STG_RESET_ASSERT_OFF	0x74U
#define JH7110_STG_RESET_STATUS_OFF	0x78U
#define JH7110_STGRST_SEC_AHB_BIT	(1U << 3)

#define JH7110_TRNG_CTRL		0x00U
#define JH7110_TRNG_STAT		0x04U
#define JH7110_TRNG_MODE		0x08U
#define JH7110_TRNG_IE			0x10U
#define JH7110_TRNG_ISTAT		0x14U
#define JH7110_TRNG_RAND0		0x20U

#define JH7110_TRNG_CMD_GEN		0x1U
#define JH7110_TRNG_CMD_RESEED		0x2U
#define JH7110_TRNG_STAT_SEEDED		(1U << 9)
#define JH7110_TRNG_STAT_GENERATING	(1U << 30)
#define JH7110_TRNG_STAT_SEEDING	(1U << 31)
#define JH7110_TRNG_MODE_R256		(1U << 3)
#define JH7110_TRNG_ISTAT_RAND_RDY	(1U << 0)
#define JH7110_TRNG_ISTAT_SEED_DONE	(1U << 1)
#define JH7110_TRNG_ISTAT_LFSR_LOCKUP	(1U << 4)
#define JH7110_TRNG_WORDS		8U
#define JH7110_TRNG_POLL_LIMIT		200000U

static int trng_initialized;
static int fdt_seed_consumed;

static void cons_puts(const char *string);
static void put_number(uint64_t value, unsigned base, int width);

/*
 * Reports the length of a terminated string.
 */
int
hal_strlen(
	const char *s)
{
	int length;

	/* Counts up to the terminator. */
	length = 0;
	while (s[length] != '\0')
		length++;

	/* Succeeded: the length. */
	return length;
}

/*
 * Fills bytes with a value.
 */
void *
hal_memset(
	void *s,
	int c,
	size_t n)
{
	uint8_t *destination;
	size_t index;

	/* Stores the byte n times. */
	destination = s;
	for (index = 0; index < n; index++)
		destination[index] = (uint8_t)c;

	/* Succeeded: the destination. */
	return s;
}

/*
 * Fills 16-bit words with a value.
 */
void *
hal_memset16(
	uint16_t *s,
	uint16_t c,
	size_t n)
{
	size_t index;

	/* Stores the word n times. */
	for (index = 0; index < n; index++)
		s[index] = c;

	/* Succeeded: the destination. */
	return s;
}

/*
 * Fills 32-bit words with a value.
 */
void *
hal_memset32(
	uint32_t *s,
	uint32_t c,
	size_t n)
{
	size_t index;

	/* Stores the word n times. */
	for (index = 0; index < n; index++)
		s[index] = c;

	/* Succeeded: the destination. */
	return s;
}

/*
 * Copies bytes between areas that do not overlap.
 */
void *
hal_memcpy(
	void *dest,
	const void *src,
	size_t n)
{
	uint8_t *destination;
	const uint8_t *source;
	size_t index;

	/* Copies byte by byte, from the start. */
	destination = dest;
	source = src;
	for (index = 0; index < n; index++)
		destination[index] = source[index];

	/* Succeeded: the destination. */
	return dest;
}

/*
 * Writes one character to the early console, or to the kernel's console
 * once the kernel has published one.
 */
void
hal_putc(
	int c)
{
	void (*kernel_output)(int character);

	/* Delegates to the kernel console after the handover. */
	kernel_output = __atomic_load_n(&kernel_putc, __ATOMIC_ACQUIRE);
	if (kernel_output != NULL) {
		kernel_output(c);
		return;
	}

	/* The firmware's console, which ends its lines itself. */
	riscv64_sbi_putchar(c);

	/* And the early console, which draws on the board's screen once it has one. */
	riscv64_cons_putc(c);
}

/*
 * Writes one character, as the C library's putchar does.
 */
int
hal_putchar(
	int c)
{
	/* The early console takes it. */
	hal_putc(c);

	/* Succeeded: the character written. */
	return c;
}

/*
 * Writes a terminated string.
 */
int
hal_puts(
	const char *s)
{
	/* Every byte in order. */
	cons_puts(s);

	/* Succeeded. */
	return 0;
}

/*
 * Writes formatted output: %c, %s, %u, %x, %p and %%, with an optional
 * zero-padded width and l or ll for a 64-bit argument.
 */
int
hal_printf(
	const char *format,
	...)
{
	__builtin_va_list arguments;
	const char *string;
	uint64_t value;
	int character;
	int width;
	int long_argument;
	char conversion;

	/* Walks the format, writing literal characters and converting the rest. */
	__builtin_va_start(arguments, format);
	while (*format != '\0') {
		/* A literal character. */
		if (*format != '%') {
			hal_putc(*format);
			format++;
			continue;
		}

		/* Steps past the percent sign. */
		format++;

		/* Skips the optional zero; every width pads with zeros. */
		if (*format == '0')
			format++;

		/* Reads the width's digits. */
		width = 0;
		while (*format >= '0' && *format <= '9') {
			width = width * 10 + (*format - '0');
			format++;
		}

		/* A lowercase l asks for a 64-bit argument. */
		long_argument = 0;
		if (*format == 'l') {
			long_argument = 1;
			format++;

			/* A second l asks for the same. */
			if (*format == 'l')
				format++;
		}

		/* A format that ends inside a conversion ends the output. */
		conversion = *format;
		if (conversion == '\0')
			break;
		format++;

		/* Converts the argument as the conversion character asks. */
		switch (conversion) {
		case 'c':
			/* One character. */
			character = __builtin_va_arg(arguments, int);
			hal_putc(character);
			break;
		case 's':
			/* A string, with a missing one spelled out. */
			string = __builtin_va_arg(arguments, const char *);
			if (string == NULL)
				string = "(null)";

			/* Writes the string. */
			cons_puts(string);
			break;
		case 'u':
		case 'x':
			/* Takes the argument at its width. */
			if (long_argument) {
				value = __builtin_va_arg(arguments, uint64_t);
			} else {
				value = __builtin_va_arg(arguments, uint32_t);
			}

			/* Spells the number in decimal or hexadecimal. */
			if (conversion == 'u') {
				put_number(value, 10, width);
			} else {
				put_number(value, 16, width);
			}

			break;
		case 'p':
			/* A pointer, as sixteen hexadecimal digits. */
			value = (uintptr_t)__builtin_va_arg(arguments, void *);
			put_number(value, 16, 16);
			break;
		case '%':
			/* A percent sign. */
			hal_putc('%');
			break;
		default:
			/* A conversion this printf does not know. */
			hal_putc('?');
			break;
		}
	}

	/* Ends the reading of the arguments. */
	__builtin_va_end(arguments);

	/* Succeeded. */
	return 0;
}

/*
 * Stops the calling hart on a failed assertion.
 */
void
hal_assert(
	const char *file,
	int line,
	const char *exp)
{
	/* Prints the failed assertion. */
	hal_printf("assert: %s:%u: %s\n", file, (uint32_t)line, exp);

	/* Halts with interrupts off for good. */
	riscv64_halt_forever();
}

/*
 * Stops the calling hart on a fatal error.
 */
void
hal_fatal(
	const char *file,
	int line,
	const char *s)
{
	/* Prints the site and the message. */
	hal_printf("fatal: %s:%u: %s\n", file, (uint32_t)line, s);

	/* Halts with interrupts off for good. */
	riscv64_halt_forever();
}

/*
 * Fills a buffer from the RISC-V Zkr seed CSR when the boot hart has Zkr.
 *
 * Procedure referenced: RISC-V Cryptography Extensions Volume I (Scalar &
 * Entropy Source Instructions), section on polling the seed CSR (csrrw
 * with rd, seed, zero) and checking OPST == ES16.
 */
static bool
fill_from_zkr(
	uint8_t *bytes,
	size_t size)
{
	const struct riscv64_fdt_info *info;
	uint64_t boot_hart;
	uint64_t seed_val;
	uint32_t opst;
	uint16_t sample;
	unsigned index;
	unsigned retries;
	int has_zkr;

	info = riscv64_boot_fdt();
	if (info == NULL)
		return false;

	boot_hart = riscv64_boot_hart();
	has_zkr = 0;
	for (index = 0U; index < info->cpu_count; index++) {
		if (info->cpus[index].hart_id == boot_hart &&
		    (info->cpus[index].extensions & RISCV64_FDT_EXT_ZKR) != 0U) {
			has_zkr = 1;
			break;
		}
	}
	if (has_zkr == 0)
		return false;

	while (size != 0U) {
		retries = 0U;
		for (;;) {
			__asm__ volatile("csrrw %0, 0x015, zero" : "=r"(seed_val));
			opst = (uint32_t)((seed_val >> RISCV64_SEED_CSR_OPST_SHIFT) & 0x3U);
			if (opst == RISCV64_SEED_OPST_ES16)
				break;
			if (opst == RISCV64_SEED_OPST_DEAD)
				return false;
			retries++;
			if (retries >= RISCV64_SEED_RETRIES)
				return false;
		}

		sample = (uint16_t)(seed_val & 0xffffU);
		*bytes++ = (uint8_t)(sample & 0xffU);
		size--;
		if (size != 0U) {
			*bytes++ = (uint8_t)((sample >> 8) & 0xffU);
			size--;
		}
	}

	/* Succeeded. */
	return true;
}

/*
 * Polls the JH7110 TRNG status register until generation and seeding finish.
 *
 * Procedure referenced: Linux v6.12 drivers/char/hw_random/jh7110-trng.c
 * (starfive_trng_wait_idle).
 */
static bool
jh7110_trng_wait_idle(
	volatile uint8_t *base)
{
	unsigned poll;
	uint32_t stat;

	for (poll = 0U; poll < JH7110_TRNG_POLL_LIMIT; poll++) {
		stat = hal_mmio_read32(base + JH7110_TRNG_STAT);
		if ((stat & (JH7110_TRNG_STAT_GENERATING | JH7110_TRNG_STAT_SEEDING)) == 0U)
			return true;
	}
	return false;
}

/*
 * Enables the JH7110 security AHB clocks, releases reset, and seeds the TRNG.
 *
 * Procedure referenced: Linux v6.12 drivers/char/hw_random/jh7110-trng.c
 * (starfive_trng_init) and OpenBSD sys/dev/fdt/stfclock.c (STG clock gate
 * and reset deassert).
 */
static bool
jh7110_trng_prepare(
	volatile uint8_t *base)
{
	volatile uint8_t *stgcrg;
	uint32_t val;
	unsigned poll;

	if (trng_initialized != 0)
		return true;

	stgcrg = (volatile uint8_t *)(uintptr_t)(RISCV64_DEVICE_BASE + JH7110_STGCRG_BASE);
	val = hal_mmio_read32(stgcrg + JH7110_STGCLK_SEC_AHB_OFF);
	hal_mmio_write32(stgcrg + JH7110_STGCLK_SEC_AHB_OFF, val | JH7110_CRG_CLK_ENABLE);
	val = hal_mmio_read32(stgcrg + JH7110_STGCLK_SEC_MISC_OFF);
	hal_mmio_write32(stgcrg + JH7110_STGCLK_SEC_MISC_OFF, val | JH7110_CRG_CLK_ENABLE);

	val = hal_mmio_read32(stgcrg + JH7110_STG_RESET_ASSERT_OFF);
	hal_mmio_write32(stgcrg + JH7110_STG_RESET_ASSERT_OFF, val & ~JH7110_STGRST_SEC_AHB_BIT);
	for (poll = 0U; poll < JH7110_TRNG_POLL_LIMIT; poll++) {
		if ((hal_mmio_read32(stgcrg + JH7110_STG_RESET_STATUS_OFF) &
		     JH7110_STGRST_SEC_AHB_BIT) != 0U)
			break;
	}
	if (poll == JH7110_TRNG_POLL_LIMIT)
		return false;

	/* Masks interrupts and clears pending status for polling operation. */
	hal_mmio_write32(base + JH7110_TRNG_IE, 0U);
	val = hal_mmio_read32(base + JH7110_TRNG_ISTAT);
	hal_mmio_write32(base + JH7110_TRNG_ISTAT, val);

	/* Selects 256-bit PRNG mode and triggers an initial reseed if needed. */
	val = hal_mmio_read32(base + JH7110_TRNG_MODE);
	hal_mmio_write32(base + JH7110_TRNG_MODE, val | JH7110_TRNG_MODE_R256);

	if ((hal_mmio_read32(base + JH7110_TRNG_STAT) & JH7110_TRNG_STAT_SEEDED) == 0U) {
		if (!jh7110_trng_wait_idle(base))
			return false;
		hal_mmio_write32(base + JH7110_TRNG_CTRL, JH7110_TRNG_CMD_RESEED);
		for (poll = 0U; poll < JH7110_TRNG_POLL_LIMIT; poll++) {
			if ((hal_mmio_read32(base + JH7110_TRNG_ISTAT) &
			     JH7110_TRNG_ISTAT_SEED_DONE) != 0U)
				break;
		}
		if (poll == JH7110_TRNG_POLL_LIMIT)
			return false;
		hal_mmio_write32(base + JH7110_TRNG_ISTAT, JH7110_TRNG_ISTAT_SEED_DONE);
	}

	trng_initialized = 1;
	return true;
}

/*
 * Fills a buffer from the StarFive JH7110 TRNG when present in the tree.
 *
 * Procedure referenced: Linux v6.12 drivers/char/hw_random/jh7110-trng.c
 * (starfive_trng_read).
 */
static bool
fill_from_jh7110_trng(
	uint8_t *bytes,
	size_t size)
{
	const struct riscv64_fdt_info *info;
	volatile uint8_t *base;
	bool irq_state;
	uint32_t word;
	unsigned poll;
	unsigned idx;
	size_t chunk;

	info = riscv64_boot_fdt();
	if (info == NULL || info->trng_base == 0ULL)
		return false;
	if (info->trng_base >= RISCV64_DEVICE_LIMIT - 0x100U)
		return false;

	base = (volatile uint8_t *)(uintptr_t)(RISCV64_DEVICE_BASE + info->trng_base);
	irq_state = riscv64_irq_save();

	if (!jh7110_trng_prepare(base)) {
		riscv64_irq_restore(irq_state);
		return false;
	}

	while (size != 0U) {
		if (!jh7110_trng_wait_idle(base)) {
			riscv64_irq_restore(irq_state);
			return false;
		}
		if ((hal_mmio_read32(base + JH7110_TRNG_ISTAT) &
		     JH7110_TRNG_ISTAT_LFSR_LOCKUP) != 0U) {
			hal_mmio_write32(base + JH7110_TRNG_ISTAT,
					 JH7110_TRNG_ISTAT_LFSR_LOCKUP);
			hal_mmio_write32(base + JH7110_TRNG_CTRL, JH7110_TRNG_CMD_RESEED);
			if (!jh7110_trng_wait_idle(base)) {
				riscv64_irq_restore(irq_state);
				return false;
			}
		}

		hal_mmio_write32(base + JH7110_TRNG_ISTAT, JH7110_TRNG_ISTAT_RAND_RDY);
		hal_mmio_write32(base + JH7110_TRNG_CTRL, JH7110_TRNG_CMD_GEN);
		for (poll = 0U; poll < JH7110_TRNG_POLL_LIMIT; poll++) {
			if ((hal_mmio_read32(base + JH7110_TRNG_ISTAT) &
			     JH7110_TRNG_ISTAT_RAND_RDY) != 0U)
				break;
		}
		if (poll == JH7110_TRNG_POLL_LIMIT) {
			riscv64_irq_restore(irq_state);
			return false;
		}
		hal_mmio_write32(base + JH7110_TRNG_ISTAT, JH7110_TRNG_ISTAT_RAND_RDY);

		for (idx = 0U; idx < JH7110_TRNG_WORDS && size != 0U; idx++) {
			word = hal_mmio_read32(base + JH7110_TRNG_RAND0 + idx * 4U);
			chunk = size < sizeof(word) ? size : sizeof(word);
			hal_memcpy(bytes, &word, chunk);
			bytes += chunk;
			size -= chunk;
		}
	}

	riscv64_irq_restore(irq_state);

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
	const struct riscv64_fdt_info *info;
	uintptr_t fdt_phys;
	const uint8_t *seed;
	bool irq_state;

	info = riscv64_boot_fdt();
	fdt_phys = riscv64_boot_fdt_phys();
	if (info == NULL || fdt_phys == 0U)
		return false;
	if (info->rng_seed_offset == 0U || info->rng_seed_length < size)
		return false;

	irq_state = riscv64_irq_save();
	if (fdt_seed_consumed != 0) {
		riscv64_irq_restore(irq_state);
		return false;
	}
	fdt_seed_consumed = 1;
	riscv64_irq_restore(irq_state);

	seed = (const uint8_t *)(uintptr_t)(RISCV64_DIRECT_BASE + fdt_phys + info->rng_seed_offset);
	hal_memcpy(bytes, seed, size);

	/* Succeeded. */
	return true;
}

/*
 * Fills a buffer from Zkr, the board TRNG, or a boot entropy seed.
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
	if (fill_from_zkr(bytes, size))
		return true;
	if (fill_from_jh7110_trng(bytes, size))
		return true;
	if (fill_from_fdt_seed(bytes, size))
		return true;

	/* Failed: no source. */
	return false;
}

/* Writes one terminated string through the early console. */
static void
cons_puts(
	const char *string)
{
	/* Ignores a missing string. */
	if (string == NULL)
		return;

	/* Emits every byte in order. */
	while (*string != '\0') {
		hal_putc((unsigned char)*string);
		string++;
	}
}

/* Writes a number in a base, zero-padded to a width. */
static void
put_number(
	uint64_t value,
	unsigned base,
	int width)
{
	char digits[24];
	unsigned digit;
	int count;

	/* Collects the digits, least significant first. */
	count = 0;
	do {
		/* Spells the lowest digit, a letter unless it is below ten. */
		digit = (unsigned)(value % base);
		digits[count] = (char)('a' + digit - 10U);
		if (digit < 10U)
			digits[count] = (char)('0' + digit);

		/* Moves on to the next digit. */
		count++;
		value /= base;
	} while (value != 0);

	/* Pads with zeros up to the width. */
	while (width > count) {
		hal_putc('0');
		width--;
	}

	/* Writes the digits most significant first. */
	while (count != 0) {
		count--;
		hal_putc(digits[count]);
	}
}
