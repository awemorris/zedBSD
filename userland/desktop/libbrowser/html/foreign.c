/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Foreign content: SVG and MathML inside HTML.  The dispatcher's choice
 * between the insertion mode and the foreign rules, the rules themselves,
 * and the adjustments that give SVG names their mixed case and namespaced
 * attributes their namespaces.
 */

#include "html/parser.h"

#include <string.h>

/*
 * A name the tokenizer folded to lower case and the name it stands for.
 */
struct foreign_rename {
	const char *lower;
	const char *name;
};

/*
 * An attribute that belongs to a namespace in foreign content.
 */
struct foreign_attribute {
	const char *name;
	const char *prefix;
	const char *local;
	int ns;
};

static int foreign_breaks_out(const struct tb_token *token);
static void foreign_any_other_end_tag(struct html_parser *p, const struct tb_token *token);
static int foreign_name_matches(const struct dom_element *element, const struct html_token *raw);
static const char *foreign_rename(const struct foreign_rename *table, const struct wb_units *name);
static int foreign_add_attributes(struct html_parser *p, struct dom_element *element, const struct tb_token *token, int ns);

/* The SVG element names with capitals. */
static const struct foreign_rename foreign_svg_elements[] = {
	{ "altglyph", "altGlyph" },
	{ "altglyphdef", "altGlyphDef" },
	{ "altglyphitem", "altGlyphItem" },
	{ "animatecolor", "animateColor" },
	{ "animatemotion", "animateMotion" },
	{ "animatetransform", "animateTransform" },
	{ "clippath", "clipPath" },
	{ "feblend", "feBlend" },
	{ "fecolormatrix", "feColorMatrix" },
	{ "fecomponenttransfer", "feComponentTransfer" },
	{ "fecomposite", "feComposite" },
	{ "feconvolvematrix", "feConvolveMatrix" },
	{ "fediffuselighting", "feDiffuseLighting" },
	{ "fedisplacementmap", "feDisplacementMap" },
	{ "fedistantlight", "feDistantLight" },
	{ "fedropshadow", "feDropShadow" },
	{ "feflood", "feFlood" },
	{ "fefunca", "feFuncA" },
	{ "fefuncb", "feFuncB" },
	{ "fefuncg", "feFuncG" },
	{ "fefuncr", "feFuncR" },
	{ "fegaussianblur", "feGaussianBlur" },
	{ "feimage", "feImage" },
	{ "femerge", "feMerge" },
	{ "femergenode", "feMergeNode" },
	{ "femorphology", "feMorphology" },
	{ "feoffset", "feOffset" },
	{ "fepointlight", "fePointLight" },
	{ "fespecularlighting", "feSpecularLighting" },
	{ "fespotlight", "feSpotLight" },
	{ "fetile", "feTile" },
	{ "feturbulence", "feTurbulence" },
	{ "foreignobject", "foreignObject" },
	{ "glyphref", "glyphRef" },
	{ "lineargradient", "linearGradient" },
	{ "radialgradient", "radialGradient" },
	{ "textpath", "textPath" },
	{ NULL, NULL }
};

/* The SVG attribute names with capitals. */
static const struct foreign_rename foreign_svg_attributes[] = {
	{ "attributename", "attributeName" },
	{ "attributetype", "attributeType" },
	{ "basefrequency", "baseFrequency" },
	{ "baseprofile", "baseProfile" },
	{ "calcmode", "calcMode" },
	{ "clippathunits", "clipPathUnits" },
	{ "diffuseconstant", "diffuseConstant" },
	{ "edgemode", "edgeMode" },
	{ "filterunits", "filterUnits" },
	{ "glyphref", "glyphRef" },
	{ "gradienttransform", "gradientTransform" },
	{ "gradientunits", "gradientUnits" },
	{ "kernelmatrix", "kernelMatrix" },
	{ "kernelunitlength", "kernelUnitLength" },
	{ "keypoints", "keyPoints" },
	{ "keysplines", "keySplines" },
	{ "keytimes", "keyTimes" },
	{ "lengthadjust", "lengthAdjust" },
	{ "limitingconeangle", "limitingConeAngle" },
	{ "markerheight", "markerHeight" },
	{ "markerunits", "markerUnits" },
	{ "markerwidth", "markerWidth" },
	{ "maskcontentunits", "maskContentUnits" },
	{ "maskunits", "maskUnits" },
	{ "numoctaves", "numOctaves" },
	{ "pathlength", "pathLength" },
	{ "patterncontentunits", "patternContentUnits" },
	{ "patterntransform", "patternTransform" },
	{ "patternunits", "patternUnits" },
	{ "pointsatx", "pointsAtX" },
	{ "pointsaty", "pointsAtY" },
	{ "pointsatz", "pointsAtZ" },
	{ "preservealpha", "preserveAlpha" },
	{ "preserveaspectratio", "preserveAspectRatio" },
	{ "primitiveunits", "primitiveUnits" },
	{ "refx", "refX" },
	{ "refy", "refY" },
	{ "repeatcount", "repeatCount" },
	{ "repeatdur", "repeatDur" },
	{ "requiredextensions", "requiredExtensions" },
	{ "requiredfeatures", "requiredFeatures" },
	{ "specularconstant", "specularConstant" },
	{ "specularexponent", "specularExponent" },
	{ "spreadmethod", "spreadMethod" },
	{ "startoffset", "startOffset" },
	{ "stddeviation", "stdDeviation" },
	{ "stitchtiles", "stitchTiles" },
	{ "surfacescale", "surfaceScale" },
	{ "systemlanguage", "systemLanguage" },
	{ "tablevalues", "tableValues" },
	{ "targetx", "targetX" },
	{ "targety", "targetY" },
	{ "textlength", "textLength" },
	{ "viewbox", "viewBox" },
	{ "viewtarget", "viewTarget" },
	{ "xchannelselector", "xChannelSelector" },
	{ "ychannelselector", "yChannelSelector" },
	{ "zoomandpan", "zoomAndPan" },
	{ NULL, NULL }
};

/* The MathML attribute names with capitals. */
static const struct foreign_rename foreign_mathml_attributes[] = {
	{ "definitionurl", "definitionURL" },
	{ NULL, NULL }
};

/* The attributes of foreign content that belong to a namespace. */
static const struct foreign_attribute foreign_namespaced[] = {
	{ "xlink:actuate", "xlink", "actuate", DOM_NS_XLINK },
	{ "xlink:arcrole", "xlink", "arcrole", DOM_NS_XLINK },
	{ "xlink:href", "xlink", "href", DOM_NS_XLINK },
	{ "xlink:role", "xlink", "role", DOM_NS_XLINK },
	{ "xlink:show", "xlink", "show", DOM_NS_XLINK },
	{ "xlink:title", "xlink", "title", DOM_NS_XLINK },
	{ "xlink:type", "xlink", "type", DOM_NS_XLINK },
	{ "xml:lang", "xml", "lang", DOM_NS_XML },
	{ "xml:space", "xml", "space", DOM_NS_XML },
	{ "xmlns", NULL, "xmlns", DOM_NS_XMLNS },
	{ "xmlns:xlink", "xmlns", "xlink", DOM_NS_XMLNS },
	{ NULL, NULL, NULL, 0 }
};

/*
 * Tells whether the tree builder handles a token by the rules for foreign
 * content rather than by the insertion mode.
 */
int
tb_use_foreign_rules(
	const struct html_parser *p,
	const struct tb_token *token)
{
	struct dom_element *adjusted;
	int text_point;
	int html_point;

	/* An empty stack, an HTML adjusted current node and end of file use the mode. */
	adjusted = tb_adjusted_current(p);
	if (adjusted == NULL || adjusted->ns == DOM_NS_HTML)
		return 0;
	if (token->type == HTML_TOKEN_EOF)
		return 0;

	/* A MathML text integration point takes text and most start tags as HTML. */
	text_point = tb_is_mathml_text_integration_point(adjusted);
	if (text_point) {
		if (token->type == HTML_TOKEN_CHARACTERS)
			return 0;
		if (token->type == HTML_TOKEN_START_TAG &&
		    token->tag != DOM_TAG_MGLYPH &&
		    token->tag != DOM_TAG_MALIGNMARK)
			return 0;
	}

	/* annotation-xml takes an <svg> as HTML would. */
	if (adjusted->ns == DOM_NS_MATHML && adjusted->tag == DOM_TAG_ANNOTATION_XML &&
	    token->type == HTML_TOKEN_START_TAG && token->tag == DOM_TAG_SVG)
		return 0;

	/* An HTML integration point takes text and start tags as HTML. */
	html_point = tb_is_html_integration_point(adjusted);
	if (html_point) {
		if (token->type == HTML_TOKEN_CHARACTERS || token->type == HTML_TOKEN_START_TAG)
			return 0;
	}

	/* Everything else is foreign content. */
	return 1;
}

/*
 * Processes a token by the rules for parsing tokens in foreign content.
 */
void
tb_foreign_content(
	struct html_parser *p,
	const struct tb_token *token)
{
	struct dom_element *adjusted;
	struct dom_element *current;
	static const uint16_t replacement[1] = { 0xfffdU };
	size_t index;
	int breaks;
	int text_point;
	int html_point;

	/* Characters: NULs become U+FFFD, other text rules out a frameset. */
	if (token->type == HTML_TOKEN_CHARACTERS) {
		if (token->tag == TB_TEXT_NULL) {
			tb_error(p);
			for (index = 0; index < token->length; index++)
				tb_insert_characters(p, replacement, 1);
			return;
		}

		/* Other text is inserted as it is. */
		tb_insert_characters(p, token->text, token->length);
		if (token->tag == TB_TEXT_OTHER)
			p->frameset_ok = 0;
		return;
	}

	/* Comments are inserted; a DOCTYPE is an error. */
	if (token->type == HTML_TOKEN_COMMENT) {
		tb_insert_comment(p, token, NULL);
		return;
	}

	/* A DOCTYPE is an error. */
	if (token->type == HTML_TOKEN_DOCTYPE) {
		tb_error(p);
		return;
	}

	/* An HTML tag that breaks out of foreign content closes it and is reprocessed. */
	breaks = foreign_breaks_out(token);
	if (breaks) {
		tb_error(p);
		for (;;) {
			current = tb_current(p);
			if (current == NULL || current->ns == DOM_NS_HTML)
				break;
			text_point = tb_is_mathml_text_integration_point(current);
			if (text_point)
				break;
			html_point = tb_is_html_integration_point(current);
			if (html_point)
				break;

			/* The foreign element is closed. */
			tb_pop(p);
		}

		/* The tag is handled by the insertion mode. */
		tb_process_in_mode(p, p->mode, token);
		return;
	}

	/* Any other start tag is a foreign element of the adjusted current node's namespace. */
	if (token->type == HTML_TOKEN_START_TAG) {
		adjusted = tb_adjusted_current(p);
		tb_insert_foreign(p, token, adjusted->ns);
		return;
	}

	/* </script> in SVG closes the script. */
	current = tb_current(p);
	if (token->type == HTML_TOKEN_END_TAG && token->tag == DOM_TAG_SCRIPT &&
	    current != NULL && current->ns == DOM_NS_SVG && current->tag == DOM_TAG_SCRIPT) {
		tb_pop(p);
		return;
	}

	/* Any other end tag. */
	if (token->type == HTML_TOKEN_END_TAG)
		foreign_any_other_end_tag(p, token);
}

/*
 * Inserts a foreign element for a start tag: adjusts the name and the
 * attributes for its namespace, and pops it at once when self-closing.
 */
void
tb_insert_foreign(
	struct html_parser *p,
	const struct tb_token *token,
	int ns)
{
	struct dom_element *element;
	struct vm_string *local_name;
	const char *renamed;

	/* SVG element names get their capitals back. */
	renamed = NULL;
	if (ns == DOM_NS_SVG)
		renamed = foreign_rename(foreign_svg_elements, &token->raw->name);
	if (renamed != NULL) {
		local_name = vm_atom_from_ascii(p->heap, renamed);
	} else {
		local_name = vm_atom_from_units(p->heap, token->raw->name.data, token->raw->name.length);
	}

	/* An atom that could not be made stops the parse. */
	if (local_name == NULL) {
		p->failed = 1;
		return;
	}

	/* Makes the element with its adjusted attributes. */
	element = dom_element_create(p->document, ns, local_name, NULL);
	if (element == NULL) {
		p->failed = 1;
		return;
	}
	if (p->context == NULL)
		element->node.flags |= DOM_NODE_PARSER_CREATED;

	/* Gives it the adjusted attributes. */
	foreign_add_attributes(p, element, token, ns);

	/* Inserts it; a self-closing tag is closed at once. */
	tb_insert_node(p, &element->node, NULL);
	tb_push(p, element);
	if (token->raw->self_closing)
		tb_pop(p);
}

/*
 * Tells whether an element is an HTML integration point: SVG foreignObject,
 * desc and title, and MathML annotation-xml with an HTML encoding.
 */
int
tb_is_html_integration_point(
	const struct dom_element *element)
{
	const struct dom_attribute *encoding;
	uint16_t unit;
	size_t index;
	int html;
	int xhtml;
	int same;

	/* The SVG elements that hold HTML. */
	if (element->ns == DOM_NS_SVG) {
		if (element->tag == DOM_TAG_FOREIGNOBJECT || element->tag == DOM_TAG_DESC || element->tag == DOM_TAG_TITLE)
			return 1;
		return 0;
	}

	/* annotation-xml with encoding text/html or application/xhtml+xml. */
	if (element->ns != DOM_NS_MATHML || element->tag != DOM_TAG_ANNOTATION_XML)
		return 0;
	encoding = NULL;
	for (index = 0; index < element->attribute_count; index++) {
		/* Only the encoding attribute without a namespace counts. */
		if (element->attributes[index].ns != DOM_NS_NONE)
			continue;
		same = vm_string_equal_ascii(element->attributes[index].name, "encoding");
		if (same)
			encoding = &element->attributes[index];
	}

	/* Without an encoding it is not an integration point. */
	if (encoding == NULL)
		return 0;

	/* Compares the value with the two encodings, ignoring ASCII case. */
	html = 1;
	xhtml = 1;
	if (encoding->value->length != 9)
		html = 0;
	if (encoding->value->length != 21)
		xhtml = 0;
	for (index = 0; index < encoding->value->length; index++) {
		unit = vm_string_at(encoding->value, index);
		if (unit >= 'A' && unit <= 'Z')
			unit = (uint16_t)(unit + 0x20U);
		if (html && unit != (unsigned char)"text/html"[index])
			html = 0;
		if (xhtml && unit != (unsigned char)"application/xhtml+xml"[index])
			xhtml = 0;
	}

	/* Either encoding makes the element an integration point. */
	if (html || xhtml)
		return 1;

	/* Another encoding is not an integration point. */
	return 0;
}

/*
 * Tells whether an element is a MathML text integration point: mi, mo, mn,
 * ms and mtext.
 */
int
tb_is_mathml_text_integration_point(
	const struct dom_element *element)
{
	/* Only MathML elements are. */
	if (element->ns != DOM_NS_MATHML)
		return 0;

	/* The five text elements. */
	switch (element->tag) {
	case DOM_TAG_MI:
	case DOM_TAG_MO:
	case DOM_TAG_MN:
	case DOM_TAG_MS:
	case DOM_TAG_MTEXT:
		return 1;
	default:
		return 0;
	}
}

/* Tells whether a token is one of the HTML tags that break out of foreign content. */
static int
foreign_breaks_out(
	const struct tb_token *token)
{
	const struct html_token_attribute *attribute;

	/* </br> and </p> break out. */
	if (token->type == HTML_TOKEN_END_TAG)
		return token->tag == DOM_TAG_BR || token->tag == DOM_TAG_P;
	if (token->type != HTML_TOKEN_START_TAG)
		return 0;

	/* <font> breaks out only with color, face or size. */
	if (token->tag == DOM_TAG_FONT) {
		attribute = tb_token_attribute(token, "color");
		if (attribute != NULL)
			return 1;
		attribute = tb_token_attribute(token, "face");
		if (attribute != NULL)
			return 1;
		attribute = tb_token_attribute(token, "size");
		if (attribute != NULL)
			return 1;
		return 0;
	}

	/* The standard's list of start tags. */
	switch (token->tag) {
	case DOM_TAG_B:
	case DOM_TAG_BIG:
	case DOM_TAG_BLOCKQUOTE:
	case DOM_TAG_BODY:
	case DOM_TAG_BR:
	case DOM_TAG_CENTER:
	case DOM_TAG_CODE:
	case DOM_TAG_DD:
	case DOM_TAG_DIV:
	case DOM_TAG_DL:
	case DOM_TAG_DT:
	case DOM_TAG_EM:
	case DOM_TAG_EMBED:
	case DOM_TAG_H1:
	case DOM_TAG_H2:
	case DOM_TAG_H3:
	case DOM_TAG_H4:
	case DOM_TAG_H5:
	case DOM_TAG_H6:
	case DOM_TAG_HEAD:
	case DOM_TAG_HR:
	case DOM_TAG_I:
	case DOM_TAG_IMG:
	case DOM_TAG_LI:
	case DOM_TAG_LISTING:
	case DOM_TAG_MENU:
	case DOM_TAG_META:
	case DOM_TAG_NOBR:
	case DOM_TAG_OL:
	case DOM_TAG_P:
	case DOM_TAG_PRE:
	case DOM_TAG_RUBY:
	case DOM_TAG_S:
	case DOM_TAG_SMALL:
	case DOM_TAG_SPAN:
	case DOM_TAG_STRONG:
	case DOM_TAG_STRIKE:
	case DOM_TAG_SUB:
	case DOM_TAG_SUP:
	case DOM_TAG_TABLE:
	case DOM_TAG_TT:
	case DOM_TAG_U:
	case DOM_TAG_UL:
	case DOM_TAG_VAR:
		return 1;
	default:
		return 0;
	}
}

/* The foreign content's "any other end tag": closes the matching foreign element, or defers to the mode. */
static void
foreign_any_other_end_tag(
	struct html_parser *p,
	const struct tb_token *token)
{
	struct dom_element *node;
	size_t index;
	int matches;

	/* The current node should be the one the tag closes. */
	index = tb_open_count(p);
	if (index == 0)
		return;
	index--;
	node = tb_open_at(p, index);
	matches = foreign_name_matches(node, token->raw);
	if (!matches)
		tb_error(p);

	/* Walks up until the matching element, or an HTML element that takes the tag by the mode. */
	for (;;) {
		/* The root is never closed this way. */
		if (index == 0)
			return;

		/* The matching element is closed with what is below it. */
		matches = foreign_name_matches(node, token->raw);
		if (matches) {
			tb_pop_until_element(p, node);
			return;
		}

		/* An HTML element above hands the tag to the insertion mode. */
		index--;
		node = tb_open_at(p, index);
		if (node->ns == DOM_NS_HTML) {
			tb_process_in_mode(p, p->mode, token);
			return;
		}
	}
}

/* Tells whether an element's name, folded to lower case, is the end tag's name. */
static int
foreign_name_matches(
	const struct dom_element *element,
	const struct html_token *raw)
{
	uint16_t unit;
	size_t index;

	/* The lengths must agree. */
	if (element->local_name->length != raw->name.length)
		return 0;

	/* Compares unit by unit, folding the element's capitals. */
	for (index = 0; index < raw->name.length; index++) {
		unit = vm_string_at(element->local_name, index);
		if (unit >= 'A' && unit <= 'Z')
			unit = (uint16_t)(unit + 0x20U);
		if (unit != raw->name.data[index])
			return 0;
	}

	/* The names match. */
	return 1;
}

/* Finds the mixed-case name for a lower case one in a table, or NULL. */
static const char *
foreign_rename(
	const struct foreign_rename *table,
	const struct wb_units *name)
{
	size_t index;
	int same;

	/* Compares with each entry. */
	for (index = 0; table[index].lower != NULL; index++) {
		same = tb_units_equal_ascii(name->data, name->length, table[index].lower, 0);
		if (same)
			return table[index].name;
	}

	/* The name keeps its case. */
	return NULL;
}

/* Adds a foreign start tag's attributes to its element, adjusted for the namespace. */
static int
foreign_add_attributes(
	struct html_parser *p,
	struct dom_element *element,
	const struct tb_token *token,
	int ns)
{
	const struct html_token_attribute *attribute;
	const struct foreign_attribute *special;
	struct dom_attribute *existing;
	const char *renamed;
	struct vm_string *name;
	struct vm_string *prefix;
	struct vm_string *value;
	size_t index;
	size_t entry;
	int attribute_ns;
	int same;
	int error;

	/* Adds each attribute the tokenizer kept. */
	for (index = 0; index < token->raw->attribute_count; index++) {
		attribute = &token->raw->attributes[index];
		if (attribute->dropped)
			continue;

		/* The namespaced attributes of foreign content. */
		special = NULL;
		for (entry = 0; foreign_namespaced[entry].name != NULL; entry++) {
			same = tb_units_equal_ascii(attribute->name.data, attribute->name.length, foreign_namespaced[entry].name, 0);
			if (same) {
				special = &foreign_namespaced[entry];
				break;
			}
		}

		/* Names the attribute: namespaced, renamed for SVG or MathML, or as it is. */
		prefix = NULL;
		attribute_ns = DOM_NS_NONE;
		if (special != NULL) {
			name = vm_atom_from_ascii(p->heap, special->local);
			if (special->prefix != NULL)
				prefix = vm_atom_from_ascii(p->heap, special->prefix);
			attribute_ns = special->ns;
		} else {
			renamed = NULL;
			if (ns == DOM_NS_SVG)
				renamed = foreign_rename(foreign_svg_attributes, &attribute->name);
			if (ns == DOM_NS_MATHML)
				renamed = foreign_rename(foreign_mathml_attributes, &attribute->name);
			if (renamed != NULL) {
				name = vm_atom_from_ascii(p->heap, renamed);
			} else {
				name = vm_atom_from_units(p->heap, attribute->name.data, attribute->name.length);
			}
		}

		/* Makes the value. */
		value = vm_string_from_units(p->heap, attribute->value.data, attribute->value.length);
		if (name == NULL || value == NULL) {
			p->failed = 1;
			return 1;
		}

		/* Adds it unless the element has it already. */
		existing = dom_element_find_attribute(element, attribute_ns, name);
		if (existing != NULL)
			continue;
		error = dom_element_add_attribute(element, attribute_ns, prefix, name, value);
		if (error != 0) {
			p->failed = 1;
			return 1;
		}
	}

	/* Succeeded: the attributes are on the element. */
	return 0;
}
