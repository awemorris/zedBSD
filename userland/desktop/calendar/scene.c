/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Calendar's 3D things (WS155 p000; scene.h).
 *
 * The desk calendar, in its own units: the page shown is 1.5 wide and 1.8
 * high with its middle at the origin, facing the camera (-z); the pages
 * under it, a board behind them and a stand behind that go back along +z,
 * and a bar with two rings binds them at the top.  A page turns over the
 * top about the line where the rings hold it.
 */

#include "scene.h"

#include <math.h>
#include <string.h>

/* Pi. */
#define SC_PI			3.14159265f

/* The page: its half width, its top and bottom, and where it turns. */
#define SC_PAGE_HALF		0.75f
#define SC_PAGE_TOP		0.85f
#define SC_PAGE_BOTTOM		-0.95f

/* The steps round an ellipsoid and a ring. */
#define SC_ELLIPSOID_STEPS	14
#define SC_RING_MAJOR_STEPS	20
#define SC_RING_MINOR_STEPS	8

/* The heart's outline points. */
#define SC_HEART_POINTS		40

/* The colors of the desk calendar. */
#define SC_COLOR_PAPER		KL_RGB(0xf4f6fa)
#define SC_COLOR_PAPER_DARK	KL_RGB(0xe3e8f0)
#define SC_COLOR_BACK		KL_RGB(0xf8f9fb)
#define SC_COLOR_BOARD		KL_RGB(0x2b3a55)
#define SC_COLOR_STAND		KL_RGB(0x3a4c6c)
#define SC_COLOR_RING		KL_RGB(0x3d8bff)

static void sc_add(struct sc_mesh *mesh, struct r3_vec a, struct r3_vec b, struct r3_vec c, const float *uv, kl_color color, int texture, unsigned flags);
static struct r3_vec sc_vec(float x, float y, float z);

/*
 * Empties a mesh.
 */
void
sc_mesh_clear(
	struct sc_mesh *mesh)
{
	/* No triangles. */
	mesh->count = 0;
}

/*
 * Adds a quad (corners in order round it; the texture's corners (0, 0),
 * (1, 0), (1, 1) and (0, 1) on them) facing along a normal.
 */
void
sc_quad(
	struct sc_mesh *mesh,
	const struct r3_vec *corners,
	struct r3_vec normal,
	kl_color color,
	int texture,
	unsigned flags)
{
	static const float first[] = { 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f };
	static const float second[] = { 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f };
	struct sc_triangle *triangle;
	struct r3_vertex swap;
	struct r3_vec edge_a;
	struct r3_vec edge_b;
	struct r3_vec across;
	float agree;
	size_t i;

	/* Two triangles. */
	sc_add(mesh, corners[0], corners[1], corners[2], first, color, texture, flags);
	sc_add(mesh, corners[0], corners[2], corners[3], second, color, texture, flags);

	/* A full mesh took neither. */
	if (mesh->count < 2U)
		return;

	/* Each faces along the normal: one turned the other way goes round the other way. */
	for (i = mesh->count - 2U; i < mesh->count; i++) {
		/* Its own normal against the one asked. */
		triangle = &mesh->triangles[i];
		edge_a = sc_vec(triangle->corners[1].position.x - triangle->corners[0].position.x,
				triangle->corners[1].position.y - triangle->corners[0].position.y,
				triangle->corners[1].position.z - triangle->corners[0].position.z);
		edge_b = sc_vec(triangle->corners[2].position.x - triangle->corners[0].position.x,
				triangle->corners[2].position.y - triangle->corners[0].position.y,
				triangle->corners[2].position.z - triangle->corners[0].position.z);
		across = r3_cross(edge_a, edge_b);
		agree = r3_dot(across, normal);
		if (agree < 0.0f) {
			swap = triangle->corners[1];
			triangle->corners[1] = triangle->corners[2];
			triangle->corners[2] = swap;
		}
	}
}

/*
 * Adds a box between two corners, its six faces outwards.
 */
void
sc_box(
	struct sc_mesh *mesh,
	struct r3_vec low,
	struct r3_vec high,
	kl_color color)
{
	struct r3_vec face[4];

	/* The front (-z) and the back (+z). */
	face[0] = sc_vec(low.x, low.y, low.z);
	face[1] = sc_vec(high.x, low.y, low.z);
	face[2] = sc_vec(high.x, high.y, low.z);
	face[3] = sc_vec(low.x, high.y, low.z);
	sc_quad(mesh, face, sc_vec(0.0f, 0.0f, -1.0f), color, SC_TEXTURE_NONE, 0U);
	face[0].z = high.z;
	face[1].z = high.z;
	face[2].z = high.z;
	face[3].z = high.z;
	sc_quad(mesh, face, sc_vec(0.0f, 0.0f, 1.0f), color, SC_TEXTURE_NONE, 0U);

	/* The left (-x) and the right (+x). */
	face[0] = sc_vec(low.x, low.y, low.z);
	face[1] = sc_vec(low.x, high.y, low.z);
	face[2] = sc_vec(low.x, high.y, high.z);
	face[3] = sc_vec(low.x, low.y, high.z);
	sc_quad(mesh, face, sc_vec(-1.0f, 0.0f, 0.0f), color, SC_TEXTURE_NONE, 0U);
	face[0].x = high.x;
	face[1].x = high.x;
	face[2].x = high.x;
	face[3].x = high.x;
	sc_quad(mesh, face, sc_vec(1.0f, 0.0f, 0.0f), color, SC_TEXTURE_NONE, 0U);

	/* The bottom (-y) and the top (+y). */
	face[0] = sc_vec(low.x, low.y, low.z);
	face[1] = sc_vec(high.x, low.y, low.z);
	face[2] = sc_vec(high.x, low.y, high.z);
	face[3] = sc_vec(low.x, low.y, high.z);
	sc_quad(mesh, face, sc_vec(0.0f, -1.0f, 0.0f), color, SC_TEXTURE_NONE, 0U);
	face[0].y = high.y;
	face[1].y = high.y;
	face[2].y = high.y;
	face[3].y = high.y;
	sc_quad(mesh, face, sc_vec(0.0f, 1.0f, 0.0f), color, SC_TEXTURE_NONE, 0U);
}

/*
 * Adds an outline (x, y pairs, round it in order; every point seen from
 * its mean) extruded between two depths: its front and back faces fanned
 * from the mean, and its sides.
 */
void
sc_extrude(
	struct sc_mesh *mesh,
	const float *outline,
	size_t count,
	float front,
	float back,
	kl_color color)
{
	struct r3_vec side[4];
	struct r3_vec normal;
	struct r3_vec outward;
	float agree;
	float cx;
	float cy;
	size_t next;
	size_t i;

	/* The mean of the points, which the faces fan from. */
	cx = 0.0f;
	cy = 0.0f;
	for (i = 0; i < count; i++) {
		cx += outline[2U * i];
		cy += outline[2U * i + 1U];
	}

	/* Their sums divided. */
	cx /= (float)count;
	cy /= (float)count;

	/* Each edge: a triangle of the front, one of the back, and a quad of the side. */
	for (i = 0; i < count; i++) {
		/* The front and the back, facing out (a degenerate quad of three corners and the mean). */
		next = (i + 1U) % count;
		side[0] = sc_vec(cx, cy, front);
		side[1] = sc_vec(outline[2U * i], outline[2U * i + 1U], front);
		side[2] = sc_vec(outline[2U * next], outline[2U * next + 1U], front);
		side[3] = side[0];
		sc_quad(mesh, side, sc_vec(0.0f, 0.0f, -1.0f), color, SC_TEXTURE_NONE, 0U);
		side[0].z = back;
		side[1].z = back;
		side[2].z = back;
		side[3].z = back;
		sc_quad(mesh, side, sc_vec(0.0f, 0.0f, 1.0f), color, SC_TEXTURE_NONE, 0U);

		/* The side, facing away from the mean. */
		side[0] = sc_vec(outline[2U * i], outline[2U * i + 1U], front);
		side[1] = sc_vec(outline[2U * next], outline[2U * next + 1U], front);
		side[2] = sc_vec(outline[2U * next], outline[2U * next + 1U], back);
		side[3] = sc_vec(outline[2U * i], outline[2U * i + 1U], back);
		normal = sc_vec(outline[2U * next + 1U] - outline[2U * i + 1U], -(outline[2U * next] - outline[2U * i]), 0.0f);
		outward = sc_vec(outline[2U * i] - cx, outline[2U * i + 1U] - cy, 0.0f);
		agree = r3_dot(normal, outward);
		if (agree < 0.0f) {
			normal.x = -normal.x;
			normal.y = -normal.y;
		}

		/* The side's quad. */
		sc_quad(mesh, side, normal, color, SC_TEXTURE_NONE, 0U);
	}
}

/*
 * Adds an ellipsoid: its surface in bands of latitude and longitude.
 */
void
sc_ellipsoid(
	struct sc_mesh *mesh,
	struct r3_vec centre,
	struct r3_vec radii,
	kl_color color)
{
	struct r3_vec quad[4];
	struct r3_vec normal;
	float latitude[2];
	float longitude[2];
	int band;
	int step;
	int k;

	/* Each band and each step round it. */
	for (band = 0; band < SC_ELLIPSOID_STEPS; band++) {
		for (step = 0; step < 2 * SC_ELLIPSOID_STEPS; step++) {
			/* The quad's corners on the surface. */
			latitude[0] = SC_PI * ((float)band / (float)SC_ELLIPSOID_STEPS - 0.5f);
			latitude[1] = SC_PI * ((float)(band + 1) / (float)SC_ELLIPSOID_STEPS - 0.5f);
			longitude[0] = SC_PI * (float)step / (float)SC_ELLIPSOID_STEPS;
			longitude[1] = SC_PI * (float)(step + 1) / (float)SC_ELLIPSOID_STEPS;
			for (k = 0; k < 4; k++) {
				/* One corner: the latitudes of 0 and 3 are the first, the longitudes of 0 and 1. */
				quad[k].x = centre.x + radii.x * cosf(latitude[k / 2]) * cosf(longitude[(k == 1 || k == 2)]);
				quad[k].y = centre.y + radii.y * sinf(latitude[k / 2]);
				quad[k].z = centre.z + radii.z * cosf(latitude[k / 2]) * sinf(longitude[(k == 1 || k == 2)]);
			}

			/* Facing out from the centre. */
			normal = sc_vec(quad[0].x + quad[2].x - 2.0f * centre.x, quad[0].y + quad[2].y - 2.0f * centre.y, quad[0].z + quad[2].z - 2.0f * centre.z);
			sc_quad(mesh, quad, normal, color, SC_TEXTURE_NONE, 0U);
		}
	}
}

/*
 * Adds a ring round the x axis through a centre: a tube of a minor
 * radius round a circle of a major one in the plane of y and z.
 */
void
sc_ring(
	struct sc_mesh *mesh,
	struct r3_vec centre,
	float major,
	float minor,
	kl_color color)
{
	struct r3_vec quad[4];
	struct r3_vec middle;
	float around[2];
	float tube[2];
	float radius;
	int i;
	int j;
	int k;

	/* Each step round the circle and round the tube. */
	for (i = 0; i < SC_RING_MAJOR_STEPS; i++) {
		for (j = 0; j < SC_RING_MINOR_STEPS; j++) {
			/* The quad's corners. */
			around[0] = 2.0f * SC_PI * (float)i / (float)SC_RING_MAJOR_STEPS;
			around[1] = 2.0f * SC_PI * (float)(i + 1) / (float)SC_RING_MAJOR_STEPS;
			tube[0] = 2.0f * SC_PI * (float)j / (float)SC_RING_MINOR_STEPS;
			tube[1] = 2.0f * SC_PI * (float)(j + 1) / (float)SC_RING_MINOR_STEPS;
			for (k = 0; k < 4; k++) {
				/* One corner: the steps round the circle of 0 and 3 are the first, round the tube of 0 and 1. */
				radius = major + minor * cosf(tube[k / 2]);
				quad[k].x = centre.x + minor * sinf(tube[k / 2]);
				quad[k].y = centre.y + radius * cosf(around[(k == 1 || k == 2)]);
				quad[k].z = centre.z + radius * sinf(around[(k == 1 || k == 2)]);
			}

			/* Facing out from the circle. */
			middle = sc_vec(centre.x, centre.y + major * cosf(0.5f * (around[0] + around[1])), centre.z + major * sinf(0.5f * (around[0] + around[1])));
			sc_quad(mesh, quad, sc_vec(quad[0].x + quad[2].x - 2.0f * middle.x, quad[0].y + quad[2].y - 2.0f * middle.y, quad[0].z + quad[2].z - 2.0f * middle.z), color, SC_TEXTURE_NONE, 0U);
		}
	}
}

/*
 * Makes the desk calendar: the page shown (turned over the top by an
 * angle while flipping, and fading by an opacity as it goes), the next
 * page under it, the pages under them, the board, the stand, the bar and
 * the rings.
 */
void
sc_desk_calendar(
	struct sc_mesh *mesh,
	float flip,
	float opacity)
{
	struct r3_matrix turn;
	struct r3_matrix to_hinge;
	struct r3_matrix from_hinge;
	struct r3_vec page[4];
	struct r3_vec normal;
	unsigned alpha;
	unsigned flags;
	int i;

	/* The stand behind and the board the pages hang on. */
	page[0] = sc_vec(-0.6f, 0.7f, 0.24f);
	page[1] = sc_vec(0.6f, 0.7f, 0.24f);
	page[2] = sc_vec(0.6f, -1.0f, 0.95f);
	page[3] = sc_vec(-0.6f, -1.0f, 0.95f);
	sc_quad(mesh, page, sc_vec(0.0f, 0.4f, 1.0f), SC_COLOR_STAND, SC_TEXTURE_NONE, R3_TWO_SIDED);
	sc_box(mesh, sc_vec(-0.8f, -1.0f, 0.17f), sc_vec(0.8f, 0.9f, 0.23f), SC_COLOR_BOARD);

	/* The pages under the one shown, a little smaller each, so that their edges show. */
	sc_box(mesh, sc_vec(-0.75f, -0.96f, 0.02f), sc_vec(0.75f, 0.85f, 0.07f), SC_COLOR_PAPER);
	sc_box(mesh, sc_vec(-0.745f, -0.975f, 0.07f), sc_vec(0.745f, 0.85f, 0.12f), SC_COLOR_PAPER_DARK);
	sc_box(mesh, sc_vec(-0.74f, -0.99f, 0.12f), sc_vec(0.74f, 0.85f, 0.17f), SC_COLOR_PAPER);

	/* The next page, which the page turning over shows. */
	page[0] = sc_vec(-SC_PAGE_HALF, SC_PAGE_TOP, 0.015f);
	page[1] = sc_vec(SC_PAGE_HALF, SC_PAGE_TOP, 0.015f);
	page[2] = sc_vec(SC_PAGE_HALF, SC_PAGE_BOTTOM, 0.015f);
	page[3] = sc_vec(-SC_PAGE_HALF, SC_PAGE_BOTTOM, 0.015f);
	sc_quad(mesh, page, sc_vec(0.0f, 0.0f, -1.0f), KL_RGB(0xffffff), SC_TEXTURE_NEXT, 0U);

	/* The page shown, turned about the line at its top while flipping: its face, and its back. */
	r3_translate(&to_hinge, 0.0f, -SC_PAGE_TOP, 0.0f);
	r3_rotate_x(&turn, flip);
	r3_translate(&from_hinge, 0.0f, SC_PAGE_TOP, 0.0f);
	r3_combine(&turn, &to_hinge, &turn);
	r3_combine(&from_hinge, &turn, &turn);
	page[0] = sc_vec(-SC_PAGE_HALF, SC_PAGE_TOP, 0.0f);
	page[1] = sc_vec(SC_PAGE_HALF, SC_PAGE_TOP, 0.0f);
	page[2] = sc_vec(SC_PAGE_HALF, SC_PAGE_BOTTOM, 0.0f);
	page[3] = sc_vec(-SC_PAGE_HALF, SC_PAGE_BOTTOM, 0.0f);
	for (i = 0; i < 4; i++)
		page[i] = r3_apply(&turn, page[i]);
	normal = r3_apply(&turn, sc_vec(0.0f, 0.0f, -1.0f));
	normal.x -= turn.t.x;
	normal.y -= turn.t.y;
	normal.z -= turn.t.z;
	alpha = (unsigned)(opacity * 255.0f + 0.5f);
	flags = 0U;
	if (alpha < 255U)
		flags = R3_NO_DEPTH_WRITE;
	sc_quad(mesh, page, normal, KL_RGBA(0xffffff, alpha), SC_TEXTURE_PAGE, flags);
	normal.x = -normal.x;
	normal.y = -normal.y;
	normal.z = -normal.z;
	sc_quad(mesh, page, normal, (SC_COLOR_BACK & 0xffffffU) | ((kl_color)alpha << 24), SC_TEXTURE_NONE, flags);

	/* The bar at the top and the two rings through the pages. */
	sc_box(mesh, sc_vec(-0.8f, 0.86f, 0.0f), sc_vec(0.8f, 0.98f, 0.23f), SC_COLOR_BOARD);
	sc_ring(mesh, sc_vec(-0.42f, 0.88f, 0.06f), 0.12f, 0.022f, SC_COLOR_RING);
	sc_ring(mesh, sc_vec(0.42f, 0.88f, 0.06f), 0.12f, 0.022f, SC_COLOR_RING);
}

/*
 * Makes the icon of a kind of event: a brief case for work, a heart for
 * personal, a book for study, two people for family.
 */
void
sc_icon(
	struct sc_mesh *mesh,
	enum sc_icon icon)
{
	float heart[2U * SC_HEART_POINTS];
	float t;
	float s;
	int i;

	/* Each kind. */
	switch (icon) {
	case SC_ICON_WORK:
		/* The case, the band round it, the clasp and the handle. */
		sc_box(mesh, sc_vec(-0.8f, -0.56f, -0.28f), sc_vec(0.8f, 0.42f, 0.28f), KL_RGB(0x4f8ef7));
		sc_box(mesh, sc_vec(-0.81f, 0.02f, -0.29f), sc_vec(0.81f, 0.1f, 0.29f), KL_RGB(0x2f6fe0));
		sc_box(mesh, sc_vec(-0.11f, -0.04f, -0.31f), sc_vec(0.11f, 0.16f, -0.28f), KL_RGB(0xdbeafe));
		sc_box(mesh, sc_vec(-0.32f, 0.42f, -0.06f), sc_vec(-0.23f, 0.66f, 0.06f), KL_RGB(0x1e40af));
		sc_box(mesh, sc_vec(0.23f, 0.42f, -0.06f), sc_vec(0.32f, 0.66f, 0.06f), KL_RGB(0x1e40af));
		sc_box(mesh, sc_vec(-0.32f, 0.58f, -0.06f), sc_vec(0.32f, 0.66f, 0.06f), KL_RGB(0x1e40af));
		break;
	case SC_ICON_PERSONAL:
		/* The heart's outline (the usual curve), extruded. */
		for (i = 0; i < SC_HEART_POINTS; i++) {
			t = 2.0f * SC_PI * (float)i / (float)SC_HEART_POINTS;
			s = sinf(t);
			heart[2 * i] = 0.045f * 16.0f * s * s * s;
			heart[2 * i + 1] = 0.045f * (13.0f * cosf(t) - 5.0f * cosf(2.0f * t) - 2.0f * cosf(3.0f * t) - cosf(4.0f * t)) + 0.12f;
		}

		/* Extruded. */
		sc_extrude(mesh, heart, SC_HEART_POINTS, -0.2f, 0.2f, KL_RGB(0xef4444));
		break;
	case SC_ICON_STUDY:
		/* The back cover, the pages, the front cover and the spine. */
		sc_box(mesh, sc_vec(-0.55f, -0.72f, 0.2f), sc_vec(0.62f, 0.72f, 0.27f), KL_RGB(0x2ecc71));
		sc_box(mesh, sc_vec(-0.5f, -0.67f, -0.2f), sc_vec(0.57f, 0.67f, 0.2f), KL_RGB(0xfbfaf5));
		sc_box(mesh, sc_vec(-0.55f, -0.72f, -0.27f), sc_vec(0.62f, 0.72f, -0.2f), KL_RGB(0x2ecc71));
		sc_box(mesh, sc_vec(-0.63f, -0.72f, -0.27f), sc_vec(-0.53f, 0.72f, 0.27f), KL_RGB(0x22a85a));
		sc_box(mesh, sc_vec(0.1f, -0.9f, -0.28f), sc_vec(0.2f, -0.6f, -0.27f), KL_RGB(0xef4444));
		break;
	case SC_ICON_FAMILY:
		/* The taller person and the smaller one: heads and bodies. */
		sc_ellipsoid(mesh, sc_vec(-0.3f, 0.42f, 0.0f), sc_vec(0.2f, 0.2f, 0.2f), KL_RGB(0xfbbf24));
		sc_ellipsoid(mesh, sc_vec(-0.3f, -0.22f, 0.0f), sc_vec(0.33f, 0.42f, 0.26f), KL_RGB(0xf59e0b));
		sc_ellipsoid(mesh, sc_vec(0.34f, 0.18f, -0.08f), sc_vec(0.16f, 0.16f, 0.16f), KL_RGB(0xfbbf24));
		sc_ellipsoid(mesh, sc_vec(0.34f, -0.34f, -0.08f), sc_vec(0.26f, 0.32f, 0.21f), KL_RGB(0xf59e0b));
		break;
	default:
		break;
	}
}

/*
 * Draws a mesh into a target through a matrix (its space to the
 * camera's), with the textures its triangles name.
 */
void
sc_draw(
	struct r3_target *target,
	const struct sc_mesh *mesh,
	const struct r3_matrix *matrix,
	const struct r3_texture *textures)
{
	const struct sc_triangle *triangle;
	const struct r3_texture *texture;
	struct r3_vertex corners[3];
	size_t i;
	int k;

	/* Each triangle, moved into the camera's space. */
	for (i = 0; i < mesh->count; i++) {
		/* Its corners. */
		triangle = &mesh->triangles[i];
		for (k = 0; k < 3; k++) {
			corners[k] = triangle->corners[k];
			corners[k].position = r3_apply(matrix, triangle->corners[k].position);
		}

		/* Its texture, when it names one. */
		texture = NULL;
		if (triangle->texture >= 0 && textures != NULL)
			texture = &textures[triangle->texture];
		r3_triangle(target, corners, triangle->color, texture, triangle->flags);
	}
}

/*
 * Adds a triangle with its texture's coordinates (u, v pairs) when there
 * is room.
 */
static void
sc_add(
	struct sc_mesh *mesh,
	struct r3_vec a,
	struct r3_vec b,
	struct r3_vec c,
	const float *uv,
	kl_color color,
	int texture,
	unsigned flags)
{
	struct sc_triangle *triangle;

	/* A full mesh takes no more. */
	if (mesh->count >= SC_TRIANGLES_MAX)
		return;

	/* The triangle. */
	triangle = &mesh->triangles[mesh->count];
	triangle->corners[0].position = a;
	triangle->corners[1].position = b;
	triangle->corners[2].position = c;
	triangle->corners[0].u = uv[0];
	triangle->corners[0].v = uv[1];
	triangle->corners[1].u = uv[2];
	triangle->corners[1].v = uv[3];
	triangle->corners[2].u = uv[4];
	triangle->corners[2].v = uv[5];
	triangle->color = color;
	triangle->texture = texture;
	triangle->flags = flags;
	mesh->count++;
}

/*
 * Reports a point or a direction of three components.
 */
static struct r3_vec
sc_vec(
	float x,
	float y,
	float z)
{
	struct r3_vec made;

	/* The components. */
	made.x = x;
	made.y = y;
	made.z = z;
	return made;
}
