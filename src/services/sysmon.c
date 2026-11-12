/*
 * System monitor readings from procfs and sysfs (see sysmon.h). What does
 * not change while running (cpufreq policies, core names, thermal zones,
 * the GPU's devfreq, the battery, the disks) is found once in
 * sysmon_open(); sysmon_sample() reads the values and keeps the counters
 * that loads and rates need.
 */
#include "sysmon.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "core/util.h"

#define MAX_POLICIES 32
#define MAX_PARTS 64		/* /proc/diskstats lines looked at */
#define MAX_KIDS 8		/* processes whose CPU time is followed */
#define PATH_LEN 320

struct ticks {
	unsigned long long busy, total;
};

struct policy {
	int num;		/* policyN */
	int first, count;
};

struct zone {
	int num;		/* thermal_zoneN */
	enum sysmon_temp_kind kind;
	char name[SYSMON_NAME];
};

struct disk_counters {
	unsigned long long rd_sectors, wr_sectors, io_ms;
};

struct proc_ticks {
	int pid;
	unsigned long long ticks;
};

struct sysmon {
	char root[128];
	long clk_tck, page_kb;
	uint64_t prev_ms;	/* 0: no previous sample */
	struct ticks cpu_prev[SYSMON_MAX_CPUS + 1];	/* [0]: all, [1 + n]: cpuN */
	bool cpu_prev_ok[SYSMON_MAX_CPUS + 1];
	int npolicies;
	struct policy policies[MAX_POLICIES];
	int nclusters;
	struct sysmon_cluster clusters[SYSMON_MAX_CLUSTERS];
	int cluster_of[MAX_POLICIES];
	int nzones;
	struct zone zones[SYSMON_MAX_TEMPS];
	char gpu[64];		/* devfreq device, "" none */
	char battery[64];	/* power_supply of type Battery, "" none */
	int ndisks;
	struct {
		char name[SYSMON_NAME];
		enum sysmon_disk_kind kind;
		struct disk_counters prev;
		bool prev_ok;
	} disks[SYSMON_MAX_DISKS];
	int nkids;
	struct proc_ticks kids[MAX_KIDS];	/* last sample's, bare_app included */
};

/* ---- files ------------------------------------------------------------ */

static void path_of(const struct sysmon *s, char *buf, const char *fmt, const char *a,
		    const char *b)
{
	int n = snprintf(buf, PATH_LEN, "%s", s->root);

	snprintf(buf + n, PATH_LEN - n, fmt, a, b);
}

static int read_text(const struct sysmon *s, char *buf, int size, const char *fmt,
		     const char *a, const char *b)
{
	char path[PATH_LEN];

	path_of(s, path, fmt, a, b);
	return read_text_file(path, buf, size);
}

static int read_ll(const struct sysmon *s, long long *val, const char *fmt, const char *a,
		   const char *b)
{
	char buf[32], *end;

	if (read_text(s, buf, sizeof(buf), fmt, a, b) <= 0)
		return -1;
	*val = strtoll(buf, &end, 10);
	return end == buf ? -1 : 0;
}

static FILE *open_file(const struct sysmon *s, const char *rel)
{
	char path[PATH_LEN];

	path_of(s, path, "%s%s", rel, "");
	return fopen(path, "r");
}

/* As much of src as fits, always terminated */
static void copy_str(char *dst, size_t size, const char *src)
{
	size_t n = strnlen(src, size - 1);

	memcpy(dst, src, n);
	dst[n] = '\0';
}

static void copy_name(char *dst, const char *src)
{
	copy_str(dst, SYSMON_NAME, src);
}

/* Entries of a directory starting with prefix and a number, sorted by it */
static int numbered_entries(const struct sysmon *s, const char *dir, const char *prefix, int *nums,
			    int max)
{
	char path[PATH_LEN];
	struct dirent *de;
	size_t plen = strlen(prefix);
	DIR *d;
	int n = 0, i, j;

	path_of(s, path, "%s%s", dir, "");
	d = opendir(path);
	if (!d)
		return 0;
	while ((de = readdir(d)) && n < max)
		if (!strncmp(de->d_name, prefix, plen) && isdigit((unsigned char)de->d_name[plen]))
			nums[n++] = atoi(de->d_name + plen);
	closedir(d);
	for (i = 1; i < n; i++)
		for (j = i; j > 0 && nums[j - 1] > nums[j]; j--) {
			int t = nums[j];

			nums[j] = nums[j - 1];
			nums[j - 1] = t;
		}
	return n;
}

/* ---- found once ------------------------------------------------------- */

static const char *core_name(unsigned part)
{
	static const struct { unsigned part; const char *name; } parts[] = {
		{ 0xd03, "A53" }, { 0xd04, "A35" }, { 0xd05, "A55" }, { 0xd07, "A57" },
		{ 0xd08, "A72" }, { 0xd09, "A73" }, { 0xd0a, "A75" }, { 0xd0b, "A76" },
		{ 0xd0d, "A77" }, { 0xd41, "A78" }, { 0xd44, "X1" }, { 0xd46, "A510" },
		{ 0xd47, "A710" }, { 0xd48, "X2" }, { 0xd4d, "A715" }, { 0xd4e, "X3" },
		{ 0xd80, "A520" }, { 0xd81, "A720" }, { 0xd82, "X4" },
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(parts); i++)
		if (parts[i].part == part)
			return parts[i].name;
	return "";
}

/* "CPU part" of each processor in /proc/cpuinfo (Arm), 0 unknown */
static void cpu_parts(const struct sysmon *s, unsigned *parts, int max)
{
	FILE *f = open_file(s, "/proc/cpuinfo");
	char line[256];
	int cpu = -1;

	memset(parts, 0, sizeof(*parts) * max);
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char *colon = strchr(line, ':');

		if (!colon)
			continue;
		if (!strncmp(line, "processor", 9))
			cpu = atoi(colon + 1);
		else if (!strncmp(line, "CPU part", 8) && cpu >= 0 && cpu < max)
			parts[cpu] = (unsigned)strtoul(colon + 1, NULL, 0);
	}
	fclose(f);
}

static void find_clusters(struct sysmon *s)
{
	unsigned parts[SYSMON_MAX_CPUS];
	int nums[MAX_POLICIES], n, i;
	char buf[256], num[16];

	n = numbered_entries(s, "/sys/devices/system/cpu/cpufreq", "policy", nums, MAX_POLICIES);
	for (i = 0; i < n; i++) {
		struct policy *p = &s->policies[s->npolicies];
		char *tok, *save;

		snprintf(num, sizeof(num), "%d", nums[i]);
		if (read_text(s, buf, sizeof(buf), "/sys/devices/system/cpu/cpufreq/policy%s%s",
			      num, "/related_cpus") <= 0)
			continue;
		p->num = nums[i];
		p->first = -1;
		p->count = 0;
		for (tok = strtok_r(buf, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
			if (p->first < 0)
				p->first = atoi(tok);
			p->count++;
		}
		if (p->first >= 0)
			s->npolicies++;
	}
	cpu_parts(s, parts, SYSMON_MAX_CPUS);
	if (s->npolicies > 0 && s->npolicies <= SYSMON_MAX_CLUSTERS) {
		for (i = 0; i < s->npolicies; i++) {
			struct sysmon_cluster *c = &s->clusters[i];
			const struct policy *p = &s->policies[i];

			c->first = p->first;
			c->count = p->count;
			if (p->first < SYSMON_MAX_CPUS)
				copy_name(c->core, core_name(parts[p->first]));
			s->cluster_of[i] = i;
		}
		s->nclusters = s->npolicies;
		return;
	}
	/* no cpufreq, or a policy per CPU (PCs): all of them as one */
	s->nclusters = 1;
	s->clusters[0].first = 0;
	s->clusters[0].count = 0;	/* filled from /proc/stat */
	copy_name(s->clusters[0].core, core_name(parts[0]));
	for (i = 0; i < s->npolicies; i++)
		s->cluster_of[i] = 0;
}

static void find_zones(struct sysmon *s)
{
	int nums[64], n, i;
	char type[64], num[16];

	n = numbered_entries(s, "/sys/class/thermal", "thermal_zone", nums, 64);
	for (i = 0; i < n && s->nzones < SYSMON_MAX_TEMPS; i++) {
		struct zone *z = &s->zones[s->nzones];
		char *suffix;

		snprintf(num, sizeof(num), "%d", nums[i]);
		if (read_text(s, type, sizeof(type), "/sys/class/thermal/thermal_zone%s%s", num,
			      "/type") <= 0)
			continue;
		/*
		 * A battery's zone is its power_supply TEMP: without a
		 * thermistor drivers report a default (rk818 on the GO
		 * Ultra: 20.0 degrees), so it is left out.
		 */
		if (strstr(type, "battery") || strstr(type, "bat"))
			continue;
		z->num = nums[i];
		if (strstr(type, "gpu"))
			z->kind = SYSMON_TEMP_GPU;
		else if (strstr(type, "ddr") || strstr(type, "dmc") || strstr(type, "mem"))
			z->kind = SYSMON_TEMP_DDR;
		else if (strstr(type, "soc"))
			z->kind = SYSMON_TEMP_SOC;
		else if (strstr(type, "cpu"))
			z->kind = SYSMON_TEMP_CPU;
		else
			z->kind = SYSMON_TEMP_OTHER;
		suffix = strstr(type, "-thermal");
		if (!suffix)
			suffix = strstr(type, "_thermal");
		if (suffix && suffix != type)
			*suffix = '\0';
		copy_name(z->name, type);
		s->nzones++;
	}
}

/* First entry of a class directory that matches */
static void find_entry(const struct sysmon *s, const char *dir, bool (*match)(const struct sysmon *,
		       const char *), char *out, size_t size)
{
	char path[PATH_LEN];
	struct dirent *de;
	DIR *d;

	out[0] = '\0';
	path_of(s, path, "%s%s", dir, "");
	d = opendir(path);
	if (!d)
		return;
	while ((de = readdir(d)))
		if (de->d_name[0] != '.' && match(s, de->d_name)) {
			copy_str(out, size, de->d_name);
			break;
		}
	closedir(d);
}

static bool is_gpu(const struct sysmon *s, const char *name)
{
	(void)s;
	return strstr(name, "gpu") != NULL;
}

static bool is_battery(const struct sysmon *s, const char *name)
{
	char type[32];

	return read_text(s, type, sizeof(type), "/sys/class/power_supply/%s%s", name, "/type") > 0 &&
	       !strcmp(type, "Battery");
}

static enum sysmon_disk_kind disk_kind(const struct sysmon *s, const char *name)
{
	char buf[PATH_LEN], link[PATH_LEN];
	ssize_t n;

	if (read_text(s, buf, sizeof(buf), "/sys/block/%s%s", name, "/device/type") > 0) {
		if (!strcmp(buf, "MMC"))
			return SYSMON_DISK_EMMC;
		if (!strcmp(buf, "SD"))
			return SYSMON_DISK_SD;
	}
	if (!strncmp(name, "nvme", 4))
		return SYSMON_DISK_NVME;
	path_of(s, buf, "/sys/block/%s%s", name, "");
	n = readlink(buf, link, sizeof(link) - 1);
	if (n > 0) {
		link[n] = '\0';
		if (strstr(link, "/usb"))
			return SYSMON_DISK_USB;
	}
	return SYSMON_DISK_OTHER;
}

/* Whole disks with a device behind them: no loop, ram, zram, nor the eMMC's
 * boot and RPMB areas, nor SPI flash (mtdblock: the boot loader's) */
static void find_disks(struct sysmon *s)
{
	char path[PATH_LEN];
	struct dirent *de;
	struct stat st;
	DIR *d;

	path_of(s, path, "%s%s", "/sys/block", "");
	d = opendir(path);
	if (!d)
		return;
	while ((de = readdir(d)) && s->ndisks < SYSMON_MAX_DISKS) {
		const char *n = de->d_name;

		if (n[0] == '.' || strstr(n, "boot") || strstr(n, "rpmb") || !strncmp(n, "mtd", 3) ||
		    strlen(n) >= SYSMON_NAME)
			continue;
		path_of(s, path, "/sys/block/%s%s", n, "/device");
		if (stat(path, &st) < 0)
			continue;
		copy_name(s->disks[s->ndisks].name, n);
		s->disks[s->ndisks].kind = disk_kind(s, n);
		s->ndisks++;
	}
	closedir(d);
}

struct sysmon *sysmon_open(const char *root)
{
	struct sysmon *s = calloc(1, sizeof(*s));

	if (!s)
		return NULL;
	snprintf(s->root, sizeof(s->root), "%s", root ? root : "");
	s->clk_tck = sysconf(_SC_CLK_TCK);
	if (s->clk_tck <= 0)
		s->clk_tck = 100;
	s->page_kb = sysconf(_SC_PAGESIZE) / 1024;
	if (s->page_kb <= 0)
		s->page_kb = 4;
	find_clusters(s);
	find_zones(s);
	find_entry(s, "/sys/class/devfreq", is_gpu, s->gpu, sizeof(s->gpu));
	find_entry(s, "/sys/class/power_supply", is_battery, s->battery, sizeof(s->battery));
	find_disks(s);
	return s;
}

void sysmon_close(struct sysmon *s)
{
	free(s);
}

/* ---- read each time --------------------------------------------------- */

static int percent(unsigned long long part, unsigned long long whole)
{
	unsigned long long pc;

	if (!whole)
		return 0;
	pc = (part * 100 + whole / 2) / whole;
	return pc > 100 ? 100 : (int)pc;
}

static void sample_cpus(struct sysmon *s, struct sysmon_info *out)
{
	FILE *f = open_file(s, "/proc/stat");
	char line[512];
	int i;

	out->cpu_total = -1;
	for (i = 0; i < SYSMON_MAX_CPUS; i++)
		out->cpu[i] = -1;
	out->ncpus = 0;
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		unsigned long long v[8] = { 0 }, idle, total = 0;
		int cpu = -1, slot, k;
		char *p;

		if (strncmp(line, "cpu", 3))
			break;		/* the cpu lines come first */
		p = line + 3;
		if (isdigit((unsigned char)*p))
			cpu = (int)strtol(p, &p, 10);
		for (k = 0; k < 8; k++)
			v[k] = strtoull(p, &p, 10);
		for (k = 0; k < 8; k++)
			total += v[k];
		idle = v[3] + v[4];	/* idle, iowait */
		if (cpu >= SYSMON_MAX_CPUS)
			continue;
		if (cpu >= 0 && cpu + 1 > out->ncpus)
			out->ncpus = cpu + 1;
		slot = cpu + 1;
		if (s->cpu_prev_ok[slot] && total > s->cpu_prev[slot].total) {
			int pc = percent((total - idle) - s->cpu_prev[slot].busy,
					 total - s->cpu_prev[slot].total);

			if (cpu < 0)
				out->cpu_total = pc;
			else
				out->cpu[cpu] = pc;
		}
		s->cpu_prev[slot] = (struct ticks){ total - idle, total };
		s->cpu_prev_ok[slot] = true;
	}
	fclose(f);
}

static void sample_clusters(struct sysmon *s, struct sysmon_info *out)
{
	char num[16];
	int i;

	out->nclusters = s->nclusters;
	memcpy(out->clusters, s->clusters, sizeof(out->clusters));
	if (s->nclusters == 1 && !s->clusters[0].count)
		out->clusters[0].count = out->ncpus;
	for (i = 0; i < s->nclusters; i++)
		out->clusters[i].mhz = out->clusters[i].max_mhz = 0;
	for (i = 0; i < s->npolicies; i++) {
		struct sysmon_cluster *c = &out->clusters[s->cluster_of[i]];
		long long cur, max;

		snprintf(num, sizeof(num), "%d", s->policies[i].num);
		if (!read_ll(s, &cur, "/sys/devices/system/cpu/cpufreq/policy%s%s", num,
			     "/scaling_cur_freq"))
			c->mhz = MAX(c->mhz, (int)(cur / 1000));
		if (!read_ll(s, &max, "/sys/devices/system/cpu/cpufreq/policy%s%s", num,
			     "/cpuinfo_max_freq"))
			c->max_mhz = MAX(c->max_mhz, (int)(max / 1000));
	}
}

static void sample_memory(struct sysmon *s, struct sysmon_info *out)
{
	FILE *f = open_file(s, "/proc/meminfo");
	unsigned long long total = 0, avail = 0, free_kb = 0, buffers = 0, cached = 0;
	unsigned long long swap_total = 0, swap_free = 0, v;
	bool have_avail = false;
	char line[128], key[64];

	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%63[^:]: %llu", key, &v) != 2)
			continue;
		if (!strcmp(key, "MemTotal"))
			total = v;
		else if (!strcmp(key, "MemAvailable"))
			avail = v, have_avail = true;
		else if (!strcmp(key, "MemFree"))
			free_kb = v;
		else if (!strcmp(key, "Buffers"))
			buffers = v;
		else if (!strcmp(key, "Cached"))
			cached = v;
		else if (!strcmp(key, "SwapTotal"))
			swap_total = v;
		else if (!strcmp(key, "SwapFree"))
			swap_free = v;
	}
	fclose(f);
	if (!have_avail)
		avail = free_kb + buffers + cached;
	if (avail > total)
		avail = total;
	if (swap_free > swap_total)
		swap_free = swap_total;
	out->mem_total_mb = (uint32_t)(total / 1024);
	out->mem_used_mb = (uint32_t)((total - avail) / 1024);
	out->swap_total_mb = (uint32_t)(swap_total / 1024);
	out->swap_used_mb = (uint32_t)((swap_total - swap_free) / 1024);
}

static void sample_temps(struct sysmon *s, struct sysmon_info *out)
{
	char num[16];
	int i;

	out->ntemps = 0;
	for (i = 0; i < s->nzones; i++) {
		struct sysmon_temp *t = &out->temps[out->ntemps];
		long long mdeg;

		snprintf(num, sizeof(num), "%d", s->zones[i].num);
		if (read_ll(s, &mdeg, "/sys/class/thermal/thermal_zone%s%s", num, "/temp"))
			continue;
		t->kind = s->zones[i].kind;
		memcpy(t->name, s->zones[i].name, sizeof(t->name));
		t->mdeg = (int)mdeg;
		out->ntemps++;
	}
}

static void sample_gpu(struct sysmon *s, struct sysmon_info *out)
{
	long long hz;

	out->gpu_mhz = out->gpu_max_mhz = 0;
	if (!s->gpu[0])
		return;
	if (!read_ll(s, &hz, "/sys/class/devfreq/%s%s", s->gpu, "/cur_freq"))
		out->gpu_mhz = (int)(hz / 1000000);
	if (!read_ll(s, &hz, "/sys/class/devfreq/%s%s", s->gpu, "/max_freq"))
		out->gpu_max_mhz = (int)(hz / 1000000);
}

static void sample_battery(struct sysmon *s, struct sysmon_info *out)
{
	const char *dir = "/sys/class/power_supply/%s%s";
	long long uv = 0, ua = 0, uw = -1, cap = -1;
	char status[32] = "";
	bool have_power;

	out->battery = s->battery[0] != '\0';
	out->bat_capacity = -1;
	if (!out->battery)
		return;
	read_ll(s, &cap, dir, s->battery, "/capacity");
	/* the RK817's driver has only the averages */
	if (read_ll(s, &uv, dir, s->battery, "/voltage_now"))
		read_ll(s, &uv, dir, s->battery, "/voltage_avg");
	if (read_ll(s, &ua, dir, s->battery, "/current_now"))
		read_ll(s, &ua, dir, s->battery, "/current_avg");
	have_power = !read_ll(s, &uw, dir, s->battery, "/power_now");
	read_text(s, status, sizeof(status), dir, s->battery, "/status");
	/* drivers disagree on the sign: the status says which way it flows */
	if ((!strcmp(status, "Discharging") && ua > 0) || (!strcmp(status, "Charging") && ua < 0))
		ua = -ua;
	if (!have_power)
		uw = uv * ua / 1000000;
	else if ((ua < 0) != (uw < 0))
		uw = -uw;
	out->bat_capacity = (int)cap;
	out->bat_mv = (int)(uv / 1000);
	out->bat_ma = (int)(ua / 1000);
	out->bat_mw = (int)(uw / 1000);
}

/* /proc/diskstats: counters of the known disks; every line's numbers and
 * name for finding the partition of a mount by its device number */
struct part {
	unsigned major, minor;
	char name[SYSMON_NAME];
};

static int read_diskstats(struct sysmon *s, struct disk_counters *now, bool *have, struct part *parts,
			  int max)
{
	FILE *f = open_file(s, "/proc/diskstats");
	char line[512], name[64];
	int n = 0, i;

	memset(have, 0, sizeof(*have) * SYSMON_MAX_DISKS);
	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f)) {
		unsigned long long v[10];
		unsigned major, minor;

		if (sscanf(line, " %u %u %63s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
			   &major, &minor, name, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6],
			   &v[7], &v[8], &v[9]) != 13)
			continue;
		if (n < max && strlen(name) < SYSMON_NAME) {
			parts[n] = (struct part){ major, minor, "" };
			copy_name(parts[n].name, name);
			n++;
		}
		for (i = 0; i < s->ndisks; i++)
			if (!strcmp(name, s->disks[i].name)) {
				/* sectors read, sectors written, time doing I/O */
				now[i] = (struct disk_counters){ v[2], v[6], v[9] };
				have[i] = true;
			}
	}
	fclose(f);
	return n;
}

/* The disk a partition (or the disk itself) is on */
static int disk_of(const struct sysmon *s, const char *part)
{
	int i;

	for (i = 0; i < s->ndisks; i++) {
		size_t len = strlen(s->disks[i].name);
		const char *rest = part + len;

		if (strncmp(part, s->disks[i].name, len))
			continue;
		/* mmcblk1p2, nvme0n1p1, sda1 */
		if (!*rest || isdigit((unsigned char)*rest) ||
		    (*rest == 'p' && isdigit((unsigned char)rest[1])))
			return i;
	}
	return -1;
}

static void sample_mounts(struct sysmon *s, struct sysmon_info *out, const struct part *parts,
			  int nparts)
{
	FILE *f = open_file(s, "/proc/mounts");
	char line[512], dev[256], dir[256], path[PATH_LEN];

	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		struct sysmon_disk *d;
		struct sysmon_mount *m;
		const char *part = NULL;
		struct statvfs vfs;
		struct stat st;
		int i, k;

		/* the boot partition is not where space runs out */
		if (sscanf(line, "%255s %255s", dev, dir) != 2 || strncmp(dev, "/dev/", 5) ||
		    !strcmp(dir, "/boot") || !strncmp(dir, "/boot/", 6))
			continue;
		path_of(s, path, "%s%s", dir, "");
		if (!strcmp(dev, "/dev/root")) {
			/* the kernel's name for root=: look it up by number */
			if (stat(path, &st) < 0)
				continue;
			for (i = 0; i < nparts; i++)
				if (parts[i].major == major(st.st_dev) &&
				    parts[i].minor == minor(st.st_dev))
					part = parts[i].name;
		} else {
			part = dev + 5;
		}
		k = part ? disk_of(s, part) : -1;
		if (k < 0)
			continue;
		d = &out->disks[k];
		for (i = 0; i < d->nmounts; i++)
			if (!strncmp(d->mounts[i].path, dir, SYSMON_NAME - 1))
				break;
		if (i < d->nmounts || d->nmounts == SYSMON_MAX_MOUNTS)
			continue;	/* mounted twice, or no room */
		if (statvfs(path, &vfs) < 0)
			continue;
		m = &d->mounts[d->nmounts++];
		copy_name(m->path, dir);
		m->used_mb = (uint32_t)((unsigned long long)(vfs.f_blocks - vfs.f_bfree) * vfs.f_frsize >> 20);
		m->free_mb = (uint32_t)((unsigned long long)vfs.f_bavail * vfs.f_frsize >> 20);
		d->used_mb += m->used_mb;
		d->free_mb += m->free_mb;
		m->total_mb = (uint32_t)((unsigned long long)vfs.f_blocks * vfs.f_frsize >> 20);
	}
	fclose(f);
}

static void sample_disks(struct sysmon *s, struct sysmon_info *out, uint64_t dt)
{
	struct disk_counters now[SYSMON_MAX_DISKS];
	bool have[SYSMON_MAX_DISKS];
	struct part parts[MAX_PARTS];
	int nparts, i;

	nparts = read_diskstats(s, now, have, parts, MAX_PARTS);
	out->ndisks = s->ndisks;
	for (i = 0; i < s->ndisks; i++) {
		struct sysmon_disk *d = &out->disks[i];

		memset(d, 0, sizeof(*d));
		memcpy(d->name, s->disks[i].name, sizeof(d->name));
		d->kind = s->disks[i].kind;
		d->busy = -1;
		if (have[i] && s->disks[i].prev_ok && dt) {
			const struct disk_counters *p = &s->disks[i].prev;

			/* sectors are 512 bytes here whatever the device's are */
			d->read_kbs = (uint32_t)((now[i].rd_sectors - p->rd_sectors) * 512 / dt);
			d->write_kbs = (uint32_t)((now[i].wr_sectors - p->wr_sectors) * 512 / dt);
			d->busy = percent(now[i].io_ms - p->io_ms, dt);
		}
		s->disks[i].prev = now[i];
		s->disks[i].prev_ok = have[i];
	}
	sample_mounts(s, out, parts, nparts);
}

/* Name, parent, CPU ticks and resident pages from /proc/PID/stat */
static int read_proc(const struct sysmon *s, int pid, char *name, int *ppid,
		     unsigned long long *ticks, long *rss)
{
	char buf[1024], num[16], *open, *close;
	unsigned long long utime, stime;
	int n;

	snprintf(num, sizeof(num), "%d", pid);
	if (read_text(s, buf, sizeof(buf), "/proc/%s%s", num, "/stat") <= 0)
		return -1;
	open = strchr(buf, '(');
	close = strrchr(buf, ')');
	if (!open || !close || close < open)
		return -1;
	n = (int)MIN(close - open - 1, SYSMON_NAME - 1);
	memcpy(name, open + 1, n);
	name[n] = '\0';
	/* after the name: state ppid pgrp session tty tpgid flags minflt cminflt
	 * majflt cmajflt utime stime cutime cstime priority nice threads
	 * itrealvalue starttime vsize rss */
	if (sscanf(close + 2, "%*s %d %*s %*s %*s %*s %*s %*s %*s %*s %*s %llu %llu "
		   "%*s %*s %*s %*s %*s %*s %*s %*s %ld", ppid, &utime, &stime, rss) != 4)
		return -1;
	*ticks = utime + stime;
	return 0;
}

static void add_proc(struct sysmon *s, struct sysmon_info *out, int pid, bool self,
		     struct proc_ticks *seen, int *nseen, uint64_t dt)
{
	struct sysmon_proc *p;
	unsigned long long ticks;
	char name[SYSMON_NAME];
	int ppid, i;
	long rss;

	if (out->nprocs == SYSMON_MAX_PROCS || read_proc(s, pid, name, &ppid, &ticks, &rss))
		return;
	if (!self && ppid != out->procs[0].pid)
		return;
	p = &out->procs[out->nprocs++];
	memcpy(p->name, name, sizeof(p->name));
	p->pid = pid;
	p->self = self;
	p->cpu = -1;
	p->rss_mb = (uint32_t)(rss * s->page_kb / 1024);
	for (i = 0; i < s->nkids && dt; i++)
		if (s->kids[i].pid == pid && ticks >= s->kids[i].ticks)
			p->cpu = (int)((ticks - s->kids[i].ticks) * 100000ULL / (s->clk_tck * dt));
	if (*nseen < MAX_KIDS)
		seen[(*nseen)++] = (struct proc_ticks){ pid, ticks };
}

/* bare_app itself and the processes it started (the games) */
static void sample_procs(struct sysmon *s, struct sysmon_info *out, uint64_t dt)
{
	struct proc_ticks seen[MAX_KIDS];
	char path[PATH_LEN];
	struct dirent *de;
	int nseen = 0;
	DIR *d;

	out->nprocs = 0;
	add_proc(s, out, (int)getpid(), true, seen, &nseen, dt);
	if (!out->nprocs)
		return;
	path_of(s, path, "%s%s", "/proc", "");
	d = opendir(path);
	if (d) {
		while ((de = readdir(d)) && out->nprocs < SYSMON_MAX_PROCS) {
			int pid = atoi(de->d_name);

			if (pid > 0 && pid != out->procs[0].pid)
				add_proc(s, out, pid, false, seen, &nseen, dt);
		}
		closedir(d);
	}
	memcpy(s->kids, seen, sizeof(seen[0]) * nseen);
	s->nkids = nseen;
}

void sysmon_sample(struct sysmon *s, uint64_t now, struct sysmon_info *out)
{
	uint64_t dt = s->prev_ms && now > s->prev_ms ? now - s->prev_ms : 0;
	char buf[64];

	memset(out, 0, sizeof(*out));
	sample_cpus(s, out);
	sample_clusters(s, out);
	sample_memory(s, out);
	sample_temps(s, out);
	sample_gpu(s, out);
	sample_battery(s, out);
	sample_disks(s, out, dt);
	sample_procs(s, out, dt);
	if (read_text(s, buf, sizeof(buf), "%s%s", "/proc/uptime", "") > 0)
		out->uptime_s = (uint32_t)strtoul(buf, NULL, 10);
	s->prev_ms = now;
}
