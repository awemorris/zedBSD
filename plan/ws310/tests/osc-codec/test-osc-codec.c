/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host unit test of the _OSC negotiation's pure parts
 * (acpi-osc-codec.c): the UUIDs against their text laid out by the ToUUID
 * byte table, the Capabilities Buffers a query and a commit pass, the
 * reading of the buffers a firmware returns (granted, masked with and
 * without the masked bit, each refusal, a short buffer, an Integer, extra
 * bits, two and three DWORDs), and the commit that follows a query.
 *
 * The expected values are worked out by hand from ACPI 6.5 section 6.2.11
 * (the first DWORD), Table 6.13 (the platform-wide bits) and Table 19.41
 * (the ToUUID byte order), and from the Latitude 5330's \_SB.PC00._OSC
 * answer to a query (status 0x11, control 0x18 for 0x1D asked).
 *
 *   test-osc-codec          run the checks; the last line is
 *                           "test-osc-codec: PASS (N checks)" or
 *                           "test-osc-codec: FAIL (M of N checks)"
 *   test-osc-codec --uuids  print the three UUIDs as hex, one per line
 */

#include <stdio.h>
#include <string.h>

#include <drivers/acpi/acpi-osc-codec.h>
#include <uapi/errno.h>

/* The number of checks run and failed. */
static unsigned checks;
static unsigned failures;

static void check(int passed, const char *what);
static int uuid_from_text(const char *text, unsigned char *uuid);
static void print_uuid(const char *name, const unsigned char *uuid);
static void test_uuids(void);
static void test_encode(void);
static void test_decode(void);
static void test_commit(void);
static void test_names(void);

/*
 * Runs every check and prints the verdict, or prints the UUIDs.
 */
int
main(
	int argc,
	char **argv)
{
	/* Prints the UUIDs for the comparison with iasl's ToUUID. */
	if (argc == 2 && strcmp(argv[1], "--uuids") == 0) {
		print_uuid("platform", drv_acpi_osc_platform_uuid);
		print_uuid("processor", drv_acpi_osc_processor_uuid);
		print_uuid("pcie", drv_acpi_osc_pcie_uuid);
		return 0;
	}

	/* The five parts. */
	test_uuids();
	test_encode();
	test_decode();
	test_commit();
	test_names();

	/* The verdict. */
	if (failures != 0) {
		printf("test-osc-codec: FAIL (%u of %u checks)\n", failures, checks);
		return 1;
	}
	printf("test-osc-codec: PASS (%u checks)\n", checks);
	return 0;
}

/* Counts one check and reports it when it fails. */
static void
check(
	int passed,
	const char *what)
{
	checks++;
	if (passed)
		return;
	failures++;
	printf("  FAIL: %s\n", what);
}

/*
 * Lays a UUID's text out as ToUUID does: the pairs aa bb cc dd go to bytes
 * 3 2 1 0, ee ff to 5 4, gg hh to 7 6, and the last eight pairs in order
 * (ACPI 6.5 Table 19.41).
 */
static int
uuid_from_text(
	const char *text,
	unsigned char *uuid)
{
	static const unsigned places[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
	unsigned pair;
	unsigned value;
	int read;

	/* Reads the sixteen pairs, skipping the dashes. */
	for (pair = 0; pair < 16; pair++) {
		/* Skips a dash between groups. */
		if (*text == '-')
			text++;

		/* Reads one pair into its place. */
		read = sscanf(text, "%2x", &value);
		if (read != 1)
			return -1;
		uuid[places[pair]] = (unsigned char)value;
		text += 2;
	}

	/* Succeeded: uuid holds the ToUUID bytes. */
	return 0;
}

/* Prints a UUID's bytes as hex. */
static void
print_uuid(
	const char *name,
	const unsigned char *uuid)
{
	unsigned index;

	/* Prints the name and the sixteen bytes. */
	printf("%s", name);
	for (index = 0; index < DRV_ACPI_OSC_UUID_LENGTH; index++)
		printf(" %02X", uuid[index]);
	printf("\n");
}

/* The three UUIDs against their text. */
static void
test_uuids(void)
{
	unsigned char expected[16];
	int error;

	/* The platform-wide UUID (ACPI 6.5 section 6.2.11.2). */
	error = uuid_from_text("0811B06E-4A27-44F9-8D60-3CBBC22E7B48", expected);
	check(error == 0, "platform UUID text");
	check(memcmp(expected, drv_acpi_osc_platform_uuid, 16) == 0, "platform UUID bytes");
	check(drv_acpi_osc_platform_uuid[0] == 0x6E && drv_acpi_osc_platform_uuid[15] == 0x48, "platform UUID first and last byte");

	/* The processor UUID (ACPI 6.5 Appendix C, the _PDC example). */
	error = uuid_from_text("4077A616-290C-47BE-9EBD-D87058713953", expected);
	check(error == 0, "processor UUID text");
	check(memcmp(expected, drv_acpi_osc_processor_uuid, 16) == 0, "processor UUID bytes");

	/* The PCI Express host bridge UUID. */
	error = uuid_from_text("33DB4D5B-1FF7-401C-9657-7441C03DD766", expected);
	check(error == 0, "pcie UUID text");
	check(memcmp(expected, drv_acpi_osc_pcie_uuid, 16) == 0, "pcie UUID bytes");
}

/* The Capabilities Buffers a query and a commit pass. */
static void
test_encode(void)
{
	static const unsigned char platform_query[8] = { 0x01, 0, 0, 0, 0x60, 0, 0, 0 };
	static const unsigned char pcie_commit[12] = { 0, 0, 0, 0, 0x11, 0, 0, 0, 0x30, 0, 0, 0 };
	static const unsigned char wide[12] = { 0, 0, 0, 0, 0x04, 0x03, 0x02, 0x01, 0xF0, 0xDE, 0xBC, 0x9A };
	struct drv_acpi_osc_request request;
	unsigned char buffer[16];
	size_t length;

	/* A platform-wide query for CPPC and CPPC 2: 2 DWORDs, the query bit, 0x60. */
	request.uuid = drv_acpi_osc_platform_uuid;
	request.revision = DRV_ACPI_OSC_PLATFORM_REVISION;
	request.count = DRV_ACPI_OSC_PLATFORM_DWORDS;
	request.support = DRV_ACPI_OSC_PLATFORM_CPPC | DRV_ACPI_OSC_PLATFORM_CPPC2;
	request.control = 0x12345678U;
	memset(buffer, 0xAA, sizeof(buffer));
	length = drv_acpi_osc_encode(&request, true, buffer, sizeof(buffer));
	check(length == 8, "platform query length 8");
	check(memcmp(buffer, platform_query, 8) == 0, "platform query bytes");
	check(buffer[8] == 0xAA, "platform query writes no control field");

	/* A PCI Express commit: 3 DWORDs, status 0, support 0x11, control 0x30. */
	request.uuid = drv_acpi_osc_pcie_uuid;
	request.revision = DRV_ACPI_OSC_PCIE_REVISION;
	request.count = DRV_ACPI_OSC_PCIE_DWORDS;
	request.support = DRV_ACPI_OSC_PCIE_EXTENDED_CONFIG | DRV_ACPI_OSC_PCIE_MSI;
	request.control = DRV_ACPI_OSC_PCIE_CAPABILITY | DRV_ACPI_OSC_PCIE_LTR;
	memset(buffer, 0xAA, sizeof(buffer));
	length = drv_acpi_osc_encode(&request, false, buffer, sizeof(buffer));
	check(length == 12, "pcie commit length 12");
	check(memcmp(buffer, pcie_commit, 12) == 0, "pcie commit bytes");

	/* Every byte of a wide value lands lowest first. */
	request.support = 0x01020304U;
	request.control = 0x9ABCDEF0U;
	length = drv_acpi_osc_encode(&request, false, buffer, sizeof(buffer));
	check(length == 12 && memcmp(buffer, wide, 12) == 0, "little-endian DWORDs");

	/* A buffer one byte short is refused. */
	length = drv_acpi_osc_encode(&request, false, buffer, 11);
	check(length == 0, "short buffer refused");

	/* Counts other than 2 and 3 are refused. */
	request.count = 1;
	length = drv_acpi_osc_encode(&request, true, buffer, sizeof(buffer));
	check(length == 0, "count 1 refused");
	request.count = 4;
	length = drv_acpi_osc_encode(&request, true, buffer, sizeof(buffer));
	check(length == 0, "count 4 refused");

	/* A missing request or buffer is refused. */
	length = drv_acpi_osc_encode(NULL, true, buffer, sizeof(buffer));
	check(length == 0, "no request refused");
	request.count = 2;
	length = drv_acpi_osc_encode(&request, true, NULL, sizeof(buffer));
	check(length == 0, "no buffer refused");
}

/* The reading of the buffers a firmware returns. */
static void
test_decode(void)
{
	static const unsigned char granted[8] = { 0x00, 0, 0, 0, 0x60, 0, 0, 0 };
	static const unsigned char masked_bit[8] = { 0x10, 0, 0, 0, 0x20, 0, 0, 0 };
	static const unsigned char masked_silent[8] = { 0x00, 0, 0, 0, 0x20, 0, 0, 0 };
	static const unsigned char failure[8] = { 0x02, 0, 0, 0, 0x60, 0, 0, 0 };
	static const unsigned char unknown_uuid[8] = { 0x04, 0, 0, 0, 0x60, 0, 0, 0 };
	static const unsigned char unknown_revision[8] = { 0x08, 0, 0, 0, 0x60, 0, 0, 0 };
	static const unsigned char extra[12] = { 0x00, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0 };
	static const unsigned char echoed_query[8] = { 0x01, 0, 0, 0, 0x60, 0, 0, 0 };
	static const unsigned char longer[12] = { 0x00, 0, 0, 0, 0x60, 0, 0, 0, 0x55, 0x55, 0x55, 0x55 };
	static const unsigned char pcie_5330[12] = { 0x11, 0, 0, 0, 0x1F, 0, 0, 0, 0x18, 0, 0, 0 };
	struct drv_acpi_osc_request platform;
	struct drv_acpi_osc_request pcie;
	struct drv_acpi_osc_result result;
	int error;

	/* The platform-wide request for CPPC and CPPC 2. */
	platform.uuid = drv_acpi_osc_platform_uuid;
	platform.revision = DRV_ACPI_OSC_PLATFORM_REVISION;
	platform.count = DRV_ACPI_OSC_PLATFORM_DWORDS;
	platform.support = 0x60U;
	platform.control = 0;

	/* Both bits acknowledged: GRANTED, 0x60. */
	error = drv_acpi_osc_decode(&platform, granted, 8, &result);
	check(error == 0 && result.outcome == DRV_ACPI_OSC_GRANTED && result.support == 0x60, "granted");
	check(result.status == 0 && result.control == 0, "granted status and control");

	/* The masked bit with CPPC 2 dropped: MASKED, 0x20. */
	error = drv_acpi_osc_decode(&platform, masked_bit, 8, &result);
	check(error == 0 && result.outcome == DRV_ACPI_OSC_MASKED && result.support == 0x20, "masked with the bit");
	check(result.status == 0x10, "masked status kept");

	/* CPPC 2 dropped without the masked bit: still MASKED. */
	error = drv_acpi_osc_decode(&platform, masked_silent, 8, &result);
	check(error == 0 && result.outcome == DRV_ACPI_OSC_MASKED && result.support == 0x20, "masked without the bit");

	/* Each refusal grants nothing, even with the support bits left set. */
	error = drv_acpi_osc_decode(&platform, failure, 8, &result);
	check(error == 0 && result.outcome == DRV_ACPI_OSC_REFUSED && result.support == 0, "_OSC failure refused");
	check(result.status == 0x02, "failure status kept");
	error = drv_acpi_osc_decode(&platform, unknown_uuid, 8, &result);
	check(error == 0 && result.outcome == DRV_ACPI_OSC_REFUSED && result.support == 0, "unknown UUID refused");
	error = drv_acpi_osc_decode(&platform, unknown_revision, 8, &result);
	check(error == 0 && result.outcome == DRV_ACPI_OSC_REFUSED && result.support == 0, "unknown revision refused");

	/* A buffer one byte short, an empty one and an Integer are BROKEN. */
	error = drv_acpi_osc_decode(&platform, granted, 7, &result);
	check(error == 0 && result.outcome == DRV_ACPI_OSC_BROKEN && result.support == 0, "short buffer broken");
	error = drv_acpi_osc_decode(&platform, granted, 0, &result);
	check(error == 0 && result.outcome == DRV_ACPI_OSC_BROKEN, "empty buffer broken");
	error = drv_acpi_osc_decode(&platform, NULL, 0, &result);
	check(error == 0 && result.outcome == DRV_ACPI_OSC_BROKEN && result.status == 0, "integer broken");

	/* The query bit echoed back (bit 0 of the returned DWORD is reserved) does not matter. */
	error = drv_acpi_osc_decode(&platform, echoed_query, 8, &result);
	check(error == 0 && result.outcome == DRV_ACPI_OSC_GRANTED, "echoed query bit ignored");

	/* A longer buffer is read for its first DWORDs. */
	error = drv_acpi_osc_decode(&platform, longer, 12, &result);
	check(error == 0 && result.outcome == DRV_ACPI_OSC_GRANTED && result.control == 0, "longer buffer");

	/* The PCI Express request of the 5330's query: support 0x1F, control 0x1D. */
	pcie.uuid = drv_acpi_osc_pcie_uuid;
	pcie.revision = DRV_ACPI_OSC_PCIE_REVISION;
	pcie.count = DRV_ACPI_OSC_PCIE_DWORDS;
	pcie.support = 0x1FU;
	pcie.control = 0x1DU;

	/* The 5330's answer (status 0x11, control 0x18): MASKED, support 0x1F, control 0x18. */
	error = drv_acpi_osc_decode(&pcie, pcie_5330, 12, &result);
	check(error == 0 && result.outcome == DRV_ACPI_OSC_MASKED, "5330 pcie masked");
	check(result.support == 0x1F && result.control == 0x18, "5330 pcie bits");

	/* Bits not asked for are not granted: support 0x11, control 0x10 of all ones. */
	pcie.support = 0x11U;
	pcie.control = 0x10U;
	error = drv_acpi_osc_decode(&pcie, extra, 12, &result);
	check(error == 0 && result.outcome == DRV_ACPI_OSC_GRANTED, "extra bits granted");
	check(result.support == 0x11 && result.control == 0x10, "extra bits cut");

	/* Three DWORDs asked and eight bytes back: BROKEN. */
	error = drv_acpi_osc_decode(&pcie, granted, 8, &result);
	check(error == 0 && result.outcome == DRV_ACPI_OSC_BROKEN, "three DWORDs, two returned");

	/* A request that cannot be read is refused. */
	pcie.count = 5;
	error = drv_acpi_osc_decode(&pcie, extra, 12, &result);
	check(error == EINVAL, "count 5 EINVAL");
	error = drv_acpi_osc_decode(&platform, granted, 8, NULL);
	check(error == EINVAL, "no result EINVAL");
}

/* The commit that follows a query. */
static void
test_commit(void)
{
	struct drv_acpi_osc_request asked;
	struct drv_acpi_osc_request commit;
	struct drv_acpi_osc_result query;
	bool follow;

	/* The PCI Express request: support 0x1F, control 0x1D. */
	asked.uuid = drv_acpi_osc_pcie_uuid;
	asked.revision = DRV_ACPI_OSC_PCIE_REVISION;
	asked.count = DRV_ACPI_OSC_PCIE_DWORDS;
	asked.support = 0x1FU;
	asked.control = 0x1DU;

	/* A MASKED query (control 0x18): the commit asks for 0x1F and 0x18. */
	query.outcome = DRV_ACPI_OSC_MASKED;
	query.status = 0x10;
	query.support = 0x1F;
	query.control = 0x18;
	follow = drv_acpi_osc_commit_request(&asked, &query, &commit);
	check(follow, "masked query commits");
	check(commit.support == 0x1F && commit.control == 0x18, "commit asks for the query's bits");
	check(commit.uuid == drv_acpi_osc_pcie_uuid && commit.revision == 1 && commit.count == 3, "commit names the interface");

	/* A GRANTED query that left nothing still commits, asking for nothing. */
	query.outcome = DRV_ACPI_OSC_GRANTED;
	query.support = 0;
	query.control = 0;
	asked.support = 0;
	asked.control = 0;
	follow = drv_acpi_osc_commit_request(&asked, &query, &commit);
	check(follow && commit.support == 0 && commit.control == 0, "empty commit still made");

	/* A REFUSED or BROKEN query is not followed by a commit. */
	asked.support = 0x1FU;
	query.outcome = DRV_ACPI_OSC_REFUSED;
	query.support = 0x1F;
	follow = drv_acpi_osc_commit_request(&asked, &query, &commit);
	check(!follow && commit.support == 0, "refused query does not commit");
	query.outcome = DRV_ACPI_OSC_BROKEN;
	follow = drv_acpi_osc_commit_request(&asked, &query, &commit);
	check(!follow, "broken query does not commit");
	query.outcome = DRV_ACPI_OSC_ABSENT;
	follow = drv_acpi_osc_commit_request(&asked, &query, &commit);
	check(!follow, "absent _OSC does not commit");
}

/* The outcome names for the log. */
static void
test_names(void)
{
	/* Each outcome and a value outside them. */
	check(strcmp(drv_acpi_osc_outcome_name(DRV_ACPI_OSC_ABSENT), "absent") == 0, "name absent");
	check(strcmp(drv_acpi_osc_outcome_name(DRV_ACPI_OSC_GRANTED), "granted") == 0, "name granted");
	check(strcmp(drv_acpi_osc_outcome_name(DRV_ACPI_OSC_MASKED), "masked") == 0, "name masked");
	check(strcmp(drv_acpi_osc_outcome_name(DRV_ACPI_OSC_REFUSED), "refused") == 0, "name refused");
	check(strcmp(drv_acpi_osc_outcome_name(DRV_ACPI_OSC_BROKEN), "broken") == 0, "name broken");
	check(strcmp(drv_acpi_osc_outcome_name((enum drv_acpi_osc_outcome)99), "unknown") == 0, "name unknown");
}
