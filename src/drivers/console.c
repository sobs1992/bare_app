#include "console.h"

#include <errno.h>
#include <stdbool.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "core/log.h"
#include "core/util.h"

#define TAG "console"

/* Global so that a crash handler can reach it */
static volatile int console_fd = -1;

void console_restore(void)
{
	if (console_fd >= 0)
		ioctl(console_fd, KDSETMODE, KD_TEXT);
}

static void on_fatal(int sig)
{
	console_restore();
	signal(sig, SIG_DFL);
	raise(sig);
}

void console_graphics(const char *tty)
{
	static const int fatal[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
	static bool hooked;
	size_t i;
	int fd;

	if (!tty || console_fd >= 0)
		return;
	fd = open(tty, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		LOGW(TAG, "%s: %s, the console may draw over the screen", tty, strerror(errno));
		return;
	}
	if (ioctl(fd, KDSETMODE, KD_GRAPHICS) < 0) {
		LOGW(TAG, "KD_GRAPHICS on %s: %s", tty, strerror(errno));
		close(fd);
		return;
	}
	console_fd = fd;
	if (hooked)
		return;
	hooked = true;
	atexit(console_restore);
	for (i = 0; i < ARRAY_SIZE(fatal); i++)
		signal(fatal[i], on_fatal);
}

int console_rotation(void)
{
	int r = 0;

	if (read_int_file("/sys/class/graphics/fbcon/rotate", &r) < 0)
		r = 0;
	return r & 3;
}
