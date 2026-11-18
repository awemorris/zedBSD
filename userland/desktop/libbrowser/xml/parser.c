/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Strict UTF8 and XML namespace parsing produce one independently owned immutable native tree. */

#include "xml/xml.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define XML_BYTES_MAX		(16U * 1024U * 1024U)
#define XML_DEPTH_MAX		256U
#define XML_RECORDS_MAX		65536U

/* A synchronous parser owns only its model and current native tree position; no VM cells participate. */
struct xml_parser {
	struct xml_document *document;
	struct xml_node *open;
	size_t offset;
	unsigned depth;
	int root_seen;
	struct xml_units doctype;
	int doctype_seen;
	int xhtml_nbsp;
};

static int xml_decode(struct wb_units *units, const unsigned char *bytes, size_t length);
static int xml_character(uint32_t point);
static int xml_append_point(struct wb_units *units, uint32_t point);
static int xml_name_start(uint32_t point);
static int xml_name_char(uint32_t point);
static uint32_t xml_scalar(const uint16_t *units, size_t length, size_t *width);
static int xml_space(uint16_t unit);
static void xml_skip_space(struct xml_parser *parser);
static int xml_at(struct xml_parser *parser, const char *text);
static int xml_equal(struct xml_units left, struct xml_units right);
static int xml_ascii(struct xml_units units, const char *text, int fold);
static int xml_literal(struct xml_document *document, const char *text, struct xml_units *units);
static int xml_copy(struct xml_document *document, const struct wb_units *input, struct xml_units *units);
static int xml_name(struct xml_parser *parser, struct xml_units *name, struct xml_units *prefix, struct xml_units *local);
static int xml_reference(struct xml_parser *parser, struct wb_units *units);
static int xml_value(struct xml_parser *parser, struct xml_units *value, int declaration);
static int xml_node_create(struct xml_parser *parser, int type, struct xml_node **out);
static int xml_attribute_read(struct xml_parser *parser, struct xml_node *node);
static int xml_namespace_lookup(struct xml_parser *parser, struct xml_node *node, struct xml_units prefix, struct xml_units *uri);
static int xml_namespaces(struct xml_parser *parser, struct xml_node *node);
static int xml_start(struct xml_parser *parser);
static int xml_end(struct xml_parser *parser);
static int xml_text(struct xml_parser *parser);
static int xml_delimited(struct xml_parser *parser, int type, const char *ending);
static int xml_instruction(struct xml_parser *parser);
static int xml_declaration(struct xml_parser *parser);
static int xml_doctype_literal(struct xml_parser *parser, int public_id, struct xml_units *value);
static int xml_doctype(struct xml_parser *parser);
static int xml_parse_tree(struct xml_parser *parser);

/*
 * Parses a finite UTF8 XML profile into actual namespace-aware native records.
 */
int
xml_document_parse(
	struct xml_document **document,
	const unsigned char *bytes,
	size_t length,
	struct xml_error *error)
{
	struct xml_document *made;
	struct xml_parser parser;
	int status;

	/* No partially parsed model is ever published to a caller. */
	*document = NULL;
	error->offset = 0;
	error->status = 0;
	if (length > XML_BYTES_MAX) {
		error->status = EOVERFLOW;
		return EOVERFLOW;
	}

	/* The unpublished model owns all later parser allocations. */
	made = calloc(1, sizeof(*made));
	if (made == NULL) {
		error->status = ENOMEM;
		return ENOMEM;
	}

	/* Decode completely before constructing any normal XML tree output. */
	wb_arena_init(&made->arena, 4096U);
	wb_units_init(&made->input);
	made->node.type = XML_DOCUMENT;
	made->records = 1;
	status = xml_decode(&made->input, bytes, length);
	if (status != 0) {
		error->offset = made->input.length;
		error->status = status;
		xml_document_destroy(made);
		return status;
	}

	/* Reserved namespace identity belongs to this model rather than a VM atom pool. */
	status = xml_literal(made, "http://www.w3.org/XML/1998/namespace", &made->xml_uri);
	if (status == 0)
		status = xml_literal(made, "http://www.w3.org/2000/xmlns/", &made->xmlns_uri);
	if (status != 0) {
		error->status = status;
		xml_document_destroy(made);
		return status;
	}

	/* Tree construction has explicit bounded depth and stable arena-backed owner records. */
	memset(&parser, 0, sizeof(parser));
	parser.document = made;
	parser.open = &made->node;
	status = xml_parse_tree(&parser);
	if (status != 0) {
		error->offset = parser.offset;
		error->status = status;
		xml_document_destroy(made);
		return status;
	}

	/* Succeeded: every input byte, matching tag and namespace constraint was accepted before publication. */
	*document = made;
	return 0;
}

/*
 * Releases every model-owned source, immutable value and native record together.
 */
void
xml_document_destroy(
	struct xml_document *document)
{
	/* Partial construction and normal teardown share one nonrecursive owner release. */
	if (document == NULL)
		return;
	wb_arena_release(&document->arena);
	wb_units_release(&document->input);
	free(document);

	/* Succeeded: no native XML storage or borrowed VM edge remains. */
	return;
}

/* Decodes only well-formed UTF8 scalar sequences and normalizes XML literal line endings. */
static int
xml_decode(
	struct wb_units *units,
	const unsigned char *bytes,
	size_t length)
{
	size_t offset;
	uint32_t point;
	uint32_t minimum;
	unsigned needed;
	unsigned index;
	unsigned char byte;
	int legal;
	int status;

	/* Known non-UTF8 byte order marks require a separately supported encoding profile. */
	if (length >= 2U) {
		if ((bytes[0] == 0xffU &&
		     bytes[1] == 0xfeU) ||
		    (bytes[0] == 0xfeU &&
		     bytes[1] == 0xffU))
			return ENOTSUP;
	}

	/* An optional UTF8 BOM is consumed before actual XML source characters. */
	offset = 0;
	if (length >= 3U) {
		if (bytes[0] == 0xefU &&
		    bytes[1] == 0xbbU &&
		    bytes[2] == 0xbfU)
			offset = 3U;
	}

	/* Each sequence must have the shortest scalar representation and every required continuation byte. */
	while (offset < length) {
		byte = bytes[offset++];
		point = byte;
		needed = 0;
		minimum = 0;
		if (byte >= 0x80U) {
			if (byte >= 0xc2U && byte <= 0xdfU) {
				point = byte & 0x1fU;
				needed = 1;
				minimum = 0x80U;
			} else if (byte >= 0xe0U && byte <= 0xefU) {
				point = byte & 0x0fU;
				needed = 2;
				minimum = 0x800U;
			} else if (byte >= 0xf0U && byte <= 0xf4U) {
				point = byte & 7U;
				needed = 3;
				minimum = 0x10000U;
			} else {
				return EINVAL;
			}
		}

		/* Continuation consumption never reads beyond the remaining actual byte input. */
		if (needed > length - offset)
			return EINVAL;
		for (index = 0; index < needed; index++) {
			byte = bytes[offset++];
			if (byte < 0x80U || byte > 0xbfU)
				return EINVAL;
			point = (point << 6) | (byte & 0x3fU);
		}

		/* Overlong sequences cannot represent an accepted XML scalar. */
		if (point < minimum)
			return EINVAL;
		legal = xml_character(point);
		if (!legal)
			return EINVAL;

		/* Literal CRLF and CR normalize once; character references are decoded later and retain their value. */
		if (point == 13U) {
			point = 10U;
			if (offset < length && bytes[offset] == 10U)
				offset++;
		}

		/* Commit only the validated and normalized actual scalar. */
		status = xml_append_point(units, point);
		if (status != 0)
			return status;
	}

	/* Succeeded: input contains only legal XML characters represented as normalized UTF16. */
	return 0;
}

/* Classifies XML1.0 Char scalars without accepting lone surrogates or forbidden controls. */
static int
xml_character(
	uint32_t point)
{
	/* Only tab, LF and CR are legal low control characters. */
	if (point == 9U ||
	    point == 10U ||
	    point == 13U)
		return 1;
	if (point >= 0x20U && point <= 0xd7ffU)
		return 1;
	if (point >= 0xe000U && point <= 0xfffdU)
		return 1;
	if (point >= 0x10000U && point <= 0x10ffffU)
		return 1;

	/* Rejected: this value cannot occur as an XML character. */
	return 0;
}

/* Appends one already validated scalar without losing supplementary-plane characters. */
static int
xml_append_point(
	struct wb_units *units,
	uint32_t point)
{
	uint16_t pair[2];
	size_t length;
	int status;

	/* Supplementary scalars use the standard UTF16 surrogate pair representation. */
	length = 1;
	pair[0] = (uint16_t)point;
	if (point >= 0x10000U) {
		point -= 0x10000U;
		pair[0] = (uint16_t)(0xd800U + (point >> 10));
		pair[1] = (uint16_t)(0xdc00U + (point & 0x3ffU));
		length = 2;
	}

	/* Succeeded or failed: the growable source buffer keeps ordinary C ownership. */
	status = wb_units_append(units, pair, length);
	return status;
}

/* Implements the XML1.0 Fifth Edition Unicode NameStartChar ranges. */
static int
xml_name_start(
	uint32_t point)
{
	/* ASCII starts are letters, underscore and the QName separator. */
	if (point == ':' || point == '_')
		return 1;
	if (point >= 'A' && point <= 'Z')
		return 1;
	if (point >= 'a' && point <= 'z')
		return 1;

	/* Explicit Unicode ranges preserve every gap specified by the XML name grammar. */
	if (point >= 0xc0U && point <= 0xd6U)
		return 1;
	if (point >= 0xd8U && point <= 0xf6U)
		return 1;
	if (point >= 0xf8U && point <= 0x2ffU)
		return 1;
	if (point >= 0x370U && point <= 0x37dU)
		return 1;
	if (point >= 0x37fU && point <= 0x1fffU)
		return 1;
	if (point >= 0x200cU && point <= 0x200dU)
		return 1;
	if (point >= 0x2070U && point <= 0x218fU)
		return 1;
	if (point >= 0x2c00U && point <= 0x2fefU)
		return 1;
	if (point >= 0x3001U && point <= 0xd7ffU)
		return 1;
	if (point >= 0xf900U && point <= 0xfdcfU)
		return 1;
	if (point >= 0xfdf0U && point <= 0xfffdU)
		return 1;
	if (point >= 0x10000U && point <= 0xeffffU)
		return 1;

	/* Rejected: a name cannot begin with this scalar. */
	return 0;
}

/* Adds the remaining scalar ranges allowed after an XML name's initial character. */
static int
xml_name_char(
	uint32_t point)
{
	int start;

	/* Every start character remains valid in later positions. */
	start = xml_name_start(point);
	if (start)
		return 1;
	if (point == '-' ||
	    point == '.' ||
	    point == 0xb7U)
		return 1;
	if (point >= '0' && point <= '9')
		return 1;
	if (point >= 0x300U && point <= 0x36fU)
		return 1;
	if (point >= 0x203fU && point <= 0x2040U)
		return 1;

	/* Rejected: this scalar cannot continue an XML name. */
	return 0;
}

/* Reads a scalar from the already strictly validated UTF16 source. */
static uint32_t
xml_scalar(
	const uint16_t *units,
	size_t length,
	size_t *width)
{
	uint32_t point;

	/* Strict UTF8 decoding guarantees that every high surrogate has its corresponding low surrogate. */
	*width = 1;
	point = units[0];
	if (point >= 0xd800U &&
	    point <= 0xdbffU &&
	    length >= 2U) {
		*width = 2;
		point = 0x10000U + ((point - 0xd800U) << 10) + (units[1] - 0xdc00U);
	}

	/* Succeeded: callers advance by the actual number of native UTF16 units. */
	return point;
}

/* Recognizes precisely XML's literal whitespace characters. */
static int
xml_space(
	uint16_t unit)
{
	/* Input line ending normalization already replaced literal CR with LF. */
	if (unit == ' ' ||
	    unit == '\t' ||
	    unit == '\n' ||
	    unit == '\r')
		return 1;

	/* Rejected: this code unit is not XML whitespace. */
	return 0;
}

/* Advances over actual XML whitespace without consuming data or markup. */
static void
xml_skip_space(
	struct xml_parser *parser)
{
	int space;

	/* Check each native source unit before advancing the synchronous cursor. */
	while (parser->offset < parser->document->input.length) {
		space = xml_space(parser->document->input.data[parser->offset]);
		if (!space)
			break;
		parser->offset++;
	}

	/* Succeeded: the cursor points at the next non-whitespace unit or actual EOF. */
	return;
}

/* Matches one literal ASCII delimiter against bounded actual UTF16 input. */
static int
xml_at(
	struct xml_parser *parser,
	const char *text)
{
	size_t index;
	size_t length;

	/* The complete delimiter must fit before any source comparison reads it. */
	length = strlen(text);
	if (length > parser->document->input.length - parser->offset)
		return 0;
	for (index = 0; index < length; index++) {
		if (parser->document->input.data[parser->offset + index] != (unsigned char)text[index])
			return 0;
	}

	/* Succeeded: this exact delimiter occurs at the current actual source position. */
	return 1;
}

/* Compares two model-owned immutable UTF16 views without namespace or case normalization. */
static int
xml_equal(
	struct xml_units left,
	struct xml_units right)
{
	int same;

	/* Empty namespace identity never needs a dereference of a null source view. */
	if (left.length != right.length)
		return 0;
	if (left.length == 0)
		return 1;
	same = memcmp(left.data, right.data, left.length * sizeof(*left.data)) == 0;

	/* Succeeded: actual normalized units alone establish name or namespace identity. */
	return same;
}

/* Matches finite ASCII grammar values with optional encoding-name case folding. */
static int
xml_ascii(
	struct xml_units units,
	const char *text,
	int fold)
{
	size_t index;
	size_t length;
	uint16_t unit;
	unsigned char byte;

	/* Exact length prevents prefix matches for reserved names and declarations. */
	length = strlen(text);
	if (units.length != length)
		return 0;
	for (index = 0; index < length; index++) {
		unit = units.data[index];
		byte = (unsigned char)text[index];
		if (fold) {
			if (unit >= 'A' && unit <= 'Z')
				unit = (uint16_t)(unit + 0x20U);
			if (byte >= 'A' && byte <= 'Z')
				byte = (unsigned char)(byte + 0x20U);
		}

		/* Compare the expected literal after any permitted ASCII case folding. */
		if (unit != byte)
			return 0;
	}

	/* Succeeded: every actual unit matches the expected grammar value. */
	return 1;
}

/* Places reserved ASCII namespace identities into this model's immutable arena. */
static int
xml_literal(
	struct xml_document *document,
	const char *text,
	struct xml_units *units)
{
	uint16_t *made;
	size_t index;
	size_t length;

	/* Only bounded internal literals use this helper. */
	length = strlen(text);
	made = wb_arena_alloc(&document->arena, length * sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	for (index = 0; index < length; index++)
		made[index] = (unsigned char)text[index];
	units->data = made;
	units->length = length;

	/* Succeeded: the reserved identity shares this model's exact storage lifetime. */
	return 0;
}

/* Copies a complete decoded value into the arena before releasing temporary growable units. */
static int
xml_copy(
	struct xml_document *document,
	const struct wb_units *input,
	struct xml_units *units)
{
	uint16_t *made;

	/* Empty values have an explicit zero-length immutable view. */
	units->data = NULL;
	units->length = input->length;
	if (input->length == 0)
		return 0;
	made = wb_arena_alloc(&document->arena, input->length * sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	memcpy(made, input->data, input->length * sizeof(*made));
	units->data = made;

	/* Succeeded: later buffer growth or release cannot change the published native value. */
	return 0;
}

/* Reads one exact QName and preserves prefix/local views into immutable normalized source. */
static int
xml_name(
	struct xml_parser *parser,
	struct xml_units *name,
	struct xml_units *prefix,
	struct xml_units *local)
{
	const uint16_t *input;
	size_t length;
	size_t start;
	size_t width;
	size_t colon;
	uint32_t point;
	int legal;

	/* The complete source is already valid UTF16, but a name must begin at an actual remaining scalar. */
	input = parser->document->input.data;
	length = parser->document->input.length;
	start = parser->offset;
	if (start >= length)
		return EINVAL;
	point = xml_scalar(input + start, length - start, &width);
	legal = xml_name_start(point);
	if (!legal || point == ':')
		return EINVAL;
	colon = (size_t)-1;
	parser->offset += width;

	/* Continue through XML NameChar scalars while permitting only one QName separator. */
	while (parser->offset < length) {
		point = xml_scalar(input + parser->offset, length - parser->offset, &width);
		legal = xml_name_char(point);
		if (!legal)
			break;
		if (point == ':') {
			if (colon != (size_t)-1)
				return EINVAL;
			colon = parser->offset;
		}

		/* The complete scalar remains part of this actual QName. */
		parser->offset += width;
	}

	/* Native name views borrow only the completed owning input buffer. */
	name->data = input + start;
	name->length = parser->offset - start;
	prefix->data = NULL;
	prefix->length = 0;
	*local = *name;
	if (colon != (size_t)-1) {
		if (colon + 1U >= parser->offset)
			return EINVAL;
		point = xml_scalar(input + colon + 1U, parser->offset - colon - 1U, &width);
		legal = xml_name_start(point);
		if (!legal || point == ':')
			return EINVAL;
		prefix->data = input + start;
		prefix->length = colon - start;
		local->data = input + colon + 1U;
		local->length = parser->offset - colon - 1U;
	}

	/* Succeeded: case and supplementary characters remain exact in both qualified and expanded names. */
	return 0;
}

/* Expands only actual predefined or legal numeric XML references into an owning temporary value. */
static int
xml_reference(
	struct xml_parser *parser,
	struct wb_units *units)
{
	struct xml_units name;
	const uint16_t *input;
	size_t length;
	size_t start;
	uint32_t point;
	unsigned base;
	unsigned digit;
	uint16_t unit;
	int digits;
	int matched;
	int legal;
	int status;

	/* The caller points at an actual ampersand in character data or an attribute value. */
	input = parser->document->input.data;
	length = parser->document->input.length;
	parser->offset++;
	if (parser->offset >= length)
		return EINVAL;
	point = 0;
	if (input[parser->offset] == '#') {
		parser->offset++;
		base = 10;
		if (parser->offset < length && input[parser->offset] == 'x') {
			base = 16;
			parser->offset++;
		}

		/* At least one legal digit must precede the required reference terminator. */
		digits = 0;

		/* Checked accumulation rejects overflow before converting a numeric reference to a scalar. */
		while (parser->offset < length) {
			unit = input[parser->offset];
			if (unit == ';')
				break;
			if (unit >= '0' && unit <= '9') {
				digit = unit - '0';
			} else if (base == 16U &&
			    unit >= 'a' &&
			    unit <= 'f') {
				digit = unit - 'a' + 10U;
			} else if (base == 16U &&
			    unit >= 'A' &&
			    unit <= 'F') {
				digit = unit - 'A' + 10U;
			} else {
				return EINVAL;
			}

			/* The next digit must fit the finite Unicode scalar range before arithmetic. */
			if (point > (0x10ffffU - digit) / base)
				return EINVAL;
			point = point * base + digit;
			parser->offset++;
			digits = 1;
		}

		/* Empty or unterminated numeric references are not XML character data. */
		if (!digits || parser->offset >= length)
			return EINVAL;
		legal = xml_character(point);
		if (!legal)
			return EINVAL;
	} else {
		/* Named references must match exactly one of XML's five predefined entities. */
		start = parser->offset;
		while (parser->offset < length && input[parser->offset] != ';')
			parser->offset++;
		if (parser->offset >= length)
			return EINVAL;
		name.data = input + start;
		name.length = parser->offset - start;
		matched = xml_ascii(name, "amp", 0);
		if (matched)
			point = '&';
		matched = xml_ascii(name, "lt", 0);
		if (matched)
			point = '<';
		matched = xml_ascii(name, "gt", 0);
		if (matched)
			point = '>';
		matched = xml_ascii(name, "apos", 0);
		if (matched)
			point = '\'';
		matched = xml_ascii(name, "quot", 0);
		if (matched)
			point = '"';
		/* The XHTML 1.0 Strict DTD declares nbsp as U+00A0; other XML profiles do not. */
		matched = parser->xhtml_nbsp && xml_ascii(name, "nbsp", 0);
		if (matched)
			point = 0xa0U;
		if (point == 0)
			return EINVAL;
	}

	/* A referenced tab, LF or CR is retained rather than treated as literal attribute whitespace. */
	parser->offset++;
	status = xml_append_point(units, point);

	/* Succeeded or failed: the reference never publishes partial normal tree output. */
	return status;
}

/* Reads one quoted attribute or declaration value, preserving exact reference normalization rules. */
static int
xml_value(
	struct xml_parser *parser,
	struct xml_units *value,
	int declaration)
{
	struct wb_units units;
	const uint16_t *input;
	size_t length;
	uint16_t quote;
	uint16_t unit;
	int space;
	int status;
	int closed;

	/* Only XML quote delimiters can begin an attribute or declaration value. */
	input = parser->document->input.data;
	length = parser->document->input.length;
	if (parser->offset >= length)
		return EINVAL;
	quote = input[parser->offset++];
	if (quote != '\'' && quote != '"')
		return EINVAL;
	wb_units_init(&units);
	status = 0;
	closed = 0;

	/* Entity expansion and literal whitespace normalization precede namespace binding decisions. */
	while (parser->offset < length) {
		unit = input[parser->offset];
		if (unit == quote) {
			parser->offset++;
			closed = 1;
			break;
		}

		/* Literal markup cannot occur inside an XML attribute value. */
		if (unit == '<') {
			status = EINVAL;
			break;
		}

		/* Reference syntax is interpreted only in ordinary XML character and attribute data. */
		if (unit == '&') {
			if (declaration) {
				status = EINVAL;
				break;
			}

			/* Expand the actual reference before committing immutable attribute data. */
			status = xml_reference(parser, &units);
		} else {
			/* Only literal whitespace is replaced with a space in ordinary attribute values. */
			parser->offset++;
			space = xml_space(unit);
			if (space && !declaration)
				unit = ' ';
			status = wb_units_append(&units, &unit, 1U);
		}

		/* A failed data operation stops normal processing immediately. */
		if (status != 0)
			break;
	}

	/* Unterminated values cannot leave a successful immutable record behind. */
	if (status == 0 && !closed)
		status = EINVAL;
	if (status == 0)
		status = xml_copy(parser->document, &units, value);
	wb_units_release(&units);

	/* Succeeded or failed: every temporary decoded value has been released. */
	return status;
}

/* Creates one stable bounded native record in its actual owner tree. */
static int
xml_node_create(
	struct xml_parser *parser,
	int type,
	struct xml_node **out)
{
	struct xml_node *made;
	struct xml_node *parent;

	/* Native node and attribute records share one finite allocation bound. */
	*out = NULL;
	if (parser->document->records >= XML_RECORDS_MAX)
		return EOVERFLOW;
	made = wb_arena_zalloc(&parser->document->arena, sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	parser->document->records++;

	/* Only this unpublished model observes construction until the complete parse succeeds. */
	parent = parser->open;
	made->type = type;
	made->parent = parent;
	if (parent->last_child == NULL) {
		parent->first_child = made;
	} else {
		parent->last_child->next = made;
	}

	/* The owner retains the last record for the next actual child append. */
	parent->last_child = made;
	*out = made;

	/* Succeeded: native tree links and record storage share the same owning model lifetime. */
	return 0;
}

/* Reads an actual attribute before binding namespaces across the complete start tag. */
static int
xml_attribute_read(
	struct xml_parser *parser,
	struct xml_node *node)
{
	struct xml_attribute *made;
	struct xml_attribute *previous;
	int same;
	int status;

	/* Attribute storage remains stable even while later start-tag records allocate. */
	if (parser->document->records >= XML_RECORDS_MAX)
		return EOVERFLOW;
	made = wb_arena_zalloc(&parser->document->arena, sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	parser->document->records++;
	status = xml_name(parser, &made->name, &made->prefix, &made->local);
	if (status != 0)
		return status;

	/* Equal qualified names are invalid even before namespace expansion can detect alias duplicates. */
	for (previous = node->attributes; previous != NULL; previous = previous->next) {
		same = xml_equal(previous->name, made->name);
		if (same)
			return EINVAL;
	}

	/* XML permits whitespace around the attribute assignment operator. */
	xml_skip_space(parser);
	if (parser->offset >= parser->document->input.length)
		return EINVAL;
	if (parser->document->input.data[parser->offset] != '=')
		return EINVAL;
	parser->offset++;
	xml_skip_space(parser);
	status = xml_value(parser, &made->value, 0);
	if (status != 0)
		return status;

	/* Current namespace declarations become visible only after their complete normalized values exist. */
	if (node->last_attribute == NULL) {
		node->attributes = made;
	} else {
		node->last_attribute->next = made;
	}

	/* The complete current start tag retains its final actual attribute. */
	node->last_attribute = made;

	/* Succeeded: actual source order and duplicate checking include this attribute. */
	return 0;
}

/* Resolves actual default or prefixed namespace identity through current and ancestor declarations. */
static int
xml_namespace_lookup(
	struct xml_parser *parser,
	struct xml_node *node,
	struct xml_units prefix,
	struct xml_units *uri)
{
	struct xml_attribute *attribute;
	int reserved;
	int declaration;
	int same;

	/* Reserved prefixes have implicit exact namespace identities. */
	uri->data = NULL;
	uri->length = 0;
	reserved = xml_ascii(prefix, "xml", 0);
	if (reserved) {
		*uri = parser->document->xml_uri;
		return 0;
	}

	/* Namespace declarations have their own independently reserved prefix identity. */
	reserved = xml_ascii(prefix, "xmlns", 0);
	if (reserved) {
		*uri = parser->document->xmlns_uri;
		return 0;
	}

	/* A declaration on this actual element takes precedence over every ancestor's binding. */
	while (node != NULL && node->type == XML_ELEMENT) {
		for (attribute = node->attributes; attribute != NULL; attribute = attribute->next) {
			if (prefix.length == 0) {
				declaration = 0;
				if (attribute->prefix.length == 0)
					declaration = xml_ascii(attribute->local, "xmlns", 0);
			} else {
				declaration = xml_ascii(attribute->prefix, "xmlns", 0);
				if (declaration) {
					same = xml_equal(attribute->local, prefix);
					declaration = same;
				}
			}

			/* The nearest complete matching declaration supplies the exact namespace value. */
			if (declaration) {
				*uri = attribute->value;
				return 0;
			}
		}

		/* Continue with the actual ancestor namespace scope. */
		node = node->parent;
	}

	/* No default namespace is legal; an undeclared actual prefix is a namespace well-formedness error. */
	if (prefix.length != 0)
		return EINVAL;

	/* Succeeded: an unbound default represents the absent namespace. */
	return 0;
}

/* Checks reserved bindings and resolves every actual expanded name before accepting an element. */
static int
xml_namespaces(
	struct xml_parser *parser,
	struct xml_node *node)
{
	struct xml_attribute *attribute;
	struct xml_attribute *previous;
	struct xml_units declared;
	int declaration;
	int reserved;
	int xml_prefix;
	int xml_uri;
	int xmlns_uri;
	int same;
	int status;

	/* Validate every declaration before any namespace lookup can use an invalid binding. */
	for (attribute = node->attributes; attribute != NULL; attribute = attribute->next) {
		declaration = xml_ascii(attribute->prefix, "xmlns", 0);
		declared.data = NULL;
		declared.length = 0;
		if (declaration) {
			declared = attribute->local;
		} else if (attribute->prefix.length == 0) {
			declaration = xml_ascii(attribute->local, "xmlns", 0);
		}

		/* Only actual namespace declaration attributes alter a binding. */
		if (!declaration)
			continue;
		attribute->uri = parser->document->xmlns_uri;
		reserved = xml_ascii(declared, "xmlns", 0);
		if (reserved)
			return EINVAL;
		xml_prefix = xml_ascii(declared, "xml", 0);
		xml_uri = xml_equal(attribute->value, parser->document->xml_uri);
		xmlns_uri = xml_equal(attribute->value, parser->document->xmlns_uri);
		if (xmlns_uri)
			return EINVAL;
		if (xml_prefix != xml_uri)
			return EINVAL;
		if (declared.length != 0 && attribute->value.length == 0)
			return EINVAL;
	}

	/* Element prefix xmlns is reserved for declarations, never ordinary element identity. */
	reserved = xml_ascii(node->prefix, "xmlns", 0);
	if (reserved)
		return EINVAL;
	status = xml_namespace_lookup(parser, node, node->prefix, &node->uri);
	if (status != 0)
		return status;

	/* Unprefixed ordinary attributes do not inherit the element's default namespace. */
	for (attribute = node->attributes; attribute != NULL; attribute = attribute->next) {
		if (attribute->uri.length == 0 && attribute->prefix.length != 0) {
			status = xml_namespace_lookup(parser, node, attribute->prefix, &attribute->uri);
			if (status != 0)
				return status;
		}

		/* Aliased prefixes cannot introduce two attributes with the same actual expanded name. */
		for (previous = node->attributes; previous != attribute; previous = previous->next) {
			same = xml_equal(previous->local, attribute->local);
			if (!same)
				continue;
			same = xml_equal(previous->uri, attribute->uri);
			if (same)
				return EINVAL;
		}
	}

	/* Succeeded: actual qualified names and exact namespace identities satisfy this profile's constraints. */
	return 0;
}

/* Consumes a real start tag and binds its complete attributes before opening its child scope. */
static int
xml_start(
	struct xml_parser *parser)
{
	struct xml_node *node;
	size_t before_space;
	int ending;
	int self_closing;
	int status;

	/* Exactly one document element is permitted, and every element contributes to the bounded depth. */
	if (parser->open->type == XML_DOCUMENT && parser->root_seen)
		return EINVAL;
	if (parser->depth >= XML_DEPTH_MAX)
		return EOVERFLOW;
	parser->offset++;
	status = xml_node_create(parser, XML_ELEMENT, &node);
	if (status != 0)
		return status;
	status = xml_name(parser, &node->name, &node->prefix, &node->local);
	if (status != 0)
		return status;
	if (parser->open->type == XML_DOCUMENT && parser->doctype_seen &&
	    !xml_equal(node->name, parser->doctype))
		return EINVAL;
	self_closing = 0;
	ending = 0;

	/* Attributes require separating whitespace; closing delimiters do not. */
	while (parser->offset < parser->document->input.length) {
		before_space = parser->offset;
		xml_skip_space(parser);
		ending = xml_at(parser, "/>");
		if (ending) {
			self_closing = 1;
			parser->offset += 2U;
			break;
		}

		/* An ordinary close delimiter completes a nonempty start tag. */
		ending = xml_at(parser, ">");
		if (ending) {
			parser->offset++;
			break;
		}

		/* Another attribute requires actual separating XML whitespace. */
		if (parser->offset == before_space)
			return EINVAL;
		status = xml_attribute_read(parser, node);
		if (status != 0)
			return status;
	}

	/* EOF cannot substitute for a required actual markup terminator. */
	if (!ending)
		return EINVAL;
	status = xml_namespaces(parser, node);
	if (status != 0)
		return status;

	/* The current element's namespace declarations affect only its actual descendant scope. */
	if (parser->open->type == XML_DOCUMENT)
		parser->root_seen = 1;
	if (!self_closing) {
		parser->open = node;
		parser->depth++;
	}

	/* Succeeded: this complete real tag contributes exactly one stable native element. */
	return 0;
}

/* Closes only a matching actual qualified element name, preserving XML's case sensitivity. */
static int
xml_end(
	struct xml_parser *parser)
{
	struct xml_units name;
	struct xml_units prefix;
	struct xml_units local;
	int same;
	int ending;
	int status;

	/* Closing tags cannot close the native document record itself. */
	if (parser->open->type != XML_ELEMENT)
		return EINVAL;
	parser->offset += 2U;
	status = xml_name(parser, &name, &prefix, &local);
	if (status != 0)
		return status;
	xml_skip_space(parser);
	ending = xml_at(parser, ">");
	if (!ending)
		return EINVAL;
	same = xml_equal(name, parser->open->name);
	if (!same)
		return EINVAL;

	/* Namespace scope returns to the actual parent only after a complete matching end tag. */
	parser->offset++;
	parser->open = parser->open->parent;
	parser->depth--;

	/* Succeeded: one actual open element was closed. */
	return 0;
}

/* Emits genuine character data while rejecting references or non-whitespace outside the root. */
static int
xml_text(
	struct xml_parser *parser)
{
	struct wb_units units;
	struct xml_node *node;
	uint16_t unit;
	int space;
	int forbidden;
	int status;

	/* XML document Misc permits literal whitespace but no character references or other data. */
	if (parser->open->type == XML_DOCUMENT) {
		while (parser->offset < parser->document->input.length) {
			unit = parser->document->input.data[parser->offset];
			if (unit == '<')
				break;
			space = xml_space(unit);
			if (!space)
				return EINVAL;
			parser->offset++;
		}

		/* Succeeded: outside-root whitespace contributes no invalid Document Text child. */
		return 0;
	}

	/* Actual element character data expands references into independently owned immutable values. */
	wb_units_init(&units);
	status = 0;
	while (parser->offset < parser->document->input.length) {
		unit = parser->document->input.data[parser->offset];
		if (unit == '<')
			break;
		forbidden = xml_at(parser, "]]>");
		if (forbidden) {
			status = EINVAL;
			break;
		}

		/* Reference syntax is interpreted only in ordinary XML character and attribute data. */
		if (unit == '&') {
			status = xml_reference(parser, &units);
		} else {
			parser->offset++;
			status = wb_units_append(&units, &unit, 1U);
		}

		/* A failed data operation stops normal processing immediately. */
		if (status != 0)
			break;
	}

	/* Only complete accepted character data contributes a native Text record. */
	if (status == 0)
		status = xml_node_create(parser, XML_TEXT, &node);
	if (status == 0)
		status = xml_copy(parser->document, &units, &node->value);
	wb_units_release(&units);

	/* Succeeded or failed: normal processing never retains a temporary growable value. */
	return status;
}

/* Preserves a real Comment or CDATA record without interpreting markup or entity-looking data. */
static int
xml_delimited(
	struct xml_parser *parser,
	int type,
	const char *ending)
{
	struct xml_node *node;
	size_t start;
	size_t length;
	int found;
	int forbidden;
	int status;

	/* CDATA is character content and therefore cannot occur outside the actual root element. */
	if (type == XML_CDATA && parser->open->type != XML_ELEMENT)
		return EINVAL;
	start = parser->offset;
	length = strlen(ending);
	found = 0;

	/* Every comment must end before an internal double hyphen can be accepted as normal data. */
	while (parser->offset < parser->document->input.length) {
		found = xml_at(parser, ending);
		if (found)
			break;
		if (type == XML_COMMENT) {
			forbidden = xml_at(parser, "--");
			if (forbidden)
				return EINVAL;
		}

		/* Preserve this uninterpreted actual data unit until the terminator. */
		parser->offset++;
	}

	/* EOF cannot supply the required complete delimiter. */
	if (!found)
		return EINVAL;
	status = xml_node_create(parser, type, &node);
	if (status != 0)
		return status;

	/* Normalized source remains model-owned and stable for these uninterpreted data views. */
	node->value.data = parser->document->input.data + start;
	node->value.length = parser->offset - start;
	parser->offset += length;

	/* Succeeded: the complete delimited native record preserves actual character content. */
	return 0;
}

/* Parses declaration pseudo-attributes with strict order and this finite UTF8/version profile. */
static int
xml_declaration(
	struct xml_parser *parser)
{
	struct xml_units name;
	struct xml_units prefix;
	struct xml_units local;
	struct xml_units value;
	size_t before_space;
	size_t index;
	uint16_t unit;
	int stage;
	int ending;
	int matched;
	int legal;
	int status;

	/* Version is required first, followed optionally by encoding and then standalone. */
	stage = 0;
	ending = 0;
	while (parser->offset < parser->document->input.length) {
		before_space = parser->offset;
		xml_skip_space(parser);
		ending = xml_at(parser, "?>");
		if (ending)
			break;
		if (before_space == parser->offset)
			return EINVAL;
		status = xml_name(parser, &name, &prefix, &local);
		if (status != 0)
			return status;
		if (prefix.length != 0)
			return EINVAL;
		xml_skip_space(parser);
		if (parser->offset >= parser->document->input.length)
			return EINVAL;
		if (parser->document->input.data[parser->offset] != '=')
			return EINVAL;
		parser->offset++;
		xml_skip_space(parser);
		status = xml_value(parser, &value, 1);
		if (status != 0)
			return status;

		/* Version grammar is checked independently of whether this profile implements the declared version. */
		matched = xml_ascii(name, "version", 0);
		if (matched) {
			if (stage != 0 || value.length < 3U)
				return EINVAL;
			if (value.data[0] != '1' || value.data[1] != '.')
				return EINVAL;
			for (index = 2U; index < value.length; index++) {
				unit = value.data[index];
				if (unit < '0' || unit > '9')
					return EINVAL;
			}

			/* A valid declaration of another version needs a separate parser profile. */
			matched = xml_ascii(value, "1.0", 0);
			if (!matched)
				return ENOTSUP;
			parser->document->version = value;
			stage = 1;
			continue;
		}

		/* EncName syntax is ASCII; valid but unsupported encodings never produce a successful UTF8 model. */
		matched = xml_ascii(name, "encoding", 0);
		if (matched) {
			if (stage != 1 || value.length == 0)
				return EINVAL;
			for (index = 0; index < value.length; index++) {
				unit = value.data[index];
				legal = 0;
				if (unit >= 'A' && unit <= 'Z')
					legal = 1;
				if (unit >= 'a' && unit <= 'z')
					legal = 1;
				if (index != 0) {
					if (unit >= '0' && unit <= '9')
						legal = 1;
					if (unit == '.' ||
					    unit == '_' ||
					    unit == '-')
						legal = 1;
				}

				/* Every declared encoding character must satisfy EncName syntax. */
				if (!legal)
					return EINVAL;
			}

			/* Only this finite encoding profile can decode actual incoming bytes. */
			matched = xml_ascii(value, "UTF-8", 1);
			if (!matched)
				return ENOTSUP;
			parser->document->encoding = value;
			stage = 2;
			continue;
		}

		/* Standalone is the final optional declaration field and has only two legal literal values. */
		matched = xml_ascii(name, "standalone", 0);
		if (!matched ||
		    stage == 0 ||
		    stage == 3)
			return EINVAL;
		matched = xml_ascii(value, "yes", 0);
		if (!matched)
			matched = xml_ascii(value, "no", 0);
		if (!matched)
			return EINVAL;
		parser->document->standalone = value;
		stage = 3;
	}

	/* A declaration must contain its version and an actual closing delimiter. */
	if (!ending || stage == 0)
		return EINVAL;
	parser->offset += 2U;

	/* Succeeded: declaration metadata is retained separately from real ProcessingInstruction nodes. */
	return 0;
}

/* Preserves genuine processing instructions and handles only an initial exact XML declaration specially. */
static int
xml_instruction(
	struct xml_parser *parser)
{
	struct xml_node *node;
	struct xml_units name;
	struct xml_units prefix;
	struct xml_units local;
	size_t instruction_start;
	size_t data_start;
	int reserved;
	int exact;
	int ending;
	int space;
	int status;

	/* Namespace-aware XML requires an NCName processing target rather than a prefixed QName. */
	instruction_start = parser->offset;
	parser->offset += 2U;
	status = xml_name(parser, &name, &prefix, &local);
	if (status != 0)
		return status;
	if (prefix.length != 0)
		return EINVAL;
	reserved = xml_ascii(name, "xml", 1);
	if (reserved) {
		exact = xml_ascii(name, "xml", 0);
		if (!exact || instruction_start != 0)
			return EINVAL;
		status = xml_declaration(parser);

		/* Succeeded or failed: declaration content cannot masquerade as a normal processing instruction. */
		return status;
	}

	/* Data needs an XML whitespace separator unless the processing instruction has no data at all. */
	ending = xml_at(parser, "?>");
	if (!ending) {
		if (parser->offset >= parser->document->input.length)
			return EINVAL;
		space = xml_space(parser->document->input.data[parser->offset]);
		if (!space)
			return EINVAL;
		xml_skip_space(parser);
	}

	/* Processing instruction data excludes the required whitespace separator. */
	data_start = parser->offset;

	/* Only the actual PI terminator closes the native target/data record. */
	while (parser->offset < parser->document->input.length) {
		ending = xml_at(parser, "?>");
		if (ending)
			break;
		parser->offset++;
	}

	/* EOF cannot substitute for a required actual markup terminator. */
	if (!ending)
		return EINVAL;
	status = xml_node_create(parser, XML_PI, &node);
	if (status != 0)
		return status;
	node->name = name;
	node->local = local;
	node->value.data = parser->document->input.data + data_start;
	node->value.length = parser->offset - data_start;
	parser->offset += 2U;

	/* Succeeded: target and uninterpreted data are real model-owned XML records. */
	return 0;
}

/* Reads one system/public identifier literally, without fetching or expanding its content. */
static int
xml_doctype_literal(
	struct xml_parser *parser,
	int public_id,
	struct xml_units *value)
{
	const uint16_t *input;
	size_t length;
	size_t start;
	uint16_t quote;
	uint16_t unit;
	int legal;

	input = parser->document->input.data;
	length = parser->document->input.length;
	if (parser->offset >= length)
		return EINVAL;
	quote = input[parser->offset];
	if (quote != '\'' && quote != '"')
		return EINVAL;
	parser->offset++;
	start = parser->offset;
	while (parser->offset < length) {
		unit = input[parser->offset++];
		if (unit == quote) {
			if (value != NULL) {
				value->data = input + start;
				value->length = parser->offset - start - 1U;
			}
			return 0;
		}
		if (!public_id)
			continue;

		/* PubidChar is a finite ASCII alphabet; a system literal admits any decoded XML character. */
		legal = (unit >= 'A' && unit <= 'Z') ||
		    (unit >= 'a' && unit <= 'z') ||
		    (unit >= '0' && unit <= '9') ||
		    unit == ' ' || unit == '\r' || unit == '\n' || unit == '-' ||
		    unit == '\'' || unit == '(' || unit == ')' || unit == '+' ||
		    unit == ',' || unit == '.' || unit == '/' || unit == ':' ||
		    unit == '=' || unit == '?' || unit == ';' || unit == '!' ||
		    unit == '*' || unit == '#' || unit == '@' || unit == '$' ||
		    unit == '_' || unit == '%';
		if (!legal)
			return EINVAL;
	}
	return EINVAL;
}

/* Accepts only a declaration without an internal subset; external IDs have no resource effects. */
static int
xml_doctype(
	struct xml_parser *parser)
{
	struct xml_units name;
	struct xml_units prefix;
	struct xml_units local;
	struct xml_units public_name;
	const uint16_t *input;
	size_t length;
	int public_id;
	int status;

	if (parser->open->type != XML_DOCUMENT || parser->root_seen || parser->doctype_seen)
		return EINVAL;
	input = parser->document->input.data;
	length = parser->document->input.length;
	public_name.data = NULL;
	public_name.length = 0;
	public_id = 0;
	parser->offset += 9U;
	if (parser->offset >= length || !xml_space(input[parser->offset]))
		return EINVAL;
	xml_skip_space(parser);
	status = xml_name(parser, &name, &prefix, &local);
	if (status != 0)
		return status;
	if (parser->offset >= length)
		return EINVAL;
	if (xml_space(input[parser->offset])) {
		xml_skip_space(parser);
		if (xml_at(parser, "SYSTEM") || xml_at(parser, "PUBLIC")) {
			public_id = xml_at(parser, "PUBLIC");
			parser->offset += 6U;
			if (parser->offset >= length || !xml_space(input[parser->offset]))
				return EINVAL;
			xml_skip_space(parser);
			status = xml_doctype_literal(parser, public_id, &public_name);
			if (status != 0)
				return status;
			if (public_id) {
				if (parser->offset >= length || !xml_space(input[parser->offset]))
					return EINVAL;
				xml_skip_space(parser);
				status = xml_doctype_literal(parser, 0, NULL);
				if (status != 0)
					return status;
			}
			xml_skip_space(parser);
		}
	}
	if (parser->offset >= length)
		return EINVAL;
	if (input[parser->offset] == '[')
		return ENOTSUP;
	if (input[parser->offset] != '>')
		return EINVAL;
	parser->offset++;
	parser->doctype = name;
	parser->doctype_seen = 1;
	parser->xhtml_nbsp = public_id && xml_ascii(name, "html", 0) &&
	    xml_ascii(public_name, "-//W3C//DTD XHTML 1.0 Strict//EN", 0);
	return 0;
}

/* Dispatches strict actual XML markup without HTML recovery, script execution or external resource effects. */
static int
xml_parse_tree(
	struct xml_parser *parser)
{
	int matched;
	int status;

	/* Every successful step advances the bounded actual normalized source cursor. */
	while (parser->offset < parser->document->input.length) {
		if (parser->document->input.data[parser->offset] != '<') {
			status = xml_text(parser);
			if (status != 0)
				return status;
			continue;
		}

		/* Comments and CDATA retain separate real record types. */
		matched = xml_at(parser, "<!--");
		if (matched) {
			parser->offset += 4U;
			status = xml_delimited(parser, XML_COMMENT, "-->");
			if (status != 0)
				return status;
			continue;
		}

		/* CDATA delimiters preserve literal character content without reference expansion. */
		matched = xml_at(parser, "<![CDATA[");
		if (matched) {
			parser->offset += 9U;
			status = xml_delimited(parser, XML_CDATA, "]]>");
			if (status != 0)
				return status;
			continue;
		}

		/* Processing targets remain actual native XML data unless the initial XML declaration applies. */
		matched = xml_at(parser, "<?");
		if (matched) {
			status = xml_instruction(parser);
			if (status != 0)
				return status;
			continue;
		}

		/* End tags close only their exact actual qualified owner names. */
		matched = xml_at(parser, "</");
		if (matched) {
			status = xml_end(parser);
			if (status != 0)
				return status;
			continue;
		}

		/* Only a well-formed external identifier is admitted; DTD declarations remain unavailable. */
		matched = xml_at(parser, "<!DOCTYPE");
		if (matched) {
			status = xml_doctype(parser);
			if (status != 0)
				return status;
			continue;
		}

		/* Other declarations are not accepted as ordinary XML start tags. */
		matched = xml_at(parser, "<!");
		if (matched)
			return EINVAL;
		status = xml_start(parser);
		if (status != 0)
			return status;
	}

	/* EOF can finish only one fully closed document element. */
	if (!parser->root_seen || parser->open->type != XML_DOCUMENT)
		return EINVAL;

	/* Succeeded: all real tree output is complete and namespace-aware before publication. */
	return 0;
}
