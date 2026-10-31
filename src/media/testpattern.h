/*
 * Test video source: animated colour bars and a bouncing box drawn into a
 * HAL layer from a timer of the given loop. Stands in for a camera or a
 * GPU renderer until there is one; uses only the HAL layer API.
 */
#pragma once

struct hal_layer;
struct mainloop;
struct testpattern;

struct testpattern *testpattern_start(struct mainloop *ml, struct hal_layer *layer, int fps);
void testpattern_stop(struct testpattern *tp);	/* leaves the layer black */
