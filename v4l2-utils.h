/*
 * V4L2 utility functions definition
 *
 * Copyright (C) 2016 ELVEES NeoTek JSC
 * Author: Anton Leontiev <aleontiev@elvees.com>
 *
 * SPDX-License-Identifier:	GPL-2.0
 */

#ifndef V4L2_H
#define V4L2_H

#include <linux/videodev2.h>

const char *v4l2_field_name(enum v4l2_field const field);
const char *v4l2_type_name(enum v4l2_buf_type const type);
const char *v4l2_memory_name(enum v4l2_memory const memory);

void v4l2_print_format(struct v4l2_format const *const p);
void v4l2_print_buffer(struct v4l2_buffer const *const p);
int v4l2_open(char const *const device, uint32_t positive, uint32_t negative,
		char card[32]);
void v4l2_configure(int const fd, enum v4l2_buf_type const type,
		uint32_t const pixelformat, uint32_t const width,
		uint32_t const height);
void v4l2_framerate_configure(int const fd, enum v4l2_buf_type const type,
		unsigned const framerate);
float v4l2_framerate_get(int const fd, enum v4l2_buf_type const type);
uint32_t v4l2_buffers_request(int const fd, enum v4l2_buf_type const type,
		uint32_t const num, enum v4l2_memory const memory);
void v4l2_buffers_mmap(int const fd, enum v4l2_buf_type const type,
		uint32_t const num, void *bufs[], int const prot);
void v4l2_buffers_export(int const fd, enum v4l2_buf_type const type,
		uint32_t const num, int bufs[]);
void v4l2_dqbuf(int const fd, struct v4l2_buffer *const restrict buf);
void v4l2_qbuf(int const fd, struct v4l2_buffer *const restrict buf);
void v4l2_streamon(int const fd, enum v4l2_buf_type const type);

#endif /* V4L2_H */
