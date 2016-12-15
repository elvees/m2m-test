/*
 * Tool to benchmark access time to device-allocated buffers
 *
 * Copyright 2016 ELVEES NeoTek JSC
 * Author: Anton Leontiev <aleontiev@elvees.com>
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <error.h>
#include <limits.h>
#include <time.h>

#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/videodev2.h>

#include "v4l2-utils.h"

#define SZ_1K 1024
#define SZ_1M (1024 * 1024)

#define NSEC_IN_SEC 1000000000

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#ifndef VERSION
#define VERSION "unversioned"
#endif

volatile uint64_t s;

static inline struct timespec timespec_subtract(struct timespec const start,
		struct timespec const stop) {
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

static inline float timespec2float(struct timespec const t) {
	return t.tv_sec + (float)t.tv_nsec / 1e9;
}

__attribute__((noinline))
static void sum(void *ptr, size_t size) {
	const uint8_t *const a = ptr;
	uint64_t sum = 0;

	for (size_t i = 0; i < size; ++i)
		sum += a[i];

	*(uint64_t *)ptr = sum;
}

__attribute__((noinline))
static void fill(void *ptr, size_t size) {
	uint8_t *const a = ptr;
	uint8_t c = 0;

	for (size_t i = 0; i < size; ++i)
		a[i] = c++;
}

/**
 * Calculation function
 *
 * This function simulates real videoanalitycs. The array is not regular to
 * avoid data parallelism.
 */
static inline uint8_t calc(const uint8_t x) {
	static const uint8_t f[] = {
		0x46, 0x43, 0x11, 0x32, 0x01, 0x06, 0xf4, 0xe1,
		0x46, 0x81, 0x11, 0x32, 0x01, 0x06, 0xf4, 0xe1,
		0x46, 0x43, 0x11, 0x32, 0x01, 0x06, 0xf4, 0xe1,
		0x46, 0x85, 0x11, 0x33, 0x01, 0x06, 0xf4, 0xe1,
		0x46, 0x43, 0x11, 0x32, 0x01, 0x06, 0xf4, 0xe1,
		0x46, 0x85, 0x11, 0x32, 0x01, 0x06, 0xf4, 0xe1,
		0x46, 0x43, 0x11, 0x32, 0x01, 0x06, 0xf4, 0xe1,
		0x46, 0x85, 0x11, 0x32, 0x01, 0x06, 0xf4, 0xe1,
		0x46, 0x43, 0x11, 0x32, 0x01, 0x06, 0xf4, 0xe1,
		0x46, 0x85, 0x11, 0x32, 0x01, 0x06, 0xf4, 0xe1,
		0x56, 0x43, 0x11, 0x32, 0x01, 0x07, 0xf5, 0xe1,
		0x46, 0x85, 0x11, 0x32, 0x01, 0x06, 0xf4, 0xe1,
		0x46, 0x43, 0x11, 0x32, 0x01, 0x06, 0xf4, 0xe1,
		0x46, 0x85, 0x11, 0x32, 0x01, 0x06, 0xf4, 0xe1,
		0x46, 0x43, 0x11, 0x32, 0x01, 0x06, 0xf4, 0xe1,
		0x46, 0x85, 0x11, 0x32, 0x01, 0x06, 0xf4, 0xe1,
		0x46, 0x43, 0x11, 0x32, 0x01, 0x06, 0xf4, 0xe1,
		0x46, 0x85, 0x11, 0x32, 0x01, 0x06, 0xf4, 0xe1,
		0x46, 0x43, 0x11, 0x32, 0x02, 0x06, 0xf4, 0xe1,
		0x46, 0x85, 0x11, 0x32, 0x01, 0x06, 0xf4, 0xe1,
		0x46, 0x43, 0x11, 0x32, 0x01, 0x06, 0xf4, 0xe1,
		0x46, 0x85, 0x11, 0x32, 0x01, 0x06, 0xf4, 0xe1,
		0x46, 0x43, 0x11, 0x32, 0x01, 0x06, 0xf4, 0xe1,
		0x46, 0x85, 0x11, 0x32, 0x01, 0x06, 0xf4, 0xe1,
		0x46, 0x43, 0x11, 0x32, 0x01, 0x06, 0xf4, 0xe1,
		0x46, 0x85, 0x12, 0x32, 0x01, 0x06, 0xf4, 0xe1,
		0x46, 0x43, 0x11, 0x32, 0x01, 0x06, 0xf4, 0xe1,
		0x46, 0x85, 0x11, 0x32, 0x01, 0x06, 0xf4, 0xe1,
		0x46, 0x43, 0x11, 0x32, 0x01, 0x06, 0xf4, 0xe2,
		0x46, 0x85, 0x11, 0x32, 0x01, 0x06, 0xf4, 0xe1,
		0x46, 0x43, 0x11, 0x32, 0x01, 0x06, 0xf4, 0xe1,
		0x46, 0x85, 0x11, 0x32, 0x01, 0x06, 0xf4, 0xee
	};

	return f[x];
}

__attribute__((noinline))
static void rw(void *ptr, size_t size) {
	uint8_t *const a = ptr;

	for (size_t i = 0; i < size; ++i)
		a[i] = calc(a[i]);
}


static void help(const char *program_name) {
	puts("v4l2mmapbench " VERSION " \n");
	printf("Synopsis: %s [options] v4l2-device\n\n", program_name);
	puts("Options:");
	puts("    -n arg    Number of iterations");
	puts("    -r        Benchmark reads");
	puts("    -w        Benchmark writes");

}

int main(int argc, char *argv[]) {
	int opt, rc, fd;
	struct timespec start, stop, time;

	uint32_t *mallocbuf, *mmapbuf;
	unsigned num = 1;
	bool read = false, write = false;

	while ((opt = getopt(argc, argv, "hn:rw")) != -1) {
		switch (opt) {
			case 'h': help(argv[0]); return EXIT_SUCCESS;
			case 'n': num = atoi(optarg); break;
			case 'r': read = true; break;
			case 'w': write = true; break;
			default: error(EXIT_FAILURE, 0, "Try %s -h for help.", argv[0]);
		}
	}

	if (argc < optind + 1)
		error(EXIT_FAILURE, 0, "Not enough arguments");

	const char *device = argv[optind];

	char card[32];
	fd = v4l2_open(device, V4L2_CAP_VIDEO_M2M, 0, card);
	printf("Card: %.32s\n", card);

	v4l2_buffers_request(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, 1, V4L2_MEMORY_MMAP);

	struct v4l2_buffer v4l2buf = {
		.index = 0,
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE
	};

	rc = ioctl(fd, VIDIOC_QUERYBUF, &v4l2buf);
	if (rc != 0)
		error(EXIT_FAILURE, errno, "Can not query buffer");

	printf("Buffer length: %u\n", v4l2buf.length);

	mmapbuf = mmap(NULL, v4l2buf.length, PROT_READ | PROT_WRITE, MAP_SHARED,
	               fd, v4l2buf.m.offset);

	if (mmapbuf == MAP_FAILED)
		error(EXIT_FAILURE, errno, "Can not mmap buffer");

	mallocbuf = malloc(v4l2buf.length);
	if (!mallocbuf)
		error(EXIT_FAILURE, 0, "Failed to allocate memory");

	struct test {
		bool condition;
		void (*func)(void *ptr, size_t size);
		void *buf;
		char *const message;
	} tests[] = {
		{ read,          sum,  mallocbuf, "Read malloc" },
		{ read,          sum,  mmapbuf,   "Read mmap" },
		{ write,         fill, mallocbuf, "Write malloc" },
		{ write,         fill, mmapbuf,   "Write mmap" },
		{ read && write, rw,   mallocbuf, "Read & write malloc" },
		{ read && write, rw,   mmapbuf,   "Read & write mmap" }
	};

	for (unsigned t = 0; t < ARRAY_SIZE(tests); ++t) {
		if (!tests[t].condition)
			continue;

		rc = clock_gettime(CLOCK_MONOTONIC, &start);

		for (unsigned i = 0; i < num; ++i) {
			tests[t].func(tests[t].buf, v4l2buf.length);
		}

		rc = clock_gettime(CLOCK_MONOTONIC, &stop);
		time = timespec_subtract(start, stop);

		printf("%s: %.1f s\n", tests[t].message, timespec2float(time));
	}

	close(fd);

	return EXIT_SUCCESS;
}
