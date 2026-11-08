/* Host unit tests for the toolkit independent core. */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/frametimes.h"
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
#ifdef CONFIG_SYSMON
#include <sys/stat.h>
#include "services/sysmon.h"
#endif
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

/* A stick with a square gate, off center, each half axis its own reach:
 * the farthest point it reaches at angle a (radians) */
static void square_stick(float a, float *x, float *y)
{
	float cx = cosf(a), cy = sinf(a), m = fmaxf(fabsf(cx), fabsf(cy));

	cx /= m;
	cy /= m;
	*x = 0.1f + cx * (cx < 0 ? 0.6f : 0.8f);
	*y = -0.05f + cy * (cy < 0 ? 0.9f : 0.7f);
}

static void test_joystick(void)
{
	/* OGA rev 1.1 X axis: 172..772 */
	struct joy_axis_cal c = { .min = 172, .max = 772 };
	struct joy_stick_cal sc, back;
	struct joy_cal_rec rec;
	char buf[400];
	float x, y, a;
	int i;

	CHECK(NEAR(joy_linear(&c, 472), 0));
	CHECK(NEAR(joy_linear(&c, 772), 1));
	CHECK(NEAR(joy_linear(&c, 172), -1));
	CHECK(NEAR(joy_linear(&c, 900), 1));		/* clamped */
	CHECK(NEAR(joy_linear(&c, 622), 0.5f));
	c.invert = true;
	CHECK(NEAR(joy_linear(&c, 772), -1));

	/* no calibration: from the rest position to the range's ends */
	joy_stick_default(&sc, 0.2f, 0);
	joy_stick_apply(&sc, 0.1f, 0.2f, 0, &x, &y);
	CHECK(NEAR(x, 0) && NEAR(y, 0));
	joy_stick_apply(&sc, 0.1f, 1, 0, &x, &y);
	CHECK(NEAR(x, 1) && NEAR(y, 0));
	joy_stick_apply(&sc, 0.1f, -1, 0, &x, &y);
	CHECK(NEAR(x, -1));
	joy_stick_apply(&sc, 0.1f, 0.25f, 0.05f, &x, &y);	/* round dead zone */
	CHECK(NEAR(x, 0) && NEAR(y, 0));
	joy_stick_apply(&sc, 0.1f, 0.2f, 0.5f, &x, &y);	/* half way down */
	CHECK(NEAR(x, 0) && NEAR(y, 0.4f / 0.9f));

	/* calibration: rest position, then turned along the edge twice */
	joy_cal_begin(&rec, 0.1f, -0.05f);
	CHECK(joy_cal_covered(&rec) == 0);
	for (i = 0; i < 60; i++) {
		square_stick((float)i * 0.07f, &x, &y);
		joy_cal_add(&rec, x, y);
	}
	CHECK(joy_cal_covered(&rec) < JOY_SECTORS);	/* not all the way round yet */
	CHECK(!joy_cal_finish(&rec, &sc));
	for (; i < 200; i++) {
		square_stick((float)i * 0.07f, &x, &y);
		joy_cal_add(&rec, x, y);
	}
	CHECK(joy_cal_covered(&rec) == JOY_SECTORS);
	CHECK(joy_cal_finish(&rec, &sc));
	/* the edge is now the unit circle everywhere, the rest position 0 */
	for (a = 0; a < 6.28f; a += 0.1f) {
		square_stick(a, &x, &y);
		joy_stick_apply(&sc, 0.15f, x, y, &x, &y);
		CHECK(fabsf(hypotf(x, y) - 1) < 0.02f);
	}
	joy_stick_apply(&sc, 0.15f, 0.1f, -0.05f, &x, &y);
	CHECK(NEAR(x, 0) && NEAR(y, 0));
	/* straight right and straight up stay so */
	joy_stick_apply(&sc, 0.15f, 0.9f, -0.05f, &x, &y);
	CHECK(NEAR(x, 1) && NEAR(y, 0));
	joy_stick_apply(&sc, 0.15f, 0.1f, -0.95f, &x, &y);
	CHECK(NEAR(x, 0) && NEAR(y, -1));

	/* settings file text */
	CHECK(joy_stick_format(&sc, buf, sizeof(buf)) > 0);
	CHECK(joy_stick_parse(buf, &back));
	CHECK(NEAR(back.cx, sc.cx) && NEAR(back.yp, sc.yp) && NEAR(back.edge[5], sc.edge[5]));
	CHECK(!joy_stick_parse("0 0 1 1 1", &back));
	CHECK(!joy_stick_parse("", &back));

	CHECK(joy_direction(0, 0.6f, 0.5f, 0.3f) == 1);
	CHECK(joy_direction(1, 0.4f, 0.5f, 0.3f) == 1);		/* hysteresis holds */
	CHECK(joy_direction(1, 0.2f, 0.5f, 0.3f) == 0);
	CHECK(joy_direction(0, 0.4f, 0.5f, 0.3f) == 0);
	CHECK(joy_direction(1, -0.6f, 0.5f, 0.3f) == -1);	/* flick to the other side */
	CHECK(joy_direction(-1, -0.4f, 0.5f, 0.3f) == -1);
}

static void test_frametimes(void)
{
	static struct frametimes f;
	uint64_t t[FRAMETIMES_MAX];
	struct frametimes_summary s;
	unsigned n, i;
	uint64_t us = 1000000;

	frametimes_reset(&f);
	CHECK(frametimes_last(&f, t, FRAMETIMES_MAX) == 0);
	frametimes_summarize(t, 0, 0, &s);
	CHECK(s.frames == 0 && s.fps_x10 == -1);
	/* 2000 frames at 30 fps (33.333 ms), one 200 ms stall, the ring keeps the last 1024 */
	for (i = 0; i < 2000; i++) {
		us += i == 1990 ? 200000 : 33333;
		frametimes_add(&f, us);
	}
	n = frametimes_last(&f, t, FRAMETIMES_MAX);
	CHECK(n == FRAMETIMES_MAX);
	CHECK(t[n - 1] == us);
	for (i = 1; i < n; i++)
		CHECK(t[i] > t[i - 1]);
	frametimes_summarize(t, n, us - 1000000, &s);	/* the last second */
	CHECK(s.frames >= 25 && s.frames <= 27);
	CHECK(s.median_us == 33333);
	CHECK(s.worst_us == 200000);
	CHECK(s.hitches == 1);
	CHECK(s.fps_x10 >= 240 && s.fps_x10 <= 270);
	CHECK(frametimes_is_hitch(70000, 33333));
	CHECK(!frametimes_is_hitch(20000, 5000));	/* short ones never are */
	n = frametimes_last(&f, t, 10);
	CHECK(n == 10 && t[9] == us);
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

static struct {
	int keys, key_code, key_pressed, buttons, button, button_pressed, moves, mbuttons;
} hid_seen;
static bool hid_raw;

static void on_hid_input(const struct gp_event *ev, void *ud)
{
	(void)ud;
	if (ev->type == GP_EV_KEY) {
		hid_seen.keys++;
		hid_seen.key_code = ev->code;
		hid_seen.key_pressed = ev->pressed;
	} else if (ev->type == GP_EV_BUTTON && !ev->repeat) {
		hid_seen.buttons++;
		hid_seen.button = ev->code;
		hid_seen.button_pressed = ev->pressed;
	} else if (ev->type == GP_EV_MOUSE_MOVE) {
		hid_seen.moves++;
	} else if (ev->type == GP_EV_MOUSE_BUTTON) {
		hid_seen.mbuttons++;
	}
}

static bool hid_raw_query(void)
{
	return hid_raw;
}

/* Keyboards and mice: gamepad buttons in the UI, as they are to a game */
static void test_hid(void)
{
	struct mainloop *ml = mainloop_new("test");

	input_init(ml, NULL, on_hid_input, NULL);
	input_set_raw_query(hid_raw_query);
	memset(&hid_seen, 0, sizeof(hid_seen));
	hid_raw = false;
	input_report_key(28 /* KEY_ENTER */, true);
	CHECK(hid_seen.buttons == 1 && hid_seen.button == GP_A && hid_seen.button_pressed);
	hid_raw = true;		/* a game comes to the front: the release still frees A */
	input_report_key(28, false);
	CHECK(hid_seen.buttons == 2 && hid_seen.button == GP_A && !hid_seen.button_pressed);
	CHECK(hid_seen.keys == 0);
	input_report_key(30 /* KEY_A */, true);
	CHECK(hid_seen.keys == 1 && hid_seen.key_code == 30 && hid_seen.key_pressed);
	hid_raw = false;	/* back to the menu: the game still gets the release */
	input_report_key(30, false);
	CHECK(hid_seen.keys == 2 && !hid_seen.key_pressed);
	input_report_key(30, true);	/* a letter means nothing to the menu */
	input_report_key(30, false);
	CHECK(hid_seen.keys == 2 && hid_seen.buttons == 2);
	input_report_mouse_move(5, 0, 0);
	input_report_mouse_button(0, true);
	CHECK(hid_seen.moves == 0 && hid_seen.mbuttons == 0);
	hid_raw = true;
	input_report_mouse_move(5, -3, 1);
	input_report_mouse_button(0, true);
	hid_raw = false;
	input_report_mouse_button(0, false);
	CHECK(hid_seen.moves == 1 && hid_seen.mbuttons == 2);
	CHECK(input_key_button(1 /* KEY_ESC */) == GP_B && input_key_button(30) == GP_NONE);
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

	/* blit_compose: opaque wins, transparent shows the bottom, half red over blue */
	{
		uint32_t t[3] = { 0xff123456, 0x00ffffff, 0x80ff0000 };
		uint32_t b[3] = { 0xff0000ff, 0x000000ff, 0xff0000ff };
		uint32_t d[3] = { 0 };
		struct hal_fb top = { t, 3, 1, sizeof(t), 0 }, bottom = { b, 3, 1, sizeof(b), 0 };
		struct hal_fb out = { d, 3, 1, sizeof(d), 0 };

		blit_compose(&out, &top, &bottom, &(struct hal_rect){ 0, 0, 2, 0 });
		CHECK(d[0] == 0xff123456 && d[1] == 0xff0000ff && d[2] == 0xff80007f);
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

#ifdef CONFIG_SYSMON
/* Write root/rel, making the directories on the way */
static void put(const char *root, const char *rel, const char *text)
{
	char path[512], *p;
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", root, rel);
	for (p = path + strlen(root) + 1; (p = strchr(p, '/')); p++) {
		*p = '\0';
		mkdir(path, 0755);
		*p = '/';
	}
	f = fopen(path, "w");
	if (!f) {
		CHECK(!"cannot write the fake tree");
		return;
	}
	fputs(text, f);
	fclose(f);
}

static void put_procs(const char *root, int self, int game, unsigned long game_ticks)
{
	char rel[64], text[256];

	snprintf(rel, sizeof(rel), "proc/%d/stat", self);
	snprintf(text, sizeof(text), "%d (bare_app) S 1 1 1 0 -1 4194560 100 0 0 0 10 5 0 0 20 0 3 0 "
		 "100 1000000 2560 18446744073709551615\n", self);
	put(root, rel, text);
	snprintf(rel, sizeof(rel), "proc/%d/stat", game);
	/* a name with a space and a bracket, as /proc gives it */
	snprintf(text, sizeof(text), "%d (vcmi (main) x) R %d %d %d 0 -1 0 0 0 0 0 %lu 0 0 0 20 0 9 0 "
		 "200 900000000 25600\n", game, self, game, game, game_ticks);
	put(root, rel, text);
	put(root, "proc/1/stat", "1 (init) S 0 1 1 0 -1 0 0 0 0 0 1 1 0 0 20 0 1 0 1 100 50\n");
}

/* A GO Ultra-like system in a directory: two clusters, a battery zone to
 * leave out, an eMMC with boot areas, a game started by bare_app */
static void test_sysmon(void)
{
	char root[] = "/tmp/bare_app_sysmon_XXXXXX";
	struct sysmon_info in;
	struct sysmon *s;
	int self = (int)getpid(), game = self + 1, i;
	char cmd[64];

	if (!mkdtemp(root)) {
		CHECK(!"mkdtemp");
		return;
	}
	put(root, "proc/cpuinfo", "processor\t: 0\nCPU part\t: 0xd03\n\nprocessor\t: 1\n"
				  "CPU part\t: 0xd09\n\nprocessor\t: 2\nCPU part\t: 0xd09\n");
	put(root, "sys/devices/system/cpu/cpufreq/policy0/related_cpus", "0\n");
	put(root, "sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq", "1000000\n");
	put(root, "sys/devices/system/cpu/cpufreq/policy0/cpuinfo_max_freq", "1704000\n");
	put(root, "sys/devices/system/cpu/cpufreq/policy1/related_cpus", "1 2\n");
	put(root, "sys/devices/system/cpu/cpufreq/policy1/scaling_cur_freq", "1398000\n");
	put(root, "sys/devices/system/cpu/cpufreq/policy1/cpuinfo_max_freq", "1908000\n");
	put(root, "proc/stat", "cpu  150 0 150 1050 150 0 0 0\n"
			       "cpu0 50 0 50 350 50 0 0 0\ncpu1 50 0 50 350 50 0 0 0\n"
			       "cpu2 50 0 50 350 50 0 0 0\nintr 1 2 3\n");
	put(root, "proc/meminfo", "MemTotal:        2048000 kB\nMemFree:          100000 kB\n"
				  "MemAvailable:    1024000 kB\nSwapTotal:             0 kB\n"
				  "SwapFree:              0 kB\n");
	put(root, "sys/class/thermal/thermal_zone0/type", "cpu-thermal\n");
	put(root, "sys/class/thermal/thermal_zone0/temp", "45500\n");
	put(root, "sys/class/thermal/thermal_zone1/type", "rk818-battery\n");
	put(root, "sys/class/thermal/thermal_zone1/temp", "20000\n");
	put(root, "sys/class/thermal/thermal_zone10/type", "ddr-thermal\n");
	put(root, "sys/class/thermal/thermal_zone10/temp", "40000\n");
	put(root, "sys/class/devfreq/ffe40000.gpu/cur_freq", "400000000\n");
	put(root, "sys/class/devfreq/ffe40000.gpu/max_freq", "800000000\n");
	put(root, "sys/class/power_supply/rk818-usb/type", "USB\n");
	put(root, "sys/class/power_supply/rk818-battery/type", "Battery\n");
	put(root, "sys/class/power_supply/rk818-battery/capacity", "87\n");
	put(root, "sys/class/power_supply/rk818-battery/voltage_now", "3900000\n");
	put(root, "sys/class/power_supply/rk818-battery/current_now", "500000\n");	/* wrong sign */
	put(root, "sys/class/power_supply/rk818-battery/status", "Discharging\n");
	put(root, "sys/block/mmcblk1/device/type", "MMC\n");
	put(root, "sys/block/mmcblk1boot0/device/type", "MMC\n");
	put(root, "sys/block/loop0/size", "0\n");
	put(root, "proc/diskstats", " 179 0 mmcblk1 10 0 2000 0 5 0 1000 0 0 100 0\n"
				    " 179 3 mmcblk1p3 1 0 8 0 0 0 0 0 0 0 0\n");
	put(root, "proc/mounts", "/dev/mmcblk1p3 /data ext4 rw 0 0\n"
				 "/dev/mmcblk1p1 /boot vfat rw 0 0\ntmpfs /tmp tmpfs rw 0 0\n");
	put(root, "data/.keep", "");
	put(root, "proc/uptime", "3725.50 100.00\n");
	put_procs(root, self, game, 100);

	s = sysmon_open(root);
	CHECK(s != NULL);
	if (!s)
		return;
	sysmon_sample(s, 1000, &in);
	CHECK(in.ncpus == 3 && in.cpu_total == -1 && in.cpu[0] == -1);	/* no loads yet */
	CHECK(in.nclusters == 2);
	CHECK(!strcmp(in.clusters[0].core, "A53") && in.clusters[0].first == 0 &&
	      in.clusters[0].count == 1 && in.clusters[0].mhz == 1000 && in.clusters[0].max_mhz == 1704);
	CHECK(!strcmp(in.clusters[1].core, "A73") && in.clusters[1].first == 1 &&
	      in.clusters[1].count == 2 && in.clusters[1].mhz == 1398);
	CHECK(in.mem_total_mb == 2000 && in.mem_used_mb == 1000);
	CHECK(in.ntemps == 2);				/* the battery's zone left out */
	CHECK(in.temps[0].kind == SYSMON_TEMP_CPU && !strcmp(in.temps[0].name, "cpu") &&
	      in.temps[0].mdeg == 45500);
	CHECK(in.temps[1].kind == SYSMON_TEMP_DDR && in.temps[1].mdeg == 40000);
	CHECK(in.gpu_mhz == 400 && in.gpu_max_mhz == 800);
	CHECK(in.battery && in.bat_capacity == 87 && in.bat_mv == 3900);
	CHECK(in.bat_ma == -500 && in.bat_mw == -1950);	/* out of the battery */
	CHECK(in.ndisks == 1);				/* no boot areas, no loop */
	CHECK(!strcmp(in.disks[0].name, "mmcblk1") && in.disks[0].kind == SYSMON_DISK_EMMC);
	CHECK(in.disks[0].busy == -1);
	CHECK(in.disks[0].nmounts == 1 && !strcmp(in.disks[0].mounts[0].path, "/data"));
	CHECK(in.disks[0].mounts[0].total_mb > 0 &&
	      in.disks[0].mounts[0].used_mb <= in.disks[0].mounts[0].total_mb &&
	      in.disks[0].used_mb == in.disks[0].mounts[0].used_mb &&
	      in.disks[0].free_mb == in.disks[0].mounts[0].free_mb);
	CHECK(in.nprocs == 2 && in.procs[0].self && in.procs[0].pid == self);
	CHECK(!strcmp(in.procs[0].name, "bare_app") && in.procs[0].cpu == -1);
	CHECK(in.procs[0].rss_mb == (uint32_t)(2560 * (sysconf(_SC_PAGESIZE) / 1024) / 1024));
	CHECK(!in.procs[1].self && in.procs[1].pid == game && !strcmp(in.procs[1].name, "vcmi (main) x"));
	CHECK(in.uptime_s == 3725);

	/* a second later: cpu0 half busy, cpu1 and cpu2 all busy; the disk read
	 * 1 MB, wrote 2 MB, busy a quarter of the time; the game used a CPU */
	put(root, "proc/stat", "cpu  400 0 150 1100 150 0 0 0\n"
			       "cpu0 100 0 50 400 50 0 0 0\ncpu1 150 0 50 350 50 0 0 0\n"
			       "cpu2 150 0 50 350 50 0 0 0\n");
	put(root, "proc/diskstats", " 179 0 mmcblk1 20 0 4000 0 9 0 5000 0 0 350 0\n");
	put_procs(root, self, game, 100 + sysconf(_SC_CLK_TCK));
	sysmon_sample(s, 2000, &in);
	CHECK(in.cpu[0] == 50 && in.cpu[1] == 100 && in.cpu[2] == 100);
	CHECK(in.cpu_total == 83);			/* 250 of 300 */
	CHECK(in.disks[0].read_kbs == 1024 && in.disks[0].write_kbs == 2048);
	CHECK(in.disks[0].busy == 25);
	CHECK(in.procs[0].cpu == 0 && in.procs[1].cpu == 100);
	sysmon_close(s);

	snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
	i = system(cmd);
	(void)i;
}
#endif

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
	test_frametimes();
	test_hid();
#ifdef CONFIG_SYSMON
	test_sysmon();
#endif
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
