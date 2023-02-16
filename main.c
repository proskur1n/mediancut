/*
 * Copyright (c) 2023 Andrey Proskurin (proskur1n)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>
#include <getopt.h>
#include <errno.h>
#include <stdarg.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_FAILURE_USERMSG
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#pragma GCC diagnostic pop

#define MAX_PALETTE 128

char const *argv0 = "mediancut";

/// Prints a formatted error message to the stderr and aborts the program.
void fatal(char const *format, ...)
{
	va_list va;
	va_start(va, format);
	fprintf(stderr, "%s: ", argv0);
	vfprintf(stderr, format, va);
	fputc('\n', stderr);
	va_end(va);
	exit(EXIT_FAILURE);
}

struct color {
	unsigned char rgba[4];
};

struct node {
	union {
		// Internal nodes of the binary tree.
		struct split {
			struct node *left; // Contains values less-or-equals than the 'threshold'.
			struct node *right; // Contains values larger than the 'threshold'.
			unsigned char threshold;
			unsigned char chan;
		} split;

		// Leaf nodes of the binary tree.
		struct bucket {
			struct color *data;
			size_t data_count;
			struct color avg_color;
			unsigned char range; // Range of the longest dimension (range_chan)
			unsigned char range_chan; // 0: red, 1: green, 2: blue
		} bucket;
	};
	bool leaf;
};

unsigned int compare_chan;

/// Compares two colors based on the color channel specified in the 'compare_chan' global variable.
int compare_color(void const *a, void const *b)
{
	int arg1 = ((struct color *) a)->rgba[compare_chan];
	int arg2 = ((struct color *) b)->rgba[compare_chan];
	return arg1 - arg2;
}

/// Initializes a new leaf node with a bucket. This procedure does not initialize the average color
/// 'avg_color' inside the new bucket.
/// @param rgb Pointer to the RGB data.
/// @param count Array length in 'rgb'.
struct node make_bucket(struct color *rgb, size_t count)
{
	if (count < 2) {
		return (struct node) {.bucket = {.data=rgb, .data_count=count}, .leaf = true};
	}

	unsigned char max_range = 0;
	unsigned char max_range_chan = 0;

	for (int chan = 0; chan < 3; ++chan) {
		unsigned char min = rgb[0].rgba[chan];
		unsigned char max = min;
		for (size_t i = 1; i < count; ++i) {
			unsigned char v = rgb[i].rgba[chan];
			if (v < min) {
				min = v;
			} else if (v > max) {
				max = v;
			}
		}

		if (max - min > max_range) {
			max_range = max - min;
			max_range_chan = chan;
		}
	}

	struct bucket bucket = {
			.data = rgb,
			.data_count = count,
			.range = max_range,
			.range_chan = max_range_chan
	};
	return (struct node) {.bucket = bucket, .leaf = true};
}

/// Returns the average of the 'count' elements inside 'pixels'. This procedure always returns 255
/// for alpha.
struct color compute_average_color(struct color *pixels, size_t count)
{
	struct color result = {{0, 0, 0, 255}};

	for (int c = 0; c < 3; ++c) {
		// This algorithm computes the mean of numbers without overflowing.
		// It is taken from this Quora post: https://qr.ae/pG68Dk

		unsigned char x = 0;
		size_t y = 0;
		for (size_t i = 0; i < count; ++i) {
			x += pixels[i].rgba[c] / count;
			unsigned char b = pixels[i].rgba[c] % count;
			if (y >= count - b) {
				++x;
				y -= count - b;
			} else {
				y += b;
			}
		}
		// Average is exactly x + y / N
		// 0 <= y < N
		result.rgba[c] = x;
	}

	return result;
}

/// Turns the given leaf node into an internal node with two buckets as children. This procedure may
/// change the order of elements inside node->bucket.data to find its median. 'node' must have at
/// least one element in it.
void cut_bucket(struct node *out_left, struct node *out_right, struct node *node)
{
	assert(node->leaf);
	assert(node->bucket.data_count > 0);
	struct bucket *bucket = &node->bucket;

	compare_chan = bucket->range_chan;
	qsort(bucket->data, bucket->data_count, sizeof(struct color), compare_color);

	struct split split = {
			.left = out_left,
			.right = out_right,
			.threshold = bucket->data[bucket->data_count / 2].rgba[bucket->range_chan],
			.chan = bucket->range_chan
	};
	size_t cut = 0;
	while (cut < bucket->data_count && bucket->data[cut].rgba[split.chan] <= split.threshold) {
		++cut;
	}
	// Note that this is a slightly modified version of the median cut algorithm, as it does not
	// divide exactly at the median (bucket->data_count / 2), but at the first value that is
	// greater than the median (threshold).

	*out_left = make_bucket(bucket->data, cut);
	*out_right = make_bucket(bucket->data + cut, bucket->data_count - cut);
	*node = (struct node) {.split = split, .leaf = false};
}

/// Computes the quantized color using the provided palette specified by its root node. You must
/// call set_average_color on every bucket inside the binary tree before calling this function.
struct color lookup_color_from_palette(struct node const *root, struct color color)
{
	while (true) {
		if (root->leaf) {
			return root->bucket.avg_color;
		}
		if (color.rgba[root->split.chan] <= root->split.threshold) {
			root = root->split.left;
		} else {
			root = root->split.right;
		}
	}
}

/// Performs the median cut color quantization algorithm in-place on the given image pixels.
/// @param palette_count Number of distinct colors in the output image. Must be <= MAX_PALETTE.
/// @param image_data    Image pixels
/// @param w Width of the image.
/// @param h Height of the image.
void median_cut(int palette_count, struct color *image_data, int w, int h)
{
	struct color *temp = malloc(w * h * sizeof(struct color));
	if (temp == NULL) {
		fatal("no memory");
	}
	memcpy(temp, image_data, w * h * sizeof(struct color));

	struct node nodes[MAX_PALETTE * 2 - 1];
	int nodes_count = 0;
	nodes[nodes_count++] = make_bucket(temp, w * h);

	for (int p = 1; p < palette_count; ++p) {
		// Find the bucket with the largest range.
		struct node *largest = NULL;
		unsigned char max_range = 0;
		for (int i = 0; i < nodes_count; ++i) {
			if (nodes[i].leaf && nodes[i].bucket.range >= max_range) {
				max_range = nodes[i].bucket.range;
				largest = &nodes[i];
			}
		}
		if (max_range == 0) {
			// There are no more buckets that can be divided.
			break;
		}

		// Cut the bucket with the largest range into two buckets.
		cut_bucket(&nodes[nodes_count], &nodes[nodes_count + 1], largest);
		nodes_count += 2;
	}

	for (int i = 0; i < nodes_count; ++i) {
		if (nodes[i].leaf) {
			nodes[i].bucket.avg_color = compute_average_color(nodes[i].bucket.data, nodes[i].bucket.data_count);
		}
	}
	for (size_t i = 0; i < (size_t) w * h; ++i) {
		image_data[i] = lookup_color_from_palette(&nodes[0], image_data[i]);
	}
	free(temp);
}

/// Parses an unsigned integer inside str and returns 0 on failure.
int parse_uint(char const *str)
{
	errno = 0;
	char *end = NULL;
	long n = strtol(str, &end, 10);
	if (str[0] == 0 || *end != 0 || errno != 0 || n < 0 || n >= INT_MAX) {
		return 0;
	}
	return (int) n;
}

/// Prints usage information to the provided stream and exits the program.
void usage(FILE *stream)
{
	fprintf(stream, "Usage: %s [-p N] INPUT OUTPUT\n\n", argv0);
	fputs("Performs color quantization on the given image using a slightly modified\n", stream);
	fputs("version of the median cut algorithm.\n\n", stream);
	fprintf(stream, "  -p N    Number of colors in the output image (default 4)\n");
	exit(stream == stderr ? EXIT_FAILURE : EXIT_SUCCESS);
}

int main(int argc, char *argv[])
{
	if (argc > 0) {
		argv0 = argv[0];
	}
	int palette_count = 4;
	char const *input = NULL;
	char const *output = NULL;

	struct option long_options[] = {
			{"help", no_argument, NULL, 'h'},
			{0},
	};
	int opt;
	while ((opt = getopt_long(argc, argv, "hp:", long_options, NULL)) != -1) {
		switch (opt) {
		case 'p':
			if ((palette_count = parse_uint(optarg)) < 1) {
				usage(stderr);
			}
			break;
		case 'h':
			usage(stdout);
			break;
		default:
			usage(stderr);
		}
	}

	if (optind + 2 != argc) {
		usage(stderr);
	}
	input = argv[optind];
	output = argv[optind + 1];

	int w = 0, h = 0;
	struct color *data = (struct color *) stbi_load(input, &w, &h, NULL, sizeof(struct color));
	if (data == NULL) {
		fatal("cannot parse image '%s': %s", input, stbi_failure_reason());
	}

	median_cut(palette_count, data, w, h);

	if (stbi_write_png(output, w, h, sizeof(struct color), data, 0) == 0) {
		fatal("cannot write image '%s'", output);
	}
	stbi_image_free(data);

	return EXIT_SUCCESS;
}
