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
 * @file speex_echo.h
 * @brief Acoustic echo canceller, fully decoupled from the codec.
 *
 * The echo canceller estimates the echo path between the loudspeaker
 * (far-end/"play") signal and the microphone (near-end/"rec") signal
 * with an adaptive FIR filter (block NLMS) and subtracts the estimated
 * echo from the microphone signal. A Geigel-style double-talk detector
 * freezes adaptation while near-end speech is present so the near-end
 * talker is not cancelled.
 *
 * The module has no dependency on the Speex codec and can be toggled
 * on/off at runtime. When disabled, the output is a sample-exact copy
 * of the microphone input. The complete algorithm state can be saved
 * to and restored from an opaque buffer.
 */

#ifndef SPEEX_ECHO_H
#define SPEEX_ECHO_H

#include "speex/speex_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Enable/disable the canceller (int, 1=enabled default, 0=passthrough) */
#define SPEEX_ECHO_SET_ENABLED 0
#define SPEEX_ECHO_GET_ENABLED 1
/** Set the sampling rate in Hz (int, informational, default 8000) */
#define SPEEX_ECHO_SET_SAMPLING_RATE 2
#define SPEEX_ECHO_GET_SAMPLING_RATE 3
/** Get the number of bytes required to save the state (spx_int32_t*) */
#define SPEEX_ECHO_GET_STATE_SIZE 4
/** Save the complete algorithm state into a caller-provided buffer
 *  (void* of at least SPEEX_ECHO_GET_STATE_SIZE bytes) */
#define SPEEX_ECHO_SAVE_STATE 5
/** Restore a state previously obtained with SPEEX_ECHO_SAVE_STATE.
 *  The frame_size/filter_length of the saved state must match. */
#define SPEEX_ECHO_RESTORE_STATE 6
/** Copy the current echo-path impulse response estimate
 *  (float* with room for filter_length values) */
#define SPEEX_ECHO_GET_IMPULSE 7

/** Opaque echo canceller state */
typedef struct SpeexEchoState SpeexEchoState;

/** Create an echo canceller state.
 * @param frame_size Number of samples processed per call
 * @param filter_length Number of taps of the echo path model
 *        (should cover the echo tail, e.g. 1024 taps = 128 ms @ 8 kHz)
 * @return New state, or NULL on allocation failure
 */
SpeexEchoState *speex_echo_state_init(int frame_size, int filter_length);

/** Free an echo canceller state */
void speex_echo_state_destroy(SpeexEchoState *st);

/** Process one frame.
 * @param st   Echo canceller state
 * @param rec  Microphone signal (near-end speech + echo), frame_size samples
 * @param play Loudspeaker (far-end reference) signal, frame_size samples
 * @param out  Echo-free output signal, frame_size samples.
 *             When the canceller is disabled, out == rec (sample-exact).
 */
void speex_echo_cancellation(SpeexEchoState *st, const spx_int16_t *rec,
                             const spx_int16_t *play, spx_int16_t *out);

/** Control interface, see SPEEX_ECHO_* requests above.
 * @return 0 on success, -1 on invalid request/argument
 */
int speex_echo_ctl(SpeexEchoState *st, int request, void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* SPEEX_ECHO_H */
