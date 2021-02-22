/*
 * Logging functions definition
 *
 * Copyright (C) 2016 ELVEES NeoTek JSC
 * Author: Anton Leontiev <aleontiev@elvees.com>
 *
 * SPDX-License-Identifier:	GPL-2.0
 */

#ifndef LOG_H
#define LOG_H

enum loglevel {
	LOG_ERROR,
	LOG_WARNING,
	LOG_INFO,
	LOG_VERBOSE,
	LOG_DEBUG
};

extern enum loglevel vlevel;

void pr_raw(enum loglevel const level, char const *format, ...);
void pr_cont(enum loglevel const level, char const *format, ...);

#ifdef LOG_PREFIX
#define pr_level(level, format, ...) pr_raw(level, LOG_PREFIX ": " format, ##__VA_ARGS__)
#else
#define pr_level(level, format, ...) pr_raw(level, format, ##__VA_ARGS__)
#endif

#define pr_err(format, ...)   pr_level(LOG_ERROR, format, ##__VA_ARGS__)
#define pr_warn(format, ...)  pr_level(LOG_WARNING, format, ##__VA_ARGS__)
#define pr_info(format, ...)  pr_level(LOG_INFO, format, ##__VA_ARGS__)
#define pr_verb(format, ...)  pr_level(LOG_VERBOSE, format, ##__VA_ARGS__)
#define pr_debug(format, ...) pr_level(LOG_DEBUG, format, ##__VA_ARGS__)

#endif /* LOG_H */
