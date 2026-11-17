// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../libs/frame.h"


typedef enum {
	US_H264_ENCODER_TYPE_M2M,
	US_H264_ENCODER_TYPE_CPU,
} us_h264_encoder_type_e;

typedef struct us_h264_encoder_s us_h264_encoder_s;


int us_h264_encoder_parse_type(const char *str);
us_h264_encoder_s *us_h264_encoder_init(us_h264_encoder_type_e type, const char *path, uint bitrate, uint gop, bool boost);
void us_h264_encoder_destroy(us_h264_encoder_s *enc);
uint us_h264_encoder_get_fps_limit(const us_h264_encoder_s *enc);
int us_h264_encoder_compress(us_h264_encoder_s *enc, const us_frame_s *src, us_frame_s *dest, bool force_key, uint fps);
