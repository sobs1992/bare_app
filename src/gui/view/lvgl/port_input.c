/*
 * UI keys -> LVGL keypad in event mode: no polling timer, every key state
 * is queued and fed with one lv_indev_read() each. Auto-repeat comes from
 * core/input as press events with 'repeat' set; LVGL needs a release in
 * between to see a new press.
 *
 * Up/Down move the focus (PREV/NEXT), except in a button matrix (the
 * on-screen keyboard), which takes them like Left/Right; Left/Right go to
 * the focused widget (sliders, rollers), Accept is ENTER, X is
 * UI_LV_KEY_ALT for screens that give the focused item a second action.
 */
#include <string.h>

#include "lvgl_view.h"

#include "core/log.h"
#include "core/util.h"

#define QLEN 32

static struct {
	lv_indev_t *indev;
	struct { uint32_t key; lv_indev_state_t state; } q[QLEN];
	int head, count;
	uint32_t last_key;
	lv_indev_state_t last_state;
} pi;

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
	(void)indev;
	if (pi.count) {
		pi.last_key = pi.q[pi.head].key;
		pi.last_state = pi.q[pi.head].state;
		pi.head = (pi.head + 1) % QLEN;
		pi.count--;
	}
	data->key = pi.last_key;
	data->state = pi.last_state;
}

static void push(uint32_t key, lv_indev_state_t state)
{
	if (pi.count == QLEN)
		return;
	pi.q[(pi.head + pi.count) % QLEN].key = key;
	pi.q[(pi.head + pi.count) % QLEN].state = state;
	pi.count++;
}

static uint32_t to_lv_key(enum ui_key key)
{
	lv_group_t *g = lv_indev_get_group(pi.indev);
	lv_obj_t *f = g ? lv_group_get_focused(g) : NULL;
	bool grid = f && lv_obj_has_class(f, &lv_buttonmatrix_class);

	switch (key) {
	case UI_KEY_UP:     return grid ? LV_KEY_UP : LV_KEY_PREV;
	case UI_KEY_DOWN:   return grid ? LV_KEY_DOWN : LV_KEY_NEXT;
	case UI_KEY_LEFT:   return LV_KEY_LEFT;
	case UI_KEY_RIGHT:  return LV_KEY_RIGHT;
	case UI_KEY_ACCEPT: return LV_KEY_ENTER;
	case UI_KEY_BACK:   return LV_KEY_ESC;
	case UI_KEY_PREV:   return LV_KEY_PREV;
	case UI_KEY_NEXT:   return LV_KEY_NEXT;
	case UI_KEY_ALT:    return UI_LV_KEY_ALT;
	default:            return 0;
	}
}

bool port_input_key(enum ui_key key, bool pressed, bool repeat)
{
	uint32_t k = to_lv_key(key);

	if (!k)
		return false;
	if (repeat)
		push(k, LV_INDEV_STATE_RELEASED);
	push(k, pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED);
	while (pi.count)
		lv_indev_read(pi.indev);
	/* BACK is left to the view core: it closes the screen */
	return key != UI_KEY_BACK;
}

void port_input_set_group(lv_group_t *g)
{
	lv_indev_set_group(pi.indev, g);
}

int port_input_init(void)
{
	memset(&pi, 0, sizeof(pi));
	pi.last_state = LV_INDEV_STATE_RELEASED;
	pi.indev = lv_indev_create();	/* after the display: it binds to the default one */
	if (!pi.indev)
		return -1;
	lv_indev_set_type(pi.indev, LV_INDEV_TYPE_KEYPAD);
	lv_indev_set_read_cb(pi.indev, read_cb);
	lv_indev_set_mode(pi.indev, LV_INDEV_MODE_EVENT);
	return 0;
}

void port_input_deinit(void)
{
	if (pi.indev)
		lv_indev_delete(pi.indev);
	pi.indev = NULL;
}
