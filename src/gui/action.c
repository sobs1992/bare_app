#include "action.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

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

void gui_action_post_secret(enum gui_action_type type, const char *str, const char *secret)
{
	struct gui_action a = { .type = type };

	snprintf(a.str, sizeof(a.str), "%s", str);
	snprintf(a.secret, sizeof(a.secret), "%s", secret);
	msgq_post(q, &a);
	memset(&a, 0, sizeof(a));	/* the queue keeps its own copy */
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
	case ACT_SET_VOLUME:     return "set-volume";
	case ACT_VOLUME_STEP:    return "volume-step";
	case ACT_SET_AUDIO_OUT:  return "set-audio-out";
	case ACT_WIFI_ENABLE:    return "wifi-enable";
	case ACT_WIFI_SCAN:      return "wifi-scan";
	case ACT_WIFI_ASK_KEY:   return "wifi-ask-key";
	case ACT_WIFI_CONNECT:   return "wifi-connect";
	case ACT_WIFI_FORGET:    return "wifi-forget";
	case ACT_QUIT:           return "quit";
	}
	return "?";
}
