/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the viewer's model, camera and input owners, which need neither
 * Vulkan nor Wayland and are therefore also built by the host test.
 */

#ifndef MVIEW_MODEL_H
#define MVIEW_MODEL_H

#include <stddef.h>
#include <stdint.h>

/* Bounds that keep a malformed or hostile model file from exhausting memory. */
#define MVIEW_LINE_MAX		1024U
#define MVIEW_NAME_MAX		128U
#define MVIEW_PATH_MAX		512U
#define MVIEW_TEXTURE_MAX	256U
#define MVIEW_MATERIAL_MAX	256U
#define MVIEW_MESH_MAX		256U
#define MVIEW_VERTEX_MAX	4000000U
#define MVIEW_TRIANGLE_MAX	4000000U
#define MVIEW_TEXTURE_SIDE_MAX	4096U
#define MVIEW_ERROR_MAX		256U

/* How a material's texel alpha is interpreted by the draw that uses it. */
enum mview_alpha {
	MVIEW_ALPHA_OPAQUE = 0,
	MVIEW_ALPHA_CUTOUT = 1,
	MVIEW_ALPHA_BLEND = 2
};

/* Whether a material's back faces are discarded by the rasterizer. */
enum mview_cull {
	MVIEW_CULL_BACK = 0,
	MVIEW_CULL_NONE = 1
};

/*
 * One decoded texture image.
 *
 * The pixels are tightly packed 8-bit RGBA rows, top row first, exactly as
 * they are uploaded to the GPU.  The model owns the storage until it is freed.
 */
struct mview_texture {
	char file[MVIEW_PATH_MAX];
	uint32_t width;
	uint32_t height;
	uint8_t *pixels;
};

/* One material: its texture, how alpha is used, face culling and a base colour. */
struct mview_material {
	char name[MVIEW_NAME_MAX];
	int32_t texture;
	enum mview_alpha alpha;
	enum mview_cull cull;
	float color[4];
	uint32_t triangles;
};

/*
 * One vertex as the vertex shader receives it.
 *
 * The layout is the 32-byte binding stride of the pipeline: position, normal,
 * then texture coordinate, all 32-bit floats.
 */
struct mview_vertex {
	float position[3];
	float normal[3];
	float uv[2];
};

/*
 * One contiguous run of the sorted index buffer drawn with one material.
 *
 * Groups are ordered opaque, cutout, then blend, so a frame draws them in
 * array order.
 */
struct mview_group {
	uint32_t material;
	uint32_t first;
	uint32_t count;
};

/*
 * A complete model, as read from model.txt and its textures.
 *
 * Vertices of every mesh share one array; triangle indices are already
 * rebased onto it.  The index array is sorted into draw groups when the text
 * has been read, so the renderer uploads it unchanged.
 */
struct mview_model {
	struct mview_texture *textures;
	uint32_t texture_count;
	struct mview_material *materials;
	uint32_t material_count;
	struct mview_vertex *vertices;
	uint32_t vertex_count;
	uint32_t vertex_capacity;
	uint32_t *triangles;
	uint32_t triangle_count;
	uint32_t triangle_capacity;
	uint32_t *indices;
	struct mview_group *groups;
	uint32_t group_count;
	uint32_t mesh_count;
	float minimum[3];
	float maximum[3];
	char error[MVIEW_ERROR_MAX];
};

/*
 * The orbit camera.
 *
 * Yaw and pitch rotate the model about its bounds centre, pan shifts the
 * rotated model in the view plane, and distance places the viewer on the
 * view axis.  The initial values are kept so that a reset restores exactly
 * the first frame's parameters.
 */
struct mview_camera {
	float center[3];
	float radius;
	float yaw;
	float pitch;
	float distance;
	float pan[2];
	float initial_distance;
};

/*
 * The per-draw constants pushed to both shader stages.
 *
 * The clip transform and the normal rotation are stored as columns so the
 * shaders need no matrix type; the colour is the material's base colour.
 */
struct mview_push {
	float clip[16];
	float normal[12];
	float color[4];
};

/* The lights of the per-pixel shading: one directional, two points. */
#define MVIEW_LIGHT_COUNT	3U

/*
 * The scene block of the per-pixel shading (--shading=pixel), laid out as
 * the shaders' std140 uniform block reads it: the model, view and projection
 * matrices and the normal matrix, column-major; the ambient light; and per
 * light its view-space position (w 1) or direction towards it (w 0), its
 * colour with the specular strength in alpha, and its constant, linear and
 * quadratic attenuation with the shininess in w.
 */
struct mview_scene {
	float model[16];
	float view[16];
	float projection[16];
	float normal[16];
	float ambient[4];
	float light_position[MVIEW_LIGHT_COUNT][4];
	float light_color[MVIEW_LIGHT_COUNT][4];
	float light_factors[MVIEW_LIGHT_COUNT][4];
};

/*
 * Pointer and keyboard state that turns input events into camera changes.
 *
 * The window's listeners feed it; the main loop reads the changed and quit
 * flags once per frame.
 */
struct mview_input {
	struct mview_camera *camera;
	const char *token;
	uint32_t height;
	double x;
	double y;
	int have_position;
	int left;
	int right;
	int middle;
	int changed;
	uint32_t motion_events;
	double motion_dx;
	double motion_dy;
	int quit;
};

/* The model owns every array it reads; free releases them after any failure. */
int mview_model_load(struct mview_model *model, const char *directory);
void mview_model_free(struct mview_model *model);

/* The camera is plain arithmetic, shared by the renderer and the input handler. */
void mview_camera_fit(struct mview_camera *camera, const float minimum[3], const float maximum[3], uint32_t width, uint32_t height);
void mview_camera_reset(struct mview_camera *camera);
void mview_camera_orbit(struct mview_camera *camera, float yaw, float pitch);
void mview_camera_pan(struct mview_camera *camera, float dx, float dy, uint32_t height);
void mview_camera_zoom(struct mview_camera *camera, int notches);
void mview_camera_push(const struct mview_camera *camera, uint32_t width, uint32_t height, struct mview_push *push);
void mview_camera_scene(const struct mview_camera *camera, uint32_t width, uint32_t height, struct mview_scene *scene);

/* Input translation applies each event to the camera and logs the new view. */
void mview_input_init(struct mview_input *input, struct mview_camera *camera, const char *token, uint32_t height);
void mview_input_motion(struct mview_input *input, double x, double y);
void mview_input_position(struct mview_input *input, double x, double y);
void mview_input_button(struct mview_input *input, uint32_t button, int pressed);
void mview_input_axis(struct mview_input *input, uint32_t axis, double value, int32_t discrete);
void mview_input_key(struct mview_input *input, uint32_t key, int pressed);
void mview_input_release_all(struct mview_input *input);
void mview_input_flush(struct mview_input *input);

#endif
