/*
 * Copyright 2019 RnD Center "ELVEES", JSC
 */

#ifndef DRMDISPLAY_H
#define DRMDISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include <xf86drmMode.h>

#define DRMDISPLAY_AUTO_CONNID (-1)

struct drmdisplay {
	int fd;
	uint32_t conn_id;
	uint32_t crtc_id;
	drmModeModeInfo *modes;
	int count_modes;
	drmModeModeInfoPtr cur_mode;
	uint32_t old_fb;
	drmModeModeInfo old_mode;
};

struct drmdisplay_dumb {
	uint32_t handle;
	uint32_t fb_id;
	uint32_t pitch;
	size_t size;
	void *addr;
};

/* Check DRM, scan available modes and get current mode.
 * Return 0 on success or -1 on error.
 */
int drmdisplay_init(struct drmdisplay *data, int connector_id, bool verbose);

/* Close DRM device descriptor and free resources.
 * Return 0 on success or -1 on error.
 */
void drmdisplay_close(struct drmdisplay *data);

/* Fill width and height variables.
 * If all variables are 0 then will used current display mode.
 * If some variables are non-zero then will fill another variables by correct values.
 * Return 0 on success or -1 if no suitable modes found.
 */
int drmdisplay_fill_mode(struct drmdisplay *data, int *width, int *height);

/* Change resolution to use framebuffer identified by fb_id.
 */
void drmdisplay_set_mode(struct drmdisplay *data, drmModeModeInfoPtr mode, uint32_t fb_id, bool verbose);

/* Get mode, that corresponds to width and height.
 * Return pointer to mode or NULL if there isn't corresponding mode.
 */
drmModeModeInfoPtr drmdisplay_get_mode(struct drmdisplay *data, int width, int height);

/* Restore old mode and free all resources.
 * Return 0 on success or -1 on error.
 */
int drmdisplay_restore_mode(struct drmdisplay *data);

/* Destroy, unmap dumb-buffer */
void drmdisplay_destroy_unmap_dumb(int fd, struct drmdisplay_dumb *fb);

/* Create, map dumb-buffer */
void drmdisplay_create_mmap_dumb(int fd, int width, int height, int bpp, struct drmdisplay_dumb *fb);

#endif
