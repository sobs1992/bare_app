#include "input_script.h"

#include <ctype.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/input.h"
#include "core/log.h"
#include "core/mainloop.h"

#define TAG "script"
#define MAX_WORDS 256

struct script {
	struct mainloop *ml;
	int interval;
	char words[MAX_WORDS][24];
	int head, count;
	char partial[24];
	int plen;
	struct ml_timer *timer;
	enum gp_button held;
};

static void step(void *ud);

static void schedule(struct script *s, int delay)
{
	if (!s->timer)
		s->timer = mainloop_add_timer(s->ml, delay, 0, step, s);
}

static void step(void *ud)
{
	struct script *s = ud;
	const char *w;

	s->timer = NULL;
	if (s->held != GP_NONE) {
		input_report_button(s->held, false);
		s->held = GP_NONE;
		if (s->count)
			schedule(s, s->interval);
		return;
	}
	if (!s->count)
		return;
	w = s->words[s->head];
	s->head = (s->head + 1) % MAX_WORDS;
	s->count--;
	if (!strncmp(w, "wait:", 5)) {
		schedule(s, atoi(w + 5));
		return;
	}
	s->held = gp_button_from_name(w);
	if (s->held == GP_NONE) {
		LOGW(TAG, "unknown word \"%s\"", w);
		schedule(s, 0);
		return;
	}
	LOGD(TAG, "press %s", w);
	input_report_button(s->held, true);
	schedule(s, 30);	/* release shortly after */
}

static void push_word(struct script *s)
{
	if (!s->plen)
		return;
	s->partial[s->plen] = '\0';
	s->plen = 0;
	if (s->count == MAX_WORDS) {
		LOGW(TAG, "script too long, dropping \"%s\"", s->partial);
		return;
	}
	strcpy(s->words[(s->head + s->count) % MAX_WORDS], s->partial);
	s->count++;
	schedule(s, s->interval);
}

static void on_stdin(int fd, short revents, void *ud)
{
	struct script *s = ud;
	char buf[256];
	ssize_t n, i;

	(void)revents;
	n = read(fd, buf, sizeof(buf));
	if (n <= 0) {
		push_word(s);
		mainloop_remove_fd(s->ml, fd);
		return;
	}
	for (i = 0; i < n; i++) {
		if (isspace((unsigned char)buf[i]))
			push_word(s);
		else if (s->plen < (int)sizeof(s->partial) - 1)
			s->partial[s->plen++] = buf[i];
	}
}

static void *script_open(const void *params, struct mainloop *ml)
{
	const struct script_params *p = params;
	struct script *s;

	if (isatty(STDIN_FILENO))
		return NULL;
	s = calloc(1, sizeof(*s));
	if (!s)
		return NULL;
	s->ml = ml;
	s->interval = p && p->interval_ms ? p->interval_ms : 150;
	s->held = GP_NONE;
	mainloop_add_fd(ml, STDIN_FILENO, POLLIN, on_stdin, s);
	input_declare(GP_BIT(GP_COUNT) - 1, 0);	/* a script can press anything */
	LOGI(TAG, "reading input script from stdin");
	return s;
}

static void script_close(void *h)
{
	struct script *s = h;

	mainloop_remove_fd(s->ml, STDIN_FILENO);
	mainloop_timer_cancel(s->ml, s->timer);
	free(s);
}

const struct hal_input_ops input_script = {
	.name = "script",
	.open = script_open,
	.close = script_close,
};
