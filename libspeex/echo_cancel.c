/* Copyright (C) 2026 Xiph.Org Foundation
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   - Neither the name of the Xiph.org Foundation nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/**
 * Acoustic echo canceller, decoupled from the Speex codec.
 *
 * Algorithm: per-sample normalized LMS (NLMS) adaptive FIR filter that
 * estimates the echo path from the far-end reference to the microphone,
 * plus a Geigel-style double-talk detector with hangover that freezes
 * adaptation while near-end speech is present.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "speex/speex_echo.h"

#include <stdlib.h>
#include <string.h>

#define ECHO_STATE_MAGIC 0x53584543u /* "SXEC" */
#define ECHO_STATE_VERSION 1

/* NLMS step size (0 < mu < 2 for stability) */
#define ECHO_MU 0.5f
/* Small regularization term for the NLMS normalization */
#define ECHO_EPS 1.0e4f
/* Geigel detector: near-end declared when max|mic| >= GEIGEL_THRESH * max|far| */
#define GEIGEL_THRESH 0.5f
/* Below this mic magnitude no double-talk is declared (near silence) */
#define GEIGEL_ABS_MIN 100.0f
/* Frames adaptation stays frozen after the last double-talk detection */
#define DTD_HANGOVER_FRAMES 12

struct SpeexEchoState {
   int frame_size;
   int filter_length;
   int enabled;
   int sampling_rate;
   float mu;
   float x_power;      /* running far-end power over the filter window */
   int x_pos;          /* write position in the circular far-end history */
   int ring_pos;       /* write position in the per-frame max|x| ring */
   int ring_len;       /* number of frame slots in the ring */
   int hangover;       /* remaining double-talk hangover frames */
   float *W;           /* adaptive filter taps [filter_length] */
   float *x_hist;      /* circular far-end history [filter_length] */
   float *x_max_ring;  /* per-frame max |far-end| [ring_len] */
};

/* Serialized header, followed by W, x_hist and x_max_ring arrays.
   The saved state is opaque and only valid for the same build/arch. */
typedef struct {
   spx_uint32_t magic;
   spx_uint32_t version;
   spx_int32_t frame_size;
   spx_int32_t filter_length;
   spx_int32_t enabled;
   spx_int32_t sampling_rate;
   float mu;
   float x_power;
   spx_int32_t x_pos;
   spx_int32_t ring_pos;
   spx_int32_t ring_len;
   spx_int32_t hangover;
} EchoStateHeader;

static spx_int32_t echo_state_size(const SpeexEchoState *st)
{
   return (spx_int32_t)(sizeof(EchoStateHeader)
          + (size_t)st->filter_length * sizeof(float)   /* W */
          + (size_t)st->filter_length * sizeof(float)   /* x_hist */
          + (size_t)st->ring_len * sizeof(float));      /* x_max_ring */
}

EXPORT SpeexEchoState *speex_echo_state_init(int frame_size, int filter_length)
{
   SpeexEchoState *st;
   if (frame_size <= 0 || filter_length <= 0)
      return NULL;
   st = (SpeexEchoState*)calloc(1, sizeof(SpeexEchoState));
   if (!st)
      return NULL;
   st->frame_size = frame_size;
   st->filter_length = filter_length;
   st->enabled = 1;
   st->sampling_rate = 8000;
   st->mu = ECHO_MU;
   st->ring_len = filter_length / frame_size + 2;
   st->W = (float*)calloc((size_t)filter_length, sizeof(float));
   st->x_hist = (float*)calloc((size_t)filter_length, sizeof(float));
   st->x_max_ring = (float*)calloc((size_t)st->ring_len, sizeof(float));
   if (!st->W || !st->x_hist || !st->x_max_ring) {
      speex_echo_state_destroy(st);
      return NULL;
   }
   return st;
}

EXPORT void speex_echo_state_destroy(SpeexEchoState *st)
{
   if (!st)
      return;
   free(st->W);
   free(st->x_hist);
   free(st->x_max_ring);
   free(st);
}

static float ring_max(const SpeexEchoState *st)
{
   int i;
   float m = 0.0f;
   for (i = 0; i < st->ring_len; i++)
      if (st->x_max_ring[i] > m)
         m = st->x_max_ring[i];
   return m;
}

EXPORT void speex_echo_cancellation(SpeexEchoState *st, const spx_int16_t *rec,
                             const spx_int16_t *play, spx_int16_t *out)
{
   int i, k;
   float frame_max_x = 0.0f;
   float frame_max_d = 0.0f;
   int adapt;

   if (!st || !rec || !play || !out)
      return;

   /* Disabled: sample-exact pass-through, algorithm state untouched. */
   if (!st->enabled) {
      if (out != rec)
         memcpy(out, rec, (size_t)st->frame_size * sizeof(spx_int16_t));
      return;
   }

   /* Geigel double-talk detector on frame magnitudes. */
   for (i = 0; i < st->frame_size; i++) {
      float ax = (float)(play[i] < 0 ? -(int)play[i] : (int)play[i]);
      float ad = (float)(rec[i] < 0 ? -(int)rec[i] : (int)rec[i]);
      if (ax > frame_max_x)
         frame_max_x = ax;
      if (ad > frame_max_d)
         frame_max_d = ad;
   }
   st->x_max_ring[st->ring_pos] = frame_max_x;
   st->ring_pos = (st->ring_pos + 1) % st->ring_len;

   if (frame_max_d >= GEIGEL_ABS_MIN &&
       frame_max_d >= GEIGEL_THRESH * ring_max(st))
      st->hangover = DTD_HANGOVER_FRAMES;

   adapt = (st->hangover == 0);
   if (st->hangover > 0)
      st->hangover--;

   for (i = 0; i < st->frame_size; i++) {
      int pos = st->x_pos;
      float x = (float)play[i];
      float y = 0.0f;
      float e;

      /* Push far-end sample into the circular history. */
      st->x_power += x * x - st->x_hist[pos] * st->x_hist[pos];
      if (st->x_power < 0.0f)
         st->x_power = 0.0f;
      st->x_hist[pos] = x;
      st->x_pos = (pos + 1 == st->filter_length) ? 0 : pos + 1;

      /* Echo estimate: y = sum_k W[k] * x[n-k] */
      for (k = 0; k < st->filter_length; k++) {
         int idx = pos - k;
         if (idx < 0)
            idx += st->filter_length;
         y += st->W[k] * st->x_hist[idx];
      }

      e = (float)rec[i] - y;

      if (adapt) {
         float step = st->mu * e / (st->x_power + ECHO_EPS);
         for (k = 0; k < st->filter_length; k++) {
            int idx = pos - k;
            if (idx < 0)
               idx += st->filter_length;
            st->W[k] += step * st->x_hist[idx];
         }
      }

      if (e > 32767.0f)
         e = 32767.0f;
      else if (e < -32768.0f)
         e = -32768.0f;
      out[i] = (spx_int16_t)(e >= 0.0f ? e + 0.5f : e - 0.5f);
   }
}

EXPORT int speex_echo_ctl(SpeexEchoState *st, int request, void *ptr)
{
   if (!st || !ptr)
      return -1;
   switch (request) {
   case SPEEX_ECHO_SET_ENABLED:
      st->enabled = (*(const int*)ptr) != 0;
      return 0;
   case SPEEX_ECHO_GET_ENABLED:
      *(int*)ptr = st->enabled;
      return 0;
   case SPEEX_ECHO_SET_SAMPLING_RATE:
      st->sampling_rate = *(const int*)ptr;
      return 0;
   case SPEEX_ECHO_GET_SAMPLING_RATE:
      *(int*)ptr = st->sampling_rate;
      return 0;
   case SPEEX_ECHO_GET_STATE_SIZE:
      *(spx_int32_t*)ptr = echo_state_size(st);
      return 0;
   case SPEEX_ECHO_SAVE_STATE:
   {
      EchoStateHeader h;
      unsigned char *p = (unsigned char*)ptr;
      memset(&h, 0, sizeof(h));
      h.magic = ECHO_STATE_MAGIC;
      h.version = ECHO_STATE_VERSION;
      h.frame_size = st->frame_size;
      h.filter_length = st->filter_length;
      h.enabled = st->enabled;
      h.sampling_rate = st->sampling_rate;
      h.mu = st->mu;
      h.x_power = st->x_power;
      h.x_pos = st->x_pos;
      h.ring_pos = st->ring_pos;
      h.ring_len = st->ring_len;
      h.hangover = st->hangover;
      memcpy(p, &h, sizeof(h));
      p += sizeof(h);
      memcpy(p, st->W, (size_t)st->filter_length * sizeof(float));
      p += (size_t)st->filter_length * sizeof(float);
      memcpy(p, st->x_hist, (size_t)st->filter_length * sizeof(float));
      p += (size_t)st->filter_length * sizeof(float);
      memcpy(p, st->x_max_ring, (size_t)st->ring_len * sizeof(float));
      return 0;
   }
   case SPEEX_ECHO_RESTORE_STATE:
   {
      EchoStateHeader h;
      const unsigned char *p = (const unsigned char*)ptr;
      memcpy(&h, p, sizeof(h));
      if (h.magic != ECHO_STATE_MAGIC || h.version != ECHO_STATE_VERSION)
         return -1;
      if (h.frame_size != st->frame_size ||
          h.filter_length != st->filter_length ||
          h.ring_len != st->ring_len)
         return -1;
      st->enabled = h.enabled;
      st->sampling_rate = h.sampling_rate;
      st->mu = h.mu;
      st->x_power = h.x_power;
      st->x_pos = h.x_pos;
      st->ring_pos = h.ring_pos;
      st->hangover = h.hangover;
      p += sizeof(h);
      memcpy(st->W, p, (size_t)st->filter_length * sizeof(float));
      p += (size_t)st->filter_length * sizeof(float);
      memcpy(st->x_hist, p, (size_t)st->filter_length * sizeof(float));
      p += (size_t)st->filter_length * sizeof(float);
      memcpy(st->x_max_ring, p, (size_t)st->ring_len * sizeof(float));
      return 0;
   }
   case SPEEX_ECHO_GET_IMPULSE:
      memcpy(ptr, st->W, (size_t)st->filter_length * sizeof(float));
      return 0;
   default:
      return -1;
   }
}
