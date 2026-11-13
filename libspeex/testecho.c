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

/* Acceptance test for the echo canceller:
   1. echo suppression (ERLE) on an echo-only synthetic scene
   2. double-talk: near-end speech is preserved while echo is present
   3. algorithm state save/restore resumes bit-exactly
   4. disabled canceller is a sample-exact pass-through
   Exit status is 0 only if all checks pass. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "speex/speex_echo.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME 160
#define TAPS 1024
#define RATE 8000

/* Deterministic 32-bit LCG so the test needs no input files. */
static unsigned long lcg_state = 12345;
static float frand(void)
{
   lcg_state = lcg_state * 1664525UL + 1013904223UL;
   return ((lcg_state >> 8) & 0xFFFFFF) / 8388608.0f - 1.0f;
}

/* Speech-like test signal: harmonics with a slow envelope plus noise. */
static float speech_sample(int n, float f0, float env_phase)
{
   float t = (float)n / RATE;
   float env = 0.6f + 0.4f * sinf(2.0f * 3.14159265f * 2.3f * t + env_phase);
   float v = env * (0.45f * sinf(2.0f * 3.14159265f * f0 * t)
                  + 0.25f * sinf(2.0f * 3.14159265f * 2.0f * f0 * t + 0.7f)
                  + 0.12f * sinf(2.0f * 3.14159265f * 3.0f * f0 * t + 1.9f));
   return v + 0.02f * frand();
}

static double db(double ratio)
{
   return 10.0 * log10(ratio + 1e-30);
}

int main(void)
{
   int i, k, f;
   int failures = 0;
   const int conv_frames = 200;   /* echo only: convergence + measurement */
   const int dt_frames = 200;     /* double-talk */
   const int total = conv_frames + dt_frames;
   float h[512];                  /* synthetic room impulse response */
   static float far_buf[64000], near_buf[64000], mic_buf[64000];
   static float out_buf[64000];
   static spx_int16_t rec[FRAME], play[FRAME], out[FRAME];
   SpeexEchoState *st;
   double p_mic, p_out, p_near, p_err;
   int n0, n1;

   if (total * FRAME > 64000) {
      fprintf(stderr, "testecho: buffer too small\n");
      return 2;
   }

   /* Exponentially decaying echo path, 64 ms tail, normalized to an
      energy of 0.09 (echo RMS ~ 0.3x the far-end, as with a typical
      loudspeaker-microphone path). */
   {
      double energy = 0.0;
      for (k = 0; k < 512; k++) {
         h[k] = expf(-(float)k / 120.0f) * frand();
         energy += (double)h[k] * h[k];
      }
      for (k = 0; k < 512; k++)
         h[k] = (float)(h[k] * 0.3 / sqrt(energy));
   }

   /* Far-end everywhere, near-end speech only in the double-talk part. */
   for (i = 0; i < total * FRAME; i++) {
      far_buf[i] = 12000.0f * speech_sample(i, 180.0f, 0.0f);
      near_buf[i] = (i >= conv_frames * FRAME)
                  ? 9000.0f * speech_sample(i, 290.0f, 1.3f) : 0.0f;
   }
   /* Microphone = near-end + far-end convolved with the echo path. */
   for (i = 0; i < total * FRAME; i++) {
      float echo = 0.0f;
      for (k = 0; k < 512 && k <= i; k++)
         echo += h[k] * far_buf[i - k];
      mic_buf[i] = near_buf[i] + echo;
   }

   st = speex_echo_state_init(FRAME, TAPS);
   if (!st) {
      fprintf(stderr, "testecho: init failed\n");
      return 2;
   }
   i = RATE;
   if (speex_echo_ctl(st, SPEEX_ECHO_SET_SAMPLING_RATE, &i) != 0) {
      fprintf(stderr, "testecho: SET_SAMPLING_RATE failed\n");
      return 2;
   }

   for (f = 0; f < total; f++) {
      for (i = 0; i < FRAME; i++) {
         float v = mic_buf[f * FRAME + i];
         if (v > 32767.0f) v = 32767.0f;
         if (v < -32768.0f) v = -32768.0f;
         rec[i] = (spx_int16_t)(v >= 0.0f ? v + 0.5f : v - 0.5f);
         play[i] = (spx_int16_t)far_buf[f * FRAME + i];
      }
      speex_echo_cancellation(st, rec, play, out);
      for (i = 0; i < FRAME; i++)
         out_buf[f * FRAME + i] = (float)out[i];
   }

   /* --- Check 1: echo suppression (ERLE) over the converged echo-only part */
   n0 = (conv_frames - 50) * FRAME;
   n1 = conv_frames * FRAME;
   p_mic = p_out = 0.0;
   for (i = n0; i < n1; i++) {
      p_mic += (double)mic_buf[i] * mic_buf[i];
      p_out += (double)out_buf[i] * out_buf[i];
   }
   {
      double erle = db(p_mic / p_out);
      int ok = erle >= 15.0;
      printf("echo suppression: ERLE = %.1f dB (require >= 15 dB) ... %s\n",
             erle, ok ? "PASS" : "FAIL");
      if (!ok) failures++;
   }

   /* --- Check 2: double-talk, near-end speech must survive undistorted */
   n0 = (conv_frames + 20) * FRAME;
   n1 = total * FRAME;
   p_near = p_err = 0.0;
   for (i = n0; i < n1; i++) {
      double e = (double)out_buf[i] - near_buf[i];
      p_near += (double)near_buf[i] * near_buf[i];
      p_err += e * e;
   }
   {
      double snr = db(p_near / p_err);
      int ok = snr >= 10.0;
      printf("double-talk: near-end SNR = %.1f dB (require >= 10 dB) ... %s\n",
             snr, ok ? "PASS" : "FAIL");
      if (!ok) failures++;
   }
   speex_echo_state_destroy(st);

   /* --- Check 3: state save/restore resumes bit-exactly */
   {
      SpeexEchoState *a = speex_echo_state_init(FRAME, TAPS);
      SpeexEchoState *b = speex_echo_state_init(FRAME, TAPS);
      spx_int32_t size = 0;
      unsigned char *blob;
      static spx_int16_t out_a[30 * FRAME], out_b[30 * FRAME];
      int ok = 1;

      speex_echo_ctl(a, SPEEX_ECHO_GET_STATE_SIZE, &size);
      blob = (unsigned char*)malloc((size_t)size);
      if (!a || !b || !blob || size <= 0) {
         fprintf(stderr, "testecho: alloc failed\n");
         return 2;
      }
      for (f = 0; f < 80; f++) {
         for (i = 0; i < FRAME; i++) {
            rec[i] = (spx_int16_t)(mic_buf[f * FRAME + i]);
            play[i] = (spx_int16_t)far_buf[f * FRAME + i];
         }
         if (f == 50)
            speex_echo_ctl(a, SPEEX_ECHO_SAVE_STATE, blob);
         if (f >= 50) {
            speex_echo_cancellation(a, rec, play, out);
            memcpy(out_a + (f - 50) * FRAME, out, sizeof(out));
         } else {
            speex_echo_cancellation(a, rec, play, out);
         }
      }
      if (speex_echo_ctl(b, SPEEX_ECHO_RESTORE_STATE, blob) != 0) {
         printf("state save/restore: RESTORE rejected ... FAIL\n");
         failures++;
         ok = 0;
      }
      for (f = 50; f < 80 && ok; f++) {
         for (i = 0; i < FRAME; i++) {
            rec[i] = (spx_int16_t)(mic_buf[f * FRAME + i]);
            play[i] = (spx_int16_t)far_buf[f * FRAME + i];
         }
         speex_echo_cancellation(b, rec, play, out);
         memcpy(out_b + (f - 50) * FRAME, out, sizeof(out));
      }
      if (ok && memcmp(out_a, out_b, sizeof(out_a)) != 0)
         ok = 0;
      printf("state save/restore: %d bytes, continuation bit-exact ... %s\n",
             (int)size, ok ? "PASS" : "FAIL");
      if (!ok) failures++;

      /* A corrupted blob must be rejected, not crash. */
      blob[0] ^= 0xFF;
      if (speex_echo_ctl(b, SPEEX_ECHO_RESTORE_STATE, blob) == 0) {
         printf("state restore: corrupted blob accepted ... FAIL\n");
         failures++;
      }
      free(blob);
      speex_echo_state_destroy(a);
      speex_echo_state_destroy(b);
   }

   /* --- Check 4: disabled canceller is a sample-exact pass-through */
   {
      SpeexEchoState *d = speex_echo_state_init(FRAME, TAPS);
      int flag = 0, ok = 1;
      speex_echo_ctl(d, SPEEX_ECHO_SET_ENABLED, &flag);
      for (f = 0; f < 40 && ok; f++) {
         for (i = 0; i < FRAME; i++) {
            rec[i] = (spx_int16_t)(mic_buf[f * FRAME + i]);
            play[i] = (spx_int16_t)far_buf[f * FRAME + i];
         }
         speex_echo_cancellation(d, rec, play, out);
         if (memcmp(out, rec, sizeof(out)) != 0)
            ok = 0;
      }
      /* Toggle on and off again mid-stream: still exact when off. */
      flag = 1;
      speex_echo_ctl(d, SPEEX_ECHO_SET_ENABLED, &flag);
      speex_echo_cancellation(d, rec, play, out);
      flag = 0;
      speex_echo_ctl(d, SPEEX_ECHO_SET_ENABLED, &flag);
      speex_echo_cancellation(d, rec, play, out);
      if (memcmp(out, rec, sizeof(out)) != 0)
         ok = 0;
      printf("disabled: output sample-exact pass-through ... %s\n",
             ok ? "PASS" : "FAIL");
      if (!ok) failures++;
      speex_echo_state_destroy(d);
   }

   if (failures) {
      printf("testecho: %d check(s) FAILED\n", failures);
      return 1;
   }
   printf("testecho: all checks passed\n");
   return 0;
}
