/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "userland/base/common/command.h"
#include "userland/base/m4/m4.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define M4_DEPTH_MAX 128
#define M4_ARGUMENT_MAX 128
#define M4_OUTPUT_MAX (16U * 1024U * 1024U)
#define M4_DIVERSION_COUNT 10

struct builder {
	char *text;
	size_t length;
	size_t capacity;
};

struct definition {
	char *name;
	char *value;
};

struct arguments {
	char **item;
	size_t count;
};

struct m4_context {
	struct definition *definition;
	size_t definition_count;
	size_t definition_capacity;
	struct builder output;
	struct builder diversion[M4_DIVERSION_COUNT];
	int current_diversion;
	char quote_open[16];
	char quote_close[16];
	char error[256];
	const char *source;
	unsigned line;
	int discard_line;
};

static int expand_text(struct m4_context *, const char *, size_t,
		       struct builder *, unsigned);

static void
set_error(struct m4_context *context, const char *message)
{
	if (context->error[0] == '\0')
		(void)snprintf(context->error, sizeof(context->error),
			       "%s:%u: %s",
			       context->source == NULL ? "-" : context->source,
			       context->line, message);
}

const char *
m4_error(const struct m4_context *context)
{
	return context->error[0] == '\0' ? NULL : context->error;
}

static void
builder_free(struct builder *builder)
{
	free(builder->text);
	memset(builder, 0, sizeof(*builder));
}

static int
builder_append(struct m4_context *context, struct builder *builder,
	       const char *text, size_t length)
{
	size_t needed;
	char *grown;

	if (length > M4_OUTPUT_MAX - builder->length) {
		set_error(context, "output or diversion limit exceeded");
		errno = EOVERFLOW;
		return -1;
	}
	needed = builder->length + length + 1;
	if (needed > builder->capacity) {
		size_t capacity =
		    builder->capacity == 0 ? 256 : builder->capacity;

		while (capacity < needed) {
			if (capacity > M4_OUTPUT_MAX / 2) {
				capacity = needed;
				break;
			}
			capacity *= 2;
		}
		grown = realloc(builder->text, capacity);
		if (grown == NULL) {
			set_error(context, "out of memory");
			return -1;
		}
		builder->text = grown;
		builder->capacity = capacity;
	}
	memcpy(builder->text + builder->length, text, length);
	builder->length += length;
	builder->text[builder->length] = '\0';
	return 0;
}

static int
emit(struct m4_context *context, struct builder *capture, const char *text,
     size_t length)
{
	struct builder *target = capture;

	if (target == NULL) {
		if (context->current_diversion < 0)
			return 0;
		target = context->current_diversion == 0
			     ? &context->output
			     : &context->diversion[context->current_diversion];
	}
	return builder_append(context, target, text, length);
}

static int
starts_with(const char *text, size_t length, size_t offset, const char *word)
{
	size_t word_length = strlen(word);

	return word_length != 0 && word_length <= length - offset &&
	       memcmp(text + offset, word, word_length) == 0;
}

static int
is_name_start(char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static int
is_name_character(char c)
{
	return is_name_start(c) || (c >= '0' && c <= '9');
}

static struct definition *
find_definition(struct m4_context *context, const char *name, size_t length)
{
	size_t index = context->definition_count;

	while (index != 0) {
		index--;
		if (strlen(context->definition[index].name) == length &&
		    memcmp(context->definition[index].name, name, length) == 0)
			return &context->definition[index];
	}
	return NULL;
}

int
m4_define(struct m4_context *context, const char *name, const char *value)
{
	struct definition *definition =
	    find_definition(context, name, strlen(name));
	char *copy = strdup(value);

	if (copy == NULL)
		return -1;
	if (definition != NULL) {
		free(definition->value);
		definition->value = copy;
		return 0;
	}
	if (context->definition_count == context->definition_capacity) {
		size_t capacity = context->definition_capacity == 0
				      ? 32
				      : context->definition_capacity * 2;
		struct definition *grown;

		if (capacity < context->definition_capacity ||
		    capacity > SIZE_MAX / sizeof(*grown)) {
			free(copy);
			errno = EOVERFLOW;
			return -1;
		}
		grown = realloc(context->definition, capacity * sizeof(*grown));
		if (grown == NULL) {
			free(copy);
			return -1;
		}
		context->definition = grown;
		context->definition_capacity = capacity;
	}
	definition = &context->definition[context->definition_count];
	definition->name = strdup(name);
	if (definition->name == NULL) {
		free(copy);
		return -1;
	}
	definition->value = copy;
	context->definition_count++;
	return 0;
}

int
m4_undefine(struct m4_context *context, const char *name)
{
	size_t index;

	for (index = context->definition_count; index != 0; index--) {
		if (strcmp(context->definition[index - 1].name, name) == 0) {
			free(context->definition[index - 1].name);
			free(context->definition[index - 1].value);
			memmove(context->definition + index - 1,
				context->definition + index,
				(context->definition_count - index) *
				    sizeof(*context->definition));
			context->definition_count--;
			return 0;
		}
	}
	return 0;
}

static int
is_builtin(const char *name, size_t length)
{
	static const char *const names[] = {
	    "changequote", "decr",     "define", "divert",   "divnum",
	    "dnl",	   "errprint", "eval",	 "ifdef",    "ifelse",
	    "include",	   "incr",     "index",	 "len",	     "m4exit",
	    "sinclude",	   "shift",    "substr", "translit", "undefine",
	    "undivert",	   NULL};
	size_t index;

	for (index = 0; names[index] != NULL; index++)
		if (strlen(names[index]) == length &&
		    memcmp(names[index], name, length) == 0)
			return 1;
	return 0;
}

static void
arguments_free(struct arguments *arguments)
{
	size_t index;

	for (index = 0; index < arguments->count; index++)
		free(arguments->item[index]);
	free(arguments->item);
	memset(arguments, 0, sizeof(*arguments));
}

static int
arguments_add(struct m4_context *context, struct arguments *arguments,
	      const char *text, size_t length)
{
	char **grown;
	char *copy;

	while (length != 0 && (*text == ' ' || *text == '\t')) {
		text++;
		length--;
	}
	while (length != 0 &&
	       (text[length - 1] == ' ' || text[length - 1] == '\t'))
		length--;
	if (arguments->count >= M4_ARGUMENT_MAX) {
		set_error(context, "macro argument limit exceeded");
		return -1;
	}
	copy = malloc(length + 1);
	if (copy == NULL)
		return -1;
	memcpy(copy, text, length);
	copy[length] = '\0';
	grown =
	    realloc(arguments->item, (arguments->count + 1) * sizeof(*grown));
	if (grown == NULL) {
		free(copy);
		return -1;
	}
	arguments->item = grown;
	arguments->item[arguments->count++] = copy;
	return 0;
}

static int
parse_arguments(struct m4_context *context, const char *text, size_t length,
		size_t *offset, struct arguments *arguments)
{
	size_t start;
	size_t cursor;
	unsigned depth = 1;
	unsigned quote_depth = 0;

	memset(arguments, 0, sizeof(*arguments));
	start = cursor = *offset + 1;
	while (cursor < length) {
		if (starts_with(text, length, cursor, context->quote_open)) {
			quote_depth++;
			cursor += strlen(context->quote_open);
			continue;
		}
		if (quote_depth != 0 &&
		    starts_with(text, length, cursor, context->quote_close)) {
			quote_depth--;
			cursor += strlen(context->quote_close);
			continue;
		}
		if (quote_depth == 0) {
			if (text[cursor] == '(')
				depth++;
			else if (text[cursor] == ')') {
				if (--depth == 0) {
					if (cursor != start ||
					    arguments->count != 0)
						if (arguments_add(
							context, arguments,
							text + start,
							cursor - start) != 0)
							goto fail;
					*offset = cursor + 1;
					return 0;
				}
			} else if (text[cursor] == ',' && depth == 1) {
				if (arguments_add(context, arguments,
						  text + start,
						  cursor - start) != 0)
					goto fail;
				start = cursor + 1;
			}
		}
		if (text[cursor++] == '\n')
			context->line++;
	}
	set_error(context, "unterminated macro argument list");
fail:
	arguments_free(arguments);
	return -1;
}

static int
argument_expand(struct m4_context *context, const struct arguments *arguments,
		size_t index, struct builder *result, unsigned depth)
{
	if (index >= arguments->count)
		return 0;
	return expand_text(context, arguments->item[index],
			   strlen(arguments->item[index]), result, depth + 1);
}

static char *
expanded_argument(struct m4_context *context, const struct arguments *arguments,
		  size_t index, unsigned depth)
{
	struct builder result = {0};

	if (argument_expand(context, arguments, index, &result, depth) != 0) {
		builder_free(&result);
		return NULL;
	}
	if (result.text == NULL)
		result.text = strdup("");
	return result.text;
}

static int
read_file(const char *path, char **result, size_t *result_length)
{
	FILE *stream = fopen(path, "r");
	struct builder builder = {0};
	char buffer[4096];
	size_t count;

	if (stream == NULL)
		return -1;
	while ((count = fread(buffer, 1, sizeof(buffer), stream)) != 0) {
		if (builder.length > M4_OUTPUT_MAX - count) {
			errno = EOVERFLOW;
			goto fail;
		}
		if (builder.length + count + 1 > builder.capacity) {
			size_t capacity =
			    builder.capacity == 0 ? 4096 : builder.capacity;
			char *grown;

			while (capacity < builder.length + count + 1)
				capacity *= 2;
			grown = realloc(builder.text, capacity);
			if (grown == NULL)
				goto fail;
			builder.text = grown;
			builder.capacity = capacity;
		}
		memcpy(builder.text + builder.length, buffer, count);
		builder.length += count;
	}
	if (ferror(stream) || fclose(stream) != 0)
		goto fail_closed;
	if (builder.text == NULL) {
		builder.text = malloc(1);
		if (builder.text == NULL)
			return -1;
	}
	builder.text[builder.length] = '\0';
	*result = builder.text;
	*result_length = builder.length;
	return 0;

fail:
	(void)fclose(stream);
fail_closed:
	builder_free(&builder);
	return -1;
}

struct expression_parser {
	const char *text;
	int failed;
};

static void
expression_space(struct expression_parser *parser)
{
	while (*parser->text == ' ' || *parser->text == '\t')
		parser->text++;
}

static long long expression_add(struct expression_parser *);

static long long
expression_primary(struct expression_parser *parser)
{
	long long value;
	char *end;

	expression_space(parser);
	if (*parser->text == '(') {
		parser->text++;
		value = expression_add(parser);
		expression_space(parser);
		if (*parser->text++ != ')')
			parser->failed = 1;
		return value;
	}
	errno = 0;
	value = strtoll(parser->text, &end, 0);
	if (end == parser->text || errno != 0) {
		parser->failed = 1;
		return 0;
	}
	parser->text = end;
	return value;
}

static long long
expression_unary(struct expression_parser *parser)
{
	expression_space(parser);
	if (*parser->text == '+') {
		parser->text++;
		return expression_unary(parser);
	}
	if (*parser->text == '-') {
		parser->text++;
		return -expression_unary(parser);
	}
	return expression_primary(parser);
}

static long long
expression_product(struct expression_parser *parser)
{
	long long value = expression_unary(parser);

	for (;;) {
		char operation;
		long long right;

		expression_space(parser);
		operation = *parser->text;
		if (operation != '*' && operation != '/' && operation != '%')
			break;
		parser->text++;
		right = expression_unary(parser);
		if ((operation == '/' || operation == '%') && right == 0) {
			parser->failed = 2;
			return 0;
		}
		if (operation == '*')
			value *= right;
		else if (operation == '/')
			value /= right;
		else
			value %= right;
	}
	return value;
}

static long long
expression_add(struct expression_parser *parser)
{
	long long value = expression_product(parser);

	for (;;) {
		char operation;
		long long right;

		expression_space(parser);
		operation = *parser->text;
		if (operation != '+' && operation != '-')
			break;
		parser->text++;
		right = expression_product(parser);
		value = operation == '+' ? value + right : value - right;
	}
	return value;
}

static int
builtin_eval(struct m4_context *context, const char *expression,
	     struct builder *capture)
{
	struct expression_parser parser = {expression, 0};
	long long value = expression_add(&parser);
	char output[64];
	int length;

	expression_space(&parser);
	if (parser.failed || *parser.text != '\0') {
		set_error(context, parser.failed == 2 ? "division by zero"
						      : "invalid expression");
		return -1;
	}
	length = snprintf(output, sizeof(output), "%lld", value);
	return length < 0 || (size_t)length >= sizeof(output)
		   ? -1
		   : emit(context, capture, output, (size_t)length);
}

static int
emit_user_macro(struct m4_context *context, struct definition *definition,
		const char *name, size_t name_length,
		const struct arguments *arguments, struct builder *capture,
		unsigned depth)
{
	struct builder replaced = {0};
	const char *cursor = definition->value;

	while (*cursor != '\0') {
		if (*cursor == '$' && cursor[1] >= '0' && cursor[1] <= '9') {
			size_t index = (size_t)(cursor[1] - '0');

			if (index == 0) {
				if (builder_append(context, &replaced, name,
						   name_length) != 0)
					goto fail;
			} else if (index <= arguments->count) {
				char *argument = expanded_argument(
				    context, arguments, index - 1, depth);

				if (argument == NULL ||
				    builder_append(context, &replaced, argument,
						   strlen(argument)) != 0) {
					free(argument);
					goto fail;
				}
				free(argument);
			}
			cursor += 2;
		} else if (builder_append(context, &replaced, cursor++, 1) != 0)
			goto fail;
	}
	if (expand_text(context, replaced.text == NULL ? "" : replaced.text,
			replaced.length, capture, depth + 1) != 0)
		goto fail;
	builder_free(&replaced);
	return 0;

fail:
	builder_free(&replaced);
	return -1;
}

static int
emit_builtin(struct m4_context *context, const char *name, size_t name_length,
	     const struct arguments *arguments, struct builder *capture,
	     unsigned depth)
{
	char *a = NULL;
	char *b = NULL;
	char *c = NULL;
	int result = -1;

#define BUILTIN(word)                                                          \
	(name_length == sizeof(word) - 1 &&                                    \
	 memcmp(name, word, sizeof(word) - 1) == 0)
	if (BUILTIN("dnl")) {
		context->discard_line = 1;
		return 0;
	}
	if (BUILTIN("define") || BUILTIN("undefine") || BUILTIN("ifdef") ||
	    BUILTIN("ifelse") || BUILTIN("include") || BUILTIN("sinclude") ||
	    BUILTIN("len") || BUILTIN("incr") || BUILTIN("decr") ||
	    BUILTIN("eval") || BUILTIN("index") || BUILTIN("substr") ||
	    BUILTIN("translit") || BUILTIN("divert") || BUILTIN("undivert") ||
	    BUILTIN("changequote") || BUILTIN("errprint") || BUILTIN("shift") ||
	    BUILTIN("m4exit")) {
		a = expanded_argument(context, arguments, 0, depth);
		if (a == NULL)
			goto done;
	}
	if (BUILTIN("define")) {
		b = expanded_argument(context, arguments, 1, depth);
		if (b == NULL)
			goto done;
		result = m4_define(context, a, b);
	} else if (BUILTIN("undefine"))
		result = m4_undefine(context, a);
	else if (BUILTIN("ifdef")) {
		b = expanded_argument(
		    context, arguments,
		    find_definition(context, a, strlen(a)) != NULL ? 1 : 2,
		    depth);
		result = b == NULL ? -1 : emit(context, capture, b, strlen(b));
	} else if (BUILTIN("ifelse")) {
		b = expanded_argument(context, arguments, 1, depth);
		if (b == NULL)
			goto done;
		c = expanded_argument(context, arguments,
				      strcmp(a, b) == 0 ? 2 : 3, depth);
		result = c == NULL ? -1 : emit(context, capture, c, strlen(c));
	} else if (BUILTIN("include") || BUILTIN("sinclude")) {
		char *file_text;
		size_t file_length;
		const char *saved_source = context->source;
		unsigned saved_line = context->line;

		if (read_file(a, &file_text, &file_length) != 0) {
			if (BUILTIN("sinclude"))
				result = 0;
			else
				set_error(context, "cannot read included file");
			goto done;
		}
		context->source = a;
		context->line = 1;
		result = expand_text(context, file_text, file_length, capture,
				     depth + 1);
		context->source = saved_source;
		context->line = saved_line;
		free(file_text);
	} else if (BUILTIN("len")) {
		char output[64];
		int length = snprintf(output, sizeof(output), "%zu", strlen(a));

		result = length < 0 || (size_t)length >= sizeof(output)
			     ? -1
			     : emit(context, capture, output, (size_t)length);
	} else if (BUILTIN("incr") || BUILTIN("decr")) {
		char *end;
		long long value;
		char output[64];
		int length;

		errno = 0;
		value = strtoll(a, &end, 10);
		if (errno != 0 || *a == '\0' || *end != '\0' ||
		    (BUILTIN("incr") && value == LLONG_MAX) ||
		    (BUILTIN("decr") && value == LLONG_MIN)) {
			set_error(context, "invalid integer");
			goto done;
		}
		value += BUILTIN("incr") ? 1 : -1;
		length = snprintf(output, sizeof(output), "%lld", value);
		result = length < 0 || (size_t)length >= sizeof(output)
			     ? -1
			     : emit(context, capture, output, (size_t)length);
	} else if (BUILTIN("eval"))
		result = builtin_eval(context, a, capture);
	else if (BUILTIN("index")) {
		const char *found;
		char output[64];
		int length;

		b = expanded_argument(context, arguments, 1, depth);
		if (b == NULL)
			goto done;
		found = strstr(a, b);
		length =
		    snprintf(output, sizeof(output), "%lld",
			     found == NULL ? -1LL : (long long)(found - a));
		result = length < 0 || (size_t)length >= sizeof(output)
			     ? -1
			     : emit(context, capture, output, (size_t)length);
	} else if (BUILTIN("substr")) {
		char *end;
		unsigned long long start;
		unsigned long long count = ULLONG_MAX;
		size_t available;

		b = expanded_argument(context, arguments, 1, depth);
		c = expanded_argument(context, arguments, 2, depth);
		if (b == NULL || c == NULL)
			goto done;
		errno = 0;
		start = strtoull(b, &end, 10);
		if (errno != 0 || *b == '\0' || *end != '\0')
			goto done;
		if (arguments->count > 2) {
			errno = 0;
			count = strtoull(c, &end, 10);
			if (errno != 0 || *c == '\0' || *end != '\0')
				goto done;
		}
		available = strlen(a);
		if (start >= available)
			result = 0;
		else {
			available -= (size_t)start;
			if (count < available)
				available = (size_t)count;
			result = emit(context, capture, a + start, available);
		}
	} else if (BUILTIN("translit")) {
		struct builder translated = {0};
		size_t index;

		b = expanded_argument(context, arguments, 1, depth);
		c = expanded_argument(context, arguments, 2, depth);
		if (b == NULL || c == NULL)
			goto done;
		for (index = 0; a[index] != '\0'; index++) {
			const char *mapped = strchr(b, a[index]);

			if (mapped == NULL) {
				if (builder_append(context, &translated,
						   a + index, 1) != 0)
					goto translit_done;
			} else if ((size_t)(mapped - b) < strlen(c) &&
				   builder_append(context, &translated,
						  c + (mapped - b), 1) != 0)
				goto translit_done;
		}
		result = emit(context, capture,
			      translated.text == NULL ? "" : translated.text,
			      translated.length);
	translit_done:
		builder_free(&translated);
	} else if (BUILTIN("divert")) {
		char *end;
		long value = *a == '\0' ? 0 : strtol(a, &end, 10);

		if ((*a != '\0' && *end != '\0') || value < -1 ||
		    value >= M4_DIVERSION_COUNT) {
			set_error(context, "invalid diversion");
			goto done;
		}
		context->current_diversion = (int)value;
		result = 0;
	} else if (BUILTIN("divnum")) {
		char output[32];
		int length = snprintf(output, sizeof(output), "%d",
				      context->current_diversion);

		result = emit(context, capture, output, (size_t)length);
	} else if (BUILTIN("undivert")) {
		char *end;
		long value = strtol(a, &end, 10);

		if (*a == '\0') {
			for (value = 1; value < M4_DIVERSION_COUNT; value++) {
				if (emit(context, capture,
					 context->diversion[value].text == NULL
					     ? ""
					     : context->diversion[value].text,
					 context->diversion[value].length) != 0)
					goto done;
				builder_free(&context->diversion[value]);
			}
			result = 0;
		} else if (*end == '\0' && value > 0 &&
			   value < M4_DIVERSION_COUNT) {
			result = emit(context, capture,
				      context->diversion[value].text == NULL
					  ? ""
					  : context->diversion[value].text,
				      context->diversion[value].length);
			builder_free(&context->diversion[value]);
		}
	} else if (BUILTIN("changequote")) {
		b = expanded_argument(context, arguments, 1, depth);
		if (b == NULL || strlen(a) >= sizeof(context->quote_open) ||
		    strlen(b) >= sizeof(context->quote_close))
			goto done;
		strcpy(context->quote_open, *a == '\0' ? "`" : a);
		strcpy(context->quote_close, *b == '\0' ? "'" : b);
		result = 0;
	} else if (BUILTIN("errprint")) {
		result = fputs(a, stderr) == EOF ? -1 : 0;
	} else if (BUILTIN("shift")) {
		size_t index;

		result = 0;
		for (index = 1; index < arguments->count; index++) {
			char *value =
			    expanded_argument(context, arguments, index, depth);

			if (value == NULL ||
			    (index != 1 &&
			     emit(context, capture, ",", 1) != 0) ||
			    emit(context, capture, value, strlen(value)) != 0) {
				free(value);
				result = -1;
				break;
			}
			free(value);
		}
	} else if (BUILTIN("m4exit")) {
		set_error(context, "m4exit is not implemented locally");
		result = -1;
	} else {
		set_error(context, "unsupported builtin");
		result = -1;
	}
done:
	free(a);
	free(b);
	free(c);
	if (result != 0 && context->error[0] == '\0')
		set_error(context, errno == ENOMEM
				       ? "out of memory"
				       : "invalid builtin arguments");
	return result;
#undef BUILTIN
}

static int
expand_quoted(struct m4_context *context, const char *text, size_t length,
	      size_t *offset, struct builder *capture)
{
	size_t cursor = *offset + strlen(context->quote_open);
	size_t start = cursor;
	unsigned depth = 1;

	while (cursor < length) {
		if (starts_with(text, length, cursor, context->quote_open)) {
			if (emit(context, capture, text + start,
				 cursor - start) != 0 ||
			    emit(context, capture, context->quote_open,
				 strlen(context->quote_open)) != 0)
				return -1;
			cursor += strlen(context->quote_open);
			start = cursor;
			depth++;
			continue;
		}
		if (starts_with(text, length, cursor, context->quote_close)) {
			if (--depth == 0) {
				if (emit(context, capture, text + start,
					 cursor - start) != 0)
					return -1;
				*offset = cursor + strlen(context->quote_close);
				return 0;
			}
			if (emit(context, capture, text + start,
				 cursor - start) != 0 ||
			    emit(context, capture, context->quote_close,
				 strlen(context->quote_close)) != 0)
				return -1;
			cursor += strlen(context->quote_close);
			start = cursor;
			continue;
		}
		if (text[cursor++] == '\n')
			context->line++;
	}
	set_error(context, "unterminated quote");
	return -1;
}

static int
expand_text(struct m4_context *context, const char *text, size_t length,
	    struct builder *capture, unsigned depth)
{
	size_t offset = 0;

	if (depth > M4_DEPTH_MAX) {
		set_error(context, "macro expansion depth exceeded");
		return -1;
	}
	while (offset < length) {
		if (context->discard_line) {
			while (offset < length && text[offset] != '\n')
				offset++;
			if (offset < length) {
				offset++;
				context->line++;
			}
			context->discard_line = 0;
			continue;
		}
		if (starts_with(text, length, offset, context->quote_open)) {
			if (expand_quoted(context, text, length, &offset,
					  capture) != 0)
				return -1;
			continue;
		}
		if (text[offset] == '#') {
			size_t start = offset;

			while (offset < length && text[offset] != '\n')
				offset++;
			if (offset < length)
				offset++;
			if (emit(context, capture, text + start,
				 offset - start) != 0)
				return -1;
			context->line++;
			continue;
		}
		if (is_name_start(text[offset])) {
			size_t start = offset++;
			size_t after_name;
			struct definition *definition;
			struct arguments arguments = {0};
			int builtin;

			while (offset < length &&
			       is_name_character(text[offset]))
				offset++;
			after_name = offset;
			definition = find_definition(context, text + start,
						     offset - start);
			builtin = is_builtin(text + start, offset - start);
			if (definition == NULL && !builtin) {
				if (emit(context, capture, text + start,
					 offset - start) != 0)
					return -1;
				continue;
			}
			while (offset < length &&
			       (text[offset] == ' ' || text[offset] == '\t'))
				offset++;
			if (offset < length && text[offset] == '(') {
				if (parse_arguments(context, text, length,
						    &offset, &arguments) != 0)
					return -1;
			} else
				offset = after_name;
			if (definition != NULL) {
				if (emit_user_macro(
					context, definition, text + start,
					after_name - start, &arguments, capture,
					depth) != 0) {
					arguments_free(&arguments);
					return -1;
				}
			} else if (emit_builtin(context, text + start,
						after_name - start, &arguments,
						capture, depth) != 0) {
				arguments_free(&arguments);
				return -1;
			}
			arguments_free(&arguments);
			continue;
		}
		if (emit(context, capture, text + offset, 1) != 0)
			return -1;
		if (text[offset++] == '\n')
			context->line++;
	}
	return 0;
}

struct m4_context *
m4_context_create(void)
{
	struct m4_context *context = calloc(1, sizeof(*context));

	if (context != NULL) {
		strcpy(context->quote_open, "`");
		strcpy(context->quote_close, "'");
	}
	return context;
}

void
m4_context_destroy(struct m4_context *context)
{
	size_t index;

	if (context == NULL)
		return;
	for (index = 0; index < context->definition_count; index++) {
		free(context->definition[index].name);
		free(context->definition[index].value);
	}
	free(context->definition);
	builder_free(&context->output);
	for (index = 0; index < M4_DIVERSION_COUNT; index++)
		builder_free(&context->diversion[index]);
	free(context);
}

int
m4_process(struct m4_context *context, const char *source, const char *text,
	   size_t length)
{
	context->source = source;
	context->line = 1;
	return expand_text(context, text, length, NULL, 0);
}

int
m4_finish(struct m4_context *context, int descriptor)
{
	size_t index;

	for (index = 1; index < M4_DIVERSION_COUNT; index++)
		if (context->diversion[index].length != 0 &&
		    builder_append(context, &context->output,
				   context->diversion[index].text,
				   context->diversion[index].length) != 0)
			return -1;
	return command_write_all(
	    descriptor,
	    context->output.text == NULL ? "" : context->output.text,
	    context->output.length);
}
