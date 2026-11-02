/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Symbol (ws074-p087): the Symbol function with the well-known symbols the
 * realm made (vm_symbol_key), Symbol.for and Symbol.keyFor over the realm's
 * registry, and Symbol.prototype.
 *
 * The registry is an object with no prototype whose keys are the
 * descriptions and whose values are the registered symbols (a registered
 * symbol is marked, so keyFor needs no reverse search).
 */

#include "js/builtin.h"

#include <errno.h>
#include <string.h>

/* The well-known symbols as properties of Symbol, in the order of enum vm_well_known. */
static const char *const symbol_names[VM_SYMBOLS] = {
    "asyncIterator",
    "hasInstance",
    "isConcatSpreadable",
    "iterator",
    "match",
    "matchAll",
    "replace",
    "search",
    "species",
    "split",
    "toPrimitive",
    "toStringTag",
    "unscopables"};

static int symbol_call(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int symbol_for(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int symbol_key_for(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int symbol_to_string(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int symbol_value_of(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int symbol_description(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int symbol_this(struct vm_realm *realm, vm_value value, struct vm_symbol **symbol);
static int symbol_species(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);

/*
 * Installs Symbol and its prototype, registry methods and well-known symbols.
 * The function cannot be used as a constructor.
 */
int
js_builtin_install_symbol(
	struct vm_realm *realm)
{
	struct vm_function *constructor;
	struct vm_function *function;
	struct vm_object *prototype;
	struct vm_object *registry;
	vm_value tag;
	int index;
	int error;

	/* The prototype, and the function on the global object (no construct: new Symbol is a TypeError). */
	prototype = vm_object_create(realm->heap, realm->object_prototype);
	if (prototype == NULL)
		return ENOMEM;

	/* Roots the prototype before constructing its public function. */
	realm->intrinsics[VM_INTRINSIC_SYMBOL_PROTOTYPE] = prototype;
	error = js_builtin_constructor(realm, "Symbol", 0, symbol_call, NULL, prototype, &constructor);
	if (error != 0)
		return error;

	/* The well-known symbols: neither writable, enumerable nor configurable. */
	for (index = 0; index < (int)VM_SYMBOLS; index++) {
		error = js_builtin_value(realm, &constructor->object, symbol_names[index], vm_symbol_key(realm, index), 0);
		if (error != 0)
			return error;
	}

	/* The registry and the functions over it. */
	registry = vm_object_create(realm->heap, NULL);
	if (registry == NULL)
		return ENOMEM;

	/* Roots the private registry before publishing methods that use it. */
	realm->intrinsics[VM_INTRINSIC_SYMBOL_REGISTRY] = registry;
	error = js_builtin_method(realm, &constructor->object, "for", 1, symbol_for);
	if (error != 0)
		return error;

	/* Exposes reverse lookup of registered symbols. */
	error = js_builtin_method(realm, &constructor->object, "keyFor", 1, symbol_key_for);
	if (error != 0)
		return error;

	/* The prototype's methods and description. */
	error = js_builtin_method(realm, prototype, "toString", 0, symbol_to_string);
	if (error != 0)
		return error;

	/* Exposes the primitive symbol behind a symbol or wrapper. */
	error = js_builtin_method(realm, prototype, "valueOf", 0, symbol_value_of);
	if (error != 0)
		return error;

	/* Exposes the optional description through a getter. */
	error = js_builtin_accessor(realm, prototype, "description", symbol_description, NULL);
	if (error != 0)
		return error;

	/* Symbol.prototype[Symbol.toPrimitive] (configurable only) is valueOf's twin. */
	error = js_builtin_function(realm, "[Symbol.toPrimitive]", 1, symbol_value_of, NULL, &function);
	if (error != 0)
		return error;

	/* Publishes the primitive conversion method with configurable attributes. */
	error = js_builtin_symbol_value(realm, prototype, VM_SYMBOL_TO_PRIMITIVE, vm_value_cell(function), VM_PROPERTY_CONFIGURABLE);
	if (error != 0)
		return error;

	/* Symbol.prototype[Symbol.toStringTag]. */
	error = js_builtin_string(realm, "Symbol", &tag);
	if (error != 0)
		return error;

	/* Publishes the configurable prototype tag. */
	error = js_builtin_symbol_value(realm, prototype, VM_SYMBOL_TO_STRING_TAG, tag, VM_PROPERTY_CONFIGURABLE);
	if (error != 0)
		return error;

	/* Succeeded: Symbol is installed. */
	return 0;
}

/*
 * Defines a built-in object property keyed by a well-known symbol.
 */
int
js_builtin_symbol_value(
	struct vm_realm *realm,
	struct vm_object *object,
	int which,
	vm_value value,
	uint32_t attributes)
{
	int error;

	/* The property. */
	error = vm_object_define(realm->heap, object, vm_symbol_key(realm, which), value, attributes);
	if (error != 0)
		return error;

	/* Succeeded: the property is defined. */
	return 0;
}

/*
 * Defines a built-in object's configurable Symbol.toStringTag.
 * The tag supplies the name reported by Object.prototype.toString.
 */
int
js_builtin_tag(
	struct vm_realm *realm,
	struct vm_object *object,
	const char *tag)
{
	vm_value text;
	int error;

	/* The name as a string. */
	error = js_builtin_string(realm, tag, &text);
	if (error != 0)
		return error;

	/* The property. */
	error = js_builtin_symbol_value(realm, object, VM_SYMBOL_TO_STRING_TAG, text, VM_PROPERTY_CONFIGURABLE);
	if (error != 0)
		return error;

	/* Succeeded: the tag is defined. */
	return 0;
}

/*
 * Makes a symbol's descriptive string for SymbolDescriptiveString.
 * Joins "Symbol(" with its optional description and ")".
 */
int
js_symbol_descriptive_string(
	struct vm_realm *realm,
	struct vm_symbol *symbol,
	vm_value *result)
{
	struct vm_string *open;
	struct vm_string *close;
	struct vm_string *text;

	/* "Symbol(". */
	open = vm_string_from_utf8(realm->heap, "Symbol(", 7);
	if (open == NULL)
		return ENOMEM;

	/* Starts the result with the opening delimiter. */
	text = open;

	/* The description, when there is one. */
	if (symbol->description != VM_VALUE_UNDEFINED) {
		text = vm_string_concat(realm->heap, open, (struct vm_string *)vm_value_as_cell(symbol->description));
		if (text == NULL)
			return ENOMEM;
	}

	/* ")". */
	close = vm_string_from_utf8(realm->heap, ")", 1);
	if (close == NULL)
		return ENOMEM;

	/* Joins the completed description with its closing delimiter. */
	text = vm_string_concat(realm->heap, text, close);
	if (text == NULL)
		return ENOMEM;

	/* Publishes the result to the caller. */
	*result = vm_value_cell(text);

	/* Succeeded: the string. */
	return 0;
}

/*
 * Defines a built-in constructor's Symbol.species getter.
 * Returns this, the constructor used for derived objects.
 */
int
js_builtin_species(
	struct vm_realm *realm,
	struct vm_object *constructor)
{
	struct vm_function *getter;
	struct vm_accessor *accessor;
	int error;

	/* The getter. */
	error = js_builtin_function(realm, "get [Symbol.species]", 0, symbol_species, NULL, &getter);
	if (error != 0)
		return error;

	/* The accessor property (configurable only). */
	accessor = vm_accessor_create(realm->heap, vm_value_cell(getter), VM_VALUE_UNDEFINED);
	if (accessor == NULL)
		return ENOMEM;

	/* Publishes the getter as a configurable accessor. */
	error = js_builtin_symbol_value(realm, constructor, VM_SYMBOL_SPECIES, vm_value_cell(accessor), VM_PROPERTY_ACCESSOR | VM_PROPERTY_CONFIGURABLE);
	if (error != 0)
		return error;

	/* Succeeded: the constructor has its species. */
	return 0;
}

/*
 * Installs tags and species on built-ins created before Symbol.
 * Covers Math, JSON, promise/generator/async prototypes, and the Array,
 * Promise and RegExp constructors.
 */
int
js_builtin_install_tags(
	struct vm_realm *realm)
{
	const char *const globals[3] = {"Math", "JSON", "Reflect"};
	const char *const species[3] = {"Array", "Promise", "RegExp"};
	struct vm_property property;
	vm_value key;
	int found;
	int index;
	int error;

	/* Math, JSON and Reflect by their global names. */
	for (index = 0; index < 3; index++) {
		key = vm_key_from_ascii(realm->heap, globals[index]);
		if (key == VM_VALUE_EMPTY)
			return ENOMEM;

		/* Reads only the constructor or namespace's own global binding. */
		found = vm_object_get_own(realm->global, key, &property);
		if (found < 0)
			return -found;

		/* A missing descriptor follows the absence path after errors have been excluded. */
		if (!found)
			continue;

		/* Assigns the namespace object its built-in tag. */
		error = js_builtin_tag(realm, (struct vm_object *)vm_value_as_cell(*property.value), globals[index]);
		if (error != 0)
			return error;
	}

	/* The prototypes. */
	error = js_builtin_tag(realm, realm->intrinsics[VM_INTRINSIC_PROMISE_PROTOTYPE], "Promise");
	if (error != 0)
		return error;

	/* Identifies generator instances through their prototype tag. */
	error = js_builtin_tag(realm, realm->intrinsics[VM_INTRINSIC_GENERATOR_PROTOTYPE], "Generator");
	if (error != 0)
		return error;

	/* Identifies generator functions through their prototype tag. */
	error = js_builtin_tag(realm, realm->intrinsics[VM_INTRINSIC_GENERATOR_FUNCTION_PROTOTYPE], "GeneratorFunction");
	if (error != 0)
		return error;

	/* Identifies async functions through their prototype tag. */
	error = js_builtin_tag(realm, realm->intrinsics[VM_INTRINSIC_ASYNC_FUNCTION_PROTOTYPE], "AsyncFunction");
	if (error != 0)
		return error;

	/* The constructors with a species. */
	for (index = 0; index < 3; index++) {
		key = vm_key_from_ascii(realm->heap, species[index]);
		if (key == VM_VALUE_EMPTY)
			return ENOMEM;

		/* Reads only the constructor or namespace's own global binding. */
		found = vm_object_get_own(realm->global, key, &property);
		if (found < 0)
			return -found;

		/* A missing descriptor follows the absence path after errors have been excluded. */
		if (!found)
			continue;

		/* Assigns the constructor its default species getter. */
		error = js_builtin_species(realm, (struct vm_object *)vm_value_as_cell(*property.value));
		if (error != 0)
			return error;
	}

	/* Succeeded: the tags and species are defined. */
	return 0;
}

/* Symbol(description): a new symbol, its description the argument's string (undefined for none). */
static int
symbol_call(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_symbol *symbol;
	struct vm_string *string;
	vm_value description;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The description. */
	*result = VM_VALUE_UNDEFINED;
	description = js_argument(args, count, 0);
	if (description != VM_VALUE_UNDEFINED) {
		status = vm_to_string(realm, description, &string);
		if (status != 0)
			return status;

		/* Retains the converted description for symbol allocation. */
		description = vm_value_cell(string);
	}

	/* The symbol. */
	symbol = vm_symbol_create(realm->heap, description);
	if (symbol == NULL)
		return ENOMEM;

	/* Publishes the result to the caller. */
	*result = vm_value_cell(symbol);

	/* Succeeded: the new symbol. */
	return 0;
}

/* Symbol.for(key): the registry's symbol for the key's string, made and registered the first time. */
static int
symbol_for(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_object *registry;
	struct vm_property property;
	struct vm_symbol *symbol;
	struct vm_string *string;
	vm_value argument;
	vm_value key;
	int found;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The key's string, as a property key of the registry. */
	*result = VM_VALUE_UNDEFINED;
	argument = js_argument(args, count, 0);
	status = vm_to_string(realm, argument, &string);
	if (status != 0)
		return status;

	/* Interns the description as the registry's property key. */
	status = vm_key_from_string(realm->heap, string, &key);
	if (status != 0)
		return status;

	/* A symbol registered already. */
	registry = realm->intrinsics[VM_INTRINSIC_SYMBOL_REGISTRY];
	found = vm_object_get_own(registry, key, &property);
	if (found < 0)
		return -found;

	/* Present descriptors retain their ordinary accessor and attribute rules. */
	if (found) {
		*result = *property.value;
		return 0;
	}

	/* A new symbol, registered. */
	symbol = vm_symbol_create(realm->heap, vm_value_cell(string));
	if (symbol == NULL)
		return ENOMEM;

	/* Marks the new symbol before exposing it through the registry. */
	symbol->registered = 1;
	status = vm_object_define(realm->heap, registry, key, vm_value_cell(symbol), VM_PROPERTY_DEFAULT);
	if (status != 0)
		return status;

	/* Publishes the result to the caller. */
	*result = vm_value_cell(symbol);

	/* Succeeded: the registered symbol. */
	return 0;
}

/* Symbol.keyFor(symbol): a registered symbol's key, undefined for any other symbol. */
static int
symbol_key_for(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_symbol *symbol;
	struct vm_cell *cell;
	vm_value value;
	int is_cell;
	int status;

	UNUSED_PARAMETER(this_value);

	/* Only a symbol has a key. */
	*result = VM_VALUE_UNDEFINED;
	value = js_argument(args, count, 0);
	is_cell = vm_value_is_cell(value);
	cell = NULL;
	if (is_cell)
		cell = vm_value_as_cell(value);

	/* Rejects non-symbols without consulting the registry. */
	if (cell == NULL || cell->type != &vm_symbol_type) {
		status = vm_throw_type_error(realm, "Symbol.keyFor: value is not a symbol");
		if (status != 0)
			return status;

		/* Succeeded: the incompatible argument is reported as a VM exception. */
		return 0;
	}

	/* A registered symbol's key is its description. */
	symbol = (struct vm_symbol *)cell;
	if (symbol->registered)
		*result = symbol->description;

	/* Succeeded: the key or undefined. */
	return 0;
}

/* Symbol.prototype.toString(): "Symbol(description)". */
static int
symbol_to_string(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_symbol *symbol;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The symbol this is or wraps. */
	*result = VM_VALUE_UNDEFINED;
	status = symbol_this(realm, this_value, &symbol);
	if (status != 0)
		return status;

	/* Its descriptive string. */
	status = js_symbol_descriptive_string(realm, symbol, result);
	if (status != 0)
		return status;

	/* Succeeded: the string. */
	return 0;
}

/* Symbol.prototype.valueOf() and [Symbol.toPrimitive](): the symbol this is or wraps. */
static int
symbol_value_of(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_symbol *symbol;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The symbol. */
	*result = VM_VALUE_UNDEFINED;
	status = symbol_this(realm, this_value, &symbol);
	if (status != 0)
		return status;

	/* Publishes the result to the caller. */
	*result = vm_value_cell(symbol);

	/* Succeeded: the symbol's value. */
	return 0;
}

/* get Symbol.prototype.description: the description of the symbol this is or wraps. */
static int
symbol_description(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_symbol *symbol;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The symbol. */
	*result = VM_VALUE_UNDEFINED;
	status = symbol_this(realm, this_value, &symbol);
	if (status != 0)
		return status;

	/* Publishes the result to the caller. */
	*result = symbol->description;

	/* Succeeded: its description (undefined for none). */
	return 0;
}

/* Finds the symbol a this value is or wraps (thisSymbolValue); a TypeError for anything else. */
static int
symbol_this(
	struct vm_realm *realm,
	vm_value value,
	struct vm_symbol **symbol)
{
	struct vm_object *object;
	struct vm_cell *cell;
	int is_cell;
	int is_object;
	int status;

	/* A symbol itself. */
	*symbol = NULL;
	is_cell = vm_value_is_cell(value);
	if (is_cell) {
		cell = vm_value_as_cell(value);
		if (cell->type == &vm_symbol_type) {
			*symbol = (struct vm_symbol *)cell;
			return 0;
		}
	}

	/* A Symbol object's symbol. */
	is_object = vm_value_is_object(value);
	if (is_object) {
		object = (struct vm_object *)vm_value_as_cell(value);
		if (object->kind == VM_KIND_SYMBOL) {
			*symbol = (struct vm_symbol *)vm_value_as_cell(object->internal);
			return 0;
		}
	}

	/* Anything else. */
	status = vm_throw_type_error(realm, "Symbol.prototype method called on incompatible receiver");
	if (status != 0)
		return status;

	/* Succeeded: the incompatible receiver is reported as a VM exception. */
	return 0;
}

/* get [Symbol.species] of the built-in constructors: this. */
static int
symbol_species(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	UNUSED_PARAMETER(realm);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Publishes the result to the caller. */
	*result = this_value;

	/* Succeeded: this. */
	return 0;
}
