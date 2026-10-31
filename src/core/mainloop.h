/*
 * Per-thread event loop: poll() over registered fds plus timers.
 *
 * Every thread that owns state (control, view) runs its own loop. All
 * callbacks run in the loop's thread. Only mainloop_quit() may be called
 * from another thread.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

struct mainloop;
struct ml_timer;

typedef void (*ml_fd_cb)(int fd, short revents, void *ud);
typedef void (*ml_timer_cb)(void *ud);
/* Called before each poll(); returns the longest allowed wait in ms, -1 = no limit */
typedef int (*ml_prepare_cb)(void *ud);

struct mainloop *mainloop_new(const char *name);
void mainloop_free(struct mainloop *ml);

int mainloop_add_fd(struct mainloop *ml, int fd, short events, ml_fd_cb cb, void *ud);
void mainloop_remove_fd(struct mainloop *ml, int fd);

/*
 * Fires after delay_ms, then every period_ms. period_ms == 0 makes a one-shot
 * timer: its handle is invalid once the callback has run.
 */
struct ml_timer *mainloop_add_timer(struct mainloop *ml, uint32_t delay_ms, uint32_t period_ms,
				    ml_timer_cb cb, void *ud);
void mainloop_timer_cancel(struct mainloop *ml, struct ml_timer *t);

void mainloop_set_prepare(struct mainloop *ml, ml_prepare_cb cb, void *ud);

void mainloop_run(struct mainloop *ml);
void mainloop_quit(struct mainloop *ml);	/* any thread */
bool mainloop_quitting(struct mainloop *ml);
