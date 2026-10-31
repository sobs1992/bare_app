/*
 * bare_app: GUI application for the OdroidLinux SDK.
 *
 * Threads: this one is the control thread (services, bus, control
 * modules); the view thread is started by view_start(). See
 * docs/ARCHITECTURE.md.
 */
#include <getopt.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include "core/bus.h"
#include "core/log.h"
#include "core/mainloop.h"
#include "core/util.h"
#include "gui/action.h"
#include "gui/control/control.h"
#include "gui/ctx.h"
#include "gui/notify.h"
#include "gui/view/core/view.h"
#include "hal/platform.h"
#include "services/config.h"
#include "services/services.h"

#define TAG "main"

extern const struct view_backend_ops view_backend_headless, view_backend_lvgl;

static const struct view_backend_ops *const backends[] = {
#ifdef CONFIG_VIEW_LVGL
	&view_backend_lvgl,	/* the first one is the default */
#endif
#ifdef CONFIG_VIEW_HEADLESS
	&view_backend_headless,
#endif
};

static void usage(FILE *f)
{
	size_t i;

	fprintf(f, "usage: bare_app [-p PLATFORM] [-b BACKEND] [-L LANG] [-v] [-l]\n"
		   "  -p, --platform NAME   skip detection (see -l)\n"
		   "  -b, --backend NAME    view backend:");
	for (i = 0; i < ARRAY_SIZE(backends); i++)
		fprintf(f, " %s", backends[i]->name);
	fprintf(f, "\n  -L, --lang en|ru      UI language (default: $BARE_APP_LANG, the saved one, en)\n"
		   "  -v, --verbose         debug log\n"
		   "  -l, --list            list platforms\n");
}

static void on_signal(int fd, short revents, void *ud)
{
	struct signalfd_siginfo si;

	(void)revents;
	if (read(fd, &si, sizeof(si)) != sizeof(si))
		return;
#ifdef CONFIG_SNAPSHOT
	if (si.ssi_signo == SIGUSR1) {
		gui_notify_post(NOTIFY_SNAPSHOT, 0);	/* screenshot, see view.c */
		return;
	}
#endif
	LOGI(TAG, "signal %u, quitting", si.ssi_signo);
	mainloop_quit(ud);
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "platform", required_argument, NULL, 'p' },
		{ "backend", required_argument, NULL, 'b' },
		{ "lang", required_argument, NULL, 'L' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "list", no_argument, NULL, 'l' },
		{ "help", no_argument, NULL, 'h' },
		{ 0 }
	};
	const struct view_backend_ops *be = backends[0];
	const char *pf_name = NULL, *lang = getenv("BARE_APP_LANG");
	const struct platform *pf;
	struct mainloop *ml;
	sigset_t sigs;
	int c, sfd, ret = 1;
	size_t i;

	while ((c = getopt_long(argc, argv, "p:b:L:vlh", opts, NULL)) != -1) {
		switch (c) {
		case 'p':
			pf_name = optarg;
			break;
		case 'b':
			be = NULL;
			for (i = 0; i < ARRAY_SIZE(backends); i++)
				if (!strcmp(backends[i]->name, optarg))
					be = backends[i];
			if (!be) {
				fprintf(stderr, "bare_app: unknown backend \"%s\"\n", optarg);
				return 1;
			}
			break;
		case 'L':
			lang = optarg;
			break;
		case 'v':
			log_set_level(LOG_DEBUG);
			break;
		case 'l':
			platform_list();
			return 0;
		case 'h':
			usage(stdout);
			return 0;
		default:
			usage(stderr);
			return 1;
		}
	}

	pf = platform_probe(pf_name);
	if (!pf)
		return 1;
	LOGI(TAG, "platform %s, backend %s", pf->name, be->name);

	/* Signals go to the control loop only; the view thread inherits the mask */
	sigemptyset(&sigs);
	sigaddset(&sigs, SIGINT);
	sigaddset(&sigs, SIGTERM);
	sigaddset(&sigs, SIGHUP);
#ifdef CONFIG_SNAPSHOT
	sigaddset(&sigs, SIGUSR1);
#endif
	pthread_sigmask(SIG_BLOCK, &sigs, NULL);
	signal(SIGPIPE, SIG_IGN);
	sfd = signalfd(-1, &sigs, SFD_NONBLOCK | SFD_CLOEXEC);

	ml = mainloop_new("control");
	if (!ml || sfd < 0 || bus_init(ml) || ctx_init() || action_init() || notify_init()) {
		LOGE(TAG, "init failed");
		return 1;
	}
	mainloop_add_fd(ml, sfd, POLLIN, on_signal, ml);

#ifdef CONFIG_SETTINGS_FILE
	config_open(ml, pf->config_path);
#else
	config_open(ml, NULL);		/* settings live in memory only */
#endif
	if (services_start(ml, pf) || control_start(&(struct control_env){ ml, pf, lang }))
		goto out;
	if (view_start(be, pf))
		goto out_control;

	mainloop_run(ml);
	ret = 0;

	view_stop();
out_control:
	control_stop();
out:
	config_close();
	services_stop();
	notify_deinit();
	action_deinit();
	ctx_deinit();
	bus_deinit();
	mainloop_free(ml);
	close(sfd);
	LOGI(TAG, "bye");
	return ret;
}
