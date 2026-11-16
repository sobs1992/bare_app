/*
 * bare_app_bench: a stress test of the GPU, the CPUs and the memory path
 * between them, made to catch bit flips like the GO Ultra's bit 42 (a
 * cache line filled from DRAM with one bit wrong, under GPU load).
 *
 * Load:
 *   - GPU: a fur donut (shells of a torus, discard, lots of fragments and
 *     vertices), a background of random texel fetches from big textures,
 *     a strip of those textures uploaded again every frame and now and
 *     then a whole texture made again (as a game loading a map);
 *   - CPUs: one memory checker per CPU (nice 19).
 *
 * Detectors (each counts its checks, its errors and the bits that flipped):
 *   MEM  the CPUs write patterns into their own memory and read them back
 *        (8 bytes a word: bit 0..63 of the word as it lies in DRAM); a
 *        wrong word is read again from the cache, then from DRAM (dc civac)
 *        and 1 ms later: "cache wrong, DRAM right" is a bad line fill;
 *   GPU  a compute shader reads a buffer the CPU wrote and checks it
 *        against the same formula (CPU write -> DRAM -> GPU read);
 *   CPU  a compute shader writes a pattern, the CPU checks it (GPU write ->
 *        DRAM -> CPU read);
 *   TEX  a region of a big texture copied by the GPU into a small frame
 *        and read back (texture path; bits of the 32-bit texel);
 *   and the kernel log: GPU faults, Oopses.
 *
 * Under bare_app it shows the donut and the counters; B ends it, A hides
 * the text. On its own (no bare_app descriptors) it runs -t seconds
 * without a picture and prints the counters. Everything goes to the log
 * (-o, default bench.log in the working directory), errors at once.
 *
 *   bare_app_bench [-t seconds] [-o log] [-lang ru] [-m MB per CPU]
 *                  [-cpus 0,1,2] [-shells N] [-scale F]
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>

#include "font.h"
#include "games/game_ipc_client.h"
#include "core/input.h"

#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif

#define MAX_CPUS 16
#define TEX_SIZE 2048
#define TEX_COUNT 6
#define STRIP_ROWS 32
#define STRIPS (TEX_SIZE / STRIP_ROWS)
#define ECHO 256		/* the texture check's square */
#define CHECK_S 0.25		/* between the GPU checks */
#define CHURN_S 20		/* a texture made again */
#define REFILL_S 10		/* the GPU's buffer written again by the CPU */
#define RES_ENTRIES 64
#define LAST_LEN 160

/* ---- settings -------------------------------------------------------------- */

static struct {
	int seconds;		/* on its own: how long */
	const char *log;
	bool ru;
	int mem_mb;		/* per CPU */
	int cpus[MAX_CPUS], n_cpus;
	int shells;
	float scale;		/* the scene's size over the frame's */
	int gpu_mb;		/* the GPU-read buffer */
	int out_mb;		/* the GPU-written buffer, per check */
	bool inject;		/* test of the detectors: bit 42 flipped on purpose now and then */
	const char *shot;	/* on its own: the last frame into this PPM file */
	const char *off;	/* parts left out, for tuning: upload,back,fur,check,read;
				 * gpu: no OpenGL at all (only the memory checks) */
	bool prof;		/* time the parts of a frame */
} cfg = { 600, "bench.log", false, 24, { 0 }, 0, 16, 1.25f, 32, 4, false, NULL, "", false };

static bool off(const char *part)
{
	return strstr(cfg.off, part) != NULL;
}

static atomic_bool quit;

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static double t_start;

/* ---- the log ----------------------------------------------------------------- */

static FILE *log_file;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static bool to_stdout;

static void logm(bool sync, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void logm(bool sync, const char *fmt, ...)
{
	char line[512];
	va_list ap;
	time_t t = time(NULL);
	struct tm tm;

	localtime_r(&t, &tm);
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	pthread_mutex_lock(&log_lock);
	if (log_file) {
		fprintf(log_file, "%02d:%02d:%02d [%8.1f] %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
			now_s() - t_start, line);
		fflush(log_file);
		if (sync)
			fsync(fileno(log_file));
	}
	if (to_stdout) {
		printf("[%8.1f] %s\n", now_s() - t_start, line);
		fflush(stdout);
	}
	pthread_mutex_unlock(&log_lock);
}

/* ---- counters ---------------------------------------------------------------- */

enum { D_MEM, D_GPU, D_CPU, D_TEX, D_COUNT };
static const char *const det_name[D_COUNT] = { "MEM", "GPU", "CPU", "TEX" };

struct det {
	atomic_ullong checks;	/* words (MEM, GPU, CPU) or texels (TEX) checked */
	atomic_ullong passes;	/* rounds */
	atomic_ullong errors;	/* wrong words / texels */
	atomic_ullong up[64], down[64];	/* bit went 0->1, 1->0 */
	atomic_ullong mem_right;	/* read again from memory: right (a read glitch) */
	atomic_ullong mem_wrong;	/* memory itself wrong */
};
static struct det det[D_COUNT];
static atomic_ullong cpu_errors[MAX_CPUS];

static struct {
	atomic_ullong gpu_fault, oops, warn;
} kern;

static pthread_mutex_t last_lock = PTHREAD_MUTEX_INITIALIZER;
static char last_error[LAST_LEN], last_kernel[LAST_LEN];

static void set_last(char *dst, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_last(char *dst, const char *fmt, ...)
{
	va_list ap;

	pthread_mutex_lock(&last_lock);
	va_start(ap, fmt);
	vsnprintf(dst, LAST_LEN, fmt, ap);
	va_end(ap);
	pthread_mutex_unlock(&last_lock);
}

static void count_bits(struct det *d, uint64_t expected, uint64_t got, int bits)
{
	uint64_t x = expected ^ got;

	for (int b = 0; b < bits; b++)
		if (x >> b & 1)
			atomic_fetch_add(got >> b & 1 ? &d->up[b] : &d->down[b], 1);
}

static uint64_t total_errors(void)
{
	uint64_t n = 0;

	for (int i = 0; i < D_COUNT; i++)
		n += det[i].errors;
	return n;
}

/* "42:+3/-1 10:+1" of a detector, the most flipped first */
static void bits_text(const struct det *d, int bits, char *buf, size_t size)
{
	bool used[64] = { 0 };
	size_t len = 0;

	buf[0] = 0;
	for (;;) {
		int best = -1;
		uint64_t bn = 0;

		for (int b = 0; b < bits; b++) {
			uint64_t n = d->up[b] + d->down[b];

			if (!used[b] && n > bn) {
				bn = n;
				best = b;
			}
		}
		if (best < 0 || len + 24 >= size)
			break;
		used[best] = true;
		len += snprintf(buf + len, size - len, "%s%d:+%llu/-%llu", len ? " " : "", best,
				(unsigned long long)d->up[best], (unsigned long long)d->down[best]);
	}
}

/* ---- hashes shared with the shaders ------------------------------------------- */

static inline uint32_t h32(uint32_t x)
{
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

/* Word i of the GPU's buffers: like a kernel pointer, the next one its
 * complement (as in the memory checks that saw bit 42) */
static inline uint64_t gpu_word(uint32_t i, uint32_t seed)
{
	uint32_t lo = h32(i * 2u ^ seed), hi = 0xffff0000u | (h32((i * 2u + 1u) ^ seed) & 0xffffu);

	if (i & 1u) {
		lo = ~lo;
		hi = ~hi;
	}
	return (uint64_t)hi << 32 | lo;
}

#define GLSL_HASH \
	"uint h32(uint x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu;\n" \
	"	x ^= x >> 16; return x; }\n" \
	"uvec2 word(uint i, uint seed) {\n" \
	"	uint lo = h32(i * 2u ^ seed), hi = 0xffff0000u | (h32((i * 2u + 1u) ^ seed) & 0xffffu);\n" \
	"	if ((i & 1u) != 0u) { lo = ~lo; hi = ~hi; }\n" \
	"	return uvec2(lo, hi); }\n"

/* Texel (x, y) of texture t, its strip made for the gen-th time */
static inline uint32_t strip_key(int t, uint32_t gen)
{
	return h32((uint32_t)t * 0x9e3779b9u ^ gen * 0x85ebca6bu);
}

static inline uint32_t texel(uint32_t key, int x, int y)
{
	return h32(key ^ (uint32_t)(y * TEX_SIZE + x));
}

/* ---- memory checks on the CPUs ---------------------------------------------- */

struct mc {
	int cpu;
	uint64_t *buf;
	size_t words;
	pthread_t th;
};
static struct mc mcs[MAX_CPUS];

static inline uint64_t mix64(uint64_t z)
{
	z += 0x9e3779b97f4a7c15ull;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
	return z ^ (z >> 31);
}

static inline uint64_t mc_word(uint64_t i, uint64_t seed, unsigned kind)
{
	uint64_t h = mix64(i ^ seed), v;

	switch (kind) {
	case 0:		/* kernel pointers and their complements */
		v = 0xffff000000000000ull | (h & 0x0000ffffffffffffull);
		return i & 1 ? ~v : v;
	case 1:		/* random */
		return h;
	default:	/* walking ones and zeros */
		v = 1ull << ((i + seed) & 63);
		return (i >> 6) & 1 ? ~v : v;
	}
}

static inline void flush_line(const volatile void *p)
{
#if defined(__aarch64__)
	__asm__ volatile("dc civac, %0\n\tdsb sy" : : "r"(p) : "memory");
#elif defined(__x86_64__) || defined(__i386__)
	__builtin_ia32_clflush((const void *)p);
	__builtin_ia32_mfence();
#else
	(void)p;
#endif
}

static uint64_t phys_of(const volatile void *p)
{
	long page = sysconf(_SC_PAGESIZE);
	uint64_t e = 0;
	int fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return 0;
	if (pread(fd, &e, sizeof(e), (off_t)((uintptr_t)p / page * 8)) != sizeof(e))
		e = 0;
	close(fd);
	if (!(e >> 63) || !(e & ((1ull << 55) - 1)))
		return 0;	/* not present, or no rights for the frame number */
	return (e & ((1ull << 55) - 1)) * page + (uintptr_t)p % page;
}

static __attribute__((noinline)) void mc_error(struct mc *m, volatile uint64_t *p, uint64_t expected,
					     uint64_t got)
{
	uint64_t again = *p, dram, later;
	struct timespec ms = { 0, 1000000 };
	struct det *d = &det[D_MEM];

	flush_line(p);
	dram = *p;
	nanosleep(&ms, NULL);
	flush_line(p);
	later = *p;
	atomic_fetch_add(&d->errors, 1);
	atomic_fetch_add(&cpu_errors[m->cpu % MAX_CPUS], 1);
	count_bits(d, expected, got, 64);
	atomic_fetch_add(dram == expected ? &d->mem_right : &d->mem_wrong, 1);
	logm(true, "ERROR MEM cpu%d phys %09llx expected %016llx got %016llx xor %016llx; "
	     "again %s, from DRAM %s, 1 ms later %s",
	     m->cpu, (unsigned long long)phys_of(p), (unsigned long long)expected,
	     (unsigned long long)got, (unsigned long long)(expected ^ got),
	     again == expected ? "right" : "WRONG", dram == expected ? "right" : "WRONG",
	     later == expected ? "right" : "WRONG");
	set_last(last_error, "MEM cpu%d: %016llx xor %016llx", m->cpu, (unsigned long long)expected,
		 (unsigned long long)(expected ^ got));
	*p = expected;
}

static void *mc_thread(void *arg)
{
	struct mc *m = arg;
	cpu_set_t set;
	uint64_t pass = 0;

	CPU_ZERO(&set);
	CPU_SET(m->cpu, &set);
	sched_setaffinity(0, sizeof(set), &set);
	setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19);
	while (!quit) {
		uint64_t seed = mix64(pass * 0x51ed27u + (uint64_t)m->cpu), *p = m->buf;
		unsigned kind = (unsigned)(pass % 3);
		size_t i, n = m->words;

		for (i = 0; i < n; i++)
			p[i] = mc_word(i, seed, kind);
		if (cfg.inject && m == &mcs[0] && pass % 64 == 7)
			p[n / 2] ^= 1ull << 42;
		for (i = 0; i < n && !quit;) {
			size_t end = i + 65536 < n ? i + 65536 : n;

			for (; i < end; i++) {
				uint64_t v = ((volatile uint64_t *)p)[i], e = mc_word(i, seed, kind);

				if (__builtin_expect(v != e, 0))
					mc_error(m, &p[i], e, v);
			}
		}
		atomic_fetch_add(&det[D_MEM].checks, i);
		atomic_fetch_add(&det[D_MEM].passes, 1);
		pass++;
	}
	return NULL;
}

/* ---- the kernel log ----------------------------------------------------------- */

static bool has(const char *s, const char *what)
{
	return strstr(s, what) != NULL;
}

static void *kmsg_thread(void *arg)
{
	int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	char rec[1024];

	(void)arg;
	if (fd < 0) {
		logm(false, "no kernel log: %s", strerror(errno));
		return NULL;
	}
	lseek(fd, 0, SEEK_END);
	while (!quit) {
		struct pollfd pfd = { fd, POLLIN, 0 };
		ssize_t n;
		char *msg;

		if (poll(&pfd, 1, 200) <= 0)
			continue;
		n = read(fd, rec, sizeof(rec) - 1);
		if (n <= 0)
			continue;	/* EPIPE: records lost, go on */
		rec[n] = 0;
		msg = strchr(rec, ';');
		msg = msg ? msg + 1 : rec;
		msg[strcspn(msg, "\n")] = 0;
		if (has(msg, "rk818-charger") || has(msg, "rk817"))
			continue;
		if (has(msg, "Unable to handle") || has(msg, "Internal error") || has(msg, "Oops") ||
		    has(msg, "BUG:") || has(msg, "Bad page") || has(msg, "stack-protector") ||
		    has(msg, "Kernel panic") || has(msg, "detected stalls") || has(msg, "Bad rss") ||
		    has(msg, "SError")) {
			atomic_fetch_add(&kern.oops, 1);
		} else if ((has(msg, "panfrost") || has(msg, ".gpu")) &&
			   (has(msg, "fault") || has(msg, "timeout") || has(msg, "timed out") ||
			    has(msg, "reset"))) {
			atomic_fetch_add(&kern.gpu_fault, 1);
		} else if (has(msg, "WARNING:")) {
			atomic_fetch_add(&kern.warn, 1);
		} else {
			continue;
		}
		set_last(last_kernel, "%.150s", msg);
		logm(true, "KERNEL %s", msg);
	}
	close(fd);
	return NULL;
}

/* ---- sensors ---------------------------------------------------------------------- */

static struct {
	char cpu_temp[128], ddr_temp[128], gpu_freq[160], cpu_freq[2][128];
	int cpu_c, ddr_c, gpu_mhz, cpu_mhz[2];
} sens;

static int read_int(const char *path)
{
	char buf[32];
	int fd = open(path, O_RDONLY | O_CLOEXEC), v = -1;
	ssize_t n;

	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n > 0) {
		buf[n] = 0;
		v = (int)(strtoll(buf, NULL, 10) / 1000);	/* m°C, kHz -> °C, MHz; Hz -> kHz */
	}
	return v;
}

static void sensors_find(void)
{
	char path[128], type[64];
	DIR *dir;
	struct dirent *e;

	for (int i = 0; i < 16; i++) {
		FILE *f;

		snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/type", i);
		if (!(f = fopen(path, "r")))
			break;
		if (fgets(type, sizeof(type), f)) {
			type[strcspn(type, "\n")] = 0;
			snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", i);
			if (!strcmp(type, "cpu-thermal") || (!sens.cpu_temp[0] && strstr(type, "cpu")))
				snprintf(sens.cpu_temp, sizeof(sens.cpu_temp), "%s", path);
			else if (strstr(type, "ddr"))
				snprintf(sens.ddr_temp, sizeof(sens.ddr_temp), "%s", path);
		}
		fclose(f);
	}
	if ((dir = opendir("/sys/class/devfreq"))) {
		while ((e = readdir(dir)))
			if (strstr(e->d_name, "gpu"))
				snprintf(sens.gpu_freq, sizeof(sens.gpu_freq),
					 "/sys/class/devfreq/%.64s/cur_freq", e->d_name);
		closedir(dir);
	}
	/* the first CPU's cluster and the last one's */
	snprintf(sens.cpu_freq[0], sizeof(sens.cpu_freq[0]),
		 "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq");
	snprintf(sens.cpu_freq[1], sizeof(sens.cpu_freq[1]),
		 "/sys/devices/system/cpu/cpu%ld/cpufreq/scaling_cur_freq", sysconf(_SC_NPROCESSORS_CONF) - 1);
}

static void sensors_read(void)
{
	int f;

	sens.cpu_c = sens.cpu_temp[0] ? read_int(sens.cpu_temp) : -1;
	sens.ddr_c = sens.ddr_temp[0] ? read_int(sens.ddr_temp) : -1;
	f = sens.gpu_freq[0] ? read_int(sens.gpu_freq) : -1;	/* Hz / 1000 */
	sens.gpu_mhz = f > 0 ? (f + 500) / 1000 : -1;
	for (int i = 0; i < 2; i++) {
		f = read_int(sens.cpu_freq[i]);			/* kHz / 1000 */
		sens.cpu_mhz[i] = f;
	}
}

/* ---- GL helpers --------------------------------------------------------------------- */

static GLuint shader(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	GLint ok = 0;

	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char msg[2048];

		glGetShaderInfoLog(s, sizeof(msg), NULL, msg);
		logm(true, "shader: %s", msg);
		fprintf(stderr, "bench: shader: %s\n%s\n", msg, src);
		exit(3);
	}
	return s;
}

static GLuint program(const char *vs, const char *fs)
{
	GLuint p = glCreateProgram();
	GLint ok = 0;

	glAttachShader(p, shader(GL_VERTEX_SHADER, vs));
	glAttachShader(p, shader(GL_FRAGMENT_SHADER, fs));
	glLinkProgram(p);
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		char msg[2048];

		glGetProgramInfoLog(p, sizeof(msg), NULL, msg);
		fprintf(stderr, "bench: link: %s\n", msg);
		exit(3);
	}
	return p;
}

static GLuint compute(const char *cs)
{
	GLuint p = glCreateProgram();
	GLint ok = 0;

	glAttachShader(p, shader(GL_COMPUTE_SHADER, cs));
	glLinkProgram(p);
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		char msg[2048];

		glGetProgramInfoLog(p, sizeof(msg), NULL, msg);
		fprintf(stderr, "bench: link: %s\n", msg);
		exit(3);
	}
	return p;
}

/* ---- shaders --------------------------------------------------------------------------- */

static const char *const fur_vs =
	"#version 300 es\n"
	"layout(location = 0) in vec3 pos;\n"
	"layout(location = 1) in vec3 nrm;\n"
	"layout(location = 2) in vec2 uv;\n"
	"uniform mat4 viewproj, model;\n"
	"uniform float shell, fur_len, time;\n"
	"uniform vec3 eye;\n"
	"out vec2 v_uv; out vec3 v_n; out vec3 v_view;\n"
	"void main() {\n"
	"	vec3 n = normalize(mat3(model) * nrm);\n"
	"	vec4 wp = model * vec4(pos, 1.0);\n"
	"	float s2 = shell * shell * fur_len;\n"
	"	wp.xyz += n * shell * fur_len + vec3(0.0, -0.6, 0.0) * s2\n"
	"		+ 0.2 * s2 * vec3(sin(time * 2.1 + pos.x * 4.0), 0.0, cos(time * 1.7 + pos.y * 4.0));\n"
	"	v_n = n; v_uv = uv; v_view = eye - wp.xyz;\n"
	"	gl_Position = viewproj * wp;\n"
	"}\n";

static const char *const fur_fs =
	"#version 300 es\n"
	"precision highp float;\n"
	"uniform sampler2D fur;\n"
	"uniform float shell, time;\n"
	"uniform vec3 light;\n"
	"in vec2 v_uv; in vec3 v_n; in vec3 v_view;\n"
	"out vec4 o;\n"
	"void main() {\n"
	"	vec4 f = texture(fur, v_uv);\n"
	"	if (shell > 0.0 && f.r < shell) discard;\n"
	"	vec3 n = normalize(v_n), v = normalize(v_view);\n"
	"	float st = 0.5 + 0.5 * sin(v_uv.x * 37.0 + 3.0 * sin(v_uv.y * 9.0 + time * 0.7));\n"
	"	vec3 base = mix(vec3(0.95, 0.50, 0.12), vec3(0.12, 0.06, 0.02), smoothstep(0.55, 0.68, st));\n"
	"	base *= mix(0.8, 1.2, f.g);\n"
	"	float ao = mix(0.22, 1.0, shell);\n"
	"	float diff = max(dot(n, light), 0.0) * 0.8 + 0.2;\n"
	"	vec3 t = normalize(cross(n, vec3(0.0, 0.0, 1.0)) + 1e-4);\n"
	"	float th = dot(t, normalize(light + v));\n"
	"	float spec = pow(sqrt(max(1.0 - th * th, 0.0)), 40.0) * 0.4 * shell;\n"
	"	float rim = pow(1.0 - max(dot(n, v), 0.0), 3.0) * 0.25;\n"
	"	o = vec4(base * diff * ao + vec3(spec + rim * shell), 1.0);\n"
	"}\n";

static const char *const quad_vs =
	"#version 300 es\n"
	"out vec2 uv;\n"
	"void main() {\n"
	"	vec2 p = vec2(float((gl_VertexID & 1) << 2) - 1.0, float((gl_VertexID & 2) << 1) - 1.0);\n"
	"	uv = p * 0.5 + 0.5;\n"
	"	gl_Position = vec4(p, 0.0, 1.0);\n"
	"}\n";

/* random texels of two big textures, 2 x 2 pixels sharing one */
static const char *const back_fs =
	"#version 300 es\n"
	"precision highp float; precision highp int;\n"
	"uniform highp sampler2D t0, t1;\n"
	"uniform uint frame;\n"
	"uniform int taps;\n"
	"in vec2 uv; out vec4 o;\n"
	"uint h32(uint x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu;\n"
	"	x ^= x >> 16; return x; }\n"
	"void main() {\n"
	"	uvec2 p = uvec2(gl_FragCoord.xy) >> 1u;\n"
	"	uint k = (p.y * 4096u + p.x) ^ (frame * 0x9e3779b9u);\n"
	"	vec4 acc = vec4(0.0);\n"
	"	for (int i = 0; i < taps; i++) {\n"
	"		uint a = h32(k + uint(i) * 0x68e31da4u), b = h32(a);\n"
	"		acc += texelFetch(t0, ivec2(a & 2047u, (a >> 11) & 2047u), 0)\n"
	"		     * texelFetch(t1, ivec2(b & 2047u, (b >> 11) & 2047u), 0);\n"
	"	}\n"
	"	vec3 bg = mix(vec3(0.015, 0.02, 0.06), vec3(0.05, 0.07, 0.16), uv.y);\n"
	"	o = vec4(bg + acc.rgb * (0.05 / float(taps)), 1.0);\n"
	"}\n";

/* the scene into the frame: upside down (glReadPixels reads the bottom row
 * first) and blue first (XRGB8888 in memory is B G R X) */
static const char *const out_fs =
	"#version 300 es\n"
	"precision mediump float;\n"
	"uniform sampler2D scene;\n"
	"in vec2 uv; out vec4 o;\n"
	"void main() {\n"
	"	vec3 c = texture(scene, vec2(uv.x, 1.0 - uv.y)).rgb;\n"
	"	float vig = 1.0 - 0.35 * dot(uv - 0.5, uv - 0.5);\n"
	"	o = vec4(c.bgr * vig, 1.0);\n"
	"}\n";

static const char *const echo_fs =
	"#version 300 es\n"
	"precision highp float;\n"
	"uniform highp sampler2D src;\n"
	"uniform ivec2 off;\n"
	"out vec4 o;\n"
	"void main() { o = texelFetch(src, ivec2(gl_FragCoord.xy) + off, 0); }\n";

static const char *const verify_cs =
	"#version 310 es\n"
	"layout(local_size_x = 256) in;\n"
	"layout(std430, binding = 0) readonly buffer Pat { uvec2 w[]; };\n"
	"layout(std430, binding = 1) buffer Res { uint count; uint n; uint pad0; uint pad1; uvec4 e[64]; };\n"
	"uniform uint seed;\n"
	"uniform uint words;\n"
	GLSL_HASH
	"void main() {\n"
	"	uint i = gl_GlobalInvocationID.x;\n"
	"	if (i >= words) return;\n"
	"	uvec2 g = w[i], x = word(i, seed);\n"
	"	if (g != x) {\n"
	"		atomicAdd(count, 1u);\n"
	"		uint s = atomicAdd(n, 1u);\n"
	"		if (s < 64u) e[s] = uvec4(i, g.x, g.y, 0u);\n"
	"	}\n"
	"}\n";

static const char *const write_cs =
	"#version 310 es\n"
	"layout(local_size_x = 256) in;\n"
	"layout(std430, binding = 2) writeonly buffer Out { uvec2 o[]; };\n"
	"uniform uint seed;\n"
	"uniform uint words;\n"
	GLSL_HASH
	"void main() {\n"
	"	uint i = gl_GlobalInvocationID.x;\n"
	"	if (i < words) o[i] = word(i, seed);\n"
	"}\n";

/* ---- matrices ----------------------------------------------------------------------------- */

typedef float mat4[16];	/* column-major */

static void mat_mul(mat4 r, const mat4 a, const mat4 b)
{
	mat4 t;

	for (int c = 0; c < 4; c++)
		for (int i = 0; i < 4; i++)
			t[c * 4 + i] = a[0 * 4 + i] * b[c * 4 + 0] + a[1 * 4 + i] * b[c * 4 + 1] +
				       a[2 * 4 + i] * b[c * 4 + 2] + a[3 * 4 + i] * b[c * 4 + 3];
	memcpy(r, t, sizeof(t));
}

static void mat_rot(mat4 r, float ax, float ay, float az, float x, float y, float z)
{
	float cx = cosf(ax), sx = sinf(ax), cy = cosf(ay), sy = sinf(ay), cz = cosf(az), sz = sinf(az);
	mat4 rx = { 1, 0, 0, 0, 0, cx, sx, 0, 0, -sx, cx, 0, 0, 0, 0, 1 };
	mat4 ry = { cy, 0, -sy, 0, 0, 1, 0, 0, sy, 0, cy, 0, 0, 0, 0, 1 };
	mat4 rz = { cz, sz, 0, 0, -sz, cz, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

	mat_mul(r, ry, rx);
	mat_mul(r, rz, r);
	r[12] = x;
	r[13] = y;
	r[14] = z;
}

static void mat_persp(mat4 r, float fovy, float aspect, float zn, float zf)
{
	float f = 1.0f / tanf(fovy / 2);

	memset(r, 0, sizeof(mat4));
	r[0] = f / aspect;
	r[5] = f;
	r[10] = (zf + zn) / (zn - zf);
	r[11] = -1;
	r[14] = 2 * zf * zn / (zn - zf);
}

/* ---- the GPU's work --------------------------------------------------------------------- */

static struct gpu {
	int w, h, sw, sh;	/* frame, scene */
	GLuint fur_prog, back_prog, out_prog, echo_prog, verify_prog, write_prog;
	GLuint vao, vbo, ibo, fur_tex;
	GLsizei n_idx;
	GLuint scene_fbo, scene_tex, scene_depth, out_fbo, out_tex, echo_fbo, echo_tex;
	GLuint tex[TEX_COUNT];
	uint32_t gen[TEX_COUNT][STRIPS];
	uint32_t gen_next;
	uint32_t *strip;	/* TEX_SIZE x STRIP_ROWS texels */
	int up_tex, up_strip;	/* the next strip to upload */
	GLuint pbo[2];
	bool compute;		/* OpenGL ES 3.1 */
	GLuint pat, res, out;
	uint32_t pat_seed, pat_words, out_words, out_seed;
	unsigned fills;
	uint32_t echo_px[ECHO * ECHO];
	uint32_t rng;
} g;

static uint32_t rnd(void)
{
	g.rng = g.rng * 1664525u + 1013904223u;
	return h32(g.rng);
}

static void make_torus(int seg_u, int seg_v)
{
	const float R = 1.0f, r = 0.42f;
	int nv = (seg_u + 1) * (seg_v + 1);
	float *v = malloc(sizeof(float) * 8 * (size_t)nv), *p = v;
	GLuint *idx = malloc(sizeof(GLuint) * 6 * (size_t)seg_u * seg_v), *q = idx;

	for (int i = 0; i <= seg_u; i++)
		for (int j = 0; j <= seg_v; j++) {
			float u = (float)i / seg_u * 2 * (float)M_PI, w = (float)j / seg_v * 2 * (float)M_PI;
			float cu = cosf(u), su = sinf(u), cw = cosf(w), sw = sinf(w);

			*p++ = (R + r * cw) * cu;
			*p++ = (R + r * cw) * su;
			*p++ = r * sw;
			*p++ = cw * cu;
			*p++ = cw * su;
			*p++ = sw;
			*p++ = (float)i / seg_u * 6;
			*p++ = (float)j / seg_v * 2;
		}
	for (int i = 0; i < seg_u; i++)
		for (int j = 0; j < seg_v; j++) {
			GLuint a = (GLuint)(i * (seg_v + 1) + j), b = a + (GLuint)seg_v + 1;

			*q++ = a; *q++ = b; *q++ = a + 1;
			*q++ = a + 1; *q++ = b; *q++ = b + 1;
		}
	g.n_idx = (GLsizei)(q - idx);
	glGenVertexArrays(1, &g.vao);
	glBindVertexArray(g.vao);
	glGenBuffers(1, &g.vbo);
	glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 8 * (size_t)nv, v, GL_STATIC_DRAW);
	glGenBuffers(1, &g.ibo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ibo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(GLuint) * (size_t)g.n_idx, idx, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 32, (void *)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 32, (void *)12);
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 32, (void *)24);
	glBindVertexArray(0);
	free(v);
	free(idx);
}

/* Strands: R how far up a strand goes, G its shade */
static void make_fur_texture(void)
{
	const int n = 512;
	uint32_t *px = malloc(sizeof(uint32_t) * n * n);

	for (int y = 0; y < n; y++)
		for (int x = 0; x < n; x++) {
			uint32_t h = h32((uint32_t)(y * n + x) * 0x27d4eb2du);
			uint32_t len = (h & 0xff) < 90 ? 0 : 120 + (h >> 8) % 136;	/* gaps between strands */

			px[y * n + x] = len | ((h >> 16) & 0xff) << 8 | 0xff000000u;
		}
	glGenTextures(1, &g.fur_tex);
	glBindTexture(GL_TEXTURE_2D, g.fur_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, n, n, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
	glGenerateMipmap(GL_TEXTURE_2D);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	free(px);
}

static void upload_strip(int t, int s)
{
	uint32_t key;

	g.gen[t][s] = ++g.gen_next;
	key = strip_key(t, g.gen[t][s]);
	for (int y = 0; y < STRIP_ROWS; y++)
		for (int x = 0; x < TEX_SIZE; x++)
			g.strip[y * TEX_SIZE + x] = texel(key, x, s * STRIP_ROWS + y);
	if (cfg.inject && g.gen_next % 61 == 0)
		g.strip[STRIP_ROWS / 2 * TEX_SIZE + TEX_SIZE / 2] ^= 1u << 10;
	glBindTexture(GL_TEXTURE_2D, g.tex[t]);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, s * STRIP_ROWS, TEX_SIZE, STRIP_ROWS, GL_RGBA,
			GL_UNSIGNED_BYTE, g.strip);
}

/* A big texture made again from nothing: a new buffer, all of it uploaded */
static void make_big_texture(int t)
{
	if (g.tex[t])
		glDeleteTextures(1, &g.tex[t]);
	glGenTextures(1, &g.tex[t]);
	glBindTexture(GL_TEXTURE_2D, g.tex[t]);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, TEX_SIZE, TEX_SIZE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	for (int s = 0; s < STRIPS; s++)
		upload_strip(t, s);
}

static GLuint color_target(GLuint *tex, int w, int h, bool linear)
{
	GLuint fbo;

	glGenTextures(1, tex);
	glBindTexture(GL_TEXTURE_2D, *tex);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
	return fbo;
}

static void fill_pattern(void)
{
	uint64_t *p;

	g.pat_seed = rnd();
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, g.pat);
	p = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)g.pat_words * 8,
			     GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
	if (!p) {
		logm(true, "pattern buffer: no mapping (0x%x)", glGetError());
		return;
	}
	for (uint32_t i = 0; i < g.pat_words; i++)
		p[i] = gpu_word(i, g.pat_seed);
	if (cfg.inject && g.fills % 3 == 0)
		p[g.pat_words / 3] ^= 1ull << 42;
	g.fills++;
	glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
}

static void gpu_init(int w, int h)
{
	GLint major = 0, minor = 0;

	g.w = w;
	g.h = h;
	g.sw = (int)(w * cfg.scale) & ~1;
	g.sh = (int)(h * cfg.scale) & ~1;
	g.rng = (uint32_t)time(NULL);
	glGetIntegerv(GL_MAJOR_VERSION, &major);
	glGetIntegerv(GL_MINOR_VERSION, &minor);
	g.compute = major > 3 || (major == 3 && minor >= 1);
	logm(false, "GL: %s, %s; scene %dx%d, frame %dx%d, %d shells, compute checks %s",
	     (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION), g.sw,
	     g.sh, w, h, cfg.shells, g.compute ? "on" : "off (no OpenGL ES 3.1)");

	g.fur_prog = program(fur_vs, fur_fs);
	g.back_prog = program(quad_vs, back_fs);
	g.out_prog = program(quad_vs, out_fs);
	g.echo_prog = program(quad_vs, echo_fs);
	make_torus(128, 64);
	make_fur_texture();

	g.strip = malloc(sizeof(uint32_t) * TEX_SIZE * STRIP_ROWS);
	for (int t = 0; t < TEX_COUNT; t++)
		make_big_texture(t);

	g.scene_fbo = color_target(&g.scene_tex, g.sw, g.sh, true);
	glGenRenderbuffers(1, &g.scene_depth);
	glBindRenderbuffer(GL_RENDERBUFFER, g.scene_depth);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, g.sw, g.sh);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g.scene_depth);
	g.out_fbo = color_target(&g.out_tex, w, h, false);
	g.echo_fbo = color_target(&g.echo_tex, ECHO, ECHO, false);
	glGenBuffers(2, g.pbo);
	for (int i = 0; i < 2; i++) {
		glBindBuffer(GL_PIXEL_PACK_BUFFER, g.pbo[i]);
		glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)w * h * 4, NULL, GL_STREAM_READ);
	}
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

	if (g.compute) {
		g.verify_prog = compute(verify_cs);
		g.write_prog = compute(write_cs);
		g.pat_words = (uint32_t)cfg.gpu_mb * (1u << 20) / 8;
		g.out_words = (uint32_t)cfg.out_mb * (1u << 20) / 8;
		glGenBuffers(1, &g.pat);
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, g.pat);
		glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)g.pat_words * 8, NULL, GL_DYNAMIC_DRAW);
		fill_pattern();
		glGenBuffers(1, &g.res);
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, g.res);
		glBufferData(GL_SHADER_STORAGE_BUFFER, 16 + 16 * RES_ENTRIES, NULL, GL_DYNAMIC_READ);
		glGenBuffers(1, &g.out);
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, g.out);
		glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)g.out_words * 8, NULL, GL_STREAM_READ);
	}
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
}

static void draw_scene(double t, uint32_t frame)
{
	mat4 proj, view, vp, model;
	float aspect = (float)g.sw / g.sh;
	float eye[3] = { 0, 0, 4.3f }, light[3] = { 0.4f, 0.7f, 0.6f };
	/* the donut's centre at 0.52 of the half width right of the middle:
	 * the text is on the left */
	float shift = 0.52f * eye[2] * tanf(0.45f) * aspect;
	float ll = sqrtf(light[0] * light[0] + light[1] * light[1] + light[2] * light[2]);

	glBindFramebuffer(GL_FRAMEBUFFER, g.scene_fbo);
	glViewport(0, 0, g.sw, g.sh);
	glDisable(GL_DEPTH_TEST);
	glClear(GL_DEPTH_BUFFER_BIT);

	/* the background: random texels of two big textures */
	glClearColor(0.02f, 0.03f, 0.08f, 1);
	if (off("back"))
		glClear(GL_COLOR_BUFFER_BIT);
	glUseProgram(g.back_prog);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, g.tex[frame % TEX_COUNT]);
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, g.tex[(frame + TEX_COUNT / 2) % TEX_COUNT]);
	glUniform1i(glGetUniformLocation(g.back_prog, "t0"), 1);
	glUniform1i(glGetUniformLocation(g.back_prog, "t1"), 2);
	glUniform1ui(glGetUniformLocation(g.back_prog, "frame"), frame);
	glUniform1i(glGetUniformLocation(g.back_prog, "taps"), 2);
	if (!off("back"))
		glDrawArrays(GL_TRIANGLES, 0, 3);

	/* the donut, its shells from the skin out */
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);
	mat_persp(proj, 0.9f, aspect, 0.1f, 20.0f);
	memset(view, 0, sizeof(view));
	view[0] = view[5] = view[10] = view[15] = 1;
	view[12] = shift;
	view[14] = -eye[2];
	mat_mul(vp, proj, view);
	mat_rot(model, (float)(t * 0.31) + 0.9f, (float)(t * 0.47), (float)(t * 0.13), 0, 0, 0);
	glUseProgram(g.fur_prog);
	glUniformMatrix4fv(glGetUniformLocation(g.fur_prog, "viewproj"), 1, GL_FALSE, vp);
	glUniformMatrix4fv(glGetUniformLocation(g.fur_prog, "model"), 1, GL_FALSE, model);
	glUniform1f(glGetUniformLocation(g.fur_prog, "fur_len"), 0.16f);
	glUniform1f(glGetUniformLocation(g.fur_prog, "time"), (float)t);
	glUniform3f(glGetUniformLocation(g.fur_prog, "eye"), -shift, 0, eye[2]);
	glUniform3f(glGetUniformLocation(g.fur_prog, "light"), light[0] / ll, light[1] / ll, light[2] / ll);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, g.fur_tex);
	glUniform1i(glGetUniformLocation(g.fur_prog, "fur"), 0);
	glBindVertexArray(g.vao);
	for (int s = 0; s < cfg.shells && !off("fur"); s++) {
		glUniform1f(glGetUniformLocation(g.fur_prog, "shell"), (float)s / cfg.shells);
		glDrawElements(GL_TRIANGLES, g.n_idx, GL_UNSIGNED_INT, 0);
	}
	glBindVertexArray(0);
	glDisable(GL_DEPTH_TEST);

	/* into the frame */
	glBindFramebuffer(GL_FRAMEBUFFER, g.out_fbo);
	glViewport(0, 0, g.w, g.h);
	glUseProgram(g.out_prog);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, g.scene_tex);
	glUniform1i(glGetUniformLocation(g.out_prog, "scene"), 0);
	glDrawArrays(GL_TRIANGLES, 0, 3);
}

/* GPU checks, waited for: the buffer the CPU wrote, a pattern for the CPU,
 * a square of a big texture */
static void gpu_checks(void)
{
	struct det *d;

	if (g.compute) {
		const uint32_t *r;
		uint32_t zero[4] = { 0 };
		const uint64_t *o;
		uint32_t groups;

		/* GPU reads what the CPU wrote */
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, g.res);
		glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(zero), zero);
		glUseProgram(g.verify_prog);
		glUniform1ui(glGetUniformLocation(g.verify_prog, "seed"), g.pat_seed);
		glUniform1ui(glGetUniformLocation(g.verify_prog, "words"), g.pat_words);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, g.pat);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, g.res);
		groups = (g.pat_words + 255) / 256;
		glDispatchCompute(groups, 1, 1);

		/* GPU writes for the CPU */
		g.out_seed = rnd();
		glUseProgram(g.write_prog);
		glUniform1ui(glGetUniformLocation(g.write_prog, "seed"), g.out_seed);
		glUniform1ui(glGetUniformLocation(g.write_prog, "words"), g.out_words);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, g.out);
		glDispatchCompute((g.out_words + 255) / 256, 1, 1);
		glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);

		d = &det[D_GPU];
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, g.res);
		r = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, 16 + 16 * RES_ENTRIES, GL_MAP_READ_BIT);
		if (r) {
			uint32_t count = r[0], n = r[1] < RES_ENTRIES ? r[1] : RES_ENTRIES;
			uint32_t idx[RES_ENTRIES];
			uint64_t got[RES_ENTRIES];

			for (uint32_t k = 0; k < n; k++) {
				idx[k] = r[4 + k * 4];
				got[k] = (uint64_t)r[4 + k * 4 + 2] << 32 | r[4 + k * 4 + 1];
			}
			glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
			atomic_fetch_add(&d->checks, g.pat_words);
			atomic_fetch_add(&d->passes, 1);
			if (count) {
				atomic_fetch_add(&d->errors, count);
				/* the buffer as the CPU sees it now: right means the GPU misread */
				glBindBuffer(GL_SHADER_STORAGE_BUFFER, g.pat);
				for (uint32_t k = 0; k < n; k++) {
					uint64_t e = gpu_word(idx[k], g.pat_seed), now = 0;
					const uint64_t *m = glMapBufferRange(GL_SHADER_STORAGE_BUFFER,
									     (GLintptr)idx[k] * 8, 8,
									     GL_MAP_READ_BIT);

					if (m) {
						now = *m;
						glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
					}
					count_bits(d, e, got[k], 64);
					atomic_fetch_add(now == e ? &d->mem_right : &d->mem_wrong, 1);
					logm(true, "ERROR GPU read word %u expected %016llx got %016llx xor %016llx;"
					     " in memory %s", idx[k], (unsigned long long)e,
					     (unsigned long long)got[k], (unsigned long long)(e ^ got[k]),
					     now == e ? "right" : "WRONG");
					set_last(last_error, "GPU: %016llx xor %016llx", (unsigned long long)e,
						 (unsigned long long)(e ^ got[k]));
				}
				if (count > n)
					logm(true, "ERROR GPU read: %u wrong words, %u shown", count, n);
				fill_pattern();	/* fresh, so one bad word is not counted again */
			}
		}

		d = &det[D_CPU];
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, g.out);
		o = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)g.out_words * 8, GL_MAP_READ_BIT);
		if (o) {
			uint32_t bad = 0;

			for (uint32_t i = 0; i < g.out_words; i++) {
				uint64_t v = ((const volatile uint64_t *)o)[i], e = gpu_word(i, g.out_seed);

				if (cfg.inject && i == 7 && det[D_CPU].passes % 32 == 5)
					e ^= 1ull << 42;	/* as if the GPU's word had come wrong */

				if (__builtin_expect(v != e, 0)) {
					uint64_t again;

					flush_line(&o[i]);
					again = ((const volatile uint64_t *)o)[i];
					bad++;
					count_bits(d, e, v, 64);
					atomic_fetch_add(again == e ? &d->mem_right : &d->mem_wrong, 1);
					if (bad <= 16)
						logm(true, "ERROR CPU read of GPU word %u expected %016llx got %016llx"
						     " xor %016llx; from DRAM %s", i, (unsigned long long)e,
						     (unsigned long long)v, (unsigned long long)(e ^ v),
						     again == e ? "right" : "WRONG");
					set_last(last_error, "CPU<-GPU: %016llx xor %016llx", (unsigned long long)e,
						 (unsigned long long)(e ^ v));
				}
			}
			glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
			atomic_fetch_add(&d->checks, g.out_words);
			atomic_fetch_add(&d->passes, 1);
			if (bad) {
				atomic_fetch_add(&d->errors, bad);
				if (bad > 16)
					logm(true, "ERROR CPU read of GPU: %u wrong words", bad);
			}
		}
	}

	/* a square of a big texture through the GPU */
	{
		int t = (int)(rnd() % TEX_COUNT);
		int ox = (int)(rnd() % (TEX_SIZE - ECHO)), oy = (int)(rnd() % (TEX_SIZE - ECHO));
		uint32_t bad = 0;

		d = &det[D_TEX];
		glBindFramebuffer(GL_FRAMEBUFFER, g.echo_fbo);
		glViewport(0, 0, ECHO, ECHO);
		glUseProgram(g.echo_prog);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, g.tex[t]);
		glUniform1i(glGetUniformLocation(g.echo_prog, "src"), 0);
		glUniform2i(glGetUniformLocation(g.echo_prog, "off"), ox, oy);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		glReadPixels(0, 0, ECHO, ECHO, GL_RGBA, GL_UNSIGNED_BYTE, g.echo_px);
		for (int y = 0; y < ECHO; y++) {
			int ty = oy + y;
			uint32_t key = strip_key(t, g.gen[t][ty / STRIP_ROWS]);

			for (int x = 0; x < ECHO; x++) {
				uint32_t e = texel(key, ox + x, ty), v = g.echo_px[y * ECHO + x];

				if (__builtin_expect(v != e, 0)) {
					bad++;
					count_bits(d, e, v, 32);
					if (bad <= 8)
						logm(true, "ERROR TEX texture %d texel %d,%d expected %08x got %08x"
						     " xor %08x", t, ox + x, ty, e, v, e ^ v);
					set_last(last_error, "TEX: %08x xor %08x", e, e ^ v);
				}
			}
		}
		atomic_fetch_add(&d->checks, ECHO * ECHO);
		atomic_fetch_add(&d->passes, 1);
		if (bad) {
			atomic_fetch_add(&d->errors, bad);
			if (bad > 8)
				logm(true, "ERROR TEX: %u wrong texels in texture %d at %d,%d", bad, t, ox, oy);
		}
	}
}

/* ---- text on the frame ------------------------------------------------------------------- */

static const struct bench_glyph *glyph(const struct bench_font *f, uint32_t cp)
{
	int lo = 0, hi = f->n - 1;

	while (lo <= hi) {
		int mid = (lo + hi) / 2;

		if (f->g[mid].cp == cp)
			return &f->g[mid];
		if (f->g[mid].cp < cp)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return cp != '?' ? glyph(f, '?') : NULL;
}

static uint32_t utf8_next(const char **s)
{
	const unsigned char *p = (const unsigned char *)*s;
	uint32_t c = *p++;

	if (c >= 0xc0 && c < 0xe0 && (*p & 0xc0) == 0x80) {
		c = (c & 0x1f) << 6 | (*p++ & 0x3f);
	} else if (c >= 0xe0 && c < 0xf0 && (p[0] & 0xc0) == 0x80 && (p[1] & 0xc0) == 0x80) {
		c = (c & 0x0f) << 12 | (p[0] & 0x3f) << 6 | (p[1] & 0x3f);
		p += 2;
	}
	*s = (const char *)p;
	return c;
}

struct canvas {
	uint8_t *px;
	int w, h, stride;
};

static void blend(uint8_t *d, uint32_t rgb, int a)
{
	d[0] = (uint8_t)(d[0] + (((int)(rgb & 0xff) - d[0]) * a) / 255);
	d[1] = (uint8_t)(d[1] + (((int)(rgb >> 8 & 0xff) - d[1]) * a) / 255);
	d[2] = (uint8_t)(d[2] + (((int)(rgb >> 16 & 0xff) - d[2]) * a) / 255);
}

/* x, y: the pen on the baseline; the width drawn */
static int text(struct canvas *c, const struct bench_font *f, int x, int y, uint32_t rgb, const char *s)
{
	int x0 = x;

	while (*s) {
		const struct bench_glyph *gl = glyph(f, utf8_next(&s));

		if (!gl)
			continue;
		for (int r = 0; r < gl->h; r++) {
			int py = y + gl->y + r;

			if (py < 0 || py >= c->h)
				continue;
			for (int k = 0; k < gl->w; k++) {
				int px = x + gl->x + k, a = f->pix[gl->pix + r * gl->w + k];

				if (a && px >= 0 && px < c->w)
					blend(c->px + py * c->stride + px * 4, rgb, a);
			}
		}
		x += gl->adv;
	}
	return x - x0;
}

static void shade(struct canvas *c, int x0, int y0, int x1, int y1, int a)
{
	for (int y = y0 < 0 ? 0 : y0; y < y1 && y < c->h; y++)
		for (int x = x0 < 0 ? 0 : x0; x < x1 && x < c->w; x++)
			blend(c->px + y * c->stride + x * 4, 0, a);
}

#define TR(en_s, ru_s) (cfg.ru ? (ru_s) : (en_s))
#define C_TEXT 0xe8e8e8
#define C_DIM 0x9aa3b0
#define C_OK 0x52d273
#define C_BAD 0xff4d4d
#define C_WARN 0xffc93c

static double fps;

static void overlay(struct canvas *c, double elapsed)
{
	char line[256], bits[200];
	const struct bench_font *ft = &font_text, *fb = &font_big;
	int lh = ft->ascent + ft->descent + 3, x = 14, y = 14 + fb->ascent;
	uint64_t errs = total_errors(), kbad = kern.oops + kern.gpu_fault;
	int e = (int)elapsed;

	shade(c, 0, 0, c->w * 54 / 100, c->h, 150);
	text(c, ft, x, y - fb->ascent + ft->ascent, C_DIM, TR("Stress test: GPU + CPU + memory",
							       "Стресс-тест: GPU + CPU + память"));
	y += lh;
	if (errs)
		snprintf(line, sizeof(line), TR("ERRORS: %llu", "ОШИБОК: %llu"), (unsigned long long)errs);
	else
		snprintf(line, sizeof(line), "%s", TR("NO ERRORS", "ОШИБОК НЕТ"));
	text(c, fb, x, y + 6, errs ? C_BAD : C_OK, line);
	y += fb->ascent + fb->descent - 2;

	snprintf(line, sizeof(line), TR("Time %d:%02d:%02d    %.1f fps", "Время %d:%02d:%02d    %.1f кадр/с"),
		 e / 3600, e / 60 % 60, e % 60, fps);
	text(c, ft, x, y += lh, C_TEXT, line);
	snprintf(line, sizeof(line), TR("GPU %d MHz   CPU %d/%d MHz", "GPU %d МГц   CPU %d/%d МГц"),
		 sens.gpu_mhz, sens.cpu_mhz[0], sens.cpu_mhz[1]);
	text(c, ft, x, y += lh, C_TEXT, line);
	snprintf(line, sizeof(line), TR("CPU %d °C   DDR %d °C", "CPU %d °C   DDR %d °C"), sens.cpu_c,
		 sens.ddr_c);
	text(c, ft, x, y += lh, sens.ddr_c >= 85 || sens.cpu_c >= 85 ? C_WARN : C_TEXT, line);
	y += lh / 3;

	snprintf(line, sizeof(line), TR("Memory (CPU %d × %d MB): %llu passes", "Память (CPU %d × %d МБ): %llu проходов"),
		 cfg.n_cpus, cfg.mem_mb, (unsigned long long)det[D_MEM].passes);
	text(c, ft, x, y += lh, C_TEXT, line);
	snprintf(line, sizeof(line), TR("   errors %llu   bit 42: %llu", "   ошибок %llu   бит 42: %llu"),
		 (unsigned long long)det[D_MEM].errors,
		 (unsigned long long)(det[D_MEM].up[42] + det[D_MEM].down[42]));
	text(c, ft, x, y += lh, det[D_MEM].errors ? C_BAD : C_DIM, line);
	if (g.compute) {
		snprintf(line, sizeof(line), TR("GPU reads CPU's: %llu, errors %llu", "GPU читает от CPU: %llu, ошибок %llu"),
			 (unsigned long long)det[D_GPU].passes, (unsigned long long)det[D_GPU].errors);
		text(c, ft, x, y += lh, det[D_GPU].errors ? C_BAD : C_TEXT, line);
		snprintf(line, sizeof(line), TR("CPU reads GPU's: %llu, errors %llu", "CPU читает от GPU: %llu, ошибок %llu"),
			 (unsigned long long)det[D_CPU].passes, (unsigned long long)det[D_CPU].errors);
		text(c, ft, x, y += lh, det[D_CPU].errors ? C_BAD : C_TEXT, line);
	}
	snprintf(line, sizeof(line), TR("Textures: %llu, errors %llu", "Текстуры: %llu, ошибок %llu"),
		 (unsigned long long)det[D_TEX].passes, (unsigned long long)det[D_TEX].errors);
	text(c, ft, x, y += lh, det[D_TEX].errors ? C_BAD : C_TEXT, line);
	snprintf(line, sizeof(line), TR("Kernel: GPU faults %llu, oops %llu", "Ядро: сбоев GPU %llu, oops %llu"),
		 (unsigned long long)kern.gpu_fault, (unsigned long long)kern.oops);
	text(c, ft, x, y += lh, kbad ? C_BAD : C_TEXT, line);

	if (errs) {
		struct det all = { 0 };

		for (int d = 0; d < D_TEX; d++)
			for (int b = 0; b < 64; b++) {
				all.up[b] += det[d].up[b];
				all.down[b] += det[d].down[b];
			}
		bits_text(&all, 64, bits, 60);
		if (bits[0]) {
			snprintf(line, sizeof(line), TR("Bits: %s", "Биты: %s"), bits);
			text(c, ft, x, y += lh, C_WARN, line);
		}
		pthread_mutex_lock(&last_lock);
		snprintf(line, sizeof(line), "%.60s", last_error);
		pthread_mutex_unlock(&last_lock);
		text(c, ft, x, y += lh, C_DIM, line);
	}
	if (y + lh <= c->h - 12)	/* a small frame: the lines go first */
		text(c, ft, x, c->h - 12, C_DIM, TR("B  quit    A  hide the text", "B  выход    A  скрыть текст"));
}

/* ---- main ---------------------------------------------------------------------------------- */

static void on_signal(int sig)
{
	(void)sig;
	quit = true;
}

static void parse_cpus(const char *s)
{
	cfg.n_cpus = 0;
	while (*s && cfg.n_cpus < MAX_CPUS) {
		char *end;
		long c = strtol(s, &end, 10);

		if (end == s)
			break;
		cfg.cpus[cfg.n_cpus++] = (int)c;
		s = *end == ',' ? end + 1 : end;
	}
}

static void summary(const char *what, double elapsed)
{
	char bits[400];

	logm(true, "%s after %.0f s: %llu errors; kernel: %llu GPU faults, %llu oopses, %llu warnings; "
	     "CPU %d °C, DDR %d °C, GPU %d MHz, %.1f fps", what, elapsed,
	     (unsigned long long)total_errors(), (unsigned long long)kern.gpu_fault,
	     (unsigned long long)kern.oops, (unsigned long long)kern.warn, sens.cpu_c, sens.ddr_c,
	     sens.gpu_mhz, fps);
	for (int d = 0; d < D_COUNT; d++) {
		bits_text(&det[d], d == D_TEX ? 32 : 64, bits, sizeof(bits));
		logm(true, "  %s: %llu passes, %llu %s checked, %llu errors (memory right %llu, wrong %llu)%s%s",
		     det_name[d], (unsigned long long)det[d].passes, (unsigned long long)det[d].checks,
		     d == D_TEX ? "texels" : "words", (unsigned long long)det[d].errors,
		     (unsigned long long)det[d].mem_right, (unsigned long long)det[d].mem_wrong,
		     bits[0] ? "; bits " : "", bits);
	}
	if (det[D_MEM].errors) {
		char per[200];
		size_t len = 0;

		per[0] = 0;
		for (int i = 0; i < cfg.n_cpus; i++)
			len += snprintf(per + len, sizeof(per) - len, " cpu%d:%llu", cfg.cpus[i],
					(unsigned long long)cpu_errors[cfg.cpus[i] % MAX_CPUS]);
		logm(true, "  MEM errors by CPU:%s", per);
	}
}

int main(int argc, char **argv)
{
	struct game_link link;
	bool linked, show_text = true;
	EGLDisplay dpy = EGL_NO_DISPLAY;
	EGLContext ctx = EGL_NO_CONTEXT;
	EGLConfig conf;
	EGLint n_conf = 0;
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	pthread_t kth;
	int w = 854, h = 480;
	uint8_t *own_frame = NULL;
	double last_s, last_min, last_churn, last_refill, last_check, fps_t;
	uint32_t frame = 0, fps_frames = 0;
	int churn_tex = 0;
	long ncpu = sysconf(_SC_NPROCESSORS_ONLN);

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;

		if (!strcmp(a, "-t") && v)
			cfg.seconds = atoi(argv[++i]);
		else if (!strcmp(a, "-o") && v)
			cfg.log = argv[++i];
		else if (!strcmp(a, "-lang") && v)
			cfg.ru = !strncmp(argv[++i], "ru", 2);
		else if (!strcmp(a, "-m") && v)
			cfg.mem_mb = atoi(argv[++i]);
		else if (!strcmp(a, "-cpus") && v)
			parse_cpus(argv[++i]);
		else if (!strcmp(a, "-shells") && v)
			cfg.shells = atoi(argv[++i]);
		else if (!strcmp(a, "-scale") && v)
			cfg.scale = (float)atof(argv[++i]);
		else if (!strcmp(a, "-off") && v)
			cfg.off = argv[++i];
		else if (!strcmp(a, "-prof"))
			cfg.prof = true;
		else if (!strcmp(a, "-inject"))
			cfg.inject = true;
		else if (!strcmp(a, "-shot") && v)
			cfg.shot = argv[++i];
		else {
			fprintf(stderr, "usage: %s [-t s] [-o log] [-lang ru] [-m MB] [-cpus 0,1] "
				"[-shells N] [-scale F]\n", argv[0]);
			return 2;
		}
	}
	if (!cfg.n_cpus)
		for (long c = 0; c < ncpu && cfg.n_cpus < MAX_CPUS; c++)
			cfg.cpus[cfg.n_cpus++] = (int)c;
	if (cfg.shells < 1)
		cfg.shells = 1;
	if (cfg.mem_mb < 0)
		cfg.mem_mb = 0;

	t_start = now_s();
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGPIPE, SIG_IGN);
	linked = game_link_open(&link, false) == 0;
	to_stdout = !linked;
	if (linked) {
		w = (int)link.shm->width;
		h = (int)link.shm->height;
	} else {
		own_frame = malloc((size_t)w * h * 4);
	}
	log_file = fopen(cfg.log, "a");
	if (!log_file)
		fprintf(stderr, "bench: %s: %s\n", cfg.log, strerror(errno));
	{
		time_t t = time(NULL);
		char when[64];

		strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", localtime(&t));
		logm(true, "==== start %s: %s, memory %d × %d MB, %d shells%s", when,
		     linked ? "under bare_app" : "on its own", cfg.n_cpus, cfg.mem_mb, cfg.shells,
		     cfg.inject ? ", ERRORS INJECTED (test)" : "");
	}
	sensors_find();
	sensors_read();

	/* OpenGL ES without a window: Mesa's surfaceless platform, frames read back */
	if (off("gpu")) {
		logm(true, "no GPU load (-off gpu): memory checks only");
		goto checkers;
	}
	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL)
			  : EGL_NO_DISPLAY;
	if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API)) {
		logm(true, "no EGL display (0x%x)", eglGetError());
		return 3;
	}
	{
		const EGLint ca[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE,
				      EGL_PBUFFER_BIT, EGL_NONE };
		const EGLint v31[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE };
		const EGLint v30[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE };

		if (!eglChooseConfig(dpy, ca, &conf, 1, &n_conf) || n_conf < 1) {
			logm(true, "no EGL config (0x%x)", eglGetError());
			return 3;
		}
		ctx = eglCreateContext(dpy, conf, EGL_NO_CONTEXT, v31);
		if (ctx == EGL_NO_CONTEXT)
			ctx = eglCreateContext(dpy, conf, EGL_NO_CONTEXT, v30);
		if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
			logm(true, "no OpenGL ES 3 context (0x%x)", eglGetError());
			return 3;
		}
	}
	gpu_init(w, h);

checkers:
	/* the memory checkers, then the kernel log */
	for (int i = 0; i < cfg.n_cpus && cfg.mem_mb > 0; i++) {
		struct mc *m = &mcs[i];
		size_t bytes = (size_t)cfg.mem_mb << 20;

		m->cpu = cfg.cpus[i];
		m->words = bytes / 8;
		m->buf = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
			      -1, 0);
		if (m->buf == MAP_FAILED) {
			logm(true, "memory for cpu%d: %s", m->cpu, strerror(errno));
			m->buf = NULL;
			continue;
		}
		pthread_create(&m->th, NULL, mc_thread, m);
	}
	pthread_create(&kth, NULL, kmsg_thread, NULL);

	last_s = last_min = last_churn = last_refill = last_check = fps_t = now_s();
	while (!quit) {
		double t = now_s(), elapsed = t - t_start;
		struct game_msg msg;
		int r;

		while (linked && (r = game_link_recv(&msg)) != 0) {
			if (r < 0) {
				quit = true;	/* bare_app is gone */
				break;
			}
			if (msg.type == GAME_MSG_BUTTON && msg.pressed && msg.code == GP_B)
				quit = true;
			else if (msg.type == GAME_MSG_BUTTON && msg.pressed && msg.code == GP_A)
				show_text = !show_text;
		}
		if (quit || (!linked && elapsed >= cfg.seconds))
			break;
		if (off("gpu")) {
			struct timespec ts = { 0, 100000000 };

			nanosleep(&ts, NULL);
			t = now_s();
			if (t - fps_t >= 1.0) {
				sensors_read();
				fps_t = t;
			}
			if (t - last_min >= 60 || (!linked && t - last_s >= 10)) {
				logm(t - last_min >= 60, "CPU %d °C DDR %d °C GPU %d MHz; errors MEM %llu; kernel GPU "
				     "faults %llu oops %llu; passes MEM %llu", sens.cpu_c, sens.ddr_c, sens.gpu_mhz,
				     (unsigned long long)det[D_MEM].errors, (unsigned long long)kern.gpu_fault,
				     (unsigned long long)kern.oops, (unsigned long long)det[D_MEM].passes);
				if (t - last_min >= 60)
					last_min = t;
				last_s = t;
			}
			continue;
		}

		/* a strip of a big texture new every frame, a whole texture now and then */
		double p0 = now_s(), p1, p2, p3, p4;

		if (!off("upload"))
			upload_strip(g.up_tex, g.up_strip);
		if (++g.up_strip == STRIPS) {
			g.up_strip = 0;
			g.up_tex = (g.up_tex + 1) % TEX_COUNT;
		}
		if (t - last_churn >= CHURN_S) {
			make_big_texture(churn_tex);
			churn_tex = (churn_tex + 1) % TEX_COUNT;
			last_churn = t;
		}
		if (g.compute && t - last_refill >= REFILL_S) {
			fill_pattern();
			last_refill = t;
		}

		p1 = now_s();
		draw_scene(elapsed, frame);
		if (!off("read")) {
			glBindBuffer(GL_PIXEL_PACK_BUFFER, g.pbo[frame & 1]);
			glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, 0);
			glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		}
		if (cfg.prof)
			glFinish();
		p2 = now_s();
		if (t - last_check >= CHECK_S && !off("check")) {
			gpu_checks();
			last_check = t;
		}
		p3 = now_s();

		/* the frame before this one, read back by now */
		if (frame > 0 && !off("read")) {
			const uint8_t *px;
			struct canvas c = { linked ? game_link_frame(&link) : own_frame, w, h,
					    linked ? (int)link.shm->stride : w * 4 };

			glBindBuffer(GL_PIXEL_PACK_BUFFER, g.pbo[(frame - 1) & 1]);
			px = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)w * h * 4, GL_MAP_READ_BIT);
			if (px) {
				for (int y = 0; y < h; y++)
					memcpy(c.px + (size_t)y * c.stride, px + (size_t)y * w * 4, (size_t)w * 4);
				glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
			}
			glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
			if (show_text)
				overlay(&c, elapsed);
			if (linked && game_link_publish(&link) < 0)
				quit = true;
		}
		p4 = now_s();
		if (cfg.prof && frame % 30 == 0)
			logm(false, "ms: upload %.1f, draw+read %.1f, checks %.1f, copy+text %.1f",
			     (p1 - p0) * 1e3, (p2 - p1) * 1e3, (p3 - p2) * 1e3, (p4 - p3) * 1e3);
		frame++;
		fps_frames++;

		t = now_s();
		if (t - fps_t >= 1.0) {
			fps = fps_frames / (t - fps_t);
			fps_frames = 0;
			fps_t = t;
			sensors_read();
		}
		if (t - last_min >= 60 || (!linked && t - last_s >= 10)) {
			char bits[200];
			struct det all = { 0 };

			for (int d = 0; d < D_TEX; d++)
				for (int b = 0; b < 64; b++) {
					all.up[b] += det[d].up[b];
					all.down[b] += det[d].down[b];
				}
			bits_text(&all, 64, bits, sizeof(bits));
			logm(t - last_min >= 60, "%.1f fps; CPU %d °C DDR %d °C GPU %d MHz; errors MEM %llu "
			     "GPU %llu CPU %llu TEX %llu; kernel GPU faults %llu oops %llu; passes MEM %llu "
			     "GPU %llu TEX %llu%s%s", fps, sens.cpu_c, sens.ddr_c, sens.gpu_mhz,
			     (unsigned long long)det[D_MEM].errors, (unsigned long long)det[D_GPU].errors,
			     (unsigned long long)det[D_CPU].errors, (unsigned long long)det[D_TEX].errors,
			     (unsigned long long)kern.gpu_fault, (unsigned long long)kern.oops,
			     (unsigned long long)det[D_MEM].passes, (unsigned long long)det[D_GPU].passes,
			     (unsigned long long)det[D_TEX].passes, bits[0] ? "; bits " : "", bits);
			if (t - last_min >= 60)
				last_min = t;
			last_s = t;
		}
	}

	quit = true;
	for (int i = 0; i < cfg.n_cpus; i++)
		if (mcs[i].buf)
			pthread_join(mcs[i].th, NULL);
	pthread_join(kth, NULL);
	summary("==== end", now_s() - t_start);
	if (cfg.shot && own_frame) {
		FILE *f = fopen(cfg.shot, "wb");

		if (f) {
			fprintf(f, "P6\n%d %d\n255\n", w, h);
			for (int i = 0; i < w * h; i++) {
				const uint8_t *p = own_frame + (size_t)i * 4;

				fputc(p[2], f);
				fputc(p[1], f);
				fputc(p[0], f);
			}
			fclose(f);
		}
	}
	if (ctx != EGL_NO_CONTEXT) {
		eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		eglDestroyContext(dpy, ctx);
		eglTerminate(dpy);
	}
	if (log_file)
		fclose(log_file);
	return 0;
}
