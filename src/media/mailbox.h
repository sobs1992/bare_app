/*
 * Frame mailbox between one producer thread (camera, GPU, test pattern)
 * and the view thread, triple buffered:
 *
 *   producer: fb = mailbox_begin(); draw into fb; mailbox_end();
 *   consumer: on mailbox_fd() readable: mailbox_ack(); fb = mailbox_take();
 *
 * The producer never waits: there is always a free buffer. A frame that
 * was not taken yet is replaced by the newer one (only the latest counts
 * for video). A taken frame stays untouched until the next take.
 */
#pragma once

#include "hal/hal.h"

struct mailbox;

struct mailbox *mailbox_new(int width, int height);
void mailbox_free(struct mailbox *mb);

struct hal_fb *mailbox_begin(struct mailbox *mb);
void mailbox_end(struct mailbox *mb);

int mailbox_fd(struct mailbox *mb);
void mailbox_ack(struct mailbox *mb);
struct hal_fb *mailbox_take(struct mailbox *mb);	/* newest frame or NULL */
unsigned mailbox_dropped(struct mailbox *mb);		/* frames replaced unread */
