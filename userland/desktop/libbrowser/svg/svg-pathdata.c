/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * SVG path data read into a vector path.
 */

#include "svg/svg-pathdata.h"

#include <errno.h>

#include "svg/svg-values.h"

/*
 * The most numbers one argument set of any command has: the arc's rx, ry,
 * x-axis-rotation, large-arc-flag, sweep-flag, x and y.
 *
 * Values referenced: SVG 2 §9.3.8 (the arc's seven parameters).
 */
#define PATHDATA_ARGS_MAX	7

/*
 * What the previous command left for a smooth curve to reflect: nothing
 * (any command but the four curves, or no command), the second control
 * point of a cubic curve, or the control point of a quadratic curve.
 *
 * Values referenced: SVG 2 §9.3.6 and §9.3.7 (S follows C or S, T follows
 * Q or T), §9.5.2 (the reflected point).
 */
enum pathdata_previous {
	PATHDATA_PREVIOUS_OTHER = 0,
	PATHDATA_PREVIOUS_CUBIC,
	PATHDATA_PREVIOUS_QUAD
};

/*
 * The state of one reading: the path being built and the pen.
 *
 * current is the current point and start the first point of the current
 * subpath, both kept here (and not read back from the path) so that the
 * relative commands are measured from the point the data itself reached.
 * control is meaningful only when previous says a curve left one.  The
 * structure lives on the stack of svg_path_data_parse and goes away with it.
 */
struct pathdata_state {
	struct vec_path *path;
	float current_x;
	float current_y;
	float start_x;
	float start_y;
	enum pathdata_previous previous;
	float control_x;
	float control_y;
};

/* The forward declarations of the static functions. */
static int pathdata_is_wsp(char c);
static int pathdata_starts_number(char c);
static char pathdata_upper(char c);
static size_t pathdata_set_size(char upper);
static const char *pathdata_skip_wsp(const char *p);
static int pathdata_read_separator(const char **cursor, int *comma);
static int pathdata_read_flag(const char **cursor, float *flag);
static int pathdata_read_plain_set(const char **cursor, size_t count, float *args);
static int pathdata_read_arc_set(const char **cursor, float *args);
static int pathdata_read_set(const char **cursor, char upper, float *args);
static int pathdata_apply_set(struct pathdata_state *state, char letter, const float *args);
static int pathdata_close(struct pathdata_state *state);
static int pathdata_read_command(struct pathdata_state *state, char letter, const char **cursor, int *valid);

/*
 * Parses path data into a path.
 *
 * The data is read command by command.  Every argument set is added to
 * the path as soon as it has been read whole, so that a later error leaves
 * everything before it in the path (SVG 2 §9.5.4).  A syntax error ends
 * the reading with *complete 0; only a failed path command is an error of
 * the function.  A number that a float cannot hold is a syntax error; a
 * coordinate the path refuses (beyond VEC_COORD_MAX) is the path's error.
 */
int
svg_path_data_parse(
	const char *text,
	struct vec_path *out,
	int *complete)
{
	struct pathdata_state state;
	const char *p;
	char letter;
	char upper;
	int valid;
	int error;

	/* Refuses a missing result flag, so that no caller reads it unset. */
	if (complete == NULL)
		return EINVAL;

	/* Starts with the data taken as not complete. */
	*complete = 0;

	/* Refuses missing arguments, so that a NULL is never read. */
	if (text == NULL || out == NULL)
		return EINVAL;

	/*
	 * Puts the pen at the origin, from where a relative moveto that
	 * starts the data is measured, which makes it an absolute one
	 * (SVG 2 §9.3.3).
	 */
	state.path = out;
	state.current_x = 0.0f;
	state.current_y = 0.0f;
	state.start_x = 0.0f;
	state.start_y = 0.0f;
	state.previous = PATHDATA_PREVIOUS_OTHER;
	state.control_x = 0.0f;
	state.control_y = 0.0f;

	/* Skips the white space in front of the first command. */
	p = pathdata_skip_wsp(text);

	/* Takes empty data as valid and as drawing nothing (SVG 2 §9.3.9). */
	if (*p == '\0') {
		*complete = 1;
		return 0;
	}

	/* Draws nothing when the data does not start with a moveto (SVG 2 §9.3.3). */
	if (*p != 'M' && *p != 'm')
		return 0;

	/* Reads the commands one after another up to the end of the text. */
	while (*p != '\0') {
		letter = *p;
		p++;
		upper = pathdata_upper(letter);

		if (upper == 'Z') {
			/* Closes the subpath; the command has no arguments. */
			error = pathdata_close(&state);
			if (error != 0)
				return error;
		} else {
			/* Reads the command's argument sets; a syntax error ends the data here. */
			valid = 1;
			error = pathdata_read_command(&state, letter, &p, &valid);
			if (error != 0)
				return error;
			if (!valid)
				return 0;
		}

		/* Skips the white space between commands; a comma is not allowed there. */
		p = pathdata_skip_wsp(p);
	}

	/* Reports that the whole text matched the grammar. */
	*complete = 1;

	/* Succeeded: the path holds all of the data. */
	return 0;
}

/*
 * Tells whether a character is white space of the path data grammar.
 *
 * Values referenced: SVG 2 §9.3.9 (wsp: tab, space, line feed, form feed,
 * carriage return).
 */
static int
pathdata_is_wsp(
	char c)
{
	/* Takes the five characters of the grammar. */
	if (c == '\t')
		return 1;
	if (c == ' ')
		return 1;
	if (c == '\n')
		return 1;
	if (c == '\f')
		return 1;
	if (c == '\r')
		return 1;

	/* Every other character, a vertical tab and non-ASCII bytes included, is not white space. */
	return 0;
}

/*
 * Tells whether a character can begin a number: a digit, a point or a sign.
 */
static int
pathdata_starts_number(
	char c)
{
	/* Takes a digit. */
	if (c >= '0' && c <= '9')
		return 1;

	/* Takes a point, which begins a fraction. */
	if (c == '.')
		return 1;

	/* Takes a sign. */
	if (c == '+' || c == '-')
		return 1;

	/* Every other character begins a command or is an error. */
	return 0;
}

/*
 * Makes a letter upper case, leaving every other character as it is.
 */
static char
pathdata_upper(
	char c)
{
	/* Moves a lower case ASCII letter to its upper case. */
	if (c >= 'a' && c <= 'z')
		return (char)(c - 'a' + 'A');

	/* Reports anything else unchanged. */
	return c;
}

/*
 * Gives how many numbers one argument set of a command has, or 0 for a
 * letter that is not a command with arguments.
 *
 * Values referenced: SVG 2 §9.3.3 to §9.3.8 (the parameters of each
 * command): M L T x y; H V x or y; C x1 y1 x2 y2 x y; S Q x2 y2 x y or
 * x1 y1 x y; A rx ry x-axis-rotation large-arc-flag sweep-flag x y.
 */
static size_t
pathdata_set_size(
	char upper)
{
	/* Looks the command up. */
	switch (upper) {
	case 'M':
	case 'L':
	case 'T':
		return 2;
	case 'H':
	case 'V':
		return 1;
	case 'S':
	case 'Q':
		return 4;
	case 'C':
		return 6;
	case 'A':
		return PATHDATA_ARGS_MAX;
	default:
		break;
	}

	/* Reports a letter that is no command with arguments (Z has none). */
	return 0;
}

/*
 * Skips white space.
 */
static const char *
pathdata_skip_wsp(
	const char *p)
{
	int space;

	/* Moves past every white space character. */
	space = pathdata_is_wsp(*p);
	while (space) {
		p++;
		space = pathdata_is_wsp(*p);
	}

	/* Reports the first character that is not white space. */
	return p;
}

/*
 * Reads a comma_wsp separator if there is one: white space, at most one
 * comma, then white space, and not nothing.
 *
 * Returns 1 when something was read (the cursor moves past it and *comma
 * says whether a comma was part of it), 0 when there was no separator (the
 * cursor does not move).
 *
 * Values referenced: SVG 2 §9.3.9 (comma_wsp ::= (wsp+ ","? wsp*) |
 * ("," wsp*)).
 */
static int
pathdata_read_separator(
	const char **cursor,
	int *comma)
{
	const char *p;
	int space;
	int consumed;

	/* Starts at the cursor with nothing read. */
	p = *cursor;
	consumed = 0;
	*comma = 0;

	/* Reads the white space in front of the comma. */
	space = pathdata_is_wsp(*p);
	if (space) {
		consumed = 1;
		p = pathdata_skip_wsp(p);
	}

	/* Reads the comma, and the white space after it. */
	if (*p == ',') {
		consumed = 1;
		*comma = 1;
		p++;
		p = pathdata_skip_wsp(p);
	}

	/* Moves the cursor only when a separator was there. */
	if (consumed)
		*cursor = p;

	/* Reports whether there was a separator. */
	return consumed;
}

/*
 * Reads an arc flag: the single character 0 or 1, which needs no
 * separator after it.  *flag becomes 0 or 1.
 *
 * Returns 0 when read, EINVAL when the character is neither (the cursor
 * does not move then).
 *
 * Values referenced: SVG 2 §9.3.9 (flag ::= "0" | "1").
 */
static int
pathdata_read_flag(
	const char **cursor,
	float *flag)
{
	const char *p;

	/* Starts at the cursor. */
	p = *cursor;

	/* Takes a 0 or a 1, and refuses every other character. */
	if (*p == '0') {
		*flag = 0.0f;
	} else if (*p == '1') {
		*flag = 1.0f;
	} else {
		return EINVAL;
	}

	/* Moves the cursor past the flag. */
	*cursor = p + 1;

	/* Succeeded: one flag was read. */
	return 0;
}

/*
 * Reads count numbers, a comma_wsp that may be absent between them.
 *
 * Returns 0 with the numbers in args and the cursor past the last, or
 * EINVAL when a number is missing or malformed (the cursor is then
 * unspecified).
 *
 * Values referenced: SVG 2 §9.3.9 (coordinate_pair, coordinate_sequence
 * and the coordinate sequences of the curve commands: numbers separated by
 * comma_wsp?).
 */
static int
pathdata_read_plain_set(
	const char **cursor,
	size_t count,
	float *args)
{
	const char *p;
	size_t index;
	int comma;
	int error;

	/* Starts at the cursor. */
	p = *cursor;

	/* Reads the numbers, with an optional separator before each but the first. */
	for (index = 0;
	     index < count;
	     index++) {
		if (index != 0)
			(void)pathdata_read_separator(&p, &comma);

		/* Reads one number (an optional sign, then the number of SVG 2 §4.2). */
		error = svg_number_read(&p, &args[index]);
		if (error != 0)
			return error;
	}

	/* Moves the cursor past the set. */
	*cursor = p;

	/* Succeeded: the whole set was read. */
	return 0;
}

/*
 * Reads the seven parameters of one arc: rx, ry and x-axis-rotation as
 * numbers, then the two flags, then the end point.
 *
 * A separator is required between the rotation and the first flag and
 * optional elsewhere; a flag is one character, so the flags and the
 * number after them need no separator.  The three leading values are read
 * with the sign a number may carry as svg_number_read reads it: SVG 2
 * §9.5.1 names negative radii and the arcs01 example of §9.3.8 has a
 * negative rotation, which an unsigned reading of the EBNF's "number"
 * would refuse.
 *
 * Returns 0 with args holding rx, ry, rotation, large-arc-flag,
 * sweep-flag, x, y (the flags as 0 or 1) and the cursor past the set, or
 * EINVAL (the cursor is then unspecified).
 *
 * Values referenced: SVG 2 §9.3.9 (elliptical_arc_argument).
 */
static int
pathdata_read_arc_set(
	const char **cursor,
	float *args)
{
	const char *p;
	int comma;
	int found;
	int error;

	/* Starts at the cursor. */
	p = *cursor;

	/* Reads rx. */
	error = svg_number_read(&p, &args[0]);
	if (error != 0)
		return error;

	/* Reads ry. */
	(void)pathdata_read_separator(&p, &comma);
	error = svg_number_read(&p, &args[1]);
	if (error != 0)
		return error;

	/* Reads the rotation of the ellipse's x axis. */
	(void)pathdata_read_separator(&p, &comma);
	error = svg_number_read(&p, &args[2]);
	if (error != 0)
		return error;

	/* Requires the separator between the rotation and the flags. */
	found = pathdata_read_separator(&p, &comma);
	if (!found)
		return EINVAL;

	/* Reads the large-arc flag. */
	error = pathdata_read_flag(&p, &args[3]);
	if (error != 0)
		return error;

	/* Reads the sweep flag, which may follow the first flag at once. */
	(void)pathdata_read_separator(&p, &comma);
	error = pathdata_read_flag(&p, &args[4]);
	if (error != 0)
		return error;

	/* Reads the end point, which may follow the second flag at once. */
	(void)pathdata_read_separator(&p, &comma);
	error = svg_number_read(&p, &args[5]);
	if (error != 0)
		return error;
	(void)pathdata_read_separator(&p, &comma);
	error = svg_number_read(&p, &args[6]);
	if (error != 0)
		return error;

	/* Moves the cursor past the set. */
	*cursor = p;

	/* Succeeded: the whole arc was read. */
	return 0;
}

/*
 * Reads one argument set of a command (upper case letter).
 *
 * Returns 0 with the set in args, or EINVAL when it is not whole.
 */
static int
pathdata_read_set(
	const char **cursor,
	char upper,
	float *args)
{
	size_t count;
	int error;

	/* Reads an arc by its own grammar, every other command as plain numbers. */
	if (upper == 'A') {
		error = pathdata_read_arc_set(cursor, args);
	} else {
		count = pathdata_set_size(upper);
		error = pathdata_read_plain_set(cursor, count, args);
	}

	/* Reports that the set was not whole. */
	if (error != 0)
		return error;

	/* Succeeded: one whole set was read. */
	return 0;
}

/*
 * Adds the segment of one argument set to the path and moves the pen.
 *
 * letter is the command letter in force (a moveto after its first set has
 * become the lineto it implies); args the set as read.  Relative
 * coordinates are the current point plus the number (SVG 2 §9.3.3 and the
 * following sections).
 *
 * Returns 0, or the path's sticky error.
 *
 * Values referenced: SVG 2 §9.3.3 to §9.3.8 (the commands), §9.5.2 (the
 * reflected control point is 2 * current - previous control point).
 */
static int
pathdata_apply_set(
	struct pathdata_state *state,
	char letter,
	const float *args)
{
	struct vec_path *path;
	enum pathdata_previous previous;
	char upper;
	float base_x;
	float base_y;
	float x;
	float y;
	float x1;
	float y1;
	float x2;
	float y2;
	float control_x;
	float control_y;
	int large_arc;
	int sweep;
	int error;

	/* Starts with the path, and with nothing for a smooth curve to reflect. */
	path = state->path;
	previous = PATHDATA_PREVIOUS_OTHER;
	control_x = 0.0f;
	control_y = 0.0f;
	upper = pathdata_upper(letter);
	x = state->current_x;
	y = state->current_y;

	/* Takes the origin the coordinates are measured from: the current point for a lower case letter. */
	base_x = 0.0f;
	base_y = 0.0f;
	if (letter != upper) {
		base_x = state->current_x;
		base_y = state->current_y;
	}

	/* Adds the segment of the command. */
	switch (upper) {
	case 'M':
		/* Starts a subpath at the point. */
		x = base_x + args[0];
		y = base_y + args[1];
		error = vec_path_move_to(path, x, y);
		if (error != 0)
			return error;
		state->start_x = x;
		state->start_y = y;
		break;
	case 'L':
		/* Draws a line to the point. */
		x = base_x + args[0];
		y = base_y + args[1];
		error = vec_path_line_to(path, x, y);
		if (error != 0)
			return error;
		break;
	case 'H':
		/* Draws a horizontal line: the line to (x, current y). */
		x = base_x + args[0];
		y = state->current_y;
		error = vec_path_line_to(path, x, y);
		if (error != 0)
			return error;
		break;
	case 'V':
		/* Draws a vertical line: the line to (current x, y). */
		x = state->current_x;
		y = base_y + args[0];
		error = vec_path_line_to(path, x, y);
		if (error != 0)
			return error;
		break;
	case 'C':
		/* Draws a cubic curve through its two control points. */
		x1 = base_x + args[0];
		y1 = base_y + args[1];
		x2 = base_x + args[2];
		y2 = base_y + args[3];
		x = base_x + args[4];
		y = base_y + args[5];
		error = vec_path_cubic_to(path, x1, y1, x2, y2, x, y);
		if (error != 0)
			return error;
		previous = PATHDATA_PREVIOUS_CUBIC;
		control_x = x2;
		control_y = y2;
		break;
	case 'S':
		/* Draws a cubic curve whose first control point is the reflection of the previous second one. */
		x2 = base_x + args[0];
		y2 = base_y + args[1];
		x = base_x + args[2];
		y = base_y + args[3];
		x1 = state->current_x;
		y1 = state->current_y;
		if (state->previous == PATHDATA_PREVIOUS_CUBIC) {
			x1 = 2.0f * state->current_x - state->control_x;
			y1 = 2.0f * state->current_y - state->control_y;
		}
		error = vec_path_cubic_to(path, x1, y1, x2, y2, x, y);
		if (error != 0)
			return error;
		previous = PATHDATA_PREVIOUS_CUBIC;
		control_x = x2;
		control_y = y2;
		break;
	case 'Q':
		/* Draws a quadratic curve through its control point. */
		x1 = base_x + args[0];
		y1 = base_y + args[1];
		x = base_x + args[2];
		y = base_y + args[3];
		error = vec_path_quad_to(path, x1, y1, x, y);
		if (error != 0)
			return error;
		previous = PATHDATA_PREVIOUS_QUAD;
		control_x = x1;
		control_y = y1;
		break;
	case 'T':
		/* Draws a quadratic curve whose control point is the reflection of the previous one. */
		x = base_x + args[0];
		y = base_y + args[1];
		x1 = state->current_x;
		y1 = state->current_y;
		if (state->previous == PATHDATA_PREVIOUS_QUAD) {
			x1 = 2.0f * state->current_x - state->control_x;
			y1 = 2.0f * state->current_y - state->control_y;
		}
		error = vec_path_quad_to(path, x1, y1, x, y);
		if (error != 0)
			return error;
		previous = PATHDATA_PREVIOUS_QUAD;
		control_x = x1;
		control_y = y1;
		break;
	case 'A':
		/* Draws an elliptical arc, which the path turns into cubic curves; the radii and rotation are never relative. */
		large_arc = 0;
		if (args[3] != 0.0f)
			large_arc = 1;
		sweep = 0;
		if (args[4] != 0.0f)
			sweep = 1;
		x = base_x + args[5];
		y = base_y + args[6];
		error = vec_path_arc_to(path, args[0], args[1], args[2], large_arc, sweep, x, y);
		if (error != 0)
			return error;
		break;
	default:
		/* The caller passes only the letters of the commands above. */
		return EINVAL;
	}

	/* Moves the pen to the end of the segment and keeps what a smooth curve may reflect. */
	state->current_x = x;
	state->current_y = y;
	state->previous = previous;
	state->control_x = control_x;
	state->control_y = control_y;

	/* Succeeded: the segment is in the path. */
	return 0;
}

/*
 * Closes the current subpath: the pen goes back to the subpath's first
 * point, and nothing is left for a smooth curve to reflect.
 *
 * Returns 0, or the path's sticky error.
 *
 * Values referenced: SVG 2 §9.3.4 (the new current point is the subpath's
 * initial point).
 */
static int
pathdata_close(
	struct pathdata_state *state)
{
	int error;

	/* Closes the subpath in the path. */
	error = vec_path_close(state->path);
	if (error != 0)
		return error;

	/* Moves the pen back to the subpath's first point. */
	state->current_x = state->start_x;
	state->current_y = state->start_y;
	state->previous = PATHDATA_PREVIOUS_OTHER;

	/* Succeeded: the subpath is closed. */
	return 0;
}

/*
 * Reads the argument sets of one command (its letter is already read) and
 * adds each to the path as it is whole.
 *
 * *valid is 1 on return when the command's text matched the grammar, and
 * 0 when a set was missing or malformed or a separator ended the command
 * with no set after it; the sets before that point are in the path
 * (SVG 2 §9.5.4).  Returns 0, or the path's sticky error.
 *
 * Values referenced: SVG 2 §9.3.2 (a command repeats implicitly while
 * further sets follow; after M or m the further sets are L or l), §9.3.9
 * (wsp* after the letter; comma_wsp? between sets; no comma after the
 * last set).
 */
static int
pathdata_read_command(
	struct pathdata_state *state,
	char letter,
	const char **cursor,
	int *valid)
{
	const char *p;
	char upper;
	char active;
	float args[PATHDATA_ARGS_MAX];
	size_t size;
	int more;
	int comma;
	int number;
	int error;

	/* Finds the command; a letter that is no command is an error of the data. */
	upper = pathdata_upper(letter);
	size = pathdata_set_size(upper);
	if (size == 0) {
		*valid = 0;
		return 0;
	}

	/* Skips the white space between the letter and its first set. */
	p = pathdata_skip_wsp(*cursor);

	/* Reads the sets, each added to the path once it is whole. */
	active = letter;
	more = 1;
	while (more) {
		/* Reads one set; the first one is required. */
		error = pathdata_read_set(&p, upper, args);
		if (error != 0) {
			*valid = 0;
			*cursor = p;
			return 0;
		}

		/* Adds the set to the path. */
		error = pathdata_apply_set(state, active, args);
		if (error != 0)
			return error;

		/* Makes the sets after a moveto's first one the linetos they stand for. */
		if (active == 'M')
			active = 'L';
		if (active == 'm')
			active = 'l';

		/* Reads the separator before a further set; one with no set after it is an error. */
		more = 0;
		(void)pathdata_read_separator(&p, &comma);
		number = pathdata_starts_number(*p);
		if (number) {
			more = 1;
		} else if (comma) {
			*valid = 0;
			*cursor = p;
			return 0;
		}
	}

	/* Hands back the text after the command. */
	*cursor = p;

	/* Succeeded: the command and all its sets matched the grammar. */
	return 0;
}
