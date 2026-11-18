/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Reads the mview text model and its PAM textures into draw-ready arrays.
 */

#include "model.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The most whitespace-separated words any model line may carry. */
#define MODEL_TOKEN_MAX		24U

/* The largest PAM header line the texture reader accepts. */
#define MODEL_PAM_LINE_MAX	128U

/* The number of PAM header lines after which a header is refused as runaway. */
#define MODEL_PAM_HEADER_MAX	32U

/*
 * The state of one model.txt reading pass.
 *
 * The current mesh's declared vertex and triangle counts are checked as its
 * lines arrive; its vertex base rebases the mesh-local triangle indices.
 */
struct model_reader {
	struct mview_model *model;
	FILE *file;
	unsigned long line;
	char *tokens[MODEL_TOKEN_MAX];
	uint32_t token_count;
	int have_header;
	int have_bounds;
	int in_mesh;
	uint32_t mesh_vertices;
	uint32_t mesh_triangles;
	uint32_t mesh_vertex_seen;
	uint32_t mesh_triangle_seen;
	uint32_t mesh_base;
};

/* One keyword of the text format and the function that applies its line. */
struct model_handler {
	const char *keyword;
	int (*apply)(struct model_reader *reader);
};

static int model_read_text(struct mview_model *model, const char *path);
static int model_line(struct model_reader *reader, char *text);
static int model_header(struct model_reader *reader);
static int model_bounds(struct model_reader *reader);
static int model_texture(struct model_reader *reader);
static int model_material(struct model_reader *reader);
static int model_mesh(struct model_reader *reader);
static int model_vertex(struct model_reader *reader);
static int model_triangle(struct model_reader *reader);
static int model_mesh_complete(struct model_reader *reader);
static int model_finish(struct model_reader *reader);
static int model_groups(struct mview_model *model);
static int model_read_texture(struct mview_model *model, const char *directory, struct mview_texture *texture);
static int model_pam_header(FILE *file, uint32_t *width, uint32_t *height, uint32_t *depth);
static int model_token_split(struct model_reader *reader, char *text);
static int model_expect(struct model_reader *reader, uint32_t index, const char *word);
static int model_unsigned(struct model_reader *reader, uint32_t index, uint32_t maximum, uint32_t *number);
static int model_float(struct model_reader *reader, uint32_t index, float *number);
static int model_file_name(const char *name);
static int model_keyword(const char *word, const char *first, const char *second, const char *third);
static int model_fail(struct model_reader *reader, const char *format, ...) __attribute__((format(printf, 2, 3)));
static int model_error(struct mview_model *model, const char *format, ...) __attribute__((format(printf, 2, 3)));

/*
 * The keywords of the text format and the functions that apply their lines.
 *
 * The table is constant; "mview" is handled separately because it must be
 * the first line.
 */
static const struct model_handler model_handlers[] = {
	{ "v", model_vertex },
	{ "t", model_triangle },
	{ "mesh", model_mesh },
	{ "material", model_material },
	{ "texture", model_texture },
	{ "bounds", model_bounds }
};

/*
 * Reads directory/model.txt and every texture it names.
 *
 * On failure model->error says which file and line was refused and why; the
 * caller still owns whatever was read and releases it with mview_model_free.
 */
int
mview_model_load(
	struct mview_model *model,
	const char *directory)
{
	char path[MVIEW_PATH_MAX];
	uint32_t index;
	int length;
	int status;

	/* No array is owned before reading starts. */
	memset(model, 0, sizeof(*model));

	/* The text model lives at a fixed name inside the model directory. */
	length = snprintf(path, sizeof(path), "%s/model.txt", directory);
	if (length < 0 || (size_t)length >= sizeof(path))
		return model_error(model, "%s: model directory name is too long", directory);

	/* Reads, checks and groups the geometry and material table. */
	status = model_read_text(model, path);
	if (status != 0)
		return -1;

	/* Decodes each texture the materials may sample. */
	for (index = 0U; index < model->texture_count; index++) {
		status = model_read_texture(model, directory, &model->textures[index]);
		if (status != 0)
			return -1;
	}

	/* Succeeded: the model holds geometry, groups and decoded textures. */
	return 0;
}

/*
 * Releases every array a load produced, even after a failed load.
 */
void
mview_model_free(
	struct mview_model *model)
{
	uint32_t index;

	/* Texture pixels are owned one texture at a time. */
	if (model->textures != NULL) {
		for (index = 0U; index < model->texture_count; index++)
			free(model->textures[index].pixels);
	}

	/* The remaining arrays are single allocations. */
	free(model->textures);
	free(model->materials);
	free(model->vertices);
	free(model->triangles);
	free(model->indices);
	free(model->groups);

	/* Clears stale pointers so a second free is harmless. */
	memset(model, 0, sizeof(*model));

	/* Succeeded: the model owns nothing. */
	return;
}

/* Reads and checks every line of model.txt, then sorts the triangles into groups. */
static int
model_read_text(
	struct mview_model *model,
	const char *path)
{
	struct model_reader reader;
	char text[MVIEW_LINE_MAX + 2U];
	char *read;
	int status;

	/* The reader starts before the first line with no mesh open. */
	memset(&reader, 0, sizeof(reader));
	reader.model = model;

	/* Opens the text model for reading. */
	reader.file = fopen(path, "r");
	if (reader.file == NULL)
		return model_error(model, "%s: %s", path, strerror(errno));

	/* Applies each line in order; the first refused line ends the read. */
	status = 0;
	for (;;) {
		read = fgets(text, sizeof(text), reader.file);
		if (read == NULL)
			break;

		/* One line is consumed per read. */
		reader.line++;
		status = model_line(&reader, text);
		if (status != 0)
			break;
	}

	/* A read error is distinct from reaching the end of the file. */
	if (status == 0) {
		status = ferror(reader.file);
		if (status != 0)
			status = model_fail(&reader, "read error");
	}

	/* The file is no longer needed whatever the outcome. */
	fclose(reader.file);
	reader.file = NULL;
	if (status != 0)
		return -1;

	/* The last mesh must be complete and every reference must resolve. */
	status = model_finish(&reader);
	if (status != 0)
		return -1;

	/* Orders the triangles into opaque, cutout and blend draw groups. */
	status = model_groups(model);
	if (status != 0)
		return -1;

	/* Succeeded: the text model is complete and grouped. */
	return 0;
}

/* Applies one line of the text model. */
static int
model_line(
	struct model_reader *reader,
	char *text)
{
	size_t length;
	uint32_t index;
	int match;
	int status;

	/* A line that fills the buffer without a newline is longer than the format allows. */
	length = strlen(text);
	if (length > MVIEW_LINE_MAX)
		return model_fail(reader, "line longer than %u bytes", MVIEW_LINE_MAX);

	/* Splits the line; blank lines and comments carry nothing. */
	status = model_token_split(reader, text);
	if (status != 0)
		return -1;

	/* A blank or comment-only line leaves the model unchanged. */
	if (reader->token_count == 0U)
		return 0;

	/* The format version must precede everything else. */
	if (reader->have_header == 0) {
		status = model_header(reader);
		if (status != 0)
			return -1;

		/* The header line carries nothing else. */
		return 0;
	}

	/* Finds the keyword's handler; vertex lines, the most common, come first. */
	for (index = 0U; index < sizeof(model_handlers) / sizeof(model_handlers[0]); index++) {
		match = strcmp(reader->tokens[0], model_handlers[index].keyword);
		if (match == 0)
			break;
	}

	/* An unknown keyword is refused rather than silently skipped. */
	if (index == sizeof(model_handlers) / sizeof(model_handlers[0])) {
		model_fail(reader, "unknown keyword '%s'", reader->tokens[0]);
		return -1;
	}

	/* Applies the line; the handler has recorded why it refused one. */
	status = model_handlers[index].apply(reader);
	if (status != 0)
		return -1;

	/* Succeeded: the line has been applied to the model. */
	return 0;
}

/* Accepts the only format version this viewer understands. */
static int
model_header(
	struct model_reader *reader)
{
	int status;

	/* The first line is exactly "mview 1". */
	if (reader->token_count != 2U)
		return model_fail(reader, "expected 'mview 1' as the first line");

	/* The magic word names the format. */
	status = model_expect(reader, 0U, "mview");
	if (status != 0)
		return -1;

	/* Later format versions may change meaning and are refused. */
	status = model_expect(reader, 1U, "1");
	if (status != 0)
		return -1;

	/* Every other keyword is now accepted. */
	reader->have_header = 1;

	/* Succeeded: the file declares the supported format. */
	return 0;
}

/* Reads "bounds minx miny minz maxx maxy maxz". */
static int
model_bounds(
	struct model_reader *reader)
{
	uint32_t axis;
	int status;

	/* One bounds line with six coordinates is allowed. */
	if (reader->token_count != 7U)
		return model_fail(reader, "bounds needs six coordinates");

	/* A second box would contradict the first. */
	if (reader->have_bounds != 0)
		return model_fail(reader, "bounds given twice");

	/* Reads the minimum corner, then the maximum corner. */
	for (axis = 0U; axis < 3U; axis++) {
		status = model_float(reader, 1U + axis, &reader->model->minimum[axis]);
		if (status != 0)
			return -1;

		/* The matching maximum coordinate follows the three minimum ones. */
		status = model_float(reader, 4U + axis, &reader->model->maximum[axis]);
		if (status != 0)
			return -1;

		/* An inverted box has no inside to look at. */
		if (reader->model->minimum[axis] > reader->model->maximum[axis])
			return model_fail(reader, "bounds minimum exceeds maximum");
	}

	/* The explicit box replaces the one computed from the vertices. */
	reader->have_bounds = 1;

	/* Succeeded: the bounds are recorded. */
	return 0;
}

/* Reads "texture <index> <file> [<width> <height>]". */
static int
model_texture(
	struct model_reader *reader)
{
	struct mview_model *model;
	struct mview_texture *textures;
	struct mview_texture *texture;
	size_t length;
	uint32_t index;
	int status;

	/* The declared size is optional and checked against the file when present. */
	model = reader->model;
	if (reader->token_count != 3U && reader->token_count != 5U)
		return model_fail(reader, "texture needs an index, a file and optionally a size");

	/* Textures are numbered densely in file order. */
	status = model_unsigned(reader, 1U, MVIEW_TEXTURE_MAX - 1U, &index);
	if (status != 0)
		return -1;

	/* A gap or repeat would leave materials pointing at the wrong image. */
	if (index != model->texture_count)
		return model_fail(reader, "texture %u out of order, expected %u", index, model->texture_count);

	/* The file must stay inside the model directory. */
	status = model_file_name(reader->tokens[2]);
	if (status != 0)
		return model_fail(reader, "texture file '%s' is not a plain relative path", reader->tokens[2]);

	/* The name must fit the texture record. */
	length = strlen(reader->tokens[2]);
	if (length >= MVIEW_PATH_MAX)
		return model_fail(reader, "texture file name is too long");

	/* Grows the texture table by one entry. */
	textures = realloc(model->textures, (model->texture_count + 1U) * sizeof(*textures));
	if (textures == NULL)
		return model_fail(reader, "out of memory");

	model->textures = textures;
	texture = &textures[model->texture_count];
	memset(texture, 0, sizeof(*texture));
	model->texture_count++;

	/* Records the file; the pixels are read after the text is complete. */
	strcpy(texture->file, reader->tokens[2]);
	if (reader->token_count == 5U) {
		status = model_unsigned(reader, 3U, MVIEW_TEXTURE_SIDE_MAX, &texture->width);
		if (status != 0)
			return -1;

		/* The height follows the width. */
		status = model_unsigned(reader, 4U, MVIEW_TEXTURE_SIDE_MAX, &texture->height);
		if (status != 0)
			return -1;
	}

	/* Succeeded: the texture is declared. */
	return 0;
}

/*
 * Reads "material <index> <name> texture <index|-> alpha <mode> cull <mode>
 * color <r> <g> <b> <a>".
 */
static int
model_material(
	struct model_reader *reader)
{
	struct mview_model *model;
	struct mview_material *materials;
	struct mview_material *material;
	size_t length;
	uint32_t index;
	uint32_t channel;
	int match;
	int status;

	/* Every field is required and appears in a fixed order. */
	model = reader->model;
	if (reader->token_count != 14U)
		return model_fail(reader, "material needs index, name, texture, alpha, cull and color");

	/* Materials are numbered densely in file order. */
	status = model_unsigned(reader, 1U, MVIEW_MATERIAL_MAX - 1U, &index);
	if (status != 0)
		return -1;

	/* A gap or repeat would leave triangles drawn with the wrong material. */
	if (index != model->material_count)
		return model_fail(reader, "material %u out of order, expected %u", index, model->material_count);

	/* Materials may not appear once meshes have started to reference them. */
	if (model->mesh_count != 0U)
		return model_fail(reader, "material after the first mesh");

	/* The name is a label only; it must fit the table. */
	length = strlen(reader->tokens[2]);
	if (length >= MVIEW_NAME_MAX)
		return model_fail(reader, "material name is too long");

	/* Grows the material table by one entry. */
	materials = realloc(model->materials, (model->material_count + 1U) * sizeof(*materials));
	if (materials == NULL)
		return model_fail(reader, "out of memory");

	model->materials = materials;
	material = &materials[model->material_count];
	memset(material, 0, sizeof(*material));
	model->material_count++;
	strcpy(material->name, reader->tokens[2]);

	/* The texture is a declared index, or "-" for the base colour alone. */
	status = model_expect(reader, 3U, "texture");
	if (status != 0)
		return -1;

	/* A dash means the material has no texture. */
	match = strcmp(reader->tokens[4], "-");
	if (match == 0) {
		material->texture = -1;
	} else {
		status = model_unsigned(reader, 4U, MVIEW_TEXTURE_MAX - 1U, &index);
		if (status != 0)
			return -1;

		/* Textures are declared before the materials that use them. */
		if (index >= model->texture_count)
			return model_fail(reader, "material uses undeclared texture %u", index);

		material->texture = (int32_t)index;
	}

	/* The alpha mode selects the pipeline the material is drawn with. */
	status = model_expect(reader, 5U, "alpha");
	if (status != 0)
		return -1;

	status = model_keyword(reader->tokens[6], "opaque", "cutout", "blend");
	if (status < 0)
		return model_fail(reader, "unknown alpha mode '%s'", reader->tokens[6]);

	/* The keyword position is the alpha mode's enumeration value. */
	material->alpha = (enum mview_alpha)status;

	/* The cull mode says whether back faces are visible. */
	status = model_expect(reader, 7U, "cull");
	if (status != 0)
		return -1;

	status = model_keyword(reader->tokens[8], "back", "none", NULL);
	if (status < 0)
		return model_fail(reader, "unknown cull mode '%s'", reader->tokens[8]);

	/* The keyword position is the cull mode's enumeration value. */
	material->cull = (enum mview_cull)status;

	/* The base colour multiplies the texture; each channel lies in 0..1. */
	status = model_expect(reader, 9U, "color");
	if (status != 0)
		return -1;

	/* Reads the four channels in RGBA order. */
	for (channel = 0U; channel < 4U; channel++) {
		status = model_float(reader, 10U + channel, &material->color[channel]);
		if (status != 0)
			return -1;

		/* A colour outside 0..1 cannot be represented by the target. */
		if (material->color[channel] < 0.0f || material->color[channel] > 1.0f)
			return model_fail(reader, "material color outside 0..1");
	}

	/* Succeeded: the material is declared. */
	return 0;
}

/* Reads "mesh <name> vertices <n> triangles <m>" and reserves its arrays. */
static int
model_mesh(
	struct model_reader *reader)
{
	struct mview_model *model;
	struct mview_vertex *vertices;
	uint32_t *triangles;
	uint32_t capacity;
	int status;

	/* The counts let every following line be checked as it arrives. */
	model = reader->model;
	if (reader->token_count != 6U)
		return model_fail(reader, "mesh needs a name, a vertex count and a triangle count");

	/* The previous mesh must have delivered everything it declared. */
	status = model_mesh_complete(reader);
	if (status != 0)
		return -1;

	/* A bounded number of meshes keeps the scan finite. */
	if (model->mesh_count == MVIEW_MESH_MAX)
		return model_fail(reader, "more than %u meshes", MVIEW_MESH_MAX);

	/* Reads the declared vertex count. */
	status = model_expect(reader, 2U, "vertices");
	if (status != 0)
		return -1;

	/* The count bounds the vertex lines that follow. */
	status = model_unsigned(reader, 3U, MVIEW_VERTEX_MAX, &reader->mesh_vertices);
	if (status != 0)
		return -1;

	/* Reads the declared triangle count. */
	status = model_expect(reader, 4U, "triangles");
	if (status != 0)
		return -1;

	/* The count bounds the triangle lines that follow. */
	status = model_unsigned(reader, 5U, MVIEW_TRIANGLE_MAX, &reader->mesh_triangles);
	if (status != 0)
		return -1;

	/* The whole model stays within the vertex bound. */
	if (reader->mesh_vertices > MVIEW_VERTEX_MAX - model->vertex_count)
		return model_fail(reader, "model has more than %u vertices", MVIEW_VERTEX_MAX);

	/* The whole model stays within the triangle bound. */
	if (reader->mesh_triangles > MVIEW_TRIANGLE_MAX - model->triangle_count)
		return model_fail(reader, "model has more than %u triangles", MVIEW_TRIANGLE_MAX);

	/* Reserves room for this mesh's vertices. */
	capacity = model->vertex_count + reader->mesh_vertices;
	if (capacity > model->vertex_capacity) {
		vertices = realloc(model->vertices, (size_t)capacity * sizeof(*vertices));
		if (vertices == NULL)
			return model_fail(reader, "out of memory");

		model->vertices = vertices;
		model->vertex_capacity = capacity;
	}

	/* Reserves room for this mesh's triangles: a material and three indices each. */
	capacity = model->triangle_count + reader->mesh_triangles;
	if (capacity > model->triangle_capacity) {
		triangles = realloc(model->triangles, (size_t)capacity * 4U * sizeof(*triangles));
		if (triangles == NULL)
			return model_fail(reader, "out of memory");

		model->triangles = triangles;
		model->triangle_capacity = capacity;
	}

	/* The mesh's local indices start at the current end of the shared vertex array. */
	reader->in_mesh = 1;
	reader->mesh_base = model->vertex_count;
	reader->mesh_vertex_seen = 0U;
	reader->mesh_triangle_seen = 0U;
	model->mesh_count++;

	/* Succeeded: the mesh is open for its vertex and triangle lines. */
	return 0;
}

/* Reads "v x y z nx ny nz u v" into the open mesh. */
static int
model_vertex(
	struct model_reader *reader)
{
	struct mview_model *model;
	struct mview_vertex *vertex;
	float length;
	uint32_t index;
	int status;

	/* A vertex belongs to the open mesh and must fit its declared count. */
	model = reader->model;
	if (reader->in_mesh == 0)
		return model_fail(reader, "vertex outside a mesh");

	/* Eight coordinates follow the keyword. */
	if (reader->token_count != 9U)
		return model_fail(reader, "vertex needs position, normal and texture coordinate");

	/* The declared count is a hard limit. */
	if (reader->mesh_vertex_seen == reader->mesh_vertices)
		return model_fail(reader, "mesh has more vertices than declared");

	/* All of a mesh's vertices precede its triangles. */
	if (reader->mesh_triangle_seen != 0U)
		return model_fail(reader, "vertex after the mesh's first triangle");

	/* Reads position, normal and texture coordinate in file order. */
	vertex = &model->vertices[model->vertex_count];
	for (index = 0U; index < 3U; index++) {
		status = model_float(reader, 1U + index, &vertex->position[index]);
		if (status != 0)
			return -1;
	}

	/* The normal follows the position. */
	for (index = 0U; index < 3U; index++) {
		status = model_float(reader, 4U + index, &vertex->normal[index]);
		if (status != 0)
			return -1;
	}

	/* The texture coordinate ends the line. */
	for (index = 0U; index < 2U; index++) {
		status = model_float(reader, 7U + index, &vertex->uv[index]);
		if (status != 0)
			return -1;
	}

	/*
	 * Lighting needs a unit normal.  A degenerate normal from the source is
	 * replaced by the view axis rather than producing NaN in the shader.
	 */
	length = sqrtf(vertex->normal[0] * vertex->normal[0] +
		       vertex->normal[1] * vertex->normal[1] +
		       vertex->normal[2] * vertex->normal[2]);
	if (length < 1.0e-6f) {
		vertex->normal[0] = 0.0f;
		vertex->normal[1] = 0.0f;
		vertex->normal[2] = 1.0f;
	} else {
		vertex->normal[0] /= length;
		vertex->normal[1] /= length;
		vertex->normal[2] /= length;
	}

	/* The vertex is now part of the shared array. */
	model->vertex_count++;
	reader->mesh_vertex_seen++;

	/* Succeeded: the vertex is stored. */
	return 0;
}

/* Reads "t <material> <i0> <i1> <i2>" with indices local to the open mesh. */
static int
model_triangle(
	struct model_reader *reader)
{
	struct mview_model *model;
	uint32_t *triangle;
	uint32_t material;
	uint32_t corner;
	uint32_t index;
	int status;

	/* A triangle belongs to the open mesh and must fit its declared count. */
	model = reader->model;
	if (reader->in_mesh == 0)
		return model_fail(reader, "triangle outside a mesh");

	/* A material and three corners follow the keyword. */
	if (reader->token_count != 5U)
		return model_fail(reader, "triangle needs a material and three indices");

	/* The declared count is a hard limit. */
	if (reader->mesh_triangle_seen == reader->mesh_triangles)
		return model_fail(reader, "mesh has more triangles than declared");

	/* Triangles index only vertices the mesh has fully delivered. */
	if (reader->mesh_vertex_seen != reader->mesh_vertices)
		return model_fail(reader, "triangle before all of the mesh's vertices");

	/* The material must already be declared. */
	status = model_unsigned(reader, 1U, MVIEW_MATERIAL_MAX - 1U, &material);
	if (status != 0)
		return -1;

	/* An undeclared material has no pipeline or texture. */
	if (material >= model->material_count)
		return model_fail(reader, "triangle uses undeclared material %u", material);

	/* Stores the material, then the three corners rebased onto the shared array. */
	triangle = &model->triangles[(size_t)model->triangle_count * 4U];
	triangle[0] = material;
	for (corner = 0U; corner < 3U; corner++) {
		status = model_unsigned(reader, 2U + corner, UINT32_MAX, &index);
		if (status != 0)
			return -1;

		/* A corner must name one of this mesh's vertices. */
		if (index >= reader->mesh_vertices)
			return model_fail(reader, "triangle index %u outside the mesh's %u vertices", index, reader->mesh_vertices);

		triangle[1U + corner] = reader->mesh_base + index;
	}

	/* The triangle is now part of the model and counts toward its material. */
	model->triangle_count++;
	model->materials[material].triangles++;
	reader->mesh_triangle_seen++;

	/* Succeeded: the triangle is stored. */
	return 0;
}

/* Refuses a mesh that ended before delivering what it declared. */
static int
model_mesh_complete(
	struct model_reader *reader)
{
	/* No mesh is open before the first mesh line. */
	if (reader->in_mesh == 0)
		return 0;

	/* Every declared vertex must have arrived. */
	if (reader->mesh_vertex_seen != reader->mesh_vertices)
		return model_fail(reader, "previous mesh has %u of %u vertices", reader->mesh_vertex_seen, reader->mesh_vertices);

	/* Every declared triangle must have arrived. */
	if (reader->mesh_triangle_seen != reader->mesh_triangles)
		return model_fail(reader, "previous mesh has %u of %u triangles", reader->mesh_triangle_seen, reader->mesh_triangles);

	/* Succeeded: the mesh is complete. */
	return 0;
}

/* Checks the file as a whole once every line has been read. */
static int
model_finish(
	struct model_reader *reader)
{
	struct mview_model *model;
	uint32_t index;
	uint32_t axis;
	int status;

	/* An empty file has no header. */
	model = reader->model;
	if (reader->have_header == 0)
		return model_fail(reader, "missing 'mview 1' header");

	/* The last mesh ends at the end of the file. */
	reader->line++;
	status = model_mesh_complete(reader);
	if (status != 0)
		return -1;

	/* A model without triangles draws nothing. */
	if (model->triangle_count == 0U)
		return model_fail(reader, "model has no triangles");

	/* Without an explicit box, the bounds are those of the vertices. */
	if (reader->have_bounds == 0) {
		for (axis = 0U; axis < 3U; axis++) {
			model->minimum[axis] = model->vertices[0].position[axis];
			model->maximum[axis] = model->vertices[0].position[axis];
		}

		/* Widens the box to every vertex. */
		for (index = 1U; index < model->vertex_count; index++) {
			for (axis = 0U; axis < 3U; axis++) {
				if (model->vertices[index].position[axis] < model->minimum[axis])
					model->minimum[axis] = model->vertices[index].position[axis];

				/* The maximum widens independently of the minimum. */
				if (model->vertices[index].position[axis] > model->maximum[axis])
					model->maximum[axis] = model->vertices[index].position[axis];
			}
		}
	}

	/* Succeeded: the text model is internally consistent. */
	return 0;
}

/*
 * Sorts triangle indices into one draw group per used material.
 *
 * Groups are ordered by alpha mode (opaque, cutout, blend) and then by
 * material index, so drawing them in array order draws blend materials
 * last.  Within a group the triangles keep their file order.
 */
static int
model_groups(
	struct mview_model *model)
{
	uint32_t *next;
	uint32_t alpha;
	uint32_t material;
	uint32_t index;
	uint32_t offset;
	uint32_t corner;
	uint32_t *triangle;
	struct mview_group *group;

	/* Each triangle contributes three indices. */
	model->indices = malloc((size_t)model->triangle_count * 3U * sizeof(*model->indices));
	if (model->indices == NULL)
		return model_error(model, "model.txt: out of memory for indices");

	/* One group per material is the most the model can use. */
	model->groups = calloc(model->material_count, sizeof(*model->groups));
	if (model->groups == NULL)
		return model_error(model, "model.txt: out of memory for groups");

	/* Holds, per material, where its next index is written. */
	next = calloc(model->material_count, sizeof(*next));
	if (next == NULL)
		return model_error(model, "model.txt: out of memory for groups");

	/* Lays the groups out in alpha order, skipping materials with no triangles. */
	offset = 0U;
	for (alpha = MVIEW_ALPHA_OPAQUE; alpha <= MVIEW_ALPHA_BLEND; alpha++) {
		for (material = 0U; material < model->material_count; material++) {
			/* Only materials of the current alpha mode belong to this pass. */
			if ((uint32_t)model->materials[material].alpha != alpha)
				continue;

			/* An unused material produces no draw. */
			if (model->materials[material].triangles == 0U)
				continue;

			/* The group's range starts where the previous group ended. */
			group = &model->groups[model->group_count];
			group->material = material;
			group->first = offset;
			group->count = model->materials[material].triangles * 3U;
			next[material] = offset;
			offset += group->count;
			model->group_count++;
		}
	}

	/* Scatters each triangle's corners into its material's range. */
	for (index = 0U; index < model->triangle_count; index++) {
		triangle = &model->triangles[(size_t)index * 4U];
		material = triangle[0];
		for (corner = 0U; corner < 3U; corner++) {
			model->indices[next[material]] = triangle[1U + corner];
			next[material]++;
		}
	}

	/* The write positions are no longer needed. */
	free(next);

	/* Succeeded: the index array is sorted into draw groups. */
	return 0;
}

/* Reads one PAM texture and converts it to RGBA rows. */
static int
model_read_texture(
	struct mview_model *model,
	const char *directory,
	struct mview_texture *texture)
{
	char path[MVIEW_PATH_MAX * 2U];
	FILE *file;
	uint8_t *row;
	uint32_t width;
	uint32_t height;
	uint32_t depth;
	uint32_t y;
	uint32_t x;
	size_t bytes;
	size_t count;
	int length;
	int status;

	/* The texture file name is relative to the model directory. */
	length = snprintf(path, sizeof(path), "%s/%s", directory, texture->file);
	if (length < 0 || (size_t)length >= sizeof(path))
		return model_error(model, "%s: texture path is too long", texture->file);

	/* Opens the texture for binary reading. */
	file = fopen(path, "rb");
	if (file == NULL)
		return model_error(model, "%s: %s", path, strerror(errno));

	/* Reads the header, which fixes the geometry and channel count. */
	status = model_pam_header(file, &width, &height, &depth);
	if (status != 0) {
		fclose(file);
		return model_error(model, "%s: not an 8-bit RGB or RGB_ALPHA PAM of at most %u pixels per side", path, MVIEW_TEXTURE_SIDE_MAX);
	}

	/* A size declared in model.txt must match the file. */
	if (texture->width != 0U && (texture->width != width || texture->height != height)) {
		fclose(file);
		return model_error(model, "%s: size %ux%u differs from the declared %ux%u", path, width, height, texture->width, texture->height);
	}

	/* The decoded image is always four bytes per texel. */
	texture->width = width;
	texture->height = height;
	bytes = (size_t)width * (size_t)height * 4U;
	texture->pixels = malloc(bytes);
	if (texture->pixels == NULL) {
		fclose(file);
		return model_error(model, "%s: out of memory", path);
	}

	/* Reads each row, expanding RGB rows to opaque RGBA in place. */
	for (y = 0U; y < height; y++) {
		row = texture->pixels + (size_t)y * width * 4U;
		count = fread(row, depth, width, file);
		if (count != width) {
			fclose(file);
			return model_error(model, "%s: truncated at row %u", path, y);
		}

		/* An RGB row is spread from the back so no texel is overwritten early. */
		if (depth == 3U) {
			for (x = width; x > 0U; x--) {
				row[(x - 1U) * 4U + 3U] = 255U;
				row[(x - 1U) * 4U + 2U] = row[(x - 1U) * 3U + 2U];
				row[(x - 1U) * 4U + 1U] = row[(x - 1U) * 3U + 1U];
				row[(x - 1U) * 4U + 0U] = row[(x - 1U) * 3U + 0U];
			}
		}
	}

	/* The pixel data has been consumed completely. */
	fclose(file);

	/* Succeeded: the texture holds its RGBA pixels. */
	return 0;
}

/*
 * Parses a PAM (P7) header up to and including ENDHDR.
 *
 * Only MAXVAL 255 with DEPTH 4 (RGB_ALPHA) or DEPTH 3 (RGB) is accepted.
 */
static int
model_pam_header(
	FILE *file,
	uint32_t *width,
	uint32_t *height,
	uint32_t *depth)
{
	char line[MODEL_PAM_LINE_MAX];
	char key[16];
	char value[32];
	char *read;
	unsigned long number;
	uint32_t lines;
	uint32_t maximum;
	int fields;
	int field;
	int tuple;
	int match;

	/* The magic line identifies the arbitrary-map format. */
	read = fgets(line, sizeof(line), file);
	if (read == NULL)
		return -1;

	/* Any other magic is a different image format. */
	match = strcmp(line, "P7\n");
	if (match != 0)
		return -1;

	/* Reads keyed header lines until ENDHDR. */
	*width = 0U;
	*height = 0U;
	*depth = 0U;
	maximum = 0U;
	tuple = 0;
	for (lines = 0U; lines < MODEL_PAM_HEADER_MAX; lines++) {
		read = fgets(line, sizeof(line), file);
		if (read == NULL)
			return -1;

		/* Comments and blank lines carry nothing. */
		if (line[0] == '#' || line[0] == '\n')
			continue;

		/* The header ends at its terminator. */
		match = strcmp(line, "ENDHDR\n");
		if (match == 0)
			break;

		/* Every other line is one key and one value. */
		fields = sscanf(line, "%15s %31s", key, value);
		if (fields != 2)
			return -1;

		/* The tuple type is the only non-numeric value. */
		match = strcmp(key, "TUPLTYPE");
		if (match == 0) {
			/* RGB carries three channels and RGB_ALPHA four. */
			tuple = model_keyword(value, "RGB", "RGB_ALPHA", NULL);
			if (tuple < 0)
				return -1;

			/* Converts the keyword position to the channel count. */
			tuple += 3;
			continue;
		}

		/* Every other known value is a bounded decimal number. */
		if (value[0] < '0' || value[0] > '9')
			return -1;

		/* The whole value must be a decimal number of at most 16 bits. */
		number = strtoul(value, &read, 10);
		if (*read != '\0' || number > 65535UL)
			return -1;

		/* Stores the value under its key; unknown keys are refused. */
		field = model_keyword(key, "WIDTH", "HEIGHT", "DEPTH");
		if (field == 0) {
			*width = (uint32_t)number;
		} else if (field == 1) {
			*height = (uint32_t)number;
		} else if (field == 2) {
			*depth = (uint32_t)number;
		} else {
			/* MAXVAL is the one remaining key the reader knows. */
			match = strcmp(key, "MAXVAL");
			if (match != 0)
				return -1;

			maximum = (uint32_t)number;
		}
	}

	/* A header without a terminator is refused. */
	if (lines == MODEL_PAM_HEADER_MAX)
		return -1;

	/* Only 8-bit channels are supported. */
	if (maximum != 255U)
		return -1;

	/* The tuple type and the depth must agree. */
	if (tuple == 0 || (uint32_t)tuple != *depth)
		return -1;

	/* An empty image has nothing to sample. */
	if (*width == 0U || *height == 0U)
		return -1;

	/* An oversized image would exceed the texture limit. */
	if (*width > MVIEW_TEXTURE_SIDE_MAX || *height > MVIEW_TEXTURE_SIDE_MAX)
		return -1;

	/* Succeeded: the pixel data follows. */
	return 0;
}

/* Splits one line into whitespace-separated words, ignoring comments. */
static int
model_token_split(
	struct model_reader *reader,
	char *text)
{
	char *position;

	/* Walks the line, terminating each word in place. */
	reader->token_count = 0U;
	position = text;
	for (;;) {
		/* Skips the whitespace between words. */
		while (*position == ' ' || *position == '\t' || *position == '\r' || *position == '\n')
			position++;

		/* The line, or a comment, ends the words. */
		if (*position == '\0' || *position == '#')
			break;

		/* Refuses a line with more words than any keyword needs. */
		if (reader->token_count == MODEL_TOKEN_MAX)
			return model_fail(reader, "too many words");

		/* Records the word and finds its end. */
		reader->tokens[reader->token_count] = position;
		reader->token_count++;
		while (*position != '\0' &&
		       *position != ' ' &&
		       *position != '\t' &&
		       *position != '\r' &&
		       *position != '\n')
			position++;

		/* The line ended inside the word. */
		if (*position == '\0')
			break;

		/* Terminates the word and continues after it. */
		*position = '\0';
		position++;
	}

	/* Succeeded: the words are available. */
	return 0;
}

/* Refuses a line whose word at index is not the expected keyword. */
static int
model_expect(
	struct model_reader *reader,
	uint32_t index,
	const char *word)
{
	int match;

	/* Compares the word with the keyword the format requires there. */
	match = strcmp(reader->tokens[index], word);
	if (match != 0)
		return model_fail(reader, "expected '%s', found '%s'", word, reader->tokens[index]);

	/* Succeeded: the keyword is in place. */
	return 0;
}

/* Reads a decimal unsigned integer no larger than maximum. */
static int
model_unsigned(
	struct model_reader *reader,
	uint32_t index,
	uint32_t maximum,
	uint32_t *number)
{
	const char *text;
	uint32_t parsed;
	uint32_t digit;

	/* Only plain decimal digits are accepted: no sign, no base prefix. */
	text = reader->tokens[index];
	parsed = 0U;
	if (*text == '\0')
		return model_fail(reader, "empty number");

	/* Extends the value one digit at a time, refusing overflow first. */
	while (*text != '\0') {
		/* Signs, prefixes and fractions are not part of the grammar. */
		if (*text < '0' || *text > '9')
			return model_fail(reader, "'%s' is not a decimal number", reader->tokens[index]);

		/* The next digit must keep the value within its bound. */
		digit = (uint32_t)(*text - '0');
		if (parsed > maximum / 10U ||
		    (parsed == maximum / 10U && digit > maximum % 10U))
			return model_fail(reader, "'%s' exceeds %u", reader->tokens[index], maximum);

		/* The checked digit extends the value. */
		parsed = parsed * 10U + digit;
		text++;
	}

	/* Publishes the value only after the whole word is accepted. */
	*number = parsed;

	/* Succeeded: the number is within its bound. */
	return 0;
}

/* Reads a finite decimal floating-point number. */
static int
model_float(
	struct model_reader *reader,
	uint32_t index,
	float *number)
{
	char *end;
	float parsed;
	int finite;

	/* The whole word must be one number. */
	errno = 0;
	parsed = strtof(reader->tokens[index], &end);
	if (end == reader->tokens[index] || *end != '\0')
		return model_fail(reader, "'%s' is not a number", reader->tokens[index]);

	/* Infinities, NaN and overflow cannot be drawn. */
	finite = isfinite(parsed);
	if (errno == ERANGE || finite == 0)
		return model_fail(reader, "'%s' is not a finite number", reader->tokens[index]);

	/* Publishes the accepted value. */
	*number = parsed;

	/* Succeeded: the number is finite. */
	return 0;
}

/* Accepts a relative path made of plain name characters and no parent step. */
static int
model_file_name(
	const char *name)
{
	const char *position;

	/* An absolute path or a leading dot could leave the model directory. */
	if (name[0] == '\0' || name[0] == '/' || name[0] == '.')
		return -1;

	/* Each character is a letter, digit, '-', '_', '.' or a separating '/'. */
	for (position = name; *position != '\0'; position++) {
		/* Plain name characters are always allowed. */
		if ((*position >= 'a' && *position <= 'z') ||
		    (*position >= 'A' && *position <= 'Z') ||
		    (*position >= '0' && *position <= '9') ||
		    *position == '-' ||
		    *position == '_')
			continue;

		/* A dot may not begin a path component, which rules out "..". */
		if (*position == '.' && position[-1] != '/')
			continue;

		/* A slash separates components and may not be doubled or trailing. */
		if (*position == '/' && position[-1] != '/' && position[1] != '\0')
			continue;

		/* Anything else could escape the directory or confuse the log. */
		return -1;
	}

	/* Succeeded: the name stays inside the model directory. */
	return 0;
}

/* Reports the position (0, 1 or 2) of word among up to three keywords, or -1. */
static int
model_keyword(
	const char *word,
	const char *first,
	const char *second,
	const char *third)
{
	int match;

	/* The first keyword is always given. */
	match = strcmp(word, first);
	if (match == 0)
		return 0;

	/* The second keyword is always given. */
	match = strcmp(word, second);
	if (match == 0)
		return 1;

	/* The third keyword is optional. */
	if (third == NULL)
		return -1;

	match = strcmp(word, third);
	if (match == 0)
		return 2;

	/* Refuses a word that is none of the keywords. */
	return -1;
}

/* Records a line-numbered model.txt error and reports failure. */
static int
model_fail(
	struct model_reader *reader,
	const char *format,
	...)
{
	char message[MVIEW_ERROR_MAX - 48U];
	va_list arguments;

	/* Formats the reason. */
	va_start(arguments, format);
	vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);

	/* Prefixes the line so the file can be corrected. */
	snprintf(reader->model->error, sizeof(reader->model->error), "model.txt:%lu: %s", reader->line, message);

	/* Reports the refusal to the caller. */
	return -1;
}

/* Records an error that is not tied to a model.txt line and reports failure. */
static int
model_error(
	struct mview_model *model,
	const char *format,
	...)
{
	va_list arguments;

	/* Formats the reason into the model's error text. */
	va_start(arguments, format);
	vsnprintf(model->error, sizeof(model->error), format, arguments);
	va_end(arguments);

	/* Reports the refusal to the caller. */
	return -1;
}
