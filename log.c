/*
 * Implementation of logging functions
 *
 * Copyright (C) 2016 ELVEES NeoTek JSC
 * Author: Anton Leontiev <aleontiev@elvees.com>
 *
 * SPDX-License-Identifier:	GPL-2.0
 */

#include <stdarg.h>
#include <stdio.h>

#include "log.h"

enum loglevel vlevel = LOG_WARNING;

void pr_raw(enum loglevel const level, char const *format, ...)
{
	if (level <= vlevel) {
		FILE *const stream = level < LOG_INFO ? stderr : stdout;
		va_list va;
		va_start(va, format);
		vfprintf(stream, format, va);
		putc('\n', stream);
		va_end(va);
	}
}

void pr_cont(enum loglevel const level, char const *format, ...)
{
	if (level <= vlevel) {
		FILE *const stream = level < LOG_INFO ? stderr : stdout;
		va_list va;
		va_start(va, format);
		vfprintf(stream, format, va);
		va_end(va);
	}
}
