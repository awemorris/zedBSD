/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * remacs
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Utility functions available to remacs scripts:
 *
 *   print(x)        Serialize a value and write it with a newline.
 *                   Same contract as the noct CLI's print(), so unit
 *                   tests behave identically under both binaries.
 */

#include <noct/noct.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "remacs.h"

static bool cfunc_print(NoctEnv *env);
static bool cfunc_error(NoctEnv *env);
static bool serialize_value(NoctEnv *env, char *buf, size_t size, NoctValue *value);

bool
remacs_register_api_util(
	NoctEnv *env)
{
	const char *params[] = {"msg"};

	if (!noct_register_cfunc(env, "print", 1, params, cfunc_print, NULL))
		return false;
	if (!noct_register_cfunc(env, "error", 1, params, cfunc_error, NULL))
		return false;
	return true;
}

/*
 * error(msg): raise a runtime error. Until the VM grows a catch
 * mechanism, this aborts the current entry into the VM, like a failing
 * native call does.
 */
static bool
cfunc_error(
	NoctEnv *env)
{
	NoctValue msg;
	const char *msg_s;

	memset(&msg, 0, sizeof(msg));
	if (!noct_pin_local(env, 1, &msg))
		return false;
	if (!noct_get_arg_check_string(env, 0, &msg, &msg_s))
		return false;
	noct_error(env, "%s", msg_s);
	return false;
}

static bool
cfunc_print(
	NoctEnv *env)
{
	char buf[8192];
	NoctValue value;

	memset(&value, 0, sizeof(value));
	if (!noct_pin_local(env, 1, &value))
		return false;
	if (!noct_get_arg(env, 0, &value))
		return false;

	memset(buf, 0, sizeof(buf));
	if (!serialize_value(env, buf, sizeof(buf) - 1, &value))
		return false;
	printf("%s\n", buf);
	fflush(stdout);

	noct_unpin_local(env, 1, &value);
	return true;
}

static void
append(
	char *buf,
	size_t size,
	const char *s)
{
	size_t len = strlen(buf);
	if (len < size)
		strncat(buf, s, size - len);
}

static bool
serialize_value(
	NoctEnv *env,
	char *buf,
	size_t size,
	NoctValue *value)
{
	int type;
	char digits[64];

	if (!noct_get_value_type(env, value, &type))
		return false;

	switch (type) {
	case NOCT_VALUE_INT:
	{
		int v;
		if (!noct_get_int(env, value, &v))
			return false;
		snprintf(digits, sizeof(digits), "%d", v);
		append(buf, size, digits);
		break;
	}
	case NOCT_VALUE_LONG:
	{
		int64_t v;
		if (!noct_get_long(env, value, &v))
			return false;
		snprintf(digits, sizeof(digits), "%" PRId64, v);
		append(buf, size, digits);
		break;
	}
	case NOCT_VALUE_FLOAT:
	{
		float v;
		if (!noct_get_float(env, value, &v))
			return false;
		snprintf(digits, sizeof(digits), "%g", (double)v);
		append(buf, size, digits);
		break;
	}
	case NOCT_VALUE_DOUBLE:
	{
		double v;
		if (!noct_get_double(env, value, &v))
			return false;
		snprintf(digits, sizeof(digits), "%g", v);
		append(buf, size, digits);
		break;
	}
	case NOCT_VALUE_STRING:
	{
		const char *s;
		if (!noct_get_string(env, value, &s))
			return false;
		append(buf, size, s);
		break;
	}
	case NOCT_VALUE_ARRAY:
	{
		NoctValue elem;
		size_t n, i;

		memset(&elem, 0, sizeof(elem));
		noct_pin_local(env, 1, &elem);
		if (!noct_get_array_size(env, value, &n))
			return false;
		append(buf, size, "[");
		for (i = 0; i < n; i++) {
			if (i > 0)
				append(buf, size, ", ");
			if (!noct_get_array_elem(env, value, i, &elem))
				return false;
			if (!serialize_value(env, buf, size, &elem))
				return false;
		}
		append(buf, size, "]");
		noct_unpin_local(env, 1, &elem);
		break;
	}
	case NOCT_VALUE_DICT:
	{
		NoctValue key, elem;
		size_t n, i;
		const char *ks;

		memset(&key, 0, sizeof(key));
		memset(&elem, 0, sizeof(elem));
		noct_pin_local(env, 2, &key, &elem);
		if (!noct_get_dict_size(env, value, &n))
			return false;
		append(buf, size, "{");
		for (i = 0; i < n; i++) {
			if (i > 0)
				append(buf, size, ", ");
			if (!noct_get_dict_by_index(env, value, i, &key, &elem))
				return false;
			if (!noct_get_string(env, &key, &ks))
				return false;
			append(buf, size, ks);
			append(buf, size, ": ");
			if (!serialize_value(env, buf, size, &elem))
				return false;
		}
		append(buf, size, "}");
		noct_unpin_local(env, 2, &key, &elem);
		break;
	}
	case NOCT_VALUE_FUNC:
		append(buf, size, "<func>");
		break;
	case NOCT_VALUE_PACKED:
		append(buf, size, "<packed>");
		break;
	default:
		append(buf, size, "<unknown>");
		break;
	}

	return true;
}
