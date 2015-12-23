/*
 * Copyright (C) 2009 by Pawel Osciak, p.osciak <at> samsung.com
 * Copyright (C) 2009 by Samsung Electronics Co., Ltd.
 * Copyright (C) 2012 by Tomasz Moń <desowin@gmail.com>
 * Copyright (C) 2015 by Anton Leontiev <aleontiev@elvees.com>
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
#include <unistd.h>
#include <errno.h>
#include <malloc.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <linux/videodev2.h>

#include <libavdevice/avdevice.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/dict.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>

#include "m420.h"

#define V4L2_CID_TRANS_TIME_MSEC (V4L2_CID_PRIVATE_BASE)
#define V4L2_CID_TRANS_NUM_BUFS  (V4L2_CID_PRIVATE_BASE + 1)

#define NUM_BUFS 4

static struct m2m_buffer {
	struct v4l2_buffer v4l2;
	void *buf;
	AVFrame *frame;
} out_bufs[NUM_BUFS], cap_bufs[NUM_BUFS];

enum loglevel {
	LOG_ERROR,
	LOG_WARNING,
	LOG_INFO,
	LOG_VERBOSE,
	LOG_DEBUG
};

static enum loglevel vlevel = LOG_WARNING;

static void pr_level(enum loglevel const level, char const *format, ...) {
	if (level <= vlevel) {
		va_list va;
		va_start(va, format);
		vfprintf(level < LOG_INFO ? stderr : stdout, format, va);
		putchar('\n');
		va_end(va);
	}
}

#define pr_err(format, ...)   pr_level(LOG_ERROR, format, ##__VA_ARGS__)
#define pr_warn(format, ...)  pr_level(LOG_WARNING, format, ##__VA_ARGS__)
#define pr_info(format, ...)  pr_level(LOG_INFO, format, ##__VA_ARGS__)
#define pr_verb(format, ...)  pr_level(LOG_VERBOSE, format, ##__VA_ARGS__)
#define pr_debug(format, ...) pr_level(LOG_DEBUG, format, ##__VA_ARGS__)

static int m2m_init(char const *const device, char card[32]) {
	int ret;
	struct v4l2_capability cap;

	pr_verb("M2M: Open device...");

	int fd = open(device, O_RDWR, 0);
	if (fd < 0) error(EXIT_FAILURE, errno, "Can not open %s", device);

	ret = ioctl(fd, VIDIOC_QUERYCAP, &cap);
	if (ret != 0) error(EXIT_FAILURE, errno, "ioctl");

	if (!(cap.capabilities & V4L2_CAP_VIDEO_M2M))
		error(EXIT_FAILURE, 0, "Device %s does not support memory-to-memory interface", device);

	if (card) memcpy(card, cap.card, 32);

	return fd;
}

static void m2m_vim2m_controls(int const fd) {
	bool hflip = false, vflip = false;
	int rc;
	struct v4l2_control ctrl;

	pr_verb("M2M: Setup vim2m controls...");

	if (hflip) {
		ctrl.id = V4L2_CID_HFLIP;
		ctrl.value = 1;
		rc = ioctl(fd, VIDIOC_S_CTRL, &ctrl);
		if (rc != 0) error(EXIT_SUCCESS, errno, "Can not set HFLIP");
	}

	if (vflip) {
		ctrl.id = V4L2_CID_VFLIP;
		ctrl.value = 1;
		rc = ioctl(fd, VIDIOC_S_CTRL, &ctrl);
		if (rc != 0) error(EXIT_SUCCESS, errno, "Can not set VFLIP");
	}

	ctrl.id = V4L2_CID_TRANS_TIME_MSEC;
	ctrl.value = 100;
	rc = ioctl(fd, VIDIOC_S_CTRL, &ctrl);
	if (rc != 0) error(EXIT_SUCCESS, errno, "Can not set transaction time");

	ctrl.id = V4L2_CID_TRANS_NUM_BUFS;
	ctrl.value = 1;
	rc = ioctl(fd, VIDIOC_S_CTRL, &ctrl);
	if (rc != 0) error(EXIT_SUCCESS, errno, "Can not set transaction length");
}

static void m2m_configure(int const fd, int const width, int const height) {
	int rc;
	struct v4l2_format fmt;

	pr_verb("M2M: Setup formats...");

	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	fmt.fmt.pix.width = width;
	fmt.fmt.pix.height = height;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_M420;
	fmt.fmt.pix.field = V4L2_FIELD_ANY;

	rc = ioctl(fd, VIDIOC_S_FMT, &fmt);
	if (rc != 0) error(EXIT_FAILURE, 0, "Can not set output format");

	pr_debug("M2M: Output configured: width = %u, height = %u, sizeimage = %u",
			fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.sizeimage);

	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = width;
	fmt.fmt.pix.height = height;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_H264;
	fmt.fmt.pix.field = V4L2_FIELD_ANY;

	rc = ioctl(fd, VIDIOC_S_FMT, &fmt);
	if (rc != 0) error(EXIT_FAILURE, 0, "Can not set output format");

	pr_debug("M2M: Capture configured: width = %u, height = %u, sizeimage = %u",
			fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.sizeimage);
}

static void m2m_buffers_get(int const fd) {
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
	if (rc != 0) error(EXIT_FAILURE, errno, "Can not request output buffers");
	if (outreqbuf.count == 0) error(EXIT_FAILURE, 0, "Device gives zero output buffers");
	pr_debug("M2M: Got %d output buffers", outreqbuf.count);

	rc = ioctl(fd, VIDIOC_REQBUFS, &capreqbuf);
	if (rc != 0) error(EXIT_FAILURE, errno, "Can not request capture buffers");
	if (capreqbuf.count == 0) error(EXIT_FAILURE, 0, "Device gives zero capture buffers");
	pr_debug("M2M: Got %d capture buffers", capreqbuf.count);

	for (int i = 0; i < outreqbuf.count; ++i) {
		struct v4l2_buffer *vbuf = &out_bufs[i].v4l2;
		vbuf->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		vbuf->memory = V4L2_MEMORY_MMAP;
		vbuf->index = i;

		rc = ioctl(fd, VIDIOC_QUERYBUF, vbuf);
		if (rc != 0) error(EXIT_FAILURE, errno, "Can not query output buffer");
		pr_debug("M2M: Got output buffer #%u: length = %u", i, vbuf->length);

		out_bufs[i].buf = mmap(NULL, vbuf->length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, vbuf->m.offset);
		if (out_bufs[i].buf == MAP_FAILED) error(EXIT_FAILURE, errno, "Can not mmap output buffer");
	}

	for (int i = 0; i < capreqbuf.count; ++i) {
		struct v4l2_buffer *vbuf = &cap_bufs[i].v4l2;
		vbuf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		vbuf->memory = V4L2_MEMORY_MMAP;
		vbuf->index = i;

		rc = ioctl(fd, VIDIOC_QUERYBUF, vbuf);
		if (rc != 0) error(EXIT_FAILURE, errno, "Can not query capture buffer");
		pr_debug("M2M: Got capture buffer #%u: length = %u", i, vbuf->length);

		cap_bufs[i].buf = mmap(NULL, vbuf->length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, vbuf->m.offset);
		if (cap_bufs[i].buf == MAP_FAILED) error(EXIT_FAILURE, errno, "Can not mmap capture buffer");
	}
}

static void m2m_streamon(int const fd) {
	int rc;
	enum v4l2_buf_type type;

	pr_verb("M2M: Stream on...");

	type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	rc = ioctl(fd, VIDIOC_STREAMON, &type);
	if (rc != 0) error(EXIT_FAILURE, errno, "Failed to start output stream");

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	rc = ioctl(fd, VIDIOC_STREAMON, &type);
	if (rc != 0) error(EXIT_FAILURE, errno, "Failed to start capture stream");
}

static void m2m_process(int const fd, struct v4l2_buffer const *const out, struct v4l2_buffer const *const cap) {
	pr_verb("M2M: Processing...");
	ioctl(fd, VIDIOC_QBUF, out);
	ioctl(fd, VIDIOC_QBUF, cap);

	ioctl(fd, VIDIOC_DQBUF, cap);
	ioctl(fd, VIDIOC_DQBUF, out);
}

#ifndef VERSION
#define VERSION "unversioned"
#endif

static void help(const char *program_name) {
	puts("m2m-test " VERSION " \n");
	printf("Synopsys: %s -d device [options] file | /dev/videoX\n\n", program_name);
	puts("Options:");
	puts("    -d arg    Specify M2M device to use [mandatory]");
	puts("    -f arg    Output file descriptor number");
	puts("    -n arg    Specify how many frames should be processed");
	puts("    -o arg    Output file name (takes precedence over -f)");
	puts("    -p arg    Specify output pixel format for M2M device");
	puts("    -r arg    When grabbing from camera specify desired framerate");
	puts("    -s arg    From which frame processing should be started");
	puts("    -t        Transform video to M420 [Avico-specific]");
	puts("    -v        Be more verbose. Can be specified multiple times");
}

int main(int argc, char *argv[]) {
	AVFormatContext *ifc; //!< Input format context
	AVFormatContext *ofc = NULL; //!< Output format context
	AVInputFormat *ifmt = NULL; //!< Input format
	AVCodecContext *icc; //!< Input codec context
	//AVCodecContext *occ; //!< Output codec context
	AVCodec *ic; //!< Input codec
	// AVCodec *oc; //!< Output codec
	AVDictionary *options = NULL;
	enum AVPixelFormat opf = AV_PIX_FMT_NONE; //!< Output pixel format
	struct SwsContext *dsc = NULL; //!< Device swscale context
	struct SwsContext *osc = NULL; //!< Output swscale context
	AVFrame *iframe = NULL; //!< Input frame
	AVFrame *oframe = NULL; //!< Output frame

	struct timespec start, stop, loopstart, loopstop;
	int rc, opt;
	int outfd = -1;

	unsigned offset = 0, frames = UINT_MAX, total_time = 0, looptime;
	char *framerate = NULL;
	bool use_v4l2 = false, transform = false;

	char const *output = NULL, *device = NULL;
	char const *opfn = NULL; //!< Output pixel format name

	av_register_all();

	while ((opt = getopt(argc, argv, "d:f:hn:o:p:r:s:tv")) != -1) {
		switch (opt) {
			case 'd': device = optarg; break;
			case 'f': outfd = atoi(optarg); break;
			case 'h': help(argv[0]); return EXIT_SUCCESS;
			case 'n': frames = atoi(optarg); break;
			case 'o': output = optarg; break;
			case 'p': opfn = optarg; break;
			case 'r': framerate = optarg; break;
			case 's': offset = atoi(optarg); break;
			case 't': transform = true; break;
			case 'v': vlevel++; break;
			default: error(EXIT_FAILURE, 0, "Try %s -h for help.", argv[0]);
		}
	}

	if (argc < optind + 1) error(EXIT_FAILURE, 0, "Not enough arguments");
	if (device == NULL) error(EXIT_FAILURE, 0, "You must specify device");

	char const *input = argv[optind];

	if (strncmp(input, "/dev/video", 10) == 0) {
		use_v4l2 = true; offset = 0;
		ifmt = av_find_input_format("v4l2");
		if (!ifmt) error(EXIT_FAILURE, 0, "Unknown input format: 'v4l2'");
	}

	if (framerate && ifmt && ifmt->priv_class &&
			av_opt_find(&ifmt->priv_class, "framerate", NULL, 0, AV_OPT_SEARCH_FAKE_OBJ)) {
		av_dict_set(&options, "framerate", framerate, 0);
	}

	ifc = avformat_alloc_context();
	if (!ifc) error(EXIT_FAILURE, 0, "Can not allocate input format context");

	// Open video file
	if (avformat_open_input(&ifc, input, ifmt, &options) < 0)
		error(EXIT_FAILURE, 0, "Can't open file: %s!", argv[optind]);

	// Retrieve stream information
	if(avformat_find_stream_info(ifc, NULL) < 0)
		error(EXIT_FAILURE, 0, "Could not find stream information");

	// Dump information about file onto standard error
	if (vlevel >= LOG_INFO) av_dump_format(ifc, 0, input, 0);

	// Find the first video stream
	int video_stream_number = -1;
	for (int i = 0; i < ifc->nb_streams; i++)
		if (ifc->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
			video_stream_number = i;
			break;
		}

	if (video_stream_number == -1)
		error(EXIT_FAILURE, 0, "Didn't find a video stream");

	// Get a pointer to the codec context for the video stream
	icc = ifc->streams[video_stream_number]->codec;

	// Find the decoder for the video stream
	ic = avcodec_find_decoder(icc->codec_id);
	if (!ic) error(EXIT_FAILURE, 0, "Unsupported codec");

	// Open codec
	if (avcodec_open2(icc, ic, NULL) < 0)
		error(EXIT_FAILURE, 0, "Could not open codec");

	iframe = av_frame_alloc();
	if (iframe == NULL) error(EXIT_FAILURE, 0, "Can not allocate memory for input frame");

	enum AVPixelFormat format = AV_PIX_FMT_YUV420P;

	//! \brief Device swscale context
	//! \detail Is used to convert read frame to M2M device output pixel format.
	dsc = sws_getContext(icc->width, icc->height, icc->pix_fmt,
			icc->width, icc->height, format, SWS_BILINEAR, NULL, NULL, NULL);
	if (dsc == NULL) error(EXIT_FAILURE, 0, "Can't allocate output swscale context");

	if (opfn) opf = av_get_pix_fmt(opfn);
	if (opf == AV_PIX_FMT_NONE) opf = format;
	if (opf != format) osc = sws_getContext(icc->width, icc->height, format,
			icc->width, icc->height, opf, SWS_BILINEAR, NULL, NULL, NULL);

	if (osc) {
		oframe = av_frame_alloc();
		if (oframe == NULL) error(EXIT_FAILURE, 0, "Can not allocate output frame structure");

		oframe->width = icc->width;
		oframe->height = icc->height;
		oframe->format = opf;

		rc = av_frame_get_buffer(oframe, 0);
		if (rc < 0) error(EXIT_FAILURE, 0, "Can not allocate output frame buffers");
	}

	int m2m_fd;
	char card[32];

	m2m_fd = m2m_init(device, card);
	pr_info("Card: %.32s", card);

	if (strncmp(card, "vim2m", 32) == 0) {
		m2m_vim2m_controls(m2m_fd);
	}

	m2m_configure(m2m_fd, icc->width, icc->height);
	m2m_buffers_get(m2m_fd);
	m2m_streamon(m2m_fd);

	pr_verb("Allocating AVFrames for obtained buffers...");

	int av_frame_size = avpicture_get_size(format, icc->width, icc->height);
	if (av_frame_size != out_bufs[0].v4l2.length)
		error(EXIT_FAILURE, 0, "FFmpeg and V4L2 buffer sizes are not equal");

	for (int i = 0; out_bufs[i].buf; i++) {
		AVFrame *frame = out_bufs[i].frame = av_frame_alloc();
		if (!frame) error(EXIT_FAILURE, 0, "Not enough memory");

		frame->format = format;
		frame->width = icc->width;
		frame->height = icc->height;

		avpicture_fill((AVPicture *)frame, out_bufs[i].buf, frame->format, frame->width, frame->height);
	}

	if (output) {
		outfd = creat(output, S_IRUSR | S_IRGRP | S_IROTH | S_IWUSR);
		if (outfd < 0)
			error(EXIT_FAILURE, errno, "Can not open output file");
	}

	/* if (output) {
		avformat_alloc_output_context2(&ofc, NULL, NULL, output);
		if (!ofc) error(EXIT_FAILURE, 0, "Can not allocate output context for %s", output);

		oc = avcodec_find_encoder(AV_CODEC_ID_RAWVIDEO);
		if (!oc) error(EXIT_FAILURE, 0, "Can not find rawvideo codec");

		AVStream *os = avformat_new_stream(ofc, oc);
		if (!os) error(EXIT_FAILURE, 0, "Can not allocate output stream");

		occ = os->codec;
		occ->width = icc->width;
		occ->height = icc->height;
		occ->pix_fmt = opf;
		occ->sample_aspect_ratio = icc->sample_aspect_ratio;

		rc = avcodec_open2(occ, oc, NULL);
		if (rc < 0) error(EXIT_FAILURE, 0, "Can not initialize output codec context");

		if (vlevel >= LOG_INFO) av_dump_format(ofc, 0, output, 1);

		if (!(ofc->oformat->flags & AVFMT_NOFILE)) {
			rc = avio_open(&ofc->pb, output, AVIO_FLAG_WRITE);
			if (rc < 0) error(EXIT_FAILURE, 0, "Could not open output file '%s'", output);
		}

		rc = avformat_write_header(ofc, NULL);
		if (rc < 0) error(EXIT_FAILURE, 0, "Can not write header for output file");
	} */

	AVPacket packet;

//	rc = clock_getres(CLOCK_MONOTONIC, &start);

	int64_t start_pts = 0, start_time = av_gettime();
	bool valid = true;
	unsigned int frame_number = offset;

	rc = clock_gettime(CLOCK_MONOTONIC, &loopstart);
	pr_verb("Begin processing...");

	while (av_read_frame(ifc, &packet) >= 0) {
		// Is this a packet from the video stream

		if (!start_pts) start_pts = packet.pts;

		if (use_v4l2 && packet.pts - start_pts + packet.duration < av_gettime() - start_time) {
			valid = false;
			pr_info("Frame dropped");
		} else valid = true;

		if (packet.stream_index == video_stream_number && valid) {
			int frame_read;

			avcodec_decode_video2(icc, iframe, &frame_read, &packet);

			if (frame_read) {
				pr_verb("Frame is read...");

				if (!offset) {
					sws_scale(dsc, (uint8_t const* const*)iframe->data, iframe->linesize, 0, iframe->height, out_bufs[0].frame->data, out_bufs[0].frame->linesize);

					rc = clock_gettime(CLOCK_MONOTONIC, &start);

					// Process frame
					if (transform) yuv420_to_m420(out_bufs[0].frame);
					out_bufs[0].v4l2.bytesused = out_bufs[0].frame->width * out_bufs[0].frame->height * 3 / 2;

					m2m_process(m2m_fd, &out_bufs[0].v4l2, &cap_bufs[0].v4l2);
					rc = clock_gettime(CLOCK_MONOTONIC, &stop);

					unsigned msec = (stop.tv_sec - start.tv_sec)*1000U +
							(unsigned)((stop.tv_nsec - start.tv_nsec)/1000000L);
					total_time += msec;

					pr_info("Frame %u (%u bytes): %u ms", frame_number, cap_bufs[0].v4l2.bytesused, msec);

					if (outfd >= 0)
						if (write(outfd, cap_bufs[0].buf, cap_bufs[0].v4l2.bytesused) < 0)
							error(EXIT_FAILURE, errno, "Can not write to output");

					/*if (ofc) {
						AVPacket packet = { };
						int finished;

						if (osc) sws_scale(osc, (uint8_t const* const*)cap_bufs[0].frame->data, cap_bufs[0].frame->linesize, 0, cap_bufs[0].frame->height,
								oframe->data, oframe->linesize);

						av_init_packet(&packet);
						packet.stream_index = 0;

						// \todo Use processed video
						rc = avcodec_encode_video2(occ, &packet, oframe ?: cap_bufs[0].frame, &finished);
						if (rc < 0) error(EXIT_FAILURE, 0, "Can not encode frame");

						if (finished) {
							rc = av_interleaved_write_frame(ofc, &packet);
							if (rc < 0) error(EXIT_FAILURE, 0, "Can not write output packet");
						}
					}*/

					frame_number += 1;

					--frames;
					pr_debug("-- frames = %u\n", frames);
					if (frames == 0) break;
				} else {
					offset--;
					pr_info("Frame skipped!\n");
				}
			}
		}

		// Free the packet that was allocated by av_read_frame
		av_free_packet(&packet);

		if (ofc) av_write_trailer(ofc);
	}

	rc = clock_gettime(CLOCK_MONOTONIC, &loopstop);
	looptime = (loopstop.tv_sec - loopstart.tv_sec)*1000U +
			(unsigned)((loopstop.tv_nsec - loopstart.tv_nsec)/1000000L);

	pr_info("Total time in M2M: %.1f s (%.1f FPS)",
			(float)total_time / 1000.0,
			(float)(frame_number - offset) * 1000.0f / (float)total_time);

	pr_info("Total time in main loop: %.1f s (%.1f FPS)",
			(float)looptime / 1000.0,
			(float)(frame_number - offset) * 1000.0f / (float)looptime);

	if (outfd >= 0)
		close(outfd);

	return EXIT_SUCCESS;
}
