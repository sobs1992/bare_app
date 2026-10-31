/*
 * GUI context: the only state shared by control and view.
 *
 * control writes it between ctx_lock() and ctx_unlock(changed sections);
 * the view thread is woken and takes a snapshot with ctx_snapshot(), so it
 * never reads the live copy. Several writes between two snapshots arrive
 * as one change with the latest values.
 *
 * Values only (no pointers) and meaning only (no colours, icons or
 * formatted text): presentation belongs to the view.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum screen_id {
	SCREEN_HOME,
	SCREEN_INPUT_TEST,
	SCREEN_SETTINGS,
	SCREEN_LAYERS,
	SCREEN_DOOM,
	SCREEN_WIFI,
	SCREEN_WIFI_KEY,	/* key for the network in wifi.asked */
	SCREEN_COUNT
};

enum ctx_section {
	CTX_POWER	= 1u << 0,
	CTX_DISPLAY	= 1u << 1,
	CTX_NAV		= 1u << 2,
	CTX_SYSTEM	= 1u << 3,
	CTX_SETTINGS	= 1u << 4,
	CTX_AUDIO	= 1u << 5,
	CTX_WIFI	= 1u << 6,
	CTX_ALL		= 0xffffffffu,
};

#define NAV_DEPTH 8
#define WIFI_SSID_MAX 32
#define WIFI_MAX_NETS 16

/* Where the sound goes: the setting */
enum audio_output { AUDIO_OUT_AUTO, AUDIO_OUT_SPEAKER, AUDIO_OUT_HEADPHONES, AUDIO_OUT_COUNT };

enum wifi_link { WIFI_OFF, WIFI_DISCONNECTED, WIFI_CONNECTING, WIFI_CONNECTED };

struct wifi_net {
	char ssid[WIFI_SSID_MAX + 1];
	int signal;		/* percent */
	bool secured;		/* needs a key */
	bool saved;		/* known: connects without asking */
};

struct gui_ctx {
	struct {
		bool available;		/* the platform has a battery driver */
		bool present;
		int capacity;		/* percent, -1 unknown */
		bool charging, full, online;
	} power;
	struct {
		bool has_backlight;
		int brightness;		/* percent */
	} display;
	struct {
		enum screen_id stack[NAV_DEPTH];
		int depth;		/* stack[depth - 1] is shown */
	} nav;
	struct {
		char platform[16];
		char model[64];
	} system;
	struct {
		char lang[16];		/* UI language code, "en", "ru"... */
	} settings;
	struct {
		bool available;		/* the platform has a mixer */
		int volume;		/* percent, -1 unknown */
		bool can_route;		/* the output can be chosen */
		enum audio_output output;
		bool headphones;	/* the sound goes to the headphones now */
		bool has_jack, jack;	/* plug detection, headphones plugged in */
	} audio;
	struct {
		bool available;		/* the Wi-Fi daemon answers */
		bool enabled;		/* the setting */
		enum wifi_link link;
		char ssid[WIFI_SSID_MAX + 1];	/* connected or connecting to */
		char ip[16];		/* "" until DHCP gives one */
		bool scanning;
		int count;		/* networks in range, connected one first */
		struct wifi_net nets[WIFI_MAX_NETS];
		char asked[WIFI_SSID_MAX + 1];	/* the key screen asks for this one */
	} wifi;
};

int ctx_init(void);
void ctx_deinit(void);

/* control side */
struct gui_ctx *ctx_lock(void);
void ctx_unlock(uint32_t changed);

/* view side: copies the context if anything changed, returns the mask */
uint32_t ctx_snapshot(struct gui_ctx *snap);
int ctx_fd(void);		/* readable when there is something new */
void ctx_ack(void);

static inline enum screen_id ctx_screen(const struct gui_ctx *c)
{
	return c->nav.depth > 0 ? c->nav.stack[c->nav.depth - 1] : SCREEN_HOME;
}
