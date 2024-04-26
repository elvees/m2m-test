/*
 * Tool to decode video from H.264 using V4L2 device and output to file, file
 * descriptor or display on screen.
 *
 * Copyright 2019 RnD Center "ELVEES", JSC
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
#include <pthread.h>

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
#include <libswresample/swresample.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

#include "drmdisplay.h"
#include "log.h"
#include "v4l2-utils.h"

#define ROUND_DOWN(x, a) ((x) & ~((a)-1))

#define NUM_BUFS 4

#define DEFAULT_WIDTH 1280
#define DEFAULT_HEIGHT 720

/* Some evident defines */
#define MSEC_IN_SEC 1000
#define USEC_IN_SEC 1000000
#define NSEC_IN_SEC 1000000000

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000

/* TODO: It seems that video queue size of 128 MiB is too high */
#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)
#define MAX_VIDEOQ_SIZE (128 * 4 * 256 * 1024)

/* no AV correction is done if too big error */
#define AV_NOSYNC_THRESHOLD 10.0
#define AV_SYNC_THRESHOLD 0.01

enum nalu_type {
	NALU_SLICE = 1,
	NALU_DPA,
	NALU_DPB,
	NALU_DPC,
	NALU_IDR,
	NALU_SEI,
	NALU_SPS,
	NALU_PPS,
	NALU_AUD,
	NALU_EOSEQ,
	NALU_EOSTREAM,
	NALU_FILL,
	NALU_NONE = 32
};

enum nalu_priority {
	NALU_PRIOR_DISPOSABLE,
	NALU_PRIOR_LOW,
	NALU_PRIOR_HIGH,
	NALU_PRIOR_HIGHEST,
	NALU_PRIOR_NONE
};

typedef struct packet_queue {
	AVPacketList *first_pkt, *last_pkt;
	int nb_packets;
	int size;
	int abort_request;
	SDL_mutex *mutex;
	SDL_cond *cond;
} packet_queue;

struct video_data {
	int width;
	int height;
	uint64_t size;
	uint64_t frames;
};

struct sink_data {
	pthread_t thread;

	int outfd;
	bool m420_to_i420;

	struct video_data vd;

	struct SwsContext *dsc;
	uint8_t *fb_addr;
	AVFrame *fb_frame;

	struct drmdisplay drm;
	struct drmdisplay_dumb dumb;
};

struct src_data {
	pthread_t thread;

	AVBSFContext *bsf;

	struct video_data vd;
};

static struct video_state {
	AVFormatContext *ic;
	unsigned int frames;
	unsigned int loops;
	int quit;
	int m2mfd;

	int audio_stream_number;
	SDL_AudioDeviceID audio_dev;
	double audio_clock;
	AVStream *audio_st;
	AVCodecContext *audio_ctx;
	packet_queue audioq;
	unsigned int audio_buf_size;
	unsigned int audio_buf_index;
	struct SwrContext *swr_ctx;

	int video_stream_number;
	AVStream *video_st;
	packet_queue videoq;
	/* pts of last decoded frame / predicted pts of next decoded frame */
	double video_clock;
	double frame_timer;

	struct src_data src;
	struct sink_data sink;
} video_state;

static AVPacket flush_pkt;

static struct m2m_buffer {
	struct v4l2_buffer v4l2;
	void *buf;
	AVFrame *frame;
} out_bufs[NUM_BUFS], cap_bufs[NUM_BUFS];

static void init_video_state(struct video_state *vs)
{
	vs->frame_timer = av_gettime_relative() / 1000000.0;

	vs->loops = 1;
	vs->frames = 0;
	vs->video_stream_number = -1;
	vs->audio_stream_number = -1;

	vs->sink.outfd = -1;
}

static inline bool is_valid_cap_buf(unsigned const bufn)
{
	return bufn < NUM_BUFS && cap_bufs[bufn].buf;
}

static inline bool is_valid_out_buf(unsigned const bufn)
{
	return bufn < NUM_BUFS && out_bufs[bufn].buf;
}

static void packet_queue_init(packet_queue *q)
{
	memset(q, 0, sizeof(packet_queue));
	q->mutex = SDL_CreateMutex();
	q->cond = SDL_CreateCond();
}

static void packet_queue_abort(packet_queue *q)
{
	SDL_LockMutex(q->mutex);

	q->abort_request = 1;

	SDL_CondSignal(q->cond);

	SDL_UnlockMutex(q->mutex);
}

static int packet_queue_get(packet_queue *q, AVPacket *pkt, int block)
{
	AVPacketList *pkt1;
	int ret;

	SDL_LockMutex(q->mutex);

	for (;;) {
		if (q->abort_request) {
			ret = -1;
			break;
		}
		pkt1 = q->first_pkt;
		if (pkt1) {
			q->first_pkt = pkt1->next;
			if (!q->first_pkt)
				q->last_pkt = NULL;
			q->nb_packets--;
			q->size -= pkt1->pkt.size;
			*pkt = pkt1->pkt;
			av_free(pkt1);
			ret = 1;
			break;
		} else if (!block) {
			ret = 0;
			break;
		} else {
			SDL_CondWait(q->cond, q->mutex);
		}
	}

	SDL_UnlockMutex(q->mutex);

	return ret;
}

static int packet_queue_put(packet_queue *q, AVPacket *pkt)
{
	AVPacketList *pkt1 = av_malloc(sizeof(AVPacketList));

	if (!pkt1)
		return -1;

	pkt1->pkt = *pkt;
	pkt1->next = NULL;

	SDL_LockMutex(q->mutex);

	if (q->abort_request)
		return -1;

	if (!q->last_pkt)
		q->first_pkt = pkt1;
	else
		q->last_pkt->next = pkt1;
	q->last_pkt = pkt1;
	q->nb_packets++;
	q->size += pkt1->pkt.size;
	SDL_CondSignal(q->cond);

	SDL_UnlockMutex(q->mutex);

	return 0;
}

static void configure_yuv2rgb(struct sink_data *sink)
{
	int ret;

	if (sink->vd.width > sink->fb_frame->width)
		error(EXIT_FAILURE, 0, "Width of decoded frame is greater than framebuffer's");

	if (sink->vd.height > sink->fb_frame->height)
		error(EXIT_FAILURE, 0, "Height of decoded frame is greater than framebuffer's");

	ret = av_image_fill_arrays(sink->fb_frame->data, sink->fb_frame->linesize, sink->fb_addr,
			sink->fb_frame->format, sink->fb_frame->width, sink->fb_frame->height, 1);
	if (ret < 0)
		error(EXIT_FAILURE, 0, "Error image fill destination frame arrays");

	if (sink->dsc)
		sws_freeContext(sink->dsc);

	/* Set the same resolution as in video because scaling isn't supported */
	sink->dsc = sws_getContext(sink->vd.width, sink->vd.height, AV_PIX_FMT_M420,
			sink->vd.width, sink->vd.height, sink->fb_frame->format,
			SWS_BILINEAR, NULL, NULL, NULL);
	if (!sink->dsc)
		error(EXIT_FAILURE, 0, "Can't allocate swscale context");
}

static void configure_capture_frames(int width, int height)
{
	int i;

	for (i = 0; is_valid_cap_buf(i); i++) {
		AVFrame *frame;
		int rc;

		if (!cap_bufs[i].frame) {
			cap_bufs[i].frame = av_frame_alloc();
			if (!cap_bufs[i].frame) error(EXIT_FAILURE, 0, "Not enough memory");
		}

		frame = cap_bufs[i].frame;

		frame->format = AV_PIX_FMT_M420;
		frame->width = width;
		frame->height = height;

		rc = av_image_fill_arrays(frame->data, frame->linesize, cap_bufs[i].buf,
				frame->format, frame->width, frame->height, 16);
		if (rc < 0)
			error(EXIT_FAILURE, 0, "Error image fill capture arrays");
	}
}

static void queue_outbuf(struct video_state *vs, unsigned const index, AVPacket *pkt)
{
	AVRational v4l2_timebase = { 1, USEC_IN_SEC };
	int64_t v4l2_pts = av_rescale_q(pkt->pts, vs->video_st->time_base, v4l2_timebase);

	out_bufs[index].v4l2.timestamp.tv_usec = v4l2_pts % USEC_IN_SEC;
	out_bufs[index].v4l2.timestamp.tv_sec = v4l2_pts / USEC_IN_SEC;
	out_bufs[index].v4l2.bytesused = pkt->size;
	out_bufs[index].v4l2.flags = (pkt->flags & AV_PKT_FLAG_KEY ? V4L2_BUF_FLAG_KEYFRAME : 0);
	v4l2_qbuf(vs->m2mfd, &out_bufs[index].v4l2);
}

static void dequeue_outbuf(int const fd, unsigned const index)
{
	v4l2_dqbuf(fd, &out_bufs[index].v4l2);
	if (index != out_bufs[index].v4l2.index)
		error(EXIT_FAILURE, 0, "Error index of buffer.");
}

static int m420_to_i420_write(const void *const buf, unsigned int const linesize,
		unsigned int const height, int const outfd) {
	size_t const size = linesize * height * 3 / 2;
	uint8_t *const temp = malloc(size);
	const uint8_t *const m420 = (const uint8_t *const)buf;
	int rc;
	size_t i, j, k;

	if (!temp) error(EXIT_FAILURE, 0, "Can not allocate memory for convertion buffer");

	/* Luma */
	for (i = 0, j = 0; i < height; i += 2, j += 3)
		memcpy(temp + i * linesize, m420 + j * linesize, 2 * linesize);

	/* Chroma */
	for (i = 0, j = 2; i < height / 2; i++, j += 3) {
		const uint8_t *const inp = &m420[j * linesize];
		uint8_t *const incb = temp + height * linesize + i * linesize / 2;
		uint8_t *const incr = temp + height * linesize + height / 4 * linesize + i * linesize / 2;

		for (k = 0; k < linesize / 2; k++) {
			incb[k] = inp[2 * k];
			incr[k] = inp[2 * k + 1];
		}
	}

	rc = write(outfd, temp, size);

	free(temp);

	return rc;
}

static void process_capbuf(struct sink_data *sink, int capn, size_t size)
{
	int rc = 0;

	if (sink->outfd >= 0) {
		if (sink->m420_to_i420)
			rc = m420_to_i420_write(cap_bufs[capn].buf, sink->vd.width,
						sink->vd.height, sink->outfd);
		else
			rc = write(sink->outfd, cap_bufs[capn].buf, size);
		if (rc < 0)
			error(EXIT_FAILURE, errno, "Can not write to output");
	}

	if (sink->fb_addr) {
		AVFrame *src_frame = cap_bufs[capn].frame;
		AVFrame *dst_frame = sink->fb_frame;

		sws_scale(sink->dsc, (uint8_t const * const*)src_frame->data,
			src_frame->linesize, 0, src_frame->height,
			dst_frame->data, dst_frame->linesize);
	}
}

static void start_capture(struct video_state *vs)
{
	struct sink_data *sink = &vs->sink;
	struct v4l2_format f_dst = {
		.fmt = {
			.pix = {
				.width = sink->vd.width,
				.height = sink->vd.height,
				.pixelformat = V4L2_PIX_FMT_M420,
				.field = V4L2_FIELD_ANY,
				.bytesperline = sink->vd.width
			}
		}
	};
	struct v4l2_requestbuffers capreqbuf = {
		.count = NUM_BUFS,
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_MMAP
	};
	int ret;

	v4l2_setformat(vs->m2mfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, &f_dst);
	v4l2_pix_fmt_validate(&f_dst.fmt.pix, V4L2_PIX_FMT_M420, sink->vd.width,
			      sink->vd.height, sink->vd.width);

	ret = ioctl(vs->m2mfd, VIDIOC_REQBUFS, &capreqbuf);
	if (ret != 0) error(EXIT_FAILURE, errno, "Can not request capture buffers");
	if (capreqbuf.count == 0) error(EXIT_FAILURE, 0, "Device gives zero capture buffers");
	pr_debug("M2M: Got %d capture buffers", capreqbuf.count);

	for (int i = 0; i < capreqbuf.count; ++i) {
		struct v4l2_buffer *vbuf = &cap_bufs[i].v4l2;

		memset(vbuf, 0, sizeof(*vbuf));
		vbuf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		vbuf->memory = V4L2_MEMORY_MMAP;
		vbuf->index = i;
		vbuf->flags = 0;

		ret = ioctl(vs->m2mfd, VIDIOC_QUERYBUF, vbuf);
		if (ret != 0) error(EXIT_FAILURE, errno, "Can not query capture buffer");
		pr_debug("M2M: Got capture buffer #%u: length = %u", i, vbuf->length);

		cap_bufs[i].buf = mmap(NULL, vbuf->length, PROT_READ | PROT_WRITE, MAP_SHARED,
				       vs->m2mfd, vbuf->m.offset);
		if (cap_bufs[i].buf == MAP_FAILED) error(EXIT_FAILURE, errno, "Can not mmap capture buffer");
	}

	pr_verb("Allocating AVFrames for obtained buffers...");
	configure_capture_frames(sink->vd.width, sink->vd.height);

	for (int i = 0; i < capreqbuf.count; ++i) {
		struct v4l2_buffer buf = {
			.index = i,
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
			.memory = V4L2_MEMORY_MMAP
		};

		v4l2_qbuf(vs->m2mfd, &buf);
	}

	v4l2_streamon(vs->m2mfd, V4L2_BUF_TYPE_VIDEO_CAPTURE);
}

static void stop_capture(struct video_state *vs)
{
	struct v4l2_requestbuffers req = {
		.memory = V4L2_MEMORY_MMAP,
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.count = 0, /* 0 -> unmaps buffers from the driver */
	};
	int i, rc;

	v4l2_streamoff(vs->m2mfd, V4L2_BUF_TYPE_VIDEO_CAPTURE);

	for (i = 0; is_valid_cap_buf(i); ++i) {
		struct v4l2_buffer *vbuf = &cap_bufs[i].v4l2;

		if (munmap(cap_bufs[i].buf, vbuf->length) < 0)
			error(EXIT_FAILURE, errno, "Can not unmap capture buffer");
		cap_bufs[i].buf = NULL;
		av_frame_free(&cap_bufs[i].frame);
		cap_bufs[i].frame = NULL;
		memset(&cap_bufs[i].v4l2, 0, sizeof(cap_bufs[i].v4l2));
	}

	rc = ioctl(vs->m2mfd, VIDIOC_REQBUFS, &req);
	if (rc != 0) error(EXIT_FAILURE, errno, "Can not request capture buffers");
}

static void start_output(struct video_state *vs)
{
	struct src_data *src = &vs->src;
	struct v4l2_format f_src = {
		.fmt = {
			.pix = {
				.width = src->vd.width,
				.height = src->vd.height,
				.pixelformat = V4L2_PIX_FMT_H264,
				.field = V4L2_FIELD_ANY
			}
		}
	};
	struct v4l2_requestbuffers outreqbuf = {
		.count = NUM_BUFS,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
		.memory = V4L2_MEMORY_MMAP
	};
	int rc;

	v4l2_setformat(vs->m2mfd, V4L2_BUF_TYPE_VIDEO_OUTPUT, &f_src);
	v4l2_pix_fmt_validate(&f_src.fmt.pix, V4L2_PIX_FMT_H264, src->vd.width, src->vd.height, 0);

	rc = ioctl(vs->m2mfd, VIDIOC_REQBUFS, &outreqbuf);
	if (rc != 0) error(EXIT_FAILURE, errno, "Can not request output buffers");
	if (outreqbuf.count == 0) error(EXIT_FAILURE, 0, "Device gives zero output buffers");
	pr_debug("M2M: Got %d output buffers", outreqbuf.count);

	for (int i = 0; i < outreqbuf.count; ++i) {
		struct v4l2_buffer *vbuf = &out_bufs[i].v4l2;

		memset(vbuf, 0, sizeof(*vbuf));
		vbuf->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		vbuf->memory = V4L2_MEMORY_MMAP;
		vbuf->index = i;
		vbuf->flags = 0;

		rc = ioctl(vs->m2mfd, VIDIOC_QUERYBUF, vbuf);
		if (rc != 0) error(EXIT_FAILURE, errno, "Can not query output buffer");
		pr_debug("M2M: Got output buffer #%u: length = %u", i, vbuf->length);

		out_bufs[i].buf = mmap(NULL, vbuf->length, PROT_READ | PROT_WRITE, MAP_SHARED,
				       vs->m2mfd, vbuf->m.offset);
		if (out_bufs[i].buf == MAP_FAILED) error(EXIT_FAILURE, errno, "Can not mmap output buffer");
	}

	v4l2_streamon(vs->m2mfd, V4L2_BUF_TYPE_VIDEO_OUTPUT);
}

static bool handle_event(struct video_state *vs)
{
	struct v4l2_event evt = { 0 };

	if (ioctl(vs->m2mfd, VIDIOC_DQEVENT, &evt) < 0)
		error(EXIT_FAILURE, errno, "VIDIOC_DQEVENT error");

	if (evt.type == V4L2_EVENT_SOURCE_CHANGE) {
		pr_debug("Got event source change");

		return true;
	} else {
		pr_warn("Got unexpected V4L2 event");
	}

	return false;
}

static double get_audio_clock(struct video_state *vs)
{
	double pts = vs->audio_clock; /* maintained in the audio thread */
	int hw_buf_size = vs->audio_buf_size - vs->audio_buf_index;
	int bytes_per_sec = 0;
	int n = vs->audio_ctx->channels * av_get_bytes_per_sample(vs->audio_ctx->sample_fmt);

	if (vs->audio_st)
		bytes_per_sec = vs->audio_ctx->sample_rate * n;
	if (bytes_per_sec != 0)
		pts -= (double)hw_buf_size / bytes_per_sec;

	return pts;
}

static double synchronize_video(struct video_state *vs, double pts)
{
	if (pts != 0) /* if we have pts, set video clock to it */
		vs->video_clock = pts;
	else /* if we aren't given a pts, set it to the clock */
		pts = vs->video_clock;

	return pts;
}

/*
 * Limitations: The next parts work synchronously and can influence
 * each other and overall test performance:
 * - conversion from M420 to BGRA
 * - m420_to_i420_write()
 */
static void *start_video_sink(void *arg)
{
	struct video_state *vs = (struct video_state *)arg;
	struct sink_data *sink = &vs->sink;
	struct pollfd fds[1] = {
		{ vs->m2mfd, POLLIN }
	};
	double frame_last_pts = 0;
	double frame_last_delay = 10e-3;
	int ret = 0;

	while (1) {
		ret = poll(fds, 1, MSEC_IN_SEC);
		if (ret < 0)
			error(EXIT_FAILURE, errno, "Poll error");
		if (ret == 0) {
			pr_info("decoded and presented frames: %llu", sink->vd.frames);
			error(EXIT_FAILURE, 0, "Timeout waiting for capture buffer...");
		}

		if (fds[0].revents & POLLIN) {
			struct v4l2_buffer buf = {
				.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
				.memory = V4L2_MEMORY_MMAP
			};

			if (ioctl(vs->m2mfd, VIDIOC_DQBUF, &buf)) {
				if (errno != EPIPE)
					error(EXIT_FAILURE, errno, "VIDIOC_DQBUF capture buf");
				/* The last capture buffer has been already dequeued */
				break;
			}

			/*
			 * The last buffer may be empty (with v4l2_buffer bytesused = 0)
			 * and in that case it must be ignored by the client, as it does
			 * not contain a decoded frame.
			 */
			if (buf.bytesused != 0) {
				int64_t v4l2_pts = (int64_t)buf.timestamp.tv_sec * USEC_IN_SEC +
						   buf.timestamp.tv_usec;
				double pts = synchronize_video(vs, v4l2_pts / (double)USEC_IN_SEC);
				double delay = pts - frame_last_pts;
				double actual_delay;

				/* if incorrect delay, use previous one */
				if (delay <= 0 || delay >= 1.0)
					delay = frame_last_delay;

				/* save for next time */
				frame_last_delay = delay;
				frame_last_pts = pts;

				if (vs->audio_stream_number != -1) {
					double diff = pts - get_audio_clock(vs);
					/* Skip or repeat the frame. Take delay into account" */
					double sync_threshold = (delay > AV_SYNC_THRESHOLD) ?
								delay : AV_SYNC_THRESHOLD;

					if (fabs(diff) < AV_NOSYNC_THRESHOLD) {
						if (diff <= -sync_threshold) /* speed up */
							delay = 0;
						else if (diff >= sync_threshold) /* slow down */
							delay = 2 * delay;
					}
				}
				vs->frame_timer += delay;
				/* compute the REAL delay */
				actual_delay = vs->frame_timer - (av_gettime_relative() / 1000000.0);

				/*
				 * If delay is too small don't sleep, display frame now.
				 * TODO: Add ability to skip frame displaying.
				 */
				if (actual_delay > 0.010)
					av_usleep(actual_delay * 1000000.0 + 0.5);
				process_capbuf(sink, buf.index, buf.bytesused);

				pr_verb("Frame %u is decoded and processed", sink->vd.frames);
				sink->vd.size += buf.bytesused;
				sink->vd.frames += 1;
			}

			if (!(buf.flags & V4L2_BUF_FLAG_LAST)) {
				buf.flags = 0;
				buf.bytesused = 0;
				v4l2_qbuf(vs->m2mfd, &buf);
			} else {
				break;
			}
		}
	}

	return NULL;
}

static int filter_mp4toannexb(struct video_state *vs, AVPacket *pkt)
{
	if (vs->src.bsf) {
		int ret = av_bsf_send_packet(vs->src.bsf, pkt);
		if (ret < 0)
			error(EXIT_FAILURE, 0, "h264_mp4toannexb filter failed to send input packet");

		while (!ret)
			ret = av_bsf_receive_packet(vs->src.bsf, pkt);

		if (ret < 0 && (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF))
			error(EXIT_FAILURE, 0, "h264_mp4toannexb filter failed to receive output packet");
	}

	return 0;
}

static bool try_send_access_unit(struct video_state *vs, unsigned const outn)
{
	static AVPacket pkt;

	int ret;

	if (out_bufs[outn].v4l2.flags & V4L2_BUF_FLAG_QUEUED) {
		struct pollfd fds[1] = {
			{ vs->m2mfd, POLLOUT | POLLPRI }
		};

		while (1) {
			ret = poll(fds, 1, MSEC_IN_SEC);
			if (ret < 0)
				error(EXIT_FAILURE, errno, "Poll error");
			if (ret == 0)
				error(EXIT_FAILURE, 0, "Timeout waiting for event/output buffer...");

			if (fds[0].revents & POLLPRI) {
				if (handle_event(vs)) {
					struct v4l2_format f_dst = { 0 };

					/* Wait for capture draining before STREAM_OFF */
					ret = pthread_join(vs->sink.thread, NULL);
					if (ret != 0)
						error(EXIT_FAILURE, errno, "Can not join thread");

					stop_capture(vs);
					v4l2_getformat(vs->m2mfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, &f_dst);
					vs->sink.vd.width = f_dst.fmt.pix.width;
					vs->sink.vd.height = f_dst.fmt.pix.height;

					if (vs->sink.fb_addr)
						configure_yuv2rgb(&vs->sink);

					start_capture(vs);
					/* TODO: reallocate output buffers */

					ret = pthread_create(&vs->sink.thread, NULL, start_video_sink, vs);
					if (ret != 0)
						error(EXIT_FAILURE, errno, "Can not create thread");
				}
			}

			if (fds[0].revents & POLLOUT) {
				dequeue_outbuf(vs->m2mfd, outn);
				break;
			}
		}
	}

	ret = packet_queue_get(&vs->videoq, &pkt, 1);
	if (ret < 0 || pkt.data == flush_pkt.data)
		return false;

	filter_mp4toannexb(vs, &pkt);

	if (pkt.size > out_bufs[outn].v4l2.length)
		error(EXIT_FAILURE, 0, "Insufficient size of output buffer");
	memcpy(out_bufs[outn].buf, pkt.data, pkt.size);
	if (pkt.size == 0)
		pr_warn("Got empty encoded frame");

	queue_outbuf(vs, outn, &pkt);
	av_packet_unref(&pkt);

	return true;
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

static inline float timespec2float(struct timespec const t)
{
	return t.tv_sec + (float)t.tv_nsec / 1e9;
}

static inline bool checklimit(unsigned const value, unsigned const limit)
{
	return limit == 0 || value < limit;
}

static int v4l2_stop_decode(int fd)
{
	struct v4l2_decoder_cmd cmd = {
		.cmd = V4L2_DEC_CMD_STOP,
		.flags = 0,
	};
	int ret;

	ret = ioctl(fd, VIDIOC_DECODER_CMD, &cmd);
	if (ret) {
		/* DECODER_CMD is optional */
		if (errno == ENOTTY) {
			enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

			return ioctl(fd, VIDIOC_STREAMOFF, &type);
		}
	}

	return 0;
}

#ifndef VERSION
#define VERSION "unversioned"
#endif

static void help(const char *program_name)
{
	puts("dec-vpout " VERSION " \n");
	printf("Synopsys: %s [options] decode-device\n", program_name);
	puts("Options:");
	puts("    -a arg    Name of the audio device");
	puts("    -d        Display video on screen");
	puts("    -f arg    Output file descriptor number");
	puts("    -h arg    display this help and exit");
	puts("    -i arg    Input file name (defaults to stdin)");
	puts("    -l arg    Loop over input file (-1 means infinitely)");
	puts("    -m arg    Input video is in MP4 format");
	puts("    -n arg    Specify how many frames should be processed");
	puts("    -o arg    Output file name (takes precedence over -f)");
	printf("    -s arg    Set desired screen resolution [defaults to %dx%d]\n",
	       DEFAULT_WIDTH, DEFAULT_HEIGHT);
	puts("    -t        Transform video to I420 before output");
	puts("    -v        Be more verbose. Can be specified multiple times");
}

static int audio_decode_frame(struct video_state *vs, uint8_t *buf, int buf_size, double *pts)
{
	static AVPacket pkt;
	static uint8_t *audio_pkt_data;
	static int audio_pkt_size;
	static AVFrame audio_frame;
	static uint8_t converted_data[(192000 * 3) / 2];

	for (;;) {
		while (audio_pkt_size > 0) {
			int len1, len2, got_frame = 0, data_size = 0, bytes_per_sec;

			len1 = avcodec_decode_audio4(vs->audio_ctx, &audio_frame, &got_frame, &pkt);
			/* if error, skip frame */
			if (len1 < 0) {
				audio_pkt_size = 0;
				break;
			}
			audio_pkt_data += len1;
			audio_pkt_size -= len1;
			if (got_frame) {
				uint8_t *converted = converted_data;

				data_size = av_samples_get_buffer_size(NULL, vs->audio_ctx->channels,
								       audio_frame.nb_samples,
								       AV_SAMPLE_FMT_FLT, 1);
				if (data_size < 0)
					error(EXIT_FAILURE, 0,
					      "Can not get buffer size for audio parameters");
				if (data_size > buf_size)
					error(EXIT_FAILURE, 0, "Insufficient buffer size");
				len2 = swr_convert(vs->swr_ctx, &converted, audio_frame.nb_samples,
						   (const uint8_t**)&audio_frame.data[0],
						   audio_frame.nb_samples);
				if (len2 < 0)
					error(EXIT_FAILURE, 0, "Can not convert audio");
				memcpy(buf, converted_data, data_size);
			}
			if (data_size == 0)
				continue; /* No data yet, get more frames */
			*pts = vs->audio_clock;
			bytes_per_sec = av_get_bytes_per_sample(vs->audio_ctx->sample_fmt) *
					vs->audio_ctx->channels * vs->audio_ctx->sample_rate;
			vs->audio_clock += (double)data_size / bytes_per_sec;

			/* We have data, return it and come back for more later */
			return data_size;
		}
		if (pkt.data)
			av_packet_unref(&pkt);

		if (vs->quit)
			return -1;

		/* next packet */
		if (packet_queue_get(&vs->audioq, &pkt, 1) < 0)
			return -1;

		audio_pkt_data = pkt.data;
		audio_pkt_size = pkt.size;
		/* update the audio clock */
		if (pkt.pts != AV_NOPTS_VALUE)
			vs->audio_clock = av_q2d(vs->audio_st->time_base) * pkt.pts;
	}

	error(EXIT_FAILURE, errno, "This code must be unreachable");

	return -1;
}

static void audio_callback(void *userdata, uint8_t *stream, int len)
{
	static uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];

	struct video_state *vs = (struct video_state *)userdata;
	int len1, audio_size;
	double pts;

	while (len > 0) {
		if (vs->audio_buf_index >= vs->audio_buf_size) {
			/* We have already sent all our data; get more */
			audio_size = audio_decode_frame(vs, audio_buf, sizeof(audio_buf), &pts);
			/* If error, output silence */
			if (audio_size < 0) {
				vs->audio_buf_size = SDL_AUDIO_BUFFER_SIZE;
				memset(audio_buf, 0, vs->audio_buf_size);
			} else {
				vs->audio_buf_size = audio_size;
			}
			vs->audio_buf_index = 0;
		}
		len1 = vs->audio_buf_size - vs->audio_buf_index;
		if (len1 > len)
			len1 = len;
		memcpy(stream, (uint8_t *)(audio_buf + vs->audio_buf_index), len1);
		len -= len1;
		stream += len1;
		vs->audio_buf_index += len1;
	}
}

static void *start_video_src(void *arg)
{
	struct video_state *vs = (struct video_state *)arg;
	unsigned int outn = 0;

	for (;;) {
		if (!try_send_access_unit(vs, outn))
			break;

		if (!is_valid_out_buf(++outn))
			outn = 0;
	}

	return NULL;
}

static int is_realtime(AVFormatContext *s)
{
	if(!strcmp(s->iformat->name, "rtp") ||
	   !strcmp(s->iformat->name, "rtsp") ||
	   !strcmp(s->iformat->name, "sdp"))
		return 1;

	if(s->pb && (!strncmp(s->filename, "rtp:", 4) ||
		     !strncmp(s->filename, "udp:", 4)))
		return 1;

	return 0;
}

static void read_av(struct video_state *vs)
{
	unsigned int loop = 0;
	AVPacket pkt1, *pkt = &pkt1;
	int ret;

	while (checklimit(loop, vs->loops) && checklimit(vs->src.vd.frames, vs->frames)) {
		pr_verb("Loop #%u", loop);

		if (vs->audioq.size > MAX_AUDIOQ_SIZE || vs->videoq.size > MAX_VIDEOQ_SIZE) {
			SDL_Delay(10);
			continue;
		}
		if ((ret = av_read_frame(vs->ic, pkt)) < 0) {
			if (ret == AVERROR_EOF || avio_feof(vs->ic->pb)) {
				/* if it is streaming don't restart video */
				if (is_realtime(vs->ic) || !checklimit(++loop, vs->loops))
					break;
				if (strcmp("h264", vs->ic->iformat->name) == 0)
					ret = avformat_seek_file(vs->ic, vs->video_stream_number,
								 0, 0, 0, AVSEEK_FLAG_BYTE);
				else
					ret = avformat_seek_file(vs->ic, -1, INT64_MIN,
								 vs->ic->start_time, INT64_MAX, 0);
				if (ret < 0)
					error(EXIT_FAILURE, 0, "Can not rewind input file: %d", ret);
				continue;
			}

			if (vs->ic->pb && vs->ic->pb->error)
				break;
		}
		if (pkt->stream_index == vs->video_stream_number) {
			packet_queue_put(&vs->videoq, pkt);
			vs->src.vd.frames++;
		} else if (pkt->stream_index == vs->audio_stream_number) {
			packet_queue_put(&vs->audioq, pkt);
		} else {
			av_packet_unref(pkt);
		}
	}
}

static int init_audio(struct video_state *vs, char const *audio_device)
{
	int ret;
	AVCodecContext *acc;
	SDL_AudioSpec wanted_spec = { 0 }, spec;
	AVCodecContext *acc_orig = vs->ic->streams[vs->audio_stream_number]->codec;
	AVCodec *acodec = avcodec_find_decoder(acc_orig->codec_id);
	uint64_t layout;

	if (!acodec)
		error(EXIT_FAILURE, 0, "Unsupported audio codec");

	acc = avcodec_alloc_context3(acodec);
	if (!acc)
		error(EXIT_FAILURE, 0, "Couldn't allocate audio codec context");

	if (avcodec_copy_context(acc, acc_orig) != 0)
		error(EXIT_FAILURE, 0, "Couldn't copy audio codec context");

	if (avcodec_open2(acc, acodec, NULL))
		error(EXIT_FAILURE, 0, "Could not open codec");

	vs->swr_ctx = swr_alloc();
	if (!vs->swr_ctx)
		error(EXIT_FAILURE, 0, "Could not allocate resampler context");

	layout = av_get_default_channel_layout(acc->channels);
	av_opt_set_channel_layout(vs->swr_ctx, "in_channel_layout", layout, 0);
	av_opt_set_int(vs->swr_ctx, "in_sample_rate", acc->sample_rate, 0);
	av_opt_set_sample_fmt(vs->swr_ctx, "in_sample_fmt", acc->sample_fmt, 0);

	av_opt_set_channel_layout(vs->swr_ctx, "out_channel_layout", layout, 0);
	av_opt_set_int(vs->swr_ctx, "out_sample_rate", acc->sample_rate, 0);
	av_opt_set_sample_fmt(vs->swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);

	ret = swr_init(vs->swr_ctx);
	if (ret < 0)
		error(EXIT_FAILURE, 0, "Failed to initialize the resampling context");

	/* Set audio settings from codec info */
	wanted_spec.freq = acc->sample_rate;
	wanted_spec.format = AUDIO_F32;
	wanted_spec.channels = acc->channels;
	wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
	wanted_spec.callback = audio_callback;
	wanted_spec.userdata = vs;

	vs->audio_dev = SDL_OpenAudioDevice(audio_device, 0, &wanted_spec, &spec, 0);
	if (vs->audio_dev == 0) {
		int i, count = SDL_GetNumAudioDevices(0);

		if (!audio_device)
			pr_err("Failed to open default audio device");
		else
			pr_err("Failed to open audio device \"%s\"", audio_device);

		if (count > 0) {
			pr_err("Available audio devices:");
			for (i = 0; i < count; ++i)
				pr_err("  Audio device %d: \"%s\"", i, SDL_GetAudioDeviceName(i, 0));
		}

		swr_free(&vs->swr_ctx);
		avcodec_close(acc);
		avcodec_free_context(&acc);

		return -1;
	}

	vs->audio_st = vs->ic->streams[vs->audio_stream_number];
	vs->audio_ctx = acc;
	vs->audio_buf_size = 0;
	vs->audio_buf_index = 0;

	packet_queue_init(&vs->audioq);

	SDL_PauseAudioDevice(vs->audio_dev, 0); /* start audio playing. */

	return 0;
}

int main(int argc, char *argv[])
{
	struct timespec loopstart, loopstop, looptime = { 0 };
	char const *input = "pipe:", *audio_device = NULL, *output = NULL, *device = NULL;
	int ret, opt;
	const char *optstring = "a:df:i:l:mn:o:s:tvh";
	bool display = false, is_mp4 = false;
	int fb_width = 0, fb_height = 0;
	struct video_state *vs = &video_state;
	struct sink_data *sink = &vs->sink;
	struct drmdisplay *drm = &vs->sink.drm;
	struct src_data *src = &vs->src;
	AVDictionary *opts = NULL;

	av_register_all();
	avformat_network_init();

	init_video_state(vs);

	av_init_packet(&flush_pkt);
	flush_pkt.data = (uint8_t *)&flush_pkt;

	if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER))
		error(EXIT_FAILURE, 0, "SDL_Init failed: %s", SDL_GetError());

	while ((opt = getopt(argc, argv, optstring)) != -1) {
		switch (opt) {
			case 'a': audio_device = optarg; break;
			case 'd': display = true; break;
			case 'f': sink->outfd = atoi(optarg); break;
			case 'h': help(argv[0]); return EXIT_SUCCESS;
			case 'i': input = optarg; break;
			case 'l': vs->loops = atoi(optarg); break;
			case 'm': is_mp4 = true; break;
			case 'n': vs->frames = atoi(optarg); break;
			case 'o': output = optarg; break;
			case 's': {
				char *endptr;

				display = true;

				fb_width = strtol(optarg, &endptr, 10);
				if (*endptr != 'x')
					error(EXIT_FAILURE, 0, "Malformed argument: %s", optarg);

				fb_height = strtol(endptr + 1, &endptr, 10);
				if (*endptr != '\0')
					error(EXIT_FAILURE, 0, "Malformed argument: %s", optarg);

				break;
			}
			case 't': sink->m420_to_i420 = true; break;
			case 'v': vlevel++; break;
			default: error(EXIT_FAILURE, 0, "Try %s -h for help.", argv[0]);
		}
	}

	if (argc < optind + 1)
		error(EXIT_FAILURE, 0, "Not enough arguments");
	device = argv[optind];

	char card[32];
	vs->m2mfd = v4l2_open(device, V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING, 0, card);
	pr_info("Card: %.32s", card);

	if (display) {
		int bpp = 32;

		if (drmdisplay_init(drm, DRMDISPLAY_AUTO_CONNID, vlevel == LOG_VERBOSE))
			error(EXIT_FAILURE, errno, "DRM initialize failed");

		if (drmdisplay_fill_mode(drm, &fb_width, &fb_height))
			error(EXIT_FAILURE, 0, "No suitable display modes found\n");

		drm->cur_mode = drmdisplay_get_mode(drm, fb_width, fb_height);
		if (!drm->cur_mode)
			error(EXIT_FAILURE, 0, "Resolution %dx%d is not supported by display",
			      fb_width, fb_height);

		/* Assume the same video resolution */
		src->vd.width = sink->vd.width = ROUND_DOWN(fb_width, 16);
		src->vd.height = sink->vd.height = ROUND_DOWN(fb_height, 16);

		drmdisplay_create_mmap_dumb(drm->fd, fb_width, fb_height, bpp, &sink->dumb);

		drmdisplay_set_mode(drm, drm->cur_mode, sink->dumb.fb_id, vlevel == LOG_INFO);

		sink->fb_frame = av_frame_alloc();
		if (!sink->fb_frame)
			error(EXIT_FAILURE, 0, "Cannot allocate memory for AVFrame");

		/* TODO: check format of framebuffer */
		sink->fb_frame->format = AV_PIX_FMT_BGRA;
		sink->fb_frame->width = fb_width;
		sink->fb_frame->height = fb_height;
		sink->fb_addr = sink->dumb.addr;

		configure_yuv2rgb(sink);
	} else {
		src->vd.width = sink->vd.width = DEFAULT_WIDTH;
		src->vd.height = sink->vd.height = DEFAULT_HEIGHT;
	}

	v4l2_subscribe_event(vs->m2mfd, V4L2_EVENT_SOURCE_CHANGE);

	start_capture(vs);
	start_output(vs);

	if (output) {
		sink->outfd = creat(output, S_IRUSR | S_IRGRP | S_IROTH | S_IWUSR);
		if (sink->outfd < 0)
			error(EXIT_FAILURE, errno, "Can not open output file");
	}

	av_dict_set(&opts, "probesize", "32", 0);
	av_dict_set(&opts, "analyzeduration", "32", 0);
	av_dict_set(&opts, "protocol_whitelist", "pipe,file,rtsp,rtp,tcp", 0);
	av_dict_set(&opts, "rtsp_transport", "tcp", 0);
	av_dict_set(&opts, "stimeout", "10000000", 0); /* default: 0 */
	if ((ret = avformat_open_input(&vs->ic, input, 0, &opts)) < 0)
		error(EXIT_FAILURE, 0, "Could not open input file '%s'", input);

	if ((ret = avformat_find_stream_info(vs->ic, NULL)) < 0)
		error(EXIT_FAILURE, 0, "Failed to retrieve input stream information");
	/* Dump information about file onto standard error */
	if (vlevel >= LOG_INFO) av_dump_format(vs->ic, 0, input, 0);

	pr_verb("Begin processing...");

	if (clock_gettime(CLOCK_MONOTONIC, &loopstart))
		error(EXIT_FAILURE, errno, "Error of retrieving the time of clock %u", CLOCK_MONOTONIC);

	/* Find first video and audio streams */
	for (int i = 0; i < vs->ic->nb_streams; i++) {
		if (vs->ic->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
		    vs->video_stream_number < 0)
			vs->video_stream_number = i;
		if (vs->ic->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
		    vs->audio_stream_number < 0)
			vs->audio_stream_number = i;
	}

	if (vs->audio_stream_number != -1) {
		if (init_audio(vs, audio_device))
			vs->audio_stream_number = -1;
	}

	if (vs->video_stream_number == -1)
		error(EXIT_FAILURE, 0, "Didn't find a video stream");

	vs->video_st = vs->ic->streams[vs->video_stream_number];

	/* TODO: Think how to add filter without command line option */
	if (is_mp4) {
		const AVBitStreamFilter *filter;

		pr_verb("Inserting h264_mp4toannexb bitstream filter");

		filter = av_bsf_get_by_name("h264_mp4toannexb");
		if (!filter)
			error(EXIT_FAILURE, 0, "h264_mp4toannexb bitstream filter required for H.264 streams\n");

		ret = av_bsf_alloc(filter, &src->bsf);
		if (ret < 0)
			error(EXIT_FAILURE, 0, "Couldn't allocate context for h264_mp4toannexb filter\n");
		ret = avcodec_parameters_copy(src->bsf->par_in, vs->ic->streams[vs->video_stream_number]->codecpar);
		if (ret < 0)
			error(EXIT_FAILURE, 0, "Couldn't copy codec parameters\n");

		ret = av_bsf_init(src->bsf);
		if (ret < 0)
			error(EXIT_FAILURE, 0, "Couldn't init filter\n");
	}

	packet_queue_init(&vs->videoq);

	ret = pthread_create(&sink->thread, NULL, start_video_sink, vs);
	if (ret != 0)
		error(EXIT_FAILURE, errno, "Can not create video src thread");

	ret = pthread_create(&src->thread, NULL, start_video_src, vs);
	if (ret != 0)
		error(EXIT_FAILURE, errno, "Can not create video sink thread");

	read_av(vs);

	packet_queue_put(&vs->videoq, &flush_pkt);

	ret = pthread_join(src->thread, NULL);
	if (ret != 0)
		error(EXIT_FAILURE, errno, "Can not join video src thread");

	/* Drain sequence */
	v4l2_stop_decode(vs->m2mfd);

	ret = pthread_join(vs->sink.thread, NULL);
	if (ret != 0)
		error(EXIT_FAILURE, errno, "Can not join video sink thread");

	vs->quit = 1;
	packet_queue_abort(&vs->audioq);
	packet_queue_abort(&vs->videoq);
	SDL_CondSignal(vs->audioq.cond);
	SDL_CondSignal(vs->videoq.cond);

	if (clock_gettime(CLOCK_MONOTONIC, &loopstop))
		error(EXIT_FAILURE, errno, "Error of retrieving the time of clock %u", CLOCK_MONOTONIC);

	pr_info("Output size: %" PRIu64 " KiB", sink->vd.size / 1024);

	looptime = timespec_subtract(loopstart, loopstop);

	pr_info("Total time in main loop: %.1f s (%.1f FPS)",
		timespec2float(looptime), src->vd.frames / timespec2float(looptime));

	if (sink->outfd >= 0)
		close(sink->outfd);

	if (src->vd.frames != sink->vd.frames)
		pr_warn("Some frames weren't decoded");

	if (vs->audio_ctx) {
		avcodec_close(vs->audio_ctx);
		avcodec_free_context(&vs->audio_ctx);
	}

	if (vs->swr_ctx)
		swr_free(&vs->swr_ctx);

	if (src->bsf)
		av_bsf_free(&src->bsf);

	if (sink->dsc)
		sws_freeContext(sink->dsc);

	avformat_close_input(&vs->ic);

	if (display) {
		av_frame_free(&sink->fb_frame);

		/* restore old mode */
		drmdisplay_set_mode(drm, &drm->old_mode, drm->old_fb, vlevel == LOG_INFO);
		drmdisplay_destroy_unmap_dumb(drm->fd, &sink->dumb);
		drmdisplay_close(drm);
	}

	if (vs->audio_dev != 0)
		SDL_CloseAudioDevice(vs->audio_dev);

	SDL_Quit();

	return EXIT_SUCCESS;
}
