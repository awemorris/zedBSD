/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The inside of the built-in objects (plan/ws074/design.md §12.3), shared
 * by js/builtin*.c: the helpers that make functions, constructors and
 * properties of a realm, and the installers of each group.
 *
 * A native function reads its arguments with js_argument (a missing one
 * is undefined) and finds which function it is with js_builtin_callee
 * (the realm's callee, set by the caller just before it runs, and read
 * before anything that could call another function).
 */

#ifndef KEILAND_BROWSER_JS_BUILTIN_H
#define KEILAND_BROWSER_JS_BUILTIN_H

#include "js/js.h"
#include "vm/bytecode.h"

/* The groups (builtin_*.c). */
int js_builtin_install_error(struct vm_realm *realm);
int js_builtin_install_object(struct vm_realm *realm);
int js_builtin_install_function(struct vm_realm *realm);
int js_builtin_install_proxy(struct vm_realm *realm);
int js_builtin_install_reflect(struct vm_realm *realm);
int js_builtin_install_array(struct vm_realm *realm);
int js_builtin_install_string(struct vm_realm *realm);
int js_builtin_install_json(struct vm_realm *realm);
int js_builtin_install_boolean(struct vm_realm *realm);
int js_builtin_install_number(struct vm_realm *realm);
int js_builtin_install_math(struct vm_realm *realm);
int js_builtin_install_global(struct vm_realm *realm);
int js_builtin_install_regexp(struct vm_realm *realm);
int js_builtin_install_date(struct vm_realm *realm);
int js_builtin_install_promise(struct vm_realm *realm);
int js_builtin_install_generator(struct vm_realm *realm);
int js_builtin_install_symbol(struct vm_realm *realm);
int js_builtin_install_bigint(struct vm_realm *realm);
int js_builtin_install_typed(struct vm_realm *realm);
int js_typed_bytes(vm_value value, unsigned char **bytes, size_t *length);
int js_typed_random_fill(vm_value value);
int js_builtin_install_iterator(struct vm_realm *realm);
int js_builtin_install_collection(struct vm_realm *realm);
int js_builtin_install_uri(struct vm_realm *realm);

/* Symbols (builtin_symbol.c). */
int js_builtin_symbol_value(struct vm_realm *realm, struct vm_object *object, int which, vm_value value, uint32_t attributes);
int js_builtin_tag(struct vm_realm *realm, struct vm_object *object, const char *tag);
int js_symbol_descriptive_string(struct vm_realm *realm, struct vm_symbol *symbol, vm_value *result);
int js_builtin_species(struct vm_realm *realm, struct vm_object *constructor);
int js_builtin_install_tags(struct vm_realm *realm);

/* Iterators (builtin_iterator.c) and the collections they walk (builtin_collection.c). */
#define JS_ITERATE_KEYS		0U
#define JS_ITERATE_VALUES	1U
#define JS_ITERATE_ENTRIES	2U
#define JS_ITERATOR_ARRAY	0U
#define JS_ITERATOR_STRING	1U
#define JS_ITERATOR_MAP		2U
#define JS_ITERATOR_SET		3U
int js_iterator_create(struct vm_realm *realm, uint32_t family, vm_value source, uint32_t kind, vm_value *result);
int js_iterator_result(struct vm_realm *realm, vm_value value, int done, vm_value *result);
int js_collection_step(vm_value collection, uint32_t *index, vm_value *key, vm_value *value);

/* RegExp's algorithms that String.prototype's methods use (builtin_regexp.c). */
int js_regexp_is(vm_value value);
int js_regexp_create(struct vm_realm *realm, vm_value pattern, vm_value flags, vm_value *result);
int js_regexp_symbol_match(struct vm_realm *realm, vm_value regexp, struct vm_string *string, vm_value *result);
int js_regexp_symbol_replace(struct vm_realm *realm, vm_value regexp, struct vm_string *string, vm_value replace_value, vm_value *result);
int js_regexp_symbol_search(struct vm_realm *realm, vm_value regexp, struct vm_string *string, vm_value *result);
int js_regexp_symbol_split(struct vm_realm *realm, vm_value regexp, struct vm_string *string, vm_value limit, vm_value *result);
int js_regexp_substitution(struct vm_realm *realm, struct vm_string *matched, struct vm_string *string, size_t position, const vm_value *captures, uint32_t capture_count, vm_value named, struct vm_string *replacement, vm_value *result);

/* Evaluating source text for eval and the Function constructor (builtin_global.c). */
int js_builtin_evaluate(struct vm_realm *realm, const struct vm_string *source, int strict, vm_value *result);

#endif
