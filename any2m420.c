/*
 * Tool to convert any video to prepared Y4M in M420 pixel format.
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

#include "log.h"
#include "m420.h"

#ifndef VERSION
#define VERSION "unversioned"
#endif

static void help(const char *program_name) {
	puts("any2m420 " VERSION "\n");
	printf("Synopsys: %s -o output-file input-file\n", program_name);
	puts("\nThis tool converts input video to M420 pixel format and outputs");
	puts("it to YUV4MPEG container.");
}

int main(int argc, char *argv[]) {
	AVFormatContext *ifc = NULL; //!< Input format context
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

	oc = avcodec_find_encoder(AV_CODEC_ID_WRAPPED_AVFRAME);
	if (!oc) error(EXIT_FAILURE, 0, "Can not find wrapped avframe codec");

	AVStream *os = avformat_new_stream(ofc, oc);
	if (!os) error(EXIT_FAILURE, 0, "Can not allocate output stream");

	// Without setting os->time_base ffmpeg issues a warning.
	os->time_base = ifc->streams[video_stream_number]->time_base;

	rc = avcodec_parameters_copy(os->codecpar, ifc->streams[video_stream_number]->codecpar);
	if (rc)
		error(EXIT_FAILURE, 0, "Failed to copy codec parameters to output stream");

	os->codecpar->codec_id = oc->id;
	os->codecpar->format = AV_PIX_FMT_YUV420P;

	occ = avcodec_alloc_context3(oc);
	if (!occ)
		error(EXIT_FAILURE, 0, "Failed to allocate encoder context");

	rc = avcodec_parameters_to_context(occ, os->codecpar);
	if (rc)
		error(EXIT_FAILURE, 0, "Failed to copy codec parameters to encoder context");

	// avcodec_parameters_to_context() does not set time_base, but
	// acvodec_open2() requires it.
	occ->time_base = os->time_base;

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

	while (true) {
		rc = av_read_frame(ifc, &ipacket);
		if (rc == AVERROR_EOF)
			break; /// \todo Draining
		else if (rc != 0)
			error(EXIT_FAILURE, 0, "Failed to read next packet: %d", rc);

		if (ipacket.stream_index == video_stream_number) {
			rc = avcodec_send_packet(icc, &ipacket);
			if (rc)
				error(EXIT_FAILURE, 0, "Failed to send packet to decoder");

			while ((rc = avcodec_receive_frame(icc, iframe)) == 0) {
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

			if (rc != 0 && rc != AVERROR(EAGAIN) && rc != AVERROR_EOF)
				error(EXIT_FAILURE, 0, "Failed to read decoded frame");
		}

		// Unref data that was allocated by av_read_frame()
		av_packet_unref(&ipacket);
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
