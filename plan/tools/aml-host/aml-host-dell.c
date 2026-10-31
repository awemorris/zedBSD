/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The harness's calls of the Dell provider (src/drivers/acpi/acpi-dell.c),
 * with stand-ins for the thermal core's registrations that keep what the
 * provider registers, so the calls can run the receiver and the fans as
 * the core's threads would.  Each call prints one line for its outcome and
 * one line with the writes of the SMI command port it caused.
 *
 *   --dell            attach the provider, list what it registered
 *   --dell-apply S    apply a strategy: performance, balanced, powersave,
 *                     firmware or manual:SPEED:NOISE:GATING
 *   --dell-poll       run the receiver's poll once
 *   --dell-fan        read every fan
 *   --dell-boost F:L  set fan F's boost to level L
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <drivers/acpi/dell.h>
#include <kern/thermal.h>

#include "aml-host-dell.h"
#include "aml-host-hardware.h"

/* The most receivers and fans the stand-ins keep. */
#define DELL_RECEIVERS_MAX	4U
#define DELL_FANS_MAX		8U

/* The room for a firmware mode's name, as the thermal core gives it. */
#define DELL_NAME_ROOM		16U

/* One registration the stand-ins kept. */
struct host_registration {
	char name[32];
	const void *ops;
	void *context;
};

/*
 * The receivers and the fans the provider registered, for the life of the
 * harness.
 */
static struct host_registration host_receivers[DELL_RECEIVERS_MAX];
static unsigned host_receiver_count;
static struct host_registration host_fans[DELL_FANS_MAX];
static unsigned host_fan_count;

static void print_smi(unsigned long before);
static void print_mode(void);
static int parse_strategy(const char *request, struct kern_thermal_strategy *strategy);

/*
 * Keeps a receiver as the thermal core would register it.
 */
int
kern_thermal_receiver_register(
	const char *name,
	const struct kern_thermal_receiver_ops *ops,
	void *context)
{
	struct host_registration *entry;

	/* Refuses a full table. */
	if (host_receiver_count >= DELL_RECEIVERS_MAX)
		return 28;

	/* Keeps it and says so. */
	entry = &host_receivers[host_receiver_count];
	snprintf(entry->name, sizeof(entry->name), "%s", name);
	entry->ops = ops;
	entry->context = context;
	host_receiver_count++;
	printf("Dell registered receiver %s\n", name);

	/* Succeeded: the receiver is kept. */
	return 0;
}

/*
 * Keeps a fan as the thermal core would register it.
 */
int
kern_thermal_fan_register(
	const char *name,
	uint32_t rpm_max,
	const struct kern_thermal_fan_ops *ops,
	void *context)
{
	struct host_registration *entry;
	const char *boost;

	/* Refuses a full table. */
	if (host_fan_count >= DELL_FANS_MAX)
		return 28;

	/* Keeps it. */
	entry = &host_fans[host_fan_count];
	snprintf(entry->name, sizeof(entry->name), "%s", name);
	entry->ops = ops;
	entry->context = context;
	host_fan_count++;

	/* Says so, and whether the fan can be boosted. */
	boost = "no";
	if (ops->boost != NULL)
		boost = "yes";
	printf("Dell registered fan %s rpm_max %u boost %s\n", name, (unsigned)rpm_max, boost);

	/* Succeeded: the fan is kept. */
	return 0;
}

/*
 * Attaches the Dell provider as the kernel does after the WMI attach.
 */
int
host_dell_attach(void)
{
	unsigned long before;
	int error;

	/* Attaches it, counting the SMI writes on the way. */
	before = hardware_smi_writes();
	error = drv_acpi_dell_attach();
	printf("Dell attach error %d\n", error);
	print_mode();
	print_smi(before);

	/* Succeeded: ENODEV, no Dell machine, is an outcome the output shows. */
	return 0;
}

/*
 * Gives the Dell receiver a strategy as the strategy thread would.
 */
int
host_dell_apply(
	const char *request)
{
	const struct kern_thermal_receiver_ops *ops;
	struct kern_thermal_strategy strategy;
	unsigned long before;
	uint32_t flags;
	int error;

	/* Reads the strategy. */
	error = parse_strategy(request, &strategy);
	if (error != 0) {
		fprintf(stderr, "--dell-apply needs performance, balanced, powersave, firmware or manual:S:N:G: %s\n", request);
		return 1;
	}

	/* Refuses a harness without the receiver. */
	if (host_receiver_count == 0U) {
		printf("Dell apply %s = no receiver\n", request);
		return 1;
	}

	/* Applies it, counting the SMI writes on the way. */
	ops = host_receivers[0].ops;
	before = hardware_smi_writes();
	flags = 0;
	error = ops->apply(host_receivers[0].context, &strategy, &flags);
	printf("Dell apply %s = error %d flags 0x%x\n", request, error, (unsigned)flags);
	print_mode();
	print_smi(before);

	/* Succeeded: the receiver's answer is in the output. */
	return 0;
}

/*
 * Runs the Dell receiver's poll once, as the poll thread would.
 */
int
host_dell_poll(void)
{
	const struct kern_thermal_receiver_ops *ops;
	unsigned long before;

	/* Refuses a harness without the receiver. */
	if (host_receiver_count == 0U) {
		printf("Dell poll = no receiver\n");
		return 1;
	}

	/* Polls, counting the SMI writes on the way. */
	ops = host_receivers[0].ops;
	before = hardware_smi_writes();
	ops->poll(host_receivers[0].context);
	printf("Dell poll\n");
	print_smi(before);

	/* Succeeded: the poll ran. */
	return 0;
}

/*
 * Reads every Dell fan, as the poll thread would.
 */
int
host_dell_fan(void)
{
	const struct kern_thermal_fan_ops *ops;
	struct kern_thermal_fan_state state;
	unsigned long before;
	unsigned index;
	int error;

	/* Reads each fan, counting the SMI writes on the way. */
	before = hardware_smi_writes();
	for (index = 0; index < host_fan_count; index++) {
		ops = host_fans[index].ops;
		memset(&state, 0, sizeof(state));
		error = ops->read(host_fans[index].context, &state);
		printf("Dell fan %s = error %d flags 0x%x rpm %u boost_count %u\n", host_fans[index].name, error,
		    (unsigned)state.flags, (unsigned)state.rpm, (unsigned)state.boost_count);
	}

	/* Says how many fans were read. */
	printf("Dell fans %u\n", host_fan_count);
	print_smi(before);

	/* Succeeded: the reads are in the output. */
	return 0;
}

/*
 * Sets a Dell fan's boost as the strategy thread would: FAN:LEVEL.
 */
int
host_dell_boost(
	const char *request)
{
	const struct kern_thermal_fan_ops *ops;
	unsigned long before;
	unsigned long fan;
	unsigned long level;
	char *end;
	int error;

	/* Reads the fan and the level. */
	fan = strtoul(request, &end, 0);
	if (end == request || *end != ':') {
		fprintf(stderr, "--dell-boost needs FAN:LEVEL: %s\n", request);
		return 1;
	}

	/* The level after the colon. */
	level = strtoul(end + 1, NULL, 0);

	/* Refuses a fan not registered. */
	if (fan >= host_fan_count) {
		printf("Dell boost %s = no fan\n", request);
		return 1;
	}

	/* Refuses a fan whose driver has no boost. */
	ops = host_fans[fan].ops;
	if (ops->boost == NULL) {
		printf("Dell boost %s = no boost\n", request);
		return 1;
	}

	/* Boosts it, counting the SMI writes on the way. */
	before = hardware_smi_writes();
	error = ops->boost(host_fans[fan].context, (uint32_t)level);
	printf("Dell boost %s = error %d\n", request, error);
	print_smi(before);

	/* Succeeded: the answer is in the output. */
	return 0;
}

/* Prints the SMI writes since a count. */
static void
print_smi(
	unsigned long before)
{
	/* Prints the difference. */
	printf("SMI writes %lu\n", hardware_smi_writes() - before);
}

/* Prints the firmware mode the receiver names. */
static void
print_mode(void)
{
	const struct kern_thermal_receiver_ops *ops;
	char name[DELL_NAME_ROOM];
	int error;

	/* Nothing to name without the receiver. */
	if (host_receiver_count == 0U)
		return;

	/* Asks the receiver as a GET of the strategy would. */
	ops = host_receivers[0].ops;
	memset(name, 0, sizeof(name));
	error = ops->firmware_name(host_receivers[0].context, name, sizeof(name));
	if (error != 0) {
		printf("Dell mode none (error %d)\n", error);
	} else {
		printf("Dell mode %s\n", name);
	}
}

/* Reads a strategy: a name, or manual:SPEED:NOISE:GATING. */
static int
parse_strategy(
	const char *request,
	struct kern_thermal_strategy *strategy)
{
	static const struct {
		const char *name;
		uint32_t value;
	} named[] = {
		{ "performance", KERN_THERMAL_STRATEGY_PERFORMANCE },
		{ "balanced", KERN_THERMAL_STRATEGY_BALANCED },
		{ "powersave", KERN_THERMAL_STRATEGY_POWERSAVE },
		{ "firmware", KERN_THERMAL_STRATEGY_FIRMWARE },
	};
	unsigned speed;
	unsigned noise;
	unsigned gating;
	unsigned index;
	int compared;
	int read;

	/* Starts from no axis. */
	memset(strategy, 0, sizeof(*strategy));

	/* Reads the named strategies. */
	for (index = 0; index < sizeof(named) / sizeof(named[0]); index++) {
		compared = strcmp(request, named[index].name);
		if (compared == 0) {
			strategy->value = named[index].value;
			return 0;
		}
	}

	/* Reads a manual strategy's axes. */
	read = sscanf(request, "manual:%u:%u:%u", &speed, &noise, &gating);
	if (read != 3)
		return 1;
	if (speed > KERN_THERMAL_AXIS_MAX || noise > KERN_THERMAL_AXIS_MAX || gating > KERN_THERMAL_AXIS_MAX)
		return 1;
	strategy->value = KERN_THERMAL_STRATEGY_MANUAL;
	strategy->speed = (uint8_t)speed;
	strategy->noise = (uint8_t)noise;
	strategy->gating = (uint8_t)gating;

	/* Succeeded: strategy holds the manual strategy. */
	return 0;
}
