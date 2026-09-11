/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

struct color_count {
	uint32_t color;
	unsigned long count;
	unsigned long minimum;
	const char *name;
};

static int
next_number(FILE *file, unsigned long *value)
{
	int character;
	unsigned long result = 0;
	int have_digit = 0;

	for (;;) {
		character = fgetc(file);
		if (character == '#') {
			do {
				character = fgetc(file);
			} while (character != '\n' && character != EOF);
			continue;
		}
		if (character == EOF)
			return 0;
		if (!isspace((unsigned char)character))
			break;
	}
	while (isdigit((unsigned char)character)) {
		have_digit = 1;
		if (result > 100000000UL)
			return 0;
		result = result * 10UL + (unsigned long)(character - '0');
		character = fgetc(file);
	}
	if (!have_digit || character == EOF ||
	    !isspace((unsigned char)character))
		return 0;
	*value = result;
	return 1;
}

int
main(int argc, char **argv)
{
	struct color_count colors[] = {
		{0x102030U, 0, 1000, "background"},
		{0x406080U, 0, 100, "pattern"},
		{0xffffffU, 0, 10, "line/glyph"},
		{0xff0000U, 0, 1, "BMP red"},
		{0x00ff00U, 0, 1, "BMP green"},
		{0x0000ffU, 0, 1, "BMP blue"},
		{0xffff00U, 0, 1, "BMP yellow"},
	};
	FILE *file;
	unsigned long width, height, maximum, pixel_count, index;
	int failed = 0;

	if (argc != 2 || (file = fopen(argv[1], "rb")) == NULL) {
		fprintf(stderr, "usage: %s SCREENSHOT.ppm\n", argv[0]);
		return 2;
	}
	if (fgetc(file) != 'P' || fgetc(file) != '6' ||
	    !next_number(file, &width) || !next_number(file, &height) ||
	    !next_number(file, &maximum) || width == 0 || height == 0 ||
	    maximum != 255 || width > 16384 || height > 16384) {
		fprintf(stderr, "invalid P6 screenshot\n");
		fclose(file);
		return 1;
	}
	pixel_count = width * height;
	for (index = 0; index < pixel_count; index++) {
		int red = fgetc(file), green = fgetc(file), blue = fgetc(file);
		uint32_t color;
		size_t color_index;

		if (red == EOF || green == EOF || blue == EOF) {
			fprintf(stderr, "truncated P6 screenshot\n");
			fclose(file);
			return 1;
		}
		color = ((uint32_t)red << 16) | ((uint32_t)green << 8) |
			(uint32_t)blue;
		for (color_index = 0;
		     color_index < sizeof(colors) / sizeof(colors[0]);
		     color_index++)
			if (colors[color_index].color == color)
				colors[color_index].count++;
	}
	fclose(file);
	for (index = 0; index < sizeof(colors) / sizeof(colors[0]); index++) {
		printf("%s %#08x %lu\n", colors[index].name,
		       colors[index].color, colors[index].count);
		if (colors[index].count < colors[index].minimum)
			failed = 1;
	}
	if (failed) {
		fprintf(stderr, "required BeUI drawing colors were not observed\n");
		return 1;
	}
	printf("NOCT-T011-PIXELS-OK %lux%lu\n", width, height);
	return 0;
}
