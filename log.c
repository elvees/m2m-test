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

void pr_level(enum loglevel const level, char const *format, ...)
{
	if (level <= vlevel) {
		va_list va;
		va_start(va, format);
		vfprintf(level < LOG_INFO ? stderr : stdout, format, va);
		putchar('\n');
		va_end(va);
	}
}

void pr_cont(enum loglevel const level, char const *format, ...)
{
	if (level <= vlevel) {
		va_list va;
		va_start(va, format);
		vfprintf(level < LOG_INFO ? stderr : stdout, format, va);
		va_end(va);
	}
}
