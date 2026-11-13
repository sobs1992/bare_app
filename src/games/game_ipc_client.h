/*
 * The game's side of game_ipc.h: what a program started by bare_app does to
 * show frames and get the gamepad. Used by ports/doom, ports/wine, SDL's
 * bareapp drivers; a new port needs nothing else:
 *
 *	struct game_link link;
 *	struct game_msg m;
 *
 *	if (game_link_open(&link, false) < 0)
 *		exit(2);			// not started by bare_app
 *	for (;;) {
 *		while (game_link_recv(&m) > 0)
 *			...			// GAME_MSG_BUTTON, GAME_MSG_AXIS
 *		draw(game_link_frame(&link));	// link.shm->width x height, stride
 *		if (game_link_publish(&link) < 0)
 *			break;			// bare_app is gone
 *	}
 *
 * Header only, C and C++. It is compiled into games under their own
 * licences (GPL, LGPL, zlib), so unlike the rest of bare_app it is under
 * the MIT licence, as game_ipc.h is.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "game_ipc.h"

struct game_link {
	struct game_shm *shm;	/* the frames' width, height, stride */
	size_t size;		/* of the mapping */
	unsigned back;		/* the frame being drawn */
	unsigned full_back;	/* the game's own picture being drawn */
};

/* Whether descriptors 3 and 4 are bare_app's: for a program that also runs
 * on its own */
static inline bool game_link_there(void)
{
	struct stat st;
	int type = 0;
	socklen_t len = sizeof(type);
	uint32_t magic = 0;

	return fstat(GAME_IPC_SOCK, &st) == 0 && S_ISSOCK(st.st_mode) &&
	       getsockopt(GAME_IPC_SOCK, SOL_SOCKET, SO_TYPE, &type, &len) == 0 &&
	       type == SOCK_SEQPACKET &&
	       fstat(GAME_IPC_SHM, &st) == 0 && st.st_size >= GAME_SHM_HEADER &&
	       pread(GAME_IPC_SHM, &magic, sizeof(magic), 0) == (ssize_t)sizeof(magic) &&
	       magic == GAME_SHM_MAGIC;
}

/*
 * Maps the frames. inherit: programs this one starts keep the link (else
 * the descriptors are closed on exec, so that a helper left running does
 * not keep bare_app waiting). 0, or -1 with errno (ENOTCONN: not started
 * by bare_app).
 */
static inline int game_link_open(struct game_link *l, bool inherit)
{
	struct stat st;
	void *map;

	l->shm = NULL;
	if (!game_link_there()) {
		errno = ENOTCONN;
		return -1;
	}
	if (fstat(GAME_IPC_SHM, &st) < 0)
		return -1;
	map = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, GAME_IPC_SHM, 0);
	if (map == MAP_FAILED)
		return -1;
	l->shm = (struct game_shm *)map;
	l->size = (size_t)st.st_size;
	if (game_shm_size(l->shm) > l->size) {
		munmap(map, l->size);
		l->shm = NULL;
		errno = EINVAL;
		return -1;
	}
	if (!inherit) {
		fcntl(GAME_IPC_SOCK, F_SETFD, FD_CLOEXEC);
		fcntl(GAME_IPC_SHM, F_SETFD, FD_CLOEXEC);
	}
	l->back = l->full_back = 0;	/* the producer starts with buffer 0 */
	return 0;
}

/*
 * The game's own picture, for the stream (game_ipc.h): a game that draws
 * bigger than the frame and takes it down hands over what it drew while
 * bare_app wants it, before the frame it publishes:
 *
 *	if (game_link_full_wanted(&link, w, h)) {
 *		draw w x h into game_link_full(&link), full_width * 4 bytes a row
 *		game_link_full_publish(&link, w, h);
 *	}
 *	game_link_publish(&link);
 */
static inline bool game_link_full_wanted(const struct game_link *l, int width, int height)
{
	return width > 0 && height > 0 && (uint32_t)width <= l->shm->full_width &&
	       (uint32_t)height <= l->shm->full_height &&
#ifdef __cplusplus
	       l->shm->full_want.load();
#else
	       atomic_load(&l->shm->full_want);
#endif
}

static inline uint32_t *game_link_full(const struct game_link *l)
{
	return (uint32_t *)game_shm_full(l->shm, l->full_back);
}

static inline void game_link_full_publish(struct game_link *l, int width, int height)
{
	uint32_t size = (uint32_t)width | (uint32_t)height << 16;

#ifdef __cplusplus
	l->shm->full_size[l->full_back].store(size);
	l->full_back = l->shm->full_middle.exchange(l->full_back | GAME_SHM_FRESH) & ~GAME_SHM_FRESH;
#else
	atomic_store(&l->shm->full_size[l->full_back], size);
	l->full_back = atomic_exchange(&l->shm->full_middle, l->full_back | GAME_SHM_FRESH) & ~GAME_SHM_FRESH;
#endif
}

/* The frame to draw into next: shm->height rows of shm->stride bytes, XRGB8888 */
static inline uint8_t *game_link_frame(const struct game_link *l)
{
	return (uint8_t *)game_shm_frame(l->shm, l->back);
}

/*
 * Hands the frame drawn to bare_app and moves on to another one (its
 * contents are not the frame just published). Frames that did not change
 * are better not published at all: game_link_same() says one was made.
 * -1 once bare_app is gone.
 */
static inline int game_link_publish(struct game_link *l)
{
	struct game_msg m = { GAME_MSG_FRAME, 0, 0, 0, 0 };

#ifdef __cplusplus
	l->back = l->shm->middle.exchange(l->back | GAME_SHM_FRESH) & ~GAME_SHM_FRESH;
#else
	l->back = atomic_exchange(&l->shm->middle, l->back | GAME_SHM_FRESH) & ~GAME_SHM_FRESH;
#endif
	/* a full socket already holds a "frame" message: nothing to add */
	if (send(GAME_IPC_SOCK, &m, sizeof(m), MSG_DONTWAIT | MSG_NOSIGNAL) < 0 && errno == EPIPE)
		return -1;
	return 0;
}

/*
 * A frame was made, but it is the one before: nothing is published. The
 * frame rate bare_app shows then stays the game's own over a still
 * picture, and a long gap means the game really stopped. Once per frame
 * the game would have shown. -1 once bare_app is gone.
 */
static inline int game_link_same(struct game_link *l)
{
	struct game_msg m = { GAME_MSG_FRAME, 0, 0, GAME_FRAME_SAME, 0 };

	(void)l;
	if (send(GAME_IPC_SOCK, &m, sizeof(m), MSG_DONTWAIT | MSG_NOSIGNAL) < 0 && errno == EPIPE)
		return -1;
	return 0;
}

/* The next message from bare_app, without waiting: 1 (in *m), 0 none
 * now, -1 bare_app is gone. Poll GAME_IPC_SOCK to wait for one. */
static inline int game_link_recv(struct game_msg *m)
{
	ssize_t n;

	for (;;) {
		n = recv(GAME_IPC_SOCK, m, sizeof(*m), MSG_DONTWAIT);
		if (n == (ssize_t)sizeof(*m))
			return 1;
		if (n == 0)
			return -1;
		if (n < 0)
			return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
		/* a short packet: not ours, skip it */
	}
}
