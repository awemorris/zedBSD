/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The pattern scanning language (POSIX XCU awk): what a program parses to,
 * what values and variables are, and what the files of awk offer one
 * another.
 *
 * The program parses to a tree of nodes, which is walked to run it.  A
 * value is unset, a number, a string, or a numeric string (a string from
 * the input that looks like a number, which compares as a number).  A
 * variable is a cell that holds a scalar or an array; a function's
 * parameter may instead refer to its caller's unset variable, so that the
 * function can make it an array.
 */

#ifndef KERN_USERLAND_BASE_AWK_AWK_H
#define KERN_USERLAND_BASE_AWK_AWK_H

#include <regex.h>
#include <stddef.h>
#include <stdio.h>

/* The kinds of value. */
#define VALUE_UNSET	0	/* never assigned: both "" and 0 */
#define VALUE_NUMBER	1	/* a number */
#define VALUE_STRING	2	/* a string */
#define VALUE_STRNUM	3	/* a string from the input that looks numeric */

/* The kinds of cell. */
#define CELL_UNSET	0	/* neither a scalar nor an array yet */
#define CELL_SCALAR	1	/* a scalar value */
#define CELL_ARRAY	2	/* an array */
#define CELL_REFERENCE	3	/* a parameter bound to a caller's unset cell */

/* The variables awk itself gives a meaning to. */
#define SPECIAL_NONE		0
#define SPECIAL_NF		1
#define SPECIAL_NR		2
#define SPECIAL_FNR		3
#define SPECIAL_FS		4
#define SPECIAL_OFS		5
#define SPECIAL_ORS		6
#define SPECIAL_RS		7
#define SPECIAL_SUBSEP		8
#define SPECIAL_CONVFMT		9
#define SPECIAL_OFMT		10
#define SPECIAL_RSTART		11
#define SPECIAL_RLENGTH		12
#define SPECIAL_FILENAME	13
#define SPECIAL_ENVIRON		14
#define SPECIAL_ARGC		15
#define SPECIAL_ARGV		16
#define SPECIAL_COUNT		17

/* The tokens of the language. */
#define TOKEN_EOF		0
#define TOKEN_NEWLINE		1
#define TOKEN_LEFT_BRACE	2
#define TOKEN_RIGHT_BRACE	3
#define TOKEN_LEFT_PAREN	4
#define TOKEN_RIGHT_PAREN	5
#define TOKEN_LEFT_BRACKET	6
#define TOKEN_RIGHT_BRACKET	7
#define TOKEN_SEMICOLON		8
#define TOKEN_COMMA		9
#define TOKEN_PLUS		10
#define TOKEN_MINUS		11
#define TOKEN_STAR		12
#define TOKEN_SLASH		13
#define TOKEN_PERCENT		14
#define TOKEN_CARET		15
#define TOKEN_NOT		16
#define TOKEN_GREATER		17
#define TOKEN_LESS		18
#define TOKEN_PIPE		19
#define TOKEN_QUESTION		20
#define TOKEN_COLON		21
#define TOKEN_TILDE		22
#define TOKEN_NOT_TILDE		23
#define TOKEN_DOLLAR		24
#define TOKEN_ASSIGN		25
#define TOKEN_ADD_ASSIGN	26
#define TOKEN_SUBTRACT_ASSIGN	27
#define TOKEN_MULTIPLY_ASSIGN	28
#define TOKEN_DIVIDE_ASSIGN	29
#define TOKEN_MODULO_ASSIGN	30
#define TOKEN_POWER_ASSIGN	31
#define TOKEN_EQUAL		32
#define TOKEN_NOT_EQUAL		33
#define TOKEN_LESS_EQUAL	34
#define TOKEN_GREATER_EQUAL	35
#define TOKEN_APPEND		36
#define TOKEN_INCREMENT		37
#define TOKEN_DECREMENT		38
#define TOKEN_AND		39
#define TOKEN_OR		40
#define TOKEN_NUMBER		41
#define TOKEN_STRING		42
#define TOKEN_REGEX		43
#define TOKEN_NAME		44
#define TOKEN_FUNCTION_NAME	45
#define TOKEN_BUILTIN		46
#define TOKEN_BEGIN		47
#define TOKEN_END		48
#define TOKEN_FUNCTION		49
#define TOKEN_GETLINE		50
#define TOKEN_PRINT		51
#define TOKEN_PRINTF		52
#define TOKEN_IF		53
#define TOKEN_ELSE		54
#define TOKEN_WHILE		55
#define TOKEN_FOR		56
#define TOKEN_DO		57
#define TOKEN_BREAK		58
#define TOKEN_CONTINUE		59
#define TOKEN_NEXT		60
#define TOKEN_NEXTFILE		61
#define TOKEN_EXIT		62
#define TOKEN_RETURN		63
#define TOKEN_DELETE		64
#define TOKEN_IN		65

/* The built-in functions. */
#define BUILTIN_LENGTH		0
#define BUILTIN_SUBSTR		1
#define BUILTIN_INDEX		2
#define BUILTIN_SPLIT		3
#define BUILTIN_SUB		4
#define BUILTIN_GSUB		5
#define BUILTIN_MATCH		6
#define BUILTIN_SPRINTF		7
#define BUILTIN_SIN		8
#define BUILTIN_COS		9
#define BUILTIN_ATAN2		10
#define BUILTIN_EXP		11
#define BUILTIN_LOG		12
#define BUILTIN_SQRT		13
#define BUILTIN_INT		14
#define BUILTIN_RAND		15
#define BUILTIN_SRAND		16
#define BUILTIN_TOLOWER		17
#define BUILTIN_TOUPPER		18
#define BUILTIN_SYSTEM		19
#define BUILTIN_CLOSE		20
#define BUILTIN_FFLUSH		21

/* The kinds of node: expressions first, then statements. */
#define NODE_NUMBER		0	/* a numeric constant */
#define NODE_STRING		1	/* a string constant */
#define NODE_REGEX		2	/* /ERE/: a match against $0 as a value */
#define NODE_VARIABLE		3	/* a global variable */
#define NODE_LOCAL		4	/* a function's parameter or local */
#define NODE_FIELD		5	/* $expression */
#define NODE_ELEMENT		6	/* name[subscripts] */
#define NODE_ASSIGN		7	/* lvalue op= expression */
#define NODE_CONDITIONAL	8	/* condition ? then : otherwise */
#define NODE_OR			9	/* || */
#define NODE_AND		10	/* && */
#define NODE_IN			11	/* (subscripts) in array */
#define NODE_MATCH		12	/* ~ and !~ */
#define NODE_COMPARE		13	/* < <= == != >= > */
#define NODE_CONCATENATE	14	/* two expressions side by side */
#define NODE_ARITHMETIC		15	/* + - * / % ^ */
#define NODE_NEGATE		16	/* unary - */
#define NODE_UNARY_PLUS		17	/* unary + */
#define NODE_NOT		18	/* ! */
#define NODE_PRE_INCREMENT	19	/* ++lvalue and --lvalue */
#define NODE_POST_INCREMENT	20	/* lvalue++ and lvalue-- */
#define NODE_CALL		21	/* a call of a function of the program */
#define NODE_BUILTIN		22	/* a call of a built-in function */
#define NODE_GETLINE		23	/* the forms of getline */
#define NODE_GROUPING		24	/* (expression, expression...) */
#define NODE_BLOCK		25	/* { statements } */
#define NODE_EXPRESSION		26	/* an expression as a statement */
#define NODE_PRINT		27	/* print */
#define NODE_PRINTF		28	/* printf */
#define NODE_IF			29	/* if, else */
#define NODE_WHILE		30	/* while */
#define NODE_DO			31	/* do, while */
#define NODE_FOR		32	/* for (;;) */
#define NODE_FOR_IN		33	/* for (name in array) */
#define NODE_NEXT		34	/* next */
#define NODE_NEXTFILE		35	/* nextfile */
#define NODE_EXIT		36	/* exit */
#define NODE_RETURN		37	/* return */
#define NODE_BREAK		38	/* break */
#define NODE_CONTINUE		39	/* continue */
#define NODE_DELETE		40	/* delete name[subscripts] and delete name */

/* Where getline reads from. */
#define GETLINE_MAIN		0	/* the main input */
#define GETLINE_FILE		1	/* getline < file */
#define GETLINE_COMMAND		2	/* command | getline */

/* Where print and printf write to. */
#define OUTPUT_STANDARD		0	/* standard output */
#define OUTPUT_FILE		1	/* > file */
#define OUTPUT_APPEND		2	/* >> file */
#define OUTPUT_PIPE		3	/* | command */

/* How a statement finished, which the statements around it act on. */
#define FLOW_NORMAL		0	/* go on with the next statement */
#define FLOW_BREAK		1	/* leave the innermost loop */
#define FLOW_CONTINUE		2	/* start the next turn of the loop */
#define FLOW_NEXT		3	/* go on with the next record */
#define FLOW_NEXTFILE		4	/* go on with the next input file */
#define FLOW_RETURN		5	/* leave the function */

/* The kinds of rule. */
#define RULE_BEGIN		0
#define RULE_MAIN		1
#define RULE_END		2

/*
 * A value.  text is owned by the value and ends with a NUL (the length
 * counts bytes without it); it is NULL for an unset value and a number.
 */
struct value {
	int type;
	double number;
	char *text;
	size_t length;
};

/*
 * One element of an array.  An element is in the chain of its hash bucket
 * and in the list of the array in the order the elements were made, which
 * is the order for (name in array) visits them in.
 */
struct element {
	char *key;
	size_t key_length;
	unsigned long hash;
	struct value value;
	struct element *bucket_next;
	struct element *previous;
	struct element *next;
};

/* An array: a hash table of elements keyed by strings. */
struct array {
	struct element **buckets;
	size_t bucket_count;
	size_t count;
	struct element *first;
	struct element *last;
};

/*
 * What a variable holds.  An array held by a cell is freed with the cell
 * only when owns_array is set; a parameter that was passed an array shares
 * it with the caller.  target is the caller's cell of a reference.
 */
struct cell {
	int kind;
	struct value value;
	struct array *array;
	int owns_array;
	struct cell *target;
};

/* A global variable, in the chain of its hash bucket. */
struct variable {
	char *name;
	struct cell cell;
	int special;
	struct variable *next;
};

/* A function of the program. */
struct function {
	char *name;
	char **parameters;
	size_t parameter_count;
	struct node *body;
	int defined;
	int first_use_line;
	struct function *next;
};

/*
 * A node of the parsed program.  Each kind uses the fields its comment in
 * the NODE_ list names; the others stay zero.
 *
 *	left, right	the operands of a binary operator; the lvalue and
 *			the value of an assignment; the subject and the
 *			regex of a match; the operand of a unary operator;
 *			the index of a field; the source of getline (file
 *			or command, in right) and its lvalue (in left)
 *	condition	of ?:, if, while, do and for
 *	then, otherwise	the arms of ?: and if
 *	initial, step	of for (;;)
 *	body		of a loop and a block (the first statement)
 *	arguments	of a call, print, printf, a grouping, and the
 *			subscripts of an element, in and delete (a list
 *			linked by next)
 *	destination	of print and printf, with how in output
 *	array		the array of an element, in, for-in and delete
 *	next		the next node of a list
 */
struct node {
	int kind;
	int operator;
	int line;
	struct node *left;
	struct node *right;
	struct node *condition;
	struct node *then;
	struct node *otherwise;
	struct node *initial;
	struct node *step;
	struct node *body;
	struct node *arguments;
	struct node *destination;
	struct node *array;
	struct node *next;
	int output;
	int source;
	double number;
	char *text;
	size_t length;
	regex_t *regex;
	struct variable *variable;
	size_t local;
	struct function *function;
	int builtin;
	size_t argument_count;
};

/*
 * A rule: a pattern (none, an expression, or a range of two) and an action
 * (NULL prints the record).  in_range is set while a range rule is between
 * the records its two patterns match.
 */
struct rule {
	int kind;
	struct node *pattern;
	struct node *pattern_end;
	struct node *action;
	int in_range;
	struct rule *next;
};

/* Where an lvalue is. */
#define PLACE_CELL		0	/* a variable or a parameter */
#define PLACE_ELEMENT		1	/* an element of an array, by its key */
#define PLACE_FIELD		2	/* a field */

/*
 * An lvalue resolved once, so that reading and then writing it does not
 * evaluate its subscripts or its field index twice.  variable is the
 * global variable of a cell, NULL for a parameter; an element is found
 * again by its key, since the array may grow in between.
 */
struct place {
	int kind;
	struct cell *cell;
	struct variable *variable;
	struct array *array;
	struct value key;
	size_t field;
};

/* A token of the program, as the lexer gives it to the parser. */
struct token {
	int kind;
	int line;
	size_t start;
	double number;
	char *text;
	size_t length;
	int builtin;
};

/*
 * The call of a function being run: its parameters and locals, and what
 * return gave.
 */
struct frame {
	struct cell *cells;
	size_t count;
	struct value result;
	struct frame *caller;
};

/* A growing byte string. */
struct buffer {
	char *data;
	size_t length;
	size_t capacity;
};

/*
 * The state of the program being run.  It is filled by the parser and by
 * main, and lives until awk ends.
 */
struct awk_state {
	/* The rules in program order, and the functions. */
	struct rule *begin_rules;
	struct rule *main_rules;
	struct rule *end_rules;
	struct function *functions;

	/* The special variables, indexed by SPECIAL_*. */
	struct variable *specials[SPECIAL_COUNT];

	/* The record ($0) and its fields ($1 on, at index 1 on). */
	struct value record;
	struct value *fields;
	size_t field_count;
	size_t field_capacity;

	/* The main input: the stream, and the next ARGV index to look at. */
	FILE *input;
	int input_is_stdin;
	double argument_index;
	int input_used_file;
	int input_ended;

	/* The call being run, NULL outside every function. */
	struct frame *frame;

	/* Set while the program is parsed, when an error is a syntax error. */
	int parsing;

	/* Set while the END rules run; exit then ends awk at once. */
	int in_end;
	int exit_status;

	/* A next or nextfile run inside a function, on its way to the rule. */
	int pending_flow;

	/* The state and the seed of rand(). */
	double random_seed;
	unsigned long long random_state;
};

/* The state of the program being run, defined in main.c. */
extern struct awk_state awk;

/* lex.c */
void lex_start(const char *source, size_t length);
void lex_next(struct token *token);
void lex_regex(struct token *token, size_t start);
int lex_line(void);

/* parse.c */
void parse_program(const char *source, size_t length);

/* value.c */
void value_free(struct value *value);
void value_set_number(struct value *value, double number);
void value_set_text(struct value *value, const char *text, size_t length);
void value_set_input(struct value *value, const char *text, size_t length);
void value_copy(struct value *to, const struct value *from);
void value_move(struct value *to, struct value *from);
double value_number(const struct value *value);
void value_string(const struct value *value, int output, struct value *string);
int value_truth(const struct value *value);
int value_compare(const struct value *left, const struct value *right);
int value_is_numeric(const struct value *value);
double text_number(const char *text);
int text_looks_numeric(const char *text, size_t length, double *number);
size_t format_number(double number, const char *format, struct buffer *buffer);
struct variable *variable_find(const char *name, int create);
struct array *array_new(void);
struct element *array_find(struct array *array, const char *key, size_t length, int create);
void array_remove(struct array *array, const char *key, size_t length);
void array_clear(struct array *array);
void array_free(struct array *array);
struct array *cell_array(struct cell *cell);
void cell_read(struct cell *cell, struct value *value);
void cell_write(struct cell *cell, const struct value *value);
void cell_release(struct cell *cell);
void buffer_append(struct buffer *buffer, const char *data, size_t length);
void buffer_append_byte(struct buffer *buffer, char byte);
void *awk_allocate(size_t size);
void *awk_reallocate(void *memory, size_t size);
char *awk_copy(const char *text, size_t length);
void awk_fatal(const char *format, ...);

/* record.c */
void record_set(const char *text, size_t length);
void record_set_value(const struct value *value);
void record_set_field_count(size_t count);
void field_read(size_t index, struct value *value);
void field_write(size_t index, const struct value *value);
int record_read(FILE *stream, struct buffer *buffer);
int input_next(struct buffer *buffer);
void input_skip_file(void);
size_t split_text(const char *text, size_t length, const char *separator, size_t separator_length, regex_t *regex, struct array *array);
const char *special_text(int special, size_t *length);
void special_assigned(struct variable *variable);
int command_assignment(const char *operand);

/* run.c */
int run_statement(struct node *statement);
void run_expression(struct node *expression, struct value *value);
void run_rules(struct rule *rules);
int run_main_rules(void);
void run_exit(void);
void assign_variable(struct variable *variable, const struct value *value);
struct array *node_array(struct node *node);
void subscript_key(struct node *subscripts, struct value *key);
struct cell *node_cell(struct node *node);
void place_resolve(struct node *lvalue, struct place *place);
void place_read(struct place *place, struct value *value);
void place_write(struct place *place, const struct value *value);
void place_release(struct place *place);

/* io.c */
FILE *io_output(int output, const char *name, size_t length);
int io_getline(int source, const char *name, size_t length, struct buffer *buffer);
int io_close(const char *name, size_t length);
int io_flush(const char *name, size_t length);
int io_system(const char *command);
void io_close_all(void);

/* builtin.c */
void builtin_call(struct node *call, struct value *value);
void format_values(const char *format, size_t length, struct value *arguments, size_t count, struct buffer *buffer);
regex_t *regex_compile(const char *text, size_t length, int cached);
regex_t *regex_of(struct node *expression, struct value *scratch);
int regex_search(regex_t *regex, const char *text, size_t length, size_t from, size_t *match_start, size_t *match_end);

#endif
