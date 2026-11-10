/*
 * The stream as an MPEG transport stream (ISO/IEC 13818-1), for players that
 * take RTP only as MP2T (RFC 2250): VLC built without live555, as Debian and
 * Ubuntu ship it, opens rtsp:// with its SAT>IP module, which SETUPs the
 * aggregate URL without a DESCRIBE and reads the RTP payload as a TS.
 *
 * One program: H.264 (stream type 0x1B) and the sound as Blu-ray LPCM
 * (0x80 in a program registered "HDMV", as VLC and FFmpeg read it), 16-bit
 * stereo at 48 kHz, big-endian. PAT and PMT go before every key frame and
 * at least every quarter of a second; the PCR is on the sound when there is
 * sound (a PES every 10 ms), else on the video.
 *
 * No network here: packets go to a callback, up to TS_GROUP of them at a
 * time (one RTP packet), a PES never shares a group with another.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TS_PACKET 188
#define TS_GROUP 7		/* packets an RTP packet carries: 1316 bytes */
#define TS_PID_PMT 0x1000
#define TS_PID_VIDEO 0x100
#define TS_PID_AUDIO 0x101

struct ts_mux {
	bool audio;		/* the program has the sound */
	bool psi_due;		/* PAT and PMT before the next PES */
	uint64_t psi_at;	/* 90 kHz time they last went */
	uint8_t cc[4];		/* continuity counters: PAT, PMT, video, audio */
};

typedef void (*ts_send_fn)(void *ud, const uint8_t *packets, size_t len);

void ts_init(struct ts_mux *m, bool audio);
/* A player came: PAT and PMT before anything else */
void ts_restart(struct ts_mux *m);
/*
 * An access unit (Annex B). On a key frame params (SPS and PPS, Annex B) go
 * before it unless it carries them itself. pts and clock (the PCR, now)
 * in 90 kHz.
 */
void ts_video(struct ts_mux *m, const uint8_t *au, size_t size, bool idr, const uint8_t *params,
	      size_t params_size, uint64_t pts, uint64_t clock, ts_send_fn send, void *ud);
/* count stereo frames (host order) */
void ts_audio(struct ts_mux *m, const int16_t *frames, int count, uint64_t pts, uint64_t clock,
	      ts_send_fn send, void *ud);
