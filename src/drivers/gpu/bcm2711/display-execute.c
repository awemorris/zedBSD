/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Ordered execution of one fully prepared BCM2711 display hardware program. */
#include <stdbool.h>
#include <stdint.h>

#include <kern/clock.h>
#include <kern/dcache.h>
#include <kern/device-io.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"
#include "drivers/gpu/bcm2711/display-program.h"
#include "drivers/platform/rpi4/rpi4-firmware.h"

static int execute_command(const struct bcm2711_display_command *command, const struct bcm2711_window *windows, const struct drv_bcm2711_boot_screen *screen, struct bcm2711_display *display, uint32_t *captured);
static int firmware_clock_request(uint32_t tag, uint32_t id, uint32_t value);
static int wait_register(const struct bcm2711_display_command *command, volatile uint8_t *window);
static int verify_framebuffer(const struct drv_bcm2711_boot_screen *screen);

/*
 * Executes an immutable prepared boot program and retains its failure index.
 * The caller owns every mapping and the framebuffer throughout execution.
 * Clock/notification/wait failures prevent all later hardware operations.
 * This function is also exercised with kernel I/O stubs in the host test.
 */
int
bcm2711_display_program_execute(
	struct bcm2711_display_program *program,
	const struct bcm2711_window *windows,
	const struct drv_bcm2711_boot_screen *screen,
	struct bcm2711_display *display)
{
	uint32_t captured;
	uint32_t index;
	int error;

	/* Refuses a failed preparation or an empty/truncated command stream. */
	program->failure = 0;
	if (program->error != 0)
		return program->error;
	if (program->count == 0 || program->count > BCM2711_DISPLAY_COMMANDS)
		return EINVAL;

	/* Initializes scratch storage before the program's first capture. */
	captured = 0;
	for (index = 0; index < program->count; index++) {
		/* Stops immediately instead of enabling later pipeline stages after failure. */
		error = execute_command(&program->commands[index], windows, screen, display, &captured);
		if (error != 0) {
			program->failure = index;
			bcm2711_display_irq_mask(display);
			return error;
		}
	}

	/* Succeeded: every command, including frame/list confirmation, completed. */
	return 0;
}

/* Executes one checked command without consulting DRM or foreign driver code. */
static int
execute_command(
	const struct bcm2711_display_command *command,
	const struct bcm2711_window *windows,
	const struct drv_bcm2711_boot_screen *screen,
	struct bcm2711_display *display,
	uint32_t *captured)
{
	volatile uint8_t *window;
	uint32_t value;
	uint32_t hz;
	uint32_t answered;
	uint32_t waited;
	void *framebuffer;
	int error;

	/* Non-register commands own the firmware or cache operation they invoke. */
	switch (command->operation) {
	case BCM2711_DISPLAY_IRQ_OPEN:
		/* Device-source callbacks were installed before the destructive attempt. */
		error = bcm2711_display_irq_open(display, command->region);
		if (error != 0)
			return error;
		break;
	case BCM2711_DISPLAY_FRAME_ARM:
		/* Excludes old firmware frames before opening the new pixel flow. */
		bcm2711_display_frame_arm(display);
		break;
	case BCM2711_DISPLAY_FRAME_WAIT:
		/* Completion requires a serviced vblank and matching current list. */
		waited = 0;
		while (!display->first_frame && waited < command->limit_us) {
			/* The IRQ handler owns the volatile completion flag. */
			kern_usleep_range(10, 10);
			waited += 10;
		}

		/* Reports completion only when the source handler observed adoption. */
		if (!display->first_frame)
			return ETIMEDOUT;
		break;
	case BCM2711_DISPLAY_NOTIFY:
		/* Sends the exact zero-byte display-done notification. */
		error = drv_rpi4_firmware_property(0x00030066U, NULL, 0, 0, &answered);
		if (error != 0)
			return error;
		error = verify_framebuffer(screen);
		if (error != 0)
			return error;
		break;
	case BCM2711_DISPLAY_CLOCK_ON:
		/* Prepares a shared firmware clock, then guards against a zero rate. */
		error = firmware_clock_request(0x00038001U, command->region, 1);
		if (error != 0)
			return error;
		error = bcm2711_clock_hz(command->region, &hz);
		if (error != 0)
			return error;
		if (hz == 0)
			return ENODEV;
		break;
	case BCM2711_DISPLAY_CLOCK_RATE:
		/* Applies the rate request through the firmware-owned clock provider. */
		error = firmware_clock_request(0x00038002U, command->region, command->value);
		if (error != 0)
			return error;
		break;
	case BCM2711_DISPLAY_DELAY:
		/* Waits using the existing early-boot microsecond counter. */
		kern_usleep_range(command->limit_us, command->limit_us);
		break;
	case BCM2711_DISPLAY_CLEAN:
		/* Makes the console's RGB bytes visible before installing their list. */
		framebuffer = kern_pmem_to_kernel(screen->physical);
		kern_dcache_clean_range(framebuffer, (size_t)screen->size);
		kern_io_write_barrier();
		break;
	default:
		/* Register commands are checked against their mapped role below. */
		if (command->region >= BCM2711_REGION_COUNT)
			return EINVAL;
		if (windows[command->region].size < 4U)
			return EINVAL;
		if ((command->offset & 3U) != 0)
			return EINVAL;
		if (command->offset > windows[command->region].size - 4U)
			return EINVAL;
		window = windows[command->region].mapped;
		if (window == NULL)
			return ENODEV;
		if (command->operation == BCM2711_DISPLAY_WAIT) {
			/* A timed-out dependency prevents subsequent writes. */
			error = wait_register(command, window);
			if (error != 0)
				return error;
		} else if (command->operation == BCM2711_DISPLAY_CAPTURE) {
			/* Captures one writable value for repeated identical FIFO operations. */
			value = kern_mmio_read32(window + command->offset);
			*captured = value & command->mask;
		} else if (command->operation == BCM2711_DISPLAY_REPLAY) {
			/* Replays from the single capture, without rereading changing status. */
			value = (*captured & ~command->mask) | command->value;
			kern_mmio_write32(window + command->offset, value);
		} else if (command->operation == BCM2711_DISPLAY_WRITE) {
			/* Exact writes include ordered SRAM words and W1C status. */
			kern_mmio_write32(window + command->offset, command->value);
		} else if (command->operation == BCM2711_DISPLAY_UPDATE) {
			/* Replaces only the specified register fields. */
			value = kern_mmio_read32(window + command->offset);
			value = (value & ~command->mask) | command->value;
			kern_mmio_write32(window + command->offset, value);
		} else {
			/* Unknown operations must not be interpreted as hardware writes. */
			return EINVAL;
		}

		break;
	}

	/* Succeeded: the command's precondition and hardware action completed. */
	return 0;
}

/* Sends one three-word clock-provider request and checks its response identity. */
static int
firmware_clock_request(
	uint32_t tag,
	uint32_t id,
	uint32_t value)
{
	uint32_t values[3];
	uint32_t answered;
	int error;

	/* The third provider word leaves the firmware's turbo setting unchanged. */
	values[0] = id;
	values[1] = value;
	values[2] = 0;
	error = drv_rpi4_firmware_property(tag, values, 3, 3, &answered);
	if (error != 0)
		return error;
	if (answered < 8U || values[0] != id)
		return EIO;

	/* Succeeded: the firmware accepted this clock's request. */
	return 0;
}

/* Polls one register until its required state is observed or its time bound ends. */
static int
wait_register(
	const struct bcm2711_display_command *command,
	volatile uint8_t *window)
{
	uint32_t waited;
	uint32_t value;

	/* The deadline includes one final observation, never an unbounded spin. */
	waited = 0;
	while (waited <= command->limit_us) {
		/* Observes the exact field without assuming when the pipeline samples it. */
		value = kern_mmio_read32(window + command->offset);
		if ((value & command->mask) == command->value)
			return 0;
		if (waited == command->limit_us)
			return ETIMEDOUT;
		kern_usleep_range(10U, 10U);
		waited += 10U;
	}

	/* An invalid, non-aligned time bound cannot grant completion. */
	return ETIMEDOUT;
}

/* Detects visible framebuffer geometry changes after firmware notification. */
static int
verify_framebuffer(
	const struct drv_bcm2711_boot_screen *screen)
{
	uint32_t values[2];
	int error;

	/* A changed size is an observable lifetime failure, not permission to reuse RAM. */
	values[0] = 0;
	values[1] = 0;
	error = bcm2711_firmware_get(0x00040003U, values, 0, 2);
	if (error != 0)
		return error;
	if (values[0] != screen->width || values[1] != screen->height)
		return EIO;
	values[0] = 0;
	error = bcm2711_firmware_get(0x00040008U, values, 0, 1);
	if (error != 0)
		return error;
	if (values[0] != screen->pitch)
		return EIO;

	/* Succeeded: the exposed framebuffer geometry is unchanged; lifetime is unproved. */
	return 0;
}
