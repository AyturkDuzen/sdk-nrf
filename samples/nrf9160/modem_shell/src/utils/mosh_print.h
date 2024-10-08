/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef MOSH_PRINT_H
#define MOSH_PRINT_H

enum mosh_print_level {
	MOSH_PRINT_LEVEL_PRINT,
	MOSH_PRINT_LEVEL_WARN,
	MOSH_PRINT_LEVEL_ERROR,
};

/**
 * Print without formatting, i.e., without including timestamp or any other formatting.
 * This is intended to be used for printing usage information for a command
 * but can be used elsewhere too.
 */
void mosh_print_no_format(const char *usage);

/**
 * printf-like function which sends formatted data stream to the shell output.
 * Not inteded to be used outside below macros.
 */
void mosh_fprintf(enum mosh_print_level print_level, const char *fmt, ...);

/** Print normal level information to output. This should be used for normal application output. */
#define mosh_print(fmt, ...) mosh_fprintf(MOSH_PRINT_LEVEL_PRINT, fmt, ##__VA_ARGS__)
/** Print warning level information to output. */
#define mosh_warn(fmt, ...)  mosh_fprintf(MOSH_PRINT_LEVEL_WARN, fmt, ##__VA_ARGS__)
/** Print error level information to output. */
#define mosh_error(fmt, ...) mosh_fprintf(MOSH_PRINT_LEVEL_ERROR, fmt, ##__VA_ARGS__)

#endif
