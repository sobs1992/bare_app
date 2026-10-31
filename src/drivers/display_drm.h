#pragma once

#include "hal/hal.h"

/*
 * DRM/KMS display with hardware planes, raw kernel UAPI (no libdrm).
 *
 * Layers map to planes of the CRTC: LAYER_VIDEO -> primary, LAYER_UI ->
 * the first overlay (ARGB: the video shows through transparent pixels),
 * LAYER_OSD -> the next overlay or the cursor plane. Without an overlay
 * the UI goes to the primary plane and there is no video layer.
 *
 * Each layer has a logical shadow buffer and two dumb buffers; frames are
 * flipped with nonblocking atomic commits, one per vblank at most.
 * Planes cannot rotate on every SoC, so rotation is done while copying the
 * damaged areas, as with fbdev.
 */
struct drm_params {
	const char *device;	/* "/dev/dri/card0" */
	const char *tty;	/* switched to KD_GRAPHICS while running, may be NULL */
	int rotation;		/* enum hal_rotation or HAL_ROT_AUTO */
};

extern const struct hal_display_ops display_drm;
