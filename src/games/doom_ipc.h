/*
 * Between bare_app (view thread, games/doom.c) and the game process
 * bare_app_doom (ports/doom/doom_main.c, GPLv2: this header is part of
 * bare_app and only describes the exchange). The game inherits two
 * descriptors:
 *
 *   DOOM_IPC_SOCK  SOCK_SEQPACKET socket, one struct doom_msg per packet:
 *                  game -> app: DOOM_MSG_FRAME after publishing a frame;
 *                  app -> game: DOOM_MSG_BUTTON (gamepad buttons; the game
 *                  has its own key map). EOF means the other side is gone.
 *   DOOM_IPC_SHM   memfd: struct doom_shm, then three frames of
 *                  height * stride bytes each, XRGB8888, at doom_shm_frame().
 *
 * Frames are triple buffered without locks, so a game that dies in the
 * middle of a frame cannot block the app: the producer owns one buffer, the
 * consumer another, the third is in 'middle'. Each side swaps its own
 * buffer with 'middle'; DOOM_SHM_FRESH marks a published frame not taken yet.
 */
#pragma once

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define DOOM_IPC_SOCK 3
#define DOOM_IPC_SHM 4

#define DOOM_SHM_MAGIC 0x4d4f4f44u	/* "DOOM" */
#define DOOM_SHM_FRESH 4u
#define DOOM_SHM_HEADER 4096

struct doom_shm {
	uint32_t magic;
	uint32_t width, height, stride;	/* written by the app before the start */
	_Atomic uint32_t middle;	/* buffer index | DOOM_SHM_FRESH */
};

static inline size_t doom_shm_size(const struct doom_shm *s)
{
	return DOOM_SHM_HEADER + 3 * (size_t)s->height * s->stride;
}

static inline void *doom_shm_frame(struct doom_shm *s, unsigned i)
{
	return (uint8_t *)s + DOOM_SHM_HEADER + i * (size_t)s->height * s->stride;
}

enum doom_msg_type {
	DOOM_MSG_FRAME,
	DOOM_MSG_BUTTON,
};

struct doom_msg {
	uint8_t type;		/* enum doom_msg_type */
	uint8_t pressed;
	uint8_t code;		/* enum gp_button */
};
