#include "mainloop.h"

#include <errno.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "log.h"
#include "util.h"

#define MAX_FDS 32

struct watch {
	int fd;			/* -1: free or removed */
	short events;
	ml_fd_cb cb;
	void *ud;
};

struct ml_timer {
	struct ml_timer *next;
	uint64_t due;
	uint32_t period;
	ml_timer_cb cb;
	void *ud;
	bool dead;
};

struct mainloop {
	const char *name;
	struct watch watches[MAX_FDS];
	int nwatches;
	struct ml_timer *timers;
	ml_prepare_cb prepare;
	void *prepare_ud;
	int wake_fd;
	atomic_bool quit;
};

static void on_wake(int fd, short revents, void *ud)
{
	uint64_t v;

	(void)revents;
	(void)ud;
	if (read(fd, &v, sizeof(v)) < 0 && errno != EAGAIN)
		LOGW("mainloop", "wake read: %s", strerror(errno));
}

struct mainloop *mainloop_new(const char *name)
{
	struct mainloop *ml = calloc(1, sizeof(*ml));

	if (!ml)
		return NULL;
	ml->name = name;
	ml->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (ml->wake_fd < 0) {
		free(ml);
		return NULL;
	}
	mainloop_add_fd(ml, ml->wake_fd, POLLIN, on_wake, NULL);
	return ml;
}

void mainloop_free(struct mainloop *ml)
{
	struct ml_timer *t, *n;

	if (!ml)
		return;
	for (t = ml->timers; t; t = n) {
		n = t->next;
		free(t);
	}
	close(ml->wake_fd);
	free(ml);
}

int mainloop_add_fd(struct mainloop *ml, int fd, short events, ml_fd_cb cb, void *ud)
{
	int i;

	for (i = 0; i < ml->nwatches; i++)
		if (ml->watches[i].fd < 0)
			break;
	if (i == ml->nwatches) {
		if (ml->nwatches == MAX_FDS) {
			LOGE("mainloop", "%s: too many fds", ml->name);
			return -1;
		}
		ml->nwatches++;
	}
	ml->watches[i] = (struct watch){ fd, events, cb, ud };
	return 0;
}

void mainloop_remove_fd(struct mainloop *ml, int fd)
{
	int i;

	for (i = 0; i < ml->nwatches; i++)
		if (ml->watches[i].fd == fd)
			ml->watches[i].fd = -1;
}

struct ml_timer *mainloop_add_timer(struct mainloop *ml, uint32_t delay_ms, uint32_t period_ms,
				    ml_timer_cb cb, void *ud)
{
	struct ml_timer *t = calloc(1, sizeof(*t));

	if (!t)
		return NULL;
	t->due = now_ms() + delay_ms;
	t->period = period_ms;
	t->cb = cb;
	t->ud = ud;
	t->next = ml->timers;
	ml->timers = t;
	return t;
}

void mainloop_timer_cancel(struct mainloop *ml, struct ml_timer *t)
{
	(void)ml;
	if (t)
		t->dead = true;		/* freed by the next sweep, safe inside callbacks */
}

void mainloop_set_prepare(struct mainloop *ml, ml_prepare_cb cb, void *ud)
{
	ml->prepare = cb;
	ml->prepare_ud = ud;
}

/* Milliseconds until the nearest timer, -1 if none */
static int timers_wait(struct mainloop *ml, uint64_t now)
{
	struct ml_timer *t;
	int64_t best = -1;

	for (t = ml->timers; t; t = t->next) {
		int64_t d;

		if (t->dead)
			continue;
		d = t->due > now ? (int64_t)(t->due - now) : 0;
		if (best < 0 || d < best)
			best = d;
	}
	return best > INT32_MAX ? INT32_MAX : (int)best;
}

static void timers_run(struct mainloop *ml)
{
	uint64_t now = now_ms();
	struct ml_timer *t, **pp;

	/* New timers are pushed at the head, so ones added by callbacks
	 * here are not visited until the next iteration */
	for (t = ml->timers; t; t = t->next) {
		if (t->dead || t->due > now)
			continue;
		if (t->period)
			t->due = now + t->period;
		else
			t->dead = true;
		t->cb(t->ud);
	}
	for (pp = &ml->timers; *pp;) {
		t = *pp;
		if (t->dead) {
			*pp = t->next;
			free(t);
		} else {
			pp = &t->next;
		}
	}
}

void mainloop_run(struct mainloop *ml)
{
	struct pollfd pfd[MAX_FDS];
	struct watch *map[MAX_FDS];

	while (!atomic_load(&ml->quit)) {
		int timeout = -1, tw, n, i, nfds = 0;

		if (ml->prepare)
			timeout = ml->prepare(ml->prepare_ud);
		if (atomic_load(&ml->quit))
			break;
		tw = timers_wait(ml, now_ms());
		if (tw >= 0 && (timeout < 0 || tw < timeout))
			timeout = tw;

		for (i = 0; i < ml->nwatches; i++) {
			if (ml->watches[i].fd < 0)
				continue;
			pfd[nfds].fd = ml->watches[i].fd;
			pfd[nfds].events = ml->watches[i].events;
			pfd[nfds].revents = 0;
			map[nfds++] = &ml->watches[i];
		}
		n = poll(pfd, nfds, timeout);
		if (n < 0 && errno != EINTR) {
			LOGE("mainloop", "%s: poll: %s", ml->name, strerror(errno));
			break;
		}
		for (i = 0; n > 0 && i < nfds; i++) {
			struct watch *w = map[i];

			/* the watch may have been removed by an earlier callback */
			if (!pfd[i].revents || w->fd != pfd[i].fd)
				continue;
			w->cb(w->fd, pfd[i].revents, w->ud);
		}
		timers_run(ml);
	}
}

void mainloop_quit(struct mainloop *ml)
{
	uint64_t one = 1;

	atomic_store(&ml->quit, true);
	if (write(ml->wake_fd, &one, sizeof(one)) < 0)
		LOGW("mainloop", "wake: %s", strerror(errno));
}

bool mainloop_quitting(struct mainloop *ml)
{
	return atomic_load(&ml->quit);
}
