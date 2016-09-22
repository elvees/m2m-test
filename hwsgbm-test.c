/*
 * Copyright (C) 2009 by Pawel Osciak, p.osciak <at> samsung.com
 * Copyright (C) 2009 by Samsung Electronics Co., Ltd.
 * Copyright (C) 2012 by Tomasz Moń <desowin@gmail.com>
 * Copyright (C) 2015-2016 by Anton Leontiev <aleontiev@elvees.com>
 *
 * Based on V4L2 video capture example and process-vmalloc.c
 * Capture + output (process) V4L2 device tester.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <error.h>

#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <malloc.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <linux/videodev2.h>
#include <linux/v4l2-mem2mem-stereo.h>

#include <libavdevice/avdevice.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/dict.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>

#include "m420.h"
#include "log.h"
#include "v4l2-utils.h"

#define V4L2_CID_TRANS_TIME_MSEC (V4L2_CID_PRIVATE_BASE)
#define V4L2_CID_TRANS_NUM_BUFS  (V4L2_CID_PRIVATE_BASE + 1)

#define NUM_BUFS 4

/* Some evident defines */
#define MSEC_IN_SEC 1000
#define USEC_IN_SEC 1000000
#define NSEC_IN_SEC 1000000000

static struct m2m_buffer {
	struct v4l2_buffer v4l2;
	void *buf;
	AVFrame *frame;
} out_bufs[NUM_BUFS], cap_bufs[NUM_BUFS];

static void m2m_buffers_get(int const fd)
{
	int rc;

	pr_verb("M2M: Obtaining buffers...");

	struct v4l2_requestbuffers outreqbuf = {
		.count = NUM_BUFS,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
		.memory = V4L2_MEMORY_MMAP
	};

	struct v4l2_requestbuffers capreqbuf = {
		.count = NUM_BUFS,
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_MMAP
	};

	rc = ioctl(fd, VIDIOC_REQBUFS, &outreqbuf);
	if (rc != 0)
		error(EXIT_FAILURE, errno, "Can't request output buffers");
	if (outreqbuf.count == 0)
		error(EXIT_FAILURE, 0, "Device gives zero output buffers");
	pr_debug("M2M: Got %d output buffers", outreqbuf.count);

	rc = ioctl(fd, VIDIOC_REQBUFS, &capreqbuf);
	if (rc != 0)
		error(EXIT_FAILURE, errno, "Can not request capture buffers");
	if (capreqbuf.count == 0)
		error(EXIT_FAILURE, 0, "Device gives zero capture buffers");
	pr_debug("M2M: Got %d capture buffers", capreqbuf.count);

	for (int i = 0; i < outreqbuf.count; ++i) {
		struct v4l2_buffer *vbuf = &out_bufs[i].v4l2;

		vbuf->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		vbuf->memory = V4L2_MEMORY_MMAP;
		vbuf->index = i;

		rc = ioctl(fd, VIDIOC_QUERYBUF, vbuf);
		if (rc != 0)
			error(EXIT_FAILURE, errno,
			      "Can't query output buffer");
		pr_debug("M2M: Got output buffer #%u: length = %u",
			 i, vbuf->length);

		out_bufs[i].buf = mmap(NULL, vbuf->length,
				       PROT_READ | PROT_WRITE, MAP_SHARED,
				       fd, vbuf->m.offset);
		if (out_bufs[i].buf == MAP_FAILED)
			error(EXIT_FAILURE, errno, "Can't mmap output buffer");
	}

	for (int i = 0; i < capreqbuf.count; ++i) {
		struct v4l2_buffer *vbuf = &cap_bufs[i].v4l2;

		vbuf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		vbuf->memory = V4L2_MEMORY_MMAP;
		vbuf->index = i;

		rc = ioctl(fd, VIDIOC_QUERYBUF, vbuf);
		if (rc != 0)
			error(EXIT_FAILURE, errno,
			      "Can't query capture buffer");
		pr_debug("M2M: Got capture buffer #%u: length = %u",
			 i, vbuf->length);

		cap_bufs[i].buf = mmap(NULL, vbuf->length,
				       PROT_READ | PROT_WRITE, MAP_SHARED,
				       fd, vbuf->m.offset);
		if (cap_bufs[i].buf == MAP_FAILED)
			error(EXIT_FAILURE, errno, "Can't mmap cap buffer");
	}
}

static void m2m_process_lr(int const fd,
			   struct v4l2_buffer const * const outl,
			   struct v4l2_buffer const * const outr,
			   struct v4l2_buffer const * const cap)
{
	pr_verb("M2M: Processing...");
	ioctl(fd, VIDIOC_QBUF, outl);
	ioctl(fd, VIDIOC_QBUF, outr);
	ioctl(fd, VIDIOC_QBUF, cap);

	ioctl(fd, VIDIOC_DQBUF, cap);
	ioctl(fd, VIDIOC_DQBUF, outl);
	ioctl(fd, VIDIOC_DQBUF, outr);
}

static inline struct timespec timespec_subtract(struct timespec const start,
		struct timespec const stop)
{
	struct timespec res = {
		.tv_sec = stop.tv_sec - start.tv_sec,
		.tv_nsec = stop.tv_nsec - start.tv_nsec
	};

	if (res.tv_nsec < 0) {
		res.tv_sec -= 1;
		res.tv_nsec += NSEC_IN_SEC;
	}

	return res;
}

static inline struct timespec timespec_add(struct timespec const x,
		struct timespec const y)
{
	struct timespec res = {
		.tv_sec = y.tv_sec + x.tv_sec,
		.tv_nsec = y.tv_nsec + x.tv_nsec
	};

	if (res.tv_nsec > NSEC_IN_SEC) {
		res.tv_sec += 1;
		res.tv_nsec -= NSEC_IN_SEC;
	}

	return res;
}

static inline unsigned timespec2msec(struct timespec const t)
{
	return t.tv_sec * MSEC_IN_SEC + t.tv_nsec / (NSEC_IN_SEC / MSEC_IN_SEC);
}

static inline float timespec2float(struct timespec const t)
{
	return t.tv_sec + (float)t.tv_nsec / 1e9;
}

static inline bool checklimit(unsigned const value, unsigned const limit)
{
	return limit == 0 || value < limit;
}

int write_y4m(int outfd, const void *buf, size_t nbyte, const char *chroma)
{
	int ret;

	ret = dprintf(outfd, "FRAME\n");
	if (ret < 0)
		return ret;
	ret = write(outfd, buf, nbyte);
	if (ret != nbyte)
		return ret;
	ret = write(outfd, chroma, nbyte / 2);
	if (ret != nbyte)
		return ret;
	return 0;
}

static unsigned process_stream(
	AVFormatContext * const ifcl, int const vstreaml,
	AVFormatContext * const ifcr, int const vstreamr,
	struct SwsContext *dsc, unsigned const offset,
	unsigned const frames, int const m2mfd, int const outfd,
	char *chroma,
	struct timespec *const m2mtime)
{
	static unsigned frame, skipped;

	AVPacket packetl, packetr;
	int rc = 0;
	struct timespec start, stop, frametime;

	AVFrame *iframel = av_frame_alloc();
	AVFrame *iframer = av_frame_alloc();

	if (iframel == NULL || iframer == NULL)
		error(EXIT_FAILURE, 0,
		      "Can't allocate memory for input frame");

	while (checklimit(frame, frames)) {
		int frame_readl = 0, frame_readr = 0;

		while (!frame_readl) {
			if (av_read_frame(ifcl, &packetl) == 0) {
				if (packetl.stream_index != vstreaml) {
					av_free_packet(&packetl);
					continue;
				}
				avcodec_decode_video2(
					ifcl->streams[vstreaml]->codec,
					iframel, &frame_readl, &packetl);
			} else {
				av_free_packet(&packetl);
				return frame;
			}
		}
		while (!frame_readr) {
			if (av_read_frame(ifcr, &packetr) == 0) {
				if (packetr.stream_index != vstreamr) {
					av_free_packet(&packetr);
					continue;
				}
				avcodec_decode_video2(
					ifcr->streams[vstreamr]->codec,
					iframer, &frame_readr, &packetr);
			} else {
				av_free_packet(&packetr);
				return frame;
			}
		}

		pr_verb("Frames are read...");

		if (skipped < offset) {
			skipped++;
			pr_verb("Frame skipped!");
			goto forth;
		}

		sws_scale(dsc, (uint8_t const * const *)iframel->data,
			iframel->linesize, 0,  iframel->height,
			out_bufs[0].frame->data, out_bufs[0].frame->linesize);
		sws_scale(dsc, (uint8_t const * const *)iframer->data,
			iframer->linesize, 0, iframer->height,
			out_bufs[1].frame->data, out_bufs[1].frame->linesize);

		rc = clock_gettime(CLOCK_MONOTONIC, &start);

		out_bufs[0].v4l2.bytesused = out_bufs[0].frame->width *
			out_bufs[0].frame->height * 3 / 2;
		out_bufs[0].v4l2.field = V4L2_FIELD_LEFT;
		out_bufs[1].v4l2.bytesused = out_bufs[1].frame->width *
			out_bufs[1].frame->height * 3 / 2;
		out_bufs[1].v4l2.field = V4L2_FIELD_RIGHT;

		m2m_process_lr(m2mfd, &out_bufs[0].v4l2, &out_bufs[1].v4l2,
			       &cap_bufs[0].v4l2);
		rc = clock_gettime(CLOCK_MONOTONIC, &stop);

		frametime = timespec_subtract(start, stop);
		*m2mtime = timespec_add(*m2mtime, frametime);

		pr_info("Frame %u (%u bytes): %u ms", frame,
			cap_bufs[0].v4l2.bytesused, timespec2msec(frametime));

		if (outfd >= 0)
			if (write_y4m(outfd, cap_bufs[0].buf,
				      cap_bufs[0].v4l2.bytesused, chroma))
				error(EXIT_FAILURE, errno,
				      "Can't write to output");
		frame += 1;

forth:
		av_free_packet(&packetl);
		av_free_packet(&packetr);
	}
	if (rc < 0 && rc != AVERROR_EOF)
		error(EXIT_FAILURE, 0,
		      "FFmpeg failed to read next packet: %d", rc);

	av_frame_free(&iframel);
	av_frame_free(&iframer);

	return frame;
}

#ifndef VERSION
#define VERSION "unversioned"
#endif

static void help(const char *program_name)
{
	puts("hwsgbm-test " VERSION "\n");
	printf(
		"Usage: %s -d device [options] leftfile rightfile\n\n",
		program_name);
	puts("Options:");
	puts("    -d arg    Specify M2M device to use [mandatory]");
	puts("    -f arg    Output file descriptor number");
	puts("    -m arg    Loop over input file (-1 means infinitely)");
	puts("    -n arg    Specify how many frames should be processed");
	puts("    -o arg    Output file name (takes precedence over -f)");
	puts("    -p arg    Specify output pixel format for M2M device");
	puts("    -s arg    From which frame processing should be started");
	puts("    -v        Be more verbose. Can be specified multiple times");
}

int main(int argc, char *argv[])
{
	AVFormatContext *ifcl, *ifcr;
	AVCodecContext *iccl, *iccr;
	AVCodec *icl, *icr;
	struct SwsContext *dsc = NULL;

	struct timespec loopstart, loopstop, looptime, m2mtime = { 0 };
	int rc, opt, ret = 0;
	int m2mfd, outfd = -1;

	unsigned int rwidth, rheight, offset = 0, frames = 0, loops = 1;

	char const *output = NULL, *device = NULL;
	char *chroma = NULL;

	av_register_all();

	while ((opt = getopt(argc, argv, "d:f:hm:n:o:p:s:tv")) != -1) {
		switch (opt) {
		case 'd':
			device = optarg;
			break;
		case 'f':
			outfd = atoi(optarg);
			break;
		case 'h':
			help(argv[0]); return EXIT_SUCCESS;
		case 'm':
			loops = atoi(optarg); break;
		case 'n':
			frames = atoi(optarg); break;
		case 'o':
			output = optarg; break;
		case 's':
			offset = atoi(optarg); break;
		case 'v':
			vlevel++; break;
		default:
			error(EXIT_FAILURE, 0, "Try %s -h for help.", argv[0]);
		}
	}

	if (argc < optind + 2)
		error(EXIT_FAILURE, 0, "Not enough arguments");
	char const *inputl = argv[optind];
	char const *inputr = argv[optind + 1];

	if (device == NULL)
		error(EXIT_FAILURE, 0,
		      "You must specify device");

	ifcl = avformat_alloc_context();
	ifcr = avformat_alloc_context();
	if (!ifcl || !ifcr)
		error(EXIT_FAILURE, 0,
		      "Can not allocate input format context");

	if (avformat_open_input(&ifcl, inputl, NULL, NULL) < 0)
		error(EXIT_FAILURE, 0, "Can't open left file: %s!", inputl);
	if (avformat_open_input(&ifcr, inputr, NULL, NULL) < 0)
		error(EXIT_FAILURE, 0, "Can't open right file: %s!", inputr);

	if (avformat_find_stream_info(ifcl, NULL) < 0
	    || avformat_find_stream_info(ifcr, NULL) < 0)
		error(EXIT_FAILURE, 0, "Could not find stream information");

	int vid_str_num_l = -1, vid_str_num_r = -1;

	for (int i = 0; i < ifcl->nb_streams; i++)
		if (ifcl->streams[i]->codec->codec_type
		    == AVMEDIA_TYPE_VIDEO) {
			vid_str_num_l = i;
			break;
		}
	for (int i = 0; i < ifcr->nb_streams; i++)
		if (ifcr->streams[i]->codec->codec_type
		    == AVMEDIA_TYPE_VIDEO) {
			vid_str_num_r = i;
			break;
		}

	if (vid_str_num_l == -1 || vid_str_num_r == -1)
		error(EXIT_FAILURE, 0, "Didn't find a video stream");

	iccl = ifcl->streams[vid_str_num_l]->codec;
	iccr = ifcr->streams[vid_str_num_r]->codec;

	icl = avcodec_find_decoder(iccl->codec_id);
	icr = avcodec_find_decoder(iccr->codec_id);
	if (!icl || !icr)
		error(EXIT_FAILURE, 0, "Unsupported codec");

	if (avcodec_open2(iccl, icl, NULL) < 0
		|| avcodec_open2(iccr, icr, NULL) < 0)
		error(EXIT_FAILURE, 0, "Could not open codec");

	if (iccl->width != iccr->width
	    || iccl->height != iccr->height
	    || iccl->pix_fmt != iccr->pix_fmt)
		error(EXIT_FAILURE, 0, "Left vid fmt != right vid fmt");

	enum AVPixelFormat format = AV_PIX_FMT_YUV420P;

	dsc = sws_getContext(
		iccl->width, iccl->height, iccl->pix_fmt,
		iccl->width, iccl->height,
		format, SWS_BILINEAR, NULL, NULL, NULL);
	if (dsc == NULL)
		error(EXIT_FAILURE, 0, "Can't alloc output swscale context");

	char card[32];

	m2mfd = v4l2_open(device,
			  V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING, 0, card);
	pr_info("Card: %.32s", card);

	v4l2_configure(m2mfd, V4L2_BUF_TYPE_VIDEO_OUTPUT, V4L2_PIX_FMT_NV21,
			iccl->width, iccl->height);
	v4l2_configure(m2mfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_PIX_FMT_GREY,
			0, 0);
	v4l2_getformat(m2mfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, NULL,
		       &rwidth, &rheight)

	m2m_buffers_get(m2mfd);
	chroma = malloc(rwidth * rheight / 2);
	if (chroma == NULL)
		error(EXIT_FAILURE, 0, "Can't alloc frame color array");
	memset(chroma, 0x80, rwidth * rheight / 2);

	v4l2_streamon(m2mfd, V4L2_BUF_TYPE_VIDEO_OUTPUT);
	v4l2_streamon(m2mfd, V4L2_BUF_TYPE_VIDEO_CAPTURE);

	pr_verb("Allocating AVFrames for obtained buffers...");

	int av_frame_size = avpicture_get_size(format,
					       iccl->width, iccl->height);
	if (av_frame_size != out_bufs[0].v4l2.length)
		error(EXIT_FAILURE, 0,
		      "FFmpeg and V4L2 buffer sizes aren't equal");

	for (int i = 0; out_bufs[i].buf; i++) {
		AVFrame *frame = out_bufs[i].frame = av_frame_alloc();

		if (!frame)
			error(EXIT_FAILURE, 0, "Not enough memory");

		frame->format = format;
		frame->width = iccl->width;
		frame->height = iccl->height;

		avpicture_fill((AVPicture *)frame, out_bufs[i].buf,
			       frame->format, frame->width, frame->height);
	}

	if (output) {
		outfd = creat(output, S_IRUSR | S_IRGRP | S_IROTH | S_IWUSR);
		if (outfd < 0)
			error(EXIT_FAILURE, errno, "Can not open output file");
		ret = dprintf(outfd,
				"YUV4MPEG2 W%d H%d F25:1 A0:0 C420 XYSCS=420\n",
				rwidth, rheight)
		if (ret < 0)
			error(EXIT_FAILURE, errno,
				"Can't write to output file");
	}

	unsigned int frame = 0;

	rc = clock_gettime(CLOCK_MONOTONIC, &loopstart);
	pr_verb("Begin processing...");

	for (unsigned loop = 0;
	     checklimit(loop, loops) && checklimit(frame, frames);
	     loop++) {
		pr_verb("Loop #%u", loop);

		if (loop != 0) {
			rc = avformat_seek_file(ifcl, vid_str_num_l, 0, 0, 0,
					AVSEEK_FLAG_FRAME);
			if (rc < 0)
				error(EXIT_FAILURE, 0,
				      "Can't rewind left input file: %d", rc);
			rc = avformat_seek_file(ifcr, vid_str_num_r, 0, 0, 0,
					AVSEEK_FLAG_FRAME);
			if (rc < 0)
				error(EXIT_FAILURE, 0,
				      "Can't rewind right input file: %d", rc);
		}
		frame = process_stream(ifcl, vid_str_num_l,
				       ifcr, vid_str_num_r,
				       dsc, offset, frames,
				       m2mfd, outfd, chroma, &m2mtime);
	}

	rc = clock_gettime(CLOCK_MONOTONIC, &loopstop);
	looptime = timespec_subtract(loopstart, loopstop);

	pr_info("Total time in M2M: %.1f s (%.1f FPS)",
		timespec2float(m2mtime), frame / timespec2float(m2mtime));

	pr_info("Total time in main loop: %.1f s (%.1f FPS)",
		timespec2float(looptime), frame / timespec2float(looptime));

	if (outfd >= 0)
		close(outfd);

	free(chroma);

	return EXIT_SUCCESS;
}
