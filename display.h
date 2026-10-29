#pragma once
#include <opxy/draw.hpp>
#include <opxy/types.hpp>

namespace ohmonk {
using opxy::i32;
using opxy::u32;

struct DisplayState {
    i32 vowel_s;
    i32 reson_s;
    i32 vib_s;
    i32 delay_s;
    i32 breath_s;
    i32 size_s;
    i32 rate_s;
    i32 wave_phase;
    i32 note_env;
    i32 mouth_env;
    i32 ring_env;
    i32 last_vowel_in;
    i32 last_ring_sig;
    u32 vowel_hold_ticks;
    u32 ring_hold_ticks;
    u32 last_serial;
    i32 side_l_slide;
    i32 side_r_slide;
    i32 side_l_env;
    i32 side_r_env;
    u32 side_l_hold_ticks;
    u32 side_r_hold_ticks;
    u32 vib_bloom_ticks;
    i32 vib_onset;
    i32 last_vibdly_in;
};
static_assert(sizeof(DisplayState) == 100, "DisplayState size matches definition.json");

typedef opxy::draw::Frame<0, 0, 256, 4, 9> Frame;

void display_reset(DisplayState&);
void display(DisplayState&, const opxy::draw::Input&, Frame&);
} // namespace ohmonk
