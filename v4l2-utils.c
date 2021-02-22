/*
 * V4L2 utility functions implementation
 *
 * Copyright (C) 2016 ELVEES NeoTek JSC
 * Author: Anton Leontiev <aleontiev@elvees.com>
 *
 * SPDX-License-Identifier:	GPL-2.0
 */

#include <stdint.h>
#include <math.h>
#include <error.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/videodev2.h>

#define LOG_PREFIX "V4L2"

#include "v4l2-utils.h"
#include "log.h"

static const char *v4l2_field_names[] = {
	[V4L2_FIELD_ANY]        = "any",
	[V4L2_FIELD_NONE]       = "none",
	[V4L2_FIELD_TOP]        = "top",
	[V4L2_FIELD_BOTTOM]     = "bottom",
	[V4L2_FIELD_INTERLACED] = "interlaced",
	[V4L2_FIELD_SEQ_TB]     = "seq-tb",
	[V4L2_FIELD_SEQ_BT]     = "seq-bt",
	[V4L2_FIELD_ALTERNATE]  = "alternate",
	[V4L2_FIELD_INTERLACED_TB] = "interlaced-tb",
	[V4L2_FIELD_INTERLACED_BT] = "interlaced-bt"
};

static const char *v4l2_type_names[] = {
	[V4L2_BUF_TYPE_VIDEO_CAPTURE]      = "vid-cap",
	[V4L2_BUF_TYPE_VIDEO_OVERLAY]      = "vid-overlay",
	[V4L2_BUF_TYPE_VIDEO_OUTPUT]       = "vid-out",
	[V4L2_BUF_TYPE_VBI_CAPTURE]        = "vbi-cap",
	[V4L2_BUF_TYPE_VBI_OUTPUT]         = "vbi-out",
	[V4L2_BUF_TYPE_SLICED_VBI_CAPTURE] = "sliced-vbi-cap",
	[V4L2_BUF_TYPE_SLICED_VBI_OUTPUT]  = "sliced-vbi-out",
	[V4L2_BUF_TYPE_VIDEO_OUTPUT_OVERLAY] = "vid-out-overlay",
	[V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE] = "vid-cap-mplane",
	[V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE] = "vid-out-mplane",
	[V4L2_BUF_TYPE_SDR_CAPTURE]        = "sdr-cap"
};

static const char *v4l2_memory_names[] = {
	[V4L2_MEMORY_MMAP]    = "mmap",
	[V4L2_MEMORY_USERPTR] = "userptr",
	[V4L2_MEMORY_OVERLAY] = "overlay",
	[V4L2_MEMORY_DMABUF]  = "dmabuf"
};

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#define v4l2_name(a, arr) (((unsigned)(a)) < ARRAY_SIZE(arr) ? arr[a] : "unknown")

const char *v4l2_field_name(enum v4l2_field const field)
{
	return v4l2_name(field, v4l2_field_names);
}

const char *v4l2_type_name(enum v4l2_buf_type const type)
{
	return v4l2_name(type, v4l2_type_names);
}

const char *v4l2_memory_name(enum v4l2_memory const memory)
{
	return v4l2_name(memory, v4l2_memory_names);
}

void v4l2_print_format(struct v4l2_format const *const p)
{
	const struct v4l2_pix_format *pix;
	const struct v4l2_pix_format_mplane *mp;
	const struct v4l2_vbi_format *vbi;
	const struct v4l2_sliced_vbi_format *sliced;
	const struct v4l2_window *win;
	const struct v4l2_sdr_format *sdr;
	unsigned i;

	pr_cont(LOG_DEBUG, "type=%s", v4l2_type_name(p->type));
	switch (p->type) {
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
		pix = &p->fmt.pix;
		pr_cont(LOG_DEBUG, ", width=%u, height=%u, "
			"pixelformat=%c%c%c%c, field=%s, "
			"bytesperline=%u, sizeimage=%u, colorspace=%d, "
			"flags=0x%x, ycbcr_enc=%u, quantization=%u\n",
			pix->width, pix->height,
			(pix->pixelformat & 0xff),
			(pix->pixelformat >>  8) & 0xff,
			(pix->pixelformat >> 16) & 0xff,
			(pix->pixelformat >> 24) & 0xff,
			v4l2_field_name(pix->field),
			pix->bytesperline, pix->sizeimage,
			pix->colorspace, pix->flags, pix->ycbcr_enc,
			pix->quantization);
		break;
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		mp = &p->fmt.pix_mp;
		pr_cont(LOG_DEBUG ,", width=%u, height=%u, "
			"format=%c%c%c%c, field=%s, "
			"colorspace=%d, num_planes=%u, flags=0x%x, "
			"ycbcr_enc=%u, quantization=%u\n",
			mp->width, mp->height,
			(mp->pixelformat & 0xff),
			(mp->pixelformat >>  8) & 0xff,
			(mp->pixelformat >> 16) & 0xff,
			(mp->pixelformat >> 24) & 0xff,
			v4l2_field_name(mp->field),
			mp->colorspace, mp->num_planes, mp->flags,
			mp->ycbcr_enc, mp->quantization);
		for (i = 0; i < mp->num_planes; i++)
			pr_debug("plane %u: bytesperline=%u sizeimage=%u\n", i,
					mp->plane_fmt[i].bytesperline,
					mp->plane_fmt[i].sizeimage);
		break;
	case V4L2_BUF_TYPE_VIDEO_OVERLAY:
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_OVERLAY:
		win = &p->fmt.win;
		/* Note: we can't print the clip list here since the clips
		 * pointer is a userspace pointer, not a kernelspace
		 * pointer. */
		pr_cont(LOG_DEBUG, ", wxh=%dx%d, x,y=%d,%d, field=%s, chromakey=0x%08x, clipcount=%u, clips=%p, bitmap=%p, global_alpha=0x%02x\n",
			win->w.width, win->w.height, win->w.left, win->w.top,
			v4l2_field_name(win->field),
			win->chromakey, win->clipcount, win->clips,
			win->bitmap, win->global_alpha);
		break;
	case V4L2_BUF_TYPE_VBI_CAPTURE:
	case V4L2_BUF_TYPE_VBI_OUTPUT:
		vbi = &p->fmt.vbi;
		pr_cont(LOG_DEBUG, ", sampling_rate=%u, offset=%u, samples_per_line=%u, "
			"sample_format=%c%c%c%c, start=%u,%u, count=%u,%u\n",
			vbi->sampling_rate, vbi->offset,
			vbi->samples_per_line,
			(vbi->sample_format & 0xff),
			(vbi->sample_format >>  8) & 0xff,
			(vbi->sample_format >> 16) & 0xff,
			(vbi->sample_format >> 24) & 0xff,
			vbi->start[0], vbi->start[1],
			vbi->count[0], vbi->count[1]);
		break;
	case V4L2_BUF_TYPE_SLICED_VBI_CAPTURE:
	case V4L2_BUF_TYPE_SLICED_VBI_OUTPUT:
		sliced = &p->fmt.sliced;
		pr_cont(LOG_DEBUG, ", service_set=0x%08x, io_size=%d\n",
				sliced->service_set, sliced->io_size);
		for (i = 0; i < 24; i++)
			pr_debug("line[%02u]=0x%04x, 0x%04x\n", i,
				sliced->service_lines[0][i],
				sliced->service_lines[1][i]);
		break;
	case V4L2_BUF_TYPE_SDR_CAPTURE:
		sdr = &p->fmt.sdr;
		pr_cont(LOG_DEBUG, ", pixelformat=%c%c%c%c\n",
			(sdr->pixelformat >>  0) & 0xff,
			(sdr->pixelformat >>  8) & 0xff,
			(sdr->pixelformat >> 16) & 0xff,
			(sdr->pixelformat >> 24) & 0xff);
		break;
	}
}

void v4l2_print_buffer(const char *const prefix,
		struct v4l2_buffer const *const p)
{
	pr_debug("%s: %02ld:%02d:%02d.%08ld index=%d, type=%s, "
		"flags=0x%08x, sequence=%d, memory=%s, bytesused=%d, length=%d, "
		"offset=%d",
			prefix,
			p->timestamp.tv_sec / 3600,
			(int)(p->timestamp.tv_sec / 60) % 60,
			(int)(p->timestamp.tv_sec % 60),
			(long)p->timestamp.tv_usec,
			p->index,
			v4l2_type_name(p->type),
			p->flags,
			p->sequence, v4l2_memory_name(p->memory),
			p->bytesused, p->length,
			(p->memory == V4L2_MEMORY_MMAP) ? p->m.offset : 0);
}

int v4l2_open(char const *const device, uint32_t positive, uint32_t negative,
		char card[32])
{
	int ret;
	struct v4l2_capability cap;
	struct stat stat;

	int fd = open(device, O_RDWR, 0);
	if (fd < 0) error(EXIT_FAILURE, errno, "Failed to open %s", device);

	pr_verb("Device %s descriptor is %d", device, fd);

	if (fstat(fd, &stat) == -1)
		error(EXIT_FAILURE, errno, "Failed to stat() %s", device);
	else if (!S_ISCHR(stat.st_mode))
		error(EXIT_FAILURE, 0, "%s is not a character device", device);

	ret = ioctl(fd, VIDIOC_QUERYCAP, &cap);
	if (ret != 0)
		error(EXIT_FAILURE, errno, "Failed to query device capabilities");

	if ((cap.capabilities & positive) != positive)
		error(EXIT_FAILURE, 0, "Device %s does not support required "
				"capabilities: %#08x", device, positive);

	if ((cap.capabilities & negative) != 0)
		error(EXIT_FAILURE, 0, "Device %s supports unsupported "
				"capabilities: %#08x", device, negative);

	if (card) memcpy(card, cap.card, 32);

	return fd;
}

void v4l2_pix_fmt_validate(struct v4l2_pix_format const *f, uint32_t const pixelformat,
		uint32_t const width, uint32_t const height, uint32_t const bytesperline)
{
	if ((f->width != width && width != 0) || (f->height != height && height != 0))
		error(EXIT_FAILURE, 0, "Invalid size %ux%u", f->width, f->height);

	if (f->pixelformat != pixelformat)
		error(EXIT_FAILURE, 0, "Invalid pixel format %c%c%c%c",
		      (f->pixelformat & 0xff),
		      (f->pixelformat >>  8) & 0xff,
		      (f->pixelformat >> 16) & 0xff,
		      (f->pixelformat >> 24) & 0xff);

	if (f->bytesperline != bytesperline && bytesperline != 0)
		error(EXIT_FAILURE, 0, "Invalid bytes per line %u", f->bytesperline);
}

void v4l2_setformat(int const fd, enum v4l2_buf_type const type, struct v4l2_format *f)
{
	f->type = type;

	pr_verb("Setup format for %d %s", fd, v4l2_type_name(type));

	if (ioctl(fd, VIDIOC_S_FMT, f) != 0)
		error(EXIT_FAILURE, 0, "Failed to set %s format", v4l2_type_name(type));

	v4l2_print_format(f);
}

void v4l2_getformat(int const fd, enum v4l2_buf_type const type,
		struct v4l2_format *f)
{
	f->type = type;

	pr_verb("Get format for %d %s", fd, v4l2_type_name(type));

	if (ioctl(fd, VIDIOC_G_FMT, f) != 0)
		error(EXIT_FAILURE, 0,
		      "Failed to get %s format", v4l2_type_name(type));
}


void v4l2_framerate_configure(int const fd, enum v4l2_buf_type const type,
		struct v4l2_fract *const timeperframe)
{
	int rc;
	struct v4l2_streamparm parm = {
		.type = type
	};

	struct v4l2_fract *tpf;
	uint32_t *cap;

	switch (type) {
		case V4L2_BUF_TYPE_VIDEO_CAPTURE:
			tpf = &parm.parm.capture.timeperframe;
			cap = &parm.parm.capture.capability;
			break;
		case V4L2_BUF_TYPE_VIDEO_OUTPUT:
			tpf = &parm.parm.output.timeperframe;
			cap = &parm.parm.output.capability;
			break;
		default:
			error(EXIT_FAILURE, 0, "Unsupported buffer type: %u", type);
	}

	pr_verb("Setup framerate for %d %s to %.1f", fd, v4l2_type_name(type),
			(float)timeperframe->denominator / timeperframe->numerator);

	rc = ioctl(fd, VIDIOC_G_PARM, &parm);
	if (rc != 0)
		error(EXIT_FAILURE, 0, "Failed to get device streaming parameters");

	if (!(*cap & V4L2_CAP_TIMEPERFRAME)) {
		pr_warn("Device %d %s does not support framerate adjustment", fd,
				v4l2_type_name(type));
		return;
	}

	*tpf = *timeperframe;

	rc = ioctl(fd, VIDIOC_S_PARM, &parm);
	if (rc != 0)
		error(EXIT_FAILURE, 0, "Failed to set device streaming parameters");

	if (tpf->numerator != timeperframe->numerator ||
	    tpf->denominator != timeperframe->denominator) {
		pr_warn("Device %d %s set framerate to %.1f", fd, v4l2_type_name(type),
				(float)tpf->denominator / tpf->numerator);
		*timeperframe = *tpf;
	}
}

float v4l2_framerate_get(int const fd, enum v4l2_buf_type const type)
{
	int rc;
	struct v4l2_streamparm parm = {
		.type = type
	};

	rc = ioctl(fd, VIDIOC_G_PARM, &parm);

	if (rc != 0) {
		pr_warn("Can not get device %d %s streaming parameters", fd,
				v4l2_type_name(type));
		return NAN;
	}

	return (float)parm.parm.capture.timeperframe.denominator /
			parm.parm.capture.timeperframe.numerator;
}

uint32_t v4l2_buffers_request(int const fd, enum v4l2_buf_type const type,
		uint32_t const num, enum v4l2_memory const memory)
{
	int rc;

	pr_verb("Obtaining %d %s buffers for %d %s", num, v4l2_memory_name(memory),
			fd, v4l2_type_name(type));

	struct v4l2_requestbuffers reqbuf = {
		.count = num,
		.type = type,
		.memory = memory
	};

	rc = ioctl(fd, VIDIOC_REQBUFS, &reqbuf);

	if (rc != 0)
		error(EXIT_FAILURE, errno, "Failed to request %s buffers",
				v4l2_type_name(type));

	if (reqbuf.count == 0)
		error(EXIT_FAILURE, 0, "Device gives zero %s buffers",
				v4l2_type_name(type));

	if (reqbuf.count != num)
		error(EXIT_FAILURE, 0, "Device gives %u %s buffers, but %u is requested",
				reqbuf.count, v4l2_type_name(type), num);

	pr_debug("Got %d %s buffers", reqbuf.count, v4l2_type_name(type));

	return reqbuf.count;
}

void v4l2_buffers_mmap(int const fd, enum v4l2_buf_type const type,
		uint32_t const num, void *bufs[], int const prot)
{
	int rc;

	for (int i = 0; i < num; ++i) {
		struct v4l2_buffer buf = {
			.index = i,
			.type = type
		};

		rc = ioctl(fd, VIDIOC_QUERYBUF, &buf);
		if (rc != 0) error(EXIT_FAILURE, errno, "Failed to query buffer");

		pr_debug("Got %s buffer #%u: length = %u", v4l2_type_name(type), i,
				buf.length);

		//! \todo size field is not needed
		bufs[i] = mmap(NULL, buf.length, prot, MAP_SHARED, fd,
				buf.m.offset);

		if (bufs[i] == MAP_FAILED)
			error(EXIT_FAILURE, errno, "Failed to mmap %s buffer",
					v4l2_type_name(type));
	}
}

void v4l2_buffers_export(int const fd, enum v4l2_buf_type const type,
		uint32_t const num, int bufs[])
{
	int rc;

	for (int i = 0; i < num; ++i) {
		struct v4l2_exportbuffer ebuf = {
			.index = i,
			.type = type
		};

		rc = ioctl(fd, VIDIOC_EXPBUF, &ebuf);
		if (rc != 0)
			error(EXIT_FAILURE, errno, "Failed to export %s buffer",
					v4l2_type_name(type));

		pr_debug("Exported %s buffer #%u: fd = %d", v4l2_type_name(type), i,
				ebuf.fd);

		bufs[i] = ebuf.fd;
	}
}

void v4l2_dqbuf(int const fd, struct v4l2_buffer *const restrict buf)
{
	int rc;

	rc = ioctl(fd, VIDIOC_DQBUF, buf);
	if (rc != 0)
		error(EXIT_FAILURE, errno, "Failed to dequeue %s buffer from %d",
				v4l2_type_name(buf->type), fd);

	v4l2_print_buffer("DQBUF", buf);

	if (buf->flags & V4L2_BUF_FLAG_ERROR)
		pr_warn("%s buffer #%d is flagged as erroneous",
				v4l2_type_name(buf->type), buf->sequence);
}

void v4l2_qbuf(int const fd, struct v4l2_buffer *const restrict buf)
{
	int rc;

	pr_debug("Enqueuing buffer #%d to %d %s", buf->index, fd,
			v4l2_type_name(buf->type));

	v4l2_print_buffer("QBUF", buf);
	rc = ioctl(fd, VIDIOC_QBUF, buf);

	if (rc != 0)
		error(EXIT_FAILURE, errno, "Failed to enqueue %s buffer to %d",
				v4l2_type_name(buf->type), fd);
}

void v4l2_streamon(int const fd, enum v4l2_buf_type const type)
{
	int rc;
	pr_verb("Stream on for %d %s", fd, v4l2_type_name(type));

	rc = ioctl(fd, VIDIOC_STREAMON, &type);
	if (rc != 0)
		error(EXIT_FAILURE, errno, "Failed to start %s stream",
				v4l2_type_name(type));
}

void v4l2_g_ext_ctrls(int const fd, uint32_t const which, uint32_t const count,
		      struct v4l2_ext_control *const controls)
{
	int rc;
	struct v4l2_ext_controls ctrls = {
		.ctrl_class = which,
		.count = count,
		.controls = controls
	};

	rc = ioctl(fd, VIDIOC_G_EXT_CTRLS, &ctrls);
	if (rc != 0)
		error(EXIT_FAILURE, errno, "Failed to get controls");
}

void v4l2_s_ext_ctrls(int const fd, uint32_t const which, uint32_t const count,
		      struct v4l2_ext_control *const controls)
{
	int rc;
	struct v4l2_ext_controls ctrls = {
		.ctrl_class = which,
		.count = count,
		.controls = controls
	};

	rc = ioctl(fd, VIDIOC_S_EXT_CTRLS, &ctrls);
	if (rc != 0) {
		if (ctrls.error_idx >= ctrls.count || count == 0)
			error(EXIT_FAILURE, errno, "Failed to set controls");
		else
			error(EXIT_FAILURE, errno, "Failed to set control %u",
			      controls[ctrls.error_idx].id);
	}
}

int query_ext_ctrl_ioctl(int const fd, struct v4l2_query_ext_ctrl *qctrl)
{
	struct v4l2_queryctrl qc;
	int rc;

	rc = ioctl(fd, VIDIOC_QUERY_EXT_CTRL, qctrl);
	if (rc != ENOTTY)
		return rc;

	qc.id = qctrl->id;
	rc = ioctl(fd, VIDIOC_QUERYCTRL, &qc);
	if (rc == 0) {
		qctrl->type = qc.type;
		memcpy(qctrl->name, qc.name, sizeof(qctrl->name));
		qctrl->minimum = qc.minimum;
		if (qc.type == V4L2_CTRL_TYPE_BITMASK) {
			qctrl->maximum = (__u32)qc.maximum;
			qctrl->default_value = (__u32)qc.default_value;
		} else {
			qctrl->maximum = qc.maximum;
			qctrl->default_value = qc.default_value;
		}
		qctrl->step = qc.step;
		qctrl->flags = qc.flags;
		qctrl->elems = 1;
		qctrl->nr_of_dims = 0;
		memset(qctrl->dims, 0, sizeof(qctrl->dims));
		switch (qctrl->type) {
		case V4L2_CTRL_TYPE_INTEGER64:
			qctrl->elem_size = sizeof(__s64);
			break;
		case V4L2_CTRL_TYPE_STRING:
			qctrl->elem_size = qc.maximum + 1;
			qctrl->flags |= V4L2_CTRL_FLAG_HAS_PAYLOAD;
			break;
		default:
			qctrl->elem_size = sizeof(__s32);
			break;
		}
		memset(qctrl->reserved, 0, sizeof(qctrl->reserved));
	}
	qctrl->id = qc.id;

	return rc;
}

/*
 * Copy name to var, but convert a substring of non-alphanumeric characters followed by
 * an alphanumeric character to the single underscore and convert an upper-case letter to
 * the corresponding lower-case letter.
 *
 * Examples:
 * - If name is "Gain, Automatic.", var will be "gain_automatic".
 * - If name is "H264 I-Frame QP Value", var will be "h264_i_frame_qp_value".
 */
static void name2var(const char *name, char *var)
{
	int add_underscore = 0;
	char *v = var;

	while (*name) {
		if (isalnum(*name)) {
			if (add_underscore)
				*var++ = '_';
			add_underscore = 0;
			*var++ = tolower(*name);
		} else if (v != var) {
			add_underscore = 1;
		}
		name++;
	}
	*var = '\0';
}

void find_controls(int const fd, struct class_ctrls cl[], __u32 const cl_cnt)
{
	int i, j;

	for (i = 0; i < cl_cnt; ++i) {
		struct ctrl *ctrls = cl[i].ctrls;

		for (j = 0; j < cl[i].cnt; ++j) {
			struct v4l2_query_ext_ctrl qc = {
				.id = ctrls[j].id
			};

			if (!query_ext_ctrl_ioctl(fd, &qc)) {
				name2var(qc.name, ctrls[j].name);
			} else {
				ctrls[j].unsupported = true;
				*ctrls[j].name = '\0';
			}
		}
	}
}

static bool parse_next_subopt(char **subs, char **value)
{
	static char *const subopts[] = {
		NULL
	};
	int const opt = getsubopt(subs, subopts, value);

	if (opt < 0 || *value)
		return false;

	return true;
}

static void find_supported_ctrl_by_name(struct class_ctrls const cl[],
					__u32 const cl_cnt, const char *name,
					size_t const size, struct ctrl **ctrlp)
{
	int i, j;

	for (i = 0; i < cl_cnt; ++i) {
		struct ctrl *ctrls = cl[i].ctrls;

		for (j = 0; j < cl[i].cnt; ++j)
			if (!ctrls[j].unsupported &&
			    strlen(ctrls[j].name) == size &&
			    strncmp(name, ctrls[j].name, size) == 0) {
				*ctrlp = &ctrls[j];
				return;
			}
	}
}

/*
 * Get the values of controls passed via command line and mark corresponding controls
 * using the set_value field.
 */
void parse_ctrl_opts(char *optarg, struct class_ctrls cl[], __u32 const cl_cnt)
{
	char *value, *subs;

	subs = optarg;
	while (*subs != '\0') {
		const char *equal = NULL;

		if (parse_next_subopt(&subs, &value))
			error(EXIT_FAILURE, 0, "No value given to suboption\n");

		equal = strchr(value, '=');
		if (equal) {
			struct ctrl *ctrl = NULL;

			if (equal - value <= 0)
				error(EXIT_FAILURE, 0, "Put control name before '='\n");

			find_supported_ctrl_by_name(cl, cl_cnt, value, equal - value,
						    &ctrl);
			if (ctrl) {
				ctrl->set_value = true;
				ctrl->value = strtol(equal + 1, NULL, 0);
			} else {
				pr_warn("Control %.*s isn't supported", equal - value,
					value);
			}
		} else {
			error(EXIT_FAILURE, 0, "Control '%s' without '='\n", value);
		}
	}
}

void g_s_ctrls(int const fd, struct class_ctrls cl[], __u32 const cl_cnt,
	       bool const print)
{
	int i, j;

	for (i = 0; i < cl_cnt; ++i) {
		__u32 const cnt = cl[i].cnt;
		__u32 s_cnt = 0, g_cnt = 0;
		struct ctrl *ctrls = cl[i].ctrls;
		struct v4l2_ext_control *s_ctrl, *g_ctrl, *s_p, *g_p;

		for (j = 0; j < cnt; ++j) {
			if (ctrls[j].unsupported)
				continue;

			if (ctrls[j].set_value)
				s_cnt++;
			else
				g_cnt++;
		}

		if (s_cnt == 0 && g_cnt == 0)
			continue;

		if (s_cnt) {
			s_ctrl = malloc(s_cnt * sizeof(*s_ctrl));
			memset(s_ctrl, 0, s_cnt * sizeof(*s_ctrl));
			s_p = s_ctrl;
		}

		if (g_cnt) {
			g_ctrl = malloc(g_cnt * sizeof(*g_ctrl));
			memset(g_ctrl, 0, g_cnt * sizeof(*g_ctrl));
			g_p = g_ctrl;
		}

		for (j = 0; j < cnt; ++j) {
			if (ctrls[j].unsupported)
				continue;

			if (ctrls[j].set_value) {
				s_p->id = ctrls[j].id;
				s_p->value = ctrls[j].value;
				s_p++;
			} else {
				g_p->id = ctrls[j].id;
				g_p->value = ctrls[j].value;
				g_p++;
			}
		}

		if (s_cnt) {
			v4l2_s_ext_ctrls(fd, cl[i].which, s_cnt, s_ctrl);
			s_p = s_ctrl;
		}

		if (g_cnt) {
			v4l2_g_ext_ctrls(fd, cl[i].which, g_cnt, g_ctrl);
			g_p = g_ctrl;
		}

		for (j = 0; j < cnt; ++j) {
			if (ctrls[j].unsupported)
				continue;

			if (ctrls[j].set_value) {
				ctrls[j].value = s_p->value;
				s_p++;
			} else {
				ctrls[j].value = g_p->value;
				g_p++;
			}

			if (print)
				pr_info("Control: %.32s = %d", ctrls[j].name,
					ctrls[j].value);
		}

		if (s_cnt)
			free(s_ctrl);

		if (g_cnt)
			free(g_ctrl);
	}
}
