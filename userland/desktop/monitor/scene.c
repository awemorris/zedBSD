/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The System Monitor's screen (design.md sections 2 and 3): the layout of
 * the plates, and a frame's shapes and glyphs built from the latest values,
 * the history and the state.
 *
 * ws134-p002 draws the plates flat, in their layers' tones: the summary
 * row, the CPU's tiles, the state's nested squares, the GPU cards, the
 * network's and the disks' graphs, the memory's strata and the events.
 * The 3D core, the relief and the motion come with ws134-p003; the card
 * brought forward and the keyboard's edge (interact.c's input) with
 * ws134-p004.
 *
 * The layout is made in a logical 1280x800 window and stretched to the
 * real one; the text is drawn at the smaller of the two scales, so that it
 * never overflows its plate.
 */

#include "draw.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The logical window the layout is made in. */
#define SCENE_WIDTH		1280.0f
#define SCENE_HEIGHT		800.0f

/* The longest graph, in samples, and the points of a sparkline. */
#define SCENE_POINTS		SM_HISTORY_MAX

/* The time ranges of the graphs, in samples, and their names. */
static const unsigned scene_ranges[SM_RANGES] = { 60U, 300U, 900U, 3600U };

/* Each plate's layer (design.md section 2.2): the summaries and the state in front, the details behind, the events at the back. */
static const unsigned plate_layers[SM_PLATES] = { 2U, 2U, 2U, 2U, 2U, 1U, 2U, 1U, 1U, 1U, 1U, 2U, 0U };

/* The parallax of each layer in logical pixels (design.md section 3.1): the back moves most. */
static const float layer_parallax[3] = { 4.0f, 2.0f, 1.0f };

/* The rules each plate shows the warning of (bits of 1 << enum sm_rule). */
static const unsigned plate_rules[SM_PLATES] = {
	1U << SM_RULE_CPU,
	(1U << SM_RULE_GPU) | (1U << SM_RULE_TEMPERATURE),
	(1U << SM_RULE_MEMORY) | (1U << SM_RULE_SWAP),
	0U,
	1U << SM_RULE_LATENCY,
	1U << SM_RULE_CPU,
	0U,
	(1U << SM_RULE_GPU) | (1U << SM_RULE_TEMPERATURE),
	0U,
	(1U << SM_RULE_MEMORY) | (1U << SM_RULE_SWAP),
	1U << SM_RULE_LATENCY,
	1U << SM_RULE_LATENCY,
	0U
};

/* The cards' titles (design.md section 3.9), in the order of enum sm_plate. */
static const char *const card_titles[SM_PLATES] = {
	"CPU", "GPU", "Memory", "Network", "Disk", "CPU cores", "System state", "Graphics", "Network", "Memory", "Disk", "Disk latency",
	"Events"
};

/* The time ranges as the cards name them. */
static const char *const card_ranges[SM_RANGES] = { "Last minute", "Last 5 minutes", "Last 15 minutes", "Last hour" };

/* The rules as the state's card lists them, in the order of enum sm_rule. */
static const char *const card_rules[SM_RULE_COUNT] = { "CPU", "Memory", "Swap", "Disk latency", "GPU", "GPU temperature" };

/* The most lines of the background's grid (the logical window holds 27). */
#define SCENE_GRID_LINES	64U

/* How long a value's slide takes, and how far it moves, in milliseconds and logical pixels. */
#define SCENE_SLIDE_MS		180U
#define SCENE_SLIDE_DISTANCE	4.0f

static void scene_graph(struct sm_app *app, const struct sm_box *box, enum sm_series series, float top_value, struct sm_color color, int area, float shift);
static float scene_peak(const struct sm_app *app, enum sm_series first, enum sm_series second, unsigned count);
static void scene_meter(struct sm_app *app, float x, float y, float width, const char *label, const char *value, float share, struct sm_color color);
static void build_header(struct sm_app *app, uint64_t now_ms);
static void build_summary(struct sm_app *app, float shift, uint64_t now_ms);
static void build_cores(struct sm_app *app);
static void build_state(struct sm_app *app, uint64_t now_ms);
static void build_graphics(struct sm_app *app, uint64_t now_ms);
static void build_network(struct sm_app *app, float shift);
static void build_memory(struct sm_app *app);
static void build_disk(struct sm_app *app, float shift, uint64_t now_ms);
static void build_events(struct sm_app *app);
static void shown_text(struct sm_app *app, enum sm_plate plate, const char *name, const char *value);
static void view_compute(struct sm_app *app, uint64_t now_ms);
static void short_name(char *out, size_t size, const char *name);
static void fit_name(struct sm_app *app, enum sm_style style, float width, const char *prefix, const char *name, char *out, size_t size);
static void state_plate(struct sm_app *app, enum sm_plate plate, unsigned layer, float radius);
static float value_text(struct sm_app *app, enum sm_plate plate, enum sm_style style, float x, float baseline, const char *text, uint64_t now_ms);
static void flow_tube(struct sm_app *app, float x, float y, float width, float height, float phase, float share, int backwards, float jam, struct sm_color color);
static void build_keyboard(struct sm_app *app);
static void build_card(struct sm_app *app, float shift);
static void card_text(const struct sm_app *app, enum sm_plate plate, char *value, size_t value_size, char *note, size_t note_size);
static void card_chart(struct sm_app *app, enum sm_plate plate, const struct sm_box *chart, float shift);
static void card_cores(struct sm_app *app, const struct sm_box *chart);
static void card_list(struct sm_app *app, enum sm_plate plate, const struct sm_box *chart, float alpha);
static float ease_out(float progress);

/*
 * Lays the plates out in a window of a size (design.md section 2.3).
 */
void
sm_layout_compute(
	struct sm_layout *layout,
	float width,
	float height)
{
	static const struct sm_box logical[SM_PLATES] = {
		{ 24.0f, 48.0f, 233.6f, 112.0f },
		{ 273.6f, 48.0f, 233.6f, 112.0f },
		{ 523.2f, 48.0f, 233.6f, 112.0f },
		{ 772.8f, 48.0f, 233.6f, 112.0f },
		{ 1022.4f, 48.0f, 233.6f, 112.0f },
		{ 24.0f, 176.0f, 400.0f, 344.0f },
		{ 440.0f, 176.0f, 400.0f, 344.0f },
		{ 856.0f, 176.0f, 400.0f, 344.0f },
		{ 24.0f, 536.0f, 496.0f, 204.0f },
		{ 536.0f, 536.0f, 256.0f, 204.0f },
		{ 808.0f, 536.0f, 448.0f, 204.0f },
		{ 1100.0f, 572.0f, 140.0f, 152.0f },
		{ 24.0f, 752.0f, 1232.0f, 36.0f }
	};
	float sx;
	float sy;
	unsigned plate;

	/* The scales: the boxes stretch, the text keeps to the smaller. */
	sx = width / SCENE_WIDTH;
	sy = height / SCENE_HEIGHT;
	layout->width = width;
	layout->height = height;
	layout->scale = sx;
	if (sy < layout->scale)
		layout->scale = sy;

	/* The header's band and each plate. */
	layout->header.x = 24.0f * sx;
	layout->header.y = 8.0f * sy;
	layout->header.width = 1232.0f * sx;
	layout->header.height = 32.0f * sy;
	for (plate = 0; plate < SM_PLATES; plate++) {
		layout->plates[plate].x = logical[plate].x * sx;
		layout->plates[plate].y = logical[plate].y * sy;
		layout->plates[plate].width = logical[plate].width * sx;
		layout->plates[plate].height = logical[plate].height * sy;
	}
}

/*
 * Empties a scene for the next frame (its memory is kept).
 */
void
sm_scene_clear(
	struct sm_scene *scene)
{
	/* No vertices and no draws. */
	scene->vertex_count = 0;
	scene->draw_count = 0;
}

/*
 * Frees a scene's memory.
 */
void
sm_scene_release(
	struct sm_scene *scene)
{
	/* The vertices and the draws. */
	free(scene->vertices);
	free(scene->draws);
	memset(scene, 0, sizeof(*scene));
}

/*
 * Builds the frame's shapes and glyphs at a time (milliseconds since the
 * monitor started; the graphs slide between samples by it).  Returns 0, or
 * ENOMEM when the scene could not grow.
 */
int
sm_scene_build(
	struct sm_app *app,
	uint64_t now_ms)
{
	const struct sm_layout *layout;
	uint64_t slid_ms;
	uint64_t step_ms;
	float shift;
	float step;
	float offset;
	float x;
	unsigned lines;
	int error;

	/* Room for a frame's worth, then nothing in it. */
	error = sm_scene_reserve(&app->scene, 65536U);
	if (error != 0)
		return error;
	sm_scene_clear(&app->scene);
	layout = &app->layout;

	/*
	 * How far the graphs have slid towards the next sample (none with the
	 * clock stopped).  The slide goes in whole steps of the animation's
	 * frame time from the sample (half a second at 2 fps), so the animation's
	 * frames move the graphs by even steps and an input's frame between two
	 * of them leaves the graphs where the last one put them.
	 */
	shift = 0.0f;
	if (!app->fixed_clock &&
	    app->have_frame &&
	    app->source.period_ms != 0U &&
	    now_ms > app->frame_at_ms) {
		/* The time since the sample, down to a whole step of the animation. */
		slid_ms = now_ms - app->frame_at_ms;
		if (app->fps != 0U) {
			step_ms = 1000U / app->fps;
			slid_ms -= slid_ms % step_ms;
		}
		shift = (float)slid_ms / (float)app->source.period_ms;
		if (shift > 1.0f)
			shift = 1.0f;
	}

	/* The motion to this time, and the plates where their layers' parallax puts them. */
	sm_motion_update(app, now_ms);
	view_compute(app, now_ms);

	/* The background: the deep gradient and the faint grid of layer 0, moving with that layer. */
	sm_draw_gradient(&app->scene, 0.0f, 0.0f, layout->width, layout->height, sm_rgb(TOKEN_BG_DEEP, 1.0f), sm_rgb(TOKEN_BG_MID, 1.0f));
	step = 48.0f * layout->scale;
	offset = app->view.plates[SM_PLATE_EVENTS].x - layout->plates[SM_PLATE_EVENTS].x;
	lines = 0;
	for (x = step + offset; x < layout->width; x += step) {
		/* No more lines than the window holds, whatever the offset (a runaway one once filled the vertex memory). */
		lines++;
		if (lines > SCENE_GRID_LINES)
			break;
		sm_draw_rect(&app->scene, floorf(x), layout->header.y + layout->header.height, 1.0f, layout->height, sm_rgb(0x161d29U, 1.0f));
	}

	/* The parts, back to front. */
	build_header(app, now_ms);
	build_summary(app, shift, now_ms);
	build_cores(app);
	build_state(app, now_ms);
	build_graphics(app, now_ms);
	build_network(app, shift);
	build_memory(app);
	build_disk(app, shift, now_ms);
	build_events(app);

	/* In front of everything: the keyboard's plate's edge, then the card (ws134-p004). */
	build_keyboard(app);
	build_card(app, shift);

	/* Succeeded: the scene is ready to draw. */
	return 0;
}

/*
 * Draws a series of the history in a box over the range shown: a line,
 * and (area) the area under it fading down.  top_value is the value at the
 * box's top; shift slides it left by up to a sample towards the next one.
 */
static void
scene_graph(
	struct sm_app *app,
	const struct sm_box *box,
	enum sm_series series,
	float top_value,
	struct sm_color color,
	int area,
	float shift)
{
	static float values[SCENE_POINTS];
	float corners[8];
	float locals[8];
	float shape[4];
	float step;
	float x0;
	float y0;
	float x1;
	float y1;
	float bottom;
	float middle;
	unsigned range;
	unsigned count;
	unsigned index;
	struct sm_color clear;

	/* The range's samples, as many as were kept. */
	range = scene_ranges[app->range];
	count = sm_history_read(&app->history, series, range, values);
	if (count < 2U || top_value <= 0.0f)
		return;

	/* The step between samples across the box, and the shift. */
	step = box->width / (float)(range - 1U);
	bottom = box->y + box->height;
	middle = box->y + box->height * 0.5f;
	clear = color;
	clear.a = 0.0f;

	/* Each segment, from the oldest; the newest is at the box's right edge less the shift. */
	for (index = 0; index + 1U < count; index++) {
		x0 = box->x + box->width - (float)(count - 1U - index) * step - shift * step;
		x1 = x0 + step;
		if (x1 < box->x)
			continue;
		y0 = bottom - box->height * fminf(values[index] / top_value, 1.0f);
		y1 = bottom - box->height * fminf(values[index + 1U] / top_value, 1.0f);

		/* The area under it, fading towards the bottom. */
		if (area) {
			corners[0] = x0;
			corners[1] = y0;
			corners[2] = x1;
			corners[3] = y1;
			corners[4] = x1;
			corners[5] = bottom;
			corners[6] = x0;
			corners[7] = bottom;
			locals[0] = 0.0f;
			locals[1] = y0 - middle;
			locals[2] = 0.0f;
			locals[3] = y1 - middle;
			locals[4] = 0.0f;
			locals[5] = bottom - middle;
			locals[6] = 0.0f;
			locals[7] = bottom - middle;
			shape[0] = box->width * 0.5f;
			shape[1] = box->height * 0.5f;
			shape[2] = 0.0f;
			shape[3] = SM_SHAPE_AREA;
			sm_draw_quad(&app->scene, SM_PIPE_SHAPE, corners, locals, shape, sm_color_mix(clear, color, 0.28f), clear);
		}

		/* The line. */
		sm_draw_segment(&app->scene, x0, y0, x1, y1, 1.6f * app->layout.scale, color);
	}
}

/* The peak of one or two series over the range shown, rounded up to 1, 2 or 5 of a power of ten (at least 1). */
static float
scene_peak(
	const struct sm_app *app,
	enum sm_series first,
	enum sm_series second,
	unsigned count)
{
	float peak;
	float other;
	float power;

	/* The larger of the two series' peaks, with room above it. */
	peak = sm_history_peak(&app->history, first, count);
	if (second != first) {
		other = sm_history_peak(&app->history, second, count);
		if (other > peak)
			peak = other;
	}

	/* Room above it. */
	peak *= 1.25f;
	if (peak < 1.0f)
		return 1.0f;

	/* The next 1, 2 or 5 step. */
	power = powf(10.0f, floorf(log10f(peak)));
	if (peak <= power)
		return power;
	if (peak <= 2.0f * power)
		return 2.0f * power;
	if (peak <= 5.0f * power)
		return 5.0f * power;

	/* Succeeded: the next power of ten. */
	return 10.0f * power;
}

/* A labelled meter: the label, the value at the right, and a bar filled to a share. */
static void
scene_meter(
	struct sm_app *app,
	float x,
	float y,
	float width,
	const char *label,
	const char *value,
	float share,
	struct sm_color color)
{
	float s;
	float bar_y;

	/* The label and the value. */
	s = app->layout.scale;
	(void)sm_draw_text(app, SM_STYLE_SMALL, x, y + 12.0f * s, label, sm_rgb(TOKEN_TEXT_DIM, 1.0f), 0);
	(void)sm_draw_text(app, SM_STYLE_SMALL, x + width, y + 12.0f * s, value, sm_rgb(TOKEN_TEXT, 1.0f), 2);

	/* The track and its fill. */
	bar_y = y + 18.0f * s;
	if (share < 0.0f)
		share = 0.0f;
	if (share > 1.0f)
		share = 1.0f;
	sm_draw_round(&app->scene, x, bar_y, width, 5.0f * s, 2.5f * s, sm_rgb(0x2c3a4cU, 1.0f), sm_rgb(0x2c3a4cU, 0.0f));
	if (share > 0.0f)
		sm_draw_round(&app->scene, x, bar_y, fmaxf(width * share, 5.0f * s), 5.0f * s, 2.5f * s, color, sm_rgb(0, 0.0f));
}

/* The header's band: the machine, its CPUs and uptime, and the state's chip. */
static void
build_header(
	struct sm_app *app,
	uint64_t now_ms)
{
	const struct sm_box *box;
	struct timespec now;
	char uptime[32];
	char line[160];
	float s;
	float baseline;
	float chip_width;
	float chip_x;
	uint64_t seconds;
	int error;

	UNUSED_PARAMETER(now_ms);

	/* The system's uptime (the stopped clock's in a test). */
	s = app->layout.scale;
	box = &app->layout.header;
	seconds = app->fixed_ms / 1000U;
	if (!app->fixed_clock) {
		error = clock_gettime(CLOCK_MONOTONIC, &now);
		seconds = 0;
		if (error == 0)
			seconds = (uint64_t)now.tv_sec;
	}

	/* As text. */
	(void)sm_format_uptime(uptime, sizeof(uptime), seconds);

	/* The machine's line. */
	baseline = box->y + box->height * 0.5f + 5.0f * s;
	(void)snprintf(line, sizeof(line), "%s  |  %u CPUs  |  up %s", app->source.info.host, app->source.info.cpu_count, uptime);
	(void)sm_draw_text(app, SM_STYLE_NOTE, box->x + 4.0f * s, baseline, line, sm_rgb(TOKEN_TEXT_DIM, 1.0f), 0);

	/* The state's chip at the right: a dot and the level's name. */
	chip_width = sm_atlas_width(&app->atlas, SM_STYLE_NOTE, sm_level_name(app->level)) + 40.0f * s;
	chip_x = box->x + box->width - chip_width;
	sm_draw_round(&app->scene, chip_x, box->y + 2.0f * s, chip_width, box->height - 4.0f * s, (box->height - 4.0f * s) * 0.5f,
		    sm_color_mix(sm_rgb(TOKEN_SURFACE_1, 1.0f), sm_level_color(app->level), 0.14f), sm_color_mix(sm_rgb(TOKEN_EDGE_1, 1.0f), sm_level_color(app->level), 0.5f));
	sm_draw_disc(&app->scene, chip_x + 16.0f * s, box->y + box->height * 0.5f, 4.5f * s, sm_level_color(app->level));
	(void)sm_draw_text(app, SM_STYLE_NOTE, chip_x + 28.0f * s, baseline, sm_level_name(app->level), sm_rgb(TOKEN_TEXT, 1.0f), 0);
	shown_text(app, SM_PLATE_STATE, "state", sm_level_name(app->level));
}

/* The summary row: five plates of a title, a value, a note and a graph. */
static void
build_summary(
	struct sm_app *app,
	float shift,
	uint64_t now_ms)
{
	static const char *const titles[5] = { "CPU", "GPU", "Memory", "Network", "Disk" };
	static const char *const names[5] = { "cpu", "gpu", "memory", "network", "disk" };
	const struct sm_frame *frame;
	const struct sm_info *info;
	const struct sm_box *box;
	struct sm_box graph;
	char value[48];
	char note[64];
	char first[24];
	char second[24];
	float s;
	float peak;
	unsigned plate;

	/* Each plate. */
	s = app->layout.scale;
	frame = &app->frame;
	info = &app->source.info;
	for (plate = 0; plate < 5U; plate++) {
		box = &app->view.plates[SM_PLATE_CPU + plate];
		state_plate(app, (enum sm_plate)(SM_PLATE_CPU + plate), 2, 14.0f * s);
		(void)sm_draw_text(app, SM_STYLE_TITLE, box->x + 16.0f * s, box->y + 26.0f * s, titles[plate], sm_rgb(TOKEN_TEXT, 1.0f), 0);

		/* The value and the note. */
		value[0] = '\0';
		note[0] = '\0';
		switch (plate) {
		case 0:
			(void)sm_format_percent(value, sizeof(value), frame->cpu);
			(void)snprintf(note, sizeof(note), "%u cores", info->cpu_count);
			break;
		case 1:
			if (info->gpu_count == 0U) {
				strcpy(value, "-");
				strcpy(note, "none");
			} else {
				(void)sm_format_percent(value, sizeof(value), frame->gpu_busy[0]);
				short_name(note, sizeof(note), info->gpu_name[0]);
			}

			break;
		case 2:
			(void)sm_format_bytes(value, sizeof(value), frame->memory_used);
			(void)sm_format_bytes(first, sizeof(first), info->memory_total);
			(void)snprintf(note, sizeof(note), "of %s", first);
			break;
		case 3:
			(void)sm_format_rate(value, sizeof(value), frame->rx_rate + frame->tx_rate, 1);
			(void)sm_format_rate(first, sizeof(first), frame->rx_rate, 1);
			(void)sm_format_rate(second, sizeof(second), frame->tx_rate, 1);
			(void)snprintf(note, sizeof(note), "RX %s", first);
			break;
		default:
			(void)sm_format_rate(value, sizeof(value), frame->read_rate + frame->write_rate, 0);
			(void)sm_format_rate(first, sizeof(first), frame->read_rate, 0);
			(void)snprintf(note, sizeof(note), "R %s", first);
			break;
		}

		/* A plate without a frame yet shows a dash. */
		if (!app->have_frame)
			strcpy(value, "-");
		(void)sm_draw_text(app, SM_STYLE_NOTE, box->x + box->width - 16.0f * s, box->y + 26.0f * s, note, sm_rgb(TOKEN_TEXT_DIM, 1.0f), 2);
		(void)value_text(app, (enum sm_plate)(SM_PLATE_CPU + plate), SM_STYLE_VALUE, box->x + 16.0f * s, box->y + 66.0f * s, value, now_ms);
		shown_text(app, (enum sm_plate)(SM_PLATE_CPU + plate), names[plate], value);

		/* The graph across the plate's foot. */
		graph.x = box->x + 16.0f * s;
		graph.y = box->y + 76.0f * s;
		graph.width = box->width - 32.0f * s;
		graph.height = 26.0f * s;
		switch (plate) {
		case 0:
			scene_graph(app, &graph, SM_SERIES_CPU, 1.0f, sm_rgb(TOKEN_CYAN, 1.0f), 1, shift);
			break;
		case 1:
			scene_graph(app, &graph, SM_SERIES_GPU, 1.0f, sm_rgb(TOKEN_ICE, 1.0f), 1, shift);
			break;
		case 2:
			scene_graph(app, &graph, SM_SERIES_MEMORY, 1.0f, sm_rgb(TOKEN_CYAN, 1.0f), 1, shift);
			break;
		case 3:
			peak = scene_peak(app, SM_SERIES_RX, SM_SERIES_TX, scene_ranges[app->range]);
			scene_graph(app, &graph, SM_SERIES_RX, peak, sm_rgb(TOKEN_MINT, 1.0f), 1, shift);
			break;
		default:
			peak = scene_peak(app, SM_SERIES_READ, SM_SERIES_WRITE, scene_ranges[app->range]);
			scene_graph(app, &graph, SM_SERIES_READ, peak, sm_rgb(TOKEN_CYAN, 1.0f), 0, shift);
			scene_graph(app, &graph, SM_SERIES_WRITE, peak, sm_rgb(TOKEN_AMBER, 1.0f), 0, shift);
			break;
		}
	}
}

/* The CPU cores: a tile a CPU, lit by its load, and the overall, highest and lowest. */
static void
build_cores(
	struct sm_app *app)
{
	const struct sm_box *box;
	const struct sm_frame *frame;
	char line[96];
	char overall[16];
	char highest[16];
	char lowest[16];
	float s;
	unsigned count;
	unsigned index;
	unsigned high_index;
	double high;
	double low;

	/* The plate and its title. */
	s = app->layout.scale;
	box = &app->view.plates[SM_PLATE_CORES];
	frame = &app->frame;
	state_plate(app, SM_PLATE_CORES, 1, 14.0f * s);
	(void)sm_draw_text(app, SM_STYLE_TITLE, box->x + 18.0f * s, box->y + 28.0f * s, "CPU cores", sm_rgb(TOKEN_TEXT, 1.0f), 0);
	(void)sm_draw_text(app, SM_STYLE_NOTE, box->x + box->width - 18.0f * s, box->y + 28.0f * s, "per-core load", sm_rgb(TOKEN_TEXT_DIM, 1.0f), 2);

	/* The relief (space.c), and the highest and the lowest core. */
	sm_space_relief(app, box);
	count = app->source.info.cpu_count;
	high = -1.0;
	low = 2.0;
	high_index = 0;
	for (index = 0; index < count; index++) {
		/* The highest so far. */
		if (frame->cpu_core[index] > high) {
			high = frame->cpu_core[index];
			high_index = index;
		}

		/* And the lowest. */
		if (frame->cpu_core[index] < low)
			low = frame->cpu_core[index];
	}

	/* None without CPUs. */
	if (count == 0U)
		return;

	/* The overall, the highest (with its core) and the lowest. */
	(void)sm_format_percent(overall, sizeof(overall), frame->cpu);
	(void)sm_format_percent(highest, sizeof(highest), high);
	(void)sm_format_percent(lowest, sizeof(lowest), low);
	(void)snprintf(line, sizeof(line), "Overall %s    Highest %s (core %u)    Lowest %s", overall, highest, high_index, lowest);
	(void)sm_draw_text(app, SM_STYLE_NOTE, box->x + 18.0f * s, box->y + box->height - 20.0f * s, line, sm_rgb(TOKEN_TEXT_DIM, 1.0f), 0);
}

/* The system's state: the 3D core (space.c), the level and its cause. */
static void
build_state(
	struct sm_app *app,
	uint64_t now_ms)
{
	const struct sm_box *box;
	const char *note;
	float s;
	float cx;

	/* The plate and its title. */
	s = app->layout.scale;
	box = &app->view.plates[SM_PLATE_STATE];
	state_plate(app, SM_PLATE_STATE, 2, 14.0f * s);
	(void)sm_draw_text(app, SM_STYLE_TITLE, box->x + 18.0f * s, box->y + 28.0f * s, "System state", sm_rgb(TOKEN_TEXT, 1.0f), 0);
	note = "all nominal";
	if (app->level != SM_LEVEL_NORMAL)
		note = "attention";
	(void)sm_draw_text(app, SM_STYLE_NOTE, box->x + box->width - 18.0f * s, box->y + 28.0f * s, note, sm_rgb(TOKEN_TEXT_DIM, 1.0f), 2);

	/* The core. */
	sm_space_core(app, box, now_ms);

	/* The level's word and what holds it. */
	cx = box->x + box->width * 0.5f;
	(void)sm_draw_text(app, SM_STYLE_STATE, cx, box->y + box->height - 46.0f * s, sm_level_name(app->level), sm_rgb(TOKEN_TEXT, 1.0f), 1);
	(void)sm_draw_text(app, SM_STYLE_NOTE, cx, box->y + box->height - 22.0f * s, app->rules.summary, sm_rgb(TOKEN_TEXT_DIM, 1.0f), 1);
}

/* The GPUs: a module card each (two fit), its use, memory, temperature and power. */
static void
build_graphics(
	struct sm_app *app,
	uint64_t now_ms)
{
	const struct sm_box *box;
	const struct sm_frame *frame;
	const struct sm_info *info;
	struct sm_box card;
	struct sm_box history;
	struct sm_color warm;
	const char *plural;
	char note[32];
	char value[32];
	char first[16];
	char text[SM_NAME_MAX + 16];
	float s;
	float share;
	float card_y;
	float card_height;
	float x;
	float width;
	unsigned gpu;
	unsigned shown;

	/* The plate and its title. */
	s = app->layout.scale;
	box = &app->view.plates[SM_PLATE_GRAPHICS];
	frame = &app->frame;
	info = &app->source.info;
	state_plate(app, SM_PLATE_GRAPHICS, 1, 14.0f * s);
	(void)sm_draw_text(app, SM_STYLE_TITLE, box->x + 18.0f * s, box->y + 28.0f * s, "Graphics", sm_rgb(TOKEN_TEXT, 1.0f), 0);
	plural = "s";
	if (info->gpu_count == 1U)
		plural = "";
	(void)snprintf(note, sizeof(note), "%u device%s", info->gpu_count, plural);
	(void)sm_draw_text(app, SM_STYLE_NOTE, box->x + box->width - 18.0f * s, box->y + 28.0f * s, note, sm_rgb(TOKEN_TEXT_DIM, 1.0f), 2);

	/* No GPU: a line saying so. */
	if (info->gpu_count == 0U) {
		(void)sm_draw_text(app, SM_STYLE_NOTE, box->x + box->width * 0.5f, box->y + box->height * 0.5f, "No GPU",
				 sm_rgb(TOKEN_TEXT_FAINT, 1.0f), 1);
		return;
	}

	/* A card each, the first two; with one, its use's history under it. */
	shown = info->gpu_count;
	if (shown > 2U)
		shown = 2U;
	card_height = (box->height - 64.0f * s - 12.0f * s) / 2.0f;
	if (shown == 1U) {
		history.x = box->x + 18.0f * s;
		history.y = box->y + 46.0f * s + card_height + 34.0f * s;
		history.width = box->width - 36.0f * s;
		history.height = box->height - (history.y - box->y) - 22.0f * s;
		(void)sm_draw_text(app, SM_STYLE_SMALL, history.x, history.y - 10.0f * s, "Utilization over time", sm_rgb(TOKEN_TEXT_FAINT, 1.0f), 0);
		sm_draw_rect(&app->scene, history.x, history.y + history.height, history.width, 1.0f, sm_rgb(TOKEN_EDGE_0, 1.0f));
		scene_graph(app, &history, SM_SERIES_GPU, 1.0f, sm_rgb(TOKEN_ICE, 1.0f), 1, 0.0f);
	}

	/* Each card. */
	for (gpu = 0; gpu < shown; gpu++) {
		/* The card. */
		card_y = box->y + 46.0f * s + (float)gpu * (card_height + 12.0f * s);
		card.x = box->x + 16.0f * s;
		card.y = card_y;
		card.width = box->width - 32.0f * s;
		card.height = card_height;
		sm_draw_plate(&app->scene, &card, 2, 10.0f * s);
		(void)snprintf(first, sizeof(first), "GPU %u  ", gpu);
		fit_name(app, SM_STYLE_NOTE, card.width - 28.0f * s, first, info->gpu_name[gpu], text, sizeof(text));
		(void)sm_draw_text(app, SM_STYLE_NOTE, card.x + 14.0f * s, card.y + 22.0f * s, text, sm_rgb(TOKEN_TEXT, 1.0f), 0);

		/* Its use, large. */
		(void)sm_format_percent(value, sizeof(value), frame->gpu_busy[gpu]);
		if (gpu == 0U) {
			(void)value_text(app, SM_PLATE_GRAPHICS, SM_STYLE_VALUE_SMALL, card.x + 14.0f * s, card.y + 58.0f * s, value, now_ms);
		} else {
			(void)sm_draw_text(app, SM_STYLE_VALUE_SMALL, card.x + 14.0f * s, card.y + 58.0f * s, value, sm_rgb(TOKEN_TEXT, 1.0f), 0);
		}

		/* Its label. */
		(void)sm_draw_text(app, SM_STYLE_SMALL, card.x + 14.0f * s, card.y + 76.0f * s, "Utilization", sm_rgb(TOKEN_TEXT_DIM, 1.0f), 0);

		/* The meters: memory, temperature, power. */
		x = card.x + 120.0f * s;
		width = card.width - 134.0f * s;
		(void)sm_format_pair(text, sizeof(text), frame->gpu_memory_used[gpu], info->gpu_memory_total[gpu]);
		share = 0.0f;
		if (info->gpu_memory_total[gpu] != 0U)
			share = (float)frame->gpu_memory_used[gpu] / (float)info->gpu_memory_total[gpu];
		scene_meter(app, x, card.y + 30.0f * s, width, "Memory", text, share, sm_rgb(TOKEN_CYAN, 1.0f));
		(void)snprintf(text, sizeof(text), "%.0f C", frame->gpu_celsius[gpu]);
		warm = sm_rgb(TOKEN_MINT, 1.0f);
		if (frame->gpu_celsius[gpu] > 80.0)
			warm = sm_rgb(TOKEN_AMBER, 1.0f);
		scene_meter(app, x, card.y + 58.0f * s, width, "Temperature", text, (float)(frame->gpu_celsius[gpu] - 30.0) / 70.0f, warm);
		(void)snprintf(text, sizeof(text), "%.0f W", frame->gpu_watts[gpu]);
		scene_meter(app, x, card.y + 86.0f * s, width, "Power", text, (float)frame->gpu_watts[gpu] / 60.0f, sm_rgb(TOKEN_MINT, 1.0f));
	}
}

/* The network: the received and the sent flows over the range shown. */
static void
build_network(
	struct sm_app *app,
	float shift)
{
	const struct sm_box *box;
	struct sm_box graph;
	char note[64];
	char rx[24];
	char tx[24];
	char scale_text[24];
	float s;
	float peak;
	float tube_y;

	/* The plate and its title. */
	s = app->layout.scale;
	box = &app->view.plates[SM_PLATE_FLOW];
	state_plate(app, SM_PLATE_FLOW, 1, 14.0f * s);
	(void)sm_draw_text(app, SM_STYLE_TITLE, box->x + 18.0f * s, box->y + 28.0f * s, "Network", sm_rgb(TOKEN_TEXT, 1.0f), 0);
	(void)sm_format_rate(rx, sizeof(rx), app->frame.rx_rate, 1);
	(void)sm_format_rate(tx, sizeof(tx), app->frame.tx_rate, 1);
	(void)snprintf(note, sizeof(note), "RX %s   TX %s", rx, tx);
	(void)sm_draw_text(app, SM_STYLE_NOTE, box->x + box->width - 18.0f * s, box->y + 28.0f * s, note, sm_rgb(TOKEN_TEXT_DIM, 1.0f), 2);

	/* The graph and its scale. */
	graph.x = box->x + 18.0f * s;
	graph.y = box->y + 52.0f * s;
	graph.width = box->width - 36.0f * s;
	graph.height = box->height - 108.0f * s;
	peak = scene_peak(app, SM_SERIES_RX, SM_SERIES_TX, scene_ranges[app->range]);
	sm_draw_rect(&app->scene, graph.x, graph.y, graph.width, 1.0f, sm_rgb(TOKEN_EDGE_0, 1.0f));
	sm_draw_rect(&app->scene, graph.x, graph.y + graph.height, graph.width, 1.0f, sm_rgb(TOKEN_EDGE_0, 1.0f));
	(void)sm_format_rate(scale_text, sizeof(scale_text), peak, 1);
	(void)sm_draw_text(app, SM_STYLE_SMALL, graph.x, graph.y + 14.0f * s, scale_text, sm_rgb(TOKEN_TEXT_FAINT, 1.0f), 0);
	scene_graph(app, &graph, SM_SERIES_RX, peak, sm_rgb(TOKEN_CYAN, 1.0f), 1, shift);
	scene_graph(app, &graph, SM_SERIES_TX, peak, sm_rgb(TOKEN_MINT, 1.0f), 1, shift);

	/* The flows (design.md section 3.5): received from the left, sent from the right, faster and denser with more. */
	tube_y = graph.y + graph.height + 10.0f * s;
	flow_tube(app, graph.x, tube_y, graph.width, 10.0f * s, app->motion.rx_phase, (float)(app->frame.rx_rate / fmax(peak, 1.0)), 0, 0.0f,
		  sm_rgb(TOKEN_CYAN, 1.0f));
	flow_tube(app, graph.x, tube_y + 14.0f * s, graph.width, 10.0f * s, app->motion.tx_phase, (float)(app->frame.tx_rate / fmax(peak, 1.0)), 1, 0.0f,
		  sm_rgb(TOKEN_MINT, 1.0f));
}

/* The memory: its strata, used, cache, available and swap. */
static void
build_memory(
	struct sm_app *app)
{
	static const char *const names[4] = { "Used", "Cache", "Available", "Swap" };
	const struct sm_box *box;
	const struct sm_frame *frame;
	const struct sm_info *info;
	struct sm_color colors[4];
	char value[24];
	uint64_t amounts[4];
	uint64_t totals[4];
	float s;
	float y;
	float height;
	float width;
	float share;
	unsigned layer;

	/* The plate and its title. */
	s = app->layout.scale;
	box = &app->view.plates[SM_PLATE_STRATA];
	frame = &app->frame;
	info = &app->source.info;
	state_plate(app, SM_PLATE_STRATA, 1, 14.0f * s);
	(void)sm_draw_text(app, SM_STYLE_TITLE, box->x + 18.0f * s, box->y + 28.0f * s, "Memory", sm_rgb(TOKEN_TEXT, 1.0f), 0);
	(void)sm_format_bytes(value, sizeof(value), info->memory_total);
	(void)sm_draw_text(app, SM_STYLE_NOTE, box->x + box->width - 18.0f * s, box->y + 28.0f * s, value, sm_rgb(TOKEN_TEXT_DIM, 1.0f), 2);

	/* The four strata, each a bar of its share of its total. */
	amounts[0] = frame->memory_used;
	amounts[1] = frame->memory_cache;
	amounts[2] = frame->memory_available;
	amounts[3] = frame->swap_used;
	totals[0] = info->memory_total;
	totals[1] = info->memory_total;
	totals[2] = info->memory_total;
	totals[3] = info->swap_total;
	colors[0] = sm_rgb(TOKEN_CYAN, 1.0f);
	colors[1] = sm_rgb(TOKEN_ICE, 1.0f);
	colors[2] = sm_rgb(0x3c5068U, 1.0f);
	colors[3] = sm_rgb(TOKEN_AMBER, 1.0f);
	height = (box->height - 64.0f * s) / 4.0f - 6.0f * s;
	width = box->width - 36.0f * s;
	for (layer = 0; layer < 4U; layer++) {
		y = box->y + 46.0f * s + (float)layer * (height + 6.0f * s);
		share = 0.0f;
		if (totals[layer] != 0U)
			share = (float)amounts[layer] / (float)totals[layer];
		sm_draw_round(&app->scene, box->x + 18.0f * s, y, width, height, 6.0f * s, sm_rgb(0x243041U, 1.0f), sm_rgb(TOKEN_EDGE_0, 1.0f));
		if (share > 0.0f) {
			sm_draw_round(&app->scene, box->x + 18.0f * s, y, fmaxf(width * fminf(share, 1.0f), 8.0f * s), height, 6.0f * s,
				    sm_color_mix(sm_rgb(0x243041U, 1.0f), colors[layer], 0.75f), sm_rgb(0, 0.0f));
		}

		/* Its name and amount. */
		(void)sm_draw_text(app, SM_STYLE_SMALL, box->x + 28.0f * s, y + height * 0.5f + 4.0f * s, names[layer], sm_rgb(TOKEN_TEXT, 1.0f), 0);
		(void)sm_format_bytes(value, sizeof(value), amounts[layer]);
		(void)sm_draw_text(app, SM_STYLE_SMALL, box->x + box->width - 28.0f * s, y + height * 0.5f + 4.0f * s, value,
				 sm_rgb(TOKEN_TEXT, 1.0f), 2);
	}
}

/* The disks: the read and the written flows, and the latency's plate. */
static void
build_disk(
	struct sm_app *app,
	float shift,
	uint64_t now_ms)
{
	static const char *const lane_names[2] = { "Read", "Write" };
	static const enum sm_series lane_series[2] = { SM_SERIES_READ, SM_SERIES_WRITE };
	static const uint32_t lane_colors[2] = { TOKEN_CYAN, TOKEN_AMBER };
	const struct sm_box *box;
	const struct sm_box *latency;
	struct sm_box graph;
	struct sm_color dot;
	char note[64];
	char first[24];
	char second[24];
	char value[24];
	const char *word;
	float s;
	float peak;
	float share;
	float phase;
	unsigned lane;

	/* The plate and its title. */
	s = app->layout.scale;
	box = &app->view.plates[SM_PLATE_LANES];
	state_plate(app, SM_PLATE_LANES, 1, 14.0f * s);
	(void)sm_draw_text(app, SM_STYLE_TITLE, box->x + 18.0f * s, box->y + 28.0f * s, "Disk", sm_rgb(TOKEN_TEXT, 1.0f), 0);
	(void)sm_format_rate(first, sizeof(first), app->frame.read_rate, 0);
	(void)sm_format_rate(second, sizeof(second), app->frame.write_rate, 0);
	(void)snprintf(note, sizeof(note), "Read %s   Write %s", first, second);
	(void)sm_draw_text(app, SM_STYLE_NOTE, box->x + box->width - 172.0f * s, box->y + 28.0f * s, note, sm_rgb(TOKEN_TEXT_DIM, 1.0f), 2);

	/* Two lanes: read above, write below, on one scale. */
	peak = scene_peak(app, SM_SERIES_READ, SM_SERIES_WRITE, scene_ranges[app->range]);
	for (lane = 0; lane < 2U; lane++) {
		graph.x = box->x + 70.0f * s;
		graph.y = box->y + 50.0f * s + (float)lane * 72.0f * s;
		graph.width = box->width - 252.0f * s;
		graph.height = 44.0f * s;
		sm_draw_rect(&app->scene, graph.x, graph.y + graph.height, graph.width, 1.0f, sm_rgb(TOKEN_EDGE_0, 1.0f));
		(void)sm_draw_text(app, SM_STYLE_SMALL, box->x + 18.0f * s, graph.y + graph.height * 0.5f + 4.0f * s, lane_names[lane],
				 sm_rgb(TOKEN_TEXT_DIM, 1.0f), 0);
		scene_graph(app, &graph, lane_series[lane], peak, sm_rgb(lane_colors[lane], 1.0f), 1, shift);

		/* Its flow below, jammed towards its end as the latency rises (design.md section 3.6). */
		share = (float)(app->frame.read_rate / fmax(peak, 1.0));
		phase = app->motion.read_phase;
		if (lane == 1U) {
			share = (float)(app->frame.write_rate / fmax(peak, 1.0));
			phase = app->motion.write_phase;
		}

		/* The tube under the lane. */
		flow_tube(app, graph.x, graph.y + graph.height + 6.0f * s, graph.width, 8.0f * s, phase, share, 0,
			  (float)fmin(app->frame.disk_latency_ms / 10.0, 1.0), sm_rgb(lane_colors[lane], 1.0f));
	}

	/* The latency's plate, in front: its value and a word for it. */
	latency = &app->view.plates[SM_PLATE_LATENCY];
	state_plate(app, SM_PLATE_LATENCY, 2, 10.0f * s);
	(void)sm_draw_text(app, SM_STYLE_NOTE, latency->x + 14.0f * s, latency->y + 24.0f * s, "Latency", sm_rgb(TOKEN_TEXT, 1.0f), 0);
	(void)snprintf(value, sizeof(value), "%.1f ms", app->frame.disk_latency_ms);
	(void)value_text(app, SM_PLATE_LATENCY, SM_STYLE_VALUE_SMALL, latency->x + 14.0f * s, latency->y + 64.0f * s, value, now_ms);
	shown_text(app, SM_PLATE_LATENCY, "latency", value);
	word = "Good";
	if (app->frame.disk_latency_ms > 50.0)
		word = "Stalled";
	else if (app->frame.disk_latency_ms > 5.0)
		word = "Slow";
	dot = sm_rgb(TOKEN_MINT, 1.0f);
	if (app->frame.disk_latency_ms > 5.0)
		dot = sm_rgb(TOKEN_AMBER, 1.0f);
	sm_draw_disc(&app->scene, latency->x + 20.0f * s, latency->y + latency->height - 24.0f * s, 4.0f * s, dot);
	(void)sm_draw_text(app, SM_STYLE_SMALL, latency->x + 32.0f * s, latency->y + latency->height - 19.0f * s, word, sm_rgb(TOKEN_TEXT_DIM, 1.0f), 0);
}

/* The events' strip: the newest three, newest first. */
static void
build_events(
	struct sm_app *app)
{
	const struct sm_box *box;
	const struct sm_event *event;
	char line[120];
	float s;
	float x;
	float baseline;
	unsigned shown;
	unsigned index;
	uint64_t seconds;

	/* The strip and its title. */
	s = app->layout.scale;
	box = &app->view.plates[SM_PLATE_EVENTS];
	state_plate(app, SM_PLATE_EVENTS, 0, 10.0f * s);
	baseline = box->y + box->height * 0.5f + 5.0f * s;
	x = box->x + 18.0f * s;
	x += sm_draw_text(app, SM_STYLE_NOTE, x, baseline, "Events", sm_rgb(TOKEN_TEXT, 1.0f), 0) + 28.0f * s;

	/* None yet. */
	if (app->event_count == 0U) {
		(void)sm_draw_text(app, SM_STYLE_NOTE, x, baseline, "No events", sm_rgb(TOKEN_TEXT_FAINT, 1.0f), 0);
		return;
	}

	/* The newest three: a dot in the level's colour, the time and the text. */
	shown = app->event_count;
	if (shown > 3U)
		shown = 3U;
	for (index = 0; index < shown; index++) {
		event = &app->events[(app->event_count - 1U - index) % SM_EVENTS_MAX];
		seconds = event->time_ms / 1000U;
		sm_draw_disc(&app->scene, x + 4.0f * s, box->y + box->height * 0.5f, 4.0f * s, sm_level_color(event->level));
		(void)snprintf(line, sizeof(line), "%02llu:%02llu  %s", (unsigned long long)((seconds / 60U) % 60U),
			       (unsigned long long)(seconds % 60U), event->text);
		x += 14.0f * s;
		x += sm_draw_text(app, SM_STYLE_NOTE, x, baseline, line, sm_rgb(TOKEN_TEXT_DIM, 1.0f), 0) + 32.0f * s;
	}
}

/* Logs a plate's value when it changes ("ZMON TEXT plate=cpu value=37%"), for the tests to compare with the input. */
static void
shown_text(
	struct sm_app *app,
	enum sm_plate plate,
	const char *name,
	const char *value)
{
	int same;

	/* The same text as last time says nothing. */
	same = strncmp(app->shown[plate], value, sizeof(app->shown[plate]) - 1U);
	if (same == 0)
		return;

	/* The new text, kept and logged. */
	(void)snprintf(app->shown[plate], sizeof(app->shown[plate]), "%s", value);
	printf("ZMON TEXT plate=%s value=\"%s\"\n", name, value);
}

/*
 * Places the plates for this frame: each moved by its layer's parallax
 * (the camera's tilt and a slow sway, design.md section 3.1), and a plate
 * whose rule is at Warning or above a little forward (section 3.7).
 */
static void
view_compute(
	struct sm_app *app,
	uint64_t now_ms)
{
	struct sm_box *box;
	float sway;
	float tilt_x;
	float tilt_y;
	float lift;
	unsigned plate;
	unsigned rule;
	enum sm_level level;

	/* The tilt: the pointer's, and a sway of twelve seconds. */
	sway = 0.3f * sinf((float)(now_ms % 12000U) / 12000.0f * 6.283185307f);
	tilt_x = app->motion.tilt_x + sway;
	tilt_y = app->motion.tilt_y + sway * 0.5f;

	/* Each plate by its layer, the ones warned of lifted. */
	app->view = app->layout;
	for (plate = 0; plate < SM_PLATES; plate++) {
		box = &app->view.plates[plate];
		box->x += tilt_x * layer_parallax[plate_layers[plate]] * app->layout.scale;
		box->y += tilt_y * layer_parallax[plate_layers[plate]] * app->layout.scale;

		/* The highest level of its rules. */
		level = SM_LEVEL_NORMAL;
		for (rule = 0; rule < SM_RULE_COUNT; rule++) {
			if ((plate_rules[plate] & (1U << rule)) != 0U && app->rules.rule_levels[rule] > level)
				level = app->rules.rule_levels[rule];
		}

		/* Forward when warned: up a little and a little larger. */
		if (level >= SM_LEVEL_WARNING) {
			lift = 3.0f * app->layout.scale;
			box->x -= lift;
			box->y -= lift * 1.5f;
			box->width += lift * 2.0f;
			box->height += lift * 2.0f;
		}
	}
}

/* Draws a plate of a layer, its edge amber or coral when one of its rules is raised (design.md section 3.7). */
static void
state_plate(
	struct sm_app *app,
	enum sm_plate plate,
	unsigned layer,
	float radius)
{
	const struct sm_box *box;
	struct sm_color edge;
	struct sm_color warning;
	enum sm_level level;
	unsigned rule;

	/* The highest level of its rules. */
	box = &app->view.plates[plate];
	level = SM_LEVEL_NORMAL;
	for (rule = 0; rule < SM_RULE_COUNT; rule++) {
		if ((plate_rules[plate] & (1U << rule)) != 0U && app->rules.rule_levels[rule] > level)
			level = app->rules.rule_levels[rule];
	}

	/* Normal: the layer's own plate. */
	if (level == SM_LEVEL_NORMAL) {
		sm_draw_plate(&app->scene, box, layer, radius);
		return;
	}

	/* Raised: the edge amber (faint while only elevated), or coral when critical. */
	warning = sm_rgb(TOKEN_AMBER, 1.0f);
	if (level == SM_LEVEL_CRITICAL)
		warning = sm_rgb(TOKEN_CORAL, 1.0f);
	edge = sm_color_mix(sm_rgb(TOKEN_EDGE_2, 1.0f), warning, 0.45f);
	if (level >= SM_LEVEL_WARNING)
		edge = warning;
	sm_draw_round(&app->scene, box->x, box->y, box->width, box->height, radius, sm_rgb(TOKEN_SURFACE_2, 1.0f), edge);
}

/*
 * Draws a value that slides when it changes (design.md section 3.1): the
 * characters that differ from the last value come in from below when it
 * rose (above when it fell) over 180 ms, and the old ones leave the other
 * way, faint.  The values' style is monospaced, so the characters keep
 * their places.  Reports the width.
 */
static float
value_text(
	struct sm_app *app,
	enum sm_plate plate,
	enum sm_style style,
	float x,
	float baseline,
	const char *text,
	uint64_t now_ms)
{
	struct sm_slide *slide;
	struct sm_color color;
	struct sm_color faint;
	char single[2];
	float progress;
	float distance;
	float pen;
	float advance;
	size_t index;
	size_t length;
	size_t old_length;
	double new_value;
	double old_value;
	int same;

	/* A new value starts a slide: up when it rose. */
	slide = &app->slides[plate];
	same = strcmp(slide->text, text);
	if (same != 0) {
		memcpy(slide->old, slide->text, sizeof(slide->old));
		(void)snprintf(slide->text, sizeof(slide->text), "%s", text);
		slide->changed_ms = now_ms;
		new_value = strtod(text, NULL);
		old_value = strtod(slide->old, NULL);
		slide->direction = 1;
		if (new_value < old_value)
			slide->direction = -1;
	}

	/*
	 * How far the slide has come: done at once with the clock stopped, no
	 * old value, or frames further apart than the slide (2 fps), where the
	 * first frame of a new value would show its changed characters faint
	 * until the next one, half a second later (2026-10-12 user).
	 */
	progress = 1.0f;
	if (!app->fixed_clock &&
	    slide->old[0] != '\0' &&
	    app->fps != 0U && 1000U / app->fps < SCENE_SLIDE_MS &&
	    now_ms < slide->changed_ms + SCENE_SLIDE_MS)
		progress = (float)(now_ms - slide->changed_ms) / (float)SCENE_SLIDE_MS;
	progress = 1.0f - (1.0f - progress) * (1.0f - progress);
	distance = SCENE_SLIDE_DISTANCE * app->layout.scale;

	/* Each character: the same ones still, the changed ones sliding in, the old ones out. */
	color = sm_rgb(TOKEN_TEXT, 1.0f);
	faint = sm_rgb(TOKEN_TEXT, 0.25f * (1.0f - progress));
	length = strlen(text);
	old_length = strlen(slide->old);
	pen = x;
	single[1] = '\0';
	for (index = 0; index < length; index++) {
		single[0] = text[index];
		advance = sm_atlas_width(&app->atlas, style, single);

		/* A character as it was. */
		if (progress >= 1.0f ||
		    (index < old_length &&
		     slide->old[index] == text[index])) {
			(void)sm_draw_text(app, style, pen, baseline, single, color, 0);
			pen += advance;
			continue;
		}

		/* A new one coming in, and the old one going. */
		color.a = progress;
		(void)sm_draw_text(app, style, pen, baseline + (float)slide->direction * distance * (1.0f - progress), single, color, 0);
		color.a = 1.0f;
		if (index < old_length) {
			single[0] = slide->old[index];
			(void)sm_draw_text(app, style, pen, baseline - (float)slide->direction * distance * progress, single, faint, 0);
		}

		/* The next place. */
		pen += advance;
	}

	/* Succeeded: the width. */
	return pen - x;
}

/*
 * Draws a flow (design.md sections 3.5 and 3.6): a faint tube and the
 * light pulses moving along it, as many and as bright as the share says,
 * from the left (or the right when backwards); jam crowds them towards
 * the far end.
 */
static void
flow_tube(
	struct sm_app *app,
	float x,
	float y,
	float width,
	float height,
	float phase,
	float share,
	int backwards,
	float jam,
	struct sm_color color)
{
	struct sm_color tube;
	struct sm_color pulse;
	float position;
	float place;
	unsigned count;
	unsigned index;

	/* The tube. */
	tube = color;
	tube.a = 0.12f;
	sm_draw_round(&app->scene, x, y, width, height, height * 0.5f, tube, sm_rgb(0, 0.0f));

	/* The pulses: four to sixteen, brighter with more. */
	if (share < 0.0f)
		share = 0.0f;
	if (share > 1.0f)
		share = 1.0f;
	count = 4U + (unsigned)(share * 12.0f);
	pulse = color;
	pulse.a = 0.35f + 0.5f * share;
	for (index = 0; index < count; index++) {
		/* Its place along the tube, crowded towards the end by the jam. */
		position = (float)index / (float)count + phase;
		position -= floorf(position);
		position = powf(position, 1.0f / (1.0f + 2.0f * jam));
		place = position;
		if (backwards)
			place = 1.0f - position;
		sm_draw_disc(&app->scene, x + height * 0.5f + place * (width - height), y + height * 0.5f, height * 0.32f, pulse);
	}
}

/* A device's name without what follows its first parenthesis or comma ("Virtio-GPU Venus (llvmpipe)" is "Virtio-GPU Venus"). */
static void
short_name(
	char *out,
	size_t size,
	const char *name)
{
	size_t length;

	/* Up to the first parenthesis or comma, without the space before it. */
	length = strcspn(name, "(,");
	while (length > 0U && name[length - 1U] == ' ')
		length--;
	if (length + 1U > size)
		length = size - 1U;
	memcpy(out, name, length);
	out[length] = '\0';
}

/*
 * Writes prefix and a device's name into out so that it fits a width in a
 * style: the whole name when it fits, else the name up to its first
 * parenthesis or comma ("llvmpipe (LLVM 19.1.7, 256 bits)" is "llvmpipe"),
 * else that cut further with "..." at the end (ws134-p003: the name was cut
 * at 24 characters, in the middle of a word).
 */
static void
fit_name(
	struct sm_app *app,
	enum sm_style style,
	float width,
	const char *prefix,
	const char *name,
	char *out,
	size_t size)
{
	char shorter[SM_NAME_MAX];
	size_t length;
	float used;

	/* The whole name, when it fits. */
	(void)snprintf(out, size, "%s%s", prefix, name);
	used = sm_atlas_width(&app->atlas, style, out);
	if (used <= width)
		return;

	/* The name without its details, when that fits. */
	short_name(shorter, sizeof(shorter), name);
	(void)snprintf(out, size, "%s%s", prefix, shorter);
	used = sm_atlas_width(&app->atlas, style, out);
	if (used <= width)
		return;

	/* Shortened a character at a time, with "..." after what is left. */
	length = strlen(shorter);
	while (length > 0U) {
		length--;
		while (length > 0U && shorter[length - 1U] == ' ')
			length--;
		shorter[length] = '\0';
		(void)snprintf(out, size, "%s%s...", prefix, shorter);
		used = sm_atlas_width(&app->atlas, style, out);
		if (used <= width)
			return;
	}
}

/* Draws an ice edge around the plate the keyboard is on (Tab), a little outside it. */
static void
build_keyboard(
	struct sm_app *app)
{
	const struct sm_box *box;
	struct sm_color ice;
	float gap;
	float line;
	float x;
	float y;
	float width;
	float height;

	/* No plate has the keyboard. */
	if (app->focus.keyboard < 0)
		return;

	/* The edge's box, a few pixels around the plate. */
	box = &app->view.plates[app->focus.keyboard];
	gap = 4.0f * app->layout.scale;
	line = fmaxf(2.0f * app->layout.scale, 1.0f);
	x = box->x - gap;
	y = box->y - gap;
	width = box->width + 2.0f * gap;
	height = box->height + 2.0f * gap;
	ice = sm_rgb(TOKEN_ICE, 0.9f);

	/* Its four sides. */
	sm_draw_rect(&app->scene, x, y, width, line, ice);
	sm_draw_rect(&app->scene, x, y + height - line, width, line, ice);
	sm_draw_rect(&app->scene, x, y + line, line, height - 2.0f * line, ice);
	sm_draw_rect(&app->scene, x + width - line, y + line, line, height - 2.0f * line, ice);
}

/*
 * Draws the card brought forward (design.md section 3.9): the screen
 * behind it darkened, the card moving from its plate to the middle as it
 * comes out (eased), its title, value and note, a "Pinned" chip, and its
 * chart rising from below once it is mostly out.
 */
static void
build_card(
	struct sm_app *app,
	float shift)
{
	struct sm_box box;
	struct sm_box chart;
	struct sm_color edge;
	struct sm_color text;
	struct sm_color dim;
	struct sm_color fill;
	enum sm_plate plate;
	char value[64];
	char note[128];
	float s;
	float eased;
	float alpha;
	float rise;
	float title_width;
	float chip_width;
	float chip_x;
	float full_height;

	/* No card, or one all the way back. */
	if (app->focus.plate < 0)
		return;
	if (app->focus.progress <= 0.0f)
		return;

	/* How far it is out, eased; the words come in over its second half. */
	s = app->layout.scale;
	plate = (enum sm_plate)app->focus.plate;
	eased = ease_out(app->focus.progress);
	alpha = fminf(fmaxf((eased - 0.4f) / 0.6f, 0.0f), 1.0f);

	/* The screen behind it, darkened. */
	sm_draw_rect(&app->scene, 0.0f, 0.0f, app->layout.width, app->layout.height, sm_rgb(TOKEN_BG_DEEP, 0.4f * eased));

	/* The card: in the front surface's tone, its edge ice when pinned. */
	sm_card_box(app, eased, &box);
	edge = sm_rgb(TOKEN_EDGE_2, 1.0f);
	if (app->focus.pinned)
		edge = sm_rgb(TOKEN_ICE, 1.0f);
	fill = sm_color_mix(sm_rgb(TOKEN_SURFACE_1, 1.0f), sm_rgb(TOKEN_SURFACE_2, 1.0f), eased);
	sm_draw_round(&app->scene, box.x, box.y, box.width, box.height, 18.0f * s, fill, edge);

	/* Nothing in it until its words come in. */
	if (alpha <= 0.0f)
		return;
	text = sm_rgb(TOKEN_TEXT, alpha);
	dim = sm_rgb(TOKEN_TEXT_DIM, alpha);

	/* The title, and the range at the right. */
	title_width = sm_draw_text(app, SM_STYLE_TITLE, box.x + 24.0f * s, box.y + 36.0f * s, card_titles[plate], text, 0);
	(void)sm_draw_text(app, SM_STYLE_NOTE, box.x + box.width - 24.0f * s, box.y + 36.0f * s, card_ranges[app->range], dim, 2);

	/* The pin's chip after the title. */
	if (app->focus.pinned) {
		chip_width = sm_atlas_width(&app->atlas, SM_STYLE_SMALL, "Pinned") + 20.0f * s;
		chip_x = box.x + 24.0f * s + title_width + 14.0f * s;
		sm_draw_round(&app->scene, chip_x, box.y + 20.0f * s, chip_width, 22.0f * s, 11.0f * s, sm_rgb(TOKEN_SURFACE_1, alpha),
			      sm_rgb(TOKEN_ICE, alpha));
		(void)sm_draw_text(app, SM_STYLE_SMALL, chip_x + 10.0f * s, box.y + 35.0f * s, "Pinned", sm_rgb(TOKEN_ICE, alpha), 0);
	}

	/* The value, large, and the note beside it. */
	card_text(app, plate, value, sizeof(value), note, sizeof(note));
	(void)sm_draw_text(app, SM_STYLE_VALUE, box.x + 24.0f * s, box.y + 88.0f * s, value, text, 0);
	(void)sm_draw_text(app, SM_STYLE_NOTE, box.x + box.width - 24.0f * s, box.y + 88.0f * s, note, dim, 2);

	/* The chart's room, filled from below as it rises over the last two thirds of the card's coming out. */
	rise = ease_out(fminf(fmaxf((app->focus.progress - 0.33f) / 0.67f, 0.0f), 1.0f));
	if (rise <= 0.0f)
		return;
	full_height = box.height - 136.0f * s;
	chart.x = box.x + 24.0f * s;
	chart.width = box.width - 48.0f * s;
	chart.height = full_height * rise;
	chart.y = box.y + 112.0f * s + full_height - chart.height;
	sm_draw_rect(&app->scene, chart.x, box.y + 112.0f * s + full_height, chart.width, 1.0f, sm_rgb(TOKEN_EDGE_1, alpha));
	card_chart(app, plate, &chart, shift);

	/* The log says once that the card is all the way out. */
	if (app->focus.progress < 1.0f) {
		app->focus.drawn = 0;
		return;
	}

	/* Not again for the same card. */
	if (app->focus.drawn == app->focus.plate + 1)
		return;
	app->focus.drawn = app->focus.plate + 1;
	printf("ZMON CARD shown plate=%s pinned=%d value=\"%s\"\n", sm_plate_name(plate), app->focus.pinned, value);
}

/* Writes a card's value (its plate's main figure) and its note (the figures behind it). */
static void
card_text(
	const struct sm_app *app,
	enum sm_plate plate,
	char *value,
	size_t value_size,
	char *note,
	size_t note_size)
{
	const struct sm_frame *frame;
	const struct sm_info *info;
	char first[24];
	char second[24];
	char third[24];

	/* Each plate's figures. */
	frame = &app->frame;
	info = &app->source.info;
	value[0] = '\0';
	note[0] = '\0';
	switch (plate) {
	case SM_PLATE_CPU:
	case SM_PLATE_CORES:
		(void)sm_format_percent(value, value_size, frame->cpu);
		(void)snprintf(note, note_size, "%u cores", info->cpu_count);
		break;
	case SM_PLATE_GPU:
	case SM_PLATE_GRAPHICS:
		/* A machine without a GPU says so. */
		if (info->gpu_count == 0U) {
			(void)snprintf(value, value_size, "-");
			(void)snprintf(note, note_size, "No GPU");
			break;
		}

		/* The first GPU's use, temperature and power. */
		(void)sm_format_percent(value, value_size, frame->gpu_busy[0]);
		(void)snprintf(note, note_size, "%.0f C   %.0f W   %u device(s)", frame->gpu_celsius[0], frame->gpu_watts[0], info->gpu_count);
		break;
	case SM_PLATE_MEMORY:
	case SM_PLATE_STRATA:
		(void)sm_format_bytes(value, value_size, frame->memory_used);
		(void)sm_format_bytes(first, sizeof(first), frame->memory_cache);
		(void)sm_format_bytes(second, sizeof(second), frame->memory_available);
		(void)sm_format_bytes(third, sizeof(third), frame->swap_used);
		(void)snprintf(note, note_size, "Cache %s   Available %s   Swap %s", first, second, third);
		break;
	case SM_PLATE_NETWORK:
	case SM_PLATE_FLOW:
		(void)sm_format_rate(value, value_size, frame->rx_rate + frame->tx_rate, 1);
		(void)sm_format_rate(first, sizeof(first), frame->rx_rate, 1);
		(void)sm_format_rate(second, sizeof(second), frame->tx_rate, 1);
		(void)snprintf(note, note_size, "RX %s   TX %s", first, second);
		break;
	case SM_PLATE_DISK:
	case SM_PLATE_LANES:
	case SM_PLATE_LATENCY:
		(void)sm_format_rate(value, value_size, frame->read_rate + frame->write_rate, 0);
		(void)sm_format_rate(first, sizeof(first), frame->read_rate, 0);
		(void)sm_format_rate(second, sizeof(second), frame->write_rate, 0);
		(void)snprintf(note, note_size, "Read %s   Write %s   Latency %.1f ms", first, second, frame->disk_latency_ms);
		break;
	case SM_PLATE_STATE:
		(void)snprintf(value, value_size, "%s", sm_level_name(app->level));
		(void)snprintf(note, note_size, "%s", app->rules.summary);
		break;
	default:
		(void)snprintf(value, value_size, "%u", app->event_count);
		(void)snprintf(note, note_size, "events since the start");
		break;
	}

	/* No frame yet: a dash. */
	if (!app->have_frame)
		(void)snprintf(value, value_size, "-");
}

/* Draws a card's chart: its plate's series over the range, the cores' bars, or the state's rules or the events as a list. */
static void
card_chart(
	struct sm_app *app,
	enum sm_plate plate,
	const struct sm_box *chart,
	float shift)
{
	float peak;
	float alpha;

	/* The lists fade in as the chart's room rises. */
	alpha = 1.0f;
	if (chart->height < 40.0f * app->layout.scale)
		alpha = chart->height / (40.0f * app->layout.scale);

	/* Each plate's chart. */
	switch (plate) {
	case SM_PLATE_CPU:
		scene_graph(app, chart, SM_SERIES_CPU, 1.0f, sm_rgb(TOKEN_CYAN, 1.0f), 1, shift);
		break;
	case SM_PLATE_CORES:
		card_cores(app, chart);
		break;
	case SM_PLATE_GPU:
	case SM_PLATE_GRAPHICS:
		scene_graph(app, chart, SM_SERIES_GPU, 1.0f, sm_rgb(TOKEN_ICE, 1.0f), 1, shift);
		break;
	case SM_PLATE_MEMORY:
	case SM_PLATE_STRATA:
		scene_graph(app, chart, SM_SERIES_MEMORY, 1.0f, sm_rgb(TOKEN_CYAN, 1.0f), 1, shift);
		break;
	case SM_PLATE_NETWORK:
	case SM_PLATE_FLOW:
		/* Received and sent on one scale. */
		peak = scene_peak(app, SM_SERIES_RX, SM_SERIES_TX, scene_ranges[app->range]);
		scene_graph(app, chart, SM_SERIES_RX, peak, sm_rgb(TOKEN_CYAN, 1.0f), 1, shift);
		scene_graph(app, chart, SM_SERIES_TX, peak, sm_rgb(TOKEN_MINT, 1.0f), 1, shift);
		break;
	case SM_PLATE_DISK:
	case SM_PLATE_LANES:
	case SM_PLATE_LATENCY:
		/* Read and written on one scale. */
		peak = scene_peak(app, SM_SERIES_READ, SM_SERIES_WRITE, scene_ranges[app->range]);
		scene_graph(app, chart, SM_SERIES_READ, peak, sm_rgb(TOKEN_CYAN, 1.0f), 1, shift);
		scene_graph(app, chart, SM_SERIES_WRITE, peak, sm_rgb(TOKEN_AMBER, 1.0f), 1, shift);
		break;
	default:
		card_list(app, plate, chart, alpha);
		break;
	}
}

/* Draws the cores' bars across a chart, each as high as its core's load (amber past 85%). */
static void
card_cores(
	struct sm_app *app,
	const struct sm_box *chart)
{
	struct sm_color color;
	float pitch;
	float width;
	float height;
	float x;
	unsigned count;
	unsigned index;

	/* None without CPUs. */
	count = app->source.info.cpu_count;
	if (count == 0U)
		return;

	/* A bar a core, with a gap of a quarter of its pitch. */
	pitch = chart->width / (float)count;
	width = fmaxf(pitch * 0.75f, 1.0f);
	for (index = 0; index < count; index++) {
		/* Its height, and amber when the core is nearly full. */
		height = chart->height * (float)fmin(fmax(app->frame.cpu_core[index], 0.0), 1.0);
		color = sm_rgb(TOKEN_CYAN, 0.85f);
		if (app->frame.cpu_core[index] > 0.85)
			color = sm_rgb(TOKEN_AMBER, 0.9f);

		/* The bar on the chart's floor. */
		x = chart->x + (float)index * pitch + (pitch - width) * 0.5f;
		sm_draw_round(&app->scene, x, chart->y + chart->height - height, width, height, fminf(width * 0.25f, 4.0f), color, sm_rgb(0, 0.0f));
	}
}

/* Draws the state's rules (a dot in each one's level and its name) or the newest events as rows in a chart's room. */
static void
card_list(
	struct sm_app *app,
	enum sm_plate plate,
	const struct sm_box *chart,
	float alpha)
{
	const struct sm_event *event;
	char line[120];
	float s;
	float row;
	float baseline;
	unsigned rows;
	unsigned index;
	uint64_t seconds;

	/* The rows that fit. */
	s = app->layout.scale;
	row = 26.0f * s;
	rows = (unsigned)(chart->height / row);

	/* The state: each rule and its level. */
	if (plate == SM_PLATE_STATE) {
		for (index = 0; index < SM_RULE_COUNT && index < rows; index++) {
			baseline = chart->y + row * (float)index + 18.0f * s;
			sm_draw_disc(&app->scene, chart->x + 6.0f * s, baseline - 5.0f * s, 5.0f * s, sm_level_color(app->rules.rule_levels[index]));
			(void)sm_draw_text(app, SM_STYLE_NOTE, chart->x + 22.0f * s, baseline, card_rules[index], sm_rgb(TOKEN_TEXT, alpha), 0);
			(void)sm_draw_text(app, SM_STYLE_NOTE, chart->x + chart->width, baseline, sm_level_name(app->rules.rule_levels[index]),
					   sm_rgb(TOKEN_TEXT_DIM, alpha), 2);
		}

		/* The rules are the state's whole list. */
		return;
	}

	/* No events yet. */
	if (app->event_count == 0U) {
		(void)sm_draw_text(app, SM_STYLE_NOTE, chart->x, chart->y + 18.0f * s, "No events", sm_rgb(TOKEN_TEXT_FAINT, alpha), 0);
		return;
	}

	/* The newest events first, as many as fit. */
	for (index = 0; index < app->event_count && index < SM_EVENTS_MAX && index < rows; index++) {
		event = &app->events[(app->event_count - 1U - index) % SM_EVENTS_MAX];
		seconds = event->time_ms / 1000U;
		baseline = chart->y + row * (float)index + 18.0f * s;
		sm_draw_disc(&app->scene, chart->x + 6.0f * s, baseline - 5.0f * s, 5.0f * s, sm_level_color(event->level));
		(void)snprintf(line, sizeof(line), "%02llu:%02llu  %s", (unsigned long long)((seconds / 60U) % 60U), (unsigned long long)(seconds % 60U),
			       event->text);
		(void)sm_draw_text(app, SM_STYLE_NOTE, chart->x + 22.0f * s, baseline, line, sm_rgb(TOKEN_TEXT_DIM, alpha), 0);
	}
}

/* Eases a progress from 0 to 1 out: fast first, settling at the end (1 - (1 - p)^3). */
static float
ease_out(
	float progress)
{
	float rest;

	/* The share still to go, cubed. */
	rest = 1.0f - progress;

	/* Succeeded: the eased progress. */
	return 1.0f - rest * rest * rest;
}
