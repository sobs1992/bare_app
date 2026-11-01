/*
 * Hardware abstraction: what a driver must provide. Nothing here knows
 * about the GUI toolkit; drivers know nothing about the GUI either.
 *
 * Display and input drivers are opened in the view thread and may register
 * fds/timers in its loop. Power and backlight drivers are used from the
 * control thread.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

struct mainloop;

/* ---- display ---------------------------------------------------------- */

/* Like fbcon=rotate:N: how far the picture is turned clockwise on the panel */
enum hal_rotation { HAL_ROT_0, HAL_ROT_90, HAL_ROT_180, HAL_ROT_270 };
#define HAL_ROT_AUTO (-1)	/* take it from the text console (fbcon) */

enum layer_role {
	LAYER_VIDEO,		/* bottom: camera, GPU output */
	LAYER_UI,		/* GUI toolkit */
	LAYER_OSD,		/* top: overlays, cursor */
	LAYER_COUNT
};

/* Inclusive rectangle in logical (as the user sees it) coordinates */
struct hal_rect {
	int x1, y1, x2, y2;
};

enum hal_format {
	HAL_FMT_XRGB8888,	/* opaque */
	HAL_FMT_ARGB8888,	/* straight alpha: layers below show through */
};

/* CPU-accessible buffer in logical orientation */
struct hal_fb {
	uint32_t *pixels;
	int width, height;
	int stride;		/* bytes per row */
	enum hal_format format;	/* chosen by the driver for this layer */
};

struct hal_display_info {
	int width, height;	/* logical size, after rotation */
	int rotation;		/* enum hal_rotation applied by the driver */
};

struct hal_display;
struct hal_layer;

struct hal_display_ops {
	const char *name;
	struct hal_display *(*open)(const void *params, struct mainloop *ml);
	void (*close)(struct hal_display *d);
	void (*info)(struct hal_display *d, struct hal_display_info *info);
	/* NULL if the display has no such layer */
	struct hal_layer *(*layer)(struct hal_display *d, enum layer_role role);
	/*
	 * The buffer to draw the next frame into. Its content is kept between
	 * frames, so only damaged areas need to be redrawn.
	 */
	struct hal_fb *(*layer_acquire)(struct hal_layer *l);
	void (*layer_damage)(struct hal_layer *l, const struct hal_rect *r);
	/* Show what was drawn: damaged areas reach the panel (rotated) */
	void (*layer_submit)(struct hal_layer *l);
	/* Optional: write the composed layers (logical orientation) as PPM */
	int (*snapshot)(struct hal_display *d, const char *path);
};

/* Drivers embed these as the first member of their own structs */
struct hal_display {
	const struct hal_display_ops *ops;
};

struct hal_layer {
	struct hal_display *display;
	enum layer_role role;
};

static inline struct hal_layer *hal_display_layer(struct hal_display *d, enum layer_role role)
{
	return d->ops->layer ? d->ops->layer(d, role) : 0;
}

static inline void hal_display_info(struct hal_display *d, struct hal_display_info *info)
{
	d->ops->info(d, info);
}

static inline struct hal_fb *hal_layer_acquire(struct hal_layer *l)
{
	return l->display->ops->layer_acquire(l);
}

static inline void hal_layer_damage(struct hal_layer *l, const struct hal_rect *r)
{
	l->display->ops->layer_damage(l, r);
}

static inline void hal_layer_submit(struct hal_layer *l)
{
	l->display->ops->layer_submit(l);
}

/* ---- input ------------------------------------------------------------ */

/*
 * An input driver turns device events into logical gamepad events with
 * input_report_button()/input_report_axis() (core/input.h).
 */
struct hal_input_ops {
	const char *name;
	void *(*open)(const void *params, struct mainloop *ml);
	void (*close)(void *h);
};

/* ---- power supply ----------------------------------------------------- */

struct hal_power_state {
	bool present;		/* a battery is there */
	int capacity;		/* percent, -1 unknown */
	bool charging;
	bool full;
	bool online;		/* external power connected */
};

struct hal_power_ops {
	const char *name;
	void *(*open)(const void *params);
	int (*read)(void *h, struct hal_power_state *st);
	void (*close)(void *h);
};

/* ---- backlight -------------------------------------------------------- */

struct hal_backlight_ops {
	const char *name;
	void *(*open)(const void *params);
	int (*get)(void *h);			/* percent, -1 on error */
	int (*set)(void *h, int percent);
	int (*power)(void *h, bool on);		/* screen off and back, level kept; may be NULL */
	void (*close)(void *h);
};

/* ---- audio mixer ------------------------------------------------------ */

enum hal_audio_route { HAL_ROUTE_SPEAKER, HAL_ROUTE_HEADPHONES };

struct hal_mixer_state {
	int volume;		/* percent, -1 unknown */
	bool has_route;		/* the output can be switched */
	enum hal_audio_route route;
	bool has_jack;		/* a headphone plug is detected */
	bool jack;		/* headphones are plugged in */
};

struct hal_mixer_ops {
	const char *name;
	void *(*open)(const void *params);
	/* readable when something changed the mixer (the jack, another program) */
	int (*fd)(void *h);
	/* takes pending change events too */
	int (*read)(void *h, struct hal_mixer_state *st);
	int (*set_volume)(void *h, int percent);
	int (*set_route)(void *h, enum hal_audio_route route);
	void (*close)(void *h);
};

/* ---- sound output ----------------------------------------------------- */

struct hal_pcm_ops {
	const char *name;
	/* 16-bit stereo; *rate is a wish, the driver puts what it took there */
	void *(*open)(const void *params, int *rate);
	int (*write)(void *h, const int16_t *frames, int count);	/* blocks */
	void (*drain)(void *h);		/* returns when all is played */
	void (*close)(void *h);
};

/* ---- Wi-Fi ------------------------------------------------------------ */

#define HAL_SSID_MAX 32		/* bytes, without the terminating NUL */
#define HAL_WIFI_KEY_MAX 63	/* WPA passphrase */

enum hal_wifi_link {
	HAL_WIFI_DOWN,		/* switched off, or no interface */
	HAL_WIFI_DISCONNECTED,	/* includes scanning */
	HAL_WIFI_CONNECTING,	/* authentication, association, handshake */
	HAL_WIFI_CONNECTED,
};

struct hal_wifi_status {
	enum hal_wifi_link link;
	char ssid[HAL_SSID_MAX + 1];
	char ip[16];		/* IPv4, "" until DHCP gives one */
};

struct hal_wifi_net {
	char ssid[HAL_SSID_MAX + 1];
	int signal;		/* percent */
	bool secured;		/* needs a key */
	bool saved;		/* known: connects without asking */
};

/* What process() saw */
enum hal_wifi_event {
	HAL_WIFI_EV_STATUS	= 1u << 0,	/* read the status again */
	HAL_WIFI_EV_SCAN	= 1u << 1,	/* new scan results */
	HAL_WIFI_EV_WRONG_KEY	= 1u << 2,	/* the network refused the key */
	HAL_WIFI_EV_LOST	= 1u << 3,	/* the daemon went away: close, open later */
};

struct hal_wifi_ops {
	const char *name;
	/* NULL when the interface or its daemon is not there (yet) */
	void *(*open)(const void *params);
	int (*fd)(void *h);			/* readable: call process() */
	uint32_t (*process)(void *h);		/* hal_wifi_event mask */
	int (*status)(void *h, struct hal_wifi_status *st);
	/* Results come later with HAL_WIFI_EV_SCAN */
	int (*scan)(void *h);
	/* Last scan, one entry per network name, strongest first; returns the count */
	int (*networks)(void *h, struct hal_wifi_net *nets, int max);
	/* key NULL: an open network or a saved one with its saved key */
	int (*connect)(void *h, const char *ssid, const char *key);
	int (*forget)(void *h, const char *ssid);
	int (*set_enabled)(void *h, bool on);
	void (*close)(void *h);
};
