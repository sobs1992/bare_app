/*
 * The hardware parameters are given exactly (16-bit interleaved stereo,
 * the rate, period and buffer); everything else is left to the kernel to
 * choose, like tinyalsa does. If the rate is refused, 48 and 44.1 kHz are
 * tried.
 */
#include "pcm_alsa.h"

#include <errno.h>
#include <fcntl.h>
#include <sound/asound.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "core/log.h"

#define TAG "pcm"
#define PERIOD 512		/* frames */
#define PERIODS 4

static void mask_set(struct snd_pcm_hw_params *p, int param, unsigned bit)
{
	struct snd_mask *m = &p->masks[param - SNDRV_PCM_HW_PARAM_FIRST_MASK];

	memset(m, 0, sizeof(*m));
	m->bits[bit >> 5] = 1u << (bit & 31);
}

static void interval_set(struct snd_pcm_hw_params *p, int param, unsigned v)
{
	struct snd_interval *i = &p->intervals[param - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];

	i->min = i->max = v;
	i->integer = 1;
}

static int setup(int fd, unsigned rate)
{
	struct snd_pcm_hw_params p;
	unsigned n;

	memset(&p, 0, sizeof(p));
	for (n = 0; n < sizeof(p.masks) / sizeof(p.masks[0]); n++)
		memset(&p.masks[n], 0xff, sizeof(p.masks[n]));
	for (n = 0; n < sizeof(p.intervals) / sizeof(p.intervals[0]); n++)
		p.intervals[n].max = ~0u;
	p.rmask = ~0u;
	p.info = ~0u;
	mask_set(&p, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
	mask_set(&p, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
	mask_set(&p, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
	interval_set(&p, SNDRV_PCM_HW_PARAM_CHANNELS, 2);
	interval_set(&p, SNDRV_PCM_HW_PARAM_RATE, rate);
	interval_set(&p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, PERIOD);
	interval_set(&p, SNDRV_PCM_HW_PARAM_PERIODS, PERIODS);
	if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &p) < 0)
		return -1;
	return ioctl(fd, SNDRV_PCM_IOCTL_PREPARE);
}

static void *pcm_open(const void *params, int *rate)
{
	const struct pcm_alsa_params *p = params;
	const int rates[] = { *rate, 48000, 44100 };
	int fd = open(p->device, O_RDWR | O_CLOEXEC), *h;
	size_t i;

	if (fd < 0) {
		/* EBUSY: a game plays */
		LOGD(TAG, "%s: %s", p->device, strerror(errno));
		return NULL;
	}
	for (i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
		if (setup(fd, rates[i]) == 0) {
			*rate = rates[i];
			h = malloc(sizeof(*h));
			if (!h)
				break;
			*h = fd;
			return h;
		}
	LOGW(TAG, "%s: cannot set up 16-bit stereo: %s", p->device, strerror(errno));
	close(fd);
	return NULL;
}

static int pcm_write(void *h, const int16_t *frames, int count)
{
	int fd = *(int *)h;

	while (count > 0) {
		struct snd_xferi x = { 0, (void *)frames, (snd_pcm_uframes_t)count };

		if (ioctl(fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) < 0) {
			if (errno != EPIPE && errno != EINTR)
				return -1;
			ioctl(fd, SNDRV_PCM_IOCTL_PREPARE);	/* underrun */
			continue;
		}
		frames += 2 * x.result;
		count -= (int)x.result;
	}
	return 0;
}

static void pcm_drain(void *h)
{
	int fd = *(int *)h;

	ioctl(fd, SNDRV_PCM_IOCTL_DRAIN);
	ioctl(fd, SNDRV_PCM_IOCTL_PREPARE);	/* ready for the next write */
}

static void pcm_close(void *h)
{
	close(*(int *)h);
	free(h);
}

const struct hal_pcm_ops pcm_alsa = {
	.name = "pcm-alsa",
	.open = pcm_open,
	.write = pcm_write,
	.drain = pcm_drain,
	.close = pcm_close,
};
