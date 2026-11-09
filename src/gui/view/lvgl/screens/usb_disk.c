/*
 * USB disk (CTX_USB_DISK): the games partition for the computer while
 * this screen is open. Control shares it when the screen opens and takes
 * it back when it closes or the computer ejects the disk.
 *
 * The screen takes the buttons itself: while the computer has the disk, B
 * asks to eject it there first and closes only when pressed again within
 * AGAIN_MS (copying on the computer may not be written out yet). A shares
 * the partition again after it came back or could not be shared.
 */
#include <stdlib.h>

#include "../widget.h"
#include "core/util.h"
#include "gui/action.h"

#define AGAIN_MS 3000
#define TEXT_CELLS 20	/* the text's width */

struct usb_disk {
	lv_obj_t *icon, *text, *hint;
	enum usb_disk_state state;
	bool available;
	uint64_t b_at;		/* when B asked to eject first, 0 not asked */
};

static bool retry_allowed(enum usb_disk_state s)
{
	return s == USB_DISK_EJECTED || s == USB_DISK_BUSY || s == USB_DISK_FAILED;
}

static bool computer_has_it(enum usb_disk_state s)
{
	return s == USB_DISK_SHARED;
}

static void on_input(const struct gp_event *ev, void *ud)
{
	struct usb_disk *d = ud;
	uint64_t now;

	if (ev->type != GP_EV_BUTTON || !ev->pressed || ev->repeat)
		return;
	if (ev->code == GP_A && d->available && retry_allowed(d->state)) {
		gui_action_post(ACT_USB_DISK, 1);
		return;
	}
	if (ev->code != GP_B)
		return;
	now = now_ms();
	if (computer_has_it(d->state) && (!d->b_at || now - d->b_at > AGAIN_MS)) {
		d->b_at = now;
		ui_toast(tr(S_USB_DISK_AGAIN));
		return;
	}
	gui_action_post(ACT_BACK, 0);	/* control takes the partition back */
}

static void *usb_disk_init(lv_obj_t *parent, lv_obj_t **root)
{
	struct usb_disk *d = calloc(1, sizeof(*d));
	lv_obj_t *scr, *body;

	(void)parent;
	if (!d)
		return NULL;
	d->state = (enum usb_disk_state)-1;
	scr = console_screen(LV_SYMBOL_USB, tr(S_USB_DISK));
	body = screen_body(scr);
	lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	d->icon = lv_label_create(body);
	lv_obj_set_style_text_font(d->icon, UI_FONT_MID, 0);
	d->text = lv_label_create(body);
	lv_obj_set_width(d->text, LV_MIN(TEXT_CELLS * ui_cell(), LV_PCT(100)));
	lv_label_set_long_mode(d->text, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_align(d->text, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(d->text, UI_FONT_MID, 0);
	d->hint = lv_label_create(body);
	lv_obj_set_style_text_color(d->hint, UI_COLOR_DIM, 0);

	view_input_grab(on_input, d);
	*root = scr;
	return d;
}

static void usb_disk_update(void *self, const struct gui_ctx *c, uint32_t changed)
{
	struct usb_disk *d = self;
	enum str_id text, hint = S_USB_DISK_HINT_BACK;
	const char *icon = LV_SYMBOL_USB;
	lv_color_t color = lv_color_white();

	(void)changed;
	if (d->state == c->usb_disk.state && d->available == c->usb_disk.available)
		return;
	d->state = c->usb_disk.state;
	d->available = c->usb_disk.available;
	if (!computer_has_it(d->state))
		d->b_at = 0;

	if (!d->available) {
		text = S_USB_DISK_UNAVAILABLE;
		icon = LV_SYMBOL_WARNING;
		color = UI_COLOR_DIM;
	} else {
		switch (d->state) {
		case USB_DISK_SHARED:
			text = S_USB_DISK_SHARED;
			hint = S_USB_DISK_HINT_SHARED;
			color = UI_COLOR_ON;
			break;
		case USB_DISK_NO_HOST:
			text = S_USB_DISK_NO_HOST;
			hint = S_USB_DISK_HINT_SHARED;
			break;
		case USB_DISK_EJECTED:
			text = S_USB_DISK_EJECTED;
			icon = LV_SYMBOL_OK;
			hint = S_USB_DISK_HINT_RETRY;
			break;
		case USB_DISK_BUSY:
			text = S_USB_DISK_BUSY;
			icon = LV_SYMBOL_WARNING;
			color = UI_COLOR_WARN;
			hint = S_USB_DISK_HINT_RETRY;
			break;
		case USB_DISK_FAILED:
			text = S_USB_DISK_FAILED;
			icon = LV_SYMBOL_WARNING;
			color = UI_COLOR_WARN;
			hint = S_USB_DISK_HINT_RETRY;
			break;
		case USB_DISK_OFF:
		case USB_DISK_STARTING:
		default:
			text = S_USB_DISK_STARTING;
			hint = S_USB_DISK_HINT_BACK;
			break;
		}
	}
	lv_label_set_text(d->icon, icon);
	lv_obj_set_style_text_color(d->icon, color, 0);
	lv_label_set_text(d->text, tr(text));
	lv_label_set_text(d->hint, tr(hint));
}

static void usb_disk_deinit(void *self)
{
	view_input_release(self);
	free(self);
}

const struct widget_ops usb_disk_screen = {
	.name = "usb-disk",
	.deps = CTX_USB_DISK,
	.init = usb_disk_init,
	.update = usb_disk_update,
	.deinit = usb_disk_deinit,
};
