/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Running an awk program: the statements, the expressions, the lvalues,
 * the calls of the program's functions, and the rules.
 *
 * A statement reports how it finished (FLOW_*), which the loops, the
 * blocks and the rules act on.  A next run inside a function cannot report
 * through the expression that called the function, so it is left in
 * awk.pending_flow and each statement passes it on.
 */

#include "userland/base/awk/awk.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int run_block(struct node *block);
static int run_while(struct node *statement);
static int run_do(struct node *statement);
static int run_for(struct node *statement);
static int run_for_in(struct node *statement);
static int run_print(struct node *statement);
static int run_printf(struct node *statement);
static void run_delete(struct node *statement);
static int condition_truth(struct node *condition);
static FILE *output_stream(struct node *statement);
static void write_value(FILE *stream, const struct value *value);
static void run_assign(struct node *expression, struct value *value);
static void run_increment(struct node *expression, struct value *value);
static void run_arithmetic(struct node *expression, struct value *value);
static double arithmetic(int operator, double left, double right);
static void run_compare(struct node *expression, struct value *value);
static void run_match(struct node *expression, struct value *value);
static void run_concatenate(struct node *expression, struct value *value);
static void run_element(struct node *expression, struct value *value);
static void run_in(struct node *expression, struct value *value);
static void run_call(struct node *call, struct value *value);
static void bind_argument(struct node *argument, struct cell *cell);
static void run_getline(struct node *expression, struct value *value);
static int regex_matches_record(regex_t *regex);
static size_t field_index(struct node *index);
static int pass_pending(int flow);
static void finish(void);

/*
 * Runs a statement and reports how it finished.
 */
int
run_statement(
	struct node *statement)
{
	struct value scratch;
	int truth;
	int flow;

	/* An empty statement. */
	if (statement == NULL)
		return FLOW_NORMAL;

	/* The statement of its kind. */
	memset(&scratch, 0, sizeof(scratch));
	flow = FLOW_NORMAL;
	switch (statement->kind) {
	case NODE_BLOCK:
		flow = run_block(statement);
		break;
	case NODE_EXPRESSION:
		run_expression(statement->left, &scratch);
		value_free(&scratch);
		break;
	case NODE_PRINT:
		flow = run_print(statement);
		break;
	case NODE_PRINTF:
		flow = run_printf(statement);
		break;
	case NODE_IF:
		truth = condition_truth(statement->condition);
		if (awk.pending_flow != FLOW_NORMAL)
			break;
		if (truth)
			flow = run_statement(statement->then);
		else
			flow = run_statement(statement->otherwise);
		break;
	case NODE_WHILE:
		flow = run_while(statement);
		break;
	case NODE_DO:
		flow = run_do(statement);
		break;
	case NODE_FOR:
		flow = run_for(statement);
		break;
	case NODE_FOR_IN:
		flow = run_for_in(statement);
		break;
	case NODE_NEXT:
		flow = FLOW_NEXT;
		break;
	case NODE_NEXTFILE:
		flow = FLOW_NEXTFILE;
		break;
	case NODE_EXIT:
		if (statement->left != NULL) {
			run_expression(statement->left, &scratch);
			awk.exit_status = (int)value_number(&scratch);
			value_free(&scratch);
		}

		/* END, then the end of awk. */
		run_exit();
		break;
	case NODE_RETURN:
		if (statement->left != NULL)
			run_expression(statement->left, &awk.frame->result);
		flow = FLOW_RETURN;
		break;
	case NODE_BREAK:
		flow = FLOW_BREAK;
		break;
	case NODE_CONTINUE:
		flow = FLOW_CONTINUE;
		break;
	case NODE_DELETE:
		run_delete(statement);
		break;
	default:
		awk_fatal("internal error: statement of kind %d", statement->kind);
		break;
	}

	/* Succeeded: a next from a function goes on up. */
	flow = pass_pending(flow);
	return flow;
}

/*
 * Evaluates an expression into a value.
 */
void
run_expression(
	struct node *expression,
	struct value *value)
{
	struct value operand;
	struct cell *cell;
	double number;
	size_t index;
	int truth;

	/* The expression of its kind. */
	memset(&operand, 0, sizeof(operand));
	switch (expression->kind) {
	case NODE_NUMBER:
		value_set_number(value, expression->number);
		break;
	case NODE_STRING:
		value_set_text(value, expression->text, expression->length);
		break;
	case NODE_REGEX:
		truth = regex_matches_record(expression->regex);
		value_set_number(value, (double)truth);
		break;
	case NODE_VARIABLE:
	case NODE_LOCAL:
		cell = node_cell(expression);
		cell_read(cell, value);
		break;
	case NODE_FIELD:
		index = field_index(expression->left);
		field_read(index, value);
		break;
	case NODE_ELEMENT:
		run_element(expression, value);
		break;
	case NODE_ASSIGN:
		run_assign(expression, value);
		break;
	case NODE_CONDITIONAL:
		truth = condition_truth(expression->condition);
		if (truth)
			run_expression(expression->then, value);
		else
			run_expression(expression->otherwise, value);
		break;
	case NODE_OR:
		truth = condition_truth(expression->left);
		if (!truth)
			truth = condition_truth(expression->right);
		value_set_number(value, (double)truth);
		break;
	case NODE_AND:
		truth = condition_truth(expression->left);
		if (truth)
			truth = condition_truth(expression->right);
		value_set_number(value, (double)truth);
		break;
	case NODE_IN:
		run_in(expression, value);
		break;
	case NODE_MATCH:
		run_match(expression, value);
		break;
	case NODE_COMPARE:
		run_compare(expression, value);
		break;
	case NODE_CONCATENATE:
		run_concatenate(expression, value);
		break;
	case NODE_ARITHMETIC:
		run_arithmetic(expression, value);
		break;
	case NODE_NEGATE:
		run_expression(expression->left, &operand);
		number = value_number(&operand);
		value_set_number(value, -number);
		break;
	case NODE_UNARY_PLUS:
		run_expression(expression->left, &operand);
		number = value_number(&operand);
		value_set_number(value, number);
		break;
	case NODE_NOT:
		truth = condition_truth(expression->left);
		if (truth)
			value_set_number(value, 0);
		else
			value_set_number(value, 1);
		break;
	case NODE_PRE_INCREMENT:
	case NODE_POST_INCREMENT:
		run_increment(expression, value);
		break;
	case NODE_CALL:
		run_call(expression, value);
		break;
	case NODE_BUILTIN:
		builtin_call(expression, value);
		break;
	case NODE_GETLINE:
		run_getline(expression, value);
		break;
	case NODE_GROUPING:
		awk_fatal("syntax error at source line %d: a list of expressions outside print",
			  expression->line);
		break;
	default:
		awk_fatal("internal error: expression of kind %d", expression->kind);
		break;
	}

	/* Succeeded. */
	value_free(&operand);
}

/*
 * Runs the rules of BEGIN or END in order.
 */
void
run_rules(
	struct rule *rules)
{
	struct rule *rule;

	/* Each action; a next from a function goes no further. */
	for (rule = rules; rule != NULL; rule = rule->next) {
		run_statement(rule->action);
		awk.pending_flow = FLOW_NORMAL;
	}
}

/*
 * Runs the main rules on the current record.  Returns the flow that ended
 * them: FLOW_NORMAL, FLOW_NEXT or FLOW_NEXTFILE.
 */
int
run_main_rules(
	void)
{
	struct rule *rule;
	struct value record;
	const char *separator;
	size_t separator_length;
	int matched;
	int ended;
	int flow;

	/* Each rule whose pattern matches. */
	for (rule = awk.main_rules; rule != NULL; rule = rule->next) {
		/* No pattern, one pattern, or a range. */
		matched = 1;
		if (rule->pattern != NULL && rule->pattern_end == NULL) {
			matched = condition_truth(rule->pattern);
		} else if (rule->pattern_end != NULL) {
			if (!rule->in_range) {
				matched = condition_truth(rule->pattern);
				if (matched) {
					ended = condition_truth(rule->pattern_end);
					if (!ended)
						rule->in_range = 1;
				}
			} else {
				ended = condition_truth(rule->pattern_end);
				if (ended)
					rule->in_range = 0;
			}
		}

		/* A record the pattern does not select. */
		if (!matched)
			continue;

		/* No action prints the record. */
		if (rule->action == NULL) {
			memset(&record, 0, sizeof(record));
			field_read(0, &record);
			write_value(stdout, &record);
			value_free(&record);
			separator = special_text(SPECIAL_ORS, &separator_length);
			fwrite(separator, 1, separator_length, stdout);
			continue;
		}

		/* The action; next and nextfile end the rules for the record. */
		flow = run_statement(rule->action);
		awk.pending_flow = FLOW_NORMAL;
		if (flow == FLOW_NEXT || flow == FLOW_NEXTFILE)
			return flow;
	}

	/* Succeeded: every rule ran. */
	return FLOW_NORMAL;
}

/*
 * Ends awk as exit does: the END rules run unless they are running, and
 * then the output is flushed.  It does not return.
 */
void
run_exit(
	void)
{
	/* END, once. */
	if (!awk.in_end) {
		awk.in_end = 1;
		run_rules(awk.end_rules);
	}

	/* Succeeded: the end. */
	finish();
}

/*
 * Assigns a value to a global variable, acting on what awk gives the
 * variable a meaning for.
 */
void
assign_variable(
	struct variable *variable,
	const struct value *value)
{
	/* The cell, then the meaning. */
	cell_write(&variable->cell, value);
	if (variable->special != SPECIAL_NONE)
		special_assigned(variable);
}

/*
 * Returns the array a name node stands for, making an unset variable an
 * array.
 */
struct array *
node_array(
	struct node *node)
{
	struct cell *cell;
	struct array *array;

	/* The cell of the variable or the parameter. */
	cell = node_cell(node);
	if (cell == NULL)
		awk_fatal("attempt to use a non-array as an array");

	/* Succeeded. */
	array = cell_array(cell);
	return array;
}

/*
 * Returns the cell of a variable or a parameter node, NULL for any other
 * node.
 */
struct cell *
node_cell(
	struct node *node)
{
	/* A global variable. */
	if (node->kind == NODE_VARIABLE)
		return &node->variable->cell;

	/* A parameter of the function being run. */
	if (node->kind == NODE_LOCAL)
		return &awk.frame->cells[node->local];

	/* Anything else. */
	return NULL;
}

/*
 * Makes the key of an element: the subscripts in their string forms,
 * joined with SUBSEP.
 */
void
subscript_key(
	struct node *subscripts,
	struct value *key)
{
	struct buffer buffer;
	struct value subscript;
	struct node *argument;
	const char *separator;
	size_t separator_length;

	/* One subscript is its own string. */
	memset(&subscript, 0, sizeof(subscript));
	if (subscripts->next == NULL) {
		run_expression(subscripts, &subscript);
		value_string(&subscript, 0, key);
		value_free(&subscript);
		return;
	}

	/* Several, with SUBSEP between them. */
	memset(&buffer, 0, sizeof(buffer));
	buffer_append(&buffer, "", 0);
	for (argument = subscripts; argument != NULL; argument = argument->next) {
		if (argument != subscripts) {
			separator = special_text(SPECIAL_SUBSEP, &separator_length);
			buffer_append(&buffer, separator, separator_length);
		}

		/* The subscript's string form. */
		run_expression(argument, &subscript);
		value_string(&subscript, 0, &subscript);
		buffer_append(&buffer, subscript.text, subscript.length);
	}

	/* The scratch value goes. */
	value_free(&subscript);

	/* Succeeded: the key takes the buffer. */
	value_free(key);
	key->type = VALUE_STRING;
	key->text = buffer.data;
	key->length = buffer.length;
}

/*
 * Resolves an lvalue: its cell, its array and key, or its field index.
 */
void
place_resolve(
	struct node *lvalue,
	struct place *place)
{
	/* Nothing resolved yet. */
	memset(place, 0, sizeof(*place));

	/* The lvalue of its kind. */
	switch (lvalue->kind) {
	case NODE_VARIABLE:
		place->kind = PLACE_CELL;
		place->cell = &lvalue->variable->cell;
		place->variable = lvalue->variable;
		break;
	case NODE_LOCAL:
		place->kind = PLACE_CELL;
		place->cell = node_cell(lvalue);
		break;
	case NODE_ELEMENT:
		place->kind = PLACE_ELEMENT;
		place->array = node_array(lvalue->array);
		subscript_key(lvalue->arguments, &place->key);
		break;
	case NODE_FIELD:
		place->kind = PLACE_FIELD;
		place->field = field_index(lvalue->left);
		break;
	default:
		awk_fatal("assignment to a non-lvalue");
		break;
	}
}

/*
 * Reads the value at a place; an element read is made if absent.
 */
void
place_read(
	struct place *place,
	struct value *value)
{
	struct element *element;

	/* A cell. */
	if (place->kind == PLACE_CELL) {
		cell_read(place->cell, value);
		return;
	}

	/* A field. */
	if (place->kind == PLACE_FIELD) {
		field_read(place->field, value);
		return;
	}

	/* Succeeded: an element. */
	element = array_find(place->array, place->key.text, place->key.length, 1);
	value_copy(value, &element->value);
}

/*
 * Writes a value to a place.
 */
void
place_write(
	struct place *place,
	const struct value *value)
{
	struct element *element;

	/* A global variable, with its meaning. */
	if (place->kind == PLACE_CELL && place->variable != NULL) {
		assign_variable(place->variable, value);
		return;
	}

	/* A parameter. */
	if (place->kind == PLACE_CELL) {
		cell_write(place->cell, value);
		return;
	}

	/* A field. */
	if (place->kind == PLACE_FIELD) {
		field_write(place->field, value);
		return;
	}

	/* Succeeded: an element. */
	element = array_find(place->array, place->key.text, place->key.length, 1);
	value_copy(&element->value, value);
}

/*
 * Frees what resolving a place took.
 */
void
place_release(
	struct place *place)
{
	/* The key of an element. */
	value_free(&place->key);
}

/* Runs the statements of a block in order. */
static int
run_block(
	struct node *block)
{
	struct node *statement;
	int flow;

	/* Each statement, until one does not finish normally. */
	for (statement = block->body; statement != NULL; statement = statement->next) {
		flow = run_statement(statement);
		if (flow != FLOW_NORMAL)
			return flow;
	}

	/* Succeeded: every statement ran. */
	return FLOW_NORMAL;
}

/* Runs while (condition) body. */
static int
run_while(
	struct node *statement)
{
	int truth;
	int flow;

	/* The body while the condition holds. */
	for (;;) {
		truth = condition_truth(statement->condition);
		if (awk.pending_flow != FLOW_NORMAL)
			return awk.pending_flow;
		if (!truth)
			break;
		flow = run_statement(statement->body);
		if (flow == FLOW_BREAK)
			break;
		if (flow != FLOW_NORMAL && flow != FLOW_CONTINUE)
			return flow;
	}

	/* Succeeded: the loop ended. */
	return FLOW_NORMAL;
}

/* Runs do body while (condition). */
static int
run_do(
	struct node *statement)
{
	int truth;
	int flow;

	/* The body, then again while the condition holds. */
	for (;;) {
		flow = run_statement(statement->body);
		if (flow == FLOW_BREAK)
			break;
		if (flow != FLOW_NORMAL && flow != FLOW_CONTINUE)
			return flow;
		truth = condition_truth(statement->condition);
		if (awk.pending_flow != FLOW_NORMAL)
			return awk.pending_flow;
		if (!truth)
			break;
	}

	/* Succeeded: the loop ended. */
	return FLOW_NORMAL;
}

/* Runs for (initial; condition; step) body. */
static int
run_for(
	struct node *statement)
{
	struct value scratch;
	int truth;
	int flow;

	/* The initial expression. */
	memset(&scratch, 0, sizeof(scratch));
	if (statement->initial != NULL)
		run_expression(statement->initial, &scratch);

	/* The body while the condition holds, and the step after it. */
	for (;;) {
		truth = 1;
		if (statement->condition != NULL)
			truth = condition_truth(statement->condition);
		if (awk.pending_flow != FLOW_NORMAL)
			break;
		if (!truth)
			break;
		flow = run_statement(statement->body);
		if (flow == FLOW_BREAK)
			break;
		if (flow != FLOW_NORMAL && flow != FLOW_CONTINUE) {
			value_free(&scratch);
			return flow;
		}

		/* The step before the next turn. */
		if (statement->step != NULL)
			run_expression(statement->step, &scratch);
	}

	/* Succeeded: the loop ended. */
	value_free(&scratch);
	return FLOW_NORMAL;
}

/*
 * Runs for (name in array) body over the keys the array has when the loop
 * starts; a key deleted meanwhile is skipped.
 */
static int
run_for_in(
	struct node *statement)
{
	struct array *array;
	struct element *element;
	struct value *keys;
	struct place place;
	size_t count;
	size_t index;
	int flow;

	/* The keys, copied. */
	array = node_array(statement->array);
	count = array->count;
	keys = awk_allocate(sizeof(*keys) * count);
	index = 0;
	for (element = array->first; element != NULL; element = element->next) {
		value_set_text(&keys[index], element->key, element->key_length);
		index++;
	}

	/* The body for each key still there. */
	flow = FLOW_NORMAL;
	for (index = 0; index < count; index++) {
		element = array_find(array, keys[index].text, keys[index].length, 0);
		if (element == NULL)
			continue;
		place_resolve(statement->left, &place);
		place_write(&place, &keys[index]);
		place_release(&place);
		flow = run_statement(statement->body);
		if (flow == FLOW_BREAK) {
			flow = FLOW_NORMAL;
			break;
		}

		/* continue goes on with the next key. */
		if (flow == FLOW_CONTINUE)
			flow = FLOW_NORMAL;
		if (flow != FLOW_NORMAL)
			break;
	}

	/* Succeeded: the keys go. */
	for (index = 0; index < count; index++)
		value_free(&keys[index]);
	free(keys);
	return flow;
}

/* Runs print: the arguments (or $0) with OFS between them, then ORS. */
static int
run_print(
	struct node *statement)
{
	struct node *argument;
	struct value value;
	struct value *values;
	const char *separator;
	size_t separator_length;
	size_t count;
	size_t index;
	FILE *stream;

	/* The values first, since evaluating them may change OFS. */
	memset(&value, 0, sizeof(value));
	count = statement->argument_count;
	values = awk_allocate(sizeof(*values) * (count + 1U));
	index = 0;
	for (argument = statement->arguments; argument != NULL; argument = argument->next) {
		run_expression(argument, &values[index]);
		index++;
	}

	/* print alone prints $0. */
	if (count == 0) {
		field_read(0, &values[0]);
		count = 1;
	}

	/* Where the output goes. */
	stream = output_stream(statement);

	/* The values, with OFS between them. */
	for (index = 0; index < count; index++) {
		if (index > 0) {
			separator = special_text(SPECIAL_OFS, &separator_length);
			fwrite(separator, 1, separator_length, stream);
		}

		/* The value. */
		write_value(stream, &values[index]);
		value_free(&values[index]);
	}

	/* ORS after them. */
	separator = special_text(SPECIAL_ORS, &separator_length);
	fwrite(separator, 1, separator_length, stream);
	free(values);

	/* Succeeded. */
	return FLOW_NORMAL;
}

/* Runs printf: the arguments formatted by the first. */
static int
run_printf(
	struct node *statement)
{
	struct node *argument;
	struct value *values;
	struct value format;
	struct buffer buffer;
	size_t count;
	size_t index;
	FILE *stream;

	/* The format and the values. */
	count = statement->argument_count;
	values = awk_allocate(sizeof(*values) * count);
	index = 0;
	for (argument = statement->arguments; argument != NULL; argument = argument->next) {
		run_expression(argument, &values[index]);
		index++;
	}

	/* The format as a string. */
	memset(&format, 0, sizeof(format));
	value_string(&values[0], 0, &format);

	/* The text they make. */
	memset(&buffer, 0, sizeof(buffer));
	buffer_append(&buffer, "", 0);
	format_values(format.text, format.length, values + 1, count - 1U, &buffer);

	/* Succeeded: the text where the output goes. */
	stream = output_stream(statement);
	fwrite(buffer.data, 1, buffer.length, stream);
	free(buffer.data);
	value_free(&format);
	for (index = 0; index < count; index++)
		value_free(&values[index]);
	free(values);
	return FLOW_NORMAL;
}

/* Runs delete name[subscripts] and delete name. */
static void
run_delete(
	struct node *statement)
{
	struct array *array;
	struct value key;

	/* The whole array. */
	array = node_array(statement->array);
	if (statement->arguments == NULL) {
		array_clear(array);
		return;
	}

	/* Succeeded: the element of the key. */
	memset(&key, 0, sizeof(key));
	subscript_key(statement->arguments, &key);
	array_remove(array, key.text, key.length);
	value_free(&key);
}

/* Evaluates a condition to whether it holds. */
static int
condition_truth(
	struct node *condition)
{
	struct value value;
	int truth;

	/* The value, then its truth. */
	memset(&value, 0, sizeof(value));
	run_expression(condition, &value);
	truth = value_truth(&value);
	value_free(&value);

	/* Succeeded. */
	return truth;
}

/* Returns the stream print and printf write to: stdout, or a redirection. */
static FILE *
output_stream(
	struct node *statement)
{
	struct value name;
	FILE *stream;

	/* Standard output. */
	if (statement->output == OUTPUT_STANDARD)
		return stdout;

	/* Succeeded: the file or the command the destination names. */
	memset(&name, 0, sizeof(name));
	run_expression(statement->destination, &name);
	value_string(&name, 0, &name);
	stream = io_output(statement->output, name.text, name.length);
	value_free(&name);
	return stream;
}

/* Writes a value as print does: a number through OFMT. */
static void
write_value(
	FILE *stream,
	const struct value *value)
{
	struct value string;

	/* The string form for output. */
	memset(&string, 0, sizeof(string));
	value_string(value, 1, &string);
	fwrite(string.text, 1, string.length, stream);
	value_free(&string);
}

/* Evaluates lvalue = value and lvalue op= value. */
static void
run_assign(
	struct node *expression,
	struct value *value)
{
	struct value right;
	struct value old;
	struct place place;
	double old_number;
	double right_number;
	double result;

	/* The value first, then where it goes. */
	memset(&right, 0, sizeof(right));
	memset(&old, 0, sizeof(old));
	run_expression(expression->right, &right);
	place_resolve(expression->left, &place);

	/* A plain assignment stores the value as it is. */
	if (expression->operator == TOKEN_ASSIGN) {
		place_write(&place, &right);
		place_release(&place);
		value_move(value, &right);
		return;
	}

	/* Succeeded: the operator on the old value and the new. */
	place_read(&place, &old);
	old_number = value_number(&old);
	right_number = value_number(&right);

	/* The operator the assignment stands for. */
	switch (expression->operator) {
	case TOKEN_ADD_ASSIGN:
		result = arithmetic(TOKEN_PLUS, old_number, right_number);
		break;
	case TOKEN_SUBTRACT_ASSIGN:
		result = arithmetic(TOKEN_MINUS, old_number, right_number);
		break;
	case TOKEN_MULTIPLY_ASSIGN:
		result = arithmetic(TOKEN_STAR, old_number, right_number);
		break;
	case TOKEN_DIVIDE_ASSIGN:
		result = arithmetic(TOKEN_SLASH, old_number, right_number);
		break;
	case TOKEN_MODULO_ASSIGN:
		result = arithmetic(TOKEN_PERCENT, old_number, right_number);
		break;
	default:
		result = arithmetic(TOKEN_CARET, old_number, right_number);
		break;
	}

	/* Succeeded: the result is stored and is the value. */
	value_set_number(value, result);
	place_write(&place, value);
	place_release(&place);
	value_free(&old);
	value_free(&right);
}

/* Evaluates ++ and --, before or after the lvalue. */
static void
run_increment(
	struct node *expression,
	struct value *value)
{
	struct value old;
	struct value updated;
	struct place place;
	double number;
	double step;

	/* The old number. */
	memset(&old, 0, sizeof(old));
	memset(&updated, 0, sizeof(updated));
	place_resolve(expression->left, &place);
	place_read(&place, &old);
	number = value_number(&old);
	value_free(&old);

	/* The new one, stored. */
	step = 1;
	if (expression->operator == TOKEN_DECREMENT)
		step = -1;
	value_set_number(&updated, number + step);
	place_write(&place, &updated);
	place_release(&place);

	/* Succeeded: the new number before, the old one after. */
	if (expression->kind == NODE_PRE_INCREMENT)
		value_move(value, &updated);
	else
		value_set_number(value, number);
	value_free(&updated);
}

/* Evaluates + - * / % and ^. */
static void
run_arithmetic(
	struct node *expression,
	struct value *value)
{
	struct value left;
	struct value right;
	double left_number;
	double right_number;
	double result;

	/* Both operands, as numbers. */
	memset(&left, 0, sizeof(left));
	memset(&right, 0, sizeof(right));
	run_expression(expression->left, &left);
	run_expression(expression->right, &right);
	left_number = value_number(&left);
	right_number = value_number(&right);
	value_free(&left);
	value_free(&right);

	/* The operator on them. */
	result = arithmetic(expression->operator, left_number, right_number);

	/* Succeeded. */
	value_set_number(value, result);
}

/* Returns the result of an arithmetic operator. */
static double
arithmetic(
	int operator,
	double left,
	double right)
{
	double result;

	/* The operator. */
	switch (operator) {
	case TOKEN_PLUS:
		result = left + right;
		break;
	case TOKEN_MINUS:
		result = left - right;
		break;
	case TOKEN_STAR:
		result = left * right;
		break;
	case TOKEN_SLASH:
		if (right == 0)
			awk_fatal("division by zero attempted");
		result = left / right;
		break;
	case TOKEN_PERCENT:
		if (right == 0)
			awk_fatal("division by zero attempted in %%");
		result = fmod(left, right);
		break;
	default:
		result = pow(left, right);
		break;
	}

	/* Succeeded. */
	return result;
}

/* Evaluates < <= == != >= >. */
static void
run_compare(
	struct node *expression,
	struct value *value)
{
	struct value left;
	struct value right;
	int order;
	int truth;

	/* The order of the two values. */
	memset(&left, 0, sizeof(left));
	memset(&right, 0, sizeof(right));
	run_expression(expression->left, &left);
	run_expression(expression->right, &right);
	order = value_compare(&left, &right);
	value_free(&left);
	value_free(&right);

	/* Whether the operator holds for the order. */
	truth = 0;
	switch (expression->operator) {
	case TOKEN_LESS:
		if (order < 0)
			truth = 1;
		break;
	case TOKEN_LESS_EQUAL:
		if (order <= 0)
			truth = 1;
		break;
	case TOKEN_EQUAL:
		if (order == 0)
			truth = 1;
		break;
	case TOKEN_NOT_EQUAL:
		if (order != 0)
			truth = 1;
		break;
	case TOKEN_GREATER_EQUAL:
		if (order >= 0)
			truth = 1;
		break;
	default:
		if (order > 0)
			truth = 1;
		break;
	}

	/* Succeeded. */
	value_set_number(value, (double)truth);
}

/* Evaluates ~ and !~. */
static void
run_match(
	struct node *expression,
	struct value *value)
{
	struct value subject;
	struct value scratch;
	regex_t *regex;
	size_t match_start;
	size_t match_end;
	int found;

	/* The subject as a string, then the regex. */
	memset(&subject, 0, sizeof(subject));
	memset(&scratch, 0, sizeof(scratch));
	run_expression(expression->left, &subject);
	value_string(&subject, 0, &subject);
	regex = regex_of(expression->right, &scratch);
	found = regex_search(regex, subject.text, subject.length, 0, &match_start, &match_end);
	value_free(&subject);
	value_free(&scratch);

	/* !~ holds when there is no match. */
	if (expression->operator == TOKEN_NOT_TILDE) {
		if (found)
			found = 0;
		else
			found = 1;
	}

	/* Succeeded. */
	value_set_number(value, (double)found);
}

/* Evaluates two expressions side by side. */
static void
run_concatenate(
	struct node *expression,
	struct value *value)
{
	struct value left;
	struct value right;
	char *text;
	size_t length;

	/* Both strings. */
	memset(&left, 0, sizeof(left));
	memset(&right, 0, sizeof(right));
	run_expression(expression->left, &left);
	value_string(&left, 0, &left);
	run_expression(expression->right, &right);
	value_string(&right, 0, &right);

	/* One after the other. */
	length = left.length + right.length;
	text = awk_allocate(length);
	memcpy(text, left.text, left.length);
	memcpy(text + left.length, right.text, right.length);
	text[length] = '\0';
	value_free(&left);
	value_free(&right);

	/* Succeeded: the value takes the text. */
	value_free(value);
	value->type = VALUE_STRING;
	value->text = text;
	value->length = length;
}

/* Evaluates name[subscripts], making the element if absent. */
static void
run_element(
	struct node *expression,
	struct value *value)
{
	struct array *array;
	struct element *element;
	struct value key;

	/* The key, then the array. */
	memset(&key, 0, sizeof(key));
	subscript_key(expression->arguments, &key);
	array = node_array(expression->array);

	/* Succeeded: the element's value. */
	element = array_find(array, key.text, key.length, 1);
	value_copy(value, &element->value);
	value_free(&key);
}

/* Evaluates (subscripts) in array, without making the element. */
static void
run_in(
	struct node *expression,
	struct value *value)
{
	struct array *array;
	struct element *element;
	struct value key;
	int found;

	/* The key, then whether the array has it. */
	memset(&key, 0, sizeof(key));
	subscript_key(expression->arguments, &key);
	array = node_array(expression->array);
	element = array_find(array, key.text, key.length, 0);
	value_free(&key);

	/* Succeeded. */
	found = 0;
	if (element != NULL)
		found = 1;
	value_set_number(value, (double)found);
}

/*
 * Calls a function of the program: the arguments are bound to the
 * parameters (scalars by value, arrays by reference), the parameters past
 * them are unset locals, and the body runs in a frame of its own.
 */
static void
run_call(
	struct node *call,
	struct value *value)
{
	struct function *function;
	struct frame *frame;
	struct node *argument;
	size_t index;
	int flow;

	/* No more arguments than parameters. */
	function = call->function;
	if (call->argument_count > function->parameter_count) {
		awk_fatal("function %s called with %lu arguments, declared with %lu",
			  function->name,
			  (unsigned long)call->argument_count,
			  (unsigned long)function->parameter_count);
	}

	/* The frame, with the arguments bound in the caller's frame. */
	frame = awk_allocate(sizeof(*frame));
	frame->count = function->parameter_count;
	frame->cells = awk_allocate(sizeof(*frame->cells) * frame->count);
	index = 0;
	for (argument = call->arguments; argument != NULL; argument = argument->next) {
		bind_argument(argument, &frame->cells[index]);
		index++;
	}

	/* The body, in the frame. */
	frame->caller = awk.frame;
	awk.frame = frame;
	flow = run_statement(function->body);
	awk.frame = frame->caller;

	/* A next inside goes on to the rule that called. */
	if (flow == FLOW_NEXT || flow == FLOW_NEXTFILE)
		awk.pending_flow = flow;

	/* Succeeded: what return gave, and the frame goes. */
	value_move(value, &frame->result);
	for (index = 0; index < frame->count; index++)
		cell_release(&frame->cells[index]);
	free(frame->cells);
	free(frame);
}

/*
 * Binds an argument to a parameter's cell: an array is shared, an unset
 * variable is referred to (so that the function may make it an array),
 * and anything else is copied.
 */
static void
bind_argument(
	struct node *argument,
	struct cell *cell)
{
	struct cell *source;
	struct cell *target;
	struct value value;

	/* An expression other than a name is a value. */
	source = node_cell(argument);
	if (source == NULL) {
		memset(&value, 0, sizeof(value));
		run_expression(argument, &value);
		cell->kind = CELL_SCALAR;
		value_move(&cell->value, &value);
		return;
	}

	/* The cell the name stands for. */
	target = source;
	while (target->kind == CELL_REFERENCE)
		target = target->target;

	/* An array is shared. */
	if (target->kind == CELL_ARRAY) {
		cell->kind = CELL_ARRAY;
		cell->array = target->array;
		cell->owns_array = 0;
		return;
	}

	/* An unset variable is referred to. */
	if (target->kind == CELL_UNSET) {
		cell->kind = CELL_REFERENCE;
		cell->target = target;
		return;
	}

	/* Succeeded: a scalar is copied, unless the name only referred to it. */
	if (source->kind == CELL_REFERENCE)
		return;
	cell->kind = CELL_SCALAR;
	value_copy(&cell->value, &target->value);
}

/*
 * Evaluates getline: the next record of the main input (counted in NR and
 * FNR), of a file, or of a command, into $0 or an lvalue.  The value is 1
 * for a record, 0 at the end, and -1 when the file or command cannot be
 * opened.
 */
static void
run_getline(
	struct node *expression,
	struct value *value)
{
	struct buffer buffer;
	struct value record;
	struct value name;
	struct place place;
	int read;

	/* The record, from where the source says. */
	memset(&buffer, 0, sizeof(buffer));
	memset(&name, 0, sizeof(name));
	if (expression->source == GETLINE_MAIN) {
		read = input_next(&buffer);
	} else {
		run_expression(expression->right, &name);
		value_string(&name, 0, &name);
		read = io_getline(expression->source, name.text, name.length, &buffer);
		value_free(&name);
	}

	/* The end, or a file or command that cannot be opened. */
	if (read <= 0) {
		free(buffer.data);
		value_set_number(value, (double)read);
		return;
	}

	/* Into the lvalue, or into $0. */
	if (expression->left != NULL) {
		memset(&record, 0, sizeof(record));
		value_set_input(&record, buffer.data, buffer.length);
		place_resolve(expression->left, &place);
		place_write(&place, &record);
		place_release(&place);
		value_free(&record);
	} else {
		record_set(buffer.data, buffer.length);
	}

	/* Succeeded: a record was read. */
	free(buffer.data);
	value_set_number(value, 1);
}

/* Returns whether a regex matches $0. */
static int
regex_matches_record(
	regex_t *regex)
{
	struct value record;
	size_t match_start;
	size_t match_end;
	int found;

	/* The record as a string. */
	memset(&record, 0, sizeof(record));
	field_read(0, &record);
	value_string(&record, 0, &record);
	found = regex_search(regex, record.text, record.length, 0, &match_start, &match_end);
	value_free(&record);

	/* Succeeded. */
	return found;
}

/* Evaluates the index of a field, which must not be negative. */
static size_t
field_index(
	struct node *index)
{
	struct value value;
	double number;

	/* The number. */
	memset(&value, 0, sizeof(value));
	run_expression(index, &value);
	number = value_number(&value);
	value_free(&value);
	if (number < 0)
		awk_fatal("attempt to access field %.0f", number);

	/* Succeeded. */
	return (size_t)number;
}

/* Returns a flow, or a next from a function if one is on its way. */
static int
pass_pending(
	int flow)
{
	/* A statement that did not end normally keeps its flow. */
	if (flow != FLOW_NORMAL)
		return flow;

	/* Succeeded: the pending flow, if any. */
	return awk.pending_flow;
}

/* Flushes the output and ends awk with the exit status. */
static void
finish(
	void)
{
	int error;

	/* The files and commands, then standard output, whose failure is an error. */
	io_close_all();
	error = fflush(stdout);
	if (error != 0 && awk.exit_status == 0)
		awk.exit_status = 2;

	/* Succeeded: the end. */
	exit(awk.exit_status);
}
