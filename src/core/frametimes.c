#include "frametimes.h"

#include <stdlib.h>
#include <string.h>

void frametimes_reset(struct frametimes *f)
{
	f->count = 0;
}

void frametimes_add(struct frametimes *f, uint64_t us)
{
	f->us[f->count++ % FRAMETIMES_MAX] = us;
}

unsigned frametimes_last(const struct frametimes *f, uint64_t *out, unsigned max)
{
	unsigned n = f->count < FRAMETIMES_MAX ? f->count : FRAMETIMES_MAX, i;

	if (n > max)
		n = max;
	for (i = 0; i < n; i++)
		out[i] = f->us[(f->count - n + i) % FRAMETIMES_MAX];
	return n;
}

static int cmp_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

	return x < y ? -1 : x > y;
}

void frametimes_summarize(const uint64_t *t, unsigned n, uint64_t from_us,
			  struct frametimes_summary *s)
{
	static uint32_t d[FRAMETIMES_MAX];	/* view thread only */
	unsigned first = 0, k, i;

	memset(s, 0, sizeof(*s));
	s->fps_x10 = -1;
	while (first < n && t[first] < from_us)
		first++;
	if (first)
		first--;	/* the interval into the stretch counts */
	for (i = first + 1, k = 0; i < n && k < FRAMETIMES_MAX; i++)
		d[k++] = t[i] > t[i - 1] ? (uint32_t)(t[i] - t[i - 1]) : 0;
	if (!k)
		return;
	s->frames = k;
	s->span_us = t[n - 1] - t[first];
	for (i = 0; i < k; i++)
		s->worst_us = d[i] > s->worst_us ? d[i] : s->worst_us;
	qsort(d, k, sizeof(d[0]), cmp_u32);
	s->median_us = d[k / 2];
	s->p99_us = d[k - 1 - k / 100];
	for (i = 0; i < k; i++)
		s->hitches += frametimes_is_hitch(d[i], s->median_us);
	if (s->span_us)
		s->fps_x10 = (int)((k * 10000000ULL + s->span_us / 2) / s->span_us);
}
