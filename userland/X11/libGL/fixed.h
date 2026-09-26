/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libGL's fixed-function OpenGL 1.x (WS069 p005): the state of a
 * context's matrices, lights, material and immediate mode, and its
 * display lists.  fixed.c keeps the state and draws with it (through the
 * translation's hooks, gles.h); immediate.c has glBegin/glEnd, the
 * vertex arrays and the display lists.
 */

#ifndef LIBGL_FIXED_H
#define LIBGL_FIXED_H

#include "../../base/libglesv2/gles.h"

#include <GL/gl.h>

/* The lights, the depths of the matrix stacks, and how deep lists may call lists. */
#define FIXED_LIGHTS		8U
#define FIXED_MODELVIEW_DEPTH	32U
#define FIXED_OTHER_DEPTH	4U
#define FIXED_LIST_NESTING	64U

/* The fixed-function attributes (generic attributes 0 to 3, as GL's compatibility profile aliases them). */
#define FIXED_POSITION		0U
#define FIXED_COLOR		1U
#define FIXED_NORMAL		2U
#define FIXED_TEXCOORD		3U

/* What fixed_record tells a recordable call: carry on, or stop (only compiling into a list). */
#define FIXED_EXECUTE		0
#define FIXED_COMPILE_ONLY	1

/*
 * The display list commands (each is one call of the public function).
 */
enum fixed_op {
	FIXED_OP_BEGIN = 1,
	FIXED_OP_END,
	FIXED_OP_VERTEX,
	FIXED_OP_COLOR,
	FIXED_OP_NORMAL,
	FIXED_OP_TEXCOORD,
	FIXED_OP_MATERIAL,
	FIXED_OP_LIGHT,
	FIXED_OP_LIGHT_MODEL,
	FIXED_OP_COLOR_MATERIAL,
	FIXED_OP_SHADE_MODEL,
	FIXED_OP_MATRIX_MODE,
	FIXED_OP_LOAD_IDENTITY,
	FIXED_OP_LOAD_MATRIX,
	FIXED_OP_MULT_MATRIX,
	FIXED_OP_TRANSLATE,
	FIXED_OP_ROTATE,
	FIXED_OP_SCALE,
	FIXED_OP_FRUSTUM,
	FIXED_OP_ORTHO,
	FIXED_OP_PUSH_MATRIX,
	FIXED_OP_POP_MATRIX,
	FIXED_OP_CALL_LIST
};

/*
 * One recorded command: its op, up to two enums and sixteen floats.
 */
struct fixed_command {
	enum fixed_op op;
	GLenum e0;
	GLenum e1;
	GLfloat f[16];
};

/*
 * A display list: its commands.
 */
struct fixed_list {
	GLuint name;
	struct fixed_command *commands;
	unsigned count;
	unsigned capacity;
	struct fixed_list *next;
};

/*
 * One vertex of glBegin/glEnd: its position, colour, normal and texture
 * coordinates, interleaved as the draw reads them.
 */
struct fixed_vertex {
	GLfloat position[4];
	GLfloat color[4];
	GLfloat normal[3];
	GLfloat texcoord[4];
};

/*
 * One light.
 */
struct fixed_light {
	GLfloat ambient[4];
	GLfloat diffuse[4];
	GLfloat specular[4];
	GLfloat position[4];
	GLfloat attenuation[4];
};

/*
 * The uniform locations of a fixed-function program.
 */
struct fixed_uniforms {
	GLint mvp;
	GLint modelview;
	GLint normal_matrix;
	GLint texture_matrix;
	GLint scene_ambient;
	GLint material_ambient;
	GLint material_diffuse;
	GLint material_specular;
	GLint material_emission;
	GLint light_position;
	GLint light_ambient;
	GLint light_diffuse;
	GLint light_specular;
	GLint light_attenuation;
	GLint shininess;
	GLint point_size;
	GLint alpha_ref;
	GLint lighting;
	GLint lights;
	GLint color_material;
	GLint normalize;
	GLint texturing;
	GLint alpha_func;
	GLint texture_replace;
	GLint texture;
};

/*
 * A context's fixed-function state.
 */
struct fixed_state {
	/* The capabilities glEnable takes that OpenGL ES does not have. */
	int lighting;
	int light_enabled[FIXED_LIGHTS];
	int color_material;
	int normalize;
	int rescale_normal;
	int texture_2d;
	int alpha_test;
	int fog;
	int line_smooth;
	int point_smooth;
	int polygon_smooth;
	int line_stipple;
	int polygon_stipple;
	int auto_normal;

	/* The matrix mode and the three stacks, with the index of each one's top. */
	GLenum matrix_mode;
	GLfloat modelview[FIXED_MODELVIEW_DEPTH][16];
	GLfloat projection[FIXED_OTHER_DEPTH][16];
	GLfloat texture[FIXED_OTHER_DEPTH][16];
	unsigned modelview_top;
	unsigned projection_top;
	unsigned texture_top;

	/* The lights and the light model's ambient colour. */
	struct fixed_light lights[FIXED_LIGHTS];
	GLfloat scene_ambient[4];

	/* The front material, and which of it the vertex colour stands for. */
	GLfloat material_ambient[4];
	GLfloat material_diffuse[4];
	GLfloat material_specular[4];
	GLfloat material_emission[4];
	GLfloat shininess;
	GLenum color_material_mode;

	/* The shading, the alpha test, the texture environment and the point size. */
	GLenum shade_model;
	GLenum alpha_func;
	GLfloat alpha_ref;
	GLenum texture_env_mode;
	GLfloat point_size;

	/* glBegin/glEnd: whether inside, the mode, and the vertices so far. */
	int in_begin;
	GLenum begin_mode;
	struct fixed_vertex *vertices;
	unsigned vertex_count;
	unsigned vertex_capacity;

	/* The display lists, the one being compiled and how, and how deep glCallList is. */
	struct fixed_list *lists;
	GLuint next_list;
	struct fixed_list *compiling;
	GLenum compile_mode;
	unsigned executing;

	/* The two programs (smooth and flat), made at the first draw, and their uniforms. */
	GLuint programs[2];
	struct fixed_uniforms uniforms[2];
};

/* fixed.c: the calling thread's context and its state (made at the first call), and the hooks. */
struct fixed_state *fixed_current(struct zegl_context **context);
void fixed_install(void);
GLfloat *fixed_matrix(struct fixed_state *fixed);

/* immediate.c: recording into the list being compiled, and freeing the lists. */
int fixed_record(struct fixed_state *fixed, enum fixed_op op, GLenum e0, GLenum e1, const GLfloat *f, unsigned count);
void fixed_lists_free(struct fixed_state *fixed);

#endif
