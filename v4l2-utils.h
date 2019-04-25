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

struct ctrl {
	__u32 id;
	char name[32];
	__s32 value;
	bool unsupported;
	bool set_value;
};

struct class_ctrls {
	__u32 which;
	struct ctrl *ctrls;
	__u32 cnt;
};

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
void v4l2_getformat(int const fd, enum v4l2_buf_type const type,
	uint32_t *pixelformat, uint32_t *width,
	uint32_t *height);
void v4l2_framerate_configure(int const fd, enum v4l2_buf_type const type,
		struct v4l2_fract *const timeperframe);
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
void v4l2_g_ext_ctrls(int const fd, uint32_t const which, uint32_t const count,
		      struct v4l2_ext_control *const controls);
void v4l2_s_ext_ctrls(int const fd, uint32_t const which, uint32_t const count,
		      struct v4l2_ext_control *const controls);
int query_ext_ctrl_ioctl(int const fd, struct v4l2_query_ext_ctrl *qctrl);

void find_controls(int const fd, struct class_ctrls cl[], __u32 const cl_cnt);
void parse_ctrl_opts(char *optarg, struct class_ctrls cl[], __u32 const cl_cnt);
void g_s_ctrls(int const fd, struct class_ctrls cl[], __u32 const cl_cnt, bool const print);

#endif /* V4L2_H */
