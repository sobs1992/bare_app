/*
 * System services (control thread). They own the control-side HAL devices
 * and publish what they learn on the bus.
 */
#pragma once

#include <stdbool.h>

#include "hal/hal.h"

struct mainloop;
struct platform;

int services_start(struct mainloop *ml, const struct platform *pf);
void services_stop(void);

/* Power: polls the battery, posts EV_POWER_CHANGED on every change */
bool power_available(void);

/* Backlight, percent */
bool backlight_available(void);
int backlight_get(void);
int backlight_set(int percent);
int backlight_power(bool on);	/* -1 if the backlight cannot be switched off */

/* Audio mixer: posts EV_MIXER_CHANGED on every change */
bool audio_available(void);
int audio_get(struct hal_mixer_state *st);
int audio_set_volume(int percent);
int audio_set_route(enum hal_audio_route route);

/*
 * Interface sounds (services/sound.c): played by a thread of its own on
 * the platform's PCM device, which is free again a second after the last
 * one. Without a device nothing happens.
 */
enum sound_id { SOUND_VOLUME };
int sound_start(const struct platform *pf);
void sound_stop(void);
void sound_play(enum sound_id id);	/* control thread */

/*
 * Wi-Fi (services/wifi.c): reaches the platform's daemon whenever it is
 * there, posts EV_WIFI_STATUS on changes, EV_WIFI_SCAN when
 * wifi_networks() has new results, EV_WIFI_WRONG_KEY.
 */
int wifi_service_start(struct mainloop *ml, const struct platform *pf);
void wifi_service_stop(void);
bool wifi_available(void);
void wifi_set_enabled(bool on);		/* kept, applied whenever the daemon comes */
int wifi_scan(void);
int wifi_networks(struct hal_wifi_net *nets, int max);
int wifi_connect(const char *ssid, const char *key);	/* key NULL: open or saved */
int wifi_forget(const char *ssid);
