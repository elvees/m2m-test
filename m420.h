/*
 * Copyright 2015, Anton Leontiev <aleontiev@elvees.com>
 *
 * SPDX-License-Identifier:	GPL-2.0
 */

#ifndef M420_H
#define M420_H

#include <libavformat/avformat.h>

void yuv420_to_m420(AVFrame *frame);

#endif /* M420_H */
