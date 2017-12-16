/*
 * Tool to benchmark access time to device-allocated buffers
 *
 * Copyright 2017 RnD Center "ELVEES", JSC
 * Author: Anton Leontiev <aleontiev@elvees.com>
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include <fcntl.h>
#include <errno.h>
#include <error.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/videodev2.h>
#include "v4l2-utils.h"

#ifdef DMABUFEXP
#include <linux/dmabuf_exporter.h>
#endif

#ifdef LIBDRM
#include <libkms.h>
#include <xf86drm.h>
#endif

#define SZ_1K 1024
#define SZ_1M (1024 * 1024)

#define NSEC_IN_SEC 1000000000

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#ifndef VERSION
#define VERSION "unversioned"
#endif

static void timespec_gettime(struct timespec *ts) {
	int rc = clock_gettime(CLOCK_MONOTONIC, ts);
	if (rc < 0)
		error(EXIT_FAILURE, errno, "Failed to get time");
}

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

uint64_t res;

__attribute__((noinline))
static void sum(void *ptr, size_t size) {
	const uint8_t *const a = ptr;
	uint64_t sum = 0;

	for (size_t i = 0; i < size; ++i)
		sum += a[i];

	/* This assignment is required to prevent compiler from optimizing out
	 * the whole function. */
	res = sum;
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

/* Suffix is added to avoid clash with v4l2_open() */
static int v4l2_open2(const char *const device) {
	char card[32];
	int fd = v4l2_open(device, V4L2_CAP_VIDEO_M2M, 0, card);

	printf("Card: %.32s\n", card);

	return fd;
}

static void *v4l2_alloc(const int fd, size_t *const size) {
	v4l2_buffers_request(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, 1, V4L2_MEMORY_MMAP);

	struct v4l2_buffer v4l2buf = {
		.index = 0,
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE
	};

	int rc = ioctl(fd, VIDIOC_QUERYBUF, &v4l2buf);
	if (rc != 0)
		error(EXIT_FAILURE, errno, "Can not query buffer");

	void *buf = mmap(NULL, v4l2buf.length, PROT_READ | PROT_WRITE, MAP_SHARED,
	                 fd, v4l2buf.m.offset);

	if (buf == MAP_FAILED)
		error(EXIT_FAILURE, errno, "Can not mmap buffer");

	*size = v4l2buf.length;

	return buf;
}

static void v4l2_free(void *const ptr, size_t const size) {
	if (munmap(ptr, size))
		error(EXIT_FAILURE, errno, "Failed to munmap() buffer");
}

static void simple_close(const int fd) {
	if (close(fd))
		error(EXIT_FAILURE, errno, "Failed to close device descriptor");
}

#ifdef DMABUFEXP
static int buffd;

static int simple_open(const char *const device) {
	const int fd = open(device, O_RDWR);

	if (fd < 0)
		error(EXIT_FAILURE, errno, "Failed to open %s", device);

	return fd;
}

static void *dmabufexp_alloc(const int fd, size_t *const size) {
	int rc = ioctl(fd, DMABUF_IOCTL_CREATE, *size);
	if (rc < 0)
		error(EXIT_FAILURE, rc, "Failed to create buffer");

	buffd = ioctl(fd, DMABUF_IOCTL_EXPORT, O_RDWR);
	if (buffd < 0)
		error(EXIT_FAILURE, buffd, "Failed to export buffer");

	void *const buf = mmap(NULL, *size, PROT_READ | PROT_WRITE, MAP_SHARED, buffd, 0);

	if (buf == MAP_FAILED)
		error(EXIT_FAILURE, errno, "Failed to mmap buffer");

	return buf;
}

static void dmabufexp_free(void *const ptr, size_t const size) {
	if (munmap(ptr, size))
		error(EXIT_FAILURE, errno, "Failed to munmap() buffer");

	if (close(buffd))
		error(EXIT_FAILURE, errno, "Failed to close buffer file descriptor");
}
#endif

#ifdef LIBDRM
static struct kms_driver *kms;
static struct kms_bo *bo;

static int drm_open(const char *const device) {
	static const char *const drivers[] = {
		"pdp-drm", "i915", "radeon", "nouveau", "vmwgfx", "exynos", "amdgpu",
		"imx-drm", "rockchip", "atmel-hlcdc", "msm", "xilinx_drm", "vc4",
		"omapdrm", "tilcdc", "sti", "tegra"
	};

	for (int i = 0; i < ARRAY_SIZE(drivers); i++) {
		int fd = drmOpen(drivers[i], NULL);
		if (fd >= 0) {
			int rc = kms_create(fd, &kms);
			if (rc)
				error(EXIT_FAILURE, rc, "Failed to create KMS driver");

			return fd;
		}
	}

	error(EXIT_FAILURE, 0, "Failed to open DRM device");
	return -ENODEV;
}

static void *drm_alloc(const int fd, size_t *const size) {
	void *buf;
	int rc;

	unsigned attrs[] = { KMS_WIDTH, 1024, KMS_HEIGHT, *size / 4096, KMS_TERMINATE_PROP_LIST };
	*size = attrs[1] * attrs[3] * 4;

	rc = kms_bo_create(kms, attrs, &bo);
	if (rc < 0)
		error(EXIT_FAILURE, rc, "Failed to create binary object");

	rc = kms_bo_map(bo, (void **)&buf);
	if (rc < 0)
		error(EXIT_FAILURE, rc, "Failed to map binary object");

	return buf;
}

static void drm_free(void *const ptr, size_t const size) {
	int rc;

	rc = kms_bo_unmap(bo);
	if (rc)
		error(EXIT_FAILURE, rc, "Failed to unmap binary object");

	rc = kms_bo_destroy(&bo);
	if (rc)
		error(EXIT_FAILURE, rc, "Failed to destroy binary object");
}

static void drm_close(const int fd) {
	int rc;

	rc = kms_destroy(&kms);
	if (rc)
		error(EXIT_FAILURE, rc, "Failed to destroy KMS driver");

	if (drmClose(fd))
		error(EXIT_FAILURE, errno, "Failed to close DRM device");
}
#endif

const struct backend {
	char *name;
	int (*device_open)(const char *const device);
	void *(*buffer_alloc)(const int fd, size_t *const size);
	void (*buffer_free)(void *const ptr, size_t const size);
	void (*device_close)(int fd);
	void *priv;
} backends[] = {
	{ "v4l2", v4l2_open2, v4l2_alloc, v4l2_free, simple_close },
#ifdef DMABUFEXP
	{ "dmabufexp", simple_open, dmabufexp_alloc, dmabufexp_free, simple_close },
#endif
#ifdef LIBDRM
	{ "drm", drm_open, drm_alloc, drm_free, drm_close }
#endif
};

static void help(const char *program_name) {
	puts("devbufbench " VERSION " \n");
	printf("Synopsis: %s [options] -t device-type device\n\n", program_name);
	puts("Options:");
	puts("    -h        Print help message");
	puts("    -n arg    Number of iterations");
	puts("    -r        Benchmark reads");
	puts("    -s arg    Buffer size in MiB");
	fputs("    -t arg    Device type. Supported types are: ", stdout);

	for (int i = 0; i < ARRAY_SIZE(backends); ++i)
		printf("%s ", backends[i].name);
	puts("");

	puts("    -w        Benchmark writes");
}

int main(int argc, char *argv[]) {
	int opt, fd;
	struct timespec start, stop, time;

	uint32_t *mallocbuf, *devbuf;
	unsigned num = 1;
	bool read = false, write = false;
	char *devicetype = NULL;
	size_t size = SZ_1M;

	while ((opt = getopt(argc, argv, "hn:rs:t:w")) != -1) {
		switch (opt) {
			case 'h': help(argv[0]); return EXIT_SUCCESS;
			case 'n': num = atoi(optarg); break;
			case 'r': read = true; break;
			case 's': size = atoi(optarg) * SZ_1M; break;
			case 't': devicetype = optarg; break;
			case 'w': write = true; break;
			default: error(EXIT_FAILURE, 0, "Try %s -h for help.", argv[0]);
		}
	}

	if (argc < optind + 1)
		error(EXIT_FAILURE, 0, "Not enough arguments");

	if (!devicetype)
		error(EXIT_FAILURE, 0, "Device type is not specified");

	const struct backend *backend = NULL;

	for (int i = 0; i < ARRAY_SIZE(backends); ++i)
		if (strcmp(devicetype, backends[i].name) == 0) {
			backend = &backends[i];
			break;
		}

	if (!backend)
		error(EXIT_FAILURE, 0, "Unknown device type: %s", devicetype);

	const char *device = argv[optind];

	fd = backend->device_open(device);
	devbuf = backend->buffer_alloc(fd, &size);

	if (strcmp(devicetype, "drm") != 0)
		printf("Device: %s\n", device);

	printf("Device type: %s\n", devicetype);
	printf("Buffer size: %zu KiB\n", size / SZ_1K);
	printf("Iterations: %u\n", num);

	mallocbuf = malloc(size);
	if (!mallocbuf)
		error(EXIT_FAILURE, 0, "Failed to allocate memory");

	struct test {
		bool condition;
		void (*func)(void *ptr, size_t size);
		void *buf;
		char *const message;
	} tests[] = {
		{ read,          sum,  mallocbuf, "Read malloc" },
		{ read,          sum,  devbuf,    "Read dev" },
		{ write,         fill, mallocbuf, "Write malloc" },
		{ write,         fill, devbuf,    "Write dev" },
		{ read && write, rw,   mallocbuf, "Read & write malloc" },
		{ read && write, rw,   devbuf,    "Read & write dev" }
	};

	for (unsigned t = 0; t < ARRAY_SIZE(tests); ++t) {
		if (!tests[t].condition)
			continue;

		timespec_gettime(&start);

		for (unsigned i = 0; i < num; ++i) {
			tests[t].func(tests[t].buf, size);
		}

		timespec_gettime(&stop);
		time = timespec_subtract(start, stop);

		printf("%s: %.1f s\n", tests[t].message, timespec2float(time));
	}

	backend->buffer_free(devbuf, size);
	backend->device_close(fd);

	return EXIT_SUCCESS;
}
