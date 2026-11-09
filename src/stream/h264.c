#include "h264.h"

#include <stdlib.h>
#include <string.h>

#include <wels/codec_api.h>

#include "core/log.h"

#define TAG "h264"

struct h264 {
	ISVCEncoder *enc;
	int width, height;
	uint8_t *yuv;			/* I420 */
	uint8_t *out;			/* the access unit, all layers in a row */
	size_t out_size, out_cap;
};

struct h264 *h264_new(int width, int height, int fps, int kbps)
{
	struct h264 *e = calloc(1, sizeof(*e));
	SEncParamExt p;
	int fmt = videoFormatI420;

	if (!e || (width | height) & 1)
		goto fail;
	e->width = width;
	e->height = height;
	e->yuv = malloc((size_t)width * height * 3 / 2);
	if (!e->yuv || WelsCreateSVCEncoder(&e->enc) || !e->enc)
		goto fail;
	(*e->enc)->GetDefaultParams(e->enc, &p);
	p.iUsageType = SCREEN_CONTENT_REAL_TIME;	/* menus and games alike */
	p.iPicWidth = width;
	p.iPicHeight = height;
	p.iTargetBitrate = kbps * 1000;
	p.iMaxBitrate = kbps * 1500;
	p.iRCMode = RC_BITRATE_MODE;
	p.fMaxFrameRate = (float)fps;
	p.uiIntraPeriod = (unsigned)fps * 2;	/* a viewer joining sees a picture soon */
	p.eSpsPpsIdStrategy = CONSTANT_ID;
	p.bEnableFrameSkip = true;
	p.iMultipleThreadIdc = 2;
	p.iSpatialLayerNum = 1;
	p.iTemporalLayerNum = 1;
	p.iComplexityMode = LOW_COMPLEXITY;
	p.sSpatialLayers[0].iVideoWidth = width;
	p.sSpatialLayers[0].iVideoHeight = height;
	p.sSpatialLayers[0].fFrameRate = (float)fps;
	p.sSpatialLayers[0].iSpatialBitrate = p.iTargetBitrate;
	p.sSpatialLayers[0].iMaxSpatialBitrate = p.iMaxBitrate;
	/* RTP packets of their own: slices up to a packet where it can */
	p.sSpatialLayers[0].sSliceArgument.uiSliceMode = SM_SINGLE_SLICE;
	if ((*e->enc)->InitializeExt(e->enc, &p)) {
		LOGE(TAG, "the encoder takes no %dx%d at %d kbit/s", width, height, kbps);
		goto fail;
	}
	(*e->enc)->SetOption(e->enc, ENCODER_OPTION_DATAFORMAT, &fmt);
	LOGI(TAG, "%dx%d, %d fps, %d kbit/s", width, height, fps, kbps);
	return e;
fail:
	h264_free(e);
	return NULL;
}

void h264_free(struct h264 *e)
{
	if (!e)
		return;
	if (e->enc) {
		(*e->enc)->Uninitialize(e->enc);
		WelsDestroySVCEncoder(e->enc);
	}
	free(e->yuv);
	free(e->out);
	free(e);
}

/* The layers of info one after the other into e->out */
static int collect(struct h264 *e, const SFrameBSInfo *info)
{
	size_t total = 0;
	int l, n;

	for (l = 0; l < info->iLayerNum; l++)
		for (n = 0; n < info->sLayerInfo[l].iNalCount; n++)
			total += (size_t)info->sLayerInfo[l].pNalLengthInByte[n];
	if (total > e->out_cap) {
		uint8_t *p = realloc(e->out, total);

		if (!p)
			return -1;
		e->out = p;
		e->out_cap = total;
	}
	e->out_size = 0;
	for (l = 0; l < info->iLayerNum; l++) {
		const SLayerBSInfo *li = &info->sLayerInfo[l];
		size_t len = 0;

		for (n = 0; n < li->iNalCount; n++)
			len += (size_t)li->pNalLengthInByte[n];
		memcpy(e->out + e->out_size, li->pBsBuf, len);
		e->out_size += len;
	}
	return 0;
}

int h264_params(struct h264 *e, const uint8_t **data, size_t *size)
{
	SFrameBSInfo info;

	memset(&info, 0, sizeof(info));
	if ((*e->enc)->EncodeParameterSets(e->enc, &info) || collect(e, &info))
		return -1;
	*data = e->out;
	*size = e->out_size;
	return 0;
}

void h264_force_idr(struct h264 *e)
{
	(*e->enc)->ForceIntraFrame(e->enc, true);
}

/* BT.601, studio range, a 2x2 block's colour from its average */
static void to_i420(struct h264 *e, const struct hal_fb *f)
{
	int w = e->width, h = e->height, x, y;
	uint8_t *Y = e->yuv, *U = Y + (size_t)w * h, *V = U + (size_t)w * h / 4;

	for (y = 0; y < h; y += 2) {
		const uint32_t *r0 = (const uint32_t *)((const uint8_t *)f->pixels + (size_t)y * f->stride);
		const uint32_t *r1 = (const uint32_t *)((const uint8_t *)r0 + f->stride);
		uint8_t *y0 = Y + (size_t)y * w, *y1 = y0 + w;

		for (x = 0; x < w; x += 2) {
			uint32_t p[4] = { r0[x], r0[x + 1], r1[x], r1[x + 1] };
			int rs = 0, gs = 0, bs = 0, i;

			for (i = 0; i < 4; i++) {
				int r = (int)(p[i] >> 16 & 0xff), g = (int)(p[i] >> 8 & 0xff), b = (int)(p[i] & 0xff);
				uint8_t lum = (uint8_t)((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;

				(i < 2 ? y0 : y1)[x + (i & 1)] = lum;
				rs += r;
				gs += g;
				bs += b;
			}
			rs >>= 2;
			gs >>= 2;
			bs >>= 2;
			U[(size_t)(y / 2) * (w / 2) + x / 2] = (uint8_t)(((-38 * rs - 74 * gs + 112 * bs + 128) >> 8) + 128);
			V[(size_t)(y / 2) * (w / 2) + x / 2] = (uint8_t)(((112 * rs - 94 * gs - 18 * bs + 128) >> 8) + 128);
		}
	}
}

int h264_encode(struct h264 *e, const struct hal_fb *frame, uint64_t ms, const uint8_t **data,
		size_t *size, int *idr)
{
	SSourcePicture pic;
	SFrameBSInfo info;

	if (frame->width < e->width || frame->height < e->height)
		return -1;
	to_i420(e, frame);
	memset(&pic, 0, sizeof(pic));
	memset(&info, 0, sizeof(info));
	pic.iColorFormat = videoFormatI420;
	pic.iPicWidth = e->width;
	pic.iPicHeight = e->height;
	pic.iStride[0] = e->width;
	pic.iStride[1] = pic.iStride[2] = e->width / 2;
	pic.pData[0] = e->yuv;
	pic.pData[1] = e->yuv + (size_t)e->width * e->height;
	pic.pData[2] = pic.pData[1] + (size_t)e->width * e->height / 4;
	pic.uiTimeStamp = (long long)ms;
	if ((*e->enc)->EncodeFrame(e->enc, &pic, &info) != cmResultSuccess)
		return -1;
	*idr = info.eFrameType == videoFrameTypeIDR;
	if (info.eFrameType == videoFrameTypeSkip || info.eFrameType == videoFrameTypeInvalid) {
		*size = 0;
		return 0;
	}
	if (collect(e, &info))
		return -1;
	*data = e->out;
	*size = e->out_size;
	return 0;
}
