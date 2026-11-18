/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GLSL compiler's preprocessor: the directives (#version, #define,
 * #undef, the conditionals, #error, #pragma, #extension, #line) and the
 * expansion of macros, turning the lexer's tokens into the tokens the
 * parser reads.
 *
 * Macros expand the usual way: a token taken from the expansion of a
 * macro is never expanded as that macro again (it is marked while the
 * macro's expansion is being read), the arguments of a function-like
 * macro are expanded on their own before they replace the parameters,
 * and ## pastes two tokens into one.  The lines of text between two
 * directives are expanded together, so the arguments of a macro may
 * span lines.
 */

#include "internal.h"

#include <stdio.h>
#include <string.h>

/* The deepest nesting of #if groups, and of macro expansions being read. */
#define PP_MAX_CONDITIONALS	64U
#define PP_MAX_FRAMES		128U

/* The most parameters a function-like macro may have. */
#define PP_MAX_PARAMETERS	32U

/* The macros whose expansion is computed when they are used. */
#define PP_SPECIAL_NONE		0U
#define PP_SPECIAL_LINE		1U
#define PP_SPECIAL_FILE		2U
#define PP_SPECIAL_VERSION	3U

/* The directives. */
#define PP_DIRECTIVE_UNKNOWN	0U
#define PP_DIRECTIVE_VERSION	1U
#define PP_DIRECTIVE_IF		2U
#define PP_DIRECTIVE_IFDEF	3U
#define PP_DIRECTIVE_IFNDEF	4U
#define PP_DIRECTIVE_ELIF	5U
#define PP_DIRECTIVE_ELSE	6U
#define PP_DIRECTIVE_ENDIF	7U
#define PP_DIRECTIVE_DEFINE	8U
#define PP_DIRECTIVE_UNDEF	9U
#define PP_DIRECTIVE_EXTENSION	10U
#define PP_DIRECTIVE_PRAGMA	11U
#define PP_DIRECTIVE_LINE	12U
#define PP_DIRECTIVE_ERROR	13U

/*
 * A directive's name and number.
 */
struct pp_directive_name {
	const char *name;
	unsigned directive;
};

/*
 * The directives by name, for pp_directive_number.
 */
static const struct pp_directive_name pp_directive_names[] = {
	{ "version", PP_DIRECTIVE_VERSION },
	{ "if", PP_DIRECTIVE_IF },
	{ "ifdef", PP_DIRECTIVE_IFDEF },
	{ "ifndef", PP_DIRECTIVE_IFNDEF },
	{ "elif", PP_DIRECTIVE_ELIF },
	{ "else", PP_DIRECTIVE_ELSE },
	{ "endif", PP_DIRECTIVE_ENDIF },
	{ "define", PP_DIRECTIVE_DEFINE },
	{ "undef", PP_DIRECTIVE_UNDEF },
	{ "extension", PP_DIRECTIVE_EXTENSION },
	{ "pragma", PP_DIRECTIVE_PRAGMA },
	{ "line", PP_DIRECTIVE_LINE },
	{ "error", PP_DIRECTIVE_ERROR }
};

/*
 * A growing list of tokens (in the arena; a grown list leaves the old
 * array there unused).
 */
struct pp_list {
	struct glsl_token *tokens;
	unsigned count;
	unsigned capacity;
};

/*
 * A macro: its name, its parameters when it is function-like, and the
 * tokens it expands to.
 *
 * `active` is set while its expansion is being read, which is what keeps
 * a macro from expanding inside itself.
 */
struct pp_macro {
	const char *name;
	unsigned length;
	unsigned function_like;
	struct glsl_token *parameters;
	unsigned parameter_count;
	struct glsl_token *body;
	unsigned body_count;
	unsigned special;
	unsigned active;
	struct pp_macro *next;
};

/*
 * One #if group: whether its lines are being kept, whether one of its
 * branches was taken already, whether #else was seen, and whether the
 * group around it is being kept.
 */
struct pp_conditional {
	unsigned keeping;
	unsigned taken;
	unsigned seen_else;
	unsigned outer_keeping;
	unsigned line;
};

/*
 * A list of tokens being read by an expansion, and the macro whose
 * expansion it is (NULL for the text the expansion started from).
 */
struct pp_frame {
	const struct glsl_token *tokens;
	unsigned count;
	unsigned position;
	struct pp_macro *macro;
};

/*
 * The tokens an expansion reads from: a stack of frames, the newest a
 * macro's expansion that is being read.
 */
struct pp_stream {
	struct pp_frame frames[PP_MAX_FRAMES];
	unsigned count;
};

/*
 * The preprocessor's state for one source.
 */
struct pp_state {
	struct glsl_shader *shader;

	/* The defined macros. */
	struct pp_macro *macros;

	/* The open #if groups. */
	struct pp_conditional conditionals[PP_MAX_CONDITIONALS];
	unsigned depth;

	/* The lines of text waiting to be expanded, and the expanded tokens so far. */
	struct pp_list pending;
	struct pp_list output;

	/* What #line added to the source's line numbers. */
	int line_delta;

	/* Whether anything but #version was seen (the version's macros exist then), and whether #version was. */
	unsigned started;
	unsigned version_seen;

	/* Whether #version asked for the compatibility profile. */
	unsigned compatibility;
};

static void pp_line_of_source(struct pp_state *state, struct glsl_token *tokens, unsigned first, unsigned last);
static void pp_start(struct pp_state *state);
static void pp_define_special(struct pp_state *state, const char *name, unsigned special);
static void pp_define_text(struct pp_state *state, const char *name, const char *value);
static unsigned pp_directive_number(const struct glsl_token *name);
static void pp_directive(struct pp_state *state, const struct glsl_token *line, unsigned count);
static void pp_version(struct pp_state *state, const struct glsl_token *line, unsigned count, unsigned directive_line);
static void pp_define(struct pp_state *state, const struct glsl_token *line, unsigned count, unsigned directive_line);
static int pp_parameters(struct pp_state *state, struct pp_macro *macro, const struct glsl_token *line, unsigned count, unsigned *index);
static void pp_undef(struct pp_state *state, const struct glsl_token *line, unsigned count, unsigned directive_line);
static void pp_if(struct pp_state *state, const struct glsl_token *line, unsigned count, unsigned directive, unsigned directive_line);
static void pp_else(struct pp_state *state, const struct glsl_token *line, unsigned count, unsigned directive, unsigned directive_line);
static void pp_extension(struct pp_state *state, const struct glsl_token *line, unsigned count, unsigned directive_line);
static void pp_line(struct pp_state *state, const struct glsl_token *line, unsigned count, unsigned directive_line);
static void pp_error_directive(struct pp_state *state, const struct glsl_token *line, unsigned count, unsigned directive_line);
static int pp_evaluate(struct pp_state *state, const struct glsl_token *line, unsigned count, unsigned directive_line, int64_t *value);
static int pp_defined(struct pp_state *state, const struct glsl_token *line, unsigned count, unsigned *index, unsigned directive_line, struct pp_list *out);
static int pp_expression(struct pp_state *state, const struct glsl_token *tokens, unsigned count, unsigned *at, unsigned precedence, int64_t *value);
static int pp_primary(struct pp_state *state, const struct glsl_token *tokens, unsigned count, unsigned *at, int64_t *value);
static unsigned pp_binary_precedence(const struct glsl_token *token);
static int pp_apply(struct pp_state *state, unsigned punct, int64_t left, int64_t right, unsigned line, int64_t *value);
static void pp_flush(struct pp_state *state);
static void pp_expand(struct pp_state *state, const struct glsl_token *tokens, unsigned count, struct pp_list *out);
static void pp_expand_macro(struct pp_state *state, struct pp_stream *stream, struct pp_macro *macro, const struct glsl_token *name, struct pp_list *out);
static const struct glsl_token *pp_next(struct pp_stream *stream);
static const struct glsl_token *pp_peek(struct pp_stream *stream);
static void pp_push(struct pp_state *state, struct pp_stream *stream, const struct glsl_token *tokens, unsigned count, struct pp_macro *macro, unsigned line);
static int pp_collect(struct pp_state *state, struct pp_stream *stream, struct pp_macro *macro, unsigned line, struct pp_list *arguments);
static void pp_substitute(struct pp_state *state, struct pp_macro *macro, struct pp_list *arguments, unsigned line, struct pp_list *out);
static void pp_paste(struct pp_state *state, struct pp_list *out, const struct glsl_token *right, unsigned line);
static int pp_parameter(const struct pp_macro *macro, const struct glsl_token *token);
static void pp_special(struct pp_state *state, const struct pp_macro *macro, unsigned line, struct pp_list *out);
static struct pp_macro *pp_find(struct pp_state *state, const struct glsl_token *token);
static void pp_list_add(struct pp_state *state, struct pp_list *list, const struct glsl_token *token);
static int pp_same_tokens(const struct glsl_token *left, unsigned left_count, const struct glsl_token *right, unsigned right_count);
static int pp_reserved_name(const struct glsl_token *name);
static int pp_keeping(const struct pp_state *state);

/*
 * Preprocesses a shader's source into shader->tokens (ending with an EOF
 * token), and sets the shader's version from #version or the default.
 */
void
glsl_preprocess(
	struct glsl_shader *shader,
	const char *source,
	unsigned default_version)
{
	struct pp_state *state;
	struct glsl_token *tokens;
	struct glsl_token end;
	unsigned count;
	unsigned first;
	unsigned last;

	/* The state, with the default version until #version says otherwise. */
	state = glsl_alloc(&shader->arena, sizeof(*state));
	state->shader = shader;
	shader->version = default_version;
	shader->es = 0U;
	if (default_version == GLSL_VERSION_ES100)
		shader->es = 1U;

	/* The raw tokens of the whole source. */
	count = glsl_lex(shader, source, strlen(source), 1U, &tokens);

	/* Each line in turn, up to its end-of-line token. */
	first = 0U;
	while (first < count) {
		last = first;
		while (last < count && tokens[last].kind != GLSL_TOKEN_NEWLINE)
			last++;

		/* The line, then the next one after its end-of-line token. */
		pp_line_of_source(state, tokens, first, last);
		first = last + 1U;
	}

	/* The last text, and groups left open. */
	pp_start(state);
	pp_flush(state);
	if (state->depth != 0U)
		glsl_error(shader, state->conditionals[state->depth - 1U].line, "#if without #endif");

	/* Ends the output with an EOF token. */
	memset(&end, 0, sizeof(end));
	end.kind = GLSL_TOKEN_EOF;
	end.text = "";
	end.line = (unsigned)((int)tokens[count].line + state->line_delta);
	pp_list_add(state, &state->output, &end);

	/* Succeeded: the parser reads the output. */
	shader->tokens = state->output.tokens;
	shader->token_count = state->output.count - 1U;
}

/* Handles one line of the source: a directive, or text kept for expansion when its group is kept. */
static void
pp_line_of_source(
	struct pp_state *state,
	struct glsl_token *tokens,
	unsigned first,
	unsigned last)
{
	unsigned index;
	int keeping;

	/* An empty line. */
	if (last == first)
		return;

	/* A line starting with # is a directive; the text before it is expanded first. */
	if (tokens[first].kind == GLSL_TOKEN_PUNCT && tokens[first].punct == GLSL_P_HASH) {
		pp_flush(state);
		pp_directive(state, tokens + first + 1U, last - first - 1U);
		return;
	}

	/* Text: the version's macros exist from here on. */
	pp_start(state);

	/* A kept line waits for expansion, renumbered by #line. */
	keeping = pp_keeping(state);
	if (!keeping)
		return;
	for (index = first; index < last; index++) {
		tokens[index].line = (unsigned)((int)tokens[index].line + state->line_delta);
		pp_list_add(state, &state->pending, &tokens[index]);
	}
}

/* Defines the version's predefined macros, once, before the first text or directive other than #version. */
static void
pp_start(
	struct pp_state *state)
{
	/* Only once. */
	if (state->started)
		return;
	state->started = 1U;

	/* __LINE__, __FILE__ and __VERSION__, whose values are made when they are used. */
	pp_define_special(state, "__LINE__", PP_SPECIAL_LINE);
	pp_define_special(state, "__FILE__", PP_SPECIAL_FILE);
	pp_define_special(state, "__VERSION__", PP_SPECIAL_VERSION);

	/* OpenGL ES's macros: GL_ES, high precision in fragment shaders, and (1.00) the derivatives extension. */
	if (state->shader->es) {
		pp_define_text(state, "GL_ES", "1");
		pp_define_text(state, "GL_FRAGMENT_PRECISION_HIGH", "1");
		if (state->shader->version < GLSL_VERSION_ES300)
			pp_define_text(state, "GL_OES_standard_derivatives", "1");
	}

	/* The desktop profiles of 1.50 on (a shader that names none is core), and the doubles and GL_ARB_gpu_shader5 on a device that has them. */
	if (!state->shader->es && state->shader->version >= GLSL_VERSION_150) {
		pp_define_text(state, "GL_core_profile", "1");
		if (state->compatibility)
			pp_define_text(state, "GL_compatibility_profile", "1");
		pp_define_text(state, "GL_ARB_tessellation_shader", "1");
		pp_define_text(state, "GL_ARB_sample_shading", "1");
		if ((state->shader->features & GLSL_FEATURE_SHADER5) != 0U)
			pp_define_text(state, "GL_ARB_gpu_shader5", "1");
		if ((state->shader->features & GLSL_FEATURE_FP64) != 0U)
			pp_define_text(state, "GL_ARB_gpu_shader_fp64", "1");
		pp_define_text(state, "GL_ARB_shader_subroutine", "1");
	}

	/* The desktop extensions of 1.30 on. */
	if (!state->shader->es && state->shader->version >= GLSL_VERSION_130) {
		pp_define_text(state, "GL_ARB_texture_gather", "1");
		pp_define_text(state, "GL_ARB_texture_cube_map_array", "1");
		pp_define_text(state, "GL_ARB_texture_query_lod", "1");
	}
}

/* Defines a macro whose value is computed where it is used. */
static void
pp_define_special(
	struct pp_state *state,
	const char *name,
	unsigned special)
{
	struct pp_macro *macro;

	/* The macro, first in the list. */
	macro = glsl_alloc(&state->shader->arena, sizeof(*macro));
	macro->name = name;
	macro->length = (unsigned)strlen(name);
	macro->special = special;
	macro->next = state->macros;
	state->macros = macro;
}

/* Defines an object-like macro from text, as the compiler's own predefined macro. */
static void
pp_define_text(
	struct pp_state *state,
	const char *name,
	const char *value)
{
	struct pp_macro *macro;
	struct glsl_token *body;
	unsigned count;

	/* The value's tokens. */
	count = glsl_lex(state->shader, value, strlen(value), 0U, &body);

	/* The macro, first in the list. */
	macro = glsl_alloc(&state->shader->arena, sizeof(*macro));
	macro->name = name;
	macro->length = (unsigned)strlen(name);
	macro->body = body;
	macro->body_count = count;
	macro->next = state->macros;
	state->macros = macro;
}

/* Returns the PP_DIRECTIVE_* a directive's name token names. */
static unsigned
pp_directive_number(
	const struct glsl_token *name)
{
	size_t index;
	int same;

	/* Only identifiers name directives. */
	if (name->kind != GLSL_TOKEN_IDENTIFIER)
		return PP_DIRECTIVE_UNKNOWN;

	/* The directive of that spelling. */
	for (index = 0U; index < sizeof(pp_directive_names) / sizeof(pp_directive_names[0]); index++) {
		same = glsl_token_is(name, pp_directive_names[index].name);
		if (same)
			return pp_directive_names[index].directive;
	}

	/* No directive is spelled so. */
	return PP_DIRECTIVE_UNKNOWN;
}

/* Carries out one directive line (the tokens after the #). */
static void
pp_directive(
	struct pp_state *state,
	const struct glsl_token *line,
	unsigned count)
{
	unsigned directive;
	unsigned directive_line;
	int keeping;

	/* The null directive: a # alone. */
	if (count == 0U)
		return;

	/* The directive and the line it is on (as #line numbers it). */
	directive = pp_directive_number(&line[0]);
	directive_line = (unsigned)((int)line[0].line + state->line_delta);

	/* #version comes before everything else, so it is handled before the version's macros exist. */
	if (directive == PP_DIRECTIVE_VERSION) {
		pp_version(state, line + 1, count - 1U, directive_line);
		return;
	}

	/* Any other directive starts the text. */
	pp_start(state);

	/* The conditionals are carried out even inside a group being skipped. */
	switch (directive) {
	case PP_DIRECTIVE_IF:
	case PP_DIRECTIVE_IFDEF:
	case PP_DIRECTIVE_IFNDEF:
		pp_if(state, line + 1, count - 1U, directive, directive_line);
		return;
	case PP_DIRECTIVE_ELIF:
	case PP_DIRECTIVE_ELSE:
		pp_else(state, line + 1, count - 1U, directive, directive_line);
		return;
	case PP_DIRECTIVE_ENDIF:
		if (state->depth == 0U) {
			glsl_error(state->shader, directive_line, "#endif without #if");
			return;
		}

		/* The innermost group ends. */
		state->depth--;
		return;
	default:
		break;
	}

	/* Every other directive is ignored in a group being skipped. */
	keeping = pp_keeping(state);
	if (!keeping)
		return;

	/* The directives of a kept group (pragmas change nothing this compiler does). */
	switch (directive) {
	case PP_DIRECTIVE_DEFINE:
		pp_define(state, line + 1, count - 1U, directive_line);
		break;
	case PP_DIRECTIVE_UNDEF:
		pp_undef(state, line + 1, count - 1U, directive_line);
		break;
	case PP_DIRECTIVE_EXTENSION:
		pp_extension(state, line + 1, count - 1U, directive_line);
		break;
	case PP_DIRECTIVE_PRAGMA:
		break;
	case PP_DIRECTIVE_LINE:
		pp_line(state, line + 1, count - 1U, directive_line);
		break;
	case PP_DIRECTIVE_ERROR:
		pp_error_directive(state, line + 1, count - 1U, directive_line);
		break;
	default:
		glsl_error(state->shader, directive_line, "unknown directive '#%.*s'", (int)line[0].length, line[0].text);
		break;
	}
}

/* Carries out #version: the version and the profile, which must come before anything else. */
static void
pp_version(
	struct pp_state *state,
	const struct glsl_token *line,
	unsigned count,
	unsigned directive_line)
{
	struct glsl_shader *shader;
	unsigned version;
	unsigned es;
	int is_es;
	int is_core;
	int is_compatibility;

	/* #version must come first, and once. */
	shader = state->shader;
	if (state->started || state->version_seen) {
		glsl_error(shader, directive_line, "#version must come before anything else");
		return;
	}

	/* A number. */
	state->version_seen = 1U;
	if (count == 0U || line[0].kind != GLSL_TOKEN_INT) {
		glsl_error(shader, directive_line, "#version needs a number");
		return;
	}

	/* The version, then an optional profile. */
	version = line[0].integer;
	es = 0U;
	if (count > 1U) {
		is_es = glsl_token_is(&line[1], "es");
		is_core = glsl_token_is(&line[1], "core");
		is_compatibility = glsl_token_is(&line[1], "compatibility");
		if (is_es) {
			es = 1U;
		} else if (is_compatibility) {
			state->compatibility = 1U;
		} else if (!is_core) {
			glsl_error(shader, directive_line, "unknown profile '%.*s'", (int)line[1].length, line[1].text);
		}
	}

	/* OpenGL ES's languages: 1.00 (no profile), 3.00 es and 3.10 es (ws101-p008). */
	if ((version == GLSL_VERSION_ES100 && es == 0U) ||
	    (version == GLSL_VERSION_ES300 && es != 0U) ||
	    (version == GLSL_VERSION_ES310 && es != 0U)) {
		shader->version = version;
		shader->es = 1U;
		return;
	}

	/* Desktop GLSL 1.10 to 3.30. */
	if (es == 0U) {
		switch (version) {
		case GLSL_VERSION_110:
		case GLSL_VERSION_120:
		case GLSL_VERSION_130:
		case GLSL_VERSION_140:
		case GLSL_VERSION_150:
		case GLSL_VERSION_330:
		case GLSL_VERSION_400:
		case GLSL_VERSION_410:
		case GLSL_VERSION_420:
		case GLSL_VERSION_430:
		case GLSL_VERSION_440:
		case GLSL_VERSION_450:
		case GLSL_VERSION_460:
			shader->version = version;
			shader->es = 0U;
			return;
		default:
			break;
		}
	}

	/* Any other version is not taken (yet). */
	glsl_error(shader, directive_line, "GLSL version %u is not supported (100, 300 es, 310 es, and 110 to 460 are)", version);
}

/* Carries out #define: an object-like or function-like macro. */
static void
pp_define(
	struct pp_state *state,
	const struct glsl_token *line,
	unsigned count,
	unsigned directive_line)
{
	struct glsl_shader *shader;
	struct pp_macro *macro;
	struct pp_macro *existing;
	unsigned body_start;
	int reserved;
	int status;
	int same;

	/* A name. */
	shader = state->shader;
	if (count == 0U || line[0].kind != GLSL_TOKEN_IDENTIFIER) {
		glsl_error(shader, directive_line, "#define needs a name");
		return;
	}

	/* Names starting with GL_ are the implementation's. */
	reserved = pp_reserved_name(&line[0]);
	if (reserved) {
		glsl_error(shader, directive_line, "macro names starting with GL_ are reserved");
		return;
	}

	/* The macro and its name. */
	macro = glsl_alloc(&shader->arena, sizeof(*macro));
	macro->name = line[0].text;
	macro->length = line[0].length;
	body_start = 1U;

	/* A ( right after the name (no space) makes it function-like, with parameters up to the ). */
	if (count > 1U && line[1].kind == GLSL_TOKEN_PUNCT && line[1].punct == GLSL_P_LPAREN && !line[1].space) {
		body_start = 2U;
		status = pp_parameters(state, macro, line, count, &body_start);
		if (status != 0)
			return;
	}

	/* The body: the rest of the line. */
	macro->body = (struct glsl_token *)(line + body_start);
	macro->body_count = count - body_start;

	/* ## may not start or end the body. */
	if (macro->body_count > 0U) {
		if ((macro->body[0].kind == GLSL_TOKEN_PUNCT && macro->body[0].punct == GLSL_P_HASH_HASH) ||
		    (macro->body[macro->body_count - 1U].kind == GLSL_TOKEN_PUNCT &&
		     macro->body[macro->body_count - 1U].punct == GLSL_P_HASH_HASH)) {
			glsl_error(shader, directive_line, "'##' cannot be at either end of a macro");
			return;
		}
	}

	/* A new name becomes a new macro. */
	existing = pp_find(state, &line[0]);
	if (existing == NULL) {
		macro->next = state->macros;
		state->macros = macro;
		return;
	}

	/* A macro defined again must be defined the same way. */
	same = 1;
	if (existing->special != PP_SPECIAL_NONE || existing->function_like != macro->function_like)
		same = 0;
	if (same && existing->parameter_count != macro->parameter_count)
		same = 0;
	if (same && existing->parameter_count != 0U)
		same = pp_same_tokens(existing->parameters, existing->parameter_count, macro->parameters, macro->parameter_count);
	if (same)
		same = pp_same_tokens(existing->body, existing->body_count, macro->body, macro->body_count);
	if (!same)
		glsl_error(shader, directive_line, "macro '%.*s' redefined differently", (int)macro->length, macro->name);
}

/* Reads the parameter names of a function-like macro from line[*index] (after its "(") to the ")". */
static int
pp_parameters(
	struct pp_state *state,
	struct pp_macro *macro,
	const struct glsl_token *line,
	unsigned count,
	unsigned *index)
{
	struct glsl_shader *shader;
	const struct glsl_token *token;
	unsigned other;
	int same;

	/* Room for the parameters. */
	shader = state->shader;
	macro->function_like = 1U;
	macro->parameters = glsl_alloc(&shader->arena, PP_MAX_PARAMETERS * sizeof(*macro->parameters));

	/* No parameters at all: "()". */
	if (*index < count && line[*index].kind == GLSL_TOKEN_PUNCT && line[*index].punct == GLSL_P_RPAREN) {
		(*index)++;
		return 0;
	}

	/* Names separated by commas. */
	for (;;) {
		/* A name, and room for it. */
		if (*index >= count || line[*index].kind != GLSL_TOKEN_IDENTIFIER)
			break;
		if (macro->parameter_count == PP_MAX_PARAMETERS)
			break;
		token = &line[*index];

		/* The name, unless it repeats an earlier one. */
		for (other = 0U; other < macro->parameter_count; other++) {
			same = pp_same_tokens(&macro->parameters[other], 1U, token, 1U);
			if (same)
				glsl_error(shader, token->line, "macro parameter '%.*s' repeated", (int)token->length, token->text);
		}

		/* The parameter takes the next place. */
		macro->parameters[macro->parameter_count] = *token;
		macro->parameter_count++;
		(*index)++;

		/* A comma goes on to the next name. */
		if (*index < count && line[*index].kind == GLSL_TOKEN_PUNCT && line[*index].punct == GLSL_P_COMMA) {
			(*index)++;
			continue;
		}

		/* A ) ends them. */
		if (*index < count && line[*index].kind == GLSL_TOKEN_PUNCT && line[*index].punct == GLSL_P_RPAREN) {
			(*index)++;
			return 0;
		}

		/* Anything else is malformed. */
		break;
	}

	/* The parameters are malformed. */
	glsl_error(shader, line[0].line, "malformed parameters of macro '%.*s'", (int)macro->length, macro->name);
	return -1;
}

/* Carries out #undef. */
static void
pp_undef(
	struct pp_state *state,
	const struct glsl_token *line,
	unsigned count,
	unsigned directive_line)
{
	struct pp_macro **link;
	struct pp_macro *macro;
	int reserved;
	int same;

	/* A name. */
	if (count == 0U || line[0].kind != GLSL_TOKEN_IDENTIFIER) {
		glsl_error(state->shader, directive_line, "#undef needs a name");
		return;
	}

	/* The implementation's names cannot go. */
	reserved = pp_reserved_name(&line[0]);
	if (reserved) {
		glsl_error(state->shader, directive_line, "macro names starting with GL_ are reserved");
		return;
	}

	/* Unlinks the macro of the name, if there is one. */
	for (link = &state->macros; *link != NULL; link = &(*link)->next) {
		macro = *link;
		if (macro->length != line[0].length)
			continue;
		same = memcmp(macro->name, line[0].text, macro->length);
		if (same != 0)
			continue;

		/* The predefined macros stay. */
		if (macro->special != PP_SPECIAL_NONE) {
			glsl_error(state->shader, directive_line, "cannot undefine '%.*s'", (int)macro->length, macro->name);
			return;
		}

		/* The macro goes. */
		*link = macro->next;
		return;
	}
}

/* Opens an #if, #ifdef or #ifndef group. */
static void
pp_if(
	struct pp_state *state,
	const struct glsl_token *line,
	unsigned count,
	unsigned directive,
	unsigned directive_line)
{
	struct pp_conditional *group;
	struct pp_macro *macro;
	int64_t value;
	int outer;
	int status;

	/* Room for one more group. */
	if (state->depth == PP_MAX_CONDITIONALS)
		glsl_fatal(state->shader, directive_line, "#if groups nested too deeply");

	/* The group, inside a kept group or not. */
	outer = pp_keeping(state);
	group = &state->conditionals[state->depth];
	state->depth++;
	memset(group, 0, sizeof(*group));
	group->outer_keeping = (unsigned)outer;
	group->line = directive_line;

	/* Inside a skipped group only the nesting counts. */
	if (!outer)
		return;

	/* #if evaluates its expression (an error counts as false). */
	value = 0;
	if (directive == PP_DIRECTIVE_IF) {
		status = pp_evaluate(state, line, count, directive_line, &value);
		if (status != 0)
			value = 0;
	} else if (count == 0U || line[0].kind != GLSL_TOKEN_IDENTIFIER) {
		/* #ifdef and #ifndef need a name. */
		glsl_error(state->shader, directive_line, "#ifdef and #ifndef need a name");
	} else {
		/* #ifdef holds for a defined name, #ifndef for an undefined one. */
		macro = pp_find(state, &line[0]);
		if (macro != NULL)
			value = 1;
		if (directive == PP_DIRECTIVE_IFNDEF)
			value = !value;
	}

	/* The group is kept when the test holds. */
	if (value != 0) {
		group->keeping = 1U;
		group->taken = 1U;
	}
}

/* Carries out #elif or #else. */
static void
pp_else(
	struct pp_state *state,
	const struct glsl_token *line,
	unsigned count,
	unsigned directive,
	unsigned directive_line)
{
	struct pp_conditional *group;
	int64_t value;
	int status;

	/* An open group. */
	if (state->depth == 0U) {
		glsl_error(state->shader, directive_line, "#elif or #else without #if");
		return;
	}

	/* One without #else yet. */
	group = &state->conditionals[state->depth - 1U];
	if (group->seen_else) {
		glsl_error(state->shader, directive_line, "#elif or #else after #else");
		return;
	}

	/* #else ends the choice: its lines are kept when no branch was taken. */
	group->keeping = 0U;
	if (directive == PP_DIRECTIVE_ELSE) {
		group->seen_else = 1U;
		if (group->outer_keeping && !group->taken) {
			group->keeping = 1U;
			group->taken = 1U;
		}

		/* The choice is made. */
		return;
	}

	/* #elif is only evaluated when no branch was taken yet. */
	if (!group->outer_keeping || group->taken)
		return;
	status = pp_evaluate(state, line, count, directive_line, &value);
	if (status != 0)
		value = 0;

	/* Its lines are kept when it holds. */
	if (value != 0) {
		group->keeping = 1U;
		group->taken = 1U;
	}
}

/* Carries out #extension NAME : BEHAVIOR. */
static void
pp_extension(
	struct pp_state *state,
	const struct glsl_token *line,
	unsigned count,
	unsigned directive_line)
{
	struct glsl_shader *shader;
	const struct glsl_token *name;
	const struct glsl_token *behavior;
	int required;
	int enabled;
	int warned;
	int disabled;
	int all;
	int derivatives;
	int tessellation;
	int sample_shading;
	int texture_gather;
	int cube_map_array;
	int fp64;
	int gpu_shader5;
	int query_lod;
	int subroutine;

	/* The form: a name, a colon, a behavior. */
	shader = state->shader;
	if (count != 3U ||
	    line[0].kind != GLSL_TOKEN_IDENTIFIER ||
	    line[1].kind != GLSL_TOKEN_PUNCT ||
	    line[1].punct != GLSL_P_COLON ||
	    line[2].kind != GLSL_TOKEN_IDENTIFIER) {
		glsl_error(shader, directive_line, "malformed #extension");
		return;
	}

	/* The behavior: require, enable, warn or disable. */
	name = &line[0];
	behavior = &line[2];
	required = glsl_token_is(behavior, "require");
	enabled = glsl_token_is(behavior, "enable");
	warned = glsl_token_is(behavior, "warn");
	disabled = glsl_token_is(behavior, "disable");
	if (!required && !enabled && !warned && !disabled) {
		glsl_error(shader, directive_line, "unknown extension behavior '%.*s'", (int)behavior->length, behavior->text);
		return;
	}

	/* "all" can only warn or disable; require also enables. */
	if (required)
		enabled = 1;
	all = glsl_token_is(name, "all");
	if (all) {
		if (enabled)
			glsl_error(shader, directive_line, "extension 'all' cannot be required or enabled");
		return;
	}

	/* OES_standard_derivatives: dFdx, dFdy and fwidth in OpenGL ES fragment shaders. */
	derivatives = glsl_token_is(name, "GL_OES_standard_derivatives");
	if (derivatives && shader->es) {
		shader->derivatives = (unsigned)enabled;
		return;
	}

	/* GL_ARB_tessellation_shader: the tessellation stages in desktop GLSL 1.50 and 3.30 (4.00 on has them). */
	tessellation = glsl_token_is(name, "GL_ARB_tessellation_shader");
	if (tessellation && !shader->es && shader->version >= GLSL_VERSION_150) {
		if (shader->version < GLSL_VERSION_400)
			shader->tessellation = (unsigned)enabled;
		return;
	}

	/* GL_ARB_sample_shading: the per-sample built-ins in desktop GLSL 1.50 and 3.30 (4.00 on has them). */
	sample_shading = glsl_token_is(name, "GL_ARB_sample_shading");
	if (sample_shading && !shader->es && shader->version >= GLSL_VERSION_150) {
		if (shader->version < GLSL_VERSION_400)
			shader->sample_shading = (unsigned)enabled;
		return;
	}

	/* GL_ARB_texture_gather: textureGather and textureGatherOffset in desktop GLSL 1.30 to 3.30 (4.00 on has them). */
	texture_gather = glsl_token_is(name, "GL_ARB_texture_gather");
	if (texture_gather && !shader->es && shader->version >= GLSL_VERSION_130) {
		if (shader->version < GLSL_VERSION_400)
			shader->texture_gather = (unsigned)enabled;
		return;
	}

	/* GL_ARB_texture_cube_map_array: the cube map array samplers in desktop GLSL 1.30 to 3.30 (4.00 on has them). */
	cube_map_array = glsl_token_is(name, "GL_ARB_texture_cube_map_array");
	if (cube_map_array && !shader->es && shader->version >= GLSL_VERSION_130) {
		if (shader->version < GLSL_VERSION_400)
			shader->cube_map_array = (unsigned)enabled;
		return;
	}

	/*
	 * GL_ARB_gpu_shader_fp64: the double types and functions in desktop
	 * GLSL 1.50 and 3.30 (4.00 on has them), on a device that has them.
	 */
	fp64 = glsl_token_is(name, "GL_ARB_gpu_shader_fp64");
	if (fp64 && !shader->es && shader->version >= GLSL_VERSION_150 && (shader->features & GLSL_FEATURE_FP64) != 0U) {
		if (shader->version < GLSL_VERSION_400)
			shader->fp64 = (unsigned)enabled;
		return;
	}

	/*
	 * GL_ARB_gpu_shader5: its built-ins, gathers, interpolation
	 * functions, invocations, precise, sampler indices and the conversion
	 * of int to uint in desktop GLSL 1.50 and 3.30 (4.00 on has them), on
	 * a device that has what they need.
	 */
	gpu_shader5 = glsl_token_is(name, "GL_ARB_gpu_shader5");
	if (gpu_shader5 && !shader->es && shader->version >= GLSL_VERSION_150 && (shader->features & GLSL_FEATURE_SHADER5) != 0U) {
		if (shader->version < GLSL_VERSION_400)
			shader->gpu_shader5 = (unsigned)enabled;
		return;
	}

	/* GL_ARB_shader_subroutine: subroutines in desktop GLSL 1.50 and 3.30 (4.00 on has them). */
	subroutine = glsl_token_is(name, "GL_ARB_shader_subroutine");
	if (subroutine && !shader->es && shader->version >= GLSL_VERSION_150) {
		if (shader->version < GLSL_VERSION_400)
			shader->shader_subroutine = (unsigned)enabled;
		return;
	}

	/* GL_ARB_texture_query_lod: its spelling textureQueryLOD in desktop GLSL 1.30 on (4.00 on has textureQueryLod as well). */
	query_lod = glsl_token_is(name, "GL_ARB_texture_query_lod");
	if (query_lod && !shader->es && shader->version >= GLSL_VERSION_130) {
		shader->texture_query_lod = (unsigned)enabled;
		return;
	}

	/* Any other: required is an error, enabled a warning. */
	if (required) {
		glsl_error(shader, directive_line, "extension '%.*s' is not supported", (int)name->length, name->text);
	} else if (enabled) {
		glsl_warning(shader, directive_line, "extension '%.*s' is not supported", (int)name->length, name->text);
	}
}

/* Carries out #line LINE [SOURCE]: the next line gets the number LINE + 1. */
static void
pp_line(
	struct pp_state *state,
	const struct glsl_token *line,
	unsigned count,
	unsigned directive_line)
{
	struct pp_list expanded;
	int64_t value;
	unsigned at;
	int status;

	/* A line number. */
	if (count == 0U) {
		glsl_error(state->shader, directive_line, "#line needs a line number");
		return;
	}

	/* The operands after macro expansion. */
	memset(&expanded, 0, sizeof(expanded));
	pp_expand(state, line, count, &expanded);
	at = 0U;
	status = pp_expression(state, expanded.tokens, expanded.count, &at, 1U, &value);
	if (status != 0)
		return;

	/* The line numbers from the next line on. */
	state->line_delta += (int)value - (int)directive_line;
}

/* Carries out #error: its text becomes an error. */
static void
pp_error_directive(
	struct pp_state *state,
	const struct glsl_token *line,
	unsigned count,
	unsigned directive_line)
{
	const char *start;
	const char *end;

	/* An #error without a message. */
	if (count == 0U) {
		glsl_error(state->shader, directive_line, "#error");
		return;
	}

	/* The message is the line's text after #error. */
	start = line[0].text;
	end = line[count - 1U].text + line[count - 1U].length;
	glsl_error(state->shader, directive_line, "#error %.*s", (int)(end - start), start);
}

/*
 * Evaluates the expression of #if or #elif: defined is taken first, then
 * the macros are expanded and the integer expression computed.
 */
static int
pp_evaluate(
	struct pp_state *state,
	const struct glsl_token *line,
	unsigned count,
	unsigned directive_line,
	int64_t *value)
{
	struct pp_list replaced;
	struct pp_list expanded;
	unsigned index;
	unsigned at;
	int is_defined;
	int status;

	/* An expression at all. */
	if (count == 0U) {
		glsl_error(state->shader, directive_line, "#if needs an expression");
		return -1;
	}

	/* Each "defined NAME" and "defined ( NAME )" replaced by 1 or 0. */
	memset(&replaced, 0, sizeof(replaced));
	for (index = 0U; index < count; index++) {
		is_defined = 0;
		if (line[index].kind == GLSL_TOKEN_IDENTIFIER)
			is_defined = glsl_token_is(&line[index], "defined");
		if (!is_defined) {
			pp_list_add(state, &replaced, &line[index]);
			continue;
		}

		/* The operator and its name. */
		status = pp_defined(state, line, count, &index, directive_line, &replaced);
		if (status != 0)
			return -1;
	}

	/* The macros expanded. */
	memset(&expanded, 0, sizeof(expanded));
	pp_expand(state, replaced.tokens, replaced.count, &expanded);

	/* The expression. */
	at = 0U;
	status = pp_expression(state, expanded.tokens, expanded.count, &at, 1U, value);
	if (status != 0)
		return -1;

	/* It must use every token. */
	if (at != expanded.count) {
		glsl_error(state->shader, directive_line, "unexpected '%.*s' in #if", (int)expanded.tokens[at].length, expanded.tokens[at].text);
		return -1;
	}

	/* Succeeded: the value. */
	return 0;
}

/* Replaces "defined NAME" or "defined ( NAME )" at line[*index] by the number 1 or 0. */
static int
pp_defined(
	struct pp_state *state,
	const struct glsl_token *line,
	unsigned count,
	unsigned *index,
	unsigned directive_line,
	struct pp_list *out)
{
	const struct glsl_token *name;
	struct pp_macro *macro;
	struct glsl_token number;
	unsigned at;

	/* The name, with or without parentheses. */
	at = *index;
	name = NULL;
	if (at + 1U < count && line[at + 1U].kind == GLSL_TOKEN_IDENTIFIER) {
		name = &line[at + 1U];
		*index = at + 1U;
	} else if (at + 3U < count &&
		   line[at + 1U].kind == GLSL_TOKEN_PUNCT &&
		   line[at + 1U].punct == GLSL_P_LPAREN &&
		   line[at + 2U].kind == GLSL_TOKEN_IDENTIFIER &&
		   line[at + 3U].kind == GLSL_TOKEN_PUNCT &&
		   line[at + 3U].punct == GLSL_P_RPAREN) {
		name = &line[at + 2U];
		*index = at + 3U;
	}

	/* Without a name it is malformed. */
	if (name == NULL) {
		glsl_error(state->shader, directive_line, "'defined' needs a name");
		return -1;
	}

	/* The number that replaces it, which is never expanded. */
	macro = pp_find(state, name);
	memset(&number, 0, sizeof(number));
	number.kind = GLSL_TOKEN_INT;
	number.text = "0";
	number.length = 1U;
	number.line = directive_line;
	number.noexpand = 1U;
	if (macro != NULL) {
		number.integer = 1U;
		number.text = "1";
	}

	/* Succeeded: the number is in place. */
	pp_list_add(state, out, &number);
	return 0;
}

/*
 * Evaluates an integer expression of the preprocessor from tokens[*at]
 * with binary operators of at least a precedence (precedence climbing).
 */
static int
pp_expression(
	struct pp_state *state,
	const struct glsl_token *tokens,
	unsigned count,
	unsigned *at,
	unsigned precedence,
	int64_t *value)
{
	const struct glsl_token *token;
	unsigned token_precedence;
	int64_t right;
	int status;

	/* The left operand. */
	status = pp_primary(state, tokens, count, at, value);
	if (status != 0)
		return -1;

	/* Binary operators binding at least as tightly as asked. */
	while (*at < count) {
		token = &tokens[*at];
		token_precedence = pp_binary_precedence(token);
		if (token_precedence == 0U || token_precedence < precedence)
			break;
		(*at)++;

		/* The right operand binds the next level up (the operators are left associative). */
		status = pp_expression(state, tokens, count, at, token_precedence + 1U, &right);
		if (status != 0)
			return -1;

		/* The operation. */
		status = pp_apply(state, token->punct, *value, right, token->line, value);
		if (status != 0)
			return -1;
	}

	/* Succeeded: the value so far. */
	return 0;
}

/* Evaluates a unary expression, a number or a parenthesized expression of the preprocessor. */
static int
pp_primary(
	struct pp_state *state,
	const struct glsl_token *tokens,
	unsigned count,
	unsigned *at,
	int64_t *value)
{
	const struct glsl_token *token;
	unsigned line;
	int status;

	/* Something to read. */
	if (*at >= count) {
		line = 0U;
		if (count > 0U)
			line = tokens[count - 1U].line;
		glsl_error(state->shader, line, "#if expression ends early");
		return -1;
	}

	/* An integer. */
	token = &tokens[*at];
	(*at)++;
	if (token->kind == GLSL_TOKEN_INT || token->kind == GLSL_TOKEN_UINT) {
		*value = (int64_t)(int32_t)token->integer;
		return 0;
	}

	/* An identifier that is not a macro. */
	if (token->kind == GLSL_TOKEN_IDENTIFIER) {
		glsl_error(state->shader, token->line, "'%.*s' is not a defined macro", (int)token->length, token->text);
		return -1;
	}

	/* Anything else but an operator is out of place. */
	if (token->kind != GLSL_TOKEN_PUNCT) {
		glsl_error(state->shader, token->line, "unexpected '%.*s' in #if", (int)token->length, token->text);
		return -1;
	}

	/* A parenthesized expression. */
	if (token->punct == GLSL_P_LPAREN) {
		status = pp_expression(state, tokens, count, at, 1U, value);
		if (status != 0)
			return -1;
		if (*at >= count || tokens[*at].kind != GLSL_TOKEN_PUNCT || tokens[*at].punct != GLSL_P_RPAREN) {
			glsl_error(state->shader, token->line, "missing ')' in #if");
			return -1;
		}

		/* The ) is taken with it. */
		(*at)++;
		return 0;
	}

	/* Only the unary operators remain. */
	if (token->punct != GLSL_P_PLUS &&
	    token->punct != GLSL_P_MINUS &&
	    token->punct != GLSL_P_BANG &&
	    token->punct != GLSL_P_TILDE) {
		glsl_error(state->shader, token->line, "unexpected '%.*s' in #if", (int)token->length, token->text);
		return -1;
	}

	/* The operand. */
	status = pp_primary(state, tokens, count, at, value);
	if (status != 0)
		return -1;

	/* The operator on the operand (unary plus changes nothing). */
	if (token->punct == GLSL_P_MINUS) {
		*value = -*value;
	} else if (token->punct == GLSL_P_BANG) {
		*value = (*value == 0);
	} else if (token->punct == GLSL_P_TILDE) {
		*value = ~*value;
	}

	/* Succeeded: the value. */
	return 0;
}

/* Returns a binary operator's precedence in #if (higher binds tighter), 0 for anything else. */
static unsigned
pp_binary_precedence(
	const struct glsl_token *token)
{
	/* Only punctuators are operators. */
	if (token->kind != GLSL_TOKEN_PUNCT)
		return 0U;

	/* C's levels, from || up to the multiplicative operators. */
	switch (token->punct) {
	case GLSL_P_OR_OR:
		return 1U;
	case GLSL_P_AND_AND:
		return 2U;
	case GLSL_P_BAR:
		return 3U;
	case GLSL_P_CARET:
		return 4U;
	case GLSL_P_AMP:
		return 5U;
	case GLSL_P_EQ:
	case GLSL_P_NE:
		return 6U;
	case GLSL_P_LT:
	case GLSL_P_GT:
	case GLSL_P_LE:
	case GLSL_P_GE:
		return 7U;
	case GLSL_P_SHL:
	case GLSL_P_SHR:
		return 8U;
	case GLSL_P_PLUS:
	case GLSL_P_MINUS:
		return 9U;
	case GLSL_P_STAR:
	case GLSL_P_SLASH:
	case GLSL_P_PERCENT:
		return 10U;
	default:
		break;
	}

	/* Not a binary operator. */
	return 0U;
}

/* Applies a binary operator of #if to two values. */
static int
pp_apply(
	struct pp_state *state,
	unsigned punct,
	int64_t left,
	int64_t right,
	unsigned line,
	int64_t *value)
{
	/* The operators that cannot fail. */
	switch (punct) {
	case GLSL_P_OR_OR:
		*value = (left != 0 || right != 0);
		return 0;
	case GLSL_P_AND_AND:
		*value = (left != 0 && right != 0);
		return 0;
	case GLSL_P_BAR:
		*value = left | right;
		return 0;
	case GLSL_P_CARET:
		*value = left ^ right;
		return 0;
	case GLSL_P_AMP:
		*value = left & right;
		return 0;
	case GLSL_P_EQ:
		*value = (left == right);
		return 0;
	case GLSL_P_NE:
		*value = (left != right);
		return 0;
	case GLSL_P_LT:
		*value = (left < right);
		return 0;
	case GLSL_P_GT:
		*value = (left > right);
		return 0;
	case GLSL_P_LE:
		*value = (left <= right);
		return 0;
	case GLSL_P_GE:
		*value = (left >= right);
		return 0;
	case GLSL_P_SHL:
		*value = (int64_t)((uint64_t)left << (right & 63));
		return 0;
	case GLSL_P_SHR:
		*value = left >> (right & 63);
		return 0;
	case GLSL_P_PLUS:
		*value = left + right;
		return 0;
	case GLSL_P_MINUS:
		*value = left - right;
		return 0;
	case GLSL_P_STAR:
		*value = left * right;
		return 0;
	default:
		break;
	}

	/* Division and remainder refuse a zero divisor. */
	if (right == 0) {
		glsl_error(state->shader, line, "division by zero in #if");
		return -1;
	}

	/* The quotient or the remainder. */
	if (punct == GLSL_P_SLASH) {
		*value = left / right;
	} else {
		*value = left % right;
	}

	/* Succeeded: the value. */
	return 0;
}

/* Expands the text waiting since the last directive and appends it to the output. */
static void
pp_flush(
	struct pp_state *state)
{
	/* Nothing waiting. */
	if (state->pending.count == 0U)
		return;

	/* The text expanded onto the output, and the list emptied. */
	pp_expand(state, state->pending.tokens, state->pending.count, &state->output);
	state->pending.count = 0U;
}

/*
 * Expands the macros of a list of tokens onto another list.
 */
static void
pp_expand(
	struct pp_state *state,
	const struct glsl_token *tokens,
	unsigned count,
	struct pp_list *out)
{
	struct pp_stream stream;
	struct pp_macro *macro;
	const struct glsl_token *token;
	struct glsl_token copy;

	/* The stream starts with the tokens themselves. */
	stream.count = 0U;
	pp_push(state, &stream, tokens, count, NULL, 0U);

	/* Each token read: kept, or replaced by a macro's expansion that is read in turn. */
	for (;;) {
		token = pp_next(&stream);
		if (token == NULL)
			break;
		copy = *token;

		/* Anything but an identifier that may still expand is kept. */
		macro = NULL;
		if (copy.kind == GLSL_TOKEN_IDENTIFIER && !copy.noexpand)
			macro = pp_find(state, &copy);
		if (macro == NULL) {
			pp_list_add(state, out, &copy);
			continue;
		}

		/* A macro whose expansion is being read is not expanded again, now or later. */
		if (macro->active) {
			copy.noexpand = 1U;
			pp_list_add(state, out, &copy);
			continue;
		}

		/* The macro's expansion. */
		pp_expand_macro(state, &stream, macro, &copy, out);
	}
}

/* Expands one use of a macro: its value, its body, or its body with the arguments that follow in the stream. */
static void
pp_expand_macro(
	struct pp_state *state,
	struct pp_stream *stream,
	struct pp_macro *macro,
	const struct glsl_token *name,
	struct pp_list *out)
{
	struct pp_list arguments[PP_MAX_PARAMETERS];
	struct pp_list replacement;
	const struct glsl_token *next;
	int status;

	/* The macros computed on use. */
	if (macro->special != PP_SPECIAL_NONE) {
		pp_special(state, macro, name->line, out);
		return;
	}

	/* An object-like macro: its body is read next. */
	memset(&replacement, 0, sizeof(replacement));
	if (!macro->function_like) {
		pp_substitute(state, macro, NULL, name->line, &replacement);
		pp_push(state, stream, replacement.tokens, replacement.count, macro, name->line);
		return;
	}

	/* A function-like macro without ( after its name is only a name. */
	next = pp_peek(stream);
	if (next == NULL || next->kind != GLSL_TOKEN_PUNCT || next->punct != GLSL_P_LPAREN) {
		pp_list_add(state, out, name);
		return;
	}

	/* Its arguments. */
	memset(arguments, 0, sizeof(arguments));
	status = pp_collect(state, stream, macro, name->line, arguments);
	if (status != 0)
		return;

	/* Its body with them in place is read next. */
	pp_substitute(state, macro, arguments, name->line, &replacement);
	pp_push(state, stream, replacement.tokens, replacement.count, macro, name->line);
}

/* Takes the next token of a stream, leaving the expansions that are used up (and letting their macros expand again). */
static const struct glsl_token *
pp_next(
	struct pp_stream *stream)
{
	struct pp_frame *frame;

	/* The newest frame with a token left. */
	while (stream->count > 0U) {
		frame = &stream->frames[stream->count - 1U];
		if (frame->position < frame->count) {
			frame->position++;
			return &frame->tokens[frame->position - 1U];
		}

		/* A used-up expansion ends, and its macro may expand again. */
		if (frame->macro != NULL)
			frame->macro->active = 0U;
		stream->count--;
	}

	/* The stream is empty. */
	return NULL;
}

/* Looks at the next token of a stream without taking it. */
static const struct glsl_token *
pp_peek(
	struct pp_stream *stream)
{
	struct pp_frame *frame;
	unsigned index;

	/* The newest frame with a token left. */
	for (index = stream->count; index > 0U; index--) {
		frame = &stream->frames[index - 1U];
		if (frame->position < frame->count)
			return &frame->tokens[frame->position];
	}

	/* The stream is empty. */
	return NULL;
}

/* Pushes tokens to be read next: a macro's expansion (which marks the macro active) or the start of a stream. */
static void
pp_push(
	struct pp_state *state,
	struct pp_stream *stream,
	const struct glsl_token *tokens,
	unsigned count,
	struct pp_macro *macro,
	unsigned line)
{
	struct pp_frame *frame;

	/* Room for one more expansion. */
	if (stream->count == PP_MAX_FRAMES)
		glsl_fatal(state->shader, line, "macros nested too deeply");

	/* The frame. */
	frame = &stream->frames[stream->count];
	stream->count++;
	frame->tokens = tokens;
	frame->count = count;
	frame->position = 0U;
	frame->macro = macro;

	/* The macro is active while its expansion is read. */
	if (macro != NULL)
		macro->active = 1U;
}

/*
 * Reads the arguments of a function-like macro from its ( to its
 * matching ), split at the commas outside inner parentheses.
 */
static int
pp_collect(
	struct pp_state *state,
	struct pp_stream *stream,
	struct pp_macro *macro,
	unsigned line,
	struct pp_list *arguments)
{
	const struct glsl_token *token;
	unsigned count;
	unsigned depth;
	int is_punct;

	/* The ( itself. */
	(void)pp_next(stream);

	/* The tokens up to the matching ). */
	count = 1U;
	depth = 0U;
	for (;;) {
		token = pp_next(stream);
		if (token == NULL) {
			glsl_error(state->shader, line, "unterminated arguments of macro '%.*s'", (int)macro->length, macro->name);
			return -1;
		}

		/* The ) that ends them. */
		is_punct = (token->kind == GLSL_TOKEN_PUNCT);
		if (is_punct && depth == 0U && token->punct == GLSL_P_RPAREN)
			break;

		/* A comma outside parentheses starts the next argument. */
		if (is_punct && depth == 0U && token->punct == GLSL_P_COMMA) {
			if (count == PP_MAX_PARAMETERS) {
				glsl_error(state->shader, line, "too many arguments to macro '%.*s'", (int)macro->length, macro->name);
				return -1;
			}

			/* The next argument. */
			count++;
			continue;
		}

		/* Parentheses nest. */
		if (is_punct && token->punct == GLSL_P_LPAREN)
			depth++;
		if (is_punct && token->punct == GLSL_P_RPAREN)
			depth--;

		/* The token belongs to the current argument. */
		pp_list_add(state, &arguments[count - 1U], token);
	}

	/* A macro without parameters takes one empty argument. */
	if (macro->parameter_count == 0U && count == 1U && arguments[0].count == 0U)
		return 0;

	/* The count must match. */
	if (count != macro->parameter_count) {
		glsl_error(state->shader, line, "macro '%.*s' takes %u arguments, not %u", (int)macro->length, macro->name,
			   macro->parameter_count, count);
		return -1;
	}

	/* Succeeded: the arguments. */
	return 0;
}

/*
 * Builds a macro's replacement: its body with each parameter replaced by
 * its argument (expanded, unless next to ##), and ## pasting its operands.
 */
static void
pp_substitute(
	struct pp_state *state,
	struct pp_macro *macro,
	struct pp_list *arguments,
	unsigned line,
	struct pp_list *out)
{
	struct pp_list expanded;
	struct glsl_token copy;
	const struct glsl_token *body;
	unsigned index;
	unsigned element;
	int parameter;
	int pasting;
	int before_paste;

	/* Each body token in turn. */
	body = macro->body;
	pasting = 0;
	for (index = 0U; index < macro->body_count; index++) {
		/* ## joins the token before it with the one after it. */
		if (body[index].kind == GLSL_TOKEN_PUNCT && body[index].punct == GLSL_P_HASH_HASH) {
			pasting = 1;
			continue;
		}

		/* Whether ## follows. */
		before_paste = 0;
		if (index + 1U < macro->body_count &&
		    body[index + 1U].kind == GLSL_TOKEN_PUNCT &&
		    body[index + 1U].punct == GLSL_P_HASH_HASH)
			before_paste = 1;

		/* A token other than a parameter is copied (or pasted), on the line of the macro's use. */
		parameter = -1;
		if (arguments != NULL)
			parameter = pp_parameter(macro, &body[index]);
		if (parameter < 0) {
			copy = body[index];
			copy.line = line;
			copy.noexpand = 0U;
			if (pasting) {
				pp_paste(state, out, &copy, line);
			} else {
				pp_list_add(state, out, &copy);
			}

			/* The paste, if any, is done. */
			pasting = 0;
			continue;
		}

		/* A parameter next to ## is its argument as written; otherwise the argument expanded first. */
		if (pasting || before_paste) {
			expanded = arguments[parameter];
		} else {
			memset(&expanded, 0, sizeof(expanded));
			pp_expand(state, arguments[parameter].tokens, arguments[parameter].count, &expanded);
		}

		/* Its tokens; the first one is pasted onto what came before when ## was between them. */
		for (element = 0U; element < expanded.count; element++) {
			copy = expanded.tokens[element];
			copy.line = line;
			if (pasting && element == 0U) {
				pp_paste(state, out, &copy, line);
			} else {
				pp_list_add(state, out, &copy);
			}
		}

		/* The paste, if any, is done. */
		pasting = 0;
	}
}

/* Pastes a token onto the last token of a list: their spellings joined must form one token. */
static void
pp_paste(
	struct pp_state *state,
	struct pp_list *out,
	const struct glsl_token *right,
	unsigned line)
{
	struct glsl_token *left;
	struct glsl_token *tokens;
	char *text;
	size_t length;
	unsigned count;

	/* Nothing on the left (an empty argument) leaves the right token alone. */
	if (out->count == 0U) {
		pp_list_add(state, out, right);
		return;
	}

	/* The joined spelling, lexed again. */
	left = &out->tokens[out->count - 1U];
	length = (size_t)left->length + right->length;
	text = glsl_alloc(&state->shader->arena, length + 1U);
	memcpy(text, left->text, left->length);
	memcpy(text + left->length, right->text, right->length);
	count = glsl_lex(state->shader, text, length, line, &tokens);
	if (count != 1U) {
		glsl_error(state->shader, line, "'##' of '%.*s' and '%.*s' is not one token", (int)left->length, left->text,
			   (int)right->length, right->text);
		return;
	}

	/* The pasted token replaces the left one. */
	tokens[0].space = left->space;
	*left = tokens[0];
}

/* Returns the index of the parameter a token names, or -1. */
static int
pp_parameter(
	const struct pp_macro *macro,
	const struct glsl_token *token)
{
	unsigned index;
	int same;

	/* Only identifiers name parameters. */
	if (token->kind != GLSL_TOKEN_IDENTIFIER)
		return -1;

	/* The parameter of the same spelling. */
	for (index = 0U; index < macro->parameter_count; index++) {
		same = pp_same_tokens(&macro->parameters[index], 1U, token, 1U);
		if (same)
			return (int)index;
	}

	/* Not a parameter. */
	return -1;
}

/* Appends the value of __LINE__, __FILE__ or __VERSION__. */
static void
pp_special(
	struct pp_state *state,
	const struct pp_macro *macro,
	unsigned line,
	struct pp_list *out)
{
	struct glsl_token token;
	char *text;
	uint32_t value;

	/* The value (__FILE__ is source string 0). */
	value = 0U;
	if (macro->special == PP_SPECIAL_LINE)
		value = line;
	if (macro->special == PP_SPECIAL_VERSION)
		value = state->shader->version;

	/* An integer token of it. */
	text = glsl_alloc(&state->shader->arena, 16U);
	(void)snprintf(text, 16U, "%u", (unsigned)value);
	memset(&token, 0, sizeof(token));
	token.kind = GLSL_TOKEN_INT;
	token.text = text;
	token.length = (unsigned)strlen(text);
	token.line = line;
	token.space = 1U;
	token.integer = value;
	pp_list_add(state, out, &token);
}

/* Returns the macro a token names, or NULL. */
static struct pp_macro *
pp_find(
	struct pp_state *state,
	const struct glsl_token *token)
{
	struct pp_macro *macro;
	int differs;

	/* The macros, newest first. */
	for (macro = state->macros; macro != NULL; macro = macro->next) {
		if (macro->length != token->length)
			continue;
		differs = memcmp(macro->name, token->text, token->length);
		if (differs == 0)
			return macro;
	}

	/* No macro of that name. */
	return NULL;
}

/* Appends a copy of a token to a list. */
static void
pp_list_add(
	struct pp_state *state,
	struct pp_list *list,
	const struct glsl_token *token)
{
	struct glsl_token *grown;
	unsigned capacity;

	/* Grows the list when it is full. */
	if (list->count == list->capacity) {
		capacity = list->capacity * 2U;
		if (capacity < 8U)
			capacity = 8U;
		grown = glsl_alloc(&state->shader->arena, capacity * sizeof(*grown));
		if (list->count != 0U)
			memcpy(grown, list->tokens, list->count * sizeof(*grown));
		list->tokens = grown;
		list->capacity = capacity;
	}

	/* The copy. */
	list->tokens[list->count] = *token;
	list->count++;
}

/* Reports whether two lists of tokens are spelled the same, with white space in the same places. */
static int
pp_same_tokens(
	const struct glsl_token *left,
	unsigned left_count,
	const struct glsl_token *right,
	unsigned right_count)
{
	unsigned index;
	int differs;

	/* The same number of tokens. */
	if (left_count != right_count)
		return 0;

	/* Each pair spelled the same, and spaced the same after the first. */
	for (index = 0U; index < left_count; index++) {
		if (left[index].length != right[index].length)
			return 0;
		differs = memcmp(left[index].text, right[index].text, left[index].length);
		if (differs != 0)
			return 0;
		if (index > 0U && (left[index].space != 0U) != (right[index].space != 0U))
			return 0;
	}

	/* Succeeded: the same. */
	return 1;
}

/* Reports whether a macro name is reserved for the implementation (starts with GL_). */
static int
pp_reserved_name(
	const struct glsl_token *name)
{
	int differs;

	/* Shorter than GL_ is not. */
	if (name->length < 3U)
		return 0;

	/* The prefix. */
	differs = memcmp(name->text, "GL_", 3U);
	if (differs == 0)
		return 1;

	/* Any other name is the shader's. */
	return 0;
}

/* Reports whether the lines at this point are kept (every open group is being kept). */
static int
pp_keeping(
	const struct pp_state *state)
{
	/* Outside any group everything is kept. */
	if (state->depth == 0U)
		return 1;

	/* Otherwise the innermost group decides (it knows whether the ones around it keep). */
	if (state->conditionals[state->depth - 1U].keeping)
		return 1;

	/* Skipped. */
	return 0;
}
