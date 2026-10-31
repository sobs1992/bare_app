/* Small helpers shared by every layer. */
#pragma once

#include <stdint.h>
#include <time.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

/* Monotonic time in milliseconds */
static inline uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* Read a whole small text file (sysfs attribute) into buf without the
 * trailing newline. Returns length or -1. */
int read_text_file(const char *path, char *buf, int size);
/* Read an integer from a text file; returns 0 on success */
int read_int_file(const char *path, int *val);
int write_int_file(const char *path, int val);
