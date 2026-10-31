/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Complete FIFO draw preparation owns cached GPU inputs without launching a native command list. */
#include <kern/clock.h>
#include <kern/dcache.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"
#include "drivers/gpu/bcm2711/native-state.h"
#include "drivers/gpu/bcm2711/vk/vulkan-native-draw.h"
#if CONFIG_DRIVER_BCM2711_VIDEO
#include "drivers/gpu/bcm2711/vk/vulkan-video.h"
#endif

/* One job's aggregate native staging includes page padding and every draw's independently uploaded inputs. */
#define NATIVE_DRAW_BYTES (256ULL * 1024U * 1024U)

/* One canonical location contributes leading float scalars from a checked coherent source, then Vulkan default components. */
struct native_fetch {
	const uint8_t *cpu;
	uint32_t stride;
	uint32_t source_components;
	uint32_t components;
};

/*
 * One sampled texture's raster source: a whole RGBA8 or BGRA8 image, or one
 * plane of a two-plane NV12 picture.
 *
 * offset and bytes bound the source inside the image's binding; width,
 * height and pitch describe its raster rows of texel texels; swap selects
 * BGRA.  It lives on the stack of one texture's preparation and borrows the
 * image, which the descriptor or the meta draw retains.
 */
struct native_texture_source {
	struct bcm2711_vulkan_resource *image;
	uint64_t offset;
	uint64_t bytes;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	enum bcm2711_native_texel texel;
	uint32_t texel_bytes;
	uint32_t type;
	uint32_t swap;
	/* The mip levels sampled from level 0: one, or the view's levels of a mip-mapped image. */
	uint32_t levels;
};

/* One upload chunk's size, and the largest upload carved from a chunk; larger ones get storage of their own. */
#define NATIVE_UPLOAD_CHUNK_BYTES (64U * 1024U)
#define NATIVE_UPLOAD_LARGEST (16U * 1024U)

/* Every carved upload starts on this boundary, which covers the shader record's and texture records' alignment. */
#define NATIVE_UPLOAD_ALIGNMENT 64U

/* One upload chunk of a pass's pool: its mapped storage and the bytes carved from it. */
struct bcm2711_vulkan_native_upload_chunk {
	struct bcm2711_vulkan_native_upload_chunk *next;
	struct bcm2711_native_storage *storage;
	uint32_t used;
};

/* One chunk of a pass's draw pool: its draws in creation order, the first used of them. */
struct bcm2711_vulkan_native_draw_chunk {
	struct bcm2711_vulkan_native_draw_chunk *next;
	uint32_t used;
	struct bcm2711_vulkan_native_draw draws[BCM2711_VULKAN_DRAW_CHUNK];
};

static int allocate_storage(struct bcm2711_vulkan_native_draw *draw, uint64_t bytes, uint64_t available, const struct bcm2711_shader_binary *program, unsigned fill, struct bcm2711_native_storage **storage);
static struct bcm2711_vulkan_native_draw *pool_take(struct bcm2711_vulkan_native_draw_pool *pool);
static int upload_take(struct bcm2711_vulkan_native_draw *draw, uint64_t bytes, uint64_t available, uint8_t **cpu, uint32_t *gpu);
static struct bcm2711_native_storage *find_texture(const struct bcm2711_vulkan_native_draw *draw, const struct bcm2711_vulkan_resource *image, uint64_t offset, uint32_t levels);
static uint32_t mip_levels(const struct native_texture_source *source, uint32_t address, bool unit, struct bcm2711_native_miptree *tree);
static int prepare_textures(struct bcm2711_vulkan_native_draw *draw, const struct bcm2711_vulkan_prepared_event *event, uint64_t available);
static int prepare_texture(struct bcm2711_vulkan_native_draw *draw, const struct bcm2711_vulkan_descriptor *descriptor, uint64_t available, struct bcm2711_vulkan_native_binding *binding);
static int texture_source(struct bcm2711_vulkan_resource *image, uint32_t plane, uint32_t swap, struct native_texture_source *source);
static bool conversion_supported(const struct native_texture_source *source, uint32_t address);
static uint64_t texture_area(uint32_t size);
static int prepare_texture_image(struct bcm2711_vulkan_native_draw *draw, const struct native_texture_source *texture_source, const struct bcm2711_vulkan_sampler *source, uint64_t available, struct bcm2711_vulkan_native_binding *binding);
static int prepare_conversion(const struct native_texture_source *source, uint32_t address, const struct bcm2711_native_storage *pixels, uint32_t bytes, uint32_t levels, const struct bcm2711_native_miptree *tree, struct bcm2711_v3d_tfu_job *tfu);
static int prepare_meta_texture(struct bcm2711_vulkan_native_draw *draw, const struct bcm2711_vulkan_meta_draw *meta, uint64_t available);
static int prepare_sampler(const struct bcm2711_vulkan_sampler *source, struct bcm2711_native_sampler *sampler);
static struct bcm2711_native_storage *find_program(const struct bcm2711_vulkan_native_draw *draw, const struct bcm2711_shader_binary *program, uint32_t stage);
static int prepare_programs(struct bcm2711_vulkan_native_draw *draw, const struct bcm2711_vulkan_prepared_event *event, uint64_t available, struct bcm2711_native_shader *shader);
static int prepare_records(struct bcm2711_vulkan_native_draw *draw, const struct bcm2711_vulkan_prepared_event *event, uint64_t available, const struct bcm2711_native_shader *shader);
static int prepare_fetch(struct bcm2711_vulkan_native_draw *draw, const struct bcm2711_vulkan_prepared_event *event, uint64_t available, struct bcm2711_native_attribute attributes[BCM2711_VULKAN_VERTEX_ATTRIBUTES]);
static int prepare_indices(struct bcm2711_vulkan_native_draw *draw, const struct bcm2711_vulkan_prepared_event *event);
static int direct_fetch(const struct bcm2711_vulkan_prepared_event *event, uint32_t location, uint32_t components, struct bcm2711_native_attribute *attribute);
static int pack_fetch(struct bcm2711_vulkan_native_draw *draw, const struct bcm2711_vulkan_prepared_event *event, uint64_t available, const uint32_t *locations, const uint32_t *widths, uint32_t count, struct bcm2711_native_attribute attributes[BCM2711_VULKAN_VERTEX_ATTRIBUTES]);
static int resolve_fetch(const struct bcm2711_vulkan_prepared_event *event, uint32_t location, uint32_t components, struct native_fetch *fetch);
static int format_components(VkFormat format, uint32_t *components);
static void write_word(uint8_t *destination, uint32_t word);

/*
 * Builds a complete draw at FIFO execution after earlier writes become CPU-visible.
 *
 * The enclosing job retains the pending prepared primary through DMA and
 * uncertainty.  Code, uniforms and vertices have independent owners; converted
 * texels may borrow an earlier draw in this same pass.  Every referenced draw
 * remains alive until whole-pass retirement.  Failure releases only this
 * root's acquired uploads, leaving borrowed texels with their original owner.
 */
int
bcm2711_vulkan_native_draw_create(
	struct bcm2711_v3d_space *space,
	const struct bcm2711_vulkan_prepared_event *event,
	uint64_t *available,
	const struct bcm2711_vulkan_native_draw *earlier,
	struct bcm2711_vulkan_native_draw_pool *pool,
	struct bcm2711_vulkan_native_draw **draw)
{
	struct bcm2711_vulkan_native_draw *created;
	struct bcm2711_native_shader shader;
	uint64_t started;
	int error;
	int released;

	/* No partial root may escape through the caller's output. */
	if (draw == NULL)
		return EINVAL;
	*draw = NULL;

	/* Only real prepared draws and an identified native controller provide the required immutable inputs. */
	if (space == NULL ||
	    space->native == NULL ||
	    event == NULL ||
	    event->pipeline == NULL)
		return EINVAL;
	if (event->opcode != GPU_OP_CMD_DRAW && event->opcode != GPU_OP_CMD_DRAW_INDEXED)
		return EINVAL;
	if (event->draw[0] == 0 || event->draw[1] == 0)
		return EINVAL;

	/* One finite job-wide budget prevents independent draws from bypassing the total native staging bound. */
	if (available == NULL || *available > NATIVE_DRAW_BYTES)
		return EINVAL;

	/* A pass's draw comes from its pool; an independent draw has an allocation of its own. */
	if (pool != NULL) {
		created = pool_take(pool);
	} else {
		created = kern_calloc(1, sizeof(*created));
	}
	if (created == NULL)
		return ENOMEM;
	created->pool = pool;
	created->space = space;
	created->earlier = earlier;
	created->vertices = event->draw[0];
	created->instances = event->draw[1];
	created->first = event->draw[2];
	created->base_instance = event->draw[3];
	if (event->opcode == GPU_OP_CMD_DRAW_INDEXED) {
		created->base_vertex = event->draw[3];
		created->base_instance = event->draw[4];
	}
	kern_memset(&shader, 0, sizeof(shader));

	/* Real IDENT1 determines available VPM; native encoders reject insufficient capacity without launching. */
	shader.vpm_bytes = ((space->native->hardware.core_ident[1] >> 28) & 15U) * 8192U;

	/* Texture snapshots are made or reused within this pass after prior queue writes retire. */
	started = clock_ticks();
	error = prepare_textures(created, event, *available);
	created->texture_ticks = clock_ticks() - started;
	if (error == 0) {
		/* Separates code/uniform publication from image conversion costs. */
		started = clock_ticks();
		error = prepare_programs(created, event, *available, &shader);
		created->program_ticks = clock_ticks() - started;
	}

	/* Measures vertex fetch and shader records independently of texture uploads. */
	if (error == 0) {
		started = clock_ticks();
		error = prepare_records(created, event, *available, &shader);
		created->record_ticks = clock_ticks() - started;
	}

	/* Binning state is encoded only after all measured input stages succeed. */
	if (error == 0)
		error = bcm2711_vulkan_native_bin_prepare(event, created);

	/* Pre-launch retirement is proven here; failed translation teardown remains in the native space's quarantine. */
	if (error != 0) {
		released = bcm2711_vulkan_native_draw_release(&created, true);
		if (released != 0)
			return released;
		return error;
	}

	/* The job budget changes only when a complete native root becomes caller-owned. */
	created->earlier = NULL;
	*available -= created->bytes;
	*draw = created;

	/* Succeeded: all inputs remain owned by this root or the caller's retained same-pass prefix. */
	return 0;
}

/*
 * Uploads one internal texture quad without publishing or fabricating any Vulkan namespace object.
 */
int
bcm2711_vulkan_native_meta_draw_create(
	struct bcm2711_v3d_space *space,
	const struct bcm2711_vulkan_meta_draw *meta,
	uint64_t *available,
	struct bcm2711_vulkan_native_draw **draw)
{
	struct bcm2711_vulkan_prepared_event *event;
	int error;

	/* Only a complete actual source and independently compiled temporary meta pipeline can supply native inputs. */
	if (draw == NULL)
		return EINVAL;
	*draw = NULL;
	if (meta == NULL || meta->image == NULL)
		return EINVAL;

	/* The large prepared description stays off the kernel stack and owns no public descriptor edge. */
	event = kern_calloc(1, sizeof(*event));
	if (event == NULL)
		return ENOMEM;
	event->opcode = GPU_OP_CMD_DRAW;
	event->pipeline = (struct bcm2711_vulkan_pipeline *)&meta->pipeline;
	event->meta = meta;
	event->width = meta->width;
	event->height = meta->height;
	event->area = meta->area;
	event->scissor = meta->area;
	event->draw[0] = 6;
	event->draw[1] = 1;
	kern_memcpy(event->viewport, meta->viewport, sizeof(event->viewport));

	/* Native upload copies every shader, scalar, source texel and vertex before the temporary description retires. */
	error = bcm2711_vulkan_native_draw_create(space, event, available, NULL, NULL, draw);
	kern_free(event);
	if (error != 0)
		return error;

	/* Succeeded: the returned native root retains no temporary pipeline, geometry or prepared-event pointer. */
	return 0;
}

/*
 * Retires all inputs of one complete or unpublished native draw only after DMA is proved stopped.
 */
int
bcm2711_vulkan_native_draw_release(
	struct bcm2711_vulkan_native_draw **draw,
	bool retired)
{
	struct bcm2711_vulkan_native_draw *owned;
	uint32_t index;
	int error;
	int first;

	/* Missing pointer ownership cannot participate in explicit root consumption. */
	if (draw == NULL)
		return EINVAL;
	owned = *draw;
	if (owned == NULL)
		return 0;

	/* Callback completion alone never retires code or any other GPU-read input. */
	if (!retired)
		return EBUSY;

	/* Lent snapshots get their use back now that the draw's DMA has retired. */
	*draw = NULL;
	for (index = 0; index < owned->texture_count; index++) {
		/* Only lent entries hold a use; private pixels are among the owned storage. */
		if (owned->textures[index].snapshot != NULL)
			bcm2711_native_snapshot_put(owned->textures[index].snapshot);
	}
	if (owned->index_snapshot != NULL) {
		bcm2711_native_snapshot_put(owned->index_snapshot);
		owned->index_snapshot = NULL;
	}

	/* Each mapped reference is consumed once; the space preserves failed unmaps without this CPU root. */
	first = 0;
	for (index = 0; index < owned->count; index++) {
		error = bcm2711_native_storage_release(owned->space, &owned->storage[index], true);
		if (first == 0 && error != 0)
			first = error;
	}

	/* A pooled draw's memory belongs to its pass's chunk; an independent draw is freed now. */
	if (owned->pool == NULL)
		kern_free(owned);

	/* Complete remaining owners still retire after an earlier translation failure, preserving the first refusal. */
	if (first != 0)
		return first;

	/* Succeeded: no independently mapped draw input remains owned by the consumed root. */
	return 0;
}

/* Acquires one bounded padded upload owner and immediately attaches it to the unpublished draw root. */
static int
allocate_storage(
	struct bcm2711_vulkan_native_draw *draw,
	uint64_t bytes,
	uint64_t available,
	const struct bcm2711_shader_binary *program,
	unsigned fill,
	struct bcm2711_native_storage **storage)
{
	uint64_t padded;
	int error;

	/* Output remains absent until a complete native mapping exists. */
	*storage = NULL;
	if (bytes == 0 || bytes > NATIVE_DRAW_BYTES)
		return ENOTSUP;
	padded = (bytes + 4095U) & ~4095ULL;

	/* Both allocation count and the enclosing job's remaining total are checked before physical acquisition. */
	if (draw->count == BCM2711_VULKAN_DRAW_STORAGE ||
	    draw->bytes > available ||
	    padded > available - draw->bytes)
		return ENOMEM;
	error = 0;
	if (program != NULL)
		error = bcm2711_native_program_upload(draw->space, program, storage);
	else
		error = bcm2711_native_storage_create_as(draw->space, bytes, fill, storage);
	if (error != 0)
		return error;

	/* From this point every late refusal can release the whole acquired prefix through its root. */
	draw->storage[draw->count] = *storage;
	draw->count++;
	draw->bytes += padded;

	/* Succeeded: the actual zeroed padded run and its VA are owned before any bytes or pointers are published. */
	return 0;
}

/* Copies only consumed sampled clones; UBO descriptors contribute scalar reads instead of texture storage. */
static int
prepare_textures(
	struct bcm2711_vulkan_native_draw *draw,
	const struct bcm2711_vulkan_prepared_event *event,
	uint64_t available)
{
	const struct bcm2711_vulkan_descriptor *descriptor;
	uint32_t set;
	uint32_t slot;
	int error;

	/* Internal transfer sampling has one exact actual image and a copied filter, with no user descriptor namespace. */
	if (event->meta != NULL) {
		error = prepare_meta_texture(draw, event->meta, available);
		if (error != 0)
			return error;
		return 0;
	}

	/* Canonical consumed masks never reinterpret undefined bits as descriptor slots. */
	for (set = 0; set < BCM2711_VULKAN_PIPELINE_SETS; set++) {
		if ((event->used[set] >> BCM2711_VULKAN_LAYOUT_BINDINGS) != 0)
			return EINVAL;
		for (slot = 0; slot < BCM2711_VULKAN_LAYOUT_BINDINGS; slot++) {
			/* Unconsumed slots require no image conversion or native descriptor record. */
			if ((event->used[set] & (1U << slot)) == 0)
				continue;
			descriptor = &event->descriptors[set][slot];
			if (descriptor->buffer != NULL)
				continue;
			error = prepare_texture(draw, descriptor, available, &draw->bindings[set][slot]);
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: each consumed sampled descriptor points exclusively to job-owned scratch and records. */
	return 0;
}

/* Converts one actual raster image to strict non-XOR UIF and encodes its exact retained sampler in owned aligned storage. */
static int
prepare_texture(
	struct bcm2711_vulkan_native_draw *draw,
	const struct bcm2711_vulkan_descriptor *descriptor,
	uint64_t available,
	struct bcm2711_vulkan_native_binding *binding)
{
	struct bcm2711_vulkan_image_view *image_view;
	struct bcm2711_vulkan_resource *image;
	struct native_texture_source source;
	uint32_t swap;
	int error;

	/* Retained typed clones supply both immutable sampler parameters and the actual image dependency. */
	if (descriptor->view == NULL || descriptor->sampler == NULL)
		return EINVAL;
	if (descriptor->view->kind != I915_VK_OBJ_IMAGE_VIEW || descriptor->sampler->kind != I915_VK_OBJ_SAMPLER)
		return EINVAL;
	image_view = descriptor->view->payload;
	if (image_view == NULL ||
	    image_view->owner.parent == NULL ||
	    descriptor->sampler->payload == NULL)
		return EINVAL;
	image = image_view->owner.parent->payload;
	if (image == NULL)
		return ENOTSUP;

	/* Ordinary sampled input preserves the actual image's canonical red/blue interpretation. */
	swap = 0;
	if (image->format == VK_FORMAT_B8G8R8A8_UNORM)
		swap = 1;

	/* The view's raster: the whole image, or the plane of a picture it reads. */
	error = texture_source(image, image_view->plane, swap, &source);
	if (error != 0)
		return error;

	/* A view of a mip-mapped image samples its levels from level 0. */
	if (image_view->plane == 0 && image_view->levels > 1U && image->levels > 1U) {
		source.levels = image_view->levels;
		if (source.levels > image->levels)
			source.levels = image->levels;
	}
	error = prepare_texture_image(draw, &source, descriptor->sampler->payload, available, binding);
	if (error != 0)
		return error;

	/* Succeeded: each binding uses pass-owned texture storage and independently uploaded sampler records. */
	return 0;
}

/* Stages one actual coherent source and encodes copied sampling choices without borrowing a public descriptor object. */
static int
prepare_texture_image(
	struct bcm2711_vulkan_native_draw *draw,
	const struct native_texture_source *texture_source,
	const struct bcm2711_vulkan_sampler *source,
	uint64_t available,
	struct bcm2711_vulkan_native_binding *binding)
{
	struct bcm2711_vulkan_resource *image;
	struct bcm2711_vulkan_native_texture_copy *entry;
	struct bcm2711_native_storage *pixels;
	uint32_t records_address;
	struct bcm2711_native_texture texture;
	struct bcm2711_native_sampler sampler;
	struct bcm2711_native_miptree tree;
	struct bcm2711_v3d_view *source_view;
	void *cpu;
	uint8_t *destination;
	uint32_t address;
	uint32_t bytes;
	uint64_t started;
	uint64_t fills;
	uint64_t updates;
	uint32_t levels;
	uint32_t lod;
	bool unit;
	char kind;
	int error;

	/* Complete coherent logical raster storage is read only at execution, after previous GPU writes become visible. */
	image = texture_source->image;
	error = bcm2711_vulkan_resource_backing(image, texture_source->offset, texture_source->bytes, &source_view, &address, &cpu);
	if (error != 0)
		return error;
	error = bcm2711_native_texture_size_texel(texture_source->width, texture_source->height, texture_source->texel_bytes, &bytes);
	if (error != 0)
		return error;

	/*
	 * A mip tree when the unit converts each level and the tree can be laid
	 * out (it then takes the whole tree's bytes); otherwise level 0 alone.
	 */
	unit = draw->space->native->hardware.tfu_uif_ready && conversion_supported(texture_source, address);
	levels = mip_levels(texture_source, address, unit && !source_view->buffer->host_mapped, &tree);
	if (levels > 1U)
		bytes = tree.bytes;

	/* Reuse only a snapshot within this pass, which contains no intervening image writes. */
	entry = NULL;
	pixels = find_texture(draw, image, texture_source->offset, levels);
	if (pixels == NULL) {
		/* A new image consumes the same bounded owner slot and staging budget as before. */
		if (draw->texture_count == BCM2711_VULKAN_PIPELINE_SETS * BCM2711_VULKAN_LAYOUT_BINDINGS)
			return ENOMEM;
		entry = &draw->textures[draw->texture_count];

		/* A source no process maps reuses its converted snapshot until a device or kernel write changes it. */
		started = clock_ticks();
		fills = draw->space->snapshot_fills;
		updates = draw->space->snapshot_updates;
		entry->levels = levels;
		error = bcm2711_native_snapshot_get_levels(
			draw->space,
			source_view->buffer,
			address - source_view->address,
			texture_source->width,
			texture_source->height,
			texture_source->pitch,
			texture_source->texel_bytes,
			cpu,
			(size_t)texture_source->bytes,
			unit,
			levels,
			bytes,
			&entry->snapshot);
		draw->copy_ticks += clock_ticks() - started;
		if (error != 0 && error != ENOTSUP)
			return error;
		if (error == 0)
			pixels = entry->snapshot->pixels;

		/* A snapshot left to the unit is filled when the pass runs, before its draws. */
		if (error == 0 && unit && draw->space->snapshot_fills != fills) {
			error = prepare_conversion(texture_source, address, pixels, bytes, levels, &tree, &entry->tfu);
			if (error == 0)
				error = bcm2711_native_storage_clean(pixels);
			if (error != 0) {
				bcm2711_native_snapshot_put(entry->snapshot);
				entry->snapshot = NULL;
				return error;
			}
			entry->convert = true;
		}

		/* Diagnostics count how the texture was obtained and keep the largest conversion. */
		kind = 0;
		if (error == 0 && draw->space->snapshot_updates != updates) {
			draw->snapshot_updates++;
		} else if (error == 0 && draw->space->snapshot_fills == fills) {
			draw->snapshot_hits++;
		} else if (error == 0) {
			draw->snapshot_fills++;
			kind = 'f';
		} else if (source_view->buffer->host_mapped) {
			draw->mapped_textures++;
			draw->mapped_bytes += texture_source->bytes;
			kind = 'm';
		} else {
			draw->private_textures++;
			kind = 'p';
		}

		/* Keeps the largest texture obtained so, for the cost report. */
		if (kind != 0 && (uint64_t)texture_source->width * texture_source->height > texture_area(draw->largest_size)) {
			draw->largest_size = texture_source->width << 16 | texture_source->height;
			draw->largest_kind = kind;
		}
	}

	/* A private conversion serves a source that may change behind the device's back. */
	if (pixels == NULL) {
		/* The conversion writes the image; its padding may keep only this session's earlier bytes. */
		error = allocate_storage(draw, bytes, available, NULL, BCM2711_NATIVE_STORAGE_OWNED, &pixels);
		if (error != 0)
			return error;

		/*
		 * The texture formatting unit converts at GPU speed when the pass
		 * runs; the CPU reads the uncached raster only when it cannot.
		 */
		started = clock_ticks();
		error = ENOTSUP;
		if (draw->space->native->hardware.tfu_uif_ready)
			error = prepare_conversion(texture_source, address, pixels, bytes, levels, &tree, &entry->tfu);
		if (error == 0) {
			entry->convert = true;
		} else if (error == ENOTSUP) {
			error = bcm2711_native_texture_copy_texel(
				texture_source->width,
				texture_source->height,
				texture_source->texel_bytes,
				cpu,
				texture_source->pitch,
				(size_t)texture_source->bytes,
				pixels->view->buffer->address,
				(size_t)pixels->bytes);
		}
		draw->copy_ticks += clock_ticks() - started;
		if (error != 0)
			return error;

		/* Writes back the CPU's zeroing or conversion, so no dirty line can later overwrite device output. */
		error = bcm2711_native_storage_clean(pixels);
		if (error != 0)
			return error;
	}

	/* A newly converted or lent texture becomes this draw's entry for later draws of the pass, published only when complete. */
	if (entry != NULL) {
		entry->image = image;
		entry->offset = texture_source->offset;
		entry->pixels = pixels;
		draw->texture_count++;
	}

	/* Both 24-byte records have independent 32-byte alignment within the same zeroed upload. */
	error = upload_take(draw, 64, available, &destination, &records_address);
	if (error != 0)
		return error;
	kern_memset(&texture, 0, sizeof(texture));
	texture.address = pixels->view->address;
	texture.width = texture_source->width;
	texture.height = texture_source->height;
	texture.bytes = bytes;
	texture.swap_red_blue = texture_source->swap;
	texture.texel = texture_source->texel;
	if (levels > 1U) {
		/* A mip tree is sampled from its level 0, which follows the smaller levels. */
		texture.address = pixels->view->address + tree.level[0].offset;
		texture.levels = levels;
		texture.level0_tiling = tree.level[0].tiling;
		texture.level0_ub_pad = tree.level[0].ub_pad;
	}
	error = bcm2711_native_texture_encode(&texture, destination, 32);
	if (error != 0)
		return error;
	error = prepare_sampler(source, &sampler);
	if (error != 0)
		return error;

	/* The sampler's levels: up to its max LOD within the tree's, blended as it asks. */
	if (levels > 1U) {
		lod = (levels - 1U) << 8;
		if (source->max_lod < lod)
			lod = source->max_lod;
		sampler.max_lod = lod;
		sampler.mip_linear = source->mip_linear;
	}
	error = bcm2711_native_sampler_encode(&sampler, destination + 32, 32);
	if (error != 0)
		return error;
	kern_dcache_clean_range(destination, 64);

	/* Uniform words borrow these exact addresses only after the whole input records have been cleaned. */
	binding->texture = records_address;
	binding->sampler = records_address + 32U;

	/* Succeeded: sampling cannot reference the raster layout or another job's transient descriptor records. */
	return 0;
}

/* Converts the supported Vulkan normalized filters and wraps to numerical hardware choices. */
static int
prepare_sampler(
	const struct bcm2711_vulkan_sampler *source,
	struct bcm2711_native_sampler *sampler)
{
	/* Unsupported retained metadata never silently changes filtering. */
	if ((source->mag != VK_FILTER_NEAREST &&
	    source->mag != VK_FILTER_LINEAR) ||
	    (source->min != VK_FILTER_NEAREST && source->min != VK_FILTER_LINEAR))
		return ENOTSUP;
	kern_memset(sampler, 0, sizeof(*sampler));
	if (source->mag == VK_FILTER_NEAREST)
		sampler->nearest_mag = 1;
	if (source->min == VK_FILTER_NEAREST)
		sampler->nearest_min = 1;

	/* Vulkan wrap enumeration differs from the native clamp/mirror numerical order. */
	switch (source->u) {
	case VK_SAMPLER_ADDRESS_MODE_REPEAT:
		break;
	case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:
		sampler->wrap_u = 1;
		break;
	case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT:
		sampler->wrap_u = 2;
		break;
	default:
		return ENOTSUP;
	}

	/* V uses the same explicitly supported conversion without inheriting U's choice. */
	switch (source->v) {
	case VK_SAMPLER_ADDRESS_MODE_REPEAT:
		break;
	case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:
		sampler->wrap_v = 1;
		break;
	case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT:
		sampler->wrap_v = 2;
		break;
	default:
		return ENOTSUP;
	}

	/* Succeeded: native sampler encoding receives exact supported filter and wrap values. */
	return 0;
}

/* Reuses immutable pass code and defaults while uploading each draw's exact FIFO scalar streams. */
static int
prepare_programs(
	struct bcm2711_vulkan_native_draw *draw,
	const struct bcm2711_vulkan_prepared_event *event,
	uint64_t available,
	struct bcm2711_native_shader *shader)
{
	const struct bcm2711_shader_binary *program;
	struct bcm2711_native_storage *code;
	uint32_t uniforms;
	struct bcm2711_native_storage *defaults;
	uint8_t *destination;
	uint64_t bytes;
	uint32_t stage;
	uint32_t index;
	int error;

	/* Shared code stays pass-owned; each draw keeps independent current scalar consumption. */
	for (stage = 0; stage < 3; stage++) {
		program = event->pipeline->programs[stage];
		if (program == NULL ||
		    program->code == NULL ||
		    program->code_count == 0 ||
		    (uint32_t)program->stage != stage)
			return EINVAL;
		/*
		 * A user pipeline's code is uploaded once and kept with the pipeline;
		 * an internal draw's program is uploaded once in the pass (only exact
		 * compiled program identity permits an earlier upload to be reused).
		 */
		code = NULL;
		if (event->meta == NULL && event->pipeline->code[stage] != NULL && event->pipeline->code[stage]->space == draw->space)
			code = event->pipeline->code[stage];
		if (code == NULL)
			code = find_program(draw, program, stage);
		if (code == NULL && event->meta == NULL && event->pipeline->code[stage] == NULL) {
			error = bcm2711_native_program_upload(draw->space, program, &code);
			if (error != 0)
				return error;
			event->pipeline->code[stage] = code;
		}
		if (code == NULL) {
			bytes = (uint64_t)program->code_count * 8U;
			error = allocate_storage(draw, bytes, available, program, BCM2711_NATIVE_STORAGE_WRITTEN, &code);
			if (error != 0)
				return error;
		}

		/* The pass retains the owning draw even if a later draw borrows this same upload. */
		draw->programs[stage] = program;
		draw->code[stage] = code;

		/* Empty streams still receive a valid aligned placeholder, without fabricating a consumed word. */
		bytes = (uint64_t)program->uniform_count * 4U;
		if (bytes == 0)
			bytes = 4;
		error = upload_take(draw, bytes, available, &destination, &uniforms);
		if (error != 0)
			return error;

		/* Push constants and texture bindings always produce this draw's own scalar stream, written in place; an empty stream's placeholder is a written zero. */
		error = bcm2711_vulkan_uniform_write(event, stage, draw->bindings, destination, program->uniform_count);
		if (error != 0)
			return error;
		if (program->uniform_count == 0)
			write_word(destination, 0U);
		kern_dcache_clean_range(destination, (size_t)bytes);
		shader->programs[stage].binary = program;
		shader->programs[stage].code = code->view->address;
		shader->programs[stage].uniforms = uniforms;
	}

	/* Every default attribute is exactly float (0,0,0,1), independent of pipeline and draw. */
	defaults = NULL;
	if (draw->earlier != NULL)
		defaults = draw->earlier->defaults;

	/* Only the first draw allocates defaults; later draws borrow its whole-pass lifetime. */
	if (defaults == NULL) {
		error = allocate_storage(draw, 256, available, NULL, BCM2711_NATIVE_STORAGE_ZEROED, &defaults);
		if (error != 0)
			return error;
		destination = defaults->view->buffer->address;
		for (index = 0; index < 16; index++)
			write_word(destination + index * 16U + 12U, 0x3f800000U);
		error = bcm2711_native_storage_clean(defaults);
		if (error != 0)
			return error;
	}

	/* The shader record references only an existing independently owned pass allocation. */
	draw->defaults = defaults;
	shader->defaults = defaults->view->address;

	/* Succeeded: shader record construction can reference only independently uploaded complete code/scalar/default intervals. */
	return 0;
}

/* Finds an exact immutable compiled program uploaded by an earlier retained draw. */
static struct bcm2711_native_storage *
find_program(
	const struct bcm2711_vulkan_native_draw *draw,
	const struct bcm2711_shader_binary *program,
	uint32_t stage)
{
	const struct bcm2711_vulkan_native_draw *owner;

	/* The pass retains all earlier owners until no referencing command can execute. */
	for (owner = draw->earlier; owner != NULL; owner = owner->next) {
		if (owner->programs[stage] == program)
			return owner->code[stage];
	}

	/* A different compiled object receives its own upload even if its words happen to match. */
	return NULL;
}

/* Encodes contiguous shader and canonical fetch records after every backing interval exists. */
static int
prepare_records(
	struct bcm2711_vulkan_native_draw *draw,
	const struct bcm2711_vulkan_prepared_event *event,
	uint64_t available,
	const struct bcm2711_native_shader *shader)
{
	struct bcm2711_native_attribute attributes[BCM2711_VULKAN_VERTEX_ATTRIBUTES];
	uint32_t records;
	uint8_t *destination;
	uint32_t record_bytes;
	uint32_t bytes;
	uint32_t index;
	int error;

	/* Each fetch record receives a bounded private packed source rather than a speculative user-buffer stride. */
	kern_memset(attributes, 0, sizeof(attributes));
	error = prepare_fetch(draw, event, available, attributes);
	if (error != 0)
		return error;

	/* V3D 4.2 requires at least one CS/VS attribute read even when neither compiled program consumes an input (GFXH-930). */
	if (draw->attributes == 0) {
		attributes[0].address = shader->defaults;
		attributes[0].components = 1;
		attributes[0].coordinate_values = 1;
		attributes[0].vertex_values = 1;
		attributes[0].maximum_index = draw->vertices - 1U;
		draw->attributes = 1;
	}

	/* The mandatory unused fetch reuses owned defaults and requires no extra physical allocation; the record's size is the board encoder's. */
	record_bytes = bcm2711_native_shader_bytes();
	bytes = record_bytes + draw->attributes * BCM2711_NATIVE_ATTRIBUTE_BYTES;
	error = upload_take(draw, bytes, available, &destination, &records);
	if (error != 0)
		return error;
	error = bcm2711_native_shader_encode(shader, destination, bytes);
	if (error != 0)
		return error;

	/* Hardware locates each attribute immediately after the shader state record. */
	for (index = 0; index < draw->attributes; index++) {
		error = bcm2711_native_attribute_encode(&attributes[index], destination + record_bytes + index * BCM2711_NATIVE_ATTRIBUTE_BYTES, BCM2711_NATIVE_ATTRIBUTE_BYTES);
		if (error != 0)
			return error;
	}

	/* No pointer can reach the native BCL before the complete records are CPU-clean. */
	kern_dcache_clean_range(destination, bytes);
	draw->shader = records;
	draw->vcm = bcm2711_native_shader_vcm(shader);

	/* Succeeded: one aligned native shader pointer and its exact attribute count describe this complete owned draw. */
	return 0;
}

/*
 * Describes the attribute fetch of one draw's shader inputs.
 *
 * A user draw's hardware reads each location in place from its vertex
 * buffer: address, stride and per-instance divisor go into the attribute
 * record, so no vertex is copied.  A location the hardware cannot read so,
 * and the private meta quad, take the packed copy instead, which only a
 * single-instance vertex array can use.
 */
static int
prepare_fetch(
	struct bcm2711_vulkan_native_draw *draw,
	const struct bcm2711_vulkan_prepared_event *event,
	uint64_t available,
	struct bcm2711_native_attribute attributes[BCM2711_VULKAN_VERTEX_ATTRIBUTES])
{
	const struct bcm2711_shader_binary *coordinate;
	const struct bcm2711_shader_binary *vertex;
	uint32_t locations[BCM2711_VULKAN_VERTEX_ATTRIBUTES];
	uint32_t widths[BCM2711_VULKAN_VERTEX_ATTRIBUTES];
	uint32_t coordinate_widths[BCM2711_VULKAN_VERTEX_ATTRIBUTES];
	bool read_any;
	uint32_t input;
	uint32_t location;
	uint32_t components;
	uint32_t count;
	uint32_t index;
	bool direct;
	int error;

	/* The coordinate variant reads at most the vertex variant's canonical inputs. */
	coordinate = event->pipeline->programs[BCM2711_SHADER_COORDINATE];
	vertex = event->pipeline->programs[BCM2711_SHADER_VERTEX];
	if (coordinate->input_count > vertex->input_count || vertex->input_count > BCM2711_SHADER_INTERFACE_WORDS)
		return EINVAL;

	/* Shader locations are ascending and every declared component is a leading consecutive scalar. */
	count = 0;
	input = 0;
	while (input < vertex->input_count) {
		location = vertex->inputs[input].location;
		if (location >= BCM2711_VULKAN_VERTEX_ATTRIBUTES || count == BCM2711_VULKAN_VERTEX_ATTRIBUTES)
			return EINVAL;
		if (count != 0 && location <= vertex->inputs[input - 1U].location)
			return EINVAL;
		components = 0;
		while (input < vertex->input_count && vertex->inputs[input].location == location) {
			if (vertex->inputs[input].component != components || components == 4)
				return EINVAL;
			components++;
			input++;
		}
		locations[count] = location;
		widths[count] = components;
		count++;
	}

	/*
	 * The coordinate variant's leading components of each location, in
	 * the same ascending order; a location it does not read has none.
	 */
	input = 0;
	for (index = 0; index < count; index++) {
		coordinate_widths[index] = 0;
		while (input < coordinate->input_count && coordinate->inputs[input].location == locations[index]) {
			if (coordinate->inputs[input].component != coordinate_widths[index] ||
			    coordinate_widths[index] == widths[index])
				return EINVAL;
			coordinate_widths[index]++;
			input++;
		}
	}
	if (input != coordinate->input_count)
		return EINVAL;

	/* An indexed draw's indices are read in place as well. */
	if (event->opcode == GPU_OP_CMD_DRAW_INDEXED) {
		error = prepare_indices(draw, event);
		if (error != 0)
			return error;
	}

	/* Each location of a user draw is read in place unless one of them cannot be. */
	direct = false;
	if (event->meta == NULL)
		direct = true;
	for (index = 0; index < count && direct; index++) {
		error = direct_fetch(event, locations[index], widths[index], &attributes[index]);
		if (error == ENOTSUP) {
			direct = false;
		} else if (error != 0) {
			return error;
		}
	}

	/* The hardware reads every location in place, or from the packed copy, which holds one instance of a vertex array only. */
	if (direct) {
		draw->attributes = count;
	} else {
		if (event->opcode != GPU_OP_CMD_DRAW || draw->instances != 1 || draw->base_instance != 0)
			return ENOTSUP;
		kern_memset(attributes, 0, sizeof(*attributes) * BCM2711_VULKAN_VERTEX_ATTRIBUTES);
		error = pack_fetch(draw, event, available, locations, widths, count, attributes);
		if (error != 0)
			return error;
	}

	/*
	 * The coordinate variant is given only the components it reads.  The
	 * hardware needs at least one value read by each variant (GFXH-930),
	 * so a coordinate variant that reads nothing is given one value of
	 * the first attribute, which it never loads.
	 * Procedure referenced: Mesa 25.3.6 src/broadcom/vulkan/v3dvx_cmd_buffer.c
	 * (the GFXH-930 dummy coordinate shader load).
	 */
	read_any = false;
	for (index = 0; index < count; index++) {
		attributes[index].coordinate_values = coordinate_widths[index];
		if (coordinate_widths[index] != 0)
			read_any = true;
	}
	if (count != 0 && !read_any)
		attributes[0].coordinate_values = 1;

	/* Succeeded: every attribute record points into its vertex buffer or the packed copy. */
	return 0;
}

/*
 * Resolves an indexed draw's index buffer to the interval the hardware
 * reads: from the bound offset to the end of the buffer, the first index
 * as a byte offset into it.  Recording checked that the drawn indices lie
 * inside the buffer.
 */
static int
prepare_indices(
	struct bcm2711_vulkan_native_draw *draw,
	const struct bcm2711_vulkan_prepared_event *event)
{
	struct bcm2711_vulkan_resource *resource;
	struct bcm2711_v3d_view *view;
	void *cpu;
	uint64_t bytes;
	uint64_t first;
	uint32_t address;
	int error;

	/* The retained index buffer and its type, 16 or 32 bits. */
	resource = event->indices;
	if (resource == NULL || event->index_offset >= resource->bytes)
		return EINVAL;
	draw->index_size = 2U;
	if (event->index_type == VK_INDEX_TYPE_UINT32)
		draw->index_size = 4U;

	/* The interval from the bound offset to the end of the buffer, in current storage. */
	bytes = resource->bytes - event->index_offset;
	if (bytes > 0xffffffffU)
		return EINVAL;
	error = bcm2711_vulkan_resource_backing(resource, event->index_offset, bytes, &view, &address, &cpu);
	if (error != 0)
		return error;

	/* The first index is a byte offset inside that interval. */
	first = (uint64_t)event->draw[2] * draw->index_size;
	if (first > bytes)
		return EINVAL;
	draw->indexed = 1;
	draw->index_address = address;
	draw->index_bytes = (uint32_t)bytes;
	draw->first = (uint32_t)first;

	/*
	 * 32-bit indices that all fit 16 bits are read from a 16-bit copy kept
	 * until the buffer changes, which halves what binning reads of them
	 * (each instance reads its indices again).  Without one the hardware
	 * reads the 32-bit indices in place.
	 */
	if (draw->index_size == 4U && bytes >= 4U && draw->index_snapshot == NULL) {
		error = bcm2711_native_snapshot_get_indices(
			draw->space,
			view->buffer,
			address - view->address,
			(uint32_t)(bytes / 4U),
			cpu,
			(size_t)bytes,
			&draw->index_snapshot);
		if (error == 0) {
			draw->index_size = 2U;
			draw->index_address = draw->index_snapshot->pixels->view->address;
			draw->index_bytes = (uint32_t)(bytes / 4U) * 2U;
			draw->first = (uint32_t)(first / 2U);
		} else if (error != ENOTSUP) {
			return error;
		}
	}

	/* Succeeded: the hardware reads the draw's indices in place or from their 16-bit copy. */
	return 0;
}

/*
 * Describes one location the hardware reads in place from its vertex
 * buffer, or reports ENOTSUP when it cannot: the shader reads more
 * components than the format has, or the address or stride is not a
 * multiple of four bytes.
 *
 * The maximum index keeps every fetch inside the buffer, whichever
 * vertex an index or instance names (the hardware's field has 24 bits).
 */
static int
direct_fetch(
	const struct bcm2711_vulkan_prepared_event *event,
	uint32_t location,
	uint32_t components,
	struct bcm2711_native_attribute *attribute)
{
	const VkVertexInputAttributeDescription *description;
	const VkVertexInputBindingDescription *binding;
	struct bcm2711_vulkan_resource *resource;
	struct bcm2711_v3d_view *view;
	void *cpu;
	uint64_t begin;
	uint64_t bytes;
	uint64_t maximum;
	uint32_t address;
	uint32_t source_components;
	uint32_t index;
	int error;

	/* Immutable pipeline array counts precede all declaration searches. */
	if (event->pipeline->attribute_count > BCM2711_VULKAN_VERTEX_ATTRIBUTES || event->pipeline->binding_count > BCM2711_VULKAN_VERTEX_BINDINGS)
		return EINVAL;
	description = NULL;
	for (index = 0; index < event->pipeline->attribute_count; index++) {
		if (event->pipeline->attributes[index].location == location)
			description = &event->pipeline->attributes[index];
	}

	/* A shader-consumed location cannot inherit an undefined user vertex source. */
	if (description == NULL || description->binding >= BCM2711_VULKAN_VERTEX_BINDINGS)
		return EINVAL;
	resource = event->vertices[description->binding];
	if (resource == NULL)
		return EINVAL;

	/* The binding declaring the stride and input rate. */
	binding = NULL;
	for (index = 0; index < event->pipeline->binding_count; index++) {
		if (event->pipeline->bindings[index].binding == description->binding)
			binding = &event->pipeline->bindings[index];
	}
	if (binding == NULL)
		return EINVAL;
	error = format_components(description->format, &source_components);
	if (error != 0)
		return error;

	/* Missing format components would need Vulkan's defaults, which only the packed copy supplies. */
	if (components > source_components)
		return ENOTSUP;

	/* The hardware reads whole aligned floats. */
	begin = event->offsets[description->binding];
	if (begin > resource->bytes || description->offset > resource->bytes - begin)
		return EINVAL;
	begin += description->offset;
	if ((begin & 3U) != 0 || (binding->stride & 3U) != 0)
		return ENOTSUP;

	/* At least the first element lies inside the buffer's current storage. */
	bytes = source_components * 4U;
	if (bytes > resource->bytes - begin)
		return EINVAL;
	error = bcm2711_vulkan_resource_backing(resource, begin, bytes, &view, &address, &cpu);
	if (error != 0)
		return error;

	/* The last element whose bytes all lie inside the buffer bounds every fetch. */
	maximum = 0xffffffU;
	if (binding->stride != 0)
		maximum = (resource->bytes - begin - bytes) / binding->stride;
	if (maximum > 0xffffffU)
		maximum = 0xffffffU;

	/* The record: format width, the shader's leading components, stride and rate. */
	attribute->address = address;
	attribute->stride = binding->stride;
	attribute->maximum_index = (uint32_t)maximum;
	attribute->components = source_components;
	attribute->coordinate_values = components;
	attribute->vertex_values = components;
	attribute->divisor = 0;
	if (binding->inputRate == VK_VERTEX_INPUT_RATE_INSTANCE)
		attribute->divisor = 1;

	/* Succeeded: the record reads the location from the user's buffer. */
	return 0;
}

/* Packs canonical float inputs once, supplying missing format components and rebasing the copied first vertex to zero. */
static int
pack_fetch(
	struct bcm2711_vulkan_native_draw *draw,
	const struct bcm2711_vulkan_prepared_event *event,
	uint64_t available,
	const uint32_t *locations,
	const uint32_t *widths,
	uint32_t count,
	struct bcm2711_native_attribute attributes[BCM2711_VULKAN_VERTEX_ATTRIBUTES])
{
	struct native_fetch fetch[BCM2711_VULKAN_VERTEX_ATTRIBUTES];
	uint32_t fetched;
	uint8_t *destination;
	const uint8_t *source;
	uint64_t bytes;
	uint32_t stride;
	uint32_t item;
	uint32_t scalar;
	uint32_t index;
	uint32_t offset;
	int error;

	/* Resolve complete logical source intervals before any packed GPU input can be published. */
	kern_memset(fetch, 0, sizeof(fetch));
	stride = 0;
	for (index = 0; index < count; index++) {
		error = resolve_fetch(event, locations[index], widths[index], &fetch[index]);
		if (error != 0)
			return error;
		stride += widths[index] * 4U;
	}

	/* The copy starts at the source's first vertex, so the hardware's array starts at zero. */
	draw->first = 0;

	/* Zero-input shaders need no fetch allocation; the independently uploaded defaults still exist. */
	if (stride != 0) {
		bytes = (uint64_t)draw->vertices * stride;
		error = upload_take(draw, bytes, available, &destination, &fetched);
		if (error != 0)
			return error;

		/* Each copied vertex contains only consumed scalars; native draws start at zero after source firstVertex is applied. */
		for (item = 0; item < draw->vertices; item++) {
			offset = 0;
			for (index = 0; index < count; index++) {
				source = fetch[index].cpu + (size_t)item * fetch[index].stride;
				for (scalar = 0; scalar < fetch[index].components; scalar++) {
					/* Missing float format components follow Vulkan's zero/one defaults instead of reading neighbouring attributes. */
					if (scalar < fetch[index].source_components)
						kern_memcpy(destination + (size_t)item * stride + offset, source + scalar * 4U, 4);
					else if (scalar == 3)
						write_word(destination + (size_t)item * stride + offset, 0x3f800000U);
					else
						write_word(destination + (size_t)item * stride + offset, 0);
					offset += 4;
				}
			}
		}

		/* Exact leading counts preserve both VPM FIFOs while native maximum index cannot enter padded storage. */
		offset = 0;
		for (index = 0; index < count; index++) {
			attributes[index].address = fetched + offset;
			attributes[index].stride = stride;
			attributes[index].maximum_index = draw->vertices - 1U;
			attributes[index].components = fetch[index].components;
			attributes[index].coordinate_values = fetch[index].components;
			attributes[index].vertex_values = fetch[index].components;
			offset += fetch[index].components * 4U;
		}

		/* Complete packed values become device-visible before record construction. */
		kern_dcache_clean_range(destination, (size_t)bytes);
	}

	/* No attribute metadata retains a coherent source pointer after the complete packed GPU copy exists. */
	draw->attributes = count;

	/* Succeeded: all logical source strides are replaced by checked independently owned native fetch intervals. */
	return 0;
}

/* Resolves one location's exact source format, binding and last fetched logical vertex. */
static int
resolve_fetch(
	const struct bcm2711_vulkan_prepared_event *event,
	uint32_t location,
	uint32_t components,
	struct native_fetch *fetch)
{
	const VkVertexInputAttributeDescription *attribute;
	const VkVertexInputBindingDescription *binding;
	struct bcm2711_vulkan_resource *resource;
	struct bcm2711_v3d_view *view;
	void *cpu;
	uint64_t begin;
	uint64_t bytes;
	uint32_t address;
	uint32_t index;
	int error;

	/* Internal geometry already contains exactly six owned position/UV vertices; no typed buffer or fake allocation is involved. */
	if (event->meta != NULL) {
		if ((location == 0 &&
		    components != 4) ||
		    (location == 1 &&
		    components != 2) ||
		    location > 1)
			return EINVAL;
		fetch->cpu = (const uint8_t *)event->meta->vertices;
		if (location == 1)
			fetch->cpu += 16;
		fetch->stride = 24;
		fetch->source_components = components;
		fetch->components = components;
		return 0;
	}

	/* Immutable pipeline array counts precede all declaration searches. */
	if (event->pipeline->attribute_count > BCM2711_VULKAN_VERTEX_ATTRIBUTES || event->pipeline->binding_count > BCM2711_VULKAN_VERTEX_BINDINGS)
		return EINVAL;
	attribute = NULL;
	for (index = 0; index < event->pipeline->attribute_count; index++) {
		if (event->pipeline->attributes[index].location == location)
			attribute = &event->pipeline->attributes[index];
	}

	/* A shader-consumed location cannot inherit an undefined user vertex source. */
	if (attribute == NULL || attribute->binding >= BCM2711_VULKAN_VERTEX_BINDINGS)
		return EINVAL;
	resource = event->vertices[attribute->binding];
	if (resource == NULL)
		return EINVAL;
	binding = NULL;
	for (index = 0; index < event->pipeline->binding_count; index++) {
		if (event->pipeline->bindings[index].binding == attribute->binding)
			binding = &event->pipeline->bindings[index];
	}

	/* Only per-vertex float fetch is implemented; absent declarations cannot provide an implicit stride. */
	if (binding == NULL || binding->inputRate != VK_VERTEX_INPUT_RATE_VERTEX)
		return EINVAL;
	error = format_components(attribute->format, &fetch->source_components);
	if (error != 0)
		return error;

	/* Source firstVertex is applied before packing; every last complete source format remains inside logical resource bytes. */
	begin = event->offsets[attribute->binding];
	if (begin > resource->bytes || attribute->offset > resource->bytes - begin)
		return EINVAL;
	begin += attribute->offset;
	bytes = (uint64_t)event->draw[2] * binding->stride;
	if (bytes > resource->bytes - begin)
		return EINVAL;
	begin += bytes;
	bytes = (uint64_t)(event->draw[0] - 1U) * binding->stride + fetch->source_components * 4U;
	error = bcm2711_vulkan_resource_backing(resource, begin, bytes, &view, &address, &cpu);
	if (error != 0)
		return error;
	fetch->cpu = cpu;
	fetch->stride = binding->stride;
	fetch->components = components;

	/* Succeeded: the borrowed pointer remains valid under the controller mutex until this synchronous packed copy finishes. */
	return 0;
}

/* Maps only the float formats already accepted by native graphics pipeline creation to exact physical source widths. */
static int
format_components(
	VkFormat format,
	uint32_t *components)
{
	/* Every admitted format contributes a finite number of four-byte scalar words. */
	switch (format) {
	case VK_FORMAT_R32_SFLOAT:
		*components = 1;
		break;
	case VK_FORMAT_R32G32_SFLOAT:
		*components = 2;
		break;
	case VK_FORMAT_R32G32B32_SFLOAT:
		*components = 3;
		break;
	case VK_FORMAT_R32G32B32A32_SFLOAT:
		*components = 4;
		break;
	default:
		return ENOTSUP;
	}

	/* Succeeded: later logical source checks never use native allocation padding as another float component. */
	return 0;
}

/* Stores one numerical native word explicitly in little-endian byte order. */
static void
write_word(
	uint8_t *destination,
	uint32_t word)
{
	/* Unaligned record fields and host fixtures observe the same exact target byte order. */
	destination[0] = (uint8_t)word;
	destination[1] = (uint8_t)(word >> 8);
	destination[2] = (uint8_t)(word >> 16);
	destination[3] = (uint8_t)(word >> 24);

	/* Succeeded: the complete numerical word occupies its exact four-byte native field. */
	return;
}

/*
 * Describes the raster a texture samples: the whole RGBA8 or BGRA8 image
 * (plane 0), or a plane of a sampled NV12 picture -- 1 its luma as R8, 2 its
 * interleaved chroma as RG8 at half the width and height.
 */
static int
texture_source(
	struct bcm2711_vulkan_resource *image,
	uint32_t plane,
	uint32_t swap,
	struct native_texture_source *source)
{
#if CONFIG_DRIVER_BCM2711_VIDEO
	uint64_t chroma;
	uint64_t bytes;
	uint32_t pitch;
#endif
	int error;

	/* A whole RGBA8 or BGRA8 image, at its level 0 alone. */
	kern_memset(source, 0, sizeof(*source));
	source->image = image;
	source->levels = 1;
	if (plane == 0) {
		if (image->format != VK_FORMAT_R8G8B8A8_UNORM && image->format != VK_FORMAT_B8G8R8A8_UNORM)
			return ENOTSUP;
		source->offset = 0;
		source->bytes = image->bytes;
		source->width = image->width;
		source->height = image->height;
		source->pitch = image->pitch;
		source->texel = BCM2711_NATIVE_TEXEL_RGBA8;
		source->swap = swap;
	}

#if CONFIG_DRIVER_BCM2711_VIDEO
	/* A plane of an NV12 picture, in the decoder's layout (vulkan-video.c). */
	if (plane != 0) {
		if (image->format != (VkFormat)BCM2711_VIDEO_FORMAT_NV12)
			return ENOTSUP;
		bcm2711_vulkan_video_nv12_layout(image->width, image->height, &pitch, &chroma, &bytes);
		if (bytes > image->bytes)
			return EINVAL;
		source->pitch = pitch;
		if (plane == 1U) {
			/* The luma rows from the image's start. */
			source->offset = 0;
			source->bytes = chroma;
			source->width = image->width;
			source->height = image->height;
			source->texel = BCM2711_NATIVE_TEXEL_R8;
		} else if (plane == 2U) {
			/* The chroma rows after the luma plane, one Cb and Cr pair for every two by two samples. */
			source->offset = chroma;
			source->bytes = bytes - chroma;
			source->width = (image->width + 1U) / 2U;
			source->height = (image->height + 1U) / 2U;
			source->texel = BCM2711_NATIVE_TEXEL_RG8;
		} else {
			return EINVAL;
		}
	}
#else
	/* Without video, an image has no planes. */
	if (plane != 0)
		return ENOTSUP;
#endif

	/* The texel's size and native type. */
	error = bcm2711_native_texel_layout(source->texel, &source->texel_bytes, &source->type);
	if (error != 0)
		return error;

	/* Succeeded: the source names the raster's place, extent and texels. */
	return 0;
}

/* Reports whether the texture formatting unit can read the raster image at address. */
static bool
conversion_supported(
	const struct native_texture_source *source,
	uint32_t address)
{
	/*
	 * The unit copies texels unchanged: RGBA8 (which also keeps BGRA bytes
	 * in place), and the R8 and RG8 planes of a picture (the
	 * unit's one- and two-byte UIF matched the CPU's on the Pi).
	 */
	if (source->texel_bytes != 1U && source->texel_bytes != 2U && source->texel_bytes != 4U)
		return false;

	/* Raster input is addressed in whole texels from a 64-byte aligned start. */
	if ((address & 63U) != 0 ||
	    (source->pitch % source->texel_bytes) != 0 ||
	    source->pitch / source->texel_bytes < source->width)
		return false;

	/* The stride field holds sixteen bits of texels. */
	if (source->pitch / source->texel_bytes > 0xffffU)
		return false;

	/* Succeeded: the unit reads this raster. */
	return true;
}

/*
 * Describes one raster-to-UIF conversion by the texture formatting unit.
 *
 * ENOTSUP leaves the CPU conversion in charge of a layout the unit cannot read.
 */
static int
prepare_conversion(
	const struct native_texture_source *source,
	uint32_t address,
	const struct bcm2711_native_storage *pixels,
	uint32_t bytes,
	uint32_t levels,
	const struct bcm2711_native_miptree *tree,
	struct bcm2711_v3d_tfu_job *tfu)
{
	struct bcm2711_native_conversion conversion;
	uint64_t input_bytes;
	uint32_t block_height;
	bool supported;
	int error;

	/* Formats and layouts the unit cannot read stay with the CPU conversion. */
	supported = conversion_supported(source, address);
	if (!supported)
		return ENOTSUP;

	/* The read covers every visible texel of every row inside the source's binding. */
	input_bytes = (uint64_t)(source->height - 1U) * source->pitch + (uint64_t)source->width * source->texel_bytes;
	if (input_bytes > source->bytes || input_bytes > 0xffffffffU)
		return EINVAL;

	/* The output is the whole strict UIF scratch image, without extra block padding. */
	if (bytes > pixels->bytes || (pixels->view->address & 63U) != 0)
		return EINVAL;

	/*
	 * Input is raster of the texel's type and of the given stride in
	 * texels; output is UIF without XOR at level 0, its rows the image's
	 * whole UIF blocks.
	 */
	kern_memset(tfu, 0, sizeof(*tfu));
	tfu->input = address;
	tfu->input_bytes = (uint32_t)input_bytes;
	tfu->output = pixels->view->address;
	tfu->output_bytes = bytes;
	tfu->stride = source->pitch / source->texel_bytes;
	tfu->size = (source->height << 16) | source->width;
	kern_memset(&conversion, 0, sizeof(conversion));
	conversion.type = source->type;
	conversion.texel_bytes = source->texel_bytes;
	conversion.tiling = BCM2711_NATIVE_TILING_UIF;
	conversion.height = source->height;
	block_height = source->texel_bytes == 1U ? 16U : 8U;
	conversion.padded_height = (source->height + block_height - 1U) / block_height * block_height;

	/* A mip tree's level 0 goes to its place in the tree, in its own tiling and UB pad (the smaller levels follow, convert_textures). */
	if (levels > 1U) {
		tfu->output = pixels->view->address + tree->level[0].offset;
		tfu->output_bytes = tree->level[0].bytes;
		conversion.tiling = tree->level[0].tiling;
		conversion.padded_height = tree->level[0].padded_height;
	}
	error = bcm2711_native_conversion_words(&conversion, &tfu->output_format, &tfu->configuration);
	if (error != 0)
		return error;

	/* Succeeded: the pass runs this conversion before any draw samples the pixels. */
	return 0;
}

/*
 * Tells how many mip levels a texture is sampled with: the source's when the
 * texture formatting unit converts each of them (unit) from four-byte
 * texels and their tree can be laid out (tree), otherwise one.
 */
static uint32_t
mip_levels(
	const struct native_texture_source *source,
	uint32_t address,
	bool unit,
	struct bcm2711_native_miptree *tree)
{
	struct bcm2711_vulkan_image_level level;
	uint32_t index;
	int error;

	/* Level 0 alone without the unit, or for another texel. */
	if (source->levels <= 1U || !unit || source->texel_bytes != 4U || source->offset != 0)
		return 1;

	/* The tree the hardware reads (none on a unit with no layout here). */
	error = bcm2711_native_miptree_layout(source->width, source->height, source->levels, tree);
	if (error != 0)
		return 1;

	/* Every smaller level's raster is one the unit reads. */
	for (index = 1; index < source->levels; index++) {
		error = bcm2711_vulkan_image_level(source->image, index, &level);
		if (error != 0)
			return 1;
		if (((address + (uint32_t)level.offset) & 63U) != 0 || (level.pitch & 3U) != 0)
			return 1;
	}

	/* Succeeded: the tree's levels. */
	return source->levels;
}

/*
 * Describes the conversion of one smaller level (1 or more) of a converted
 * mip tree: the level's raster after level 0's in the image, into its place
 * and tiling in the tree, after the job of level 0 (entry->tfu).
 */
int
bcm2711_vulkan_native_mip_job(
	const struct bcm2711_vulkan_native_texture_copy *entry,
	uint32_t index,
	struct bcm2711_v3d_tfu_job *tfu)
{
	struct bcm2711_vulkan_image_level level;
	struct bcm2711_native_miptree tree;
	struct bcm2711_native_conversion conversion;
	uint64_t input_bytes;
	uint32_t texel_bytes;
	int error;

	/* Only a tree's smaller levels. */
	if (entry->levels <= 1U || index == 0 || index >= entry->levels)
		return EINVAL;
	error = bcm2711_native_miptree_layout(entry->image->width, entry->image->height, entry->levels, &tree);
	if (error != 0)
		return error;
	error = bcm2711_vulkan_image_level(entry->image, index, &level);
	if (error != 0)
		return error;

	/* The level's rows, from level 0's input moved by the level's place in the image. */
	input_bytes = (uint64_t)(level.height - 1U) * level.pitch + (uint64_t)level.width * 4U;
	if (input_bytes > 0xffffffffU)
		return EINVAL;
	kern_memset(tfu, 0, sizeof(*tfu));
	tfu->input = entry->tfu.input + (uint32_t)level.offset;
	tfu->input_bytes = (uint32_t)input_bytes;
	tfu->output = entry->pixels->view->address + tree.level[index].offset;
	tfu->output_bytes = tree.level[index].bytes;
	tfu->stride = level.pitch / 4U;
	tfu->size = (level.height << 16) | level.width;

	/* The level is RGBA8 (the only texel a tree has) in its own tiling and padded rows. */
	error = bcm2711_native_texel_layout(BCM2711_NATIVE_TEXEL_RGBA8, &texel_bytes, &conversion.type);
	if (error != 0)
		return error;
	conversion.texel_bytes = texel_bytes;
	conversion.tiling = tree.level[index].tiling;
	conversion.height = level.height;
	conversion.padded_height = tree.level[index].padded_height;
	error = bcm2711_native_conversion_words(&conversion, &tfu->output_format, &tfu->configuration);
	if (error != 0)
		return error;

	/* Succeeded: the pass runs it after level 0's. */
	return 0;
}

/* Stages one transfer source with exact nearest/linear clamp semantics and optional raw-channel copy interpretation. */
static int
prepare_meta_texture(
	struct bcm2711_vulkan_native_draw *draw,
	const struct bcm2711_vulkan_meta_draw *meta,
	uint64_t available)
{
	struct bcm2711_vulkan_sampler sampler;
	struct native_texture_source source;
	uint32_t swap;
	int error;

	/* Meta recording validation supplies an actual bound colour source; its application usage is transfer rather than sampled. */
	if (meta->image == NULL || meta->image->memory == NULL)
		return EINVAL;
	if (meta->filter != VK_FILTER_NEAREST && meta->filter != VK_FILTER_LINEAR)
		return ENOTSUP;

	/* Clamp is to the complete source image, as required for Vulkan blit; one mip has no independent mip filtering state. */
	kern_memset(&sampler, 0, sizeof(sampler));
	sampler.mag = meta->filter;
	sampler.min = meta->filter;
	sampler.u = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.v = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	swap = 0;
	if (!meta->raw && meta->image->format == VK_FORMAT_B8G8R8A8_UNORM)
		swap = 1;
	error = texture_source(meta->image, 0, swap, &source);
	if (error != 0)
		return error;
	error = prepare_texture_image(draw, &source, &sampler, available, &draw->bindings[0][0]);
	if (error != 0)
		return error;

	/* Succeeded: transfer shader binding zero owns all staged texels and numerical sampler/texture records. */
	return 0;
}

/* Finds a same-pass raster snapshot without transferring or duplicating its allocation ownership. */
static struct bcm2711_native_storage *
find_texture(
	const struct bcm2711_vulkan_native_draw *draw,
	const struct bcm2711_vulkan_resource *image,
	uint64_t offset,
	uint32_t levels)
{
	const struct bcm2711_vulkan_native_draw *owner;
	uint32_t index;

	/* Multiple descriptors in this draw may use the same raster with different sampler records. */
	for (index = 0; index < draw->texture_count; index++) {
		if (draw->textures[index].image == image && draw->textures[index].offset == offset && draw->textures[index].levels == levels)
			return draw->textures[index].pixels;
	}

	/* The pass retains every earlier owner until all referencing commands have stopped. */
	owner = draw->earlier;
	while (owner != NULL) {
		for (index = 0; index < owner->texture_count; index++) {
			if (owner->textures[index].image == image && owner->textures[index].offset == offset && owner->textures[index].levels == levels)
				return owner->textures[index].pixels;
		}

		/* Traverse only the caller's completed prefix, never an unrelated pass or frame. */
		owner = owner->next;
	}

	/* No retained snapshot exists in this pass; the caller must convert the current raster. */
	return NULL;
}

/* The area of a texture size kept as width << 16 | height. */
static uint64_t
texture_area(
	uint32_t size)
{
	/* Width and height are each sixteen bits of the packed size. */
	return (uint64_t)(size >> 16) * (size & 0xffffU);
}

/*
 * Frees a pass pool's draw chunks and releases its upload chunks.
 */
int
bcm2711_vulkan_native_draw_pool_release(
	struct bcm2711_vulkan_native_draw_pool *pool)
{
	struct bcm2711_vulkan_native_draw_chunk *chunk;
	struct bcm2711_vulkan_native_draw_chunk *next;
	struct bcm2711_vulkan_native_upload_chunk *upload;
	struct bcm2711_vulkan_native_upload_chunk *following;
	int first;
	int error;

	/* Every draw of every chunk has been released by the pass. */
	for (chunk = pool->chunks; chunk != NULL; chunk = next) {
		next = chunk->next;
		kern_free(chunk);
	}
	pool->chunks = NULL;

	/* The upload chunks retire after the pass's DMA; a failed translation teardown stays with the native space. */
	first = 0;
	for (upload = pool->uploads; upload != NULL; upload = following) {
		following = upload->next;
		error = bcm2711_native_storage_release(upload->storage->space, &upload->storage, true);
		if (first == 0 && error != 0)
			first = error;
		kern_free(upload);
	}
	pool->uploads = NULL;

	/* Reports the first failed upload release. */
	if (first != 0)
		return first;

	/* Succeeded: the pool owns nothing. */
	return 0;
}

/* Takes the next zeroed draw from a pass pool, adding a chunk when the newest is full. */
static struct bcm2711_vulkan_native_draw *
pool_take(
	struct bcm2711_vulkan_native_draw_pool *pool)
{
	struct bcm2711_vulkan_native_draw_chunk *chunk;

	/* A new chunk goes to the head of the pool; its draws are zeroed. */
	chunk = pool->chunks;
	if (chunk == NULL || chunk->used == BCM2711_VULKAN_DRAW_CHUNK) {
		chunk = kern_calloc(1, sizeof(*chunk));
		if (chunk == NULL)
			return NULL;
		chunk->next = pool->chunks;
		pool->chunks = chunk;
	}

	/* Reports the next unused draw of the newest chunk. */
	chunk->used++;
	return &chunk->draws[chunk->used - 1U];
}

/*
 * Carves one zeroed, aligned upload for a draw and reports its CPU and GPU addresses.
 *
 * A pass's draw takes it from the pool's newest upload chunk, adding a
 * chunk when it does not fit; the chunk is zeroed and cleaned once when it
 * is made, and the caller cleans the bytes it writes.  An independent draw,
 * or an upload larger than NATIVE_UPLOAD_LARGEST, gets storage of its own.
 */
static int
upload_take(
	struct bcm2711_vulkan_native_draw *draw,
	uint64_t bytes,
	uint64_t available,
	uint8_t **cpu,
	uint32_t *gpu)
{
	struct bcm2711_vulkan_native_upload_chunk *chunk;
	struct bcm2711_native_storage *storage;
	uint32_t offset;
	int error;

	/* An independent draw or a large upload keeps the per-allocation path. */
	if (draw->pool == NULL || bytes == 0 || bytes > NATIVE_UPLOAD_LARGEST) {
		error = allocate_storage(draw, bytes, available, NULL, BCM2711_NATIVE_STORAGE_ZEROED, &storage);
		if (error != 0)
			return error;
		*cpu = storage->view->buffer->address;
		*gpu = storage->view->address;
		return 0;
	}

	/* The newest chunk serves the upload when the aligned bytes still fit in it. */
	chunk = draw->pool->uploads;
	offset = 0;
	if (chunk != NULL)
		offset = (chunk->used + NATIVE_UPLOAD_ALIGNMENT - 1U) & ~(NATIVE_UPLOAD_ALIGNMENT - 1U);
	if (chunk == NULL || offset + bytes > NATIVE_UPLOAD_CHUNK_BYTES) {
		/* A new chunk counts against the job's padded staging budget like any upload. */
		if (draw->bytes > available || NATIVE_UPLOAD_CHUNK_BYTES > available - draw->bytes)
			return ENOMEM;
		chunk = kern_calloc(1, sizeof(*chunk));
		if (chunk == NULL)
			return ENOMEM;
		/* Each carved upload writes all of its bytes the device reads; a reused chunk is not zeroed. */
		error = bcm2711_native_storage_create_as(draw->space, NATIVE_UPLOAD_CHUNK_BYTES, BCM2711_NATIVE_STORAGE_WRITTEN, &chunk->storage);
		if (error != 0) {
			kern_free(chunk);
			return error;
		}

		/* The chunk is written back once (a new one zeroed, a reused one as the cache kept it clean), so no dirty line can later land over device-read bytes. */
		error = bcm2711_native_storage_clean(chunk->storage);
		if (error != 0) {
			(void)bcm2711_native_storage_release(draw->space, &chunk->storage, true);
			kern_free(chunk);
			return error;
		}
		chunk->next = draw->pool->uploads;
		draw->pool->uploads = chunk;
		draw->bytes += NATIVE_UPLOAD_CHUNK_BYTES;
		offset = 0;
	}

	/* Succeeded: the caller writes the carved bytes and cleans them before any pointer reaches the device. */
	chunk->used = offset + (uint32_t)bytes;
	*cpu = (uint8_t *)chunk->storage->view->buffer->address + offset;
	*gpu = chunk->storage->view->address + offset;
	return 0;
}
