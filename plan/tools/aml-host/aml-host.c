/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test harness of the AML interpreter (WS049).
 *
 * It compiles src/drivers/acpi/aml-*.c with the host compiler, implements
 * aml-os.h over the host C library, and simulates the address spaces the
 * way acpiexec does: every byte reads as zero until something writes it.
 *
 *   aml-host [options] TABLE...
 *     --dump          print the namespace, one normalized line per node
 *     --eval PATH     evaluate PATH and print the result (repeatable)
 *     --devices       evaluate _HID _CID _UID _ADR _STA _CRS of every device
 *     --methods       evaluate every method that takes no arguments, in
 *                     namespace order (the paths are listed first)
 *     --main          evaluate \MAIN and exit with 0 when it returned 0
 *     --notify PATH   print the notifications PATH receives (repeatable)
 *     --dynamic FILE  a table LoadTable may load, not loaded at start
 *     --firmware FILE load the tables from a simulated physical memory
 *                     (make-firmware.py): RSDP, XSDT, FADT, DSDT, SSDTs
 *     --reg           connect the address spaces (run _REG) after loading
 *     --init          initialize the devices (_INI by _STA) after loading
 *     --shared-pci    simulate one PCI configuration space for every
 *                     function, as acpiexec does (it keeps one buffer per
 *                     region address, and PCI regions all start at 0)
 *     --absent-pci B:D.F  the PCI function (hex) is not there: its
 *                     configuration space reads as all ones and drops
 *                     writes, as the kernel answers (repeatable)
 *     --absent-pci-fails  fail an access to an absent function with ENODEV
 *                     instead, as the kernel did before BUG-165
 *     --events        read the event hardware from the FADT (the firmware's,
 *                     or a simulated q35-like one) and enable the GPEs
 *     --ec            attach the Embedded Controller (PNP0C09)
 *     --ecdt FILE     start the EC from an ECDT before _REG and _INI, as
 *                     the kernel does when firmware lists one
 *     --ec-ram A=V    preset byte A of the simulated EC (repeatable)
 *     --ec-ports D,C  the simulated EC's data and command ports (hex),
 *                     where a real table's _CRS puts them (default 62,66)
 *     --gpe N         raise GPE N and handle the SCI (repeatable, in order)
 *     --power-button  press the fixed power button and handle the SCI
 *     --poweroff      at the end, turn the power off as KERN_SYSTEM_POWEROFF
 *                     does (with --events: \_S5 and the PM1 control writes;
 *                     the simulated hardware prints the SLP_EN write, BUG-197)
 *     --ec-query Q@G  queue EC query Q, raise the EC's GPE G, handle the SCI
 *     --global-lock   share a simulated FACS Global Lock with a simulated
 *                     firmware (with --events); after MAIN, print the lock
 *                     and handle the SCI firmware raised
 *     --resources PATH  after MAIN, walk the resources of the device PATH
 *                     (drv_acpi_resources_walk) and print one line each
 *     --run-locked MUTEX,PATH  after MAIN, evaluate PATH inside
 *                     drv_acpi_run_locked(MUTEX) and print whether the work
 *                     held the mutex
 *     --notify-once PATH  install a handler on PATH that removes itself at
 *                     its first notification; print how often it ran
 *     --package-arg PATH  after MAIN, evaluate PATH with a package built by
 *                     drv_acpi_object_package_new/set (Integer 0x1234,
 *                     String "zed") as its argument
 *     --stack         print the deepest stack use of the interpreter
 *     --budget BYTES  the stack budget (default 1 MiB)
 *     --quiet         do not print the interpreter's log
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <drivers/acpi/acpi.h>

#include "drivers/acpi/acpi-tables.h"
#include "drivers/acpi/acpi-text.h"
#include "drivers/acpi/aml-internal.h"
#include "drivers/acpi/aml-os.h"

#include "aml-host-hardware.h"

/*
 * The size of one simulated memory page.
 */
#define PAGE_SIZE 4096U

/*
 * The number of hash buckets of simulated pages.
 */
#define PAGE_BUCKETS 1024U

/*
 * The longest path the harness prints.
 */
#define PATH_MAX_LENGTH 512U

/*
 * How many of each repeatable option the harness keeps.
 */
#define OPTION_LIST_MAX 64U

/*
 * The errors the harness reports in place of the kernel's: a page that
 * could not be allocated, memory that is not there, a table that is not
 * listed, and an absent PCI function under --absent-pci-fails.
 */
#define HOST_ENOMEM	12
#define HOST_EFAULT	14
#define HOST_ENOENT	2
#define HOST_ENODEV	19

/*
 * The error drv_acpi_object_package_set() reports for an index past the
 * end, which the --package-arg check expects.
 */
#define HOST_EINVAL	22

/*
 * The options the harness understands.
 */
enum option_kind {
	OPTION_TABLE = 0,
	OPTION_DUMP,
	OPTION_EVAL,
	OPTION_DEVICES,
	OPTION_METHODS,
	OPTION_MAIN,
	OPTION_NOTIFY,
	OPTION_DYNAMIC,
	OPTION_FIRMWARE,
	OPTION_STACK,
	OPTION_SHARED_PCI,
	OPTION_ABSENT_PCI,
	OPTION_ABSENT_PCI_FAILS,
	OPTION_EVENTS,
	OPTION_EC,
	OPTION_ECDT,
	OPTION_EC_RAM,
	OPTION_EC_PORTS,
	OPTION_GPE,
	OPTION_POWER_BUTTON,
	OPTION_EC_QUERY,
	OPTION_GLOBAL_LOCK,
	OPTION_REG,
	OPTION_INIT,
	OPTION_BUDGET,
	OPTION_QUIET,
	OPTION_RESOURCES,
	OPTION_RUN_LOCKED,
	OPTION_NOTIFY_ONCE,
	OPTION_PACKAGE_ARG,
	OPTION_POWEROFF
};

/*
 * One option name and whether a value follows it.
 */
struct option_name {
	const char *text;
	enum option_kind kind;
	int takes_value;
};

/*
 * What the command line asked for.
 *
 * Each list keeps at most OPTION_LIST_MAX entries; its count may run past
 * that while the arguments are read, and parse_arguments() then refuses
 * the command line, so a count the rest of the harness sees always fits.
 */
struct harness_options {
	const char *tables[OPTION_LIST_MAX];
	const char *evaluations[OPTION_LIST_MAX];
	const char *notified[OPTION_LIST_MAX];
	const char *dynamic[OPTION_LIST_MAX];
	const char *firmware;
	const char *ecdt;
	const char *resources;
	const char *run_locked;
	const char *notify_once;
	const char *package_arg;
	unsigned table_count;
	unsigned evaluation_count;
	unsigned notified_count;
	unsigned dynamic_count;
	int dump;
	int devices;
	int methods;
	int run_main;
	int stack;
	int connect;
	int initialize;
	int events;
	int ec;
	int global_lock;
	int poweroff;
	const char *actions[OPTION_LIST_MAX];
	enum option_kind action_kinds[OPTION_LIST_MAX];
	unsigned action_count;
};

/*
 * One table LoadTable may load, read from a file at start.
 */
struct dynamic_table {
	uint8_t *data;
	size_t length;
};

/*
 * One piece of the simulated physical memory of --firmware: a file's bytes
 * at an address.
 */
struct memory_piece {
	uint64_t address;
	uint8_t *data;
	size_t length;
};

/*
 * One simulated page of an address space: zeros until written.
 */
struct page {
	uint64_t space;
	uint64_t number;
	uint8_t bytes[PAGE_SIZE];
	struct page *next;
};

/*
 * The paths of the methods --methods evaluates, collected before any runs,
 * because a method may load or unload tables and change the namespace.
 */
struct method_list {
	char **paths;
	size_t count;
	size_t capacity;
};

/*
 * The mutex and the path the --run-locked work uses, which the work reads
 * because drv_acpi_run_locked() hands it one argument.
 */
struct locked_request {
	const char *mutex;
	const char *path;
};

/*
 * The option names, which option_of() looks the arguments up in.  The
 * table is constant for the life of the harness.
 */
static const struct option_name option_names[] = {
	{ "--dump", OPTION_DUMP, 0 },
	{ "--eval", OPTION_EVAL, 1 },
	{ "--devices", OPTION_DEVICES, 0 },
	{ "--methods", OPTION_METHODS, 0 },
	{ "--main", OPTION_MAIN, 0 },
	{ "--notify", OPTION_NOTIFY, 1 },
	{ "--dynamic", OPTION_DYNAMIC, 1 },
	{ "--firmware", OPTION_FIRMWARE, 1 },
	{ "--stack", OPTION_STACK, 0 },
	{ "--shared-pci", OPTION_SHARED_PCI, 0 },
	{ "--absent-pci", OPTION_ABSENT_PCI, 1 },
	{ "--absent-pci-fails", OPTION_ABSENT_PCI_FAILS, 0 },
	{ "--events", OPTION_EVENTS, 0 },
	{ "--ec", OPTION_EC, 0 },
	{ "--ecdt", OPTION_ECDT, 1 },
	{ "--ec-ram", OPTION_EC_RAM, 1 },
	{ "--ec-ports", OPTION_EC_PORTS, 1 },
	{ "--gpe", OPTION_GPE, 1 },
	{ "--power-button", OPTION_POWER_BUTTON, 0 },
	{ "--ec-query", OPTION_EC_QUERY, 1 },
	{ "--global-lock", OPTION_GLOBAL_LOCK, 0 },
	{ "--reg", OPTION_REG, 0 },
	{ "--init", OPTION_INIT, 0 },
	{ "--budget", OPTION_BUDGET, 1 },
	{ "--quiet", OPTION_QUIET, 0 },
	{ "--resources", OPTION_RESOURCES, 1 },
	{ "--run-locked", OPTION_RUN_LOCKED, 1 },
	{ "--notify-once", OPTION_NOTIFY_ONCE, 1 },
	{ "--package-arg", OPTION_PACKAGE_ARG, 1 },
	{ "--poweroff", OPTION_POWEROFF, 0 },
};

/*
 * The simulated pages of every space, by the hash of space and number.
 * Pages are created on the first access and live until the harness exits.
 */
static struct page *pages[PAGE_BUCKETS];

/*
 * The tables LoadTable may load, filled from --dynamic at start.
 *
 * dynamic_table_count counts the entries filled, each with a whole header;
 * the harness frees their data at exit.
 */
static struct dynamic_table dynamic_tables[OPTION_LIST_MAX];
static unsigned dynamic_table_count;

/*
 * Whether every PCI function shares one simulated configuration space.
 */
static int shared_pci;

/*
 * The PCI functions --absent-pci named, as segment, bus, device and
 * function packed the way space_key() packs them, and whether an access to
 * one fails (--absent-pci-fails) instead of reading all ones.  Filled from
 * the command line and kept until the harness exits.
 */
static uint64_t absent_functions[OPTION_LIST_MAX];
static unsigned absent_function_count;
static int absent_fails;

/*
 * The simulated physical memory of --firmware, and the tables found in it.
 *
 * memory_piece_count counts the pieces read, each with its data, which the
 * harness frees at exit.  firmware_loaded says the firmware record holds
 * tables, which serve LoadTable and are released at exit.
 */
static struct memory_piece memory_pieces[OPTION_LIST_MAX];
static unsigned memory_piece_count;
static struct drv_acpi_firmware firmware;
static int firmware_loaded;

/*
 * Whether the interpreter's log is printed.
 */
static int log_enabled = 1;

/*
 * The stack budget the interpreter is given.
 */
static size_t stack_budget = 1024U * 1024U;

/*
 * The simulated time Sleep and Stall have added, in 100-nanosecond units.
 */
static uint64_t slept;

/*
 * How often the --notify-once handler ran; it removes itself at its first
 * notification, so anything but 1 after a test that notifies twice is a
 * failure of drv_acpi_notify_remove().
 */
static unsigned notify_once_calls;

/*
 * Whether the interpreter lock is held.  The harness has one thread, so
 * the lock only checks that the interpreter takes and lets it go in pairs.
 */
static int lock_held;

static int parse_arguments(int argc, char **argv, struct harness_options *options);
static int record_option(enum option_kind kind, const char *value, struct harness_options *options);
static enum option_kind option_of(const char *text, int *takes_value);
static int install_spaces(const struct harness_options *options);
static int start_events(const struct harness_options *options);
static void run_actions(const struct harness_options *options);
static bool raise_action(enum option_kind kind, const char *value);
static void print_fixed_event(enum drv_acpi_fixed_event event, void *argument);
static int load_tables(const struct harness_options *options);
static int install_notifications(const struct harness_options *options);
static int run_main(void);
static void print_sci(void);
static void release_all(void);
static int read_file(const char *path, uint8_t **data, size_t *length);
static int start_ecdt(const char *path);
static void preset_ec(const struct harness_options *options);
static int load_file(const char *path);
static int load_firmware(const char *description);
static void read_memory_pieces(FILE *stream, unsigned long long *rsdp);
static int read_memory(uint64_t address, void *buffer, size_t length, void *argument);
static int simulated_space(const struct drv_acpi_region_access *access, uint64_t *value, void *argument);
static int simulated_memory(const struct drv_acpi_region_access *access, uint64_t space, uint64_t *value);
static uint64_t space_key(const struct drv_acpi_region_access *access, unsigned space);
static int parse_absent(const char *text);
static int parse_ec_ports(const char *text);
static int absent_function(const struct drv_acpi_region_access *access);
static int absent_access(const struct drv_acpi_region_access *access, uint64_t *value);
static uint8_t *page_byte(uint64_t space, uint64_t address);
static int devices_visitor(struct drv_acpi_node *node, unsigned depth, void *argument);
static void print_notification(struct drv_acpi_node *node, uint32_t value, void *argument);
static int evaluate_and_print(struct drv_acpi_node *scope, const char *path, const char *label);
static int print_namespace(void);
static int evaluate_methods(void);
static int method_visitor(struct drv_acpi_node *node, unsigned depth, void *argument);
static int check_resources(const char *path);
static int print_resource(const struct drv_acpi_resource *resource, void *argument);
static int check_run_locked(const char *request);
static int locked_work(void *argument);
static int install_notify_once(const char *path);
static void notify_once(struct drv_acpi_node *node, uint32_t value, void *argument);
static int check_package_arg(const char *path);

/*
 * Runs the harness.
 */
int
main(
	int argc,
	char **argv)
{
	struct harness_options options;
	unsigned index;
	int error;
	int status;

	/* Reads the options. */
	error = parse_arguments(argc, argv, &options);
	if (error != 0)
		return 2;

	/* Installs the simulated spaces before any AML runs. */
	error = install_spaces(&options);
	if (error != 0)
		return 2;

	/* Presets the EC's bytes before any AML reads them. */
	preset_ec(&options);

	/* Loads the tables; a table that fails makes the exit status 1. */
	status = load_tables(&options);

	/* Prepares their objects as the kernel does. */
	error = drv_acpi_initialize_objects();
	if (error != 0)
		fprintf(stderr, "initialize_objects: error %d\n", error);

	/* Starts the EC an ECDT describes before _REG and _INI, as the kernel does. */
	if (options.ecdt != NULL) {
		error = start_ecdt(options.ecdt);
		if (error != 0)
			status = 1;
	}

	/* Connects the address spaces as the kernel does, when asked to. */
	if (options.connect) {
		error = drv_acpi_region_connect_all();
		if (error != 0)
			status = 1;
	}

	/* Initializes the devices as the kernel does, when asked to. */
	if (options.initialize)
		drv_acpi_initialize_devices();

	/* Installs the notification printers. */
	error = install_notifications(&options);
	if (error != 0)
		status = 1;

	/* Installs the handler that removes itself at its first notification. */
	if (options.notify_once != NULL) {
		error = install_notify_once(options.notify_once);
		if (error != 0)
			status = 1;
	}

	/* Starts the events and the EC. */
	error = start_events(&options);
	if (error != 0)
		status = 1;

	/* Plays the hardware actions. */
	run_actions(&options);

	/* Prints the namespace. */
	if (options.dump) {
		error = print_namespace();
		if (error != 0)
			status = 1;
	}

	/* Runs an ASL test's MAIN first, which returns 0 when every check passed. */
	if (options.run_main) {
		error = run_main();
		if (error != 0)
			status = 1;
	}

	/* Reports how often the self-removing handler ran. */
	if (options.notify_once != NULL)
		printf("NOTIFY-ONCE calls %u\n", notify_once_calls);

	/* Walks the resources of the device the options named; the lines printed (api.output) are the check, a refused template included. */
	if (options.resources != NULL)
		(void)check_resources(options.resources);

	/* Runs an evaluation with an AML mutex held. */
	if (options.run_locked != NULL) {
		error = check_run_locked(options.run_locked);
		if (error != 0)
			status = 1;
	}

	/* Evaluates a method with a package a driver built. */
	if (options.package_arg != NULL) {
		error = check_package_arg(options.package_arg);
		if (error != 0)
			status = 1;
	}

	/* Prints the Global Lock and handles the SCI its firmware raised. */
	if (options.global_lock) {
		printf("GLOBAL-LOCK 0x%x\n", (unsigned)*hardware_global_lock());
		print_sci();
		drv_acpi_events_process();
		print_sci();
	}

	/* Evaluates the paths the options named, in order. */
	for (index = 0; index < options.evaluation_count; index++) {
		/* Evaluates one path and prints its result. */
		error = evaluate_and_print(NULL, options.evaluations[index], options.evaluations[index]);
		if (error != 0)
			status = 1;
	}

	/* Evaluates the identification objects of every device; the visitor never stops the walk. */
	if (options.devices)
		(void)drv_acpi_walk(NULL, devices_visitor, NULL);

	/* Evaluates every method that takes no arguments. */
	if (options.methods) {
		error = evaluate_methods();
		if (error != 0)
			status = 1;
	}

	/* Reports how deep the interpreter went. */
	if (options.stack)
		printf("stack deepest %zu bytes\n", drv_acpi_stack_deepest());

	/* Turns the simulated power off last; the hardware stays on, so the driver reports ETIMEDOUT. */
	if (options.poweroff) {
		error = drv_acpi_poweroff();
		printf("POWEROFF error %d\n", error);
	}

	/* Frees everything so that the leak checker sees a clean exit. */
	release_all();

	/* Reports the outcome: 0 when everything passed, 1 when anything failed. */
	return status;
}

/*
 * Allocates interpreter memory from the host heap.
 */
void *
drv_acpi_os_alloc(
	size_t size)
{
	void *pointer;

	/* A zero-size request still gets a unique pointer. */
	if (size == 0)
		size = 1;

	/* Takes the memory from the host heap. */
	pointer = malloc(size);
	if (pointer == NULL)
		return NULL;

	/* Succeeded: the interpreter owns size bytes until it frees them. */
	return pointer;
}

/*
 * Frees interpreter memory.
 */
void
drv_acpi_os_free(
	void *pointer)
{
	/* Gives the memory back to the host heap; NULL is allowed. */
	free(pointer);
}

/*
 * Prints a line of the interpreter's log to standard error.
 */
void
drv_acpi_os_log(
	const char *format,
	...)
{
	va_list arguments;

	/* Drops the log when asked to be quiet. */
	if (!log_enabled)
		return;

	/* Prints the line to standard error. */
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
}

/*
 * Reports the stack budget.
 */
size_t
drv_acpi_os_stack_budget(void)
{
	/* Reports the configured budget. */
	return stack_budget;
}

/*
 * Simulates a sleep by moving the simulated clock.
 */
void
drv_acpi_os_sleep(
	uint64_t milliseconds)
{
	/* Moves the clock without waiting. */
	slept += milliseconds * 10000U;

	/* Lets the simulated firmware run meanwhile. */
	hardware_tick();
}

/*
 * Simulates a stall by moving the simulated clock.
 */
void
drv_acpi_os_stall(
	uint64_t microseconds)
{
	/* Moves the clock without waiting. */
	slept += microseconds * 10U;
}

/*
 * Reads the clock in 100-nanosecond units: host time plus simulated sleeps.
 */
uint64_t
drv_acpi_os_timer(void)
{
	struct timespec now;
	uint64_t ticks;
	int failed;

	/* Reads the host's monotonic clock; a clock that cannot be read counts as zero. */
	failed = clock_gettime(CLOCK_MONOTONIC, &now);
	if (failed != 0) {
		now.tv_sec = 0;
		now.tv_nsec = 0;
	}

	/* Converts it and adds the simulated time. */
	ticks = (uint64_t)now.tv_sec * 10000000U + (uint64_t)now.tv_nsec / 100U + slept;

	/* Reports the time in 100-nanosecond units. */
	return ticks;
}

/*
 * Reads a port: the simulated hardware, or plain memory elsewhere.
 */
int
drv_acpi_os_port_read(
	uint32_t port,
	unsigned width,
	uint32_t *value)
{
	uint8_t *byte;
	unsigned index;
	int modelled;

	/* The simulated hardware answers for its ports. */
	modelled = hardware_port(port, width, false, value);
	if (modelled)
		return 0;

	/* Anything else is the plain memory of the I/O space. */
	*value = 0;
	for (index = 0; index < width / 8U; index++) {
		/* Finds the byte's simulated memory. */
		byte = page_byte(DRV_ACPI_SPACE_SYSTEM_IO, port + index);
		if (byte == NULL)
			return HOST_ENOMEM;

		/* Puts the byte in its place. */
		*value |= (uint32_t)*byte << (index * 8U);
	}

	/* Succeeded: value holds the port's bytes. */
	return 0;
}

/*
 * Writes a port: the simulated hardware, or plain memory elsewhere.
 */
int
drv_acpi_os_port_write(
	uint32_t port,
	unsigned width,
	uint32_t value)
{
	uint8_t *byte;
	unsigned index;
	int modelled;

	/* The simulated hardware answers for its ports. */
	modelled = hardware_port(port, width, true, &value);
	if (modelled)
		return 0;

	/* Anything else is the plain memory of the I/O space. */
	for (index = 0; index < width / 8U; index++) {
		/* Finds the byte's simulated memory. */
		byte = page_byte(DRV_ACPI_SPACE_SYSTEM_IO, port + index);
		if (byte == NULL)
			return HOST_ENOMEM;

		/* Stores the byte. */
		*byte = (uint8_t)(value >> (index * 8U));
	}

	/* Succeeded: the port holds the value. */
	return 0;
}

/*
 * Takes the event lock; the harness has no interrupt to keep out.
 */
unsigned long
drv_acpi_os_event_lock(void)
{
	/* Reports no interrupt state to restore. */
	return 0;
}

/*
 * Lets the event lock go.
 */
void
drv_acpi_os_event_unlock(
	unsigned long state)
{
	UNUSED_PARAMETER(state);
}

/*
 * Takes the interpreter lock.
 */
void
drv_acpi_os_lock(void)
{
	/* A second take without a let-go is a bug in the interpreter. */
	if (lock_held) {
		fprintf(stderr, "aml-host: interpreter lock taken twice\n");
		abort();
	}

	/* lock_held says the interpreter runs AML now. */
	lock_held = 1;
}

/*
 * Lets the interpreter lock go.
 */
void
drv_acpi_os_unlock(void)
{
	/* A let-go without a take is a bug in the interpreter. */
	if (!lock_held) {
		fprintf(stderr, "aml-host: interpreter lock let go twice\n");
		abort();
	}

	/* lock_held clear says no AML runs. */
	lock_held = 0;
}

/*
 * Reports whether this thread holds the interpreter lock.
 */
bool
drv_acpi_os_lock_owned(void)
{
	/* The harness's one thread holds it whenever it is held. */
	if (lock_held)
		return true;

	/* Reports a lock nobody holds. */
	return false;
}

/*
 * Finds a table LoadTable asks for among the --dynamic tables.
 */
int
drv_acpi_os_table(
	const char *signature,
	const char *oem_id,
	const char *oem_table_id,
	const uint8_t **data,
	size_t *length)
{
	struct dynamic_table *table;
	struct dynamic_table *found;
	unsigned index;
	int compared;

	/* The firmware's tables come first when the tables were loaded from it. */
	if (firmware_loaded) {
		compared = drv_acpi_firmware_find(&firmware, signature, oem_id, oem_table_id, data, length);
		if (compared == 0)
			return 0;
	}

	/* Compares each table's header; an empty identifier matches any. */
	found = NULL;
	for (index = 0; index < dynamic_table_count; index++) {
		/* Skips a table with another signature. */
		table = &dynamic_tables[index];
		compared = memcmp(table->data, signature, 4);
		if (compared != 0)
			continue;

		/* Skips a table with another OEM ID. */
		compared = strncmp((const char *)table->data + 10, oem_id, 6);
		if (oem_id[0] != '\0' && compared != 0)
			continue;

		/* Skips a table with another OEM table ID. */
		compared = strncmp((const char *)table->data + 16, oem_table_id, 8);
		if (oem_table_id[0] != '\0' && compared != 0)
			continue;

		/* Stops at the matching table. */
		found = table;
		break;
	}

	/* Reports that no table matches. */
	if (found == NULL)
		return HOST_ENOENT;

	/* Hands over the table's bytes. */
	*data = found->data;
	*length = found->length;

	/* Succeeded: data and length name the table. */
	return 0;
}

/* Reads the command line into the options. */
static int
parse_arguments(
	int argc,
	char **argv,
	struct harness_options *options)
{
	enum option_kind kind;
	const char *value;
	int takes_value;
	int refused;
	int index;

	/* Starts with nothing asked for. */
	memset(options, 0, sizeof(*options));

	/* Reads each argument. */
	for (index = 1; index < argc; index++) {
		/* Finds what the argument is; it is its own value unless one follows. */
		kind = option_of(argv[index], &takes_value);
		value = argv[index];
		if (takes_value) {
			/* Refuses an option whose value is missing. */
			if (index + 1 >= argc) {
				fprintf(stderr, "%s needs a value\n", argv[index]);
				return 1;
			}

			/* The value is the next argument. */
			index++;
			value = argv[index];
		}

		/* Records the option; one with a bad value refuses the command line. */
		refused = record_option(kind, value, options);
		if (refused != 0)
			return 1;
	}

	/* Refuses more tables or evaluations than the harness keeps. */
	if (options->table_count > OPTION_LIST_MAX || options->evaluation_count > OPTION_LIST_MAX)
		return 1;

	/* Refuses more notified nodes or dynamic tables than the harness keeps. */
	if (options->notified_count > OPTION_LIST_MAX || options->dynamic_count > OPTION_LIST_MAX)
		return 1;

	/* Refuses more hardware actions than the harness keeps. */
	if (options->action_count > OPTION_LIST_MAX) {
		fprintf(stderr, "more than %u --ec-ram, --gpe, --power-button and --ec-query options\n", OPTION_LIST_MAX);
		return 1;
	}

	/* Succeeded: options holds what the command line asked for. */
	return 0;
}

/*
 * Records one option and its value.  A list keeps its first entries and
 * counts the rest, which parse_arguments() refuses afterwards.
 */
static int
record_option(
	enum option_kind kind,
	const char *value,
	struct harness_options *options)
{
	int refused;

	/* Records the option by its kind. */
	refused = 0;
	switch (kind) {
	case OPTION_DUMP:
		options->dump = 1;
		break;
	case OPTION_DEVICES:
		options->devices = 1;
		break;
	case OPTION_METHODS:
		options->methods = 1;
		break;
	case OPTION_MAIN:
		options->run_main = 1;
		break;
	case OPTION_STACK:
		options->stack = 1;
		break;
	case OPTION_SHARED_PCI:
		shared_pci = 1;
		break;
	case OPTION_ABSENT_PCI:
		/* Refuses a function the harness cannot read or keep. */
		refused = parse_absent(value);
		if (refused != 0)
			fprintf(stderr, "--absent-pci needs BUS:DEVICE.FUNCTION: %s\n", value);
		break;
	case OPTION_ABSENT_PCI_FAILS:
		absent_fails = 1;
		break;
	case OPTION_EC_PORTS:
		/* Refuses ports the harness cannot read. */
		refused = parse_ec_ports(value);
		if (refused != 0)
			fprintf(stderr, "--ec-ports needs DATA,COMMAND: %s\n", value);
		break;
	case OPTION_EVENTS:
		options->events = 1;
		break;
	case OPTION_EC:
		options->ec = 1;
		break;
	case OPTION_GLOBAL_LOCK:
		options->global_lock = 1;
		break;
	case OPTION_POWEROFF:
		options->poweroff = 1;
		break;
	case OPTION_EC_RAM:
	case OPTION_GPE:
	case OPTION_POWER_BUTTON:
	case OPTION_EC_QUERY:
		/* The hardware actions run in the order given, after the loading. */
		if (options->action_count < OPTION_LIST_MAX) {
			options->actions[options->action_count] = value;
			options->action_kinds[options->action_count] = kind;
		}

		/* Counts it, kept or not, so that too many refuse the command line. */
		options->action_count++;
		break;
	case OPTION_REG:
		options->connect = 1;
		break;
	case OPTION_INIT:
		options->initialize = 1;
		break;
	case OPTION_QUIET:
		log_enabled = 0;
		break;
	case OPTION_BUDGET:
		stack_budget = (size_t)strtoul(value, NULL, 0);
		break;
	case OPTION_EVAL:
		/* Keeps a path to evaluate. */
		if (options->evaluation_count < OPTION_LIST_MAX)
			options->evaluations[options->evaluation_count] = value;
		options->evaluation_count++;
		break;
	case OPTION_NOTIFY:
		/* Keeps a node whose notifications are printed. */
		if (options->notified_count < OPTION_LIST_MAX)
			options->notified[options->notified_count] = value;
		options->notified_count++;
		break;
	case OPTION_DYNAMIC:
		/* Keeps a table LoadTable may load. */
		if (options->dynamic_count < OPTION_LIST_MAX)
			options->dynamic[options->dynamic_count] = value;
		options->dynamic_count++;
		break;
	case OPTION_FIRMWARE:
		options->firmware = value;
		break;
	case OPTION_ECDT:
		options->ecdt = value;
		break;
	case OPTION_RESOURCES:
		options->resources = value;
		break;
	case OPTION_RUN_LOCKED:
		options->run_locked = value;
		break;
	case OPTION_NOTIFY_ONCE:
		options->notify_once = value;
		break;
	case OPTION_PACKAGE_ARG:
		options->package_arg = value;
		break;
	default:
		/* Keeps a table to load. */
		if (options->table_count < OPTION_LIST_MAX)
			options->tables[options->table_count] = value;
		options->table_count++;
		break;
	}

	/* Reports an option whose value was refused. */
	if (refused != 0)
		return 1;

	/* Succeeded: the option is recorded. */
	return 0;
}

/* Reports which option an argument is, or a table when it is none. */
static enum option_kind
option_of(
	const char *text,
	int *takes_value)
{
	size_t index;
	int compared;

	/* Compares the argument with each option name. */
	*takes_value = 0;
	for (index = 0; index < sizeof(option_names) / sizeof(option_names[0]); index++) {
		/* Reports the matching option. */
		compared = strcmp(text, option_names[index].text);
		if (compared == 0) {
			*takes_value = option_names[index].takes_value;
			return option_names[index].kind;
		}
	}

	/* Anything else is a table file. */
	return OPTION_TABLE;
}

/* Installs the simulated handler for every address space. */
static int
install_spaces(
	const struct harness_options *options)
{
	unsigned space;
	int error;

	/* Every space is plain memory here, as acpiexec simulates it. */
	for (space = 0; space < DRV_ACPI_SPACE_COUNT; space++) {
		/* The EC's space is left to the EC driver when it attaches. */
		if (options->ec && space == DRV_ACPI_SPACE_EMBEDDED_CONTROL)
			continue;

		/* Installs the simulated handler; its argument is the space's number. */
		error = drv_acpi_region_install((enum drv_acpi_space)space, simulated_space, (void *)(uintptr_t)space);
		if (error != 0)
			return error;
	}

	/* Succeeded: every space has a handler. */
	return 0;
}

/*
 * Reads the event hardware from the firmware's FADT or a simulated one,
 * and attaches the EC, when the options ask.
 */
static int
start_events(
	const struct harness_options *options)
{
	uint8_t fadt[512];
	const uint8_t *table;
	size_t length;
	int error;

	/* Reads the event hardware. */
	if (options->events) {
		/* Takes the firmware's FADT, or the simulated one without firmware. */
		table = fadt;
		length = hardware_default_fadt(fadt, sizeof(fadt));
		if (firmware_loaded && firmware.fadt != NULL) {
			table = firmware.fadt;
			length = firmware.fadt_length;
		}

		/* Starts the events from it. */
		error = drv_acpi_events_init(table, length);
		if (error != 0) {
			fprintf(stderr, "events: initialization failed (error %d)\n", error);
			return error;
		}

		/* Prints the power button's presses. */
		error = drv_acpi_fixed_event_install(DRV_ACPI_EVENT_POWER_BUTTON, print_fixed_event, NULL);
		if (error != 0)
			fprintf(stderr, "events: no fixed power button (error %d)\n", error);
	}

	/* Shares the simulated FACS Global Lock. */
	if (options->global_lock) {
		error = drv_acpi_global_lock_attach(hardware_global_lock());
		if (error != 0)
			return error;
	}

	/* Attaches the EC. */
	if (options->ec) {
		error = drv_acpi_ec_attach();
		if (error != 0) {
			fprintf(stderr, "ec: attach failed (error %d)\n", error);
			return error;
		}
	}

	/* Succeeded: the events and the EC the options asked for run. */
	return 0;
}

/* Presets the simulated EC's bytes the --ec-ram options name, before any AML runs. */
static void
preset_ec(
	const struct harness_options *options)
{
	unsigned index;
	int address;
	int value;
	int fields;

	/* Stores each byte an "address=value" names. */
	for (index = 0; index < options->action_count; index++) {
		/* Only the --ec-ram actions preset. */
		if (options->action_kinds[index] != OPTION_EC_RAM)
			continue;

		/* Stores the byte; a value that does not parse presets nothing. */
		fields = sscanf(options->actions[index], "%i=%i", &address, &value);
		if (fields == 2)
			hardware_ec_ram((uint8_t)address, (uint8_t)value);
	}
}

/* Reads an ECDT and starts the EC it describes. */
static int
start_ecdt(
	const char *path)
{
	uint8_t *data;
	size_t length;
	int error;

	/* Reads the table. */
	error = read_file(path, &data, &length);
	if (error != 0) {
		fprintf(stderr, "%s: cannot read\n", path);
		return error;
	}

	/* Starts the EC from it; the EC keeps nothing of the table. */
	error = drv_acpi_ec_ecdt(data, length);
	free(data);
	if (error != 0) {
		fprintf(stderr, "ecdt: error %d\n", error);
		return error;
	}

	/* Succeeded: the ECDT's EC answers AML. */
	return 0;
}

/* Prints whether the SCI has events pending, as the interrupt would find them. */
static void
print_sci(void)
{
	bool pending;

	/* Runs the interrupt's part and prints what it found. */
	pending = drv_acpi_sci_interrupt();
	if (pending) {
		printf("SCI pending\n");
	} else {
		printf("SCI none\n");
	}
}

/* Raises the events the options ask for, in order, and handles each SCI. */
static void
run_actions(
	const struct harness_options *options)
{
	unsigned index;
	bool raised;
	bool pending;

	/* Plays each action. */
	for (index = 0; index < options->action_count; index++) {
		/* Raises the event in the simulated hardware; an action that raises none is skipped. */
		raised = raise_action(options->action_kinds[index], options->actions[index]);
		if (!raised)
			continue;

		/* Handles the SCI as the kernel's interrupt would. */
		pending = drv_acpi_sci_interrupt();
		if (!pending) {
			printf("SCI none\n");
			continue;
		}

		/* The thread's part runs the handlers. */
		printf("SCI pending\n");
		drv_acpi_events_process();
	}
}

/* Raises the event one action names, and reports whether it raised one. */
static bool
raise_action(
	enum option_kind kind,
	const char *value)
{
	unsigned long gpe_number;
	int query;
	int gpe;
	int fields;

	/* Raises the event by the kind of action. */
	switch (kind) {
	case OPTION_GPE:
		/* Raises the GPE the value numbers. */
		gpe_number = strtoul(value, NULL, 0);
		hardware_raise_gpe((unsigned)gpe_number);
		break;
	case OPTION_POWER_BUTTON:
		/* Presses the fixed power button. */
		hardware_raise_fixed(DRV_ACPI_EVENT_POWER_BUTTON);
		break;
	case OPTION_EC_QUERY:
		/* A "query@gpe" queues the query and raises the EC's GPE; another value raises nothing. */
		fields = sscanf(value, "%i@%i", &query, &gpe);
		if (fields != 2)
			return false;
		hardware_ec_query((uint8_t)query);
		hardware_raise_gpe((unsigned)gpe);
		break;
	default:
		/* An --ec-ram action raises nothing; it preset the EC already. */
		return false;
	}

	/* Reports an event raised. */
	return true;
}

/* Prints a fixed event. */
static void
print_fixed_event(
	enum drv_acpi_fixed_event event,
	void *argument)
{
	UNUSED_PARAMETER(argument);

	/* Prints the power button by its name. */
	if (event == DRV_ACPI_EVENT_POWER_BUTTON) {
		printf("FIXED power-button\n");
		return;
	}

	/* Prints any other fixed event by its number. */
	printf("FIXED %u\n", (unsigned)event);
}

/* Reads the dynamic tables and loads the others in order. */
static int
load_tables(
	const struct harness_options *options)
{
	struct dynamic_table *table;
	unsigned index;
	int error;
	int status;

	/* Reads each table LoadTable may load. */
	status = 0;
	for (index = 0; index < options->dynamic_count; index++) {
		/* Reads the file into the next free entry. */
		table = &dynamic_tables[dynamic_table_count];
		error = read_file(options->dynamic[index], &table->data, &table->length);
		if (error != 0) {
			status = 1;
			continue;
		}

		/* Keeps it only when it has a whole header. */
		if (table->length < 36U) {
			free(table->data);
			status = 1;
			continue;
		}

		/* The entry is filled; the count keeps it for LoadTable. */
		dynamic_table_count++;
	}

	/* Loads the firmware's tables from the simulated memory. */
	if (options->firmware != NULL) {
		error = load_firmware(options->firmware);
		if (error != 0)
			status = 1;
	}

	/* Loads each table in order. */
	for (index = 0; index < options->table_count; index++) {
		/* Loads the table the file holds. */
		error = load_file(options->tables[index]);
		if (error != 0)
			status = 1;
	}

	/* Reports 0 when every table loaded, 1 otherwise. */
	return status;
}

/* Installs a printer of notifications on each node the options named. */
static int
install_notifications(
	const struct harness_options *options)
{
	struct drv_acpi_node *node;
	unsigned index;
	int error;

	/* Resolves each path and installs the printer. */
	for (index = 0; index < options->notified_count; index++) {
		/* Resolves the path. */
		error = drv_acpi_lookup(NULL, options->notified[index], &node);
		if (error != 0) {
			fprintf(stderr, "%s: no such node\n", options->notified[index]);
			return error;
		}

		/* Installs the printer. */
		error = drv_acpi_notify_install(node, print_notification, NULL);
		if (error != 0)
			return error;
	}

	/* Succeeded: every named node prints its notifications. */
	return 0;
}

/* Runs \MAIN and reports whether it returned zero. */
static int
run_main(void)
{
	struct drv_acpi_object *result;
	uint64_t value;
	int error;

	/* Evaluates \MAIN. */
	error = drv_acpi_evaluate(NULL, "\\MAIN", NULL, 0, &result);
	if (error != 0) {
		printf("MAIN failed: error %d\n", error);
		return 1;
	}

	/* Takes what it returned. */
	value = drv_acpi_object_integer(result);
	drv_acpi_object_release(result);

	/* Zero is success, anything else the failing check. */
	if (value != 0) {
		printf("MAIN returned 0x%llx\n", (unsigned long long)value);
		return 1;
	}

	/* Succeeded: every check of the test passed. */
	printf("MAIN passed\n");
	return 0;
}

/* Frees what the harness holds, so that the leak checker sees a clean exit. */
static void
release_all(void)
{
	unsigned index;

	/* Forgets the namespace and the tables. */
	drv_acpi_reset();

	/* Releases the firmware record. */
	if (firmware_loaded)
		drv_acpi_firmware_release(&firmware);

	/* Frees the simulated physical memory. */
	for (index = 0; index < memory_piece_count; index++)
		free(memory_pieces[index].data);

	/* Frees the tables LoadTable might have loaded. */
	for (index = 0; index < dynamic_table_count; index++)
		free(dynamic_tables[index].data);
}

/* Reads a whole file into memory the caller frees. */
static int
read_file(
	const char *path,
	uint8_t **data,
	size_t *length)
{
	FILE *stream;
	uint8_t *bytes;
	long size;
	size_t count;
	int failed;

	/* Opens the file. */
	stream = fopen(path, "rb");
	if (stream == NULL) {
		perror(path);
		return 1;
	}

	/* Moves to its end to measure it. */
	failed = fseek(stream, 0, SEEK_END);
	if (failed != 0) {
		fclose(stream);
		return 1;
	}

	/* Measures it. */
	size = ftell(stream);
	if (size < 0) {
		fclose(stream);
		return 1;
	}

	/* Moves back to its start. */
	failed = fseek(stream, 0, SEEK_SET);
	if (failed != 0) {
		fclose(stream);
		return 1;
	}

	/* Allocates room for it. */
	bytes = malloc((size_t)size + 1U);
	if (bytes == NULL) {
		fclose(stream);
		return 1;
	}

	/* Reads it whole. */
	count = fread(bytes, 1, (size_t)size, stream);
	fclose(stream);
	if (count != (size_t)size) {
		free(bytes);
		return 1;
	}

	/* Hands over the bytes. */
	*data = bytes;
	*length = (size_t)size;

	/* Succeeded: the caller owns the file's bytes. */
	return 0;
}

/* Reads a table file and loads it. */
static int
load_file(
	const char *path)
{
	uint8_t *data;
	size_t length;
	int error;

	/* Reads the file. */
	error = read_file(path, &data, &length);
	if (error != 0)
		return error;

	/* Loads the table; the interpreter keeps its own copy. */
	error = drv_acpi_load_table(data, length);
	free(data);
	if (error != 0) {
		fprintf(stderr, "%s: load error %d\n", path, error);
		return 1;
	}

	/* Succeeded: the table's names are in the namespace. */
	return 0;
}

/*
 * Reads the description of a simulated physical memory, then finds and
 * loads the tables in it as the kernel does.
 */
static int
load_firmware(
	const char *description)
{
	unsigned long long rsdp;
	FILE *stream;
	int error;

	/* Opens the description. */
	stream = fopen(description, "r");
	if (stream == NULL) {
		perror(description);
		return 1;
	}

	/* Reads the RSDP's address and each piece of memory. */
	rsdp = 0;
	read_memory_pieces(stream, &rsdp);

	/* The description is read. */
	fclose(stream);

	/* Finds the tables from the RSDP. */
	error = drv_acpi_firmware_discover(rsdp, read_memory, NULL, &firmware);
	if (error != 0) {
		fprintf(stderr, "%s: no ACPI tables found (error %d)\n", description, error);
		return 1;
	}

	/* firmware_loaded says the record is released at exit and serves LoadTable meanwhile. */
	firmware_loaded = 1;

	/* Loads the DSDT and the SSDTs. */
	error = drv_acpi_firmware_load(&firmware);
	if (error != 0) {
		fprintf(stderr, "%s: the DSDT did not load (error %d)\n", description, error);
		return 1;
	}

	/* Succeeded: the firmware's tables are loaded. */
	return 0;
}

/* Reads the lines of a memory description: the RSDP's address and the pieces. */
static void
read_memory_pieces(
	FILE *stream,
	unsigned long long *rsdp)
{
	struct memory_piece *piece;
	unsigned long long address;
	char line[1024];
	char path[1024];
	char *got;
	int fields;
	int error;

	/* Reads the description a line at a time. */
	for (;;) {
		/* Stops at the end of the description. */
		got = fgets(line, sizeof(line), stream);
		if (got == NULL)
			break;

		/* Takes the RSDP line. */
		fields = sscanf(line, "rsdp %llx", rsdp);
		if (fields == 1)
			continue;

		/* Skips a line that is no piece, and any piece beyond those the harness keeps. */
		fields = sscanf(line, "%llx %1023s", &address, path);
		if (fields != 2 || memory_piece_count == OPTION_LIST_MAX)
			continue;

		/* Reads the file whose bytes are at the address; one that cannot be read is skipped. */
		piece = &memory_pieces[memory_piece_count];
		error = read_file(path, &piece->data, &piece->length);
		if (error != 0)
			continue;

		/* The piece is filled; the count keeps it. */
		piece->address = address;
		memory_piece_count++;
	}
}

/* Reads the simulated physical memory; a range outside every piece cannot be read. */
static int
read_memory(
	uint64_t address,
	void *buffer,
	size_t length,
	void *argument)
{
	struct memory_piece *piece;
	struct memory_piece *found;
	unsigned index;

	UNUSED_PARAMETER(argument);

	/* Finds the piece that holds the whole range. */
	found = NULL;
	for (index = 0; index < memory_piece_count; index++) {
		/* Skips a piece the range does not start in. */
		piece = &memory_pieces[index];
		if (address < piece->address || address - piece->address > piece->length)
			continue;

		/* Skips a piece the range runs past. */
		if (length > piece->length - (size_t)(address - piece->address))
			continue;

		/* Stops at the piece. */
		found = piece;
		break;
	}

	/* Reports memory that is not there. */
	if (found == NULL)
		return HOST_EFAULT;

	/* Copies the bytes. */
	memcpy(buffer, found->data + (address - found->address), length);

	/* Succeeded: buffer holds the memory's bytes. */
	return 0;
}

/* Reads or writes a simulated address space. */
static int
simulated_space(
	const struct drv_acpi_region_access *access,
	uint64_t *value,
	void *argument)
{
	uint64_t space;
	uint32_t port_value;
	int modelled;
	int absent;
	int error;

	/* The simulated hardware answers for its I/O ports. */
	if ((uintptr_t)argument == DRV_ACPI_SPACE_SYSTEM_IO) {
		port_value = (uint32_t)*value;
		modelled = hardware_port((uint32_t)access->address, access->width, access->write, &port_value);
		if (modelled) {
			/* Hands a read's value over. */
			if (!access->write)
				*value = port_value;
			return 0;
		}
	}

	/* Finds whether the access is to a PCI function that is not there. */
	absent = 0;
	if ((uintptr_t)argument == DRV_ACPI_SPACE_PCI_CONFIG)
		absent = absent_function(access);

	/*
	 * An absent function answers as the kernel does, without the memory;
	 * anything else is the simulated memory of its space, which the
	 * argument numbers.
	 */
	if (absent) {
		error = absent_access(access, value);
	} else {
		space = space_key(access, (unsigned)(uintptr_t)argument);
		error = simulated_memory(access, space, value);
	}

	/* Reports an access that failed. */
	if (error != 0)
		return error;

	/* Succeeded: a read leaves the space's value in value. */
	return 0;
}

/* Moves the bytes of one access through the simulated memory of a space. */
static int
simulated_memory(
	const struct drv_acpi_region_access *access,
	uint64_t space,
	uint64_t *value)
{
	unsigned bytes;
	unsigned index;
	uint8_t *byte;

	/* A read starts from zero. */
	bytes = access->width / 8U;
	if (!access->write)
		*value = 0;

	/* Moves the bytes lowest first. */
	for (index = 0; index < bytes; index++) {
		/* Finds the simulated byte. */
		byte = page_byte(space, access->address + index);
		if (byte == NULL)
			return HOST_ENOMEM;

		/* Stores or loads it. */
		if (access->write) {
			*byte = (uint8_t)(*value >> (index * 8U));
		} else {
			*value |= (uint64_t)*byte << (index * 8U);
		}
	}

	/* Succeeded: every byte of the access moved. */
	return 0;
}

/*
 * Keeps every address space apart, and every PCI function in a space of
 * its own unless --shared-pci asked for one for all.
 */
static uint64_t
space_key(
	const struct drv_acpi_region_access *access,
	unsigned space)
{
	uint64_t function;

	/* A space other than PCI configuration is keyed by its number alone. */
	if (space != DRV_ACPI_SPACE_PCI_CONFIG)
		return space;

	/* One configuration space for every function, as acpiexec simulates it. */
	if (shared_pci)
		return DRV_ACPI_SPACE_PCI_CONFIG;

	/* Combines the segment, bus, device and function. */
	function = (uint64_t)access->pci_segment << 16;
	function |= (uint64_t)access->pci_bus << 8;
	function |= (uint64_t)access->pci_device << 3;
	function |= access->pci_function;

	/* Reports the key, leaving room below for the numbers of the spaces. */
	return (function + 1U) << 8;
}

/* Records the PCI function an --absent-pci value names (hex bus:device.function). */
static int
parse_absent(
	const char *text)
{
	unsigned long bus;
	unsigned long device;
	unsigned long function;
	char *end;

	/* Refuses more functions than the harness keeps. */
	if (absent_function_count >= OPTION_LIST_MAX)
		return 1;

	/* Reads the bus, which a colon ends. */
	bus = strtoul(text, &end, 16);
	if (*end != ':' || bus > 0xffU)
		return 1;

	/* Reads the device, which a dot ends. */
	device = strtoul(end + 1, &end, 16);
	if (*end != '.' || device > 0x1fU)
		return 1;

	/* Reads the function, which ends the value. */
	function = strtoul(end + 1, &end, 16);
	if (*end != '\0' || function > 7U)
		return 1;

	/* Keeps it packed as space_key() packs a function of segment 0. */
	absent_functions[absent_function_count] = (uint64_t)bus << 8;
	absent_functions[absent_function_count] |= (uint64_t)device << 3;
	absent_functions[absent_function_count] |= (uint64_t)function;
	absent_function_count++;

	/* Succeeded: accesses to the function answer as absent. */
	return 0;
}

/* Moves the simulated EC to the ports an --ec-ports value names (hex data,command). */
static int
parse_ec_ports(
	const char *text)
{
	unsigned long data;
	unsigned long command;
	char *end;

	/* Reads the data port, which a comma ends. */
	data = strtoul(text, &end, 16);
	if (*end != ',' || data > 0xffffU)
		return 1;

	/* Reads the command port, which ends the value. */
	command = strtoul(end + 1, &end, 16);
	if (*end != '\0' || command > 0xffffU)
		return 1;

	/* Moves the EC there. */
	hardware_ec_ports((uint32_t)data, (uint32_t)command);

	/* Succeeded: the EC answers on the ports. */
	return 0;
}

/* Reports whether a PCI_Config access is to a function --absent-pci named. */
static int
absent_function(
	const struct drv_acpi_region_access *access)
{
	uint64_t function;
	unsigned index;

	/* Packs the function the access is to. */
	function = (uint64_t)access->pci_segment << 16;
	function |= (uint64_t)access->pci_bus << 8;
	function |= (uint64_t)access->pci_device << 3;
	function |= access->pci_function;

	/* Looks for it among the absent ones. */
	for (index = 0; index < absent_function_count; index++) {
		/* Reports a function --absent-pci named. */
		if (absent_functions[index] == function)
			return 1;
	}

	/* The function is there. */
	return 0;
}

/*
 * Answers an access to an absent PCI function: every bit set on a read and
 * a write dropped, as the bus and the kernel do, or ENODEV with
 * --absent-pci-fails, as the kernel did before BUG-165.
 */
static int
absent_access(
	const struct drv_acpi_region_access *access,
	uint64_t *value)
{
	/* Fails the access the way the kernel used to. */
	if (absent_fails)
		return HOST_ENODEV;

	/* A write goes nowhere. */
	if (access->write)
		return 0;

	/* A read finds every bit of its width set; 64 bits are all of them. */
	if (access->width >= 64U) {
		*value = UINT64_MAX;
	} else {
		*value = ((uint64_t)1 << access->width) - 1U;
	}

	/* Succeeded: the read has its value. */
	return 0;
}

/* Finds the simulated byte at an address, creating its page. */
static uint8_t *
page_byte(
	uint64_t space,
	uint64_t address)
{
	struct page *page;
	uint64_t number;
	unsigned bucket;

	/* Finds the page in its bucket. */
	number = address / PAGE_SIZE;
	bucket = (unsigned)((number * 31U + space) % PAGE_BUCKETS);
	for (page = pages[bucket];
	     page != NULL;
	     page = page->next) {
		/* Stops at the page of this space and number. */
		if (page->space == space && page->number == number)
			return &page->bytes[address % PAGE_SIZE];
	}

	/* Allocates the page, zero-filled. */
	page = calloc(1, sizeof(*page));
	if (page == NULL)
		return NULL;

	/* Names it and puts it at the head of its bucket. */
	page->space = space;
	page->number = number;
	page->next = pages[bucket];
	pages[bucket] = page;

	/* Succeeded: reports the byte in the new page. */
	return &page->bytes[address % PAGE_SIZE];
}

/* Evaluates the identification objects of one device. */
static int
devices_visitor(
	struct drv_acpi_node *node,
	unsigned depth,
	void *argument)
{
	static const char *const names[] = { "_HID", "_CID", "_UID", "_ADR", "_STA", "_CRS" };
	struct drv_acpi_node *found;
	char path[PATH_MAX_LENGTH];
	char label[PATH_MAX_LENGTH + 8];
	size_t index;
	int error;

	UNUSED_PARAMETER(depth);
	UNUSED_PARAMETER(argument);

	/* Only devices are visited. */
	if (node->object == NULL || node->object->type != DRV_ACPI_TYPE_DEVICE)
		return 0;

	/* Writes the device's path for the labels. */
	error = drv_acpi_node_path(node, path, sizeof(path));
	if (error != 0)
		return 0;

	/* Evaluates each object the device itself has. */
	for (index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
		/* Skips an object the device does not have. */
		error = drv_acpi_lookup(node, names[index], &found);
		if (error != 0 || found->parent != node)
			continue;

		/* Evaluates it under the label "device.name"; a failure is printed as its value. */
		snprintf(label, sizeof(label), "%s.%s", path, names[index]);
		(void)evaluate_and_print(node, names[index], label);
	}

	/* Goes on into the children. */
	return 0;
}

/* Lists every method without arguments, then evaluates each by its path. */
static int
evaluate_methods(void)
{
	struct method_list list;
	size_t index;
	int decision;

	/* Lists the methods; the visitor stops the walk when memory runs out. */
	memset(&list, 0, sizeof(list));
	decision = drv_acpi_walk(NULL, method_visitor, &list);

	/* Evaluates each listed method, unless the list is incomplete. */
	for (index = 0; index < list.count; index++) {
		/* Evaluates one method; a failure is printed and the next one runs. */
		if (decision >= 0)
			(void)evaluate_and_print(NULL, list.paths[index], list.paths[index]);

		/* Frees the path. */
		free(list.paths[index]);
	}

	/* Frees the list. */
	free(list.paths);

	/* Reports a list the walk could not complete. */
	if (decision < 0) {
		fprintf(stderr, "methods: the list of methods ran out of memory\n");
		return HOST_ENOMEM;
	}

	/* Succeeded: every method without arguments was evaluated. */
	return 0;
}

/* Adds a method without arguments to the list. */
static int
method_visitor(
	struct drv_acpi_node *node,
	unsigned depth,
	void *argument)
{
	struct method_list *list;
	char path[PATH_MAX_LENGTH];
	char **grown;
	char *copy;
	int error;

	UNUSED_PARAMETER(depth);

	/* Only methods are listed. */
	list = argument;
	if (node->object == NULL || node->object->type != DRV_ACPI_TYPE_METHOD)
		return 0;

	/* Only methods without arguments are listed. */
	if (node->object->value.method.argument_count != 0)
		return 0;

	/* Writes the path; a path too long to print is not listed. */
	error = drv_acpi_node_path(node, path, sizeof(path));
	if (error != 0)
		return 0;

	/* Grows the list when it is full; running out of memory stops the walk. */
	if (list->count == list->capacity) {
		list->capacity = list->capacity * 2U + 64U;
		grown = realloc(list->paths, list->capacity * sizeof(list->paths[0]));
		if (grown == NULL)
			return -1;
		list->paths = grown;
	}

	/* Copies the path; running out of memory stops the walk. */
	copy = strdup(path);
	if (copy == NULL)
		return -1;

	/* Keeps the copy. */
	list->paths[list->count] = copy;
	list->count++;

	/* Succeeded: goes on with the walk. */
	return 0;
}

/* Evaluates one path and prints the result under a label, as /dev/acpi writes it. */
static int
evaluate_and_print(
	struct drv_acpi_node *scope,
	const char *path,
	const char *label)
{
	struct drv_acpi_text text;
	int error;

	/* Writes the result line. */
	drv_acpi_text_init(&text);
	error = drv_acpi_text_evaluate(&text, scope, path, label);

	/* Prints it, a failed evaluation's line too. */
	if (text.data != NULL)
		fputs(text.data, stdout);

	/* Frees the text. */
	drv_acpi_text_release(&text);

	/* Reports a failed evaluation. */
	if (error != 0)
		return error;

	/* Succeeded: the result is printed. */
	return 0;
}

/* Prints the namespace, one line per node, as /dev/acpi writes it. */
static int
print_namespace(void)
{
	struct drv_acpi_text text;
	int error;

	/* Writes the lines. */
	drv_acpi_text_init(&text);
	error = drv_acpi_text_namespace(&text);

	/* Prints them, as many as were written. */
	if (text.data != NULL)
		fputs(text.data, stdout);

	/* Frees the text. */
	drv_acpi_text_release(&text);

	/* Reports a namespace that could not be written whole. */
	if (error != 0)
		return error;

	/* Succeeded: the namespace is printed. */
	return 0;
}

/* Prints a notification a node received. */
static void
print_notification(
	struct drv_acpi_node *node,
	uint32_t value,
	void *argument)
{
	char path[PATH_MAX_LENGTH];
	int error;

	UNUSED_PARAMETER(argument);

	/* Finds the node's path, or a mark when it does not fit. */
	error = drv_acpi_node_path(node, path, sizeof(path));
	if (error != 0)
		strcpy(path, "(long)");

	/* Prints the node and the value. */
	printf("NOTIFY %s 0x%X\n", path, (unsigned)value);
}

/* Walks the resources of a device and prints one line each. */
static int
check_resources(
	const char *path)
{
	struct drv_acpi_node *device;
	int error;

	/* Finds the device. */
	error = drv_acpi_lookup(NULL, path, &device);
	if (error != 0) {
		fprintf(stderr, "%s: no such device\n", path);
		return error;
	}

	/* Walks its _CRS. */
	error = drv_acpi_resources_walk(device, NULL, print_resource, NULL);
	if (error != 0) {
		printf("RESOURCES error %d\n", error);
		return error;
	}

	/* Marks the end of the list. */
	printf("RESOURCES end\n");

	/* Succeeded: every resource is printed. */
	return 0;
}

/* Prints one resource of a walk. */
static int
print_resource(
	const struct drv_acpi_resource *resource,
	void *argument)
{
	const char *kind;

	UNUSED_PARAMETER(argument);

	/* A connection is printed with its controller (ws159-p002). */
	if (resource->kind == DRV_ACPI_RESOURCE_I2C) {
		printf("RESOURCE i2c address=0x%llX speed=%u ten_bit=%u desc=0x%02X p=%u source=%s\n",
		       (unsigned long long)resource->base,
		       (unsigned)resource->speed,
		       (unsigned)resource->ten_bit,
		       (unsigned)resource->descriptor,
		       (unsigned)resource->producer,
		       resource->source);
		return 0;
	}
	if (resource->kind == DRV_ACPI_RESOURCE_GPIO_INT) {
		printf("RESOURCE gpio-int pin=0x%llX pins=%llu desc=0x%02X p=%u level=%u low=%u shared=%u wake=%u source=%s\n",
		       (unsigned long long)resource->base,
		       (unsigned long long)resource->length,
		       (unsigned)resource->descriptor,
		       (unsigned)resource->producer,
		       (unsigned)resource->level,
		       (unsigned)resource->active_low,
		       (unsigned)resource->shared,
		       (unsigned)resource->wake,
		       resource->source);
		return 0;
	}

	/* Names the kind. */
	kind = "irq";
	if (resource->kind == DRV_ACPI_RESOURCE_IO) {
		kind = "io";
	} else if (resource->kind == DRV_ACPI_RESOURCE_MEMORY) {
		kind = "memory";
	}

	/* Prints the resource. */
	printf("RESOURCE %s base=0x%llX len=0x%llX desc=0x%02X w=%u p=%u level=%u low=%u shared=%u\n",
	       kind,
	       (unsigned long long)resource->base,
	       (unsigned long long)resource->length,
	       (unsigned)resource->descriptor,
	       (unsigned)resource->writable,
	       (unsigned)resource->producer,
	       (unsigned)resource->level,
	       (unsigned)resource->active_low,
	       (unsigned)resource->shared);

	/* Goes on to the next resource. */
	return 0;
}

/* Evaluates a path inside drv_acpi_run_locked(), as "MUTEX,PATH" names them. */
static int
check_run_locked(
	const char *request)
{
	struct locked_request locked;
	char mutex[PATH_MAX_LENGTH];
	const char *comma;
	size_t length;
	int error;

	/* Splits the mutex from the path. */
	comma = strchr(request, ',');
	if (comma == NULL || (size_t)(comma - request) >= sizeof(mutex)) {
		fprintf(stderr, "--run-locked needs MUTEX,PATH: %s\n", request);
		return 1;
	}

	/* Copies the mutex's name, terminated. */
	length = (size_t)(comma - request);
	memcpy(mutex, request, length);
	mutex[length] = '\0';

	/* Runs the work. */
	locked.mutex = mutex;
	locked.path = comma + 1;
	error = drv_acpi_run_locked(mutex, locked_work, &locked);
	if (error != 0) {
		printf("LOCKED error %d\n", error);
		return error;
	}

	/* Succeeded: the work ran with the mutex held. */
	return 0;
}

/* The --run-locked work: reports whether the entry holds the mutex, then evaluates the path. */
static int
locked_work(
	void *argument)
{
	struct locked_request *locked;
	struct drv_acpi_node *node;
	struct drv_acpi_thread *thread;
	int error;

	/* Finds the mutex. */
	locked = argument;
	error = drv_acpi_lookup(NULL, locked->mutex, &node);
	if (error != 0)
		return error;

	/* Prints whether the running entry owns it. */
	thread = drv_acpi_active_thread();
	if (node->object != NULL && node->object->value.mutex.owner == thread) {
		printf("LOCKED held\n");
	} else {
		printf("LOCKED not-held\n");
	}

	/* Evaluates the path, which may acquire the mutex again. */
	error = evaluate_and_print(NULL, locked->path, locked->path);
	if (error != 0)
		return error;

	/* Succeeded: the path was evaluated with the mutex held. */
	return 0;
}

/* Installs the handler that removes itself at its first notification. */
static int
install_notify_once(
	const char *path)
{
	struct drv_acpi_node *node;
	int error;

	/* Finds the node. */
	error = drv_acpi_lookup(NULL, path, &node);
	if (error != 0) {
		fprintf(stderr, "%s: no such node\n", path);
		return error;
	}

	/* Installs the handler. */
	error = drv_acpi_notify_install(node, notify_once, NULL);
	if (error != 0)
		return error;

	/* Succeeded: the node's first notification reaches the handler. */
	return 0;
}

/* Counts a notification and removes the handler, so that no later one reaches it. */
static void
notify_once(
	struct drv_acpi_node *node,
	uint32_t value,
	void *argument)
{
	int error;

	UNUSED_PARAMETER(value);
	UNUSED_PARAMETER(argument);

	/* Counts the call. */
	notify_once_calls++;

	/* Removes the handler from inside the delivery. */
	error = drv_acpi_notify_remove(node, notify_once, NULL);
	if (error != 0)
		printf("NOTIFY-ONCE remove error %d\n", error);
}

/* Evaluates a method with a package of an integer and a string as its argument. */
static int
check_package_arg(
	const char *path)
{
	struct drv_acpi_object *arguments[1];
	struct drv_acpi_object *package;
	struct drv_acpi_object *integer;
	struct drv_acpi_object *string;
	struct drv_acpi_object *result;
	int error;

	/* Allocates the package of two elements. */
	package = drv_acpi_object_package_new(2);
	if (package == NULL)
		return HOST_ENOMEM;

	/* Sets the integer element; the package takes a reference of its own. */
	integer = drv_acpi_object_integer_new(0x1234);
	error = drv_acpi_object_package_set(package, 0, integer);
	drv_acpi_object_release(integer);
	if (error != 0) {
		drv_acpi_object_release(package);
		return error;
	}

	/* Sets the string element the same way. */
	string = drv_acpi_object_string_new("zed");
	error = drv_acpi_object_package_set(package, 1, string);
	drv_acpi_object_release(string);
	if (error != 0) {
		drv_acpi_object_release(package);
		return error;
	}

	/* Checks that the setter refuses an index past the end. */
	error = drv_acpi_object_package_set(package, 2, NULL);
	if (error != HOST_EINVAL) {
		printf("PACKAGE-ARG index past the end gave %d\n", error);
		drv_acpi_object_release(package);
		return 1;
	}

	/* Evaluates the method with the package; the package is done with afterwards. */
	arguments[0] = package;
	result = NULL;
	error = drv_acpi_evaluate(NULL, path, arguments, 1, &result);
	drv_acpi_object_release(package);
	if (error != 0) {
		printf("PACKAGE-ARG error %d\n", error);
		return error;
	}

	/* Prints the method's answer. */
	printf("PACKAGE-ARG %s = 0x%llX\n", path, (unsigned long long)drv_acpi_object_integer(result));
	drv_acpi_object_release(result);

	/* Succeeded: the method saw the package. */
	return 0;
}
