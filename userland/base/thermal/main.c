/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The thermal command: shows the machine's temperatures, fans, energy
 * meters, CPUs and strategy from /dev/thermal, and lets root choose the
 * strategy, raise a fan, or set the CPUs directly.
 *
 * Every line it prints is one item: its kind, its number and name where it
 * has them, then key=value fields, so that a script reads it as easily as
 * a person.  A chosen strategy is kept in /etc/thermal.conf, which the
 * service at boot gives back to the kernel with "thermal restore"; this
 * command is the only writer of that file.
 *
 * It exits 0 when it did what it was asked, 1 when the kernel refused
 * (the reason on one line), and 2 for a command it does not know.  A
 * strategy no part of the machine acts on (a virtual machine) is not a
 * failure: it is kept and said so.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <uapi/thermal.h>

/* Where the device is, and where the chosen strategy is kept. */
#define THERMAL_DEVICE			"/dev/thermal"
#define THERMAL_CONF			"/etc/thermal.conf"
#define THERMAL_CONF_NEW		"/etc/thermal.conf.new"

/* The exit statuses: done, refused by the kernel, a command not understood. */
#define EXIT_DONE			0
#define EXIT_REFUSED			1
#define EXIT_USAGE			2

/* The longest line of the kept strategy. */
#define CONF_LINE_MAX			128U

/* The interval of the two readings of power and idle, in seconds, by default and at most. */
#define INTERVAL_DEFAULT		1U
#define INTERVAL_MAX			3600U

/* The event records one read takes. */
#define WATCH_BATCH			8U

/* The text of one temperature or energy, with its terminator. */
#define NUMBER_TEXT_MAX			32U

/* The text of a list of flags, with its terminator. */
#define FLAGS_TEXT_MAX			96U

/* One name of a value the kernel uses: a strategy, an event type, a kind. */
struct value_name {
	const char *name;
	uint32_t value;
};

/* One flag bit and the word that shows it. */
struct flag_name {
	uint32_t bit;
	const char *name;
};

/* The strategies by name; FIRMWARE is shown but never chosen. */
static const struct value_name strategy_names[] = {
	{ "firmware", KERN_THERMAL_STRATEGY_FIRMWARE },
	{ "performance", KERN_THERMAL_STRATEGY_PERFORMANCE },
	{ "balanced", KERN_THERMAL_STRATEGY_BALANCED },
	{ "powersave", KERN_THERMAL_STRATEGY_POWERSAVE },
	{ "manual", KERN_THERMAL_STRATEGY_MANUAL },
	{ NULL, 0 }
};

/* The event types by the name "thermal watch" takes and prints. */
static const struct value_name event_names[] = {
	{ "strategy", KERN_THERMAL_EVENT_STRATEGY },
	{ "boost", KERN_THERMAL_EVENT_BOOST },
	{ "passive", KERN_THERMAL_EVENT_PASSIVE },
	{ "critical", KERN_THERMAL_EVENT_CRITICAL },
	{ "emergency", KERN_THERMAL_EVENT_EMERGENCY },
	{ "fan", KERN_THERMAL_EVENT_FAN_STALLED },
	{ "cpu", KERN_THERMAL_EVENT_CPU },
	{ "overflow", KERN_THERMAL_EVENT_OVERFLOW },
	{ NULL, 0 }
};

/* The emergency's states by name. */
static const struct value_name emergency_names[] = {
	{ "none", KERN_THERMAL_EMERGENCY_NONE },
	{ "signaled", KERN_THERMAL_EMERGENCY_SIGNALED },
	{ "init", KERN_THERMAL_EMERGENCY_INIT },
	{ "cutting", KERN_THERMAL_EMERGENCY_CUTTING },
	{ "halted", KERN_THERMAL_EMERGENCY_HALTED },
	{ NULL, 0 }
};

/* The kinds of meter by name. */
static const struct value_name meter_names[] = {
	{ "package", KERN_THERMAL_METER_PACKAGE },
	{ "cores", KERN_THERMAL_METER_CORES },
	{ "graphics", KERN_THERMAL_METER_GRAPHICS },
	{ "platform", KERN_THERMAL_METER_PLATFORM },
	{ "dram", KERN_THERMAL_METER_DRAM },
	{ NULL, 0 }
};

/* The kinds of core by the letter a CPU line shows. */
static const struct value_name core_names[] = {
	{ "-", KERN_THERMAL_CPU_CORE_UNKNOWN },
	{ "P", KERN_THERMAL_CPU_CORE_PERFORMANCE },
	{ "E", KERN_THERMAL_CPU_CORE_EFFICIENCY },
	{ NULL, 0 }
};

/* The ways of measuring the relative performance by name. */
static const struct value_name relative_names[] = {
	{ "frequency", KERN_THERMAL_CPU_RELATIVE_FREQUENCY },
	{ "feedback", KERN_THERMAL_CPU_RELATIVE_FEEDBACK },
	{ "provider", KERN_THERMAL_CPU_RELATIVE_PROVIDER },
	{ NULL, 0 }
};

/* What a sensor is and what its reading holds. */
static const struct flag_name sensor_flags[] = {
	{ KERN_THERMAL_READING_VALID, "valid" },
	{ KERN_THERMAL_READING_STALE, "stale" },
	{ KERN_THERMAL_READING_ARMED, "armed" },
	{ 0, NULL }
};

/* What a fan has and how its last reading went. */
static const struct flag_name fan_flags[] = {
	{ KERN_THERMAL_FAN_KNOWN, "known" },
	{ KERN_THERMAL_FAN_STALE, "stale" },
	{ KERN_THERMAL_FAN_STALLED, "stalled" },
	{ KERN_THERMAL_FAN_CAN_BOOST, "boost" },
	{ 0, NULL }
};

/* How a meter's last reading went. */
static const struct flag_name meter_flags[] = {
	{ KERN_THERMAL_METER_KNOWN, "known" },
	{ KERN_THERMAL_METER_STALE, "stale" },
	{ 0, NULL }
};

/* What a strategy's outcome says. */
static const struct flag_name strategy_flags[] = {
	{ KERN_THERMAL_STRATEGY_CUSTOM, "custom" },
	{ KERN_THERMAL_STRATEGY_PARTIAL, "partial" },
	{ KERN_THERMAL_STRATEGY_PENDING, "pending" },
	{ KERN_THERMAL_STRATEGY_SUPERSEDED, "superseded" },
	{ 0, NULL }
};

/* What a CPU has. */
static const struct flag_name cpu_flags[] = {
	{ KERN_THERMAL_CPU_HAS_CONTROL, "control" },
	{ KERN_THERMAL_CPU_ESTIMATED_MHZ, "estimated" },
	{ KERN_THERMAL_CPU_LIMITED, "limited" },
	{ KERN_THERMAL_CPU_PLATFORM_LIMIT, "platform-limit" },
	{ KERN_THERMAL_CPU_CUSTOM, "custom" },
	{ KERN_THERMAL_CPU_SAMPLE_TIMEOUT, "sample-timeout" },
	{ 0, NULL }
};

/* What a domain's request says. */
static const struct flag_name domain_flags[] = {
	{ KERN_THERMAL_CPU_DOMAIN_CUSTOM, "custom" },
	{ KERN_THERMAL_CPU_DOMAIN_LIMITED, "limited" },
	{ KERN_THERMAL_CPU_DOMAIN_PENDING, "pending" },
	{ 0, NULL }
};

/*
 * The open /dev/thermal, -1 until a command opens it.  Opened once per
 * run and closed by the exit.
 */
static int thermal_descriptor = -1;

/* Set by SIGINT or SIGTERM to end "thermal watch". */
static volatile sig_atomic_t watch_stopped;

static void usage(void);
static int device_open(void);
static int device_request(unsigned long request, void *argument);
static int refused(const char *what, int error);
static bool parse_unsigned(const char *text, uint32_t maximum, uint32_t *value);
static bool parse_interval(int argc, char **argv, int *used, unsigned *seconds);
static const char *value_text(const struct value_name *names, uint32_t value);
static bool value_find(const struct value_name *names, const char *name, uint32_t *value);
static void flags_text(const struct flag_name *names, uint32_t flags, char *text, size_t size);
static void millicelsius_text(int32_t millicelsius, char *text, size_t size);
static void outcome_text(int error, char *text, size_t size);
static int show_summary(void);
static int show_sensors(void);
static int show_fans(void);
static int show_meters(void);
static int show_receivers(void);
static int show_strategy(void);
static int command_strategy(int argc, char **argv);
static int strategy_choose(const struct thermal_strategy *wanted, bool keep);
static void strategy_keep(const struct thermal_strategy *wanted);
static int command_restore(void);
static int command_boost(int argc, char **argv);
static int command_cpu(int argc, char **argv);
static int show_cpus(void);
static int show_cpu(uint32_t cpu, bool sample);
static int show_domains(void);
static int command_cpu_set(int argc, char **argv);
static int command_relative(int argc, char **argv);
static int command_idle(int argc, char **argv);
static int idle_read(bool package, uint32_t target, struct thermal_cpu_idle *idle);
static void idle_print(bool package, const struct thermal_cpu_idle *first, const struct thermal_cpu_idle *second);
static int command_power(int argc, char **argv);
static int command_watch(int argc, char **argv);
static void watch_stop(int signal_number);

/*
 * Runs the thermal command.
 */
int
main(
	int argc,
	char **argv)
{
	const char *command;
	int status;

	/* Opens the device every command reads. */
	status = device_open();
	if (status != EXIT_DONE)
		return status;

	/* Without a command, the summary. */
	if (argc < 2) {
		status = show_summary();
		return status;
	}

	/* Hands the command its own arguments. */
	command = argv[1];
	if (strcmp(command, "sensors") == 0 && argc == 2) {
		status = show_sensors();
	} else if (strcmp(command, "fans") == 0 && argc == 2) {
		status = show_fans();
	} else if (strcmp(command, "meters") == 0 && argc == 2) {
		status = show_meters();
	} else if (strcmp(command, "receivers") == 0 && argc == 2) {
		status = show_receivers();
	} else if (strcmp(command, "strategy") == 0) {
		status = command_strategy(argc - 2, argv + 2);
	} else if (strcmp(command, "restore") == 0 && argc == 2) {
		status = command_restore();
	} else if (strcmp(command, "boost") == 0) {
		status = command_boost(argc - 2, argv + 2);
	} else if (strcmp(command, "cpu") == 0) {
		status = command_cpu(argc - 2, argv + 2);
	} else if (strcmp(command, "idle") == 0) {
		status = command_idle(argc - 2, argv + 2);
	} else if (strcmp(command, "power") == 0) {
		status = command_power(argc - 2, argv + 2);
	} else if (strcmp(command, "watch") == 0) {
		status = command_watch(argc - 2, argv + 2);
	} else {
		usage();
		status = EXIT_USAGE;
	}

	/* Succeeded or not, the command's own status. */
	return status;
}

/* Says how the command is used. */
static void
usage(void)
{
	fprintf(stderr,
		"usage: thermal\n"
		"       thermal sensors | fans | meters | receivers\n"
		"       thermal strategy [performance | balanced | powersave | manual <speed> <noise> <gating>]\n"
		"       thermal restore\n"
		"       thermal boost <fan|all> <level>\n"
		"       thermal cpu [<n> [sample] | domains | set <domain|all> [min=N] [max=N] [desired=N] [epp=N]]\n"
		"       thermal cpu relative [frequency | feedback | provider]\n"
		"       thermal idle [cpu <n> | package <n>] [-i <seconds>]\n"
		"       thermal power [-i <seconds>]\n"
		"       thermal watch [strategy | boost | passive | critical | emergency | fan | cpu]...\n");
}

/* Opens /dev/thermal for the run. */
static int
device_open(void)
{
	/* Every request goes through the one descriptor. */
	thermal_descriptor = open(THERMAL_DEVICE, O_RDONLY | O_CLOEXEC);
	if (thermal_descriptor < 0) {
		fprintf(stderr, "thermal: %s: %s\n", THERMAL_DEVICE, strerror(errno));
		return EXIT_REFUSED;
	}

	/* Succeeded: the device is open. */
	return EXIT_DONE;
}

/* Sends one request to the device and returns 0 or the errno the kernel answered. */
static int
device_request(
	unsigned long request,
	void *argument)
{
	int answer;

	/* Hands the request to the kernel. */
	answer = ioctl(thermal_descriptor, request, argument);
	if (answer < 0)
		return errno;

	/* Succeeded: the kernel served it. */
	return 0;
}

/* Says on one line what the kernel refused and why, and gives the refused status. */
static int
refused(
	const char *what,
	int error)
{
	/* The reason, in the C library's words. */
	fprintf(stderr, "thermal: %s: %s\n", what, strerror(error));

	/* The status of a refusal. */
	return EXIT_REFUSED;
}

/* Reads a whole decimal number no larger than maximum. */
static bool
parse_unsigned(
	const char *text,
	uint32_t maximum,
	uint32_t *value)
{
	unsigned long number;
	char *end;

	/* Refuses an empty text, a sign or a space before the digits. */
	if (text[0] < '0' || text[0] > '9')
		return false;

	/* Reads the digits; anything after them, or a value too large, is refused. */
	errno = 0;
	number = strtoul(text, &end, 10);
	if (errno != 0 || *end != '\0')
		return false;
	if (number > (unsigned long)maximum)
		return false;

	/* Succeeded: the number. */
	*value = (uint32_t)number;
	return true;
}

/*
 * Reads "-i <seconds>" at the start of the arguments, or leaves the
 * default; *used is how many arguments it took.
 */
static bool
parse_interval(
	int argc,
	char **argv,
	int *used,
	unsigned *seconds)
{
	uint32_t value;
	bool parsed;

	/* Without the option, the default interval. */
	*used = 0;
	*seconds = INTERVAL_DEFAULT;
	if (argc < 1 || strcmp(argv[0], "-i") != 0)
		return true;

	/* The option takes one positive number of seconds. */
	if (argc < 2)
		return false;
	parsed = parse_unsigned(argv[1], INTERVAL_MAX, &value);
	if (!parsed || value == 0U)
		return false;

	/* Succeeded: the interval and the two arguments it took. */
	*seconds = value;
	*used = 2;
	return true;
}

/* Gives the name of a value, or "?" for one the command does not know. */
static const char *
value_text(
	const struct value_name *names,
	uint32_t value)
{
	unsigned index;

	/* Finds the value in the table. */
	for (index = 0; names[index].name != NULL; index++) {
		if (names[index].value == value)
			return names[index].name;
	}

	/* A value this command is older than. */
	return "?";
}

/* Finds the value of a name. */
static bool
value_find(
	const struct value_name *names,
	const char *name,
	uint32_t *value)
{
	unsigned index;

	/* Finds the name in the table. */
	for (index = 0; names[index].name != NULL; index++) {
		if (strcmp(names[index].name, name) == 0) {
			*value = names[index].value;
			return true;
		}
	}

	/* No such name. */
	return false;
}

/* Writes the names of the flags that are set, separated by commas, or "-" when none is. */
static void
flags_text(
	const struct flag_name *names,
	uint32_t flags,
	char *text,
	size_t size)
{
	unsigned index;
	size_t length;

	/* Appends each set flag's name. */
	text[0] = '\0';
	length = 0;
	for (index = 0; names[index].name != NULL; index++) {
		if ((flags & names[index].bit) == 0U)
			continue;
		if (length != 0U && length + 1U < size) {
			text[length] = ',';
			length++;
			text[length] = '\0';
		}
		(void)snprintf(text + length, size - length, "%s", names[index].name);
		length = strlen(text);
	}

	/* No flag is shown as a dash, so the field is never empty. */
	if (length == 0U)
		(void)snprintf(text, size, "-");
}

/* Writes a temperature in degrees with three decimals. */
static void
millicelsius_text(
	int32_t millicelsius,
	char *text,
	size_t size)
{
	long magnitude;
	const char *sign;

	/* Splits the sign off, so -0.500 keeps its sign. */
	sign = "";
	magnitude = (long)millicelsius;
	if (magnitude < 0) {
		sign = "-";
		magnitude = -magnitude;
	}

	/* The degrees and the thousandths. */
	(void)snprintf(text, size, "%s%ld.%03ld", sign, magnitude / 1000L, magnitude % 1000L);
}

/* Writes the outcome of a strategy: ok, unsupported or the error. */
static void
outcome_text(
	int error,
	char *text,
	size_t size)
{
	/* Says the outcome in one word where there is one. */
	if (error == 0) {
		(void)snprintf(text, size, "ok");
	} else if (error == EOPNOTSUPP) {
		(void)snprintf(text, size, "unsupported");
	} else {
		(void)snprintf(text, size, "\"%s\"", strerror(error));
	}
}

/* Shows the strategy, the emergency, and every sensor, fan and meter. */
static int
show_summary(void)
{
	struct thermal_status status;
	char temperature[NUMBER_TEXT_MAX];
	int error;
	int result;

	/* Reads the state at a glance. */
	memset(&status, 0, sizeof(status));
	error = device_request(KERN_THERMAL_GET_STATUS, &status);
	if (error != 0)
		return refused("status", error);

	/* The strategy, then the emergency and the passive ceiling. */
	result = show_strategy();
	if (result != EXIT_DONE)
		return result;
	if (status.emergency == KERN_THERMAL_EMERGENCY_NONE) {
		printf("emergency none\n");
	} else {
		millicelsius_text(status.emergency_millicelsius, temperature, sizeof(temperature));
		printf("emergency %s sensor=%u temperature=%s\n",
		       value_text(emergency_names, status.emergency), status.emergency_sensor, temperature);
	}
	printf("passive permille=%u\n", status.passive_permille);

	/* Every sensor, fan and meter. */
	result = show_sensors();
	if (result != EXIT_DONE)
		return result;
	result = show_fans();
	if (result != EXIT_DONE)
		return result;
	result = show_meters();
	if (result != EXIT_DONE)
		return result;

	/* Succeeded: the summary is out. */
	return EXIT_DONE;
}

/* Shows every temperature sensor, one per line. */
static int
show_sensors(void)
{
	struct thermal_sensor_info sensor;
	char temperature[NUMBER_TEXT_MAX];
	char passive[NUMBER_TEXT_MAX];
	char critical[NUMBER_TEXT_MAX];
	char flags[FLAGS_TEXT_MAX];
	const char *kind;
	uint32_t index;
	int error;

	/* Walks the sensors until the kernel says there are no more. */
	for (index = 0;; index++) {
		memset(&sensor, 0, sizeof(sensor));
		sensor.index = index;
		error = device_request(KERN_THERMAL_GET_SENSOR, &sensor);
		if (error == ENOENT)
			break;
		if (error != 0)
			return refused("sensor", error);

		/* The levels are shown only where they are known. */
		millicelsius_text(sensor.millicelsius, temperature, sizeof(temperature));
		(void)snprintf(passive, sizeof(passive), "-");
		if ((sensor.reading & KERN_THERMAL_READING_PASSIVE) != 0U)
			millicelsius_text(sensor.passive, passive, sizeof(passive));
		(void)snprintf(critical, sizeof(critical), "-");
		if ((sensor.reading & KERN_THERMAL_READING_CRITICAL) != 0U)
			millicelsius_text(sensor.critical, critical, sizeof(critical));
		flags_text(sensor_flags, sensor.reading, flags, sizeof(flags));

		/* A zone has trip points, a device only its reading. */
		kind = "device";
		if (sensor.kind == KERN_THERMAL_KIND_ZONE)
			kind = "zone";
		printf("sensor %u %s kind=%s cpu=%s temperature=%s passive=%s critical=%s flags=%s\n",
		       index, sensor.name, kind,
		       (sensor.flags & KERN_THERMAL_SENSOR_CPU) != 0U ? "yes" : "no",
		       temperature, passive, critical, flags);
	}

	/* Succeeded: every sensor is out. */
	return EXIT_DONE;
}

/* Shows every fan, one per line. */
static int
show_fans(void)
{
	struct thermal_fan_info fan;
	char flags[FLAGS_TEXT_MAX];
	uint32_t index;
	int error;

	/* Walks the fans until the kernel says there are no more. */
	for (index = 0;; index++) {
		memset(&fan, 0, sizeof(fan));
		fan.index = index;
		error = device_request(KERN_THERMAL_GET_FAN, &fan);
		if (error == ENOENT)
			break;
		if (error != 0)
			return refused("fan", error);

		/* The speed, the automatic level and the boost, each out of its count. */
		flags_text(fan_flags, fan.flags, flags, sizeof(flags));
		printf("fan %u %s rpm=%u rpm_max=%u level=%u/%u boost=%u/%u flags=%s\n",
		       index, fan.name, fan.rpm, fan.rpm_max, fan.level, fan.level_count,
		       fan.boost, fan.boost_count, flags);
	}

	/* Succeeded: every fan is out. */
	return EXIT_DONE;
}

/* Shows every energy meter, one per line, with the energy in joules. */
static int
show_meters(void)
{
	struct thermal_meter_info meter;
	char flags[FLAGS_TEXT_MAX];
	uint32_t index;
	int error;

	/* Walks the meters until the kernel says there are no more. */
	for (index = 0;; index++) {
		memset(&meter, 0, sizeof(meter));
		meter.index = index;
		error = device_request(KERN_THERMAL_GET_METER, &meter);
		if (error == ENOENT)
			break;
		if (error != 0)
			return refused("meter", error);

		/* The energy since the kernel first read it, in joules to the nanojoule. */
		flags_text(meter_flags, meter.flags, flags, sizeof(flags));
		printf("meter %u %s kind=%s energy=%llu.%09llu flags=%s\n",
		       index, meter.name, value_text(meter_names, meter.kind),
		       (unsigned long long)(meter.energy_nj / 1000000000ULL),
		       (unsigned long long)(meter.energy_nj % 1000000000ULL), flags);
	}

	/* Succeeded: every meter is out. */
	return EXIT_DONE;
}

/* Shows every receiver of the strategy and the outcome of its last strategy. */
static int
show_receivers(void)
{
	struct thermal_receiver_info receiver;
	char outcome[FLAGS_TEXT_MAX];
	uint32_t index;
	int error;

	/* Walks the receivers until the kernel says there are no more. */
	for (index = 0;; index++) {
		memset(&receiver, 0, sizeof(receiver));
		receiver.index = index;
		error = device_request(KERN_THERMAL_GET_RECEIVER, &receiver);
		if (error == ENOENT)
			break;
		if (error != 0)
			return refused("receiver", error);

		/* A receiver that has not been given a strategy yet has no outcome. */
		if (receiver.generation == 0U) {
			printf("receiver %u %s result=- generation=0\n", index, receiver.name);
			continue;
		}
		outcome_text(receiver.error, outcome, sizeof(outcome));
		printf("receiver %u %s result=%s generation=%u duration_ms=%u unconfirmed=%s\n",
		       index, receiver.name, outcome, receiver.generation, receiver.duration_ms,
		       (receiver.flags & KERN_THERMAL_APPLY_UNCONFIRMED) != 0U ? "yes" : "no");
	}

	/* Succeeded: every receiver is out. */
	return EXIT_DONE;
}

/* Shows the strategy in force, the firmware's mode and the receivers' outcomes. */
static int
show_strategy(void)
{
	struct thermal_strategy strategy;
	char flags[FLAGS_TEXT_MAX];
	char outcome[FLAGS_TEXT_MAX];
	int error;
	int result;

	/* Reads the strategy in force. */
	memset(&strategy, 0, sizeof(strategy));
	error = device_request(KERN_THERMAL_GET_STRATEGY, &strategy);
	if (error != 0)
		return refused("strategy", error);

	/* The strategy, with the axes for MANUAL, and its outcome. */
	flags_text(strategy_flags, strategy.flags, flags, sizeof(flags));
	outcome_text(strategy.error, outcome, sizeof(outcome));
	if (strategy.value == KERN_THERMAL_STRATEGY_MANUAL) {
		printf("strategy manual speed=%u noise=%u gating=%u generation=%u result=%s flags=%s\n",
		       strategy.speed, strategy.noise, strategy.gating, strategy.generation, outcome, flags);
	} else {
		printf("strategy %s generation=%u result=%s flags=%s\n",
		       value_text(strategy_names, strategy.value), strategy.generation, outcome, flags);
	}

	/* The firmware's own mode, where a receiver knows it. */
	if (strategy.firmware_name[0] != '\0')
		printf("firmware %s\n", strategy.firmware_name);

	/* The receivers' outcomes. */
	result = show_receivers();
	if (result != EXIT_DONE)
		return result;

	/* Succeeded: the strategy is out. */
	return EXIT_DONE;
}

/* Shows the strategy, or chooses one and keeps it. */
static int
command_strategy(
	int argc,
	char **argv)
{
	struct thermal_strategy wanted;
	uint32_t value;
	uint32_t speed;
	uint32_t noise;
	uint32_t gating;
	bool parsed;
	bool known;
	int status;

	/* Without a name, the strategy in force. */
	if (argc == 0) {
		status = show_strategy();
		return status;
	}

	/* The name must be one that can be chosen. */
	known = value_find(strategy_names, argv[0], &value);
	if (!known || value == KERN_THERMAL_STRATEGY_FIRMWARE) {
		usage();
		return EXIT_USAGE;
	}

	/* MANUAL takes its three axes, the others none. */
	memset(&wanted, 0, sizeof(wanted));
	wanted.value = value;
	if (value == KERN_THERMAL_STRATEGY_MANUAL) {
		if (argc != 4) {
			usage();
			return EXIT_USAGE;
		}
		parsed = parse_unsigned(argv[1], KERN_THERMAL_AXIS_MAX, &speed);
		if (parsed)
			parsed = parse_unsigned(argv[2], KERN_THERMAL_AXIS_MAX, &noise);
		if (parsed)
			parsed = parse_unsigned(argv[3], KERN_THERMAL_AXIS_MAX, &gating);
		if (!parsed) {
			usage();
			return EXIT_USAGE;
		}
		wanted.speed = (uint8_t)speed;
		wanted.noise = (uint8_t)noise;
		wanted.gating = (uint8_t)gating;
	} else if (argc != 1) {
		usage();
		return EXIT_USAGE;
	}

	/* Chooses it and keeps it for the next boot. */
	status = strategy_choose(&wanted, true);
	return status;
}

/*
 * Hands a strategy to the kernel, says how the receivers took it, and
 * keeps it when asked and when the kernel took it in force.
 */
static int
strategy_choose(
	const struct thermal_strategy *wanted,
	bool keep)
{
	struct thermal_strategy strategy;
	char flags[FLAGS_TEXT_MAX];
	int error;
	int result;

	/* Hands the choice to the kernel, which waits up to five seconds for the receivers. */
	strategy = *wanted;
	error = device_request(KERN_THERMAL_SET_STRATEGY, &strategy);

	/* A choice the kernel did not take in force is neither kept nor shown. */
	if (error == EPERM || error == EINVAL || error == ENOTTY || error == EFAULT)
		return refused("strategy", error);

	/* The kernel keeps the strategy whatever the receivers answered; so does the file. */
	if (keep)
		strategy_keep(wanted);

	/* No receiver acts on it here: kept all the same, and not a failure. */
	if (error == EOPNOTSUPP) {
		printf("strategy %s result=unsupported kept=yes\n", value_text(strategy_names, wanted->value));
		return EXIT_DONE;
	}

	/* Every receiver that acts on it failed: the receivers say how. */
	if (error != 0) {
		(void)show_receivers();
		return refused("strategy", error);
	}

	/* The outcome, then each receiver's. */
	flags_text(strategy_flags, strategy.flags, flags, sizeof(flags));
	printf("strategy %s generation=%u flags=%s\n",
	       value_text(strategy_names, wanted->value), strategy.generation, flags);
	result = show_receivers();
	if (result != EXIT_DONE)
		return result;

	/* Succeeded: the strategy is in force. */
	return EXIT_DONE;
}

/*
 * Writes the chosen strategy to /etc/thermal.conf, through a new file
 * renamed over the old one so a crash leaves one or the other whole.  A
 * file that cannot be written leaves the strategy in force and is said.
 */
static void
strategy_keep(
	const struct thermal_strategy *wanted)
{
	char line[CONF_LINE_MAX];
	int descriptor;
	int length;
	ssize_t written;
	int synced;
	int renamed;

	/* The one line of the file. */
	if (wanted->value == KERN_THERMAL_STRATEGY_MANUAL) {
		length = snprintf(line, sizeof(line), "strategy manual %u %u %u\n",
				  wanted->speed, wanted->noise, wanted->gating);
	} else {
		length = snprintf(line, sizeof(line), "strategy %s\n", value_text(strategy_names, wanted->value));
	}
	if (length <= 0 || (size_t)length >= sizeof(line))
		return;

	/* Writes the new file and its contents out before it replaces the old one. */
	descriptor = open(THERMAL_CONF_NEW, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (descriptor < 0) {
		fprintf(stderr, "thermal: warning: %s: %s; the strategy is not kept\n", THERMAL_CONF_NEW, strerror(errno));
		return;
	}
	written = write(descriptor, line, (size_t)length);
	synced = fsync(descriptor);
	(void)close(descriptor);
	if (written != (ssize_t)length || synced != 0) {
		fprintf(stderr, "thermal: warning: %s: not written; the strategy is not kept\n", THERMAL_CONF_NEW);
		return;
	}

	/* Puts it in place of the old one. */
	renamed = rename(THERMAL_CONF_NEW, THERMAL_CONF);
	if (renamed != 0)
		fprintf(stderr, "thermal: warning: %s: %s; the strategy is not kept\n", THERMAL_CONF, strerror(errno));
}

/*
 * Gives the kernel the strategy kept in /etc/thermal.conf, or balanced
 * when there is none or it cannot be read.  The file is not written.
 */
static int
command_restore(void)
{
	struct thermal_strategy wanted;
	char line[CONF_LINE_MAX];
	char name[CONF_LINE_MAX];
	unsigned speed;
	unsigned noise;
	unsigned gating;
	uint32_t value;
	FILE *conf;
	char *read;
	bool known;
	int fields;
	int status;

	/* Balanced unless the file says otherwise. */
	memset(&wanted, 0, sizeof(wanted));
	wanted.value = KERN_THERMAL_STRATEGY_BALANCED;

	/* Reads the one line of the file, when there is one. */
	conf = fopen(THERMAL_CONF, "r");
	read = NULL;
	if (conf != NULL) {
		read = fgets(line, sizeof(line), conf);
		(void)fclose(conf);
	}

	/* Takes the strategy the line names; a line that names none is said and ignored. */
	if (read != NULL) {
		speed = 0;
		noise = 0;
		gating = 0;
		fields = sscanf(line, "strategy %127s %u %u %u", name, &speed, &noise, &gating);
		known = false;
		if (fields >= 1)
			known = value_find(strategy_names, name, &value);
		if (known && value == KERN_THERMAL_STRATEGY_MANUAL) {
			if (fields != 4 ||
			    speed > KERN_THERMAL_AXIS_MAX ||
			    noise > KERN_THERMAL_AXIS_MAX ||
			    gating > KERN_THERMAL_AXIS_MAX)
				known = false;
		} else if (known && (fields != 1 || value == KERN_THERMAL_STRATEGY_FIRMWARE)) {
			known = false;
		}
		if (known) {
			wanted.value = value;
			wanted.speed = (uint8_t)speed;
			wanted.noise = (uint8_t)noise;
			wanted.gating = (uint8_t)gating;
		} else {
			fprintf(stderr, "thermal: warning: %s: not a strategy; balanced instead\n", THERMAL_CONF);
		}
	}

	/* Hands it to the kernel without writing the file again. */
	status = strategy_choose(&wanted, false);
	return status;
}

/* Raises one fan, or every fan that can be raised, to a level; 0 gives it back. */
static int
command_boost(
	int argc,
	char **argv)
{
	struct thermal_boost boost;
	uint32_t fan;
	uint32_t level;
	bool parsed;
	int error;

	/* The fan, or all, and the level. */
	if (argc != 2) {
		usage();
		return EXIT_USAGE;
	}
	fan = KERN_THERMAL_ALL;
	if (strcmp(argv[0], "all") != 0) {
		parsed = parse_unsigned(argv[0], UINT32_MAX - 1U, &fan);
		if (!parsed) {
			usage();
			return EXIT_USAGE;
		}
	}
	parsed = parse_unsigned(argv[1], UINT32_MAX, &level);
	if (!parsed) {
		usage();
		return EXIT_USAGE;
	}

	/* Hands it to the kernel, which waits up to five seconds for the fans' drivers. */
	memset(&boost, 0, sizeof(boost));
	boost.fan = fan;
	boost.level = level;
	error = device_request(KERN_THERMAL_SET_BOOST, &boost);
	if (error != 0)
		return refused("boost", error);

	/* Says whether a driver was still at work. */
	printf("boost level=%u pending=%s\n", level, (boost.flags & KERN_THERMAL_BOOST_PENDING) != 0U ? "yes" : "no");

	/* Succeeded: the fans are at the level, or will be. */
	return EXIT_DONE;
}

/* Shows the CPUs or their domains, or sets them, as the arguments say. */
static int
command_cpu(
	int argc,
	char **argv)
{
	uint32_t cpu;
	bool parsed;
	bool sample;
	int status;

	/* Without arguments, one line per CPU. */
	if (argc == 0) {
		status = show_cpus();
		return status;
	}

	/* The domains, a direct setting, or the relative performance. */
	if (strcmp(argv[0], "domains") == 0 && argc == 1) {
		status = show_domains();
		return status;
	}
	if (strcmp(argv[0], "set") == 0) {
		status = command_cpu_set(argc - 1, argv + 1);
		return status;
	}
	if (strcmp(argv[0], "relative") == 0) {
		status = command_relative(argc - 1, argv + 1);
		return status;
	}

	/* One CPU, sampled when asked. */
	parsed = parse_unsigned(argv[0], UINT32_MAX - 1U, &cpu);
	sample = false;
	if (argc == 2 && strcmp(argv[1], "sample") == 0)
		sample = true;
	if (!parsed || argc > 2 || (argc == 2 && !sample)) {
		usage();
		return EXIT_USAGE;
	}
	status = show_cpu(cpu, sample);
	return status;
}

/* Shows one line per CPU: its kind, domain, relative performance and rate. */
static int
show_cpus(void)
{
	struct thermal_cpu_info cpu;
	char relative[NUMBER_TEXT_MAX];
	uint32_t index;
	int error;

	/* Walks the CPUs until the kernel says there are no more. */
	for (index = 0;; index++) {
		memset(&cpu, 0, sizeof(cpu));
		cpu.cpu = index;
		error = device_request(KERN_THERMAL_GET_CPU, &cpu);
		if (error == ENOENT)
			break;
		if (error != 0)
			return refused("cpu", error);

		/* A relative performance nobody knows is a dash. */
		(void)snprintf(relative, sizeof(relative), "-");
		if (cpu.relative != KERN_THERMAL_UNKNOWN)
			(void)snprintf(relative, sizeof(relative), "%u", cpu.relative);
		printf("cpu %u core=%s domain=%u package=%u relative=%s highest_mhz=%u mhz=%u\n",
		       index, value_text(core_names, cpu.core_class), cpu.domain, cpu.package,
		       relative, cpu.highest_mhz, cpu.delivered_mhz);
	}

	/* Succeeded: every CPU is out. */
	return EXIT_DONE;
}

/* Shows every field of one CPU, sampling its rate when asked. */
static int
show_cpu(
	uint32_t index,
	bool sample)
{
	struct thermal_cpu_info cpu;
	char flags[FLAGS_TEXT_MAX];
	int error;

	/* Reads the CPU; a sample wakes it, which only root may do. */
	memset(&cpu, 0, sizeof(cpu));
	cpu.cpu = index;
	if (sample)
		cpu.request = KERN_THERMAL_CPU_READ_SAMPLE;
	error = device_request(KERN_THERMAL_GET_CPU, &cpu);
	if (error != 0)
		return refused("cpu", error);

	/* One field per line, the unknown ones as the kernel gives them. */
	flags_text(cpu_flags, cpu.flags, flags, sizeof(flags));
	printf("cpu %u\n", index);
	printf("package %u\ndie %u\nmodule %u\ncore %u\nthread %u\n",
	       cpu.package, cpu.die, cpu.module, cpu.core, cpu.thread);
	printf("core_class %s\ncore_type 0x%x\nfirmware_id %u\n",
	       value_text(core_names, cpu.core_class), cpu.core_type, cpu.firmware_id);
	printf("domain %u\nvoltage_group %u\nrelative %u\navailable %u\nefficiency %u\n",
	       cpu.domain, cpu.voltage_group, cpu.relative, cpu.available, cpu.efficiency);
	printf("lowest %u\nefficient %u\nguaranteed %u\nhighest %u\n",
	       cpu.lowest, cpu.efficient, cpu.guaranteed, cpu.highest);
	printf("highest_mhz %u\ndelivered_mhz %u\nflags %s\n", cpu.highest_mhz, cpu.delivered_mhz, flags);

	/* Succeeded: the CPU is out. */
	return EXIT_DONE;
}

/* Shows one line per domain: its CPUs, levels, request, ceiling and flags. */
static int
show_domains(void)
{
	struct thermal_cpu_domain domain;
	char flags[FLAGS_TEXT_MAX];
	char limit[NUMBER_TEXT_MAX];
	uint32_t index;
	uint32_t word;
	int error;

	/* Walks the domains until the kernel says there are no more. */
	for (index = 0;; index++) {
		memset(&domain, 0, sizeof(domain));
		domain.domain = index;
		error = device_request(KERN_THERMAL_GET_CPU_DOMAIN, &domain);
		if (error == ENOENT)
			break;
		if (error != 0)
			return refused("domain", error);

		/* The members, as the lowest words of their mask that hold any. */
		printf("domain %u cpus=", index);
		for (word = KERN_THERMAL_CPU_MAX / 64U; word > 1U; word--) {
			if (domain.cpus[word - 1U] != 0U)
				break;
		}
		while (word > 0U) {
			word--;
			printf("%016llx", (unsigned long long)domain.cpus[word]);
		}

		/* The levels, the request and the ceiling. */
		(void)snprintf(limit, sizeof(limit), "-");
		if (domain.limit != KERN_THERMAL_UNKNOWN)
			(void)snprintf(limit, sizeof(limit), "%u", domain.limit);
		flags_text(domain_flags, domain.flags, flags, sizeof(flags));
		if (domain.kind == KERN_THERMAL_CPU_LEVELS_RANGE) {
			printf(" levels=%u..%u", domain.steps[0], domain.steps[1]);
		} else if (domain.kind == KERN_THERMAL_CPU_LEVELS_STEPS) {
			printf(" steps=%u lowest=%u highest=%u", domain.step_count, domain.steps[0],
			       domain.steps[domain.step_count - 1U]);
		} else {
			printf(" levels=-");
		}
		printf(" min=%u max=%u desired=%u epp=%u limit=%s flags=%s\n",
		       domain.minimum, domain.maximum, domain.desired, domain.preference, limit, flags);
	}

	/* Succeeded: every domain is out. */
	return EXIT_DONE;
}

/* Sets one domain, or every domain, directly until the next strategy. */
static int
command_cpu_set(
	int argc,
	char **argv)
{
	struct thermal_cpu_setting setting;
	const char *equal;
	uint32_t value;
	bool parsed;
	int index;
	int error;

	/* The domain, or all, and at least one field. */
	if (argc < 2) {
		usage();
		return EXIT_USAGE;
	}
	memset(&setting, 0, sizeof(setting));
	setting.domain = KERN_THERMAL_ALL;
	if (strcmp(argv[0], "all") != 0) {
		parsed = parse_unsigned(argv[0], UINT32_MAX - 1U, &setting.domain);
		if (!parsed) {
			usage();
			return EXIT_USAGE;
		}
	}

	/* Reads each field=value; a field named twice keeps the last. */
	for (index = 1; index < argc; index++) {
		equal = strchr(argv[index], '=');
		parsed = false;
		if (equal != NULL)
			parsed = parse_unsigned(equal + 1, UINT32_MAX, &value);
		if (!parsed) {
			usage();
			return EXIT_USAGE;
		}
		if (strncmp(argv[index], "min=", 4) == 0) {
			setting.fields |= KERN_THERMAL_CPU_SET_MINIMUM;
			setting.minimum = value;
		} else if (strncmp(argv[index], "max=", 4) == 0) {
			setting.fields |= KERN_THERMAL_CPU_SET_MAXIMUM;
			setting.maximum = value;
		} else if (strncmp(argv[index], "desired=", 8) == 0) {
			setting.fields |= KERN_THERMAL_CPU_SET_DESIRED;
			setting.desired = value;
		} else if (strncmp(argv[index], "epp=", 4) == 0) {
			setting.fields |= KERN_THERMAL_CPU_SET_PREFERENCE;
			setting.preference = value;
		} else {
			usage();
			return EXIT_USAGE;
		}
	}

	/* Hands it to the kernel; a setting the CPUs did not confirm stays and takes effect later. */
	error = device_request(KERN_THERMAL_SET_CPU_DOMAIN, &setting);
	if (error == ETIMEDOUT) {
		printf("cpu set: not confirmed yet; it takes effect at the CPUs' next chance\n");
		return EXIT_DONE;
	}
	if (error != 0)
		return refused("cpu set", error);

	/* Succeeded: the setting is in force until the next strategy. */
	printf("cpu set: in force until the next strategy\n");
	return EXIT_DONE;
}

/* Shows the ways of measuring the relative performance, or chooses one. */
static int
command_relative(
	int argc,
	char **argv)
{
	struct thermal_cpu_relative relative;
	char available[FLAGS_TEXT_MAX];
	static const struct flag_name relative_flags[] = {
		{ KERN_THERMAL_CPU_RELATIVE_FREQUENCY, "frequency" },
		{ KERN_THERMAL_CPU_RELATIVE_FEEDBACK, "feedback" },
		{ KERN_THERMAL_CPU_RELATIVE_PROVIDER, "provider" },
		{ 0, NULL }
	};
	uint32_t source;
	bool known;
	int error;

	/* Chooses the way named, when one is. */
	if (argc > 1) {
		usage();
		return EXIT_USAGE;
	}
	if (argc == 1) {
		known = value_find(relative_names, argv[0], &source);
		if (!known) {
			usage();
			return EXIT_USAGE;
		}
		memset(&relative, 0, sizeof(relative));
		relative.source = source;
		error = device_request(KERN_THERMAL_SET_CPU_RELATIVE, &relative);
		if (error != 0)
			return refused("cpu relative", error);
	}

	/* Shows the ways and the one in force. */
	memset(&relative, 0, sizeof(relative));
	error = device_request(KERN_THERMAL_GET_CPU_RELATIVE, &relative);
	if (error != 0)
		return refused("cpu relative", error);
	flags_text(relative_flags, relative.available, available, sizeof(available));
	printf("relative source=%s available=%s\n", value_text(relative_names, relative.source), available);

	/* Succeeded: the ways are out. */
	return EXIT_DONE;
}

/*
 * Shows the idle residency of a CPU's core or of a package (package 0
 * without arguments), over an interval when one is given.
 */
static int
command_idle(
	int argc,
	char **argv)
{
	struct thermal_cpu_idle first;
	struct thermal_cpu_idle second;
	uint32_t target;
	unsigned seconds;
	bool package;
	bool parsed;
	bool interval;
	int used;
	int error;

	/* The CPU or the package. */
	package = true;
	target = 0;
	if (argc >= 2 && (strcmp(argv[0], "cpu") == 0 || strcmp(argv[0], "package") == 0)) {
		if (strcmp(argv[0], "cpu") == 0)
			package = false;
		parsed = parse_unsigned(argv[1], UINT32_MAX - 1U, &target);
		if (!parsed) {
			usage();
			return EXIT_USAGE;
		}
		argc -= 2;
		argv += 2;
	}

	/* An interval, when one is given, and nothing else. */
	interval = argc > 0;
	parsed = parse_interval(argc, argv, &used, &seconds);
	if (!parsed || used != argc) {
		usage();
		return EXIT_USAGE;
	}

	/* One reading, or two an interval apart. */
	error = idle_read(package, target, &first);
	if (error != 0)
		return refused("idle", error);
	if (!interval) {
		idle_print(package, &first, NULL);
		return EXIT_DONE;
	}
	(void)sleep(seconds);
	error = idle_read(package, target, &second);
	if (error != 0)
		return refused("idle", error);
	idle_print(package, &first, &second);

	/* Succeeded: the residency is out. */
	return EXIT_DONE;
}

/* Reads the residency of a CPU's core or of a package. */
static int
idle_read(
	bool package,
	uint32_t target,
	struct thermal_cpu_idle *idle)
{
	unsigned long request;
	int error;

	/* A core is read on its CPU, a package where the kernel chooses. */
	memset(idle, 0, sizeof(*idle));
	idle->target = target;
	request = KERN_THERMAL_GET_CPU_IDLE;
	if (package)
		request = KERN_THERMAL_GET_PACKAGE_IDLE;
	error = device_request(request, idle);
	if (error != 0)
		return error;

	/* Succeeded: the residency. */
	return 0;
}

/*
 * Prints each state's residency, and with a second reading the share of
 * the interval spent in it.
 */
static void
idle_print(
	bool package,
	const struct thermal_cpu_idle *first,
	const struct thermal_cpu_idle *second)
{
	const char *prefix;
	uint64_t elapsed;
	uint64_t spent;
	uint32_t index;

	/* A package's states are PC<n>, a core's C<n>. */
	prefix = "C";
	if (package)
		prefix = "PC";
	printf("idle %s %u states=%u\n", package ? "package" : "cpu", first->target, first->state_count);

	/* Each state's time, and its share of the interval. */
	elapsed = 0;
	if (second != NULL && second->time_ns > first->time_ns)
		elapsed = second->time_ns - first->time_ns;
	for (index = 0; index < first->state_count && index < KERN_THERMAL_CPU_IDLE_STATE_MAX; index++) {
		if (second == NULL || elapsed == 0U) {
			printf("state %s%u residency_ns=%llu\n", prefix, first->state[index],
			       (unsigned long long)first->residency_ns[index]);
			continue;
		}
		spent = 0;
		if (second->residency_ns[index] > first->residency_ns[index])
			spent = second->residency_ns[index] - first->residency_ns[index];
		printf("state %s%u residency_ns=%llu percent=%llu.%01llu\n", prefix, first->state[index],
		       (unsigned long long)second->residency_ns[index],
		       (unsigned long long)(spent * 100U / elapsed),
		       (unsigned long long)((spent * 1000U / elapsed) % 10U));
	}
}

/* Shows each meter's power over an interval, in watts. */
static int
command_power(
	int argc,
	char **argv)
{
	struct thermal_meter_info first;
	struct thermal_meter_info second;
	struct thermal_status status;
	uint64_t milliwatts;
	uint64_t energy;
	uint64_t elapsed;
	unsigned seconds;
	uint32_t count;
	uint32_t index;
	bool parsed;
	int used;
	int error;

	/* An interval, when one is given, and nothing else. */
	parsed = parse_interval(argc, argv, &used, &seconds);
	if (!parsed || used != argc) {
		usage();
		return EXIT_USAGE;
	}

	/* Finds how many meters there are. */
	memset(&status, 0, sizeof(status));
	error = device_request(KERN_THERMAL_GET_STATUS, &status);
	if (error != 0)
		return refused("status", error);
	count = status.meter_count;
	if (count == 0U) {
		printf("power: no energy meter on this machine\n");
		return EXIT_DONE;
	}

	/* Reads each meter twice, the interval apart, one after the other. */
	for (index = 0; index < count; index++) {
		memset(&first, 0, sizeof(first));
		first.index = index;
		error = device_request(KERN_THERMAL_GET_METER, &first);
		if (error != 0)
			return refused("meter", error);
		if (index == 0U)
			(void)sleep(seconds);
		memset(&second, 0, sizeof(second));
		second.index = index;
		error = device_request(KERN_THERMAL_GET_METER, &second);
		if (error != 0)
			return refused("meter", error);

		/* The energy over the time between the readings the kernel took. */
		elapsed = 0;
		if (second.time_ns > first.time_ns)
			elapsed = second.time_ns - first.time_ns;
		energy = second.energy_nj - first.energy_nj;
		if (elapsed == 0U) {
			printf("power %u %s kind=%s watts=-\n", index, second.name, value_text(meter_names, second.kind));
			continue;
		}
		milliwatts = energy * 1000U / elapsed;
		printf("power %u %s kind=%s watts=%llu.%03llu\n", index, second.name,
		       value_text(meter_names, second.kind),
		       (unsigned long long)(milliwatts / 1000U), (unsigned long long)(milliwatts % 1000U));
	}

	/* Succeeded: the power is out. */
	return EXIT_DONE;
}

/*
 * Prints the events of the types named (every type without names), one
 * line each, until SIGINT or SIGTERM.
 */
static int
command_watch(
	int argc,
	char **argv)
{
	struct thermal_subscription subscription;
	struct thermal_event events[WATCH_BATCH];
	uint32_t type;
	ssize_t got;
	size_t count;
	size_t index;
	bool known;
	int argument;
	int error;

	/* The types named, or every type. */
	memset(&subscription, 0, sizeof(subscription));
	for (argument = 0; argument < argc; argument++) {
		known = value_find(event_names, argv[argument], &type);
		if (!known || type == KERN_THERMAL_EVENT_OVERFLOW) {
			usage();
			return EXIT_USAGE;
		}
		subscription.mask |= 1U << type;
	}
	if (subscription.mask == 0U)
		subscription.mask = KERN_THERMAL_EVENT_MASK & ~(1U << KERN_THERMAL_EVENT_OVERFLOW);

	/* Subscribes; the kernel takes a bounded number of readers. */
	error = device_request(KERN_THERMAL_SUBSCRIBE, &subscription);
	if (error != 0)
		return refused("watch", error);

	/* Ends at an interrupt or a termination. */
	(void)signal(SIGINT, watch_stop);
	(void)signal(SIGTERM, watch_stop);
	setvbuf(stdout, NULL, _IOLBF, 0);

	/* Prints each record as it comes. */
	while (!watch_stopped) {
		got = read(thermal_descriptor, events, sizeof(events));
		if (got < 0 && errno == EINTR)
			continue;
		if (got < 0)
			return refused("watch", errno);
		count = (size_t)got / sizeof(events[0]);
		for (index = 0; index < count; index++) {
			printf("event %llu %s index=%u value=%d name=%s\n",
			       (unsigned long long)events[index].sequence,
			       value_text(event_names, events[index].type), events[index].index,
			       events[index].value, events[index].name[0] != '\0' ? events[index].name : "-");
		}
	}

	/* Succeeded: the watch was ended. */
	return EXIT_DONE;
}

/* Ends the watch at the next record or interrupted read. */
static void
watch_stop(
	int signal_number)
{
	(void)signal_number;

	/* The loop looks at it before its next read. */
	watch_stopped = 1;
}
