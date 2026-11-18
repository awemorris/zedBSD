/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Date (ws074-p076: the Date object Amazon's scripts need first).
 *
 * A Date object is an ordinary object of kind VM_KIND_DATE whose internal
 * value is its time value: milliseconds from 1970-01-01T00:00:00Z, or NaN
 * for an invalid date.  The calendar is the proleptic Gregorian one of the
 * specification, computed with whole days from the epoch (the civil
 * algorithms of Howard Hinnant, in doubles); local time is the system's
 * (its offset from UTC at the moment, so daylight saving follows the
 * system's zone).  The getters and setters are one native function each
 * for the local and the UTC families: the function's data names the field.
 * Date.parse reads the ISO format of the specification and, like other
 * browsers, the formats toString and toUTCString write and the common
 * "Month day, year", "m/d/y" and "y/m/d" forms.  toLocale*String write
 * the en-US forms (there is no Intl).  Not in this pass:
 * Date.prototype[Symbol.toPrimitive] (the VM's ToPrimitive gives a Date the
 * string hint by default instead) and Temporal.
 */

#include "js/builtin.h"
#include "base/os-time.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Milliseconds in a day, an hour, a minute and a second. */
#define DATE_MS_PER_DAY		86400000.0
#define DATE_MS_PER_HOUR	3600000.0
#define DATE_MS_PER_MINUTE	60000.0
#define DATE_MS_PER_SECOND	1000.0

/* The largest time value a Date may hold (100 000 000 days either side of the epoch). */
#define DATE_TIME_MAX		8.64e15

/* The longest date string Date.parse reads. */
#define DATE_TEXT_MAX		256U

/* Added to a field in a getter's or setter's data: the function works in UTC, not local time. */
#define DATE_UTC		0x100

/* The fields a getter reads or a setter writes (a function's data; DATE_UTC marks the UTC family). */
enum date_field {
	DATE_FIELD_YEAR,
	DATE_FIELD_MONTH,
	DATE_FIELD_DATE,
	DATE_FIELD_HOURS,
	DATE_FIELD_MINUTES,
	DATE_FIELD_SECONDS,
	DATE_FIELD_MS,
	DATE_FIELD_DAY
};

/* The forms date_write writes. */
enum date_form {
	DATE_FORM_STRING,
	DATE_FORM_DATE,
	DATE_FORM_TIME,
	DATE_FORM_UTC,
	DATE_FORM_ISO,
	DATE_FORM_LOCALE,
	DATE_FORM_LOCALE_DATE,
	DATE_FORM_LOCALE_TIME
};

/*
 * The parts of a time value: the year, the month (0 to 11), the day of the
 * month, the time of day and the day of the week (0 for Sunday).
 */
struct date_parts {
	double year;
	double month;
	double date;
	double hours;
	double minutes;
	double seconds;
	double ms;
	double day;
};

/* One getter or setter: its name, its length, its native function and the field it works on. */
struct date_entry {
	const char *name;
	unsigned length;
	vm_native native;
	int field;
};

/*
 * What Date.parse has read of a date string in one of the legacy forms:
 * up to three numbers of the date (with their digits, which tell a year
 * from a day), the time, a month's name, AM or PM and a zone.  It lives on
 * the stack of one parse.
 */
struct date_legacy {
	double numbers[3];
	int number_digits[3];
	unsigned count;
	double hours;
	double minutes;
	double seconds;
	double ms;
	double offset;
	int named_month;
	int meridiem;
	int has_zone;
	int after_time;
};

/* A time zone's abbreviation, as libc names it, and the long name browsers write for it. */
struct date_zone_name {
	const char *abbreviation;
	const char *name;
};

/* One method with nothing to tell it apart: its name, its length and its native function. */
struct date_method {
	const char *name;
	unsigned length;
	vm_native native;
};

static int date_call(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int date_construct(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int date_now_method(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int date_parse_method(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int date_utc_method(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int date_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int date_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int date_get_time(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int date_get_timezone_offset(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int date_get_year(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int date_set_time(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int date_set_year(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int date_to_string(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int date_to_date_string(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int date_to_time_string(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int date_to_utc_string(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int date_to_iso_string(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int date_to_json(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int date_to_locale_string(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int date_to_locale_date_string(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int date_to_locale_time_string(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int date_to_primitive(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int date_this_time(struct vm_realm *realm, vm_value this_value, double *time);
static int date_store(vm_value this_value, double time, vm_value *result);
static int date_numbers(struct vm_realm *realm, const vm_value *args, unsigned count, unsigned most, double *numbers);
static int date_from_value(struct vm_realm *realm, vm_value value, double *time);
static int date_from_parts(struct vm_realm *realm, const vm_value *args, unsigned count, double *time);
static int date_text(struct vm_realm *realm, const char *text, vm_value *result);
static int date_format(struct vm_realm *realm, vm_value this_value, int form, vm_value *result);
static void date_write(double time, int form, char *out, size_t size);
static void date_zone(double time, char *out, size_t size);
static double date_now(void);
static double date_days_from_civil(double year, double month, double day);
static void date_civil_from_days(double days, double *year, double *month, double *day);
static double date_make_time(double hours, double minutes, double seconds, double ms);
static double date_make_day(double year, double month, double date);
static double date_make_date(double day, double time);
static double date_clip(double time);
static double date_offset_at(double time);
static double date_local(double time);
static double date_utc(double time);
static void date_split(double time, struct date_parts *parts);
static double date_parse(const struct vm_string *string);
static int date_parse_iso(const char *text, double *time);
static double date_parse_legacy(const char *text);
static void date_legacy_comment(const char *text, size_t *index);
static int date_legacy_word(const char *text, size_t *index, struct date_legacy *state);
static int date_legacy_is_zone(const char *word);
static void date_legacy_offset(const char *text, size_t *index, struct date_legacy *state);
static int date_legacy_number(const char *text, size_t *index, struct date_legacy *state);
static void date_legacy_time(const char *text, size_t *index, struct date_legacy *state);
static double date_legacy_value(const struct date_legacy *state);
static int date_digits(const char *text, size_t *index, unsigned count, double *value);
static size_t date_fraction(const char *text, size_t *index, double *ms);
static int date_month_of(const char *word);
static int date_day_of(const char *word);

/* The days' and months' short names. */
static const char *const date_day_names[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const date_month_names[] = {
	"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

/*
 * The long names Chromium writes in toString for common zones whose
 * abbreviation means one zone only (CST, IST and the like are left out);
 * any other zone is written by its abbreviation.
 */
static const struct date_zone_name date_zone_names[] = {
	{ "UTC", "Coordinated Universal Time" },
	{ "UCT", "Coordinated Universal Time" },
	{ "GMT", "Greenwich Mean Time" },
	{ "BST", "British Summer Time" },
	{ "JST", "Japan Standard Time" },
	{ "KST", "Korean Standard Time" },
	{ "CET", "Central European Standard Time" },
	{ "CEST", "Central European Summer Time" },
	{ "EST", "Eastern Standard Time" },
	{ "EDT", "Eastern Daylight Time" },
	{ "MST", "Mountain Standard Time" },
	{ "MDT", "Mountain Daylight Time" },
	{ "PST", "Pacific Standard Time" },
	{ "PDT", "Pacific Daylight Time" },
	{ NULL, NULL }
};

/* The getters and setters (the field, with DATE_UTC for the UTC family). */
static const struct date_entry date_fields[] = {
	{ "getDate", 0, date_get, DATE_FIELD_DATE },
	{ "getDay", 0, date_get, DATE_FIELD_DAY },
	{ "getFullYear", 0, date_get, DATE_FIELD_YEAR },
	{ "getHours", 0, date_get, DATE_FIELD_HOURS },
	{ "getMilliseconds", 0, date_get, DATE_FIELD_MS },
	{ "getMinutes", 0, date_get, DATE_FIELD_MINUTES },
	{ "getMonth", 0, date_get, DATE_FIELD_MONTH },
	{ "getSeconds", 0, date_get, DATE_FIELD_SECONDS },
	{ "getUTCDate", 0, date_get, DATE_FIELD_DATE | DATE_UTC },
	{ "getUTCDay", 0, date_get, DATE_FIELD_DAY | DATE_UTC },
	{ "getUTCFullYear", 0, date_get, DATE_FIELD_YEAR | DATE_UTC },
	{ "getUTCHours", 0, date_get, DATE_FIELD_HOURS | DATE_UTC },
	{ "getUTCMilliseconds", 0, date_get, DATE_FIELD_MS | DATE_UTC },
	{ "getUTCMinutes", 0, date_get, DATE_FIELD_MINUTES | DATE_UTC },
	{ "getUTCMonth", 0, date_get, DATE_FIELD_MONTH | DATE_UTC },
	{ "getUTCSeconds", 0, date_get, DATE_FIELD_SECONDS | DATE_UTC },
	{ "setDate", 1, date_set, DATE_FIELD_DATE },
	{ "setFullYear", 3, date_set, DATE_FIELD_YEAR },
	{ "setHours", 4, date_set, DATE_FIELD_HOURS },
	{ "setMilliseconds", 1, date_set, DATE_FIELD_MS },
	{ "setMinutes", 3, date_set, DATE_FIELD_MINUTES },
	{ "setMonth", 2, date_set, DATE_FIELD_MONTH },
	{ "setSeconds", 2, date_set, DATE_FIELD_SECONDS },
	{ "setUTCDate", 1, date_set, DATE_FIELD_DATE | DATE_UTC },
	{ "setUTCFullYear", 3, date_set, DATE_FIELD_YEAR | DATE_UTC },
	{ "setUTCHours", 4, date_set, DATE_FIELD_HOURS | DATE_UTC },
	{ "setUTCMilliseconds", 1, date_set, DATE_FIELD_MS | DATE_UTC },
	{ "setUTCMinutes", 3, date_set, DATE_FIELD_MINUTES | DATE_UTC },
	{ "setUTCMonth", 2, date_set, DATE_FIELD_MONTH | DATE_UTC },
	{ "setUTCSeconds", 2, date_set, DATE_FIELD_SECONDS | DATE_UTC },
	{ NULL, 0, NULL, 0 }
};

/* The other methods of Date.prototype. */
static const struct date_method date_methods[] = {
	{ "getTime", 0, date_get_time },
	{ "getTimezoneOffset", 0, date_get_timezone_offset },
	{ "getYear", 0, date_get_year },
	{ "setTime", 1, date_set_time },
	{ "setYear", 1, date_set_year },
	{ "toDateString", 0, date_to_date_string },
	{ "toISOString", 0, date_to_iso_string },
	{ "toJSON", 1, date_to_json },
	{ "toLocaleDateString", 0, date_to_locale_date_string },
	{ "toLocaleString", 0, date_to_locale_string },
	{ "toLocaleTimeString", 0, date_to_locale_time_string },
	{ "toString", 0, date_to_string },
	{ "toTimeString", 0, date_to_time_string },
	{ "valueOf", 0, date_get_time },
	{ NULL, 0, NULL }
};

/*
 * Installs Date: the constructor with now, parse and UTC, and its
 * prototype (an ordinary object) with the getters, the setters and the
 * conversions.
 */
int
js_builtin_install_date(
	struct vm_realm *realm)
{
	const struct date_entry *entry;
	const struct date_method *method;
	struct vm_function *constructor;
	struct vm_function *function;
	struct vm_object *prototype;
	vm_value utc_string;
	int error;

	/* The zone the system's local time uses. */
	wb_os_time_zone_start();

	/* The prototype and the constructor with its functions. */
	prototype = vm_object_create(realm->heap, realm->object_prototype);
	if (prototype == NULL)
		return ENOMEM;
	realm->intrinsics[VM_INTRINSIC_DATE_PROTOTYPE] = prototype;
	error = js_builtin_constructor(realm, "Date", 7, date_call, date_construct, prototype, &constructor);
	if (error == 0)
		error = js_builtin_method(realm, &constructor->object, "now", 0, date_now_method);
	if (error == 0)
		error = js_builtin_method(realm, &constructor->object, "parse", 1, date_parse_method);
	if (error == 0)
		error = js_builtin_method(realm, &constructor->object, "UTC", 7, date_utc_method);
	if (error != 0)
		return error;

	/* The getters and setters, each knowing its field. */
	for (entry = date_fields; entry->name != NULL; entry++) {
		error = js_builtin_function(realm, entry->name, entry->length, entry->native, NULL, &function);
		if (error != 0)
			return error;
		function->data = vm_value_int32(entry->field);
		error = js_builtin_value(realm, prototype, entry->name, vm_value_cell(function), JS_BUILTIN_METHOD);
		if (error != 0)
			return error;
	}

	/* The other methods. */
	for (method = date_methods; method->name != NULL; method++) {
		error = js_builtin_method(realm, prototype, method->name, method->length, method->native);
		if (error != 0)
			return error;
	}

	/* toUTCString, and toGMTString the same function (Annex B). */
	error = js_builtin_function(realm, "toUTCString", 0, date_to_utc_string, NULL, &function);
	if (error != 0)
		return error;
	utc_string = vm_value_cell(function);
	error = js_builtin_value(realm, prototype, "toUTCString", utc_string, JS_BUILTIN_METHOD);
	if (error == 0)
		error = js_builtin_value(realm, prototype, "toGMTString", utc_string, JS_BUILTIN_METHOD);
	if (error != 0)
		return error;

	/* Date.prototype[Symbol.toPrimitive] (configurable only), whose default hint is string (ws074-p087). */
	error = js_builtin_function(realm, "[Symbol.toPrimitive]", 1, date_to_primitive, NULL, &function);
	if (error == 0)
		error = js_builtin_symbol_value(realm, prototype, VM_SYMBOL_TO_PRIMITIVE, vm_value_cell(function), VM_PROPERTY_CONFIGURABLE);
	if (error != 0)
		return error;

	/* Succeeded: Date is installed. */
	return 0;
}

/* Date called as a function: the current time as toString writes it. */
static int
date_call(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	char text[128];
	int error;

	/* The arguments do not matter. */
	(void)this_value;
	(void)args;
	(void)count;
	date_write(date_now(), DATE_FORM_STRING, text, sizeof(text));
	error = date_text(realm, text, result);
	if (error != 0)
		return error;

	/* Succeeded: the text. */
	return 0;
}

/*
 * new Date(...): now without arguments; with one, a Date's time value, a
 * string parsed or a number; with more, the year, month and the rest in
 * local time.
 */
static int
date_construct(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_object *prototype;
	struct vm_object *object;
	vm_value new_target;
	double time;
	int error;

	/* new.target first (the conversions below may run scripts). */
	(void)this_value;
	new_target = realm->new_target;

	/* The time value from the arguments. */
	if (count == 0) {
		/* No argument is the current time. */
		time = date_now();
	} else if (count == 1) {
		/* One argument is a Date, a date string or a time value. */
		error = date_from_value(realm, args[0], &time);
		if (error != 0)
			return error;
	} else {
		/* More are the parts in local time. */
		error = date_from_parts(realm, args, count, &time);
		if (error != 0)
			return error;
		time = date_clip(date_utc(time));
	}

	/* The object from new.target's prototype. */
	error = vm_construct_prototype(realm, new_target, realm->intrinsics[VM_INTRINSIC_DATE_PROTOTYPE], &prototype);
	if (error != 0)
		return error;
	object = vm_object_create(realm->heap, prototype);
	if (object == NULL)
		return ENOMEM;
	object->kind = VM_KIND_DATE;
	object->internal = vm_value_double(time);

	/* Succeeded: the Date. */
	*result = vm_value_cell(object);
	return 0;
}

/* Date.now(): the current time value. */
static int
date_now_method(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	/* The clock, in whole milliseconds. */
	(void)realm;
	(void)this_value;
	(void)args;
	(void)count;
	*result = vm_value_number(date_now());
	return 0;
}

/* Date.parse(string): the time value the string names, or NaN. */
static int
date_parse_method(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *string;
	int error;

	/* The string, parsed. */
	(void)this_value;
	error = vm_to_string(realm, js_argument(args, count, 0), &string);
	if (error != 0)
		return error;

	/* Succeeded: the time value. */
	*result = vm_value_number(date_parse(string));
	return 0;
}

/* Date.UTC(year, month, ...): the time value of the parts in UTC. */
static int
date_utc_method(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	double time;
	int error;

	/* The parts. */
	(void)this_value;
	error = date_from_parts(realm, args, count, &time);
	if (error != 0)
		return error;

	/* Succeeded: the clipped time value. */
	*result = vm_value_number(date_clip(time));
	return 0;
}

/* The getters: the field the function's data names, in local time or UTC; NaN for an invalid date. */
static int
date_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_function *callee;
	struct date_parts parts;
	double time;
	double value;
	int field;
	int error;

	/* Which field, then the time value. */
	(void)args;
	(void)count;
	callee = js_builtin_callee(realm);
	field = vm_value_as_int32(callee->data);
	error = date_this_time(realm, this_value, &time);
	if (error != 0)
		return error;
	if (time != time) {
		*result = vm_value_number(time);
		return 0;
	}

	/* The parts in local time, or in UTC. */
	if ((field & DATE_UTC) == 0)
		time = date_local(time);
	date_split(time, &parts);

	/* The field. */
	switch (field & ~DATE_UTC) {
	case DATE_FIELD_YEAR:
		value = parts.year;
		break;
	case DATE_FIELD_MONTH:
		value = parts.month;
		break;
	case DATE_FIELD_DATE:
		value = parts.date;
		break;
	case DATE_FIELD_HOURS:
		value = parts.hours;
		break;
	case DATE_FIELD_MINUTES:
		value = parts.minutes;
		break;
	case DATE_FIELD_SECONDS:
		value = parts.seconds;
		break;
	case DATE_FIELD_MS:
		value = parts.ms;
		break;
	default:
		value = parts.day;
		break;
	}

	/* Succeeded: the field's value. */
	*result = vm_value_number(value);
	return 0;
}

/*
 * The setters: the field the function's data names (and the smaller ones
 * the other arguments give) replaced, in local time or UTC; the new time
 * value, NaN for an invalid date (setFullYear starts an invalid one from
 * +0).
 */
static int
date_set(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_function *callee;
	struct date_parts parts;
	double numbers[4];
	double time;
	double day;
	double moment;
	unsigned most;
	int field;
	int local;
	int error;

	/* Which field, then the time value and the arguments as numbers (the arguments this setter takes). */
	callee = js_builtin_callee(realm);
	field = vm_value_as_int32(callee->data);
	local = (field & DATE_UTC) == 0;
	field &= ~DATE_UTC;
	error = date_this_time(realm, this_value, &time);
	if (error != 0)
		return error;
	most = 1;
	if (field == DATE_FIELD_HOURS)
		most = 4;
	else if (field == DATE_FIELD_MINUTES || field == DATE_FIELD_YEAR)
		most = 3;
	else if (field == DATE_FIELD_SECONDS || field == DATE_FIELD_MONTH)
		most = 2;
	error = date_numbers(realm, args, count, most, numbers);
	if (error != 0)
		return error;

	/* An invalid date stays invalid, but setFullYear starts from +0. */
	if (time != time && field != DATE_FIELD_YEAR) {
		*result = vm_value_number(time);
		return 0;
	}

	/* Splits the time value into the parts the setter replaces. */
	if (time != time)
		time = 0.0;
	else if (local)
		time = date_local(time);
	date_split(time, &parts);

	/* The field and the smaller ones given replace the parts. */
	switch (field) {
	case DATE_FIELD_YEAR:
		parts.year = numbers[0];
		if (count > 1)
			parts.month = numbers[1];
		if (count > 2)
			parts.date = numbers[2];
		break;
	case DATE_FIELD_MONTH:
		parts.month = numbers[0];
		if (count > 1)
			parts.date = numbers[1];
		break;
	case DATE_FIELD_DATE:
		parts.date = numbers[0];
		break;
	case DATE_FIELD_HOURS:
		parts.hours = numbers[0];
		if (count > 1)
			parts.minutes = numbers[1];
		if (count > 2)
			parts.seconds = numbers[2];
		if (count > 3)
			parts.ms = numbers[3];
		break;
	case DATE_FIELD_MINUTES:
		parts.minutes = numbers[0];
		if (count > 1)
			parts.seconds = numbers[1];
		if (count > 2)
			parts.ms = numbers[2];
		break;
	case DATE_FIELD_SECONDS:
		parts.seconds = numbers[0];
		if (count > 1)
			parts.ms = numbers[1];
		break;
	default:
		parts.ms = numbers[0];
		break;
	}

	/* The new time value (back to UTC from local time). */
	day = date_make_day(parts.year, parts.month, parts.date);
	moment = date_make_date(day, date_make_time(parts.hours, parts.minutes, parts.seconds, parts.ms));
	if (local)
		moment = date_utc(moment);
	error = date_store(this_value, date_clip(moment), result);
	if (error != 0)
		return error;

	/* Succeeded: the new time value. */
	return 0;
}

/* Date.prototype.getTime() and valueOf(): the time value. */
static int
date_get_time(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	double time;
	int error;

	/* this's time value. */
	(void)args;
	(void)count;
	error = date_this_time(realm, this_value, &time);
	if (error != 0)
		return error;

	/* Succeeded: the time value. */
	*result = vm_value_number(time);
	return 0;
}

/* Date.prototype.getTimezoneOffset(): minutes from local time to UTC. */
static int
date_get_timezone_offset(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	double time;
	int error;

	/* The time value; an invalid one has no offset. */
	(void)args;
	(void)count;
	error = date_this_time(realm, this_value, &time);
	if (error != 0)
		return error;
	if (time != time) {
		*result = vm_value_number(time);
		return 0;
	}

	/* Succeeded: (UTC - local) in minutes. */
	*result = vm_value_number((time - date_local(time)) / DATE_MS_PER_MINUTE);
	return 0;
}

/* Date.prototype.getYear() (Annex B): the local year less 1900. */
static int
date_get_year(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct date_parts parts;
	double time;
	int error;

	/* The time value; an invalid one has no year. */
	(void)args;
	(void)count;
	error = date_this_time(realm, this_value, &time);
	if (error != 0)
		return error;
	if (time != time) {
		*result = vm_value_number(time);
		return 0;
	}

	/* Succeeded: the year less 1900. */
	date_split(date_local(time), &parts);
	*result = vm_value_number(parts.year - 1900.0);
	return 0;
}

/* Date.prototype.setTime(time): a new time value. */
static int
date_set_time(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	double time;
	double number;
	int error;

	/* this must be a Date, then the number. */
	error = date_this_time(realm, this_value, &time);
	if (error == 0)
		error = vm_to_number(realm, js_argument(args, count, 0), &number);
	if (error == 0)
		error = date_store(this_value, date_clip(number), result);
	if (error != 0)
		return error;

	/* Succeeded: the new time value. */
	return 0;
}

/* Date.prototype.setYear(year) (Annex B): the local year (0 to 99 are 1900 to 1999). */
static int
date_set_year(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct date_parts parts;
	double time;
	double year;
	double moment;
	int error;

	/* The time value (an invalid one starts from +0) and the year. */
	error = date_this_time(realm, this_value, &time);
	if (error == 0)
		error = vm_to_number(realm, js_argument(args, count, 0), &year);
	if (error != 0)
		return error;
	if (year != year) {
		error = date_store(this_value, year, result);
		return error;
	}

	/* Two-digit years are the twentieth century's. */
	year = trunc(year);
	if (year >= 0.0 && year <= 99.0)
		year += 1900.0;
	if (time != time)
		time = 0.0;
	else
		time = date_local(time);
	date_split(time, &parts);
	moment = date_make_date(date_make_day(year, parts.month, parts.date), date_make_time(parts.hours, parts.minutes,
	    parts.seconds, parts.ms));
	error = date_store(this_value, date_clip(date_utc(moment)), result);
	if (error != 0)
		return error;

	/* Succeeded: the new time value. */
	return 0;
}

/* Date.prototype.toString(): the local date, time and zone. */
static int
date_to_string(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int error;

	/* The form. */
	(void)args;
	(void)count;
	error = date_format(realm, this_value, DATE_FORM_STRING, result);
	if (error != 0)
		return error;

	/* Succeeded: the text. */
	return 0;
}

/* Date.prototype.toDateString(): the local date. */
static int
date_to_date_string(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int error;

	/* The form. */
	(void)args;
	(void)count;
	error = date_format(realm, this_value, DATE_FORM_DATE, result);
	if (error != 0)
		return error;

	/* Succeeded: the text. */
	return 0;
}

/* Date.prototype.toTimeString(): the local time and zone. */
static int
date_to_time_string(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int error;

	/* The form. */
	(void)args;
	(void)count;
	error = date_format(realm, this_value, DATE_FORM_TIME, result);
	if (error != 0)
		return error;

	/* Succeeded: the text. */
	return 0;
}

/* Date.prototype.toUTCString() (and toGMTString): the date and time in UTC. */
static int
date_to_utc_string(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int error;

	/* The form. */
	(void)args;
	(void)count;
	error = date_format(realm, this_value, DATE_FORM_UTC, result);
	if (error != 0)
		return error;

	/* Succeeded: the text. */
	return 0;
}

/* Date.prototype.toISOString(): the ISO form in UTC; an invalid date is a RangeError. */
static int
date_to_iso_string(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	double time;
	int error;

	/* An invalid date has no ISO form. */
	(void)args;
	(void)count;
	error = date_this_time(realm, this_value, &time);
	if (error != 0)
		return error;
	if (time != time) {
		error = vm_throw_range_error(realm, "Invalid time value");
		return error;
	}

	/* The form. */
	error = date_format(realm, this_value, DATE_FORM_ISO, result);
	if (error != 0)
		return error;

	/* Succeeded: the text. */
	return 0;
}

/* Date.prototype.toJSON(key): toISOString's text, or null for a number that is not finite. */
static int
date_to_json(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	vm_value object;
	vm_value primitive;
	vm_value method;
	vm_value key;
	double number;
	int is_number;
	int callable;
	int finite;
	int error;

	/* The object's number primitive; one that is not finite is null. */
	(void)args;
	(void)count;
	error = vm_to_object(realm, this_value, &object);
	if (error == 0)
		error = vm_to_primitive(realm, object, VM_HINT_NUMBER, &primitive);
	if (error != 0)
		return error;
	is_number = vm_value_is_number(primitive);
	if (is_number) {
		error = vm_to_number(realm, primitive, &number);
		if (error != 0)
			return error;
		finite = isfinite(number);
		if (!finite) {
			*result = VM_VALUE_NULL;
			return 0;
		}
	}

	/* Looks up its toISOString, which a script may have replaced. */
	key = vm_key_from_ascii(realm->heap, "toISOString");
	if (key == VM_VALUE_EMPTY)
		return ENOMEM;
	error = vm_get(realm, object, key, &method);
	if (error != 0)
		return error;
	callable = vm_value_is_callable(method);
	if (!callable) {
		error = vm_throw_type_error(realm, "toISOString is not a function");
		return error;
	}

	/* Calls it on the object. */
	error = vm_call(realm, method, object, NULL, 0, result);
	if (error != 0)
		return error;

	/* Succeeded: its result. */
	return 0;
}

/* Date.prototype.toLocaleString(): the local date and time in the en-US form. */
static int
date_to_locale_string(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int error;

	/* The form. */
	(void)args;
	(void)count;
	error = date_format(realm, this_value, DATE_FORM_LOCALE, result);
	if (error != 0)
		return error;

	/* Succeeded: the text. */
	return 0;
}

/* Date.prototype.toLocaleDateString(): the local date in the en-US form. */
static int
date_to_locale_date_string(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int error;

	/* The form. */
	(void)args;
	(void)count;
	error = date_format(realm, this_value, DATE_FORM_LOCALE_DATE, result);
	if (error != 0)
		return error;

	/* Succeeded: the text. */
	return 0;
}

/* Date.prototype.toLocaleTimeString(): the local time in the en-US form. */
static int
date_to_locale_time_string(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int error;

	/* The form. */
	(void)args;
	(void)count;
	error = date_format(realm, this_value, DATE_FORM_LOCALE_TIME, result);
	if (error != 0)
		return error;

	/* Succeeded: the text. */
	return 0;
}

/*
 * Date.prototype[Symbol.toPrimitive](hint): this through toString first for
 * "string" and "default", through valueOf first for "number"; any other
 * hint is a TypeError.
 */
static int
date_to_primitive(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	const struct vm_string *name;
	vm_value hint;
	int is_object;
	int is_string;
	int matches;
	int order;
	int status;

	/* this must be an object. */
	*result = VM_VALUE_UNDEFINED;
	is_object = vm_value_is_object(this_value);
	if (!is_object) {
		status = vm_throw_type_error(realm, "Date.prototype[Symbol.toPrimitive] called on non-object");
		return status;
	}

	/* The hint must be one of the three names. */
	hint = js_argument(args, count, 0);
	is_string = vm_value_is_string(hint);
	if (!is_string) {
		status = vm_throw_type_error(realm, "Invalid hint");
		return status;
	}

	/* Which of the names it is: "number" tries valueOf first, "string" and "default" toString. */
	name = (const struct vm_string *)vm_value_as_cell(hint);
	order = -1;
	matches = vm_string_equal_ascii(name, "number");
	if (matches)
		order = VM_HINT_NUMBER;
	matches = vm_string_equal_ascii(name, "string");
	if (matches)
		order = VM_HINT_STRING;
	matches = vm_string_equal_ascii(name, "default");
	if (matches)
		order = VM_HINT_STRING;

	/* Anything else is refused. */
	if (order < 0) {
		status = vm_throw_type_error(realm, "Invalid hint");
		return status;
	}

	/* valueOf and toString in the hint's order. */
	status = vm_ordinary_to_primitive(realm, this_value, order, result);
	if (status != 0)
		return status;

	/* Succeeded: the primitive. */
	return 0;
}

/* Reads this's time value (thisTimeValue); anything but a Date is a TypeError. */
static int
date_this_time(
	struct vm_realm *realm,
	vm_value this_value,
	double *time)
{
	struct vm_object *object;
	int is_object;
	int error;

	/* A Date object. */
	is_object = vm_value_is_object(this_value);
	object = NULL;
	if (is_object)
		object = (struct vm_object *)vm_value_as_cell(this_value);
	if (object == NULL || object->kind != VM_KIND_DATE) {
		error = vm_throw_type_error(realm, "this is not a Date object.");
		return error;
	}

	/* Succeeded: its time value. */
	*time = vm_value_as_double(object->internal);
	return 0;
}

/* Stores a time value in this (a Date, checked before) and reports it. */
static int
date_store(
	vm_value this_value,
	double time,
	vm_value *result)
{
	struct vm_object *object;

	/* The object's internal value. */
	object = (struct vm_object *)vm_value_as_cell(this_value);
	object->internal = vm_value_double(time);

	/* Succeeded: the time value. */
	*result = vm_value_number(time);
	return 0;
}

/* Converts the first arguments (at most most of them) to numbers. */
static int
date_numbers(
	struct vm_realm *realm,
	const vm_value *args,
	unsigned count,
	unsigned most,
	double *numbers)
{
	unsigned index;
	int error;

	/* The first always (undefined is NaN), the rest when given. */
	numbers[0] = NAN;
	for (index = 0; index < most; index++) {
		if (index > 0 && index >= count)
			break;
		error = vm_to_number(realm, js_argument(args, count, index), &numbers[index]);
		if (error != 0)
			return error;
	}

	/* Succeeded: the numbers. */
	return 0;
}

/*
 * Makes the time value of new Date(value): a Date's own time value, or the
 * primitive's (a string is parsed, anything else is a number), clipped.
 */
static int
date_from_value(
	struct vm_realm *realm,
	vm_value value,
	double *time)
{
	struct vm_object *other;
	struct vm_string *string;
	vm_value primitive;
	int is_object;
	int is_string;
	int error;

	/* Finds a Date object, whose time value is copied without a conversion. */
	other = NULL;
	is_object = vm_value_is_object(value);
	if (is_object)
		other = (struct vm_object *)vm_value_as_cell(value);

	/* Copies a Date's time value. */
	if (other != NULL && other->kind == VM_KIND_DATE) {
		*time = vm_value_as_double(other->internal);
		return 0;
	}

	/* Converts anything else to a primitive, which may run the object's own methods. */
	error = vm_to_primitive(realm, value, VM_HINT_DEFAULT, &primitive);
	if (error != 0)
		return error;

	/* Parses a string as a date. */
	is_string = vm_value_is_string(primitive);
	if (is_string) {
		string = (struct vm_string *)vm_value_as_cell(primitive);
		*time = date_clip(date_parse(string));
		return 0;
	}

	/* Takes any other primitive as a time value. */
	error = vm_to_number(realm, primitive, time);
	if (error != 0)
		return error;

	/* Succeeded: the number clipped to the range a Date holds. */
	*time = date_clip(*time);
	return 0;
}

/*
 * Makes a time value from year, month, date, hours, minutes, seconds and
 * ms arguments (the date defaults to 1, the rest to 0; years 0 to 99 are
 * 1900 to 1999), without converting from local time.
 */
static int
date_from_parts(
	struct vm_realm *realm,
	const vm_value *args,
	unsigned count,
	double *time)
{
	double parts[7];
	double integer;
	unsigned index;
	int error;

	/* The parts given, and the defaults. */
	parts[0] = NAN;
	parts[1] = 0.0;
	parts[2] = 1.0;
	parts[3] = 0.0;
	parts[4] = 0.0;
	parts[5] = 0.0;
	parts[6] = 0.0;
	for (index = 0; index < 7U && index < count; index++) {
		error = vm_to_number(realm, args[index], &parts[index]);
		if (error != 0)
			return error;
	}

	/* Two-digit years. */
	if (parts[0] == parts[0]) {
		integer = trunc(parts[0]);
		if (integer >= 0.0 && integer <= 99.0)
			parts[0] = 1900.0 + integer;
	}

	/* Succeeded: the time value. */
	*time = date_make_date(date_make_day(parts[0], parts[1], parts[2]), date_make_time(parts[3], parts[4], parts[5], parts[6]));
	return 0;
}

/* Makes a string value of ASCII text. */
static int
date_text(
	struct vm_realm *realm,
	const char *text,
	vm_value *result)
{
	int error;

	/* The string. */
	error = js_builtin_string(realm, text, result);
	if (error != 0)
		return error;

	/* Succeeded: its value. */
	return 0;
}

/* Writes this's time value (this must be a Date) in a form; an invalid date is "Invalid Date". */
static int
date_format(
	struct vm_realm *realm,
	vm_value this_value,
	int form,
	vm_value *result)
{
	char text[160];
	double time;
	int error;

	/* The time value. */
	error = date_this_time(realm, this_value, &time);
	if (error != 0)
		return error;
	if (time != time) {
		error = date_text(realm, "Invalid Date", result);
		return error;
	}

	/* The text. */
	date_write(time, form, text, sizeof(text));
	error = date_text(realm, text, result);
	if (error != 0)
		return error;

	/* Succeeded: the text. */
	return 0;
}

/* Writes a valid time value in a form. */
static void
date_write(
	double time,
	int form,
	char *out,
	size_t size)
{
	struct date_parts parts;
	char year[16];
	char zone[64];
	double offset;
	int minutes;
	int hour;
	const char *sign;
	const char *meridiem;

	/* The parts in UTC for the UTC and ISO forms, in local time for the others. */
	offset = 0.0;
	if (form != DATE_FORM_UTC && form != DATE_FORM_ISO)
		offset = date_local(time) - time;
	date_split(time + offset, &parts);

	/* The year: at least four digits, a - before a negative one. */
	if (parts.year < 0.0) {
		snprintf(year, sizeof(year), "-%04.0f", -parts.year);
	} else {
		snprintf(year, sizeof(year), "%04.0f", parts.year);
	}

	/* The offset from UTC as +hhmm. */
	minutes = (int)(offset / DATE_MS_PER_MINUTE);
	sign = "+";
	if (minutes < 0) {
		sign = "-";
		minutes = -minutes;
	}

	/* The zone's name, which toString and toTimeString write in parentheses. */
	date_zone(time, zone, sizeof(zone));

	/* The form. */
	switch (form) {
	case DATE_FORM_STRING:
		snprintf(out, size, "%s %s %02.0f %s %02.0f:%02.0f:%02.0f GMT%s%02d%02d (%s)", date_day_names[(int)parts.day],
		    date_month_names[(int)parts.month], parts.date, year, parts.hours, parts.minutes, parts.seconds, sign, minutes / 60,
		    minutes % 60, zone);
		break;
	case DATE_FORM_DATE:
		snprintf(out, size, "%s %s %02.0f %s", date_day_names[(int)parts.day], date_month_names[(int)parts.month], parts.date,
		    year);
		break;
	case DATE_FORM_TIME:
		snprintf(out, size, "%02.0f:%02.0f:%02.0f GMT%s%02d%02d (%s)", parts.hours, parts.minutes, parts.seconds, sign,
		    minutes / 60, minutes % 60, zone);
		break;
	case DATE_FORM_UTC:
		snprintf(out, size, "%s, %02.0f %s %s %02.0f:%02.0f:%02.0f GMT", date_day_names[(int)parts.day], parts.date,
		    date_month_names[(int)parts.month], year, parts.hours, parts.minutes, parts.seconds);
		break;
	case DATE_FORM_ISO:
		/* Years beyond 0 to 9999 take six digits and a sign. */
		if (parts.year < 0.0) {
			snprintf(year, sizeof(year), "-%06.0f", -parts.year);
		} else if (parts.year > 9999.0) {
			snprintf(year, sizeof(year), "+%06.0f", parts.year);
		}

		/* The date, the time with milliseconds and Z. */
		snprintf(out, size, "%s-%02.0f-%02.0fT%02.0f:%02.0f:%02.0f.%03.0fZ", year, parts.month + 1.0, parts.date, parts.hours,
		    parts.minutes, parts.seconds, parts.ms);
		break;
	default:
		/* en-US: m/d/yyyy, h:mm:ss AM. */
		hour = (int)parts.hours % 12;
		if (hour == 0)
			hour = 12;
		meridiem = "AM";
		if (parts.hours >= 12.0)
			meridiem = "PM";
		if (form == DATE_FORM_LOCALE_DATE) {
			snprintf(out, size, "%.0f/%.0f/%.0f", parts.month + 1.0, parts.date, parts.year);
		} else if (form == DATE_FORM_LOCALE_TIME) {
			snprintf(out, size, "%d:%02.0f:%02.0f %s", hour, parts.minutes, parts.seconds, meridiem);
		} else {
			snprintf(out, size, "%.0f/%.0f/%.0f, %d:%02.0f:%02.0f %s", parts.month + 1.0, parts.date, parts.year, hour,
			    parts.minutes, parts.seconds, meridiem);
		}

		break;
	}
}

/* Writes the local zone's name at a moment (the long names browsers write for the common zones). */
static void
date_zone(
	double time,
	char *out,
	size_t size)
{
	const struct date_zone_name *entry;
	char zone[WB_OS_ZONE_NAME_MAX + 1U];
	int64_t seconds;
	const char *name;
	int differs;
	int error;

	/* The zone's name at the moment (an abbreviation on POSIX systems); a zone the system cannot name is UTC. */
	name = "UTC";
	seconds = (int64_t)floor(time / DATE_MS_PER_SECOND);
	error = wb_os_zone_name(seconds, zone, sizeof(zone));
	if (error == 0 && zone[0] != '\0')
		name = zone;

	/* Finds the long name browsers write for a common abbreviation. */
	for (entry = date_zone_names; entry->abbreviation != NULL; entry++) {
		differs = strcmp(name, entry->abbreviation);
		if (differs == 0) {
			name = entry->name;
			break;
		}
	}

	/* Writes the long name, or the abbreviation when there is none. */
	snprintf(out, size, "%s", name);
}

/* Reads the current time value (whole milliseconds). */
static double
date_now(void)
{
	int64_t now;
	int64_t milliseconds;

	/* The real-time clock. */
	now = wb_os_wall_ns();

	/* The whole milliseconds, rounded down (also before 1970). */
	milliseconds = now / 1000000;
	if (now % 1000000 < 0)
		milliseconds--;
	return (double)milliseconds;
}

/* Counts the days from 1970-01-01 to a date of the proleptic Gregorian calendar (month 1 to 12). */
static double
date_days_from_civil(
	double year,
	double month,
	double day)
{
	double era;
	double year_of_era;
	double day_of_year;
	double day_of_era;
	double shifted;

	/* The year starts in March, so that the leap day is last. */
	if (month <= 2.0)
		year -= 1.0;
	era = floor(year / 400.0);
	year_of_era = year - era * 400.0;
	shifted = month + 9.0;
	if (month > 2.0)
		shifted = month - 3.0;
	day_of_year = floor((153.0 * shifted + 2.0) / 5.0) + day - 1.0;
	day_of_era = year_of_era * 365.0 + floor(year_of_era / 4.0) - floor(year_of_era / 100.0) + day_of_year;

	/* The days. */
	return era * 146097.0 + day_of_era - 719468.0;
}

/* Finds the date of a day count from 1970-01-01 (month 1 to 12). */
static void
date_civil_from_days(
	double days,
	double *year,
	double *month,
	double *day)
{
	double era;
	double day_of_era;
	double year_of_era;
	double day_of_year;
	double shifted;

	/* The 400-year era and the day in it, from a March-based year. */
	days += 719468.0;
	era = floor(days / 146097.0);
	day_of_era = days - era * 146097.0;
	year_of_era = floor((day_of_era - floor(day_of_era / 1460.0) + floor(day_of_era / 36524.0) - floor(day_of_era / 146096.0)) /
	    365.0);
	day_of_year = day_of_era - (365.0 * year_of_era + floor(year_of_era / 4.0) - floor(year_of_era / 100.0));
	shifted = floor((5.0 * day_of_year + 2.0) / 153.0);

	/* The civil date (January and February belong to the next year). */
	*day = day_of_year - floor((153.0 * shifted + 2.0) / 5.0) + 1.0;
	*month = shifted + 3.0;
	if (shifted >= 10.0)
		*month = shifted - 9.0;
	*year = year_of_era + era * 400.0;
	if (*month <= 2.0)
		*year += 1.0;
}

/* Makes a time of day in milliseconds (MakeTime); NaN when a part is not finite. */
static double
date_make_time(
	double hours,
	double minutes,
	double seconds,
	double ms)
{
	double time;
	int finite;

	/*
	 * Adds up the whole parts.  A part that is not finite leaves the sum
	 * infinite or NaN, so the one test below covers every part.
	 */
	time = trunc(hours) * DATE_MS_PER_HOUR;
	time += trunc(minutes) * DATE_MS_PER_MINUTE;
	time += trunc(seconds) * DATE_MS_PER_SECOND;
	time += trunc(ms);

	/* A part that is not finite makes no time. */
	finite = isfinite(time);
	if (!finite)
		return NAN;

	/* The milliseconds into the day. */
	return time;
}

/* Makes a day number from a year, a month (any integer) and a date (MakeDay); NaN when a part is not finite. */
static double
date_make_day(
	double year,
	double month,
	double date)
{
	double whole_year;
	double whole_month;
	double sum;
	int finite;

	/* A part that is not finite (which leaves their sum so) makes no day. */
	sum = year + month + date;
	finite = isfinite(sum);
	if (!finite)
		return NAN;

	/* The month folded into the year; a year far past any date is no day. */
	whole_year = trunc(year) + floor(trunc(month) / 12.0);
	whole_month = trunc(month) - floor(trunc(month) / 12.0) * 12.0;
	if (whole_year > 400000.0 || whole_year < -400000.0)
		return NAN;

	/* The first of the month, then the date. */
	return date_days_from_civil(whole_year, whole_month + 1.0, 1.0) + trunc(date) - 1.0;
}

/* Makes a time value from a day and a time of day (MakeDate). */
static double
date_make_date(
	double day,
	double time)
{
	double value;
	int finite;

	/* The milliseconds, if finite. */
	value = day * DATE_MS_PER_DAY + time;
	finite = isfinite(value);
	if (!finite)
		return NAN;

	/* The time value. */
	return value;
}

/* Clips a time value to the range a Date holds (TimeClip): whole, and NaN beyond it. */
static double
date_clip(
	double time)
{
	int finite;

	/* NaN and anything beyond 100 000 000 days. */
	finite = isfinite(time);
	if (!finite)
		return NAN;
	if (time > DATE_TIME_MAX || time < -DATE_TIME_MAX)
		return NAN;

	/* The whole milliseconds (+0 for -0). */
	return trunc(time) + 0.0;
}

/* Measures the local zone's offset from UTC at a UTC moment, in milliseconds. */
static double
date_offset_at(
	double time)
{
	int64_t seconds;
	long offset;
	int finite;
	int error;

	/* The system's offset at the moment. */
	finite = isfinite(time);
	if (!finite)
		return 0.0;
	seconds = (int64_t)floor(time / DATE_MS_PER_SECOND);
	error = wb_os_local_offset(seconds, &offset);
	if (error != 0)
		return 0.0;

	/* In milliseconds. */
	return (double)offset * DATE_MS_PER_SECOND;
}

/* Converts a UTC time value to local time (LocalTime). */
static double
date_local(
	double time)
{
	/* The offset at that moment. */
	return time + date_offset_at(time);
}

/* Converts a local time value to UTC (UTC): the offset at the moment the local time names. */
static double
date_utc(
	double time)
{
	double guess;
	int finite;

	/* An invalid time stays so. */
	finite = isfinite(time);
	if (!finite)
		return time;

	/* The offset at the local time taken as UTC, then at the moment that gives. */
	guess = date_offset_at(time);
	return time - date_offset_at(time - guess);
}

/* Splits a time value (UTC or local) into its parts. */
static void
date_split(
	double time,
	struct date_parts *parts)
{
	double day;
	double within;

	/* The day and the time within it. */
	day = floor(time / DATE_MS_PER_DAY);
	within = time - day * DATE_MS_PER_DAY;
	date_civil_from_days(day, &parts->year, &parts->month, &parts->date);
	parts->month -= 1.0;

	/* The time of day. */
	parts->hours = floor(within / DATE_MS_PER_HOUR);
	parts->minutes = floor(fmod(within, DATE_MS_PER_HOUR) / DATE_MS_PER_MINUTE);
	parts->seconds = floor(fmod(within, DATE_MS_PER_MINUTE) / DATE_MS_PER_SECOND);
	parts->ms = fmod(within, DATE_MS_PER_SECOND);

	/* The day of the week: 1970-01-01 was a Thursday. */
	parts->day = fmod(day + 4.0, 7.0);
	if (parts->day < 0.0)
		parts->day += 7.0;
}

/* Reads a date string (Date.parse): the ISO format, or the common other forms; NaN for anything else. */
static double
date_parse(
	const struct vm_string *string)
{
	char text[DATE_TEXT_MAX + 1U];
	double time;
	size_t index;
	uint16_t unit;
	int iso;

	/* ASCII only, and not too long. */
	if (string->length > DATE_TEXT_MAX)
		return NAN;
	for (index = 0; index < string->length; index++) {
		unit = vm_string_at(string, index);
		if (unit == 0 || unit > 0x7FU)
			return NAN;
		text[index] = (char)unit;
	}

	/* Ends the copy as a C string. */
	text[string->length] = '\0';

	/* The ISO format first. */
	iso = date_parse_iso(text, &time);
	if (iso)
		return time;

	/* Then the others. */
	return date_parse_legacy(text);
}

/*
 * Reads the specification's date time string format: YYYY, YYYY-MM or
 * YYYY-MM-DD (a six-digit year with a sign too), optionally THH:mm,
 * THH:mm:ss or THH:mm:ss.sss and Z or an offset; a date alone is UTC, a
 * date with a time and no zone local time.  0 when the text is not in the
 * format; *time is NaN for values out of range.
 */
static int
date_parse_iso(
	const char *text,
	double *time)
{
	double year;
	double month;
	double day;
	double hours;
	double minutes;
	double seconds;
	double ms;
	double offset;
	double value;
	double sign;
	size_t index;
	size_t digits;
	int has_time;
	int has_seconds;
	int has_zone;
	int valid;

	/* The year: four digits, or a sign and six. */
	index = 0;
	sign = 1.0;
	if (text[0] == '+' || text[0] == '-') {
		if (text[0] == '-')
			sign = -1.0;
		index = 1;
		valid = date_digits(text, &index, 6, &year);
		if (!valid)
			return 0;
		if (sign < 0.0 && year == 0.0) {
			*time = NAN;
			return 1;
		}

		/* The sign applies to the whole year; -000000 was refused above. */
		year *= sign;
	} else {
		valid = date_digits(text, &index, 4, &year);
		if (!valid)
			return 0;
	}

	/* -MM and -DD. */
	month = 1.0;
	day = 1.0;
	if (text[index] == '-') {
		index++;
		valid = date_digits(text, &index, 2, &month);
		if (!valid)
			return 0;
		if (text[index] == '-') {
			index++;
			valid = date_digits(text, &index, 2, &day);
			if (!valid)
				return 0;
		}
	}

	/* THH:mm[:ss[.sss]]. */
	hours = 0.0;
	minutes = 0.0;
	seconds = 0.0;
	ms = 0.0;
	has_time = 0;
	has_seconds = 0;
	if (text[index] == 'T' || text[index] == 't') {
		has_time = 1;
		index++;
		valid = date_digits(text, &index, 2, &hours);
		if (!valid || text[index] != ':')
			return 0;
		index++;
		valid = date_digits(text, &index, 2, &minutes);
		if (!valid)
			return 0;
		if (text[index] == ':') {
			index++;
			valid = date_digits(text, &index, 2, &seconds);
			if (!valid)
				return 0;
			has_seconds = 1;
		}

		/* A fraction follows the seconds only, and needs a digit at least. */
		if (has_seconds && text[index] == '.') {
			index++;
			digits = date_fraction(text, &index, &ms);
			if (digits == 0)
				return 0;
		}
	}

	/* Z or ±HH:mm. */
	has_zone = 0;
	offset = 0.0;
	if (has_time && (text[index] == 'Z' || text[index] == 'z')) {
		has_zone = 1;
		index++;
	} else if (has_time && (text[index] == '+' || text[index] == '-')) {
		sign = 1.0;
		if (text[index] == '-')
			sign = -1.0;
		index++;
		valid = date_digits(text, &index, 2, &value);
		if (!valid || text[index] != ':')
			return 0;
		index++;
		offset = value * 60.0;
		valid = date_digits(text, &index, 2, &value);
		if (!valid)
			return 0;
		offset = sign * (offset + value);
		has_zone = 1;
	}

	/* Nothing may follow. */
	if (text[index] != '\0')
		return 0;

	/* The ranges (24:00 only as the end of a day). */
	*time = NAN;
	if (month < 1.0 || month > 12.0 || day < 1.0 || day > 31.0)
		return 1;
	if (hours > 24.0 || minutes > 59.0 || seconds > 59.0)
		return 1;
	if (hours == 24.0 && (minutes != 0.0 || seconds != 0.0 || ms != 0.0))
		return 1;

	/*
	 * The day in the month.  A day past the month's end (February 30)
	 * rolls into the next month, as Chromium reads it, rather than
	 * making the string invalid.
	 */
	value = date_make_day(year, month - 1.0, day);

	/* The time value: UTC for a date alone or with a zone, local time otherwise. */
	value = date_make_date(value, date_make_time(hours, minutes, seconds, ms));
	if (has_zone)
		value -= offset * DATE_MS_PER_MINUTE;
	else if (has_time)
		value = date_utc(value);
	*time = date_clip(value);
	return 1;
}

/*
 * Reads the other common date forms: the day and month names, numbers
 * separated by /, - or . (m/d/y, or y/m/d when the first has more than two
 * digits), a time h:mm[:ss[.sss]] with AM or PM, GMT, UTC or Z and a +hhmm
 * offset, and comments in parentheses; local time without a zone.
 */
static double
date_parse_legacy(
	const char *text)
{
	struct date_legacy state;
	size_t index;
	int valid;
	char next;

	/* Nothing read yet: no time, no month's name, no zone. */
	memset(&state, 0, sizeof(state));
	state.hours = -1.0;
	state.named_month = -1;

	/* Reads each token into the state; one that belongs to no date form ends the parse. */
	index = 0;
	while (text[index] != '\0') {
		next = text[index];

		/* Spaces, commas and the separators between numbers. */
		if (next == ' ' || next == ',' || next == '\t' || next == '/' || next == '.') {
			index++;
			continue;
		}

		/* A comment in parentheses. */
		if (next == '(') {
			date_legacy_comment(text, &index);
			continue;
		}

		/* A word: a month, a day, AM or PM, a zone. */
		if ((next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z')) {
			valid = date_legacy_word(text, &index, &state);
			if (!valid)
				return NAN;
			continue;
		}

		/* A sign after the time or a zone is an offset. */
		if ((next == '+' || next == '-') && state.after_time) {
			date_legacy_offset(text, &index, &state);
			continue;
		}

		/* A - between the numbers of a date. */
		if (next == '-') {
			index++;
			continue;
		}

		/* A number: a time when a : follows, a part of the date otherwise. */
		if (next >= '0' && next <= '9') {
			valid = date_legacy_number(text, &index, &state);
			if (!valid)
				return NAN;
			continue;
		}

		/* Anything else is not a date. */
		return NAN;
	}

	/* The time value the tokens name. */
	return date_legacy_value(&state);
}

/* Skips a comment in parentheses (they may nest) at an index. */
static void
date_legacy_comment(
	const char *text,
	size_t *index)
{
	int depth;

	/* Counts the parentheses until the one that closes the first. */
	depth = 0;
	while (text[*index] != '\0') {
		if (text[*index] == '(')
			depth++;
		if (text[*index] == ')')
			depth--;
		(*index)++;
		if (depth == 0)
			break;
	}
}

/* Reads a word of a date: a month's or a day's name, AM or PM, a zone or T; 0 for any other word. */
static int
date_legacy_word(
	const char *text,
	size_t *index,
	struct date_legacy *state)
{
	char word[16];
	size_t length;
	int found;
	int differs;

	/* Copies the letters in lower case (a long word is cut, since no name is that long). */
	length = 0;
	while ((text[*index] >= 'a' && text[*index] <= 'z') || (text[*index] >= 'A' && text[*index] <= 'Z')) {
		if (length + 1U < sizeof(word)) {
			word[length] = (char)(text[*index] | 0x20);
			length++;
		}

		/* Letters past the buffer are skipped. */
		(*index)++;
	}

	/* Ends the copy as a C string. */
	word[length] = '\0';

	/* A month's name gives the month. */
	found = date_month_of(word);
	if (found >= 0) {
		state->named_month = found;
		return 1;
	}

	/* AM and PM place the hour in the day's half. */
	if (length == 2U && word[1] == 'm') {
		if (word[0] == 'a' || word[0] == 'p') {
			state->meridiem = word[0];
			return 1;
		}
	}

	/* GMT, UTC, UT and Z mean UTC, and an offset may follow. */
	found = date_legacy_is_zone(word);
	if (found) {
		state->has_zone = 1;
		state->after_time = 1;
		return 1;
	}

	/* T between the date and the time. */
	differs = strcmp(word, "t");
	if (differs == 0)
		return 1;

	/* A day's name adds nothing. */
	found = date_day_of(word);
	if (found >= 0)
		return 1;

	/* Any other word is not a date. */
	return 0;
}

/* Tells whether a lower-case word names UTC: gmt, utc, ut or z. */
static int
date_legacy_is_zone(
	const char *word)
{
	static const char *const names[] = { "gmt", "utc", "ut", "z", NULL };
	unsigned index;
	int differs;

	/* Compares the word with each name. */
	for (index = 0; names[index] != NULL; index++) {
		differs = strcmp(word, names[index]);
		if (differs == 0)
			return 1;
	}

	/* No zone's name. */
	return 0;
}

/* Reads an offset from UTC at a sign: +hhmm, +hh:mm or +hh. */
static void
date_legacy_offset(
	const char *text,
	size_t *index,
	struct date_legacy *state)
{
	double sign;
	double value;
	double minutes;
	int digits;

	/* The sign. */
	sign = 1.0;
	if (text[*index] == '-')
		sign = -1.0;
	(*index)++;

	/* The hours, or the hours and minutes run together. */
	digits = 0;
	value = 0.0;
	while (text[*index] >= '0' && text[*index] <= '9') {
		value = value * 10.0 + (double)(text[*index] - '0');
		digits++;
		(*index)++;
	}

	/* Minutes after a colon, two digits of hours alone, or hhmm. */
	if (text[*index] == ':') {
		(*index)++;
		minutes = 0.0;
		while (text[*index] >= '0' && text[*index] <= '9') {
			minutes = minutes * 10.0 + (double)(text[*index] - '0');
			(*index)++;
		}

		/* The hours and the minutes after the colon. */
		state->offset = sign * (value * 60.0 + minutes);
	} else if (digits <= 2) {
		state->offset = sign * value * 60.0;
	} else {
		state->offset = sign * (floor(value / 100.0) * 60.0 + fmod(value, 100.0));
	}

	/* The date now has a zone. */
	state->has_zone = 1;
}

/* Reads a number: the time h:mm[:ss[.sss]] when a colon follows, a part of the date otherwise; 0 for a fourth part. */
static int
date_legacy_number(
	const char *text,
	size_t *index,
	struct date_legacy *state)
{
	double value;
	int digits;

	/* The digits. */
	value = 0.0;
	digits = 0;
	while (text[*index] >= '0' && text[*index] <= '9') {
		value = value * 10.0 + (double)(text[*index] - '0');
		digits++;
		(*index)++;
	}

	/* The first number with a colon is the time's hour. */
	if (text[*index] == ':' && state->hours < 0.0) {
		(*index)++;
		state->hours = value;
		date_legacy_time(text, index, state);
		return 1;
	}

	/* A date has three numbers at most. */
	if (state->count >= 3U)
		return 0;

	/* Succeeded: one more part of the date, with its digits (which tell a year from a day). */
	state->numbers[state->count] = value;
	state->number_digits[state->count] = digits;
	state->count++;
	return 1;
}

/* Reads the rest of a time after its hour and colon: mm[:ss[.sss]]. */
static void
date_legacy_time(
	const char *text,
	size_t *index,
	struct date_legacy *state)
{
	/* The minutes. */
	state->minutes = 0.0;
	while (text[*index] >= '0' && text[*index] <= '9') {
		state->minutes = state->minutes * 10.0 + (double)(text[*index] - '0');
		(*index)++;
	}

	/* The seconds after another colon, and their fraction after a dot. */
	if (text[*index] == ':') {
		(*index)++;
		while (text[*index] >= '0' && text[*index] <= '9') {
			state->seconds = state->seconds * 10.0 + (double)(text[*index] - '0');
			(*index)++;
		}

		/* The fraction of the second. */
		if (text[*index] == '.') {
			(*index)++;
			date_fraction(text, index, &state->ms);
		}
	}

	/* A sign may now start an offset. */
	state->after_time = 1;
}

/* Makes the time value of the tokens a legacy date string gave; NaN when they make no date. */
static double
date_legacy_value(
	const struct date_legacy *state)
{
	double year;
	double month;
	double day;
	double hours;
	double moment;

	/* The date's parts: with a month's name, the day and the year; otherwise m/d/y or y/m/d. */
	year = NAN;
	month = NAN;
	day = 1.0;
	if (state->named_month >= 0) {
		month = (double)state->named_month;
		if (state->count == 2U && (state->number_digits[0] > 2 || state->numbers[0] > 31.0)) {
			year = state->numbers[0];
			day = state->numbers[1];
		} else if (state->count == 2U) {
			day = state->numbers[0];
			year = state->numbers[1];
		} else if (state->count == 1U) {
			day = state->numbers[0];
			year = 2001.0;
		}
	} else if (state->count == 3U && state->number_digits[0] > 2) {
		year = state->numbers[0];
		month = state->numbers[1] - 1.0;
		day = state->numbers[2];
	} else if (state->count == 3U) {
		month = state->numbers[0] - 1.0;
		day = state->numbers[1];
		year = state->numbers[2];
	} else if (state->count == 2U && state->number_digits[0] > 2) {
		year = state->numbers[0];
		month = state->numbers[1] - 1.0;
	}

	/* A date needs a year and a month, both in range. */
	if (year != year || month != month)
		return NAN;
	if (month < 0.0 || month > 11.0 || day < 1.0 || day > 31.0)
		return NAN;

	/* Two-digit years: 00 to 49 are 2000 to 2049, 50 to 99 1950 to 1999. */
	if (year < 50.0)
		year += 2000.0;
	else if (year < 100.0)
		year += 1900.0;

	/* The hour: none is midnight, and with AM or PM it must be 1 to 12. */
	hours = state->hours;
	if (hours < 0.0)
		hours = 0.0;
	if (state->meridiem != 0 && (hours < 1.0 || hours > 12.0))
		return NAN;

	/* 12 AM is midnight, and PM is the afternoon. */
	if (state->meridiem == 'a' && hours == 12.0)
		hours = 0.0;
	if (state->meridiem == 'p' && hours < 12.0)
		hours += 12.0;

	/* A time past the day's end is not one. */
	if (hours > 24.0 || state->minutes > 59.0 || state->seconds > 59.0)
		return NAN;

	/* The moment in the zone given, or in local time without one. */
	moment = date_make_date(date_make_day(year, month, day), date_make_time(hours, state->minutes, state->seconds, state->ms));
	if (state->has_zone)
		moment -= state->offset * DATE_MS_PER_MINUTE;
	else
		moment = date_utc(moment);

	/* Succeeded: the time value, clipped to the range a Date holds. */
	return date_clip(moment);
}

/* Reads exactly a number of digits at an index; 0 when they are not there. */
static int
date_digits(
	const char *text,
	size_t *index,
	unsigned count,
	double *value)
{
	unsigned read;
	double result;

	/* Each digit. */
	result = 0.0;
	for (read = 0; read < count; read++) {
		if (text[*index + read] < '0' || text[*index + read] > '9')
			return 0;
		result = result * 10.0 + (double)(text[*index + read] - '0');
	}

	/* Succeeded: the value, and the index past the digits. */
	*index += count;
	*value = result;
	return 1;
}

/*
 * Reads the digits of a fraction of a second as milliseconds (the first
 * three count, fewer are scaled up); the count of digits read.
 */
static size_t
date_fraction(
	const char *text,
	size_t *index,
	double *ms)
{
	double value;
	size_t digits;

	/* Accumulates the first three digits and skips the rest. */
	value = 0.0;
	digits = 0;
	while (text[*index] >= '0' && text[*index] <= '9') {
		if (digits < 3U)
			value = value * 10.0 + (double)(text[*index] - '0');
		digits++;
		(*index)++;
	}

	/* Scales tenths and hundredths up to milliseconds. */
	*ms = value;
	if (digits == 1U)
		*ms = value * 100.0;
	else if (digits == 2U)
		*ms = value * 10.0;

	/* Succeeded: how many digits there were. */
	return digits;
}

/* Finds the day of the week a word names (its first three letters at least); -1 for none. */
static int
date_day_of(
	const char *word)
{
	char lower[4];
	size_t length;
	int day;
	int same;

	/* Three letters at least. */
	length = strlen(word);
	if (length < 3U)
		return -1;

	/* Each day's short name. */
	for (day = 0; day < 7; day++) {
		lower[0] = (char)(date_day_names[day][0] | 0x20);
		lower[1] = date_day_names[day][1];
		lower[2] = date_day_names[day][2];
		lower[3] = '\0';
		same = strncmp(word, lower, 3);
		if (same == 0)
			return day;
	}

	/* No day. */
	return -1;
}

/* Finds the month a word names (its first three letters at least); -1 for none. */
static int
date_month_of(
	const char *word)
{
	char lower[4];
	size_t length;
	int month;
	int same;

	/* Three letters at least. */
	length = strlen(word);
	if (length < 3U)
		return -1;

	/* Each month's short name. */
	for (month = 0; month < 12; month++) {
		lower[0] = (char)(date_month_names[month][0] | 0x20);
		lower[1] = date_month_names[month][1];
		lower[2] = date_month_names[month][2];
		lower[3] = '\0';
		same = strncmp(word, lower, 3);
		if (same == 0)
			return month;
	}

	/* No month. */
	return -1;
}
