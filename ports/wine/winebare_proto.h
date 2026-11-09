/*
 * Between bare_app_wine (ports/wine/bare_app_wine.c, the "host") and the
 * winebare.drv graphics driver loaded in every Wine process of a game
 * (patch of the SDK package wine-wow64). This header only describes the
 * exchange; it is also included by the driver (LGPL), so it holds no code.
 *
 * bare_app starts bare_app_wine as an ordinary game (game_ipc.h). The host
 * starts Wine with WINEBARE_SOCKET=<path> of a listening SOCK_SEQPACKET
 * socket; the driver of each Wine process connects to it. The host puts
 * the windows of all processes together into bare_app's frame, scaled
 * from the Windows screen (the display mode) to the frame size, and turns
 * bare_app's buttons into mouse and keyboard input for Wine.
 *
 * One struct wb_msg per packet. Pixel memory travels as a memfd in
 * SCM_RIGHTS with WB_SURFACE: width * height * 4 bytes per buffer, rows
 * top-down, BGRX (Windows' 32 bpp DIB = XRGB8888 little endian); with
 * WB_CURSOR the cursor's picture, width * height * 4 bytes, rows top-down,
 * BGRA with straight alpha (0 transparent). A shown cursor without a
 * picture (width 0) is an arrow drawn by the host.
 */
#pragma once

#include <stdint.h>

#define WINEBARE_SOCKET_ENV "WINEBARE_SOCKET"
#define WB_PROTO_VERSION 3	/* 2: key flags, 3: cursor pictures */

#define WB_MAX_MODES 16

enum wb_msg_type {
	/* driver -> host */
	WB_HELLO = 1,		/* pid, version; the host answers WB_CONFIG */
	WB_SURFACE,		/* surface id, width, height, buffers: memfd attached */
	WB_SURFACE_DEL,		/* surface id */
	WB_PLACE,		/* surface id, hwnd, screen position and size, visible, layer */
	WB_FLUSH,		/* surface id, buffer, dirty rect: new pixels there */
	WB_ZORDER,		/* hwnds of visible top-level windows, top first */
	WB_MODE_SET,		/* width, height: ChangeDisplaySettings */
	WB_CURSOR,		/* visible, picture: memfd attached when width > 0 */
	WB_CURSOR_POS,		/* x, y: SetCursorPos */

	/* host -> driver */
	WB_CONFIG = 64,		/* modes, current mode */
	WB_INPUT_MOUSE,		/* x, y (screen), buttons changed: flags */
	WB_INPUT_KEY,		/* virtual key, pressed */
};

/* WB_PLACE layer: GDI pixels of the window, or GL pixels of its client area
 * drawn over them */
#define WB_LAYER_GDI 0
#define WB_LAYER_GL 1

/* WB_INPUT_MOUSE flags */
#define WB_MOUSE_MOVE 0x01
#define WB_MOUSE_LEFT_DOWN 0x02
#define WB_MOUSE_LEFT_UP 0x04
#define WB_MOUSE_RIGHT_DOWN 0x08
#define WB_MOUSE_RIGHT_UP 0x10
#define WB_MOUSE_MIDDLE_DOWN 0x20
#define WB_MOUSE_MIDDLE_UP 0x40
#define WB_MOUSE_WHEEL 0x80	/* wheel: steps, + = away from the user */

struct wb_mode {
	uint16_t width, height;
};

/* WB_INPUT_KEY flags. Without WB_KEY_EXTENDED a key of two places on the
 * keyboard (the arrows, Insert, Delete, Home, End, Page Up and Down) is
 * the one Wine's keyboard layout names first: the numeric keypad's. */
#define WB_KEY_EXTENDED 1u	/* the cursor block's key: E0 scan code */
/* vk is (virtual key << 8) | PC scan code (set 1, E0 by WB_KEY_EXTENDED):
 * a real keyboard's key, nothing is looked up */
#define WB_KEY_SCANCODE 2u

struct wb_msg {
	uint32_t type;
	union {
		struct {
			uint32_t pid, version;
		} hello;
		struct {
			uint32_t id, width, height, buffers;
		} surface;
		struct {
			uint32_t id;
		} surface_del;
		struct {
			uint32_t id;
			uint64_t hwnd;
			int32_t x, y;		/* screen position of pixel (0, 0) */
			uint32_t width, height;	/* shown part of the buffer */
			uint32_t visible, layer;
		} place;
		struct {
			uint32_t id, buffer;
			int32_t left, top, right, bottom;
		} flush;
		struct {
			uint32_t count;
			uint64_t hwnd[60];
		} zorder;
		struct {
			uint16_t width, height;
		} mode_set;
		struct {
			uint32_t visible;
			uint16_t width, height;	/* the picture, 0 x 0: none */
			int16_t hot_x, hot_y;	/* its pixel at the pointer's position */
		} cursor;
		struct {
			int32_t x, y;
		} cursor_pos;
		struct {
			uint32_t mode_count;
			struct wb_mode modes[WB_MAX_MODES];
			struct wb_mode current;
		} config;
		struct {
			int32_t x, y;
			uint32_t flags;
			int32_t wheel;
		} mouse;
		struct {
			uint16_t vk;
			uint8_t pressed;
			uint8_t flags;	/* WB_KEY_* */
		} key;
	};
};
