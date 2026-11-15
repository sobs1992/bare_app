/*
 * Commands for tests and tools: lines written to a FIFO, $BARE_APP_CTL or
 * /tmp/bare_app.ctl, read by the view thread (the SDK's scripts start the
 * games of a device over ssh this way, without going through the menu):
 *   launch WHAT     start the game WHAT names (game_find(): a directory's
 *                   name such as monkey2, a title, a key); a minimised
 *                   game is closed first, a game in front is refused
 *   quit-game       end the game in front, or the minimised one
 *   back            what B does on a screen: back one screen
 * Windows games take their own commands: bare_app_wine's CTL_PATH.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "view_priv.h"
#include "core/log.h"
#include "core/mainloop.h"
#include "games/game.h"
#include "gui/action.h"

#define TAG "ctl"
#define CTL_DEFAULT "/tmp/bare_app.ctl"

static int ctl_fd = -1;
static char ctl_path[256];
static char buf[256];
static size_t len;

static void launch(const char *what)
{
#ifdef CONFIG_GAMES
	struct game_ref g;

	if (!game_find(what, &g)) {
		LOGW(TAG, "launch %s: no such game", what);
		return;
	}
	if (view_game_in_front()) {
		LOGW(TAG, "launch %s: a game is in front (quit-game first)", what);
		return;
	}
	if (view_video_minimized() != VIDEO_NONE)
		view_video_set(VIDEO_NONE);	/* ends the minimised game */
	LOGI(TAG, "launch %s: %s", what, game_title(g));
	view_game_select(g);
	gui_action_post(ACT_OPEN_SCREEN, SCREEN_GAME);
#else
	LOGW(TAG, "launch %s: no games in this build", what);
#endif
}

static void line(char *l)
{
	char *arg = strchr(l, ' ');

	if (arg)
		*arg++ = '\0';
	if (!strcmp(l, "launch") && arg && *arg) {
		launch(arg);
	} else if (!strcmp(l, "quit-game")) {
		if (view_game_in_front())
			view_game_quit();
		else if (view_video_minimized() != VIDEO_NONE)
			view_video_set(VIDEO_NONE);
	} else if (!strcmp(l, "back")) {
		gui_action_post(ACT_BACK, 0);
	} else if (*l) {
		LOGW(TAG, "%s?", l);
	}
}

static void on_ctl(int fd, short revents, void *ud)
{
	ssize_t n;
	char *nl;

	(void)revents;
	(void)ud;
	while ((n = read(fd, buf + len, sizeof(buf) - 1 - len)) > 0) {
		len += (size_t)n;
		buf[len] = '\0';
		while ((nl = strchr(buf, '\n'))) {
			*nl = '\0';
			line(buf);
			len -= (size_t)(nl + 1 - buf);
			memmove(buf, nl + 1, len + 1);
		}
		if (len == sizeof(buf) - 1)
			len = 0;	/* a line too long: dropped */
	}
}

void view_ctl_open(struct mainloop *ml)
{
	const char *path = getenv("BARE_APP_CTL");

	snprintf(ctl_path, sizeof(ctl_path), "%s", path && *path ? path : CTL_DEFAULT);
	unlink(ctl_path);
	if (mkfifo(ctl_path, 0600) < 0) {
		LOGW(TAG, "%s: %s", ctl_path, strerror(errno));
		return;
	}
	/* read and write: no end of file when a writer closes it */
	ctl_fd = open(ctl_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (ctl_fd < 0) {
		LOGW(TAG, "%s: %s", ctl_path, strerror(errno));
		unlink(ctl_path);
		return;
	}
	mainloop_add_fd(ml, ctl_fd, POLLIN, on_ctl, NULL);
}

void view_ctl_close(void)
{
	if (ctl_fd < 0)
		return;
	close(ctl_fd);
	unlink(ctl_path);
	ctl_fd = -1;
}
