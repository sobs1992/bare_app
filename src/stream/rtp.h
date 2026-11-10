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
 *   the same moment and the counts sent, and an SDES CNAME.
 */
#pragma once

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
