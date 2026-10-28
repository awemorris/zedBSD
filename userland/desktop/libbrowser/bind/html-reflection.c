/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Reflects scoped button, label, meta and object attributes on actual interfaces. */

#include "bind/internal.h"
#include "net/net.h"

#include <errno.h>
#include <string.h>

/* Native function names must be declared before registry initializers use them. */
static int button_type_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int button_type_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int button_value_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int button_value_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int button_form_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int label_for_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int label_for_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int meta_http_equiv_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int meta_http_equiv_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int object_data_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int object_data_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);

/* Button type and value are content-attribute views, not activation state. */
static const struct bind_attribute button_attributes[] = {
	{ "type", button_type_get, button_type_set },
	{ "value", button_value_get, button_value_set },
	{ "form", button_form_get, NULL },
	{ NULL, NULL, NULL }
};

/* A label's alias names its existing for content attribute. */
static const struct bind_attribute label_attributes[] = {
	{ "htmlFor", label_for_get, label_for_set },
	{ NULL, NULL, NULL }
};

/* The metadata alias preserves the raw http-equiv attribute text. */
static const struct bind_attribute meta_attributes[] = {
	{ "httpEquiv", meta_http_equiv_get, meta_http_equiv_set },
	{ NULL, NULL, NULL }
};

/* An object data getter serializes a URL while its setter preserves scalar attribute text. */
static const struct bind_attribute object_attributes[] = {
	{ "data", object_data_get, object_data_set },
	{ "contentDocument", bind_object_document, NULL },
	{ NULL, NULL, NULL }
};

/* Object SVG access reports only an actual loaded SVG Document. */
static const struct bind_operation object_operations[] = {
	{ "getSVGDocument", 0, bind_object_svg_document },
	{ NULL, 0, NULL }
};

/* Objects retain the normal HTML graph without implicitly creating a loaded document. */
const struct bind_interface bind_html_object_element_interface = {
	"HTMLObjectElement", BIND_HTML_ELEMENT, 0, NULL, object_attributes, object_operations, NULL
};

/* HTML buttons inherit ordinary HTML styling, events and node ownership. */
const struct bind_interface bind_html_button_element_interface = {
	"HTMLButtonElement", BIND_HTML_ELEMENT, 0, NULL, button_attributes, NULL, NULL
};

/* HTML labels inherit the ordinary HTMLElement graph. */
const struct bind_interface bind_html_label_element_interface = {
	"HTMLLabelElement", BIND_HTML_ELEMENT, 0, NULL, label_attributes, NULL, NULL
};

/* HTML metadata has no implicit network or parsing side effect in this binding. */
const struct bind_interface bind_html_meta_element_interface = {
	"HTMLMetaElement", BIND_HTML_ELEMENT, 0, NULL, meta_attributes, NULL, NULL
};

static void reflection_url_release(struct net_url *base, struct net_url *parsed, struct wb_buffer *raw, struct wb_buffer *serialized);
static int reflection_scalar(struct vm_realm *realm, struct vm_string *text, struct vm_string **scalar);
static int reflection_this(struct vm_realm *realm, vm_value receiver, int tag, struct dom_element **element);
static int reflection_get(struct vm_realm *realm, vm_value receiver, int tag, const char *name, vm_value *result);
static int reflection_set(struct vm_realm *realm, vm_value receiver, int tag, const char *name, const vm_value *args, unsigned count);
static int reflection_equal_folded(const struct vm_string *text, const char *keyword);

/* HTMLLinkElement's reflected attributes (a script that builds a link sets href and rel before inserting it). */
#define LINK_STRING(function, attribute) \
	static int link_##function##_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result) \
	{ \
		UNUSED_PARAMETER(args); \
		UNUSED_PARAMETER(count); \
		return reflection_get(realm, this_value, DOM_TAG_LINK, attribute, result); \
	} \
	static int link_##function##_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result) \
	{ \
		int error; \
		error = reflection_set(realm, this_value, DOM_TAG_LINK, attribute, args, count); \
		if (error == 0) \
			*result = VM_VALUE_UNDEFINED; \
		return error; \
	}

LINK_STRING(rel, "rel")
LINK_STRING(media, "media")
LINK_STRING(type, "type")
LINK_STRING(as, "as")
LINK_STRING(cross_origin, "crossorigin")
LINK_STRING(integrity, "integrity")
LINK_STRING(hreflang, "hreflang")
LINK_STRING(sizes, "sizes")

static int
link_href_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result)
{
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);
	return bind_reflect_url_get(realm, this_value, DOM_TAG_LINK, "href", result);
}

static int
link_href_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result)
{
	return bind_reflect_url_set(realm, this_value, DOM_TAG_LINK, "href", args, count, result);
}

static const struct bind_attribute link_attributes[] = {
	{ "href", link_href_get, link_href_set },
	{ "rel", link_rel_get, link_rel_set },
	{ "media", link_media_get, link_media_set },
	{ "type", link_type_get, link_type_set },
	{ "as", link_as_get, link_as_set },
	{ "crossOrigin", link_cross_origin_get, link_cross_origin_set },
	{ "integrity", link_integrity_get, link_integrity_set },
	{ "hreflang", link_hreflang_get, link_hreflang_set },
	{ "sizes", link_sizes_get, link_sizes_set },
	{ NULL, NULL, NULL }
};

/* HTML links inherit the ordinary HTMLElement graph. */
const struct bind_interface bind_html_link_element_interface = {
	"HTMLLinkElement", BIND_HTML_ELEMENT, 0, NULL, link_attributes, NULL, NULL
};


/*
 * Resolves a branded URL attribute against its actual owning Document.
 */
int
bind_reflect_url_get(
	struct vm_realm *realm,
	vm_value this_value,
	int tag,
	const char *name,
	vm_value *result)
{
	struct dom_element *element;
	struct vm_string *attribute;
	struct vm_string *text;
	struct net_url base;
	struct net_url parsed;
	struct wb_buffer raw;
	struct wb_buffer serialized;
	int error;

	/* Rejects forged or foreign-tag receivers before reading any attribute. */
	error = reflection_this(realm, this_value, tag, &element);
	if (error != 0)
		return error;

	/* Absence is distinct from a present empty URL, which resolves against the base. */
	attribute = dom_attribute_ascii(element, name);
	if (attribute == NULL) {
		error = bind_string(realm, "", result);
		if (error != 0)
			return error;

		/* Succeeded: no attribute means no URL to resolve. */
		return 0;
	}

	/* Initializes every owned C record before any fallible URL or text operation. */
	memset(&base, 0, sizeof(base));
	memset(&parsed, 0, sizeof(parsed));
	wb_buffer_init(&raw);
	wb_buffer_init(&serialized);

	/* UTF-8 conversion replaces lone surrogates for the URL and its failure fallback. */
	error = vm_string_to_utf8(attribute, &raw);
	if (error != 0) {
		reflection_url_release(&base, &parsed, &raw, &serialized);
		return error;
	}

	/* The node's document supplies the base even through a foreign-realm getter. */
	error = bind_reflection_base(element->node.document, &base);
	if (error != 0) {
		reflection_url_release(&base, &parsed, &raw, &serialized);
		return error;
	}

	/* Ordinary URL failure returns the scalar attribute; allocation failure remains an error. */
	error = net_url_parse(wb_buffer_string(&raw), raw.length, &base, &parsed);
	if (error == EINVAL) {
		error = 0;
		text = vm_string_from_utf8(realm->heap, wb_buffer_string(&raw), raw.length);
		if (text == NULL) {
			error = ENOMEM;
			reflection_url_release(&base, &parsed, &raw, &serialized);
			return error;
		}

		/* The unparseable spelling is still reflected as scalar text. */
		*result = vm_value_cell(text);
	} else {
		/* A non-syntax parser failure cannot be mistaken for the raw-URL fallback. */
		if (error != 0) {
			reflection_url_release(&base, &parsed, &raw, &serialized);
			return error;
		}

		/* Serializes the parsed record, retaining query and fragment. */
		error = net_url_serialize(&parsed, 0, &serialized);
		if (error != 0) {
			reflection_url_release(&base, &parsed, &raw, &serialized);
			return error;
		}

		/* The script string copies bytes before the temporary URL buffers are released. */
		text = vm_string_from_utf8(realm->heap, wb_buffer_string(&serialized), serialized.length);
		if (text == NULL) {
			error = ENOMEM;
			reflection_url_release(&base, &parsed, &raw, &serialized);
			return error;
		}

		/* Publishes the fully serialized URL without rewriting the content attribute. */
		*result = vm_value_cell(text);
	}

	/* Releases the temporary URL/text records after the script string copied its bytes. */
	reflection_url_release(&base, &parsed, &raw, &serialized);

	/* Succeeded: the caller sees the resolved URL or its scalar syntax-failure fallback. */
	return 0;
}

/*
 * Reflects a branded URL attribute after one scalar string conversion.
 */
int
bind_reflect_url_set(
	struct vm_realm *realm,
	vm_value this_value,
	int tag,
	const char *attribute,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct vm_string *text;
	struct vm_string *scalar;
	struct vm_string *name;
	struct vm_cell *roots[3];
	unsigned index;
	unsigned registered;
	int error;

	/* Brands the actual node before a potentially reentrant argument conversion. */
	*result = VM_VALUE_UNDEFINED;
	error = reflection_this(realm, this_value, tag, &element);
	if (error != 0)
		return error;

	/* Evaluates the user conversion exactly once and preserves its thrown value. */
	error = bind_to_string(realm, js_argument(args, count, 0), &text);
	if (error != 0)
		return error;

	/* Retain the converted text, scalar result and attribute atom until publication. */
	roots[0] = &text->cell;
	roots[1] = NULL;
	roots[2] = NULL;
	registered = 0;
	for (index = 0; index < 3U; index++) {
		error = vm_heap_add_root(realm->heap, &roots[index]);
		if (error != 0)
			goto cleanup;

		/* Cleanup owns only successfully registered slots. */
		registered++;
	}

	/* WebIDL USVString repairs lone surrogates before publishing attribute text. */
	error = reflection_scalar(realm, text, &scalar);
	if (error != 0)
		goto cleanup;
	roots[1] = &scalar->cell;

	/* Allocates the known attribute name before changing the real DOM. */
	name = vm_atom_from_ascii(realm->heap, attribute);
	if (name == NULL) {
		error = ENOMEM;
		goto cleanup;
	}

	/* The known name stays live until the actual element owns the attribute. */
	roots[2] = &name->cell;

	/* The ordinary DOM mutation path retains parser, clone and wrapper identity. */
	error = dom_element_set_attribute(element, name, scalar);

cleanup:
	/* No temporary root slot survives either DOM publication or failure. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* A failed conversion or mutation cannot report a completed attribute. */
	if (error != 0)
		return error;

	/* Succeeded: data holds scalar text rather than an expando or serialized rewrite. */
	return 0;
}

/*
 * Finds the actual document fallback base and first connected HTML base.
 */
int
bind_reflection_base(
	struct dom_document *document,
	struct net_url *base)
{
	struct dom_node *walk;
	struct dom_element *element;
	struct vm_string *attribute;
	struct net_url candidate;
	struct wb_buffer text;
	int forbidden;
	int javascript;
	int data;
	int error;

	/* Documents without a location host retain their existing about:blank URL. */
	wb_buffer_init(&text);
	error = bind_document_url(document, 1, &text);
	if (error != 0) {
		wb_buffer_release(&text);
		return error;
	}

	/* An absent host URL is about:blank, not the caller's location. */
	if (text.length == 0) {
		error = wb_buffer_append_string(&text, "about:blank");
		if (error != 0) {
			wb_buffer_release(&text);
			return error;
		}
	}

	/* Parses the actual host URL before considering any base element. */
	error = net_url_parse(wb_buffer_string(&text), text.length, NULL, base);
	wb_buffer_release(&text);
	if (error != 0)
		return error;

	/* Only the first HTML base carrying href participates, even if it is invalid. */
	for (walk = bind_following(&document->node, &document->node);
	     walk != NULL;
	     walk = bind_following(walk, &document->node)) {
		/* Foreign elements named base cannot change HTML URL resolution. */
		if (walk->type != DOM_ELEMENT)
			continue;

		/* Namespace and canonical tag determine the HTML base element. */
		element = (struct dom_element *)walk;
		if (element->ns != DOM_NS_HTML || element->tag != DOM_TAG_BASE)
			continue;

		/* A base without href contributes no URL and does not hide later bases. */
		attribute = dom_attribute_ascii(element, "href");
		if (attribute == NULL)
			continue;

		/* Encodes the relative base without retaining any raw DOM pointers in C storage. */
		wb_buffer_init(&text);
		error = vm_string_to_utf8(attribute, &text);
		if (error != 0) {
			wb_buffer_release(&text);
			return error;
		}

		/* A syntax failure in this first href keeps the document's fallback URL. */
		error = net_url_parse(wb_buffer_string(&text), text.length, base, &candidate);
		wb_buffer_release(&text);
		if (error == EINVAL)
			break;

		/* Memory failure is not a successful fallback. */
		if (error != 0)
			return error;

		/* Data and javascript URLs cannot replace the HTML document base. */
		javascript = strcmp(candidate.scheme, "javascript");
		data = strcmp(candidate.scheme, "data");
		forbidden = 0;
		if (javascript == 0 || data == 0)
			forbidden = 1;

		/* Transfers the parsed candidate only when its scheme may act as an HTML base. */
		if (forbidden) {
			net_url_release(&candidate);
		} else {
			net_url_release(base);
			*base = candidate;
		}

		/* Later bases never override the first href in tree order. */
		break;
	}

	/* Succeeded: the caller owns either its document URL or its first usable base. */
	return 0;
}

/* Reports the button's form owner, or null (form). */
static int
button_form_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct dom_element *owner;
	int error;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Requires an actual HTML button even when the getter is borrowed. */
	error = reflection_this(realm, this_value, DOM_TAG_BUTTON, &element);
	if (error != 0)
		return error;

	/* No owner is null. */
	owner = dom_form_owner(element);
	if (owner == NULL) {
		*result = VM_VALUE_NULL;
		return 0;
	}

	/* Succeeded: the owner's wrapper. */
	return bind_wrap(NULL, &owner->node, result);
}

/* Reports canonical button type, including the context-dependent Auto state. */
static int
button_type_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct dom_node *parent;
	struct vm_string *attribute;
	const char *type;
	int matches;
	int error;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Requires an actual HTML button even when the getter is borrowed. */
	error = reflection_this(realm, this_value, DOM_TAG_BUTTON, &element);
	if (error != 0)
		return error;

	/* Missing or unrecognized type begins in the Auto state. */
	attribute = dom_attribute_ascii(element, "type");
	type = NULL;
	matches = reflection_equal_folded(attribute, "submit");
	if (matches) {
		type = "submit";
	} else {
		/* An explicit reset keyword names the Reset Button state. */
		matches = reflection_equal_folded(attribute, "reset");
		if (matches) {
			type = "reset";
		} else {
			/* An explicit button keyword names the inert Button state. */
			matches = reflection_equal_folded(attribute, "button");
			if (matches)
				type = "button";
		}
	}

	/* Auto buttons normally submit unless command targeting or select context applies. */
	if (type == NULL) {
		type = "submit";
		attribute = dom_attribute_ascii(element, "command");
		if (attribute != NULL)
			type = "button";

		/* A command target also prevents the Auto state from being a submit button. */
		attribute = dom_attribute_ascii(element, "commandfor");
		if (attribute != NULL)
			type = "button";

		/* Any direct HTML select parent makes Auto report button. */
		parent = element->node.parent;
		if (parent != NULL && parent->type == DOM_ELEMENT) {
			if (((struct dom_element *)parent)->ns == DOM_NS_HTML &&
			    ((struct dom_element *)parent)->tag == DOM_TAG_SELECT)
				type = "button";
		}
	}

	/* Publishes the canonical type without changing the raw content attribute. */
	error = bind_string(realm, type, result);
	if (error != 0)
		return error;

	/* Succeeded: the caller sees the button's current canonical type. */
	return 0;
}

/* Sets the raw type attribute after DOMString conversion. */
static int
button_type_set(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int error;

	/* Setters have undefined completion while reflecting through the actual DOM. */
	*result = VM_VALUE_UNDEFINED;

	/* Uses ordinary DOM attribute storage rather than a wrapper expando. */
	error = reflection_set(realm, this_value, DOM_TAG_BUTTON, "type", args, count);
	if (error != 0)
		return error;

	/* Succeeded: the reflected attribute follows the branded DOM element. */
	return 0;
}

/* Reports the raw value attribute on the branded HTML receiver. */
static int
button_value_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int error;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Uses ordinary DOM attribute storage rather than a wrapper expando. */
	error = reflection_get(realm, this_value, DOM_TAG_BUTTON, "value", result);
	if (error != 0)
		return error;

	/* Succeeded: the reflected attribute follows the branded DOM element. */
	return 0;
}

/* Sets the raw value attribute after DOMString conversion. */
static int
button_value_set(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int error;

	/* Setters have undefined completion while reflecting through the actual DOM. */
	*result = VM_VALUE_UNDEFINED;

	/* Uses ordinary DOM attribute storage rather than a wrapper expando. */
	error = reflection_set(realm, this_value, DOM_TAG_BUTTON, "value", args, count);
	if (error != 0)
		return error;

	/* Succeeded: the reflected attribute follows the branded DOM element. */
	return 0;
}

/* Reports the raw for attribute on the branded HTML receiver. */
static int
label_for_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int error;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Uses ordinary DOM attribute storage rather than a wrapper expando. */
	error = reflection_get(realm, this_value, DOM_TAG_LABEL, "for", result);
	if (error != 0)
		return error;

	/* Succeeded: the reflected attribute follows the branded DOM element. */
	return 0;
}

/* Sets the raw for attribute after DOMString conversion. */
static int
label_for_set(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int error;

	/* Setters have undefined completion while reflecting through the actual DOM. */
	*result = VM_VALUE_UNDEFINED;

	/* Uses ordinary DOM attribute storage rather than a wrapper expando. */
	error = reflection_set(realm, this_value, DOM_TAG_LABEL, "for", args, count);
	if (error != 0)
		return error;

	/* Succeeded: the reflected attribute follows the branded DOM element. */
	return 0;
}

/* Reports the raw http-equiv attribute on the branded HTML receiver. */
static int
meta_http_equiv_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int error;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Uses ordinary DOM attribute storage rather than a wrapper expando. */
	error = reflection_get(realm, this_value, DOM_TAG_META, "http-equiv", result);
	if (error != 0)
		return error;

	/* Succeeded: the reflected attribute follows the branded DOM element. */
	return 0;
}

/* Sets the raw http-equiv attribute after DOMString conversion. */
static int
meta_http_equiv_set(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int error;

	/* Setters have undefined completion while reflecting through the actual DOM. */
	*result = VM_VALUE_UNDEFINED;

	/* Uses ordinary DOM attribute storage rather than a wrapper expando. */
	error = reflection_set(realm, this_value, DOM_TAG_META, "http-equiv", args, count);
	if (error != 0)
		return error;

	/* Succeeded: the reflected attribute follows the branded DOM element. */
	return 0;
}

/* Resolves object data through the shared native URL attribute path. */
static int
object_data_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int error;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The helper brands an actual object before accessing its data attribute. */
	error = bind_reflect_url_get(realm, this_value, DOM_TAG_OBJECT, "data", result);
	if (error != 0)
		return error;

	/* Succeeded: data reflects the actual owning Document URL. */
	return 0;
}

/* Reflects object data through the shared scalar conversion path. */
static int
object_data_set(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int error;

	/* The helper brands before potentially reentrant conversion. */
	error = bind_reflect_url_set(realm, this_value, DOM_TAG_OBJECT, "data", args, count, result);
	if (error != 0)
		return error;

	/* Succeeded: data owns the converted scalar attribute text. */
	return 0;
}

/* Releases URL getter temporaries on every checked error or successful completion. */
static void
reflection_url_release(
	struct net_url *base,
	struct net_url *parsed,
	struct wb_buffer *raw,
	struct wb_buffer *serialized)
{
	/* URL parser outputs are zero initialized even when syntax or allocation fails. */
	net_url_release(parsed);
	net_url_release(base);
	wb_buffer_release(serialized);
	wb_buffer_release(raw);

	/* Succeeded: no temporary URL records or encoded text remain owned by the getter. */
	return;
}

/* Copies scalar text by replacing lone surrogates through the existing UTF-8 codec. */
static int
reflection_scalar(
	struct vm_realm *realm,
	struct vm_string *text,
	struct vm_string **scalar)
{
	struct wb_buffer bytes;
	int error;

	/* Encodes complete code points, replacing lone UTF-16 surrogates. */
	wb_buffer_init(&bytes);
	error = vm_string_to_utf8(text, &bytes);
	if (error != 0) {
		wb_buffer_release(&bytes);
		return error;
	}

	/* The new string owns its characters before the temporary bytes are released. */
	*scalar = vm_string_from_utf8(realm->heap, wb_buffer_string(&bytes), bytes.length);
	wb_buffer_release(&bytes);
	if (*scalar == NULL)
		return ENOMEM;

	/* Succeeded: the string contains scalar values only. */
	return 0;
}

/* Rejects receivers whose actual node is not the required HTML element kind. */
static int
reflection_this(
	struct vm_realm *realm,
	vm_value receiver,
	int tag,
	struct dom_element **element)
{
	struct dom_node *node;
	int error;

	/* Checks internal DOM identity rather than prototypes or script properties. */
	*element = NULL;
	node = bind_node_of(receiver);
	if (node == NULL || node->type != DOM_ELEMENT) {
		error = bind_throw_illegal(realm);
		if (error != 0)
			return error;

		/* A host that failed to publish the exception still cannot admit this receiver. */
		return EINVAL;
	}

	/* The HTML namespace and exact tag define this interface's receiver brand. */
	*element = (struct dom_element *)node;
	if ((*element)->ns != DOM_NS_HTML || (*element)->tag != tag) {
		error = bind_throw_illegal(realm);
		if (error != 0)
			return error;

		/* A host that failed to publish the exception still cannot admit this receiver. */
		return EINVAL;
	}

	/* Succeeded: the caller may read or update this element's content attributes. */
	return 0;
}

/* Reads a branded element's raw content attribute, with absence yielding empty text. */
static int
reflection_get(
	struct vm_realm *realm,
	vm_value receiver,
	int tag,
	const char *name,
	vm_value *result)
{
	struct dom_element *element;
	struct vm_string *attribute;
	int error;

	/* Brands the actual node before reading an attribute or invoking script conversion. */
	error = reflection_this(realm, receiver, tag, &element);
	if (error != 0)
		return error;

	/* Existing strings already belong to the traced DOM attribute collection. */
	attribute = dom_attribute_ascii(element, name);
	if (attribute != NULL) {
		*result = vm_value_cell(attribute);
		return 0;
	}

	/* An absent ordinary reflected DOMString is the empty string. */
	error = bind_string(realm, "", result);
	if (error != 0)
		return error;

	/* Succeeded: an absent attribute is represented without creating one. */
	return 0;
}

/* Reflects one DOMString setter through existing DOM mutation storage. */
static int
reflection_set(
	struct vm_realm *realm,
	vm_value receiver,
	int tag,
	const char *name,
	const vm_value *args,
	unsigned count)
{
	struct dom_element *element;
	struct vm_string *attribute;
	struct vm_string *text;
	struct vm_cell *roots[2];
	unsigned index;
	unsigned registered;
	int error;

	/* Rejects a foreign receiver before evaluating the setter argument. */
	error = reflection_this(realm, receiver, tag, &element);
	if (error != 0)
		return error;

	/* Allocates the constant attribute atom before the one potentially reentrant conversion. */
	attribute = vm_atom_from_ascii(realm->heap, name);
	if (attribute == NULL)
		return ENOMEM;

	/* A user conversion may collect before either string enters the DOM. */
	roots[0] = &attribute->cell;
	roots[1] = NULL;
	registered = 0;
	for (index = 0; index < 2U; index++) {
		error = vm_heap_add_root(realm->heap, &roots[index]);
		if (error != 0)
			goto cleanup;

		/* Cleanup removes only the slots acquired by this setter. */
		registered++;
	}

	/* Converts once; Symbol or user conversion exceptions leave the attribute untouched. */
	error = bind_to_string(realm, js_argument(args, count, 0), &text);
	if (error != 0)
		goto cleanup;
	roots[1] = &text->cell;

	/* Mutates the real DOM and its ordinary generation, preserving exact UTF-16 text. */
	error = dom_element_set_attribute(element, attribute, text);

cleanup:
	/* Published attributes are now owned by the actual DOM element. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* A failed string conversion or native mutation reports its exact error. */
	if (error != 0)
		return error;

	/* Succeeded: both script and content-attribute views see the same value. */
	return 0;
}

/* Compares an attribute with one canonical ASCII keyword without allocating text. */
static int
reflection_equal_folded(
	const struct vm_string *text,
	const char *keyword)
{
	size_t length;
	size_t index;
	uint16_t unit;

	/* Absence cannot name an explicit enumerated state. */
	if (text == NULL)
		return 0;

	/* Enumerated keywords match exactly, without trimming attribute whitespace. */
	length = strlen(keyword);
	if (text->length != length)
		return 0;

	/* Folds only ASCII uppercase units while comparing the canonical keyword. */
	for (index = 0; index < length; index++) {
		unit = vm_string_at(text, index);
		if (unit >= 'A' && unit <= 'Z')
			unit += 'a' - 'A';

		/* A mismatched unit prevents this keyword from selecting its state. */
		if (unit != (uint16_t)(unsigned char)keyword[index])
			return 0;
	}

	/* Succeeded: the attribute names this exact enumerated keyword. */
	return 1;
}
