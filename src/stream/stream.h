/*
 * The screen and the sound over the network: an RTSP server (RFC 2326)
 * at rtsp://<address>:<port>/live that any player opens (VLC, ffplay,
 * mpv). Video is H.264 (h264.c), sound L16 stereo at 48 kHz, both over
 * RTP (rtp.c) to UDP ports the player names or interleaved in its RTSP
 * connection (TCP), with RTCP sender reports for each, so players keep
 * them in sync.
 *
 * The players' RTCP comes back: their receiver reports bring the bit rate
 * down while packets get lost (UDP, Wi-Fi) or a TCP player falls behind,
 * and up again when they stop; BYE and a player that says nothing for two
 * session timeouts end its session, PLI/FIR ask for a key frame.
 *
 * The view thread owns it: it starts and stops it, and while a player
 * plays it composes the screen into stream_frame_begin() at the frame
 * rate: an XRGB frame of the display's size, or bigger, the picture a
 * game drew before it was taken down to the screen (up to max_width x
 * max_height). The encoder starts again at each new size, the players get
 * its parameters with the key frame that follows. Encoding and the network
 * are the stream's own thread, the sound another one: it reads what
 * games play into the ALSA loopback (games get its playback side while
 * the stream runs: stream_loopback()) and passes it on to the device.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "hal/hal.h"

struct stream_config {
	int width, height;	/* of the frames, at first */
	int max_width, max_height;	/* the biggest frame (0: width, height) */
	int fps;
	int kbps;		/* video bit rate */
	int port;		/* RTSP, TCP */
	/* the sound (ALSA PCM nodes): the loopback's capture side, NULL no
	 * sound; the device it goes on to, NULL none */
	const char *capture, *playback;
};

int stream_start(const struct stream_config *c);
void stream_stop(void);
bool stream_running(void);
int stream_viewers(void);	/* players playing; any thread */
int stream_kbps(void);		/* the bit rate aimed at now; any thread */

/* View thread: a frame of width x height (up to the maximum) to draw into,
 * NULL if nobody plays */
struct hal_fb *stream_frame_begin(int width, int height);
void stream_frame_end(void);

/*
 * The ALSA loopback (snd-aloop, loaded if it is not): the PCM node games
 * play into (buf, "/dev/snd/pcmC1D0p") and the one the stream captures.
 * -1 if there is none.
 */
int stream_loopback(char *play, char *capture, size_t size);
