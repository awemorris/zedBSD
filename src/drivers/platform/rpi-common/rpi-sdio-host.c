/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Raspberry Pi 4 legacy EMMC controller as an SDIO host.
 *
 * The register layout and the command, data, clock and interrupt
 * procedures are those of the SD Host Controller Simplified Specification
 * (version 3.00).  Two habits of this controller shape the code:
 *
 * - It takes only 32-bit writes.  A register narrower than a word is
 *   changed by writing its whole word: the transfer mode with the command,
 *   the block size with the block count, the clock control with the
 *   timeout and reset bytes, the host control with the power byte.
 * - Two writes closer together than two cycles of a slow bus clock can
 *   lose the first, so at 400 kHz and below every write is followed by a
 *   pause of two bus clock cycles.  The capabilities register's base clock
 *   is not trusted either; the firmware's clock rate is.
 *
 * Commands and data move by PIO, polled with short bounded busy waits
 * (pause_us: real busy waits, not the scheduler's sleep).
 * Only the card's interrupt uses the interrupt line: its handler disarms
 * it, which stops the level the controller raises, and wakes the thread
 * that serves the card, which rearms it afterwards.
 */

#include "drivers/platform/rpi4/rpi4-sdio-host.h"
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/klog.h>
#include <uapi/errno.h>

/* The registers, by their offset in the block. */
#define REG_BLOCK		0x04U	/* block size, block count */
#define REG_ARGUMENT		0x08U
#define REG_COMMAND		0x0cU	/* transfer mode, command */
#define REG_RESPONSE		0x10U
#define REG_DATA_PORT		0x20U
#define REG_PRESENT_STATE	0x24U
#define REG_HOST_CONTROL	0x28U	/* host control 1, power control, block gap, wakeup */
#define REG_CLOCK_CONTROL	0x2cU	/* clock control, timeout control, software reset */
#define REG_INTERRUPT_STATUS	0x30U	/* normal, error */
#define REG_STATUS_ENABLE	0x34U	/* normal, error */
#define REG_SIGNAL_ENABLE	0x38U	/* normal, error */
#define REG_CAPABILITIES	0x40U
#define REG_HOST_VERSION	0xfcU	/* slot interrupt status, host controller version */

/* The present state: a command or the data lines still busy. */
#define PRESENT_COMMAND_INHIBIT	0x00000001U
#define PRESENT_DATA_INHIBIT	0x00000002U

/* The host control 1 byte: four data lines.  The power byte: on at 3.3 V. */
#define HOST_CONTROL_DATA_4BIT	0x02U
#define POWER_VOLTAGE_3V3	0x0eU
#define POWER_ON		0x01U
#define POWER_SHIFT		8U

/*
 * The clock control halfword: the internal clock and when it is stable,
 * the bus clock, and the divider's low eight bits and its two high bits.
 */
#define CLOCK_INTERNAL_ENABLE	0x0001U
#define CLOCK_INTERNAL_STABLE	0x0002U
#define CLOCK_CARD_ENABLE	0x0004U
#define CLOCK_DIVISOR_LOW_SHIFT	8U
#define CLOCK_DIVISOR_HIGH_SHIFT 6U
#define CLOCK_CONTROL_MASK	0x0000ffffU

/* The timeout control byte: the longest data timeout, TMCLK x 2^27. */
#define TIMEOUT_LONGEST		0x0eU
#define TIMEOUT_SHIFT		16U
#define TIMEOUT_MASK		0x00ff0000U

/* The software reset byte: the whole controller, the command line, the data lines. */
#define RESET_SHIFT		24U
#define RESET_ALL		0x01U
#define RESET_COMMAND		0x02U
#define RESET_DATA		0x04U

/* The transfer mode halfword: count the blocks, read from the card, more than one block. */
#define TRANSFER_BLOCK_COUNT	0x0002U
#define TRANSFER_READ		0x0010U
#define TRANSFER_MULTIPLE	0x0020U

/* The command halfword: the response length, the checks, data present, the index. */
#define COMMAND_RESPONSE_48	0x0002U
#define COMMAND_RESPONSE_48_BUSY 0x0003U
#define COMMAND_CHECK_CRC	0x0008U
#define COMMAND_CHECK_INDEX	0x0010U
#define COMMAND_DATA_PRESENT	0x0020U
#define COMMAND_INDEX_SHIFT	8U
#define COMMAND_INDEX_MAX	63U
#define COMMAND_SHIFT		16U

/* The block word: the size in the low halfword, the count in the high one. */
#define BLOCK_COUNT_SHIFT	16U

/* The normal interrupt status bits the host waits for. */
#define STATUS_COMMAND_COMPLETE	0x00000001U
#define STATUS_TRANSFER_COMPLETE 0x00000002U
#define STATUS_WRITE_READY	0x00000010U
#define STATUS_READ_READY	0x00000020U
#define STATUS_CARD_INTERRUPT	0x00000100U
#define STATUS_ERROR		0x00008000U

/* The error interrupt status bits that mean the card did not answer in time. */
#define STATUS_COMMAND_TIMEOUT	0x00010000U
#define STATUS_DATA_TIMEOUT	0x00100000U

/*
 * Every status bit a write may clear.  The card's interrupt is not among
 * them: it follows the card and goes away only when the card is served.
 */
#define STATUS_CLEARABLE	(~STATUS_CARD_INTERRUPT)

/*
 * The status bits the controller records: the command and data events the
 * host polls for and every error of specification 3.00 (command timeout,
 * CRC, end bit and index, data timeout, CRC and end bit, current limit,
 * auto CMD12 and ADMA).
 */
#define ENABLE_EVENTS		0x00000033U
#define ENABLE_ERRORS		0x03ff0000U

/* The specification version in the host version word: 2 is 3.00. */
#define VERSION_SHIFT		16U
#define VERSION_MASK		0xffU
#define SPECIFICATION_3_00	2U

/* The capabilities' base clock in MHz, logged only. */
#define CAPABILITIES_BASE_SHIFT	8U
#define CAPABILITIES_BASE_MASK	0xffU

/* The largest divider N of specification 3.00, and of the older ones. */
#define DIVISOR_MAX_3_00	1023U
#define DIVISOR_MAX_2_00	128U

/*
 * The pause after a write: two cycles of the bus clock, at 400 kHz and
 * below, and a fixed pause while the bus clock is stopped.
 */
#define WRITE_PAUSE_CYCLES	2U
#define WRITE_PAUSE_MAX_HZ	400000U
#define WRITE_PAUSE_STOPPED_US	10U

/* How long the host pauses between two looks at a register (a real busy wait, see pause_us). */
#define POLL_US			5U

/* How long a reset, a stable clock, a command and one block of data may take. */
#define RESET_TIMEOUT_US	100000U
#define CLOCK_TIMEOUT_US	150000U
#define COMMAND_TIMEOUT_US	100000U
#define DATA_TIMEOUT_US		1000000U

static int reset_lines_locked(struct drv_rpi4_sdio_host *host, uint32_t lines);
static int restore_locked(struct drv_rpi4_sdio_host *host);
static int program_clock_locked(struct drv_rpi4_sdio_host *host, uint32_t target_hz);
static void program_host_control_locked(struct drv_rpi4_sdio_host *host, uint32_t power);
static int send_locked(struct drv_rpi4_sdio_host *host, struct drv_rpi4_sdio_command *command);
static int move_block_locked(struct drv_rpi4_sdio_host *host, struct drv_rpi4_sdio_command *command, unsigned block);
static int fail_command_locked(struct drv_rpi4_sdio_host *host, int error, uint32_t status);
static int wait_status(struct drv_rpi4_sdio_host *host, uint32_t bits, unsigned timeout_us, uint32_t *status);
static int wait_present_clear(struct drv_rpi4_sdio_host *host, uint32_t bits, unsigned timeout_us);
static void write_enables_locked(struct drv_rpi4_sdio_host *host);
static uint32_t read_register(struct drv_rpi4_sdio_host *host, unsigned offset);
static void write_register(struct drv_rpi4_sdio_host *host, unsigned offset, uint32_t value);
static void pause_us(unsigned us);

/*
 * Prepares the controller for an SDIO card.
 */
int
drv_rpi4_sdio_host_init(
	struct drv_rpi4_sdio_host *host,
	volatile void *registers,
	uint32_t base_clock_hz)
{
	uint32_t version;
	uint32_t capabilities;
	int error;

	/* Refuses a controller without registers or without a clock to divide. */
	if (host == NULL)
		return EINVAL;
	if (registers == NULL)
		return EINVAL;
	if (base_clock_hz == 0)
		return EINVAL;

	/* Describes the controller before anything has been programmed. */
	host->registers = registers;
	host->base_clock_hz = base_clock_hz;
	host->clock_target_hz = DRV_RPI4_SDIO_HOST_IDENTIFY_HZ;
	host->card_clock_hz = 0;
	host->bus_width = 1;

	/* Starts with the card's interrupt kept away and nothing pending. */
	host->card_irq_wanted = 0;
	host->card_irq_armed = 0;
	host->card_irq_pending = 0;
	host->card_irq_kicked = 0;
	host->card_irq_count = 0;

	/* Prepares the locks and the queue the serving thread waits on. */
	spin_init(&host->irq_lock, LOCK_RANK_DEVICE, "rpi4 sdio irq");
	error = mutex_init(&host->command_lock, LOCK_RANK_DEVICE, "rpi4 sdio");
	if (error != 0)
		return error;
	waitq_init(&host->card_irq_wait, "rpi4 sdio card irq");

	/* Reads which specification the divider and the registers follow. */
	version = read_register(host, REG_HOST_VERSION);
	host->specification = (version >> VERSION_SHIFT) & VERSION_MASK;

	/* Records what the controller says of itself, for the first bring-up on a board. */
	capabilities = read_register(host, REG_CAPABILITIES);
	kern_logf(
		"rpi4-sdio: host specification %u, capabilities %08x (%u MHz), firmware clock %u Hz\n",
		host->specification,
		capabilities,
		(capabilities >> CAPABILITIES_BASE_SHIFT) & CAPABILITIES_BASE_MASK,
		base_clock_hz);

	/* Resets the controller and powers the bus at the enumeration clock. */
	error = drv_rpi4_sdio_host_reset(host);
	if (error != 0)
		return error;

	/* Succeeded: the bus runs at 400 kHz and accepts commands. */
	return 0;
}

/*
 * Resets the whole controller and restores what the reset clears.
 */
int
drv_rpi4_sdio_host_reset(
	struct drv_rpi4_sdio_host *host)
{
	int error;

	/* Resets and restores between two commands, not in the middle of one. */
	mutex_lock(&host->command_lock);

	error = reset_lines_locked(host, RESET_ALL);
	if (error != 0) {
		mutex_unlock(&host->command_lock);

		kern_logf("rpi4-sdio: host stayed in reset (%d)\n", error);
		return error;
	}

	error = restore_locked(host);

	mutex_unlock(&host->command_lock);

	/* Reports a controller whose clock did not come back after the reset. */
	if (error != 0) {
		kern_logf("rpi4-sdio: host not restored after its reset (%d)\n", error);
		return error;
	}

	/* Succeeded: the controller is as it was before the reset, with nothing in flight. */
	return 0;
}

/*
 * Runs the bus at the fastest rate at or below target_hz.
 */
int
drv_rpi4_sdio_host_set_clock(
	struct drv_rpi4_sdio_host *host,
	uint32_t target_hz)
{
	int error;

	/* Changes the clock between two commands. */
	mutex_lock(&host->command_lock);

	error = program_clock_locked(host, target_hz);

	mutex_unlock(&host->command_lock);

	/* Reports a rate the divider cannot reach or a clock that did not settle. */
	if (error != 0)
		return error;

	/* Succeeded: the bus runs at card_clock_hz. */
	return 0;
}

/*
 * Moves data on one line or on four.
 */
int
drv_rpi4_sdio_host_set_bus_width(
	struct drv_rpi4_sdio_host *host,
	unsigned width)
{
	/* Refuses a width the controller does not have (eight lines are eMMC only). */
	if (width != 1 && width != 4)
		return EINVAL;

	/* Rewrites the host control byte between two commands. */
	mutex_lock(&host->command_lock);

	host->bus_width = width;
	program_host_control_locked(host, POWER_VOLTAGE_3V3 | POWER_ON);

	mutex_unlock(&host->command_lock);

	/* Succeeded: the next data command uses the new width. */
	return 0;
}

/*
 * Sends one command, waits for its response and moves its data.
 */
int
drv_rpi4_sdio_host_command(
	struct drv_rpi4_sdio_host *host,
	struct drv_rpi4_sdio_command *command)
{
	int error;

	/* Refuses a command the controller's registers cannot express. */
	if (command == NULL)
		return EINVAL;
	if (command->index > COMMAND_INDEX_MAX)
		return EINVAL;
	if (command->response > DRV_RPI4_SDIO_RESPONSE_SHORT_UNCHECKED)
		return EINVAL;

	/* Refuses data without a buffer or outside the block register's range. */
	if (command->data != DRV_RPI4_SDIO_DATA_NONE) {
		if (command->buffer == NULL)
			return EINVAL;
		if (command->block_size == 0 || command->block_size > DRV_RPI4_SDIO_HOST_BLOCK_MAX)
			return EINVAL;
		if (command->block_count == 0 || command->block_count > DRV_RPI4_SDIO_HOST_BLOCK_COUNT_MAX)
			return EINVAL;
	}

	/* Runs the command alone on the controller. */
	mutex_lock(&host->command_lock);

	error = send_locked(host, command);

	mutex_unlock(&host->command_lock);

	/* Reports a command the card did not complete; the lines are reset already. */
	if (error != 0)
		return error;

	/* Succeeded: reply holds the response and the data has moved. */
	return 0;
}

/*
 * Finds the divider for a bus clock.
 */
int
drv_rpi4_sdio_host_clock_divisor(
	uint32_t base_hz,
	uint32_t target_hz,
	unsigned specification,
	unsigned *divisor,
	uint32_t *actual_hz)
{
	uint64_t needed;
	uint64_t twice_target;
	unsigned power;

	/* Refuses a missing clock, a rate of zero or nowhere to put the answer. */
	if (base_hz == 0 || target_hz == 0)
		return EINVAL;
	if (divisor == NULL || actual_hz == NULL)
		return EINVAL;

	/* A base clock no faster than the target is used undivided. */
	if (base_hz <= target_hz) {
		*divisor = 0;
		*actual_hz = base_hz;
		return 0;
	}

	/* The least N for which base / 2N does not exceed the target. */
	twice_target = 2ULL * (uint64_t)target_hz;
	needed = ((uint64_t)base_hz + twice_target - 1U) / twice_target;

	/*
	 * Specification 3.00 takes any N up to 1023; the older ones only a
	 * power of two up to 128, so N rounds up to the next one.
	 */
	if (specification >= SPECIFICATION_3_00) {
		if (needed > DIVISOR_MAX_3_00)
			return ERANGE;
	} else {
		/* Finds the least power of two that divides enough. */
		power = 1;
		while (power < needed)
			power <<= 1;

		/* Refuses a divider beyond the eight bits of the old register. */
		if (power > DIVISOR_MAX_2_00)
			return ERANGE;
		needed = power;
	}

	/* Reports the divider and the rate it gives. */
	*divisor = (unsigned)needed;
	*actual_hz = (uint32_t)((uint64_t)base_hz / (2ULL * needed));

	/* Succeeded: the rate is the fastest at or below the target. */
	return 0;
}

/*
 * Serves the controller's share of the SD interrupt line.
 */
bool
drv_rpi4_sdio_host_interrupt(
	void *argument)
{
	struct drv_rpi4_sdio_host *host;
	unsigned long enabled;
	uint32_t status;
	bool mine;

	host = argument;
	mine = false;

	/*
	 * Takes the card's interrupt.  Only an armed card interrupt is
	 * signalled to the line, so a disarmed controller is never the one
	 * raising it and its registers are not read.
	 */
	enabled = spin_lock_irqsave(&host->irq_lock);

	if (host->card_irq_armed != 0) {
		status = read_register(host, REG_INTERRUPT_STATUS);
		if ((status & STATUS_CARD_INTERRUPT) != 0) {
			/*
			 * Disarming drops the level, which a write of the
			 * status cannot clear; pending tells the serving
			 * thread to look at the card and rearm.
			 */
			host->card_irq_armed = 0;
			write_enables_locked(host);
			host->card_irq_pending = 1;
			host->card_irq_count++;
			waitq_wake_all(&host->card_irq_wait);
			mine = true;
		}
	}

	spin_unlock_irqrestore(&host->irq_lock, enabled);

	/* Reports whether the line was raised by this controller. */
	return mine;
}

/*
 * Lets the card's interrupt reach the CPU or keeps it away.
 */
void
drv_rpi4_sdio_host_card_irq_enable(
	struct drv_rpi4_sdio_host *host,
	int enable)
{
	unsigned long enabled;

	/*
	 * Records the wish and applies it.  Enabling arms the interrupt at
	 * once; disabling also forgets one that fired and was not served.
	 */
	enabled = spin_lock_irqsave(&host->irq_lock);

	if (enable) {
		host->card_irq_wanted = 1;
		host->card_irq_armed = 1;
	} else {
		host->card_irq_wanted = 0;
		host->card_irq_armed = 0;
		host->card_irq_pending = 0;
	}
	write_enables_locked(host);

	spin_unlock_irqrestore(&host->irq_lock, enabled);
}

/*
 * Arms the card's interrupt again after the thread has served it.
 */
void
drv_rpi4_sdio_host_card_irq_rearm(
	struct drv_rpi4_sdio_host *host)
{
	unsigned long enabled;

	/* Arms an interrupt the card layer wants and the handler disarmed. */
	enabled = spin_lock_irqsave(&host->irq_lock);

	if (host->card_irq_wanted != 0 && host->card_irq_armed == 0) {
		host->card_irq_armed = 1;
		write_enables_locked(host);
	}

	spin_unlock_irqrestore(&host->irq_lock, enabled);
}

/*
 * Waits until the card's interrupt fires or the deadline passes.
 */
int
drv_rpi4_sdio_host_card_irq_wait(
	struct drv_rpi4_sdio_host *host,
	uint64_t deadline)
{
	unsigned long enabled;
	uint64_t observed;
	int error;

	/* Sleeps until the handler leaves an interrupt pending, or until the deadline. */
	error = 0;
	enabled = spin_lock_irqsave(&host->irq_lock);

	for (;;) {
		/* Consumes an interrupt the handler left. */
		if (host->card_irq_pending != 0) {
			host->card_irq_pending = 0;
			error = 0;
			break;
		}

		/* Consumes a kick: the serving thread has work besides the card. */
		if (host->card_irq_kicked != 0) {
			host->card_irq_kicked = 0;
			error = EAGAIN;
			break;
		}

		/* Sleeps on the handler's wakeup; EAGAIN means one came meanwhile. */
		observed = waitq_sequence(&host->card_irq_wait);
		error = waitq_sleep(&host->card_irq_wait, &host->irq_lock, observed, deadline, 0);
		if (error == ETIMEDOUT)
			break;
	}

	spin_unlock_irqrestore(&host->irq_lock, enabled);

	/* Reports a kick or a deadline without an interrupt; the caller looks at the card anyway. */
	if (error != 0)
		return error;

	/* Succeeded: the card raised its interrupt. */
	return 0;
}

/*
 * Wakes the thread waiting for the card's interrupt without one.
 */
void
drv_rpi4_sdio_host_card_irq_kick(
	struct drv_rpi4_sdio_host *host)
{
	unsigned long enabled;

	/*
	 * kicked stays set until a wait consumes it, so a kick that comes
	 * before the thread sleeps still ends its next wait at once.
	 */
	enabled = spin_lock_irqsave(&host->irq_lock);

	host->card_irq_kicked = 1;
	waitq_wake_all(&host->card_irq_wait);

	spin_unlock_irqrestore(&host->irq_lock, enabled);
}

/*
 * Stops the controller.
 */
void
drv_rpi4_sdio_host_stop(
	struct drv_rpi4_sdio_host *host)
{
	uint32_t word;

	/* Keeps the card's interrupt away from the line first. */
	drv_rpi4_sdio_host_card_irq_enable(host, 0);

	/* Stops the bus clock and the internal clock, then the bus power. */
	mutex_lock(&host->command_lock);

	word = read_register(host, REG_CLOCK_CONTROL);
	word &= TIMEOUT_MASK;
	host->card_clock_hz = 0;
	write_register(host, REG_CLOCK_CONTROL, word);
	program_host_control_locked(host, 0);

	mutex_unlock(&host->command_lock);
}

/*
 * Resets the command line, the data lines or the whole controller, and
 * waits until the controller reports the reset done.
 */
static int
reset_lines_locked(
	struct drv_rpi4_sdio_host *host,
	uint32_t lines)
{
	uint32_t word;
	unsigned waited;

	/*
	 * Asks for the reset in the clock control word, keeping the clock and
	 * timeout bytes as they are.  The whole reset also stops the clock.
	 */
	word = read_register(host, REG_CLOCK_CONTROL);
	word &= ~(0xffU << RESET_SHIFT);
	word |= lines << RESET_SHIFT;
	if ((lines & RESET_ALL) != 0)
		host->card_clock_hz = 0;
	write_register(host, REG_CLOCK_CONTROL, word);

	/* Waits for the controller to clear the bits it was asked to reset. */
	for (waited = 0; waited < RESET_TIMEOUT_US; waited += POLL_US) {
		/* Finishes once every requested line is out of reset. */
		word = read_register(host, REG_CLOCK_CONTROL);
		if (((word >> RESET_SHIFT) & lines) == 0)
			return 0;

		/* Lets the reset run a little longer. */
		pause_us(POLL_US);
	}

	/* Reports a controller that stayed in reset. */
	return ETIMEDOUT;
}

/*
 * Restores the bus power, the clock, the bus width and the interrupt
 * enables after a whole reset cleared them.
 */
static int
restore_locked(
	struct drv_rpi4_sdio_host *host)
{
	unsigned long enabled;
	uint32_t word;
	int error;

	/* Gives data the longest timeout, keeping the stopped clock. */
	word = read_register(host, REG_CLOCK_CONTROL);
	word &= CLOCK_CONTROL_MASK;
	word |= (uint32_t)TIMEOUT_LONGEST << TIMEOUT_SHIFT;
	write_register(host, REG_CLOCK_CONTROL, word);

	/* Selects 3.3 V before switching the bus power on, as the specification orders. */
	program_host_control_locked(host, POWER_VOLTAGE_3V3);
	program_host_control_locked(host, POWER_VOLTAGE_3V3 | POWER_ON);

	/* Starts the bus clock at the rate last asked for. */
	error = program_clock_locked(host, host->clock_target_hz);
	if (error != 0)
		return error;

	/*
	 * Rewrites the interrupt enables the reset set to zero.  An interrupt
	 * the handler disarmed stays disarmed until the thread rearms it.
	 */
	enabled = spin_lock_irqsave(&host->irq_lock);

	write_enables_locked(host);

	spin_unlock_irqrestore(&host->irq_lock, enabled);

	/* Succeeded: the controller is ready for a command. */
	return 0;
}

/*
 * Stops the bus clock, programs the divider, waits for the internal clock
 * to settle and starts the bus clock again.
 */
static int
program_clock_locked(
	struct drv_rpi4_sdio_host *host,
	uint32_t target_hz)
{
	uint32_t word;
	uint32_t word_now;
	uint32_t clock;
	uint32_t actual_hz;
	unsigned divisor;
	unsigned waited;
	int stable;
	int error;

	/* Finds the divider before anything is stopped. */
	error = drv_rpi4_sdio_host_clock_divisor(
		host->base_clock_hz,
		target_hz,
		host->specification,
		&divisor,
		&actual_hz);
	if (error != 0) {
		kern_logf(
			"rpi4-sdio: no divider of %u Hz reaches %u Hz (%d)\n",
			host->base_clock_hz,
			target_hz,
			error);
		return error;
	}

	/*
	 * Stops the bus clock and the internal clock, keeping the timeout
	 * byte.  The rate is zero from this write on, so the writes pause.
	 */
	word = read_register(host, REG_CLOCK_CONTROL);
	word &= TIMEOUT_MASK;
	host->card_clock_hz = 0;
	write_register(host, REG_CLOCK_CONTROL, word);

	/* Starts the internal clock with the new divider's low and high bits. */
	clock = (divisor & 0xffU) << CLOCK_DIVISOR_LOW_SHIFT;
	clock |= ((divisor >> 8) & 0x3U) << CLOCK_DIVISOR_HIGH_SHIFT;
	clock |= CLOCK_INTERNAL_ENABLE;
	write_register(host, REG_CLOCK_CONTROL, word | clock);

	/* Waits for the internal clock to report itself stable. */
	stable = 0;
	for (waited = 0; waited <= CLOCK_TIMEOUT_US; waited += POLL_US) {
		/* Goes on once the clock is stable. */
		word_now = read_register(host, REG_CLOCK_CONTROL);
		if ((word_now & CLOCK_INTERNAL_STABLE) != 0) {
			stable = 1;
			break;
		}

		/* Lets the clock settle a little longer. */
		pause_us(POLL_US);
	}

	/* Gives up on a clock that never settled. */
	if (!stable) {
		kern_logf("rpi4-sdio: internal clock not stable\n");
		return ETIMEDOUT;
	}

	/* Lets the clock out to the bus, and records the rate the writes now pause for. */
	write_register(host, REG_CLOCK_CONTROL, word | clock | CLOCK_CARD_ENABLE);
	host->card_clock_hz = actual_hz;
	host->clock_target_hz = target_hz;

	/* Succeeded: the bus runs at actual_hz. */
	return 0;
}

/*
 * Writes the host control byte for the bus width together with a power
 * byte, leaving the block gap and wakeup bytes at zero.
 */
static void
program_host_control_locked(
	struct drv_rpi4_sdio_host *host,
	uint32_t power)
{
	uint32_t word;

	/* Selects the data width; SDIO at 25 MHz uses neither high speed nor DMA. */
	word = 0;
	if (host->bus_width == 4)
		word |= HOST_CONTROL_DATA_4BIT;

	/* Writes the power byte in the same word. */
	word |= power << POWER_SHIFT;
	write_register(host, REG_HOST_CONTROL, word);
}

/*
 * Sends one command and moves its data, with the command lock held.
 */
static int
send_locked(
	struct drv_rpi4_sdio_host *host,
	struct drv_rpi4_sdio_command *command)
{
	uint32_t inhibit;
	uint32_t mode;
	uint32_t code;
	uint32_t status;
	unsigned block;
	int error;

	/* Names the lines the command needs free: the data lines too for data or a busy signal. */
	inhibit = PRESENT_COMMAND_INHIBIT;
	if (command->data != DRV_RPI4_SDIO_DATA_NONE)
		inhibit |= PRESENT_DATA_INHIBIT;
	if (command->response == DRV_RPI4_SDIO_RESPONSE_SHORT_BUSY)
		inhibit |= PRESENT_DATA_INHIBIT;

	/* Waits for the previous command and data to leave the lines. */
	error = wait_present_clear(host, inhibit, COMMAND_TIMEOUT_US);
	if (error != 0) {
		error = fail_command_locked(host, error, 0);
		return error;
	}

	/* Clears the events of an earlier command, so only this one's are seen. */
	write_register(host, REG_INTERRUPT_STATUS, STATUS_CLEARABLE);

	/* Describes the blocks of a data command. */
	if (command->data != DRV_RPI4_SDIO_DATA_NONE) {
		write_register(
			host,
			REG_BLOCK,
			command->block_size | (command->block_count << BLOCK_COUNT_SHIFT));
	}

	/* Writes the argument before the command word that starts the command. */
	write_register(host, REG_ARGUMENT, command->argument);

	/* Builds the transfer mode: counted blocks, their direction, more than one. */
	mode = 0;
	if (command->data != DRV_RPI4_SDIO_DATA_NONE) {
		mode |= TRANSFER_BLOCK_COUNT;
		if (command->data == DRV_RPI4_SDIO_DATA_READ)
			mode |= TRANSFER_READ;
		if (command->block_count > 1)
			mode |= TRANSFER_MULTIPLE;
	}

	/* Builds the command: its index, the response it expects and the checks. */
	code = command->index << COMMAND_INDEX_SHIFT;
	switch (command->response) {
	case DRV_RPI4_SDIO_RESPONSE_NONE:
		break;
	case DRV_RPI4_SDIO_RESPONSE_SHORT:
		code |= COMMAND_RESPONSE_48 | COMMAND_CHECK_CRC | COMMAND_CHECK_INDEX;
		break;
	case DRV_RPI4_SDIO_RESPONSE_SHORT_BUSY:
		code |= COMMAND_RESPONSE_48_BUSY | COMMAND_CHECK_CRC | COMMAND_CHECK_INDEX;
		break;
	case DRV_RPI4_SDIO_RESPONSE_SHORT_UNCHECKED:
		code |= COMMAND_RESPONSE_48;
		break;
	default:
		return EINVAL;
	}

	/* Tells the controller that data follows the command. */
	if (command->data != DRV_RPI4_SDIO_DATA_NONE)
		code |= COMMAND_DATA_PRESENT;

	/* Starts the command: the transfer mode and the command go in one word. */
	write_register(host, REG_COMMAND, mode | (code << COMMAND_SHIFT));

	/* Waits for the response, or for the command to fail. */
	error = wait_status(host, STATUS_COMMAND_COMPLETE, COMMAND_TIMEOUT_US, &status);
	if (error != 0) {
		error = fail_command_locked(host, error, status);
		return error;
	}

	/* Takes the response and acknowledges it. */
	write_register(host, REG_INTERRUPT_STATUS, STATUS_COMMAND_COMPLETE);
	command->reply = read_register(host, REG_RESPONSE);

	/* Waits out the busy signal of an R1b response. */
	if (command->response == DRV_RPI4_SDIO_RESPONSE_SHORT_BUSY &&
	    command->data == DRV_RPI4_SDIO_DATA_NONE) {
		error = wait_status(host, STATUS_TRANSFER_COMPLETE, DATA_TIMEOUT_US, &status);
		if (error != 0) {
			error = fail_command_locked(host, error, status);
			return error;
		}

		/* Acknowledges the end of the busy signal. */
		write_register(host, REG_INTERRUPT_STATUS, STATUS_TRANSFER_COMPLETE);
	}

	/* Succeeds at once for a command without data. */
	if (command->data == DRV_RPI4_SDIO_DATA_NONE)
		return 0;

	/* Moves each block through the data port as the controller has room or data for it. */
	for (block = 0; block < command->block_count; block++) {
		error = move_block_locked(host, command, block);
		if (error != 0)
			return error;
	}

	/* Waits for the controller to finish the transfer on the bus. */
	error = wait_status(host, STATUS_TRANSFER_COMPLETE, DATA_TIMEOUT_US, &status);
	if (error != 0) {
		error = fail_command_locked(host, error, status);
		return error;
	}

	/* Acknowledges the end of the transfer. */
	write_register(host, REG_INTERRUPT_STATUS, STATUS_TRANSFER_COMPLETE);

	/* Succeeded: the response is in reply and the data has moved. */
	return 0;
}

/*
 * Moves one block through the data port once the controller is ready
 * for it.  The last word of a block whose size is not a multiple of four
 * carries the block's last bytes in its low bytes.
 */
static int
move_block_locked(
	struct drv_rpi4_sdio_host *host,
	struct drv_rpi4_sdio_command *command,
	unsigned block)
{
	uint8_t *bytes;
	uint32_t ready;
	uint32_t status;
	uint32_t word;
	unsigned offset;
	unsigned count;
	unsigned i;
	int error;

	/* Waits until the buffer holds a block to read, or has room for one to write. */
	ready = STATUS_WRITE_READY;
	if (command->data == DRV_RPI4_SDIO_DATA_READ)
		ready = STATUS_READ_READY;
	error = wait_status(host, ready, DATA_TIMEOUT_US, &status);
	if (error != 0) {
		error = fail_command_locked(host, error, status);
		return error;
	}

	/* Acknowledges the readiness before the block moves, as the specification orders. */
	write_register(host, REG_INTERRUPT_STATUS, ready);

	/* Moves the block one word at a time. */
	bytes = command->buffer + (size_t)block * command->block_size;
	for (offset = 0; offset < command->block_size; offset += 4U) {
		/* The bytes of this word that belong to the block. */
		count = command->block_size - offset;
		if (count > 4U)
			count = 4U;

		/* Reads a word and keeps its bytes in order. */
		if (command->data == DRV_RPI4_SDIO_DATA_READ) {
			word = read_register(host, REG_DATA_PORT);
			for (i = 0; i < count; i++)
				bytes[offset + i] = (uint8_t)(word >> (8U * i));
			continue;
		}

		/* Assembles a word from the block's bytes, padding the last one. */
		word = 0;
		for (i = 0; i < count; i++)
			word |= (uint32_t)bytes[offset + i] << (8U * i);
		write_register(host, REG_DATA_PORT, word);
	}

	/* Succeeded: the block has moved. */
	return 0;
}

/*
 * Ends a failed command: names the error, clears the events and resets
 * the command and data lines so the next command starts clean.
 */
static int
fail_command_locked(
	struct drv_rpi4_sdio_host *host,
	int error,
	uint32_t status)
{
	int reset_error;

	/* A card that did not answer in time is a timeout; any other error the controller saw is EIO. */
	if (error == EIO) {
		if ((status & (STATUS_COMMAND_TIMEOUT | STATUS_DATA_TIMEOUT)) != 0)
			error = ETIMEDOUT;
	}

	/* Names the controller's errors of a failure that was no timeout. */
	if (error == EIO)
		kern_logf("rpi4-sdio: command failed, interrupt status %08x\n", status);

	/* Clears what the command left in the status. */
	write_register(host, REG_INTERRUPT_STATUS, STATUS_CLEARABLE);

	/* Resets the command and data lines; a failure here is logged, the command's error kept. */
	reset_error = reset_lines_locked(host, RESET_COMMAND | RESET_DATA);
	if (reset_error != 0)
		kern_logf("rpi4-sdio: command and data lines stayed in reset (%d)\n", reset_error);

	/* Reports why the command failed. */
	return error;
}

/*
 * Polls the interrupt status for any of bits or for an error.
 *
 * Returns 0 when one of bits is set, EIO when the controller reports an
 * error and ETIMEDOUT when neither happens in time; status receives the
 * last value read.
 */
static int
wait_status(
	struct drv_rpi4_sdio_host *host,
	uint32_t bits,
	unsigned timeout_us,
	uint32_t *status)
{
	uint32_t value;
	unsigned waited;

	/* Looks at the status until the event comes, an error comes or the time is up. */
	value = 0;
	for (waited = 0; waited <= timeout_us; waited += POLL_US) {
		/* An error ends the wait, even when the event came with it. */
		value = read_register(host, REG_INTERRUPT_STATUS);
		if ((value & STATUS_ERROR) != 0) {
			*status = value;
			return EIO;
		}

		/* The event the caller waits for. */
		if ((value & bits) != 0) {
			*status = value;
			return 0;
		}

		/* Lets the bus move a little further. */
		pause_us(POLL_US);
	}

	/* Reports an event that never came. */
	*status = value;
	return ETIMEDOUT;
}

/*
 * Polls the present state until the given lines are free.
 */
static int
wait_present_clear(
	struct drv_rpi4_sdio_host *host,
	uint32_t bits,
	unsigned timeout_us)
{
	uint32_t present;
	unsigned waited;

	/* Looks at the lines until they are free or the time is up. */
	for (waited = 0; waited <= timeout_us; waited += POLL_US) {
		/* The lines the command needs are free. */
		present = read_register(host, REG_PRESENT_STATE);
		if ((present & bits) == 0)
			return 0;

		/* Lets the previous command finish. */
		pause_us(POLL_US);
	}

	/* Reports lines that stayed busy. */
	return ETIMEDOUT;
}

/*
 * Writes the status enable and signal enable registers from the host's
 * state, with irq_lock held.
 *
 * Every event and error is recorded in the status, but only an armed
 * card interrupt is signalled to the line.
 */
static void
write_enables_locked(
	struct drv_rpi4_sdio_host *host)
{
	uint32_t status_enable;
	uint32_t signal_enable;

	/* Records the command and data events and the errors; the card's interrupt only when armed. */
	status_enable = ENABLE_EVENTS | ENABLE_ERRORS;
	signal_enable = 0;
	if (host->card_irq_armed != 0) {
		status_enable |= STATUS_CARD_INTERRUPT;
		signal_enable |= STATUS_CARD_INTERRUPT;
	}

	/* Writes the status enable first, so a disarm drops the level at once. */
	write_register(host, REG_STATUS_ENABLE, status_enable);
	write_register(host, REG_SIGNAL_ENABLE, signal_enable);
}

/* Reads one register word. */
static uint32_t
read_register(
	struct drv_rpi4_sdio_host *host,
	unsigned offset)
{
	uint32_t value;

	/* Reads through the kernel's device accessor. */
	value = kern_mmio_read32(host->registers + offset);

	/* Reports the register's contents. */
	return value;
}

/*
 * Writes one register word, then pauses for two bus clock cycles while the
 * bus runs at 400 kHz or below, or a fixed pause while its clock is
 * stopped, so the next write is not lost.
 */
static void
write_register(
	struct drv_rpi4_sdio_host *host,
	unsigned offset,
	uint32_t value)
{
	unsigned pause;

	/* Writes through the kernel's device accessor. */
	kern_mmio_write32(host->registers + offset, value);

	/* No pause is needed above 400 kHz. */
	if (host->card_clock_hz > WRITE_PAUSE_MAX_HZ)
		return;

	/* Waits two cycles of the bus clock, rounded up to whole microseconds. */
	pause = WRITE_PAUSE_STOPPED_US;
	if (host->card_clock_hz != 0)
		pause = (WRITE_PAUSE_CYCLES * 1000000U + host->card_clock_hz - 1U) / host->card_clock_hz;
	pause_us(pause);
}

/*
 * Waits at least us microseconds by watching the monotonic counter, without
 * giving the CPU up.
 *
 * kern_usleep_range() yields until the next scheduler tick, so each of its
 * pauses lasts a millisecond or more; a poll that pauses that long per look
 * moved the Wi-Fi firmware download about one 64-byte block a millisecond
 * (BUG-311: about 20 seconds, and the interface stayed busy meanwhile),
 * and stretched every timeout counted in pauses.  Without a counter it
 * falls back to that sleep.
 */
static void
pause_us(
	unsigned us)
{
	uint64_t start;
	uint64_t now;
	uint64_t frequency;
	uint64_t ticks;
	bool counter;

	/* The counter, and the counts the pause takes. */
	counter = kern_rtc_read_counter(&start, &frequency);
	if (!counter || frequency == 0) {
		kern_usleep_range(us, us + 1U);
		return;
	}
	ticks = ((uint64_t)us * frequency + 999999U) / 1000000U;

	/* Watches the counter until the counts have passed. */
	for (;;) {
		counter = kern_rtc_read_counter(&now, &frequency);
		if (!counter)
			break;
		if (now - start >= ticks)
			break;
	}
}
