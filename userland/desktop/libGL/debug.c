/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libGL's debug output (GL_KHR_debug, OpenGL 4.3's): messages the
 * application inserts, the debug groups' push and pop, and every GL error
 * the translation records (source API, type ERROR, severity HIGH, the id
 * the error), filtered by glDebugMessageControl's state (one per debug
 * group, a group starting with its parent's) and handed to the
 * application's callback, or kept in a log of the latest few when there is
 * none.  Objects take labels.  Everything happens in the call that makes
 * the message, so the output is always synchronous.
 *
 * The state lives in the context's fixed-function state (fixed_state.debug),
 * made at its first use.
 */

#include "fixed.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The longest message and label (with the terminator), the messages logged without a callback, and the deepest group stack. */
#define DEBUG_MESSAGE_LENGTH	256
#define DEBUG_LOGGED		16U
#define DEBUG_GROUPS		64U
#define DEBUG_LABEL_LENGTH	256

/* The sources, types and severities that control states keep apart, and the id rules a group keeps. */
#define DEBUG_SOURCES		6U
#define DEBUG_TYPES		9U
#define DEBUG_SEVERITIES	4U
#define DEBUG_ID_RULES		64U

/*
 * A message: its source, type, id and severity, and its text (length
 * without the terminator).
 */
struct debug_message {
	GLenum source;
	GLenum type;
	GLuint id;
	GLenum severity;
	GLsizei length;
	char text[DEBUG_MESSAGE_LENGTH];
};

/*
 * One rule glDebugMessageControl gave for ids of a source and a type.
 */
struct debug_id_rule {
	GLenum source;
	GLenum type;
	GLuint id;
	int enabled;
};

/*
 * A debug group: the message its push made (its pop makes one like it),
 * and the control state inside it (each source's and type's enabled
 * severities, bit s for severity s, and the id rules).
 */
struct debug_group {
	struct debug_message pushed;
	unsigned char enabled[DEBUG_SOURCES][DEBUG_TYPES];
	struct debug_id_rule rules[DEBUG_ID_RULES];
	unsigned rule_count;
};

/*
 * A label an object took: the object (a namespace and a name, or a sync
 * object's pointer) and the text.
 */
struct debug_label {
	GLenum identifier;
	GLuint name;
	const void *pointer;
	char *text;
	struct debug_label *next;
};

/*
 * A context's debug state: GL_DEBUG_OUTPUT, the callback and its
 * parameter, the log (a ring of the oldest first), the group stack (group
 * 0 the default one), and the labels.
 */
struct gl_debug {
	int output;
	int synchronous;
	GLDEBUGPROC callback;
	const void *user;
	struct debug_message log[DEBUG_LOGGED];
	unsigned log_first;
	unsigned log_count;
	struct debug_group groups[DEBUG_GROUPS];
	unsigned depth;
	struct debug_label *labels;
};

static struct gl_debug *debug_state(struct zegl_context **context);
static int debug_source_index(GLenum source);
static int debug_type_index(GLenum type);
static int debug_severity_index(GLenum severity);
static int debug_enabled(const struct gl_debug *debug, const struct debug_message *message);
static void debug_emit(struct gl_debug *debug, const struct debug_message *message);
static void debug_message_set(struct debug_message *message, GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar *text);
static void debug_control_set(struct debug_group *group, int source, int type, int severity, int enabled);
static int debug_object_exists(struct gles_state *state, GLenum identifier, GLuint name);
static void debug_label_set(struct gl_debug *debug, GLenum identifier, GLuint name, const void *pointer, GLsizei length, const GLchar *label, struct zegl_context *context);
static void debug_label_get(struct gl_debug *debug, GLenum identifier, GLuint name, const void *pointer, GLsizei bufSize, GLsizei *length, GLchar *label);

/*
 * Makes a debug message of a GL error the translation recorded (the hook
 * gles_error calls).
 */
void
gl_debug_error(
	struct zegl_context *context,
	GLenum error)
{
	struct gles_state *state;
	struct fixed_state *fixed;
	struct debug_message message;
	char text[64];
	const char *name;

	/* A context whose debug state was made (none was asked for otherwise; the state is not made here, which records errors itself). */
	state = context->gles.state;
	if (state == NULL || state->fixed == NULL)
		return;
	fixed = state->fixed;
	if (fixed->debug == NULL || !fixed->debug->output)
		return;

	/* The error's name. */
	switch (error) {
	case GL_INVALID_ENUM:
		name = "GL_INVALID_ENUM";
		break;
	case GL_INVALID_VALUE:
		name = "GL_INVALID_VALUE";
		break;
	case GL_INVALID_OPERATION:
		name = "GL_INVALID_OPERATION";
		break;
	case GL_INVALID_FRAMEBUFFER_OPERATION:
		name = "GL_INVALID_FRAMEBUFFER_OPERATION";
		break;
	case GL_OUT_OF_MEMORY:
		name = "GL_OUT_OF_MEMORY";
		break;
	default:
		name = "an error";
		break;
	}

	/* The message, as an API error of high severity. */
	(void)snprintf(text, sizeof(text), "%s (0x%04x) recorded", name, (unsigned)error);
	debug_message_set(&message, GL_DEBUG_SOURCE_API, GL_DEBUG_TYPE_ERROR, error, GL_DEBUG_SEVERITY_HIGH, -1, text);
	debug_emit(fixed->debug, &message);
}

/*
 * Returns the flag of a debug capability (GL_DEBUG_OUTPUT,
 * GL_DEBUG_OUTPUT_SYNCHRONOUS) for glEnable and glIsEnabled; nonzero when
 * cap is not one.
 */
int
gl_debug_capability(
	GLenum cap,
	int **flag)
{
	struct zegl_context *context;
	struct gl_debug *debug;

	/* One of the two. */
	if (cap != GL_DEBUG_OUTPUT && cap != GL_DEBUG_OUTPUT_SYNCHRONOUS)
		return -1;

	/* The state's flag. */
	debug = debug_state(&context);
	if (debug == NULL)
		return -1;
	*flag = &debug->output;
	if (cap == GL_DEBUG_OUTPUT_SYNCHRONOUS)
		*flag = &debug->synchronous;

	/* Succeeded: the flag. */
	return 0;
}

/*
 * Writes a debug state's or limit's value as a float; returns how many
 * values, 0 when the name is not one.
 */
unsigned
gl_debug_get(
	GLenum pname,
	GLfloat *values)
{
	struct zegl_context *context;
	struct gl_debug *debug;

	/* The limits. */
	switch (pname) {
	case GL_MAX_DEBUG_MESSAGE_LENGTH:
		values[0] = (GLfloat)DEBUG_MESSAGE_LENGTH;
		return 1U;
	case GL_MAX_DEBUG_LOGGED_MESSAGES:
		values[0] = (GLfloat)DEBUG_LOGGED;
		return 1U;
	case GL_MAX_DEBUG_GROUP_STACK_DEPTH:
		values[0] = (GLfloat)DEBUG_GROUPS;
		return 1U;
	case GL_MAX_LABEL_LENGTH:
		values[0] = (GLfloat)DEBUG_LABEL_LENGTH;
		return 1U;
	case GL_DEBUG_LOGGED_MESSAGES:
	case GL_DEBUG_NEXT_LOGGED_MESSAGE_LENGTH:
	case GL_DEBUG_GROUP_STACK_DEPTH:
		break;
	default:
		return 0U;
	}

	/* The states. */
	debug = debug_state(&context);
	if (debug == NULL)
		return 0U;
	if (pname == GL_DEBUG_LOGGED_MESSAGES) {
		values[0] = (GLfloat)debug->log_count;
	} else if (pname == GL_DEBUG_GROUP_STACK_DEPTH) {
		values[0] = (GLfloat)(debug->depth + 1U);
	} else {
		/* The next message's length with its terminator, 0 without one. */
		values[0] = 0.0f;
		if (debug->log_count != 0U)
			values[0] = (GLfloat)(debug->log[debug->log_first].length + 1);
	}

	/* Succeeded: one value. */
	return 1U;
}

/*
 * Frees a context's debug state.
 */
void
gl_debug_release(
	struct fixed_state *fixed)
{
	struct debug_label *label;

	/* None made. */
	if (fixed->debug == NULL)
		return;

	/* The labels, then the state. */
	while (fixed->debug->labels != NULL) {
		label = fixed->debug->labels;
		fixed->debug->labels = label->next;
		free(label->text);
		free(label);
	}
	free(fixed->debug);
	fixed->debug = NULL;
}

/*
 * Sets the callback debug messages go to (NULL: the log).
 */
GL_APICALL void GL_APIENTRY
glDebugMessageCallback(
	GLDEBUGPROC callback,
	const void *userParam)
{
	struct zegl_context *context;
	struct gl_debug *debug;

	/* The state. */
	debug = debug_state(&context);
	if (debug == NULL)
		return;

	/* Succeeded: the callback and its parameter. */
	debug->callback = callback;
	debug->user = userParam;
}

/*
 * Enables or disables the messages of a source, a type and a severity
 * (GL_DONT_CARE: all of them), or of some ids of one source and type.
 */
GL_APICALL void GL_APIENTRY
glDebugMessageControl(
	GLenum source,
	GLenum type,
	GLenum severity,
	GLsizei count,
	const GLuint *ids,
	GLboolean enabled)
{
	struct zegl_context *context;
	struct gl_debug *debug;
	struct debug_group *group;
	struct debug_id_rule *rule;
	GLsizei index;
	unsigned slot;
	int source_index;
	int type_index;
	int severity_index;

	/* The state, and enums it knows (GL_DONT_CARE is -1). */
	debug = debug_state(&context);
	if (debug == NULL)
		return;
	source_index = debug_source_index(source);
	type_index = debug_type_index(type);
	severity_index = debug_severity_index(severity);
	if (source_index < -1 || type_index < -1 || severity_index < -1) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}
	if (count < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* The group's state. */
	group = &debug->groups[debug->depth];

	/* By severity, for every source and type that match. */
	if (count == 0) {
		debug_control_set(group, source_index, type_index, severity_index, enabled != GL_FALSE);
		return;
	}

	/* By ids: of one source and one type, any severity. */
	if (source_index < 0 || type_index < 0 || severity_index != -1) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* Each id's rule, a new one or the one there is. */
	for (index = 0; index < count; index++) {
		for (slot = 0U; slot < group->rule_count; slot++) {
			rule = &group->rules[slot];
			if (rule->source == source && rule->type == type && rule->id == ids[index])
				break;
		}

		/* A new rule when there is room. */
		if (slot == group->rule_count) {
			if (group->rule_count == DEBUG_ID_RULES) {
				gles_error(context, GL_OUT_OF_MEMORY);
				return;
			}
			group->rule_count++;
		}

		/* The rule. */
		rule = &group->rules[slot];
		rule->source = source;
		rule->type = type;
		rule->id = ids[index];
		rule->enabled = (enabled != GL_FALSE);
	}
}

/*
 * Makes a debug message of the application's (or a third party's).
 */
GL_APICALL void GL_APIENTRY
glDebugMessageInsert(
	GLenum source,
	GLenum type,
	GLuint id,
	GLenum severity,
	GLsizei length,
	const GLchar *buf)
{
	struct zegl_context *context;
	struct gl_debug *debug;
	struct debug_message message;
	int type_index;
	int severity_index;
	size_t size;

	/* The state, an application's or a third party's source, a type and a severity. */
	debug = debug_state(&context);
	if (debug == NULL)
		return;
	type_index = debug_type_index(type);
	severity_index = debug_severity_index(severity);
	if ((source != GL_DEBUG_SOURCE_APPLICATION && source != GL_DEBUG_SOURCE_THIRD_PARTY) || type_index < 0 || severity_index < 0) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* A message shorter than the longest. */
	size = (size_t)length;
	if (length < 0)
		size = strlen(buf);
	if (size >= (size_t)DEBUG_MESSAGE_LENGTH) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Succeeded: the message, out. */
	debug_message_set(&message, source, type, id, severity, (GLsizei)size, buf);
	debug_emit(debug, &message);
}

/*
 * Takes the oldest logged messages (at most count, each whole in the
 * room messageLog has; NULL takes them without their texts); returns how
 * many were taken.
 */
GL_APICALL GLuint GL_APIENTRY
glGetDebugMessageLog(
	GLuint count,
	GLsizei bufSize,
	GLenum *sources,
	GLenum *types,
	GLuint *ids,
	GLenum *severities,
	GLsizei *lengths,
	GLchar *messageLog)
{
	struct zegl_context *context;
	struct gl_debug *debug;
	struct debug_message *message;
	GLuint taken;
	size_t used;
	size_t need;

	/* The state, and room that is not negative. */
	debug = debug_state(&context);
	if (debug == NULL)
		return 0U;
	if (messageLog != NULL && bufSize < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return 0U;
	}

	/* The oldest ones while they fit. */
	taken = 0U;
	used = 0U;
	while (taken < count && debug->log_count != 0U) {
		message = &debug->log[debug->log_first];
		need = (size_t)message->length + 1U;
		if (messageLog != NULL && used + need > (size_t)bufSize)
			break;

		/* Its fields and text. */
		if (sources != NULL)
			sources[taken] = message->source;
		if (types != NULL)
			types[taken] = message->type;
		if (ids != NULL)
			ids[taken] = message->id;
		if (severities != NULL)
			severities[taken] = message->severity;
		if (lengths != NULL)
			lengths[taken] = message->length + 1;
		if (messageLog != NULL) {
			memcpy(messageLog + used, message->text, need);
			used += need;
		}

		/* Out of the log. */
		debug->log_first = (debug->log_first + 1U) % DEBUG_LOGGED;
		debug->log_count--;
		taken++;
	}

	/* Succeeded: how many were taken. */
	return taken;
}

/*
 * Starts a debug group: its message out, the control state its parent's.
 */
GL_APICALL void GL_APIENTRY
glPushDebugGroup(
	GLenum source,
	GLuint id,
	GLsizei length,
	const GLchar *message)
{
	struct zegl_context *context;
	struct gl_debug *debug;
	struct debug_group *group;
	size_t size;

	/* The state, an application's or a third party's source, and a message shorter than the longest. */
	debug = debug_state(&context);
	if (debug == NULL)
		return;
	if (source != GL_DEBUG_SOURCE_APPLICATION && source != GL_DEBUG_SOURCE_THIRD_PARTY) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}
	size = (size_t)length;
	if (length < 0)
		size = strlen(message);
	if (size >= (size_t)DEBUG_MESSAGE_LENGTH) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Room on the stack. */
	if (debug->depth + 1U >= DEBUG_GROUPS) {
		gles_error(context, GL_STACK_OVERFLOW);
		return;
	}

	/* The group, its control state its parent's, and its message out. */
	group = &debug->groups[debug->depth + 1U];
	memcpy(group, &debug->groups[debug->depth], sizeof(*group));
	debug_message_set(&group->pushed, source, GL_DEBUG_TYPE_PUSH_GROUP, id, GL_DEBUG_SEVERITY_NOTIFICATION, (GLsizei)size, message);
	debug->depth++;
	debug_emit(debug, &group->pushed);
}

/*
 * Ends the innermost debug group: a message like its push's, and its
 * parent's control state again.
 */
GL_APICALL void GL_APIENTRY
glPopDebugGroup(void)
{
	struct zegl_context *context;
	struct gl_debug *debug;
	struct debug_message popped;

	/* The state, and a group to end (not the default one). */
	debug = debug_state(&context);
	if (debug == NULL)
		return;
	if (debug->depth == 0U) {
		gles_error(context, GL_STACK_UNDERFLOW);
		return;
	}

	/* Its message, of the group's control state, then the parent's. */
	memcpy(&popped, &debug->groups[debug->depth].pushed, sizeof(popped));
	popped.type = GL_DEBUG_TYPE_POP_GROUP;
	debug_emit(debug, &popped);
	debug->depth--;
}

/*
 * Labels an object of a namespace (NULL: no label).
 */
GL_APICALL void GL_APIENTRY
glObjectLabel(
	GLenum identifier,
	GLuint name,
	GLsizei length,
	const GLchar *label)
{
	struct zegl_context *context;
	struct gl_debug *debug;
	int exists;

	/* The state, and an object of the namespace. */
	debug = debug_state(&context);
	if (debug == NULL)
		return;
	exists = debug_object_exists(gles_state(context), identifier, name);
	if (exists < 0) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}
	if (exists == 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Succeeded: the label. */
	debug_label_set(debug, identifier, name, NULL, length, label, context);
}

/*
 * Reports an object's label (empty without one).
 */
GL_APICALL void GL_APIENTRY
glGetObjectLabel(
	GLenum identifier,
	GLuint name,
	GLsizei bufSize,
	GLsizei *length,
	GLchar *label)
{
	struct zegl_context *context;
	struct gl_debug *debug;
	int exists;

	/* The state, room that is not negative, and an object of the namespace. */
	debug = debug_state(&context);
	if (debug == NULL)
		return;
	if (bufSize < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}
	exists = debug_object_exists(gles_state(context), identifier, name);
	if (exists < 0) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}
	if (exists == 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Succeeded: the label. */
	debug_label_get(debug, identifier, name, NULL, bufSize, length, label);
}

/*
 * Labels a sync object (NULL: no label).
 */
GL_APICALL void GL_APIENTRY
glObjectPtrLabel(
	const void *ptr,
	GLsizei length,
	const GLchar *label)
{
	struct zegl_context *context;
	struct gl_debug *debug;
	GLboolean sync;

	/* The state, and a sync object. */
	debug = debug_state(&context);
	if (debug == NULL)
		return;
	sync = glIsSync((GLsync)ptr);
	if (!sync) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Succeeded: the label. */
	debug_label_set(debug, 0U, 0U, ptr, length, label, context);
}

/*
 * Reports a sync object's label (empty without one).
 */
GL_APICALL void GL_APIENTRY
glGetObjectPtrLabel(
	const void *ptr,
	GLsizei bufSize,
	GLsizei *length,
	GLchar *label)
{
	struct zegl_context *context;
	struct gl_debug *debug;
	GLboolean sync;

	/* The state, room that is not negative, and a sync object. */
	debug = debug_state(&context);
	if (debug == NULL)
		return;
	sync = glIsSync((GLsync)ptr);
	if (bufSize < 0 || !sync) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Succeeded: the label. */
	debug_label_get(debug, 0U, 0U, ptr, bufSize, length, label);
}

/*
 * Reports the debug callback or its parameter (the only pointers libGL's
 * contexts keep).
 */
GL_APICALL void GL_APIENTRY
glGetPointerv(
	GLenum pname,
	void **params)
{
	struct zegl_context *context;
	struct gl_debug *debug;

	/* The state. */
	debug = debug_state(&context);
	if (debug == NULL)
		return;

	/* The callback or its parameter. */
	if (pname == GL_DEBUG_CALLBACK_FUNCTION) {
		memcpy(params, &debug->callback, sizeof(*params));
	} else if (pname == GL_DEBUG_CALLBACK_USER_PARAM) {
		*params = (void *)debug->user;
	} else {
		gles_error(context, GL_INVALID_ENUM);
	}
}

/*
 * Returns the current context's debug state, made at its first use with
 * GL_DEBUG_OUTPUT on in a debug context (GL_CONTEXT_FLAG_DEBUG_BIT) and off
 * otherwise, and every message enabled but those of low severity; NULL
 * without a context or memory (GL_OUT_OF_MEMORY recorded).
 */
static struct gl_debug *
debug_state(
	struct zegl_context **context)
{
	struct fixed_state *fixed;
	struct gl_debug *debug;
	GLint flags;

	/* The context's fixed-function state. */
	fixed = fixed_current(context);
	if (fixed == NULL)
		return NULL;
	if (fixed->debug != NULL)
		return fixed->debug;

	/* The state. */
	debug = calloc(1U, sizeof(*debug));
	if (debug == NULL) {
		gles_error(*context, GL_OUT_OF_MEMORY);
		return NULL;
	}

	/* The default group's control state: low severity off. */
	debug_control_set(&debug->groups[0], -1, -1, -1, 1);
	debug_control_set(&debug->groups[0], -1, -1, debug_severity_index(GL_DEBUG_SEVERITY_LOW), 0);

	/* The output on in a debug context. */
	(void)gl_context_version(&flags);
	if ((flags & GL_CONTEXT_FLAG_DEBUG_BIT) != 0)
		debug->output = 1;
	debug->synchronous = 0;

	/* Succeeded: kept for the context. */
	fixed->debug = debug;
	return debug;
}

/* Returns a source's index, -1 for GL_DONT_CARE, -2 for an enum that is not one. */
static int
debug_source_index(
	GLenum source)
{
	/* The six sources. */
	switch (source) {
	case GL_DONT_CARE:
		return -1;
	case GL_DEBUG_SOURCE_API:
		return 0;
	case GL_DEBUG_SOURCE_WINDOW_SYSTEM:
		return 1;
	case GL_DEBUG_SOURCE_SHADER_COMPILER:
		return 2;
	case GL_DEBUG_SOURCE_THIRD_PARTY:
		return 3;
	case GL_DEBUG_SOURCE_APPLICATION:
		return 4;
	case GL_DEBUG_SOURCE_OTHER:
		return 5;
	default:
		break;
	}

	/* Not a source. */
	return -2;
}

/* Returns a type's index, -1 for GL_DONT_CARE, -2 for an enum that is not one. */
static int
debug_type_index(
	GLenum type)
{
	/* The nine types. */
	switch (type) {
	case GL_DONT_CARE:
		return -1;
	case GL_DEBUG_TYPE_ERROR:
		return 0;
	case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR:
		return 1;
	case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:
		return 2;
	case GL_DEBUG_TYPE_PORTABILITY:
		return 3;
	case GL_DEBUG_TYPE_PERFORMANCE:
		return 4;
	case GL_DEBUG_TYPE_OTHER:
		return 5;
	case GL_DEBUG_TYPE_MARKER:
		return 6;
	case GL_DEBUG_TYPE_PUSH_GROUP:
		return 7;
	case GL_DEBUG_TYPE_POP_GROUP:
		return 8;
	default:
		break;
	}

	/* Not a type. */
	return -2;
}

/* Returns a severity's index, -1 for GL_DONT_CARE, -2 for an enum that is not one. */
static int
debug_severity_index(
	GLenum severity)
{
	/* The four severities. */
	switch (severity) {
	case GL_DONT_CARE:
		return -1;
	case GL_DEBUG_SEVERITY_HIGH:
		return 0;
	case GL_DEBUG_SEVERITY_MEDIUM:
		return 1;
	case GL_DEBUG_SEVERITY_LOW:
		return 2;
	case GL_DEBUG_SEVERITY_NOTIFICATION:
		return 3;
	default:
		break;
	}

	/* Not a severity. */
	return -2;
}

/* Reports whether the innermost group's control state lets a message out: an id rule for it first, else its severity. */
static int
debug_enabled(
	const struct gl_debug *debug,
	const struct debug_message *message)
{
	const struct debug_group *group;
	const struct debug_id_rule *rule;
	unsigned slot;
	int source;
	int type;
	int severity;

	/* The rule for the message's source, type and id. */
	group = &debug->groups[debug->depth];
	for (slot = 0U; slot < group->rule_count; slot++) {
		rule = &group->rules[slot];
		if (rule->source == message->source && rule->type == message->type && rule->id == message->id)
			return rule->enabled;
	}

	/* Else its source's and type's severities. */
	source = debug_source_index(message->source);
	type = debug_type_index(message->type);
	severity = debug_severity_index(message->severity);
	if (source < 0 || type < 0 || severity < 0)
		return 0;
	if ((group->enabled[source][type] & (1U << severity)) == 0U)
		return 0;

	/* Succeeded: let out. */
	return 1;
}

/* Hands a message to the callback, or keeps it in the log (dropped when the log is full), when the output is on and the control lets it out. */
static void
debug_emit(
	struct gl_debug *debug,
	const struct debug_message *message)
{
	int enabled;

	/* The output on, and the message let out. */
	if (!debug->output)
		return;
	enabled = debug_enabled(debug, message);
	if (!enabled)
		return;

	/* The callback, when the application gave one. */
	if (debug->callback != NULL) {
		debug->callback(message->source, message->type, message->id, message->severity, message->length, message->text,
				debug->user);
		return;
	}

	/* Else the log, while it has room. */
	if (debug->log_count == DEBUG_LOGGED)
		return;
	memcpy(&debug->log[(debug->log_first + debug->log_count) % DEBUG_LOGGED], message, sizeof(*message));
	debug->log_count++;
}

/* Fills a message (a length below 0: the text's own; the text cut at the longest). */
static void
debug_message_set(
	struct debug_message *message,
	GLenum source,
	GLenum type,
	GLuint id,
	GLenum severity,
	GLsizei length,
	const GLchar *text)
{
	size_t size;

	/* The fields. */
	message->source = source;
	message->type = type;
	message->id = id;
	message->severity = severity;

	/* The text, terminated. */
	size = (size_t)length;
	if (length < 0)
		size = strlen(text);
	if (size >= (size_t)DEBUG_MESSAGE_LENGTH)
		size = (size_t)DEBUG_MESSAGE_LENGTH - 1U;
	memcpy(message->text, text, size);
	message->text[size] = '\0';
	message->length = (GLsizei)size;
}

/* Sets whether the messages of the sources, types and severities matching (-1: every one) are let out. */
static void
debug_control_set(
	struct debug_group *group,
	int source,
	int type,
	int severity,
	int enabled)
{
	unsigned bits;
	unsigned each_source;
	unsigned each_type;

	/* The severities' bits. */
	bits = (1U << DEBUG_SEVERITIES) - 1U;
	if (severity >= 0)
		bits = 1U << severity;

	/* Each matching source and type. */
	for (each_source = 0U; each_source < DEBUG_SOURCES; each_source++) {
		if (source >= 0 && (unsigned)source != each_source)
			continue;
		for (each_type = 0U; each_type < DEBUG_TYPES; each_type++) {
			if (type >= 0 && (unsigned)type != each_type)
				continue;
			if (enabled) {
				group->enabled[each_source][each_type] |= (unsigned char)bits;
			} else {
				group->enabled[each_source][each_type] &= (unsigned char)~bits;
			}
		}
	}
}

/* Reports whether a name is an object of a namespace: 1, 0 when it is not, -1 for an identifier that is not a namespace. */
static int
debug_object_exists(
	struct gles_state *state,
	GLenum identifier,
	GLuint name)
{
	struct gles_shader *shader;
	void *object;
	int kind;

	/* The namespace's object of the name. */
	switch (identifier) {
	case GL_BUFFER:
		object = gles_names_get(&state->buffers, name);
		break;
	case GL_TEXTURE:
		object = gles_names_get(&state->textures, name);
		break;
	case GL_SHADER:
	case GL_PROGRAM:
		/* The shared namespace: an object of the kind (a shader's and a program's kind is their first member). */
		kind = GLES_KIND_PROGRAM;
		if (identifier == GL_SHADER)
			kind = GLES_KIND_SHADER;
		object = gles_names_get(&state->objects, name);
		shader = object;
		if (shader != NULL && shader->kind != kind)
			object = NULL;
		break;
	case GL_VERTEX_ARRAY:
		object = gles_names_get(&state->vertex_arrays, name);
		break;
	case GL_QUERY:
		object = gles_names_get(&state->query_objects, name);
		break;
	case GL_TRANSFORM_FEEDBACK:
		object = gles_names_get(&state->feedbacks, name);
		break;
	case GL_SAMPLER:
		object = gles_names_get(&state->sampler_objects, name);
		break;
	case GL_RENDERBUFFER:
		object = gles_names_get(&state->renderbuffers, name);
		break;
	case GL_FRAMEBUFFER:
		object = gles_names_get(&state->framebuffers, name);
		break;
	default:
		return -1;
	}

	/* Whether there is one. */
	if (object == NULL)
		return 0;

	/* Succeeded: an object of the namespace. */
	return 1;
}

/* Sets an object's label (a length below 0: the label's own; NULL takes the label away). */
static void
debug_label_set(
	struct gl_debug *debug,
	GLenum identifier,
	GLuint name,
	const void *pointer,
	GLsizei length,
	const GLchar *label,
	struct zegl_context *context)
{
	struct debug_label **link;
	struct debug_label *entry;
	char *text;
	size_t size;

	/* The object's label there is. */
	for (link = &debug->labels; *link != NULL; link = &(*link)->next) {
		entry = *link;
		if (entry->identifier == identifier && entry->name == name && entry->pointer == pointer)
			break;
	}

	/* No label: the one there is goes. */
	if (label == NULL) {
		if (*link != NULL) {
			entry = *link;
			*link = entry->next;
			free(entry->text);
			free(entry);
		}
		return;
	}

	/* A label shorter than the longest. */
	size = (size_t)length;
	if (length < 0)
		size = strlen(label);
	if (size >= (size_t)DEBUG_LABEL_LENGTH) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Its copy. */
	text = malloc(size + 1U);
	if (text == NULL) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return;
	}
	memcpy(text, label, size);
	text[size] = '\0';

	/* The entry there is, or a new one first in the list. */
	entry = *link;
	if (entry == NULL) {
		entry = calloc(1U, sizeof(*entry));
		if (entry == NULL) {
			free(text);
			gles_error(context, GL_OUT_OF_MEMORY);
			return;
		}
		entry->identifier = identifier;
		entry->name = name;
		entry->pointer = pointer;
		entry->next = debug->labels;
		debug->labels = entry;
	}

	/* Succeeded: the label. */
	free(entry->text);
	entry->text = text;
}

/* Copies an object's label into the room given (cut to it, terminated) with its length; an object without one has an empty label. */
static void
debug_label_get(
	struct gl_debug *debug,
	GLenum identifier,
	GLuint name,
	const void *pointer,
	GLsizei bufSize,
	GLsizei *length,
	GLchar *label)
{
	const struct debug_label *entry;
	const char *text;
	size_t size;

	/* The object's label, or none. */
	text = "";
	for (entry = debug->labels; entry != NULL; entry = entry->next) {
		if (entry->identifier == identifier && entry->name == name && entry->pointer == pointer) {
			text = entry->text;
			break;
		}
	}

	/* Without room, only its length. */
	size = strlen(text);
	if (label == NULL || bufSize == 0) {
		if (length != NULL)
			*length = (GLsizei)size;
		return;
	}

	/* Succeeded: what fits, terminated. */
	if (size > (size_t)bufSize - 1U)
		size = (size_t)bufSize - 1U;
	memcpy(label, text, size);
	label[size] = '\0';
	if (length != NULL)
		*length = (GLsizei)size;
}
