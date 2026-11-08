/*
 * A game for the tests (tests/game_smoke.cmake): speaks game_ipc_client.h,
 * draws a frame of one colour 60 times a second (another colour while A
 * is held), says on stderr what it gets, ends with status 0 on B and 3 on
 * X, and when bare_app goes away. Y makes one frame 150 ms late (a hitch
 * for the frame time graph).
 */
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/input.h"
#include "games/game_ipc_client.h"

static void fill(struct game_link *app, uint32_t colour)
{
	uint8_t *frame = game_link_frame(app);

	for (uint32_t y = 0; y < app->shm->height; y++) {
		uint32_t *row = (uint32_t *)(frame + (size_t)y * app->shm->stride);

		for (uint32_t x = 0; x < app->shm->width; x++)
			row[x] = colour;
	}
}

int main(void)
{
	struct game_link app;
	struct game_msg m;
	bool a_held = false;
	int n;

	if (game_link_open(&app, false) < 0) {
		fprintf(stderr, "fake: not started by bare_app\n");
		return 2;
	}
	fprintf(stderr, "fake: started, frames %ux%u\n", app.shm->width, app.shm->height);
	for (;;) {
		struct pollfd p = { .fd = GAME_IPC_SOCK, .events = POLLIN };

		poll(&p, 1, 16);
		while ((n = game_link_recv(&m)) > 0) {
			if (m.type == GAME_MSG_AXIS) {
				fprintf(stderr, "fake: axis %d %d\n", m.code, m.value);
				continue;
			}
			if (m.type != GAME_MSG_BUTTON)
				continue;
			fprintf(stderr, "fake: button %d %s\n", m.code, m.pressed ? "down" : "up");
			if (m.code == GP_A)
				a_held = m.pressed;
			else if (m.code == GP_B && m.pressed)
				return 0;
			else if (m.code == GP_X && m.pressed)
				return 3;
			else if (m.code == GP_Y && m.pressed)
				usleep(150000);
		}
		if (n < 0) {
			fprintf(stderr, "fake: bare_app is gone\n");
			return 0;
		}
		fill(&app, a_held ? 0x00ff00 : 0x2040c0);
		if (game_link_publish(&app) < 0)
			return 0;
	}
}
