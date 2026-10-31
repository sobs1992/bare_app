/*
 * System services (control thread). They own the control-side HAL devices
 * and publish what they learn on the bus.
 */
#pragma once

#include <stdbool.h>

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
