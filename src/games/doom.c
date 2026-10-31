#define _GNU_SOURCE
#include "doom.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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
#include "doom_ipc.h"

#define TAG "doom"
#define GAME "bare_app_doom"
#define STOP_WAIT_MS 1000

extern char **environ;

struct doom {
	pid_t pid;
	int sock;
	struct doom_shm *shm;
	size_t shm_size;
	unsigned front;		/* buffer the view thread reads */
	struct hal_fb fb;
	bool ended;
};

const char *doom_wad(void)
{
	const char *env = getenv("BARE_APP_DOOM_WAD");

	return env && *env ? env : CONFIG_GAME_DOOM_WAD;
}

/* CONFIG_GAME_DOOM_DIR with "~/" expanded, created if missing */
static int data_dir(char *buf, size_t size)
{
	const char *dir = CONFIG_GAME_DOOM_DIR, *home = getenv("HOME");
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

/* The game sits next to our own binary (build dir, /usr/bin) */
static int game_path(char *buf, size_t size)
{
	char self[PATH_MAX], *slash;
	ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);

	if (n < 0)
		return -1;
	self[n] = '\0';
	slash = strrchr(self, '/');
	if (slash)
		*slash = '\0';
	return snprintf(buf, size, "%s/" GAME, self) < (int)size ? 0 : -1;
}

static int spawn(struct doom *d, const char *game, const char *dir, int sock, int shm)
{
	char *const argv[] = { (char *)GAME, (char *)"-iwad", (char *)doom_wad(), NULL };
	posix_spawn_file_actions_t fa;
	posix_spawnattr_t attr;
	sigset_t none, all;
	int err;

	/* signals are blocked in all our threads (signalfd): not in the game */
	sigemptyset(&none);
	sigfillset(&all);
	posix_spawnattr_init(&attr);
	posix_spawnattr_setsigmask(&attr, &none);
	posix_spawnattr_setsigdefault(&attr, &all);
	posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, sock, DOOM_IPC_SOCK);
	posix_spawn_file_actions_adddup2(&fa, shm, DOOM_IPC_SHM);
	posix_spawn_file_actions_addchdir_np(&fa, dir);	/* config and saves go to "." */
	err = posix_spawn(&d->pid, game, &fa, &attr, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	posix_spawnattr_destroy(&attr);
	if (err) {
		LOGE(TAG, "%s: %s", game, strerror(err));
		errno = err;
		return -1;
	}
	LOGI(TAG, "started %s -iwad %s (pid %d, in %s)", game, doom_wad(), (int)d->pid, dir);
	return 0;
}

struct doom *doom_start(int width, int height)
{
	struct doom *d;
	char game[PATH_MAX], dir[PATH_MAX];
	int sv[2] = { -1, -1 }, mem = -1, child_sock = -1, child_mem = -1, err;
	struct doom_shm hdr = { .width = width, .height = height, .stride = width * 4 };

	if (access(doom_wad(), R_OK) < 0) {
		LOGE(TAG, "IWAD %s: %s", doom_wad(), strerror(errno));
		errno = ENOENT;
		return NULL;
	}
	if (game_path(game, sizeof(game)) < 0 || access(game, X_OK) < 0) {
		LOGE(TAG, "no %s next to this program", GAME);
		errno = ENOENT;
		return NULL;
	}
	if (data_dir(dir, sizeof(dir)) < 0) {
		LOGE(TAG, "%s: %s", dir, strerror(errno));
		return NULL;
	}
	d = calloc(1, sizeof(*d));
	if (!d)
		return NULL;
	d->sock = -1;
	d->shm = MAP_FAILED;
	d->shm_size = doom_shm_size(&hdr);
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv) < 0 ||
	    (mem = memfd_create("doom-frames", MFD_CLOEXEC)) < 0 ||
	    ftruncate(mem, (off_t)d->shm_size) < 0)
		goto fail;
	d->shm = mmap(NULL, d->shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, mem, 0);
	if (d->shm == MAP_FAILED)
		goto fail;
	*d->shm = hdr;
	atomic_store(&d->shm->middle, 1);	/* the game starts with 0, we hold 2 */
	d->shm->magic = DOOM_SHM_MAGIC;
	d->front = 2;
	d->fb = (struct hal_fb){ .width = width, .height = height, .stride = hdr.stride,
				 .format = HAL_FMT_XRGB8888 };
	/* above the target numbers, so that dup2() always makes a copy */
	child_sock = fcntl(sv[1], F_DUPFD_CLOEXEC, 10);
	child_mem = fcntl(mem, F_DUPFD_CLOEXEC, 10);
	if (child_sock < 0 || child_mem < 0 || spawn(d, game, dir, child_sock, child_mem) < 0)
		goto fail;
	d->sock = sv[0];
	close(sv[1]);
	close(mem);
	close(child_sock);
	close(child_mem);
	return d;

fail:
	err = errno;
	if (d->shm != MAP_FAILED)
		munmap(d->shm, d->shm_size);
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
	free(d);
	errno = err;
	return NULL;
}

int doom_fd(struct doom *d)
{
	return d->sock;
}

const struct hal_fb *doom_frame(struct doom *d, bool *ended)
{
	struct doom_msg m;
	ssize_t n;

	while (!d->ended) {
		n = recv(d->sock, &m, sizeof(m), 0);
		if (n > 0)
			continue;	/* only "frame published": the newest one is taken below */
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && errno == EAGAIN)
			break;
		d->ended = true;	/* EOF: the game process is gone */
	}
	*ended = d->ended;
	if (!(atomic_load(&d->shm->middle) & DOOM_SHM_FRESH))
		return NULL;
	d->front = atomic_exchange(&d->shm->middle, d->front) & ~DOOM_SHM_FRESH;
	d->fb.pixels = doom_shm_frame(d->shm, d->front);
	return &d->fb;
}

void doom_input(struct doom *d, const struct gp_event *ev)
{
	struct doom_msg m = { DOOM_MSG_BUTTON, ev->pressed, (uint8_t)ev->code };

	if (d->ended || ev->type != GP_EV_BUTTON || ev->repeat)
		return;
	if (send(d->sock, &m, sizeof(m), MSG_DONTWAIT | MSG_NOSIGNAL) < 0)
		LOGD(TAG, "button %s lost: %s", gp_button_name(ev->code), strerror(errno));
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

int doom_stop(struct doom *d)
{
	int status = 0;

	if (!d)
		return 0;
	if (!d->ended)
		kill(d->pid, SIGTERM);
	if (wait_for(d->pid, &status, STOP_WAIT_MS) == 0) {
		LOGW(TAG, "the game does not stop, killing it");
		kill(d->pid, SIGKILL);
		waitpid(d->pid, &status, 0);
	}
	if (WIFEXITED(status))
		LOGI(TAG, "game ended, exit status %d", WEXITSTATUS(status));
	else if (WIFSIGNALED(status))
		LOGI(TAG, "game ended by signal %d", WTERMSIG(status));
	close(d->sock);
	munmap(d->shm, d->shm_size);
	free(d);
	return status;
}
