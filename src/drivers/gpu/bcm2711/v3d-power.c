/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Native V3D power gates are validated before the engine's first register read. */
#include <kern/clock.h>
#include <kern/device-io.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"
#include "drivers/platform/rpi4/rpi4-firmware.h"

/* Binding IDs refer to V3D only; adjacent PM gates belong to other devices. */
#define POWER_DOMAIN_ENGINE 1U
#define POWER_RESET_ENGINE 0U
#define POWER_GRAPHICS_OFFSET 0x10cU
#define POWER_ENGINE_RELEASE 0x40U
#define POWER_WRITE_KEY 0x5a000000U
#define POWER_BRIDGE_SLAVE 0x08U
#define POWER_BRIDGE_MASTER 0x0cU
#define POWER_BRIDGE_ID 0x20U
#define POWER_BRIDGE_SIGNATURE 0x62726467U

static int provider(const struct drv_fdt *fdt, uint32_t node, const char *property, const char *cells, uint32_t *owner, uint32_t *number);
static int validate_clock(const struct drv_fdt *fdt, uint32_t node, uint32_t *owner);
static int clock_change(uint32_t tag, uint32_t value);
static int clock_value(uint32_t tag, uint32_t *value);
static int connect_engine(struct bcm2711_v3d *v3d);
static int bridge_change(struct bcm2711_v3d *v3d, uint32_t offset, bool enable);
static int map_provider_window(const struct drv_fdt *fdt, uint32_t node, const char *name, uint64_t minimum, struct bcm2711_window *window);

/*
 * Validates the exact native PM/reset/firmware-clock providers and maps their roles.
 * No PM, bridge or engine register is read or written by preparation.
 */
int
bcm2711_v3d_power_prepare(
	const struct drv_fdt *fdt,
	struct bcm2711_v3d *v3d)
{
	uint32_t node;
	uint32_t owner;
	uint32_t number;
	uint32_t reset_owner;
	uint32_t clock_owner;
	uint32_t parent_clock;
	uint32_t index;
	int error;

	/* Refuses re-preparation of permanent windows or a partly executed power transition. */
	if (!v3d->present)
		return ENODEV;
	if (v3d->power.prepared || v3d->power.attempted)
		return EBUSY;
	error = bcm2711_fdt_find(fdt, "brcm,2711-v3d", &node);
	if (error != 0)
		return error;

	/* The firmware's power-domain specifier must name this board's native V3D gate. */
	error = provider(fdt, node, "power-domains", "#power-domain-cells", &owner, &number);
	if (error != 0)
		return error;
	if (number != POWER_DOMAIN_ENGINE)
		return ENOTSUP;
	error = bcm2711_fdt_string_index(fdt, owner, "compatible", "brcm,bcm2711-pm", &index);
	if (error != 0)
		return ENOTSUP;

	/* Reset must belong to the same controller and the same V3D-only reset line. */
	error = provider(fdt, node, "resets", "#reset-cells", &reset_owner, &number);
	if (error != 0)
		return error;
	if (reset_owner != owner || number != POWER_RESET_ENGINE)
		return ENOTSUP;

	/* Engine and PM pulses must use the same firmware clock rather than a guessed rate. */
	error = validate_clock(fdt, node, &clock_owner);
	if (error != 0)
		return error;
	error = bcm2711_fdt_string_index(fdt, owner, "clock-names", "v3d", &index);
	if (error != 0 || index != 0)
		return ENOTSUP;
	error = validate_clock(fdt, owner, &parent_clock);
	if (error != 0)
		return error;
	if (parent_clock != clock_owner)
		return ENOTSUP;

	/* PM and both bridge bindings are resolved by role before any hardware access. */
	error = map_provider_window(fdt, owner, "pm", 0x110U, &v3d->power.control);
	if (error != 0)
		return error;
	error = map_provider_window(fdt, owner, "asb", 0x24U, &v3d->power.legacy_bridge);
	if (error != 0)
		return error;
	error = map_provider_window(fdt, owner, "rpivid_asb", 0x20U, &v3d->power.bridge);
	if (error != 0)
		return error;
	v3d->power.prepared = true;

	/* Succeeded: the native providers are known, but engine registers remain inaccessible. */
	return 0;
}

/*
 * Starts the native domain once, then maximizes and verifies firmware clock five.
 * Every failure keeps READY false, preventing subsequent engine register access.
 */
int
bcm2711_v3d_power_start(
	struct bcm2711_v3d *v3d)
{
	uint32_t minimum;
	uint32_t maximum;
	uint32_t current;
	uint32_t id;
	uint32_t revision;
	bool allowed;
	int error;

	/* A partly executed startup may be retried only through explicit global reset. */
	if (!v3d->power.prepared)
		return ENODEV;
	if (v3d->power.attempted)
		return EBUSY;
	allowed = bcm2711_stage_allowed(BCM2711_FAMILY_V3D, "V1");
	if (!allowed)
		return ECANCELED;

	/* Uses the same emulator revision refusal as display readout before native PM access. */
	revision = 0;
	error = bcm2711_firmware_get(0x00000001U, &revision, 0, 1);
	if (error != 0)
		return error;
	if (revision < 0x40000000U)
		return ENODEV;

	/* These provider registers are accessible independently of the engine domain. */
	id = kern_mmio_read32(v3d->power.legacy_bridge.mapped + POWER_BRIDGE_ID);
	if (id != POWER_BRIDGE_SIGNATURE)
		return ENODEV;
	id = kern_mmio_read32(v3d->power.bridge.mapped + POWER_BRIDGE_ID);
	if (id != POWER_BRIDGE_SIGNATURE)
		return ENODEV;
	bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V1 begin native PM clock/reset/bridges");
	bcm2711_stage_pause(BCM2711_FAMILY_V3D, "V1");
	v3d->power.attempted = true;

	/* Establishes reset release and AXI access before touching any V3D register. */
	error = connect_engine(v3d);
	if (error != 0)
		return error;
	bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V1 native domain and bridges enabled");
	allowed = bcm2711_stage_allowed(BCM2711_FAMILY_V3D, "V2");
	if (!allowed)
		return ECANCELED;

	/* Captures provider limits and current rate without reading a gated engine register. */
	error = clock_value(0x00030007U, &minimum);
	if (error != 0)
		return error;
	error = clock_value(0x00030004U, &maximum);
	if (error != 0)
		return error;
	if (maximum == 0 || minimum > maximum)
		return EIO;
	error = clock_value(0x00030002U, &current);
	if (error != 0)
		return error;

	/* Uses the firmware provider's maximum just as the native V3D clock variant does. */
	if (current < maximum) {
		error = clock_change(0x00038002U, maximum);
		if (error != 0)
			return error;
	}

	/* Requires a running, nonzero provider clock before granting register access. */
	error = clock_change(0x00038001U, 1);
	if (error != 0)
		return error;
	error = bcm2711_clock_hz(5, &current);
	if (error != 0)
		return error;
	if (current == 0 || current < maximum)
		return EIO;
	v3d->power.ready = true;
	bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V2 ready clock %u min %u max %u", current, minimum, maximum);

	/* Succeeded: the caller may now identify and initialize V3D registers. */
	return 0;
}

/*
 * Resets V3D only after the worker has stopped submission and masked source IRQs.
 * A bridge timeout supplies no DMA-retirement proof and keeps READY false.
 */
int
bcm2711_v3d_power_reset(
	struct bcm2711_v3d *v3d)
{
	uint32_t value;
	int error;
	int restored;

	/* Refuses an unknown provider or a domain that has never been established. */
	if (!v3d->power.prepared || !v3d->power.attempted)
		return ENODEV;
	v3d->power.ready = false;

	/* Stops slave traffic first, then waits for the engine master's outstanding traffic. */
	error = bridge_change(v3d, POWER_BRIDGE_SLAVE, false);
	if (error != 0)
		return error;
	error = bridge_change(v3d, POWER_BRIDGE_MASTER, false);
	if (error != 0) {
		/* Restores the previously stopped slave if the master cannot become idle. */
		restored = bridge_change(v3d, POWER_BRIDGE_SLAVE, true);
		if (restored != 0)
			return restored;
		return error;
	}

	/* A failed clock shutdown cannot be treated as a completed native reset. */
	error = clock_change(0x00038001U, 0);
	if (error != 0)
		return error;
	value = kern_mmio_read32(v3d->power.control.mapped + POWER_GRAPHICS_OFFSET);
	kern_mmio_write32(v3d->power.control.mapped + POWER_GRAPHICS_OFFSET, POWER_WRITE_KEY | (value & ~POWER_ENGINE_RELEASE));

	/* Reestablishes exactly the same domain path before MMU and cache reinitialization. */
	error = connect_engine(v3d);
	if (error != 0)
		return error;
	error = bcm2711_clock_hz(5, &value);
	if (error != 0)
		return error;
	if (value == 0)
		return EIO;
	v3d->power.ready = true;

	/* Succeeded: reset propagation and both bridge acknowledgements were observed. */
	return 0;
}

/* Resolves a single-cell provider specifier and verifies its controller cell count. */
static int
provider(
	const struct drv_fdt *fdt,
	uint32_t node,
	const char *property,
	const char *cells,
	uint32_t *owner,
	uint32_t *number)
{
	const uint8_t *value;
	uint32_t length;
	uint32_t phandle;
	uint32_t count;
	int error;

	/* Power and reset contain one complete specifier, never a truncated or extra pair. */
	error = drv_fdt_property(fdt, node, property, &value, &length);
	if (error != 0)
		return error;
	if (length != 8U)
		return EINVAL;
	phandle = (uint32_t)drv_fdt_cells_value(value, 0, 1);
	*number = (uint32_t)drv_fdt_cells_value(value, 1, 1);
	error = drv_fdt_find_phandle(fdt, phandle, owner);
	if (error != 0)
		return error;
	count = drv_fdt_node_cells(fdt, *owner, cells, 0);
	if (count != 1)
		return ENOTSUP;

	/* Succeeded: both provider identity and specifier width are validated. */
	return 0;
}

/* Validates the first engine or parent V3D clock without requiring the misspelled alias. */
static int
validate_clock(
	const struct drv_fdt *fdt,
	uint32_t node,
	uint32_t *owner)
{
	const uint8_t *value;
	uint32_t length;
	uint32_t phandle;
	uint32_t number;
	uint32_t count;
	uint32_t index;
	int error;

	/* The known firmware provider has exactly one clock specifier cell. */
	error = drv_fdt_property(fdt, node, "clocks", &value, &length);
	if (error != 0)
		return error;
	if (length < 8U)
		return EINVAL;
	phandle = (uint32_t)drv_fdt_cells_value(value, 0, 1);
	number = (uint32_t)drv_fdt_cells_value(value, 1, 1);
	if (number != 5)
		return ENOTSUP;
	error = drv_fdt_find_phandle(fdt, phandle, owner);
	if (error != 0)
		return error;
	count = drv_fdt_node_cells(fdt, *owner, "#clock-cells", 0);
	if (count != 1)
		return ENOTSUP;
	error = bcm2711_fdt_string_index(fdt, *owner, "compatible", "raspberrypi,firmware-clocks", &index);
	if (error != 0)
		return ENOTSUP;

	/* Succeeded: native clock operations address the firmware's V3D-only clock. */
	return 0;
}

/* Sends a V3D clock state or rate request using the native firmware wire format. */
static int
clock_change(
	uint32_t tag,
	uint32_t value)
{
	uint32_t values[3];
	uint32_t answered;
	int error;

	/* The provider's third word leaves turbo policy unchanged. */
	values[0] = 5;
	values[1] = value;
	values[2] = 0;
	error = drv_rpi4_firmware_property(tag, values, 3, 3, &answered);
	if (error != 0)
		return error;
	if (answered < 8U || values[0] != 5)
		return EIO;

	/* Succeeded: the firmware accepted the engine clock request. */
	return 0;
}

/* Reads one provider clock value and rejects a response about a different clock. */
static int
clock_value(
	uint32_t tag,
	uint32_t *value)
{
	uint32_t values[2];
	int error;

	/* Captures limits even while the provider has the clock temporarily gated. */
	values[0] = 5;
	values[1] = 0;
	error = bcm2711_firmware_get(tag, values, 1, 2);
	if (error != 0)
		return error;
	if (values[0] != 5)
		return EIO;
	*value = values[1];

	/* Succeeded: the answer describes this engine's firmware clock. */
	return 0;
}

/* Establishes reset propagation and opens both engine AXI bridge directions. */
static int
connect_engine(
	struct bcm2711_v3d *v3d)
{
	uint32_t value;
	int error;

	/* A short clock pulse propagates reset before the PM reset-release transition. */
	error = clock_change(0x00038001U, 1);
	if (error != 0)
		return error;
	kern_usleep_range(1, 1);
	error = clock_change(0x00038001U, 0);
	if (error != 0)
		return error;
	value = kern_mmio_read32(v3d->power.control.mapped + POWER_GRAPHICS_OFFSET);
	kern_mmio_write32(v3d->power.control.mapped + POWER_GRAPHICS_OFFSET, POWER_WRITE_KEY | value | POWER_ENGINE_RELEASE);

	/* The clock must run before either bridge is permitted to carry engine traffic. */
	error = clock_change(0x00038001U, 1);
	if (error != 0)
		return error;
	error = bridge_change(v3d, POWER_BRIDGE_MASTER, true);
	if (error != 0)
		return error;
	error = bridge_change(v3d, POWER_BRIDGE_SLAVE, true);
	if (error != 0)
		return error;

	/* Succeeded: PM reset release and both bridge acknowledgements precede register access. */
	return 0;
}

/* Waits at most one microsecond for the requested AXI bridge acknowledgement. */
static int
bridge_change(
	struct bcm2711_v3d *v3d,
	uint32_t offset,
	bool enable)
{
	volatile uint8_t *address;
	uint32_t value;
	uint32_t expected;

	/* Changes only the engine's traffic-stop bit, preserving neighboring bridge state. */
	address = v3d->power.bridge.mapped + offset;
	value = kern_mmio_read32(address);
	expected = 2;
	if (enable) {
		value &= ~1U;
		expected = 0;
	} else {
		value |= 1U;
	}

	/* Password-protected writes take effect before either bounded acknowledgement sample. */
	kern_mmio_write32(address, POWER_WRITE_KEY | value);
	value = kern_mmio_read32(address);
	if ((value & 2U) == expected)
		return 0;
	kern_usleep_range(1, 1);
	value = kern_mmio_read32(address);
	if ((value & 2U) != expected)
		return ETIMEDOUT;

	/* Succeeded: the hardware observed the requested AXI traffic state. */
	return 0;
}

/* Maps one provider role, including its documented bridge identity within the same page. */
static int
map_provider_window(
	const struct drv_fdt *fdt,
	uint32_t node,
	const char *name,
	uint64_t minimum,
	struct bcm2711_window *window)
{
	int error;

	/* Requires the documented register extent from the PM binding. */
	error = bcm2711_fdt_named_window(fdt, node, name, window);
	if (error != 0)
		return error;
	if (window->size < minimum)
		return EINVAL;

	/* The fixed DT ends RPiVid at 0x20; its bridge ID at 0x20 shares the mapped page. */
	if (minimum == 0x20U && window->size == 0x20U) {
		/* Extends the binding by its known identity word, never by an arbitrary engine offset. */
		if ((window->physical & 4095U) != 0)
			return EINVAL;
		window->size = 0x24;
	}

	/* Maps provider registers only, without sampling the potentially gated V3D windows. */
	error = bcm2711_map_window(window);
	if (error != 0)
		return error;

	/* Succeeded: every provider register access lies inside this permanent mapping. */
	return 0;
}
