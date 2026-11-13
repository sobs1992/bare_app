#include "rtp.h"

#include <string.h>

static void header(struct rtp_track *t, uint8_t *p, int marker, uint32_t ts)
{
	p[0] = 0x80;		/* version 2 */
	p[1] = (uint8_t)((marker ? 0x80 : 0) | (t->pt & 0x7f));
	p[2] = (uint8_t)(t->seq >> 8);
	p[3] = (uint8_t)t->seq;
	p[4] = (uint8_t)(ts >> 24);
	p[5] = (uint8_t)(ts >> 16);
	p[6] = (uint8_t)(ts >> 8);
	p[7] = (uint8_t)ts;
	p[8] = (uint8_t)(t->ssrc >> 24);
	p[9] = (uint8_t)(t->ssrc >> 16);
	p[10] = (uint8_t)(t->ssrc >> 8);
	p[11] = (uint8_t)t->ssrc;
	t->seq++;
}

static void sent(struct rtp_track *t, size_t payload)
{
	t->packets++;
	t->octets += (uint32_t)payload;
}

/* The next NAL unit from *pos: its start and length, without the start code */
static const uint8_t *next_nal(const uint8_t *au, size_t size, size_t *pos, size_t *len)
{
	size_t i = *pos, start;

	/* past the start code (00 00 01 or 00 00 00 01) */
	while (i + 3 <= size && !(au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1))
		i++;
	if (i + 3 > size) {
		*pos = size;
		return NULL;
	}
	start = i + 3;
	for (i = start; i + 3 <= size; i++)
		if (au[i] == 0 && au[i + 1] == 0 && (au[i + 2] == 1 || (au[i + 2] == 0 && i + 3 < size &&
									   au[i + 3] == 1)))
			break;
	if (i + 3 > size)
		i = size;
	*pos = i;
	*len = i - start;
	/* trailing zeros belong to the next start code */
	while (*len && au[start + *len - 1] == 0)
		(*len)--;
	return *len ? au + start : NULL;
}

void rtp_h264(struct rtp_track *t, const uint8_t *au, size_t size, uint32_t ts, rtp_send_fn send,
	      void *ud)
{
	uint8_t pkt[RTP_HEADER + RTP_MAX_PAYLOAD];
	size_t pos = 0, len, next_pos, next_len;
	const uint8_t *nal = next_nal(au, size, &pos, &len), *next;

	while (nal) {
		next_pos = pos;
		next = next_nal(au, size, &next_pos, &next_len);
		while (!next && next_pos < size)	/* skip empty ones */
			next = next_nal(au, size, &next_pos, &next_len);
		if (len <= RTP_MAX_PAYLOAD) {
			header(t, pkt, !next, ts);
			memcpy(pkt + RTP_HEADER, nal, len);
			send(ud, pkt, RTP_HEADER + len);
			sent(t, len);
		} else {	/* FU-A: the NAL header split over an indicator and a header */
			size_t off = 1, part;
			uint8_t ind = (uint8_t)((nal[0] & 0xe0) | 28), type = nal[0] & 0x1f;

			while (off < len) {
				part = len - off > RTP_MAX_PAYLOAD - 2 ? RTP_MAX_PAYLOAD - 2 : len - off;
				header(t, pkt, !next && off + part == len, ts);
				pkt[RTP_HEADER] = ind;
				pkt[RTP_HEADER + 1] = (uint8_t)(type | (off == 1 ? 0x80 : 0) |
								(off + part == len ? 0x40 : 0));
				memcpy(pkt + RTP_HEADER + 2, nal + off, part);
				send(ud, pkt, RTP_HEADER + 2 + part);
				sent(t, part + 2);
				off += part;
			}
		}
		nal = next;
		len = next_len;
		pos = next_pos;
	}
}

void rtp_l16(struct rtp_track *t, const int16_t *frames, int count, uint32_t ts, rtp_send_fn send,
	     void *ud)
{
	uint8_t pkt[RTP_HEADER + RTP_MAX_PAYLOAD];
	const int per = RTP_MAX_PAYLOAD / 4;	/* stereo frames a packet */
	int n, i;

	while (count > 0) {
		n = count < per ? count : per;
		header(t, pkt, 0, ts);
		for (i = 0; i < 2 * n; i++) {
			pkt[RTP_HEADER + 2 * i] = (uint8_t)((uint16_t)frames[i] >> 8);
			pkt[RTP_HEADER + 2 * i + 1] = (uint8_t)frames[i];
		}
		send(ud, pkt, RTP_HEADER + 4 * (size_t)n);
		sent(t, 4 * (size_t)n);
		frames += 2 * n;
		count -= n;
		ts += (uint32_t)n;
	}
}

size_t rtp_mp2t(struct rtp_track *t, const uint8_t *ts_packets, size_t len, uint32_t ts, uint8_t *pkt)
{
	header(t, pkt, 0, ts);
	memcpy(pkt + RTP_HEADER, ts_packets, len);
	sent(t, len);
	return RTP_HEADER + len;
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* NTP: seconds since 1900, the fraction in 1/2^32 */
static void ntp(uint64_t unix_us, uint64_t *sec, uint32_t *frac)
{
	*sec = unix_us / 1000000 + 2208988800ULL;
	*frac = (uint32_t)(((unix_us % 1000000) << 32) / 1000000);
}

uint32_t rtcp_ntp_mid(uint64_t unix_us)
{
	uint64_t sec;
	uint32_t frac;

	ntp(unix_us, &sec, &frac);
	return (uint32_t)(sec << 16) | frac >> 16;
}

int rtcp_rtt_ms(const struct rtcp_block *b, uint64_t unix_us)
{
	uint32_t rtt;

	if (!b->lsr)
		return -1;
	rtt = rtcp_ntp_mid(unix_us) - b->lsr - b->dlsr;
	if (rtt > 65536u * 10)	/* over 10 s: a clock gone wrong, not a round trip */
		return -1;
	return (int)((uint64_t)rtt * 1000 / 65536);
}

static void blocks(struct rtcp_info *o, const uint8_t *p, int count, const uint8_t *end)
{
	for (; count > 0 && p + 24 <= end; count--, p += 24) {
		struct rtcp_block *b;
		int32_t lost = (int32_t)(get32(p + 4) & 0xffffff);

		if (o->blocks == RTCP_BLOCKS)
			return;
		b = &o->block[o->blocks++];
		b->ssrc = get32(p);
		b->fraction = p[4];
		b->lost = lost & 0x800000 ? lost - 0x1000000 : lost;	/* 24-bit signed */
		b->highest = get32(p + 8);
		b->jitter = get32(p + 12);
		b->lsr = get32(p + 16);
		b->dlsr = get32(p + 20);
	}
}

bool rtcp_parse(const uint8_t *buf, size_t len, struct rtcp_info *out)
{
	const uint8_t *p = buf, *end = buf + len;

	memset(out, 0, sizeof(*out));
	if (len < 4 || (buf[0] >> 6) != 2 || buf[1] < 192 || buf[1] > 223)
		return false;	/* RTP, or not ours at all */
	while (p + 4 <= end) {
		size_t plen = 4 + 4 * ((size_t)p[2] << 8 | p[3]);
		const uint8_t *next = p + plen;
		int count = p[0] & 0x1f;

		if ((p[0] >> 6) != 2 || next > end)
			return false;
		switch (p[1]) {
		case 200:	/* SR: the sender's info, then its report blocks */
			blocks(out, p + 28, count, next);
			break;
		case 201:	/* RR */
			blocks(out, p + 8, count, next);
			break;
		case 203:	/* BYE */
			out->bye = true;
			break;
		case 206:	/* payload-specific feedback: 1 PLI, 4 FIR */
			if (count == 1 || count == 4)
				out->key_frame = true;
			break;
		default:	/* SDES, APP, NACKs and the rest */
			break;
		}
		p = next;
	}
	return true;
}

size_t rtcp_sr(const struct rtp_track *t, uint64_t unix_us, uint32_t ts, const char *cname,
	       uint8_t *buf, size_t size)
{
	uint64_t sec;
	uint32_t frac;
	size_t clen = strlen(cname), sdes = 4 + 4 + 2 + clen, pad;

	ntp(unix_us, &sec, &frac);
	if (clen > 255)
		return 0;
	pad = (4 - (sdes + 1) % 4) % 4 + 1;	/* the end item and up to a word */
	if (size < 28 + sdes + pad)
		return 0;
	/* SR: V=2, no reports of our own, PT 200, 6 words after the first */
	buf[0] = 0x80;
	buf[1] = 200;
	buf[2] = 0;
	buf[3] = 6;
	put32(buf + 4, t->ssrc);
	put32(buf + 8, (uint32_t)sec);
	put32(buf + 12, frac);
	put32(buf + 16, ts);
	put32(buf + 20, t->packets);
	put32(buf + 24, t->octets);
	/* SDES with one chunk: SSRC, CNAME, the end */
	buf[28] = 0x81;
	buf[29] = 202;
	buf[30] = 0;
	buf[31] = (uint8_t)((sdes + pad) / 4 - 1);
	put32(buf + 32, t->ssrc);
	buf[36] = 1;		/* CNAME */
	buf[37] = (uint8_t)clen;
	memcpy(buf + 38, cname, clen);
	memset(buf + 38 + clen, 0, pad);
	return 28 + sdes + pad;
}
