/*
 * Interface sounds: control asks with sound_play(), a thread of its own
 * plays (writes block). Requests that come while a sound plays collapse
 * into one. The device stays open for a burst of sounds (a dragged slider)
 * and is closed a second after the last one: games play to the same
 * device and need it free.
 */
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

#include "core/log.h"
#include "core/util.h"
#include "hal/platform.h"
#include "services.h"

#define TAG "sound"
#define RATE 48000
#define IDLE_CLOSE_MS 1000
#define LEAD_MS 20		/* silence after opening: the amplifier powers up */

static struct {
	const struct hal_pcm_ops *ops;
	const void *params;
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	int pending;		/* enum sound_id, -1 none */
	bool quit, running;
} so = { .lock = PTHREAD_MUTEX_INITIALIZER, .pending = -1 };

/* Volume click: a short 880 Hz tone with soft edges */
static void play_click(void *h, int rate, bool fresh)
{
	int lead = fresh ? rate * LEAD_MS / 1000 : 0, len = rate * 50 / 1000;
	int fade = rate * 6 / 1000, n = lead + len, i;
	int16_t *buf = calloc((size_t)n * 2, sizeof(*buf));
	const struct hal_pcm_ops *ops = so.ops;

	if (!buf)
		return;
	for (i = 0; i < len; i++) {
		double env = i < fade ? (double)i / fade : i > len - fade ? (double)(len - i) / fade : 1;
		int16_t v = (int16_t)(9000 * env * sin(2 * M_PI * 880 * i / rate));

		buf[2 * (lead + i)] = buf[2 * (lead + i) + 1] = v;
	}
	if (ops->write(h, buf, n) == 0)
		ops->drain(h);
	free(buf);
}

static void *player(void *ud)
{
	struct timespec until;
	void *h = NULL;
	int rate = RATE, id;
	bool fresh;

	(void)ud;
	pthread_mutex_lock(&so.lock);
	while (!so.quit) {
		if (so.pending < 0) {
			if (!h) {
				pthread_cond_wait(&so.cond, &so.lock);
				continue;
			}
			clock_gettime(CLOCK_MONOTONIC, &until);
			until.tv_sec += IDLE_CLOSE_MS / 1000;
			if (pthread_cond_timedwait(&so.cond, &so.lock, &until) == ETIMEDOUT &&
			    so.pending < 0) {
				so.ops->close(h);
				h = NULL;
			}
			continue;
		}
		id = so.pending;
		so.pending = -1;
		pthread_mutex_unlock(&so.lock);
		fresh = !h;
		if (!h)
			h = so.ops->open(so.params, &rate);
		if (h && id == SOUND_VOLUME)
			play_click(h, rate, fresh);
		pthread_mutex_lock(&so.lock);
	}
	pthread_mutex_unlock(&so.lock);
	if (h)
		so.ops->close(h);
	return NULL;
}

int sound_start(const struct platform *pf)
{
	pthread_condattr_t ca;

	so.ops = pf->pcm.ops;
	so.params = pf->pcm.params;
	so.pending = -1;
	so.quit = false;
	if (!so.ops)
		return 0;
	pthread_condattr_init(&ca);
	pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
	pthread_cond_init(&so.cond, &ca);
	pthread_condattr_destroy(&ca);
	if (pthread_create(&so.thread, NULL, player, NULL)) {
		LOGW(TAG, "no sound thread");
		so.ops = NULL;
		return 0;
	}
	so.running = true;
	return 0;
}

void sound_stop(void)
{
	if (!so.running)
		return;
	pthread_mutex_lock(&so.lock);
	so.quit = true;
	pthread_cond_signal(&so.cond);
	pthread_mutex_unlock(&so.lock);
	pthread_join(so.thread, NULL);
	pthread_cond_destroy(&so.cond);
	so.running = false;
	so.ops = NULL;
}

void sound_play(enum sound_id id)
{
	if (!so.running)
		return;
	pthread_mutex_lock(&so.lock);
	so.pending = id;
	pthread_cond_signal(&so.cond);
	pthread_mutex_unlock(&so.lock);
}
