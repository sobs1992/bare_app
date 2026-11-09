#include "shot.h"

#include <stdbool.h>
#include <stdio.h>

#include "core/log.h"

void shot_compose(struct hal_fb *dst, const struct hal_fb *ui, const struct hal_fb *video)
{
	bool alpha = ui->format == HAL_FMT_ARGB8888;
	int x, y;

	for (y = 0; y < ui->height && y < dst->height; y++) {
		const uint32_t *u = (const uint32_t *)((const uint8_t *)ui->pixels + (size_t)y * ui->stride);
		const uint32_t *v = video ? (const uint32_t *)((const uint8_t *)video->pixels +
							       (size_t)y * video->stride) : NULL;
		uint32_t *o = (uint32_t *)((uint8_t *)dst->pixels + (size_t)y * dst->stride);

		for (x = 0; x < ui->width && x < dst->width; x++) {
			uint32_t a = alpha ? u[x] >> 24 : 255, b = v ? v[x] : 0;
			uint32_t rb, g;

			if (a == 255) {
				o[x] = u[x] & 0xffffff;
			} else if (a == 0) {
				o[x] = b & 0xffffff;
			} else {	/* two channels at a time, /256 is close enough */
				rb = ((u[x] & 0xff00ff) * a + (b & 0xff00ff) * (256 - a)) >> 8;
				g = ((u[x] & 0xff00) * a + (b & 0xff00) * (256 - a)) >> 8;
				o[x] = (rb & 0xff00ff) | (g & 0xff00);
			}
		}
	}
}

int shot_write_ppm(const char *path, const struct hal_fb *ui, const struct hal_fb *video)
{
	FILE *f = fopen(path, "wb");
	int x, y, sh;

	if (!f) {
		LOGE("shot", "%s: cannot write", path);
		return -1;
	}
	fprintf(f, "P6\n%d %d\n255\n", ui->width, ui->height);
	for (y = 0; y < ui->height; y++) {
		const uint32_t *u = (const uint32_t *)((const uint8_t *)ui->pixels + (size_t)y * ui->stride);
		const uint32_t *v = video ? (const uint32_t *)((const uint8_t *)video->pixels +
							       (size_t)y * video->stride) : NULL;

		for (x = 0; x < ui->width; x++) {
			uint32_t a = ui->format == HAL_FMT_ARGB8888 ? u[x] >> 24 : 255;
			uint32_t b = v ? v[x] : 0;

			for (sh = 16; sh >= 0; sh -= 8)
				fputc((((u[x] >> sh) & 0xff) * a + ((b >> sh) & 0xff) * (255 - a)) / 255, f);
		}
	}
	fclose(f);
	LOGI("shot", "composed picture saved to %s", path);
	return 0;
}
