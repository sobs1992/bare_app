/* Host unit tests for the toolkit independent core. */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/input.h"
#include "core/joystick.h"
#include "core/mainloop.h"
#include "core/msgq.h"
#include "core/uevent.h"
#include "core/volume.h"
#include "drivers/blit.h"
#include "media/mailbox.h"
#include "gui/action.h"
#include "gui/control/control.h"
#include "gui/ctx.h"
#include "services/config.h"
#if defined(CONFIG_WIFI) && defined(CONFIG_PLATFORM_OGA)
#include "drivers/wifi_wpa.h"
#endif

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

#define NEAR(a, b) (fabsf((a) - (b)) < 1e-3f)

static void test_joystick(void)
{
	/* OGA rev 1.1 X axis: 172..772, resting off-center */
	struct joy_axis_cal c = { .min = 172, .max = 772, .center = 500, .deadzone = 0.1f };

	CHECK(NEAR(joy_normalize(&c, 500), 0));
	CHECK(NEAR(joy_normalize(&c, 772), 1));
	CHECK(NEAR(joy_normalize(&c, 172), -1));
	CHECK(NEAR(joy_normalize(&c, 900), 1));		/* clamped */
	CHECK(NEAR(joy_normalize(&c, 520), 0));		/* inside dead zone */
	/* half way up (636) is 0.5 raw -> (0.5 - 0.1) / 0.9 after the dead zone */
	CHECK(NEAR(joy_normalize(&c, 636), 0.4f / 0.9f));
	c.invert = true;
	CHECK(NEAR(joy_normalize(&c, 772), -1));

	CHECK(joy_direction(0, 0.6f, 0.5f, 0.3f) == 1);
	CHECK(joy_direction(1, 0.4f, 0.5f, 0.3f) == 1);		/* hysteresis holds */
	CHECK(joy_direction(1, 0.2f, 0.5f, 0.3f) == 0);
	CHECK(joy_direction(0, 0.4f, 0.5f, 0.3f) == 0);
	CHECK(joy_direction(1, -0.6f, 0.5f, 0.3f) == -1);	/* flick to the other side */
	CHECK(joy_direction(-1, -0.4f, 0.5f, 0.3f) == -1);
}

static void test_msgq(void)
{
	struct msgq *q = msgq_new(sizeof(int), 3);
	int v, i;

	for (i = 1; i <= 3; i++)
		CHECK(msgq_post(q, &i));
	i = 4;
	CHECK(!msgq_post(q, &i));		/* full */
	for (i = 1; i <= 3; i++) {
		CHECK(msgq_pop(q, &v));
		CHECK(v == i);			/* FIFO */
	}
	CHECK(!msgq_pop(q, &v));
	msgq_free(q);
}

static void test_ctx(void)
{
	struct gui_ctx snap;
	struct gui_ctx *c;

	CHECK(ctx_init() == 0);
	CHECK(ctx_snapshot(&snap) == CTX_ALL);	/* first snapshot: everything */
	CHECK(ctx_snapshot(&snap) == 0);

	/* two writes before the view looks: one change, latest value */
	c = ctx_lock();
	c->power.capacity = 50;
	ctx_unlock(CTX_POWER);
	c = ctx_lock();
	c->power.capacity = 49;
	c->display.brightness = 70;
	ctx_unlock(CTX_POWER | CTX_DISPLAY);
	CHECK(ctx_snapshot(&snap) == (CTX_POWER | CTX_DISPLAY));
	CHECK(snap.power.capacity == 49);
	CHECK(snap.display.brightness == 70);
	ctx_deinit();
}

static void test_nav(void)
{
	struct control_env env = { 0 };
	struct gui_action a;
	struct gui_ctx snap;

	CHECK(ctx_init() == 0);
	ctl_nav.init(&env);
	ctx_snapshot(&snap);
	CHECK(ctx_screen(&snap) == SCREEN_HOME);

	a = (struct gui_action){ .type = ACT_BACK, .arg = 0 };
	ctl_nav.on_action(&a);			/* home has nothing to go back to */
	CHECK(ctx_snapshot(&snap) == 0);

	a = (struct gui_action){ .type = ACT_OPEN_SCREEN, .arg = SCREEN_SETTINGS };
	ctl_nav.on_action(&a);
	ctl_nav.on_action(&a);			/* same screen twice: one entry */
	CHECK(ctx_snapshot(&snap) == CTX_NAV);
	CHECK(ctx_screen(&snap) == SCREEN_SETTINGS && snap.nav.depth == 2);

	a = (struct gui_action){ .type = ACT_OPEN_SCREEN, .arg = SCREEN_COUNT };
	ctl_nav.on_action(&a);			/* invalid id ignored */
	CHECK(ctx_snapshot(&snap) == 0);

	a = (struct gui_action){ .type = ACT_BACK, .arg = 0 };
	ctl_nav.on_action(&a);
	CHECK(ctx_snapshot(&snap) == CTX_NAV);
	CHECK(ctx_screen(&snap) == SCREEN_HOME && snap.nav.depth == 1);
	ctx_deinit();
}

static void test_uevent(void)
{
	static const char msg[] =
		"add@/devices/platform/adc-joystick/input/input5/event5\0"
		"ACTION=add\0DEVPATH=/devices/platform/adc-joystick/input/input5/event5\0"
		"SUBSYSTEM=input\0MAJOR=13\0MINOR=69\0DEVNAME=input/event5\0SEQNUM=1234";
	static const char udev[] = "libudev\0\xfe\xed\xca\xfe";
	char buf[sizeof(msg)];
	struct uevent ev;

	memcpy(buf, msg, sizeof(msg));
	CHECK(uevent_parse(buf, sizeof(buf), &ev) == 0);
	CHECK(!strcmp(ev.action, "add") && !strcmp(ev.subsystem, "input"));
	CHECK(ev.devname && !strcmp(ev.devname, "input/event5"));
	CHECK(ev.name == NULL);

	memcpy(buf, udev, sizeof(udev));
	CHECK(uevent_parse(buf, sizeof(udev), &ev) < 0);	/* udev monitor format ignored */
}

static void test_config(void)
{
	const char *path = "test_config.conf";
	struct mainloop *ml = mainloop_new("test");
	FILE *f;

	unlink(path);
	config_open(ml, path);
	CHECK(config_get("lang") == NULL);
	CHECK(config_get_int("brightness", -1) == -1);
	config_set("lang", "ru");
	config_set_int("brightness", 55);
	config_close();				/* writes the delayed save now */

	f = fopen(path, "r");
	CHECK(f != NULL);
	if (f)
		fclose(f);
	config_open(ml, path);
	CHECK(config_get("lang") && !strcmp(config_get("lang"), "ru"));
	CHECK(config_get_int("brightness", -1) == 55);
	config_close();
	unlink(path);

	config_open(ml, NULL);			/* memory only */
	config_set("lang", "en");
	CHECK(!strcmp(config_get("lang"), "en"));
	config_close();
	mainloop_free(ml);
}

/* Producer fills every pixel of a frame with its number; a torn frame
 * would mix two numbers, a lost order would go backwards. */
#define MB_FRAMES 3000

static void *mb_producer(void *arg)
{
	struct mailbox *mb = arg;
	uint32_t n;
	int i;

	for (n = 1; n <= MB_FRAMES; n++) {
		struct hal_fb *fb = mailbox_begin(mb);

		for (i = 0; i < fb->width * fb->height; i++)
			fb->pixels[i] = n;
		mailbox_end(mb);
	}
	return NULL;
}

static void test_mailbox(void)
{
	struct mailbox *mb = mailbox_new(64, 32);
	uint32_t last = 0, taken = 0;
	pthread_t t;
	int i, torn = 0, backwards = 0;

	CHECK(mb && mailbox_take(mb) == NULL);
	pthread_create(&t, NULL, mb_producer, mb);
	while (last < MB_FRAMES) {
		struct hal_fb *fb = mailbox_take(mb);

		if (!fb)
			continue;
		for (i = 1; i < fb->width * fb->height; i++)
			torn += fb->pixels[i] != fb->pixels[0];
		backwards += fb->pixels[0] <= last;
		last = fb->pixels[0];
		taken++;
	}
	pthread_join(t, NULL);
	CHECK(torn == 0);
	CHECK(backwards == 0);
	CHECK(taken + mailbox_dropped(mb) == MB_FRAMES);
	mailbox_free(mb);
}

/* ---- input: repeat and stick -> D-pad, driven by a real loop ---------- */

static struct {
	int down, up, repeats;
	enum gp_button last;
} seen;

static void on_input(const struct gp_event *ev, void *ud)
{
	(void)ud;
	if (ev->type != GP_EV_BUTTON)
		return;
	seen.last = ev->code;
	if (ev->repeat)
		seen.repeats++;
	else if (ev->pressed)
		seen.down++;
	else
		seen.up++;
}

static void release_right(void *ud)
{
	input_report_button(GP_RIGHT, false);
	mainloop_quit(ud);
}

static void test_input(void)
{
	struct input_config cfg = { .repeat_delay_ms = 50, .repeat_period_ms = 20,
				    .stick_press = 0.5f, .stick_release = 0.3f };
	struct mainloop *ml = mainloop_new("test");

	input_init(ml, &cfg, on_input, NULL);

	/* stick: past press -> UP, inside hysteresis -> still UP, back -> released */
	input_report_axis(GP_AXIS_LY, -0.7f);
	CHECK(seen.down == 1 && seen.last == GP_UP && (input_buttons() & GP_BIT(GP_UP)));
	input_report_axis(GP_AXIS_LY, -0.4f);
	CHECK(seen.up == 0);
	input_report_axis(GP_AXIS_LY, 0);
	CHECK(seen.up == 1 && !(input_buttons() & GP_BIT(GP_UP)));

	/* D-pad held stays pressed even when the stick lets go of the same direction */
	input_report_button(GP_UP, true);
	input_report_axis(GP_AXIS_LY, -0.9f);
	input_report_axis(GP_AXIS_LY, 0);
	CHECK(input_buttons() & GP_BIT(GP_UP));
	input_report_button(GP_UP, false);

	/* held direction repeats: 50 ms delay, then every 20 ms; hold for 130 ms */
	memset(&seen, 0, sizeof(seen));
	input_report_button(GP_RIGHT, true);
	mainloop_add_timer(ml, 130, 0, release_right, ml);
	mainloop_run(ml);
	CHECK(seen.down == 1 && seen.up == 1);
	CHECK(seen.repeats >= 3 && seen.repeats <= 5);

	input_deinit();
	mainloop_free(ml);
}

static void test_volume(void)
{
	/* rk817 "Master Playback Volume": -95..0 dB in 0.01 dB */
	int p, db, back;

	CHECK(volume_db_to_percent(-9500, -9500, 0) == 0);
	CHECK(volume_db_to_percent(0, -9500, 0) == 100);
	CHECK(volume_percent_to_db(0, -9500, 0) == -9500);
	CHECK(volume_percent_to_db(100, -9500, 0) == 0);
	/* half way sounds like half: about -18 dB, nowhere near -47.5 */
	db = volume_percent_to_db(50, -9500, 0);
	CHECK(db > -2000 && db < -1600);
	for (p = 0; p <= 100; p++) {
		db = volume_percent_to_db(p, -9500, 0);
		back = volume_db_to_percent(db, -9500, 0);
		CHECK(back == p);
		if (p > 0)
			CHECK(db > volume_percent_to_db(p - 1, -9500, 0));
	}
	CHECK(volume_db_to_percent(-500, 0, 0) == 100);	/* no range */
}

#if defined(CONFIG_WIFI) && defined(CONFIG_PLATFORM_OGA)
static void test_wpa(void)
{
	static const char results[] =
		"bssid / frequency / signal level / flags / ssid\n"
		"10:7c:61:80:07:40\t2432\t-48\t[WPA2-PSK-CCMP][WPS][ESS]\t46B\n"
		"10:7c:61:80:07:41\t2412\t-80\t[WPA2-PSK-CCMP][ESS]\t46B\n"
		"aa:bb:cc:00:00:01\t2437\t-70\t[ESS]\tCafe \\\"Free\\\"\n"
		"aa:bb:cc:00:00:02\t2462\t-90\t[RSN-SAE-CCMP][ESS]\t\\xd0\\x94\\xd0\\xbe\\xd0\\xbc\n"
		"aa:bb:cc:00:00:03\t2462\t-60\t[WPA2-PSK-CCMP][ESS]\t\\x00\\x00\n"
		"aa:bb:cc:00:00:04\t2462\t-30\t[WEP][ESS]\t\n";
	static const char saved[] =
		"network id / ssid / bssid / flags\n"
		"0\t46B\tany\t[CURRENT]\n"
		"1\tsobs1992\tany\t\n";
	struct hal_wifi_net nets[8];
	char out[40];
	int n;

	wpa_unescape("a\\\\b\\\"c\\x41\\n", out, sizeof(out));
	CHECK(!strcmp(out, "a\\b\"cA\n"));
	wpa_unescape("abcdef", out, 4);
	CHECK(!strcmp(out, "abc"));

	n = wpa_parse_scan(results, saved, nets, 8);
	CHECK(n == 3);		/* 46B once, hidden ones dropped */
	CHECK(!strcmp(nets[0].ssid, "46B") && nets[0].signal == 100 && nets[0].secured &&
	      nets[0].saved);
	CHECK(!strcmp(nets[1].ssid, "Cafe \"Free\"") && nets[1].signal == 60 && !nets[1].secured &&
	      !nets[1].saved);
	CHECK(!strcmp(nets[2].ssid, "\xd0\x94\xd0\xbe\xd0\xbc") && nets[2].signal == 20 &&
	      nets[2].secured);
	CHECK(wpa_parse_scan(results, NULL, nets, 1) == 1);
}
#endif

/* blit_rotated() against a pixel by pixel turn, as fbcon=rotate:N */
static void test_blit(void)
{
	enum { LW = 37, LH = 21, PAD = 3 };	/* odd sizes: tiles end mid-way */
	static const struct hal_rect rects[] = {
		{ 0, 0, LW - 1, LH - 1 }, { 5, 3, 30, 19 }, { 0, 0, 0, 0 },
		{ LW - 1, LH - 1, LW - 1, LH - 1 }, { 2, 0, 2, LH - 1 }, { 0, 7, LW - 1, 7 },
	};
	static uint32_t src_px[LH][LW + PAD];
	static uint8_t out[LW * (LH + PAD) * 4], want[sizeof(out)];
	struct hal_fb src = { &src_px[0][0], LW, LH, (LW + PAD) * 4, 0 };
	int x, y, rot, mode, i;

	for (y = 0; y < LH; y++)
		for (x = 0; x < LW + PAD; x++)
			src_px[y][x] = (uint32_t)(x * 2654435761u ^ y * 40503u) | (x % 3 ? 0xff000000 : 0);
	for (rot = 0; rot < 4; rot++) {
		for (mode = BLIT_COPY; mode <= BLIT_RGB565; mode++) {
			int bpp = mode == BLIT_RGB565 ? 2 : 4;
			int nw = rot & 1 ? LH : LW, nh = rot & 1 ? LW : LH;
			struct blit_dst dst = { out, nw, nh, (nw + PAD) * bpp };

			for (i = 0; i < (int)(sizeof(rects) / sizeof(rects[0])); i++) {
				const struct hal_rect *r = &rects[i];
				struct blit_dst ref = { want, nw, nh, dst.stride };

				memset(out, 0x5a, sizeof(out));
				memset(want, 0x5a, sizeof(want));
				for (y = r->y1; y <= r->y2; y++) {
					for (x = r->x1; x <= r->x2; x++) {
						struct hal_fb one = { &src_px[y][x], 1, 1, 4, 0 };
						int nx = rot == 0 ? x : rot == 1 ? nw - 1 - y : rot == 2 ? nw - 1 - x : y;
						int ny = rot == 0 ? y : rot == 1 ? x : rot == 2 ? nh - 1 - y : nh - 1 - x;

						/* the conversion of one pixel, placed by hand */
						blit_rotated(&(struct blit_dst){ want + ny * ref.stride + nx * bpp, 1, 1, bpp },
							     &one, &(struct hal_rect){ 0, 0, 0, 0 }, 0, mode);
					}
				}
				blit_rotated(&dst, &src, r, rot, mode);
				CHECK(memcmp(out, want, sizeof(out)) == 0);
			}
		}
	}
}

static void test_damage(void)
{
	struct damage d = { 0 };
	struct hal_rect all = { 0, 0, 99, 49 }, part = { 10, 10, 20, 20 }, other = { 30, 5, 40, 8 };

	damage_add(&d, &part, 100, 50);
	damage_add(&d, &other, 100, 50);
	CHECK(d.n == 2);
	damage_add(&d, &part, 100, 50);		/* covered: nothing new */
	CHECK(d.n == 2);
	damage_add(&d, &all, 100, 50);		/* covers both */
	CHECK(d.n == 1 && memcmp(&d.r[0], &all, sizeof(all)) == 0);
	damage_add(&d, &all, 100, 50);		/* the next frame, the same area */
	CHECK(d.n == 1);
	damage_add(&d, &(struct hal_rect){ -5, -5, 200, 200 }, 100, 50);	/* clipped to the same */
	CHECK(d.n == 1);
}

int main(void)
{
	test_joystick();
	test_msgq();
	test_ctx();
	test_nav();
	test_uevent();
	test_config();
	test_mailbox();
	test_input();
	test_volume();
	test_blit();
	test_damage();
#if defined(CONFIG_WIFI) && defined(CONFIG_PLATFORM_OGA)
	test_wpa();
#endif
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("all core tests passed");
	return 0;
}
