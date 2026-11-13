#define _GNU_SOURCE	/* accept4() */
#include "stream.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/sockios.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "core/log.h"
#include "core/util.h"
#include "drivers/pcm_alsa.h"
#include "h264.h"
#include "media/mailbox.h"
#include "rtp.h"
#include "ts.h"

#define TAG "stream"
#define MAX_CLIENTS 4
#define IN_MAX 4096		/* a request */
#define SR_MS 2000		/* RTCP sender reports */
#define AUDIO_RATE 48000
#define AUDIO_CHUNK 480		/* frames: 10 ms */
#define SNDBUF (1 << 20)
/* UDP: a frame goes out at once, a key frame or a new picture in a few
 * hundred packets; the default room (~90 of them) dropped some */
#define UDP_SNDBUF (2 << 20)
#define UDP_BASE 6970		/* video RTP, RTCP, audio RTP, RTCP, TS RTP, RTCP */
#define TS_DELAY 9000		/* 90 kHz: the PTS ahead of the PCR */
#define PATH "live"
#define SESSION_TIMEOUT 60	/* s, the Session header's */
#define SILENT_MS (2 * SESSION_TIMEOUT * 1000)	/* no request, no RTCP: the player is gone */
/* The bit rate follows the players: a quarter down on losses (or a TCP
 * player that falls behind), at most once a second, down to a quarter of
 * the one set; an eighth of it up again after 5 s without */
#define RATE_MIN_PART 4
#define RATE_LOSS 5		/* of 256 lost since the last report: 2% */
#define RATE_HOLD_MS 1000
#define RATE_UP_MS 5000
#define IDR_MIN_MS 1000		/* between key frames the players ask for */
#define STATS_MS 10000		/* the players' reports into the log */

/*
 * TRACK_TS: both in an MPEG transport stream, for a player that SETUPs the
 * aggregate URL instead of a track (VLC's SAT>IP module, see ts.h)
 */
enum { TRACK_VIDEO, TRACK_AUDIO, TRACK_TS, TRACKS };

struct track_out {
	bool set;		/* SETUP done */
	bool tcp;
	int channel;		/* interleaved: RTP, RTCP is the next one */
	struct sockaddr_in rtp, rtcp;	/* UDP */
	/* the player's last receiver report about it */
	bool reported;
	uint8_t fraction;	/* lost of 256 */
	int32_t lost;
	uint32_t jitter;	/* RTP time units */
	int rtt_ms;		/* -1 unknown */
};

struct client {
	int fd;			/* -1: free */
	char in[IN_MAX];
	size_t in_len;
	uint32_t session;
	bool playing;
	bool broken;		/* a TCP write failed half way, or BYE: dropped */
	uint64_t seen_ms;	/* now_ms() of its last request or RTCP */
	struct sockaddr_in peer;
	struct track_out out[TRACKS];
};

static struct {
	pthread_mutex_t lock;	/* clients, tracks, sending */
	pthread_t net, audio;
	bool net_on, audio_on;
	atomic_bool running;
	atomic_int viewers;
	int stop;		/* eventfd */
	int listen;
	int udp[TRACKS][2];	/* RTP, RTCP */
	struct stream_config cfg;
	struct mailbox *mb;
	struct h264 *enc;
	char sprop[512];	/* sprop-parameter-sets */
	struct client clients[MAX_CLIENTS];
	struct rtp_track track[TRACKS];
	struct ts_mux ts;
	uint64_t ts_audio_pts;	/* 90 kHz, of the next sound PES */
	uint64_t t0;		/* now_us() of RTP time 0 */
	bool want_idr;
	int enc_width, enc_height;	/* of the encoder's frames */
	int kbps;		/* aimed at now */
	atomic_int kbps_now;	/* the same, for other threads */
	uint64_t rate_cut_ms, rate_up_ms, idr_ms, stats_ms;
	unsigned encoded;	/* frames since stats_ms */
	unsigned unsent;	/* UDP packets the socket had no room for, since stats_ms */
	bool cut;		/* ...some of the frame being sent */
	bool audio_ok;		/* the capture device opened */
	void *cap, *play;	/* PCM handles */
	struct pcm_alsa_params cap_params, play_params;
	uint32_t audio_ts;	/* samples sent */
	uint64_t audio_at;	/* now_us() of audio_ts */
	uint8_t frame[RTP_HEADER + RTP_MAX_PAYLOAD + 4];
} st = { .lock = PTHREAD_MUTEX_INITIALIZER, .stop = -1, .listen = -1,
	 .udp = { { -1, -1 }, { -1, -1 }, { -1, -1 } } };

/* ---- the loopback --------------------------------------------------------- */

static int loopback_card(void)
{
	FILE *f = fopen("/proc/asound/cards", "r");
	char line[160];
	int card = -1, n;

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, " %d [", &n) == 1 && strstr(line, "Loopback")) {
			card = n;
			break;
		}
	fclose(f);
	return card;
}

int stream_loopback(char *play, char *capture, size_t size)
{
	int card = loopback_card();

	if (card < 0) {
		char *argv[] = { "modprobe", "snd-aloop", NULL };
		extern char **environ;
		pid_t pid;
		int status;

		if (posix_spawnp(&pid, "modprobe", NULL, NULL, argv, environ) == 0)
			waitpid(pid, &status, 0);
		card = loopback_card();
	}
	if (card < 0)
		return -1;
	snprintf(play, size, "/dev/snd/pcmC%dD0p", card);
	snprintf(capture, size, "/dev/snd/pcmC%dD1c", card);
	return 0;
}

/* ---- sending ------------------------------------------------------------- */

struct dest {
	struct client *c;
	int track;
	bool rtcp;
};

/* One packet to one client (lock held) */
static void send_to(void *ud, const uint8_t *pkt, size_t len)
{
	struct dest *d = ud;
	struct track_out *o = &d->c->out[d->track];

	if (d->c->broken)
		return;
	if (o->tcp) {
		uint8_t head[4] = { '$', (uint8_t)(o->channel + d->rtcp), (uint8_t)(len >> 8), (uint8_t)len };
		struct iovec iov[2] = { { head, 4 }, { (void *)pkt, len } };
		struct msghdr msg = { .msg_iov = iov, .msg_iovlen = 2 };
		ssize_t n = sendmsg(d->c->fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);

		/* the room was checked for the whole frame: short means the
		 * player is gone or stuck, and the stream would be cut mid-packet */
		if (n != (ssize_t)(len + 4))
			d->c->broken = true;
	} else {
		const struct sockaddr_in *to = d->rtcp ? &o->rtcp : &o->rtp;

		if (sendto(st.udp[d->track][d->rtcp], pkt, len, MSG_DONTWAIT, (const struct sockaddr *)to,
			   sizeof(*to)) < 0 && (errno == EAGAIN || errno == ENOBUFS) && !d->rtcp) {
			st.unsent++;
			st.cut = true;
		}
	}
}

/* Room for bytes in a TCP client's socket (UDP: always) */
static bool room(struct client *c, int track, size_t bytes)
{
	int queued = 0, sndbuf = SNDBUF;
	socklen_t len = sizeof(sndbuf);

	if (!c->out[track].tcp)
		return true;
	if (ioctl(c->fd, SIOCOUTQ, &queued) < 0)
		return true;
	getsockopt(c->fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, &len);
	return (size_t)(sndbuf - queued) > bytes + bytes / 8 + 1024;
}

static uint64_t unix_us(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000000 + (uint64_t)tv.tv_usec;
}

/* 90 kHz since t0 */
static uint64_t clock90(uint64_t at)
{
	return (at - st.t0) * 9 / 100;
}

static uint32_t video_ts(uint64_t at)
{
	return (uint32_t)clock90(at);
}

static uint32_t audio_ts_now(uint64_t now)
{
	return st.audio_ts + (uint32_t)((now - st.audio_at) * AUDIO_RATE / 1000000);
}

static void send_reports(void)
{
	uint64_t now = now_us(), wall = unix_us();
	uint8_t buf[128];
	int i, t;

	for (i = 0; i < MAX_CLIENTS; i++) {
		struct client *c = &st.clients[i];

		for (t = 0; c->fd >= 0 && c->playing && t < TRACKS; t++) {
			struct dest d = { c, t, true };
			uint32_t ts = t == TRACK_AUDIO ? audio_ts_now(now) : video_ts(now);
			size_t n;

			if (!c->out[t].set)
				continue;
			n = rtcp_sr(&st.track[t], wall, ts, "bare_app", buf, sizeof(buf));
			if (n && room(c, t, n))
				send_to(&d, buf, n);
		}
	}
}

/* A player of the transport stream plays (lock held) */
static bool ts_playing(void)
{
	int i;

	for (i = 0; i < MAX_CLIENTS; i++)
		if (st.clients[i].fd >= 0 && st.clients[i].playing && st.clients[i].out[TRACK_TS].set)
			return true;
	return false;
}

/* A group of TS packets to its players as one RTP packet (lock held) */
static void ts_send(void *ud, const uint8_t *packets, size_t len)
{
	size_t n = rtp_mp2t(&st.track[TRACK_TS], packets, len, *(const uint32_t *)ud, st.frame);
	int i;

	for (i = 0; i < MAX_CLIENTS; i++) {
		struct client *c = &st.clients[i];
		struct dest d = { c, TRACK_TS, false };

		if (c->fd >= 0 && c->playing && c->out[TRACK_TS].set && room(c, TRACK_TS, n))
			send_to(&d, st.frame, n);
	}
}

static void make_sprop(void);

/* ---- what the players say (RTCP) -------------------------------------- */

static void rate_set(int kbps, const char *why)
{
	int low = st.cfg.kbps / RATE_MIN_PART;

	kbps = kbps < low ? low : kbps > st.cfg.kbps ? st.cfg.kbps : kbps;
	if (kbps == st.kbps)
		return;
	st.kbps = kbps;
	atomic_store(&st.kbps_now, kbps);
	if (st.enc)
		h264_set_bitrate(st.enc, kbps);
	LOGI(TAG, "%d kbit/s: %s", kbps, why);
}

static void rate_down(const char *why)
{
	uint64_t now = now_ms();

	if (now - st.rate_cut_ms < RATE_HOLD_MS)
		return;
	st.rate_cut_ms = st.rate_up_ms = now;
	rate_set(st.kbps - st.kbps / 4, why);
}

/* Up again a step at a time after a while without losses */
static void rate_maybe_up(void)
{
	uint64_t now = now_ms();

	if (st.kbps >= st.cfg.kbps || now - st.rate_up_ms < RATE_UP_MS)
		return;
	st.rate_up_ms = now;
	rate_set(st.kbps + (st.cfg.kbps + 7) / 8, "no losses");
}

/* An RTCP packet of a player about track t (lock held) */
static void rtcp_in(struct client *c, int t, const uint8_t *buf, size_t len)
{
	struct rtcp_info info;
	struct track_out *o = &c->out[t];
	uint64_t wall = unix_us(), now = now_ms();
	int i;

	if (!rtcp_parse(buf, len, &info)) {
		LOGD(TAG, "%s: %zu bytes on the RTCP of track %d, not RTCP", inet_ntoa(c->peer.sin_addr), len, t);
		return;
	}
	LOGD(TAG, "%s: RTCP of track %d, %d report blocks%s%s", inet_ntoa(c->peer.sin_addr), t, info.blocks,
	     info.bye ? ", BYE" : "", info.key_frame ? ", key frame wanted" : "");
	c->seen_ms = now;
	for (i = 0; i < info.blocks; i++) {
		const struct rtcp_block *b = &info.block[i];

		if (b->ssrc != st.track[t].ssrc)
			continue;
		o->reported = true;
		o->fraction = b->fraction;
		o->lost = b->lost;
		o->jitter = b->jitter;
		o->rtt_ms = rtcp_rtt_ms(b, wall);
		if (t != TRACK_AUDIO && b->fraction >= RATE_LOSS)
			rate_down("the player loses packets");
	}
	if (info.key_frame && now - st.idr_ms >= IDR_MIN_MS) {
		st.idr_ms = now;
		st.want_idr = true;
	}
	if (info.bye) {
		LOGI(TAG, "%s says bye", inet_ntoa(c->peer.sin_addr));
		c->broken = true;
	}
}

/* RTCP that came to the UDP port of track t: from the player of that address */
static void rtcp_udp(int t, int fd)
{
	uint8_t buf[1500];
	struct sockaddr_in from;
	socklen_t len = sizeof(from);
	ssize_t n;
	int i;

	while ((n = recvfrom(fd, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr *)&from, &len)) > 0) {
		struct client *any = NULL;

		for (i = 0; i < MAX_CLIENTS; i++) {
			struct client *c = &st.clients[i];
			const struct track_out *o = &c->out[t];

			if (c->fd < 0 || !o->set || o->tcp || o->rtcp.sin_addr.s_addr != from.sin_addr.s_addr)
				continue;
			any = c;
			if (o->rtcp.sin_port == from.sin_port)
				break;	/* the very one; else another player of the same address */
		}
		if (any)
			rtcp_in(i < MAX_CLIENTS ? &st.clients[i] : any, t, buf, (size_t)n);
		len = sizeof(from);
	}
}

static void log_reports(void)
{
	static const char *const name[TRACKS] = { "video", "sound", "MPEG-TS" };
	char line[320];
	int i, t, n;

	for (i = 0; i < MAX_CLIENTS; i++) {
		const struct client *c = &st.clients[i];
		int rate = 0;

		if (c->fd < 0 || !c->playing)
			continue;
		n = snprintf(line, sizeof(line), "%s:", inet_ntoa(c->peer.sin_addr));
		for (t = 0; t < TRACKS; t++) {
			const struct track_out *o = &c->out[t];

			if (!o->set || !o->reported || n >= (int)sizeof(line))
				continue;
			rate = t == TRACK_AUDIO ? AUDIO_RATE / 1000 : 90;
			n += snprintf(line + n, sizeof(line) - (size_t)n,
				      " %s lost %d.%d%% (%d in all), jitter %u ms", name[t],
				      o->fraction * 100 / 256, o->fraction * 1000 / 256 % 10,
				      o->lost > 0 ? (int)o->lost : 0,	/* duplicates make it negative */
				      o->jitter / (unsigned)rate);
			if (o->rtt_ms >= 0 && n < (int)sizeof(line))
				n += snprintf(line + n, sizeof(line) - (size_t)n, ", round trip %d ms", o->rtt_ms);
		}
		if (rate)
			LOGI(TAG, "%s", line);
	}
	LOGI(TAG, "%dx%d, %u.%u frames/s encoded, %d kbit/s", st.enc_width, st.enc_height,
	     st.encoded * 1000 / STATS_MS, st.encoded * 10000 / STATS_MS % 10, st.kbps);
	if (st.unsent)
		LOGW(TAG, "%u packets not sent: no room in the socket", st.unsent);
	st.encoded = 0;
	st.unsent = 0;
}

/* The encoder for frames of another size (lock held) */
static int encoder_size(int width, int height)
{
	struct h264 *e;

	width &= ~1;
	height &= ~1;
	if (st.enc && width == st.enc_width && height == st.enc_height)
		return 0;
	e = h264_new(width, height, st.cfg.fps, st.kbps);
	if (!e) {
		LOGW(TAG, "no encoder for %dx%d", width, height);
		return -1;
	}
	if (st.enc)
		LOGI(TAG, "frames of %dx%d now", width, height);
	h264_free(st.enc);
	st.enc = e;
	st.enc_width = width;
	st.enc_height = height;
	make_sprop();
	st.want_idr = false;	/* it starts with one, its parameters in front */
	return 0;
}

static void encode_and_send(void)
{
	struct hal_fb *fb;
	const uint8_t *au;
	size_t size;
	uint64_t now = now_us();
	int idr, i;

	mailbox_ack(st.mb);
	fb = mailbox_take(st.mb);
	if (!fb)
		return;
	pthread_mutex_lock(&st.lock);
	i = encoder_size(fb->width, fb->height);
	pthread_mutex_unlock(&st.lock);
	if (i < 0)
		return;
	if (st.want_idr) {
		h264_force_idr(st.enc);
		st.want_idr = false;
	}
	if (h264_encode(st.enc, fb, now / 1000, &au, &size, &idr) < 0) {
		LOGW(TAG, "a frame failed to encode");
		return;
	}
	st.encoded++;
	if (!size)
		return;
	pthread_mutex_lock(&st.lock);
	for (i = 0; i < MAX_CLIENTS; i++) {
		struct client *c = &st.clients[i];
		struct dest d = { c, TRACK_VIDEO, false };

		if (c->fd < 0 || !c->playing || !c->out[TRACK_VIDEO].set)
			continue;
		if (!room(c, TRACK_VIDEO, size + size / RTP_MAX_PAYLOAD * 20 + 64)) {
			st.want_idr = true;	/* a frame missed: start over from a key frame */
			rate_down("a TCP player falls behind");
			continue;
		}
		rtp_h264(&st.track[TRACK_VIDEO], au, size, video_ts(now), send_to, &d);
	}
	if (ts_playing()) {
		uint64_t clock = clock90(now);
		uint32_t rtp_time = (uint32_t)clock;
		const uint8_t *params = NULL;
		size_t params_size = 0;

		if (idr && h264_params(st.enc, &params, &params_size) < 0)
			params = NULL;
		ts_video(&st.ts, au, size, idr, params, params_size, clock + TS_DELAY, clock, ts_send,
			 &rtp_time);
	}
	if (st.cut) {
		/* the players got a broken picture: a key frame, less to send */
		st.cut = false;
		rate_down("packets not sent: the link is slower than the stream");
		if (now / 1000 - st.idr_ms >= IDR_MIN_MS) {
			st.idr_ms = now / 1000;
			st.want_idr = true;
		}
	}
	pthread_mutex_unlock(&st.lock);
}

/* ---- RTSP ------------------------------------------------------------------- */

static void base64(const uint8_t *in, size_t n, char *out, size_t size)
{
	static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t i, o = 0;

	for (i = 0; i < n && o + 5 < size; i += 3) {
		uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) |
			     (i + 2 < n ? in[i + 2] : 0);

		out[o++] = abc[v >> 18 & 63];
		out[o++] = abc[v >> 12 & 63];
		out[o++] = i + 1 < n ? abc[v >> 6 & 63] : '=';
		out[o++] = i + 2 < n ? abc[v & 63] : '=';
	}
	out[o] = '\0';
}

/* SPS and PPS from the encoder, as "sps,pps" in base64 */
static void make_sprop(void)
{
	const uint8_t *p;
	size_t n, i, start = 0, len = 0;
	char b64[256];
	int nal = 0;

	st.sprop[0] = '\0';
	if (h264_params(st.enc, &p, &n) < 0)
		return;
	for (i = 0; i + 3 <= n; i++) {
		if (p[i] || p[i + 1] || p[i + 2] != 1)
			continue;
		if (nal) {
			len = i - start;
			while (len && !p[start + len - 1])
				len--;
			base64(p + start, len, b64, sizeof(b64));
			snprintf(st.sprop + strlen(st.sprop), sizeof(st.sprop) - strlen(st.sprop), "%s%s",
				 nal > 1 ? "," : "", b64);
		}
		start = i + 3;
		nal++;
	}
	if (nal) {
		base64(p + start, n - start, b64, sizeof(b64));
		snprintf(st.sprop + strlen(st.sprop), sizeof(st.sprop) - strlen(st.sprop), "%s%s",
			 nal > 1 ? "," : "", b64);
	}
}

/* The value of header name in the request, copied into buf ("" if none) */
static const char *header(const char *req, const char *name, char *buf, size_t size)
{
	size_t nl = strlen(name);
	const char *p = req;

	buf[0] = '\0';
	while ((p = strchr(p, '\n'))) {
		p++;
		if (!strncasecmp(p, name, nl) && p[nl] == ':') {
			size_t len;

			p += nl + 1;
			while (*p == ' ')
				p++;
			len = strcspn(p, "\r\n");
			snprintf(buf, size, "%.*s", (int)(len < size ? len : size - 1), p);
			break;
		}
	}
	return buf;
}

static void reply(struct client *c, int code, const char *what, const char *cseq, const char *extra,
		  const char *body)
{
	char out[2048];
	int n;

	n = snprintf(out, sizeof(out), "RTSP/1.0 %d %s\r\nCSeq: %s\r\nServer: bare_app\r\n%s",
		     code, what, cseq, extra ? extra : "");
	if (c->session)
		n += snprintf(out + n, sizeof(out) - (size_t)n, "Session: %08X;timeout=%d\r\n", c->session,
			      SESSION_TIMEOUT);
	if (body)
		n += snprintf(out + n, sizeof(out) - (size_t)n,
			      "Content-Type: application/sdp\r\nContent-Length: %zu\r\n", strlen(body));
	n += snprintf(out + n, sizeof(out) - (size_t)n, "\r\n%s", body ? body : "");
	if (n > 0 && (size_t)n < sizeof(out))
		send(c->fd, out, (size_t)n, MSG_NOSIGNAL);
}

static void describe(struct client *c, const char *cseq, const char *url)
{
	struct sockaddr_in self;
	socklen_t len = sizeof(self);
	char sdp[1400], ip[INET_ADDRSTRLEN] = "0.0.0.0", extra[600];
	int n;

	if (getsockname(c->fd, (struct sockaddr *)&self, &len) == 0)
		inet_ntop(AF_INET, &self.sin_addr, ip, sizeof(ip));
	n = snprintf(sdp, sizeof(sdp),
		     "v=0\r\no=- %u 1 IN IP4 %s\r\ns=bare_app\r\nc=IN IP4 0.0.0.0\r\nt=0 0\r\n"
		     "a=control:*\r\na=range:npt=0-\r\n"
		     "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
		     "a=fmtp:96 packetization-mode=1%s%s\r\n"
		     "a=framerate:%d\r\na=control:track1\r\n",
		     st.track[TRACK_VIDEO].ssrc, ip, st.sprop[0] ? ";sprop-parameter-sets=" : "",
		     st.sprop, st.cfg.fps);
	if (st.audio_ok)
		snprintf(sdp + n, sizeof(sdp) - (size_t)n,
			 "m=audio 0 RTP/AVP 97\r\na=rtpmap:97 L16/%d/2\r\na=control:track2\r\n",
			 AUDIO_RATE);
	snprintf(extra, sizeof(extra), "Content-Base: %s/\r\n", url);
	reply(c, 200, "OK", cseq, extra, sdp);
}

static void setup(struct client *c, const char *cseq, const char *url, const char *transport)
{
	int t = strstr(url, "/track2") ? TRACK_AUDIO : strstr(url, "/track1") ? TRACK_VIDEO : TRACK_TS;
	struct track_out *o = &c->out[t];
	char extra[256];
	const char *p;
	int a, b;

	if (t == TRACK_AUDIO && !st.audio_ok) {
		reply(c, 404, "Not Found", cseq, NULL, NULL);
		return;
	}
	if (!c->session)
		c->session = (uint32_t)rand() | 1;
	memset(o, 0, sizeof(*o));
	if (strstr(transport, "/TCP")) {
		o->tcp = true;
		o->channel = 2 * t;
		if ((p = strstr(transport, "interleaved=")) && sscanf(p, "interleaved=%d-%d", &a, &b) >= 1)
			o->channel = a;
		snprintf(extra, sizeof(extra), "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d;ssrc=%08X\r\n",
			 o->channel, o->channel + 1, st.track[t].ssrc);
	} else if ((p = strstr(transport, "client_port=")) && sscanf(p, "client_port=%d-%d", &a, &b) == 2) {
		o->rtp = c->peer;
		o->rtp.sin_port = htons((uint16_t)a);
		o->rtcp = c->peer;
		o->rtcp.sin_port = htons((uint16_t)b);
		snprintf(extra, sizeof(extra),
			 "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d;ssrc=%08X\r\n",
			 a, b, UDP_BASE + 2 * t, UDP_BASE + 2 * t + 1, st.track[t].ssrc);
	} else {
		reply(c, 461, "Unsupported Transport", cseq, NULL, NULL);
		return;
	}
	o->set = true;
	reply(c, 200, "OK", cseq, extra, NULL);
}

static void play(struct client *c, const char *cseq, const char *url)
{
	static const char *const name[TRACKS] = { "/track1", "/track2", "" };
	char extra[600];
	uint64_t now = now_us();
	const char *sep = "";
	int n, t;

	n = snprintf(extra, sizeof(extra), "Range: npt=0.000-\r\nRTP-Info: ");
	for (t = 0; t < TRACKS; t++) {
		if (!c->out[t].set)
			continue;
		n += snprintf(extra + n, sizeof(extra) - (size_t)n, "%surl=%s%s;seq=%u;rtptime=%u", sep, url,
			      name[t], st.track[t].seq, t == TRACK_AUDIO ? audio_ts_now(now) : video_ts(now));
		sep = ",";
	}
	snprintf(extra + n, sizeof(extra) - (size_t)n, "\r\n");
	reply(c, 200, "OK", cseq, extra, NULL);
	if (!c->playing)
		atomic_fetch_add(&st.viewers, 1);
	c->playing = true;
	st.want_idr = true;
	if (c->out[TRACK_TS].set)
		ts_restart(&st.ts);
	t = c->out[TRACK_TS].set ? TRACK_TS : TRACK_VIDEO;
	LOGI(TAG, "%s plays (%s%s)", inet_ntoa(c->peer.sin_addr), t == TRACK_TS ? "MPEG-TS, " : "",
	     c->out[t].tcp ? "TCP" : "UDP");
}

static void client_close(struct client *c)
{
	if (c->fd < 0)
		return;
	if (c->playing)
		atomic_fetch_sub(&st.viewers, 1);
	LOGI(TAG, "%s gone", inet_ntoa(c->peer.sin_addr));
	close(c->fd);
	memset(c, 0, sizeof(*c));
	c->fd = -1;
}

/* One request (lock held); false: close the connection */
static bool request(struct client *c, char *req)
{
	char method[16], url[256], cseq[16], transport[256], base[300];
	size_t len;

	if (sscanf(req, "%15s %255s", method, url) != 2)
		return false;
	c->seen_ms = now_ms();	/* any request keeps the session alive */
	header(req, "CSeq", cseq, sizeof(cseq));
	LOGD(TAG, "%s %s", method, url);
	/* the aggregate URL: what it was asked as, without a track */
	snprintf(base, sizeof(base), "%s", url);
	len = strlen(base);
	while (len && base[len - 1] == '/')
		base[--len] = '\0';
	if (len > 7 && !strcmp(base + len - 7, "/track1"))
		base[len - 7] = '\0';
	else if (len > 7 && !strcmp(base + len - 7, "/track2"))
		base[len - 7] = '\0';
	if (!strcmp(method, "OPTIONS")) {
		reply(c, 200, "OK", cseq,
		      "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, GET_PARAMETER\r\n", NULL);
	} else if (!strcmp(method, "DESCRIBE")) {
		if (!strstr(url, "/" PATH))
			reply(c, 404, "Not Found", cseq, NULL, NULL);
		else
			describe(c, cseq, base);
	} else if (!strcmp(method, "SETUP")) {
		setup(c, cseq, url, header(req, "Transport", transport, sizeof(transport)));
	} else if (!strcmp(method, "PLAY")) {
		play(c, cseq, base);
	} else if (!strcmp(method, "PAUSE")) {
		if (c->playing)
			atomic_fetch_sub(&st.viewers, 1);
		c->playing = false;
		reply(c, 200, "OK", cseq, NULL, NULL);
	} else if (!strcmp(method, "TEARDOWN")) {
		reply(c, 200, "OK", cseq, NULL, NULL);
		return false;
	} else if (!strcmp(method, "GET_PARAMETER") || !strcmp(method, "SET_PARAMETER")) {
		reply(c, 200, "OK", cseq, NULL, NULL);
	} else {
		reply(c, 501, "Not Implemented", cseq, NULL, NULL);
	}
	return true;
}

/* What came on a client's connection: requests, and the player's own
 * interleaved RTCP ($ frames) */
static void client_input(struct client *c)
{
	ssize_t n = recv(c->fd, c->in + c->in_len, sizeof(c->in) - 1 - c->in_len, MSG_DONTWAIT);
	bool keep = true;

	if (n <= 0) {
		if (n < 0 && (errno == EAGAIN || errno == EINTR))
			return;
		client_close(c);
		return;
	}
	c->in_len += (size_t)n;
	while (keep && c->in_len) {
		char *end;
		size_t used;

		if (c->in[0] == '$') {
			if (c->in_len < 4)
				break;
			used = 4 + ((size_t)(uint8_t)c->in[2] << 8 | (uint8_t)c->in[3]);
			if (used > c->in_len)
				break;
			for (int t = 0; t < TRACKS; t++)
				if (c->out[t].set && c->out[t].tcp && (uint8_t)c->in[1] == c->out[t].channel + 1)
					rtcp_in(c, t, (const uint8_t *)c->in + 4, used - 4);
		} else {
			c->in[c->in_len] = '\0';
			end = strstr(c->in, "\r\n\r\n");
			if (!end) {
				if (c->in_len >= sizeof(c->in) - 1)
					keep = false;	/* no end in sight */
				break;
			}
			end[2] = '\0';
			used = (size_t)(end + 4 - c->in);
			keep = request(c, c->in);
		}
		memmove(c->in, c->in + used, c->in_len - used);
		c->in_len -= used;
	}
	if (!keep)
		client_close(c);
}

static void accept_client(void)
{
	struct sockaddr_in peer;
	socklen_t len = sizeof(peer);
	int fd = accept4(st.listen, (struct sockaddr *)&peer, &len, SOCK_CLOEXEC), i, one = 1,
	    buf = SNDBUF;

	if (fd < 0)
		return;
	for (i = 0; i < MAX_CLIENTS && st.clients[i].fd >= 0; i++)
		;
	if (i == MAX_CLIENTS) {
		LOGW(TAG, "%s: too many players", inet_ntoa(peer.sin_addr));
		close(fd);
		return;
	}
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
	memset(&st.clients[i], 0, sizeof(st.clients[i]));
	st.clients[i].fd = fd;
	st.clients[i].peer = peer;
	st.clients[i].seen_ms = now_ms();
	LOGI(TAG, "%s connected", inet_ntoa(peer.sin_addr));
}

static void *net_thread(void *ud)
{
	struct pollfd pfd[3 + 2 * TRACKS + MAX_CLIENTS];
	uint64_t last_sr = 0;
	int n, i, t;

	(void)ud;
	for (;;) {
		n = 0;
		pfd[n++] = (struct pollfd){ st.stop, POLLIN, 0 };
		pfd[n++] = (struct pollfd){ st.listen, POLLIN, 0 };
		pfd[n++] = (struct pollfd){ mailbox_fd(st.mb), POLLIN, 0 };
		for (t = 0; t < TRACKS; t++)	/* RTP (nothing comes, dropped), RTCP */
			for (i = 0; i < 2; i++)
				pfd[n++] = (struct pollfd){ st.udp[t][i], POLLIN, 0 };
		for (i = 0; i < MAX_CLIENTS; i++)
			pfd[n++] = (struct pollfd){ st.clients[i].fd, POLLIN, 0 };
		if (poll(pfd, (nfds_t)n, 500) < 0 && errno != EINTR)
			break;
		if (pfd[0].revents)
			break;
		pthread_mutex_lock(&st.lock);
		if (pfd[1].revents)
			accept_client();
		for (i = 0; i < 2 * TRACKS; i++) {
			uint8_t junk[1500];

			if (!pfd[3 + i].revents)
				continue;
			if (i & 1)
				rtcp_udp(i / 2, pfd[3 + i].fd);
			else
				while (recv(pfd[3 + i].fd, junk, sizeof(junk), MSG_DONTWAIT) > 0)
					;
		}
		for (i = 0; i < MAX_CLIENTS; i++) {
			struct client *c = &st.clients[i];

			if (c->fd >= 0 && pfd[3 + 2 * TRACKS + i].revents && pfd[3 + 2 * TRACKS + i].fd == c->fd)
				client_input(c);
			if (c->fd >= 0 && now_ms() - c->seen_ms > SILENT_MS) {
				LOGI(TAG, "%s: not a word for %d s", inet_ntoa(c->peer.sin_addr), SILENT_MS / 1000);
				c->broken = true;
			}
			if (c->fd >= 0 && c->broken)
				client_close(c);
		}
		if (now_ms() - last_sr >= SR_MS) {
			last_sr = now_ms();
			send_reports();
		}
		if (atomic_load(&st.viewers)) {
			rate_maybe_up();
			if (now_ms() - st.stats_ms >= STATS_MS) {
				st.stats_ms = now_ms();
				log_reports();
			}
		}
		pthread_mutex_unlock(&st.lock);
		if (pfd[2].revents)
			encode_and_send();
	}
	return NULL;
}

/* ---- the sound ------------------------------------------------------------ */

static void *audio_thread(void *ud)
{
	int16_t buf[2 * AUDIO_CHUNK];
	uint64_t pass_retry = 0;
	int i;

	(void)ud;
	while (atomic_load(&st.running)) {
		if (pcm_alsa.read(st.cap, buf, AUDIO_CHUNK) < 0) {
			/* nothing plays into the loopback: silence goes on in time */
			memset(buf, 0, sizeof(buf));
			if (!atomic_load(&st.running))
				break;
		}
		pthread_mutex_lock(&st.lock);
		st.audio_at = now_us();
		for (i = 0; i < MAX_CLIENTS; i++) {
			struct client *c = &st.clients[i];
			struct dest d = { c, TRACK_AUDIO, false };

			if (c->fd >= 0 && c->playing && c->out[TRACK_AUDIO].set &&
			    room(c, TRACK_AUDIO, sizeof(buf) + 64))
				rtp_l16(&st.track[TRACK_AUDIO], buf, AUDIO_CHUNK, st.audio_ts, send_to, &d);
		}
		st.audio_ts += AUDIO_CHUNK;
		if (ts_playing()) {
			uint64_t clock = clock90(st.audio_at);
			uint32_t rtp_time = (uint32_t)clock;
			int64_t off = (int64_t)(st.ts_audio_pts - clock);

			/* the sound's own count of time, kept near the clock */
			if (off < -TS_DELAY || off > TS_DELAY)
				st.ts_audio_pts = clock;
			ts_audio(&st.ts, buf, AUDIO_CHUNK, st.ts_audio_pts + TS_DELAY, clock, ts_send, &rtp_time);
			st.ts_audio_pts += (uint64_t)AUDIO_CHUNK * 90000 / AUDIO_RATE;
		}
		pthread_mutex_unlock(&st.lock);
		/* on to the device: opened when it is free (a UI sound has it) */
		if (!st.play && st.cfg.playback && now_ms() >= pass_retry) {
			int rate = AUDIO_RATE;

			st.play = pcm_alsa.open(&st.play_params, &rate);
			if (st.play && rate != AUDIO_RATE) {
				pcm_alsa.close(st.play);
				st.play = NULL;
			}
			pass_retry = now_ms() + 2000;
		}
		if (st.play && pcm_alsa.write(st.play, buf, AUDIO_CHUNK) < 0) {
			pcm_alsa.close(st.play);
			st.play = NULL;
		}
	}
	return NULL;
}

/* ---- the view's side ------------------------------------------------------ */

static int udp_socket(int port)
{
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	int buf = UDP_SNDBUF;

	/* past net.core.wmem_max when it may (root) */
	if (fd >= 0 && setsockopt(fd, SOL_SOCKET, SO_SNDBUFFORCE, &buf, sizeof(buf)) < 0)
		setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
	if (fd >= 0 && bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		LOGW(TAG, "UDP port %d: %s", port, strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

int stream_start(const struct stream_config *cfg)
{
	struct sockaddr_in a = { .sin_family = AF_INET };
	int one = 1, i, t;
	int rate = AUDIO_RATE;

	if (atomic_load(&st.running))
		return 0;
	st.cfg = *cfg;
	if (st.cfg.max_width < cfg->width || st.cfg.max_height < cfg->height) {
		st.cfg.max_width = cfg->width;
		st.cfg.max_height = cfg->height;
	}
	for (i = 0; i < MAX_CLIENTS; i++)
		st.clients[i].fd = -1;
	st.kbps = cfg->kbps;
	atomic_store(&st.kbps_now, cfg->kbps);
	st.rate_cut_ms = st.rate_up_ms = st.idr_ms = st.stats_ms = 0;
	encoder_size(cfg->width, cfg->height);
	/* frames of any size up to the biggest: memory a frame touches only */
	st.mb = mailbox_new(st.cfg.max_width, st.cfg.max_height);
	st.stop = eventfd(0, EFD_CLOEXEC);
	st.listen = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (!st.enc || !st.mb || st.stop < 0 || st.listen < 0)
		goto fail;
	setsockopt(st.listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	a.sin_port = htons((uint16_t)cfg->port);
	if (bind(st.listen, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(st.listen, 4) < 0) {
		LOGE(TAG, "port %d: %s", cfg->port, strerror(errno));
		goto fail;
	}
	for (t = 0; t < TRACKS; t++)
		for (i = 0; i < 2; i++)
			st.udp[t][i] = udp_socket(UDP_BASE + 2 * t + i);
	srand((unsigned)now_us());
	for (t = 0; t < TRACKS; t++)
		st.track[t] = (struct rtp_track){ .ssrc = (uint32_t)rand(), .seq = (uint16_t)rand(),
						  .pt = (uint8_t)(t == TRACK_TS ? 33 : 96 + t) };
	make_sprop();
	st.t0 = now_us() - 1000000;	/* never 0 */
	st.audio_ts = (uint32_t)rand();
	st.audio_at = now_us();
	st.audio_ok = false;
	st.cap_params.device = cfg->capture;
	st.play_params.device = cfg->playback;
	if (cfg->capture) {
		st.cap = pcm_alsa.open(&st.cap_params, &rate);
		st.audio_ok = st.cap && rate == AUDIO_RATE;
		if (st.cap && !st.audio_ok)
			pcm_alsa.close(st.cap);
		if (!st.audio_ok) {
			st.cap = NULL;
			LOGW(TAG, "no sound in the stream: the loopback does not open");
		}
	}
	ts_init(&st.ts, st.audio_ok);
	st.ts_audio_pts = 0;
	atomic_store(&st.running, true);
	st.net_on = pthread_create(&st.net, NULL, net_thread, NULL) == 0;
	if (!st.net_on)
		goto fail;
	if (st.audio_ok)
		st.audio_on = pthread_create(&st.audio, NULL, audio_thread, NULL) == 0;
	LOGI(TAG, "rtsp://<address>:%d/" PATH ", %dx%d, %d fps%s", cfg->port, cfg->width, cfg->height,
	     cfg->fps, st.audio_ok ? ", sound" : "");
	return 0;
fail:
	stream_stop();
	return -1;
}

void stream_stop(void)
{
	int i, t;

	atomic_store(&st.running, false);
	if (st.stop >= 0)
		eventfd_write(st.stop, 1);
	if (st.net_on)
		pthread_join(st.net, NULL);
	if (st.audio_on)
		pthread_join(st.audio, NULL);	/* its read gives up within a second */
	st.net_on = st.audio_on = false;
	for (i = 0; i < MAX_CLIENTS; i++)
		if (st.clients[i].fd > 0)
			client_close(&st.clients[i]);
	for (t = 0; t < TRACKS; t++)
		for (i = 0; i < 2; i++) {
			if (st.udp[t][i] >= 0)
				close(st.udp[t][i]);
			st.udp[t][i] = -1;
		}
	if (st.cap)
		pcm_alsa.close(st.cap);
	if (st.play)
		pcm_alsa.close(st.play);
	st.cap = st.play = NULL;
	if (st.listen >= 0)
		close(st.listen);
	if (st.stop >= 0)
		close(st.stop);
	st.listen = st.stop = -1;
	h264_free(st.enc);
	st.enc = NULL;
	st.enc_width = st.enc_height = 0;
	if (st.mb)
		mailbox_free(st.mb);
	st.mb = NULL;
	atomic_store(&st.viewers, 0);
}

bool stream_running(void)
{
	return atomic_load(&st.running);
}

int stream_viewers(void)
{
	return atomic_load(&st.viewers);
}

int stream_kbps(void)
{
	return atomic_load(&st.kbps_now);
}

struct hal_fb *stream_frame_begin(int width, int height)
{
	struct hal_fb *fb;

	if (!atomic_load(&st.running) || !atomic_load(&st.viewers))
		return NULL;
	fb = mailbox_begin(st.mb);
	fb->width = width < st.cfg.max_width ? width : st.cfg.max_width;
	fb->height = height < st.cfg.max_height ? height : st.cfg.max_height;
	return fb;
}

void stream_frame_end(void)
{
	mailbox_end(st.mb);
}
