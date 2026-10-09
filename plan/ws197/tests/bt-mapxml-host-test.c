/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the Message Access Profile's XML and datetimes
 * (ws197-p003, plan/ws197/phase003/phase.md section 6), built with the
 * host's compiler under ASan and UBSan.  The documents are written by
 * hand in the form of MAP 1.4.2's examples (sections 3.1.6.1 and
 * 3.1.7.1: spaces around '='), and the seconds of each datetime were
 * worked out by hand from the calendar.
 *
 *   listing    the example listing: handles, datetimes, names, numbers,
 *              types, read, reception; a handle missing, too long or not
 *              hex skips its message; capacity drops the rest; an empty
 *              root; NUL bytes after the root
 *   syntax     a byte order mark, the XML declaration, comments, a
 *              DOCTYPE (refused with a subset), references and character
 *              references made UTF-8, single quotes, prefixed names,
 *              repeated attributes, CDATA, processing instructions, text
 *              and elements inside a msg, every cut of a document
 *   limits     the size of a document, the attributes of an element, the
 *              length of a value, the messages of a listing; texts cut
 *              without splitting a character
 *   event      the example event; a shift's old folder; an event without
 *              a handle; none, two, no type, a bad handle
 *   datetime   offsets, a Z, NUL bytes, the leap day and a leap second,
 *              the ranges, MSETime's offset and zedBSD's, and the
 *              filter's form written back
 *   fuzz       random changes to the documents into both readers
 *
 *   plan/ws197/tests/bt-phone-host-test.sh
 */

#include "userland/base/bluetoothd/mapxml.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The fuzz's rounds. */
#define TEST_FUZZ_ROUNDS	200000U

/* The room of the test's documents. */
#define TEST_DOCUMENT_MAX	(BTD_MAPXML_INPUT_MAX + 4096U)

/* The checks that failed, and those that ran. */
static unsigned failures;
static unsigned checks;

/* The entries a listing fills. */
static struct btd_map_entry entries[BTD_MAPXML_ENTRIES_MAX + 1U];

/* A document the test builds. */
static char document[TEST_DOCUMENT_MAX];

/*
 * The listing in the form of MAP section 3.1.6.1's example: three SMS and
 * an e-mail, spaces around '=', attributes the reader does not keep.
 */
static const char listing_example[] =
	"<MAP-msg-listing version = \"1.0\">\n"
	"\t<msg handle = \"20000100001\" subject = \"Hello\" datetime = \"20071213T130510Z\" "
	"sender_name = \"Jamie\" sender_addressing = \"+1-987-6543210\" "
	"recipient_addressing = \"+1-0123-456789\" type = \"SMS_GSM\" size = \"256\" "
	"attachment_size = \"0\" priority = \"no\" read = \"yes\" sent = \"no\" protected = \"no\"/>\n"
	"\t<msg handle = \"20000100002\" subject = \"Guten Tag\" datetime = \"20071214T092200+0100\" "
	"sender_name = \"Dmitri\" sender_addressing = \"8765432109\" "
	"recipient_addressing = \"+49-9012-345678\" type = \"SMS_GSM\" size = \"512\" "
	"attachment_size = \"3000\" priority = \"no\" read = \"no\" sent = \"yes\" protected = \"no\" "
	"reception_status = \"complete\"/>\n"
	"\t<msg handle = \"20000100003\" subject = \"Ohayougozaimasu\" datetime = \"20071215T171204\" "
	"sender_name = \"Andy\" sender_addressing = \"+49-7654-321098\" "
	"recipient_addressing = \"+49-89-01234567\" type = \"SMS_CDMA\" size = \"256\" "
	"attachment_size = \"0\" priority = \"no\" sent = \"no\" protected = \"no\" "
	"reception_status = \"notification\"/>\n"
	"\t<msg handle = \"20000100000\" subject = \"Meeting\" datetime = \"20071217T091500\" "
	"sender_name = \"Mary\" sender_addressing = \"mary@example.com\" "
	"recipient_addressing = \"zed@example.com\" type = \"EMAIL\" size = \"1032\" "
	"attachment_size = \"0\" priority = \"yes\" read = \"yes\" sent = \"no\" protected = \"no\"/>\n"
	"</MAP-msg-listing>\n";

/* The event report in the form of MAP section 3.1.7.1's example. */
static const char event_example[] =
	"<MAP-event-report version = \"1.0\">\n"
	"\t<event type = \"NewMessage\" handle = \"12345678\" folder = \"TELECOM/MSG/INBOX\" msg_type = \"SMS_CDMA\" />\n"
	"</MAP-event-report>\n";

static void check(int condition, const char *what);
static int listing(const char *text, size_t capacity, struct btd_mapxml_counts *counts);
static int listing_length(const char *text, size_t length, size_t capacity, struct btd_mapxml_counts *counts);
static int event(const char *text, struct btd_map_event *report);
static int one_message(const char *attributes, struct btd_map_entry *entry);
static void test_listing(void);
static void test_handles(void);
static void test_syntax(void);
static void test_references(void);
static void test_cuts(void);
static void test_limits(void);
static void test_texts(void);
static void test_event(void);
static void test_datetime(void);
static void test_format(void);
static void test_fuzz(void);

/*
 * Runs every part and reports the checks.
 */
int
main(void)
{
	/* Each part. */
	test_listing();
	test_handles();
	test_syntax();
	test_references();
	test_cuts();
	test_limits();
	test_texts();
	test_event();
	test_datetime();
	test_format();
	test_fuzz();

	/* The count of what failed. */
	printf("bt-mapxml-host-test: %u checks, %u failed\n", checks, failures);
	if (failures != 0U)
		return 1;

	/* Succeeded: every check held. */
	return 0;
}

/* Counts a check, and reports it when it failed. */
static void
check(
	int condition,
	const char *what)
{
	/* One more check. */
	checks++;

	/* A failure is reported. */
	if (!condition) {
		failures++;
		printf("FAIL: %s\n", what);
	}
}

/* Reads a listing given as a string into the test's entries. */
static int
listing(
	const char *text,
	size_t capacity,
	struct btd_mapxml_counts *counts)
{
	int error;

	/* The listing without its NUL. */
	error = listing_length(text, strlen(text), capacity, counts);

	/* What the reader said. */
	return error;
}


/* Reads a listing of length bytes into the test's entries. */
static int
listing_length(
	const char *text,
	size_t length,
	size_t capacity,
	struct btd_mapxml_counts *counts)
{
	int error;

	/* The listing as bytes. */
	error = btd_mapxml_listing((const uint8_t *)text, length, entries, capacity, counts);

	/* What the reader said. */
	return error;
}

/* Reads an event report given as a string. */
static int
event(
	const char *text,
	struct btd_map_event *report)
{
	int error;

	/* The report without its NUL. */
	error = btd_mapxml_event((const uint8_t *)text, strlen(text), report);

	/* What the reader said. */
	return error;
}

/*
 * Reads a listing of one msg with the attributes given.  Returns what the
 * reader said; the entry is copied out when one was kept, else zeroed.
 */
static int
one_message(
	const char *attributes,
	struct btd_map_entry *entry)
{
	struct btd_mapxml_counts counts;
	int error;

	/* The listing around the message. */
	snprintf(document, sizeof(document), "<MAP-msg-listing version=\"1.0\"><msg %s/></MAP-msg-listing>", attributes);
	error = listing(document, 4U, &counts);

	/* The entry kept, or nothing. */
	memset(entry, 0, sizeof(*entry));
	if (error == 0 && counts.entries == 1U)
		*entry = entries[0];

	/* What the reader said. */
	return error;
}

/* The example listing, field by field. */
static void
test_listing(void)
{
	struct btd_mapxml_counts counts;
	int error;

	/* All four messages. */
	error = listing(listing_example, 8U, &counts);
	check(error == 0, "listing: example read");
	check(counts.entries == 4U && counts.skipped == 0U && counts.dropped == 0U, "listing: four entries");

	/* The first: an SMS read, its datetime in UTC. */
	check(entries[0].handle == 0x20000100001ULL, "listing: first handle");
	check(strcmp(entries[0].datetime, "20071213T130510Z") == 0, "listing: first datetime");
	check(strcmp(entries[0].sender_name, "Jamie") == 0, "listing: first sender name");
	check(strcmp(entries[0].sender_addressing, "+1-987-6543210") == 0, "listing: first sender number");
	check(strcmp(entries[0].recipient_addressing, "+1-0123-456789") == 0, "listing: first recipient number");
	check(entries[0].recipient_name[0] == '\0', "listing: first recipient name not given");
	check(entries[0].type == BTD_MAP_TYPE_SMS_GSM, "listing: first type");
	check(entries[0].read == BTD_MAP_READ_YES, "listing: first read");
	check(entries[0].reception == BTD_MAP_RECEPTION_UNKNOWN, "listing: first reception not given");

	/* The second: unread, complete. */
	check(entries[1].handle == 0x20000100002ULL, "listing: second handle");
	check(strcmp(entries[1].datetime, "20071214T092200+0100") == 0, "listing: second datetime");
	check(entries[1].read == BTD_MAP_READ_NO, "listing: second unread");
	check(entries[1].reception == BTD_MAP_RECEPTION_COMPLETE, "listing: second complete");

	/* The third: CDMA, read not said, only a notification. */
	check(entries[2].type == BTD_MAP_TYPE_SMS_CDMA, "listing: third type");
	check(entries[2].read == BTD_MAP_READ_UNKNOWN, "listing: third read not given");
	check(entries[2].reception == BTD_MAP_RECEPTION_NOTIFICATION, "listing: third notification");

	/* The fourth: an e-mail, its handle with a leading 2 of 11 digits. */
	check(entries[3].type == BTD_MAP_TYPE_EMAIL, "listing: fourth type");
	check(entries[3].handle == 0x20000100000ULL, "listing: fourth handle");
	check(strcmp(entries[3].sender_addressing, "mary@example.com") == 0, "listing: fourth address");

	/* Room for two: the other two dropped and counted. */
	error = listing(listing_example, 2U, &counts);
	check(error == 0 && counts.entries == 2U && counts.dropped == 2U, "listing: capacity drops the rest");
	check(entries[1].handle == 0x20000100002ULL, "listing: capacity keeps the first ones");

	/* An empty root, closed at once or with an end tag. */
	error = listing("<MAP-msg-listing version=\"1.0\"/>", 4U, &counts);
	check(error == 0 && counts.entries == 0U, "listing: empty root closed at once");
	error = listing("<MAP-msg-listing version='1.0'>\r\n</MAP-msg-listing >", 4U, &counts);
	check(error == 0 && counts.entries == 0U, "listing: empty root with an end tag");

	/* A listing of another version is read on. */
	error = listing("<MAP-msg-listing version=\"1.1\"><msg handle=\"1\" type=\"SMS_GSM\"/></MAP-msg-listing>", 4U, &counts);
	check(error == 0 && counts.entries == 1U, "listing: version 1.1 read");

	/* NUL bytes after the root (a body some phones end so). */
	memset(document, 0, sizeof(document));
	strcpy(document, "<MAP-msg-listing><msg handle=\"a\"/></MAP-msg-listing>");
	error = listing_length(document, strlen(document) + 3U, 4U, &counts);
	check(error == 0 && counts.entries == 1U && entries[0].handle == 0xaU, "listing: NUL bytes after the root");

	/* Not a listing: another root. */
	error = listing(event_example, 4U, &counts);
	check(error == EINVAL, "listing: an event report is not a listing");
}

/* The handles of a listing's messages. */
static void
test_handles(void)
{
	struct btd_mapxml_counts counts;
	struct btd_map_entry entry;
	int error;

	/* Sixteen digits, in either case. */
	error = one_message("handle=\"FfFfFfFfFfFfFfFf\"", &entry);
	check(error == 0 && entry.handle == 0xffffffffffffffffULL, "handles: sixteen digits");

	/* Each malformed handle skips its message alone. */
	error = listing("<MAP-msg-listing>"
			"<msg type=\"SMS_GSM\"/>"
			"<msg handle=\"\"/>"
			"<msg handle=\"12345678901234567\"/>"
			"<msg handle=\"12g4\"/>"
			"<msg handle=\" 12\"/>"
			"<msg handle=\"0042\" type=\"SMS_GSM\"/>"
			"</MAP-msg-listing>",
			8U,
			&counts);
	check(error == 0, "handles: the listing read on");
	check(counts.entries == 1U && counts.skipped == 5U, "handles: five skipped");
	check(entries[0].handle == 0x42U, "handles: the good one kept");

	/* A child other than msg is ignored, even with a handle. */
	error = listing("<MAP-msg-listing><note handle=\"1\"/><msg handle=\"2\"/></MAP-msg-listing>", 8U, &counts);
	check(error == 0 && counts.entries == 1U && counts.skipped == 0U && entries[0].handle == 2U, "handles: other children ignored");
}

/* The parts of XML the reader takes and refuses. */
static void
test_syntax(void)
{
	struct btd_mapxml_counts counts;
	size_t used;
	char *nul;
	int error;

	/* A byte order mark, the declaration, comments everywhere and a DOCTYPE. */
	error = listing("\xef\xbb\xbf<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
			"<!-- before -->\n"
			"<!DOCTYPE MAP-msg-listing SYSTEM \"map.dtd\">\n"
			"<MAP-msg-listing version=\"1.0\"><!-- between -->\n"
			"<msg handle=\"1\"><!-- inside --> </msg>\n"
			"</MAP-msg-listing>\n<!-- after -->\n",
			8U,
			&counts);
	check(error == 0 && counts.entries == 1U, "syntax: prolog, comments, DOCTYPE");

	/* A DOCTYPE with an internal subset (it could declare entities). */
	error = listing("<!DOCTYPE MAP-msg-listing [<!ENTITY a \"b\">]><MAP-msg-listing/>", 8U, &counts);
	check(error == EINVAL, "syntax: internal subset refused");

	/* Attributes with a prefix are ignored; a prefixed element is refused. */
	error = listing("<MAP-msg-listing xmlns:x=\"urn:x\"><msg x:handle=\"9\" handle=\"3\"/></MAP-msg-listing>", 8U, &counts);
	check(error == 0 && counts.entries == 1U && entries[0].handle == 3U, "syntax: prefixed attribute ignored");
	error = listing("<MAP-msg-listing><x:msg handle=\"3\"/></MAP-msg-listing>", 8U, &counts);
	check(error == EINVAL, "syntax: prefixed child refused");
	error = listing("<x:MAP-msg-listing><msg handle=\"3\"/></x:MAP-msg-listing>", 8U, &counts);
	check(error == EINVAL, "syntax: prefixed root refused");

	/* A repeated attribute. */
	error = listing("<MAP-msg-listing><msg handle=\"1\" handle=\"2\"/></MAP-msg-listing>", 8U, &counts);
	check(error == EINVAL, "syntax: repeated attribute refused");

	/* Attributes not parted by a space. */
	error = listing("<MAP-msg-listing><msg handle=\"1\"type=\"SMS_GSM\"/></MAP-msg-listing>", 8U, &counts);
	check(error == EINVAL, "syntax: attributes without a space refused");

	/* CDATA, a processing instruction, text and an element inside the root or a msg. */
	error = listing("<MAP-msg-listing><![CDATA[x]]></MAP-msg-listing>", 8U, &counts);
	check(error == EINVAL, "syntax: CDATA refused");
	error = listing("<MAP-msg-listing><?pi x?></MAP-msg-listing>", 8U, &counts);
	check(error == EINVAL, "syntax: processing instruction refused");
	error = listing("<?xml-stylesheet href=\"a\"?><MAP-msg-listing/>", 8U, &counts);
	check(error == EINVAL, "syntax: processing instruction before the root refused");
	error = listing("<MAP-msg-listing>text</MAP-msg-listing>", 8U, &counts);
	check(error == EINVAL, "syntax: text in the root refused");
	error = listing("<MAP-msg-listing><msg handle=\"1\">text</msg></MAP-msg-listing>", 8U, &counts);
	check(error == EINVAL, "syntax: text in a msg refused");
	error = listing("<MAP-msg-listing><msg handle=\"1\"><b/></msg></MAP-msg-listing>", 8U, &counts);
	check(error == EINVAL, "syntax: element in a msg refused");

	/* End tags that do not match, or are missing. */
	error = listing("<MAP-msg-listing><msg handle=\"1\"></mgs></MAP-msg-listing>", 8U, &counts);
	check(error == EINVAL, "syntax: wrong end tag refused");
	error = listing("<MAP-msg-listing><msg handle=\"1\"/>", 8U, &counts);
	check(error == EINVAL, "syntax: missing end tag refused");
	error = listing("<MAP-msg-listing/><MAP-msg-listing/>", 8U, &counts);
	check(error == EINVAL, "syntax: a second root refused");

	/* A '<' or a NUL inside a value, an unquoted value. */
	error = listing("<MAP-msg-listing><msg handle=\"1\" sender_name=\"a<b\"/></MAP-msg-listing>", 8U, &counts);
	check(error == EINVAL, "syntax: '<' in a value refused");
	memset(document, 0, sizeof(document));
	strcpy(document, "<MAP-msg-listing><msg handle=\"1\" sender_name=\"aXb\"/></MAP-msg-listing>");
	used = strlen(document);
	nul = strchr(document, 'X');
	*nul = '\0';
	error = listing_length(document, used, 8U, &counts);
	check(error == EINVAL, "syntax: NUL in a value refused");
	error = listing("<MAP-msg-listing><msg handle=1/></MAP-msg-listing>", 8U, &counts);
	check(error == EINVAL, "syntax: unquoted value refused");

	/* An empty document, spaces alone, a comment that does not end. */
	error = listing("", 8U, &counts);
	check(error == EINVAL, "syntax: empty refused");
	error = listing(" \n", 8U, &counts);
	check(error == EINVAL, "syntax: spaces refused");
	error = listing("<!-- <MAP-msg-listing/>", 8U, &counts);
	check(error == EINVAL, "syntax: unended comment refused");
}

/* The references of values, and single quotes. */
static void
test_references(void)
{
	struct btd_map_entry entry;
	int error;

	/* The five entities, decimal and hex references made UTF-8. */
	error = one_message("handle=\"1\" sender_name=\"A&amp;B &lt;x&gt; &quot;q&quot; &apos;s&apos; &#233;&#x3042;&#x1F600;\"", &entry);
	check(error == 0, "references: read");
	check(strcmp(entry.sender_name, "A&B <x> \"q\" 's' \xc3\xa9\xe3\x81\x82\xf0\x9f\x98\x80") == 0, "references: undone");

	/* Single quotes hold double quotes. */
	error = one_message("handle='2' sender_name='say \"hi\"'", &entry);
	check(error == 0 && entry.handle == 2U && strcmp(entry.sender_name, "say \"hi\"") == 0, "references: single quotes");

	/* A tab and a line feed inside a value are spaces. */
	error = one_message("handle=\"3\" sender_name=\"a\tb\nc\"", &entry);
	check(error == 0 && strcmp(entry.sender_name, "a b c") == 0, "references: spaces normalized");

	/* References that are refused. */
	error = one_message("handle=\"1\" sender_name=\"&#0;\"", &entry);
	check(error == EINVAL, "references: NUL refused");
	error = one_message("handle=\"1\" sender_name=\"&#xD800;\"", &entry);
	check(error == EINVAL, "references: surrogate refused");
	error = one_message("handle=\"1\" sender_name=\"&#x110000;\"", &entry);
	check(error == EINVAL, "references: past U+10FFFF refused");
	error = one_message("handle=\"1\" sender_name=\"&#99999999999999999999;\"", &entry);
	check(error == EINVAL, "references: huge number refused");
	error = one_message("handle=\"1\" sender_name=\"&nbsp;\"", &entry);
	check(error == EINVAL, "references: unknown entity refused");
	error = one_message("handle=\"1\" sender_name=\"a & b\"", &entry);
	check(error == EINVAL, "references: bare ampersand refused");
	error = one_message("handle=\"1\" sender_name=\"&#;\"", &entry);
	check(error == EINVAL, "references: no digits refused");
	error = one_message("handle=\"1\" sender_name=\"&#x41g;\"", &entry);
	check(error == EINVAL, "references: bad hex digit refused");
	error = one_message("handle=\"1\" sender_name=\"&#4A;\"", &entry);
	check(error == EINVAL, "references: hex digit in decimal refused");

	/* A reference in the handle. */
	error = one_message("handle=\"&#x31;f\"", &entry);
	check(error == 0 && entry.handle == 0x1fU, "references: in the handle");
}

/* Every cut of the example: nothing crashes, and no cut short of the root's end reads. */
static void
test_cuts(void)
{
	struct btd_mapxml_counts counts;
	struct btd_map_event report;
	size_t length;
	size_t end;
	int wrong;
	int error;

	/* The listing cut at each length before its root ends. */
	end = strlen(listing_example) - 1U;
	wrong = 0;
	for (length = 0U; length < end; length++) {
		error = listing_length(listing_example, length, 8U, &counts);
		if (error == 0)
			wrong = 1;
	}

	/* None of them read. */
	check(!wrong, "cuts: every cut listing refused");

	/* The whole listing still reads, with or without its last line feed. */
	error = listing_length(listing_example, end, 8U, &counts);
	check(error == 0 && counts.entries == 4U, "cuts: whole listing without its line feed");

	/* The event cut at each length before its root ends. */
	end = strlen(event_example) - 1U;
	wrong = 0;
	for (length = 0U; length < end; length++) {
		error = btd_mapxml_event((const uint8_t *)event_example, length, &report);
		if (error == 0)
			wrong = 1;
	}

	/* None of them read. */
	check(!wrong, "cuts: every cut event refused");
}

/* The limits of a document. */
static void
test_limits(void)
{
	struct btd_mapxml_counts counts;
	struct btd_map_entry entry;
	size_t used;
	size_t index;
	int error;

	/* A value of 1024 bytes once undone is taken; 1025 is not. */
	used = (size_t)snprintf(document, sizeof(document), "<MAP-msg-listing><msg handle=\"1\" subject=\"");
	memset(document + used, 'a', 1024U);
	used += 1024U;
	snprintf(document + used, sizeof(document) - used, "\"/></MAP-msg-listing>");
	error = listing(document, 4U, &counts);
	check(error == 0 && counts.entries == 1U, "limits: value of 1024 taken");
	snprintf(document + used, sizeof(document) - used, "&amp;\"/></MAP-msg-listing>");
	error = listing(document, 4U, &counts);
	check(error == E2BIG, "limits: value of 1025 refused");

	/* 32 attributes taken, 33 refused. */
	used = (size_t)snprintf(document, sizeof(document), "<MAP-msg-listing><msg handle=\"1\"");
	for (index = 1U; index < 32U; index++)
		used += (size_t)snprintf(document + used, sizeof(document) - used, " a%u=\"x\"", (unsigned)index);
	snprintf(document + used, sizeof(document) - used, "/></MAP-msg-listing>");
	error = listing(document, 4U, &counts);
	check(error == 0 && counts.entries == 1U, "limits: 32 attributes taken");
	snprintf(document + used, sizeof(document) - used, " b=\"y\"/></MAP-msg-listing>");
	error = listing(document, 4U, &counts);
	check(error == E2BIG, "limits: 33 attributes refused");

	/* 1024 messages taken (the room holds all), 1025 refused. */
	used = (size_t)snprintf(document, sizeof(document), "<MAP-msg-listing>");
	for (index = 0U; index < 1024U; index++)
		used += (size_t)snprintf(document + used, sizeof(document) - used, "<msg handle=\"%x\"/>", (unsigned)index);
	snprintf(document + used, sizeof(document) - used, "</MAP-msg-listing>");
	error = listing(document, BTD_MAPXML_ENTRIES_MAX + 1U, &counts);
	check(error == 0 && counts.entries == 1024U, "limits: 1024 messages taken");
	check(entries[1023].handle == 1023U, "limits: the last message kept");
	snprintf(document + used, sizeof(document) - used, "<msg handle=\"1\"/></MAP-msg-listing>");
	error = listing(document, BTD_MAPXML_ENTRIES_MAX + 1U, &counts);
	check(error == E2BIG && counts.entries == 0U, "limits: 1025 messages refused");

	/* A document of 64 KB is read; one byte more is refused. */
	used = (size_t)snprintf(document, sizeof(document), "<MAP-msg-listing>");
	memset(document + used, ' ', BTD_MAPXML_INPUT_MAX);
	snprintf(document + BTD_MAPXML_INPUT_MAX - 18U, 32U, "</MAP-msg-listing>");
	error = listing_length(document, BTD_MAPXML_INPUT_MAX, 4U, &counts);
	check(error == 0, "limits: 64 KB read");
	error = listing_length(document, BTD_MAPXML_INPUT_MAX + 1U, 4U, &counts);
	check(error == E2BIG, "limits: past 64 KB refused");

	/* A datetime too long to be one is kept empty. */
	error = one_message("handle=\"1\" datetime=\"20071213T130510+0900xxxx\"", &entry);
	check(error == 0 && entry.datetime[0] == '\0', "limits: long datetime left empty");
	error = one_message("handle=\"1\" datetime=\"20071213T130510+0900\"", &entry);
	check(error == 0 && strcmp(entry.datetime, "20071213T130510+0900") == 0, "limits: datetime kept");

	/* A name longer than 65 characters is a name, not a too long one. */
	used = (size_t)snprintf(document, sizeof(document), "<MAP-msg-listing><msg handle=\"1\" ");
	memset(document + used, 'n', 65U);
	snprintf(document + used + 65U, sizeof(document) - used - 65U, "=\"x\"/></MAP-msg-listing>");
	error = listing(document, 4U, &counts);
	check(error == EINVAL, "limits: attribute name past 64 bytes refused");
}

/* Texts cut so a character is not split. */
static void
test_texts(void)
{
	struct btd_map_entry entry;
	char attributes[1200];
	size_t used;
	size_t index;
	size_t kept;
	int error;

	/* 300 two-byte characters: 254 bytes kept (127 characters), not 255. */
	used = (size_t)snprintf(attributes, sizeof(attributes), "handle=\"1\" sender_name=\"");
	for (index = 0U; index < 300U; index++) {
		attributes[used] = '\xc3';
		attributes[used + 1U] = '\xa9';
		used += 2U;
	}

	/* The closing quote. */
	snprintf(attributes + used, sizeof(attributes) - used, "\"");
	error = one_message(attributes, &entry);
	kept = strlen(entry.sender_name);
	check(error == 0 && kept == 254U, "texts: cut before a character");

	/* The cut itself. */
	kept = btd_mapxml_utf8_cut("ab\xe3\x81\x82", 5U, 4U);
	check(kept == 2U, "texts: three-byte character not split");
	kept = btd_mapxml_utf8_cut("ab\xe3\x81\x82", 5U, 5U);
	check(kept == 5U, "texts: all fits");
	kept = btd_mapxml_utf8_cut("a\x80\x80\x80\x80\x80", 6U, 5U);
	check(kept == 5U, "texts: not UTF-8 cut at the room");
	kept = btd_mapxml_utf8_cut("\xf0\x9f\x98\x80", 4U, 3U);
	check(kept == 0U, "texts: a four-byte character dropped whole");
}

/* The event report. */
static void
test_event(void)
{
	struct btd_map_event report;
	int error;

	/* The example. */
	error = event(event_example, &report);
	check(error == 0, "event: example read");
	check(report.type == BTD_MAP_EVENT_NEW_MESSAGE, "event: new message");
	check(report.has_handle && report.handle == 0x12345678U, "event: handle");
	check(strcmp(report.folder, "TELECOM/MSG/INBOX") == 0, "event: folder");
	check(report.msg_type == BTD_MAP_TYPE_SMS_CDMA, "event: message type");

	/* A shift, with its old folder. */
	error = event("<MAP-event-report version=\"1.0\"><event type=\"MessageShift\" handle=\"0A\" folder=\"telecom/msg/sent\" "
		      "old_folder=\"telecom/msg/outbox\" msg_type=\"SMS_GSM\"/></MAP-event-report>",
		      &report);
	check(error == 0 && report.type == BTD_MAP_EVENT_MESSAGE_SHIFT, "event: shift");
	check(strcmp(report.old_folder, "telecom/msg/outbox") == 0 && report.handle == 10U, "event: old folder");

	/* An event without a handle, and one of a kind not known. */
	error = event("<MAP-event-report version=\"1.0\"><event type=\"MemoryFull\"/></MAP-event-report>", &report);
	check(error == 0 && report.type == BTD_MAP_EVENT_MEMORY_FULL && !report.has_handle, "event: memory full without a handle");
	error = event("<MAP-event-report version=\"1.1\"><event type=\"ReadStatusChanged\" handle=\"1\" datetime=\"x\"/></MAP-event-report>", &report);
	check(error == 0 && report.type == BTD_MAP_EVENT_OTHER, "event: other kind");

	/* Each delivery and sending event. */
	error = event("<MAP-event-report><event type=\"SendingSuccess\" handle=\"5\"/></MAP-event-report>", &report);
	check(error == 0 && report.type == BTD_MAP_EVENT_SENDING_SUCCESS, "event: sending success");
	error = event("<MAP-event-report><event type=\"DeliveryFailure\" handle=\"5\"/></MAP-event-report>", &report);
	check(error == 0 && report.type == BTD_MAP_EVENT_DELIVERY_FAILURE, "event: delivery failure");
	error = event("<MAP-event-report><event type=\"MessageDeleted\" handle=\"5\"/></MAP-event-report>", &report);
	check(error == 0 && report.type == BTD_MAP_EVENT_MESSAGE_DELETED, "event: message deleted");

	/* Reports that are refused. */
	error = event("<MAP-event-report version=\"1.0\"/>", &report);
	check(error == EINVAL, "event: no event refused");
	error = event("<MAP-event-report><event type=\"NewMessage\" handle=\"1\"/><event type=\"NewMessage\" handle=\"2\"/></MAP-event-report>", &report);
	check(error == EINVAL, "event: two events refused");
	error = event("<MAP-event-report><event handle=\"1\"/></MAP-event-report>", &report);
	check(error == EINVAL, "event: no type refused");
	error = event("<MAP-event-report><event type=\"NewMessage\" handle=\"xyz\"/></MAP-event-report>", &report);
	check(error == EINVAL, "event: bad handle refused");
	error = event(listing_example, &report);
	check(error == EINVAL, "event: a listing is not a report");
}

/* Datetimes into seconds. */
static void
test_datetime(void)
{
	struct btd_map_time time;
	int64_t seconds;
	int64_t days;
	int from;
	int error;

	/* An offset east of UTC: 2007-12-13 13:05:10 at +09:00 is 04:05:10 UTC. */
	error = btd_mapxml_time_unix("20071213T130510+0900", 20U, 0, 0, 0, &seconds, &from);
	check(error == 0 && seconds == 1197518710LL && from == BTD_MAP_FROM_PHONE, "datetime: +0900");

	/* An offset west of UTC: 2026-10-10 03:04:05 at -05:30 is 08:34:05 UTC. */
	error = btd_mapxml_time_unix("20261010T030405-0530", 20U, 0, 0, 0, &seconds, &from);
	check(error == 0 && seconds == 1791621245LL, "datetime: -0530");

	/* A Z, with NUL bytes after it. */
	error = btd_mapxml_time_unix("19700101T000000Z\0\0", 18U, 1, 3600, 7200, &seconds, &from);
	check(error == 0 && seconds == 0 && from == BTD_MAP_FROM_PHONE, "datetime: Z with NUL bytes");

	/* No zone: the leap day's leap second is 2024-03-01 00:00:00, by zedBSD's offset 0. */
	error = btd_mapxml_time_unix("20240229T235960", 15U, 0, 0, 0, &seconds, &from);
	check(error == 0 && seconds == 1709251200LL && from == BTD_MAP_FROM_LOCAL, "datetime: leap day and second");

	/* No zone: MSETime's offset before zedBSD's. */
	error = btd_mapxml_time_unix("20071213T130510", 15U, 1, 32400, -3600, &seconds, &from);
	check(error == 0 && seconds == 1197518710LL && from == BTD_MAP_FROM_MSE, "datetime: MSETime's offset");
	error = btd_mapxml_time_unix("20071213T130510", 15U, 0, 0, 32400, &seconds, &from);
	check(error == 0 && seconds == 1197518710LL && from == BTD_MAP_FROM_LOCAL, "datetime: zedBSD's offset");

	/* The days of a few dates, worked out by hand. */
	days = btd_mapxml_days_from_civil(1970, 1, 1);
	check(days == 0, "datetime: 1970-01-01 is day 0");
	days = btd_mapxml_days_from_civil(2000, 3, 1);
	check(days == 11017, "datetime: 2000-03-01 is day 11017");
	days = btd_mapxml_days_from_civil(1, 1, 1);
	check(days == -719162, "datetime: 0001-01-01 is day -719162");

	/* The parse keeps the offset. */
	error = btd_mapxml_time_parse("20071213T130510-0130", 20U, &time);
	check(error == 0 && time.zone == BTD_MAP_ZONE_OFFSET && time.offset == -5400, "datetime: offset kept");
	check(time.year == 2007 && time.month == 12 && time.day == 13, "datetime: date kept");
	check(time.hour == 13 && time.minute == 5 && time.second == 10, "datetime: time kept");

	/* Leap years: 2000 has a 29th of February, 2100 and 2023 do not. */
	error = btd_mapxml_time_parse("20000229T000000", 15U, &time);
	check(error == 0, "datetime: 2000-02-29 taken");
	error = btd_mapxml_time_parse("21000229T000000", 15U, &time);
	check(error == EINVAL, "datetime: 2100-02-29 refused");
	error = btd_mapxml_time_parse("20230229T000000", 15U, &time);
	check(error == EINVAL, "datetime: 2023-02-29 refused");

	/* Out of range or malformed. */
	error = btd_mapxml_time_parse("20231301T000000", 15U, &time);
	check(error == EINVAL, "datetime: month 13 refused");
	error = btd_mapxml_time_parse("20230431T000000", 15U, &time);
	check(error == EINVAL, "datetime: April 31 refused");
	error = btd_mapxml_time_parse("20230101T240000", 15U, &time);
	check(error == EINVAL, "datetime: hour 24 refused");
	error = btd_mapxml_time_parse("20230101T236000", 15U, &time);
	check(error == EINVAL, "datetime: minute 60 refused");
	error = btd_mapxml_time_parse("20230101T235961", 15U, &time);
	check(error == EINVAL, "datetime: second 61 refused");
	error = btd_mapxml_time_parse("00000101T000000", 15U, &time);
	check(error == EINVAL, "datetime: year 0 refused");
	error = btd_mapxml_time_parse("20230101 000000", 15U, &time);
	check(error == EINVAL, "datetime: no T refused");
	error = btd_mapxml_time_parse("20230101T00000", 14U, &time);
	check(error == EINVAL, "datetime: short refused");
	error = btd_mapxml_time_parse("20230101T000000+2400", 20U, &time);
	check(error == EINVAL, "datetime: offset of 24 hours refused");
	error = btd_mapxml_time_parse("20230101T000000+090", 19U, &time);
	check(error == EINVAL, "datetime: short offset refused");
	error = btd_mapxml_time_parse("20230101T000000ZZ", 17U, &time);
	check(error == EINVAL, "datetime: text after Z refused");
	error = btd_mapxml_time_parse("2023-1-1T000000", 15U, &time);
	check(error == EINVAL, "datetime: not digits refused");
	error = btd_mapxml_time_unix("garbage", 7U, 0, 0, 0, &seconds, &from);
	check(error == EINVAL, "datetime: garbage refused");
}

/* Seconds written back as the filter's local datetime. */
static void
test_format(void)
{
	char text[16];
	int error;

	/* 1197518710 at +09:00 is 2007-12-13 13:05:10. */
	error = btd_mapxml_time_format(1197518710LL, 32400, text, sizeof(text));
	check(error == 0 && strcmp(text, "20071213T130510") == 0, "format: +0900");

	/* The leap day at UTC. */
	error = btd_mapxml_time_format(1709251199LL, 0, text, sizeof(text));
	check(error == 0 && strcmp(text, "20240229T235959") == 0, "format: leap day");

	/* A second before 1970, rounded down to the day before. */
	error = btd_mapxml_time_format(-1LL, 0, text, sizeof(text));
	check(error == 0 && strcmp(text, "19691231T235959") == 0, "format: before 1970");

	/* An offset west of UTC that moves the date back. */
	error = btd_mapxml_time_format(0LL, -3600, text, sizeof(text));
	check(error == 0 && strcmp(text, "19691231T230000") == 0, "format: offset west");

	/* Each written datetime reads back as the same seconds. */
	error = btd_mapxml_time_format(1791621245LL, -19800, text, sizeof(text));
	check(error == 0 && strcmp(text, "20261010T030405") == 0, "format: -0530");

	/* No room, or a year not of four digits. */
	error = btd_mapxml_time_format(0LL, 0, text, 15U);
	check(error == EINVAL, "format: no room refused");
	error = btd_mapxml_time_format(253402300800LL, 0, text, sizeof(text));
	check(error == EINVAL, "format: year 10000 refused");
	error = btd_mapxml_time_format(-62135596801LL, 0, text, sizeof(text));
	check(error == EINVAL, "format: year 0 refused");
	error = btd_mapxml_time_format(0x7fffffffffffffffLL, 0x7fffffff, text, sizeof(text));
	check(error == EINVAL, "format: huge seconds refused");
}

/* Random changes to the documents into both readers (a fixed seed): nothing crashes under ASan and UBSan. */
static void
test_fuzz(void)
{
	static const char specials[] = "<>/=\"'&;#x: \t\n!-?[]";
	struct btd_mapxml_counts counts;
	struct btd_map_event report;
	struct btd_map_time time;
	const char *source;
	size_t length;
	size_t changes;
	size_t change;
	size_t at;
	unsigned round;
	unsigned seed;
	int64_t seconds;
	int from;

	/* A fixed seed. */
	seed = 0x6d61705fU;
	for (round = 0U; round < TEST_FUZZ_ROUNDS; round++) {
		/* One of the two documents. */
		source = listing_example;
		if ((round & 1U) != 0U)
			source = event_example;
		length = strlen(source);
		memcpy(document, source, length);

		/* A few bytes changed: to a special byte, to any byte, or the document cut. */
		seed = seed * 1103515245U + 12345U;
		changes = (size_t)((seed >> 16) % 6U) + 1U;
		for (change = 0U; change < changes; change++) {
			seed = seed * 1103515245U + 12345U;
			at = (size_t)((seed >> 8) % length);
			seed = seed * 1103515245U + 12345U;
			if ((seed & 0x30000U) == 0U) {
				length = at + 1U;
			} else if ((seed & 0x40000U) != 0U) {
				document[at] = specials[(seed >> 20) % (sizeof(specials) - 1U)];
			} else {
				document[at] = (char)(seed >> 20);
			}
		}

		/* Both readers on it. */
		(void)btd_mapxml_listing((const uint8_t *)document, length, entries, 4U, &counts);
		(void)btd_mapxml_event((const uint8_t *)document, length, &report);

		/* Its first bytes as a datetime, and the seconds back as one. */
		(void)btd_mapxml_time_parse(document, length % 24U, &time);
		seconds = (int64_t)(((uint64_t)seed << 32) | seed);
		(void)btd_mapxml_time_unix(document + (length % 7U), 20U, (int)(seed & 1U), (int32_t)seed, (int32_t)(seed >> 3), &seconds, &from);
		(void)btd_mapxml_time_format(seconds, (int32_t)(seed >> 1), document, 16U);
	}

	/* Nothing crashed. */
	check(1, "fuzz: ran");
}
