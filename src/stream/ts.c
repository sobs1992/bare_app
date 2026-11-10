#include "ts.h"

#include <string.h>

#define PSI_EVERY 22500		/* 90 kHz: a quarter of a second */
#define AUDIO_MAX 1024		/* stereo frames a PES at most */

enum { CC_PAT, CC_PMT, CC_VIDEO, CC_AUDIO };

/* Packets gathered into a group */
struct out {
	uint8_t buf[TS_GROUP * TS_PACKET];
	int n;
	ts_send_fn send;
	void *ud;
};

struct chunk {
	const uint8_t *p;
	size_t n;
};

static void flush(struct out *o)
{
	if (o->n)
		o->send(o->ud, o->buf, (size_t)o->n * TS_PACKET);
	o->n = 0;
}

static uint8_t *next_packet(struct out *o)
{
	if (o->n == TS_GROUP)
		flush(o);
	return o->buf + o->n++ * TS_PACKET;
}

/* CRC-32/MPEG-2 */
static uint32_t crc32(const uint8_t *p, size_t n)
{
	uint32_t crc = 0xffffffff;
	int b;

	while (n--) {
		crc ^= (uint32_t)*p++ << 24;
		for (b = 0; b < 8; b++)
			crc = crc & 0x80000000 ? crc << 1 ^ 0x04c11db7 : crc << 1;
	}
	return crc;
}

/* A section (without its CRC, which goes after it) in one packet */
static void section(struct ts_mux *m, struct out *o, int pid, int cc, uint8_t *sec, size_t len)
{
	uint8_t *p = next_packet(o);
	uint32_t crc = crc32(sec, len);

	sec[len] = (uint8_t)(crc >> 24);
	sec[len + 1] = (uint8_t)(crc >> 16);
	sec[len + 2] = (uint8_t)(crc >> 8);
	sec[len + 3] = (uint8_t)crc;
	p[0] = 0x47;
	p[1] = (uint8_t)(0x40 | pid >> 8);	/* payload unit start */
	p[2] = (uint8_t)pid;
	p[3] = (uint8_t)(0x10 | (m->cc[cc]++ & 15));
	p[4] = 0;		/* pointer field */
	memcpy(p + 5, sec, len + 4);
	memset(p + 5 + len + 4, 0xff, TS_PACKET - 5 - len - 4);
}

static void psi(struct ts_mux *m, struct out *o, uint64_t clock)
{
	uint8_t pat[16] = {
		0x00, 0xb0, 13,		/* table 0, section length */
		0x00, 0x01,		/* transport stream id */
		0xc1, 0x00, 0x00,	/* version 0, current; section 0 of 0 */
		0x00, 0x01, 0xe0 | TS_PID_PMT >> 8, TS_PID_PMT & 0xff,	/* program 1 */
	};
	int pcr = m->audio ? TS_PID_AUDIO : TS_PID_VIDEO;
	uint8_t pmt[48] = {
		0x02, 0xb0, 0,		/* table 2, length below */
		0x00, 0x01,		/* program 1 */
		0xc1, 0x00, 0x00,
		(uint8_t)(0xe0 | pcr >> 8), (uint8_t)pcr,
		0xf0, 6,		/* program info: registration "HDMV" */
		0x05, 4, 'H', 'D', 'M', 'V',
		0x1b, 0xe0 | TS_PID_VIDEO >> 8, TS_PID_VIDEO & 0xff, 0xf0, 0,
	};
	size_t n = 23;

	if (m->audio) {
		const uint8_t es[] = { 0x80, 0xe0 | TS_PID_AUDIO >> 8, TS_PID_AUDIO & 0xff, 0xf0, 0 };

		memcpy(pmt + n, es, sizeof(es));
		n += sizeof(es);
	}
	pmt[2] = (uint8_t)(n - 3 + 4);
	section(m, o, 0, CC_PAT, pat, 12);
	section(m, o, TS_PID_PMT, CC_PMT, pmt, n);
	m->psi_at = clock;
	m->psi_due = false;
}

static void put_pts(uint8_t *p, uint64_t pts)
{
	p[0] = (uint8_t)(0x21 | (pts >> 29 & 0x0e));
	p[1] = (uint8_t)(pts >> 22);
	p[2] = (uint8_t)((pts >> 14 & 0xfe) | 1);
	p[3] = (uint8_t)(pts >> 7);
	p[4] = (uint8_t)((pts << 1 & 0xfe) | 1);
}

/*
 * One PES of the chunks on pid; the first packet carries the PCR (clock)
 * when pcr, the random access flag when key
 */
static void pes(struct ts_mux *m, struct out *o, int pid, int cc, const struct chunk *ch, int nch,
		bool pcr, bool key, uint64_t clock)
{
	size_t left = 0, take, af, base, off = 0;
	bool first = true;
	int c = 0, i;

	for (i = 0; i < nch; i++)
		left += ch[i].n;
	while (left) {
		uint8_t *p = next_packet(o), *d;

		base = first && (pcr || key) ? 2 + (pcr ? 6 : 0) : 0;
		take = left < TS_PACKET - 4 - base ? left : TS_PACKET - 4 - base;
		af = TS_PACKET - 4 - take;	/* the adaptation field, its length byte too */
		p[0] = 0x47;
		p[1] = (uint8_t)((first ? 0x40 : 0) | pid >> 8);
		p[2] = (uint8_t)pid;
		p[3] = (uint8_t)((af ? 0x30 : 0x10) | (m->cc[cc]++ & 15));
		if (af) {
			p[4] = (uint8_t)(af - 1);
			if (af > 1) {
				p[5] = (uint8_t)((base && key ? 0x40 : 0) | (base && pcr ? 0x10 : 0));
				if (base && pcr) {
					uint64_t b = clock & 0x1ffffffffULL;

					p[6] = (uint8_t)(b >> 25);
					p[7] = (uint8_t)(b >> 17);
					p[8] = (uint8_t)(b >> 9);
					p[9] = (uint8_t)(b >> 1);
					p[10] = (uint8_t)((b & 1) << 7 | 0x7e);
					p[11] = 0;
				}
				memset(p + 4 + (base ? base : 2), 0xff, af - (base ? base : 2));
			}
		}
		d = p + 4 + af;
		left -= take;
		while (take) {
			size_t n = ch[c].n - off < take ? ch[c].n - off : take;

			memcpy(d, ch[c].p + off, n);
			d += n;
			take -= n;
			off += n;
			if (off == ch[c].n) {
				c++;
				off = 0;
			}
		}
		first = false;
	}
	flush(o);
}

static bool has_sps(const uint8_t *au, size_t size)
{
	size_t i;

	for (i = 0; i + 3 < size; i++)
		if (!au[i] && !au[i + 1] && au[i + 2] == 1 && (au[i + 3] & 0x1f) == 7)
			return true;
	return false;
}

void ts_init(struct ts_mux *m, bool audio)
{
	memset(m, 0, sizeof(*m));
	m->audio = audio;
	m->psi_due = true;
}

void ts_restart(struct ts_mux *m)
{
	m->psi_due = true;
}

static void psi_if_due(struct ts_mux *m, struct out *o, uint64_t clock, bool key)
{
	if (key || m->psi_due || clock - m->psi_at >= PSI_EVERY)
		psi(m, o, clock);
}

void ts_video(struct ts_mux *m, const uint8_t *au, size_t size, bool idr, const uint8_t *params,
	      size_t params_size, uint64_t pts, uint64_t clock, ts_send_fn send, void *ud)
{
	static const uint8_t aud[] = { 0, 0, 0, 1, 0x09, 0xf0 };	/* access unit delimiter */
	uint8_t head[14] = { 0, 0, 1, 0xe0, 0, 0,	/* video 0, length 0: unbounded */
			     0x84, 0x80, 5 };	/* aligned; PTS */
	struct chunk ch[4] = { { head, sizeof(head) }, { aud, sizeof(aud) } };
	struct out o = { .send = send, .ud = ud };
	int n = 2;

	if (!size)
		return;
	psi_if_due(m, &o, clock, idr);
	put_pts(head + 9, pts);
	if (idr && params && params_size && !has_sps(au, size))
		ch[n++] = (struct chunk){ params, params_size };
	ch[n++] = (struct chunk){ au, size };
	pes(m, &o, TS_PID_VIDEO, CC_VIDEO, ch, n, !m->audio, idr, clock);
}

void ts_audio(struct ts_mux *m, const int16_t *frames, int count, uint64_t pts, uint64_t clock,
	      ts_send_fn send, void *ud)
{
	uint8_t head[18] = { 0, 0, 1, 0xbd, 0, 0,	/* private stream 1 */
			     0x84, 0x80, 5 };
	uint8_t data[4 * AUDIO_MAX];
	struct out o = { .send = send, .ud = ud };
	int n, i;

	if (!m->audio)
		return;
	while (count > 0) {
		struct chunk ch[2] = { { head, sizeof(head) }, { data, 0 } };
		size_t bytes;

		n = count < AUDIO_MAX ? count : AUDIO_MAX;
		bytes = 4 * (size_t)n;
		psi_if_due(m, &o, clock, false);
		head[4] = (uint8_t)((3 + 5 + 4 + bytes) >> 8);	/* PES length after it */
		head[5] = (uint8_t)(3 + 5 + 4 + bytes);
		put_pts(head + 9, pts);
		/* Blu-ray LPCM: the size, stereo (3), 48 kHz (1), 16 bits (1) */
		head[14] = (uint8_t)(bytes >> 8);
		head[15] = (uint8_t)bytes;
		head[16] = 0x31;
		head[17] = 0x40;
		for (i = 0; i < 2 * n; i++) {
			data[2 * i] = (uint8_t)((uint16_t)frames[i] >> 8);
			data[2 * i + 1] = (uint8_t)frames[i];
		}
		ch[1].n = bytes;
		pes(m, &o, TS_PID_AUDIO, CC_AUDIO, ch, 2, true, false, clock);
		frames += 2 * n;
		count -= n;
		pts += (uint64_t)n * 90000 / 48000;
	}
}
