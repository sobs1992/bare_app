#include "action.h"

#include <stddef.h>
#include <stdio.h>

#include "core/msgq.h"

static struct msgq *q;

int action_init(void)
{
	q = msgq_new(sizeof(struct gui_action), 64);
	return q ? 0 : -1;
}

void action_deinit(void)
{
	msgq_free(q);
	q = NULL;
}

void gui_action_post(enum gui_action_type type, int arg)
{
	struct gui_action a = { .type = type, .arg = arg };

	msgq_post(q, &a);
}

void gui_action_post_str(enum gui_action_type type, const char *str)
{
	struct gui_action a = { .type = type };

	snprintf(a.str, sizeof(a.str), "%s", str);
	msgq_post(q, &a);
}

bool action_pop(struct gui_action *a)
{
	return msgq_pop(q, a);
}

int action_fd(void)
{
	return msgq_fd(q);
}

void action_ack(void)
{
	msgq_ack(q);
}

const char *action_name(enum gui_action_type type)
{
	switch (type) {
	case ACT_BACK:           return "back";
	case ACT_OPEN_SCREEN:    return "open-screen";
	case ACT_SET_BRIGHTNESS: return "set-brightness";
	case ACT_SET_LANG:       return "set-lang";
	case ACT_QUIT:           return "quit";
	}
	return "?";
}
