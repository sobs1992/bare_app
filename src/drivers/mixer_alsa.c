/*
 * Controls are found by name (mixer interface first, then card: jacks
 * live there). The volume uses the control's dB range when it has one,
 * so the percent follows the same curve as alsamixer. Change events of
 * the card (the jack, amixer in a shell) make the fd readable.
 */
#include "mixer_alsa.h"

#include <errno.h>
#include <fcntl.h>
#include <sound/asound.h>
#include <sound/tlv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "core/log.h"
#include "core/util.h"
#include "core/volume.h"

#define TAG "alsa"

struct mixer {
	const struct mixer_alsa_params *p;
	int fd;
	struct snd_ctl_elem_id vol;
	unsigned vol_count;
	long vol_min, vol_max;
	bool db;			/* the raw value is linear in dB */
	int db_min, db_max;		/* 0.01 dB */
	struct snd_ctl_elem_id route;	/* numid 0: none */
	int item_speaker, item_headphones;
	struct snd_ctl_elem_id jack;	/* numid 0: none */
};

static int find(int fd, const char *name, struct snd_ctl_elem_info *info)
{
	static const int ifaces[] = { SNDRV_CTL_ELEM_IFACE_MIXER, SNDRV_CTL_ELEM_IFACE_CARD };
	size_t i;

	for (i = 0; name && i < ARRAY_SIZE(ifaces); i++) {
		memset(info, 0, sizeof(*info));
		info->id.iface = ifaces[i];
		snprintf((char *)info->id.name, sizeof(info->id.name), "%s", name);
		if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, info) == 0)
			return 0;
	}
	return -1;
}

static int find_item(int fd, struct snd_ctl_elem_info *info, const char *item)
{
	struct snd_ctl_elem_info e;
	unsigned i;

	for (i = 0; item && i < info->value.enumerated.items; i++) {
		e = *info;
		e.value.enumerated.item = i;
		if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &e) == 0 &&
		    !strcmp(e.value.enumerated.name, item))
			return (int)i;
	}
	return -1;
}

/* dB range from the control's TLV data; leaves m->db false if it has none */
static void read_db_range(struct mixer *m)
{
	unsigned int buf[2 + 16];
	struct snd_ctl_tlv *t = (struct snd_ctl_tlv *)buf;
	const unsigned int *v = t->tlv;

	t->numid = m->vol.numid;
	t->length = sizeof(buf) - sizeof(*t);
	if (ioctl(m->fd, SNDRV_CTL_IOCTL_TLV_READ, t) < 0 || v[1] < 2 * sizeof(int))
		return;
	switch (v[0]) {
	case SNDRV_CTL_TLVT_DB_SCALE:	/* min, step (+ mute flag) */
		m->db_min = (int)v[2];
		m->db_max = m->db_min + (int)(v[3] & 0xffff) * (int)(m->vol_max - m->vol_min);
		break;
	case SNDRV_CTL_TLVT_DB_MINMAX:
	case SNDRV_CTL_TLVT_DB_MINMAX_MUTE:
		m->db_min = (int)v[2];
		m->db_max = (int)v[3];
		break;
	default:		/* DB_LINEAR, DB_RANGE: use the raw scale */
		return;
	}
	m->db = m->db_max > m->db_min;
}

static void *mixer_open(const void *params)
{
	const struct mixer_alsa_params *p = params;
	struct snd_ctl_elem_info info;
	struct mixer *m;
	int on = 1;

	m = calloc(1, sizeof(*m));
	if (!m)
		return NULL;
	m->p = p;
	m->fd = open(p->device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (m->fd < 0) {
		LOGW(TAG, "%s: %s", p->device, strerror(errno));
		goto fail;
	}
	if (find(m->fd, p->volume, &info) < 0 || info.type != SNDRV_CTL_ELEM_TYPE_INTEGER ||
	    info.value.integer.max <= info.value.integer.min) {
		LOGW(TAG, "no volume control \"%s\"", p->volume);
		goto fail;
	}
	m->vol = info.id;
	m->vol_count = MIN(info.count, ARRAY_SIZE(((struct snd_ctl_elem_value *)0)->value.integer.value));
	m->vol_min = info.value.integer.min;
	m->vol_max = info.value.integer.max;
	read_db_range(m);

	if (find(m->fd, p->route, &info) == 0 && info.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
		m->item_speaker = find_item(m->fd, &info, p->route_speaker);
		m->item_headphones = find_item(m->fd, &info, p->route_headphones);
		if (m->item_speaker >= 0 && m->item_headphones >= 0)
			m->route = info.id;
	}
	if (find(m->fd, p->jack, &info) == 0 && info.type == SNDRV_CTL_ELEM_TYPE_BOOLEAN)
		m->jack = info.id;
	if (ioctl(m->fd, SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS, &on) < 0)
		LOGW(TAG, "no change events: %s", strerror(errno));
	LOGI(TAG, "volume \"%s\" %ld..%ld%s, route %s, jack %s", p->volume, m->vol_min, m->vol_max,
	     m->db ? " (dB)" : "", m->route.numid ? "yes" : "no", m->jack.numid ? "yes" : "no");
	return m;

fail:
	if (m->fd >= 0)
		close(m->fd);
	free(m);
	return NULL;
}

static int mixer_fd(void *h)
{
	return ((struct mixer *)h)->fd;
}

static int read_value(struct mixer *m, const struct snd_ctl_elem_id *id,
		      struct snd_ctl_elem_value *v)
{
	memset(v, 0, sizeof(*v));
	v->id = *id;
	return ioctl(m->fd, SNDRV_CTL_IOCTL_ELEM_READ, v);
}

static int raw_to_percent(struct mixer *m, long raw)
{
	long range = m->vol_max - m->vol_min;

	raw = CLAMP(raw, m->vol_min, m->vol_max);
	if (!m->db)
		return (int)(((raw - m->vol_min) * 100 + range / 2) / range);
	return volume_db_to_percent(m->db_min + (int)((raw - m->vol_min) * (m->db_max - m->db_min) / range),
				    m->db_min, m->db_max);
}

static long percent_to_raw(struct mixer *m, int percent)
{
	long range = m->vol_max - m->vol_min, db_range = m->db_max - m->db_min;

	percent = CLAMP(percent, 0, 100);
	if (!m->db)
		return m->vol_min + (percent * range + 50) / 100;
	return m->vol_min + ((long)(volume_percent_to_db(percent, m->db_min, m->db_max) - m->db_min) *
			     range + db_range / 2) / db_range;
}

static int mixer_read(void *h, struct hal_mixer_state *st)
{
	struct mixer *m = h;
	struct snd_ctl_event ev;
	struct snd_ctl_elem_value v;

	while (read(m->fd, &ev, sizeof(ev)) > 0)
		;	/* only "something changed" matters */
	memset(st, 0, sizeof(*st));
	st->volume = read_value(m, &m->vol, &v) == 0 ? raw_to_percent(m, v.value.integer.value[0]) : -1;
	if (m->route.numid && read_value(m, &m->route, &v) == 0) {
		st->has_route = true;
		st->route = (int)v.value.enumerated.item[0] == m->item_headphones ? HAL_ROUTE_HEADPHONES
										  : HAL_ROUTE_SPEAKER;
	}
	if (m->jack.numid && read_value(m, &m->jack, &v) == 0) {
		st->has_jack = true;
		st->jack = v.value.integer.value[0];
	}
	return 0;
}

static int mixer_set_volume(void *h, int percent)
{
	struct mixer *m = h;
	struct snd_ctl_elem_value v;
	long raw = percent_to_raw(m, percent);
	unsigned i;

	memset(&v, 0, sizeof(v));
	v.id = m->vol;
	for (i = 0; i < m->vol_count; i++)
		v.value.integer.value[i] = raw;
	return ioctl(m->fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &v) < 0 ? -1 : 0;
}

static int mixer_set_route(void *h, enum hal_audio_route route)
{
	struct mixer *m = h;
	struct snd_ctl_elem_value v;

	if (!m->route.numid)
		return -1;
	memset(&v, 0, sizeof(v));
	v.id = m->route;
	v.value.enumerated.item[0] = route == HAL_ROUTE_HEADPHONES ? m->item_headphones
								    : m->item_speaker;
	return ioctl(m->fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &v) < 0 ? -1 : 0;
}

static void mixer_close(void *h)
{
	struct mixer *m = h;

	close(m->fd);
	free(m);
}

const struct hal_mixer_ops mixer_alsa = {
	.name = "mixer-alsa",
	.open = mixer_open,
	.fd = mixer_fd,
	.read = mixer_read,
	.set_volume = mixer_set_volume,
	.set_route = mixer_set_route,
	.close = mixer_close,
};
