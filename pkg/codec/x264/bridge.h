#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <x264.h>

#define ERR_DEFAULT_PRESET -1
#define ERR_APPLY_PROFILE -2
#define ERR_ALLOC_PICTURE -3
#define ERR_OPEN_ENGINE -4
#define ERR_ENCODE -5
#define ERR_BITRATE_RECONFIG -6

typedef struct Slice {
  unsigned char *data;
  int data_len;
} Slice;

typedef struct Encoder {
  x264_t *h;
  x264_picture_t pic_in;
  x264_param_t param;
  int force_key_frame;
} Encoder;

Encoder *enc_new(x264_param_t param, char *preset, int *rc) {
  Encoder *e = (Encoder *)malloc(sizeof(Encoder));

  if (x264_param_default_preset(&e->param, preset, "zerolatency") < 0) {
    free(preset);
    *rc = ERR_DEFAULT_PRESET;
    goto fail;
  }
  free(preset);

  /* Configure non-default params */
  e->param.i_csp = param.i_csp;
  e->param.i_width = param.i_width;
  e->param.i_height = param.i_height;
  e->param.i_fps_num = param.i_fps_num;
  e->param.i_fps_den = 1;
  // Intra refres:
  e->param.i_keyint_max = param.i_keyint_max;
  // Rate control:
  e->param.rc.i_rc_method = X264_RC_ABR;
  e->param.rc.i_bitrate = param.rc.i_bitrate;
  e->param.rc.i_vbv_max_bitrate = param.rc.i_vbv_max_bitrate;
  e->param.rc.i_vbv_buffer_size = param.rc.i_vbv_buffer_size;
  // For streaming:
  e->param.b_repeat_headers = 1;
  e->param.b_annexb = 1;

  if (x264_param_apply_profile(&e->param, "high") < 0) {
    *rc = ERR_APPLY_PROFILE;
    goto fail;
  }

  x264_picture_t pic_in;
  if (x264_picture_alloc(&pic_in, param.i_csp, param.i_width, param.i_height) < 0) {
    *rc = ERR_ALLOC_PICTURE;
    goto fail;
  }

  // FIXME: we use x264_picture_alloc to set the metadata only, we don't need the allocated memory
  //        to store the frame. Since we free the frame memory here, we don't need to call
  //        x264_picture_clean later.
  e->pic_in = pic_in;
  x264_picture_clean(&pic_in);

  e->h = x264_encoder_open(&e->param);
  if (!e->h) {
    *rc = ERR_OPEN_ENGINE;
    goto fail;
  }

  return e;

fail:
  free(e);
  return NULL;
}

#define RC_MARGIN 10000 /* 1kilobits / second*/
static int apply_target_bitrate(Encoder *e, int target_bitrate) {
  int target_encoder_bitrate = (int)target_bitrate / 1000;
  if (e->param.rc.i_bitrate == target_encoder_bitrate || target_encoder_bitrate <= 1) {
    return 0; // if no change to bitrate or target bitrate is too small, we return no error (0)
  }

  e->param.rc.i_bitrate = target_encoder_bitrate;
  e->param.rc.f_rate_tolerance = 0.1;
  e->param.rc.i_vbv_max_bitrate = target_encoder_bitrate + RC_MARGIN / 2;
  e->param.rc.i_vbv_buffer_size = e->param.rc.i_vbv_max_bitrate;
  e->param.rc.f_vbv_buffer_init = 0.6;
  int success = x264_encoder_reconfig(e->h, &e->param);
  return success; // 0 on success or negative on error
}

Slice enc_encode(Encoder *e, uint8_t *y, uint8_t *cb, uint8_t *cr, int *rc) {
  x264_nal_t *nal;
  int i_nal;

  x264_picture_t pic_out;
  e->pic_in.img.plane[0] = y;
  e->pic_in.img.plane[1] = cb;
  e->pic_in.img.plane[2] = cr;
  if (e->force_key_frame) {
    e->pic_in.i_type = X264_TYPE_IDR;
  } else {
    e->pic_in.i_type = X264_TYPE_AUTO;
  }

  int frame_size = x264_encoder_encode(e->h, &nal, &i_nal, &e->pic_in, &pic_out);
  e->force_key_frame = 0;
  Slice s = {.data_len = frame_size};
  if (frame_size <= 0) {
    *rc = ERR_ENCODE;
    return s;
  }

  // e->pic_in.i_pts++;
  s.data = nal->p_payload;
  return s;
}

void enc_close(Encoder *e, int *rc) {
  x264_encoder_close(e->h);
  free(e);
}
