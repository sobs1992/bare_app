#define _GNU_SOURCE
#include "game_proc.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "core/log.h"
#include "game_ipc.h"

#define TAG "game"
#define STOP_WAIT_MS 1000

extern char **environ;

struct game_proc {
	pid_t pid;
	int sock;
	struct game_shm *shm;
	size_t shm_size;
	unsigned front;		/* buffer the view thread reads */
	struct hal_fb fb;
	bool ended;
	bool paused;
	uint32_t held;		/* GP_BIT()s sent as pressed */
	uint32_t axes;		/* bits of the axes sent off centre */
};

int game_proc_mkdir(const char *dir, char *buf, size_t size)
{
	const char *home = getenv("HOME");
	char *p;

	if (!strncmp(dir, "~/", 2))
		snprintf(buf, size, "%s/%s", home && *home ? home : "/tmp", dir + 2);
	else
		snprintf(buf, size, "%s", dir);
	for (p = buf + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(buf, 0755) < 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	return mkdir(buf, 0755) < 0 && errno != EEXIST ? -1 : 0;
}

int game_proc_sibling(const char *name, char *buf, size_t size)
{
	char self[PATH_MAX], *slash;
	ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);

	if (n < 0)
		return -1;
	self[n] = '\0';
	slash = strrchr(self, '/');
	if (slash)
		*slash = '\0';
	return snprintf(buf, size, "%s/%s", self, name) < (int)size ? 0 : -1;
}

/* Our environment with s->env added (an entry there replaces ours) */
static char **make_env(const char *const *add)
{
	size_t n = 0, k = 0, i, j;
	char **env;

	while (environ[n])
		n++;
	while (add && add[k])
		k++;
	env = calloc(n + k + 1, sizeof(*env));
	if (!env)
		return NULL;
	for (i = 0, n = 0; environ[i]; i++) {
		size_t len = strcspn(environ[i], "=");

		for (j = 0; j < k; j++)
			if (!strncmp(add[j], environ[i], len) && add[j][len] == '=')
				break;
		if (j == k)
			env[n++] = environ[i];
	}
	for (j = 0; j < k; j++)
		env[n++] = (char *)add[j];
	return env;
}

static int spawn(struct game_proc *g, const struct game_spawn *s, int sock, int shm)
{
	posix_spawn_file_actions_t fa;
	posix_spawnattr_t attr;
	sigset_t none, all;
	char **env = make_env(s->env);
	int err;

	if (!env)
		return -1;
	/* signals are blocked in all our threads (signalfd): not in the game */
	sigemptyset(&none);
	sigfillset(&all);
	posix_spawnattr_init(&attr);
	posix_spawnattr_setsigmask(&attr, &none);
	posix_spawnattr_setsigdefault(&attr, &all);
	posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, sock, GAME_IPC_SOCK);
	posix_spawn_file_actions_adddup2(&fa, shm, GAME_IPC_SHM);
	posix_spawn_file_actions_addchdir_np(&fa, s->dir);
	err = posix_spawn(&g->pid, s->path, &fa, &attr, s->argv, env);
	posix_spawn_file_actions_destroy(&fa);
	posix_spawnattr_destroy(&attr);
	free(env);
	if (err) {
		LOGE(TAG, "%s: %s", s->path, strerror(err));
		errno = err;
		return -1;
	}
	LOGI(TAG, "started %s (pid %d, in %s)", s->path, (int)g->pid, s->dir);
	return 0;
}

struct game_proc *game_proc_start(const struct game_spawn *s)
{
	struct game_proc *g;
	int sv[2] = { -1, -1 }, mem = -1, child_sock = -1, child_mem = -1, err;
	struct game_shm hdr = { .width = s->width, .height = s->height, .stride = s->width * 4 };

	g = calloc(1, sizeof(*g));
	if (!g)
		return NULL;
	g->sock = -1;
	g->shm = MAP_FAILED;
	g->shm_size = game_shm_size(&hdr);
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv) < 0 ||
	    (mem = memfd_create("game-frames", MFD_CLOEXEC)) < 0 ||
	    ftruncate(mem, (off_t)g->shm_size) < 0)
		goto fail;
	g->shm = mmap(NULL, g->shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, mem, 0);
	if (g->shm == MAP_FAILED)
		goto fail;
	*g->shm = hdr;
	atomic_store(&g->shm->middle, 1);	/* the game starts with 0, we hold 2 */
	g->shm->magic = GAME_SHM_MAGIC;
	g->front = 2;
	g->fb = (struct hal_fb){ .width = s->width, .height = s->height, .stride = hdr.stride,
				 .format = HAL_FMT_XRGB8888 };
	/* above the target numbers, so that dup2() always makes a copy */
	child_sock = fcntl(sv[1], F_DUPFD_CLOEXEC, 10);
	child_mem = fcntl(mem, F_DUPFD_CLOEXEC, 10);
	if (child_sock < 0 || child_mem < 0 || spawn(g, s, child_sock, child_mem) < 0)
		goto fail;
	g->sock = sv[0];
	close(sv[1]);
	close(mem);
	close(child_sock);
	close(child_mem);
	return g;

fail:
	err = errno;
	if (g->shm != MAP_FAILED)
		munmap(g->shm, g->shm_size);
	if (sv[0] >= 0)
		close(sv[0]);
	if (sv[1] >= 0)
		close(sv[1]);
	if (mem >= 0)
		close(mem);
	if (child_sock >= 0)
		close(child_sock);
	if (child_mem >= 0)
		close(child_mem);
	free(g);
	errno = err;
	return NULL;
}

int game_proc_fd(struct game_proc *g)
{
	return g->sock;
}

const struct hal_fb *game_proc_frame(struct game_proc *g, bool *ended)
{
	struct game_msg m;
	ssize_t n;

	while (!g->ended) {
		n = recv(g->sock, &m, sizeof(m), 0);
		if (n > 0)
			continue;	/* only "frame published": the newest one is taken below */
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && errno == EAGAIN)
			break;
		g->ended = true;	/* EOF: the game process is gone */
	}
	*ended = g->ended;
	if (!(atomic_load(&g->shm->middle) & GAME_SHM_FRESH))
		return NULL;
	g->front = atomic_exchange(&g->shm->middle, g->front) & ~GAME_SHM_FRESH;
	g->fb.pixels = game_shm_frame(g->shm, g->front);
	return &g->fb;
}

static void send_msg(struct game_proc *g, const struct game_msg *m)
{
	if (send(g->sock, m, sizeof(*m), MSG_DONTWAIT | MSG_NOSIGNAL) < 0) {
		LOGD(TAG, "input %d/%d lost: %s", m->type, m->code, strerror(errno));
		return;
	}
	if (m->type == GAME_MSG_BUTTON && m->code < 32)
		g->held = m->pressed ? g->held | GP_BIT(m->code) : g->held & ~GP_BIT(m->code);
	else if (m->type == GAME_MSG_AXIS && m->code < 32)
		g->axes = m->value ? g->axes | 1u << m->code : g->axes & ~(1u << m->code);
}

void game_proc_input(struct game_proc *g, const struct gp_event *ev)
{
	struct game_msg m = { .code = (uint8_t)ev->code };

	if (g->ended || g->paused)
		return;
	if (ev->type == GP_EV_BUTTON) {
		if (ev->repeat)
			return;
		m.type = GAME_MSG_BUTTON;
		m.pressed = ev->pressed;
		m.flags = ev->stick ? GAME_BTN_STICK : 0;
	} else {
		m.type = GAME_MSG_AXIS;
		m.value = (int16_t)lrintf(ev->value * GAME_AXIS_MAX);
	}
	send_msg(g, &m);
}

void game_proc_pause(struct game_proc *g)
{
	unsigned i;

	if (g->ended || g->paused)
		return;
	/* the game reads them when it goes on: nothing stays held meanwhile */
	for (i = 0; i < 32; i++) {
		if (g->held & GP_BIT(i))
			send_msg(g, &(struct game_msg){ .type = GAME_MSG_BUTTON, .code = i });
		if (g->axes & 1u << i)
			send_msg(g, &(struct game_msg){ .type = GAME_MSG_AXIS, .code = i });
	}
	kill(g->pid, SIGSTOP);
	g->paused = true;
	LOGI(TAG, "paused (pid %d)", (int)g->pid);
}

void game_proc_resume(struct game_proc *g)
{
	if (!g->paused)
		return;
	kill(g->pid, SIGCONT);
	g->paused = false;
	LOGI(TAG, "resumed (pid %d)", (int)g->pid);
}

/* waitpid() with a deadline; 0 if the process is still there */
static pid_t wait_for(pid_t pid, int *status, int ms)
{
	struct timespec tick = { 0, 10 * 1000000L };
	pid_t r;

	for (;;) {
		r = waitpid(pid, status, WNOHANG);
		if (r != 0 || ms <= 0)
			return r;
		nanosleep(&tick, NULL);
		ms -= 10;
	}
}

int game_proc_stop(struct game_proc *g)
{
	int status = 0;

	if (!g)
		return 0;
	if (!g->ended)
		kill(g->pid, SIGTERM);
	if (g->paused)
		kill(g->pid, SIGCONT);	/* a stopped process gets SIGTERM only then */
	if (wait_for(g->pid, &status, STOP_WAIT_MS) == 0) {
		LOGW(TAG, "the game does not stop, killing it");
		kill(g->pid, SIGKILL);
		waitpid(g->pid, &status, 0);
	}
	if (WIFEXITED(status))
		LOGI(TAG, "game ended, exit status %d", WEXITSTATUS(status));
	else if (WIFSIGNALED(status))
		LOGI(TAG, "game ended by signal %d", WTERMSIG(status));
	close(g->sock);
	munmap(g->shm, g->shm_size);
	free(g);
	return status;
}
