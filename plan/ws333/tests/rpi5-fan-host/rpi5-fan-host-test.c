/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the Raspberry Pi 5's fan as the thermal core's fan,
 * receiver of the strategy and boost (src/drivers/platform/rpi5/rpi5-fan.c).
 *
 * The driver's file is compiled into this one, so the test calls its
 * static functions directly and runs its thread's work one poll at a time.
 * The device trees are real ones built by dtc (zone.dtsi and the trees
 * beside it) and read by the kernel's own FDT reader.  The AVS monitor's
 * temperature register, RP1's PWM block and clock controller are arrays;
 * the GPIO, the threads and the thermal core are models that record what
 * the driver asked of them.  Each scenario starts the driver afresh.
 */

/* The kernel's sched_yield() is not the host's; the kernel's is renamed while its headers are read. */
#define sched_yield test_kernel_sched_yield
#include <drivers/platform/rpi5/rpi5-fan.c>
#undef sched_yield

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The tree's conversion: one degree per code, 500 degrees at code 0. */
#define TEST_OFFSET		500000
#define TEST_SLOPE		1000

/* The PWM channel, its period and the fan's levels in every test tree. */
#define TEST_CHANNEL		3U
#define TEST_PERIOD_NS		41566U
#define TEST_LEVELS		5U

/* The speed the tachometer register holds. */
#define TEST_RPM		1500U

static const uint32_t test_level_duty[TEST_LEVELS] = { 0U, 75U, 125U, 175U, 250U };

static int test_verbose;
static unsigned test_checks;
static unsigned test_failures;

#define CHECK(condition) test_check((condition) ? 1 : 0, #condition, __LINE__)

static void
test_check(int ok, const char *text, int line)
{
	test_checks++;
	if (!ok) {
		test_failures++;
		printf("FAIL line %d: %s\n", line, text);
		fflush(stdout);
	}
}

/* ---- The hardware: the AVS monitor, RP1's PWM block and clocks. ---- */

static uint32_t test_avs[0x1000 / 4];
static uint32_t test_pwm[0x100 / 4];
static uint32_t test_clocks[0x10038 / 4 + 1];

/* The FDT the driver reads, and whether RP1 is there at all. */
static uint8_t test_blob[1 << 16];
static struct drv_fdt test_fdt;
static int test_has_rp1;

/* What the GPIO was asked. */
static int test_gpio_pin = -1;
static int test_gpio_function = -1;
static int test_gpio_pull = -1;

/* The lock's depth, and the PWM writes and pushes made where the lock rules say they must not be. */
static int test_lock_depth;
static int test_registered;
static unsigned test_pwm_writes_unlocked;
static unsigned test_push_locked;

/* The thermal core's model. */
static int test_sensor_registered;
static char test_sensor_name[64];
static uint32_t test_sensor_kind;
static uint32_t test_sensor_flags;
static int32_t test_sensor_value;
static int test_sensor_valid;
static unsigned test_sensor_updates;
static int32_t test_sensor_passive;
static int32_t test_sensor_critical;
static int test_fan_registered;
static char test_fan_name[64];
static const struct kern_thermal_fan_ops *test_fan_ops;
static void *test_fan_context;
static int test_receiver_registered;
static char test_receiver_name[64];
static const struct kern_thermal_receiver_ops *test_receiver_ops;
static void *test_receiver_context;
static int test_thread_created;
static int test_thread_started;
static struct kern_thermal_sensor *test_sensor_handle = (struct kern_thermal_sensor *)&test_sensor_value;

void
kern_logf(const char *format, ...)
{
	va_list arguments;

	if (!test_verbose)
		return;
	va_start(arguments, format);
	vprintf(format, arguments);
	va_end(arguments);
}

uint32_t
kern_mmio_read32(const volatile void *address)
{
	return *(const volatile uint32_t *)address;
}

void
kern_mmio_write32(volatile void *address, uint32_t value)
{
	const volatile uint8_t *byte;

	/* Once the fan is the core's, every write to the PWM block is under the lock. */
	byte = address;
	if (test_registered &&
	    byte >= (const volatile uint8_t *)test_pwm &&
	    byte < (const volatile uint8_t *)test_pwm + sizeof(test_pwm) &&
	    test_lock_depth == 0)
		test_pwm_writes_unlocked++;
	*(volatile uint32_t *)address = value;
}

int
kern_device_map(uint64_t address, size_t size, unsigned attributes, void **mapped)
{
	(void)attributes;
	CHECK(address == 0x7d542000ULL);
	CHECK(size == 0xf00U);
	*mapped = test_avs;
	return 0;
}

void
spin_init(struct spinlock *lock, enum lock_rank rank, const char *name)
{
	(void)lock;
	(void)rank;
	(void)name;
}

unsigned long
spin_lock_irqsave(struct spinlock *lock)
{
	(void)lock;
	CHECK(test_lock_depth == 0);
	test_lock_depth++;
	return 1UL;
}

void
spin_unlock_irqrestore(struct spinlock *lock, unsigned long enabled)
{
	(void)lock;
	CHECK(enabled == 1UL);
	CHECK(test_lock_depth == 1);
	test_lock_depth--;
}

uint64_t
sched_ticks(void)
{
	return 0;
}

uint64_t
kern_ms_to_ticks(uint64_t milliseconds)
{
	return milliseconds;
}

void
sched_sleep(uint64_t timeout_tick)
{
	(void)timeout_tick;
}

int
kthread_create(void (*entry)(void *), void *arg, int priority, struct thread **result)
{
	(void)arg;
	(void)priority;
	CHECK(entry == fan_thread);
	test_thread_created = 1;
	*result = (struct thread *)&test_thread_created;
	return 0;
}

void
thread_start(struct thread *thread)
{
	(void)thread;
	test_thread_started = 1;
}

const struct drv_fdt *
drv_rp1_fdt(void)
{
	if (!test_has_rp1)
		return NULL;
	return &test_fdt;
}

uint32_t
drv_rp1_node(void)
{
	uint32_t node;
	int error;

	error = drv_fdt_find_compatible(&test_fdt, "pci1de4,1", DRV_FDT_NO_NODE, &node);
	CHECK(error == 0);
	return node;
}

int
drv_rp1_find_child(const char *compatible, uint32_t after, uint32_t *node)
{
	return drv_fdt_find_compatible(&test_fdt, compatible, after, node);
}

int
drv_rp1_map(uint32_t node, unsigned index, volatile uint8_t **mapped, uint64_t *size)
{
	uint32_t pwm;
	uint32_t clocks;
	int error;

	CHECK(index == 0U);
	error = drv_fdt_find_compatible(&test_fdt, "raspberrypi,rp1-pwm", DRV_FDT_NO_NODE, &pwm);
	CHECK(error == 0);
	error = drv_fdt_find_compatible(&test_fdt, "raspberrypi,rp1-clocks", DRV_FDT_NO_NODE, &clocks);
	CHECK(error == 0);
	if (node == pwm) {
		*mapped = (volatile uint8_t *)test_pwm;
		*size = sizeof(test_pwm);
		return 0;
	}
	if (node == clocks) {
		*mapped = (volatile uint8_t *)test_clocks;
		*size = 0x10038U;
		return 0;
	}
	return ENODEV;
}

int
drv_rp1_gpio_init(void)
{
	return 0;
}

int
drv_rp1_gpio_set_function(unsigned pin, unsigned function)
{
	test_gpio_pin = (int)pin;
	test_gpio_function = (int)function;
	return 0;
}

int
drv_rp1_gpio_set_pull(unsigned pin, unsigned pull)
{
	CHECK(pin == 45U);
	test_gpio_pull = (int)pull;
	return 0;
}

int
kern_thermal_sensor_register(const char *name, uint32_t kind, uint32_t flags, struct kern_thermal_sensor **sensor)
{
	test_sensor_registered = 1;
	snprintf(test_sensor_name, sizeof(test_sensor_name), "%s", name);
	test_sensor_kind = kind;
	test_sensor_flags = flags;
	*sensor = test_sensor_handle;
	return 0;
}

void
kern_thermal_sensor_update(struct kern_thermal_sensor *sensor, int32_t millicelsius, bool valid)
{
	CHECK(sensor == test_sensor_handle);
	if (test_lock_depth != 0)
		test_push_locked++;
	test_sensor_value = millicelsius;
	test_sensor_valid = valid ? 1 : 0;
	test_sensor_updates++;
}

void
kern_thermal_sensor_levels(struct kern_thermal_sensor *sensor, int32_t passive, int32_t critical)
{
	CHECK(sensor == test_sensor_handle);
	if (test_lock_depth != 0)
		test_push_locked++;
	test_sensor_passive = passive;
	test_sensor_critical = critical;
}

int
kern_thermal_fan_register(const char *name, uint32_t rpm_max, const struct kern_thermal_fan_ops *ops, void *context)
{
	(void)rpm_max;
	CHECK(test_lock_depth == 0);
	test_fan_registered = 1;
	snprintf(test_fan_name, sizeof(test_fan_name), "%s", name);
	test_fan_ops = ops;
	test_fan_context = context;
	test_registered = 1;
	return 0;
}

int
kern_thermal_receiver_register(const char *name, const struct kern_thermal_receiver_ops *ops, void *context)
{
	CHECK(test_lock_depth == 0);
	CHECK(ops->apply != NULL);
	test_receiver_registered = 1;
	snprintf(test_receiver_name, sizeof(test_receiver_name), "%s", name);
	test_receiver_ops = ops;
	test_receiver_context = context;
	return 0;
}

/* ---- Helpers. ---- */

static const char *test_dir;

/* Loads a tree built by run.sh and clears every model and the driver's state. */
static void
test_reset(const char *tree, int has_rp1)
{
	char path[1024];
	FILE *file;
	size_t length;
	int error;

	memset(test_avs, 0, sizeof(test_avs));
	memset(test_pwm, 0, sizeof(test_pwm));
	memset(test_clocks, 0, sizeof(test_clocks));
	memset(&rpi5_fan, 0, sizeof(rpi5_fan));
	test_gpio_pin = -1;
	test_gpio_function = -1;
	test_gpio_pull = -1;
	test_lock_depth = 0;
	test_registered = 0;
	test_pwm_writes_unlocked = 0;
	test_push_locked = 0;
	test_sensor_registered = 0;
	test_sensor_updates = 0;
	test_sensor_valid = 0;
	test_sensor_critical = -1;
	test_sensor_passive = -1;
	test_fan_registered = 0;
	test_fan_ops = NULL;
	test_receiver_registered = 0;
	test_receiver_ops = NULL;
	test_thread_created = 0;
	test_thread_started = 0;
	test_has_rp1 = has_rp1;

	snprintf(path, sizeof(path), "%s/%s.dtb", test_dir, tree);
	file = fopen(path, "rb");
	if (file == NULL) {
		printf("FAIL: cannot open %s\n", path);
		exit(2);
	}
	length = fread(test_blob, 1, sizeof(test_blob), file);
	fclose(file);
	error = drv_fdt_open(&test_fdt, test_blob, length);
	if (error != 0) {
		printf("FAIL: %s is not a tree (%d)\n", path, error);
		exit(2);
	}

	/* The tachometer reads a speed. */
	test_pwm[0x3c / 4] = TEST_RPM;
}

/* Sets the temperature register to a valid code for a temperature in whole degrees. */
static void
test_set_temperature(int32_t millicelsius)
{
	uint32_t code;

	code = (uint32_t)((TEST_OFFSET - millicelsius) / TEST_SLOPE);
	test_avs[0x200 / 4] = 0x00000400U | (code & 0x3ffU);
}

/* Makes the temperature register say its code is not valid. */
static void
test_set_unreadable(void)
{
	test_avs[0x200 / 4] = 0x000003ffU;
}

/* The duty the channel should carry at a level, in 50 MHz counts, worked out apart from the driver. */
static uint32_t
test_duty(unsigned level)
{
	uint64_t duty_ns;

	duty_ns = ((uint64_t)test_level_duty[level] * (TEST_PERIOD_NS - 1U) + 254U) / 255U;
	return (uint32_t)((duty_ns + 10U) / 20U);
}

/* The level the channel is set at, from its duty; -1 when it matches none or its enable disagrees. */
static int
test_channel_level(void)
{
	uint32_t duty;
	uint32_t enabled;
	unsigned level;

	duty = test_pwm[(0x20 + TEST_CHANNEL * 16U) / 4];
	enabled = test_pwm[0] & (1U << TEST_CHANNEL);
	for (level = 0; level < TEST_LEVELS; level++) {
		if (test_duty(level) != duty)
			continue;
		if ((test_level_duty[level] != 0U) != (enabled != 0U))
			return -1;
		return (int)level;
	}
	return -1;
}

/* Runs one poll of the fan's thread at a temperature. */
static void
test_poll(int32_t millicelsius)
{
	test_set_temperature(millicelsius);
	poll_once();
	CHECK(test_lock_depth == 0);
}

/* Reads the fan as the core's poll thread does. */
static struct kern_thermal_fan_state
test_read(void)
{
	struct kern_thermal_fan_state state;
	int error;

	memset(&state, 0, sizeof(state));
	error = test_fan_ops->read(test_fan_context, &state);
	CHECK(error == 0);
	return state;
}

/* Hands a strategy to the receiver as the core's strategy thread does. */
static int
test_apply(uint32_t value, uint8_t speed, uint8_t noise)
{
	struct kern_thermal_strategy strategy;
	uint32_t flags;
	int error;

	memset(&strategy, 0, sizeof(strategy));
	strategy.value = value;
	strategy.speed = speed;
	strategy.noise = noise;
	flags = 0;
	error = test_receiver_ops->apply(test_receiver_context, &strategy, &flags);
	CHECK(flags == 0U);
	CHECK(test_lock_depth == 0);
	return error;
}

/* The receiver's name of the trips in force. */
static const char *
test_trips_name(void)
{
	static char name[KERN_THERMAL_NAME_MAX];
	int error;

	memset(name, 0, sizeof(name));
	error = test_receiver_ops->firmware_name(test_receiver_context, name, sizeof(name));
	if (error != 0)
		return "";
	return name;
}

/* Sets the boost as the core's strategy thread does. */
static int
test_boost(uint32_t level)
{
	int error;

	error = test_fan_ops->boost(test_fan_context, level);
	CHECK(test_lock_depth == 0);
	return error;
}

/*
 * A model of the trips apart from the driver: the full tree's four trips,
 * lowered by a shift, each crossed at its temperature and uncrossed 4
 * degrees below it.
 */
struct test_model {
	int crossed[4];
};

static unsigned
test_model_level(struct test_model *model, int32_t millicelsius, int32_t shift)
{
	static const int32_t trips[4] = { 40000, 50000, 60000, 70000 };
	unsigned level;
	unsigned index;

	level = 0;
	for (index = 0; index < 4U; index++) {
		if (millicelsius >= trips[index] - shift)
			model->crossed[index] = 1;
		else if (millicelsius < trips[index] - shift - 4000)
			model->crossed[index] = 0;
		if (model->crossed[index] && index + 1U > level)
			level = index + 1U;
	}
	return level;
}

/* ---- Scenarios. ---- */

/* Starts the driver on the full tree at a temperature. */
static void
test_start(const char *tree, int32_t millicelsius)
{
	int error;

	test_reset(tree, 1);
	test_set_temperature(millicelsius);
	error = drv_rpi5_fan_start();
	CHECK(error == 0);
	CHECK(test_lock_depth == 0);
}

/* The start: the tree's zone and fan, the registrations, the clock, the pin and the channel. */
static void
scenario_start(void)
{
	struct kern_thermal_fan_state state;
	uint32_t control;

	test_start("full", 30000);

	/* The sensor: a CPU zone, its critical level from the tree, the first reading pushed. */
	CHECK(test_sensor_registered);
	CHECK(strcmp(test_sensor_name, "bcm2712 cpu-thermal") == 0);
	CHECK(test_sensor_kind == KERN_THERMAL_KIND_ZONE);
	CHECK(test_sensor_flags == KERN_THERMAL_SENSOR_CPU);
	CHECK(test_sensor_critical == 100000);
	CHECK(test_sensor_passive == 0);
	CHECK(test_sensor_value == 30000);
	CHECK(test_sensor_valid == 1);

	/* The tree's zone: the conversion, the period and the four mapped trips. */
	CHECK(rpi5_fan.slope == -1000);
	CHECK(rpi5_fan.offset == 500000);
	CHECK(rpi5_fan.period_ms == 1000U);
	CHECK(rpi5_fan.trips_from_tree == 1);
	CHECK(rpi5_fan.trip_count == 4U);
	CHECK(rpi5_fan.level_count == TEST_LEVELS);

	/* The fan and the receiver, with their names, and the thread. */
	CHECK(test_fan_registered);
	CHECK(strcmp(test_fan_name, "rpi5-fan") == 0);
	CHECK(test_fan_ops != NULL && test_fan_ops->boost != NULL);
	CHECK(test_receiver_registered);
	CHECK(strcmp(test_receiver_name, "rpi5-fan") == 0);
	CHECK(test_receiver_ops->limit == NULL);
	CHECK(test_receiver_ops->firmware_name != NULL);
	CHECK(test_thread_created && test_thread_started);

	/* The PWM clock from the crystal undivided, the pin on the PWM pulled down. */
	control = test_clocks[0x84 / 4];
	CHECK((control & 0x800U) != 0U);
	CHECK(((control & 0x3e0U) >> 5) == 2U);
	CHECK(test_clocks[(0x84 + 4) / 4] == 1U);
	CHECK(test_gpio_pin == 45);
	CHECK(test_gpio_function == 0);
	CHECK(test_gpio_pull == DRV_RP1_GPIO_PULL_DOWN);

	/* The channel: inverted, the range of the period, at level 0 (stopped). */
	CHECK((test_pwm[(0x14 + TEST_CHANNEL * 16U) / 4] & 0x8U) != 0U);
	CHECK(test_pwm[(0x18 + TEST_CHANNEL * 16U) / 4] == 2078U);
	CHECK(test_channel_level() == 0);

	/* The core's read: the level, the levels, the boost's range and the speed. */
	state = test_read();
	CHECK(state.flags == (KERN_THERMAL_FAN_HAS_LEVEL | KERN_THERMAL_FAN_HAS_RPM));
	CHECK(state.rpm == TEST_RPM);
	CHECK(state.level == 0U);
	CHECK(state.level_count == TEST_LEVELS);
	CHECK(state.boost_count == TEST_LEVELS - 1U);
	CHECK(strcmp(test_trips_name(), "trips 40000-70000 mC") == 0);
}

/* The trips: up and down with the hysteresis, the channel and the read follow. */
static void
scenario_trips(void)
{
	struct kern_thermal_fan_state state;
	struct test_model model;
	int32_t temperature;
	unsigned expected;

	test_start("full", 30000);
	memset(&model, 0, sizeof(model));
	(void)test_model_level(&model, 30000, 0);

	/* Up from 30 to 80 degrees and down again, a degree at a time. */
	for (temperature = 30000; temperature <= 80000; temperature += 1000) {
		test_poll(temperature);
		expected = test_model_level(&model, temperature, 0);
		CHECK(test_channel_level() == (int)expected);
		state = test_read();
		CHECK(state.level == expected);
		CHECK(test_sensor_value == temperature);
	}
	for (temperature = 80000; temperature >= 30000; temperature -= 1000) {
		test_poll(temperature);
		expected = test_model_level(&model, temperature, 0);
		CHECK(test_channel_level() == (int)expected);
	}

	/* Spot checks of the hysteresis against the tree's numbers. */
	test_poll(61000);
	CHECK(test_channel_level() == 3);
	test_poll(56000);
	CHECK(test_channel_level() == 3);
	test_poll(55000);
	CHECK(test_channel_level() == 2);
	test_poll(70000);
	CHECK(test_channel_level() == 4);
	test_poll(66000);
	CHECK(test_channel_level() == 4);
	test_poll(65000);
	CHECK(test_channel_level() == 3);
	CHECK(test_pwm_writes_unlocked == 0U);
	CHECK(test_push_locked == 0U);
}

/* The strategy: the trips it chooses, at once, and the critical level never moves. */
static void
scenario_strategy(void)
{
	int error;

	test_start("full", 37000);
	CHECK(test_channel_level() == 0);

	/* The ordinary strategies keep the tree's trips. */
	error = test_apply(KERN_THERMAL_STRATEGY_BALANCED, 0, 0);
	CHECK(error == 0);
	CHECK(test_channel_level() == 0);
	CHECK(strcmp(test_trips_name(), "trips 40000-70000 mC") == 0);

	/* A loud manual strategy brings the trips 5 degrees lower and the fan turns at once. */
	error = test_apply(KERN_THERMAL_STRATEGY_MANUAL, 50, 80);
	CHECK(error == 0);
	CHECK(strcmp(test_trips_name(), "trips 35000-65000 mC") == 0);
	CHECK(test_channel_level() == 1);
	CHECK(test_read().level == 1U);
	test_poll(46000);
	CHECK(test_channel_level() == 2);

	/* The edge of loud: 67 moves the trips, 66 does not. */
	error = test_apply(KERN_THERMAL_STRATEGY_MANUAL, 50, 66);
	CHECK(error == 0);
	CHECK(strcmp(test_trips_name(), "trips 40000-70000 mC") == 0);
	error = test_apply(KERN_THERMAL_STRATEGY_MANUAL, 50, 67);
	CHECK(error == 0);
	CHECK(strcmp(test_trips_name(), "trips 35000-65000 mC") == 0);

	/* Performance, powersave and a quiet manual strategy are the tree's trips; the fan slows by the hysteresis. */
	error = test_apply(KERN_THERMAL_STRATEGY_PERFORMANCE, 0, 0);
	CHECK(error == 0);
	CHECK(strcmp(test_trips_name(), "trips 40000-70000 mC") == 0);
	test_poll(45000);
	CHECK(test_channel_level() == 1);
	error = test_apply(KERN_THERMAL_STRATEGY_POWERSAVE, 0, 0);
	CHECK(error == 0);
	CHECK(strcmp(test_trips_name(), "trips 40000-70000 mC") == 0);
	error = test_apply(KERN_THERMAL_STRATEGY_MANUAL, 0, 0);
	CHECK(error == 0);
	CHECK(strcmp(test_trips_name(), "trips 40000-70000 mC") == 0);

	/* The firmware's state writes nothing: a shift in force stays. */
	error = test_apply(KERN_THERMAL_STRATEGY_MANUAL, 0, 100);
	CHECK(error == 0);
	error = test_apply(KERN_THERMAL_STRATEGY_FIRMWARE, 0, 0);
	CHECK(error == 0);
	CHECK(strcmp(test_trips_name(), "trips 35000-65000 mC") == 0);

	/* The critical level the guard watches is the tree's whatever the strategy. */
	test_poll(50000);
	CHECK(test_sensor_critical == 100000);
	CHECK(test_pwm_writes_unlocked == 0U);
	CHECK(test_push_locked == 0U);
}

/* No strategy makes the fan slower than the tree's trips, up and down, a half degree at a time (the code rounds to a degree). */
static void
scenario_never_slower(void)
{
	static const struct {
		uint32_t value;
		uint8_t speed;
		uint8_t noise;
		int32_t shift;
	} strategies[] = {
		{ KERN_THERMAL_STRATEGY_PERFORMANCE, 0, 0, 0 },
		{ KERN_THERMAL_STRATEGY_BALANCED, 0, 0, 0 },
		{ KERN_THERMAL_STRATEGY_POWERSAVE, 0, 0, 0 },
		{ KERN_THERMAL_STRATEGY_MANUAL, 100, 0, 0 },
		{ KERN_THERMAL_STRATEGY_MANUAL, 0, 33, 0 },
		{ KERN_THERMAL_STRATEGY_MANUAL, 100, 66, 0 },
		{ KERN_THERMAL_STRATEGY_MANUAL, 0, 67, 5000 },
		{ KERN_THERMAL_STRATEGY_MANUAL, 50, 100, 5000 },
	};
	struct test_model tree;
	struct test_model shifted;
	int32_t temperature;
	unsigned tree_level;
	unsigned shifted_level;
	unsigned index;
	int error;

	/* Each strategy from a fresh start at 20 degrees. */
	for (index = 0; index < sizeof(strategies) / sizeof(strategies[0]); index++) {
		test_start("full", 20000);
		memset(&tree, 0, sizeof(tree));
		memset(&shifted, 0, sizeof(shifted));
		error = test_apply(strategies[index].value, strategies[index].speed, strategies[index].noise);
		CHECK(error == 0);

		/* Up from 20 to 90 degrees and down again; the model takes the temperature the code gives. */
		for (temperature = 20000; temperature <= 90000; temperature += 500) {
			test_poll(temperature);
			tree_level = test_model_level(&tree, test_sensor_value, 0);
			shifted_level = test_model_level(&shifted, test_sensor_value, strategies[index].shift);
			CHECK(test_channel_level() >= (int)tree_level);
			CHECK(test_channel_level() == (int)shifted_level);
		}
		for (temperature = 90000; temperature >= 20000; temperature -= 500) {
			test_poll(temperature);
			tree_level = test_model_level(&tree, test_sensor_value, 0);
			shifted_level = test_model_level(&shifted, test_sensor_value, strategies[index].shift);
			CHECK(test_channel_level() >= (int)tree_level);
			CHECK(test_channel_level() == (int)shifted_level);
		}
	}
}

/* The boost: a floor under the trips' level, kept across a strategy, 0 gives the fan back. */
static void
scenario_boost(void)
{
	struct kern_thermal_fan_state state;
	int error;

	test_start("full", 30000);
	CHECK(test_channel_level() == 0);

	/* A boost raises the stopped fan at once; the read still says the trips' level. */
	error = test_boost(3);
	CHECK(error == 0);
	CHECK(test_channel_level() == 3);
	state = test_read();
	CHECK(state.level == 0U);
	test_poll(30000);
	CHECK(test_channel_level() == 3);

	/* The trips above the boost win, and the boost holds the fan when they fall. */
	test_poll(72000);
	CHECK(test_channel_level() == 4);
	CHECK(test_read().level == 4U);
	test_poll(30000);
	CHECK(test_channel_level() == 3);
	CHECK(test_read().level == 0U);

	/* A strategy leaves the boost as it is. */
	error = test_apply(KERN_THERMAL_STRATEGY_POWERSAVE, 0, 0);
	CHECK(error == 0);
	CHECK(test_channel_level() == 3);
	error = test_apply(KERN_THERMAL_STRATEGY_MANUAL, 0, 100);
	CHECK(error == 0);
	CHECK(test_channel_level() == 3);

	/* The highest level is a boost, one past it is refused and changes nothing. */
	error = test_boost(4);
	CHECK(error == 0);
	CHECK(test_channel_level() == 4);
	error = test_boost(5);
	CHECK(error == EINVAL);
	CHECK(test_channel_level() == 4);

	/* 0 gives the fan back to the trips. */
	error = test_boost(0);
	CHECK(error == 0);
	CHECK(test_channel_level() == 0);
	test_poll(36000);
	CHECK(test_channel_level() == 1);
	CHECK(test_pwm_writes_unlocked == 0U);
}

/* An unreadable temperature runs the fan full and says the reading is not valid. */
static void
scenario_unreadable(void)
{
	struct kern_thermal_fan_state state;
	int error;

	test_start("full", 30000);
	error = test_boost(2);
	CHECK(error == 0);

	/* Unreadable: full, whatever the boost, and the core sees no valid reading. */
	test_set_unreadable();
	poll_once();
	CHECK(test_sensor_valid == 0);
	CHECK(test_channel_level() == 4);
	state = test_read();
	CHECK(state.level == 4U);

	/* A strategy without a reading keeps the fan full. */
	error = test_apply(KERN_THERMAL_STRATEGY_MANUAL, 0, 100);
	CHECK(error == 0);
	CHECK(test_channel_level() == 4);

	/* Readable again: the trips (lowered) and the boost decide. */
	test_poll(30000);
	CHECK(test_sensor_valid == 1);
	CHECK(test_channel_level() == 2);
	test_poll(36000);
	CHECK(test_channel_level() == 2);

	/* A start with an unreadable temperature runs full until it reads. */
	test_reset("full", 1);
	test_set_unreadable();
	error = drv_rpi5_fan_start();
	CHECK(error == 0);
	CHECK(test_channel_level() == 4);
	CHECK(test_read().level == 4U);
	test_poll(30000);
	CHECK(test_channel_level() == 0);
}

/* A mapped critical trip asks the fan for its level but never moves with the strategy. */
static void
scenario_critical_mapped(void)
{
	int error;

	test_start("critmap", 30000);
	CHECK(rpi5_fan.trip_count == 2U);
	CHECK(strcmp(test_trips_name(), "trips 40000-40000 mC") == 0);

	error = test_apply(KERN_THERMAL_STRATEGY_MANUAL, 0, 100);
	CHECK(error == 0);
	CHECK(strcmp(test_trips_name(), "trips 35000-35000 mC") == 0);
	test_poll(36000);
	CHECK(test_channel_level() == 1);
	test_poll(96000);
	CHECK(test_channel_level() == 1);
	test_poll(99000);
	CHECK(test_channel_level() == 1);
	test_poll(100000);
	CHECK(test_channel_level() == 4);
	CHECK(test_sensor_critical == 100000);
}

/* Maps that name no fan keep the Pi 5's own trips; the strategy moves those. */
static void
scenario_builtin_trips(void)
{
	int error;

	test_start("nomap", 30000);
	CHECK(rpi5_fan.trips_from_tree == 0);
	CHECK(rpi5_fan.trip_count == 4U);
	CHECK(test_sensor_critical == 105000);
	CHECK(strcmp(test_trips_name(), "trips 50000-75000 mC") == 0);
	test_poll(67000);
	CHECK(test_channel_level() == 2);
	test_poll(68000);
	CHECK(test_channel_level() == 3);

	error = test_apply(KERN_THERMAL_STRATEGY_MANUAL, 0, 90);
	CHECK(error == 0);
	CHECK(strcmp(test_trips_name(), "trips 45000-70000 mC") == 0);
	CHECK(test_channel_level() == 3);
	test_poll(70000);
	CHECK(test_channel_level() == 4);
}

/* A board without a fan: the sensor only, no fan and no receiver. */
static void
scenario_no_fan(void)
{
	int error;

	test_reset("nofan", 1);
	test_set_temperature(45000);
	error = drv_rpi5_fan_start();
	CHECK(error == 0);
	CHECK(test_sensor_registered);
	CHECK(test_sensor_value == 45000);
	CHECK(test_sensor_critical == 100000);
	CHECK(!test_fan_registered);
	CHECK(!test_receiver_registered);
	CHECK(test_thread_started);
	poll_once();
	CHECK(test_sensor_updates == 2U);

	/* Without RP1 there is nothing at all. */
	test_reset("full", 0);
	error = drv_rpi5_fan_start();
	CHECK(error == ENODEV);
	CHECK(!test_sensor_registered);
	CHECK(!test_fan_registered);
}

int
main(int argc, char **argv)
{
	if (argc < 2) {
		printf("usage: rpi5-fan-host-test <dtb dir> [-v]\n");
		return 2;
	}
	test_dir = argv[1];
	if (argc > 2 && strcmp(argv[2], "-v") == 0)
		test_verbose = 1;

	scenario_start();
	scenario_trips();
	scenario_strategy();
	scenario_never_slower();
	scenario_boost();
	scenario_unreadable();
	scenario_critical_mapped();
	scenario_builtin_trips();
	scenario_no_fan();

	printf("rpi5-fan host test: %u checks, %u failures\n", test_checks, test_failures);
	if (test_failures != 0U)
		return 1;
	printf("PASS\n");
	return 0;
}
