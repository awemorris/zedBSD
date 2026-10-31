/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The size of a replaced box (CSS 2 §10.3.2 and §10.6.2, for <img>): the
 * width and height the style gives, or else the element's width and
 * height attributes (their presentational hints), and for what neither
 * gives the image's own size, or the other side through the image's ratio.
 * max-width and max-height shrink it keeping the ratio.  An element with
 * no image and no size is empty.
 */

#include "layout/layout.h"

/* The size of a <video> without a picture (HTML's default object size). */
#define REPLACED_VIDEO_WIDTH	300
#define REPLACED_VIDEO_HEIGHT	150

static int replaced_attribute(const struct layout_box *box, const char *name, layout_unit *value);
static int replaced_length(const struct css_length *length, layout_unit containing, layout_unit *value);
static layout_unit replaced_content_size(const struct layout_box *box, layout_unit size, int first, int second);
static int replaced_is_video(const struct layout_box *box);
static int replaced_is_bare_svg(const struct layout_box *box);
static int replaced_flex_main(const struct layout_box *box);
static void replaced_limit_ratio(layout_unit *width, layout_unit *height, layout_unit min_width, layout_unit max_width, layout_unit min_height, layout_unit max_height);

/*
 * Sets a replaced box's content width and height within its containing block.
 *
 * The box model is resolved already; height_definite distinguishes zero from auto.
 */
void
layout_replaced_size(
	struct layout_box *box,
	layout_unit containing_width,
	layout_unit containing_height,
	int height_definite)
{
	layout_unit natural_width;
	layout_unit natural_height;
	layout_unit width;
	layout_unit height;
	layout_unit min_width;
	layout_unit max_width;
	layout_unit min_height;
	layout_unit max_height;
	layout_unit frame_x;
	layout_unit frame_y;
	layout_unit limit_frame;
	int ratio_override;
	int has_ratio;
	int width_given;
	int height_given;
	int video;
	int flex;
	int sideways;
	layout_unit limit;

	/* The image's own size, which gives the ratio too. */
	natural_width = 0;
	natural_height = 0;
	has_ratio = 0;
	if (box->image != NULL) {
		natural_width = layout_from_px((float)box->image->width * box->style.zoom_eff);
		natural_height = layout_from_px((float)box->image->height * box->style.zoom_eff);
		has_ratio = 1;
	}

	/* A <video> without a picture yet has HTML's default object size (ws121-p004). */
	video = replaced_is_video(box);
	if (box->image == NULL && video) {
		natural_width = (layout_unit)REPLACED_VIDEO_WIDTH * LAYOUT_UNIT;
		natural_height = (layout_unit)REPLACED_VIDEO_HEIGHT * LAYOUT_UNIT;
	}

	/* A <canvas>'s natural size is its width and height attributes' (300 by 150 when absent), and gives the ratio. */
	if (box->image == NULL && layout_replaced_is_canvas(box)) {
		natural_width = (layout_unit)REPLACED_VIDEO_WIDTH * LAYOUT_UNIT;
		natural_height = (layout_unit)REPLACED_VIDEO_HEIGHT * LAYOUT_UNIT;
		replaced_attribute(box, "width", &natural_width);
		replaced_attribute(box, "height", &natural_height);
		natural_width = (layout_unit)((float)natural_width * box->style.zoom_eff);
		natural_height = (layout_unit)((float)natural_height * box->style.zoom_eff);
		box->natural_width = natural_width;
		box->natural_height = natural_height;
		has_ratio = 1;
	}

	/* A form control's natural size is its text's, and has no ratio. */
	if (box->control != DOM_CONTROL_NONE) {
		natural_width = box->natural_width;
		natural_height = box->natural_height;
	}

	/* In a vertical domain's logical coordinates the picture's width is the block size. */
	sideways = box->physical_style != NULL && box->physical_style->writing_mode != CSS_WRITING_HORIZONTAL_TB;
	if (sideways) {
		limit = natural_width;
		natural_width = natural_height;
		natural_height = limit;
	}

	/* The width: the style's (less the frame when it sizes the border box), or the attribute's. */
	width = 0;
	width_given = replaced_length(&box->style.width, containing_width, &width);
	if (width_given)
		width = replaced_content_size(box, width, CSS_LEFT, CSS_RIGHT);

	/* Only an unresolved non-control width falls back to the real content attribute. */
	if (!width_given && box->control == DOM_CONTROL_NONE && !layout_replaced_is_canvas(box))
		width_given = replaced_attribute(box, sideways ? "height" : "width", &width);

	/* The height: the style's (a percentage only with a definite containing height), or the attribute's. */
	height = 0;
	height_given = 0;
	if (box->style.height.unit == CSS_UNIT_PX ||
	    (box->style.height.unit == CSS_UNIT_PERCENT && height_definite))
		height_given = replaced_length(&box->style.height, containing_height, &height);
	if (height_given)
		height = replaced_content_size(box, height, CSS_TOP, CSS_BOTTOM);

	/* Only an unresolved non-control height falls back to the real content attribute. */
	if (!height_given && box->control == DOM_CONTROL_NONE && !layout_replaced_is_canvas(box))
		height_given = replaced_attribute(box, sideways ? "width" : "height", &height);

	/*
	 * An aspect-ratio replaces the image's own ratio (auto keeps the image's
	 * when it has one).  It relates the box-sizing boxes, so under border-box
	 * the frame is part of both sides.
	 */
	ratio_override = 0;
	frame_x = 0;
	frame_y = 0;
	if (box->style.aspect_ratio > 0.0f && (!box->style.aspect_auto || !has_ratio || natural_width <= 0 || natural_height <= 0)) {
		ratio_override = 1;
		if (box->style.box_sizing == CSS_BOX_SIZING_BORDER) {
			frame_x = box->border[CSS_LEFT] + box->padding[CSS_LEFT] + box->padding[CSS_RIGHT] + box->border[CSS_RIGHT];
			frame_y = box->border[CSS_TOP] + box->padding[CSS_TOP] + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];
		}
	}
	if (!ratio_override && (natural_width <= 0 || natural_height <= 0))
		has_ratio = 0;

	/* What is not given comes from the other side through the ratio, or from the image. */
	if (width_given && !height_given) {
		height = natural_height;
		if (ratio_override) {
			height = (layout_unit)((float)(width + frame_x) / box->style.aspect_ratio) - frame_y;
		} else if (has_ratio) {
			height = (layout_unit)((int64_t)width * natural_height / natural_width);
		}
	} else if (height_given && !width_given) {
		width = natural_width;
		if (ratio_override) {
			width = (layout_unit)((float)(height + frame_y) * box->style.aspect_ratio) - frame_x;
		} else if (has_ratio) {
			width = (layout_unit)((int64_t)height * natural_width / natural_height);
		}
	} else if (!width_given && !height_given) {
		width = natural_width;
		height = natural_height;
		if (ratio_override) {
			/* The natural height (or the default one) stays and the width follows it; an explicit ratio with a bare image keeps the image's width instead. */
			if (natural_height <= 0)
				height = 150 * LAYOUT_UNIT;
			width = (layout_unit)((float)(height + frame_y) * box->style.aspect_ratio) - frame_x;
			if (box->image != NULL && !box->style.aspect_auto && frame_x == 0 && frame_y == 0) {
				width = natural_width;
				height = (layout_unit)((float)width / box->style.aspect_ratio);
			}
		}

		/* An inline <svg> with a viewBox but no width or height has a ratio only: it fills its containing block's width (ws074-p228). */
		if (replaced_is_bare_svg(box) && containing_width > 0 && natural_width > 0) {
			width = containing_width - box->margin[CSS_LEFT] - box->margin[CSS_RIGHT];
			if (width < 0)
				width = 0;
			height = (layout_unit)((int64_t)width * natural_height / natural_width);
		}
	}

	/* The limits (a percentage height limit only with a definite containing height). */
	min_width = 0;
	max_width = -1;
	min_height = 0;
	max_height = -1;
	if (!replaced_length(&box->style.min_width, containing_width, &min_width))
		min_width = 0;
	if (!replaced_length(&box->style.max_width, containing_width, &max_width))
		max_width = -1;

	/* A percentage width limit has no basis while the content is measured in a very wide room: it is no limit. */
	if (containing_width >= ((layout_unit)1 << 24)) {
		if (box->style.min_width.unit == CSS_UNIT_PERCENT)
			min_width = 0;
		if (box->style.max_width.unit == CSS_UNIT_PERCENT)
			max_width = -1;
	}
	if (box->style.min_height.unit == CSS_UNIT_PX ||
	    (box->style.min_height.unit == CSS_UNIT_PERCENT && height_definite)) {
		if (!replaced_length(&box->style.min_height, containing_height, &min_height))
			min_height = 0;
	}
	if (box->style.max_height.unit == CSS_UNIT_PX ||
	    (box->style.max_height.unit == CSS_UNIT_PERCENT && height_definite)) {
		if (!replaced_length(&box->style.max_height, containing_height, &max_height))
			max_height = -1;
	}

	/* A border-box height limit includes the frame that the content height does not. */
	if (box->style.box_sizing == CSS_BOX_SIZING_BORDER) {
		limit_frame = box->border[CSS_TOP] + box->padding[CSS_TOP] + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];
		if (min_height > 0) {
			min_height -= limit_frame;
			if (min_height < 0)
				min_height = 0;
		}
		if (max_height >= 0) {
			max_height -= limit_frame;
			if (max_height < 0)
				max_height = 0;
		}
	}
	if (box->style.box_sizing == CSS_BOX_SIZING_BORDER) {
		limit_frame = box->border[CSS_LEFT] + box->padding[CSS_LEFT] + box->padding[CSS_RIGHT] + box->border[CSS_RIGHT];
		if (min_width > 0) {
			min_width -= limit_frame;
			if (min_width < 0)
				min_width = 0;
		}
		if (max_width >= 0) {
			max_width -= limit_frame;
			if (max_width < 0)
				max_width = 0;
		}
	}
	if (max_width >= 0 && max_width < min_width)
		max_width = min_width;
	if (max_height >= 0 && max_height < min_height)
		max_height = min_height;

	/*
	 * Both sides auto with a ratio: the limits keep the ratio where they can
	 * (a flex item's limits along the main axis come later, from the flex
	 * layout, and do not size it here).
	 */
	if (!width_given && !height_given && (has_ratio || ratio_override) && width > 0 && height > 0) {
		flex = replaced_flex_main(box);
		if (flex == 1) {
			min_width = 0;
			max_width = -1;
		} else if (flex == 2) {
			min_height = 0;
			max_height = -1;
		}
		replaced_limit_ratio(&width, &height, min_width, max_width, min_height, max_height);
	} else {
		/* max-width shrinks a wider box, and its height with it when that was not given. */
		if (max_width >= 0 && width > max_width) {
			if (!height_given && width > 0)
				height = (layout_unit)((int64_t)height * max_width / width);
			width = max_width;
		}

		/* max-height shrinks a taller box, and its width with it when that was not given. */
		if (max_height >= 0 && height > max_height) {
			if (!width_given && height > 0)
				width = (layout_unit)((int64_t)width * max_height / height);
			height = max_height;
		}

		/* The minimums hold each side, and carry the side that was not given along through the ratio. */
		if (width < min_width) {
			if (!height_given && width > 0 && has_ratio && !ratio_override)
				height = (layout_unit)((int64_t)height * min_width / width);
			width = min_width;
		}
		if (height < min_height) {
			if (!width_given && height > 0 && has_ratio && !ratio_override)
				width = (layout_unit)((int64_t)width * min_height / height);
			height = min_height;
		}
	}

	/* No side is negative. */
	if (width < 0)
		width = 0;

	/* The final used height cannot be negative either. */
	if (height < 0)
		height = 0;

	/* Publishes both final used content dimensions after all limits were resolved. */
	box->width = width;
	box->height = height;

	/* Succeeded: the existing box now holds its resolved content dimensions. */
	return;
}

/* Reads leading attribute digits as pixels, reporting absence when no initial digit exists. */
static int
replaced_attribute(
	const struct layout_box *box,
	const char *name,
	layout_unit *value)
{
	const struct dom_element *element;
	const struct dom_attribute *attribute;
	const struct vm_string *text;
	uint16_t unit;
	size_t index;
	int64_t number;
	int same;

	/* The element's attribute of that name. */
	if (box->node == NULL || box->node->type != DOM_ELEMENT)
		return 0;

	/* Searches only actual no-namespace attributes on the native element. */
	element = (const struct dom_element *)box->node;
	attribute = NULL;
	for (index = 0; index < element->attribute_count; index++) {
		same = vm_string_equal_ascii(element->attributes[index].name, name);
		if (same && element->attributes[index].ns == DOM_NS_NONE) {
			attribute = &element->attributes[index];
			break;
		}
	}

	/* No attribute gives no size. */
	if (attribute == NULL)
		return 0;

	/* Spaces before the number are skipped. */
	text = attribute->value;
	index = 0;
	unit = 0;
	while (index < text->length) {
		unit = vm_string_at(text, index);
		if (unit != 0x20U &&
		    unit != 0x09U &&
		    unit != 0x0aU)
			break;
		index++;
	}

	/* A value that starts with no digit gives no size. */
	if (index >= text->length ||
	    unit < '0' ||
	    unit > '9')
		return 0;

	/* The digits, up to a size no page needs. */
	number = 0;
	while (index < text->length) {
		unit = vm_string_at(text, index);
		if (unit < '0' || unit > '9')
			break;

		/* Accumulates digits only while the existing bounded pixel cap still permits growth. */
		if (number < 1000000)
			number = number * 10 + (unit - '0');
		index++;
	}

	/* Succeeded: the attribute's pixels. */
	*value = (layout_unit)(number * LAYOUT_UNIT);
	return 1;
}

/* Resolves pixels or a percentage with a known containing size, leaving other units unresolved. */
static int
replaced_length(
	const struct css_length *length,
	layout_unit containing,
	layout_unit *value)
{
	/* Pixels. */
	if (length->unit == CSS_UNIT_PX) {
		*value = layout_from_px(length->value);
		return 1;
	}

	/* A definite containing size can be exactly zero; height definiteness is checked by the caller. */
	if (length->unit == CSS_UNIT_PERCENT && containing >= 0) {
		*value = (layout_unit)((float)containing * length->value / 100.0f) + layout_from_px(length->offset);
		return 1;
	}

	/* Succeeded: auto, none or an unresolved containing size supplies no explicit length. */
	return 0;
}

/* Removes both sides' borders and paddings from a border-box size without making it negative. */
static layout_unit
replaced_content_size(
	const struct layout_box *box,
	layout_unit size,
	int first,
	int second)
{
	layout_unit frame;

	/* A content-box size is the content's already. */
	if (box->style.box_sizing != CSS_BOX_SIZING_BORDER)
		return size;

	/* The frame of the two sides comes off. */
	frame = box->border[first] + box->padding[first] + box->padding[second] + box->border[second];
	size -= frame;
	if (size < 0)
		size = 0;

	/* Succeeded: reports the nonnegative used content dimension. */
	return size;
}

/* Tells whether a box is an HTML <video>'s. */
static int
replaced_is_video(
	const struct layout_box *box)
{
	const struct dom_element *element;

	/* An element's box. */
	if (box->node == NULL || box->node->type != DOM_ELEMENT)
		return 0;
	element = (const struct dom_element *)box->node;
	return element->ns == DOM_NS_HTML && element->tag == DOM_TAG_VIDEO;
}

/* Finds the width over height of a replaced box's picture or canvas (the ratio its height gives its width by). */
int
layout_replaced_ratio(
	const struct layout_box *box,
	float *ratio)
{
	layout_unit width;
	layout_unit height;

	/* A picture's own size (a width attribute sizes it by itself). */
	if (box->image != NULL && box->image->width > 0 && box->image->height > 0) {
		width = 0;
		if (replaced_attribute(box, "width", &width))
			return 0;
		*ratio = (float)box->image->width / (float)box->image->height;
		return 1;
	}

	/* A canvas's attributes (300 by 150 when absent). */
	if (box->image == NULL && layout_replaced_is_canvas(box)) {
		width = (layout_unit)REPLACED_VIDEO_WIDTH * LAYOUT_UNIT;
		height = (layout_unit)REPLACED_VIDEO_HEIGHT * LAYOUT_UNIT;
		replaced_attribute(box, "width", &width);
		replaced_attribute(box, "height", &height);
		if (width <= 0 || height <= 0)
			return 0;
		*ratio = (float)width / (float)height;
		return 1;
	}
	return 0;
}

/* Tells whether a box is an HTML <canvas>'s. */
int
layout_replaced_is_canvas(
	const struct layout_box *box)
{
	const struct dom_element *element;

	/* An element's box. */
	if (box->node == NULL || box->node->type != DOM_ELEMENT)
		return 0;
	element = (const struct dom_element *)box->node;
	return element->ns == DOM_NS_HTML && element->tag == DOM_TAG_CANVAS;
}

/* Tells whether a box is an inline <svg>'s with no width and no height attribute (only its viewBox's ratio). */
static int
replaced_is_bare_svg(
	const struct layout_box *box)
{
	const struct dom_element *element;
	layout_unit ignored;

	/* An svg element's box. */
	if (box->node == NULL || box->node->type != DOM_ELEMENT)
		return 0;
	element = (const struct dom_element *)box->node;
	if (element->ns != DOM_NS_SVG || element->tag != DOM_TAG_SVG)
		return 0;

	/* Neither attribute gives a size. */
	if (replaced_attribute(box, "width", &ignored))
		return 0;
	if (replaced_attribute(box, "height", &ignored))
		return 0;
	return 1;
}

/*
 * Applies the minimum and maximum widths and heights (a maximum below 0 is
 * none) to a replaced box with a ratio whose width and height are both
 * auto, keeping the ratio unless two limits disagree.
 * https://www.w3.org/TR/CSS2/visudet.html#min-max-widths
 */
static void
replaced_limit_ratio(
	layout_unit *width,
	layout_unit *height,
	layout_unit min_width,
	layout_unit max_width,
	layout_unit min_height,
	layout_unit max_height)
{
	int64_t w;
	int64_t h;
	int64_t used_w;
	int64_t used_h;
	int wide;
	int tall;
	int narrow;
	int short_;

	/* Where the size breaks the limits. */
	w = *width;
	h = *height;
	wide = max_width >= 0 && w > max_width;
	narrow = w < min_width;
	tall = max_height >= 0 && h > max_height;
	short_ = h < min_height;
	used_w = w;
	used_h = h;

	/* The cases of the table. */
	if (wide && tall) {
		if ((int64_t)max_width * h <= (int64_t)max_height * w) {
			used_w = max_width;
			used_h = max_width * h / w;
			if (used_h < min_height)
				used_h = min_height;
		} else {
			used_h = max_height;
			used_w = max_height * w / h;
			if (used_w < min_width)
				used_w = min_width;
		}
	} else if (narrow && short_) {
		if ((int64_t)min_width * h <= (int64_t)min_height * w) {
			used_h = min_height;
			used_w = min_height * w / h;
			if (max_width >= 0 && used_w > max_width)
				used_w = max_width;
		} else {
			used_w = min_width;
			used_h = min_width * h / w;
			if (max_height >= 0 && used_h > max_height)
				used_h = max_height;
		}
	} else if (narrow && tall) {
		used_w = min_width;
		used_h = max_height;
	} else if (wide && short_) {
		used_w = max_width;
		used_h = min_height;
	} else if (wide) {
		used_w = max_width;
		used_h = max_width * h / w;
		if (used_h < min_height)
			used_h = min_height;
	} else if (narrow) {
		used_w = min_width;
		used_h = min_width * h / w;
		if (max_height >= 0 && used_h > max_height)
			used_h = max_height;
	} else if (tall) {
		used_h = max_height;
		used_w = max_height * w / h;
		if (used_w < min_width)
			used_w = min_width;
	} else if (short_) {
		used_h = min_height;
		used_w = min_height * w / h;
		if (max_width >= 0 && used_w > max_width)
			used_w = max_width;
	}

	/* The used size. */
	*width = (layout_unit)used_w;
	*height = (layout_unit)used_h;
}

/* The main axis of a flex item: 1 for a row (its width), 2 for a column (its height), 0 for another box. */
static int
replaced_flex_main(
	const struct layout_box *box)
{
	const struct layout_box *parent;
	int column;

	/* A child of a flex container. */
	parent = box->parent;
	if (parent == NULL || box->out_of_flow ||
	    (parent->style.display != CSS_DISPLAY_FLEX && parent->style.display != CSS_DISPLAY_INLINE_FLEX))
		return 0;

	/* Its direction against the writing mode. */
	column = parent->style.flex_direction == CSS_FLEX_COLUMN || parent->style.flex_direction == CSS_FLEX_COLUMN_REVERSE;
	if (parent->style.writing_mode != CSS_WRITING_HORIZONTAL_TB)
		column = !column;
	return column ? 2 : 1;
}
