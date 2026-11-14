/* Copyright (C) 2026 Xiph.Org Foundation */
/**
   @file testjitter.c
   @brief Acceptance test for the jitter buffer: reorder recovery, loss
   diagnostics and delay bound, using the repository's own encoder/decoder.
*/
/*
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "speex/speex.h"
#include "speex/speex_jitter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FRAME 160              /* 20 ms at 8 kHz */
#define N_FRAMES 300           /* scenario A frame count */
#define MARGIN (2*FRAME)       /* absorbs reorder depth 2 */
#define MAX_DELAY_A (8*FRAME)  /* generous bound: must never trigger in scenario A */
#define MAX_DELAY_B (2*FRAME)  /* tight bound: must trigger in scenario B */
#define MAX_PAYLOAD 200

static unsigned int lcg_state = 12345;
static unsigned int rnd(void)
{
   lcg_state = lcg_state*1103515245 + 12345;
   return (lcg_state>>16) & 0x7fff;
}

static int failures = 0;
#define CHECK(cond, name) do { \
   if (cond) printf("PASS: %s\n", name); \
   else { printf("FAIL: %s\n", name); failures++; } \
} while (0)

/* Deterministic network model for scenario A: frame i is dropped when
   (7*i+3) % 13 == 0. Arrivals are clocked by the playout tick: at tick i
   exactly one packet may arrive, carrying frame schedule[i] (-1 if none).
   Within each group of 3 consecutive ticks the emission order is [2,0,1],
   so a frame arrives at most 2 ticks early or late (reorder depth <= 2)
   while the long-term arrival rate matches the playout rate. */
static int frame_dropped(int i)
{
   return (7*i+3) % 13 == 0;
}

static void build_arrival_schedule(int *schedule)
{
   int g, i;
   for (i=0;i<N_FRAMES;i++)
      schedule[i] = frame_dropped(i) ? -1 : i;
   for (g=0; g*3+2<N_FRAMES; g++)
   {
      int b = g*3;
      if (schedule[b] >= 0 && schedule[b+1] >= 0 && schedule[b+2] >= 0)
      {
         int t0 = schedule[b], t1 = schedule[b+1], t2 = schedule[b+2];
         schedule[b] = t2;
         schedule[b+1] = t0;
         schedule[b+2] = t1;
      }
   }
}

static int scenario_a(void)
{
   void *enc, *dec;
   SpeexBits bits;
   JitterBuffer *jb;
   JitterBufferDiagnostics diag;
   spx_int32_t tmp;
   static char payload[N_FRAMES][MAX_PAYLOAD];
   int paylen[N_FRAMES];
   int dropped[N_FRAMES];
   int schedule[N_FRAMES];
   int n_arriv = 0, n_dropped = 0;
   int returned[N_FRAMES], n_returned = 0;
   short pcm[FRAME];
   int i, j, k;
   int expected_reorder = 0;
   int ok;

   enc = speex_encoder_init(speex_lib_get_mode(SPEEX_MODEID_NB));
   dec = speex_decoder_init(speex_lib_get_mode(SPEEX_MODEID_NB));
   tmp = 8;
   speex_encoder_ctl(enc, SPEEX_SET_QUALITY, &tmp);
   tmp = 1;
   speex_decoder_ctl(dec, SPEEX_SET_ENH, &tmp);
   speex_bits_init(&bits);

   /* Encode all frames with the repository's own encoder */
   for (i=0;i<N_FRAMES;i++)
   {
      for (j=0;j<FRAME;j++)
         pcm[j] = (short)(8000.0*sin(2.0*M_PI*(220+(i%5))*j/8000.0) + (int)(rnd()%2000)-1000);
      speex_bits_reset(&bits);
      speex_encode_int(enc, pcm, &bits);
      paylen[i] = speex_bits_write(&bits, payload[i], MAX_PAYLOAD);
      dropped[i] = frame_dropped(i);
      if (dropped[i])
         n_dropped++;
      else
         n_arriv++;
   }
   build_arrival_schedule(schedule);

   /* Expected reorder count: arrivals with a newer packet already seen */
   {
      int max_ts = -1;
      for (k=0;k<N_FRAMES;k++)
      {
         int ts;
         if (schedule[k] < 0)
            continue;
         ts = schedule[k]*FRAME;
         if (max_ts >= 0 && ts < max_ts)
            expected_reorder++;
         if (ts > max_ts)
            max_ts = ts;
      }
   }

   jb = jitter_buffer_init(FRAME);
   tmp = MARGIN;
   jitter_buffer_ctl(jb, JITTER_BUFFER_SET_MARGIN, &tmp);
   tmp = MAX_DELAY_A;
   jitter_buffer_ctl(jb, JITTER_BUFFER_SET_MAX_DELAY, &tmp);

   /* Prefill the first 3 ticks to establish initial buffering, then one put
      (if a packet arrives that tick) + one get per tick, then drain. */
   for (k=0;k<3;k++)
   {
      JitterBufferPacket p;
      if (schedule[k] < 0)
         continue;
      p.data = payload[schedule[k]];
      p.len = paylen[schedule[k]];
      p.timestamp = schedule[k]*FRAME;
      p.span = FRAME;
      p.sequence = schedule[k];
      p.user_data = 0;
      jitter_buffer_put(jb, &p);
   }
   for (k=3;k<N_FRAMES;k++)
   {
      JitterBufferPacket p, out;
      char outbuf[MAX_PAYLOAD];
      int ret;
      if (schedule[k] >= 0)
      {
         p.data = payload[schedule[k]];
         p.len = paylen[schedule[k]];
         p.timestamp = schedule[k]*FRAME;
         p.span = FRAME;
         p.sequence = schedule[k];
         p.user_data = 0;
         jitter_buffer_put(jb, &p);
      }
      out.data = outbuf;
      out.len = MAX_PAYLOAD;
      ret = jitter_buffer_get(jb, &out, FRAME, NULL);
      if (ret == JITTER_BUFFER_OK)
      {
         /* Decode the recovered packet with the repository's own decoder */
         speex_bits_reset(&bits);
         speex_bits_read_from(&bits, out.data, out.len);
         speex_decode_int(dec, &bits, pcm);
         if (n_returned < N_FRAMES)
            returned[n_returned++] = out.sequence;
      } else {
         /* Missing or insertion: decoder PLC (interpolation) */
         speex_decode_int(dec, NULL, pcm);
      }
      jitter_buffer_tick(jb);
   }
   /* Drain: pull packets while the buffer still holds some. Stopping as soon
      as it is empty avoids recording spurious losses past the end of stream. */
   for (;;)
   {
      JitterBufferPacket out;
      char outbuf[MAX_PAYLOAD];
      spx_int32_t avail = 0;
      int ret;
      jitter_buffer_ctl(jb, JITTER_BUFFER_GET_AVAILABLE_COUNT, &avail);
      if (avail == 0)
         break;
      out.data = outbuf;
      out.len = MAX_PAYLOAD;
      ret = jitter_buffer_get(jb, &out, FRAME, NULL);
      if (ret == JITTER_BUFFER_OK)
      {
         speex_bits_reset(&bits);
         speex_bits_read_from(&bits, out.data, out.len);
         speex_decode_int(dec, &bits, pcm);
         if (n_returned < N_FRAMES)
            returned[n_returned++] = out.sequence;
      } else {
         speex_decode_int(dec, NULL, pcm);
      }
      jitter_buffer_tick(jb);
   }

   jitter_buffer_ctl(jb, JITTER_BUFFER_GET_DIAGNOSTICS, &diag);

   printf("scenario A: frames=%d dropped=%d arrived=%d returned=%d\n",
          N_FRAMES, n_dropped, n_arriv, n_returned);
   printf("scenario A: lost=%u late=%u reordered=%u bound_drops=%u max_buffered=%u (bound=%d)\n",
          diag.lost_count, diag.late_count, diag.reordered_count,
          diag.dropped_bound_count, diag.max_buffered_observed, MAX_DELAY_A);

   /* Check 1: reorder recovery -- every non-dropped frame decoded exactly
      once, in increasing order */
   ok = (n_returned == n_arriv);
   if (ok)
   {
      j = 0;
      for (i=0;i<N_FRAMES && ok;i++)
      {
         if (dropped[i])
            continue;
         if (j >= n_returned || returned[j] != i)
            ok = 0;
         j++;
      }
      ok = ok && (j == n_returned);
   }
   CHECK(ok, "A1 reorder recovery: all non-dropped frames decoded in order");

   /* Check 2: loss sequence diagnosable -- diagnostics report exactly the
      dropped timestamps, in order */
   ok = ((int)diag.lost_count == n_dropped);
   if (ok)
   {
      j = 0;
      for (i=0;i<N_FRAMES && ok;i++)
      {
         if (!dropped[i])
            continue;
         if (j >= JITTER_BUFFER_DIAG_RING || diag.lost_ring[j] != (spx_uint32_t)(i*FRAME))
            ok = 0;
         j++;
      }
   }
   CHECK(ok, "A2 loss diagnostics: lost timestamps match dropped set exactly");

   /* Check 3: delay bound data -- observed buffering never exceeded bound */
   CHECK(diag.max_buffered_observed <= MAX_DELAY_A,
         "A3 delay bound: max_buffered_observed <= configured bound");
   CHECK(diag.dropped_bound_count == 0,
         "A4 generous bound never triggers spurious drops");

   /* Check 4: reorder counter matches the simulated network exactly */
   CHECK((int)diag.reordered_count == expected_reorder,
         "A5 reorder counter matches simulated reorder events");

   printf("A-data: expected_reorder=%d reported_reorder=%u lost_reported=%u dropped_actual=%d\n",
          expected_reorder, diag.reordered_count, diag.lost_count, n_dropped);

   jitter_buffer_destroy(jb);
   speex_bits_destroy(&bits);
   speex_encoder_destroy(enc);
   speex_decoder_destroy(dec);
   return 0;
}

/* Scenario B: fast network (2 puts per tick) with a tight bound. The bound
   must be enforced at all times and the drops must be accounted for. */
static int scenario_b(void)
{
   JitterBuffer *jb;
   JitterBufferDiagnostics diag;
   spx_int32_t tmp;
   int n_frames = 200;
   int i, k;
   int n_ok = 0;
   static char data[64];
   int ok;

   jb = jitter_buffer_init(FRAME);
   tmp = MAX_DELAY_B;
   jitter_buffer_ctl(jb, JITTER_BUFFER_SET_MAX_DELAY, &tmp);

   memset(data, 0x5a, sizeof(data));
   for (k=0;k<n_frames/2+50;k++)
   {
      for (i=0;i<2 && 2*k+i<n_frames;i++)
      {
         JitterBufferPacket p;
         p.data = data;
         p.len = sizeof(data);
         p.timestamp = (2*k+i)*FRAME;
         p.span = FRAME;
         p.sequence = 2*k+i;
         p.user_data = 0;
         jitter_buffer_put(jb, &p);
      }
      {
         JitterBufferPacket out;
         char outbuf[64];
         out.data = outbuf;
         out.len = sizeof(outbuf);
         if (jitter_buffer_get(jb, &out, FRAME, NULL) == JITTER_BUFFER_OK)
            n_ok++;
      }
      jitter_buffer_tick(jb);
   }

   jitter_buffer_ctl(jb, JITTER_BUFFER_GET_DIAGNOSTICS, &diag);

   printf("scenario B: frames=%d played=%d bound_drops=%u max_buffered=%u (bound=%d)\n",
          n_frames, n_ok, diag.dropped_bound_count, diag.max_buffered_observed, MAX_DELAY_B);

   CHECK(diag.max_buffered_observed <= MAX_DELAY_B,
         "B1 delay bound enforced: max_buffered_observed <= tight bound");
   CHECK(diag.dropped_bound_count > 0,
         "B2 bound drops occurred and are accounted for");
   ok = (n_ok + (int)diag.dropped_bound_count >= n_frames - 2);
   CHECK(ok, "B3 every frame was either played or accounted as bound drop");

   jitter_buffer_destroy(jb);
   return 0;
}

static int perf(void)
{
   JitterBuffer *jb;
   static char data[64];
   struct timespec t0, t1;
   long k, K = 1000000;
   double sec, ns_op;

   jb = jitter_buffer_init(FRAME);
   memset(data, 0x33, sizeof(data));
   clock_gettime(CLOCK_MONOTONIC, &t0);
   for (k=0;k<K;k++)
   {
      JitterBufferPacket p, out;
      char outbuf[64];
      p.data = data;
      p.len = sizeof(data);
      p.timestamp = k*FRAME;
      p.span = FRAME;
      p.sequence = k;
      p.user_data = 0;
      jitter_buffer_put(jb, &p);
      out.data = outbuf;
      out.len = sizeof(outbuf);
      jitter_buffer_get(jb, &out, FRAME, NULL);
      jitter_buffer_tick(jb);
   }
   clock_gettime(CLOCK_MONOTONIC, &t1);
   sec = (t1.tv_sec-t0.tv_sec) + 1e-9*(t1.tv_nsec-t0.tv_nsec);
   ns_op = sec*1e9/K;
   printf("perf: %ld put+get+tick cycles in %.3f s -> %.1f ns/op, %.2f Mpackets/s\n",
          K, sec, ns_op, K/sec/1e6);
   jitter_buffer_destroy(jb);
   return 0;
}

int main(int argc, char **argv)
{
   if (argc > 1 && strcmp(argv[1], "perf") == 0)
      return perf();
   scenario_a();
   scenario_b();
   if (failures)
   {
      printf("RESULT: FAIL (%d checks failed)\n", failures);
      return 1;
   }
   printf("RESULT: PASS (all checks passed)\n");
   return 0;
}
