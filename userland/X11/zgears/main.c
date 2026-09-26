/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * zgears: three lit gears turning in an X window, drawn with OpenGL 1.x's
 * fixed function through GLX (WS069 p005): display lists of quads and
 * quad strips, flat and smooth shading, one light, the colour of each
 * gear its material, the depth test and back-face culling.
 *
 * The first frame is read back: the red, green and blue gears must each
 * cover part of the window, over a black background (ZGEARS CHECK).  The
 * frames per second are printed every 100 frames.
 */

#include <GL/glx.h>

#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Pi. */
#define GEARS_PI	3.14159265358979f

/* How many seconds a frame may take before the watchdog says where it is, and how many times it says so. */
#define GEARS_WATCHDOG_SECONDS	3U
#define GEARS_WATCHDOG_REPORTS	5

/* The steps of a frame the watchdog names (WS069 p007). */
#define GEARS_STEP_EVENTS	1
#define GEARS_STEP_GEOMETRY	2
#define GEARS_STEP_DRAW		3
#define GEARS_STEP_SWAP		4
#define GEARS_STEP_SLEEP	5

/* libGL's own: the step glXSwapBuffers is at. */
extern volatile int zglx_swap_step;

/*
 * What the command line asked for.
 */
struct gears_options {
	const char *token;
	unsigned width;
	unsigned height;
	unsigned frames;
	unsigned delay_ms;
};

/*
 * One gear's shape.
 */
struct gears_shape {
	GLfloat inner;
	GLfloat outer;
	GLfloat width;
	unsigned teeth;
	GLfloat depth;
};

static int gears_parse(int argc, char **argv, struct gears_options *options);
static void gears_watchdog(int number);
static char *gears_decimal(char *cursor, unsigned long value);
static void gears_setup(GLuint *lists);
static void gears_make(GLuint list, const struct gears_shape *shape, const GLfloat *colour);
static void gears_faces(const struct gears_shape *shape, GLfloat z);
static void gears_outside(const struct gears_shape *shape);
static void gears_inside(const struct gears_shape *shape);
static void gears_draw(const GLuint *lists, unsigned width, unsigned height, GLfloat angle);
static int gears_check(unsigned width, unsigned height, const char *token, unsigned frame);
static double gears_now(void);
static void gears_sleep(unsigned milliseconds);

/* The frame and its step, for the watchdog, and how many times it has spoken. */
static volatile sig_atomic_t gears_frame;
static volatile sig_atomic_t gears_step;
static volatile sig_atomic_t gears_reports;

/*
 * Runs the gears.
 */
int
main(
	int argc,
	char **argv)
{
	static int visual_attributes[] = {
		GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 16, None
	};
	struct gears_options options;
	struct sigaction watchdog;
	XVisualInfo *visual;
	GLXContext context;
	Display *display;
	Window window;
	Window root;
	XEvent event;
	GLuint lists[3];
	unsigned int width;
	unsigned int height;
	unsigned int border;
	unsigned int depth;
	unsigned frame;
	double started;
	double now;
	double before;
	double drawn;
	double begun;
	double angle;
	int x;
	int y;
	int failures;
	int status;
	int pending;
	Bool done;

	/* The command line. */
	status = gears_parse(argc, argv, &options);
	if (status != 0) {
		fprintf(stderr, "usage: zgears [--size=WxH] [--frames=N (0: until closed)] [--delay-ms=N] [--token=NAME]\n");
		return 2;
	}

	/* The server, a visual with a depth buffer, and a window of it. */
	display = XOpenDisplay(NULL);
	if (display == NULL) {
		printf("ZGEARS FAILED run=%s operation=XOpenDisplay\n", options.token);
		return 1;
	}

	/* A visual with a depth buffer. */
	visual = glXChooseVisual(display, DefaultScreen(display), visual_attributes);
	if (visual == NULL) {
		printf("ZGEARS FAILED run=%s operation=glXChooseVisual\n", options.token);
		return 1;
	}

	/* A window of it, shown. */
	window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, options.width, options.height, 0,
				     BlackPixel(display, DefaultScreen(display)), BlackPixel(display, DefaultScreen(display)));
	(void)XStoreName(display, window, "Gears");
	(void)XSelectInput(display, window, ExposureMask | StructureNotifyMask | KeyPressMask);
	(void)XMapWindow(display, window);

	/* A context, current on the window. */
	context = glXCreateContext(display, visual, NULL, True);
	XFree(visual);
	if (context == NULL) {
		printf("ZGEARS FAILED run=%s operation=glXCreateContext\n", options.token);
		return 1;
	}

	/* Current on the window. */
	done = glXMakeCurrent(display, window, context);
	if (!done) {
		printf("ZGEARS FAILED run=%s operation=glXMakeCurrent\n", options.token);
		return 1;
	}

	/* What GL says it is, and the gears' lists. */
	printf("ZGEARS START run=%s vendor=\"%s\" renderer=\"%s\" version=\"%s\"\n", options.token,
	       (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER),
	       (const char *)glGetString(GL_VERSION));
	fflush(stdout);
	gears_setup(lists);

	/*
	 * Each frame at the window's size, the gears turned 70 degrees a second
	 * (the first frame at 2, for its check; turning by time, not by frame,
	 * the picture differs at any two moments) (--frames=0: until the window
	 * is closed).
	 */
	failures = 0;
	started = gears_now();
	begun = started;
	memset(&watchdog, 0, sizeof(watchdog));
	watchdog.sa_handler = gears_watchdog;
	watchdog.sa_flags = SA_RESTART;
	(void)sigaction(SIGALRM, &watchdog, NULL);
	for (frame = 1U; frame <= options.frames || options.frames == 0U; frame++) {
		/* The watchdog, armed again each frame. */
		gears_frame = (sig_atomic_t)frame;
		(void)alarm(GEARS_WATCHDOG_SECONDS);

		/* The events waiting (a closed connection ends the program in XPending). */
		gears_step = GEARS_STEP_EVENTS;
		pending = XPending(display);
		while (pending > 0) {
			(void)XNextEvent(display, &event);
			pending = XPending(display);
		}

		/* The window's size now. */
		gears_step = GEARS_STEP_GEOMETRY;
		width = options.width;
		height = options.height;
		(void)XGetGeometry(display, window, &root, &x, &y, &width, &height, &border, &depth);

		/* The gears, the first frame read back, and shown (the first ten frames say how long each part took). */
		gears_step = GEARS_STEP_DRAW;
		before = gears_now();
		angle = 2.0;
		if (frame > 1U)
			angle = fmod(2.0 + (before - begun) * 70.0, 360.0);
		gears_draw(lists, width, height, (GLfloat)angle);
		if (frame == 1U)
			failures = gears_check(width, height, options.token, frame);
		if (frame == 2U || frame == 50U)
			(void)gears_check(width, height, options.token, frame);
		drawn = gears_now();
		gears_step = GEARS_STEP_SWAP;
		glXSwapBuffers(display, window);
		if (frame <= 10U) {
			printf("ZGEARS FRAME run=%s frame=%u draw_ms=%.1f swap_ms=%.1f\n", options.token, frame,
			       (drawn - before) * 1000.0, (gears_now() - drawn) * 1000.0);
			fflush(stdout);
		}

		/* The rate every 100 frames. */
		if (frame % 100U == 0U) {
			now = gears_now();
			printf("ZGEARS FPS run=%s frames=%u fps=%.1f\n", options.token, frame, 100.0 / (now - started));
			fflush(stdout);
			started = now;
		}

		/* The delay between frames. */
		gears_step = GEARS_STEP_SLEEP;
		gears_sleep(options.delay_ms);
	}

	/* The watchdog is disarmed. */
	(void)alarm(0U);

	/* Succeeded: the context and window go. */
	(void)glXMakeCurrent(display, None, NULL);
	glXDestroyContext(display, context);
	(void)XDestroyWindow(display, window);
	(void)XCloseDisplay(display);
	printf("ZGEARS DONE run=%s frames=%u failures=%d\n", options.token, options.frames, failures);
	return 0;
}

/* Reads the command line; returns nonzero for a malformed one. */
static int
gears_parse(
	int argc,
	char **argv,
	struct gears_options *options)
{
	char *end;
	int index;
	int scanned;
	int differs;

	/* The defaults: 600x480, 1000 frames, no delay. */
	options->token = "gears";
	options->width = 600U;
	options->height = 480U;
	options->frames = 1000U;
	options->delay_ms = 0U;

	/* Each option. */
	for (index = 1; index < argc; index++) {
		/* The window's size. */
		differs = strncmp(argv[index], "--size=", 7U);
		if (differs == 0) {
			scanned = sscanf(argv[index] + 7, "%ux%u", &options->width, &options->height);
			if (scanned != 2 || options->width == 0U || options->height == 0U)
				return -1;
			continue;
		}

		/* How many frames. */
		differs = strncmp(argv[index], "--frames=", 9U);
		if (differs == 0) {
			options->frames = (unsigned)strtoul(argv[index] + 9, &end, 10);
			if (*end != '\0')
				return -1;
			continue;
		}

		/* The delay between frames. */
		differs = strncmp(argv[index], "--delay-ms=", 11U);
		if (differs == 0) {
			options->delay_ms = (unsigned)strtoul(argv[index] + 11, &end, 10);
			if (*end != '\0')
				return -1;
			continue;
		}

		/* The name of the run in the log lines. */
		differs = strncmp(argv[index], "--token=", 8U);
		if (differs == 0) {
			options->token = argv[index] + 8;
			continue;
		}

		/* An unknown option refuses the command line. */
		return -1;
	}

	/* Succeeded: the options. */
	return 0;
}

/* Sets up the light and the depth test, and makes the three gears' lists. */
static void
gears_setup(
	GLuint *lists)
{
	static const GLfloat light[4] = { 5.0f, 5.0f, 10.0f, 0.0f };
	static const GLfloat red[4] = { 0.8f, 0.1f, 0.0f, 1.0f };
	static const GLfloat green[4] = { 0.0f, 0.8f, 0.2f, 1.0f };
	static const GLfloat blue[4] = { 0.2f, 0.2f, 1.0f, 1.0f };
	static const struct gears_shape shapes[3] = {
		{ 1.0f, 4.0f, 1.0f, 20U, 0.7f },
		{ 0.5f, 2.0f, 2.0f, 10U, 0.7f },
		{ 1.3f, 2.0f, 0.5f, 10U, 0.7f }
	};
	GLuint first;

	/* One light from above right, in front; the depth test, culling, normals made unit. */
	glLightfv(GL_LIGHT0, GL_POSITION, light);
	glEnable(GL_CULL_FACE);
	glEnable(GL_LIGHTING);
	glEnable(GL_LIGHT0);
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_NORMALIZE);

	/* The three gears, red, green and blue. */
	first = glGenLists(3);
	lists[0] = first;
	lists[1] = first + 1U;
	lists[2] = first + 2U;
	gears_make(lists[0], &shapes[0], red);
	gears_make(lists[1], &shapes[1], green);
	gears_make(lists[2], &shapes[2], blue);
}

/* Compiles a gear of a shape and colour into a display list. */
static void
gears_make(
	GLuint list,
	const struct gears_shape *shape,
	const GLfloat *colour)
{
	/* The material, the flat faces and teeth, then the smooth hole. */
	glNewList(list, GL_COMPILE);
	glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, colour);
	glShadeModel(GL_FLAT);
	glNormal3f(0.0f, 0.0f, 1.0f);
	gears_faces(shape, shape->width * 0.5f);
	glNormal3f(0.0f, 0.0f, -1.0f);
	gears_faces(shape, -shape->width * 0.5f);
	gears_outside(shape);
	glShadeModel(GL_SMOOTH);
	gears_inside(shape);
	glEndList();
}

/*
 * Draws a gear's face at z (the front at +width/2, the back at -width/2):
 * the ring between the hole and the teeth's roots, and the teeth, each
 * counter-clockwise seen from its side.
 */
static void
gears_faces(
	const struct gears_shape *shape,
	GLfloat z)
{
	GLfloat root;
	GLfloat tip;
	GLfloat step;
	GLfloat angle;
	unsigned tooth;
	int front;

	/* The roots and tips of the teeth, and a quarter of a tooth's angle. */
	root = shape->outer - shape->depth * 0.5f;
	tip = shape->outer + shape->depth * 0.5f;
	step = 2.0f * GEARS_PI / (GLfloat)shape->teeth / 4.0f;
	front = 0;
	if (z > 0.0f)
		front = 1;

	/* The ring: hole and root, round the gear (the back one the other way). */
	glBegin(GL_QUAD_STRIP);
	for (tooth = 0U; tooth <= shape->teeth; tooth++) {
		angle = (GLfloat)tooth * 2.0f * GEARS_PI / (GLfloat)shape->teeth;
		if (front) {
			glVertex3f(shape->inner * cosf(angle), shape->inner * sinf(angle), z);
			glVertex3f(root * cosf(angle), root * sinf(angle), z);
		} else {
			glVertex3f(root * cosf(angle), root * sinf(angle), z);
			glVertex3f(shape->inner * cosf(angle), shape->inner * sinf(angle), z);
		}
	}

	/* The teeth. */
	glEnd();

	/* The teeth: up, across the tip, down. */
	glBegin(GL_QUADS);
	for (tooth = 0U; tooth < shape->teeth; tooth++) {
		angle = (GLfloat)tooth * 2.0f * GEARS_PI / (GLfloat)shape->teeth;
		if (front) {
			glVertex3f(root * cosf(angle), root * sinf(angle), z);
			glVertex3f(tip * cosf(angle + step), tip * sinf(angle + step), z);
			glVertex3f(tip * cosf(angle + 2.0f * step), tip * sinf(angle + 2.0f * step), z);
			glVertex3f(root * cosf(angle + 3.0f * step), root * sinf(angle + 3.0f * step), z);
		} else {
			glVertex3f(root * cosf(angle + 3.0f * step), root * sinf(angle + 3.0f * step), z);
			glVertex3f(tip * cosf(angle + 2.0f * step), tip * sinf(angle + 2.0f * step), z);
			glVertex3f(tip * cosf(angle + step), tip * sinf(angle + step), z);
			glVertex3f(root * cosf(angle), root * sinf(angle), z);
		}
	}

	/* Drawn. */
	glEnd();
}

/* Draws the outward faces of the teeth: up each side, the tip, down, and the gap to the next tooth. */
static void
gears_outside(
	const struct gears_shape *shape)
{
	GLfloat root;
	GLfloat tip;
	GLfloat step;
	GLfloat angle;
	GLfloat half;
	GLfloat dx;
	GLfloat dy;
	unsigned tooth;

	/* The roots and tips, a quarter tooth, half the width. */
	root = shape->outer - shape->depth * 0.5f;
	tip = shape->outer + shape->depth * 0.5f;
	step = 2.0f * GEARS_PI / (GLfloat)shape->teeth / 4.0f;
	half = shape->width * 0.5f;

	/* Each tooth's four faces and the gap after it, front edge then back edge. */
	glBegin(GL_QUAD_STRIP);
	for (tooth = 0U; tooth < shape->teeth; tooth++) {
		angle = (GLfloat)tooth * 2.0f * GEARS_PI / (GLfloat)shape->teeth;

		/* Up the rising side (its normal across it, outward). */
		glVertex3f(root * cosf(angle), root * sinf(angle), half);
		glVertex3f(root * cosf(angle), root * sinf(angle), -half);
		dx = tip * cosf(angle + step) - root * cosf(angle);
		dy = tip * sinf(angle + step) - root * sinf(angle);
		glNormal3f(dy, -dx, 0.0f);
		glVertex3f(tip * cosf(angle + step), tip * sinf(angle + step), half);
		glVertex3f(tip * cosf(angle + step), tip * sinf(angle + step), -half);

		/* Across the tip. */
		glNormal3f(cosf(angle), sinf(angle), 0.0f);
		glVertex3f(tip * cosf(angle + 2.0f * step), tip * sinf(angle + 2.0f * step), half);
		glVertex3f(tip * cosf(angle + 2.0f * step), tip * sinf(angle + 2.0f * step), -half);

		/* Down the falling side. */
		dx = root * cosf(angle + 3.0f * step) - tip * cosf(angle + 2.0f * step);
		dy = root * sinf(angle + 3.0f * step) - tip * sinf(angle + 2.0f * step);
		glNormal3f(dy, -dx, 0.0f);
		glVertex3f(root * cosf(angle + 3.0f * step), root * sinf(angle + 3.0f * step), half);
		glVertex3f(root * cosf(angle + 3.0f * step), root * sinf(angle + 3.0f * step), -half);

		/* Along the root to the next tooth. */
		glNormal3f(cosf(angle), sinf(angle), 0.0f);
	}

	/* Closed at the first tooth's root. */
	glVertex3f(root, 0.0f, half);
	glVertex3f(root, 0.0f, -half);
	glEnd();
}

/* Draws the hole's inside, shaded smooth: its normals point to the axis. */
static void
gears_inside(
	const struct gears_shape *shape)
{
	GLfloat angle;
	GLfloat half;
	unsigned tooth;

	/* Round the hole, back edge then front edge. */
	half = shape->width * 0.5f;
	glBegin(GL_QUAD_STRIP);
	for (tooth = 0U; tooth <= shape->teeth; tooth++) {
		angle = (GLfloat)tooth * 2.0f * GEARS_PI / (GLfloat)shape->teeth;
		glNormal3f(-cosf(angle), -sinf(angle), 0.0f);
		glVertex3f(shape->inner * cosf(angle), shape->inner * sinf(angle), -half);
		glVertex3f(shape->inner * cosf(angle), shape->inner * sinf(angle), half);
	}

	/* Drawn. */
	glEnd();
}

/* Draws a frame: the projection for the size, the view turned, and each gear at its place and angle. */
static void
gears_draw(
	const GLuint *lists,
	unsigned width,
	unsigned height,
	GLfloat angle)
{
	GLfloat aspect;

	/* The whole window, and a frustum of its shape. */
	aspect = (GLfloat)height / (GLfloat)width;
	glViewport(0, 0, (GLsizei)width, (GLsizei)height);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glFrustum(-1.0, 1.0, -(GLdouble)aspect, (GLdouble)aspect, 5.0, 60.0);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glTranslatef(0.0f, 0.0f, -40.0f);

	/* Black, and the view from a little above and to the side. */
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glPushMatrix();
	glRotatef(20.0f, 1.0f, 0.0f, 0.0f);
	glRotatef(30.0f, 0.0f, 1.0f, 0.0f);

	/* The big red gear, and the two small ones turning against it. */
	glPushMatrix();
	glTranslatef(-3.0f, -2.0f, 0.0f);
	glRotatef(angle, 0.0f, 0.0f, 1.0f);
	glCallList(lists[0]);
	glPopMatrix();
	glPushMatrix();
	glTranslatef(3.1f, -2.0f, 0.0f);
	glRotatef(-2.0f * angle - 9.0f, 0.0f, 0.0f, 1.0f);
	glCallList(lists[1]);
	glPopMatrix();
	glPushMatrix();
	glTranslatef(-3.1f, 4.2f, 0.0f);
	glRotatef(-2.0f * angle - 25.0f, 0.0f, 0.0f, 1.0f);
	glCallList(lists[2]);
	glPopMatrix();
	glPopMatrix();
}

/*
 * Reads the frame back and counts red, green, blue and black pixels; each
 * gear must cover at least 1% of the window and the background 20%.
 * Returns how many of the four fall short.
 */
static int
gears_check(
	unsigned width,
	unsigned height,
	const char *token,
	unsigned frame)
{
	uint32_t sum;
	unsigned char *pixels;
	unsigned char *pixel;
	unsigned long counts[4];
	unsigned long total;
	unsigned long index;
	unsigned red;
	unsigned green;
	unsigned blue;
	int failures;
	unsigned which;

	/* The pixels. */
	total = (unsigned long)width * height;
	pixels = malloc(total * 4UL);
	if (pixels == NULL)
		return 4;
	glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

	/* Each pixel: mostly red, mostly green, mostly blue, or black. */
	memset(counts, 0, sizeof(counts));
	sum = 2166136261U;
	for (index = 0UL; index < total; index++) {
		pixel = pixels + index * 4UL;
		sum = (sum ^ ((uint32_t)pixel[0] | (uint32_t)pixel[1] << 8 | (uint32_t)pixel[2] << 16)) * 16777619U;
		red = pixel[0];
		green = pixel[1];
		blue = pixel[2];
		if (red > 60U && red > 2U * green && red > 2U * blue)
			counts[0]++;
		else if (green > 60U && green > 2U * red && green > 2U * blue)
			counts[1]++;
		else if (blue > 60U && 2U * blue > 3U * red && 2U * blue > 3U * green)
			counts[2]++;
		else if (red < 10U && green < 10U && blue < 10U)
			counts[3]++;
	}

	/* The pixels are counted. */
	free(pixels);

	/* Enough of each. */
	failures = 0;
	for (which = 0U; which < 3U; which++) {
		if (counts[which] * 100UL < total)
			failures++;
	}

	/* Enough background. */
	if (counts[3] * 5UL < total)
		failures++;
	if (frame == 1U) {
		printf("ZGEARS CHECK run=%s red=%lu green=%lu blue=%lu black=%lu of=%lu failures=%d glerror=0x%x\n", token,
		       counts[0], counts[1], counts[2], counts[3], total, failures, (unsigned)glGetError());
	}

	/* Every frame read back has its sum said, so frames that differ can be told apart. */
	printf("ZGEARS SUM run=%s frame=%u sum=%08x red=%lu green=%lu blue=%lu\n", token, frame, (unsigned)sum,
	       counts[0], counts[1], counts[2]);
	fflush(stdout);
	return failures;
}

/* Returns the monotonic time in seconds. */
static double
gears_now(void)
{
	struct timespec now;

	/* The clock. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

/* Sleeps some milliseconds. */
static void
gears_sleep(
	unsigned milliseconds)
{
	struct timespec delay;

	/* Nothing to wait. */
	if (milliseconds == 0U)
		return;

	/* The delay. */
	delay.tv_sec = (time_t)(milliseconds / 1000U);
	delay.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
	(void)nanosleep(&delay, NULL);
}

/*
 * Says, from SIGALRM, the frame that has taken too long and the step it is
 * at (with libGL's step inside glXSwapBuffers), with async-signal-safe calls
 * only; armed again for a few more reports.
 */
static void
gears_watchdog(
	int number)
{
	char line[96];
	char *cursor;
	size_t length;

	/* The line: ZGEARS STALL frame=N step=S swap_step=W. */
	(void)number;
	memcpy(line, "ZGEARS STALL frame=", 19U);
	cursor = gears_decimal(line + 19, (unsigned long)gears_frame);
	memcpy(cursor, " step=", 6U);
	cursor = gears_decimal(cursor + 6, (unsigned long)gears_step);
	memcpy(cursor, " swap_step=", 11U);
	cursor = gears_decimal(cursor + 11, (unsigned long)zglx_swap_step);
	*cursor++ = '\n';
	length = (size_t)(cursor - line);
	(void)write(STDERR_FILENO, line, length);

	/* Again, a few times, while the frame stays stuck. */
	gears_reports++;
	if (gears_reports < GEARS_WATCHDOG_REPORTS)
		(void)alarm(GEARS_WATCHDOG_SECONDS);
}

/* Writes a number in decimal at cursor and returns the end (async-signal-safe). */
static char *
gears_decimal(
	char *cursor,
	unsigned long value)
{
	char digits[24];
	unsigned count;

	/* The digits, lowest first. */
	count = 0U;
	do {
		digits[count++] = (char)('0' + value % 10UL);
		value /= 10UL;
	} while (value != 0UL);

	/* Highest first. */
	while (count > 0U)
		*cursor++ = digits[--count];

	/* The end. */
	return cursor;
}
