/*
 * Copyright (C) 2015 by Anton Leontiev <aleontiev@elvees.com>
 *
 * Tool to convert any video to prepared Y4M in M420 pixel format.
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
#include <unistd.h>

#include <libavdevice/avdevice.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/dict.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>

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
		FILE *const stream = level < LOG_INFO ? stderr : stdout;
		va_start(va, format);
		vfprintf(stream, format, va);
		fputc('\n', stream);
		va_end(va);
	}
}

#define pr_err(format, ...)   pr_level(LOG_ERROR, format, ##__VA_ARGS__)
#define pr_warn(format, ...)  pr_level(LOG_WARNING, format, ##__VA_ARGS__)
#define pr_info(format, ...)  pr_level(LOG_INFO, format, ##__VA_ARGS__)
#define pr_verb(format, ...)  pr_level(LOG_VERBOSE, format, ##__VA_ARGS__)
#define pr_debug(format, ...) pr_level(LOG_DEBUG, format, ##__VA_ARGS__)

static void yuv420_to_m420(AVFrame *frame) {
	unsigned const width = frame->width, height = frame->height;
	uint8_t *temp = malloc(width * height * 3 / 2);
	if (!temp) pr_err("Can not allocate memory for convertion buffer");

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

static void help(const char *program_name) {
	puts("any2m420 v1.0\n");
	printf("Synopsys: %s -o output-file input-file\n", program_name);
	puts("\nThis tool converts input video to M420 pixel format and outputs");
	puts("it to YUV4MPEG container.");
}

int main(int argc, char *argv[]) {
	AVFormatContext *ifc; //!< Input format context
	AVFormatContext *ofc = NULL; //!< Output format context
	AVCodecContext *icc; //!< Input codec context
	AVCodecContext *occ; //!< Output codec context
	AVCodec *ic; //!< Input codec
	AVCodec *oc; //!< Output codec
	AVDictionary *options = NULL;
	struct SwsContext *osc = NULL; //!< Output swscale context
	AVFrame *iframe = NULL; //!< Input frame
	AVFrame *oframe = NULL; //!< Output frame

	int rc, opt;

	char const *output = NULL;

	av_register_all();

	while ((opt = getopt(argc, argv, "ho:v")) != -1) {
		switch (opt) {
			case 'h': help(argv[0]); return EXIT_SUCCESS;
			case 'o': output = optarg; break;
			case 'v': vlevel++; break;
			default: error(EXIT_FAILURE, 0, "Try %s -h for help.", argv[0]);
		}
	}

	if (argc < optind + 1) error(EXIT_FAILURE, 0, "Not enough arguments");
	if (output == NULL) error(EXIT_FAILURE, 0, "Output file is not specified");

	char const *input = argv[optind];

	ifc = avformat_alloc_context();
	if (!ifc) error(EXIT_FAILURE, 0, "Can not allocate input format context");

	// Open video file
	if (avformat_open_input(&ifc, input, NULL, &options) < 0)
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

	osc = sws_getContext(icc->width, icc->height, icc->pix_fmt,
			icc->width, icc->height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL, NULL);
	if (osc == NULL) error(EXIT_FAILURE, 0, "Can't allocate SDL swscale context");

	// Allocate output context
	avformat_alloc_output_context2(&ofc, NULL, NULL, output);
	if (!ofc) error(EXIT_FAILURE, 0, "Can not allocate output context for %s", output);

	if (ofc->oformat->flags & AVFMT_NOFILE)
		error(EXIT_FAILURE, 0, "Unsupported output format");

	oc = avcodec_find_encoder(AV_CODEC_ID_RAWVIDEO);
	if (!oc) error(EXIT_FAILURE, 0, "Can not find rawvideo codec");

	AVStream *os = avformat_new_stream(ofc, oc);
	if (!os) error(EXIT_FAILURE, 0, "Can not allocate output stream");

	rc = avcodec_copy_context(ofc->streams[0]->codec, icc);
	if (rc < 0) error(EXIT_FAILURE, 0, "Can not copy codec context");

	// Without setting os->time_base ffmpeg issues a warning.
	os->time_base = ifc->streams[video_stream_number]->time_base;

	occ = os->codec;
	occ->width = icc->width;
	occ->height = icc->height;
	occ->pix_fmt = AV_PIX_FMT_YUV420P;
	occ->sample_aspect_ratio = icc->sample_aspect_ratio;

	rc = avcodec_open2(occ, oc, NULL);
	if (rc < 0) error(EXIT_FAILURE, 0, "Can not initialize output codec context");

	if (vlevel >= LOG_INFO) av_dump_format(ofc, 0, output, 1);

	rc = avio_open(&ofc->pb, output, AVIO_FLAG_WRITE);
	if (rc < 0) error(EXIT_FAILURE, 0, "Could not open output file '%s'", output);

	rc = avformat_write_header(ofc, NULL);
	if (rc < 0) error(EXIT_FAILURE, 0, "Can not write header for output file");

	oframe = av_frame_alloc();
	if (oframe == NULL) error(EXIT_FAILURE, 0, "Can not allocate output frame structure");

	oframe->width = occ->width;
	oframe->height = occ->height;
	oframe->format = occ->pix_fmt;

	// Fill all necessary
	rc = av_frame_get_buffer(oframe, 32);
	if (rc < 0) error(EXIT_FAILURE, 0, "Can not allocate output frame buffers");

	AVPacket ipacket;

	pr_verb("Begin processing...");

	while (av_read_frame(ifc, &ipacket) >= 0) {
		// Is this a packet from the video stream

		if (ipacket.stream_index == video_stream_number) {
			int frame_read;

			avcodec_decode_video2(icc, iframe, &frame_read, &ipacket);

			if (frame_read) {
				pr_verb("Frame is read...");

				sws_scale(osc, (uint8_t const* const*)iframe->data,
						iframe->linesize, 0, iframe->height, oframe->data,
						oframe->linesize);

				yuv420_to_m420(oframe);

				oframe->pts = iframe->pts;

				AVPacket opacket;
				av_init_packet(&opacket);

				opacket.flags        |= AV_PKT_FLAG_KEY;
				opacket.stream_index  = os->index;
				opacket.data          = (uint8_t *)oframe;
				opacket.size          = sizeof(AVPicture);

				opacket.pts = opacket.dts = oframe->pts;
				rc = av_write_frame(ofc, &opacket);
			}
		}

		// Free the packet that was allocated by av_read_frame
		av_free_packet(&ipacket);
	}

	av_write_trailer(ofc);

	av_frame_free(&oframe);
	av_frame_free(&iframe);

	rc = avio_close(ofc->pb);
	if (rc < 0) error(EXIT_FAILURE, 0, "Can not properly close output file");

	avformat_free_context(ofc);
	sws_freeContext(osc);

	avformat_close_input(&ifc);

	return EXIT_SUCCESS;
}
