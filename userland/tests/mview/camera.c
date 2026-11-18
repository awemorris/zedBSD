/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Computes the orbit camera and the clip and normal transforms it implies.
 */

#include "model.h"

#include <math.h>
#include <string.h>

/* The vertical field of view, in degrees, of every projection. */
#define CAMERA_FOV_DEGREES	45.0f

/* The margin kept around the bounding sphere by the initial fit. */
#define CAMERA_FIT_MARGIN	1.05f

/* Pitch stops short of the poles so the view never flips over. */
#define CAMERA_PITCH_LIMIT	89.0f

/* One wheel notch or zoom key scales the viewing distance by this factor. */
#define CAMERA_ZOOM_STEP	1.1f

/* The viewer may come this close, relative to the model radius... */
#define CAMERA_NEAR_LIMIT	0.05f

/* ...and go this far away. */
#define CAMERA_FAR_LIMIT	20.0f

/*
 * What one view of the camera is made of: the rotation (column-major), the
 * translation that follows it, and the perspective and depth terms.
 *
 * It lives on the stack of the function turning a camera into the push
 * constants or the scene block, so both see the very same numbers.
 */
struct camera_frame {
	float rotation[9];
	float translation[3];
	float focal;
	float aspect;
	float depth_scale;
	float depth_offset;
};

static float camera_radians(float degrees);
static void camera_rotation(const struct mview_camera *camera, float rotation[9]);
static void camera_frame(const struct mview_camera *camera, uint32_t width, uint32_t height, struct camera_frame *frame);
static void camera_light(struct mview_scene *scene, uint32_t index, const float position[4], const float color[4], const float factors[4]);

/*
 * Places the camera so that the whole bounding sphere is visible.
 *
 * The view starts from the front: the model's +Z axis points at the viewer.
 * The fitted distance is also the one a reset returns to.
 */
void
mview_camera_fit(
	struct mview_camera *camera,
	const float minimum[3],
	const float maximum[3],
	uint32_t width,
	uint32_t height)
{
	float extent[3];
	float half_angle;
	float horizontal;
	float aspect;
	float vertical_distance;
	float horizontal_distance;
	uint32_t axis;

	/* The orbit turns about the centre of the axis-aligned bounds. */
	memset(camera, 0, sizeof(*camera));
	for (axis = 0U; axis < 3U; axis++) {
		camera->center[axis] = 0.5f * (minimum[axis] + maximum[axis]);
		extent[axis] = maximum[axis] - minimum[axis];
	}

	/* The bounding sphere encloses the box; an empty box still gets a usable size. */
	camera->radius = 0.5f * sqrtf(extent[0] * extent[0] + extent[1] * extent[1] + extent[2] * extent[2]);
	if (!(camera->radius > 1.0e-6f))
		camera->radius = 1.0f;

	/* A degenerate window is treated as square rather than dividing by zero. */
	aspect = 1.0f;
	if (width != 0U && height != 0U)
		aspect = (float)width / (float)height;

	/* The sphere must fit both the vertical and the horizontal field of view. */
	half_angle = camera_radians(0.5f * CAMERA_FOV_DEGREES);
	horizontal = atanf(tanf(half_angle) * aspect);
	vertical_distance = camera->radius / sinf(half_angle);
	horizontal_distance = camera->radius / sinf(horizontal);

	/* The narrower of the two directions decides how far back the viewer stands. */
	camera->initial_distance = vertical_distance;
	if (horizontal_distance > camera->initial_distance)
		camera->initial_distance = horizontal_distance;

	camera->initial_distance *= CAMERA_FIT_MARGIN;

	/* The first frame and every reset use exactly these parameters. */
	mview_camera_reset(camera);

	/* Succeeded: the camera frames the whole model from the front. */
	return;
}

/*
 * Restores the initial front view.
 *
 * Every parameter is assigned from a constant or from the fitted distance, so
 * the frame after a reset is identical to the first frame.
 */
void
mview_camera_reset(
	struct mview_camera *camera)
{
	/* The front view has no rotation and no pan. */
	camera->yaw = 0.0f;
	camera->pitch = 0.0f;
	camera->pan[0] = 0.0f;
	camera->pan[1] = 0.0f;

	/* The fitted distance shows the whole model again. */
	camera->distance = camera->initial_distance;

	/* Succeeded: the camera is back at its initial view. */
	return;
}

/*
 * Turns the model by the given yaw and pitch, in degrees.
 *
 * Yaw wraps into (-180, 180] so logged values stay small; pitch is clamped.
 */
void
mview_camera_orbit(
	struct mview_camera *camera,
	float yaw,
	float pitch)
{
	/* Accumulates the turn about the vertical axis and keeps it in one revolution. */
	camera->yaw += yaw;
	while (camera->yaw > 180.0f)
		camera->yaw -= 360.0f;

	/* The other side of the wrap mirrors the first. */
	while (camera->yaw <= -180.0f)
		camera->yaw += 360.0f;

	/* Accumulates the tilt and stops it short of looking straight down or up. */
	camera->pitch += pitch;
	if (camera->pitch > CAMERA_PITCH_LIMIT)
		camera->pitch = CAMERA_PITCH_LIMIT;

	/* The lower limit mirrors the upper one. */
	if (camera->pitch < -CAMERA_PITCH_LIMIT)
		camera->pitch = -CAMERA_PITCH_LIMIT;

	/* Succeeded: the rotation holds the accumulated, bounded angles. */
	return;
}

/*
 * Shifts the model in the view plane by a pointer movement in pixels.
 *
 * One pixel moves the model by one pixel's worth of the view-plane height at
 * the orbit centre, so the point under the cursor follows the cursor.
 */
void
mview_camera_pan(
	struct mview_camera *camera,
	float dx,
	float dy,
	uint32_t height)
{
	float visible;
	float scale;

	/* A zero-height window has no pixel size and cannot pan. */
	if (height == 0U)
		return;

	/* The height of the view plane through the orbit centre, in model units. */
	visible = 2.0f * camera->distance * tanf(camera_radians(0.5f * CAMERA_FOV_DEGREES));
	scale = visible / (float)height;

	/* Screen x grows rightward like view x; screen y grows downward, unlike view y. */
	camera->pan[0] += dx * scale;
	camera->pan[1] -= dy * scale;

	/* Succeeded: the pan holds the accumulated view-plane shift. */
	return;
}

/*
 * Moves the viewer by whole zoom steps: positive notches move away.
 *
 * The distance is clamped relative to the model radius so the model can
 * neither swallow the view nor vanish.
 */
void
mview_camera_zoom(
	struct mview_camera *camera,
	int notches)
{
	float limit;

	/* Moving away multiplies the distance once per step. */
	while (notches > 0) {
		camera->distance *= CAMERA_ZOOM_STEP;
		notches--;
	}

	/* Moving closer divides it once per step. */
	while (notches < 0) {
		camera->distance /= CAMERA_ZOOM_STEP;
		notches++;
	}

	/* The viewer never enters the model's immediate surface. */
	limit = camera->radius * CAMERA_NEAR_LIMIT;
	if (camera->distance < limit)
		camera->distance = limit;

	/* The model never shrinks to a dot. */
	limit = camera->radius * CAMERA_FAR_LIMIT;
	if (camera->distance > limit)
		camera->distance = limit;

	/* Succeeded: the distance holds the bounded zoom. */
	return;
}

/*
 * Fills the clip and normal transforms for the current view.
 *
 * A model point p is moved to view space as R (p - centre) + (pan, -distance),
 * then projected with a right-handed perspective into Vulkan clip space, whose
 * y axis points down and whose depth runs from 0 to 1.  All matrices are
 * written column by column.
 */
void
mview_camera_push(
	const struct mview_camera *camera,
	uint32_t width,
	uint32_t height,
	struct mview_push *push)
{
	struct camera_frame frame;
	uint32_t column;
	uint32_t row;

	/* The rotation, translation and projection terms of the view. */
	camera_frame(camera, width, height, &frame);

	/*
	 * Composes projection and view: each clip column is the projection of one
	 * view-matrix column.  Clip w is the negated view z.
	 */
	memset(push->clip, 0, sizeof(push->clip));
	for (column = 0U; column < 3U; column++) {
		push->clip[column * 4U + 0U] = frame.focal / frame.aspect * frame.rotation[column * 3U + 0U];
		push->clip[column * 4U + 1U] = -frame.focal * frame.rotation[column * 3U + 1U];
		push->clip[column * 4U + 2U] = frame.depth_scale * frame.rotation[column * 3U + 2U];
		push->clip[column * 4U + 3U] = -frame.rotation[column * 3U + 2U];
	}

	/* The fourth column projects the translation, which carries a w of one. */
	push->clip[12] = frame.focal / frame.aspect * frame.translation[0];
	push->clip[13] = -frame.focal * frame.translation[1];
	push->clip[14] = frame.depth_scale * frame.translation[2] + frame.depth_offset;
	push->clip[15] = -frame.translation[2];

	/* Normals only turn with the model; each column is padded to four floats. */
	memset(push->normal, 0, sizeof(push->normal));
	for (column = 0U; column < 3U; column++) {
		for (row = 0U; row < 3U; row++)
			push->normal[column * 4U + row] = frame.rotation[column * 3U + row];
	}

	/* Succeeded: the push block holds the transforms of the current view. */
	return;
}

/*
 * Fills the scene block of the per-pixel shading from the camera.
 *
 * The model matrix turns the model about its centre (the rotation, then the
 * rotated centre moved to the origin), the view matrix moves it to the pan
 * offset in front of the viewer, and the projection is the one the push
 * constants compose with them: projection * view * model is the push block's
 * clip transform.  The normal matrix is the rotation.  The lights sit at
 * fixed places of the view, so a reset restores the first frame exactly:
 * a directional light from the upper right front, a warm point light to the
 * upper left and a cool one to the lower right, both in front of the model
 * and fading with the distance in units of the model's radius.
 */
void
mview_camera_scene(
	const struct mview_camera *camera,
	uint32_t width,
	uint32_t height,
	struct mview_scene *scene)
{
	static const float ambient[4] = { 0.16f, 0.16f, 0.18f, 0.0f };
	static const float key_color[4] = { 0.55f, 0.55f, 0.55f, 0.35f };
	static const float warm_color[4] = { 0.95f, 0.62f, 0.35f, 0.5f };
	static const float cool_color[4] = { 0.3f, 0.45f, 0.95f, 0.4f };
	struct camera_frame frame;
	float position[4];
	float factors[4];
	float radius;
	uint32_t column;
	uint32_t row;

	/* The rotation, translation and projection terms of the view. */
	camera_frame(camera, width, height, &frame);
	memset(scene, 0, sizeof(*scene));

	/* The model matrix: the rotation, and the rotated centre moved to the origin. */
	for (column = 0U; column < 3U; column++) {
		for (row = 0U; row < 3U; row++)
			scene->model[column * 4U + row] = frame.rotation[column * 3U + row];
	}

	for (row = 0U; row < 3U; row++) {
		scene->model[12U + row] = 0.0f;
		for (column = 0U; column < 3U; column++)
			scene->model[12U + row] -= frame.rotation[column * 3U + row] * camera->center[column];
	}

	scene->model[15] = 1.0f;

	/* The view matrix: the pan offset in front of the viewer. */
	scene->view[0] = 1.0f;
	scene->view[5] = 1.0f;
	scene->view[10] = 1.0f;
	scene->view[12] = camera->pan[0];
	scene->view[13] = camera->pan[1];
	scene->view[14] = -camera->distance;
	scene->view[15] = 1.0f;

	/* The projection: the perspective scale (y flipped) and the depth mapping, w the negated view z. */
	scene->projection[0] = frame.focal / frame.aspect;
	scene->projection[5] = -frame.focal;
	scene->projection[10] = frame.depth_scale;
	scene->projection[11] = -1.0f;
	scene->projection[14] = frame.depth_offset;

	/* The normal matrix: normals only turn with the model. */
	for (column = 0U; column < 3U; column++) {
		for (row = 0U; row < 3U; row++)
			scene->normal[column * 4U + row] = frame.rotation[column * 3U + row];
	}

	scene->normal[15] = 1.0f;

	/* The ambient light. */
	memcpy(scene->ambient, ambient, sizeof(scene->ambient));

	/* The lights are placed and faded in units of the model's radius; a model of no size counts as one. */
	radius = camera->radius;
	if (radius <= 0.0f)
		radius = 1.0f;

	/* The directional light, from the direction the per-vertex shading uses. */
	position[0] = 0.267261f;
	position[1] = 0.534522f;
	position[2] = 0.801784f;
	position[3] = 0.0f;
	factors[0] = 1.0f;
	factors[1] = 0.0f;
	factors[2] = 0.0f;
	factors[3] = 24.0f;
	camera_light(scene, 0U, position, key_color, factors);

	/* The warm point light, upper left and in front of the model. */
	position[0] = -1.2f * radius;
	position[1] = 0.9f * radius;
	position[2] = -camera->distance + 1.5f * radius;
	position[3] = 1.0f;
	factors[0] = 1.0f;
	factors[1] = 0.35f / radius;
	factors[2] = 0.25f / (radius * radius);
	factors[3] = 32.0f;
	camera_light(scene, 1U, position, warm_color, factors);

	/* The cool point light, lower right and in front of the model. */
	position[0] = 1.4f * radius;
	position[1] = -0.4f * radius;
	position[2] = -camera->distance + 1.2f * radius;
	position[3] = 1.0f;
	factors[0] = 1.0f;
	factors[1] = 0.3f / radius;
	factors[2] = 0.2f / (radius * radius);
	factors[3] = 16.0f;
	camera_light(scene, 2U, position, cool_color, factors);

	/* Succeeded: the scene block holds the transforms and lights of the current view. */
	return;
}

/*
 * Computes the rotation, the translation and the projection terms of the
 * camera's current view, in the order the push constants always have.
 */
static void
camera_frame(
	const struct mview_camera *camera,
	uint32_t width,
	uint32_t height,
	struct camera_frame *frame)
{
	float near_plane;
	float far_plane;
	uint32_t column;
	uint32_t row;

	/* The rotation is the combined pitch and yaw, column-major. */
	camera_rotation(camera, frame->rotation);

	/* The translation carries the rotated centre to the pan offset in front of the viewer. */
	for (row = 0U; row < 3U; row++) {
		frame->translation[row] = 0.0f;
		for (column = 0U; column < 3U; column++)
			frame->translation[row] -= frame->rotation[column * 3U + row] * camera->center[column];
	}

	frame->translation[0] += camera->pan[0];
	frame->translation[1] += camera->pan[1];
	frame->translation[2] -= camera->distance;

	/* A degenerate window is treated as square rather than dividing by zero. */
	frame->aspect = 1.0f;
	if (width != 0U && height != 0U)
		frame->aspect = (float)width / (float)height;

	/*
	 * The depth range brackets the bounding sphere; a pan cannot move the
	 * model along the view axis, so the sphere stays inside it.
	 */
	far_plane = camera->distance + 2.0f * camera->radius;
	near_plane = camera->distance - 2.0f * camera->radius;
	if (near_plane < camera->distance * 0.01f)
		near_plane = camera->distance * 0.01f;

	/* The perspective scale and the depth mapping onto Vulkan's 0..1 range. */
	frame->focal = 1.0f / tanf(camera_radians(0.5f * CAMERA_FOV_DEGREES));
	frame->depth_scale = far_plane / (near_plane - far_plane);
	frame->depth_offset = near_plane * far_plane / (near_plane - far_plane);
}

/* Stores one light of the scene block. */
static void
camera_light(
	struct mview_scene *scene,
	uint32_t index,
	const float position[4],
	const float color[4],
	const float factors[4])
{
	/* The position or direction, the colour and specular strength, the attenuation and shininess. */
	memcpy(scene->light_position[index], position, sizeof(scene->light_position[index]));
	memcpy(scene->light_color[index], color, sizeof(scene->light_color[index]));
	memcpy(scene->light_factors[index], factors, sizeof(scene->light_factors[index]));
}

/* Converts an angle from degrees to radians. */
static float
camera_radians(
	float degrees)
{
	/* Reports the angle in radians. */
	return degrees * 3.14159265358979f / 180.0f;
}

/* Builds the column-major rotation Rx(pitch) Ry(yaw). */
static void
camera_rotation(
	const struct mview_camera *camera,
	float rotation[9])
{
	float cy;
	float sy;
	float cp;
	float sp;

	/* Samples the two angles once. */
	cy = cosf(camera_radians(camera->yaw));
	sy = sinf(camera_radians(camera->yaw));
	cp = cosf(camera_radians(camera->pitch));
	sp = sinf(camera_radians(camera->pitch));

	/* The model's x axis turns toward -z as yaw grows. */
	rotation[0] = cy;
	rotation[1] = sp * sy;
	rotation[2] = -cp * sy;

	/* The model's y axis tilts toward the viewer as pitch grows. */
	rotation[3] = 0.0f;
	rotation[4] = cp;
	rotation[5] = sp;

	/* The model's z axis, its front, turns toward +x with yaw and -y with pitch. */
	rotation[6] = sy;
	rotation[7] = -sp * cy;
	rotation[8] = cp * cy;

	/* Succeeded: the rotation holds both angles. */
	return;
}
