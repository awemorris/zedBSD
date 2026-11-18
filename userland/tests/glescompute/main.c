/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * glescompute: the test of OpenGL ES 3.1's compute in libGLESv2
 * (ws101-p009).  It makes a pbuffer context the way Noct's OpenGL ES
 * backend does (the surfaceless platform first, the default display
 * otherwise; an OpenGL ES 3 context over a 1x1 pbuffer), checks the
 * version and the compute limits, then runs compute shaders over shader
 * storage buffers and compares what they wrote with the same sums done on
 * the CPU: plain arithmetic with a uniform, the shape of Noct's shaders
 * (integer division, remainders, shifts, the conditional operator, an
 * atomic sum), a workgroup's shared memory with a barrier, an indirect
 * dispatch, two dispatches chained through a buffer, a dispatch left
 * recorded across releasing and taking the context again, many dispatches
 * each with new buffer storage, and the errors of refused calls.
 *
 * Each step prints "GLESCOMPUTE <step> PASS" or "... FAIL <why>", and the
 * run ends with "GLESCOMPUTE DONE failures=N"; the exit status is 0 only
 * when nothing failed.  --no-indirect leaves the indirect step out (a
 * device whose executor has no indirect dispatch yet: ws101-p010's i915).
 *
 *   glescompute [--platform=auto|surfaceless|default] [--repeat=N] [--no-indirect]
 */

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The number of elements the shaders run over, and their workgroup size. */
#define GLESCOMPUTE_COUNT	1000U
#define GLESCOMPUTE_LOCAL	64U

/* The workgroups that cover the elements. */
#define GLESCOMPUTE_GROUPS	((GLESCOMPUTE_COUNT + GLESCOMPUTE_LOCAL - 1U) / GLESCOMPUTE_LOCAL)

/* The default number of dispatches of the repeat step. */
#define GLESCOMPUTE_REPEAT	100

/* The bytes of the repeat step's buffers (new storage each time). */
#define GLESCOMPUTE_REPEAT_BYTES (256U * 1024U)

/* The platforms the display may come from. */
#define GLESCOMPUTE_PLATFORM_AUTO	 0
#define GLESCOMPUTE_PLATFORM_SURFACELESS 1
#define GLESCOMPUTE_PLATFORM_DEFAULT	 2

/* The surfaceless platform's number (EGL_MESA_platform_surfaceless), when the header lacks it. */
#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA	0x31DD
#endif

/*
 * The EGL objects of the run: the display (and the platform it came
 * from), the context, and the pbuffer it draws into.
 */
struct glescompute_egl {
	EGLDisplay display;
	const char *platform;
	EGLConfig config;
	EGLContext context;
	EGLSurface surface;
};

/*
 * The plain arithmetic shader: c[i] = a[i] + b[i] * scale for the first
 * count elements (the rest of the last workgroup returns early).
 */
static const char glescompute_add_source[] =
	"#version 310 es\n"
	"layout(local_size_x = 64) in;\n"
	"layout(std430, binding = 0) readonly buffer A { uint a[]; };\n"
	"layout(std430, binding = 1) readonly buffer B { uint b[]; };\n"
	"layout(std430, binding = 2) buffer C { uint c[]; };\n"
	"uniform uint u_scale;\n"
	"uniform uint u_count;\n"
	"void main()\n"
	"{\n"
	"	uint i = gl_GlobalInvocationID.x;\n"
	"	if (i >= u_count)\n"
	"		return;\n"
	"	c[i] = a[i] + b[i] * u_scale;\n"
	"}\n";

/*
 * The shape of Noct's shaders: words of buffers with instance names,
 * signed division through int, remainders, shifts, a comparison with the
 * conditional operator, and an atomic sum into the result's word 0.
 */
static const char glescompute_noct_source[] =
	"#version 310 es\n"
	"precision highp float;\n"
	"precision highp int;\n"
	"layout(local_size_x = 64) in;\n"
	"layout(std430, binding = 0) readonly buffer B0 { uint word[]; } b0;\n"
	"layout(std430, binding = 1) coherent buffer B1 { uint word[]; } result;\n"
	"void main()\n"
	"{\n"
	"	uint i = gl_GlobalInvocationID.x;\n"
	"	if (i >= 1000u)\n"
	"		return;\n"
	"	uint a = b0.word[i];\n"
	"	uint v = uint(int(a) / int(3)) + (a % 7u) + (a << 2u) ^ (a >> 1u);\n"
	"	v = (a > 500u) ? v : v + 1u;\n"
	"	atomicAdd(result.word[0], v);\n"
	"	result.word[i + 1u] = v;\n"
	"}\n";

/*
 * A workgroup's sum through shared memory: each invocation stores its
 * element, the barrier, then the first invocation adds the 64 and writes
 * the workgroup's sum.
 */
static const char glescompute_shared_source[] =
	"#version 310 es\n"
	"layout(local_size_x = 64) in;\n"
	"layout(std430, binding = 0) readonly buffer In { uint v[]; } src;\n"
	"layout(std430, binding = 1) writeonly buffer Out { uint sums[]; } dst;\n"
	"shared uint partial[64];\n"
	"void main()\n"
	"{\n"
	"	uint l = gl_LocalInvocationIndex;\n"
	"	partial[l] = src.v[gl_GlobalInvocationID.x];\n"
	"	barrier();\n"
	"	if (l == 0u) {\n"
	"		uint total = 0u;\n"
	"		for (uint k = 0u; k < 64u; k++)\n"
	"			total += partial[k];\n"
	"		dst.sums[gl_WorkGroupID.x] = total;\n"
	"	}\n"
	"}\n";

/* A vertex shader, which a compute program may not be linked with. */
static const char glescompute_vertex_source[] =
	"#version 310 es\n"
	"void main()\n"
	"{\n"
	"	gl_Position = vec4(0.0);\n"
	"}\n";

/* The steps that failed so far. */
static int glescompute_failures;

static int glescompute_options(int argc, char **argv, int *platform, int *repeat, int *indirect);
static int glescompute_platform(const char *name);
static int glescompute_open(struct glescompute_egl *egl, int platform);
static void glescompute_close(struct glescompute_egl *egl);
static void glescompute_result(const char *step, int passed, const char *why);
static void glescompute_version(void);
static void glescompute_limits(void);
static GLuint glescompute_program(const char *source, const char *step);
static GLuint glescompute_buffer(const uint32_t *words, size_t count);
static int glescompute_read(GLuint buffer, uint32_t *words, size_t count);
static void glescompute_inputs(uint32_t *a, uint32_t *b, size_t count);
static void glescompute_add(void);
static void glescompute_noct(void);
static void glescompute_shared(void);
static void glescompute_indirect(void);
static void glescompute_chain(void);
static void glescompute_release(struct glescompute_egl *egl);
static void glescompute_repeat(int repeat);
static void glescompute_errors(void);
static int glescompute_add_check(const uint32_t *a, const uint32_t *b, uint32_t scale, const uint32_t *c, size_t count);

/*
 * Runs every step in a pbuffer context and reports how many failed.
 */
int
main(
	int argc,
	char **argv)
{
	struct glescompute_egl egl;
	const char *renderer;
	int platform;
	int repeat;
	int indirect;
	int status;

	/* The options. */
	status = glescompute_options(argc, argv, &platform, &repeat, &indirect);
	if (status != 0)
		return 2;

	/* The context, current. */
	status = glescompute_open(&egl, platform);
	if (status != 0) {
		printf("GLESCOMPUTE DONE failures=1 (no context)\n");
		return 1;
	}

	/* Which platform gave it, and the renderer. */
	renderer = (const char *)glGetString(GL_RENDERER);
	printf("GLESCOMPUTE CONTEXT platform=%s renderer=%s\n", egl.platform, renderer);
	fflush(stdout);

	/* The version and the limits, then the first shaders. */
	glescompute_version();
	glescompute_limits();
	glescompute_add();
	glescompute_noct();
	glescompute_shared();

	/* The indirect step, unless it is left out. */
	if (indirect) {
		glescompute_indirect();
	} else {
		printf("GLESCOMPUTE indirect SKIP\n");
	}

	/* The rest: chained dispatches, the release, the repeats, the refused calls. */
	glescompute_chain();
	glescompute_release(&egl);
	glescompute_repeat(repeat);
	glescompute_errors();

	/* The context goes. */
	glescompute_close(&egl);

	/* Reports whether every step passed. */
	printf("GLESCOMPUTE DONE failures=%d\n", glescompute_failures);
	fflush(stdout);
	if (glescompute_failures != 0)
		return 1;
	return 0;
}

/* Reads the options; nonzero after printing the usage for one that is not known. */
static int
glescompute_options(
	int argc,
	char **argv,
	int *platform,
	int *repeat,
	int *indirect)
{
	int index;
	int differs;

	/* The defaults: Noct's order of platforms, the default number of repeats, every step. */
	*platform = GLESCOMPUTE_PLATFORM_AUTO;
	*repeat = GLESCOMPUTE_REPEAT;
	*indirect = 1;

	/* Each option. */
	for (index = 1; index < argc; index++) {
		/* The platform. */
		differs = strncmp(argv[index], "--platform=", 11U);
		if (differs == 0) {
			*platform = glescompute_platform(argv[index] + 11);
			if (*platform < 0)
				break;
			continue;
		}

		/* The rounds of the repeat step. */
		differs = strncmp(argv[index], "--repeat=", 9U);
		if (differs == 0) {
			*repeat = atoi(argv[index] + 9);
			continue;
		}

		/* The indirect step left out. */
		differs = strcmp(argv[index], "--no-indirect");
		if (differs == 0) {
			*indirect = 0;
			continue;
		}

		/* Anything else. */
		break;
	}

	/* An option not known, or a platform that is not one. */
	if (index < argc) {
		fprintf(stderr, "usage: glescompute [--platform=auto|surfaceless|default] [--repeat=N] [--no-indirect]\n");
		return -1;
	}

	/* Succeeded: the options. */
	return 0;
}

/* Returns the platform of a name (auto, surfaceless, default), or -1. */
static int
glescompute_platform(
	const char *name)
{
	int differs;

	/* Noct's order. */
	differs = strcmp(name, "auto");
	if (differs == 0)
		return GLESCOMPUTE_PLATFORM_AUTO;

	/* The surfaceless platform alone. */
	differs = strcmp(name, "surfaceless");
	if (differs == 0)
		return GLESCOMPUTE_PLATFORM_SURFACELESS;

	/* The default display alone. */
	differs = strcmp(name, "default");
	if (differs == 0)
		return GLESCOMPUTE_PLATFORM_DEFAULT;

	/* Not a platform. */
	return -1;
}

/*
 * Makes the display, an OpenGL ES 3 context and a 1x1 pbuffer, and makes
 * them current: the surfaceless platform first (auto), as Noct does.
 * Returns 0, or -1 after printing what failed.
 */
static int
glescompute_open(
	struct glescompute_egl *egl,
	int platform)
{
	static const EGLint config_attributes[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
		EGL_NONE
	};
	static const EGLint context_attributes[] = {
		EGL_CONTEXT_CLIENT_VERSION, 3,
		EGL_NONE
	};
	static const EGLint pbuffer_attributes[] = {
		EGL_WIDTH, 1,
		EGL_HEIGHT, 1,
		EGL_NONE
	};
	EGLint major;
	EGLint minor;
	EGLint count;
	EGLBoolean done;

	/* The display: surfaceless, else (auto) the default one. */
	memset(egl, 0, sizeof(*egl));
	egl->display = EGL_NO_DISPLAY;
	if (platform != GLESCOMPUTE_PLATFORM_DEFAULT) {
		egl->platform = "surfaceless";
		egl->display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL);
	}

	/* The default display, asked for or when there was no surfaceless one. */
	if (egl->display == EGL_NO_DISPLAY && platform != GLESCOMPUTE_PLATFORM_SURFACELESS) {
		egl->platform = "default";
		egl->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	}

	/* No display at all. */
	if (egl->display == EGL_NO_DISPLAY) {
		printf("GLESCOMPUTE FAIL no display (0x%x)\n", (unsigned)eglGetError());
		return -1;
	}

	/* EGL itself, for OpenGL ES. */
	done = eglInitialize(egl->display, &major, &minor);
	if (!done) {
		printf("GLESCOMPUTE FAIL eglInitialize (0x%x)\n", (unsigned)eglGetError());
		return -1;
	}

	/* OpenGL ES is the API contexts are made for. */
	done = eglBindAPI(EGL_OPENGL_ES_API);
	if (!done) {
		printf("GLESCOMPUTE FAIL eglBindAPI (0x%x)\n", (unsigned)eglGetError());
		return -1;
	}

	/* A pbuffer config of OpenGL ES 3. */
	done = eglChooseConfig(egl->display, config_attributes, &egl->config, 1, &count);
	if (!done || count < 1) {
		printf("GLESCOMPUTE FAIL eglChooseConfig (0x%x)\n", (unsigned)eglGetError());
		return -1;
	}

	/* The context. */
	egl->context = eglCreateContext(egl->display, egl->config, EGL_NO_CONTEXT, context_attributes);
	if (egl->context == EGL_NO_CONTEXT) {
		printf("GLESCOMPUTE FAIL eglCreateContext (0x%x)\n", (unsigned)eglGetError());
		return -1;
	}

	/* The pbuffer, which nothing shows. */
	egl->surface = eglCreatePbufferSurface(egl->display, egl->config, pbuffer_attributes);
	if (egl->surface == EGL_NO_SURFACE) {
		printf("GLESCOMPUTE FAIL eglCreatePbufferSurface (0x%x)\n", (unsigned)eglGetError());
		return -1;
	}

	/* Current. */
	done = eglMakeCurrent(egl->display, egl->surface, egl->surface, egl->context);
	if (!done) {
		printf("GLESCOMPUTE FAIL eglMakeCurrent (0x%x)\n", (unsigned)eglGetError());
		return -1;
	}

	/* Succeeded: the context is current. */
	return 0;
}

/* Releases the context and lets the EGL objects go. */
static void
glescompute_close(
	struct glescompute_egl *egl)
{
	/* Not current, then the objects and EGL. */
	(void)eglMakeCurrent(egl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	(void)eglDestroySurface(egl->display, egl->surface);
	(void)eglDestroyContext(egl->display, egl->context);
	(void)eglTerminate(egl->display);
}

/* Prints a step's outcome and counts a failure. */
static void
glescompute_result(
	const char *step,
	int passed,
	const char *why)
{
	/* A failure says why. */
	if (!passed) {
		glescompute_failures++;
		printf("GLESCOMPUTE %s FAIL %s\n", step, why);
	} else {
		printf("GLESCOMPUTE %s PASS\n", step);
	}

	/* Out before the next step (a crash would lose it). */
	fflush(stdout);
}

/* Checks that the context names itself OpenGL ES 3.1 with GLSL ES 3.10. */
static void
glescompute_version(void)
{
	const char *version;
	const char *language;
	GLint major;
	GLint minor;
	GLenum error;
	int passed;
	int differs;

	/* The strings and the numbers. */
	version = (const char *)glGetString(GL_VERSION);
	if (version == NULL)
		version = "";
	language = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
	if (language == NULL)
		language = "";
	major = 0;
	minor = 0;
	glGetIntegerv(GL_MAJOR_VERSION, &major);
	glGetIntegerv(GL_MINOR_VERSION, &minor);
	printf("GLESCOMPUTE VERSION \"%s\" \"%s\" %d.%d\n", version, language, (int)major, (int)minor);

	/* OpenGL ES 3.1 and GLSL ES 3.10. */
	passed = 1;
	differs = strncmp(version, "OpenGL ES 3.1", 13U);
	if (differs != 0)
		passed = 0;
	differs = strcmp(language, "OpenGL ES GLSL ES 3.10");
	if (differs != 0)
		passed = 0;
	if (major != 3 || minor != 1)
		passed = 0;

	/* No error along the way. */
	error = glGetError();
	if (error != GL_NO_ERROR)
		passed = 0;
	glescompute_result("version", passed, "not OpenGL ES 3.1");
}

/* Checks the compute limits Noct reads against OpenGL ES 3.1's minimums. */
static void
glescompute_limits(void)
{
	GLint count[3];
	GLint size[3];
	GLint invocations;
	GLint bindings;
	GLint blocks;
	GLint alignment;
	GLint shared;
	GLint64 block_size;
	GLint index;
	GLenum error;
	int passed;

	/* The workgroups' counts and sizes, one dimension at a time. */
	for (index = 0; index < 3; index++) {
		count[index] = 0;
		size[index] = 0;
		glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, (GLuint)index, &count[index]);
		glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, (GLuint)index, &size[index]);
	}

	/* The other limits. */
	invocations = 0;
	bindings = 0;
	blocks = 0;
	alignment = 0;
	shared = 0;
	block_size = 0;
	glGetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &invocations);
	glGetIntegerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &bindings);
	glGetIntegerv(GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS, &blocks);
	glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &alignment);
	glGetIntegerv(GL_MAX_COMPUTE_SHARED_MEMORY_SIZE, &shared);
	glGetInteger64v(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &block_size);
	printf("GLESCOMPUTE LIMITS count=%d,%d,%d size=%d,%d,%d invocations=%d bindings=%d blocks=%d alignment=%d shared=%d "
	       "block=%lld\n", (int)count[0], (int)count[1], (int)count[2], (int)size[0], (int)size[1], (int)size[2],
	       (int)invocations, (int)bindings, (int)blocks, (int)alignment, (int)shared, (long long)block_size);

	/* Each at least OpenGL ES 3.1's minimum. */
	passed = 1;
	if (count[0] < 65535 || count[1] < 65535 || count[2] < 65535)
		passed = 0;
	if (size[0] < 128 || size[1] < 128 || size[2] < 64)
		passed = 0;
	if (invocations < 128 || bindings < 4 || blocks < 4 || shared < 16384)
		passed = 0;
	if (alignment < 1 || block_size < (GLint64)1 << 27)
		passed = 0;

	/* No error along the way. */
	error = glGetError();
	if (error != GL_NO_ERROR)
		passed = 0;
	glescompute_result("limits", passed, "a limit below OpenGL ES 3.1's minimum, or an error");
}

/*
 * Compiles and links a compute program; 0 after printing the logs when
 * either fails.
 */
static GLuint
glescompute_program(
	const char *source,
	const char *step)
{
	GLuint shader;
	GLuint program;
	GLint compiled;
	GLint linked;
	char log[1024];

	/* The shader, compiled. */
	shader = glCreateShader(GL_COMPUTE_SHADER);
	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	compiled = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
	if (!compiled) {
		log[0] = '\0';
		glGetShaderInfoLog(shader, (GLsizei)sizeof(log), NULL, log);
		printf("GLESCOMPUTE %s compile log: %s\n", step, log);
		glDeleteShader(shader);
		return 0U;
	}

	/* The program, linked; the shader goes with it. */
	program = glCreateProgram();
	glAttachShader(program, shader);
	glLinkProgram(program);
	glDeleteShader(shader);
	linked = 0;
	glGetProgramiv(program, GL_LINK_STATUS, &linked);
	if (!linked) {
		log[0] = '\0';
		glGetProgramInfoLog(program, (GLsizei)sizeof(log), NULL, log);
		printf("GLESCOMPUTE %s link log: %s\n", step, log);
		glDeleteProgram(program);
		return 0U;
	}

	/* Succeeded: the program. */
	return program;
}

/* Makes a shader storage buffer holding words (NULL: zeros), as Noct does (GL_DYNAMIC_COPY). */
static GLuint
glescompute_buffer(
	const uint32_t *words,
	size_t count)
{
	GLuint buffer;
	uint32_t *zeros;

	/* The buffer's storage with the words. */
	glGenBuffers(1, &buffer);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
	if (words != NULL) {
		glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)(count * sizeof(*words)), words, GL_DYNAMIC_COPY);
		return buffer;
	}

	/* Or zeros. */
	zeros = calloc(count, sizeof(*zeros));
	glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)(count * sizeof(*zeros)), zeros, GL_DYNAMIC_COPY);
	free(zeros);

	/* Succeeded: the buffer. */
	return buffer;
}

/* Reads a buffer's words back through a mapping for reading; nonzero when it cannot be mapped. */
static int
glescompute_read(
	GLuint buffer,
	uint32_t *words,
	size_t count)
{
	void *mapped;

	/* The mapping (it waits for what the device wrote). */
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
	mapped = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)(count * sizeof(*words)), GL_MAP_READ_BIT);
	if (mapped == NULL)
		return -1;

	/* The words, then the mapping ends. */
	memcpy(words, mapped, count * sizeof(*words));
	(void)glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
	return 0;
}

/* Fills the two inputs with words of their own. */
static void
glescompute_inputs(
	uint32_t *a,
	uint32_t *b,
	size_t count)
{
	size_t index;

	/* A ramp and a pattern. */
	for (index = 0U; index < count; index++) {
		a[index] = (uint32_t)(index * 3U + 1U);
		b[index] = (uint32_t)((index * 7U) % 13U);
	}
}

/* Compares c with a + b * scale for count elements; nonzero on the first difference (printed). */
static int
glescompute_add_check(
	const uint32_t *a,
	const uint32_t *b,
	uint32_t scale,
	const uint32_t *c,
	size_t count)
{
	size_t index;

	/* Each element. */
	for (index = 0U; index < count; index++) {
		if (c[index] != a[index] + b[index] * scale) {
			printf("GLESCOMPUTE element %u is %u, not %u\n", (unsigned)index, (unsigned)c[index],
			       (unsigned)(a[index] + b[index] * scale));
			return -1;
		}
	}

	/* Every element matches. */
	return 0;
}

/* Runs the plain arithmetic shader with a uniform, and checks every element (and that the last group's rest is untouched). */
static void
glescompute_add(void)
{
	uint32_t a[GLESCOMPUTE_GROUPS * GLESCOMPUTE_LOCAL];
	uint32_t b[GLESCOMPUTE_GROUPS * GLESCOMPUTE_LOCAL];
	uint32_t c[GLESCOMPUTE_GROUPS * GLESCOMPUTE_LOCAL];
	GLuint program;
	GLuint buffers[3];
	GLint size[3];
	size_t index;
	GLenum error;
	int status;
	int passed;

	/* The program. */
	program = glescompute_program(glescompute_add_source, "add");
	if (program == 0U) {
		glescompute_result("add", 0, "the program did not link");
		return;
	}

	/* Its workgroup size as GL reports it. */
	memset(size, 0, sizeof(size));
	glGetProgramiv(program, GL_COMPUTE_WORK_GROUP_SIZE, size);

	/* The inputs and a zeroed output of whole workgroups. */
	glescompute_inputs(a, b, GLESCOMPUTE_GROUPS * GLESCOMPUTE_LOCAL);
	buffers[0] = glescompute_buffer(a, GLESCOMPUTE_GROUPS * GLESCOMPUTE_LOCAL);
	buffers[1] = glescompute_buffer(b, GLESCOMPUTE_GROUPS * GLESCOMPUTE_LOCAL);
	buffers[2] = glescompute_buffer(NULL, GLESCOMPUTE_GROUPS * GLESCOMPUTE_LOCAL);

	/* The dispatch over the elements. */
	glUseProgram(program);
	glUniform1ui(glGetUniformLocation(program, "u_scale"), 5U);
	glUniform1ui(glGetUniformLocation(program, "u_count"), GLESCOMPUTE_COUNT);
	for (index = 0U; index < 3U; index++)
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, (GLuint)index, buffers[index]);
	glDispatchCompute(GLESCOMPUTE_GROUPS, 1U, 1U);
	glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

	/* What it wrote, and nothing past the count. */
	status = glescompute_read(buffers[2], c, GLESCOMPUTE_GROUPS * GLESCOMPUTE_LOCAL);
	if (status == 0)
		status = glescompute_add_check(a, b, 5U, c, GLESCOMPUTE_COUNT);
	passed = 0;
	if (status == 0)
		passed = 1;
	for (index = GLESCOMPUTE_COUNT; passed && index < GLESCOMPUTE_GROUPS * GLESCOMPUTE_LOCAL; index++) {
		if (c[index] != 0U)
			passed = 0;
	}

	/* The workgroup size the shader gave. */
	if (size[0] != (GLint)GLESCOMPUTE_LOCAL || size[1] != 1 || size[2] != 1)
		passed = 0;

	/* No error along the way. */
	error = glGetError();
	if (error != GL_NO_ERROR)
		passed = 0;
	glescompute_result("add", passed, "wrong words, workgroup size or an error");

	/* The objects go. */
	glDeleteBuffers(3, buffers);
	glDeleteProgram(program);
}

/* Runs the shape of Noct's shaders and checks every word and the atomic sum. */
static void
glescompute_noct(void)
{
	uint32_t input[GLESCOMPUTE_COUNT];
	uint32_t output[GLESCOMPUTE_COUNT + 1U];
	uint32_t expected;
	uint32_t sum;
	uint32_t value;
	GLuint program;
	GLuint buffers[2];
	size_t index;
	GLenum error;
	int status;
	int passed;

	/* The program. */
	program = glescompute_program(glescompute_noct_source, "noct");
	if (program == 0U) {
		glescompute_result("noct", 0, "the program did not link");
		return;
	}

	/* The input words, and the result: the sum at word 0 and each element's value after it. */
	for (index = 0U; index < GLESCOMPUTE_COUNT; index++)
		input[index] = (uint32_t)(index * 37U + 11U);
	buffers[0] = glescompute_buffer(input, GLESCOMPUTE_COUNT);
	buffers[1] = glescompute_buffer(NULL, GLESCOMPUTE_COUNT + 1U);

	/* The dispatch. */
	glUseProgram(program);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0U, buffers[0]);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1U, buffers[1]);
	glDispatchCompute(GLESCOMPUTE_GROUPS, 1U, 1U);
	glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

	/* What it wrote. */
	status = glescompute_read(buffers[1], output, GLESCOMPUTE_COUNT + 1U);
	passed = 0;
	if (status == 0)
		passed = 1;

	/* The same sums on the CPU. */
	sum = 0U;
	for (index = 0U; passed && index < GLESCOMPUTE_COUNT; index++) {
		value = input[index];
		expected = ((uint32_t)((int32_t)value / 3) + (value % 7U) + (value << 2U)) ^ (value >> 1U);
		if (value <= 500U)
			expected++;
		sum += expected;
		if (output[index + 1U] != expected) {
			printf("GLESCOMPUTE noct word %u is %u, not %u\n", (unsigned)index, (unsigned)output[index + 1U],
			       (unsigned)expected);
			passed = 0;
		}
	}

	/* The atomic sum of them. */
	if (passed && output[0] != sum) {
		printf("GLESCOMPUTE noct sum is %u, not %u\n", (unsigned)output[0], (unsigned)sum);
		passed = 0;
	}

	/* No error along the way. */
	error = glGetError();
	if (error != GL_NO_ERROR)
		passed = 0;
	glescompute_result("noct", passed, "wrong words or sum, or an error");

	/* The objects go. */
	glDeleteBuffers(2, buffers);
	glDeleteProgram(program);
}

/* Runs a workgroup sum through shared memory and a barrier, and checks each workgroup's sum. */
static void
glescompute_shared(void)
{
	uint32_t input[GLESCOMPUTE_GROUPS * GLESCOMPUTE_LOCAL];
	uint32_t sums[GLESCOMPUTE_GROUPS];
	uint32_t expected;
	GLuint program;
	GLuint buffers[2];
	size_t group;
	size_t index;
	GLenum error;
	int status;
	int passed;

	/* The program. */
	program = glescompute_program(glescompute_shared_source, "shared");
	if (program == 0U) {
		glescompute_result("shared", 0, "the program did not link");
		return;
	}

	/* The input of whole workgroups, and the sums. */
	for (index = 0U; index < GLESCOMPUTE_GROUPS * GLESCOMPUTE_LOCAL; index++)
		input[index] = (uint32_t)(index * index + 3U);
	buffers[0] = glescompute_buffer(input, GLESCOMPUTE_GROUPS * GLESCOMPUTE_LOCAL);
	buffers[1] = glescompute_buffer(NULL, GLESCOMPUTE_GROUPS);

	/* The dispatch. */
	glUseProgram(program);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0U, buffers[0]);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1U, buffers[1]);
	glDispatchCompute(GLESCOMPUTE_GROUPS, 1U, 1U);

	/* Each workgroup's sum. */
	status = glescompute_read(buffers[1], sums, GLESCOMPUTE_GROUPS);
	passed = 0;
	if (status == 0)
		passed = 1;
	for (group = 0U; passed && group < GLESCOMPUTE_GROUPS; group++) {
		expected = 0U;
		for (index = 0U; index < GLESCOMPUTE_LOCAL; index++)
			expected += input[group * GLESCOMPUTE_LOCAL + index];
		if (sums[group] != expected) {
			printf("GLESCOMPUTE shared group %u is %u, not %u\n", (unsigned)group, (unsigned)sums[group],
			       (unsigned)expected);
			passed = 0;
		}
	}

	/* No error along the way. */
	error = glGetError();
	if (error != GL_NO_ERROR)
		passed = 0;
	glescompute_result("shared", passed, "a wrong workgroup sum, or an error");

	/* The objects go. */
	glDeleteBuffers(2, buffers);
	glDeleteProgram(program);
}

/* Runs the plain arithmetic shader with its grid from GL_DISPATCH_INDIRECT_BUFFER at an offset. */
static void
glescompute_indirect(void)
{
	uint32_t a[GLESCOMPUTE_GROUPS * GLESCOMPUTE_LOCAL];
	uint32_t b[GLESCOMPUTE_GROUPS * GLESCOMPUTE_LOCAL];
	uint32_t c[GLESCOMPUTE_GROUPS * GLESCOMPUTE_LOCAL];
	uint32_t grid[4];
	GLuint program;
	GLuint buffers[3];
	GLuint sizes;
	size_t index;
	GLenum error;
	int status;
	int passed;

	/* The program. */
	program = glescompute_program(glescompute_add_source, "indirect");
	if (program == 0U) {
		glescompute_result("indirect", 0, "the program did not link");
		return;
	}

	/* The buffers, and the grid one word in. */
	glescompute_inputs(a, b, GLESCOMPUTE_GROUPS * GLESCOMPUTE_LOCAL);
	buffers[0] = glescompute_buffer(a, GLESCOMPUTE_GROUPS * GLESCOMPUTE_LOCAL);
	buffers[1] = glescompute_buffer(b, GLESCOMPUTE_GROUPS * GLESCOMPUTE_LOCAL);
	buffers[2] = glescompute_buffer(NULL, GLESCOMPUTE_GROUPS * GLESCOMPUTE_LOCAL);
	grid[0] = 0xdeadbeefU;
	grid[1] = GLESCOMPUTE_GROUPS;
	grid[2] = 1U;
	grid[3] = 1U;
	glGenBuffers(1, &sizes);
	glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, sizes);
	glBufferData(GL_DISPATCH_INDIRECT_BUFFER, (GLsizeiptr)sizeof(grid), grid, GL_STATIC_DRAW);

	/* The dispatch. */
	glUseProgram(program);
	glUniform1ui(glGetUniformLocation(program, "u_scale"), 9U);
	glUniform1ui(glGetUniformLocation(program, "u_count"), GLESCOMPUTE_COUNT);
	for (index = 0U; index < 3U; index++)
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, (GLuint)index, buffers[index]);
	glDispatchComputeIndirect((GLintptr)sizeof(uint32_t));

	/* What it wrote. */
	status = glescompute_read(buffers[2], c, GLESCOMPUTE_GROUPS * GLESCOMPUTE_LOCAL);
	if (status == 0)
		status = glescompute_add_check(a, b, 9U, c, GLESCOMPUTE_COUNT);
	passed = 0;
	if (status == 0)
		passed = 1;

	/* No error along the way. */
	error = glGetError();
	if (error != GL_NO_ERROR)
		passed = 0;
	glescompute_result("indirect", passed, "wrong words, or an error");

	/* The objects go. */
	glDeleteBuffers(3, buffers);
	glDeleteBuffers(1, &sizes);
	glDeleteProgram(program);
}

/*
 * Runs the plain arithmetic shader twice, the second over the first's
 * output (a barrier between, nothing read back), and checks the second's
 * output: (a + b * 2) + b * 3.
 */
static void
glescompute_chain(void)
{
	uint32_t a[GLESCOMPUTE_GROUPS * GLESCOMPUTE_LOCAL];
	uint32_t b[GLESCOMPUTE_GROUPS * GLESCOMPUTE_LOCAL];
	uint32_t c[GLESCOMPUTE_GROUPS * GLESCOMPUTE_LOCAL];
	uint32_t d[GLESCOMPUTE_GROUPS * GLESCOMPUTE_LOCAL];
	GLuint program;
	GLuint buffers[4];
	GLint scale;
	size_t index;
	GLenum error;
	int status;
	int passed;

	/* The program. */
	program = glescompute_program(glescompute_add_source, "chain");
	if (program == 0U) {
		glescompute_result("chain", 0, "the program did not link");
		return;
	}

	/* The inputs, the first output and the second. */
	glescompute_inputs(a, b, GLESCOMPUTE_GROUPS * GLESCOMPUTE_LOCAL);
	buffers[0] = glescompute_buffer(a, GLESCOMPUTE_GROUPS * GLESCOMPUTE_LOCAL);
	buffers[1] = glescompute_buffer(b, GLESCOMPUTE_GROUPS * GLESCOMPUTE_LOCAL);
	buffers[2] = glescompute_buffer(NULL, GLESCOMPUTE_GROUPS * GLESCOMPUTE_LOCAL);
	buffers[3] = glescompute_buffer(NULL, GLESCOMPUTE_GROUPS * GLESCOMPUTE_LOCAL);

	/* The first: c = a + b * 2. */
	glUseProgram(program);
	scale = glGetUniformLocation(program, "u_scale");
	glUniform1ui(glGetUniformLocation(program, "u_count"), GLESCOMPUTE_COUNT);
	glUniform1ui(scale, 2U);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0U, buffers[0]);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1U, buffers[1]);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2U, buffers[2]);
	glDispatchCompute(GLESCOMPUTE_GROUPS, 1U, 1U);
	glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

	/* The second: d = c + b * 3. */
	glUniform1ui(scale, 3U);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0U, buffers[2]);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2U, buffers[3]);
	glDispatchCompute(GLESCOMPUTE_GROUPS, 1U, 1U);

	/* The second's output, from the first's as the CPU computes it. */
	for (index = 0U; index < GLESCOMPUTE_COUNT; index++)
		c[index] = a[index] + b[index] * 2U;
	status = glescompute_read(buffers[3], d, GLESCOMPUTE_GROUPS * GLESCOMPUTE_LOCAL);
	if (status == 0)
		status = glescompute_add_check(c, b, 3U, d, GLESCOMPUTE_COUNT);
	passed = 0;
	if (status == 0)
		passed = 1;

	/* No error along the way. */
	error = glGetError();
	if (error != GL_NO_ERROR)
		passed = 0;
	glescompute_result("chain", passed, "wrong words, or an error");

	/* The objects go. */
	glDeleteBuffers(4, buffers);
	glDeleteProgram(program);
}

/*
 * Records a dispatch, releases the context and takes it again (as Noct
 * does around each operation), then reads the output: the recording
 * survives the release and runs at the read.
 */
static void
glescompute_release(
	struct glescompute_egl *egl)
{
	uint32_t a[GLESCOMPUTE_GROUPS * GLESCOMPUTE_LOCAL];
	uint32_t b[GLESCOMPUTE_GROUPS * GLESCOMPUTE_LOCAL];
	uint32_t c[GLESCOMPUTE_GROUPS * GLESCOMPUTE_LOCAL];
	GLuint program;
	GLuint buffers[3];
	EGLBoolean done;
	size_t index;
	GLenum error;
	int status;
	int passed;

	/* The program. */
	program = glescompute_program(glescompute_add_source, "release");
	if (program == 0U) {
		glescompute_result("release", 0, "the program did not link");
		return;
	}

	/* The buffers. */
	glescompute_inputs(a, b, GLESCOMPUTE_GROUPS * GLESCOMPUTE_LOCAL);
	buffers[0] = glescompute_buffer(a, GLESCOMPUTE_GROUPS * GLESCOMPUTE_LOCAL);
	buffers[1] = glescompute_buffer(b, GLESCOMPUTE_GROUPS * GLESCOMPUTE_LOCAL);
	buffers[2] = glescompute_buffer(NULL, GLESCOMPUTE_GROUPS * GLESCOMPUTE_LOCAL);

	/* The dispatch, recorded. */
	glUseProgram(program);
	glUniform1ui(glGetUniformLocation(program, "u_scale"), 7U);
	glUniform1ui(glGetUniformLocation(program, "u_count"), GLESCOMPUTE_COUNT);
	for (index = 0U; index < 3U; index++)
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, (GLuint)index, buffers[index]);
	glDispatchCompute(GLESCOMPUTE_GROUPS, 1U, 1U);
	glFlush();

	/* The context released and taken again. */
	passed = 1;
	done = eglMakeCurrent(egl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if (!done)
		passed = 0;
	done = eglMakeCurrent(egl->display, egl->surface, egl->surface, egl->context);
	if (!done)
		passed = 0;

	/* What it wrote. */
	if (passed) {
		status = glescompute_read(buffers[2], c, GLESCOMPUTE_GROUPS * GLESCOMPUTE_LOCAL);
		if (status == 0)
			status = glescompute_add_check(a, b, 7U, c, GLESCOMPUTE_COUNT);
		if (status != 0)
			passed = 0;
	}

	/* No error along the way. */
	error = glGetError();
	if (error != GL_NO_ERROR)
		passed = 0;
	glescompute_result("release", passed, "wrong words, a failed eglMakeCurrent, or an error");

	/* The objects go. */
	glDeleteBuffers(3, buffers);
	glDeleteProgram(program);
}

/*
 * Runs many dispatches, each over buffers given new storage
 * (glBufferData) and read back, as Noct's calls do; every result is
 * checked at its first and last word.
 */
static void
glescompute_repeat(
	int repeat)
{
	uint32_t *a;
	uint32_t *b;
	uint32_t *c;
	GLuint program;
	GLuint buffers[3];
	size_t words;
	size_t last;
	uint32_t scale;
	int round;
	GLenum error;
	int status;
	int passed;

	/* The program. */
	program = glescompute_program(glescompute_add_source, "repeat");
	if (program == 0U) {
		glescompute_result("repeat", 0, "the program did not link");
		return;
	}

	/* The words of each round: the two inputs and the output. */
	words = GLESCOMPUTE_REPEAT_BYTES / sizeof(uint32_t);
	a = malloc(words * sizeof(*a));
	if (a == NULL) {
		glescompute_result("repeat", 0, "no memory");
		return;
	}

	/* The second input. */
	b = malloc(words * sizeof(*b));
	if (b == NULL) {
		free(a);
		glescompute_result("repeat", 0, "no memory");
		return;
	}

	/* The output. */
	c = malloc(words * sizeof(*c));
	if (c == NULL) {
		free(a);
		free(b);
		glescompute_result("repeat", 0, "no memory");
		return;
	}

	/* The inputs' words, the buffers, and the program with its count. */
	glescompute_inputs(a, b, words);
	glGenBuffers(3, buffers);
	glUseProgram(program);
	glUniform1ui(glGetUniformLocation(program, "u_count"), (GLuint)words);

	/* Each round: new storage, a dispatch, the read. */
	passed = 1;
	last = words - 1U;
	for (round = 0; passed && round < repeat; round++) {
		scale = (uint32_t)round + 1U;
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[0]);
		glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)(words * sizeof(*a)), a, GL_DYNAMIC_COPY);
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[1]);
		glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)(words * sizeof(*b)), b, GL_DYNAMIC_COPY);
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[2]);
		glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)(words * sizeof(*c)), NULL, GL_DYNAMIC_COPY);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0U, buffers[0]);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1U, buffers[1]);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2U, buffers[2]);
		glUniform1ui(glGetUniformLocation(program, "u_scale"), scale);
		glDispatchCompute((GLuint)(words / GLESCOMPUTE_LOCAL), 1U, 1U);

		/* The round's first and last words. */
		status = glescompute_read(buffers[2], c, words);
		if (status != 0) {
			passed = 0;
		} else if (c[0] != a[0] + b[0] * scale || c[last] != a[last] + b[last] * scale) {
			printf("GLESCOMPUTE repeat round %d is wrong\n", round);
			passed = 0;
		}
	}

	/* No error along the way. */
	error = glGetError();
	if (error != GL_NO_ERROR)
		passed = 0;
	printf("GLESCOMPUTE repeat rounds=%d\n", round);
	glescompute_result("repeat", passed, "a wrong round, or an error");

	/* The objects go. */
	glDeleteBuffers(3, buffers);
	glDeleteProgram(program);
	free(a);
	free(b);
	free(c);
}

/*
 * Checks the errors of refused calls: a dispatch without a compute
 * program or of too many workgroups, a compute shader linked with a
 * vertex shader, a binding point past the last, bad barrier bits, a draw
 * of a compute program, and a call of OpenGL ES 3.1 that is not offered.
 */
static void
glescompute_errors(void)
{
	static const char *const vertex_source = glescompute_vertex_source;
	static const char *const compute_source = glescompute_add_source;
	GLuint compute;
	GLuint vertex;
	GLuint program;
	GLuint mixed;
	GLint count;
	GLint linked;
	GLenum error;
	int passed;

	/* Nothing left over from the steps before. */
	passed = 1;
	(void)glGetError();

	/* No program current: the dispatch is refused. */
	glUseProgram(0U);
	glDispatchCompute(1U, 1U, 1U);
	error = glGetError();
	if (error != GL_INVALID_OPERATION) {
		printf("GLESCOMPUTE errors: dispatch without a program gave 0x%x\n", (unsigned)error);
		passed = 0;
	}

	/* A compute program, then more workgroups than the device has. */
	program = glescompute_program(glescompute_add_source, "errors");
	if (program == 0U) {
		glescompute_result("errors", 0, "the program did not link");
		return;
	}

	/* One workgroup more than the device's count. */
	glUseProgram(program);
	count = 0;
	glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 0U, &count);
	glDispatchCompute((GLuint)count + 1U, 1U, 1U);
	error = glGetError();
	if (error != GL_INVALID_VALUE) {
		printf("GLESCOMPUTE errors: too many workgroups gave 0x%x\n", (unsigned)error);
		passed = 0;
	}

	/* A draw with the compute program current. */
	glDrawArrays(GL_POINTS, 0, 1);
	error = glGetError();
	if (error != GL_INVALID_OPERATION) {
		printf("GLESCOMPUTE errors: a draw of a compute program gave 0x%x\n", (unsigned)error);
		passed = 0;
	}

	/* A compute shader and a vertex shader in one program do not link. */
	compute = glCreateShader(GL_COMPUTE_SHADER);
	glShaderSource(compute, 1, &compute_source, NULL);
	glCompileShader(compute);
	vertex = glCreateShader(GL_VERTEX_SHADER);
	glShaderSource(vertex, 1, &vertex_source, NULL);
	glCompileShader(vertex);
	mixed = glCreateProgram();
	glAttachShader(mixed, compute);
	glAttachShader(mixed, vertex);
	glLinkProgram(mixed);
	linked = 1;
	glGetProgramiv(mixed, GL_LINK_STATUS, &linked);
	if (linked) {
		printf("GLESCOMPUTE errors: a compute and a vertex shader linked\n");
		passed = 0;
	}

	/* The objects of the failed link go. */
	glDeleteProgram(mixed);
	glDeleteShader(compute);
	glDeleteShader(vertex);

	/* A binding point past the last. */
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4U, 0U);
	error = glGetError();
	if (error != GL_INVALID_VALUE) {
		printf("GLESCOMPUTE errors: binding point 4 gave 0x%x\n", (unsigned)error);
		passed = 0;
	}

	/* Barrier bits there are not. */
	glMemoryBarrier(0x40000000U);
	error = glGetError();
	if (error != GL_INVALID_VALUE) {
		printf("GLESCOMPUTE errors: a bad barrier bit gave 0x%x\n", (unsigned)error);
		passed = 0;
	}

	/* A call of OpenGL ES 3.1 that is not offered. */
	glDrawArraysIndirect(GL_POINTS, NULL);
	error = glGetError();
	if (error != GL_INVALID_OPERATION) {
		printf("GLESCOMPUTE errors: glDrawArraysIndirect gave 0x%x\n", (unsigned)error);
		passed = 0;
	}

	/* Every error was the expected one. */
	glescompute_result("errors", passed, "an error that was not the expected one");

	/* The program goes. */
	glUseProgram(0U);
	glDeleteProgram(program);
}
