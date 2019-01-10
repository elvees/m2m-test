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
#include <unistd.h>
#include <errno.h>
#include <malloc.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/poll.h>

#include <linux/videodev2.h>

#include <libavdevice/avdevice.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/dict.h>
#include <libavutil/imgutils.h>
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
		vbuf->flags = 0;

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
		vbuf->flags = 0;

		rc = ioctl(fd, VIDIOC_QUERYBUF, vbuf);
		if (rc != 0) error(EXIT_FAILURE, errno, "Can not query capture buffer");
		pr_debug("M2M: Got capture buffer #%u: length = %u", i, vbuf->length);

		cap_bufs[i].buf = mmap(NULL, vbuf->length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, vbuf->m.offset);
		if (cap_bufs[i].buf == MAP_FAILED) error(EXIT_FAILURE, errno, "Can not mmap capture buffer");
	}

	for (int i = 0; i < NUM_BUFS; i++) {
		struct v4l2_buffer buf = {
			.index = i,
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
			.memory = V4L2_MEMORY_MMAP
		};

		v4l2_qbuf(fd, &buf);
	}
}

static void queue_outbuf(int const fd, struct SwsContext *dsc, AVFrame * const iframe,
		bool const transform, int const index)
{
	sws_scale(dsc, (uint8_t const * const*)iframe->data,
			iframe->linesize, 0, iframe->height,
			out_bufs[index].frame->data,
			out_bufs[index].frame->linesize);
	/* Process frame */
	if (transform)
		yuv420_to_m420(out_bufs[index].frame);

	out_bufs[index].v4l2.bytesused = out_bufs[index].frame->width *
			out_bufs[index].frame->height * 3 / 2;
	out_bufs[index].v4l2.flags = 0;
	v4l2_qbuf(fd, &out_bufs[index].v4l2);
}

static void dequeue_outbuf(int const fd, int const index)
{
	v4l2_dqbuf(fd, &out_bufs[index].v4l2);
	if (index != out_bufs[index].v4l2.index)
		error(EXIT_FAILURE, 0, "Error index of buffer.");
}

static unsigned process_capbuf(int const fd, int const outfd)
{
	int rc = 0;
	struct v4l2_buffer buf = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_MMAP
	};
	unsigned bytesused = 0;

	v4l2_dqbuf(fd, &buf);
	bytesused = buf.bytesused;
	if (outfd >= 0) {
		rc = write(outfd, cap_bufs[buf.index].buf, buf.bytesused);
		if (rc < 0)
			error(EXIT_FAILURE, errno, "Can not write to output");
	}

	buf.flags = 0;
	buf.bytesused = 0;
	v4l2_qbuf(fd, &buf);

	return bytesused;
}

static void m2m_process(int const fd, int const outfd, struct SwsContext *dsc,
		AVFrame * const iframe, bool const transform, unsigned const frame,
		unsigned *const encframe)
{
	int rc = 0;
	unsigned i = frame % NUM_BUFS;
	unsigned bytesused = 0;

	if (!(out_bufs[i].v4l2.flags & V4L2_BUF_FLAG_QUEUED)) {
		queue_outbuf(fd, dsc, iframe, transform, i);
	} else {
		struct pollfd fds[1] = {
			{ fd, POLLOUT | POLLIN }
		};

		while (1) {
			rc = poll(fds, 1, 1000);
			if (rc < 0)
				error(EXIT_FAILURE, errno, "Poll error");
			if (rc == 0)
				error(EXIT_FAILURE, 0, "Timeout waiting for data...");

			if (fds[0].revents & POLLIN) {
				bytesused = process_capbuf(fd, outfd);
				pr_verb("Compressed frame %u (%u bytes)", *encframe, bytesused);
				*encframe += 1;
			}

			if (fds[0].revents & POLLOUT) {
				dequeue_outbuf(fd, i);
				queue_outbuf(fd, dsc, iframe, transform, i);
				break;
			}
		}
	}
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

/*
 * Limitations: The next parts work synchronously and can influence
 * each other and overall test performance:
 * - functions of FFmpeg
 * - yuv420_to_m420()
 * - writing of processed (V4L2_BUF_TYPE_VIDEO_CAPTURE) frame
 */
static unsigned process_stream(AVFormatContext *const ifc,
		AVCodecContext *const icc, int const stream, struct SwsContext *dsc,
		unsigned const offset, unsigned const frames, bool const transform,
		int const m2mfd, int const outfd, unsigned *const encframe)
{
	static int64_t start_pts = 0;
	static unsigned frame = 0, skipped = 0;

	AVPacket packet;
	int rc = 0;

	AVFrame *iframe = av_frame_alloc();

	if (iframe == NULL)
		error(EXIT_FAILURE, 0, "Can not allocate memory for input frame");

	while (checklimit(frame, frames)) {
		rc = av_read_frame(ifc, &packet);
		if (rc == AVERROR_EOF)
			break; /// \todo Draining
		else if (rc != 0)
			error(EXIT_FAILURE, 0, "Failed to read next packet: %d", rc);

		if (!start_pts) start_pts = packet.pts;

		if (packet.stream_index != stream)
			goto forth;

		rc = avcodec_send_packet(icc, &packet);
		if (rc)
			error(EXIT_FAILURE, 0, "Failed to send packet to decoder");

		while((rc = avcodec_receive_frame(icc, iframe)) == 0) {
			pr_verb("Frame is read...");

			if (skipped < offset) {
				skipped++;
				pr_verb("Frame skipped!");
				continue;
			}

			m2m_process(m2mfd, outfd, dsc, iframe, transform, frame, encframe);

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

			frame += 1;
		}

		if (rc != 0 && rc != AVERROR(EAGAIN) && rc != AVERROR_EOF)
			error(EXIT_FAILURE, 0, "Failed to read decoded frame");

forth:
		// Unref data that was allocated by av_read_frame()
		av_packet_unref(&packet);

		/* if (ofc) av_write_trailer(ofc); */
	}

	av_frame_free(&iframe);

	return frame;
}

static void m2m_drain(int const fd, int const outfd, unsigned encframe, unsigned const frames)
{
	int rc = 0;
	unsigned bytesused = 0;

	while (checklimit(encframe, frames)) {
		bytesused = process_capbuf(fd, outfd);
		pr_verb("Compressed frame %u (%u bytes)", encframe, bytesused);
		encframe += 1;
	}
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
	puts("    -l arg    Loop over input file (-1 means infinitely)");
	puts("    -n arg    Specify how many frames should be processed");
	puts("    -o arg    Output file name (takes precedence over -f)");
	puts("    -p arg    Specify output pixel format for M2M device");
	puts("    -r arg    When grabbing from camera specify desired framerate");
	puts("    -s arg    From which frame processing should be started");
	puts("    -t        Transform video to M420 [Avico-specific]");
	puts("    -q arg    Set quantization parameter");
	puts("    -v        Be more verbose. Can be specified multiple times");
}

int main(int argc, char *argv[]) {
	AVFormatContext *ifc = NULL; //!< Input format context
	/* AVFormatContext *ofc = NULL; //!< Output format context */
	AVInputFormat *ifmt = NULL; //!< Input format
	AVCodecContext *icc; //!< Input codec context
	//AVCodecContext *occ; //!< Output codec context
	AVCodec *ic; //!< Input codec
	// AVCodec *oc; //!< Output codec
	AVDictionary *options = NULL;
	enum AVPixelFormat opf = AV_PIX_FMT_NONE; //!< Output pixel format
	struct SwsContext *dsc = NULL; //!< Device swscale context
	struct SwsContext *osc = NULL; //!< Output swscale context
	AVFrame *oframe = NULL; //!< Output frame

	struct timespec loopstart, loopstop, looptime = { 0 };
	int rc, opt;
	int m2mfd, outfd = -1;

	unsigned offset = 0, frames = 0, loops = 1;
	char *framerate = NULL;
	bool transform = false;
	int qp = -1;

	char const *output = NULL, *device = NULL;
	char const *opfn = NULL; //!< Output pixel format name

	av_register_all();

	while ((opt = getopt(argc, argv, "d:f:hl:n:o:p:r:s:tq:v")) != -1) {
		switch (opt) {
			case 'd': device = optarg; break;
			case 'f': outfd = atoi(optarg); break;
			case 'h': help(argv[0]); return EXIT_SUCCESS;
			case 'l': loops = atoi(optarg); break;
			case 'n': frames = atoi(optarg); break;
			case 'o': output = optarg; break;
			case 'p': opfn = optarg; break;
			case 'r': framerate = optarg; break;
			case 's': offset = atoi(optarg); break;
			case 't': transform = true; break;
			case 'q': qp = atoi(optarg); break;
			case 'v': vlevel++; break;
			default: error(EXIT_FAILURE, 0, "Try %s -h for help.", argv[0]);
		}
	}

	if (argc < optind + 1) error(EXIT_FAILURE, 0, "Not enough arguments");
	if (device == NULL) error(EXIT_FAILURE, 0, "You must specify device");

	char const *input = argv[optind];

	if (framerate && ifmt && ifmt->priv_class &&
			av_opt_find(&ifmt->priv_class, "framerate", NULL, 0, AV_OPT_SEARCH_FAKE_OBJ)) {
		av_dict_set(&options, "framerate", framerate, 0);
	}

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
		if (ifc->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
			video_stream_number = i;
			break;
		}

	if (video_stream_number == -1)
		error(EXIT_FAILURE, 0, "Didn't find a video stream");

	// Find the decoder for the video stream
	ic = avcodec_find_decoder(ifc->streams[video_stream_number]->codecpar->codec_id);
	if (!ic)
		error(EXIT_FAILURE, 0, "Unsupported codec");

	// Allocate the codec context for the video stream
	icc = avcodec_alloc_context3(ic);
	if (!icc)
		error(EXIT_FAILURE, 0, "Failed to allocate codec context");

	rc = avcodec_parameters_to_context(icc, ifc->streams[video_stream_number]->codecpar);
	if (rc)
		error(EXIT_FAILURE, 0, "Failed to copy codec parameters to decoder context");

	// Open codec
	if (avcodec_open2(icc, ic, NULL) < 0)
		error(EXIT_FAILURE, 0, "Could not open codec");

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

	char card[32];

	m2mfd = v4l2_open(device, V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING, 0, card);
	pr_info("Card: %.32s", card);

	if (strncmp(card, "vim2m", 32) == 0) {
		m2m_vim2m_controls(m2mfd);
	}

	v4l2_configure(m2mfd, V4L2_BUF_TYPE_VIDEO_OUTPUT, V4L2_PIX_FMT_M420,
			icc->width, icc->height);
	v4l2_configure(m2mfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_PIX_FMT_H264,
			icc->width, icc->height);

	if (qp >= 0) {
		struct v4l2_ext_control ctrl = {
			.id = V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP,
			.value = qp
		};

		v4l2_s_ext_ctrls(m2mfd, V4L2_CTRL_CLASS_MPEG, 1, &ctrl);

		if (qp != ctrl.value)
			pr_warn("QP from VPU: %d", ctrl.value);
	}

	m2m_buffers_get(m2mfd);

	v4l2_streamon(m2mfd, V4L2_BUF_TYPE_VIDEO_OUTPUT);
	v4l2_streamon(m2mfd, V4L2_BUF_TYPE_VIDEO_CAPTURE);

	pr_verb("Allocating AVFrames for obtained buffers...");

	int av_frame_size = av_image_get_buffer_size(format, icc->width, icc->height, 1);
	if (av_frame_size != out_bufs[0].v4l2.length)
		error(EXIT_FAILURE, 0, "FFmpeg and V4L2 buffer sizes are not equal");

	for (int i = 0; out_bufs[i].buf; i++) {
		AVFrame *frame = out_bufs[i].frame = av_frame_alloc();
		if (!frame) error(EXIT_FAILURE, 0, "Not enough memory");

		frame->format = format;
		frame->width = icc->width;
		frame->height = icc->height;

		av_image_fill_arrays(frame->data, frame->linesize, out_bufs[i].buf,
				frame->format, frame->width, frame->height, 1);
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

	unsigned int frame = 0, encframe = 0;

	rc = clock_gettime(CLOCK_MONOTONIC, &loopstart);
	pr_verb("Begin processing...");

	for (unsigned loop = 0; checklimit(loop, loops) && checklimit(frame, frames); loop++) {
		pr_verb("Loop #%u", loop);

		if (loop != 0) {
			rc = avformat_seek_file(ifc, video_stream_number, 0, 0, 0,
					AVSEEK_FLAG_FRAME);
			if (rc < 0)
				error(EXIT_FAILURE, 0, "Can not rewind input file: %d", rc);
		}

		frame = process_stream(ifc, icc, video_stream_number, dsc, offset,
				frames, transform, m2mfd, outfd, &encframe);
	}

	m2m_drain(m2mfd, outfd, encframe, frame);

	rc = clock_gettime(CLOCK_MONOTONIC, &loopstop);
	looptime = timespec_subtract(loopstart, loopstop);

	pr_info("Total time in main loop: %.1f s (%.1f FPS)",
			timespec2float(looptime), frame / timespec2float(looptime));

	if (outfd >= 0)
		close(outfd);

	return EXIT_SUCCESS;
}
