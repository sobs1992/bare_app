/*
 * Times frames were shown, for the frame time graph of the system monitor
 * and the stutter numbers in the log: a ring of the last FRAMETIMES_MAX
 * times (now_us()) and what they say over a stretch of time.
 *
 * A hitch is a frame that came much later than the game's usual pace:
 * at least twice the median frame time of the stretch and over
 * FRAMETIMES_HITCH_MIN_US.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define FRAMETIMES_MAX 1024		/* > 10 s at 60 fps, a power of two */
#define FRAMETIMES_HITCH_MIN_US 25000

struct frametimes {
	uint64_t us[FRAMETIMES_MAX];
	unsigned count;			/* added since the reset, the ring holds the last ones */
};

struct frametimes_summary {
	unsigned frames;		/* intervals in the stretch */
	uint64_t span_us;		/* from the first frame of it to the last */
	uint32_t median_us, p99_us, worst_us;
	unsigned hitches;
	int fps_x10;			/* frames a second * 10, -1 unknown */
};

void frametimes_reset(struct frametimes *f);
void frametimes_add(struct frametimes *f, uint64_t us);

/* Up to max of the latest times, oldest first; returns how many */
unsigned frametimes_last(const struct frametimes *f, uint64_t *out, unsigned max);

/* What the frames shown at from_us or later say (times as from frametimes_last()) */
void frametimes_summarize(const uint64_t *t, unsigned n, uint64_t from_us,
			  struct frametimes_summary *s);

/* Is an interval of us a hitch for a game with this median? */
static inline bool frametimes_is_hitch(uint32_t us, uint32_t median_us)
{
	return us >= FRAMETIMES_HITCH_MIN_US && us >= 2 * median_us;
}
