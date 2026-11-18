/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Publishes the native Proxy constructor and revocable records over the VM's actual internal methods. */

#include "js/builtin.h"

#include <errno.h>
#include <string.h>

static int proxy_construct(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int proxy_revocable(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int proxy_revoke(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);

/*
 * Installs Proxy without an own prototype property and adds its revocable factory.
 * Temporary ownership protects the managed realm and both unpublished native functions.
 */
int
js_builtin_install_proxy(
	struct vm_realm *realm)
{
	struct vm_cell *roots[3];
	struct vm_function *constructor;
	struct vm_function *method;
	unsigned index;
	unsigned registered;
	int status;

	/* Acquire empty placeholders before the first function or cold property-key allocation. */
	memset(roots, 0, sizeof(roots));
	roots[0] = &realm->cell;
	if (!realm->managed)
		roots[0] = NULL;
	registered = 0;
	for (index = 0; index < 3U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;

		/* Cleanup owns only registrations that reached the heap registry. */
		registered++;
	}

	/* The function factory creates only name and length, so Proxy gains no own prototype property. */
	status = js_builtin_function(realm, "Proxy", 2, proxy_construct, proxy_construct, &constructor);
	if (status != 0)
		goto cleanup;
	roots[1] = &constructor->object.cell;
	status = js_builtin_function(realm, "revocable", 2, proxy_revocable, NULL, &method);
	if (status != 0)
		goto cleanup;

	/* The method must survive key creation before its constructor property owns it. */
	roots[2] = &method->object.cell;
	status = js_builtin_value(realm, &constructor->object, "revocable", vm_value_cell(method), JS_BUILTIN_METHOD);
	if (status != 0)
		goto cleanup;
	status = js_builtin_value(realm, realm->global, "Proxy", vm_value_cell(constructor), JS_BUILTIN_METHOD);
	if (status != 0)
		goto cleanup;

cleanup:
	/* All unpublished function and input-realm edges retire only after installation finishes or fails. */
	while (registered != 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Preserve allocation, key-definition and registration errors. */
	if (status != 0)
		return status;

	/* Succeeded: the realm's global object owns the complete Proxy surface. */
	return 0;
}

/* Require construction before delegating input validation and creation to the actual VM operation. */
static int
proxy_construct(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(this_value);

	/* A direct function call refuses before touching either user-provided input. */
	if (realm->new_target == VM_VALUE_UNDEFINED) {
		status = vm_throw_type_error(realm, "Proxy constructor requires new");
		return status;
	}

	/* ProxyCreate validates both arguments without reading any user-visible property. */
	status = vm_proxy_create(realm, js_argument(args, count, 0), js_argument(args, count, 1), result);
	if (status != 0)
		return status;

	/* Succeeded: return the actual managed native Proxy. */
	return 0;
}

/* Return the actual native Proxy and a detached, nonconstructable revoker with a traced data edge. */
static int
proxy_revocable(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_cell *roots[4];
	struct vm_function *revoker;
	struct vm_object *pair;
	vm_value proxy;
	unsigned index;
	unsigned registered;
	int status;

	UNUSED_PARAMETER(this_value);

	/* Root the input realm and all fresh results before any factory can collect. */
	memset(roots, 0, sizeof(roots));
	if (realm->managed)
		roots[0] = &realm->cell;
	registered = 0;
	for (index = 0; index < 4U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;

		/* Only completed registry insertions belong to the shared cleanup path. */
		registered++;
	}

	/* Both native creation entry points share the same actual validation and internal methods. */
	status = vm_proxy_create(realm, js_argument(args, count, 0), js_argument(args, count, 1), &proxy);
	if (status != 0)
		goto cleanup;
	roots[1] = vm_value_as_cell(proxy);
	status = js_builtin_function(realm, "", 0, proxy_revoke, NULL, &revoker);
	if (status != 0)
		goto cleanup;

	/* The revoker's normal traced native data represents its private RevocableProxy slot. */
	roots[2] = &revoker->object.cell;
	revoker->data = proxy;
	pair = vm_object_create(realm->heap, realm->object_prototype);
	if (pair == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* Publish proxy before revoke, with ordinary writable/enumerable/configurable own properties. */
	roots[3] = &pair->cell;
	status = js_builtin_value(realm, pair, "proxy", proxy, VM_PROPERTY_DEFAULT);
	if (status != 0)
		goto cleanup;
	status = js_builtin_value(realm, pair, "revoke", vm_value_cell(revoker), VM_PROPERTY_DEFAULT);
	if (status != 0)
		goto cleanup;
	*result = vm_value_cell(pair);

cleanup:
	/* No collector registry entry may outlive its native placeholder storage. */
	while (registered != 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* A failed factory never publishes a partially initialized result pair. */
	if (status != 0)
		return status;

	/* Succeeded: the pair owns both native results until the caller takes over. */
	return 0;
}

/* Revoke the function's own captured Proxy once, independently of this and any arguments. */
static int
proxy_revoke(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_function *function;
	vm_value proxy;
	int status;

	UNUSED_PARAMETER(this_value);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Undefined data represents the cleared internal slot and makes every subsequent invocation inert. */
	*result = VM_VALUE_UNDEFINED;
	function = js_builtin_callee(realm);
	proxy = function->data;
	if (proxy == VM_VALUE_UNDEFINED)
		return 0;

	/* Clear the revoker's ownership edge before the actual Proxy record releases target and handler. */
	function->data = VM_VALUE_UNDEFINED;
	status = vm_proxy_revoke(realm, proxy);
	if (status != 0)
		return status;

	/* Succeeded: all later operations observe the actual revoked internal slots. */
	return 0;
}
