/*
 * Wi-Fi through the control socket of a running wpa_supplicant (no
 * libwpa_client). The supplicant keeps the networks and their keys in its
 * own config file, which needs update_config=1 to be saved. Addresses are
 * the system's business: a DHCP client has to follow the joined network
 * (on the OGA a wpa_cli action script restarts udhcpc).
 */
#pragma once

#include <stddef.h>

#include "hal/hal.h"

struct wifi_wpa_params {
	const char *ctrl;		/* "/var/run/wpa_supplicant/wlan0" */
};

extern const struct hal_wifi_ops wifi_wpa;

/* Exposed for the unit tests */

/* Undo the escaping of names in replies (printf_encode: \\ \" \n \xNN...) */
void wpa_unescape(const char *in, char *out, size_t size);
/* SCAN_RESULTS + LIST_NETWORKS replies -> list as hal_wifi_ops.networks() gives it */
int wpa_parse_scan(const char *results, const char *saved, struct hal_wifi_net *nets, int max);
