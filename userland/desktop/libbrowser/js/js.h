/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * JavaScript (plan/ws074/design.md §12): the lexer and the parser that
 * turn a script's source into a syntax tree, which the compiler
 * (ws074-p025) turns into the shared bytecode.
 *
 * The parser is hand-written recursive descent over ES2024's grammar for
 * scripts and modules.  The tree lives in an arena that is thrown away
 * after compiling; its strings are the source's characters (cooked for
 * string literals and identifiers with escapes), copied into the arena.
 */

#ifndef KEILAND_BROWSER_JS_H
#define KEILAND_BROWSER_JS_H

#include "base/base.h"
#include "vm/vm.h"

/* How a source is parsed. */
#define JS_PARSE_STRICT		0x1U
#define JS_PARSE_MODULE		0x2U

/*
 * The kinds of syntax-tree node.  The comment of each names what its
 * fields hold (first, second, third, fourth are children; a list is a
 * first child and its next siblings).
 */
enum js_node_kind {
	/* The program: first is the list of statements (flags: strict, module). */
	JS_NODE_PROGRAM,

	/* Statements. */
	JS_NODE_VARIABLES,		/* op: var, let or const; first: the declarators */
	JS_NODE_DECLARATOR,		/* first: the target; second: the initializer or NULL */
	JS_NODE_FUNCTION_DECLARATION,	/* as JS_NODE_FUNCTION */
	JS_NODE_CLASS_DECLARATION,	/* as JS_NODE_CLASS */
	JS_NODE_BLOCK,			/* first: the statements */
	JS_NODE_EMPTY,			/* nothing */
	JS_NODE_EXPRESSION_STATEMENT,	/* first: the expression */
	JS_NODE_IF,			/* first: the test; second: the consequent; third: the alternate or NULL */
	JS_NODE_FOR,			/* first: init or NULL; second: test or NULL; third: update or NULL; fourth: body */
	JS_NODE_FOR_IN,			/* first: left; second: right; fourth: body */
	JS_NODE_FOR_OF,			/* first: left; second: right; fourth: body (flags: await) */
	JS_NODE_WHILE,			/* first: test; fourth: body */
	JS_NODE_DO_WHILE,		/* first: test; fourth: body */
	JS_NODE_CONTINUE,		/* text: the label or none */
	JS_NODE_BREAK,			/* text: the label or none */
	JS_NODE_RETURN,			/* first: the value or NULL */
	JS_NODE_WITH,			/* first: the object; fourth: body */
	JS_NODE_SWITCH,			/* first: the discriminant; second: the cases */
	JS_NODE_CASE,			/* first: the test (NULL for default); second: the statements */
	JS_NODE_LABELED,		/* text: the label; fourth: body */
	JS_NODE_THROW,			/* first: the value */
	JS_NODE_TRY,			/* first: block; second: catch parameter or NULL; third: catch block or NULL; fourth: finally or NULL */
	JS_NODE_DEBUGGER,		/* nothing */
	JS_NODE_IMPORT,			/* first: the specifiers; second: the module (a string) */
	JS_NODE_IMPORT_SPECIFIER,	/* first: the imported name (NULL: default, "*": namespace); second: the local name */
	JS_NODE_EXPORT,			/* first: a declaration, or the specifiers; second: the module or NULL (flags: default, all) */
	JS_NODE_EXPORT_SPECIFIER,	/* first: the local name; second: the exported name */

	/* Expressions. */
	JS_NODE_IDENTIFIER,		/* text: the name */
	JS_NODE_PRIVATE_NAME,		/* text: the name without # */
	JS_NODE_NUMBER,			/* number */
	JS_NODE_BIGINT,			/* text: the digits (with their prefix) */
	JS_NODE_STRING,			/* text: the cooked characters */
	JS_NODE_TEMPLATE,		/* first: the parts, strings and expressions in turn */
	JS_NODE_TEMPLATE_STRING,	/* text: cooked (flags: invalid cooked); raw text in raw */
	JS_NODE_TAGGED_TEMPLATE,	/* first: the tag; second: the template */
	JS_NODE_REGEXP,			/* text: the pattern; raw: the flags */
	JS_NODE_NULL,
	JS_NODE_TRUE,
	JS_NODE_FALSE,
	JS_NODE_THIS,
	JS_NODE_SUPER,
	JS_NODE_ARRAY,			/* first: the elements (JS_NODE_HOLE for an elision) */
	JS_NODE_HOLE,
	JS_NODE_OBJECT,			/* first: the properties */
	JS_NODE_PROPERTY,		/* op: kind; first: key; second: value (flags: computed, shorthand) */
	JS_NODE_FUNCTION,		/* text: name; first: parameters; second: body (flags: arrow, async, generator, expression body) */
	JS_NODE_CLASS,			/* text: name; first: heritage or NULL; second: members */
	JS_NODE_METHOD,			/* op: kind; first: key; second: the function (flags: static, computed) */
	JS_NODE_FIELD,			/* first: key; second: initializer or NULL (flags: static, computed) */
	JS_NODE_STATIC_BLOCK,		/* first: the statements */
	JS_NODE_UNARY,			/* op; first: operand */
	JS_NODE_UPDATE,			/* op; first: operand (flags: prefix) */
	JS_NODE_BINARY,			/* op; first, second */
	JS_NODE_LOGICAL,		/* op; first, second */
	JS_NODE_ASSIGN,			/* op; first: target; second: value */
	JS_NODE_CONDITIONAL,		/* first: test; second: consequent; third: alternate */
	JS_NODE_CALL,			/* first: callee; second: arguments (flags: optional) */
	JS_NODE_NEW,			/* first: callee; second: arguments */
	JS_NODE_MEMBER,			/* first: object; second: property (flags: computed, optional) */
	JS_NODE_OPTIONAL_CHAIN,		/* first: the chain, whose short-circuit ends here */
	JS_NODE_SEQUENCE,		/* first: the expressions */
	JS_NODE_SPREAD,			/* first: the argument */
	JS_NODE_YIELD,			/* first: argument or NULL (flags: delegate) */
	JS_NODE_AWAIT,			/* first: argument */
	JS_NODE_META_PROPERTY,		/* text: "new.target" or "import.meta" */
	JS_NODE_IMPORT_CALL,		/* first: the specifier; second: options or NULL */
	JS_NODE_PARENTHESIZED,		/* first: the expression (kept to tell (a) = 1 from a = 1 where it matters) */

	/* Patterns (binding and assignment targets). */
	JS_NODE_ARRAY_PATTERN,		/* first: the elements (JS_NODE_HOLE for an elision) */
	JS_NODE_OBJECT_PATTERN,		/* first: the properties (JS_NODE_PROPERTY, value is the target) */
	JS_NODE_ASSIGNMENT_PATTERN,	/* first: target; second: default */
	JS_NODE_REST,			/* first: the target */

	JS_NODE_KINDS
};

/* The flags of a node. */
#define JS_FLAG_STRICT		0x0001U
#define JS_FLAG_MODULE		0x0002U
#define JS_FLAG_AWAIT		0x0004U
#define JS_FLAG_ASYNC		0x0008U
#define JS_FLAG_GENERATOR	0x0010U
#define JS_FLAG_ARROW		0x0020U
#define JS_FLAG_EXPRESSION_BODY	0x0040U
#define JS_FLAG_COMPUTED	0x0080U
#define JS_FLAG_SHORTHAND	0x0100U
#define JS_FLAG_STATIC		0x0200U
#define JS_FLAG_OPTIONAL	0x0400U
#define JS_FLAG_PREFIX		0x0800U
#define JS_FLAG_DELEGATE	0x1000U
#define JS_FLAG_DEFAULT		0x2000U
#define JS_FLAG_ALL		0x4000U
#define JS_FLAG_INVALID_COOKED	0x8000U
#define JS_FLAG_METHOD		0x10000U
#define JS_FLAG_BLOCK_FUNCTION	0x20000U	/* set by the compiler: a function declaration made when its block is entered */
#define JS_FLAG_STATIC_INIT	0x40000U	/* made by the compiler: the function that runs a class's static fields and blocks */
#define JS_FLAG_IMPORT_CALL	0x80000U	/* made by the parser: the call that stands for import() */

/* The kinds of property and method (op of JS_NODE_PROPERTY and JS_NODE_METHOD). */
enum js_property_kind {
	JS_PROPERTY_INIT,
	JS_PROPERTY_GET,
	JS_PROPERTY_SET,
	JS_PROPERTY_METHOD,
	JS_PROPERTY_SPREAD,
	JS_PROPERTY_CONSTRUCTOR
};

struct js_scope;

/*
 * One node of the syntax tree, in the parse's arena.
 *
 * What the fields hold depends on the kind (enum js_node_kind); next links
 * the members of a list.  offset and line say where the node starts.  word
 * is the parser's number for the word text spells (0 for most texts).
 * scope is the compiler's note: the scope a function, the program or a
 * catch clause opens (NULL until the compiler's scope pass).
 */
struct js_node {
	int kind;
	int op;
	int word;
	uint32_t flags;
	uint32_t offset;
	uint32_t line;
	uint32_t column;
	struct js_node *first;
	struct js_node *second;
	struct js_node *third;
	struct js_node *fourth;
	struct js_node *next;
	const uint16_t *text;
	size_t text_length;
	const uint16_t *raw;
	size_t raw_length;
	double number;
	struct js_scope *scope;
};

/*
 * A parsed program: its tree and the arena every node and string of it
 * lives in.
 */
struct js_program {
	struct wb_arena arena;
	struct js_node *root;
};

/*
 * Why a source is not a script: the place (1-based line and column) and a
 * message.  unsupported says the source is a script but uses what the
 * compiler does not support yet.
 */
struct js_syntax_error {
	uint32_t line;
	uint32_t column;
	int unsupported;
	char message[160];
};

/* Parsing (parser.c). */
int js_parse(const uint16_t *source, size_t length, unsigned how, struct js_program *program, struct js_syntax_error *error);
void js_program_release(struct js_program *program);

/* Compiling and running (compile.c). */
struct js_module;
/* Names are borrowed from the module and remain stable until its release. */
struct js_module_import_info {
	const uint16_t *specifier;
	size_t specifier_length;
	const uint16_t *imported;
	size_t imported_length;
	const uint16_t *local;
	size_t local_length;
	/* The import attributes: the value of type when there is one, and whether another key came. */
	const uint16_t *type;
	size_t type_length;
	int has_type;
	int unknown_key;
};
int js_compile(struct vm_realm *realm, struct js_program *program, struct vm_function **function, struct js_syntax_error *error);
int js_run_script(struct vm_realm *realm, const uint16_t *source, size_t length, unsigned how, vm_value *result, struct js_syntax_error *error);
int js_module_create(struct vm_realm *realm, const uint16_t *source, size_t length, struct js_module **module, struct js_syntax_error *error);
int js_module_evaluate(struct js_module *module);
int js_module_export(struct js_module *module, const uint16_t *name, size_t length, vm_value *value);
int js_module_namespace(struct js_module *module, vm_value *object);
size_t js_module_import_count(const struct js_module *module);
int js_module_import_at(const struct js_module *module, size_t index, struct js_module_import_info *info);
int js_module_link_import(struct js_module *module, const uint16_t *specifier, size_t specifier_length, const uint16_t *local, size_t local_length, struct js_module *target, const uint16_t *exported, size_t exported_length);
void js_module_release(struct js_module *module);

/* The host resolves each import relative to its owning module, then fetches one canonical URL. */
struct js_module_graph;
typedef int (*js_module_resolve)(void *context, const char *base, const uint16_t *specifier,
	size_t length, struct wb_buffer *url);
typedef int (*js_module_fetch)(void *context, const char *url, struct wb_units *source);
int js_module_graph_set_json_fetch(struct js_module_graph *graph, js_module_fetch fetch);
int js_module_graph_create(struct vm_realm *realm, js_module_resolve resolve,
	js_module_fetch fetch, void *context, struct js_module_graph **graph);
int js_module_graph_run(struct js_module_graph *graph, const char *url,
	struct js_syntax_error *error);
int js_module_graph_import(struct js_module_graph *graph, const char *url, int json, struct js_syntax_error *error, vm_value *namespace_object);
void js_module_graph_release(struct js_module_graph *graph);

/* The built-in objects (builtin.c). */
int js_install_builtins(struct vm_realm *realm);

/* The attributes of a built-in method: writable and configurable, not enumerable. */
#define JS_BUILTIN_METHOD	(VM_PROPERTY_WRITABLE | VM_PROPERTY_CONFIGURABLE)

/* Reports an argument of a native call, undefined when the call has fewer. */
static __inline vm_value
js_argument(
	const vm_value *args,
	unsigned count,
	unsigned index)
{
	/* A missing argument. */
	if (index >= count)
		return VM_VALUE_UNDEFINED;

	/* The argument. */
	return args[index];
}

/* Making built-ins, and the host's objects made like them (builtin.c). */
int js_builtin_function(struct vm_realm *realm, const char *name, unsigned length, vm_native native, vm_native construct, struct vm_function **function);
int js_builtin_method(struct vm_realm *realm, struct vm_object *object, const char *name, unsigned length, vm_native native);
int js_builtin_value(struct vm_realm *realm, struct vm_object *object, const char *name, vm_value value, uint32_t attributes);
int js_builtin_accessor(struct vm_realm *realm, struct vm_object *object, const char *name, vm_native getter, vm_native setter);
int js_builtin_constructor(struct vm_realm *realm, const char *name, unsigned length, vm_native native, vm_native construct, struct vm_object *prototype, struct vm_function **function);
struct vm_function *js_builtin_callee(const struct vm_realm *realm);
int js_builtin_string(struct vm_realm *realm, const char *text, vm_value *value);
int js_builtin_array(struct vm_realm *realm, const vm_value *values, uint32_t count, vm_value *array);
int js_builtin_integer(struct vm_realm *realm, vm_value value, double *integer);
int js_builtin_length(struct vm_realm *realm, vm_value object, uint32_t *length);

/* The script shell's own functions (script.c). */
int js_define_print(struct vm_realm *realm);
int js_exception_text(struct vm_realm *realm, vm_value exception, struct wb_buffer *out);

/* The tree as text (dump.c). */
int js_dump(const struct js_node *node, struct wb_buffer *out);
const char *js_node_kind_name(int kind);

#endif
