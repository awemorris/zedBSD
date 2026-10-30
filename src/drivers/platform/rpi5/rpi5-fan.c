/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Raspberry Pi 5's processor temperature and its fan connector.
 *
 * The BCM2712's AVS monitor reports the processor's temperature as a code
 * that the device tree's cpu-thermal zone turns into thousandths of a
 * degree.  The board's four-pin fan connector is driven by channel 3 of
 * RP1's second PWM block on RP1's GPIO 45; its speed is read back from a
 * register of the same block.  This file's thread reads the temperature
 * every polling period, pushes it to the thermal core, and sets the fan's
 * level from the zone's active trip points and cooling maps, as Linux's
 * pwm-fan driver does with the same tree.  A temperature that cannot be
 * read runs the fan at its highest level.
 */

#include <drivers/generic/fdt.h>
#include <drivers/platform/rpi5/rp1.h>
#include <drivers/platform/rpi5/rp1-gpio.h>
#include <drivers/platform/rpi5/rpi5-fan.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/pmem.h>
#include <kern/sched.h>
#include <kern/thermal.h>
#include <kern/thread.h>
#include <uapi/errno.h>

#include <stdint.h>

/*
 * The AVS monitor's temperature register: a 10-bit code, valid when
 * either valid bit is set.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y
 * drivers/thermal/broadcom/bcm2711_thermal.c (AVS_RO_TEMP_STATUS,
 * AVS_RO_TEMP_STATUS_VALID_MSK, AVS_RO_TEMP_STATUS_DATA_MSK).
 */
#define RPI5_AVS_TEMP_STATUS		0x200U
#define RPI5_AVS_TEMP_VALID_MASK	0x00010400U
#define RPI5_AVS_TEMP_DATA_MASK		0x000003ffU

/*
 * The cpu-thermal zone as the Pi 5's tree gives it, used when the tree
 * the firmware passed lacks a part: the code's slope and offset, the
 * polling period, four active trips with their hysteresis, and the
 * critical trip.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y
 * arch/arm64/boot/dts/broadcom/bcm2712-ds.dtsi (cpu-thermal:
 * coefficients <(-550) 450000>, polling-delay 1000, cpu-crit 110000) and
 * bcm2712-rpi-5-b.dts (cpu-tepid 50000, cpu-warm 60000, cpu-hot 67500,
 * cpu-vhot 75000, each with hysteresis 5000, mapped to fan levels 1 to 4).
 */
#define RPI5_THERMAL_SLOPE		(-550)
#define RPI5_THERMAL_OFFSET		450000
#define RPI5_THERMAL_PERIOD_MS		1000U
#define RPI5_THERMAL_HYSTERESIS		5000
#define RPI5_THERMAL_CRITICAL		110000

/* The shortest and longest polling periods taken from the tree. */
#define RPI5_THERMAL_PERIOD_MIN_MS	250U
#define RPI5_THERMAL_PERIOD_MAX_MS	5000U

/*
 * A temperature that makes sense, in thousandths of a degree: a code
 * outside it is taken as unreadable and runs the fan at its highest level.
 */
#define RPI5_THERMAL_LOW		(-40000)
#define RPI5_THERMAL_HIGH		150000

/* The most trips and fan levels taken from the tree. */
#define RPI5_FAN_TRIPS_MAX		8U
#define RPI5_FAN_LEVELS_MAX		8U

/*
 * The fan as the Pi 5's tree gives it, used when the tree lacks
 * cooling-levels: the duty of each level out of 255.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y
 * arch/arm64/boot/dts/broadcom/bcm2712-rpi-5-b.dts (cooling_fan:
 * cooling-levels <0 75 125 175 250>, pwms <&rp1_pwm1 3 41566
 * PWM_POLARITY_INVERTED>, rpm-offset 0x3c).
 */
#define RPI5_FAN_LEVEL_COUNT		5U
#define RPI5_FAN_LEVEL_FULL		255U

/*
 * The cells of the fan's pwms: the PWM block's phandle, the channel, the
 * period in nanoseconds and the flags, with the flag of an inverted
 * output.
 *
 * Value referenced: Linux include/dt-bindings/pwm/pwm.h
 * (PWM_POLARITY_INVERTED).
 */
#define RPI5_FAN_PWM_BLOCK_CELL		0U
#define RPI5_FAN_PWM_CHANNEL_CELL	1U
#define RPI5_FAN_PWM_PERIOD_CELL	2U
#define RPI5_FAN_PWM_FLAGS_CELL		3U
#define RPI5_FAN_PWM_INVERTED		0x00000001U

/*
 * The fan connector's PWM pin: RP1's GPIO 45 in its pwm1 function
 * (selector 0), pulled down.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y
 * arch/arm64/boot/dts/broadcom/rp1.dtsi (rp1_pwm1_gpio45: pins "gpio45",
 * function "pwm1", bias-pull-down) and drivers/pinctrl/pinctrl-rp1.c
 * (PIN(45, pwm1, ...): pwm1 is the first function).
 */
#define RPI5_FAN_PIN			45U
#define RPI5_FAN_PIN_FUNCTION		0U

/*
 * RP1's PWM block: the global control with a channel's enable bit and the
 * bit that latches new settings, and a channel's control (its mode, its
 * FIFO pop mask and its polarity), range and duty registers.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y drivers/pwm/pwm-rp1.c
 * (PWM_GLOBAL_CTRL, PWM_CHANNEL_CTRL(x), PWM_RANGE(x), PWM_DUTY(x),
 * PWM_CHANNEL_DEFAULT, PWM_POLARITY, SET_UPDATE, PWM_CHANNEL_ENABLE(x)).
 */
#define RP1_PWM_GLOBAL_CTRL		0x000U
#define RP1_PWM_GLOBAL_UPDATE		0x80000000U
#define RP1_PWM_CHANNELS		4U
#define RP1_PWM_CHANNEL_STRIDE		16U
#define RP1_PWM_CHANNEL_CTRL		0x014U
#define RP1_PWM_CHANNEL_RANGE		0x018U
#define RP1_PWM_CHANNEL_DUTY		0x020U
#define RP1_PWM_CHANNEL_DEFAULT		0x00000101U
#define RP1_PWM_CHANNEL_INVERTED	0x00000008U

/*
 * RP1's PWM clocks: their numbers in the tree's clocks, their control and
 * divider registers in the clock controller, the control's enable bit and
 * auxiliary source field, and the crystal's place among the auxiliary
 * sources and its rate.  A PWM clock runs from the crystal undivided, at
 * the 50 MHz the tree assigns.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y drivers/clk/clk-rp1.c
 * (CLK_PWM0_CTRL, CLK_PWM1_CTRL and their DIV_INT and DIV_FRAC,
 * CLK_CTRL_ENABLE, CLK_CTRL_AUXSRC_MASK, CLK_CTRL_AUXSRC_SHIFT, the
 * clk_pwm1 parents "", "pll_video_sec", "xosc"),
 * include/dt-bindings/clock/rp1.h (RP1_CLK_PWM0, RP1_CLK_PWM1) and
 * arch/arm64/boot/dts/broadcom/rp1.dtsi (pwm@9c000 assigned-clock-rates
 * 50000000; clk_xosc 50000000).
 */
#define RP1_CLK_PWM0			17U
#define RP1_CLK_PWM1			18U
#define RP1_CLK_PWM0_CTRL		0x00074U
#define RP1_CLK_PWM1_CTRL		0x00084U
#define RP1_CLK_DIV_INT			0x4U
#define RP1_CLK_DIV_FRAC		0x8U
#define RP1_CLK_CTRL_ENABLE		0x00000800U
#define RP1_CLK_CTRL_AUXSRC_MASK	0x000003e0U
#define RP1_CLK_CTRL_AUXSRC_SHIFT	5U
#define RP1_CLK_AUXSRC_XOSC		2U
#define RP1_PWM_CLOCK_HZ		50000000U

/* A second in nanoseconds. */
#define RPI5_NS_PER_SECOND		1000000000U

/* The polls after a level change before the fan's speed is logged. */
#define RPI5_FAN_SETTLE_POLLS		5U

/* The name of the sensor in the thermal core. */
#define RPI5_FAN_SENSOR_NAME		"bcm2712 cpu-thermal"

/*
 * One active trip of the zone: its node's phandle, the temperature that
 * crosses it and how far below that it must fall to be uncrossed, the fan
 * level it asks for, and whether the temperature has crossed it.
 */
struct rpi5_fan_trip {
	uint32_t phandle;
	int32_t temperature;
	int32_t hysteresis;
	unsigned level;
	int crossed;
};

/*
 * The temperature sensor and the fan.
 *
 * drv_rpi5_fan_start() fills everything before the thread starts; after
 * that only the thread changes the trips and the fan's level.  sensor is
 * NULL without a readable sensor, pwm NULL without a fan.  core_sensor is
 * the thermal core's handle the readings are pushed through, NULL when
 * the core refused the sensor.
 */
struct rpi5_fan_state {
	volatile uint8_t *sensor;
	int32_t slope;
	int32_t offset;
	int has_critical;
	int32_t critical;
	unsigned period_ms;
	struct rpi5_fan_trip trips[RPI5_FAN_TRIPS_MAX];
	unsigned trip_count;
	int trips_from_tree;
	volatile uint8_t *pwm;
	uint64_t pwm_size;
	unsigned channel;
	uint32_t period_ns;
	int inverted;
	uint32_t range;
	uint32_t levels[RPI5_FAN_LEVELS_MAX];
	unsigned level_count;
	int has_rpm;
	uint32_t rpm_offset;
	unsigned level;
	unsigned settle;
	int sensor_failed;
	struct kern_thermal_sensor *core_sensor;
};

/*
 * The fan levels of the Pi 5's tree, out of 255.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y
 * arch/arm64/boot/dts/broadcom/bcm2712-rpi-5-b.dts (cooling-levels).
 */
static const uint32_t rpi5_fan_default_levels[RPI5_FAN_LEVEL_COUNT] = {
	0U, 75U, 125U, 175U, 250U,
};

/*
 * The active trips of the Pi 5's tree: temperature and the level each
 * asks for.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y
 * arch/arm64/boot/dts/broadcom/bcm2712-rpi-5-b.dts (thermal_trips and
 * cooling_maps).
 */
static const int32_t rpi5_fan_default_trips[RPI5_FAN_LEVEL_COUNT - 1U] = {
	50000, 60000, 67500, 75000,
};

/* The board's one sensor and fan, started once by drv_rpi5_fan_start(). */
static struct rpi5_fan_state rpi5_fan;

static void set_defaults(void);
static int find_sensor(const struct drv_fdt *fdt, uint32_t *sensor_node);
static int read_cell(const struct drv_fdt *fdt, uint32_t node, const char *name, uint32_t index, uint32_t *value);
static void read_zone(const struct drv_fdt *fdt, uint32_t sensor_node, uint32_t fan_phandle);
static int find_zone(const struct drv_fdt *fdt, uint32_t sensor_node, uint32_t *zone);
static void read_trip(const struct drv_fdt *fdt, uint32_t node, struct rpi5_fan_trip *trips, unsigned *count);
static void read_map(const struct drv_fdt *fdt, uint32_t node, uint32_t fan_phandle, struct rpi5_fan_trip *trips, unsigned count);
static int find_fan(const struct drv_fdt *fdt, uint32_t *fan_phandle);
static int start_clock(const struct drv_fdt *fdt, uint32_t pwm_node);
static int start_pwm(void);
static int read_temperature(int32_t *milli_celsius, uint32_t *status);
static void record_temperature(int error, int32_t milli_celsius);
static unsigned choose_level(int32_t milli_celsius);
static uint32_t duty_count(unsigned level);
static void set_level(unsigned level);
static uint32_t read_rpm(void);
static void poll_once(void);
static void fan_thread(void *argument);

/*
 * Starts the temperature sensor and the fan.
 */
int
drv_rpi5_fan_start(
	void)
{
	const struct drv_fdt *fdt;
	struct thread *thread;
	uint32_t fan_phandle;
	uint32_t sensor_node;
	uint32_t status;
	int32_t milli_celsius;
	int error;

	/* The tree RP1 was found in; without RP1 there is no fan to drive. */
	fdt = drv_rp1_fdt();
	if (fdt == NULL) {
		kern_logf("rpi5-fan: no RP1; no fan or temperature\n");
		return ENODEV;
	}

	/* The Pi 5's own zone and fan until the tree says otherwise. */
	set_defaults();

	/* Finds the fan, which tells the zone's maps which device is its. */
	fan_phandle = 0;
	error = find_fan(fdt, &fan_phandle);
	if (error != 0)
		kern_logf("rpi5-fan: no fan (%d)\n", error);

	/* Finds the sensor and the zone's trips. */
	error = find_sensor(fdt, &sensor_node);
	if (error != 0) {
		kern_logf("rpi5-fan: no temperature sensor (%d)\n", error);
	} else {
		read_zone(fdt, sensor_node, fan_phandle);
	}

	/* Refuses a board with neither. */
	if (rpi5_fan.sensor == NULL && rpi5_fan.pwm == NULL)
		return ENODEV;

	/* Registers the sensor with the thermal core; each reading carries the critical level. */
	if (rpi5_fan.sensor != NULL) {
		error = kern_thermal_sensor_register(
			RPI5_FAN_SENSOR_NAME,
			KERN_THERMAL_KIND_ZONE,
			KERN_THERMAL_SENSOR_CPU,
			&rpi5_fan.core_sensor);
		if (error != 0)
			kern_logf("rpi5-fan: the temperature is not registered (%d)\n", error);
	}

	/* The first reading, and the fan's level from it (the highest without one). */
	error = read_temperature(&milli_celsius, &status);
	record_temperature(error, milli_celsius);
	if (rpi5_fan.pwm != NULL) {
		rpi5_fan.level = rpi5_fan.level_count - 1U;
		if (error == 0)
			rpi5_fan.level = choose_level(milli_celsius);
		set_level(rpi5_fan.level);
		rpi5_fan.settle = RPI5_FAN_SETTLE_POLLS;
	}

	/* Says what was found, in one line. */
	if (rpi5_fan.pwm == NULL) {
		kern_logf("rpi5-fan: %d mC (status %08x), no fan\n", (int)milli_celsius, (unsigned)status);
	} else {
		kern_logf("rpi5-fan: %d mC (status %08x), fan level %u of %u (duty %u of %u, channel %u), %u trips%s\n",
		    (int)milli_celsius, (unsigned)status, rpi5_fan.level, rpi5_fan.level_count - 1U,
		    (unsigned)duty_count(rpi5_fan.level), (unsigned)rpi5_fan.range, rpi5_fan.channel,
		    rpi5_fan.trip_count, rpi5_fan.trips_from_tree ? " from the device tree" : " built in");
	}

	/* Starts the thread that reads the sensor again and sets the fan; without it the fan runs full. */
	error = kthread_create(fan_thread, &rpi5_fan, SCHED_PRIORITY_DEFAULT, &thread);
	if (error != 0) {
		kern_logf("rpi5-fan: no thread (%d); fan full\n", error);
		if (rpi5_fan.pwm != NULL)
			set_level(rpi5_fan.level_count - 1U);
		return error;
	}
	thread_start(thread);

	/* Succeeded: the thread keeps the temperature and the fan. */
	return 0;
}

/* Fills in the Pi 5's own zone and fan levels. */
static void
set_defaults(
	void)
{
	unsigned index;

	/* The code's conversion, the period and the critical trip. */
	rpi5_fan.slope = RPI5_THERMAL_SLOPE;
	rpi5_fan.offset = RPI5_THERMAL_OFFSET;
	rpi5_fan.period_ms = RPI5_THERMAL_PERIOD_MS;
	rpi5_fan.has_critical = 1;
	rpi5_fan.critical = RPI5_THERMAL_CRITICAL;

	/* The four active trips, mapped to levels 1 to 4. */
	for (index = 0; index < RPI5_FAN_LEVEL_COUNT - 1U; index++) {
		rpi5_fan.trips[index].phandle = 0;
		rpi5_fan.trips[index].temperature = rpi5_fan_default_trips[index];
		rpi5_fan.trips[index].hysteresis = RPI5_THERMAL_HYSTERESIS;
		rpi5_fan.trips[index].level = index + 1U;
		rpi5_fan.trips[index].crossed = 0;
	}
	rpi5_fan.trip_count = RPI5_FAN_LEVEL_COUNT - 1U;
	rpi5_fan.trips_from_tree = 0;

	/* The five fan levels. */
	for (index = 0; index < RPI5_FAN_LEVEL_COUNT; index++)
		rpi5_fan.levels[index] = rpi5_fan_default_levels[index];
	rpi5_fan.level_count = RPI5_FAN_LEVEL_COUNT;
}

/* Finds the AVS monitor's temperature sensor and maps the monitor's registers. */
static int
find_sensor(
	const struct drv_fdt *fdt,
	uint32_t *sensor_node)
{
	uint64_t address;
	uint64_t size;
	uint32_t monitor;
	void *mapped;
	int error;

	/* The sensor node, under the AVS monitor whose registers it reads. */
	error = drv_fdt_find_compatible(fdt, "brcm,bcm2711-thermal", DRV_FDT_NO_NODE, sensor_node);
	if (error != 0)
		return ENODEV;
	error = drv_fdt_parent(fdt, *sensor_node, &monitor);
	if (error != 0)
		return error;

	/* The monitor's registers, which must hold the temperature register. */
	error = drv_fdt_reg(fdt, monitor, 0, &address, &size);
	if (error != 0)
		return error;
	if (size < RPI5_AVS_TEMP_STATUS + 4U)
		return ERANGE;

	/* Maps them uncached. */
	error = kern_device_map(address, (size_t)size, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0)
		return error;

	/* Succeeded: the temperature can be read. */
	rpi5_fan.sensor = mapped;
	return 0;
}

/* Reads one 32-bit cell of a node's property; ENOENT when it is shorter. */
static int
read_cell(
	const struct drv_fdt *fdt,
	uint32_t node,
	const char *name,
	uint32_t index,
	uint32_t *value)
{
	const uint8_t *bytes;
	uint32_t length;
	int error;

	/* The property, long enough for the cell. */
	error = drv_fdt_property(fdt, node, name, &bytes, &length);
	if (error != 0)
		return error;
	if (length / 4U <= index)
		return ENOENT;

	/* Succeeded: the cell's number. */
	*value = (uint32_t)drv_fdt_cells_value(bytes, index, 1U);
	return 0;
}

/*
 * Reads the sensor's zone: the conversion, the period, the critical trip,
 * and the active trips the zone's maps give the fan.  What the tree lacks
 * keeps the Pi 5's own values; the trips are replaced only when the maps
 * name the fan.
 */
static void
read_zone(
	const struct drv_fdt *fdt,
	uint32_t sensor_node,
	uint32_t fan_phandle)
{
	struct rpi5_fan_trip trips[RPI5_FAN_TRIPS_MAX];
	uint32_t zone;
	uint32_t group;
	uint32_t node;
	uint32_t value;
	unsigned count;
	unsigned mapped;
	unsigned index;
	int error;

	/* The zone that reads this sensor. */
	error = find_zone(fdt, sensor_node, &zone);
	if (error != 0)
		return;

	/* The code's slope and offset. */
	error = read_cell(fdt, zone, "coefficients", 0U, &value);
	if (error == 0)
		rpi5_fan.slope = (int32_t)value;
	error = read_cell(fdt, zone, "coefficients", 1U, &value);
	if (error == 0)
		rpi5_fan.offset = (int32_t)value;

	/* The polling period, within bounds. */
	error = read_cell(fdt, zone, "polling-delay", 0U, &value);
	if (error == 0) {
		if (value < RPI5_THERMAL_PERIOD_MIN_MS)
			value = RPI5_THERMAL_PERIOD_MIN_MS;
		if (value > RPI5_THERMAL_PERIOD_MAX_MS)
			value = RPI5_THERMAL_PERIOD_MAX_MS;
		rpi5_fan.period_ms = value;
	}

	/* The trips: the children of the zone's child that carry a temperature. */
	count = 0;
	group = DRV_FDT_NO_NODE;
	for (;;) {
		error = drv_fdt_child(fdt, zone, group, &group);
		if (error != 0)
			break;

		/* Each trip of this group. */
		node = DRV_FDT_NO_NODE;
		for (;;) {
			error = drv_fdt_child(fdt, group, node, &node);
			if (error != 0)
				break;
			read_trip(fdt, node, trips, &count);
		}
	}

	/* The maps: the children of the zone's child that carry a trip and a cooling device. */
	group = DRV_FDT_NO_NODE;
	for (;;) {
		error = drv_fdt_child(fdt, zone, group, &group);
		if (error != 0)
			break;

		/* Each map of this group. */
		node = DRV_FDT_NO_NODE;
		for (;;) {
			error = drv_fdt_child(fdt, group, node, &node);
			if (error != 0)
				break;
			read_map(fdt, node, fan_phandle, trips, count);
		}
	}

	/* Counts the trips a map gives the fan. */
	mapped = 0;
	for (index = 0; index < count; index++) {
		if (trips[index].level != 0U)
			mapped++;
	}

	/* Keeps the Pi 5's own trips when no map names the fan. */
	if (mapped == 0U)
		return;

	/* Takes the mapped trips, the critical one included when it is mapped. */
	rpi5_fan.trip_count = 0;
	for (index = 0; index < count; index++) {
		if (trips[index].level == 0U)
			continue;

		/* A level past the fan's last is its last. */
		if (trips[index].level >= rpi5_fan.level_count)
			trips[index].level = rpi5_fan.level_count - 1U;
		rpi5_fan.trips[rpi5_fan.trip_count] = trips[index];
		rpi5_fan.trip_count++;
	}

	/* The start-up line says the trips are the tree's. */
	rpi5_fan.trips_from_tree = 1;
}

/*
 * Finds the thermal zone whose thermal-sensors names the sensor: a
 * grandchild of the root (under /thermal-zones).
 */
static int
find_zone(
	const struct drv_fdt *fdt,
	uint32_t sensor_node,
	uint32_t *zone)
{
	uint32_t sensor_phandle;
	uint32_t named;
	uint32_t parent;
	uint32_t root;
	uint32_t group;
	uint32_t node;
	int error;

	/* The sensor's phandle, which a zone names it by. */
	error = read_cell(fdt, sensor_node, "phandle", 0U, &sensor_phandle);
	if (error != 0)
		return error;

	/* The root, above every node. */
	root = sensor_node;
	for (;;) {
		error = drv_fdt_parent(fdt, root, &parent);
		if (error != 0)
			break;
		root = parent;
	}

	/* Looks at each grandchild of the root for the sensor's zone. */
	group = DRV_FDT_NO_NODE;
	for (;;) {
		error = drv_fdt_child(fdt, root, group, &group);
		if (error != 0)
			return ENOENT;

		/* Each child of this group. */
		node = DRV_FDT_NO_NODE;
		for (;;) {
			error = drv_fdt_child(fdt, group, node, &node);
			if (error != 0)
				break;

			/* A zone that reads the sensor. */
			error = read_cell(fdt, node, "thermal-sensors", 0U, &named);
			if (error != 0)
				continue;
			if (named == sensor_phandle)
				break;
		}

		/* Stops at the group that holds it. */
		if (error == 0)
			break;
	}

	/* Succeeded: the zone. */
	*zone = node;
	return 0;
}

/*
 * Takes a node with a temperature as a trip: an active one is kept for
 * the maps, a critical one is the sensor's critical point and may also be
 * mapped.
 */
static void
read_trip(
	const struct drv_fdt *fdt,
	uint32_t node,
	struct rpi5_fan_trip *trips,
	unsigned *count)
{
	struct rpi5_fan_trip *trip;
	uint32_t temperature;
	uint32_t hysteresis;
	uint32_t phandle;
	unsigned index;
	int error;

	/* A trip has a temperature and a phandle the maps name it by. */
	error = read_cell(fdt, node, "temperature", 0U, &temperature);
	if (error != 0)
		return;
	error = read_cell(fdt, node, "phandle", 0U, &phandle);
	if (error != 0)
		return;

	/* Its hysteresis, none when absent. */
	error = read_cell(fdt, node, "hysteresis", 0U, &hysteresis);
	if (error != 0)
		hysteresis = 0;

	/* A critical trip is also the sensor's critical point. */
	error = drv_fdt_string_index(fdt, node, "type", "critical", &index);
	if (error == 0) {
		rpi5_fan.has_critical = 1;
		rpi5_fan.critical = (int32_t)temperature;
	}

	/* Refuses a trip past the room. */
	if (*count >= RPI5_FAN_TRIPS_MAX)
		return;

	/* Keeps it, asking for no level until a map names it. */
	trip = &trips[*count];
	trip->phandle = phandle;
	trip->temperature = (int32_t)temperature;
	trip->hysteresis = (int32_t)hysteresis;
	trip->level = 0;
	trip->crossed = 0;
	(*count)++;
}

/*
 * Takes a node with a trip and cooling devices as a map: the fan's entry
 * gives the trip the entry's upper level.
 */
static void
read_map(
	const struct drv_fdt *fdt,
	uint32_t node,
	uint32_t fan_phandle,
	struct rpi5_fan_trip *trips,
	unsigned count)
{
	const uint8_t *bytes;
	uint32_t trip_phandle;
	uint32_t device;
	uint32_t upper;
	uint32_t length;
	uint32_t cell;
	unsigned index;
	int error;

	/* A map names its trip and its devices; without a fan it has nothing to give. */
	if (fan_phandle == 0U)
		return;
	error = read_cell(fdt, node, "trip", 0U, &trip_phandle);
	if (error != 0)
		return;
	error = drv_fdt_property(fdt, node, "cooling-device", &bytes, &length);
	if (error != 0)
		return;

	/* Each device entry: the device's phandle, the lower and the upper level. */
	for (cell = 0; cell + 3U <= length / 4U; cell += 3U) {
		device = (uint32_t)drv_fdt_cells_value(bytes, cell, 1U);
		upper = (uint32_t)drv_fdt_cells_value(bytes, cell + 2U, 1U);
		if (device != fan_phandle)
			continue;

		/* Gives the trip the higher of its levels. */
		for (index = 0; index < count; index++) {
			if (trips[index].phandle != trip_phandle)
				continue;
			if (upper > trips[index].level)
				trips[index].level = upper;
		}
	}
}

/*
 * Finds the fan's node and its PWM, maps the PWM block and starts the
 * channel: its clock, its settings at the fan's highest level, and the
 * pin.
 */
static int
find_fan(
	const struct drv_fdt *fdt,
	uint32_t *fan_phandle)
{
	const uint8_t *bytes;
	uint32_t pwm_phandle;
	uint32_t pwm_node;
	uint32_t parent;
	uint32_t rp1_node;
	uint32_t length;
	uint32_t value;
	uint32_t node;
	unsigned index;
	int error;

	/* The fan node and the phandle the zone's maps name it by. */
	error = drv_fdt_find_compatible(fdt, "pwm-fan", DRV_FDT_NO_NODE, &node);
	if (error != 0)
		return ENODEV;
	error = read_cell(fdt, node, "phandle", 0U, fan_phandle);
	if (error != 0)
		*fan_phandle = 0;

	/* Its PWM: the block, the channel, the period and the polarity. */
	error = read_cell(fdt, node, "pwms", RPI5_FAN_PWM_BLOCK_CELL, &pwm_phandle);
	if (error != 0)
		return error;
	error = read_cell(fdt, node, "pwms", RPI5_FAN_PWM_FLAGS_CELL, &value);
	if (error != 0)
		return error;
	rpi5_fan.inverted = 0;
	if ((value & RPI5_FAN_PWM_INVERTED) != 0U)
		rpi5_fan.inverted = 1;
	error = read_cell(fdt, node, "pwms", RPI5_FAN_PWM_CHANNEL_CELL, &rpi5_fan.channel);
	if (error != 0)
		return error;
	if (rpi5_fan.channel >= RP1_PWM_CHANNELS)
		return EINVAL;
	error = read_cell(fdt, node, "pwms", RPI5_FAN_PWM_PERIOD_CELL, &rpi5_fan.period_ns);
	if (error != 0)
		return error;
	if (rpi5_fan.period_ns == 0U)
		return EINVAL;

	/* Its levels, when the tree gives them. */
	error = drv_fdt_property(fdt, node, "cooling-levels", &bytes, &length);
	if (error == 0 && length / 4U >= 2U) {
		rpi5_fan.level_count = length / 4U;
		if (rpi5_fan.level_count > RPI5_FAN_LEVELS_MAX)
			rpi5_fan.level_count = RPI5_FAN_LEVELS_MAX;

		/* Each level's duty, at most full. */
		for (index = 0; index < rpi5_fan.level_count; index++) {
			value = (uint32_t)drv_fdt_cells_value(bytes, index, 1U);
			if (value > RPI5_FAN_LEVEL_FULL)
				value = RPI5_FAN_LEVEL_FULL;
			rpi5_fan.levels[index] = value;
		}
	}

	/* Where its speed is read, within the PWM block. */
	rpi5_fan.has_rpm = 0;
	error = read_cell(fdt, node, "rpm-offset", 0U, &rpi5_fan.rpm_offset);
	if (error == 0)
		rpi5_fan.has_rpm = 1;

	/* Maps the PWM block, which must be one of RP1's peripherals. */
	error = drv_fdt_find_phandle(fdt, pwm_phandle, &pwm_node);
	if (error != 0)
		return error;
	error = drv_fdt_parent(fdt, pwm_node, &parent);
	if (error != 0)
		return error;
	rp1_node = drv_rp1_node();
	if (parent != rp1_node)
		return ENODEV;
	error = drv_rp1_map(pwm_node, 0, &rpi5_fan.pwm, &rpi5_fan.pwm_size);
	if (error != 0) {
		rpi5_fan.pwm = NULL;
		return error;
	}

	/* Refuses a block too small for the channel or the speed. */
	if (rpi5_fan.pwm_size < RP1_PWM_CHANNEL_DUTY + RP1_PWM_CHANNELS * RP1_PWM_CHANNEL_STRIDE) {
		rpi5_fan.pwm = NULL;
		return ERANGE;
	}

	/* A speed register outside the block, or not aligned, is not read. */
	if (rpi5_fan.rpm_offset + 4U > rpi5_fan.pwm_size)
		rpi5_fan.has_rpm = 0;
	if ((rpi5_fan.rpm_offset & 3U) != 0U)
		rpi5_fan.has_rpm = 0;

	/* Runs the block's clock at the rate the period is counted in. */
	error = start_clock(fdt, pwm_node);
	if (error != 0) {
		rpi5_fan.pwm = NULL;
		return error;
	}

	/* Starts the channel and hands it the pin. */
	error = start_pwm();
	if (error != 0) {
		rpi5_fan.pwm = NULL;
		return error;
	}

	/* Succeeded: the fan can be driven. */
	return 0;
}

/*
 * Runs the PWM block's clock from RP1's crystal undivided, as Linux's
 * assigned 50 MHz does, unless it already runs so.
 */
static int
start_clock(
	const struct drv_fdt *fdt,
	uint32_t pwm_node)
{
	volatile uint8_t *clocks;
	uint64_t size;
	uint32_t control_offset;
	uint32_t clock;
	uint32_t control;
	uint32_t divider;
	uint32_t fraction;
	uint32_t source;
	uint32_t node;
	int ready;
	int error;

	/* The block's clock, which must be one of the two PWM clocks. */
	error = read_cell(fdt, pwm_node, "clocks", 1U, &clock);
	if (error != 0)
		return error;
	if (clock == RP1_CLK_PWM0) {
		control_offset = RP1_CLK_PWM0_CTRL;
	} else if (clock == RP1_CLK_PWM1) {
		control_offset = RP1_CLK_PWM1_CTRL;
	} else {
		return ENODEV;
	}

	/* Maps RP1's clock controller. */
	error = drv_rp1_find_child("raspberrypi,rp1-clocks", DRV_FDT_NO_NODE, &node);
	if (error != 0)
		return ENODEV;
	error = drv_rp1_map(node, 0, &clocks, &size);
	if (error != 0)
		return error;
	if (size < control_offset + RP1_CLK_DIV_FRAC + 4U)
		return ERANGE;

	/* The clock as it is: on or off, its source and its divider. */
	control = kern_mmio_read32(clocks + control_offset);
	divider = kern_mmio_read32(clocks + control_offset + RP1_CLK_DIV_INT);
	fraction = kern_mmio_read32(clocks + control_offset + RP1_CLK_DIV_FRAC);
	source = (control & RP1_CLK_CTRL_AUXSRC_MASK) >> RP1_CLK_CTRL_AUXSRC_SHIFT;

	/* It is ready when on, from the crystal and undivided. */
	ready = 1;
	if ((control & RP1_CLK_CTRL_ENABLE) == 0U)
		ready = 0;
	if (source != RP1_CLK_AUXSRC_XOSC)
		ready = 0;
	if (divider != 1U)
		ready = 0;
	if (fraction != 0U)
		ready = 0;

	/* Leaves a ready clock as it is. */
	if (ready)
		return 0;

	/* Stops it, sets the divider to one and the crystal as its source, and starts it. */
	kern_logf("rpi5-fan: PWM clock was control %08x divider %u.%08x; now the crystal\n",
	    (unsigned)control, (unsigned)divider, (unsigned)fraction);
	control &= ~RP1_CLK_CTRL_ENABLE;
	kern_mmio_write32(clocks + control_offset, control);
	kern_mmio_write32(clocks + control_offset + RP1_CLK_DIV_INT, 1U);
	kern_mmio_write32(clocks + control_offset + RP1_CLK_DIV_FRAC, 0U);
	control &= ~RP1_CLK_CTRL_AUXSRC_MASK;
	control |= RP1_CLK_AUXSRC_XOSC << RP1_CLK_CTRL_AUXSRC_SHIFT;
	kern_mmio_write32(clocks + control_offset, control);
	control |= RP1_CLK_CTRL_ENABLE;
	kern_mmio_write32(clocks + control_offset, control);

	/* Succeeded: the clock runs at 50 MHz. */
	return 0;
}

/*
 * Puts the channel in its default mode at the fan's highest level and
 * connects the fan's pin to it.
 *
 * Procedure referenced: Raspberry Pi Linux rpi-6.12.y drivers/pwm/pwm-rp1.c
 * (rp1_pwm_request(), rp1_pwm_apply()) and pwm-fan.c.
 */
static int
start_pwm(
	void)
{
	uint32_t clock_period_ns;
	int error;

	/* The period in counts of the 50 MHz clock, rounded as Linux rounds it. */
	clock_period_ns = (RPI5_NS_PER_SECOND + RP1_PWM_CLOCK_HZ / 2U) / RP1_PWM_CLOCK_HZ;
	rpi5_fan.range = (rpi5_fan.period_ns + clock_period_ns / 2U) / clock_period_ns;
	if (rpi5_fan.range == 0U)
		return EINVAL;

	/* The channel's default mode, then its settings at the highest level. */
	kern_mmio_write32(rpi5_fan.pwm + RP1_PWM_CHANNEL_CTRL + rpi5_fan.channel * RP1_PWM_CHANNEL_STRIDE,
	    RP1_PWM_CHANNEL_DEFAULT);
	rpi5_fan.level = rpi5_fan.level_count - 1U;
	set_level(rpi5_fan.level);

	/* Connects the fan's pin to the PWM, pulled down. */
	error = drv_rp1_gpio_init();
	if (error != 0)
		return error;
	error = drv_rp1_gpio_set_pull(RPI5_FAN_PIN, DRV_RP1_GPIO_PULL_DOWN);
	if (error != 0)
		return error;
	error = drv_rp1_gpio_set_function(RPI5_FAN_PIN, RPI5_FAN_PIN_FUNCTION);
	if (error != 0)
		return error;

	/* Succeeded: the pin carries the channel. */
	return 0;
}

/* Reads the temperature in thousandths of a degree, with the register it came from. */
static int
read_temperature(
	int32_t *milli_celsius,
	uint32_t *status)
{
	int32_t temperature;
	uint32_t code;

	/* Nothing to read without the sensor. */
	*milli_celsius = 0;
	*status = 0;
	if (rpi5_fan.sensor == NULL)
		return ENODEV;

	/* The register, which must say the code is valid. */
	*status = kern_mmio_read32(rpi5_fan.sensor + RPI5_AVS_TEMP_STATUS);
	if ((*status & RPI5_AVS_TEMP_VALID_MASK) == 0U)
		return EIO;

	/* The code through the zone's conversion, which must make sense. */
	code = *status & RPI5_AVS_TEMP_DATA_MASK;
	temperature = rpi5_fan.slope * (int32_t)code + rpi5_fan.offset;
	if (temperature < RPI5_THERMAL_LOW || temperature > RPI5_THERMAL_HIGH)
		return EIO;

	/* Succeeded: the temperature. */
	*milli_celsius = temperature;
	return 0;
}

/*
 * Pushes a reading (or its absence) to the thermal core, with the zone's
 * critical level (none without one).
 */
static void
record_temperature(
	int error,
	int32_t milli_celsius)
{
	int32_t critical;
	bool valid;

	/* The critical level the zone has, 0 for none. */
	critical = 0;
	if (rpi5_fan.has_critical)
		critical = rpi5_fan.critical;
	kern_thermal_sensor_levels(rpi5_fan.core_sensor, 0, critical);

	/* The reading, valid when the sensor answered. */
	valid = false;
	if (error == 0)
		valid = true;
	kern_thermal_sensor_update(rpi5_fan.core_sensor, milli_celsius, valid);
}

/*
 * Updates which trips the temperature has crossed and gives the level the
 * highest crossed trip asks for.  A trip is crossed at its temperature and
 * stays crossed until the temperature falls below it by its hysteresis.
 */
static unsigned
choose_level(
	int32_t milli_celsius)
{
	struct rpi5_fan_trip *trip;
	unsigned level;
	unsigned index;

	/* Each trip's state, and the highest level asked for. */
	level = 0;
	for (index = 0; index < rpi5_fan.trip_count; index++) {
		trip = &rpi5_fan.trips[index];
		if (milli_celsius >= trip->temperature) {
			/* Reached: crossed. */
			trip->crossed = 1;
		} else if (milli_celsius < trip->temperature - trip->hysteresis) {
			/* Fallen past the hysteresis: no longer crossed. */
			trip->crossed = 0;
		}

		/* A crossed trip asks for its level. */
		if (trip->crossed && trip->level > level)
			level = trip->level;
	}

	/* A level past the fan's last is its last. */
	if (level >= rpi5_fan.level_count)
		level = rpi5_fan.level_count - 1U;

	/* Succeeded: the level for the temperature. */
	return level;
}

/*
 * Converts a level to the channel's duty in clock counts, as pwm-fan and
 * pwm-rp1 together do: the level out of 255 of the period less one,
 * rounded up, then rounded to the clock.
 */
static uint32_t
duty_count(
	unsigned level)
{
	uint32_t clock_period_ns;
	uint64_t duty_ns;

	/* The duty in nanoseconds. */
	duty_ns = (uint64_t)rpi5_fan.levels[level] * (rpi5_fan.period_ns - 1U);
	duty_ns = (duty_ns + RPI5_FAN_LEVEL_FULL - 1U) / RPI5_FAN_LEVEL_FULL;

	/* The duty in counts of the 50 MHz clock. */
	clock_period_ns = (RPI5_NS_PER_SECOND + RP1_PWM_CLOCK_HZ / 2U) / RP1_PWM_CLOCK_HZ;
	return (uint32_t)((duty_ns + clock_period_ns / 2U) / clock_period_ns);
}

/*
 * Sets the channel to a level: its duty, range and polarity, then on (or
 * off at a level of zero duty, as pwm-fan turns its PWM off), latched.
 *
 * Procedure referenced: Raspberry Pi Linux rpi-6.12.y drivers/pwm/pwm-rp1.c
 * (rp1_pwm_apply(), rp1_pwm_apply_config()) and drivers/hwmon/pwm-fan.c
 * (__set_pwm(), pwm_fan_power_off()).
 */
static void
set_level(
	unsigned level)
{
	volatile uint8_t *channel;
	uint32_t control;
	uint32_t global;

	/* The duty and the range. */
	channel = rpi5_fan.pwm + rpi5_fan.channel * RP1_PWM_CHANNEL_STRIDE;
	kern_mmio_write32(channel + RP1_PWM_CHANNEL_DUTY, duty_count(level));
	kern_mmio_write32(channel + RP1_PWM_CHANNEL_RANGE, rpi5_fan.range);

	/* The polarity. */
	control = kern_mmio_read32(channel + RP1_PWM_CHANNEL_CTRL);
	if (rpi5_fan.inverted) {
		control |= RP1_PWM_CHANNEL_INVERTED;
	} else {
		control &= ~RP1_PWM_CHANNEL_INVERTED;
	}
	kern_mmio_write32(channel + RP1_PWM_CHANNEL_CTRL, control);

	/* The channel on for a turning fan, off for a stopped one. */
	global = kern_mmio_read32(rpi5_fan.pwm + RP1_PWM_GLOBAL_CTRL);
	if (rpi5_fan.levels[level] != 0U) {
		global |= 1U << rpi5_fan.channel;
	} else {
		global &= ~(1U << rpi5_fan.channel);
	}
	kern_mmio_write32(rpi5_fan.pwm + RP1_PWM_GLOBAL_CTRL, global);

	/* Latches the new settings. */
	global = kern_mmio_read32(rpi5_fan.pwm + RP1_PWM_GLOBAL_CTRL);
	global |= RP1_PWM_GLOBAL_UPDATE;
	kern_mmio_write32(rpi5_fan.pwm + RP1_PWM_GLOBAL_CTRL, global);
}

/* Reads the fan's speed in revolutions per minute; zero without the register. */
static uint32_t
read_rpm(
	void)
{
	uint32_t rpm;

	/* Nothing to read without the register. */
	if (!rpi5_fan.has_rpm)
		return 0;

	/* Succeeded: the speed the tachometer measured. */
	rpm = kern_mmio_read32(rpi5_fan.pwm + rpi5_fan.rpm_offset);
	return rpm;
}

/* Reads the temperature once, keeps it, and sets the fan from it. */
static void
poll_once(
	void)
{
	int32_t milli_celsius;
	uint32_t status;
	unsigned level;
	int error;

	/* The reading, kept for the sysctl. */
	error = read_temperature(&milli_celsius, &status);
	record_temperature(error, milli_celsius);

	/* Nothing more to do without a fan. */
	if (rpi5_fan.pwm == NULL)
		return;

	/* The level: the highest when the temperature cannot be read. */
	if (error != 0) {
		level = rpi5_fan.level_count - 1U;
		if (!rpi5_fan.sensor_failed)
			kern_logf("rpi5-fan: temperature unreadable (%d, status %08x); fan full\n", error, (unsigned)status);
		rpi5_fan.sensor_failed = 1;
	} else {
		level = choose_level(milli_celsius);
		rpi5_fan.sensor_failed = 0;
	}

	/* A new level is set and said, and the speed said once it has settled. */
	if (level != rpi5_fan.level) {
		set_level(level);
		kern_logf("rpi5-fan: %d mC, level %u to %u (duty %u of %u)\n", (int)milli_celsius, rpi5_fan.level, level,
		    (unsigned)duty_count(level), (unsigned)rpi5_fan.range);
		rpi5_fan.level = level;
		rpi5_fan.settle = RPI5_FAN_SETTLE_POLLS;
	} else if (rpi5_fan.settle != 0U) {
		rpi5_fan.settle--;
		if (rpi5_fan.settle == 0U && rpi5_fan.has_rpm)
			kern_logf("rpi5-fan: level %u at %u rpm\n", rpi5_fan.level, (unsigned)read_rpm());
	}
}

/* Reads the sensor and sets the fan every polling period, for as long as the kernel runs. */
static void
fan_thread(
	void *argument)
{
	uint64_t deadline;

	/* No argument: the board has one sensor and one fan. */
	(void)argument;

	/* For as long as the kernel runs. */
	for (;;) {
		/* Sleeps for the period. */
		deadline = sched_ticks() + kern_ms_to_ticks(rpi5_fan.period_ms);
		sched_sleep(deadline);

		/* Reads and sets again. */
		poll_once();
	}
}
