/*
 * Sound effects for bare_app_doom: Doom's sound_module_t, mixed in a
 * thread of its own and written to an ALSA PCM device through the kernel's
 * ioctls (no alsa-lib, no SDL). The device comes with "-pcm DEVICE" from
 * bare_app; without it /dev/snd/pcmC0D0p is tried. Music is not played:
 * the music module is a stub.
 *
 * Sounds are the DMX lumps of the WAD (8-bit unsigned, usually 11025 Hz),
 * resampled with linear interpolation. Channel volume and stereo
 * separation follow the SDL module of Chocolate Doom.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sound/asound.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "doomtype.h"
#include "i_sound.h"
#include "m_argv.h"
#include "w_wad.h"
#include "z_zone.h"

#define DEFAULT_PCM "/dev/snd/pcmC0D0p"
#define NUM_CHANNELS 16
#define PERIOD 512		/* frames: about 11 ms */
#define PERIODS 4
#define OPEN_TRIES 20		/* bare_app may still close the device after a click */

/* Chocolate Doom's config file knows these; they mean nothing here */
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

struct channel {
	const byte *data;	/* unsigned 8 bit */
	unsigned length;	/* samples */
	unsigned pos, step;	/* 16.16 fixed point */
	int left, right;	/* 0..254 */
	boolean active;
};

static struct {
	int fd;
	unsigned rate;
	boolean use_prefix;
	pthread_t thread;
	pthread_mutex_t lock;
	volatile boolean quit;
	struct channel ch[NUM_CHANNELS];
} snd = { .fd = -1, .lock = PTHREAD_MUTEX_INITIALIZER };

/* ---- PCM -------------------------------------------------------------- */

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

static int pcm_setup(int fd, unsigned rate)
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

static int pcm_open(const char *dev)
{
	static const unsigned rates[] = { 48000, 44100 };
	int fd = -1, i;
	size_t r;

	for (i = 0; i < OPEN_TRIES && fd < 0; i++) {
		fd = open(dev, O_RDWR | O_CLOEXEC);
		if (fd < 0 && errno != EBUSY)
			break;
		if (fd < 0)
			usleep(100000);
	}
	if (fd < 0) {
		fprintf(stderr, "doom sound: %s: %s\n", dev, strerror(errno));
		return -1;
	}
	for (r = 0; r < sizeof(rates) / sizeof(rates[0]); r++)
		if (pcm_setup(fd, rates[r]) == 0) {
			snd.rate = rates[r];
			return fd;
		}
	fprintf(stderr, "doom sound: %s: cannot set up 16-bit stereo: %s\n", dev, strerror(errno));
	close(fd);
	return -1;
}

/* ---- mixing ----------------------------------------------------------- */

static int sample_at(const struct channel *c, unsigned pos)
{
	unsigned i = pos >> 16, f = pos & 0xffff;
	int a = (c->data[i] - 128) << 8;
	int b = i + 1 < c->length ? (c->data[i + 1] - 128) << 8 : a;

	return a + (int)(((long)(b - a) * f) >> 16);
}

static void mix(short *out, unsigned frames)
{
	int buf[PERIOD * 2];
	unsigned i, k;

	memset(buf, 0, sizeof(buf));
	pthread_mutex_lock(&snd.lock);
	for (k = 0; k < NUM_CHANNELS; k++) {
		struct channel *c = &snd.ch[k];

		for (i = 0; c->active && i < frames; i++) {
			int s = sample_at(c, c->pos);

			buf[2 * i] += s * c->left / 254;
			buf[2 * i + 1] += s * c->right / 254;
			c->pos += c->step;
			if ((c->pos >> 16) >= c->length)
				c->active = false;
		}
	}
	pthread_mutex_unlock(&snd.lock);
	for (i = 0; i < 2 * frames; i++)
		out[i] = buf[i] > 32767 ? 32767 : buf[i] < -32768 ? -32768 : buf[i];
}

static void *mixer_thread(void *arg)
{
	short out[PERIOD * 2];
	struct snd_xferi x;

	(void)arg;
	while (!snd.quit) {
		mix(out, PERIOD);
		x.buf = out;
		x.frames = PERIOD;
		x.result = 0;
		/* blocks until the device has room: that is the clock */
		if (ioctl(snd.fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) < 0) {
			if (errno != EPIPE && errno != ESTRPIPE && errno != EINTR)
				break;
			ioctl(snd.fd, SNDRV_PCM_IOCTL_PREPARE);	/* underrun, or the device slept */
		}
	}
	return NULL;
}

/* ---- sound module ----------------------------------------------------- */

static boolean Snd_Init(boolean use_sfx_prefix)
{
	int p = M_CheckParmWithArgs("-pcm", 1);
	const char *dev = p ? myargv[p + 1] : DEFAULT_PCM;

	snd.use_prefix = use_sfx_prefix;
	snd.fd = pcm_open(dev);
	if (snd.fd < 0)
		return false;
	snd.quit = false;
	if (pthread_create(&snd.thread, NULL, mixer_thread, NULL)) {
		close(snd.fd);
		snd.fd = -1;
		return false;
	}
	printf("doom sound: %s, %u Hz\n", dev, snd.rate);
	return true;
}

static void Snd_Shutdown(void)
{
	if (snd.fd < 0)
		return;
	snd.quit = true;
	pthread_join(snd.thread, NULL);
	close(snd.fd);
	snd.fd = -1;
}

static int Snd_GetSfxLumpNum(sfxinfo_t *sfx)
{
	char name[9];

	if (sfx->link)
		sfx = sfx->link;
	/* lump names have 8 characters at most */
	if (snd.use_prefix)
		snprintf(name, sizeof(name), "ds%.6s", sfx->name);
	else
		snprintf(name, sizeof(name), "%.8s", sfx->name);
	return W_GetNumForName(name);
}

static void Snd_Update(void)
{
}

static void set_params(struct channel *c, int vol, int sep)
{
	int left = (254 - sep) * vol / 127, right = sep * vol / 127;

	c->left = left < 0 ? 0 : left > 254 ? 254 : left;
	c->right = right < 0 ? 0 : right > 254 ? 254 : right;
}

static void Snd_UpdateSoundParams(int channel, int vol, int sep)
{
	if (channel < 0 || channel >= NUM_CHANNELS)
		return;
	pthread_mutex_lock(&snd.lock);
	set_params(&snd.ch[channel], vol, sep);
	pthread_mutex_unlock(&snd.lock);
}

static int Snd_StartSound(sfxinfo_t *sfx, int channel, int vol, int sep)
{
	const byte *data;
	unsigned rate, length;
	int lumplen;
	struct channel *c;

	if (channel < 0 || channel >= NUM_CHANNELS || sfx->lumpnum < 0)
		return -1;
	data = W_CacheLumpNum(sfx->lumpnum, PU_STATIC);	/* stays: a few hundred KB in all */
	lumplen = W_LumpLength(sfx->lumpnum);
	/* DMX: format 3, 16-bit rate, 32-bit length, 16 bytes of padding on both ends */
	if (lumplen < 8 || data[0] != 0x03 || data[1] != 0x00)
		return -1;
	rate = data[3] << 8 | data[2];
	length = (unsigned)data[7] << 24 | data[6] << 16 | data[5] << 8 | data[4];
	if (length > (unsigned)lumplen - 8 || length <= 48 || !rate)
		return -1;

	c = &snd.ch[channel];
	pthread_mutex_lock(&snd.lock);
	c->data = data + 24;	/* 8-byte header + 16 bytes of padding */
	c->length = length - 32;
	c->pos = 0;
	c->step = (unsigned)(((unsigned long long)rate << 16) / snd.rate);
	set_params(c, vol, sep);
	c->active = true;
	pthread_mutex_unlock(&snd.lock);
	return channel;
}

static void Snd_StopSound(int channel)
{
	if (channel < 0 || channel >= NUM_CHANNELS)
		return;
	pthread_mutex_lock(&snd.lock);
	snd.ch[channel].active = false;
	pthread_mutex_unlock(&snd.lock);
}

static boolean Snd_SoundIsPlaying(int channel)
{
	boolean on;

	if (channel < 0 || channel >= NUM_CHANNELS)
		return false;
	pthread_mutex_lock(&snd.lock);
	on = snd.ch[channel].active;
	pthread_mutex_unlock(&snd.lock);
	return on;
}

static void Snd_CacheSounds(sfxinfo_t *sounds, int num_sounds)
{
	(void)sounds;
	(void)num_sounds;
}

static snddevice_t snd_devices[] = {
	SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER,
	SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32,
};

sound_module_t DG_sound_module = {
	snd_devices,
	sizeof(snd_devices) / sizeof(snd_devices[0]),
	Snd_Init,
	Snd_Shutdown,
	Snd_GetSfxLumpNum,
	Snd_Update,
	Snd_UpdateSoundParams,
	Snd_StartSound,
	Snd_StopSound,
	Snd_SoundIsPlaying,
	Snd_CacheSounds,
};

/* ---- music: not played ------------------------------------------------ */

static boolean Mus_Init(void) { return false; }
static void Mus_Shutdown(void) { }
static void Mus_SetMusicVolume(int volume) { (void)volume; }
static void Mus_Pause(void) { }
static void Mus_Resume(void) { }
static void *Mus_RegisterSong(void *data, int len) { (void)data; (void)len; return NULL; }
static void Mus_UnRegisterSong(void *handle) { (void)handle; }
static void Mus_PlaySong(void *handle, boolean looping) { (void)handle; (void)looping; }
static void Mus_StopSong(void) { }
static boolean Mus_IsPlaying(void) { return false; }

music_module_t DG_music_module = {
	NULL, 0,
	Mus_Init,
	Mus_Shutdown,
	Mus_SetMusicVolume,
	Mus_Pause,
	Mus_Resume,
	Mus_RegisterSong,
	Mus_UnRegisterSong,
	Mus_PlaySong,
	Mus_StopSong,
	Mus_IsPlaying,
	NULL,
};
