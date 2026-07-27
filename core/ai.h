// ai — deterministic reference driver: pure-pursuit steering + curvature
// speed targets. Used by the headless completability test, the app's
// self-driving demo ('c'), and as the baseline for ghost review.
#pragma once

#include "track.h"
#include "sim.h"

typedef struct {
    float line_idx;     // fractional racing-line index (monotonic)
    float max_excursion; // largest |lateral| over the run (m)
    float max_exc_idx;  // line index where max excursion happened
    float offroad_time; // total seconds beyond half_w + 1 m
    int   finished;     // reached stage end
    float finish_time;  // sim seconds to finish
} ai_state_t;

void ai_init(ai_state_t *ai, const track_t *t);
// One 60 Hz step of AI control. Fills `in` for sim_step.
void ai_drive(ai_state_t *ai, const track_t *t, const car_t *car,
              sim_input_t *in);
