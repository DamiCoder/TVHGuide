#include <string.h>  
#include <stdlib.h>
#include <stdint.h>

#include "tvhplayer.h"

int tvh_init(tvh_object_t *tvh) {
  avcodec_init();
  avcodec_register_all();

  tvh->ao = malloc(sizeof(aout_sys_t));
  memset(tvh->ao, 0, sizeof(aout_sys_t));

  tvh->acs = malloc(sizeof(acodec_sys_t));
  memset(tvh->acs, 0, sizeof(acodec_sys_t));

  tvh->vo = malloc(sizeof(vout_sys_t));
  memset(tvh->vo, 0, sizeof(vout_sys_t));

  tvh->vcs = malloc(sizeof(vcodec_sys_t));
  memset(tvh->vcs, 0, sizeof(vcodec_sys_t));

  if(opensles_open(tvh->ao) < 0) {
    return -1;
  }
  if(surface_init(tvh->vo) < 0) {
    return -1;
  }

  return 0;
}

void tvh_destroy(tvh_object_t *tvh) {
  tvh_audio_close(tvh);
  opensles_close(tvh->ao);
  free(tvh->acs);
  free(tvh->ao);

  tvh_video_close(tvh);
  surface_destroy(tvh->vo);
  free(tvh->vcs);
  free(tvh->vo);

  free(tvh);
}

int tvh_video_init(tvh_object_t *tvh, const char *codec) {
  int codec_id = 0;
  vcodec_sys_t *cs = tvh->vcs;

  if(!strcmp(codec, "H264")) {
    codec_id = CODEC_ID_H264;
  } else if(!strcmp(codec, "MPEG2VIDEO")) {
    codec_id = CODEC_ID_MPEG2VIDEO;
  }

  if(!codec_id) {
    DEBUG("Unknown video codec %s", codec);
    return -1;
  }

  cs->codec = avcodec_find_decoder(codec_id);
  if(!cs->codec) {
    DEBUG("Unable to open video codec %s", codec);
    return -1;
  }

  if(cs->codec->type != CODEC_TYPE_VIDEO) {
    DEBUG("Invalid codec type for video decoding");
    return -1;
  }
  
  cs->ctx = avcodec_alloc_context2(CODEC_TYPE_VIDEO);
  cs->frame = avcodec_alloc_frame();
  avcodec_get_frame_defaults(cs->frame);

  if(avcodec_open(cs->ctx, cs->codec) < 0) {
    DEBUG("Unable to open video codec");
    tvh_video_close(tvh);
    return -1;
  }

  return 0;
}

void tvh_video_enqueue(tvh_object_t *tvh, uint8_t *buf, size_t len, uint64_t pts, uint64_t dts, uint64_t dur) {
  AVPacket packet;
  AVPicture pict;
  int length;
  int got_picture;
  int pix_fmt = PIX_FMT_BGR32;
  vcodec_sys_t *cs = tvh->vcs;
  vout_sys_t *vo = tvh->vo;

  if(!vo->surface) {
    return;
  }

  av_init_packet(&packet);
  packet.data = buf;
  packet.size = len;
  packet.pts  = pts;
  packet.dts  = dts;
  packet.duration = dur;

  length = avcodec_decode_video2(cs->ctx, cs->frame, &got_picture, &packet);
  if(length <= 0) {
    DEBUG("Unable to decode video stream");
  }

  if(!got_picture) {
    return;
  }

  memset(&pict, 0, sizeof(pict));

  vo->lock(vo->surface, (void*)&vo->surface_info, 1);

  avpicture_fill(&pict, 
		 (uint8_t *)vo->surface_info.bits, 
		 pix_fmt, 
		 vo->surface_info.width, 
		 vo->surface_info.height);
  pict.linesize[0] = 4*vo->surface_info.size;

  cs->conv = sws_getCachedContext(cs->conv,
				  cs->ctx->width,
				  cs->ctx->height,
				  cs->ctx->pix_fmt,
				  vo->surface_info.width,
				  vo->surface_info.height,
				  pix_fmt,
				  SWS_FAST_BILINEAR,
				  NULL,
				  NULL,
				  NULL);

  sws_scale(cs->conv, 
	    (const uint8_t * const*)cs->frame->data, 
	    cs->frame->linesize,
	    0, 
	    cs->ctx->height, 
	    pict.data, 
	    pict.linesize);

  vo->unlockAndPost(vo->surface);
}

void tvh_video_close(tvh_object_t *tvh) {
  vcodec_sys_t *cs = tvh->vcs;
  
  if(cs->ctx != NULL) {
    avcodec_close(cs->ctx);
  }
  
  if(cs->frame) {
    av_free(cs->frame);
  }
  
  cs->ctx = NULL;
  cs->codec = NULL;
  cs->frame = NULL;
}

int tvh_audio_init(tvh_object_t *tvh, const char *codec) {
  int codec_id = 0;
  acodec_sys_t *cs = tvh->acs;

  if(!strcmp(codec, "AC3")) {
    codec_id = CODEC_ID_AC3;
  } else if(!strcmp(codec, "EAC3")) {
    codec_id = CODEC_ID_EAC3;
  } else if(!strcmp(codec, "AAC")) {
    codec_id = CODEC_ID_AAC;
  } else if(!strcmp(codec, "MPEG2AUDIO")) {
    codec_id = CODEC_ID_MP2;
  }

  if(!codec_id) {
    DEBUG("Unknown audio codec %s", codec);
    return -1;
  }

  cs->codec = avcodec_find_decoder(codec_id);
  if(!cs->codec) {
    DEBUG("Unable to open audio codec %s", codec);
    return -1;
  }

  if(cs->codec->type != CODEC_TYPE_AUDIO) {
    DEBUG("Invalid codec type for audio decoding");
    return -1;
  }
  
  cs->ctx = avcodec_alloc_context2(CODEC_TYPE_AUDIO);
  cs->buf = av_malloc(AVCODEC_MAX_AUDIO_FRAME_SIZE*2);

  if(avcodec_open(cs->ctx, cs->codec) < 0) {
    DEBUG("Unable to open audio codec");
    tvh_audio_close(tvh);
    return -1;
  }

  return 0;
}

void tvh_audio_enqueue(tvh_object_t *tvh, uint8_t *buf, size_t len) {
  uint8_t *ptr;
  AVPacket packet;
  int length;
  acodec_sys_t *cs = tvh->acs;
  aout_sys_t *ao = tvh->ao;

  av_init_packet(&packet);
  packet.data = ptr = buf;
  packet.size = len;
  
  do {
    cs->len = AVCODEC_MAX_AUDIO_FRAME_SIZE*2;
    int length = avcodec_decode_audio3(cs->ctx, cs->buf, &cs->len, &packet);
    if(length <= 0) {
      break;
    }

    aout_buffer_t *ab = (aout_buffer_t *) malloc(sizeof(aout_buffer_t));
    ab->ptr = malloc(cs->len);
    ab->len = cs->len;
    memcpy(ab->ptr, cs->buf, cs->len);

    opensles_enqueue(ao, ab);

    ptr += length;
    len -= length;

    packet.data = ptr;
    packet.size = len;    
  } while(len);
}

int tvh_audio_close(tvh_object_t *tvh) {
  acodec_sys_t *cs = tvh->acs;

  if(cs->ctx != NULL) {
    avcodec_close(cs->ctx);
  }

  if(cs->buf != NULL) {
    av_free(cs->buf);
  }
  
  cs->ctx = NULL;
  cs->codec = NULL;
  cs->buf = NULL;
}