/*
 * Copyright 2019 RnD Center "ELVEES", JSC
 */

#include <error.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm.h>

#include "drmdisplay.h"

void drmdisplay_destroy_unmap_dumb(int fd, struct drmdisplay_dumb *fb)
{
	int ret;
	struct drm_mode_destroy_dumb dreq = {
		.handle = fb->handle
	};

	ret = munmap(fb->addr, fb->size);
	if (ret < 0)
		error(EXIT_FAILURE, errno, "Cannot unmap dumb-buffer");

	fb->addr = NULL;

	ret = drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
	if (ret < 0)
		error(EXIT_FAILURE, errno, "Can not destroy dumb-buffer");

	fb->size = 0;

	ret = drmModeRmFB(fd, fb->fb_id);
	if (ret)
		error(EXIT_FAILURE, errno, "Can not remove framebuffer");
}

void drmdisplay_create_mmap_dumb(int fd, int width, int height, int bpp, struct drmdisplay_dumb *fb)
{
	int ret;
	struct drm_mode_create_dumb creq = { 0 };
	struct drm_mode_map_dumb mreq = { 0 };
	uint32_t handle;
	void *fb_addr;
	size_t size;
	uint32_t pitch;
	uint32_t fb_id;

	/* create dumb buffer */
	creq.width = width;
	creq.height = height;
	creq.bpp = bpp;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
	if (ret < 0)
		error(EXIT_FAILURE, errno, "Can not create dumb-buffer");

	size = creq.size;
	pitch = creq.pitch;
	handle = creq.handle;

	/* prepare buffer for memory mapping */
	mreq.handle = handle;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
	if (ret)
		error(EXIT_FAILURE, errno, "DRM buffer preparation failed");

	fb_addr = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mreq.offset);
	if (fb_addr == MAP_FAILED)
		error(EXIT_FAILURE, errno, "framebuffer memory mapping failed");

	memset(fb_addr, 0, size);

	ret = drmModeAddFB(fd, width, height, 24, bpp, pitch, handle, &fb_id);
	if (ret)
		error(EXIT_FAILURE, errno, "Can not create framebuffer via drmModeAddFB()");

	fb->handle = handle;
	fb->fb_id = fb_id;
	fb->pitch = pitch;
	fb->size = size;
	fb->addr = fb_addr;
}

static void print_resources(drmModeResPtr res)
{
	printf("\nResources:\n");
	printf("min_width:        %d\n", res->min_width);
	printf("max_width:        %d\n", res->max_width);
	printf("min_height:       %d\n", res->min_height);
	printf("max_height:       %d\n", res->max_height);
	printf("count_connectors: %d\n", res->count_connectors);
	for (int i = 0; i < res->count_connectors; i++)
		printf("  [%d]: %d\n", i, res->connectors[i]);

	printf("count_encoders:   %d\n", res->count_encoders);
	for (int i = 0; i < res->count_encoders; i++)
		printf("  [%d]: %d\n", i, res->encoders[i]);

	printf("count_crtcs:      %d\n", res->count_crtcs);
	for (int i = 0; i < res->count_crtcs; i++)
		printf("  [%d]: %d\n", i, res->crtcs[i]);

	printf("count_fbs:        %d\n", res->count_fbs);
	for (int i = 0; i < res->count_fbs; i++)
		printf("  [%d]: %d\n", i, res->fbs[i]);
}

static void print_connector(drmModeConnectorPtr conn)
{
	const char *mode_connectors[] = {
		"Unknown",
		"VGA",
		"DVII",
		"DVID",
		"DVIA",
		"Composite",
		"SVIDEO",
		"LVDS",
		"Component",
		"9PinDIN",
		"DisplayPort",
		"HDMIA",
		"HDMIB",
		"TV",
		"eDP",
		"VIRTUAL",
		"DSI",
		"DPI",
	};
	const char *mode_connections[] = {
		"DRM_MODE_CONNECTED",
		"DRM_MODE_DISCONNECTED",
		"DRM_MODE_UNKNOWNCONNECTION",
	};
	const char *mode_subpixels[] = {
		"DRM_MODE_SUBPIXEL_UNKNOWN",
		"DRM_MODE_SUBPIXEL_HORIZONTAL_RGB",
		"DRM_MODE_SUBPIXEL_HORIZONTAL_BGR",
		"DRM_MODE_SUBPIXEL_VERTICAL_RGB",
		"DRM_MODE_SUBPIXEL_VERTICAL_BGR",
		"DRM_MODE_SUBPIXEL_NONE",
	};
	const char *mode_connector = (conn->connector_type <= 17) ?
			mode_connectors[conn->connector_type] : "?";
	const char *mode_connection = (conn->connection <= 3) ?
			mode_connections[conn->connection - 1] : "?";
	const char *mode_subpixel = (conn->subpixel <= 6) ?
			mode_subpixels[conn->subpixel - 1] : "?";

	printf("\nConnector:\n");
	printf("connector_id:      %d\n", conn->connector_id);
	printf("encoder_id:        %d\n", conn->encoder_id);
	printf("connector_type:    %d (%s)\n", conn->connector_type, mode_connector);
	printf("connector_type_id: %d\n", conn->connector_type_id);
	printf("connection:        %d (%s)\n", conn->connection, mode_connection);
	printf("mmWidth:           %d\n", conn->mmWidth);
	printf("mmHeight:          %d\n", conn->mmHeight);
	printf("subpixel:          %d (%s)\n", conn->subpixel, mode_subpixel);
	printf("count_encoders:    %d\n", conn->count_encoders);
	for (int i = 0; i < conn->count_encoders; i++)
		printf("  [%d]: %d\n", i, conn->encoders[i]);

	printf("count_modes:       %d\n", conn->count_modes);
	for (int i = 0; i < conn->count_modes; i++)
		printf("  [%d]: %s (%dx%d-%d)\n", i, conn->modes[i].name, conn->modes[i].hdisplay,
		       conn->modes[i].vdisplay, conn->modes[i].vrefresh);

	printf("count_props:       %d\n", conn->count_props);
	for (int i = 0; i < conn->count_props; i++)
		printf("  [%d]: prop: %#x, value: %#" PRIx64 "\n", i, conn->props[i],
		       conn->prop_values[i]);
}

static void print_encoder(drmModeEncoderPtr enc)
{
	printf("\nEncoder:\n");
	printf("encoder_id:      %d\n", enc->encoder_id);
	printf("encoder_type:    %d\n", enc->encoder_type);
	printf("crtc_id:         %d\n", enc->crtc_id);
	printf("possible_crtcs:  %d\n", enc->possible_crtcs);
	printf("possible_clones: %d\n", enc->possible_clones);
}

static void use_drm_connector(struct drmdisplay *data, drmModeResPtr resources, uint32_t connector_id,
			      bool verbose)
{
	uint32_t encoder_id;
	drmModeEncoderPtr enc;
	drmModeCrtcPtr crtc;
	drmModeConnectorPtr conn;

	if (verbose)
		print_resources(resources);
	if (!resources->count_connectors)
		error(EXIT_FAILURE, 0, "Can not find proper DRM connector");

	if (connector_id == (uint32_t)DRMDISPLAY_AUTO_CONNID)
		connector_id = resources->connectors[0];

	conn = drmModeGetConnector(data->fd, connector_id);
	if (!conn)
		error(EXIT_FAILURE, errno, "Can not get DRM connector %d", connector_id);

	if (verbose)
		print_connector(conn);

	data->count_modes = conn->count_modes;
	data->modes = (drmModeModeInfo *)malloc(sizeof(drmModeModeInfo) * conn->count_modes);
	for (int i = 0; i < conn->count_modes; i++)
		data->modes[i] = conn->modes[i];

	encoder_id = conn->encoder_id;

	// If current encoder is not set then select first available encoder for this connector
	if (!encoder_id && conn->count_encoders)
		encoder_id = conn->encoders[0];

	if (verbose)
		printf("\nUsing encoder_id: %u\n", encoder_id);

	drmModeFreeConnector(conn);

	enc = drmModeGetEncoder(data->fd, encoder_id);
	if (!enc)
		error(EXIT_FAILURE, errno, "Can not get DRM encoder for connector %d", connector_id);

	if (verbose)
		print_encoder(enc);

	data->crtc_id = enc->crtc_id;

	// If current CRTC is not set then select first available CRTC
	if (!data->crtc_id && resources->count_crtcs)
		data->crtc_id = resources->crtcs[0];

	if (verbose)
		printf("\nUsing crtc_id: %u\n", data->crtc_id);

	drmModeFreeEncoder(enc);
	crtc = drmModeGetCrtc(data->fd, data->crtc_id);
	if (!crtc)
		error(EXIT_FAILURE, errno, "Can not get DRM crtc for encoder %d", encoder_id);

	data->conn_id = connector_id;
	data->old_fb = crtc->buffer_id;
	data->old_mode = crtc->mode;
	drmModeFreeCrtc(crtc);
}

int drmdisplay_init(struct drmdisplay *data, int connector_id, bool verbose)
{
	drmModeResPtr resources;

	memset(data, 0, sizeof(*data));
	data->fd = open("/dev/dri/card0", O_RDWR);
	if (data->fd == -1) {
		error(0, errno, "Can not open video driver");
		return -1;
	}

	resources = drmModeGetResources(data->fd);
	if (!resources)
		error(EXIT_FAILURE, errno, "Can not get DRM resources");

	use_drm_connector(data, resources, (uint32_t)connector_id, verbose);
	drmModeFreeResources(resources);

	return 0;
}

int drmdisplay_fill_mode(struct drmdisplay *data, int *width, int *height)
{
	if (*width && *height)
		return 0;  // Nothing to fill

	// if no one field is filled
	if (!*width && !*height) {
		*width = data->old_mode.hdisplay;
		*height = data->old_mode.vdisplay;
		return 0;
	}

	// if some fields are filled then filling another fields
	for (int i = 0; i < data->count_modes; i++) {
		if ((!*width || (data->modes[i].hdisplay == *width)) &&
		    (!*height || (data->modes[i].vdisplay == *height))) {
			*width = data->modes[i].hdisplay;
			*height = data->modes[i].vdisplay;
			return 0;
		}
	}

	// no suitable modes found
	return -1;
}

drmModeModeInfoPtr drmdisplay_get_mode(struct drmdisplay *data, int width, int height)
{
	for (int i = 0; i < data->count_modes; i++) {
		if ((data->modes[i].hdisplay == width) && (data->modes[i].vdisplay == height))
			return &data->modes[i];
	}

	return NULL;
}

void drmdisplay_set_mode(struct drmdisplay *data, drmModeModeInfoPtr mode, uint32_t fb_id, bool verbose)
{
	if (verbose)
		printf("Setting resolution %s %d Hz\n", mode->name, mode->vrefresh);

	if (drmModeSetCrtc(data->fd, data->crtc_id, fb_id, 0, 0, &data->conn_id, 1, mode))
		error(EXIT_FAILURE, errno, "Can not change mode for DRM crtc %d", data->crtc_id);
}


void drmdisplay_close(struct drmdisplay *data)
{
	if (data->count_modes) {
		data->count_modes = 0;
		free(data->modes);
	}

	if (close(data->fd))
		error(EXIT_FAILURE, errno, "Error at close DRM driver");
}
