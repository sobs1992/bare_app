#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>

#include "util.h"

static enum log_level max_level = LOG_INFO;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t t0;

void log_set_level(enum log_level level)
{
	max_level = level;
}

enum log_level log_get_level(void)
{
	return max_level;
}

void log_msg(enum log_level level, const char *tag, const char *fmt, ...)
{
	static const char letter[] = "EWID";
	uint64_t t;
	va_list ap;

	if (level > max_level)
		return;
	pthread_mutex_lock(&lock);
	t = now_ms();
	if (!t0)
		t0 = t;
	t -= t0;
	fprintf(stderr, "[%4u.%03u] %c %s: ", (unsigned)(t / 1000), (unsigned)(t % 1000),
		letter[level], tag);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	pthread_mutex_unlock(&lock);
}
