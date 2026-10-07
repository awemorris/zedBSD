/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the UCSI driver's ACPI transport (ws050-p003) over the
 * Latitude 5330's firmware.
 *
 * The AML interpreter (src/drivers/acpi/aml-*.c), the transport
 * (src/drivers/typec/ucsi-acpi.c), the UCSI core and the Type-C layer are
 * compiled with the host compiler and loaded with the 5330's DSDT and
 * SSDTs (plan/ws049/tests/latitude5330/).  The simulated machine has
 * plain memory, PCI functions that are absent, and an Embedded Controller
 * whose RAM is a byte array: a small PPM in it answers the commands the
 * 5330's _DSM function 1 hands it (EC 0xB0 = 0xE0) and raises the EC query
 * 0x79, which the test's wait runs as the event thread would.  GNVS says
 * the UCSI device is there (USTC 1, OSYS 2015, UBCB the mailbox), and the
 * EC holds the version 1.2.
 *
 * The checks: the device is found and present (_STA 0x0F), _CRS gives the
 * mailbox as a fixed 32-bit range of 0x1000 bytes, VERSION picks the 1.x
 * arrangement, _DSM function 0 answers Buffer {0x1F}, a refresh read runs
 * function 2 (the mailbox then holds the EC's CCI and MESSAGE IN), a write
 * runs function 1 (the EC then holds CONTROL and MESSAGE OUT), the core
 * starts over the firmware (two connectors, the first attached), and a
 * plug on the second is read after its notification and acknowledged.
 * Also: /dev/typec's text, and a mailbox in RAM refused.  Prints "PASS
 * name" or "FAIL name ..." per check and exits with 1 when one failed.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uapi/errno.h>

#include <drivers/acpi/acpi.h>
#include <drivers/typec/typec.h>
#include <drivers/typec/ucsi-acpi.h>

#include "drivers/acpi/aml-os.h"
#include "drivers/typec/typec-os.h"

/* The 5330's GNVS region and the fields of it the UCSI device reads (offsets in it). */
#define GNVS_BASE 0x6150C000ULL
#define GNVS_OSYS 0x000U
#define GNVS_UBCB 0x855U
#define GNVS_USTC 0x910U

/*
 * The GNVS fields of the 5330's USB-C connectors (ssdt8's CR01 and CR02 and
 * their _PLD, ws050-p005): how many there are, each one's use and type,
 * and the group position its _PLD takes (TPnP when TPnD's type bits are 0).
 */
#define GNVS_TTUP 0x971U
#define GNVS_TP1P 0x973U
#define GNVS_TP1D 0x974U
#define GNVS_TP2P 0x976U
#define GNVS_TP2D 0x977U
#define GNVS_TP1U 0xBBCU
#define GNVS_TP2U 0xBBDU

/* What GNVS says: Windows 2015 or later, the UCSI device enabled, and the mailbox. */
#define TEST_OSYS 0x07DFU
#define TEST_MAILBOX 0x6F000000ULL

/* The simulated memory: this many pages, each made on its first use. */
#define MEMORY_PAGES 256U
#define MEMORY_PAGE 4096U

/* A page the test calls RAM: a mailbox placed there must be refused. */
#define TEST_RAM 0x00100000ULL

/* The 5330's EC addresses of the UCSI mailbox (ssdt8's _DSM and _Q79). */
#define EC_VERSION 0x80U
#define EC_CCI 0x84U
#define EC_CONTROL 0x88U
#define EC_MESSAGE_IN 0x90U
#define EC_MESSAGE_OUT 0xA0U
#define EC_DOORBELL 0xB0U
#define EC_DOORBELL_COMMAND 0xE0U

/* The commands the PPM answers (UCSI 1.2 Table A-1). */
#define PPM_RESET 0x01U
#define PPM_ACK_CC_CI 0x04U
#define PPM_SET_NOTIFICATION_ENABLE 0x05U
#define PPM_GET_CAPABILITY 0x06U
#define PPM_GET_CONNECTOR_STATUS 0x12U

/* CCI's indicators (Table 3-2). */
#define PPM_CCI_NOT_SUPPORTED (1UL << 25)
#define PPM_CCI_RESET (1UL << 27)
#define PPM_CCI_ACK (1UL << 29)
#define PPM_CCI_DONE (1UL << 31)

/* The PPM's connectors. */
#define PPM_CONNECTORS 2U

/* The path of the EC query that copies the mailbox and notifies. */
#define QUERY_PATH "\\_SB.PC00.LPCB.ECDV._Q79"

/*
 * One page of the simulated memory: its address, and its bytes.
 */
struct memory_page {
	uint64_t address;
	uint8_t *bytes;
};

/*
 * The PPM in the EC: which connectors are attached, the connector whose
 * change is indicated until it is acknowledged, whether the EC query is
 * raised, and how many commands it saw.
 */
struct ppm {
	bool attached[PPM_CONNECTORS + 1U];
	unsigned change;
	bool query;
	unsigned commands;
	unsigned last_command;
	unsigned resets;
	unsigned writes;
	unsigned changes_acknowledged;
};

/*
 * The simulated memory's pages.  Made on use, freed at the end.
 */
static struct memory_page memory[MEMORY_PAGES];

/*
 * The EC's RAM.
 */
static uint8_t ec_ram[256];

/*
 * The PPM.
 */
static struct ppm ppm;

/*
 * The interpreter lock, and the Type-C layer's lock (the host has one
 * thread; each must not be taken twice).
 */
static bool interpreter_locked;
static bool typec_locked;

/*
 * The signal of the notification, and how many notifications came.
 */
static bool signalled;
static unsigned notifications;

/*
 * The thread the driver asked for, and whether /dev/typec was published.
 */
static void (*thread_body)(void *argument);
static unsigned devices;

/* The number of failed checks, and whether to print the logs. */
static int test_failures;
static bool verbose;

static void test_check(const char *name, bool condition, const char *detail);
static void test_firmware(void);
static void test_driver(void);
static void test_plug(void);
static void test_text(void);
static void test_ram(void);
static void test_displays(void);
static int dsm_call(unsigned function, struct drv_acpi_object **result);
static int crs_visitor(const struct drv_acpi_resource *resource, void *argument);
static int load_tables(const char *directory);
static int load_file(const char *path);
static uint8_t *memory_byte(uint64_t address);
static void memory_put(uint64_t address, uint64_t value, unsigned bytes);
static int space_handler(const struct drv_acpi_region_access *access, uint64_t *value, void *argument);
static void ppm_command(void);
static void ppm_answer(uint32_t cci, const uint8_t *message, unsigned length);
static void ppm_plug(unsigned number);

/*
 * The UUID of the UCSI _DSM as the bytes of its buffer, as the test gives
 * it to the firmware itself.
 */
static const uint8_t test_uuid[16] = {
	0xc2, 0x98, 0x83, 0x6f, 0xa4, 0x7c, 0xe4, 0x11,
	0xad, 0x36, 0x63, 0x10, 0x42, 0xb5, 0x00, 0x8f
};

/*
 * Runs the checks.
 */
int
main(
	int argc,
	char **argv)
{
	int compared;
	int error;

	/* Each line printed at once, so a sanitizer's stop does not hide the checks before it. */
	setvbuf(stdout, NULL, _IOLBF, 0);

	/* The tables' directory, and -v for the logs. */
	if (argc < 2) {
		fprintf(stderr, "usage: ucsi-acpi-host TABLES [-v]\n");
		return 2;
	}

	/* -v prints the logs. */
	compared = 1;
	if (argc > 2)
		compared = strcmp(argv[2], "-v");
	if (compared == 0)
		verbose = true;

	/* The address spaces. */
	(void)drv_acpi_region_install(DRV_ACPI_SPACE_SYSTEM_MEMORY, space_handler, (void *)(uintptr_t)DRV_ACPI_SPACE_SYSTEM_MEMORY);
	(void)drv_acpi_region_install(DRV_ACPI_SPACE_SYSTEM_IO, space_handler, (void *)(uintptr_t)DRV_ACPI_SPACE_SYSTEM_IO);
	(void)drv_acpi_region_install(DRV_ACPI_SPACE_PCI_CONFIG, space_handler, (void *)(uintptr_t)DRV_ACPI_SPACE_PCI_CONFIG);
	(void)drv_acpi_region_install(DRV_ACPI_SPACE_EMBEDDED_CONTROL, space_handler, (void *)(uintptr_t)DRV_ACPI_SPACE_EMBEDDED_CONTROL);

	/*
	 * GNVS and the EC as the 5330's firmware leaves them, before the
	 * tables load (the mailbox's region takes UBCB then): the UCSI device
	 * enabled, version 1.2.
	 */
	memory_put(GNVS_BASE + GNVS_OSYS, TEST_OSYS, 2);
	memory_put(GNVS_BASE + GNVS_UBCB, TEST_MAILBOX, 4);
	memory_put(GNVS_BASE + GNVS_USTC, 1, 1);

	/*
	 * Two USB-C connectors, CR01 and CR02, at group positions 1 and 2:
	 * those of the Type-C subsystem's USB 3 ports SS01 and SS02 (ssdt12's
	 * PLCA and PLCB), the display's TC1 and TC2 (assumed: the 5330's own
	 * values are to be read from its boot log).
	 */
	memory_put(GNVS_BASE + GNVS_TTUP, 2, 1);
	memory_put(GNVS_BASE + GNVS_TP1U, 1, 1);
	memory_put(GNVS_BASE + GNVS_TP2U, 1, 1);
	memory_put(GNVS_BASE + GNVS_TP1D, 0, 1);
	memory_put(GNVS_BASE + GNVS_TP2D, 0, 1);
	memory_put(GNVS_BASE + GNVS_TP1P, 1, 1);
	memory_put(GNVS_BASE + GNVS_TP2P, 2, 1);
	ec_ram[EC_VERSION] = 0x20;
	ec_ram[EC_VERSION + 1U] = 0x01;

	/* The tables and their objects. */
	error = load_tables(argv[1]);
	test_check("tables", error == 0, "the DSDT and the SSDTs loaded");
	if (error != 0)
		return 1;
	error = drv_acpi_initialize_objects();
	test_check("objects", error == 0, "prepared");

	/* _REG tells the EC's region is there (ECRD 1), as the kernel's EC attach does. */
	error = drv_acpi_region_connect_all();
	test_check("reg", error == 0, "_REG ran");

	/* The checks. */
	test_firmware();
	test_driver();
	test_displays();
	test_plug();
	test_text();
	test_ram();

	/* Reports whether every check passed. */
	if (test_failures != 0)
		return 1;

	/* Succeeded: every check passed. */
	return 0;
}

/* Prints one check's outcome and counts a failure. */
static void
test_check(
	const char *name,
	bool condition,
	const char *detail)
{
	/* A failure is counted. */
	if (!condition) {
		printf("FAIL %s: %s\n", name, detail);
		test_failures++;
		return;
	}

	/* A pass. */
	printf("PASS %s: %s\n", name, detail);
}

/*
 * Checks what the transport relies on in the firmware itself: _STA, _CRS,
 * _DSM function 0, and that functions 2 and 1 move the mailbox.
 */
static void
test_firmware(void)
{
	struct drv_acpi_object *result;
	struct drv_acpi_resource found;
	struct drv_acpi_node *device;
	enum drv_acpi_type type;
	const uint8_t *bytes;
	uint8_t *mailbox;
	uint64_t status;
	size_t length;
	int error;

	/* _STA: present, enabled, shown and working. */
	status = 0;
	error = drv_acpi_evaluate_integer(NULL, "\\_SB.UBTC._STA", &status);
	test_check("sta", error == 0 && status == 0x0FU, "_STA is 0x0F with USTC 1, OSYS 2015 and a version in the EC");

	/* _CRS: the mailbox, a fixed 32-bit range of 0x1000 bytes at UBCB. */
	memset(&found, 0, sizeof(found));
	error = drv_acpi_lookup(NULL, "\\_SB.UBTC", &device);
	if (error == 0)
		error = drv_acpi_resources_walk(device, "_CRS", crs_visitor, &found);
	if (error == -1)
		error = 0;
	test_check("crs", error == 0 && found.descriptor == 0x86U && found.base == TEST_MAILBOX && found.length == 0x1000U, "_CRS is Memory32Fixed at UBCB, 0x1000 bytes");

	/* _DSM function 0: a buffer whose bits 0 to 4 are set. */
	result = NULL;
	error = dsm_call(0, &result);
	bytes = NULL;
	length = 0;
	type = DRV_ACPI_TYPE_UNINITIALIZED;
	if (error == 0 && result != NULL)
		type = drv_acpi_object_type(result);
	if (type == DRV_ACPI_TYPE_BUFFER)
		bytes = drv_acpi_object_buffer(result, &length);
	test_check("dsm-0", bytes != NULL && length == 1U && bytes[0] == 0x1FU, "_DSM 0 is Buffer {0x1F}");
	if (result != NULL)
		drv_acpi_object_release(result);

	/* _DSM function 2: the EC's CCI and MESSAGE IN copied to the mailbox. */
	mailbox = memory_byte(TEST_MAILBOX);
	ec_ram[EC_CCI] = 0x12;
	ec_ram[EC_CCI + 3U] = 0x80;
	ec_ram[EC_MESSAGE_IN] = 0x5A;
	ec_ram[EC_MESSAGE_IN + 15U] = 0xA5;
	result = NULL;
	error = dsm_call(2, &result);
	if (result != NULL)
		drv_acpi_object_release(result);
	test_check("dsm-2", error == 0 && mailbox[4] == 0x12 && mailbox[7] == 0x80 && mailbox[16] == 0x5A && mailbox[31] == 0xA5, "_DSM 2 copied CCI and MESSAGE IN from the EC");

	/* _DSM function 1: CONTROL and MESSAGE OUT copied to the EC and the doorbell rung. */
	mailbox[8] = 0x55;
	mailbox[15] = 0x66;
	mailbox[32] = 0x77;
	mailbox[47] = 0x88;
	ppm.writes = 0;
	result = NULL;
	error = dsm_call(1, &result);
	if (result != NULL)
		drv_acpi_object_release(result);
	test_check("dsm-1", error == 0 && ec_ram[EC_CONTROL] == 0x55 && ec_ram[EC_CONTROL + 7U] == 0x66 && ec_ram[EC_MESSAGE_OUT] == 0x77 && ec_ram[EC_MESSAGE_OUT + 15U] == 0x88 && ppm.writes == 1U, "_DSM 1 copied CONTROL and MESSAGE OUT to the EC and rang 0xB0");

	/* The PPM forgets the test's command. */
	ppm.query = false;
	ppm.commands = 0;
	ppm.writes = 0;
}

/* Attaches and starts the driver over the firmware. */
static void
test_driver(void)
{
	struct drv_typec_connector connector;
	uint8_t *mailbox;
	unsigned count;
	int error;

	/* The PPM: connector 1 attached to a UFP, 2 not. */
	ppm.attached[1] = true;

	/* The attach: the device, _STA, _CRS, VERSION, _DSM 0, the notification and the thread. */
	error = drv_ucsi_acpi_attach();
	test_check("attach", error == 0, "the UCSI device attached");
	test_check("thread", thread_body != NULL, "the driver's thread was asked for");
	test_check("device", devices == 1U, "/dev/typec was published");
	mailbox = memory_byte(TEST_MAILBOX);
	test_check("version", mailbox[0] == 0x20 && mailbox[1] == 0x01, "_STA put the EC's VERSION 1.2 in the mailbox");

	/* The core's start over the firmware: every command through _DSM 1, every answer through _Q79 or _DSM 2. */
	error = drv_ucsi_acpi_start();
	test_check("start", error == 0, "the PPM was reset and read through the firmware");
	count = drv_typec_connector_count();
	test_check("connectors", count == PPM_CONNECTORS, "two connectors");
	test_check("doorbell", ppm.commands > 5U, "_DSM 1 rang the EC for the commands");
	test_check("notified", notifications > 0U, "the EC query's Notify reached the driver");

	/* Connector 1 as the PPM reports it. */
	memset(&connector, 0, sizeof(connector));
	error = drv_typec_connector_get(0, &connector);
	test_check("connector-1", error == 0 && connector.connected && connector.partner_type == DRV_TYPEC_PARTNER_UFP && connector.power_operation == DRV_TYPEC_POWER_PD && connector.request_data_object == 0x12345678U, "connector 1 is attached to a UFP under a PD contract");
	error = drv_typec_connector_get(1, &connector);
	test_check("connector-2", error == 0 && !connector.connected, "connector 2 is detached");
}

/* Plugs something into connector 2 and lets the driver's thread take the notification. */
static void
test_plug(void)
{
	struct drv_typec_connector connector;
	uint32_t serial;
	int error;

	/* The plug, indicated in CCI and raised as the EC query. */
	ppm_plug(2);

	/* One step of the thread: the notification, then the connector read and its change acknowledged. */
	error = drv_ucsi_acpi_step(10);
	test_check("plug-notified", error == 1, "the thread took the notification");
	test_check("plug-ack", ppm.change == 0 && ppm.changes_acknowledged == 1U, "the change was acknowledged once");
	memset(&connector, 0, sizeof(connector));
	error = drv_typec_connector_get(1, &connector);
	test_check("plug-record", error == 0 && connector.connected && connector.partner_type == DRV_TYPEC_PARTNER_UFP, "connector 2 is attached");

	/* Another step with nothing raised. */
	error = drv_ucsi_acpi_step(10);
	test_check("idle", error == 0, "no notification, nothing read");

	/* An operation asked by another driver wakes the thread, which sends it through _DSM 1. */
	ppm.last_command = 0;
	error = drv_typec_connector_reset(0, DRV_TYPEC_RESET_HARD, &serial);
	test_check("request-asked", error == 0 && signalled, "queued and the thread woken");
	error = drv_ucsi_acpi_step(10);
	memset(&connector, 0, sizeof(connector));
	(void)drv_typec_connector_get(0, &connector);
	test_check("request-done", error == 1 && ppm.resets == 1U && connector.request_serial == serial && connector.request_error == 0, "CONNECTOR_RESET reached the EC and its outcome the record");
}

/* Checks the text /dev/typec gives. */
static void
test_text(void)
{
	char text[1024];
	const char *line;
	size_t length;

	/* The text, one line per connector. */
	length = drv_typec_text(text, sizeof(text));
	if (verbose)
		printf("%s", text);
	line = strstr(text, "connector 1: attached partner=ufp power=pd role=sink usb rdo=0x12345678");
	test_check("text-1", line != NULL, "connector 1's line");
	line = strstr(text, "\nconnector 2: attached partner=ufp");
	test_check("text-2", line != NULL && length == strlen(text) && text[length - 1U] == '\n', "connector 2's line, the text whole");

	/* A small buffer is cut short and still terminated. */
	length = drv_typec_text(text, 16);
	test_check("text-short", length == 15U && text[15] == '\0', "cut to the buffer");
}

/* Moves the mailbox into RAM and checks the driver refuses it. */
static void
test_ram(void)
{
	int error;

	/* UBCB in RAM. */
	memory_put(GNVS_BASE + GNVS_UBCB, TEST_RAM, 4);
	error = drv_ucsi_acpi_attach();
	test_check("ram", error == ENODEV, "a mailbox in RAM is not used");
}

/* Calls a function of the UCSI _DSM as the firmware's caller would. */
static int
dsm_call(
	unsigned function,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *arguments[4];
	unsigned index;
	int error;

	/* The UUID, revision 1, the function and an empty package. */
	arguments[0] = drv_acpi_object_buffer_new(test_uuid, sizeof(test_uuid));
	arguments[1] = drv_acpi_object_integer_new(1);
	arguments[2] = drv_acpi_object_integer_new(function);
	arguments[3] = drv_acpi_object_package_new(0);

	/* The call. */
	*result = NULL;
	error = drv_acpi_evaluate(NULL, "\\_SB.UBTC._DSM", arguments, 4, result);

	/* The arguments are no longer needed. */
	for (index = 0; index < 4U; index++)
		drv_acpi_object_release(arguments[index]);

	/* Reports what the call reported. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Keeps the first resource of _CRS. */
static int
crs_visitor(
	const struct drv_acpi_resource *resource,
	void *argument)
{
	/* The first one, and the walk stops (a negative value is the visitor's own stop). */
	memcpy(argument, resource, sizeof(*resource));
	return -1;
}

/* Loads dsdt.dat and ssdt1.dat to ssdt15.dat from a directory. */
static int
load_tables(
	const char *directory)
{
	char path[1024];
	unsigned number;
	int error;

	/* The DSDT first. */
	(void)snprintf(path, sizeof(path), "%s/dsdt.dat", directory);
	error = load_file(path);
	if (error != 0)
		return error;

	/* Then each SSDT, in the firmware's order. */
	for (number = 1; number <= 15U; number++) {
		(void)snprintf(path, sizeof(path), "%s/ssdt%u.dat", directory, number);
		error = load_file(path);
		if (error != 0)
			return error;
	}

	/* Succeeded: every table's names are in the namespace. */
	return 0;
}

/* Loads one table from a file. */
static int
load_file(
	const char *path)
{
	uint8_t *data;
	FILE *stream;
	long size;
	size_t got;
	int error;

	/* The file's bytes. */
	stream = fopen(path, "rb");
	if (stream == NULL) {
		perror(path);
		return 1;
	}

	/* Its size. */
	(void)fseek(stream, 0, SEEK_END);
	size = ftell(stream);
	(void)fseek(stream, 0, SEEK_SET);
	if (size <= 0) {
		fclose(stream);
		return 1;
	}

	/* A buffer for it. */
	data = malloc((size_t)size);
	if (data == NULL) {
		fclose(stream);
		return 1;
	}

	/* The bytes. */
	got = fread(data, 1, (size_t)size, stream);
	fclose(stream);
	if (got != (size_t)size) {
		free(data);
		return 1;
	}

	/* The table; the interpreter keeps its own copy. */
	error = drv_acpi_load_table(data, (size_t)size);
	free(data);
	if (error != 0) {
		fprintf(stderr, "%s: load error %d\n", path, error);
		return 1;
	}

	/* Succeeded. */
	return 0;
}

/* Finds a byte of the simulated memory, making its page on first use. */
static uint8_t *
memory_byte(
	uint64_t address)
{
	uint64_t page;
	unsigned index;

	/* The page that holds it, among those made. */
	page = address & ~(uint64_t)(MEMORY_PAGE - 1U);
	for (index = 0; index < MEMORY_PAGES; index++) {
		/* A free slot makes the page. */
		if (memory[index].bytes == NULL) {
			memory[index].address = page;
			memory[index].bytes = calloc(1, MEMORY_PAGE);
			if (memory[index].bytes == NULL)
				abort();
		}

		/* The page. */
		if (memory[index].address == page)
			return memory[index].bytes + (address - page);
	}

	/* The simulated memory is full. */
	fprintf(stderr, "ucsi-acpi-host: out of simulated memory\n");
	abort();
}

/* Puts a little-endian value in the simulated memory. */
static void
memory_put(
	uint64_t address,
	uint64_t value,
	unsigned bytes)
{
	unsigned index;

	/* Each byte. */
	for (index = 0; index < bytes; index++)
		*memory_byte(address + index) = (uint8_t)(value >> (index * 8U));
}

/*
 * Makes one access of a region: memory and I/O are plain memory, every PCI
 * function is absent (all ones, writes dropped), and the EC is its RAM
 * with the PPM behind the doorbell.
 */
static int
space_handler(
	const struct drv_acpi_region_access *access,
	uint64_t *value,
	void *argument)
{
	unsigned space;
	unsigned bytes;
	unsigned index;
	uint64_t address;

	/* The space, and the bytes of the access. */
	space = (unsigned)(uintptr_t)argument;
	bytes = access->width / 8U;

	/* An absent PCI function. */
	if (space == DRV_ACPI_SPACE_PCI_CONFIG) {
		if (!access->write)
			*value = ~(uint64_t)0;
		return 0;
	}

	/* The EC's RAM, and the doorbell that runs the PPM. */
	if (space == DRV_ACPI_SPACE_EMBEDDED_CONTROL) {
		if (access->address >= sizeof(ec_ram))
			return EFAULT;
		if (!access->write) {
			*value = ec_ram[access->address];
			return 0;
		}

		/* A write, which may ring the doorbell. */
		ec_ram[access->address] = (uint8_t)*value;
		if (access->address == EC_DOORBELL && (uint8_t)*value == EC_DOORBELL_COMMAND)
			ppm_command();
		return 0;
	}

	/* Plain memory (I/O ports a space of their own above 4 GiB). */
	address = access->address;
	if (space == DRV_ACPI_SPACE_SYSTEM_IO)
		address += 0x100000000ULL;
	if (!access->write)
		*value = 0;
	for (index = 0; index < bytes; index++) {
		if (access->write) {
			*memory_byte(address + index) = (uint8_t)(*value >> (index * 8U));
		} else {
			*value |= (uint64_t)*memory_byte(address + index) << (index * 8U);
		}
	}

	/* Succeeded. */
	return 0;
}

/*
 * Answers the command the EC's CONTROL holds, as a UCSI 1.2 PPM with two
 * connectors, and raises the EC query 0x79.
 */
static void
ppm_command(void)
{
	uint8_t message[16];
	uint64_t control;
	unsigned command;
	unsigned number;
	unsigned index;

	/* The command. */
	control = 0;
	for (index = 0; index < 8U; index++)
		control |= (uint64_t)ec_ram[EC_CONTROL + index] << (index * 8U);
	command = (unsigned)(control & 0xFFU);
	number = (unsigned)((control >> 16) & 0x7FU);
	ppm.writes++;
	ppm.commands++;
	ppm.last_command = command;
	if (command == 0x03U)
		ppm.resets++;
	memset(message, 0, sizeof(message));

	/* Each command the core sends. */
	switch (command) {
	case PPM_RESET:
		ppm.change = 0;
		ppm_answer(PPM_CCI_RESET, message, 0);
		break;
	case PPM_ACK_CC_CI:
		if ((control & (1ULL << 16)) != 0 && ppm.change != 0) {
			ppm.change = 0;
			ppm.changes_acknowledged++;
		}

		/* The acknowledgement. */
		ppm_answer(PPM_CCI_ACK, message, 0);
		break;
	case PPM_SET_NOTIFICATION_ENABLE:
		ppm_answer(PPM_CCI_DONE, message, 0);
		break;
	case PPM_GET_CAPABILITY:
		message[4] = PPM_CONNECTORS;
		ppm_answer(PPM_CCI_DONE, message, 16);
		break;
	case PPM_GET_CONNECTOR_STATUS:
		if (number >= 1U && number <= PPM_CONNECTORS && ppm.attached[number]) {
			/* PD (3) at bits 16-18, attached at 19, sink, USB at 21, a UFP (2) at 29-31, the RDO at 32. */
			message[2] = 0x03 | 0x08 | 0x20;
			message[3] = 0x40;
			message[4] = 0x78;
			message[5] = 0x56;
			message[6] = 0x34;
			message[7] = 0x12;
		}

		/* The status. */
		ppm_answer(PPM_CCI_DONE, message, 9);
		break;
	default:
		/* The rest (the capability of a connector, the modes, the PDOs) has nothing to report. */
		ppm_answer(PPM_CCI_DONE, message, 0);
		break;
	}
}

/* Puts an answer in the EC (with the change still indicated) and raises the query. */
static void
ppm_answer(
	uint32_t cci,
	const uint8_t *message,
	unsigned length)
{
	unsigned index;

	/* CCI with the length and the connector whose change is indicated. */
	cci |= (uint32_t)length << 8;
	cci |= (uint32_t)ppm.change << 1;
	for (index = 0; index < 4U; index++)
		ec_ram[EC_CCI + index] = (uint8_t)(cci >> (index * 8U));

	/* MESSAGE IN. */
	for (index = 0; index < 16U; index++)
		ec_ram[EC_MESSAGE_IN + index] = message[index];

	/* The EC query that copies them and notifies. */
	ppm.query = true;
}

/* Attaches something to a connector and indicates the change. */
static void
ppm_plug(
	unsigned number)
{
	uint8_t message[16];

	/* The change, in a CCI of its own. */
	ppm.attached[number] = true;
	ppm.change = number;
	memset(message, 0, sizeof(message));
	ppm_answer(0, message, 0);
}

/*
 * The AML interpreter's OS layer over the host.
 */

void *
drv_acpi_os_alloc(
	size_t size)
{
	/* A zero-size request still gets a unique pointer. */
	if (size == 0)
		size = 1;
	return malloc(size);
}

void
drv_acpi_os_free(
	void *pointer)
{
	free(pointer);
}

void
drv_acpi_os_log(
	const char *format,
	...)
{
	va_list arguments;

	/* Quiet unless -v. */
	if (!verbose)
		return;
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
}

size_t
drv_acpi_os_stack_budget(void)
{
	/* As much as the kernel gives. */
	return 48U * 1024U;
}

void
drv_acpi_os_sleep(
	uint64_t milliseconds)
{
	(void)milliseconds;
}

void
drv_acpi_os_stall(
	uint64_t microseconds)
{
	(void)microseconds;
}

uint64_t
drv_acpi_os_timer(void)
{
	static uint64_t clock;

	/* A clock that moves by a millisecond each read. */
	clock += 10000U;
	return clock;
}

int
drv_acpi_os_port_read(
	uint32_t port,
	unsigned width,
	uint32_t *value)
{
	(void)port;
	(void)width;
	*value = 0;
	return 0;
}

int
drv_acpi_os_port_write(
	uint32_t port,
	unsigned width,
	uint32_t value)
{
	(void)port;
	(void)width;
	(void)value;
	return 0;
}

unsigned long
drv_acpi_os_event_lock(void)
{
	return 0;
}

void
drv_acpi_os_event_unlock(
	unsigned long state)
{
	(void)state;
}

void
drv_acpi_os_lock(void)
{
	/* A second take would deadlock in the kernel. */
	if (interpreter_locked) {
		fprintf(stderr, "ucsi-acpi-host: interpreter lock taken twice\n");
		abort();
	}

	/* Held. */
	interpreter_locked = true;
}

void
drv_acpi_os_unlock(void)
{
	interpreter_locked = false;
}

bool
drv_acpi_os_lock_owned(void)
{
	return interpreter_locked;
}

int
drv_acpi_os_table(
	const char *signature,
	const char *oem_id,
	const char *oem_table_id,
	const uint8_t **data,
	size_t *length)
{
	(void)signature;
	(void)oem_id;
	(void)oem_table_id;
	(void)data;
	(void)length;
	return ENOENT;
}

/*
 * The Type-C layer's OS layer over the host.
 */

void
drv_typec_os_lock(void)
{
	/* A nested lock would deadlock in the kernel; so would one held over AML. */
	if (typec_locked || interpreter_locked) {
		fprintf(stderr, "ucsi-acpi-host: the Type-C lock taken twice or inside the interpreter\n");
		abort();
	}

	/* Held. */
	typec_locked = true;
}

void
drv_typec_os_unlock(void)
{
	typec_locked = false;
}

uint64_t
drv_typec_os_now_ms(void)
{
	/* The host has no use for the time: the display comparison is the other test's. */
	return 0;
}

void
drv_typec_os_log(
	const char *format,
	...)
{
	va_list arguments;

	/* Quiet unless -v. */
	if (!verbose)
		return;
	va_start(arguments, format);
	(void)vprintf(format, arguments);
	va_end(arguments);
}

int
drv_typec_os_map(
	uint64_t physical,
	size_t size,
	volatile uint8_t **mapping)
{
	/* The HAL refuses RAM. */
	if (physical < TEST_RAM + MEMORY_PAGE && physical + size > TEST_RAM)
		return EFAULT;

	/* The simulated memory is contiguous within a page, which the mailbox fits. */
	if ((physical & (MEMORY_PAGE - 1U)) + size > MEMORY_PAGE)
		return EFAULT;
	*mapping = memory_byte(physical);
	return 0;
}

uint8_t
drv_typec_os_read8(
	const volatile uint8_t *address)
{
	return *address;
}

void
drv_typec_os_write8(
	volatile uint8_t *address,
	uint8_t value)
{
	*address = value;
}

void
drv_typec_os_signal_init(void)
{
	signalled = false;
}

void
drv_typec_os_signal(void)
{
	/* Raised (by the notification inside the interpreter, or by an operation outside it). */
	signalled = true;
	if (interpreter_locked)
		notifications++;
}

int
drv_typec_os_wait(
	uint32_t milliseconds)
{
	struct drv_acpi_object *result;
	int error;

	/* The time passes; the EC's event thread runs a raised query meanwhile. */
	(void)milliseconds;
	if (ppm.query) {
		ppm.query = false;
		result = NULL;
		error = drv_acpi_evaluate(NULL, QUERY_PATH, NULL, 0, &result);
		if (result != NULL)
			drv_acpi_object_release(result);
		if (error != 0)
			fprintf(stderr, "ucsi-acpi-host: _Q79 failed (%d)\n", error);
	}

	/* The signal, lowered. */
	if (!signalled)
		return 0;
	signalled = false;
	return 1;
}

int
drv_typec_os_thread_start(
	void (*body)(void *argument),
	void *argument)
{
	/* The test runs the thread's steps itself. */
	(void)argument;
	thread_body = body;
	return 0;
}

int
drv_typec_os_device_register(void)
{
	devices++;
	return 0;
}


/*
 * ws050-p005: the attach bound the display's Type-C ports to the USB-C
 * connectors their _PLD names: TC1 to connector 0 (CR01), TC2 to
 * connector 1 (CR02), TC3 and TC4 (whose SS03 and SS04 are not visible)
 * to none.  A connector's _PLD follows GNVS when it is read.
 */
static void
test_displays(void)
{
	struct drv_typec_location location;
	struct drv_typec_display display;
	struct drv_acpi_object *result;
	struct drv_acpi_object *element;
	const uint8_t *bytes;
	size_t length;
	int error;

	/* TC1 and TC2 bound, TC3 and TC4 not. */
	error = drv_typec_display_get(0U, &display);
	test_check("display-tc1", error == 0 && display.connector == 0U, "TC1 is bound to connector 0");
	error = drv_typec_display_get(1U, &display);
	test_check("display-tc2", error == 0 && display.connector == 1U, "TC2 is bound to connector 1");
	error = drv_typec_display_get(2U, &display);
	test_check("display-tc3", error == 0 && display.connector == DRV_TYPEC_CONNECTOR_NONE, "TC3 is not bound");
	error = drv_typec_display_get(3U, &display);
	test_check("display-tc4", error == 0 && display.connector == DRV_TYPEC_CONNECTOR_NONE, "TC4 is not bound");

	/* CR01's location read now follows GNVS: position 3. */
	memory_put(GNVS_BASE + GNVS_TP1P, 3, 1);
	result = NULL;
	error = drv_acpi_evaluate(NULL, "\\_SB.UBTC.CR01._PLD", NULL, 0U, &result);
	element = drv_acpi_object_package_element(result, 0U);
	bytes = NULL;
	length = 0U;
	if (element != NULL)
		bytes = drv_acpi_object_buffer(element, &length);
	error = drv_typec_location_decode(bytes, length, &location);
	test_check("display-pld", error == 0 && location.visible && location.group_token == 0U && location.group_position == 3U, "CR01's _PLD: visible, group 0, position 3");
	drv_acpi_object_release(result);
	memory_put(GNVS_BASE + GNVS_TP1P, 1, 1);
}
