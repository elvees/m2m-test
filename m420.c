/*
 * Copyright 2015, Anton Leontiev <aleontiev@elvees.com>
 *
 * SPDX-License-Identifier:	GPL-2.0
 */

#include <stdint.h>
#include <error.h>

#include <libavformat/avformat.h>

void yuv420_to_m420(AVFrame *frame) {
	unsigned const width = frame->width, height = frame->height;
	uint8_t *temp = malloc(width * height * 3 / 2);
	if (!temp) error(EXIT_FAILURE, 0, "Can not allocate memory for convertion buffer");

	// Luma
	for (size_t i = 0, j = 0; i < height; i += 2, j += 3) {
		memcpy(temp + j * width, &frame->data[0][i * width], 2 * width);
	}

	// Chroma
	for (size_t i = 0, j = 2; i < height / 2; i++, j += 3) {
		uint8_t *const out = &temp[j * width];
		uint8_t *const incb = &frame->data[1][i * width / 2];
		uint8_t *const incr = &frame->data[2][i * width / 2];

		for (size_t k = 0; k < width / 2; k++) {
			out[2 * k]     = incb[k];
			out[2 * k + 1] = incr[k];
		}
	}

	memcpy(frame->data[0], temp, width * height);
	memcpy(frame->data[1], temp + width * height, width * height / 4);
	memcpy(frame->data[2], temp + width * height + width * height / 4, width * height / 4);

	free(temp);
}
