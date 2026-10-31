#include "shot.h"

#include <stdio.h>

#include "core/log.h"

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
