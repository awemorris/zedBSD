/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Live scalar SVG rect width handles own a traced native graph and reflect actual content attributes.
 */

#include "bind/internal.h"

#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * One collectible cache owns the rect and three stable, distinct native length wrappers.
 * Every wrapper traces this cache, which keeps the complete owner and peer graph alive.
 */
struct svg_width_state {
	struct vm_cell cell;
	struct dom_element *owner;
	struct vm_object *animated;
	struct vm_object *base;
	struct vm_object *animation;
};

static int svg_width_get(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int svg_base_get(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int svg_animation_get(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int svg_unit_get(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int svg_value_get(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int svg_specified_get(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int svg_string_get(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int svg_value_set(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int svg_specified_set(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int svg_string_set(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);

/* Immutable process-lifetime rect accessors expose stable native width identity without public storage. */
static const struct bind_attribute svg_rect_attributes[] = {
	{ "width", svg_width_get, NULL },
	{ NULL, NULL, NULL }
};

/* Immutable process-lifetime animated accessors preserve distinct SameObject base and animated handles. */
static const struct bind_attribute svg_animated_attributes[] = {
	{ "baseVal", svg_base_get, NULL },
	{ "animVal", svg_animation_get, NULL },
	{ NULL, NULL, NULL }
};

/* Immutable process-lifetime scalar accessors use live content; unsupported conversion methods are absent. */
static const struct bind_attribute svg_length_attributes[] = {
	{ "unitType", svg_unit_get, NULL },
	{ "value", svg_value_get, svg_value_set },
	{ "valueInSpecifiedUnits", svg_specified_get, svg_specified_set },
	{ "valueAsString", svg_string_get, svg_string_set },
	{ NULL, NULL, NULL }
};

/* Immutable process-lifetime constants preserve specified SVGLength identities, including relative units. */
static const struct bind_constant svg_length_constants[] = {
	{ "SVG_LENGTHTYPE_UNKNOWN", 0 },
	{ "SVG_LENGTHTYPE_NUMBER", 1 },
	{ "SVG_LENGTHTYPE_PERCENTAGE", 2 },
	{ "SVG_LENGTHTYPE_EMS", 3 },
	{ "SVG_LENGTHTYPE_EXS", 4 },
	{ "SVG_LENGTHTYPE_PX", 5 },
	{ "SVG_LENGTHTYPE_CM", 6 },
	{ "SVG_LENGTHTYPE_MM", 7 },
	{ "SVG_LENGTHTYPE_IN", 8 },
	{ "SVG_LENGTHTYPE_PT", 9 },
	{ "SVG_LENGTHTYPE_PC", 10 },
	{ NULL, 0 }
};

/* Immutable process-lifetime suffixes serialize each supported or recognized native unit canonically. */
static const char *const svg_length_suffixes[] = {
	NULL, "", "%", "em", "ex", "px", "cm", "mm", "in", "pt", "pc"
};

/* This immutable process-lifetime descriptor retains the actual graphical SVG prototype chain. */
const struct bind_interface bind_svg_geometry_element_interface = {
	"SVGGeometryElement", BIND_SVG_GRAPHICS_ELEMENT, 0, NULL, NULL, NULL, NULL
};

/* This immutable process-lifetime rect descriptor supplies the currently supported live width attribute. */
const struct bind_interface bind_svg_rect_element_interface = {
	"SVGRectElement", BIND_SVG_GEOMETRY_ELEMENT, 0, NULL, svg_rect_attributes, NULL, NULL
};

/* This immutable process-lifetime descriptor brands animated wrappers and keeps their constructors illegal. */
const struct bind_interface bind_svg_animated_length_interface = {
	"SVGAnimatedLength", BIND_NO_PARENT, 0, NULL, svg_animated_attributes, NULL, NULL
};

/* This immutable process-lifetime scalar descriptor preserves identity through public prototype replacement. */
const struct bind_interface bind_svg_length_interface = {
	"SVGLength", BIND_NO_PARENT, 0, NULL, svg_length_attributes, NULL, svg_length_constants
};

static void svg_width_trace(struct vm_heap *heap, struct vm_cell *cell);
static const struct vm_cell_type *svg_width_type(void);
static int svg_width_make(struct dom_element *element, struct svg_width_state **out);
static int svg_width_wrapper(struct svg_width_state *state, int interface, struct vm_object **out);
static int svg_width_this(struct vm_realm *realm, vm_value receiver, int animated, struct svg_width_state **out);
static int svg_length_read(struct vm_realm *realm, vm_value receiver, int mode, vm_value *result);
static int svg_length_write(struct vm_realm *realm, vm_value receiver, vm_value argument, int mode, vm_value *result);
static int svg_length_current(struct svg_width_state *state, double *number, int *unit);
static int svg_length_parse(const struct vm_string *text, double *number, int *unit);
static int svg_length_format(double number, int unit, char *text, size_t capacity);
static int svg_length_factor(int unit, double *factor);
static int svg_length_publish(struct svg_width_state *state, double number, int unit);

/* Traces both the owner and every SameObject handle, including saved-only creator graphs. */
static void
svg_width_trace(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct svg_width_state *state;

	/* The native owner itself traces the actual Document and creator prototype snapshot. */
	state = (struct svg_width_state *)cell;
	vm_heap_mark(heap, &state->owner->node.cell);

	/* Construction may collect before the animated wrapper has been allocated. */
	if (state->animated != NULL)
		vm_heap_mark(heap, &state->animated->cell);

	/* A failed partial construction has no base handle to keep alive. */
	if (state->base != NULL)
		vm_heap_mark(heap, &state->base->cell);

	/* The readonly handle is published only after its allocation succeeds. */
	if (state->animation != NULL)
		vm_heap_mark(heap, &state->animation->cell);

	/* Succeeded: a saved scalar handle keeps its complete actual owner graph. */
	return;
}

/* Supplies one private native cell identity without a public brand property. */
static const struct vm_cell_type *
svg_width_type(
	void)
{
	/* Every width cache uses the same process-lifetime type descriptor for exact native branding. */
	static const struct vm_cell_type type = { "svg-rect-width", svg_width_trace, NULL };

	/* Succeeded: native brands compare this descriptor by identity. */
	return &type;
}

/* Returns the actual rect's stable native animated width handle. */
static int
svg_width_get(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	struct dom_element *element;
	struct svg_width_state *state;
	int same;
	int error;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Real native node identity precedes any namespace, name or cache inspection. */
	node = NULL;
	error = bind_this_node(realm, receiver, &node);
	if (error != 0)
		return error;

	/* Only native elements can carry the rect width cache. */
	if (node->type != DOM_ELEMENT) {
		error = bind_throw_illegal(realm);
		return error;
	}

	/* Exact canonical spelling rejects namespace and case lookalikes. */
	element = (struct dom_element *)node;
	same = vm_string_equal_ascii(element->local_name, "rect");
	if (element->ns != DOM_NS_SVG || !same) {
		error = bind_throw_illegal(realm);
		return error;
	}

	/* Publish the cache only after its complete private graph exists. */
	state = (struct svg_width_state *)element->svg_width;
	if (state == NULL) {
		error = svg_width_make(element, &state);
		if (error != 0)
			return error;

		/* Publish the cache only when every stable native wrapper is initialized. */
		element->svg_width = &state->cell;
	}

	/* Succeeded: repeated reads expose the identical actual native handle. */
	*result = vm_value_cell(state->animated);
	return 0;
}

/* Builds the complete cache while precise owner and private-state roots span every VM allocation. */
static int
svg_width_make(
	struct dom_element *element,
	struct svg_width_state **out)
{
	struct vm_heap *heap;
	struct svg_width_state *state;
	struct vm_cell *owner_root;
	struct vm_cell *state_root;
	const struct vm_cell_type *type;
	int error;

	/* Protect the actual native node before allocating its independent state. */
	*out = NULL;
	heap = element->node.document->heap;
	owner_root = &element->node.cell;
	error = vm_heap_add_root(heap, &owner_root);
	if (error != 0)
		return error;

	/* Allocate the branded state while its native rect owner remains precisely rooted. */
	type = svg_width_type();
	state = vm_heap_alloc(heap, type, sizeof(*state));
	if (state == NULL) {
		vm_heap_remove_root(heap, &owner_root);
		return ENOMEM;
	}

	/* Every tracer-visible field is valid before further heap allocation. */
	state->owner = element;
	state->animated = NULL;
	state->base = NULL;
	state->animation = NULL;

	/* Protect the partial state before subsequent wrapper allocation can collect. */
	state_root = &state->cell;
	error = vm_heap_add_root(heap, &state_root);
	if (error != 0) {
		vm_heap_remove_root(heap, &owner_root);
		return error;
	}

	/* Initialize each stable wrapper individually so a partially built graph remains traceable. */
	error = svg_width_wrapper(state, BIND_SVG_ANIMATED_LENGTH, &state->animated);

	/* A failed animated handle prevents creation of a partially usable base handle. */
	if (error == 0)
		error = svg_width_wrapper(state, BIND_SVG_LENGTH, &state->base);

	/* The readonly peer requires both preceding native handle allocations to succeed. */
	if (error == 0)
		error = svg_width_wrapper(state, BIND_SVG_LENGTH, &state->animation);

	/* Complete or failed construction releases both temporary precise roots. */
	vm_heap_remove_root(heap, &state_root);
	vm_heap_remove_root(heap, &owner_root);

	/* The caller cannot publish a cache whose native handles are incomplete. */
	if (error != 0)
		return error;

	/* Succeeded: the caller may publish this complete cache on its actual owner. */
	*out = state;
	return 0;
}

/* Resolves wrapper prototypes from the actual owner Document rather than the borrowed getter realm. */
static int
svg_width_wrapper(
	struct svg_width_state *state,
	int interface,
	struct vm_object **out)
{
	struct dom_document *document;
	struct bind_window *window;
	struct vm_object *prototype;
	struct vm_object *object;
	vm_value snapshot;
	int valid;
	int error;

	/* Snapshot-backed XML factory Documents retain their creator identity even without a live view. */
	document = state->owner->node.document;
	if (document->binding_prototypes != NULL) {
		error = vm_object_get(document->binding_prototypes, vm_value_int32(interface), &snapshot);
		if (error != 0)
			return error;

		/* A creator snapshot must name a real native prototype object. */
		valid = vm_value_is_object(snapshot);
		if (!valid)
			return EINVAL;

		/* Borrow the prototype retained by the protected actual owner Document. */
		prototype = (struct vm_object *)vm_value_as_cell(snapshot);
	} else {
		/* Ordinary parser Documents use their actual owning view, never the borrowed getter realm. */
		window = document->view;
		if (window == NULL)
			return EINVAL;

		/* Use the actual owning view's stable prototype rather than the caller's realm. */
		prototype = window->prototypes[interface];
	}

	/* The protected private cache spans native wrapper allocation and any resulting collection. */
	object = vm_object_create(document->heap, prototype);
	if (object == NULL)
		return ENOMEM;

	/* Only these native wrappers receive the actual private typed cache as internal state. */
	object->kind = VM_KIND_PLATFORM;
	object->internal = vm_value_cell(state);
	*out = object;

	/* Succeeded: the protected cache now owns another genuine native wrapper. */
	return 0;
}

/* Verifies the native private type and exact wrapper role independently of public prototype mutations. */
static int
svg_width_this(
	struct vm_realm *realm,
	vm_value receiver,
	int animated,
	struct svg_width_state **out)
{
	struct vm_object *object;
	struct vm_cell *cell;
	struct svg_width_state *state;
	const struct vm_cell_type *type;
	int valid;
	int error;

	/* Ordinary objects and platform wrappers without this private cell are illegal receivers. */
	valid = vm_value_is_object(receiver);
	if (!valid) {
		error = bind_throw_illegal(realm);
		return error;
	}

	/* A forged prototype cannot install the genuine native state. */
	object = (struct vm_object *)vm_value_as_cell(receiver);
	valid = vm_value_is_cell(object->internal);
	if (object->kind != VM_KIND_PLATFORM || !valid) {
		error = bind_throw_illegal(realm);
		return error;
	}

	/* The exact private cell descriptor excludes other native platform states. */
	cell = vm_value_as_cell(object->internal);
	type = svg_width_type();
	if (cell->type != type) {
		error = bind_throw_illegal(realm);
		return error;
	}

	/* Animated-length accessors cannot be borrowed onto either scalar length wrapper. */
	state = (struct svg_width_state *)cell;
	if (animated) {
		/* Only the animated-length wrapper may expose baseVal and animVal. */
		if (object != state->animated) {
			error = bind_throw_illegal(realm);
			return error;
		}
	} else {
		/* Scalar accessors accept the base or animation peer, excluding the animated-length wrapper. */
		if (object != state->base && object != state->animation) {
			error = bind_throw_illegal(realm);
			return error;
		}
	}

	/* Succeeded: the actual wrapper role and native owner are available. */
	*out = state;
	return 0;
}

/* Returns the stable writable base scalar of a real animated-length wrapper. */
static int
svg_base_get(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct svg_width_state *state;
	int error;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Genuine animated-length branding precedes access to the private SameObject edge. */
	state = NULL;
	error = svg_width_this(realm, receiver, 1, &state);
	if (error != 0)
		return error;

	/* Succeeded: the writable base scalar has its own stable native identity. */
	*result = vm_value_cell(state->base);
	return 0;
}

/* Returns the distinct readonly nonanimated scalar of a real animated-length wrapper. */
static int
svg_animation_get(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct svg_width_state *state;
	int error;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Genuine animated-length branding precedes access to the private readonly edge. */
	state = NULL;
	error = svg_width_this(realm, receiver, 1, &state);
	if (error != 0)
		return error;

	/* Succeeded: the readonly scalar remains distinct from the writable base scalar. */
	*result = vm_value_cell(state->animation);
	return 0;
}

/* Reads the actual parsed native unit identity. */
static int
svg_unit_get(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int error;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The common live reader selects the native unit without scalar conversion. */
	error = svg_length_read(realm, receiver, 0, result);
	if (error != 0)
		return error;

	/* Succeeded: the actual native unit identity is returned. */
	return 0;
}

/* Reads a supported native scalar converted into SVG user units. */
static int
svg_value_get(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int error;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The common live reader converts only units with an implemented actual scale. */
	error = svg_length_read(realm, receiver, 1, result);
	if (error != 0)
		return error;

	/* Succeeded: the actual user-unit scalar is returned. */
	return 0;
}

/* Reads the live native scalar in its specified units. */
static int
svg_specified_get(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int error;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The common live reader preserves the native numeric coefficient. */
	error = svg_length_read(realm, receiver, 2, result);
	if (error != 0)
		return error;

	/* Succeeded: the specified-unit coefficient is returned. */
	return 0;
}

/* Serializes the actual native scalar and its current specified unit. */
static int
svg_string_get(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int error;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The common live reader roots the owner while allocating its scalar serialization. */
	error = svg_length_read(realm, receiver, 3, result);
	if (error != 0)
		return error;

	/* Succeeded: the actual serialized native scalar is returned. */
	return 0;
}

/* Sets a genuine writable scalar as a unitless SVG user-unit value. */
static int
svg_value_set(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int error;

	/* The common writer performs actual coercion and publishes a unitless native attribute. */
	error = svg_length_write(realm, receiver, js_argument(args, count, 0), 1, result);
	if (error != 0)
		return error;

	/* Succeeded: live native width now reflects the new user-unit value. */
	return 0;
}

/* Sets the coefficient while preserving the current native specified unit after coercion. */
static int
svg_specified_set(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int error;

	/* The common writer rereads live unit identity after any script conversion callback. */
	error = svg_length_write(realm, receiver, js_argument(args, count, 0), 2, result);
	if (error != 0)
		return error;

	/* Succeeded: live native width keeps its actual specified unit. */
	return 0;
}

/* Parses and writes a genuine scalar length string to the actual native attribute. */
static int
svg_string_set(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int error;

	/* The common writer validates the complete converted native string before publication. */
	error = svg_length_write(realm, receiver, js_argument(args, count, 0), 3, result);
	if (error != 0)
		return error;

	/* Succeeded: live native width contains the serialized parsed scalar. */
	return 0;
}

/* Reads live native data while the private graph remains precisely rooted through serialization. */
static int
svg_length_read(
	struct vm_realm *realm,
	vm_value receiver,
	int mode,
	vm_value *result)
{
	struct svg_width_state *state;
	struct vm_cell *root;
	struct vm_string *string;
	char text[64];
	double number;
	double factor;
	int unit;
	int error;

	/* Real scalar branding rejects animated handles and ordinary prototype imitations. */
	state = NULL;
	error = svg_width_this(realm, receiver, 0, &state);
	if (error != 0)
		return error;

	/* Keep the genuine owner graph alive through scalar reads and possible string allocation. */
	root = &state->cell;
	error = vm_heap_add_root(realm->heap, &root);
	if (error != 0)
		return error;

	/* Convert supported absolute units only for a successfully parsed user-unit query. */
	error = svg_length_current(state, &number, &unit);
	if (error == 0 && mode == 1) {
		/* Relative scales remain explicitly unsupported instead of using guessed viewport or font metrics. */
		error = svg_length_factor(unit, &factor);
		if (error != 0) {
			error = bind_throw_dom(realm, "NotSupportedError", "Relative SVG user-unit conversion is not implemented.");
		} else {
			number *= factor;
		}
	}

	/* Allocate string results only after the actual scalar and unit have been read. */
	if (error == 0) {
		/* Select the unit identity, specified scalar or serialization requested by this accessor. */
		if (mode == 0) {
			*result = vm_value_int32(unit);
		} else if (mode == 3) {
			error = svg_length_format(number, unit, text, sizeof(text));
			if (error == 0) {
				/* Build the VM string while the real native owner remains precisely rooted. */
				string = vm_string_from_utf8(realm->heap, text, strlen(text));
				if (string == NULL) {
					error = ENOMEM;
				} else {
					*result = vm_value_cell(string);
				}
			}
		} else {
			*result = vm_value_number(number);
		}
	}

	/* No operation adds a permanent root to a saved or retired child owner. */
	vm_heap_remove_root(realm->heap, &root);
	if (error != 0)
		return error;

	/* Succeeded: the result derives from the current actual native width. */
	return 0;
}

/* Converts actual arguments with a precise owner root, then performs one validated native mutation. */
static int
svg_length_write(
	struct vm_realm *realm,
	vm_value receiver,
	vm_value argument,
	int mode,
	vm_value *result)
{
	struct svg_width_state *state;
	struct vm_cell *root;
	struct vm_string *string;
	double number;
	double previous;
	int unit;
	int finite;
	int readonly;
	vm_value animated;
	int error;

	/* Only genuine native scalar wrappers can enter argument conversion. */
	state = NULL;
	error = svg_width_this(realm, receiver, 0, &state);
	if (error != 0)
		return error;

	/* Protect the private owner graph across script coercion and any resulting collection. */
	root = &state->cell;
	error = vm_heap_add_root(realm->heap, &root);
	if (error != 0)
		return error;

	/* Choose the actual scalar coercion before any native mutation is permitted. */
	number = 0;
	unit = 1;
	if (mode == 3) {
		/* Convert ordinary DOMString inputs before parsing the complete scalar grammar. */
		error = bind_to_string(realm, argument, &string);
	} else {
		/* WebIDL float inputs must be finite and representable before native mutation. */
		error = vm_to_number(realm, argument, &number);
		if (error == 0) {
			finite = isfinite(number);
			if (!finite ||
			    number > FLT_MAX ||
			    number < -FLT_MAX) {
				error = vm_throw_type_error(realm, "SVG length requires a finite float.");
			} else {
				number = (float)number;
			}
		}
	}

	/* Readonly animated wrappers cannot publish any native width change after conversion. */
	animated = vm_value_cell(state->animation);
	readonly = 0;
	if (receiver == animated)
		readonly = 1;

	/* Reject a readonly receiver only after its required argument coercion succeeded. */
	if (error == 0 && readonly)
		error = bind_throw_dom(realm, "NoModificationAllowedError", "The animated SVG length is read-only.");

	/* A writable converted string must satisfy the complete native scalar grammar. */
	if (error == 0 && mode == 3) {
		error = svg_length_parse(string, &number, &unit);
		if (error == EINVAL)
			error = bind_throw_dom(realm, "SyntaxError", "Invalid scalar SVG length.");
	}

	/* Specified-unit writes preserve the actual post-conversion native unit. */
	if (error == 0 && mode == 2) {
		/* Actual conversion callbacks may have changed the owner attribute or Document. */
		error = svg_length_current(state, &previous, &unit);
	}

	/* Publish one complete actual native scalar, with no stale public-property cache. */
	if (error == 0)
		error = svg_length_publish(state, number, unit);

	/* Remove temporary protection whether conversion, validation or publication failed. */
	vm_heap_remove_root(realm->heap, &root);
	if (error != 0)
		return error;

	/* Succeeded: all saved base and animated handles read the same updated native attribute. */
	*result = VM_VALUE_UNDEFINED;
	return 0;
}

/* Reads the real unnamespaced attribute, using the initial scalar only for missing or invalid input. */
static int
svg_length_current(
	struct svg_width_state *state,
	double *number,
	int *unit)
{
	struct vm_string *attribute;
	int error;

	/* Parsing never changes the raw native attribute seen by ordinary DOM accessors. */
	attribute = dom_attribute_ascii(state->owner, "width");
	error = svg_length_parse(attribute, number, unit);
	if (error == EINVAL) {
		*number = 0;
		*unit = 1;
		return 0;
	}

	/* Allocation failures remain failures rather than becoming a fabricated zero width. */
	if (error != 0)
		return error;

	/* Succeeded: the current actual native scalar and unit are available. */
	return 0;
}

/* Parses the bounded scalar profile with an explicit decimal grammar before using the host converter. */
static int
svg_length_parse(
	const struct vm_string *text,
	double *number,
	int *unit)
{
	char *bytes;
	char *end;
	char *tail;
	size_t index;
	size_t length;
	uint16_t character;
	unsigned digits;
	unsigned exponent_digits;
	int candidate;
	int finite;
	int error;

	/* Missing attributes and strings without a complete numeric token use the caller's initial value. */
	if (text == NULL || text->length == 0)
		return EINVAL;

	/* The ASCII copy needs one terminator byte in addition to the native input units. */
	length = text->length;
	if (length == SIZE_MAX)
		return ENOMEM;

	/* Allocate C storage only after its complete byte length is proven representable. */
	bytes = malloc(length + 1U);
	if (bytes == NULL)
		return ENOMEM;

	/* Packed VM strings supply actual UTF16 units; non-ASCII tokens cannot name this scalar grammar. */
	error = 0;
	for (index = 0; index < length; index++) {
		character = vm_string_at(text, index);
		if (character == 0 || character > 127U) {
			error = EINVAL;
			break;
		}

		/* Preserve the validated native ASCII unit without transcoding or callbacks. */
		bytes[index] = (char)character;
	}

	/* ASCII-only input can now be scanned without VM allocation or callbacks. */
	if (error != 0) {
		free(bytes);
		return error;
	}

	/* Skip only leading CSS whitespace before the scalar numeric token. */
	bytes[length] = 0;
	end = bytes;
	while (*end == ' ' ||
	       *end == '\t' ||
	       *end == '\r' ||
	       *end == '\n' ||
	       *end == '\f')
		end++;

	/* A leading sign belongs to the numeric token rather than its unit suffix. */
	if (*end == '+' || *end == '-')
		end++;

	/* A decimal token requires a digit on at least one side of its optional decimal point. */
	digits = 0;
	while (*end >= '0' && *end <= '9') {
		digits++;
		end++;
	}

	/* Fractional digits may supply the token's first or remaining decimal digits. */
	if (*end == '.') {
		end++;

		/* Consume all fractional digits before deciding whether an exponent follows. */
		while (*end >= '0' && *end <= '9') {
			digits++;
			end++;
		}
	}

	/* An exponent is numeric only when its optional sign is followed by at least one digit. */
	if ((*end == 'e' || *end == 'E') &&
	    (end[1] == '+' ||
	     end[1] == '-' ||
	     (end[1] >= '0' && end[1] <= '9'))) {
		end++;

		/* The exponent sign is optional and cannot substitute for a real digit. */
		if (*end == '+' || *end == '-')
			end++;

		/* Count the complete exponent digit sequence before accepting the exponent. */
		exponent_digits = 0;
		while (*end >= '0' && *end <= '9') {
			exponent_digits++;
			end++;
		}

		/* A dangling exponent invalidates the whole numeric token. */
		if (exponent_digits == 0)
			digits = 0;
	}

	/* Host strtod cannot admit hexadecimal, infinity, junk or a unit separated from its number. */
	*number = strtod(bytes, &tail);
	finite = isfinite(*number);
	if (digits == 0 ||
	    tail != end ||
	    !finite ||
	    *number > FLT_MAX ||
	    *number < -FLT_MAX) {
		free(bytes);
		return EINVAL;
	}

	/* Trailing CSS whitespace is permitted after the exact native unit token. */
	tail = bytes + length;
	while (tail > end &&
	       (tail[-1] == ' ' ||
	        tail[-1] == '\t' ||
	        tail[-1] == '\r' ||
	        tail[-1] == '\n' ||
	        tail[-1] == '\f'))
		tail--;
	*tail = 0;

	/* CSS unit spelling is case-insensitive while its native numeric identity remains fixed. */
	for (tail = end;
	     *tail != 0;
	     tail++) {
		/* Fold ASCII unit spelling without applying a locale-dependent conversion. */
		if (*tail >= 'A' && *tail <= 'Z')
			*tail += 'a' - 'A';
	}

	/* Match one exact recognized unit rather than accepting a partial suffix. */
	*unit = 0;
	for (candidate = 1; candidate <= 10; candidate++) {
		error = strcmp(end, svg_length_suffixes[candidate]);
		if (error == 0) {
			*unit = candidate;
			break;
		}
	}

	/* Free checked C storage before exposing the completed native scalar. */
	free(bytes);
	if (*unit == 0)
		return EINVAL;

	/* Preserve the accepted finite WebIDL float value rather than extra host double precision. */
	*number = (float)*number;

	/* Succeeded: only a complete finite scalar token and recognized unit were accepted. */
	return 0;
}

/* Serializes one finite scalar with its actual canonical specified-unit suffix. */
static int
svg_length_format(
	double number,
	int unit,
	char *text,
	size_t capacity)
{
	int length;

	/* A private unit outside this supported profile cannot index the serialization table. */
	if (unit < 1 || unit > 10)
		return EINVAL;

	/* Canonical serialization hides a floating-point negative-zero sign. */
	if (number == 0)
		number = 0;

	/* Format the complete canonical scalar and suffix into the caller's bounded buffer. */
	length = snprintf(text, capacity, "%.9g%s", number, svg_length_suffixes[unit]);
	if (length < 0 || (size_t)length >= capacity)
		return EOVERFLOW;

	/* Succeeded: the complete serialized scalar fits the caller's storage. */
	return 0;
}

/* Converts only numeric and absolute CSS length units with their specified native user-unit scale. */
static int
svg_length_factor(
	int unit,
	double *factor)
{
	/* Relative geometry is not guessed from unrelated Page pixels or half an em. */
	if (unit >= 2 && unit <= 4)
		return ENOTSUP;

	/* Each absolute length has a real conversion factor at 96 CSS pixels per inch. */
	switch (unit) {
	case 1:
	case 5:
		*factor = 1;
		break;
	case 6:
		*factor = 96.0 / 2.54;
		break;
	case 7:
		*factor = 96.0 / 25.4;
		break;
	case 8:
		*factor = 96;
		break;
	case 9:
		*factor = 96.0 / 72.0;
		break;
	case 10:
		*factor = 16;
		break;
	default:
		return EINVAL;
	}

	/* Succeeded: this scalar uses an implemented actual absolute scale. */
	return 0;
}

/* Publishes one serialized value through the existing native attribute mutation and invalidation path. */
static int
svg_length_publish(
	struct svg_width_state *state,
	double number,
	int unit)
{
	struct vm_heap *heap;
	struct vm_string *name;
	struct vm_string *string;
	char text[64];
	int error;

	/* Scalar serialization is complete before any native content can change. */
	error = svg_length_format(number, unit, text, sizeof(text));
	if (error != 0)
		return error;

	/* Resolve the native width atom before allocating its new content value. */
	heap = state->owner->node.document->heap;
	name = vm_atom_from_ascii(heap, "width");
	if (name == NULL)
		return ENOMEM;

	/* Copy the validated scalar serialization into traced native attribute storage. */
	string = vm_string_from_utf8(heap, text, strlen(text));
	if (string == NULL)
		return ENOMEM;

	/* Use ordinary native attribute mutation so every saved handle reads the committed value. */
	error = dom_element_set_attribute(state->owner, name, string);
	if (error != 0)
		return error;

	/* Succeeded: ordinary DOM, base and nonanimated handles observe the same actual attribute. */
	return 0;
}
