/*
 * Tool to capture video from one V4L2 device, compress it to H.264 using
 * another V4L2 device and output to file or file descriptor.
 *
 * Copyright (C) 2015-2016 ELVEES NeoTek JSC
 * Author: Anton Leontiev <aleontiev@elvees.com>
 *
 * SPDX-License-Identifier:	GPL-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <error.h>
#include <limits.h>

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/poll.h>

#include <linux/videodev2.h>

#include "log.h"
#include "v4l2-utils.h"

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#define ROUND_UP(x, a) (((x)+(a)-1)&~((a)-1))

#define NUM_BUFS 4

static struct ctrl avico_mpeg_ctrls[] = {
	{
		.id = V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP
	},
	{
		.id = V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP
	},
	{
		.id = V4L2_CID_MPEG_VIDEO_H264_CHROMA_QP_INDEX_OFFSET
	},
	{
		.id = V4L2_CID_MPEG_VIDEO_GOP_SIZE
	}
};

static struct class_ctrls avico_ctrls[] = {
	{
		.which = V4L2_CTRL_CLASS_MPEG,
		.ctrls = avico_mpeg_ctrls,
		.cnt = ARRAY_SIZE(avico_mpeg_ctrls)
	}
};

static inline bool checklimit(unsigned const value, unsigned const limit)
{
	return limit == 0 || value < limit;
}

#ifndef VERSION
#define VERSION "unversioned"
#endif

static void help(const char *program_name)
{
	puts("cap-enc " VERSION " \n");
	printf("Synopsys: %s [options] input-device encode-device\n\n", program_name);
	puts("Options:");
	puts("    -f arg    Output file descriptor number");
	puts("    -n arg    Specify how many frames should be processed");
	puts("    -o arg    Output file name");
	puts("    -r arg    Specify desired framerate");
	puts("    -s arg    Set video size [defaults to 1280x720]");
	puts("    -c <ctrl>=<val>    Set the value of the controls [VIDIOC_S_EXT_CTRLS]");
	puts("    -v        Be more verbose. Can be specified multiple times");
}

int main(int argc, char *argv[])
{
	int opt;

	int inbufs[NUM_BUFS]; //!< Exported file descriptors of input buffers
	void *encbufs[NUM_BUFS]; //!< Mmaped addresses of encoding buffers

	unsigned frames = 0, encframe = 0, capframe = 0;
	uint32_t width = 1280, height = 720;

	unsigned framerate = 0;
	char const *output = NULL;
	int outfd = -1;

	const char *optstring = "f:hn:o:r:s:c:v";

	while ((opt = getopt(argc, argv, optstring)) != -1) {
		switch (opt) {
			case 'f': outfd = atoi(optarg); break;
			case 'h': help(argv[0]); return EXIT_SUCCESS;
			case 'n': frames = atoi(optarg); break;
			case 'o': output = optarg; break;
			case 'r': framerate = atoi(optarg); break;
			case 's': {
				char *endptr;

				width = strtol(optarg, &endptr, 10);
				if (*endptr != 'x')
					error(EXIT_FAILURE, 0, "Malformed argument: %s", optarg);

				height = strtol(endptr + 1, &endptr, 10);
				if (*endptr != '\0')
					error(EXIT_FAILURE, 0, "Malformed argument: %s", optarg);

				break;
			}
			case 'c': /* skip now, parse later */; break;
			case 'v': vlevel++; break;
			default: error(EXIT_FAILURE, 0, "Try %s -h for help.", argv[0]);
		}
	}

	if (argc < optind + 2)
		error(EXIT_FAILURE, 0, "Not enough arguments");

	char const *inputdevice = argv[optind];
	char const *m2mdevice = argv[optind + 1];

	char card[32];
	int inputfd = v4l2_open(inputdevice, V4L2_CAP_VIDEO_CAPTURE |
			V4L2_CAP_STREAMING, V4L2_CAP_VIDEO_M2M, card);
	pr_info("Capture card: %.32s", card);

	int m2mfd = v4l2_open(m2mdevice, V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING,
			0, card);
	pr_info("Encoding card: %.32s", card);

	find_controls(m2mfd, avico_ctrls, ARRAY_SIZE(avico_ctrls));
	optind = 0;
	while ((opt = getopt(argc, argv, optstring)) != -1) {
		switch (opt) {
			case 'c':
				parse_ctrl_opts(optarg, avico_ctrls, ARRAY_SIZE(avico_ctrls));
				break;
		}
	}

	v4l2_configure(inputfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_PIX_FMT_M420, width, height,
			ROUND_UP(width, 16));
	v4l2_configure(m2mfd, V4L2_BUF_TYPE_VIDEO_OUTPUT, V4L2_PIX_FMT_M420, width, height,
			ROUND_UP(width, 16));
	v4l2_configure(m2mfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_PIX_FMT_H264, width, height, 0);

	g_s_ctrls(m2mfd, avico_ctrls, ARRAY_SIZE(avico_ctrls), true);

	struct v4l2_fract timeperframe = { 1, framerate };

	v4l2_framerate_configure(inputfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, &timeperframe);
	v4l2_framerate_configure(m2mfd, V4L2_BUF_TYPE_VIDEO_OUTPUT, &timeperframe);
	v4l2_framerate_configure(m2mfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, &timeperframe);

	pr_info("Capture framerate: %.2f FPS",
			v4l2_framerate_get(inputfd, V4L2_BUF_TYPE_VIDEO_CAPTURE));
	pr_info("Encoding framerate: %.2f/%.2f FPS",
			v4l2_framerate_get(m2mfd, V4L2_BUF_TYPE_VIDEO_OUTPUT),
			v4l2_framerate_get(m2mfd, V4L2_BUF_TYPE_VIDEO_CAPTURE));

	v4l2_buffers_request(inputfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, NUM_BUFS, V4L2_MEMORY_MMAP);
	v4l2_buffers_request(m2mfd, V4L2_BUF_TYPE_VIDEO_OUTPUT, NUM_BUFS, V4L2_MEMORY_DMABUF);
	v4l2_buffers_request(m2mfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, NUM_BUFS, V4L2_MEMORY_MMAP);

	v4l2_buffers_export(inputfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, NUM_BUFS, inbufs);
	v4l2_buffers_mmap(m2mfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, NUM_BUFS, encbufs, PROT_READ);

	for (int i = 0; i < NUM_BUFS; i++) {
		struct v4l2_buffer buf = {
			.index = i,
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
			.memory = V4L2_MEMORY_MMAP
		};

		v4l2_qbuf(inputfd, &buf);

		buf.flags = 0;

		v4l2_qbuf(m2mfd, &buf);
	}

	v4l2_streamon(inputfd, V4L2_BUF_TYPE_VIDEO_CAPTURE);
	v4l2_streamon(m2mfd, V4L2_BUF_TYPE_VIDEO_OUTPUT);
	v4l2_streamon(m2mfd, V4L2_BUF_TYPE_VIDEO_CAPTURE);

	if (output) {
		outfd = creat(output, S_IRUSR | S_IRGRP | S_IROTH | S_IWUSR);
		if (outfd < 0)
			error(EXIT_FAILURE, errno, "Can not open output file");
	}

	pr_verb("Begin processing...");

	struct pollfd fds[2] = {
		{ inputfd, POLLIN },
		{ m2mfd, POLLOUT | POLLIN }
	};

	while (checklimit(encframe, frames)) {
		int rc = poll(fds, 2, 1000);
		if (rc < 0) break;
		if (rc == 0)
			error(EXIT_FAILURE, 0, "Timeout waiting for data...");

		if (fds[0].revents & POLLIN) {
			struct v4l2_buffer buf = {
				.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
				.memory = V4L2_MEMORY_MMAP
			};

			v4l2_dqbuf(inputfd, &buf);

			pr_debug("Got buffer %u from %d capture", buf.index, inputfd);
			pr_verb("Frame %u captured: %u bytes", capframe, buf.bytesused);

			buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
			buf.memory = V4L2_MEMORY_DMABUF;
			buf.m.fd = inbufs[buf.index];
			buf.flags = 0;

			v4l2_qbuf(m2mfd, &buf);

			capframe += 1;

			if (!checklimit(capframe, frames))
				fds[0].fd = -1;
		}

		if (fds[1].revents & POLLOUT) {
			struct v4l2_buffer buf = {
				.type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
				.memory = V4L2_MEMORY_DMABUF
			};

			v4l2_dqbuf(m2mfd, &buf);

			pr_debug("Got buffer %u from %d output", buf.index, m2mfd);

			buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			buf.memory = V4L2_MEMORY_MMAP;
			buf.bytesused = 0;
			buf.flags = 0;

			v4l2_qbuf(inputfd, &buf);
		}

		if (fds[1].revents & POLLIN) {
			struct v4l2_buffer buf = {
				.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
				.memory = V4L2_MEMORY_MMAP
			};

			v4l2_dqbuf(m2mfd, &buf);

			pr_debug("Got buffer %u from %d capture", buf.index, m2mfd);
			pr_info("Frame %u encoded: %u bytes", encframe, buf.bytesused);

			write(outfd, encbufs[buf.index], buf.bytesused);

			buf.flags = 0;
			buf.bytesused = 0;

			v4l2_qbuf(m2mfd, &buf);

			encframe += 1;
		}
	}

	return EXIT_SUCCESS;
}
