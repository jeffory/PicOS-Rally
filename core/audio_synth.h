// audio_synth — rally synth core (M5). Pure C, no PicOS headers: the app
// runs synth_mix() on Core 1 from the audio callback; the headless suite
// runs the same code on the host. 11025 Hz stereo, Doom-proven stream rate.
//
// Voices: engine (2 detuned saws + sub sine, 5-gear box), surface noise
// (LFSR + one-pole lowpass, gain from surface/speed/slip), one-shot event
// queue (beeps, blips, splash, thud). Deterministic given the same input
// sequence — no rand(), no time-of-day.
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define SYNTH_RATE 11025

typedef enum {
    SYN_EV_NONE = 0,
    SYN_EV_BEEP,        // countdown 3/2/1 tick
    SYN_EV_GO,          // green light
    SYN_EV_SPLIT,       // checkpoint blip
    SYN_EV_FINISH,      // results sting
    SYN_EV_SPLASH,      // water entry
    SYN_EV_THUD,        // off-course penalty
} synth_event_t;

// Written by Core 0 each frame; read by the mixer on Core 1.
typedef struct {
    float vx;           // forward speed m/s
    float throttle;     // ramped 0..1
    float slip_rear;    // rad
    int   surface;      // SURF_*
    bool  handbrake;
    bool  engine_on;    // false on intro/results
} synth_input_t;

// Event queue is SPSC: Core 0 pushes, mixer pops. 4 deep, drop-oldest.
#define SYN_QUEUE 4

typedef struct {
    // engine oscillators (phase accumulators, 0..1)
    float ph1, ph2, phsub;
    // surface noise
    uint32_t lfsr;
    float  lp;          // one-pole state
    // one-shot playback
    int   ev_type;      // SYN_EV_* currently playing (0 = none)
    int   ev_pos;       // sample position within the shot
    // SPSC queue (indices mod SYN_QUEUE)
    volatile int q_head, q_tail;
    volatile int q_buf[SYN_QUEUE];
} synth_state_t;

void synth_init(synth_state_t *st);

// Core 0 side: queue an event (safe with the mixer running on Core 1 —
// single writer, single reader, int-sized slots).
void synth_event(synth_state_t *st, int ev);

// Mixer: fills `out` with n interleaved stereo frames. Core 1 / host side.
void synth_mix(synth_state_t *st, const synth_input_t *in,
               int16_t *out, int n);
