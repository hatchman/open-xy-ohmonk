#pragma once
#include <opxy/audio.hpp>
#include <opxy/types.hpp>

namespace ohmonk {
using opxy::i16;
using opxy::i32;
using opxy::u32;

struct Voice {
    u32 carrier_phase;
    u32 vib_phase;
    u32 rng_state;
    u32 held_blocks;
    i32 random_jitter_q16;
    i32 target_jitter_q16;
    u32 jitter_counter;
    i32 vowel_smooth;
    i32 reson_smooth;
    i32 size_smooth;
    i32 breath_smooth;
    u32 last_grain_key;
    u32 buf_id;
    u32 snap_params;
};
static_assert(sizeof(Voice) == 56, "Voice size matches definition.json");

struct Shared {
    u32 initialized;
    u32 in_use_mask;
    u32 active_block_mask;
    u32 grain_key;
    i32 vowel_smooth;
    i32 size_smooth;
    i32 reson_smooth;
    u32 needs_clear[13];
    u32 last_carrier_phase[13];
    u32 overlap_read_pos[13];
    u32 delay_write_idx[13];
    i32 current_delay_l_q8[13];
    i32 current_delay_r_q8[13];
    i32 dly_in_prev[13];
    i32 dly_echo[13];
    i16 grain[442];
    i16 overlap_buf[13][512];
    i16 delay_buf[13][2048];
};
static_assert(sizeof(Shared) == 67888, "Shared size matches definition.json");

void initialize(Voice&, Shared&, u32);
void start(Voice&, Shared&, u32);
void destroy(Voice&, Shared&, u32);
void render(Voice&, Shared&, u32, opxy::Output, opxy::Parameters, opxy::Pitch);
} // namespace ohmonk
