/*
 * Inside the view core: its state and what its parts call of each other.
 * view.c runs the thread (devices, the loop, subscriptions, notifications,
 * snapshots), view_input.c routes the gamepad (the power key, the quit
 * combo, F5, the volume keys, UI keys or a screen's grab), view_video.c
 * owns the video layer (sources, games, frame times) and the stream.
 */
#pragma once

#include <pthread.h>
#include <semaphore.h>

#include "view.h"
#include "core/frametimes.h"

#define MAX_SUBS 128
#define POWER_QUIET_MS 1000	/* after waking up the key's events are the wake press */

struct mailbox;
struct testpattern;
struct game_proc;
struct ml_timer;

struct vsub {
	int id;			/* 0: free */
	uint32_t sections;
	view_cb cb;
	void *self;
};

/* The view thread's state, shared by view.c, view_input.c, view_video.c */
struct view_state {
	const struct view_backend_ops *be;
	const struct platform *pf;
	pthread_t thread;
	sem_t ready;
	int status;
	struct mainloop *ml;
	struct hal_display *display;
	void *inputs[PLATFORM_MAX_INPUTS];
	struct gui_ctx snap;
	struct vsub subs[MAX_SUBS];
	int nsubs, next_id;
	view_grab_cb grab;
	void *grab_ud;
	bool quit_sent;
	struct ml_timer *quit_hold;	/* the quit combo is held: quits when it fires */
	bool quit_game;			/* ...ends the game in front instead */
	struct ml_timer *power_hold;	/* the power key is down: warns, then turns off */
	bool power_down;		/* a press of it that counts is going on */
	bool power_warned;		/* ...and has gone past POWER_SHORT_MS */
	struct ml_timer *deep_sleep;	/* the screen is off: real sleep when it fires */
	bool screen_off;		/* first stage of sleep: no input but the power key */
	bool asleep;			/* ACT_SUSPEND posted, NOTIFY_RESUMED not yet */
	uint64_t power_quiet_until;	/* now_ms() */
	struct ml_timer *f5_hold;	/* F5 is down: the monitor when it fires */
	bool f5_down;			/* a press of F5 kept back from the rest */
	bool f5_held;			/* ...that went on long enough: not a press */
	enum video_source video_src;
	struct mailbox *video_mb;
	struct testpattern *video_tp;
	struct game_proc *video_game;	/* VIDEO_GAME: the process of 'game' */
	struct game_ref game, game_sel;	/* in the layer; to start next */
	bool video_minimized;		/* video_game is stopped in the background */
	unsigned video_frames;
	struct frametimes frame_times;	/* of video_frames, since it was set or came back */
	struct {			/* hitches since the source was set */
		uint64_t last_us;	/* the last frame, 0: none yet */
		unsigned over50, over100;	/* frames later than that many ms */
		uint32_t worst_us;
		uint64_t log_us;	/* BARE_APP_FRAME_STATS: the last line, 0 off */
		bool log_late;		/* BARE_APP_FRAME_STATS=2: also each late frame */
	} frame_stats;
	view_video_end_cb video_end;
	void *video_end_ud;
#ifdef CONFIG_STREAM
	struct ml_timer *stream_tick;	/* composes a frame for the stream */
	char stream_play[32], stream_cap[32];	/* the loopback, "" none */
#endif
};

extern struct view_state vcore;

/* view.c */
void view_notify_backend(enum notify_kind kind, int value);

/* view_input.c */
void view_on_gamepad(const struct gp_event *ev, void *ud);
void view_screen_on(void);

/* view_ctl.c: commands for tests and tools in a FIFO */
struct mainloop;
void view_ctl_open(struct mainloop *ml);
void view_ctl_close(void);

/* view_video.c */
bool view_game_in_front(void);
bool view_hid_game_in_front(bool mouse);
void view_game_quit(void);
void view_game_unminimize(void);
/* The stream follows the setting (CTX_SETTINGS changed); stopped at the end */
void view_stream_follow(void);
void view_stream_end(void);
