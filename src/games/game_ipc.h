/*
 * Between bare_app (view thread, games/game_proc.c) and a game process:
 * bare_app_doom (ports/doom), VCMI's client and Xash3D with their bare_app
 * links (SDK packages vcmi, xash3d-fwgs), bare_app_wine, SDL games through
 * SDL's bareapp drivers. game_ipc_client.h is the game's side of it. The
 * game inherits two descriptors:
 *
 *   GAME_IPC_SOCK  SOCK_SEQPACKET socket, one struct game_msg per packet:
 *                  game -> app: GAME_MSG_FRAME after publishing a frame,
 *                  or with GAME_FRAME_SAME for a frame the game made but
 *                  did not publish because it did not change (so a still
 *                  picture is not taken for a game that stopped);
 *                  app -> game: GAME_MSG_BUTTON, GAME_MSG_AXIS (the game
 *                  has its own controls); a game started with a keyboard
 *                  and a mouse also gets GAME_MSG_KEY and GAME_MSG_MOUSE_*
 *                  of a USB keyboard and mouse (the others never see them).
 *                  EOF means the other side is gone.
 *   GAME_IPC_SHM   memfd: struct game_shm, then three frames of
 *                  height * stride bytes each, XRGB8888, at game_shm_frame().
 *
 * Frames are triple buffered without locks, so a game that dies in the
 * middle of a frame cannot block the app: the producer owns one buffer, the
 * consumer another, the third is in 'middle'. Each side swaps its own
 * buffer with 'middle'; GAME_SHM_FRESH marks a published frame not taken yet.
 * The producer starts with buffer 0, 'middle' holds 1, the app holds 2.
 *
 * It is compiled into games under their own licences (GPL, LGPL, zlib), so
 * unlike the rest of bare_app this header is under the MIT licence.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Also read by C++ (VCMI): std::atomic has the same layout as _Atomic */
#ifdef __cplusplus
#include <atomic>
#define GAME_ATOMIC(t) std::atomic<t>
#else
#include <stdatomic.h>
#define GAME_ATOMIC(t) _Atomic t
#endif

#define GAME_IPC_SOCK 3
#define GAME_IPC_SHM 4

#define GAME_SHM_MAGIC 0x454d4147u	/* "GAME" */
#define GAME_SHM_FRESH 4u
#define GAME_SHM_HEADER 4096

struct game_shm {
	uint32_t magic;
	uint32_t width, height, stride;	/* written by the app before the start */
	GAME_ATOMIC(uint32_t) middle;	/* buffer index | GAME_SHM_FRESH */
};

static inline size_t game_shm_size(const struct game_shm *s)
{
	return GAME_SHM_HEADER + 3 * (size_t)s->height * s->stride;
}

static inline void *game_shm_frame(struct game_shm *s, unsigned i)
{
	return (uint8_t *)s + GAME_SHM_HEADER + i * (size_t)s->height * s->stride;
}

enum game_msg_type {
	GAME_MSG_FRAME,
	GAME_MSG_BUTTON,	/* code: enum gp_button, pressed, flags */
	GAME_MSG_AXIS,		/* code: enum gp_axis, value */
	GAME_MSG_KEY,		/* value: Linux key code (KEY_*), pressed */
	GAME_MSG_MOUSE_BUTTON,	/* code: GAME_MOUSE_LEFT..., pressed */
	GAME_MSG_MOUSE_MOVE,	/* code: GAME_MOUSE_X/Y/WHEEL, value: counts (wheel: clicks, up +) */
};

#define GAME_MOUSE_LEFT 0
#define GAME_MOUSE_RIGHT 1
#define GAME_MOUSE_MIDDLE 2
#define GAME_MOUSE_X 0
#define GAME_MOUSE_Y 1
#define GAME_MOUSE_WHEEL 2

/* GAME_MSG_FRAME flags */
#define GAME_FRAME_SAME 1u	/* nothing published: the frame is the one before */

/* GAME_MSG_BUTTON flags */
#define GAME_BTN_STICK 1u	/* a direction made by the left stick, not the D-pad */

#define GAME_AXIS_MAX 32767	/* value of a full deflection */

struct game_msg {
	uint8_t type;		/* enum game_msg_type */
	uint8_t pressed;
	uint8_t code;
	uint8_t flags;
	int16_t value;		/* axes: -GAME_AXIS_MAX..GAME_AXIS_MAX */
};
