/*
 * Fixed-size message queue between threads: any thread posts, one thread
 * consumes. msgq_fd() becomes readable when messages are waiting; the
 * consumer calls msgq_ack() and then pops until the queue is empty.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

struct msgq;

struct msgq *msgq_new(size_t msg_size, unsigned capacity);
void msgq_free(struct msgq *q);

bool msgq_post(struct msgq *q, const void *msg);	/* false when full (message dropped) */
bool msgq_pop(struct msgq *q, void *msg);		/* false when empty */

int msgq_fd(struct msgq *q);
void msgq_ack(struct msgq *q);		/* consume the wakeup */
void msgq_kick(struct msgq *q);		/* re-arm the wakeup (work left over) */
