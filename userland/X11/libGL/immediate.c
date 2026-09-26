/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libGL's immediate mode, vertex arrays and display lists (WS069 p005).
 *
 * glVertex keeps a vertex with the current colour, normal and texture
 * coordinates (generic attributes 1 to 3); glEnd draws them as client
 * arrays with one glDrawArrays, the fixed-function layer's program
 * drawing them.  glVertexPointer and the others are the generic
 * attributes' arrays.  A display list records the fixed-function calls
 * (fixed.h's commands) and glCallList makes them again.
 */

#include "fixed.h"

#include <stdlib.h>
#include <string.h>

static void immediate_vertex(GLfloat x, GLfloat y, GLfloat z, GLfloat w);
static void immediate_current(enum fixed_op op, GLuint attribute, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
static struct fixed_list *immediate_list(struct fixed_state *fixed, GLuint name, int make);
static void immediate_replay(const struct fixed_command *command);
static GLuint immediate_array(GLenum array);

/*
 * Begins a primitive of vertices.
 */
GL_APICALL void GL_APIENTRY
glBegin(
	GLenum mode)
{
	struct zegl_context *context;
	struct fixed_state *fixed;
	int recorded;

	/* The state, and the list being compiled. */
	fixed = fixed_current(&context);
	if (fixed == NULL)
		return;
	recorded = fixed_record(fixed, FIXED_OP_BEGIN, mode, 0U, NULL, 0U);
	if (recorded == FIXED_COMPILE_ONLY)
		return;

	/* Not already inside, and a mode (points up to polygons). */
	if (fixed->in_begin) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* A mode (points up to polygons). */
	if (mode > GL_POLYGON) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* Inside, with no vertices yet. */
	fixed->in_begin = 1;
	fixed->begin_mode = mode;
	fixed->vertex_count = 0U;
}

/*
 * Ends the primitive: its vertices drawn as client arrays.
 */
GL_APICALL void GL_APIENTRY
glEnd(void)
{
	struct zegl_context *context;
	struct fixed_state *fixed;
	struct gles_state *state;
	struct gles_attrib saved[4];
	struct gles_buffer *array_buffer;
	int recorded;

	/* The state, and the list being compiled. */
	fixed = fixed_current(&context);
	if (fixed == NULL)
		return;
	recorded = fixed_record(fixed, FIXED_OP_END, 0U, 0U, NULL, 0U);
	if (recorded == FIXED_COMPILE_ONLY)
		return;

	/* Inside a primitive. */
	if (!fixed->in_begin) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* Outside again; nothing drawn without vertices. */
	fixed->in_begin = 0;
	if (fixed->vertex_count == 0U)
		return;

	/* The four attributes read the vertices for this draw (their arrays are put back after it). */
	state = gles_state(context);
	memcpy(saved, state->attribs, sizeof(saved));
	array_buffer = state->array_buffer;
	state->array_buffer = NULL;
	glVertexAttribPointer(FIXED_POSITION, 4, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(struct fixed_vertex), fixed->vertices[0].position);
	glVertexAttribPointer(FIXED_COLOR, 4, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(struct fixed_vertex), fixed->vertices[0].color);
	glVertexAttribPointer(FIXED_NORMAL, 3, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(struct fixed_vertex), fixed->vertices[0].normal);
	glVertexAttribPointer(FIXED_TEXCOORD, 4, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(struct fixed_vertex), fixed->vertices[0].texcoord);
	state->attribs[FIXED_POSITION].enabled = 1;
	state->attribs[FIXED_COLOR].enabled = 1;
	state->attribs[FIXED_NORMAL].enabled = 1;
	state->attribs[FIXED_TEXCOORD].enabled = 1;

	/* One draw of them all. */
	glDrawArrays(fixed->begin_mode, 0, (GLsizei)fixed->vertex_count);

	/* The arrays as they were. */
	memcpy(state->attribs, saved, sizeof(saved));
	state->array_buffer = array_buffer;
}

/* The glVertex calls: each keeps a vertex (x, y, z, w) with the current colour, normal and texture coordinates. */

GL_APICALL void GL_APIENTRY
glVertex4f(
	GLfloat x,
	GLfloat y,
	GLfloat z,
	GLfloat w)
{
	/* The vertex as it is. */
	immediate_vertex(x, y, z, w);
}

GL_APICALL void GL_APIENTRY
glVertex4fv(
	const GLfloat *v)
{
	/* Four values. */
	immediate_vertex(v[0], v[1], v[2], v[3]);
}

GL_APICALL void GL_APIENTRY
glVertex3f(
	GLfloat x,
	GLfloat y,
	GLfloat z)
{
	/* w is 1. */
	immediate_vertex(x, y, z, 1.0f);
}

GL_APICALL void GL_APIENTRY
glVertex3fv(
	const GLfloat *v)
{
	/* Three values, w 1. */
	immediate_vertex(v[0], v[1], v[2], 1.0f);
}

GL_APICALL void GL_APIENTRY
glVertex3d(
	GLdouble x,
	GLdouble y,
	GLdouble z)
{
	/* As floats. */
	immediate_vertex((GLfloat)x, (GLfloat)y, (GLfloat)z, 1.0f);
}

GL_APICALL void GL_APIENTRY
glVertex3dv(
	const GLdouble *v)
{
	/* Three doubles as floats. */
	immediate_vertex((GLfloat)v[0], (GLfloat)v[1], (GLfloat)v[2], 1.0f);
}

GL_APICALL void GL_APIENTRY
glVertex3i(
	GLint x,
	GLint y,
	GLint z)
{
	/* As floats. */
	immediate_vertex((GLfloat)x, (GLfloat)y, (GLfloat)z, 1.0f);
}

GL_APICALL void GL_APIENTRY
glVertex2f(
	GLfloat x,
	GLfloat y)
{
	/* z 0, w 1. */
	immediate_vertex(x, y, 0.0f, 1.0f);
}

GL_APICALL void GL_APIENTRY
glVertex2fv(
	const GLfloat *v)
{
	/* Two values. */
	immediate_vertex(v[0], v[1], 0.0f, 1.0f);
}

GL_APICALL void GL_APIENTRY
glVertex2d(
	GLdouble x,
	GLdouble y)
{
	/* As floats. */
	immediate_vertex((GLfloat)x, (GLfloat)y, 0.0f, 1.0f);
}

GL_APICALL void GL_APIENTRY
glVertex2i(
	GLint x,
	GLint y)
{
	/* As floats. */
	immediate_vertex((GLfloat)x, (GLfloat)y, 0.0f, 1.0f);
}

GL_APICALL void GL_APIENTRY
glVertex2s(
	GLshort x,
	GLshort y)
{
	/* As floats. */
	immediate_vertex((GLfloat)x, (GLfloat)y, 0.0f, 1.0f);
}

/* The glColor calls: each sets the current colour (generic attribute 1). */

GL_APICALL void GL_APIENTRY
glColor4f(
	GLfloat red,
	GLfloat green,
	GLfloat blue,
	GLfloat alpha)
{
	/* The colour as it is. */
	immediate_current(FIXED_OP_COLOR, FIXED_COLOR, red, green, blue, alpha);
}

GL_APICALL void GL_APIENTRY
glColor4fv(
	const GLfloat *v)
{
	/* Four values. */
	immediate_current(FIXED_OP_COLOR, FIXED_COLOR, v[0], v[1], v[2], v[3]);
}

GL_APICALL void GL_APIENTRY
glColor4d(
	GLdouble red,
	GLdouble green,
	GLdouble blue,
	GLdouble alpha)
{
	/* As floats. */
	immediate_current(FIXED_OP_COLOR, FIXED_COLOR, (GLfloat)red, (GLfloat)green, (GLfloat)blue, (GLfloat)alpha);
}

GL_APICALL void GL_APIENTRY
glColor4ub(
	GLubyte red,
	GLubyte green,
	GLubyte blue,
	GLubyte alpha)
{
	/* Bytes as fractions of 255. */
	immediate_current(FIXED_OP_COLOR, FIXED_COLOR, (GLfloat)red / 255.0f, (GLfloat)green / 255.0f,
			  (GLfloat)blue / 255.0f, (GLfloat)alpha / 255.0f);
}

GL_APICALL void GL_APIENTRY
glColor4ubv(
	const GLubyte *v)
{
	/* Four bytes. */
	glColor4ub(v[0], v[1], v[2], v[3]);
}

GL_APICALL void GL_APIENTRY
glColor3f(
	GLfloat red,
	GLfloat green,
	GLfloat blue)
{
	/* Alpha 1. */
	immediate_current(FIXED_OP_COLOR, FIXED_COLOR, red, green, blue, 1.0f);
}

GL_APICALL void GL_APIENTRY
glColor3fv(
	const GLfloat *v)
{
	/* Three values, alpha 1. */
	immediate_current(FIXED_OP_COLOR, FIXED_COLOR, v[0], v[1], v[2], 1.0f);
}

GL_APICALL void GL_APIENTRY
glColor3d(
	GLdouble red,
	GLdouble green,
	GLdouble blue)
{
	/* As floats, alpha 1. */
	immediate_current(FIXED_OP_COLOR, FIXED_COLOR, (GLfloat)red, (GLfloat)green, (GLfloat)blue, 1.0f);
}

GL_APICALL void GL_APIENTRY
glColor3ub(
	GLubyte red,
	GLubyte green,
	GLubyte blue)
{
	/* Bytes, alpha 255. */
	glColor4ub(red, green, blue, 255U);
}

GL_APICALL void GL_APIENTRY
glColor3ubv(
	const GLubyte *v)
{
	/* Three bytes. */
	glColor4ub(v[0], v[1], v[2], 255U);
}

/* The glNormal calls: each sets the current normal (generic attribute 2). */

GL_APICALL void GL_APIENTRY
glNormal3f(
	GLfloat nx,
	GLfloat ny,
	GLfloat nz)
{
	/* The normal. */
	immediate_current(FIXED_OP_NORMAL, FIXED_NORMAL, nx, ny, nz, 1.0f);
}

GL_APICALL void GL_APIENTRY
glNormal3fv(
	const GLfloat *v)
{
	/* Three values. */
	immediate_current(FIXED_OP_NORMAL, FIXED_NORMAL, v[0], v[1], v[2], 1.0f);
}

GL_APICALL void GL_APIENTRY
glNormal3d(
	GLdouble nx,
	GLdouble ny,
	GLdouble nz)
{
	/* As floats. */
	immediate_current(FIXED_OP_NORMAL, FIXED_NORMAL, (GLfloat)nx, (GLfloat)ny, (GLfloat)nz, 1.0f);
}

/* The glTexCoord calls: each sets the current texture coordinates (generic attribute 3). */

GL_APICALL void GL_APIENTRY
glTexCoord4f(
	GLfloat s,
	GLfloat t,
	GLfloat r,
	GLfloat q)
{
	/* The coordinates. */
	immediate_current(FIXED_OP_TEXCOORD, FIXED_TEXCOORD, s, t, r, q);
}

GL_APICALL void GL_APIENTRY
glTexCoord3f(
	GLfloat s,
	GLfloat t,
	GLfloat r)
{
	/* q 1. */
	immediate_current(FIXED_OP_TEXCOORD, FIXED_TEXCOORD, s, t, r, 1.0f);
}

GL_APICALL void GL_APIENTRY
glTexCoord2f(
	GLfloat s,
	GLfloat t)
{
	/* r 0, q 1. */
	immediate_current(FIXED_OP_TEXCOORD, FIXED_TEXCOORD, s, t, 0.0f, 1.0f);
}

GL_APICALL void GL_APIENTRY
glTexCoord2fv(
	const GLfloat *v)
{
	/* Two values. */
	immediate_current(FIXED_OP_TEXCOORD, FIXED_TEXCOORD, v[0], v[1], 0.0f, 1.0f);
}

GL_APICALL void GL_APIENTRY
glTexCoord1f(
	GLfloat s)
{
	/* t and r 0, q 1. */
	immediate_current(FIXED_OP_TEXCOORD, FIXED_TEXCOORD, s, 0.0f, 0.0f, 1.0f);
}

/*
 * Draws a rectangle in the z = 0 plane.
 */
GL_APICALL void GL_APIENTRY
glRectf(
	GLfloat x1,
	GLfloat y1,
	GLfloat x2,
	GLfloat y2)
{
	/* A polygon of its four corners, counter-clockwise from (x1, y1). */
	glBegin(GL_POLYGON);
	glVertex2f(x1, y1);
	glVertex2f(x2, y1);
	glVertex2f(x2, y2);
	glVertex2f(x1, y2);
	glEnd();
}

/*
 * Makes one of the fixed-function arrays read (its generic attribute's).
 */
GL_APICALL void GL_APIENTRY
glEnableClientState(
	GLenum array)
{
	GLuint attribute;

	/* The array's attribute. */
	attribute = immediate_array(array);
	if (attribute == GLES_ATTRIBS) {
		gles_error(gles_context(), GL_INVALID_ENUM);
		return;
	}

	/* Enabled. */
	glEnableVertexAttribArray(attribute);
}

/*
 * Makes one of the fixed-function arrays not read (the current value is).
 */
GL_APICALL void GL_APIENTRY
glDisableClientState(
	GLenum array)
{
	GLuint attribute;

	/* The array's attribute. */
	attribute = immediate_array(array);
	if (attribute == GLES_ATTRIBS) {
		gles_error(gles_context(), GL_INVALID_ENUM);
		return;
	}

	/* Disabled. */
	glDisableVertexAttribArray(attribute);
}

/*
 * Describes the vertex position array.
 */
GL_APICALL void GL_APIENTRY
glVertexPointer(
	GLint size,
	GLenum type,
	GLsizei stride,
	const void *pointer)
{
	/* Generic attribute 0. */
	glVertexAttribPointer(FIXED_POSITION, size, type, GL_FALSE, stride, pointer);
}

/*
 * Describes the colour array (integer colours are fractions of their largest value).
 */
GL_APICALL void GL_APIENTRY
glColorPointer(
	GLint size,
	GLenum type,
	GLsizei stride,
	const void *pointer)
{
	GLboolean normalized;

	/* Generic attribute 1, integers normalized. */
	normalized = GL_TRUE;
	if (type == GL_FLOAT)
		normalized = GL_FALSE;
	glVertexAttribPointer(FIXED_COLOR, size, type, normalized, stride, pointer);
}

/*
 * Describes the normal array (integer normals are fractions of their largest value).
 */
GL_APICALL void GL_APIENTRY
glNormalPointer(
	GLenum type,
	GLsizei stride,
	const void *pointer)
{
	GLboolean normalized;

	/* Generic attribute 2, three components, integers normalized. */
	normalized = GL_TRUE;
	if (type == GL_FLOAT)
		normalized = GL_FALSE;
	glVertexAttribPointer(FIXED_NORMAL, 3, type, normalized, stride, pointer);
}

/*
 * Describes the texture coordinate array.
 */
GL_APICALL void GL_APIENTRY
glTexCoordPointer(
	GLint size,
	GLenum type,
	GLsizei stride,
	const void *pointer)
{
	/* Generic attribute 3. */
	glVertexAttribPointer(FIXED_TEXCOORD, size, type, GL_FALSE, stride, pointer);
}

/*
 * Makes range consecutive display list names; returns the first (0 when
 * the range is not positive).
 */
GL_APICALL GLuint GL_APIENTRY
glGenLists(
	GLsizei range)
{
	struct zegl_context *context;
	struct fixed_state *fixed;
	GLuint first;

	/* The state and a range. */
	fixed = fixed_current(&context);
	if (fixed == NULL)
		return 0U;
	if (range <= 0) {
		gles_error(context, GL_INVALID_VALUE);
		return 0U;
	}

	/* The names counted up (lists are made when compiled). */
	first = fixed->next_list;
	fixed->next_list += (GLuint)range;
	return first;
}

/*
 * Starts compiling a display list (replacing what it had).
 */
GL_APICALL void GL_APIENTRY
glNewList(
	GLuint list,
	GLenum mode)
{
	struct zegl_context *context;
	struct fixed_state *fixed;
	struct fixed_list *target;

	/* The state, a name, a mode, and no list being compiled. */
	fixed = fixed_current(&context);
	if (fixed == NULL)
		return;
	if (list == 0U) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* A mode. */
	if (mode != GL_COMPILE && mode != GL_COMPILE_AND_EXECUTE) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* No list being compiled already. */
	if (fixed->compiling != NULL) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* The list, emptied. */
	target = immediate_list(fixed, list, 1);
	if (target == NULL) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return;
	}

	/* Emptied. */
	target->count = 0U;

	/* Compiling. */
	fixed->compiling = target;
	fixed->compile_mode = mode;
	if (list >= fixed->next_list)
		fixed->next_list = list + 1U;
}

/*
 * Ends compiling the display list.
 */
GL_APICALL void GL_APIENTRY
glEndList(void)
{
	struct zegl_context *context;
	struct fixed_state *fixed;

	/* A list being compiled. */
	fixed = fixed_current(&context);
	if (fixed == NULL)
		return;
	if (fixed->compiling == NULL) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* Done. */
	fixed->compiling = NULL;
	fixed->compile_mode = 0U;
}

/*
 * Makes a display list's calls again (a name that is no list does
 * nothing).
 */
GL_APICALL void GL_APIENTRY
glCallList(
	GLuint list)
{
	struct zegl_context *context;
	struct fixed_state *fixed;
	struct fixed_list *called;
	GLfloat name[1];
	unsigned index;
	int recorded;

	/* The state, and the list being compiled. */
	fixed = fixed_current(&context);
	if (fixed == NULL)
		return;
	name[0] = 0.0f;
	recorded = fixed_record(fixed, FIXED_OP_CALL_LIST, list, 0U, name, 0U);
	if (recorded == FIXED_COMPILE_ONLY)
		return;

	/* The list, and not too deep. */
	called = immediate_list(fixed, list, 0);
	if (called == NULL || fixed->executing >= FIXED_LIST_NESTING)
		return;

	/* Each command, not recorded again. */
	fixed->executing++;
	for (index = 0U; index < called->count; index++)
		immediate_replay(&called->commands[index]);
	fixed->executing--;
}

/*
 * Calls n display lists whose names are in an array of a type.
 */
GL_APICALL void GL_APIENTRY
glCallLists(
	GLsizei n,
	GLenum type,
	const void *lists)
{
	GLsizei index;
	GLuint name;

	/* Each name as its type gives it. */
	for (index = 0; index < n; index++) {
		switch (type) {
		case GL_UNSIGNED_BYTE:
			name = ((const GLubyte *)lists)[index];
			break;
		case GL_BYTE:
			name = (GLuint)((const GLbyte *)lists)[index];
			break;
		case GL_UNSIGNED_SHORT:
			name = ((const GLushort *)lists)[index];
			break;
		case GL_SHORT:
			name = (GLuint)((const GLshort *)lists)[index];
			break;
		case GL_INT:
		case GL_UNSIGNED_INT:
			name = ((const GLuint *)lists)[index];
			break;
		default:
			gles_error(gles_context(), GL_INVALID_ENUM);
			return;
		}

		/* Called. */
		glCallList(name);
	}
}

/*
 * Deletes range display lists from a name.
 */
GL_APICALL void GL_APIENTRY
glDeleteLists(
	GLuint list,
	GLsizei range)
{
	struct zegl_context *context;
	struct fixed_state *fixed;
	struct fixed_list **link;
	struct fixed_list *entry;

	/* The state. */
	fixed = fixed_current(&context);
	if (fixed == NULL || range < 0)
		return;

	/* Each list in the range leaves the list of lists. */
	link = &fixed->lists;
	while (*link != NULL) {
		entry = *link;
		if (entry->name < list || entry->name >= list + (GLuint)range || entry == fixed->compiling) {
			link = &entry->next;
			continue;
		}

		/* Freed. */
		*link = entry->next;
		free(entry->commands);
		free(entry);
	}
}

/*
 * Reports whether a name is a display list.
 */
GL_APICALL GLboolean GL_APIENTRY
glIsList(
	GLuint list)
{
	struct zegl_context *context;
	struct fixed_state *fixed;
	struct fixed_list *entry;

	/* The state and the list. */
	fixed = fixed_current(&context);
	if (fixed == NULL)
		return GL_FALSE;
	entry = immediate_list(fixed, list, 0);
	if (entry == NULL)
		return GL_FALSE;
	return GL_TRUE;
}

/*
 * Records a call into the display list being compiled (unless a list is
 * being made again).  Returns FIXED_COMPILE_ONLY when the call must stop
 * there (GL_COMPILE), FIXED_EXECUTE when it goes on.
 */
int
fixed_record(
	struct fixed_state *fixed,
	enum fixed_op op,
	GLenum e0,
	GLenum e1,
	const GLfloat *f,
	unsigned count)
{
	struct fixed_list *list;
	struct fixed_command *commands;
	unsigned capacity;

	/* Not compiling, or making a list again: the call goes on. */
	list = fixed->compiling;
	if (list == NULL || fixed->executing != 0U)
		return FIXED_EXECUTE;

	/* Room for one more. */
	if (list->count == list->capacity) {
		capacity = list->capacity * 2U;
		if (capacity == 0U)
			capacity = 64U;
		commands = realloc(list->commands, capacity * sizeof(*commands));
		if (commands == NULL)
			return FIXED_EXECUTE;
		list->commands = commands;
		list->capacity = capacity;
	}

	/* The command. */
	memset(&list->commands[list->count], 0, sizeof(list->commands[0]));
	list->commands[list->count].op = op;
	list->commands[list->count].e0 = e0;
	list->commands[list->count].e1 = e1;
	if (count != 0U)
		memcpy(list->commands[list->count].f, f, count * sizeof(GLfloat));
	list->count++;

	/* GL_COMPILE stops the call; GL_COMPILE_AND_EXECUTE lets it go on. */
	if (fixed->compile_mode == GL_COMPILE)
		return FIXED_COMPILE_ONLY;
	return FIXED_EXECUTE;
}

/*
 * Frees every display list.
 */
void
fixed_lists_free(
	struct fixed_state *fixed)
{
	struct fixed_list *entry;

	/* Each list and its commands. */
	while (fixed->lists != NULL) {
		entry = fixed->lists;
		fixed->lists = entry->next;
		free(entry->commands);
		free(entry);
	}
}

/* Keeps a vertex with the current colour, normal and texture coordinates (outside glBegin/glEnd it does nothing). */
static void
immediate_vertex(
	GLfloat x,
	GLfloat y,
	GLfloat z,
	GLfloat w)
{
	struct zegl_context *context;
	struct fixed_state *fixed;
	struct gles_state *state;
	struct fixed_vertex *vertices;
	struct fixed_vertex *vertex;
	GLfloat values[4];
	unsigned capacity;
	int recorded;

	/* The state, and the list being compiled. */
	fixed = fixed_current(&context);
	if (fixed == NULL)
		return;
	values[0] = x;
	values[1] = y;
	values[2] = z;
	values[3] = w;
	recorded = fixed_record(fixed, FIXED_OP_VERTEX, 0U, 0U, values, 4U);
	if (recorded == FIXED_COMPILE_ONLY || !fixed->in_begin)
		return;

	/* Room for one more. */
	if (fixed->vertex_count == fixed->vertex_capacity) {
		capacity = fixed->vertex_capacity * 2U;
		if (capacity == 0U)
			capacity = 256U;
		vertices = realloc(fixed->vertices, capacity * sizeof(*vertices));
		if (vertices == NULL) {
			gles_error(context, GL_OUT_OF_MEMORY);
			return;
		}

		/* Grown. */
		fixed->vertices = vertices;
		fixed->vertex_capacity = capacity;
	}

	/* The vertex with the current values. */
	state = gles_state(context);
	vertex = &fixed->vertices[fixed->vertex_count++];
	memcpy(vertex->position, values, sizeof(values));
	memcpy(vertex->color, state->attribs[FIXED_COLOR].value, 4U * sizeof(GLfloat));
	memcpy(vertex->normal, state->attribs[FIXED_NORMAL].value, 3U * sizeof(GLfloat));
	memcpy(vertex->texcoord, state->attribs[FIXED_TEXCOORD].value, 4U * sizeof(GLfloat));
}

/* Sets a current value (colour, normal or texture coordinates), recording it into the list being compiled. */
static void
immediate_current(
	enum fixed_op op,
	GLuint attribute,
	GLfloat x,
	GLfloat y,
	GLfloat z,
	GLfloat w)
{
	struct zegl_context *context;
	struct fixed_state *fixed;
	GLfloat values[4];
	int recorded;

	/* The state, and the list being compiled. */
	fixed = fixed_current(&context);
	if (fixed == NULL)
		return;
	values[0] = x;
	values[1] = y;
	values[2] = z;
	values[3] = w;
	recorded = fixed_record(fixed, op, 0U, 0U, values, 4U);
	if (recorded == FIXED_COMPILE_ONLY)
		return;

	/* The attribute's current value. */
	glVertexAttrib4f(attribute, x, y, z, w);
}

/* Returns the display list of a name, made when asked; NULL when there is none (or no memory). */
static struct fixed_list *
immediate_list(
	struct fixed_state *fixed,
	GLuint name,
	int make)
{
	struct fixed_list *entry;

	/* The list of that name. */
	for (entry = fixed->lists; entry != NULL; entry = entry->next) {
		if (entry->name == name)
			return entry;
	}

	/* None, or a new one. */
	if (!make)
		return NULL;
	entry = calloc(1U, sizeof(*entry));
	if (entry == NULL)
		return NULL;
	entry->name = name;
	entry->next = fixed->lists;
	fixed->lists = entry;
	return entry;
}

/* Makes a recorded call again. */
static void
immediate_replay(
	const struct fixed_command *command)
{
	const GLfloat *f;

	/* The call of the command. */
	f = command->f;
	switch (command->op) {
	case FIXED_OP_BEGIN:
		glBegin(command->e0);
		break;
	case FIXED_OP_END:
		glEnd();
		break;
	case FIXED_OP_VERTEX:
		glVertex4f(f[0], f[1], f[2], f[3]);
		break;
	case FIXED_OP_COLOR:
		glColor4f(f[0], f[1], f[2], f[3]);
		break;
	case FIXED_OP_NORMAL:
		glNormal3f(f[0], f[1], f[2]);
		break;
	case FIXED_OP_TEXCOORD:
		glTexCoord4f(f[0], f[1], f[2], f[3]);
		break;
	case FIXED_OP_MATERIAL:
		glMaterialfv(command->e0, command->e1, f);
		break;
	case FIXED_OP_LIGHT:
		glLightfv(command->e0, command->e1, f);
		break;
	case FIXED_OP_LIGHT_MODEL:
		glLightModelfv(command->e0, f);
		break;
	case FIXED_OP_COLOR_MATERIAL:
		glColorMaterial(command->e0, command->e1);
		break;
	case FIXED_OP_SHADE_MODEL:
		glShadeModel(command->e0);
		break;
	case FIXED_OP_MATRIX_MODE:
		glMatrixMode(command->e0);
		break;
	case FIXED_OP_LOAD_IDENTITY:
		glLoadIdentity();
		break;
	case FIXED_OP_LOAD_MATRIX:
		glLoadMatrixf(f);
		break;
	case FIXED_OP_MULT_MATRIX:
	case FIXED_OP_TRANSLATE:
	case FIXED_OP_ROTATE:
	case FIXED_OP_SCALE:
	case FIXED_OP_FRUSTUM:
	case FIXED_OP_ORTHO:
		glMultMatrixf(f);
		break;
	case FIXED_OP_PUSH_MATRIX:
		glPushMatrix();
		break;
	case FIXED_OP_POP_MATRIX:
		glPopMatrix();
		break;
	case FIXED_OP_CALL_LIST:
		glCallList(command->e0);
		break;
	default:
		break;
	}
}

/* Returns the generic attribute of a fixed-function array, or GLES_ATTRIBS when the name is not one. */
static GLuint
immediate_array(
	GLenum array)
{
	/* The four arrays. */
	switch (array) {
	case GL_VERTEX_ARRAY:
		return FIXED_POSITION;
	case GL_COLOR_ARRAY:
		return FIXED_COLOR;
	case GL_NORMAL_ARRAY:
		return FIXED_NORMAL;
	case GL_TEXTURE_COORD_ARRAY:
		return FIXED_TEXCOORD;
	default:
		break;
	}

	/* Not one. */
	return GLES_ATTRIBS;
}
