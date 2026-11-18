/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The inside of zedBSD's GLSL compiler: the arena, the tokens, the types,
 * the syntax tree, the symbols and the constants that the preprocessor,
 * the parser, the checker and the SPIR-V emitter share.
 *
 * Everything a compile makes lives in the compile's arena and goes with
 * the shader.  An allocation that fails does not return: it jumps back to
 * the entry of the compile or the link, which reports "out of memory", so
 * the code that builds trees and lists is not interleaved with checks.
 */

#ifndef GLSL_INTERNAL_H
#define GLSL_INTERNAL_H

#include "glsl.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/* The most errors a compile reports before it gives up. */
#define GLSL_MAX_ERRORS		16U

/* The versions the compiler takes. */
#define GLSL_VERSION_ES100	100U
#define GLSL_VERSION_110	110U
#define GLSL_VERSION_120	120U
#define GLSL_VERSION_130	130U
#define GLSL_VERSION_140	140U
#define GLSL_VERSION_150	150U
#define GLSL_VERSION_ES300	300U
#define GLSL_VERSION_330	330U
#define GLSL_VERSION_ES310	310U

/* Desktop GLSL 4.00 to 4.60 (a 4.x shader's version mask is 3.30's and GLSL_IN_400). */
#define GLSL_VERSION_400	400U
#define GLSL_VERSION_410	410U
#define GLSL_VERSION_420	420U
#define GLSL_VERSION_430	430U
#define GLSL_VERSION_440	440U
#define GLSL_VERSION_450	450U
#define GLSL_VERSION_460	460U

/* The suffix of a double constant (lf or LF) in glsl_token.suffix. */
#define GLSL_SUFFIX_DOUBLE	2U

/* The kinds of tokens. */
#define GLSL_TOKEN_EOF		0U
#define GLSL_TOKEN_IDENTIFIER	1U
#define GLSL_TOKEN_INT		2U
#define GLSL_TOKEN_UINT		3U
#define GLSL_TOKEN_FLOAT	4U
#define GLSL_TOKEN_PUNCT	5U
#define GLSL_TOKEN_NEWLINE	6U
#define GLSL_TOKEN_OTHER	7U

/* The punctuators. */
enum glsl_punct {
	GLSL_P_NONE = 0,
	GLSL_P_LPAREN,
	GLSL_P_RPAREN,
	GLSL_P_LBRACKET,
	GLSL_P_RBRACKET,
	GLSL_P_LBRACE,
	GLSL_P_RBRACE,
	GLSL_P_DOT,
	GLSL_P_COMMA,
	GLSL_P_SEMICOLON,
	GLSL_P_COLON,
	GLSL_P_QUESTION,
	GLSL_P_PLUS,
	GLSL_P_MINUS,
	GLSL_P_STAR,
	GLSL_P_SLASH,
	GLSL_P_PERCENT,
	GLSL_P_LT,
	GLSL_P_GT,
	GLSL_P_LE,
	GLSL_P_GE,
	GLSL_P_EQ,
	GLSL_P_NE,
	GLSL_P_AND_AND,
	GLSL_P_OR_OR,
	GLSL_P_XOR_XOR,
	GLSL_P_BANG,
	GLSL_P_TILDE,
	GLSL_P_AMP,
	GLSL_P_BAR,
	GLSL_P_CARET,
	GLSL_P_SHL,
	GLSL_P_SHR,
	GLSL_P_ASSIGN,
	GLSL_P_ADD_ASSIGN,
	GLSL_P_SUB_ASSIGN,
	GLSL_P_MUL_ASSIGN,
	GLSL_P_DIV_ASSIGN,
	GLSL_P_MOD_ASSIGN,
	GLSL_P_SHL_ASSIGN,
	GLSL_P_SHR_ASSIGN,
	GLSL_P_AND_ASSIGN,
	GLSL_P_OR_ASSIGN,
	GLSL_P_XOR_ASSIGN,
	GLSL_P_INC,
	GLSL_P_DEC,
	GLSL_P_HASH,
	GLSL_P_HASH_HASH
};

/* The keywords (the built-in type names are GLSL_K_TYPE with the type). */
enum glsl_keyword {
	GLSL_K_NONE = 0,
	GLSL_K_TYPE,
	GLSL_K_RESERVED,
	GLSL_K_ATTRIBUTE,
	GLSL_K_CONST,
	GLSL_K_UNIFORM,
	GLSL_K_VARYING,
	GLSL_K_BREAK,
	GLSL_K_CONTINUE,
	GLSL_K_DO,
	GLSL_K_FOR,
	GLSL_K_WHILE,
	GLSL_K_IF,
	GLSL_K_ELSE,
	GLSL_K_IN,
	GLSL_K_OUT,
	GLSL_K_INOUT,
	GLSL_K_TRUE,
	GLSL_K_FALSE,
	GLSL_K_LOWP,
	GLSL_K_MEDIUMP,
	GLSL_K_HIGHP,
	GLSL_K_PRECISION,
	GLSL_K_INVARIANT,
	GLSL_K_DISCARD,
	GLSL_K_RETURN,
	GLSL_K_STRUCT,
	GLSL_K_SWITCH,
	GLSL_K_CASE,
	GLSL_K_DEFAULT,
	GLSL_K_FLAT,
	GLSL_K_SMOOTH,
	GLSL_K_NOPERSPECTIVE,
	GLSL_K_CENTROID,
	GLSL_K_LAYOUT,

	/* GLSL ES 3.10's storage and memory qualifiers (ws101-p008). */
	GLSL_K_BUFFER,
	GLSL_K_SHARED,
	GLSL_K_COHERENT,
	GLSL_K_VOLATILE,
	GLSL_K_RESTRICT,
	GLSL_K_READONLY,
	GLSL_K_WRITEONLY,
	GLSL_K_PATCH,

	/* GLSL 4.00's (GL_ARB_gpu_shader5's) qualifier that forbids contracting a value's operations. */
	GLSL_K_PRECISE,

	/* GLSL 4.00's (GL_ARB_shader_subroutine's) subroutine types, functions and uniforms. */
	GLSL_K_SUBROUTINE
};

/* The kinds of types. */
#define GLSL_KIND_VOID		0U
#define GLSL_KIND_SCALAR	1U
#define GLSL_KIND_VECTOR	2U
#define GLSL_KIND_MATRIX	3U
#define GLSL_KIND_SAMPLER	4U
#define GLSL_KIND_STRUCT	5U
#define GLSL_KIND_ARRAY		6U
#define GLSL_KIND_ERROR		7U

/* The scalar bases (GLSL_BASE_NONE for void, structs, arrays and errors). */
#define GLSL_BASE_NONE		0U
#define GLSL_BASE_BOOL		1U
#define GLSL_BASE_INT		2U
#define GLSL_BASE_UINT		3U
#define GLSL_BASE_FLOAT		4U

/* A 64-bit float (desktop GLSL 4.00, or 1.50 and 3.30 with GL_ARB_gpu_shader_fp64). */
#define GLSL_BASE_DOUBLE	5U

/* The storage qualifiers of a declaration. */
#define GLSL_STORAGE_NONE	0U
#define GLSL_STORAGE_CONST	1U
#define GLSL_STORAGE_ATTRIBUTE	2U
#define GLSL_STORAGE_UNIFORM	3U
#define GLSL_STORAGE_VARYING	4U
#define GLSL_STORAGE_IN		5U
#define GLSL_STORAGE_OUT	6U
#define GLSL_STORAGE_INOUT	7U

/* A shader storage block, and a workgroup's shared variable (GLSL ES 3.10, ws101-p008). */
#define GLSL_STORAGE_BUFFER	8U
#define GLSL_STORAGE_SHARED	9U

/* The memory qualifiers of a shader storage block or its member (GLSL ES 3.10, ws101-p008). */
#define GLSL_MEMORY_COHERENT	0x01U
#define GLSL_MEMORY_VOLATILE	0x02U
#define GLSL_MEMORY_RESTRICT	0x04U
#define GLSL_MEMORY_READONLY	0x08U
#define GLSL_MEMORY_WRITEONLY	0x10U

/* The interpolation qualifiers. */
#define GLSL_INTERP_NONE	0U
#define GLSL_INTERP_SMOOTH	1U
#define GLSL_INTERP_FLAT	2U
#define GLSL_INTERP_NOPERSPECTIVE 3U

/* The precision qualifiers. */
#define GLSL_PRECISION_NONE	0U
#define GLSL_PRECISION_LOW	1U
#define GLSL_PRECISION_MEDIUM	2U
#define GLSL_PRECISION_HIGH	3U

/* What a variable symbol is to the emitter: where it lives. */
#define GLSL_VAR_LOCAL		0U
#define GLSL_VAR_GLOBAL		1U
#define GLSL_VAR_CONST		2U
#define GLSL_VAR_UNIFORM	3U
#define GLSL_VAR_INPUT		4U
#define GLSL_VAR_OUTPUT		5U
#define GLSL_VAR_PARAMETER	6U
#define GLSL_VAR_BLOCK		7U
#define GLSL_VAR_BLOCK_MEMBER	8U

/*
 * A shader storage block (by its instance name), a member of one without
 * an instance name, and a workgroup's shared variable (ws101-p008).
 */
#define GLSL_VAR_BUFFER		9U
#define GLSL_VAR_BUFFER_MEMBER	10U
#define GLSL_VAR_SHARED		11U

/* An image uniform (GLSL ES 3.10): a storage image of its own at set 0, by its unit. */
#define GLSL_VAR_IMAGE		12U

/* The built-in variables (0: a variable the shader declared). */
#define GLSL_BUILTIN_NONE	0U
#define GLSL_BUILTIN_POSITION	1U
#define GLSL_BUILTIN_POINT_SIZE	2U
#define GLSL_BUILTIN_VERTEX_ID	3U
#define GLSL_BUILTIN_FRAG_COORD	4U
#define GLSL_BUILTIN_FRONT_FACING 5U
#define GLSL_BUILTIN_POINT_COORD 6U
#define GLSL_BUILTIN_FRAG_COLOR	7U
#define GLSL_BUILTIN_FRAG_DATA	8U
#define GLSL_BUILTIN_FRAG_DEPTH	9U
#define GLSL_BUILTIN_INSTANCE_ID 10U
#define GLSL_BUILTIN_PER_VERTEX	11U
#define GLSL_BUILTIN_PRIMITIVE_ID_IN 12U
#define GLSL_BUILTIN_PRIMITIVE_ID 13U
#define GLSL_BUILTIN_LAYER	14U

/* A compute shader's built-in inputs (ws101-p008). */
#define GLSL_BUILTIN_GLOBAL_INVOCATION_ID 15U
#define GLSL_BUILTIN_LOCAL_INVOCATION_ID 16U
#define GLSL_BUILTIN_WORK_GROUP_ID 17U
#define GLSL_BUILTIN_NUM_WORK_GROUPS 18U
#define GLSL_BUILTIN_LOCAL_INVOCATION_INDEX 19U

/* The tessellation shaders' built-ins: the levels, the coordinate, the patch's vertices and the invocation. */
#define GLSL_BUILTIN_TESS_LEVEL_OUTER 20U
#define GLSL_BUILTIN_TESS_LEVEL_INNER 21U
#define GLSL_BUILTIN_TESS_COORD	22U
#define GLSL_BUILTIN_PATCH_VERTICES 23U
#define GLSL_BUILTIN_INVOCATION_ID 24U

/* Per-sample shading's fragment built-ins: the sample, its position, and the sample masks in and out. */
#define GLSL_BUILTIN_SAMPLE_ID	25U
#define GLSL_BUILTIN_SAMPLE_POSITION 26U
#define GLSL_BUILTIN_SAMPLE_MASK_IN 27U
#define GLSL_BUILTIN_SAMPLE_MASK 28U

/*
 * A geometry shader's primitives (the layouts "in" and "out" give), and
 * how many vertices each input primitive has, by its number.
 */
#define GLSL_PRIMITIVE_NONE	0U
#define GLSL_PRIMITIVE_POINTS	1U
#define GLSL_PRIMITIVE_LINES	2U
#define GLSL_PRIMITIVE_LINES_ADJACENCY 3U
#define GLSL_PRIMITIVE_TRIANGLES 4U
#define GLSL_PRIMITIVE_TRIANGLES_ADJACENCY 5U
#define GLSL_PRIMITIVE_LINE_STRIP 6U
#define GLSL_PRIMITIVE_TRIANGLE_STRIP 7U

/* A tessellation evaluation shader's other domains (its triangles are GLSL_PRIMITIVE_TRIANGLES). */
#define GLSL_PRIMITIVE_QUADS	8U
#define GLSL_PRIMITIVE_ISOLINES	9U

/* A tessellation evaluation shader's spacings and vertex orders (0: not given; equal spacing and ccw then). */
#define GLSL_SPACING_EQUAL	1U
#define GLSL_SPACING_FRACTIONAL_EVEN 2U
#define GLSL_SPACING_FRACTIONAL_ODD 3U
#define GLSL_ORDER_CW		1U
#define GLSL_ORDER_CCW		2U

/* gl_MaxPatchVertices: the size of a tessellation shader's unsized per-vertex inputs. */
#define GLSL_MAX_PATCH_VERTICES	32U

/* The kinds of symbols. */
#define GLSL_SYMBOL_VARIABLE	1U
#define GLSL_SYMBOL_FUNCTION	2U
#define GLSL_SYMBOL_TYPE	3U

/*
 * A subroutine type (its prototype is the symbol's function) and a
 * subroutine uniform (GLSL 4.00, GL_ARB_shader_subroutine): the uniform's
 * type is the uint (or the array of them) of its hidden uniform, which
 * holds the index of the subroutine a call runs.
 */
#define GLSL_SYMBOL_SUBROUTINE	4U
#define GLSL_SYMBOL_SUBROUTINE_UNIFORM 5U

/* The most subroutine functions of a shader, and the most locations of its subroutine uniforms (GL's least maximums). */
#define GLSL_MAX_SUBROUTINES	256U
#define GLSL_MAX_SUBROUTINE_LOCATIONS 1024U

/* The kinds of syntax tree nodes. */
enum glsl_node_kind {
	/* Expressions. */
	GLSL_N_INT = 1,
	GLSL_N_UINT,
	GLSL_N_FLOAT,
	GLSL_N_DOUBLE,
	GLSL_N_BOOL,
	GLSL_N_IDENTIFIER,
	GLSL_N_UNARY,
	GLSL_N_PREINC,
	GLSL_N_PREDEC,
	GLSL_N_POSTINC,
	GLSL_N_POSTDEC,
	GLSL_N_BINARY,
	GLSL_N_ASSIGN,
	GLSL_N_TERNARY,
	GLSL_N_COMMA,
	GLSL_N_CALL,
	GLSL_N_FIELD,
	GLSL_N_INDEX,
	GLSL_N_LENGTH,
	GLSL_N_CONVERT,

	/* Types and declarations. */
	GLSL_N_TYPE,
	GLSL_N_DECLARATION,
	GLSL_N_VARIABLE,
	GLSL_N_FUNCTION,
	GLSL_N_PARAMETER,
	GLSL_N_PRECISION,
	GLSL_N_INVARIANT,

	/* Statements. */
	GLSL_N_BLOCK,
	GLSL_N_EXPRESSION,
	GLSL_N_IF,
	GLSL_N_FOR,
	GLSL_N_WHILE,
	GLSL_N_DO,
	GLSL_N_SWITCH,
	GLSL_N_CASE,
	GLSL_N_DEFAULT,
	GLSL_N_RETURN,
	GLSL_N_BREAK,
	GLSL_N_CONTINUE,
	GLSL_N_DISCARD,
	GLSL_N_EMPTY,

	/* An interface block: uniform, in or out NAME { members } [instance]. */
	GLSL_N_INTERFACE
};

/* The flags of a node. */
#define GLSL_NODE_ARRAY		0x0001U
#define GLSL_NODE_UNSIZED	0x0002U
#define GLSL_NODE_SWIZZLE	0x0004U
#define GLSL_NODE_CONSTRUCTOR	0x0008U
#define GLSL_NODE_SCOPE		0x0010U
#define GLSL_NODE_CONST_PARAM	0x0020U
#define GLSL_NODE_VOID_PARAMS	0x0040U
#define GLSL_NODE_SIDE_EFFECTS	0x0080U
#define GLSL_NODE_STRUCT_DEFINITION 0x0100U

/*
 * GLSL 4.00's subroutines: a function that subroutine types name
 * ("subroutine(T, ...) ..."; child[3] is the list of the types' names),
 * the declaration of a subroutine type ("subroutine T NAME(...);"), the
 * declaration of subroutine uniforms ("subroutine uniform T NAME;"), and
 * a call through one (child[0] is then the hidden uniform that selects
 * the subroutine, child[2] an array's index while parsed).
 */
#define GLSL_NODE_SUBROUTINE	0x0200U
#define GLSL_NODE_SUBROUTINE_TYPE 0x0400U
#define GLSL_NODE_SUBROUTINE_UNIFORM 0x0800U
#define GLSL_NODE_SUBROUTINE_CALL 0x1000U

/*
 * A memory arena: the blocks one compile or one link allocates from, all
 * freed together.  An allocation that fails jumps to `failure`.
 */
struct glsl_arena_block;

struct glsl_arena {
	struct glsl_arena_block *blocks;
	jmp_buf *failure;
};

/*
 * A growing text: an info log or a name being built.
 */
struct glsl_text {
	char *data;
	size_t length;
	size_t capacity;
};

struct glsl_type;

/*
 * One token of the source, before or after preprocessing.
 *
 * The text points into the source or into the arena (for a token a
 * macro paste or __LINE__ made); it is not terminated.
 */
struct glsl_token {
	/* GLSL_TOKEN_*, the punctuator of a GLSL_TOKEN_PUNCT. */
	unsigned kind;
	unsigned punct;

	/* The spelling and its length. */
	const char *text;
	unsigned length;

	/* The line it came from, and whether white space came before it. */
	unsigned line;
	unsigned space;

	/* Nonzero when a macro of its name is being expanded around it, so it is never expanded again. */
	unsigned noexpand;

	/*
	 * The value of a number, and whether it carried a suffix (a float's
	 * f, an integer's u: 1; a double's lf: GLSL_SUFFIX_DOUBLE); a
	 * double's value is read at its own precision.
	 */
	uint32_t integer;
	float number;
	double number_double;
	unsigned suffix;

	/* The parser's classification of an identifier: whether it was made, the keyword, a built-in type's type. */
	unsigned classified;
	unsigned keyword;
	const struct glsl_type *keyword_type;
};

/*
 * One member of a struct type.
 */
struct glsl_field {
	const char *name;
	const struct glsl_type *type;
	unsigned row_major;
	unsigned interpolation;
};

/*
 * A GLSL type.
 *
 * Scalars, vectors, matrices and samplers are static tables shared by
 * every compile (types.c); structs and arrays are made in a compile's
 * arena.  Two struct types are the same type only when they are the same
 * object (the same declaration); two array types are the same when their
 * elements and lengths are.
 */
struct glsl_type {
	/* GLSL_KIND_*, and GLSL_BASE_* of the scalars (a sampler's is the type its texels are read as). */
	unsigned kind;
	unsigned base;

	/* The components of a vector or of a matrix column, and a matrix's columns (1 otherwise). */
	unsigned components;
	unsigned columns;

	/* A sampler: GLSL_SAMPLER_*, and whether it is a shadow sampler. */
	unsigned sampler;
	unsigned shadow;

	/* The name GLSL spells the type with (NULL for an array). */
	const char *name;

	/* A struct's members. */
	struct glsl_field *fields;
	unsigned field_count;

	/* An array's element type and length. */
	const struct glsl_type *element;
	unsigned length;

	/* A sampler of an array of layers. */
	unsigned arrayed;

	/* An interface block's struct: in, out or uniform (GLSL_STORAGE_*), 0 for any other struct. */
	unsigned block;

	/*
	 * An image type (GLSL ES 3.10; its kind is GLSL_KIND_SAMPLER, an
	 * opaque type as a sampler is, with the dimension, layers and texel
	 * base of one): nonzero, and the format of a variable's own type (the
	 * layout's GLSL_IMAGE_FORMAT_*), 0 for the type a parameter names.
	 */
	unsigned image;
	unsigned image_format;
};

/*
 * A scalar of a constant value (a double takes the whole of it; the
 * other bases only their 32 bits).
 */
union glsl_scalar {
	float f;
	int32_t i;
	uint32_t u;
	double d;
};

/*
 * A constant value: its type and its scalars, flattened (vectors by
 * component, matrices column after column, arrays and structs element by
 * element and member by member; a bool is 0 or 1 in u).
 */
struct glsl_constant {
	const struct glsl_type *type;
	unsigned count;
	union glsl_scalar *values;
};

struct glsl_symbol;
struct glsl_function;
struct glsl_builtin;

/*
 * A node of the syntax tree: an expression, a statement, a declaration or
 * a type specifier.
 *
 * The parser fills the kind, the line, the operator, the children, the
 * name and the qualifiers; the checker fills the type, the symbol or
 * function a node refers to, a constant value, and the swizzle or member
 * a selection picks.
 */
struct glsl_node {
	/* enum glsl_node_kind, the line, and the punctuator of an operator. */
	unsigned kind;
	unsigned line;
	unsigned op;

	/* The children (their meaning depends on the kind), and the next node of a list. */
	struct glsl_node *child[4];
	struct glsl_node *next;

	/* A name: of an identifier, a field, a function, a declared variable or a struct. */
	const char *name;

	/* The value of a literal (a double's at its own precision). */
	uint32_t integer;
	float number;
	double number_double;

	/* GLSL_NODE_* flags. */
	unsigned flags;

	/* The qualifiers of a type specifier or a parameter, and a layout's flags, location, primitive (GLSL_PRIMITIVE_*) and most vertices. */
	unsigned storage;
	unsigned interpolation;
	unsigned precision;
	unsigned centroid;
	unsigned invariant;
	unsigned layout;
	unsigned location;
	unsigned primitive;
	unsigned max_vertices;

	/* A fragment output's layout index: which input of a dual-source blend it is (desktop GLSL 3.30). */
	unsigned output_index;

	/* GL_ARB_gpu_shader5: the precise qualifier, and a geometry shader's invocations and stream layouts. */
	unsigned precise;
	unsigned invocations;
	unsigned stream;

	/*
	 * Tessellation: the patch qualifier, and a layout's output vertices
	 * (a control shader's), spacing, vertex order and point mode (an
	 * evaluation shader's; its domain is the primitive).
	 */
	unsigned patch;
	unsigned vertices;
	unsigned spacing;
	unsigned order;
	unsigned point_mode;

	/* GLSL ES 3.10 (ws101-p008): the memory qualifiers (GLSL_MEMORY_*), a layout's binding and workgroup size. */
	unsigned memory;
	unsigned binding;
	unsigned local_size[3];

	/* An image uniform's layout format (GLSL_IMAGE_FORMAT_*, 0: none). */
	unsigned image_format;

	/* The type: a type specifier's built-in type as parsed, an expression's once checked. */
	const struct glsl_type *type;

	/* What the checker resolved: a variable, a user function, a built-in function. */
	struct glsl_symbol *symbol;
	struct glsl_function *function;
	const struct glsl_builtin *builtin;

	/* The value of an expression that is constant. */
	struct glsl_constant *constant;

	/* A selection: the member of a struct, or the components of a swizzle. */
	unsigned field;
	unsigned swizzle[4];
	unsigned swizzle_count;
};

/*
 * A declared name: a variable, a function (with its overloads) or a
 * struct type.
 *
 * A variable symbol also carries what the emitter needs: the SPIR-V id of
 * the variable holding it while the code that declares it is emitted.
 */
struct glsl_symbol {
	/* The name, GLSL_SYMBOL_*, and the line it was declared on. */
	const char *name;
	unsigned kind;
	unsigned line;

	/* A variable's or a struct's type. */
	const struct glsl_type *type;

	/* A variable's qualifiers, GLSL_VAR_* and GLSL_BUILTIN_*. */
	unsigned storage;
	unsigned interpolation;
	unsigned precision;
	unsigned centroid;
	unsigned invariant;
	unsigned where;
	unsigned builtin;

	/* A const variable's value, and a global's constant initializer. */
	struct glsl_constant *constant;
	struct glsl_constant *initial;

	/* A function's overloads. */
	struct glsl_function *functions;

	/* Whether the code main reaches reads or writes it, and whether it is written. */
	unsigned used;
	unsigned written;

	/*
	 * An array of uniform blocks: the elements the code main reaches
	 * reads (bit i element i; every element when an index is computed).
	 */
	uint32_t elements;

	/* The next global of the shader, in declaration order. */
	struct glsl_symbol *next_global;

	/* The location a layout qualifier gave (GLSL_NO_LOCATION: none). */
	unsigned explicit_location;

	/* A fragment output's layout index (0 without one; 1 is the second colour a dual-source blend reads). */
	unsigned explicit_index;

	/* A tessellation shader's patch variable (a control shader's output, an evaluation shader's input), one per patch. */
	unsigned patch;

	/* The vertex stream a geometry shader's output goes to (its stream layout, or the default one before it; GL_ARB_gpu_shader5). */
	unsigned stream;

	/* A uniform block's member without an instance name: the block's symbol and the member's index. */
	struct glsl_symbol *block;
	unsigned member;

	/* A shader storage block's memory qualifiers and the binding its layout gave (ws101-p008); an image uniform's too. */
	unsigned memory;
	unsigned layout_binding;

	/*
	 * An image uniform that an atomic function uses (set while the link
	 * emits), and while an inlined function is emitted, the image uniform
	 * an image parameter stands for.
	 */
	unsigned image_atomic;
	struct glsl_symbol *image_root;

	/* While an inlined function is emitted, the image unit of the image (or of the first element of the array) an image parameter stands for. */
	unsigned image_unit;

	/*
	 * A subroutine uniform's hidden uniform (its uint, or its array of
	 * them) and the next subroutine uniform of the shader, in declaration
	 * order; a subroutine type's prototype is in functions.
	 */
	struct glsl_symbol *hidden;
	struct glsl_symbol *next_subroutine;

	/* What the link gave: an input's or output's location, a uniform's index among the program's uniforms, a block's binding. */
	unsigned location;
	unsigned uniform;
	unsigned binding;

	/* While emitting: the SPIR-V id of the variable, and its storage class. */
	uint32_t id;
	unsigned id_storage;
};

/*
 * A user function: one overload of a name.
 */
struct glsl_function {
	/* The name, the return type, the line of the first declaration. */
	const char *name;
	const struct glsl_type *return_type;
	unsigned line;

	/* The parameters (variable symbols with GLSL_VAR_PARAMETER). */
	struct glsl_symbol **parameters;
	unsigned parameter_count;

	/* The definition's body (NULL while only declared). */
	struct glsl_node *body;

	/* The next overload of the name. */
	struct glsl_function *next;

	/* Whether it returns other than as its body's last statement (the emitter needs a loop to leave it). */
	unsigned early_return;

	/* Set while main's call graph walks through it (static recursion), and once it has been walked. */
	unsigned visiting;
	unsigned visited;

	/*
	 * A subroutine function: the subroutine types it is one of (their
	 * prototypes), its index among the shader's subroutine functions, and
	 * the next one, in declaration order (GLSL 4.00).
	 */
	struct glsl_function **subroutine_types;
	unsigned subroutine_type_count;
	unsigned subroutine_index;
	struct glsl_function *next_subroutine;
};

/*
 * A built-in function: one signature of the table in builtins.c.
 *
 * The signature is a string of type codes (types.c glsl_builtin_match):
 * the return type first, then the parameters.
 */
struct glsl_builtin {
	/* The name, and the signature: return type code, then parameter codes. */
	const char *name;
	const char *signature;

	/* GLSL_BI_* saying how it is emitted, and the GLSL.std.450 or SPIR-V number. */
	unsigned operation;
	unsigned number;

	/* The stages it exists in (bit 0 vertex, bit 1 fragment, bit 2 geometry), and the versions. */
	unsigned stages;
	unsigned versions;
};

/* The versions a built-in exists in. */
#define GLSL_IN_ES100		0x001U
#define GLSL_IN_110		0x002U
#define GLSL_IN_120		0x004U
#define GLSL_IN_130		0x008U
#define GLSL_IN_DERIVATIVES	0x010U
#define GLSL_IN_140		0x020U
#define GLSL_IN_150		0x040U
#define GLSL_IN_330		0x080U
#define GLSL_IN_ES300		0x100U

/*
 * OpenGL ES 3.10 (ws101-p008): a 3.10 shader's version mask has both
 * GLSL_IN_ES300 and this bit, so it has everything 3.00 has.  A desktop
 * 4.30 shader's mask has this bit too: the compute shaders and shader
 * storage blocks it stands for are 4.30's as well.
 */
#define GLSL_IN_ES310		0x200U

/*
 * Desktop GLSL 4.00 on: a 4.x shader's version mask has GLSL_IN_330 and
 * this bit, so it has everything 3.30 has.  GLSL_IN_TESS is in the mask
 * of a shader that has tessellation (4.00 on, or 1.50 and 3.30 with
 * GL_ARB_tessellation_shader).
 */
#define GLSL_IN_400		0x400U
#define GLSL_IN_TESS		0x800U

/* Per-sample shading's built-ins: 4.00 on, or 1.50 and 3.30 with GL_ARB_sample_shading. */
#define GLSL_IN_SAMPLE		0x1000U

/* textureGather and textureGatherOffset: 4.00 on, or 1.30 to 3.30 with GL_ARB_texture_gather. */
#define GLSL_IN_GATHER		0x2000U

/* The cube map array samplers and their lookups: 4.00 on, or 1.30 to 3.30 with GL_ARB_texture_cube_map_array. */
#define GLSL_IN_CUBE_ARRAY	0x4000U

/* The double types and their built-ins: 4.00 on, or 1.50 and 3.30 with GL_ARB_gpu_shader_fp64. */
#define GLSL_IN_FP64		0x8000U

/*
 * GL_ARB_gpu_shader5's integer and floating-point built-ins and its
 * implicit conversion of int to uint: 4.00 on, or 1.50 and 3.30 with
 * the extension.
 */
#define GLSL_IN_SHADER5		0x10000U

/*
 * The level of detail a fragment shader's lookup would take: 4.00's
 * textureQueryLod, and GL_ARB_texture_query_lod's textureQueryLOD (1.30
 * on, with the extension).
 */
#define GLSL_IN_QUERY_LOD	0x20000U
#define GLSL_IN_QUERY_LOD_ARB	0x40000U

/* Subroutines: 4.00 on, or 1.50 and 3.30 with GL_ARB_shader_subroutine. */
#define GLSL_IN_SUBROUTINE	0x80000U

/* The versions that share a feature. */
#define GLSL_IN_ALL		0x1efU
#define GLSL_IN_DESKTOP		0x0eeU
#define GLSL_IN_ES		0x101U
#define GLSL_IN_120_UP		0x1ecU
#define GLSL_IN_130_UP		0x1e8U
#define GLSL_IN_DESKTOP_130_UP	0x0e8U
#define GLSL_IN_140_UP		0x1e0U
#define GLSL_IN_DESKTOP_140_UP	0x0e0U
#define GLSL_IN_150_UP		0x0c0U
#define GLSL_IN_330_UP		0x180U
#define GLSL_IN_OLD		0x007U
#define GLSL_IN_LEGACY		0x0efU

/* How a built-in function is emitted. */
#define GLSL_BI_EXT		1U
#define GLSL_BI_OP		2U
#define GLSL_BI_SPECIAL		3U

/* The special built-ins (glsl_builtin.number when GLSL_BI_SPECIAL). */
#define GLSL_SPECIAL_TEXTURE	1U
#define GLSL_SPECIAL_TEXTURE_BIAS 2U
#define GLSL_SPECIAL_TEXTURE_PROJ 3U
#define GLSL_SPECIAL_TEXTURE_PROJ_BIAS 4U
#define GLSL_SPECIAL_TEXTURE_LOD 5U
#define GLSL_SPECIAL_TEXTURE_PROJ_LOD 6U
#define GLSL_SPECIAL_SHADOW	7U
#define GLSL_SPECIAL_SHADOW_PROJ 8U
#define GLSL_SPECIAL_TEXTURE_SIZE 9U
#define GLSL_SPECIAL_TEXEL_FETCH 10U
#define GLSL_SPECIAL_MOD	11U
#define GLSL_SPECIAL_DOT	12U
#define GLSL_SPECIAL_ANY	13U
#define GLSL_SPECIAL_ALL	14U
#define GLSL_SPECIAL_NOT	15U
#define GLSL_SPECIAL_COMPARE	16U
#define GLSL_SPECIAL_MATRIX_COMP_MULT 17U
#define GLSL_SPECIAL_NOISE	18U
#define GLSL_SPECIAL_FACEFORWARD 19U
#define GLSL_SPECIAL_MIX_BOOL	20U
#define GLSL_SPECIAL_MODF	21U
#define GLSL_SPECIAL_ATAN	22U
#define GLSL_SPECIAL_OUTER_PRODUCT 23U
#define GLSL_SPECIAL_TEXTURE_GRAD 24U
#define GLSL_SPECIAL_TEXTURE_OFFSET 25U
#define GLSL_SPECIAL_TEXTURE_OFFSET_BIAS 26U
#define GLSL_SPECIAL_TEXTURE_LOD_OFFSET 27U
#define GLSL_SPECIAL_EMIT_VERTEX 28U
#define GLSL_SPECIAL_END_PRIMITIVE 29U

/* GLSL ES 3.10's atomic memory functions and barriers (ws101-p008). */
#define GLSL_SPECIAL_ATOMIC_ADD	30U
#define GLSL_SPECIAL_ATOMIC_MIN	31U
#define GLSL_SPECIAL_ATOMIC_MAX	32U
#define GLSL_SPECIAL_ATOMIC_AND	33U
#define GLSL_SPECIAL_ATOMIC_OR	34U
#define GLSL_SPECIAL_ATOMIC_XOR	35U
#define GLSL_SPECIAL_ATOMIC_EXCHANGE 36U
#define GLSL_SPECIAL_ATOMIC_COMP_SWAP 37U
#define GLSL_SPECIAL_BARRIER	38U
#define GLSL_SPECIAL_MEMORY_BARRIER 39U
#define GLSL_SPECIAL_MEMORY_BARRIER_BUFFER 40U
#define GLSL_SPECIAL_MEMORY_BARRIER_SHARED 41U
#define GLSL_SPECIAL_GROUP_MEMORY_BARRIER 42U

/* Desktop GLSL 4.00's texture gathers (GL_ARB_texture_gather). */
#define GLSL_SPECIAL_TEXTURE_GATHER 43U
#define GLSL_SPECIAL_TEXTURE_GATHER_OFFSET 44U

/* The built-ins that write out arguments: frexp's exponent, the carry, the borrow, the halves of a product. */
#define GLSL_SPECIAL_FREXP	45U
#define GLSL_SPECIAL_UADD_CARRY	46U
#define GLSL_SPECIAL_USUB_BORROW 47U
#define GLSL_SPECIAL_UMUL_EXTENDED 48U
#define GLSL_SPECIAL_IMUL_EXTENDED 49U

/* GL_ARB_gpu_shader5's gather of four constant offsets, and a fragment input interpolated elsewhere than the shader's place. */
#define GLSL_SPECIAL_TEXTURE_GATHER_OFFSETS 50U
#define GLSL_SPECIAL_INTERPOLATE_AT_CENTROID 51U
#define GLSL_SPECIAL_INTERPOLATE_AT_SAMPLE 52U
#define GLSL_SPECIAL_INTERPOLATE_AT_OFFSET 53U

/* dFdy, taken along GL's rows (framebuffer 0 has them turned over against the target's). */
#define GLSL_SPECIAL_DFDY	54U

/* The level of detail a lookup would take (textureQueryLod). */
#define GLSL_SPECIAL_TEXTURE_QUERY_LOD 55U

/* A geometry shader's vertex and primitive end on a vertex stream (GL_ARB_gpu_shader5). */
#define GLSL_SPECIAL_EMIT_STREAM_VERTEX 56U
#define GLSL_SPECIAL_END_STREAM_PRIMITIVE 57U

/* GLSL ES 3.10's image functions, and the barrier of image memory. */
#define GLSL_SPECIAL_IMAGE_SIZE	58U
#define GLSL_SPECIAL_IMAGE_LOAD	59U
#define GLSL_SPECIAL_IMAGE_STORE 60U
#define GLSL_SPECIAL_IMAGE_ATOMIC_ADD 61U
#define GLSL_SPECIAL_IMAGE_ATOMIC_MIN 62U
#define GLSL_SPECIAL_IMAGE_ATOMIC_MAX 63U
#define GLSL_SPECIAL_IMAGE_ATOMIC_AND 64U
#define GLSL_SPECIAL_IMAGE_ATOMIC_OR 65U
#define GLSL_SPECIAL_IMAGE_ATOMIC_XOR 66U
#define GLSL_SPECIAL_IMAGE_ATOMIC_EXCHANGE 67U
#define GLSL_SPECIAL_IMAGE_ATOMIC_COMP_SWAP 68U
#define GLSL_SPECIAL_MEMORY_BARRIER_IMAGE 69U

/* The layout qualifiers of a declaration (glsl_node.layout). */
#define GLSL_LAYOUT_STD140	0x01U
#define GLSL_LAYOUT_SHARED	0x02U
#define GLSL_LAYOUT_PACKED	0x04U
#define GLSL_LAYOUT_ROW_MAJOR	0x08U
#define GLSL_LAYOUT_COLUMN_MAJOR 0x10U
#define GLSL_LAYOUT_LOCATION	0x20U
#define GLSL_LAYOUT_PRIMITIVE	0x40U
#define GLSL_LAYOUT_MAX_VERTICES 0x80U

/* GLSL ES 3.10's layouts (ws101-p008): std430, binding, a compute shader's workgroup size. */
#define GLSL_LAYOUT_STD430	0x100U
#define GLSL_LAYOUT_BINDING	0x200U
#define GLSL_LAYOUT_LOCAL_SIZE	0x400U

/* Desktop GLSL 3.30's index of a fragment output (dual-source blending). */
#define GLSL_LAYOUT_INDEX	0x800U

/* Tessellation's layouts: a control shader's output vertices, an evaluation shader's spacing, vertex order and point mode. */
#define GLSL_LAYOUT_VERTICES	0x1000U
#define GLSL_LAYOUT_SPACING	0x2000U
#define GLSL_LAYOUT_ORDER	0x4000U
#define GLSL_LAYOUT_POINT_MODE	0x8000U

/* A geometry shader's invocations of each primitive (GL_ARB_gpu_shader5). */
#define GLSL_LAYOUT_INVOCATIONS	0x10000U

/* The vertex stream a geometry shader's outputs go to (GL_ARB_gpu_shader5). */
#define GLSL_LAYOUT_STREAM	0x20000U

/* An image uniform's format (GLSL ES 3.10). */
#define GLSL_LAYOUT_FORMAT	0x40000U

/* How many vertex streams a geometry shader has (GL's GL_MAX_VERTEX_STREAMS). */
#define GLSL_MAX_VERTEX_STREAMS	4U

/* No location given. */
#define GLSL_NO_LOCATION	0xffffffffU

/*
 * The compile of one shader, and what stays of it for the link: the
 * checked tree, the globals and main.
 */
struct glsl_shader {
	/* The arena everything below lives in, and where a failed allocation or a fatal error goes. */
	struct glsl_arena arena;
	jmp_buf failure;

	/* The stage, the version, and whether it is OpenGL ES's language. */
	unsigned stage;
	unsigned version;
	unsigned es;

	/* The optional device features the shader may use (GLSL_FEATURE_*). */
	unsigned features;

	/* The info log and the errors reported so far. */
	struct glsl_text log;
	unsigned errors;

	/* The extensions the shader enabled. */
	unsigned derivatives;

	/*
	 * A geometry shader's layouts: the input primitive (GLSL_PRIMITIVE_*)
	 * and its vertices (which size the unsized inputs), the output
	 * primitive and the most vertices it emits (0 until given).
	 */
	unsigned geometry_input;
	unsigned geometry_vertices;
	unsigned geometry_output;
	unsigned max_vertices;

	/* How many times a geometry shader runs for each primitive (0 until its layout gives it: 1). */
	unsigned geometry_invocations;

	/*
	 * The vertex streams a geometry shader names (bit i: stream i), by its
	 * outputs' stream layouts and by EmitStreamVertex and
	 * EndStreamPrimitive (GL_ARB_gpu_shader5).
	 */
	unsigned geometry_streams;

	/* The stream a geometry shader's outputs declared from here on go to ("layout(stream = N) out;", 0 until one). */
	unsigned default_stream;

	/* Whether the shader declares anything precise (GL_ARB_gpu_shader5): no operation is then contracted. */
	unsigned precise;

	/*
	 * A compute shader (ws101-p008): its workgroup size (0s until its
	 * layout), and while main is checked, how many selections, loops and
	 * switches the statement is inside and whether main returned before
	 * it (a barrier() needs neither).
	 */
	unsigned local_size[3];
	unsigned control_depth;
	unsigned returned;

	/*
	 * Tessellation: whether GL_ARB_tessellation_shader was enabled (a
	 * desktop 1.50 or 3.30 shader; 4.00 on has it), a control shader's
	 * output vertices (0 until its layout), and an evaluation shader's
	 * domain (GLSL_PRIMITIVE_TRIANGLES, _QUADS, _ISOLINES; NONE until its
	 * layout), spacing, vertex order and point mode.
	 */
	unsigned tessellation;
	unsigned sample_shading;

	/* Whether GL_ARB_texture_gather was enabled (a desktop 1.30 to 3.30 shader; 4.00 on has its functions). */
	unsigned texture_gather;

	/* Whether GL_ARB_texture_cube_map_array was enabled (a desktop 1.30 to 3.30 shader; 4.00 on has its samplers). */
	unsigned cube_map_array;

	/* Whether GL_ARB_gpu_shader_fp64 was enabled (a desktop 1.50 or 3.30 shader; 4.00 on has the doubles). */
	unsigned fp64;

	/* Whether GL_ARB_gpu_shader5 was enabled (a desktop 1.50 or 3.30 shader; 4.00 on has its built-ins). */
	unsigned gpu_shader5;

	/* Whether GL_ARB_texture_query_lod was enabled (a desktop shader of 1.30 on; its function is textureQueryLOD). */
	unsigned texture_query_lod;

	/* Whether GL_ARB_shader_subroutine was enabled (a desktop 1.50 or 3.30 shader; 4.00 on has subroutines). */
	unsigned shader_subroutine;
	unsigned tess_vertices;
	unsigned tess_domain;
	unsigned tess_spacing;
	unsigned tess_order;
	unsigned tess_point_mode;

	/* The preprocessed tokens, and the parser's position in them. */
	struct glsl_token *tokens;
	unsigned token_count;
	unsigned position;

	/* The parser's scopes of struct type names. */
	struct glsl_parse_scope *type_scope;

	/* The translation unit: the external declarations in order. */
	struct glsl_node *unit;

	/* The checker's innermost scope, the function being checked, and loop and switch nesting. */
	struct glsl_scope *scope;
	struct glsl_function *current;
	unsigned loop_depth;
	unsigned switch_depth;

	/* The default precision of float in the current scope chain (ES fragment shaders have none until declared). */
	unsigned float_precision;

	/* The globals in declaration order, and main. */
	struct glsl_symbol *globals;
	struct glsl_symbol *last_global;
	struct glsl_function *main;

	/*
	 * A fragment shader's hidden uniform gl_ZedFragment (ws068-p004): what
	 * turns gl_FragCoord.y and gl_PointCoord.t into GL's directions; used
	 * when either is.
	 */
	struct glsl_symbol *zed_fragment;

	/*
	 * A compute shader's hidden uniform gl_ZedImages (GLSL ES 3.10): bit
	 * u set when image unit u holds an image its uniform may access, which
	 * libGLESv2 fills at each dispatch; used when an image is loaded,
	 * stored or changed by an atomic.
	 */
	struct glsl_symbol *zed_images;

	/*
	 * The subroutine functions and the subroutine uniforms, in
	 * declaration order, with their numbers (GLSL 4.00,
	 * GL_ARB_shader_subroutine).
	 */
	struct glsl_function *subroutines;
	struct glsl_function *last_subroutine;
	unsigned subroutine_count;
	struct glsl_symbol *subroutine_uniforms;
	struct glsl_symbol *last_subroutine_uniform;
	unsigned subroutine_uniform_count;
};

/*
 * A scope of the checker: the names declared in one block, function or
 * the global scope.
 */
struct glsl_scope_entry {
	struct glsl_symbol *symbol;
	struct glsl_scope_entry *next;
};

struct glsl_scope {
	struct glsl_scope_entry *entries;
	struct glsl_scope *parent;
	unsigned float_precision;
};

/* arena.c: memory, text and the info log. */
void *glsl_alloc(struct glsl_arena *arena, size_t size);
void glsl_arena_free(struct glsl_arena *arena);
char *glsl_strndup(struct glsl_arena *arena, const char *text, size_t length);
void glsl_text_append(struct glsl_text *text, const char *data, size_t length, jmp_buf *failure);
void glsl_text_printf(struct glsl_text *text, jmp_buf *failure, const char *format, ...);
void glsl_text_vprintf(struct glsl_text *text, jmp_buf *failure, const char *format, va_list arguments);
void glsl_error(struct glsl_shader *shader, unsigned line, const char *format, ...);
void glsl_warning(struct glsl_shader *shader, unsigned line, const char *format, ...);
void glsl_fatal(struct glsl_shader *shader, unsigned line, const char *format, ...);

/* lex.c: the tokens of a source. */
unsigned glsl_lex(struct glsl_shader *shader, const char *source, size_t length, unsigned first_line, struct glsl_token **tokens);
int glsl_token_is(const struct glsl_token *token, const char *text);

/* preprocess.c: the preprocessed tokens of a source. */
void glsl_preprocess(struct glsl_shader *shader, const char *source, unsigned default_version);

/* parse.c: the syntax tree of the tokens. */
void glsl_parse(struct glsl_shader *shader);

/* types.c: types. */
const struct glsl_type *glsl_type_void(void);
const struct glsl_type *glsl_type_error(void);
const struct glsl_type *glsl_type_scalar(unsigned base);
const struct glsl_type *glsl_type_vector(unsigned base, unsigned components);
const struct glsl_type *glsl_type_matrix(unsigned base, unsigned columns, unsigned rows);
int glsl_base_floating(unsigned base);
const struct glsl_type *glsl_type_sampler(unsigned base, unsigned sampler, unsigned shadow);
const struct glsl_type *glsl_type_image_format(struct glsl_arena *arena, const struct glsl_type *image, unsigned format);
unsigned glsl_image_format_base(unsigned format);
unsigned glsl_version_mask(const struct glsl_shader *shader);
int glsl_since(const struct glsl_shader *shader, unsigned desktop, unsigned es);
const struct glsl_type *glsl_type_named(const char *name, unsigned length, unsigned *versions);
const struct glsl_type *glsl_type_array(struct glsl_arena *arena, const struct glsl_type *element, unsigned length);
const struct glsl_type *glsl_type_column(const struct glsl_type *type);
const struct glsl_type *glsl_type_with_base(const struct glsl_type *type, unsigned base);
int glsl_type_equal(const struct glsl_type *left, const struct glsl_type *right);
int glsl_type_numeric(const struct glsl_type *type);
int glsl_type_contains_sampler(const struct glsl_type *type);
unsigned glsl_type_sampler_units(const struct glsl_type *type);
unsigned glsl_type_scalars(const struct glsl_type *type);
unsigned glsl_type_locations(const struct glsl_type *type);
void glsl_type_name(const struct glsl_type *type, char *out, size_t size);
unsigned glsl_std140_alignment(const struct glsl_type *type);
unsigned glsl_std140_size(const struct glsl_type *type);
unsigned glsl_std140_stride(const struct glsl_type *type);
unsigned glsl_std140_member_size(const struct glsl_type *type, unsigned row_major);
unsigned glsl_std140_member_stride(const struct glsl_type *type, unsigned row_major);
unsigned glsl_std140_matrix_stride(const struct glsl_type *type, unsigned row_major);
unsigned glsl_std140_member_alignment(const struct glsl_type *type, unsigned row_major);
unsigned glsl_std430_alignment(const struct glsl_type *type);
unsigned glsl_std430_size(const struct glsl_type *type);
unsigned glsl_std430_stride(const struct glsl_type *type);
unsigned glsl_std430_column_stride(const struct glsl_type *type);

/* builtins.c: built-in functions, variables and constants. */
const struct glsl_builtin *glsl_builtin_first(const char *name);
const struct glsl_builtin *glsl_builtin_next(const struct glsl_builtin *builtin);
int glsl_builtin_match(const struct glsl_shader *shader, const struct glsl_builtin *builtin, const struct glsl_type **arguments, unsigned count, int convert, const struct glsl_type **parameters, const struct glsl_type **result);
int glsl_builtin_writes(const struct glsl_builtin *builtin, unsigned index);
void glsl_builtin_variables(struct glsl_shader *shader);

/* check.c: the checker. */
void glsl_check(struct glsl_shader *shader);
struct glsl_symbol *glsl_declare(struct glsl_shader *shader, const char *name, unsigned kind, unsigned line);
void glsl_add_global(struct glsl_shader *shader, struct glsl_symbol *symbol);

/* fold.c: constant expressions. */
struct glsl_constant *glsl_fold(struct glsl_shader *shader, struct glsl_node *node);
struct glsl_constant *glsl_constant_new(struct glsl_arena *arena, const struct glsl_type *type);
struct glsl_constant *glsl_constant_convert(struct glsl_arena *arena, const struct glsl_constant *value, const struct glsl_type *type);

#endif
