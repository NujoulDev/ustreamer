// SPDX-License-Identifier: GPL-3.0-or-later

#include "h264.h"

#include <limits.h>
#include <stdlib.h>
#include <strings.h>

#include <linux/videodev2.h>

#ifdef WITH_LIBX264
#	include <x264.h>
#	include <libavutil/imgutils.h>
#	include <libswscale/swscale.h>
#endif

#include "../libs/logging.h"

#include "m2m.h"


struct us_h264_encoder_s {
	us_m2m_encoder_s *m2m;
#	ifdef WITH_LIBX264
	uint	bitrate;
	uint	gop;
	uint	width;
	uint	height;
	uint	fps;
	s64		pts;
	int		last_online;
	ldf		last_encode_ts;

	x264_t				*cpu;
	x264_picture_t		picture;
	bool				picture_allocated;
	struct SwsContext	*sws;
#	endif
};


#ifdef WITH_LIBX264
static void _cpu_cleanup(us_h264_encoder_s *enc);
static int _cpu_ensure(us_h264_encoder_s *enc, const us_frame_s *src, uint fps);
static int _cpu_compress(us_h264_encoder_s *enc, const us_frame_s *src, us_frame_s *dest, bool force_key, uint fps);
#endif


int us_h264_encoder_parse_type(const char *str) {
	if (!strcasecmp(str, "m2m")) {
		return US_H264_ENCODER_TYPE_M2M;
	}
	if (!strcasecmp(str, "cpu")) {
		return US_H264_ENCODER_TYPE_CPU;
	}
	return -1;
}

us_h264_encoder_s *us_h264_encoder_init(us_h264_encoder_type_e type, const char *path, uint bitrate, uint gop, bool boost) {
	us_h264_encoder_s *enc;
	US_CALLOC(enc, 1);
	if (type == US_H264_ENCODER_TYPE_M2M) {
		enc->m2m = us_m2m_h264_encoder_init("H264", path, bitrate, gop, boost);
	} else {
		US_A(type == US_H264_ENCODER_TYPE_CPU);
#		ifdef WITH_LIBX264
		enc->bitrate = bitrate;
		enc->gop = gop;
		enc->last_online = -1;
		US_LOG_INFO("H264: Using CPU encoder (libx264)");
#		else
		US_RAISE("CPU H264 encoding requires WITH_LIBX264=1");
#		endif
	}
	return enc;
}

void us_h264_encoder_destroy(us_h264_encoder_s *enc) {
	if (enc->m2m != NULL) {
		us_m2m_encoder_destroy(enc->m2m);
	}
#	ifdef WITH_LIBX264
	_cpu_cleanup(enc);
#	endif
	free(enc);
}

uint us_h264_encoder_get_fps_limit(const us_h264_encoder_s *enc) {
	return (enc->m2m != NULL ? enc->m2m->run->fps_limit : 0);
}

int us_h264_encoder_compress(us_h264_encoder_s *enc, const us_frame_s *src, us_frame_s *dest, bool force_key, uint fps) {
	if (enc->m2m != NULL) {
		return us_m2m_encoder_compress(enc->m2m, src, dest, force_key);
	}
#	ifdef WITH_LIBX264
	return _cpu_compress(enc, src, dest, force_key, fps);
#	else
	(void)fps;
	return -1;
#	endif
}

#ifdef WITH_LIBX264
static void _cpu_cleanup(us_h264_encoder_s *enc) {
	if (enc->cpu != NULL) {
		x264_encoder_close(enc->cpu);
		enc->cpu = NULL;
	}
	if (enc->picture_allocated) {
		x264_picture_clean(&enc->picture);
		enc->picture_allocated = false;
	}
	sws_freeContext(enc->sws);
	enc->sws = NULL;
	enc->last_online = -1;
	enc->pts = 0;
}

static int _cpu_ensure(us_h264_encoder_s *enc, const us_frame_s *src, uint fps) {
	if (enc->cpu != NULL && enc->width == src->width && enc->height == src->height && enc->fps == fps) {
		return 0;
	}
	_cpu_cleanup(enc);

	x264_param_t params;
	if (x264_param_default_preset(&params, "ultrafast", "zerolatency") < 0) {
		return -1;
	}
	params.i_width = src->width;
	params.i_height = src->height;
	params.i_csp = X264_CSP_I420;
	params.i_fps_num = fps;
	params.i_fps_den = 1;
	params.i_timebase_num = 1;
	params.i_timebase_den = fps;
	params.b_vfr_input = 0;
	params.i_keyint_max = US_MAX(1U, enc->gop);
	params.i_keyint_min = 1;
	params.i_scenecut_threshold = 0;
	params.b_repeat_headers = 1;
	params.b_annexb = 1;
	params.rc.i_rc_method = X264_RC_ABR;
	params.rc.i_bitrate = enc->bitrate;
	params.rc.i_vbv_max_bitrate = enc->bitrate;
	params.rc.i_vbv_buffer_size = enc->bitrate;
	params.i_log_level = X264_LOG_WARNING;
	if (x264_param_apply_profile(&params, "baseline") < 0) {
		return -1;
	}
	if (x264_picture_alloc(&enc->picture, X264_CSP_I420, src->width, src->height) < 0) {
		return -1;
	}
	enc->picture_allocated = true;
	enc->cpu = x264_encoder_open(&params);
	if (enc->cpu == NULL) {
		_cpu_cleanup(enc);
		return -1;
	}
	enc->width = src->width;
	enc->height = src->height;
	enc->fps = fps;
	US_LOG_INFO("H264: CPU encoder configured: %ux%u, %u fps, %u Kbps, GOP=%u",
		enc->width, enc->height, fps, enc->bitrate, enc->gop);
	return 0;
}

static int _cpu_compress(us_h264_encoder_s *enc, const us_frame_s *src, us_frame_s *dest, bool force_key, uint fps) {
	if (src->width == 0 || src->height == 0 || src->width > INT_MAX || src->height > INT_MAX
		|| (src->width & 1) || (src->height & 1) || src->stride > INT_MAX || fps == 0) {
		US_LOG_ERROR("H264: Invalid CPU encoder geometry, stride or frame rate");
		return -1;
	}

	enum AVPixelFormat format;
	switch (src->format) {
		case V4L2_PIX_FMT_UYVY: format = AV_PIX_FMT_UYVY422; break;
		case V4L2_PIX_FMT_YUYV: format = AV_PIX_FMT_YUYV422; break;
		case V4L2_PIX_FMT_YVYU: format = AV_PIX_FMT_YVYU422; break;
		case V4L2_PIX_FMT_YUV420:
		case V4L2_PIX_FMT_YVU420: format = AV_PIX_FMT_YUV420P; break;
		case V4L2_PIX_FMT_GREY: format = AV_PIX_FMT_GRAY8; break;
		case V4L2_PIX_FMT_RGB565: format = AV_PIX_FMT_RGB565LE; break;
		case V4L2_PIX_FMT_RGB24: format = AV_PIX_FMT_RGB24; break;
		case V4L2_PIX_FMT_BGR24: format = AV_PIX_FMT_BGR24; break;
		default:
			US_LOG_ERROR("H264: Unsupported CPU input format: %u", src->format);
			return -1;
	}

	int strides[4];
	if (av_image_fill_linesizes(strides, format, src->width) < 0) {
		return -1;
	}
	if (src->stride != 0) {
		if (src->stride < (uint)strides[0] || (format == AV_PIX_FMT_YUV420P && (src->stride & 1))) {
			US_LOG_ERROR("H264: Invalid CPU input stride: %u", src->stride);
			return -1;
		}
		strides[0] = src->stride;
		if (format == AV_PIX_FMT_YUV420P) {
			strides[1] = strides[2] = src->stride / 2;
		}
	}
	u8 *planes[4];
	const int size = av_image_fill_pointers(planes, format, src->height, src->data, strides);
	if (size < 0 || src->data == NULL || src->used < (uz)size) {
		US_LOG_ERROR("H264: Truncated CPU input frame: %zu bytes", src->used);
		return -1;
	}
	if (src->format == V4L2_PIX_FMT_YVU420) {
		u8 *tmp = planes[1];
		planes[1] = planes[2];
		planes[2] = tmp;
	}
	us_frame_encoding_begin(src, dest, V4L2_PIX_FMT_H264);
	if (_cpu_ensure(enc, src, fps) < 0) {
		US_LOG_ERROR("H264: Can't configure CPU encoder");
		return -1;
	}
	enc->sws = sws_getCachedContext(enc->sws,
		src->width, src->height, format, src->width, src->height, AV_PIX_FMT_YUV420P,
		SWS_FAST_BILINEAR, NULL, NULL, NULL);
	if (enc->sws == NULL || sws_scale(enc->sws, (const u8 *const*)planes, strides, 0, src->height,
		enc->picture.img.plane, enc->picture.img.i_stride) != (int)src->height) {
		US_LOG_ERROR("H264: Can't convert CPU input frame to YUV420");
		return -1;
	}

	force_key = (force_key || enc->last_online != src->online || enc->last_encode_ts + 0.5 < us_get_now_monotonic());
	enc->picture.i_pts = enc->pts++;
	enc->picture.i_type = (force_key ? X264_TYPE_IDR : X264_TYPE_AUTO);
	x264_picture_t output;
	x264_nal_t *nals;
	int n_nals;
	if (x264_encoder_encode(enc->cpu, &nals, &n_nals, &enc->picture, &output) <= 0) {
		US_LOG_ERROR("H264: CPU encoder did not produce a frame");
		_cpu_cleanup(enc);
		return -1;
	}
	for (int index = 0; index < n_nals; ++index) {
		us_frame_append_data(dest, nals[index].p_payload, nals[index].i_payload);
	}
	dest->key = output.b_keyframe;
	dest->gop = enc->gop;
	us_frame_encoding_end(dest);
	enc->last_online = src->online;
	enc->last_encode_ts = dest->encode_end_ts;
	return 0;
}
#endif
