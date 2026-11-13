/*
 * RTP packets of the stream and the RTCP reports about them, without the
 * network: each packet goes to a callback.
 *
 * - H.264 (RFC 6184, packetization-mode 1): an access unit in Annex B is
 *   split at its start codes; a NAL unit that fits a packet goes as it is,
 *   a longer one as FU-A fragments. The marker bit is on the last packet
 *   of the access unit.
 * - L16 (RFC 3551): 16-bit stereo samples, big-endian.
 * - MP2T (RFC 2250): transport stream packets, one packet built for all.
 * - RTCP: a sender report (RFC 3550) with the NTP time and the RTP time of
 *   the same moment and the counts sent, and an SDES CNAME; and what the
 *   players send back (rtcp_parse()): their receiver reports (losses,
 *   jitter, the round trip), BYE, and the requests for a key frame of
 *   RFC 4585/5104 (PLI, FIR).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RTP_MAX_PAYLOAD 1400	/* fits a Wi-Fi frame with the IP and UDP headers */
#define RTP_HEADER 12

struct rtp_track {
	uint32_t ssrc;
	uint16_t seq;
	uint8_t pt;		/* payload type */
	uint32_t packets, octets;	/* sent, for the sender report */
};

typedef void (*rtp_send_fn)(void *ud, const uint8_t *packet, size_t len);

/* One access unit at RTP time ts (90 kHz) */
void rtp_h264(struct rtp_track *t, const uint8_t *au, size_t size, uint32_t ts, rtp_send_fn send,
	      void *ud);
/* count stereo frames at RTP time ts (the sample rate); as many packets as it takes */
void rtp_l16(struct rtp_track *t, const int16_t *frames, int count, uint32_t ts, rtp_send_fn send,
	     void *ud);
/* One packet of len bytes of transport stream at RTP time ts (90 kHz) into
 * pkt (RTP_HEADER + len); its length */
size_t rtp_mp2t(struct rtp_track *t, const uint8_t *ts_packets, size_t len, uint32_t ts, uint8_t *pkt);
/* The sender report of t for wall time unix_us (gettimeofday) = RTP time ts; its length, 0 if size is short */
size_t rtcp_sr(const struct rtp_track *t, uint64_t unix_us, uint32_t ts, const char *cname,
	       uint8_t *buf, size_t size);

/* A report block of a receiver (RFC 3550 6.4.1) about one of our sources */
struct rtcp_block {
	uint32_t ssrc;		/* the source it is about */
	uint8_t fraction;	/* lost since the last report, of 256 */
	int32_t lost;		/* cumulative */
	uint32_t highest;	/* extended highest sequence number received */
	uint32_t jitter;	/* in RTP time units */
	uint32_t lsr, dlsr;	/* our last SR (middle of its NTP time), the delay since, 1/65536 s */
};

#define RTCP_BLOCKS 8

struct rtcp_info {
	int blocks;
	struct rtcp_block block[RTCP_BLOCKS];
	bool bye;		/* the player leaves */
	bool key_frame;		/* PLI or FIR: it wants a key frame */
};

/* A compound RTCP packet from a player: false if it is not one */
bool rtcp_parse(const uint8_t *buf, size_t len, struct rtcp_info *out);
/* The middle 32 bits of the NTP time of unix_us, as an SR's LSR is */
uint32_t rtcp_ntp_mid(uint64_t unix_us);
/* The round trip of a report block received at unix_us, ms; -1 when unknown */
int rtcp_rtt_ms(const struct rtcp_block *b, uint64_t unix_us);
