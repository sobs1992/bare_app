/*
 * Test video source: animated colour bars and a bouncing box, drawn by its
 * own thread into a frame mailbox at a fixed rate. Stands in for a camera
 * or a GPU renderer and exercises the same path: producer thread ->
 * mailbox -> view thread -> video layer.
 */
#pragma once

struct mailbox;
struct testpattern;

struct testpattern *testpattern_start(struct mailbox *mb, int fps);
void testpattern_stop(struct testpattern *tp);	/* joins the thread */
