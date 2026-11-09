/*
 * System monitor (CTX_SYSMON) over everything on the top layer, also over
 * a game: a dark panel with a row per CPU cluster (a bar per core), the
 * GPU, memory, temperatures, the battery, each disk (a bar for the space
 * used on its partitions, reads and writes, free space under it),
 * bare_app with its game and, while a game is on the screen, its frames
 * a second with a graph of its frame times. F5 held shows it, held again
 * over a game leaves only a small panel with the frame times, held once
 * more hides it (view core); the game keeps running under it. The rows are
 * built when it is shown and again when what there is changes (a game
 * starts, a disk comes). Always in English, whatever the UI language.
 *
 * Frame times are the view's own: when each frame of the game was shown
 * in the video layer, or made unchanged (view_video_frame_times()). The
 * graph is a line over the last GRAPH_SPAN_US, a point per pixel column:
 * a frame holds the columns from the frame before it to itself at the
 * height of the time it took, so a stall is a wide plateau. The scale
 * grows from 50 ms to 100 or 200 ms when frames take that long; longer
 * ones stay at the top. Hitches (core/frametimes.h) are in the warning
 * colour, the dashed lines are 60 and 30 frames a second. A game that
 * does not say when its frame did not change (view_video_paced()) sends
 * nothing over a still picture: a gap over FRAMETIMES_IDLE_US is a pause
 * for it, a dim stretch at the bottom, out of the numbers, and "still"
 * for the rate.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../widget.h"
#include "core/frametimes.h"
#include "core/util.h"

#define SYSMON_FONT ui_font(UI_TEXT_COMPACT)	/* dense: the same on every screen */
#define PANEL_MAX_W 560
#define MARGIN 8
#define TITLE_W 76
#define BAR_W 110
#define BAR_H 8
#define BAR_GAP 2
#define COL_GAP 8
#define HOT_LOAD 90		/* percent: a core this busy is drawn in the warning colour */
#define HOT_MDEG 80000		/* the same for temperatures */
#define HOT_DISK 90		/* percent of a disk's space used */
#define MAX_ROWS (SYSMON_MAX_CLUSTERS + 5 + SYSMON_MAX_DISKS + SYSMON_MAX_PROCS)
#define GRAPH_H 56
#define GRAPH_SPAN_US 10000000ULL	/* the width of a graph */
#define GRAPH_TOPS_US { 50000, 100000, 200000 }	/* the scales; longer frames are cut */
#define GRAPH_MAX_W 1024
#define TICK_MS 250			/* the graphs move on */
#define MINI_W 340

#ifdef CONFIG_UI_FONT_CYRILLIC
#define TIMES "\xc3\x97"	/* × */
#define DEGREE "\xc2\xb0"	/* ° */
#else
#define TIMES "x"		/* Montserrat has neither */
#define DEGREE ""
#endif

struct row {
	lv_obj_t *title, *bars, *value;
	lv_obj_t *bar[SYSMON_MAX_CPUS];
	int nbars;
};

/* What the rows were built for */
struct layout {
	int nclusters, ncpus[SYSMON_MAX_CLUSTERS];
	bool fps, gpu, temps, battery;
	int ndisks, nprocs;
	bool mounts[SYSMON_MAX_DISKS];
};

struct sysmon_view;
static lv_obj_t *graph_new(struct sysmon_view *m, lv_obj_t *parent, int32_t width);

struct sysmon_view {
	lv_obj_t *panel, *head_title, *head_up, *body;
	bool built;
	struct layout layout;
	struct row fps, clusters[SYSMON_MAX_CLUSTERS], gpu, ram, temps, battery;
	struct row disks[SYSMON_MAX_DISKS], procs[SYSMON_MAX_PROCS];
	lv_obj_t *mounts[SYSMON_MAX_DISKS];
	lv_obj_t *graph;	/* under the FPS row, NULL without it */
	lv_obj_t *mini, *mini_text, *mini_graph;	/* SYSMON_FRAMES */
	bool frames_only;	/* the mode shown */
	unsigned seq;		/* the sample shown */
	lv_timer_t *tick;	/* while a graph is there */
	/* frame times at the last tick */
	uint64_t t[FRAMETIMES_MAX], now;
	unsigned nt;
	struct frametimes_summary last_s, span;	/* over the last second, the graph */
	bool paced;		/* view_video_paced() */
	bool still;		/* no frame for a while from a game that is not paced */
};

/* ---- formatting ------------------------------------------------------- */

/* value / 10^digits */
static void fmt_fixed(char *buf, size_t size, long long value, int digits)
{
	long long scale = digits == 2 ? 100 : digits == 1 ? 10 : 1;
	long long a = value < 0 ? -value : value;

	if (!digits) {
		snprintf(buf, size, "%lld", value);
		return;
	}
	snprintf(buf, size, "%s%lld%s%0*lld", value < 0 ? "-" : "", a / scale, ".",
		 digits, a % scale);
}

/* Megabytes as "906 MB" or "8,0 GB" */
static void fmt_mb(char *buf, size_t size, uint32_t mb)	/* size: 32 is enough */
{
	char num[16];

	if (mb < 10 * 1024) {
		if (mb < 1024) {
			snprintf(buf, size, "%u %s", mb, "MB");
			return;
		}
		fmt_fixed(num, sizeof(num), (long long)mb * 10 / 1024, 1);
	} else {
		snprintf(num, sizeof(num), "%u", (mb + 512) / 1024);
	}
	snprintf(buf, size, "%s %s", num, "GB");
}

/* kilobytes a second as megabytes: "0,4", "12" */
static void fmt_rate(char *buf, size_t size, uint32_t kbs)
{
	if (kbs >= 10000)
		snprintf(buf, size, "%u", (kbs + 500) / 1000);
	else
		fmt_fixed(buf, size, (kbs + 50) / 100, 1);
}

/* lv_label_set_text() redraws the label even when the text is the same */
static void set_text(lv_obj_t *label, const char *text)
{
	if (strcmp(lv_label_get_text(label), text))
		lv_label_set_text(label, text);
}

static void set_color(lv_obj_t *obj, bool hot)
{
	lv_color_t want = hot ? UI_COLOR_WARN : lv_color_white();

	if (!lv_color_eq(lv_obj_get_style_text_color(obj, LV_PART_MAIN), want))
		lv_obj_set_style_text_color(obj, want, 0);
}

static void set_bar(lv_obj_t *bar, int value, bool hot)
{
	lv_color_t want = hot ? UI_COLOR_WARN : UI_COLOR_ON;

	value = value < 0 ? 0 : value > 100 ? 100 : value;
	if (lv_bar_get_value(bar) != value)
		lv_bar_set_value(bar, value, LV_ANIM_OFF);
	if (!lv_color_eq(lv_obj_get_style_bg_color(bar, LV_PART_INDICATOR), want))
		lv_obj_set_style_bg_color(bar, want, LV_PART_INDICATOR);
}

/* ---- rows ------------------------------------------------------------- */

static lv_obj_t *label_new(lv_obj_t *parent, int32_t width)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_label_set_text(l, "");
	if (width) {		/* one line of it, cut with dots */
		lv_obj_set_size(l, width, lv_font_get_line_height(SYSMON_FONT));
		lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
	}
	return l;
}

/* Title, nbars bars side by side in the bar column, value: one line, or
 * as many as it takes */
static void row_new(struct sysmon_view *m, struct row *r, const char *title, int nbars, bool wrap)
{
	lv_obj_t *obj = lv_obj_create(m->body);
	int i, w;

	memset(r, 0, sizeof(*r));
	lv_obj_remove_style_all(obj);
	lv_obj_set_size(obj, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(obj, COL_GAP, 0);
	r->title = label_new(obj, TITLE_W);
	lv_label_set_text(r->title, title);
	lv_obj_set_style_text_color(r->title, UI_COLOR_DIM, 0);
	r->bars = lv_obj_create(obj);
	lv_obj_remove_style_all(r->bars);
	lv_obj_set_size(r->bars, BAR_W, BAR_H);
	r->nbars = nbars;
	w = nbars ? (BAR_W - BAR_GAP * (nbars - 1)) / nbars : 0;
	for (i = 0; i < nbars; i++) {
		lv_obj_t *b = lv_bar_create(r->bars);

		lv_obj_set_size(b, i == nbars - 1 ? BAR_W - i * (w + BAR_GAP) : w, BAR_H);
		lv_obj_set_pos(b, i * (w + BAR_GAP), 0);
		lv_obj_set_style_radius(b, 2, 0);
		lv_obj_set_style_radius(b, 2, LV_PART_INDICATOR);
		lv_obj_set_style_bg_color(b, lv_color_hex(0x3a3f48), 0);
		lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
		lv_bar_set_range(b, 0, 100);
		lv_bar_set_value(b, 0, LV_ANIM_OFF);
		r->bar[i] = b;
	}
	r->value = label_new(obj, 0);
	lv_obj_set_flex_grow(r->value, 1);
	lv_label_set_long_mode(r->value, wrap ? LV_LABEL_LONG_MODE_WRAP : LV_LABEL_LONG_MODE_DOTS);
	if (!wrap)
		lv_obj_set_height(r->value, lv_font_get_line_height(SYSMON_FONT));
}

static void cluster_title(char *buf, size_t size, const struct sysmon_info *in, int i)
{
	const struct sysmon_cluster *c = &in->clusters[i];

	snprintf(buf, size, "%s " TIMES "%d", c->core[0] ? c->core : "CPU", c->count);
}

static const char *disk_title(const struct sysmon_disk *d)
{
	switch (d->kind) {
	case SYSMON_DISK_EMMC: return "eMMC";
	case SYSMON_DISK_SD:   return "SD";
	case SYSMON_DISK_USB:  return "USB";
	case SYSMON_DISK_NVME: return "NVMe";
	default:               return d->name;
	}
}

static void layout_of(struct layout *l, const struct sysmon_info *in)
{
	int i;

	memset(l, 0, sizeof(*l));
	l->fps = view_video_shown() != VIDEO_NONE;
	l->nclusters = in->nclusters;
	for (i = 0; i < in->nclusters; i++)
		l->ncpus[i] = in->clusters[i].count;
	l->gpu = in->gpu_max_mhz > 0;
	l->temps = in->ntemps > 0;
	l->battery = in->battery;
	l->ndisks = in->ndisks;
	for (i = 0; i < in->ndisks; i++)
		l->mounts[i] = in->disks[i].nmounts > 0;
	l->nprocs = in->nprocs;
}

static void build(struct sysmon_view *m, const struct sysmon_info *in)
{
	char title[32];
	int i, bars;

	lv_obj_clean(m->body);
	memset(m->mounts, 0, sizeof(m->mounts));
	layout_of(&m->layout, in);
	lv_label_set_text(m->head_title, "System monitor");
	m->graph = NULL;
	if (m->layout.fps) {
		row_new(m, &m->fps, "FPS", 0, false);
		m->graph = graph_new(m, m->body, LV_PCT(100));
	}
	for (i = 0; i < in->nclusters; i++) {
		const struct sysmon_cluster *c = &in->clusters[i];

		bars = c->first + c->count <= SYSMON_MAX_CPUS ? c->count : 1;
		cluster_title(title, sizeof(title), in, i);
		row_new(m, &m->clusters[i], title, bars, false);
	}
	if (m->layout.gpu)
		row_new(m, &m->gpu, "GPU", 1, false);
	row_new(m, &m->ram, "RAM", 1, false);
	if (m->layout.temps)
		row_new(m, &m->temps, "Temp.", 0, true);
	if (m->layout.battery)
		row_new(m, &m->battery, "Battery", 1, false);
	for (i = 0; i < in->ndisks; i++) {
		const char *t = disk_title(&in->disks[i]);

		row_new(m, &m->disks[i], in->disks[i].kind == SYSMON_DISK_OTHER && !t[0]
					 ? "Disk" : t, 1, false);
		if (m->layout.mounts[i]) {
			m->mounts[i] = label_new(m->body, 0);
			lv_obj_set_width(m->mounts[i], LV_PCT(100));
			lv_label_set_long_mode(m->mounts[i], LV_LABEL_LONG_MODE_WRAP);
			lv_obj_set_style_pad_left(m->mounts[i], TITLE_W + COL_GAP, 0);
			lv_obj_set_style_text_color(m->mounts[i], UI_COLOR_DIM, 0);
		}
	}
	for (i = 0; i < in->nprocs; i++)
		row_new(m, &m->procs[i], in->procs[i].name, 1, false);
	m->built = true;
}

/* ---- values ----------------------------------------------------------- */

static void show_clusters(struct sysmon_view *m, const struct sysmon_info *in)
{
	char buf[64];
	int i, k;

	for (i = 0; i < in->nclusters; i++) {
		const struct sysmon_cluster *c = &in->clusters[i];
		struct row *r = &m->clusters[i];
		int sum = 0, n = 0, max = 0, len;

		for (k = 0; k < c->count && c->first + k < SYSMON_MAX_CPUS; k++) {
			int load = in->cpu[c->first + k];

			if (load < 0)
				continue;
			sum += load;
			n++;
			max = load > max ? load : max;
			if (r->nbars == c->count)
				set_bar(r->bar[k], load, load >= HOT_LOAD);
		}
		if (r->nbars == 1 && r->nbars != c->count)	/* more CPUs than bars */
			set_bar(r->bar[0], in->cpu_total, false);
		len = n ? snprintf(buf, sizeof(buf), "%d%%", (sum + n / 2) / n)
			: snprintf(buf, sizeof(buf), "...");
		if (c->mhz > 0)
			snprintf(buf + len, sizeof(buf) - len, "  %d %s", c->mhz, "MHz");
		set_text(r->value, buf);
		set_color(r->value, max >= HOT_LOAD);
	}
}

static void show_memory(struct sysmon_view *m, const struct sysmon_info *in)
{
	char buf[96];
	int len;

	if (m->layout.gpu) {
		snprintf(buf, sizeof(buf), "%d / %d %s", in->gpu_mhz, in->gpu_max_mhz, "MHz");
		set_text(m->gpu.value, buf);
		set_bar(m->gpu.bar[0], in->gpu_mhz * 100 / in->gpu_max_mhz, false);
	}
	len = snprintf(buf, sizeof(buf), "%u / %u %s", in->mem_used_mb, in->mem_total_mb,
		       "MB");
	if (in->swap_total_mb)
		snprintf(buf + len, sizeof(buf) - len, "  %s %u / %u", "swap",
			 in->swap_used_mb, in->swap_total_mb);
	set_text(m->ram.value, buf);
	set_bar(m->ram.bar[0], in->mem_total_mb ? (int)(in->mem_used_mb * 100ULL / in->mem_total_mb) : 0,
		in->mem_total_mb && in->mem_used_mb * 10ULL >= in->mem_total_mb * 9ULL);
}

static const char *temp_name(const struct sysmon_temp *t)
{
	switch (t->kind) {
	case SYSMON_TEMP_CPU: return "CPU";
	case SYSMON_TEMP_SOC: return "SoC";
	case SYSMON_TEMP_GPU: return "GPU";
	case SYSMON_TEMP_DDR: return "DDR";
	default:              return t->name;
	}
}

static void show_temps(struct sysmon_view *m, const struct sysmon_info *in)
{
	char buf[160], num[16];
	int i, len = 0, hot = 0;

	if (!m->layout.temps)
		return;
	buf[0] = '\0';
	for (i = 0; i < in->ntemps && len < (int)sizeof(buf); i++) {
		fmt_fixed(num, sizeof(num), (in->temps[i].mdeg + 50) / 100, 1);
		len += snprintf(buf + len, sizeof(buf) - len, "%s%s %s" DEGREE, i ? "  " : "",
				temp_name(&in->temps[i]), num);
		hot |= in->temps[i].mdeg >= HOT_MDEG;
	}
	set_text(m->temps.value, buf);
	set_color(m->temps.value, hot);
}

static void show_battery(struct sysmon_view *m, const struct sysmon_info *in)
{
	char buf[96], volts[16], watts[16];
	int mw = in->bat_mw < 0 ? -in->bat_mw : in->bat_mw;

	if (!m->layout.battery)
		return;
	fmt_fixed(volts, sizeof(volts), (in->bat_mv + 5) / 10, 2);
	fmt_fixed(watts, sizeof(watts), (mw + 5) / 10, 2);
	if (in->bat_capacity >= 0)
		snprintf(buf, sizeof(buf), "%d%%  %s %s  %+d %s  %s %s", in->bat_capacity, volts,
			 "V", in->bat_ma, "mA", watts, "W");
	else
		snprintf(buf, sizeof(buf), "%s %s  %+d %s  %s %s", volts, "V", in->bat_ma,
			 "mA", watts, "W");
	set_text(m->battery.value, buf);
	set_bar(m->battery.bar[0], in->bat_capacity, in->bat_capacity >= 0 && in->bat_capacity < 15);
}

static void show_disks(struct sysmon_view *m, const struct sysmon_info *in)
{
	char buf[192], rd[16], wr[16], io[64], size[32];
	int i, k;

	for (i = 0; i < in->ndisks; i++) {
		const struct sysmon_disk *d = &in->disks[i];
		/* used of what users can have, as df: the reserved blocks do not count */
		uint64_t room = (uint64_t)d->used_mb + d->free_mb;
		int used = room ? (int)((d->used_mb * 100ULL + room - 1) / room) : -1;
		int len = used >= 0 ? snprintf(buf, sizeof(buf), "%d%%  ", used) : 0;

		if (d->busy < 0) {
			snprintf(buf + len, sizeof(buf) - len, "...");
		} else {
			fmt_rate(rd, sizeof(rd), d->read_kbs);
			fmt_rate(wr, sizeof(wr), d->write_kbs);
			snprintf(io, sizeof(io), "R %s  W %s MB/s", rd, wr);
			snprintf(buf + len, sizeof(buf) - len, "%s", io);
		}
		set_text(m->disks[i].value, buf);
		set_bar(m->disks[i].bar[0], used, used >= HOT_DISK);
		if (!m->mounts[i])
			continue;
		len = snprintf(buf, sizeof(buf), "%s:", "free");
		for (k = 0; k < d->nmounts && len < (int)sizeof(buf); k++) {
			fmt_mb(size, sizeof(size), d->mounts[k].free_mb);
			len += snprintf(buf + len, sizeof(buf) - len, "%s %s %s", k ? "," : "",
					d->mounts[k].path, size);
		}
		set_text(m->mounts[i], buf);
	}
}

/* ---- frame times ----------------------------------------------------- */

#define COL_NONE (-1)		/* no frame there yet */
#define COL_IDLE (-2)		/* a pause of a game with a still picture */

/* The scale for frames up to worst_us */
static uint32_t graph_top(uint32_t worst_us)
{
	static const uint32_t tops[] = GRAPH_TOPS_US;
	unsigned i;

	for (i = 0; i + 1 < sizeof(tops) / sizeof(tops[0]); i++)
		if (worst_us <= tops[i] - tops[i] / 10)
			break;
	return tops[i];
}

/* y of a frame time in the content area c, cut at the top */
static int32_t graph_y(const lv_area_t *c, int32_t us, uint32_t top)
{
	int32_t h = lv_area_get_height(c) - 2;	/* the line is 2 px: inside */

	return c->y2 - 1 - (int32_t)((int64_t)LV_MIN((uint32_t)us, top) * h / top);
}

static void graph_line(lv_layer_t *layer, lv_point_precise_t *p, int n, lv_color_t color)
{
	lv_draw_line_dsc_t dsc;

	if (n < 2)
		return;
	lv_draw_line_dsc_init(&dsc);
	dsc.points = p;
	dsc.point_cnt = n;
	dsc.width = 2;
	dsc.color = color;
	dsc.opa = LV_OPA_COVER;
	lv_draw_line(layer, &dsc);
}

/* A dim straight line: the 60 and 30 fps marks (dashed), a pause */
static void graph_flat(lv_layer_t *layer, int32_t x1, int32_t x2, int32_t y, bool dashed)
{
	lv_draw_line_dsc_t dsc;

	lv_draw_line_dsc_init(&dsc);
	dsc.p1.x = x1;
	dsc.p2.x = x2;
	dsc.p1.y = dsc.p2.y = y;
	if (dashed) {
		dsc.color = lv_color_white();
		dsc.opa = LV_OPA_30;
		dsc.width = 1;
		dsc.dash_width = dsc.dash_gap = 4;
	} else {
		dsc.color = UI_COLOR_DIM;
		dsc.opa = LV_OPA_60;
		dsc.width = 2;
	}
	lv_draw_line(layer, &dsc);
}

static void graph_draw(lv_event_t *e)
{
	static int32_t col[GRAPH_MAX_W];	/* frame time of each column (us), COL_* */
	static bool hot[GRAPH_MAX_W];
	static lv_point_precise_t pts[GRAPH_MAX_W + 1];
	struct sysmon_view *m = lv_event_get_user_data(e);
	lv_layer_t *layer = lv_event_get_layer(e);
	lv_obj_t *obj = lv_event_get_current_target(e);
	uint32_t median = m->span.median_us, idle = m->paced ? 0 : FRAMETIMES_IDLE_US;
	uint32_t top = graph_top(m->span.worst_us);
	lv_area_t c;
	int32_t w, x, n;
	uint64_t from;
	unsigned i;

	lv_obj_get_content_coords(obj, &c);
	w = LV_MIN(lv_area_get_width(&c), GRAPH_MAX_W);
	if (w <= 1 || lv_area_get_height(&c) <= 4 || !m->now)
		return;
	for (i = 1; i <= 2; i++)
		graph_flat(layer, c.x1, c.x1 + w - 1, graph_y(&c, 16667 * (int32_t)i, top), true);
	{	/* the scale at the top right */
		lv_draw_label_dsc_t ld;
		lv_area_t a = { c.x1, c.y1 + 1, c.x1 + w - 4, c.y1 + 1 + lv_font_get_line_height(SYSMON_FONT) };
		char text[16];

		snprintf(text, sizeof(text), "%u ms", (unsigned)(top / 1000));
		lv_draw_label_dsc_init(&ld);
		ld.text = text;
		ld.text_local = 1;
		ld.font = SYSMON_FONT;
		ld.color = lv_color_white();
		ld.opa = LV_OPA_50;
		ld.align = LV_TEXT_ALIGN_RIGHT;
		lv_draw_label(layer, &ld, &a);
	}

	from = m->now - GRAPH_SPAN_US;
	for (x = 0; x < w; x++) {
		col[x] = COL_NONE;
		hot[x] = false;
	}
#define COL(t) ((t) <= from ? 0 : (int32_t)LV_MIN((int64_t)(((t) - from) * w / GRAPH_SPAN_US), w - 1))
	for (i = 1; i <= m->nt; i++) {
		uint64_t t0 = m->t[i - 1], t1 = i < m->nt ? m->t[i] : m->now;
		uint32_t d = t1 > t0 ? (uint32_t)LV_MIN(t1 - t0, UINT32_MAX) : 0;
		bool pause = idle && d > idle, is_hot = !pause && frametimes_is_hitch(d, median);

		if (t1 <= from)
			continue;
		/* the frame being waited for: only once it is late or a pause */
		if (i == m->nt && !is_hot && !pause)
			break;
		for (x = COL(t0); x <= COL(t1); x++) {
			if (pause) {
				if (col[x] == COL_NONE)
					col[x] = COL_IDLE;
			} else if ((int32_t)d > col[x]) {
				col[x] = (int32_t)d;
				hot[x] = is_hot;
			}
		}
	}
#undef COL
	/* a polyline per stretch of one colour; pauses break it */
	for (x = 0; x < w;) {
		bool h = hot[x];

		if (col[x] == COL_NONE) {
			x++;
			continue;
		}
		if (col[x] == COL_IDLE) {
			int32_t x0 = x;

			while (x < w && col[x] == COL_IDLE)
				x++;
			graph_flat(layer, c.x1 + x0, c.x1 + x - 1, c.y2 - 1, false);
			continue;
		}
		n = 0;
		/* from the last point of the stretch before: no gap in the line */
		if (x > 0 && col[x - 1] >= 0) {
			pts[n].x = c.x1 + x - 1;
			pts[n++].y = graph_y(&c, col[x - 1], top);
		}
		for (; x < w && col[x] >= 0 && hot[x] == h; x++) {
			int32_t y = graph_y(&c, col[x], top);

			/* a run of one height: one segment */
			if (n >= 2 && pts[n - 1].y == y && pts[n - 2].y == y) {
				pts[n - 1].x = c.x1 + x;
				continue;
			}
			pts[n].x = c.x1 + x;
			pts[n++].y = y;
		}
		if (n == 1) {	/* a single column */
			pts[1] = pts[0];
			pts[1].x++;
			n = 2;
		}
		graph_line(layer, pts, n, h ? UI_COLOR_WARN : UI_COLOR_ON);
	}
}

static lv_obj_t *graph_new(struct sysmon_view *m, lv_obj_t *parent, int32_t width)
{
	lv_obj_t *g = lv_obj_create(parent);

	lv_obj_remove_style_all(g);
	lv_obj_set_size(g, width, GRAPH_H);
	lv_obj_set_style_bg_color(g, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(g, LV_OPA_50, 0);
	lv_obj_set_clickable(g, false);
	lv_obj_set_scrollable(g, false);
	lv_obj_add_event_cb(g, graph_draw, LV_EVENT_DRAW_MAIN_END, m);
	return g;
}

static void ms_text(char *buf, size_t size, uint32_t us)
{
	if (us < 10000)
		fmt_fixed(buf, size, (us + 50) / 100, 1);
	else
		snprintf(buf, size, "%u", (us + 500) / 1000);
}

/* "31 fps  99% 45 ms  worst 120 ms  2 hitches" of the last second and the graph */
static void frames_text(const struct sysmon_view *m, char *buf, size_t size, bool unit)
{
	char p99[16], worst[16];
	int len;

	if (m->still) {
		len = snprintf(buf, size, "%s", "still");
	} else if (m->last_s.fps_x10 < 0) {
		snprintf(buf, size, "...");
		return;
	} else {
		len = snprintf(buf, size, "%d%s", (m->last_s.fps_x10 + 5) / 10, unit ? " fps" : "");
	}
	if (!m->span.frames)
		return;
	ms_text(p99, sizeof(p99), m->span.p99_us);
	ms_text(worst, sizeof(worst), m->span.worst_us);
	snprintf(buf + len, size - len, "  99%% %s ms  worst %s ms  %u %s", p99, worst,
		 m->span.hitches, m->span.hitches == 1 ? "hitch" : "hitches");
}

/* The frame times now: to the FPS row and the graphs */
static void frames_take(struct sysmon_view *m)
{
	uint32_t idle;

	m->now = now_us();
	m->nt = view_video_frame_times(m->t, FRAMETIMES_MAX);
	m->paced = view_video_paced();
	idle = m->paced ? 0 : FRAMETIMES_IDLE_US;
	frametimes_summarize(m->t, m->nt, m->now - 1000000, idle, &m->last_s);
	frametimes_summarize(m->t, m->nt, m->now - GRAPH_SPAN_US, idle, &m->span);
	/* a still picture of a game that would not say so */
	m->still = !m->paced && m->nt && m->now - m->t[m->nt - 1] > FRAMETIMES_IDLE_US;
	/* nothing for a second: the frame rate is that, not the last stretch */
	if (m->nt && m->now - m->t[m->nt - 1] > 1000000)
		m->last_s.fps_x10 = 0;
}

static void show_frames(struct sysmon_view *m)
{
	char buf[96];
	bool game = view_video_shown() != VIDEO_NONE;

	frames_take(m);
	if (m->graph && m->layout.fps) {
		frames_text(m, buf, sizeof(buf), false);
		set_text(m->fps.value, buf);
		lv_obj_invalidate(m->graph);
	}
	if (!m->frames_only)
		return;
	if (game) {
		frames_text(m, buf, sizeof(buf), true);
		set_text(m->mini_text, buf);
		lv_obj_invalidate(m->mini_graph);
	}
	if (lv_obj_is_hidden(m->mini) == game) {
		lv_obj_set_hidden(m->mini, !game);
		if (game)
			lv_obj_move_foreground(m->mini);
	}
}

static void on_tick(lv_timer_t *t)
{
	show_frames(lv_timer_get_user_data(t));
}

static void show_procs(struct sysmon_view *m, const struct sysmon_info *in)
{
	char buf[80], rss[32];
	int i;

	for (i = 0; i < in->nprocs; i++) {
		const struct sysmon_proc *p = &in->procs[i];

		fmt_mb(rss, sizeof(rss), p->rss_mb);
		if (p->cpu >= 0)
			snprintf(buf, sizeof(buf), "%d%%  %s", p->cpu, rss);
		else
			snprintf(buf, sizeof(buf), "...  %s", rss);
		set_text(m->procs[i].value, buf);
		set_bar(m->procs[i].bar[0], p->cpu, p->cpu >= HOT_LOAD);
	}
}

static void show_uptime(struct sysmon_view *m, const struct sysmon_info *in)
{
	char t[32], buf[64];
	uint32_t s = in->uptime_s;

	snprintf(t, sizeof(t), "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
	snprintf(buf, sizeof(buf), "up %s", t);
	set_text(m->head_up, buf);
}

/* ---- widget ----------------------------------------------------------- */

static lv_obj_t *panel_new(lv_obj_t *parent, int32_t width)
{
	lv_obj_t *p = lv_obj_create(parent);

	lv_obj_remove_style_all(p);
	lv_obj_set_size(p, width, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_color(p, lv_color_hex(0x101418), 0);
	lv_obj_set_style_bg_opa(p, LV_OPA_90, 0);
	lv_obj_set_style_radius(p, 8, 0);
	lv_obj_set_style_pad_all(p, 10, 0);
	lv_obj_set_style_pad_row(p, 4, 0);
	lv_obj_set_style_text_color(p, lv_color_white(), 0);
	lv_obj_set_style_text_font(p, SYSMON_FONT, 0);
	lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scrollable(p, false);
	lv_obj_set_hidden(p, true);
	return p;
}

static void *sysmon_init(lv_obj_t *parent, lv_obj_t **root)
{
	struct sysmon_view *m = calloc(1, sizeof(*m));
	int32_t w = lv_display_get_horizontal_resolution(lv_display_get_default()) - 2 * MARGIN;
	int32_t h = lv_display_get_vertical_resolution(lv_display_get_default()) - 2 * MARGIN;
	lv_obj_t *head, *box;

	if (!m)
		return NULL;
	m->last_s.fps_x10 = -1;
	/* both panels in one see-through box over the whole screen */
	box = lv_obj_create(parent);
	lv_obj_remove_style_all(box);
	lv_obj_set_size(box, LV_PCT(100), LV_PCT(100));
	lv_obj_set_clickable(box, false);
	lv_obj_set_scrollable(box, false);

	m->panel = panel_new(box, LV_MIN(w, PANEL_MAX_W));
	lv_obj_set_style_max_height(m->panel, h, 0);	/* what does not fit is cut off */
	lv_obj_align(m->panel, LV_ALIGN_TOP_LEFT, MARGIN, MARGIN);

	head = lv_obj_create(m->panel);
	lv_obj_remove_style_all(head);
	lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
	m->head_title = label_new(head, 0);
	lv_obj_set_style_text_color(m->head_title, UI_COLOR_ON, 0);
	lv_obj_align(m->head_title, LV_ALIGN_LEFT_MID, 0, 0);
	m->head_up = label_new(head, 0);
	lv_obj_set_style_text_color(m->head_up, UI_COLOR_DIM, 0);
	lv_obj_align(m->head_up, LV_ALIGN_RIGHT_MID, 0, 0);

	m->body = lv_obj_create(m->panel);
	lv_obj_remove_style_all(m->body);
	lv_obj_set_size(m->body, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(m->body, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(m->body, 3, 0);

	m->mini = panel_new(box, LV_MIN(w, MINI_W));
	lv_obj_set_style_pad_all(m->mini, 8, 0);
	lv_obj_align(m->mini, LV_ALIGN_TOP_RIGHT, -MARGIN, MARGIN);
	m->mini_text = label_new(m->mini, 0);
	lv_obj_set_width(m->mini_text, LV_PCT(100));
	lv_label_set_long_mode(m->mini_text, LV_LABEL_LONG_MODE_DOTS);
	m->mini_graph = graph_new(m, m->mini, LV_PCT(100));

	m->tick = lv_timer_create(on_tick, TICK_MS, m);
	lv_timer_pause(m->tick);
	*root = box;
	return m;
}

static void hide_all(struct sysmon_view *m)
{
	if (!lv_obj_is_hidden(m->panel))
		lv_obj_set_hidden(m->panel, true);
	if (!lv_obj_is_hidden(m->mini))
		lv_obj_set_hidden(m->mini, true);
	lv_timer_pause(m->tick);
	m->seq = 0;
	m->now = 0;
}

static void sysmon_update(void *self, const struct gui_ctx *c, uint32_t changed)
{
	struct sysmon_view *m = self;
	const struct sysmon_info *in = &c->sysmon.info;
	struct layout now;

	(void)changed;
	if (!c->sysmon.shown) {
		hide_all(m);
		m->frames_only = false;
		return;
	}
	if (c->sysmon.frames_only) {
		/* the small panel: shown and hidden by the tick with the game */
		if (!m->frames_only) {
			hide_all(m);
			m->frames_only = true;
			lv_obj_move_foreground(lv_obj_get_parent(m->panel));
			lv_timer_resume(m->tick);
			show_frames(m);
		}
		return;
	}
	if (m->frames_only) {
		hide_all(m);
		m->frames_only = false;
	}
	if (!c->sysmon.seq) {
		hide_all(m);
		return;
	}
	if (c->sysmon.seq == m->seq)
		return;		/* the same sample */
	layout_of(&now, in);
	if (!m->built || memcmp(&now, &m->layout, sizeof(now)))
		build(m, in);
	m->seq = c->sysmon.seq;
	show_clusters(m, in);
	show_memory(m, in);
	show_temps(m, in);
	show_battery(m, in);
	show_disks(m, in);
	show_procs(m, in);
	show_uptime(m, in);
	show_frames(m);
	if (m->layout.fps)
		lv_timer_resume(m->tick);
	else
		lv_timer_pause(m->tick);
	if (lv_obj_is_hidden(m->panel)) {
		lv_obj_set_hidden(m->panel, false);
		lv_obj_move_foreground(lv_obj_get_parent(m->panel));	/* over toasts made before it */
	}
}

static void sysmon_deinit(void *self)
{
	struct sysmon_view *m = self;

	lv_timer_delete(m->tick);
	free(m);
}

const struct widget_ops sysmon_widget = {
	.name = "sysmon",
	.deps = CTX_SYSMON,
	.init = sysmon_init,
	.update = sysmon_update,
	.deinit = sysmon_deinit,
};
