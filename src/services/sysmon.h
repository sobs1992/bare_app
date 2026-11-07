/*
 * System monitor readings (services/sysmon.c): CPU load and clocks, GPU
 * clock, memory, temperatures, the battery's power, disks (I/O and free
 * space), bare_app and the games it started. Everything comes from
 * procfs and sysfs, so it works on any Linux board and on the PC
 * without platform tables. Loads and rates are taken between two
 * samples: the first one has them unknown (-1).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SYSMON_MAX_CPUS 16
#define SYSMON_MAX_CLUSTERS 4
#define SYSMON_MAX_TEMPS 6
#define SYSMON_MAX_DISKS 4
#define SYSMON_MAX_MOUNTS 4
#define SYSMON_MAX_PROCS 4
#define SYSMON_NAME 16

enum sysmon_temp_kind {
	SYSMON_TEMP_OTHER,	/* name says what it is */
	SYSMON_TEMP_CPU,
	SYSMON_TEMP_SOC,
	SYSMON_TEMP_GPU,
	SYSMON_TEMP_DDR,
};

enum sysmon_disk_kind {
	SYSMON_DISK_OTHER,
	SYSMON_DISK_EMMC,
	SYSMON_DISK_SD,
	SYSMON_DISK_USB,
	SYSMON_DISK_NVME,
};

/* CPUs that share a clock (a cpufreq policy) */
struct sysmon_cluster {
	char core[SYSMON_NAME];	/* "A73" from /proc/cpuinfo, "" unknown */
	int first, count;	/* CPUs first .. first + count - 1 */
	int mhz, max_mhz;	/* 0: no cpufreq */
};

struct sysmon_mount {
	char path[SYSMON_NAME];
	uint32_t used_mb;	/* as df counts it: blocks not free */
	uint32_t free_mb;	/* available to users (no blocks reserved for root) */
	uint32_t total_mb;
};

struct sysmon_disk {
	char name[SYSMON_NAME];	/* "mmcblk1" */
	enum sysmon_disk_kind kind;
	int busy;		/* percent of the time with I/O going on, -1 unknown */
	uint32_t used_mb, free_mb;	/* of its mounts above, added up */
	uint32_t read_kbs, write_kbs;
	int nmounts;
	struct sysmon_mount mounts[SYSMON_MAX_MOUNTS];
};

struct sysmon_temp {
	enum sysmon_temp_kind kind;
	char name[SYSMON_NAME];	/* thermal zone type without "-thermal" */
	int mdeg;		/* millidegrees Celsius */
};

struct sysmon_proc {
	char name[SYSMON_NAME];
	int pid;
	bool self;		/* bare_app; the others are the games it started */
	int cpu;		/* percent of one CPU (threads add up), -1 unknown */
	uint32_t rss_mb;
};

struct sysmon_info {
	int ncpus;
	int cpu_total;		/* percent of all CPUs, -1 unknown */
	int cpu[SYSMON_MAX_CPUS];	/* percent, -1 offline or unknown */
	int nclusters;
	struct sysmon_cluster clusters[SYSMON_MAX_CLUSTERS];
	int gpu_mhz, gpu_max_mhz;	/* 0: no GPU devfreq */
	uint32_t mem_total_mb, mem_used_mb;	/* used: what is not available */
	uint32_t swap_total_mb, swap_used_mb;
	int ntemps;
	struct sysmon_temp temps[SYSMON_MAX_TEMPS];
	bool battery;
	int bat_capacity;	/* percent, -1 unknown */
	int bat_mv, bat_ma;	/* current: + into the battery, - out of it */
	int bat_mw;		/* the same sign */
	int ndisks;
	struct sysmon_disk disks[SYSMON_MAX_DISKS];
	int nprocs;
	struct sysmon_proc procs[SYSMON_MAX_PROCS];	/* bare_app first */
	uint32_t uptime_s;
};

struct sysmon;

/* root: prefix for /proc and /sys (tests), NULL or "" for the real ones */
struct sysmon *sysmon_open(const char *root);
void sysmon_close(struct sysmon *s);
/* now: now_ms(); rates are over the time since the previous sample */
void sysmon_sample(struct sysmon *s, uint64_t now, struct sysmon_info *out);
