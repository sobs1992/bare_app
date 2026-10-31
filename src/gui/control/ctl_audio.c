/*
 * Audio: mixer state into CTX_AUDIO, volume and output actions. "Auto"
 * follows the headphone plug: some codecs (the OGA's rk817) leave that to
 * userspace. Volume and output are kept in the settings file.
 */
#include <string.h>

#include "control.h"
#include "core/bus.h"
#include "core/log.h"
#include "core/util.h"
#include "gui/ctx.h"
#include "gui/notify.h"
#include "services/config.h"
#include "services/services.h"

#define TAG "audio"

static const char *const out_names[AUDIO_OUT_COUNT] = { "auto", "speaker", "headphones" };

static int sub;
static enum audio_output output;

static enum hal_audio_route wanted_route(const struct hal_mixer_state *m)
{
	switch (output) {
	case AUDIO_OUT_SPEAKER:
		return HAL_ROUTE_SPEAKER;
	case AUDIO_OUT_HEADPHONES:
		return HAL_ROUTE_HEADPHONES;
	default:
		return m->has_jack && m->jack ? HAL_ROUTE_HEADPHONES : HAL_ROUTE_SPEAKER;
	}
}

static void publish(const struct hal_mixer_state *m)
{
	struct gui_ctx *c = ctx_lock();

	c->audio.available = audio_available();
	c->audio.volume = m ? m->volume : -1;
	c->audio.can_route = m && m->has_route;
	c->audio.output = output;
	c->audio.headphones = m && m->has_route && m->route == HAL_ROUTE_HEADPHONES;
	c->audio.has_jack = m && m->has_jack;
	c->audio.jack = m && m->jack;
	ctx_unlock(CTX_AUDIO);
}

/* Bring the route in line with the setting and the plug */
static void apply_route(const struct hal_mixer_state *m)
{
	enum hal_audio_route r = wanted_route(m);

	if (m->has_route && m->route != r) {
		LOGI(TAG, "output: %s", r == HAL_ROUTE_HEADPHONES ? "headphones" : "speaker");
		if (audio_set_route(r) < 0)
			LOGW(TAG, "cannot switch the output");
	}
}

static void on_mixer(const struct ev *e, void *ud)
{
	struct hal_mixer_state m;

	(void)e;
	(void)ud;
	if (audio_get(&m) < 0)		/* the latest state: events may queue up */
		return;
	apply_route(&m);		/* a plug change comes here */
	audio_get(&m);
	publish(&m);
}

static int audio_init(const struct control_env *env)
{
	struct hal_mixer_state m;
	const char *out = config_get("audio_out");
	int vol = config_get_int("volume", -1), i;

	(void)env;
	output = AUDIO_OUT_AUTO;
	for (i = 0; out && i < AUDIO_OUT_COUNT; i++)
		if (!strcmp(out, out_names[i]))
			output = i;
	sub = bus_subscribe(EV_MIXER_CHANGED, on_mixer, NULL);
	if (sub <= 0)
		return -1;
	if (!audio_available() || audio_get(&m) < 0) {
		publish(NULL);
		return 0;
	}
	if (vol >= 0 && audio_set_volume(vol) < 0)
		LOGW(TAG, "cannot set volume %d%%", vol);
	audio_get(&m);
	apply_route(&m);
	audio_get(&m);
	publish(&m);
	return 0;
}

/* The next multiple of the step up or down: levels stay on the slider's marks */
static int step_target(int volume, int delta)
{
	int n = delta < 0 ? -delta : delta;

	return delta > 0 ? (volume / n + 1) * n : ((volume + n - 1) / n - 1) * n;
}

/*
 * dir 0: set as asked (slider). dir +1/-1 (volume keys): the mixer reads
 * back to the nearest of its own steps, which may not move at all, so go
 * further until the level does, and show it.
 */
static void set_volume(int percent, int dir, int from)
{
	struct hal_mixer_state m;

	for (percent = CLAMP(percent, 0, 100);; percent += dir) {
		if (audio_set_volume(percent) < 0 || audio_get(&m) < 0) {
			LOGW(TAG, "cannot set volume %d%%", percent);
			return;
		}
		if (!dir || (dir > 0 ? m.volume > from : m.volume < from) || percent <= 0 ||
		    percent >= 100)
			break;
	}
	publish(&m);	/* what the mixer really took */
	config_set_int("volume", m.volume);
	sound_play(SOUND_VOLUME);	/* hear the new level */
	if (dir)
		gui_notify_post(NOTIFY_VOLUME, m.volume);
}

static void audio_action(const struct gui_action *a)
{
	struct hal_mixer_state m;

	if (!audio_available() || audio_get(&m) < 0)
		return;
	switch (a->type) {
	case ACT_SET_VOLUME:
		set_volume(a->arg, 0, m.volume);
		break;
	case ACT_VOLUME_STEP:
		if (a->arg)
			set_volume(step_target(m.volume, a->arg), a->arg > 0 ? 1 : -1, m.volume);
		break;
	case ACT_SET_AUDIO_OUT:
		if (a->arg < 0 || a->arg >= AUDIO_OUT_COUNT)
			break;
		output = a->arg;
		config_set("audio_out", out_names[output]);
		apply_route(&m);
		audio_get(&m);
		publish(&m);
		break;
	default:
		break;
	}
}

static void audio_deinit(void)
{
	bus_unsubscribe(sub);
}

const struct control_ops ctl_audio = {
	.name = "audio",
	.init = audio_init,
	.on_action = audio_action,
	.deinit = audio_deinit,
};
