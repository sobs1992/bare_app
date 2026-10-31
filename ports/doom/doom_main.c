/*
 * bare_app_doom: the doomgeneric platform layer for bare_app (GPLv2, like
 * doomgeneric it is linked with). Started by bare_app's src/games/doom.c
 * with the socket and the frame memory of doom_ipc.h; draws frames scaled to the video layer (4:3, centred) and
 * turns gamepad buttons into Doom keys. Arguments are Doom's own
 * (bare_app passes -iwad). Config and saved games go to the
 * current directory.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "doomgeneric.h"
#include "doomkeys.h"
#include "doomstat.h"
#include "i_system.h"
#include "m_controls.h"

#include "core/input.h"
#include "games/doom_ipc.h"

#define SRC_W DOOMGENERIC_RESX
#define SRC_H DOOMGENERIC_RESY
#define QUEUE 64
#define WEAPON_HOLD_MS 100	/* a digit key must be down for a whole tic */

/* m_menu.c */
extern int messageToPrint;
extern boolean messageNeedsInput;

static struct doom_shm *shm;
static unsigned back;		/* buffer being drawn */
static int dst_x, dst_y, dst_w, dst_h;
static int *xmap, *ymap;
static volatile sig_atomic_t stop;
static uint32_t last[SRC_W * SRC_H];	/* the frame published last */
static bool have_last;

static struct {
	unsigned char pressed, key;
} queue[QUEUE];
static unsigned q_head, q_tail;
static unsigned char down_key[GP_COUNT];	/* key sent for a held button, 0 none */
static unsigned char weapon_key;
static uint32_t weapon_release;

static void push(int pressed, unsigned char key)
{
	if (q_head - q_tail == QUEUE)
		return;
	queue[q_head % QUEUE].pressed = pressed;
	queue[q_head % QUEUE].key = key;
	q_head++;
}

/* ---- buttons -> keys ---------------------------------------------------- */

static bool in_menu(void)
{
	return menuactive || messageToPrint;
}

/*
 * Weapon slots behind the digit keys 1..7. The next slot after the current
 * weapon's one that holds an owned weapon; 0 if there is none.
 */
static unsigned char next_weapon_key(void)
{
	static const unsigned char slot_of[NUMWEAPONS] = {
		[wp_fist] = 1, [wp_chainsaw] = 1, [wp_pistol] = 2, [wp_shotgun] = 3,
		[wp_supershotgun] = 3, [wp_chaingun] = 4, [wp_missile] = 5, [wp_plasma] = 6,
		[wp_bfg] = 7,
	};
	const player_t *p = &players[consoleplayer];
	weapontype_t cur = p->pendingweapon != wp_nochange ? p->pendingweapon : p->readyweapon;
	int slot = slot_of[cur], i, w;

	for (i = 1; i <= 7; i++) {
		int s = (slot + i - 1) % 7 + 1;

		for (w = 0; w < NUMWEAPONS; w++)
			if (p->weaponowned[w] && (int)slot_of[w] == s && w != (int)cur)
				return '0' + s;
	}
	return 0;
}

/*
 * One key per button, chosen when it is pressed: A is Enter in the menu,
 * "y" in a yes/no question, fire in the game.
 */
static unsigned char key_for(enum gp_button b)
{
	switch (b) {
	case GP_UP:     return KEY_UPARROW;
	case GP_DOWN:   return KEY_DOWNARROW;
	case GP_LEFT:   return KEY_LEFTARROW;
	case GP_RIGHT:  return KEY_RIGHTARROW;
	case GP_A:
		if (messageToPrint && messageNeedsInput)
			return key_menu_confirm;
		return in_menu() ? KEY_ENTER : KEY_FIRE;
	case GP_B:      return in_menu() ? KEY_BACKSPACE : KEY_USE;
	case GP_X:      return KEY_RSHIFT;		/* run */
	case GP_L1:     return KEY_STRAFE_L;
	case GP_R1:     return KEY_STRAFE_R;
	case GP_L2:     return KEY_TAB;		/* automap */
	case GP_R2:     return KEY_FIRE;
	case GP_SELECT: return KEY_TAB;
	case GP_START:
	case GP_F1:     return KEY_ESCAPE;		/* menu */
	case GP_F2:     return KEY_F2;		/* save */
	case GP_F3:     return KEY_F3;		/* load */
	case GP_F4:     return KEY_PAUSE;
	default:        return 0;
	}
}

static void on_button(enum gp_button b, bool pressed)
{
	unsigned char key;

	if ((unsigned)b >= GP_COUNT)
		return;
	if (b == GP_Y) {		/* next weapon: a short digit key press */
		if (pressed && !weapon_key && gamestate == GS_LEVEL && !in_menu() &&
		    (key = next_weapon_key())) {
			push(1, key);
			weapon_key = key;
			weapon_release = DG_GetTicksMs() + WEAPON_HOLD_MS;
		}
		return;
	}
	if (pressed) {
		key = key_for(b);
		if (!key || down_key[b])
			return;
		down_key[b] = key;
		push(1, key);
	} else if (down_key[b]) {
		push(0, down_key[b]);	/* the key it pressed, even if the state changed */
		down_key[b] = 0;
	}
}

static void pump(void)
{
	struct doom_msg m;
	ssize_t n;

	if (stop)
		I_Quit();	/* SIGTERM from bare_app: leave saving the config */
	for (;;) {
		n = recv(DOOM_IPC_SOCK, &m, sizeof(m), MSG_DONTWAIT);
		if (n == sizeof(m) && m.type == DOOM_MSG_BUTTON)
			on_button(m.code, m.pressed);
		else if (n == 0)
			I_Quit();	/* bare_app is gone */
		else if (n < 0 && errno != EINTR)
			break;
	}
	if (weapon_key && (int32_t)(DG_GetTicksMs() - weapon_release) >= 0) {
		push(0, weapon_key);
		weapon_key = 0;
	}
}

/* ---- doomgeneric platform ----------------------------------------------- */

void DG_Init(void)
{
	int x, y;

	/* 4:3 like on a CRT, as large as the layer allows */
	dst_h = shm->height;
	dst_w = dst_h * 4 / 3;
	if (dst_w > (int)shm->width) {
		dst_w = shm->width;
		dst_h = dst_w * 3 / 4;
	}
	dst_x = (shm->width - dst_w) / 2;
	dst_y = (shm->height - dst_h) / 2;
	xmap = malloc(sizeof(*xmap) * dst_w);
	ymap = malloc(sizeof(*ymap) * dst_h);
	if (!xmap || !ymap)
		I_Error("out of memory");
	for (x = 0; x < dst_w; x++)
		xmap[x] = x * SRC_W / dst_w;
	for (y = 0; y < dst_h; y++)
		ymap[y] = y * SRC_H / dst_h;
	back = 0;	/* the app holds 2, 'middle' has 1 */
}

void DG_DrawFrame(void)
{
	uint8_t *frame = doom_shm_frame(shm, back);
	struct doom_msg m = { DOOM_MSG_FRAME, 0, 0 };
	int x, y;

	/*
	 * TryRunTics() returns at a tic boundary even when no tic ran, so about
	 * every second frame repeats the previous one: not worth a copy and a
	 * page flip in the app.
	 */
	if (have_last && !memcmp(last, DG_ScreenBuffer, sizeof(last))) {
		pump();
		return;
	}
	memcpy(last, DG_ScreenBuffer, sizeof(last));
	have_last = true;
	for (y = 0; y < dst_h; y++) {
		uint32_t *out = (uint32_t *)(frame + (size_t)(dst_y + y) * shm->stride) + dst_x;
		const uint32_t *in = DG_ScreenBuffer + ymap[y] * SRC_W;

		if (y && ymap[y] == ymap[y - 1]) {
			memcpy(out, (uint8_t *)out - shm->stride, (size_t)dst_w * 4);
			continue;
		}
		for (x = 0; x < dst_w; x++)
			out[x] = in[xmap[x]] & 0xffffff;
	}
	back = atomic_exchange(&shm->middle, back | DOOM_SHM_FRESH) & ~DOOM_SHM_FRESH;
	/* a full socket already holds a "frame" message: nothing to add */
	if (send(DOOM_IPC_SOCK, &m, sizeof(m), MSG_DONTWAIT | MSG_NOSIGNAL) < 0 && errno == EPIPE)
		I_Quit();
	pump();
}

void DG_SleepMs(uint32_t ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

uint32_t DG_GetTicksMs(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

int DG_GetKey(int *pressed, unsigned char *key)
{
	pump();
	if (q_tail == q_head)
		return 0;
	*pressed = queue[q_tail % QUEUE].pressed;
	*key = queue[q_tail % QUEUE].key;
	q_tail++;
	return 1;
}

void DG_SetWindowTitle(const char *title)
{
	(void)title;
}

static void on_term(int sig)
{
	(void)sig;
	stop = 1;
}

int main(int argc, char **argv)
{
	struct sigaction sa = { .sa_handler = on_term };
	struct stat st;

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (fstat(DOOM_IPC_SHM, &st) < 0 || st.st_size < DOOM_SHM_HEADER) {
		fprintf(stderr, "bare_app_doom: started by bare_app only\n");
		return 2;
	}
	shm = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, DOOM_IPC_SHM, 0);
	if (shm == MAP_FAILED || shm->magic != DOOM_SHM_MAGIC ||
	    doom_shm_size(shm) > (size_t)st.st_size) {
		fprintf(stderr, "bare_app_doom: bad frame memory\n");
		return 2;
	}
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	doomgeneric_Create(argc, argv);
	for (;;)
		doomgeneric_Tick();
	return 0;
}
