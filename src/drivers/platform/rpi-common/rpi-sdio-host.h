/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Raspberry Pi 4 legacy EMMC controller as an SDIO host.
 *
 * The BCM2711 keeps its older Arasan SDHCI at 0xfe300000 wired to the
 * onboard Wi-Fi chip through GPIO 34 to 39.  This host moves commands and
 * their data by PIO, runs the bus at 400 kHz for enumeration and 25 MHz
 * afterwards, and turns the card's interrupt into a wakeup of the thread
 * that serves the card.  It is separate from the SD card driver of the
 * eMMC2 controller and shares no code with it.
 */

#ifndef KERN_DRIVERS_RPI4_SDIO_HOST_H
#define KERN_DRIVERS_RPI4_SDIO_HOST_H

#include <stdbool.h>
#include <stdint.h>

#include <kern/lock.h>
#include <kern/waitq.h>

/* How many bytes of registers the controller has. */
#define DRV_RPI4_SDIO_HOST_REGISTER_SPAN	0x100U

/* The bus clocks of enumeration and of normal operation. */
#define DRV_RPI4_SDIO_HOST_IDENTIFY_HZ		400000U
#define DRV_RPI4_SDIO_HOST_DEFAULT_HZ		25000000U

/*
 * How often the thread serving the card looks at the card's interrupt
 * status even without an interrupt, in case one was missed.
 */
#define DRV_RPI4_SDIO_HOST_CARD_IRQ_POLL_MS	100U

/* The most bytes one block of a data command may hold. */
#define DRV_RPI4_SDIO_HOST_BLOCK_MAX		2048U

/* The most blocks one data command may move. */
#define DRV_RPI4_SDIO_HOST_BLOCK_COUNT_MAX	511U

/*
 * The response a command expects.
 *
 * SHORT is a 48-bit response the controller checks for its CRC and command
 * index (R1, R5, R6).  SHORT_BUSY is the same with a busy signal after it
 * (R1b).  SHORT_UNCHECKED is a 48-bit response without either (R4).
 */
enum drv_rpi4_sdio_response {
	DRV_RPI4_SDIO_RESPONSE_NONE = 0,
	DRV_RPI4_SDIO_RESPONSE_SHORT,
	DRV_RPI4_SDIO_RESPONSE_SHORT_BUSY,
	DRV_RPI4_SDIO_RESPONSE_SHORT_UNCHECKED
};

/* Which way a command's data moves, if it has any. */
enum drv_rpi4_sdio_data {
	DRV_RPI4_SDIO_DATA_NONE = 0,
	DRV_RPI4_SDIO_DATA_READ,
	DRV_RPI4_SDIO_DATA_WRITE
};

/*
 * One command and its data.
 *
 * The caller fills everything but reply, which receives the response's 32
 * bits of card status or register contents.  A data command moves
 * block_count blocks of block_size bytes through buffer.
 */
struct drv_rpi4_sdio_command {
	unsigned index;
	uint32_t argument;
	enum drv_rpi4_sdio_response response;
	enum drv_rpi4_sdio_data data;
	uint8_t *buffer;
	unsigned block_size;
	unsigned block_count;
	uint32_t reply;
};

/*
 * One legacy EMMC controller serving one SDIO card.
 *
 * Its owner keeps it for as long as the controller is in use.  Commands
 * and the bus settings are serialized by command_lock; only the thread
 * serving the card issues them.  clock_target_hz is the rate last asked
 * for, which a reset restores, and card_clock_hz the rate the bus runs at,
 * zero while its clock is stopped.  The interrupt fields are protected by
 * irq_lock, which the interrupt handler takes too.
 *
 * card_irq_wanted is what the card layer asked for: whether the card's
 * interrupt should reach the CPU at all.  card_irq_armed is whether it is
 * enabled in the controller right now: the handler disarms it when it
 * fires, because the controller keeps raising the level until the card is
 * served, and the serving thread rearms it afterwards.  card_irq_kicked
 * is a wakeup without an interrupt, for work the serving thread has
 * besides the card.
 */
struct drv_rpi4_sdio_host {
	volatile uint8_t *registers;
	uint32_t base_clock_hz;
	uint32_t clock_target_hz;
	uint32_t card_clock_hz;
	unsigned specification;
	unsigned bus_width;

	struct mutex command_lock;

	struct spinlock irq_lock;
	struct wait_queue card_irq_wait;
	unsigned card_irq_wanted;
	unsigned card_irq_armed;
	unsigned card_irq_pending;
	unsigned card_irq_kicked;
	uint64_t card_irq_count;
};

/*
 * Prepares the controller: resets it, powers the bus at 3.3 V and starts
 * the 400 kHz enumeration clock.
 *
 * registers is the mapped register block and base_clock_hz the clock the
 * firmware feeds the controller.  The card's interrupt starts disabled.
 * Thread context only.
 */
int drv_rpi4_sdio_host_init(struct drv_rpi4_sdio_host *host, volatile void *registers, uint32_t base_clock_hz);

/*
 * Resets the whole controller and restores its bus power, clock, bus width
 * and interrupt enables, which the reset clears.
 */
int drv_rpi4_sdio_host_reset(struct drv_rpi4_sdio_host *host);

/*
 * Runs the bus at the fastest rate the controller's divider allows at or
 * below target_hz.
 */
int drv_rpi4_sdio_host_set_clock(struct drv_rpi4_sdio_host *host, uint32_t target_hz);

/*
 * Moves data on one line or on four.  The card must be told first.
 */
int drv_rpi4_sdio_host_set_bus_width(struct drv_rpi4_sdio_host *host, unsigned width);

/*
 * Sends one command, waits for its response and moves its data.
 *
 * Returns ETIMEDOUT when the card did not answer or its data did not come,
 * and EIO for any other error the controller reports; both leave the
 * command and data lines reset and ready for the next command.
 */
int drv_rpi4_sdio_host_command(struct drv_rpi4_sdio_host *host, struct drv_rpi4_sdio_command *command);

/*
 * Finds the divider for a bus clock.
 *
 * A host of specification 3.00 or later (specification 2 and above)
 * divides its base clock by 2N for N up to 1023; an older one only by
 * powers of two up to 256.  divisor receives N (0 for the undivided base
 * clock) and actual_hz the resulting rate, the fastest at or below
 * target_hz.  Returns ERANGE when even the slowest divider is too fast.
 */
int drv_rpi4_sdio_host_clock_divisor(uint32_t base_hz, uint32_t target_hz, unsigned specification, unsigned *divisor, uint32_t *actual_hz);

/*
 * Serves the controller's share of the SD interrupt line.
 *
 * argument is the host.  When the card's interrupt is raised and armed, it
 * disarms it, which stops the level, and wakes the thread waiting in
 * drv_rpi4_sdio_host_card_irq_wait().  Interrupt context; returns whether
 * the interrupt was this controller's.
 */
bool drv_rpi4_sdio_host_interrupt(void *argument);

/*
 * Lets the card's interrupt reach the CPU (enable 1) or keeps it away
 * (enable 0).
 */
void drv_rpi4_sdio_host_card_irq_enable(struct drv_rpi4_sdio_host *host, int enable);

/*
 * Arms the card's interrupt again after the thread has served it.
 */
void drv_rpi4_sdio_host_card_irq_rearm(struct drv_rpi4_sdio_host *host);

/*
 * Waits until the card's interrupt fires or the scheduler's clock reaches
 * deadline.
 *
 * Returns 0 for an interrupt, which is consumed, EAGAIN for a kick
 * (drv_rpi4_sdio_host_card_irq_kick()), and ETIMEDOUT at the deadline;
 * the caller then looks at the card anyway.  Thread context only.
 */
int drv_rpi4_sdio_host_card_irq_wait(struct drv_rpi4_sdio_host *host, uint64_t deadline);

/*
 * Ends the serving thread's current or next wait for the card's
 * interrupt with EAGAIN.  Any thread context.
 */
void drv_rpi4_sdio_host_card_irq_kick(struct drv_rpi4_sdio_host *host);

/*
 * Stops the controller: no interrupt, no bus clock and no bus power.
 */
void drv_rpi4_sdio_host_stop(struct drv_rpi4_sdio_host *host);

#endif
