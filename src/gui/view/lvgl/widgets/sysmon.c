/*
 * System monitor (CTX_SYSMON) over everything on the top layer, also over
 * a game: a dark panel with a row per CPU cluster (a bar per core), the
 * GPU, memory, temperatures, the battery, each disk (a bar for the space
 * used on its partitions, reads and writes, free space under it),
 * bare_app with its game and, while a game is on the screen, its frames
 * a second. F5 held shows and hides it (view core), the game keeps running
 * under it. The rows are built when it is shown and again when what there
 * is changes (a game starts, a disk comes). Always in English, whatever
 * the UI language.
 *
 * Frames a second are the view's own: frames of the game shown in the
 * video layer between two samples.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../widget.h"
#include "core/util.h"

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

struct sysmon_view {
	lv_obj_t *panel, *head_title, *head_up, *body;
	bool built;
	struct layout layout;
	struct row fps, clusters[SYSMON_MAX_CLUSTERS], gpu, ram, temps, battery;
	struct row disks[SYSMON_MAX_DISKS], procs[SYSMON_MAX_PROCS];
	lv_obj_t *mounts[SYSMON_MAX_DISKS];
	unsigned seq;		/* the sample shown */
	uint64_t frames_ms;	/* now_ms() at the last sample, 0 none */
	unsigned game_frames;	/* view_video_frames() then */
	enum video_source frames_src;	/* the game shown then */
	int game_fps;	/* -1 unknown */
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
		lv_obj_set_size(l, width, lv_font_get_line_height(UI_FONT_SMALL));
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
		lv_obj_set_height(r->value, lv_font_get_line_height(UI_FONT_SMALL));
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
	if (m->layout.fps)
		row_new(m, &m->fps, "FPS", 0, false);
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

/* Frames a second from a counter: -1 without an earlier reading */
static int rate(unsigned now, unsigned then, uint64_t ms)
{
	if (!ms || now < then)
		return -1;
	return (int)(((now - then) * 1000ULL + ms / 2) / ms);
}

/* Frames of the game in the video layer since the last sample */
static void count_frames(struct sysmon_view *m)
{
	enum video_source src = view_video_shown();
	unsigned game = view_video_frames();
	uint64_t now = now_ms(), ms = m->frames_ms && now > m->frames_ms ? now - m->frames_ms : 0;

	m->game_fps = src != VIDEO_NONE && src == m->frames_src ? rate(game, m->game_frames, ms) : -1;
	m->frames_ms = now;
	m->game_frames = game;
	m->frames_src = src;
}

static void show_fps(struct sysmon_view *m)
{
	char buf[16];

	if (!m->layout.fps)
		return;
	if (m->game_fps >= 0)
		snprintf(buf, sizeof(buf), "%d", m->game_fps);
	else
		snprintf(buf, sizeof(buf), "...");
	set_text(m->fps.value, buf);
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

static void *sysmon_init(lv_obj_t *parent, lv_obj_t **root)
{
	struct sysmon_view *m = calloc(1, sizeof(*m));
	int32_t w = lv_display_get_horizontal_resolution(lv_display_get_default()) - 2 * MARGIN;
	int32_t h = lv_display_get_vertical_resolution(lv_display_get_default()) - 2 * MARGIN;
	lv_obj_t *head;

	if (!m)
		return NULL;
	m->game_fps = -1;
	m->panel = lv_obj_create(parent);
	lv_obj_remove_style_all(m->panel);
	lv_obj_set_size(m->panel, LV_MIN(w, PANEL_MAX_W), LV_SIZE_CONTENT);
	lv_obj_set_style_max_height(m->panel, h, 0);	/* what does not fit is cut off */
	lv_obj_set_style_bg_color(m->panel, lv_color_hex(0x101418), 0);
	lv_obj_set_style_bg_opa(m->panel, LV_OPA_90, 0);
	lv_obj_set_style_radius(m->panel, 8, 0);
	lv_obj_set_style_pad_all(m->panel, 10, 0);
	lv_obj_set_style_pad_row(m->panel, 4, 0);
	lv_obj_set_style_text_color(m->panel, lv_color_white(), 0);
	lv_obj_set_style_text_font(m->panel, UI_FONT_SMALL, 0);
	lv_obj_set_flex_flow(m->panel, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scrollable(m->panel, false);
	lv_obj_align(m->panel, LV_ALIGN_TOP_LEFT, MARGIN, MARGIN);
	lv_obj_set_hidden(m->panel, true);

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
	*root = m->panel;
	return m;
}

static void sysmon_update(void *self, const struct gui_ctx *c, uint32_t changed)
{
	struct sysmon_view *m = self;
	const struct sysmon_info *in = &c->sysmon.info;
	struct layout now;

	(void)changed;
	if (!c->sysmon.shown || !c->sysmon.seq) {
		if (!lv_obj_is_hidden(m->panel))
			lv_obj_set_hidden(m->panel, true);
		m->seq = 0;
		m->frames_ms = 0;
		return;
	}
	if (c->sysmon.seq == m->seq)
		return;		/* the same sample */
	layout_of(&now, in);
	if (!m->built || memcmp(&now, &m->layout, sizeof(now)))
		build(m, in);
	count_frames(m);
	m->seq = c->sysmon.seq;
	show_fps(m);
	show_clusters(m, in);
	show_memory(m, in);
	show_temps(m, in);
	show_battery(m, in);
	show_disks(m, in);
	show_procs(m, in);
	show_uptime(m, in);
	if (lv_obj_is_hidden(m->panel)) {
		lv_obj_set_hidden(m->panel, false);
		lv_obj_move_foreground(m->panel);	/* over toasts made before it */
	}
}

static void sysmon_deinit(void *self)
{
	free(self);
}

const struct widget_ops sysmon_widget = {
	.name = "sysmon",
	.deps = CTX_SYSMON,
	.init = sysmon_init,
	.update = sysmon_update,
	.deinit = sysmon_deinit,
};
