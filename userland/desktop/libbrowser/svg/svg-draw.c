/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Pictures drawn through a sink as device-space fills.
 */

#include "svg/svg-draw.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*
 * How far a clip has got in being described to the sink: not yet, in the
 * middle of it (its parent is being described first, which is how a cycle
 * of parents is found), or done.
 */
enum draw_clip_state {
	DRAW_CLIP_NEW = 0,
	DRAW_CLIP_BUSY = 1,
	DRAW_CLIP_DONE = 2
};

/*
 * The state of one drawing: the arguments, what has been described to the
 * sink, the groups the sink has been told to open, and the polylines and
 * the gradient the commands are built into (kept across the commands so
 * their arrays are reused).
 *
 * sink_failed is set when a sink function returned an error: the drawing
 * then ends at once and leaves the groups open, as the sink's own error
 * asks.
 */
struct draw {
	const struct svg_picture *picture;
	const struct vec_matrix *placement;
	float tolerance;
	const struct svg_sink *sink;
	unsigned char *clip_state;
	size_t open_groups;
	int sink_failed;
	struct vec_lines lines;
	struct vec_lines center;
	struct vec_gradient gradient;
};

static int draw_commands(struct draw *draw);
static int draw_group_open(struct draw *draw, const struct svg_command *command);
static int draw_group_close(struct draw *draw);
static int draw_groups_close(struct draw *draw);
static int draw_shape(struct draw *draw, const struct svg_command *command);
static int draw_paint_make(struct draw *draw, const struct svg_command *command, const struct vec_matrix *matrix, struct svg_draw_paint *paint, int *usable);
static int draw_lines_fill(struct draw *draw, const struct svg_command *command, const struct vec_matrix *matrix);
static int draw_lines_stroke(struct draw *draw, const struct svg_command *command, const struct vec_matrix *matrix);
static int draw_clip_describe(struct draw *draw, int index);
static int draw_clip_parts(struct draw *draw, const struct svg_clip *clip, struct vec_lines *lines, struct svg_draw_clip_part *parts, size_t *part_count);
static float draw_unit(float value);

/*
 * Draws a picture through a sink.
 *
 * Values referenced: SVG 2 §8.5 (a point mapped by the composition of the
 * matrices from the element out to the viewport), §13.4.2 (fill rules),
 * §13.5.7 (a stroke is the outline of the center line in user space,
 * mapped by the element's matrix), §14.2 (a gradient's coordinates are
 * in its own space, which the inverse of the shape's matrix reaches from
 * device pixels), CSS Masking 1 §6.1 (a clip is the union of its parts,
 * met with its parent).
 *
 * Choices where the header is silent: a command with an index outside the
 * picture's arrays, an unknown kind or a GROUP_END with no group open
 * means a malformed picture and ends the drawing with EINVAL; a command
 * whose paint is NONE, a gradient with no stops, or whose polylines have
 * no contour is skipped; a clip part that cannot be flattened (a point
 * beyond the coordinate range) is left out of the clip described to the
 * sink, so the clip only shrinks; an opacity or alpha that is not a
 * number is 0 and one outside 0 to 1 is clamped; groups still open at the
 * end are closed; when ending them fails after another error, that error
 * is the one returned.
 */
int
svg_picture_draw(
	const struct svg_picture *picture,
	const struct vec_matrix *placement,
	float tolerance,
	const struct svg_sink *sink)
{
	struct draw draw;
	int tolerance_finite;
	int placement_valid;
	int error;
	int close_error;

	/* Refuses missing arguments and a sink without all four functions. */
	if (picture == NULL || placement == NULL || sink == NULL)
		return EINVAL;
	if (sink->clip == NULL || sink->fill == NULL)
		return EINVAL;
	if (sink->group == NULL || sink->group_end == NULL)
		return EINVAL;

	/* Refuses a placement that is not valid. */
	placement_valid = vec_matrix_valid(placement);
	if (!placement_valid)
		return EINVAL;

	/* Refuses a tolerance that is not a finite number of at least VEC_TOLERANCE_MIN. */
	tolerance_finite = vec_finite(tolerance);
	if (!tolerance_finite)
		return EINVAL;
	if (tolerance < VEC_TOLERANCE_MIN)
		return EINVAL;

	/* Refuses a picture whose arrays are missing for their counts. */
	if (picture->command_count != 0 && picture->commands == NULL)
		return EINVAL;

	/* Starts the state of the drawing. */
	memset(&draw, 0, sizeof(draw));
	draw.picture = picture;
	draw.placement = placement;
	draw.tolerance = tolerance;
	draw.sink = sink;
	vec_lines_init(&draw.lines);
	vec_lines_init(&draw.center);

	/* Allocates the table of which clips the sink has been told of. */
	if (picture->clip_count != 0) {
		draw.clip_state = calloc(picture->clip_count, sizeof(*draw.clip_state));
		if (draw.clip_state == NULL)
			return ENOMEM;
	}

	/* Draws the commands in order. */
	error = draw_commands(&draw);

	/* Ends the groups still open, unless the sink's own error ended the drawing. */
	if (!draw.sink_failed) {
		close_error = draw_groups_close(&draw);
		if (error == 0)
			error = close_error;
	}

	/* Frees what the drawing held. */
	vec_lines_release(&draw.lines);
	vec_lines_release(&draw.center);
	free(draw.clip_state);

	/* Reports why the drawing ended early. */
	if (error != 0)
		return error;

	/* Succeeded: every command was passed to the sink or skipped. */
	return 0;
}

/*
 * Passes each command to the sink in order; stops at the first error that
 * ends the drawing.
 */
static int
draw_commands(
	struct draw *draw)
{
	const struct svg_command *command;
	size_t index;
	int error;

	/* Draws every command of the picture. */
	for (index = 0;
	     index < draw->picture->command_count;
	     index++) {
		command = &draw->picture->commands[index];

		/* Hands the command to the step of its kind. */
		switch (command->kind) {
		case SVG_COMMAND_GROUP:
			error = draw_group_open(draw, command);
			break;
		case SVG_COMMAND_GROUP_END:
			error = draw_group_close(draw);
			break;
		case SVG_COMMAND_FILL:
		case SVG_COMMAND_STROKE:
			error = draw_shape(draw, command);
			break;
		default:
			/* A kind this drawer does not know is a malformed picture. */
			error = EINVAL;
			break;
		}
		if (error != 0)
			return error;
	}

	/* Succeeded: no command ended the drawing. */
	return 0;
}

/*
 * Starts a group in the sink with the command's opacity.
 */
static int
draw_group_open(
	struct draw *draw,
	const struct svg_command *command)
{
	float opacity;
	int error;

	/* Passes the opacity, as a number in 0 to 1, to the sink. */
	opacity = draw_unit(command->opacity);
	error = draw->sink->group(draw->sink->context, opacity);
	if (error != 0) {
		draw->sink_failed = 1;
		return error;
	}

	/* The group is open until a GROUP_END (or the end of the drawing) closes it. */
	draw->open_groups++;

	/* Succeeded: the sink opened the group. */
	return 0;
}

/*
 * Ends the innermost group in the sink.
 */
static int
draw_group_close(
	struct draw *draw)
{
	int error;

	/* Refuses a GROUP_END that has no group to end. */
	if (draw->open_groups == 0)
		return EINVAL;

	/* Ends the group in the sink. */
	draw->open_groups--;
	error = draw->sink->group_end(draw->sink->context);
	if (error != 0) {
		draw->sink_failed = 1;
		return error;
	}

	/* Succeeded: the sink closed the group. */
	return 0;
}

/*
 * Ends every group still open, innermost first; stops at the sink's first
 * error.
 */
static int
draw_groups_close(
	struct draw *draw)
{
	int error;

	/* Ends the groups one by one. */
	while (draw->open_groups != 0) {
		error = draw_group_close(draw);
		if (error != 0)
			return error;
	}

	/* Succeeded: no group is open. */
	return 0;
}

/*
 * Turns a fill or stroke command into device-space polylines and hands
 * them to the sink with the paint and the clip.  A command that cannot be
 * drawn (nothing to paint, a map out of range, a size limit) is skipped
 * and the drawing goes on.
 */
static int
draw_shape(
	struct draw *draw,
	const struct svg_command *command)
{
	struct vec_matrix matrix;
	struct svg_draw_paint paint;
	enum vec_fill_rule rule;
	size_t clip_count;
	int matrix_valid;
	int usable;
	int error;

	/* Refuses a command that names a path or a clip the picture does not have. */
	clip_count = draw->picture->clip_count;
	if (command->path >= draw->picture->path_count)
		return EINVAL;
	if (command->clip < -1)
		return EINVAL;
	if (command->clip >= 0 && (size_t)command->clip >= clip_count)
		return EINVAL;

	/* Maps the command's user space to device pixels: its matrix, then the placement. */
	vec_matrix_multiply(&matrix, &command->matrix, draw->placement);
	matrix_valid = vec_matrix_valid(&matrix);
	if (!matrix_valid)
		return 0;

	/* Makes the paint the sink sees; a command with nothing to paint is skipped. */
	error = draw_paint_make(draw, command, &matrix, &paint, &usable);
	if (error != 0)
		return error;
	if (!usable)
		return 0;

	/* Builds the device-space polylines of the fill or the stroke. */
	vec_lines_reset(&draw->lines);
	if (command->kind == SVG_COMMAND_FILL) {
		rule = command->rule;
		error = draw_lines_fill(draw, command, &matrix);
	} else {
		rule = VEC_FILL_NONZERO;
		error = draw_lines_stroke(draw, command, &matrix);
	}
	if (error != 0)
		return error;

	/* Skips a command that has nothing to fill. */
	if (draw->lines.contour_count == 0)
		return 0;

	/* Describes the clip to the sink before the first fill that uses it. */
	if (command->clip >= 0) {
		error = draw_clip_describe(draw, command->clip);
		if (error != 0)
			return error;
	}

	/* Hands the polylines to the sink. */
	error = draw->sink->fill(draw->sink->context, &draw->lines, rule, &paint, command->clip);
	if (error != 0) {
		draw->sink_failed = 1;
		return error;
	}

	/* Succeeded: the sink took the fill. */
	return 0;
}

/*
 * Makes the paint the sink sees from the command's paint.  A gradient's
 * to_gradient maps the command's user space to the gradient's, so it is
 * composed after the inverse of the command's device matrix: the result
 * maps device pixels.  *usable is 0 for a command with nothing to paint:
 * no paint, a gradient whose matrix has no inverse or whose stops are
 * missing.
 */
static int
draw_paint_make(
	struct draw *draw,
	const struct svg_command *command,
	const struct vec_matrix *matrix,
	struct svg_draw_paint *paint,
	int *usable)
{
	struct vec_matrix inverse;
	struct vec_matrix composed;
	int composed_valid;
	int error;

	/* Starts with a paint that cannot be used, and fills in what is common to the kinds. */
	*usable = 0;
	memset(paint, 0, sizeof(*paint));
	paint->kind = command->paint.kind;
	paint->color = command->paint.color;
	paint->gradient = NULL;
	paint->alpha = draw_unit(command->paint.opacity);

	/* Accepts a color as it is. */
	if (command->paint.kind == SVG_SERVER_COLOR) {
		*usable = 1;
		return 0;
	}

	/* Skips anything that is neither a color nor a gradient (a command with nothing to paint). */
	if (command->paint.kind != SVG_SERVER_GRADIENT)
		return 0;

	/* Skips a gradient with no stops to read. */
	if (command->paint.gradient.stops == NULL || command->paint.gradient.count == 0)
		return 0;

	/* Finds the map from device pixels back to the command's user space; none means nothing is painted. */
	error = vec_matrix_invert(&inverse, matrix);
	if (error != 0)
		return 0;

	/* Composes it with the gradient's map: device pixels to user space, then user space to gradient space. */
	vec_matrix_multiply(&composed, &inverse, &command->paint.gradient.to_gradient);
	composed_valid = vec_matrix_valid(&composed);
	if (!composed_valid)
		return 0;

	/* Keeps a copy of the gradient with the composed map for the sink's call. */
	draw->gradient = command->paint.gradient;
	draw->gradient.to_gradient = composed;
	paint->gradient = &draw->gradient;
	*usable = 1;

	/* Succeeded: the paint is ready. */
	return 0;
}

/*
 * Flattens a fill command's path with the command's device matrix into
 * the drawing's polylines.  An error that only means this command cannot
 * be drawn (a point out of range, a size limit) skips it.
 */
static int
draw_lines_fill(
	struct draw *draw,
	const struct svg_command *command,
	const struct vec_matrix *matrix)
{
	struct vec_path_view view;
	int error;

	/* Flattens the path, each point mapped to device pixels. */
	vec_path_view_of(&draw->picture->paths[command->path], &view);
	error = vec_flatten(&view, matrix, draw->tolerance, &draw->lines);
	if (error == ENOMEM)
		return ENOMEM;

	/* An error that is not memory leaves empty polylines, which the caller skips. */
	if (error != 0)
		vec_lines_reset(&draw->lines);

	/* Succeeded: the polylines are filled or the command is to be skipped. */
	return 0;
}

/*
 * Flattens a stroke command's path in user space and strokes it into the
 * drawing's polylines mapped to device pixels.  The tolerance in user
 * space is the device one over the largest stretch of the matrix, with a
 * floor, so a large scale gets a fine flattening and a viewBox far
 * smaller than its viewport does not lose its lines.
 */
static int
draw_lines_stroke(
	struct draw *draw,
	const struct svg_command *command,
	const struct vec_matrix *matrix)
{
	struct vec_path_view view;
	struct vec_stroke_style style;
	float scale;
	float user_tolerance;
	int error;

	/* Draws nothing for a matrix that stretches nothing (a scale of 0). */
	scale = vec_matrix_scale_max(matrix);
	if (!(scale > 0.0f))
		return 0;

	/* Finds the tolerance in user space: the device one divided by the scale, within the usable range. */
	user_tolerance = draw->tolerance / scale;
	if (!(user_tolerance >= VEC_TOLERANCE_MIN))
		user_tolerance = VEC_TOLERANCE_MIN;
	if (!(user_tolerance <= VEC_COORD_MAX))
		user_tolerance = VEC_COORD_MAX;

	/* Flattens the center line in user space. */
	vec_lines_reset(&draw->center);
	vec_path_view_of(&draw->picture->paths[command->path], &view);
	error = vec_flatten(&view, NULL, user_tolerance, &draw->center);
	if (error == ENOMEM)
		return ENOMEM;
	if (error != 0)
		return 0;

	/* Strokes it with the command's style and the device tolerance, mapped to device pixels. */
	style = command->stroke;
	style.tolerance = draw->tolerance;
	error = vec_stroke(&draw->center, &style, matrix, &draw->lines);
	if (error == ENOMEM)
		return ENOMEM;

	/* An error that is not memory leaves empty polylines, which the caller skips. */
	if (error != 0)
		vec_lines_reset(&draw->lines);

	/* Succeeded: the polylines are filled or the command is to be skipped. */
	return 0;
}

/*
 * Describes a clip to the sink, after its parent, once per drawing.
 */
static int
draw_clip_describe(
	struct draw *draw,
	int index)
{
	const struct svg_picture *picture;
	const struct svg_clip *clip;
	struct svg_draw_clip description;
	struct svg_draw_clip_part *parts;
	struct vec_lines *lines;
	size_t part_count;
	size_t slot_count;
	size_t slot;
	int error;

	/* Does nothing for a clip the sink already knows. */
	if (draw->clip_state[index] == DRAW_CLIP_DONE)
		return 0;

	/* Refuses a clip that is its own ancestor. */
	if (draw->clip_state[index] == DRAW_CLIP_BUSY)
		return EINVAL;

	/* Refuses a parent that is outside the picture's clips. */
	picture = draw->picture;
	clip = &picture->clips[index];
	if (clip->parent < -1)
		return EINVAL;
	if (clip->parent >= 0 && (size_t)clip->parent >= picture->clip_count)
		return EINVAL;

	/* Describes the parent first, with this clip marked so that a cycle of parents is found. */
	draw->clip_state[index] = DRAW_CLIP_BUSY;
	if (clip->parent >= 0) {
		error = draw_clip_describe(draw, clip->parent);
		if (error != 0)
			return error;
	}

	/* Refuses parts that are outside the picture's parts. */
	if (clip->part_count > picture->part_count)
		return EINVAL;
	if (clip->first_part > picture->part_count - clip->part_count)
		return EINVAL;

	/* Allocates room for the parts' polylines and descriptions (one slot at least, so the pointers are valid). */
	slot_count = clip->part_count;
	if (slot_count == 0)
		slot_count = 1;
	lines = calloc(slot_count, sizeof(*lines));
	if (lines == NULL)
		return ENOMEM;
	parts = calloc(slot_count, sizeof(*parts));
	if (parts == NULL) {
		free(lines);
		return ENOMEM;
	}

	/* Flattens each part with its matrix composed with the placement. */
	part_count = 0;
	error = draw_clip_parts(draw, clip, lines, parts, &part_count);

	/* Hands the clip to the sink. */
	if (error == 0) {
		description.index = index;
		description.parts = parts;
		description.part_count = part_count;
		description.parent = clip->parent;
		error = draw->sink->clip(draw->sink->context, &description);
		if (error != 0)
			draw->sink_failed = 1;
	}

	/* Frees the polylines, which the sink may use only during its call. */
	for (slot = 0;
	     slot < slot_count;
	     slot++)
		vec_lines_release(&lines[slot]);
	free(lines);
	free(parts);
	if (error != 0)
		return error;

	/* The sink knows the clip from now on. */
	draw->clip_state[index] = DRAW_CLIP_DONE;

	/* Succeeded: the sink has the clip. */
	return 0;
}

/*
 * Flattens the parts of a clip into device-space polylines (lines holds
 * one structure per part) and fills the descriptions.  A part that cannot
 * be flattened is left out.
 */
static int
draw_clip_parts(
	struct draw *draw,
	const struct svg_clip *clip,
	struct vec_lines *lines,
	struct svg_draw_clip_part *parts,
	size_t *part_count)
{
	const struct svg_clip_part *source;
	struct vec_matrix matrix;
	struct vec_path_view view;
	size_t index;
	size_t count;
	int matrix_valid;
	int error;

	/* Walks the parts of the clip. */
	count = 0;
	for (index = 0;
	     index < clip->part_count;
	     index++) {
		source = &draw->picture->parts[clip->first_part + index];

		/* Refuses a part that names a path the picture does not have. */
		if (source->path >= draw->picture->path_count)
			return EINVAL;

		/* Maps the part's space to device pixels: its matrix, then the placement. */
		vec_matrix_multiply(&matrix, &source->matrix, draw->placement);
		matrix_valid = vec_matrix_valid(&matrix);
		if (!matrix_valid)
			continue;

		/* Flattens the part's path; one that cannot be mapped is left out. */
		vec_path_view_of(&draw->picture->paths[source->path], &view);
		error = vec_flatten(&view, &matrix, draw->tolerance, &lines[count]);
		if (error == ENOMEM)
			return ENOMEM;
		if (error != 0)
			continue;

		/* Describes the part to the sink. */
		parts[count].lines = &lines[count];
		parts[count].rule = source->rule;
		count++;
	}

	/* Reports how many parts were kept. */
	*part_count = count;

	/* Succeeded: the parts are flattened. */
	return 0;
}

/*
 * Makes an opacity or alpha a number from 0 to 1: a value that is not a
 * number or below 0 becomes 0, one above 1 becomes 1.
 */
static float
draw_unit(
	float value)
{
	/* A NaN or a negative value paints nothing. */
	if (!(value > 0.0f))
		return 0.0f;

	/* A value above one is full. */
	if (value > 1.0f)
		return 1.0f;

	/* Reports a value already in range. */
	return value;
}
