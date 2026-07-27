// audio_synth — see header. All fixed-rate arithmetic, no libm in the mixer.
#include "audio_synth.h"

// gear bands (top speed per gear, m/s) → rpm norm rises through each band
static const float GEAR_TOP[5] = { 8.0f, 14.0f, 20.0f, 27.0f, 36.0f };

static float rpm_norm(float vx) {
    if (vx < 0.0f) vx = 0.0f;
    float lo = 0.0f;
    for (int g = 0; g < 5; g++) {
        if (vx <= GEAR_TOP[g]) {
            float span = GEAR_TOP[g] - lo;
            float t = span > 0.0f ? (vx - lo) / span : 0.0f;
            return 0.25f + 0.75f * t;      // never below idle band
        }
        lo = GEAR_TOP[g];
    }
    return 1.0f;
}

// surface noise gain x10 and lowpass alpha (Q8-ish float)
// order matches SURF_*: bitumen, gravel, sand, grass, mud, water
static const float SURF_GAIN[6]  = { 0.06f, 0.22f, 0.14f, 0.16f, 0.18f, 0.30f };
static const float SURF_ALPHA[6] = { 0.35f, 0.18f, 0.10f, 0.14f, 0.12f, 0.45f };

void synth_init(synth_state_t *st) {
    st->ph1 = st->ph2 = st->phsub = 0.0f;
    st->lfsr = 0xACE1u;
    st->lp = 0.0f;
    st->ev_type = 0;
    st->ev_pos = 0;
    st->q_head = st->q_tail = 0;
}

void synth_event(synth_state_t *st, int ev) {
    int next = (st->q_head + 1) & (SYN_QUEUE - 1);
    if (next == st->q_tail) return;   // full: drop newest (beeps are cheap)
    st->q_buf[st->q_head] = ev;
    st->q_head = next;
}

static int16_t clamp16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

// one-shot definitions: freq (Hz, 0 = noise), duration (ms), gain
static void shot_params(int ev, float *freq, int *dur_ms, float *gain) {
    switch (ev) {
    case SYN_EV_BEEP:   *freq = 880.0f;  *dur_ms = 110; *gain = 0.30f; break;
    case SYN_EV_GO:     *freq = 1320.0f; *dur_ms = 320; *gain = 0.34f; break;
    case SYN_EV_SPLIT:  *freq = 1046.0f; *dur_ms = 90;  *gain = 0.26f; break;
    case SYN_EV_FINISH: *freq = 784.0f;  *dur_ms = 700; *gain = 0.32f; break;
    case SYN_EV_SPLASH: *freq = 0.0f;    *dur_ms = 450; *gain = 0.38f; break;
    case SYN_EV_THUD:   *freq = 130.0f;  *dur_ms = 220; *gain = 0.36f; break;
    default:            *freq = 440.0f;  *dur_ms = 50;  *gain = 0.20f; break;
    }
}

void synth_mix(synth_state_t *st, const synth_input_t *in,
               int16_t *out, int n) {
    const float dt = 1.0f / (float)SYNTH_RATE;
    float rn = rpm_norm(in->vx);
    float ef = 55.0f + rn * 95.0f;                 // 55..150 Hz fundamental
    float egain = in->engine_on ? (0.10f + 0.30f * in->throttle + 0.10f * rn) : 0.0f;
    if (in->handbrake) egain *= 0.8f;
    float step1 = ef * dt;
    float step2 = ef * 1.0073f * dt;               // detune
    float stepsub = ef * 0.5f * dt;

    int surf = in->surface;
    if (surf < 0 || surf > 5) surf = 3;              // grass default
    float sgain = SURF_GAIN[surf];
    float salpha = SURF_ALPHA[surf];
    float speed_n = in->vx / 25.0f;
    if (speed_n > 1.0f) speed_n = 1.0f;
    float slip_boost = 1.0f + (in->slip_rear > 0.0f ? in->slip_rear : -in->slip_rear) * 1.2f;
    if (slip_boost > 2.0f) slip_boost = 2.0f;

    // event bookkeeping (shot envelope runs in sample units)
    float ev_freq = 0.0f; int ev_dur = 1; float ev_gain = 0.0f;
    if (st->ev_type) shot_params(st->ev_type, &ev_freq, &ev_dur, &ev_gain);
    int ev_dur_samples = ev_dur * SYNTH_RATE / 1000;

    for (int i = 0; i < n; i++) {
        int32_t mix = 0;

        // ── engine: two saws + sub sine-ish ──
        st->ph1 += step1; if (st->ph1 >= 1.0f) st->ph1 -= 1.0f;
        st->ph2 += step2; if (st->ph2 >= 1.0f) st->ph2 -= 1.0f;
        st->phsub += stepsub; if (st->phsub >= 1.0f) st->phsub -= 1.0f;
        float saw1 = st->ph1 * 2.0f - 1.0f;
        float saw2 = st->ph2 * 2.0f - 1.0f;
        float sub = st->phsub < 0.5f ? 1.0f : -1.0f;   // square sub
        float eng = (saw1 + saw2) * 0.35f + sub * 0.30f;
        mix += (int32_t)(eng * egain * 22000.0f);

        // ── surface noise: LFSR → one-pole ──
        uint32_t l = st->lfsr;
        l ^= l << 13; l ^= l >> 17; l ^= l << 5;
        st->lfsr = l;
        float white = ((float)(l & 0xFFFF) - 32768.0f) / 32768.0f;
        st->lp += salpha * (white - st->lp);
        mix += (int32_t)(st->lp * sgain * speed_n * slip_boost * 24000.0f);

        // ── one-shots ──
        if (st->ev_type) {
            float env = 1.0f - (float)st->ev_pos / (float)ev_dur_samples;
            env *= env;   // decay curve
            float s;
            if (ev_freq > 0.0f) {
                float ph = (float)st->ev_pos * ev_freq * dt;
                float frac = ph - (float)(int)ph;
                s = (frac < 0.5f ? 1.0f : -1.0f);
                // finish sting: drop a fifth halfway (two-note)
                if (st->ev_type == SYN_EV_FINISH && st->ev_pos > ev_dur_samples / 2) {
                    float ph2 = (float)(st->ev_pos - ev_dur_samples / 2) * ev_freq * 1.5f * dt;
                    float fr2 = ph2 - (float)(int)ph2;
                    s = (fr2 < 0.5f ? 1.0f : -1.0f);
                }
            } else {
                uint32_t l2 = st->lfsr;
                l2 ^= l2 << 13; l2 ^= l2 >> 17; l2 ^= l2 << 5;
                st->lfsr = l2;
                s = ((float)(l2 & 0xFFFF) - 32768.0f) / 32768.0f;
            }
            mix += (int32_t)(s * env * ev_gain * 24000.0f);
            if (++st->ev_pos >= ev_dur_samples) {
                st->ev_type = 0;
                st->ev_pos = 0;
                // dequeue next
                if (st->q_tail != st->q_head) {
                    st->ev_type = st->q_buf[st->q_tail];
                    st->q_tail = (st->q_tail + 1) & (SYN_QUEUE - 1);
                    shot_params(st->ev_type, &ev_freq, &ev_dur, &ev_gain);
                    ev_dur_samples = ev_dur * SYNTH_RATE / 1000;
                }
            }
        } else if (st->q_tail != st->q_head) {
            st->ev_type = st->q_buf[st->q_tail];
            st->q_tail = (st->q_tail + 1) & (SYN_QUEUE - 1);
            st->ev_pos = 0;
            shot_params(st->ev_type, &ev_freq, &ev_dur, &ev_gain);
            ev_dur_samples = ev_dur * SYNTH_RATE / 1000;
        }

        int16_t v = clamp16(mix);
        *out++ = v;      // L
        *out++ = v;      // R (mono for now; particles can pan later)
    }
}
