/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The power manager (PMU) of the PowerBook and of QEMU's mac99 with
 * via=pmu: the battery-backed clock, turning the machine off and
 * restarting it.  The hardware layer asks it only with interrupts off and
 * waits for each answer, at boot for the clock and at the end for the
 * power; the kernel's PMU driver (its keyboard and mouse) asks the same
 * way, with interrupts off for each whole request, so on this
 * uniprocessor board the two never interleave.
 *
 * The PMU is reached through a 6522 VIA in the I/O controller, one
 * register every 0x200 bytes.  A request is the command byte, the data
 * (preceded by its length for the commands whose length varies), then
 * the reply (preceded by its length for the commands whose reply varies);
 * every byte goes through the VIA's shift register with a handshake on
 * port B: the host pulls TREQ (bit 4, active low) and waits for the
 * shift to complete (the interrupt flag's SR bit) with the PMU's TACK
 * (bit 3, active low) answering, then lets TREQ go and waits for TACK to
 * go back up.  The shift register shifts out on the external clock for
 * the host's bytes (ACR bits 2 to 4: 111) and in for the PMU's (011).
 *
 * The clock is seconds since 1904-01-01, which is 2082844800 seconds
 * before the Unix epoch; the HAL reads it once at boot and counts on with
 * the time base.
 */

#include <hal/hal.h>
#include "../asm.h"
#include "bsp.h"

/*
 * The VIA's registers, 0x200 bytes apart: port B, its direction, the
 * shift register, ACR, IFR and IER; port B's handshake lines; ACR's shift
 * register modes; the shift register's interrupt flag.
 *
 * Values referenced: the Rockwell R6522 and WDC W65C22 data sheets (the
 * register numbers, ACR bits 2 to 4, IFR and IER bit 2); QEMU 10.0
 * hw/misc/macio/pmu.c (the register stride and the TREQ and TACK bits of
 * the PowerBook's PMU99).
 */
#define VIA_B			0x0000U
#define VIA_DIRB		0x0400U
#define VIA_SR			0x1400U
#define VIA_ACR			0x1600U
#define VIA_IFR			0x1a00U
#define VIA_IER			0x1c00U
#define VIA_SIZE		0x2000U

/* Port B's handshake lines, both active low: TREQ the host's, TACK the PMU's. */
#define VIA_B_TACK		0x08U
#define VIA_B_TREQ		0x10U

/* ACR's shift register mode: its three bits, shift in and shift out on the external clock. */
#define VIA_ACR_SR_MASK		0x1cU
#define VIA_ACR_SR_IN		0x0cU
#define VIA_ACR_SR_OUT		0x1cU

/* IFR's and IER's shift register bit. */
#define VIA_SR_FLAG		0x04U

/*
 * The commands the hardware layer gives: read the clock (no data, four
 * reply bytes), shut down (the four bytes "MATT", one reply byte), reset
 * (nothing either way).
 *
 * Values referenced: QEMU 10.0 hw/misc/macio/pmu.c and
 * include/hw/misc/macio/pmu.h (the command numbers, their data and reply
 * lengths, the shut down signature, the clock's 1904 epoch), as they
 * emulate the PowerBook's PMU99.
 */
#define PMU_READ_RTC		0x38U
#define PMU_SHUTDOWN		0x7eU
#define PMU_RESET		0xd0U

/* The seconds from 1904-01-01 to the Unix epoch. */
#define PMU_RTC_EPOCH_OFFSET	2082844800U

/* How long a handshake step is waited for, in milliseconds. */
#define PMU_STEP_MS		50U

/* How long the machine is given to go off or restart, in milliseconds. */
#define PMU_POWER_WAIT_MS	2000U

/*
 * The VIA's registers, reached in a device window; NULL when the board
 * has no PMU the HAL can reach.  Set once by mac99_pmu_init().
 */
static volatile uint8_t *via;

/*
 * The clock's seconds since the Unix epoch, and the time base when they
 * were read; the seconds are 0 when the clock was not read.  Set once by
 * mac99_pmu_init().
 */
static uint64_t rtc_seconds;
static uint64_t rtc_timebase;

static int request(uint8_t command, const uint8_t *data, unsigned length, uint8_t *reply, unsigned reply_length);
static int send_byte(uint8_t value);
static int receive_byte(uint8_t *value);
static int wait_flag(void);
static int wait_tack_released(void);
static uint8_t via_read(uint32_t offset);
static void via_write(uint32_t offset, uint8_t value);
static void wait_ms(uint32_t milliseconds);

/*
 * Finds the PMU's VIA, readies its handshake lines, and reads the clock.
 */
void
mac99_pmu_init(
	void)
{
	const struct mac99_board_info *board;
	uint8_t reply[4];
	uint32_t seconds;
	uint8_t value;
	void *window;
	bool enabled;
	int error;

	/* A board without a PMU has no clock and no power control here. */
	board = mac99_board();
	if (board->pmu_base == 0) {
		hal_puts("PPC32 PMU NONE\n");
		return;
	}
	error = hal_space_map_device(board->pmu_base, VIA_SIZE, HAL_SPACE_READ | HAL_SPACE_WRITE, &window);
	if (error != HAL_OK) {
		hal_printf("PPC32 PMU %08x UNMAPPED\n", board->pmu_base);
		return;
	}
	via = window;

	/* TREQ an output, high; TACK an input; the shift register's interrupt not to the processor. */
	enabled = ppc32_irq_save();
	value = via_read(VIA_DIRB);
	via_write(VIA_DIRB, (uint8_t)((value | VIA_B_TREQ) & ~VIA_B_TACK));
	via_write(VIA_B, (uint8_t)(via_read(VIA_B) | VIA_B_TREQ));
	via_write(VIA_IER, VIA_SR_FLAG);
	ppc32_irq_restore(enabled);
	if (board->pmu_gpio_interrupt == 0xffffffffU) {
		hal_printf("PPC32 PMU %08x IRQ %u GPIO NONE\n", board->pmu_base, board->pmu_interrupt);
	} else {
		hal_printf("PPC32 PMU %08x IRQ %u GPIO %08x IRQ %u\n", board->pmu_base, board->pmu_interrupt,
			   board->pmu_gpio, board->pmu_gpio_interrupt);
	}

	/* The clock: four bytes, most significant first, seconds since 1904. */
	error = request(PMU_READ_RTC, NULL, 0, reply, sizeof(reply));
	if (error != HAL_OK) {
		hal_puts("PPC32 RTC NO ANSWER\n");
		return;
	}
	seconds = ((uint32_t)reply[0] << 24) | ((uint32_t)reply[1] << 16) | ((uint32_t)reply[2] << 8) | reply[3];
	if (seconds <= PMU_RTC_EPOCH_OFFSET) {
		hal_printf("PPC32 RTC %08x BEFORE 1970\n", seconds);
		return;
	}
	rtc_timebase = ppc32_read_timebase();
	rtc_seconds = seconds - PMU_RTC_EPOCH_OFFSET;
	hal_printf("PPC32 RTC %llu\n", rtc_seconds);
}

/*
 * Asks the PMU to turn the machine off, and gives it time to; returns
 * when there is no PMU or the machine is still on.
 */
void
mac99_pmu_poweroff(
	void)
{
	static const uint8_t signature[4] = { 'M', 'A', 'T', 'T' };
	uint8_t reply;
	int error;

	/* Without a PMU there is nothing to ask. */
	if (via == NULL)
		return;

	/* The command with its signature, then the wait. */
	error = request(PMU_SHUTDOWN, signature, sizeof(signature), &reply, 1);
	if (error != HAL_OK)
		hal_puts("PPC32 PMU SHUTDOWN NO ANSWER\n");
	wait_ms(PMU_POWER_WAIT_MS);
	hal_puts("PPC32 PMU STILL ON\n");
}

/*
 * Asks the PMU to restart the machine, and gives it time to; returns when
 * there is no PMU or the machine did not restart.
 */
void
mac99_pmu_restart(
	void)
{
	int error;

	/* Without a PMU there is nothing to ask. */
	if (via == NULL)
		return;

	/* The command, then the wait. */
	error = request(PMU_RESET, NULL, 0, NULL, 0);
	if (error != HAL_OK)
		hal_puts("PPC32 PMU RESET NO ANSWER\n");
	wait_ms(PMU_POWER_WAIT_MS);
	hal_puts("PPC32 PMU DID NOT RESTART\n");
}

/*
 * Reports the wall-clock time: the PMU's clock read at boot, counted on
 * with the time base.
 */
bool
hal_rtc_read_epoch_time(
	uint64_t *unix_seconds)
{
	const struct mac99_board_info *board;

	/* Refuses a caller with nowhere to put it, or a clock never read. */
	board = mac99_board();
	if (unix_seconds == NULL || rtc_seconds == 0 || board->timebase_frequency == 0)
		return false;

	/* Succeeded: the boot's reading and the whole seconds since. */
	*unix_seconds = rtc_seconds + (ppc32_read_timebase() - rtc_timebase) / board->timebase_frequency;
	return true;
}

/*
 * Gives the PMU one command with its data and takes a reply of a known
 * length, with interrupts off throughout.
 */
static int
request(
	uint8_t command,
	const uint8_t *data,
	unsigned length,
	uint8_t *reply,
	unsigned reply_length)
{
	unsigned index;
	bool enabled;
	int error;

	/* The command and its data, then the reply. */
	enabled = ppc32_irq_save();
	error = send_byte(command);
	for (index = 0; error == HAL_OK && index < length; index++)
		error = send_byte(data[index]);
	for (index = 0; error == HAL_OK && index < reply_length; index++)
		error = receive_byte(&reply[index]);
	ppc32_irq_restore(enabled);
	return error;
}

/* Shifts one byte out to the PMU. */
static int
send_byte(
	uint8_t value)
{
	int error;

	/* Shift out, the byte, then the request. */
	via_write(VIA_ACR, (uint8_t)((via_read(VIA_ACR) & ~VIA_ACR_SR_MASK) | VIA_ACR_SR_OUT));
	via_write(VIA_SR, value);
	via_write(VIA_B, (uint8_t)(via_read(VIA_B) & ~VIA_B_TREQ));
	error = wait_flag();

	/* The request let go, and the PMU's acknowledgement back up. */
	via_write(VIA_B, (uint8_t)(via_read(VIA_B) | VIA_B_TREQ));
	if (error != HAL_OK)
		return error;
	return wait_tack_released();
}

/* Shifts one byte in from the PMU. */
static int
receive_byte(
	uint8_t *value)
{
	int error;

	/* Shift in, the flag cleared by reading the register, then the request. */
	via_write(VIA_ACR, (uint8_t)((via_read(VIA_ACR) & ~VIA_ACR_SR_MASK) | VIA_ACR_SR_IN));
	(void)via_read(VIA_SR);
	via_write(VIA_B, (uint8_t)(via_read(VIA_B) & ~VIA_B_TREQ));
	error = wait_flag();

	/* The request let go, the byte, and the PMU's acknowledgement back up. */
	via_write(VIA_B, (uint8_t)(via_read(VIA_B) | VIA_B_TREQ));
	if (error != HAL_OK)
		return error;
	*value = via_read(VIA_SR);
	return wait_tack_released();
}

/* Waits for the shift register's interrupt flag, and clears it. */
static int
wait_flag(
	void)
{
	uint64_t deadline;
	uint64_t frequency;
	uint64_t now;
	uint8_t flags;

	/* Until the flag rises or the step's time is up. */
	frequency = mac99_board()->timebase_frequency;
	deadline = ppc32_read_timebase() + frequency * PMU_STEP_MS / 1000U;
	for (;;) {
		flags = via_read(VIA_IFR);
		if ((flags & VIA_SR_FLAG) != 0)
			break;
		now = ppc32_read_timebase();
		if (now > deadline)
			return HAL_ERR_TIMEOUT;
	}

	/* Succeeded: the flag, cleared. */
	via_write(VIA_IFR, VIA_SR_FLAG);
	return HAL_OK;
}

/* Waits for the PMU's TACK to go back up. */
static int
wait_tack_released(
	void)
{
	uint64_t deadline;
	uint64_t frequency;
	uint64_t now;
	uint8_t lines;

	/* Until the line is high or the step's time is up. */
	frequency = mac99_board()->timebase_frequency;
	deadline = ppc32_read_timebase() + frequency * PMU_STEP_MS / 1000U;
	for (;;) {
		lines = via_read(VIA_B);
		if ((lines & VIA_B_TACK) != 0)
			break;
		now = ppc32_read_timebase();
		if (now > deadline)
			return HAL_ERR_TIMEOUT;
	}

	/* Succeeded: the PMU is ready for the next byte. */
	return HAL_OK;
}

/* Reads a VIA register. */
static uint8_t
via_read(
	uint32_t offset)
{
	uint8_t value;

	/* One byte, ordered against the accesses around it. */
	value = via[offset];
	__asm__ volatile("eieio" : : : "memory");
	return value;
}

/* Writes a VIA register. */
static void
via_write(
	uint32_t offset,
	uint8_t value)
{
	/* One byte, ordered against the accesses around it. */
	via[offset] = value;
	__asm__ volatile("eieio" : : : "memory");
}

/* Spins for a number of milliseconds on the time base. */
static void
wait_ms(
	uint32_t milliseconds)
{
	uint64_t deadline;
	uint64_t now;

	/* Until the time base passes the deadline. */
	deadline = ppc32_read_timebase() + (uint64_t)mac99_board()->timebase_frequency * milliseconds / 1000U;
	do {
		now = ppc32_read_timebase();
	} while (now < deadline);
}
