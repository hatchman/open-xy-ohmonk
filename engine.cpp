#include "core.h"
#include <opxy/q31.hpp>
#include "tables.h"

// Fixed-point Q31 port of MonkSynth by Jonathan Taylor (https://github.com/JonET/monksynth),
// MIT License, Copyright (c) 2026 Jonathan Taylor.
// FOF (Formant-Wave-Function) vocal synthesis attributed to Xavier Rodet (IRCAM),
// inspired by the classic AudioNerdz Delay Lama (2002).
//
// 1-Page Curated 4-Macro Architecture (Blackfin Hardware CPU-Optimized):
//   0: Vowel (Primary 5-vowel Catmull-Rom formant sweep with automatic upper-formant warmth contour)
//   1: Shape (Monotonic capped-sweet-spot Body size + 3-Zone Nose size sweep, Neutral at 50%)
//   2: Vibes (Unified sweep of Vibrato depth/rate, Vibrato Delay bloom & sweet-spot Formant Resonance)
//   3: Delay (Modulated 3-Wall Temple Delay: 50.0ms E + 100.0ms L + 147.8ms R)

namespace ohmonk {
namespace {
using opxy::q31::multiply;

inline i32 clamp_pos(i32 v) { return v < 0 ? 0 : v; }

inline i32 clamp_i32(i32 v, i32 lo, i32 hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Fast single-instruction Q31 / Q15 sine lookups (matching 023's ultra-low L3 SDRAM footprint)
inline i32 lookup_sine(u32 phase) {
    return monk_sine_table[phase >> 22];
}

inline i32 lookup_sine_q15(u32 phase) {
    return monk_sine_table[phase >> 22] >> 16;
}

// 4-entry Most-Recently-Used (MRU) voice priority stack packed into 16 bits (4 x 4-bit slot IDs 0..12, 15 = empty):
// The most recently triggered note is at bits 0..3; when it finishes, priority falls back to the next most recent active note.
inline u32 mru_push(u32 stack, u32 b) {
    u32 out = b & 15u;
    u32 shift = 4u;
    for (u32 i = 0u; i < 4u; ++i) {
        const u32 s = (stack >> (i * 4u)) & 15u;
        if (s != b && s < 13u && shift < 16u) {
            out |= (s << shift);
            shift += 4u;
        }
    }
    while (shift < 16u) {
        out |= (15u << shift);
        shift += 4u;
    }
    return out;
}

inline u32 mru_remove(u32 stack, u32 b) {
    u32 out = 0u;
    u32 shift = 0u;
    for (u32 i = 0u; i < 4u; ++i) {
        const u32 s = (stack >> (i * 4u)) & 15u;
        if (s != b && s < 13u && shift < 16u) {
            out |= (s << shift);
            shift += 4u;
        }
    }
    while (shift < 16u) {
        out |= (15u << shift);
        shift += 4u;
    }
    return out;
}

// Evaluates the 3-Zone Shape Curve (shape_q31 = 0..0x7fffffff) into:
// - body_q13 (0..8192, Neutral 4096 at 50% center):
//     Zone 1 (0..25%):  stays constant at 0 (Tiny Body)
//     Zone 2 (25..75%): transitions smoothly 0 -> 8192 (Tiny -> Large Body, 4096 at 50%)
//     Zone 3 (75..100%): stays constant at 8192 (Large Body)
// - nose_q13 (0..8192, Neutral 4096 at 50% center):
//     Zone 1 (0..25%):  sweeps 0 -> 8192 (Thin -> Thick Nose on Tiny Body)
//     Zone 2 (25..75%): sweeps 8192 -> 0 (Thick -> Thin Nose during Tiny -> Large Body transition, 4096 at 50%)
//     Zone 3 (75..100%): sweeps 0 -> 8192 (Thin -> Thick Nose on Large Body)
inline void eval_shape_body_nose_q13(i32 shape_q31, i32& body_q13, i32& nose_q13) {
    const i32 s_q13 = clamp_i32(shape_q31, 0, 0x7fffffff) >> 18; // 0 .. 8191 (center 4096)
    if (s_q13 <= 2048) {
        body_q13 = 0;
        nose_q13 = s_q13 << 2; // 0 -> 8192 (Thin -> Thick Nose on Tiny Body)
    } else if (s_q13 <= 6144) {
        body_q13 = (s_q13 - 2048) << 1;          // 0 -> 8192 (Tiny -> Large Body; 4096 at center)
        nose_q13 = 8192 - ((s_q13 - 2048) << 1); // 8192 -> 0 (Thick -> Thin Nose; 4096 at center)
    } else {
        body_q13 = 8192;
        nose_q13 = (s_q13 - 6144) << 2; // 0 -> 8192 (Thin -> Thick Nose on Large Body)
    }
}

// 257-entry Catmull-Rom formant spline table lookup with linear fractional interpolation
inline u32 lookup_formant(const u32* tbl, i32 vowel_q31) {
    const u32 pos = static_cast<u32>(clamp_i32(vowel_q31, 0, 0x7fffffff)) >> 7;
    const u32 idx = pos >> 16;
    const i32 frac = static_cast<i32>((pos & 0xffffu) << 15);
    const i32 a = static_cast<i32>(tbl[idx] >> 1);
    const i32 b = static_cast<i32>(tbl[idx + 1] >> 1);
    return static_cast<u32>(a + multiply(b - a, frac)) << 1;
}

// Capped sweet-spot vocal-tract formant scaling centered at 1.00x when body_q13 == 4096 (50% = 1.0x):
// - body_q13 = 0    (0.5x Tiny Body)  -> +10.9% formant shift (focused young monk without ever sounding shrill)
// - body_q13 = 4096 (1.0x Neutral)    ->   0.0% formant shift (exact MonkSynth formants)
// - body_q13 = 8192 (2.0x Large Body) -> -15.6% formant shift (deep warm Tibetan vocal tract)
// Uses pure unsigned arithmetic ((span >> 6) * t) >> 6 to guarantee 0% signed Q31 overflow at body_q13 = 0 or 8192!
inline u32 scale_formant_body_q13(u32 inc, i32 body_q13) {
    const i32 b_clamped = clamp_i32(body_q13, 0, 8192);
    if (b_clamped <= 4096) {
        const u32 t = static_cast<u32>(4096 - b_clamped); // 0 .. 4096
        const u32 span = (inc >> 4) + (inc >> 5) + (inc >> 6); // 0.109375 * inc
        return inc + (((span >> 6) * t) >> 6);
    } else {
        const u32 t = static_cast<u32>(b_clamped - 4096); // 0 .. 4096
        const u32 span = (inc >> 3) + (inc >> 5); // 0.15625 * inc
        return inc - (((span >> 6) * t) >> 6);
    }
}

// C1-smooth cubic soft-saturator normalized to standard OP-XY synth level (outputPeakQ31 = 1,073,741,824 = 0.50 Q31):
// +28% stronger linear presence (46,000 vs 36,000) with wide +-31,000 linear/cubic headroom and exact
// horizontal tangent (f'(x) = 0) at |x| = 31,000 (max output = 20,667 * 46,000 = 950,682,000 < 1,073,741,824).
inline i32 monk_clean_output(i32 x_q29) {
    const i32 x = clamp_i32(x_q29 >> 13, -31000, 31000);
    const i32 x2_q15 = (x * x) >> 15;                    // 0 .. 29327
    const i32 x3_scaled = (x * x2_q15) >> 15;            // -27745 .. +27745
    const i32 shaped = x - ((x3_scaled * 12203) >> 15);  // -20667 .. +20667 (exact horizontal tangent at +-31000)
    return shaped * 46000;                               // max magnitude = 20667 * 46000 = 950,682,000 (< 1,073,741,824)
}

inline u32 pack_grain_key(i32 eff_vowel, i32 eff_shape, i32 eff_vibes) {
    return ((static_cast<u32>(eff_vowel) >> 23) << 16) |
           ((static_cast<u32>(eff_shape) >> 23) << 8)  |
           (static_cast<u32>(eff_vibes) >> 26);
}

// Curated 441-sample (10.0 ms) FOF grain shared across all active voices (computed at most ONCE per block!):
// Marked noinline so its register usage never spills the 64-sample hot loop in render().
__attribute__((noinline)) void compute_voice_grain(i16* grain, i32 vowel_q31, i32 shape_q31, i32 vibes_q31) {
    i32 body_q13 = 4096, nose_q13 = 4096;
    eval_shape_body_nose_q13(shape_q31, body_q13, nose_q13);

    const i32 nose_c = nose_q13 - 4096; // -4096 (Thin Nose) .. 0 (Neutral) .. +4096 (Thick Nose)

    // Subtle nasal cavity coupling on F2 (+-3.1%) using overflow-safe arithmetic:
    const u32 raw_f2 = lookup_formant(monk_f2_inc, vowel_q31);
    const i32 f2_span = static_cast<i32>(raw_f2 >> 5);
    const i32 f2_nasal_delta = ((f2_span >> 6) * nose_c) >> 6;
    const u32 f1_inc = scale_formant_body_q13(lookup_formant(monk_f1_inc, vowel_q31), body_q13);
    const u32 f2_inc = scale_formant_body_q13(static_cast<u32>(static_cast<i32>(raw_f2) + f2_nasal_delta), body_q13);
    const u32 f3_inc = scale_formant_body_q13(lookup_formant(monk_f3_inc, vowel_q31), body_q13);

    const i32 v_q13     = clamp_i32(vowel_q31, 0, 0x7fffffff) >> 18; // 0 .. 8191 (AAA = 4096)
    const i32 vibes_q13 = clamp_i32(vibes_q31, 0, 0x7fffffff) >> 18; // 0 .. 8191

    const i32 high_v     = v_q13 > 4096 ? (v_q13 - 4096) : 0;       // 0 .. 4095 (AAA -> EEE -> III)
    const i32 small_body = body_q13 < 4096 ? (4096 - body_q13) : 0; // 0 .. 4096 (1.0x -> 0.5x)
    const i32 large_body = body_q13 > 4096 ? (body_q13 - 4096) : 0; // 0 .. 4096 (1.0x -> 2.0x)
    const i32 vibes_res  = vibes_q13 >> 4;                          // 0 .. +511 warm midrange focus

    const i32 f1_gain_q13 = clamp_i32(
        10200 + (high_v >> 3) + (small_body >> 3) - (large_body >> 4) - (nose_c >> 3) - (vibes_res >> 2),
        8200, 11000);
    const i32 f2_gain_q13 = clamp_i32(
        5600 + (vibes_res >> 1) + (nose_c >> 2) + (large_body >> 4) - ((small_body * 3) >> 4) - ((high_v * 3) >> 4),
        4200, 6600);
    const i32 f3_gain_q13 = clamp_i32(
        1050 + (nose_c >> 3) - (small_body >> 3) - (high_v >> 3) - (vibes_res >> 3),
        550, 1350);
    const i32 breath_q13 = clamp_i32(42 + ((nose_q13 * 56) >> 13), 28, 110);

    u32 p1 = 0u, p2 = 0u, p3 = 0u;
    for (u32 k = 0u; k < 440u; k += 2u) {
        const i32 g1 = (static_cast<i32>(monk_env_f1[k]) * f1_gain_q13) >> 15;
        const i32 g2 = (static_cast<i32>(monk_env_f2[k]) * f2_gain_q13) >> 15;
        const i32 g3 = (static_cast<i32>(monk_env_f3[k]) * f3_gain_q13) >> 15;

        const i32 s0 = lookup_sine_q15(p1) * g1 + lookup_sine_q15(p2) * g2 + lookup_sine_q15(p3) * g3
                     + static_cast<i32>(monk_aspiration_table[k]) * breath_q13;
        grain[k] = static_cast<i16>(s0 >> 16);
        p1 += f1_inc;
        p2 += f2_inc;
        p3 += f3_inc;

        const i32 s1 = lookup_sine_q15(p1) * g1 + lookup_sine_q15(p2) * g2 + lookup_sine_q15(p3) * g3
                     + static_cast<i32>(monk_aspiration_table[k + 1u]) * breath_q13;
        grain[k + 1u] = static_cast<i16>(s1 >> 16);
        p1 += f1_inc;
        p2 += f2_inc;
        p3 += f3_inc;
    }
    grain[440] = 0;
    grain[MONK_GRAIN_LEN] = 0;
}

// Cold-path glottal carrier wrap handler (runs ~once every 5-20 blocks per voice).
// Passed in 3 registers (R0 = &shared, R1 = ola_buf, R2 = start_idx | (damp_step_q8 << 16))
// so the 64-sample hot loop in render() stays completely free of register pressure and stack spills.
__attribute__((noinline)) void stamp_grain(Shared& shared, i16* const ola_buf, u32 packed_idx_damp) {
    const i32 eff_vowel = clamp_i32(shared.vowel_smooth, 0, 0x7fffffff);
    const i32 eff_shape = clamp_i32(shared.size_smooth,  0, 0x7fffffff);
    const i32 eff_vibes = clamp_i32(shared.reson_smooth, 0, 0x7fffffff);
    const u32 grain_key = pack_grain_key(eff_vowel, eff_shape, eff_vibes);
    i16* const grn_buf  = shared.grain;

    if (shared.grain_key != grain_key && (shared.active_block_mask & 0x80000000u) == 0u) {
        shared.active_block_mask |= 0x80000000u;
        compute_voice_grain(grn_buf, eff_vowel, eff_shape, eff_vibes);
        shared.grain_key = grain_key;
    }

    const u32 start_idx    = packed_idx_damp & 511u;
    const i32 damp_step_q8 = static_cast<i32>(packed_idx_damp >> 16);

    if (damp_step_q8 == 0) {
        u32 chunk1 = 512u - start_idx;
        if (chunk1 > MONK_GRAIN_LEN) chunk1 = MONK_GRAIN_LEN;
        const u32 chunk2 = MONK_GRAIN_LEN - chunk1;

        for (u32 j = 0u; j < chunk1; ++j) {
            ola_buf[start_idx + j] = static_cast<i16>(ola_buf[start_idx + j] + grn_buf[j]);
        }
        for (u32 j = 0u; j < chunk2; ++j) {
            ola_buf[j] = static_cast<i16>(ola_buf[j] + grn_buf[chunk1 + j]);
        }
    } else {
        const i32 tail_damp_q8 = 256 - (damp_step_q8 << 5);
        i32 d_q8 = 256;
        for (u32 j = 0u; j < 32u; ++j) {
            const u32 idx = (start_idx + j) & 511u;
            ola_buf[idx] = static_cast<i16>(((static_cast<i32>(ola_buf[idx]) * d_q8) >> 8) + grn_buf[j]);
            d_q8 -= damp_step_q8;
        }
        for (u32 j = 32u; j < MONK_GRAIN_LEN; ++j) {
            const u32 idx = (start_idx + j) & 511u;
            ola_buf[idx] = static_cast<i16>(((static_cast<i32>(ola_buf[idx]) * tail_damp_q8) >> 8) + grn_buf[j]);
        }
    }
}
} // namespace

inline i32 step_smooth(i32 cur, i32 target) {
    const i32 diff = target - cur;
    if (diff >= -16 && diff <= 16) return target;
    return cur + (diff >> 2);
}

inline void initialize(Voice& voice, Shared& shared, u32) {
    if (shared.initialized != 0x5a5aa5a5u) {
        shared.in_use_mask = 0u;
        shared.active_block_mask = 0xffffu; // 4-entry MRU stack initialized to empty [15, 15, 15, 15]
        shared.grain_key = 0xFFFFFFFFu;
        shared.vowel_smooth = 0x3d70a3d7;
        shared.size_smooth  = 0x40000000;
        shared.reson_smooth = 0x28f5c28f;
        for (u32 b = 0u; b < 13u; ++b) {
            shared.needs_clear[b] = 0u;
            shared.last_carrier_phase[b] = 0xFFFFFFFFu - (b * 0x00800000u);
            shared.overlap_read_pos[b] = 0u;
            shared.delay_write_idx[b] = 0u;
            shared.current_delay_l_q8[b] = 0;
            shared.current_delay_r_q8[b] = 0;
            shared.dly_in_prev[b] = 0;
            shared.dly_echo[b] = 0;
            for (u32 k = 0u; k < 512u; ++k) {
                shared.overlap_buf[b][k] = 0;
            }
        }
        shared.initialized = 0x5a5aa5a5u;
    }

    voice.buf_id = 0xFFFFFFFFu;
    voice.carrier_phase = 0xFFFFFFFFu;
    voice.vib_phase = 0u;
    voice.rng_state = 12345u;
    voice.held_blocks = 0u;
    voice.random_jitter_q16 = 6 << 16;
    voice.target_jitter_q16 = 6 << 16;
    voice.jitter_counter = 0u;
    voice.vowel_smooth = 0;
    voice.reson_smooth = 0;
    voice.size_smooth = 0;
    voice.breath_smooth = 0;
    voice.last_grain_key = 0xFFFFFFFFu;
    voice.snap_params = 1u;
}

inline void start(Voice& voice, Shared& shared, u32) {
    u32 b = voice.buf_id;
    if (b >= 13u) {
        b = 0u;
        for (u32 i = 0u; i < 13u; ++i) {
            if ((shared.in_use_mask & (1u << i)) == 0u) {
                b = i;
                break;
            }
        }
        shared.in_use_mask |= (1u << b);
        voice.buf_id = b;
        voice.vib_phase = b * 0x33333333U;
        voice.rng_state = 12345u + b * 7919u;
        voice.random_jitter_q16 = (5 << 16) + static_cast<i32>((b & 3u) * 21845u);
        voice.target_jitter_q16 = voice.random_jitter_q16;
        voice.jitter_counter = (b & 3u) * 1146u;
    }

    const u32 keep_seen = (shared.active_block_mask & 0x1fff0000u) & ~(1u << (16u + b));
    shared.active_block_mask = keep_seen | mru_push(shared.active_block_mask & 0xffffu, b);

    // Always start every new note from a 100% clean grain & delay state so hardware's
    // fast 0.1ms ADSR attack never opens onto leftover pre-envelope waveform or filter memory:
    shared.needs_clear[b] = 0u;
    shared.overlap_read_pos[b] = 0u; // ola_pos = 0, prev_gain_q14 = 0 (ramps 0 -> grain_gain over 64 samples!)
    shared.delay_write_idx[b] = 0u;
    shared.current_delay_l_q8[b] = 0;
    shared.current_delay_r_q8[b] = 0;
    shared.dly_in_prev[b] = 0;
    shared.dly_echo[b] = 0;
    u32* const ola_words = reinterpret_cast<u32*>(shared.overlap_buf[b]);
    for (u32 k = 0u; k < 256u; ++k) {
        ola_words[k] = 0u;
    }

    voice.carrier_phase = 0xFFFFFFFFu;
    shared.last_carrier_phase[b] = 0xFFFFFFFFu;
    voice.vowel_smooth = 0;
    voice.reson_smooth = 0;
    voice.size_smooth = 0;
    voice.breath_smooth = 0;
    voice.held_blocks = 0u;
    voice.snap_params = 1u;
}

inline void destroy(Voice& voice, Shared& shared, u32) {
    if (voice.buf_id < 13u) {
        const u32 b = voice.buf_id;
        shared.in_use_mask &= ~(1u << b);
        const u32 keep_seen = (shared.active_block_mask & 0x1fff0000u) & ~(1u << (16u + b));
        shared.active_block_mask = keep_seen | mru_remove(shared.active_block_mask & 0xffffu, b);
        shared.needs_clear[b] = 1u;
        voice.buf_id = 0xFFFFFFFFu;
    }
}

inline void render(Voice& voice, Shared& shared, u32, opxy::Output output,
                   opxy::Parameters parameters, opxy::Pitch pitch) {
    const i32 target_vowel = clamp_pos(parameters[0]);
    const i32 target_shape = clamp_pos(parameters[1]);
    const i32 target_vibes = clamp_pos(parameters[2]);
    const i32 delay_mix    = clamp_pos(parameters[3]);

    const u32 b = (voice.buf_id < 13u) ? voice.buf_id : 0u;
    const u32 b_seen_bit = 1u << (16u + b);
    if ((shared.active_block_mask & b_seen_bit) != 0u) {
        shared.active_block_mask &= 0xffffu;
    }
    shared.active_block_mask |= b_seen_bit;

    u32 lead_b = shared.active_block_mask & 15u;
    if (lead_b >= 13u || (shared.in_use_mask & (1u << lead_b)) == 0u) {
        lead_b = b;
    }

    if (voice.snap_params != 0u) {
        if (b == lead_b) {
            shared.vowel_smooth = target_vowel;
            shared.size_smooth  = target_shape;
            shared.reson_smooth = target_vibes;
        }
        voice.snap_params = 0u;
    } else if (b == lead_b) {
        shared.vowel_smooth = step_smooth(shared.vowel_smooth, target_vowel);
        shared.size_smooth  = step_smooth(shared.size_smooth,  target_shape);
        shared.reson_smooth = step_smooth(shared.reson_smooth, target_vibes);
    }

    // Update organic per-voice random walk (5..7 Hz vibrato rate jitter)
    voice.jitter_counter += 64u;
    if (voice.jitter_counter >= 4586u) {
        voice.jitter_counter = 0u;
        voice.rng_state = voice.rng_state * 1103515245u + 12345u;
        const i32 rand_0_2_q16 = static_cast<i32>((voice.rng_state >> 15) & 0x1ffffu);
        voice.target_jitter_q16 = (5 << 16) + rand_0_2_q16;
    }
    voice.random_jitter_q16 += (voice.target_jitter_q16 - voice.random_jitter_q16) >> 6;
    const i32 organic_walk = voice.random_jitter_q16 - (6 << 16);

    const i32 eff_vowel = clamp_i32(shared.vowel_smooth, 0, 0x7fffffff);
    const i32 eff_shape = clamp_i32(shared.size_smooth,  0, 0x7fffffff);
    const i32 eff_vibes = clamp_i32(shared.reson_smooth, 0, 0x7fffffff);

    if (shared.grain_key == 0xFFFFFFFFu) {
        compute_voice_grain(shared.grain, eff_vowel, eff_shape, eff_vibes);
        shared.grain_key = pack_grain_key(eff_vowel, eff_shape, eff_vibes);
    }

    // Unified Vibes Macro (parameters[2]):
    const i32 v_norm = target_vibes >> 21; // 0 .. 1024
    i32 onset_q30;
    if (__builtin_expect(voice.held_blocks >= 248u, 1)) {
        if (voice.held_blocks < 2048u) voice.held_blocks += 1u;
        onset_q30 = 0x40000000;
    } else {
        voice.held_blocks += 1u;
        const u32 hold_limit = static_cast<u32>(12 + (((1024 - v_norm) * 52) >> 10)); // 64..12 blocks (92ms..17ms)
        const u32 ramp_step_q15 = static_cast<u32>(180 + ((v_norm * 260) >> 10));     // 180..440
        const i32 init_onset_q30 = (v_norm << 18);                                    // Immediate presence + natural bloom
        onset_q30 = init_onset_q30;
        if (voice.held_blocks > hold_limit) {
            const u32 raw_t_q15 = (voice.held_blocks - hold_limit) * ramp_step_q15;
            if (raw_t_q15 >= 32768u) {
                onset_q30 = 0x40000000;
            } else {
                const i32 t_q15 = static_cast<i32>(raw_t_q15);
                const i32 t2_q15 = (t_q15 * t_q15) >> 15;
                const i32 s_q15 = (t2_q15 * ((3 << 15) - (t_q15 << 1))) >> 15;
                const i32 span_q30 = 0x40000000 - init_onset_q30;
                onset_q30 = init_onset_q30 + multiply(span_q30, s_q15 << 16);
            }
        }
    }

    // Transpose 1 octave down (pitch[0] >> 1) into the authentic Tibetan baritone/bass register
    const u32 raw_base_pitch = pitch[0] >> 1;

    // Once-per-block Vibrato endpoints + per-sample linear step interpolation (0 table reads inside 64-sample loop!):
    const i32 v_top = v_norm > 512 ? (v_norm - 512) : 0;
    const u32 jitter_inc = static_cast<u32>(organic_walk * 14);
    const u32 vib_block_inc = 31500000u + jitter_inc + (static_cast<u32>(eff_vibes) >> 7) + (static_cast<u32>(v_top) << 14);
    const i32 d_norm = delay_mix >> 21; // 0 .. 1024
    const i32 dly_vib_tame_q14 = 16384 - ((d_norm * 2457) >> 10); // 1.00x -> 0.85x at max Delay
    const i32 raw_eff_vib      = multiply(eff_vibes, onset_q30) << 1;
    const i32 eff_vib          = (raw_eff_vib >> 14) * dly_vib_tame_q14;
    const i32 eff_vib_sq       = multiply(eff_vib, eff_vib);
    const i32 vib_depth_q31    = (eff_vib >> 4) + (eff_vib_sq >> 3); // Pronounced top-end (+-40.5 cents at 100%)

    const i32 vib_sine_0 = lookup_sine(voice.vib_phase);
    voice.vib_phase += vib_block_inc;
    const i32 vib_sine_1 = lookup_sine(voice.vib_phase);

    const i32 vib_mod_0 = (vib_sine_0 >> 11) + multiply(vib_sine_0, vib_depth_q31);
    const i32 vib_mod_1 = (vib_sine_1 >> 11) + multiply(vib_sine_1, vib_depth_q31);

    // Subtle Choir Stage ensemble tuning spread per voice slot (static constexpr avoids 52-byte stack copy!):
    static constexpr signed char STAGE_TUNE_NUDGE[13] = {0, -8, +8, -15, +15, -10, +10, -12, +12, -6, +6, -14, +14};
    const i32 tune_delta = (static_cast<i32>(raw_base_pitch >> 11) * static_cast<i32>(STAGE_TUNE_NUDGE[b])) >> 3;

    const i32 mod_delta_0 = multiply(static_cast<i32>(raw_base_pitch >> 1), vib_mod_0) >> 2;
    const i32 mod_delta_1 = multiply(static_cast<i32>(raw_base_pitch >> 1), vib_mod_1) >> 2;
    const i32 inc_0       = static_cast<i32>(raw_base_pitch) + tune_delta + mod_delta_0;
    const i32 inc_step    = (mod_delta_1 - mod_delta_0) >> 6;

    // Zero-division pitch-dependent amplitude compensation via 65-entry interpolated LUT (from voice.c)
    const u32 p_scaled = raw_base_pitch >> 8;
    const u32 p_idx = (p_scaled >> 12) < 64u ? (p_scaled >> 12) : 63u;
    const i32 p_frac = static_cast<i32>(p_scaled & 0xfffu);
    const i32 g0 = static_cast<i32>(monk_pitch_gain_q14[p_idx]);
    const i32 g1 = static_cast<i32>(monk_pitch_gain_q14[p_idx + 1u]);
    const i32 grain_gain_q14 = g0 + (((g1 - g0) * p_frac) >> 12);

    const i32 damp_step_q8 = (p_scaled > 50000u)
        ? clamp_i32(static_cast<i32>((p_scaled - 50000u) >> 14), 0, 7)
        : 0;
    const u32 damp_packed_hi = static_cast<u32>(damp_step_q8) << 16;

    const u32 ola_pos0 = shared.overlap_read_pos[b] & 511u;
    i16* const ola_buf = shared.overlap_buf[b];

    // Pass 1: Glottal carrier phase advance + FOF grain stamping.
    // Because block_frames + MONK_GRAIN_LEN - 1 = 504 <= 512, any grain stamped at sample i (0..63)
    // writes only to relative offsets [i, i + 440] mod 512, which is 100% disjoint from [0, i - 1] mod 512.
    // Moreover, for all pitches where inc_0 < 0x01800000, we can test in O(1) whether the 64-sample block wraps at all!
    u32 c_phase = voice.carrier_phase;
    const u32 total_phase_inc = (static_cast<u32>(inc_0) << 6) + static_cast<u32>(inc_step * 2016);
    const u32 next_c_phase = c_phase + total_phase_inc;
    if (__builtin_expect(static_cast<u32>(inc_0) < 0x01800000u && next_c_phase >= c_phase, 1)) {
        c_phase = next_c_phase;
    } else {
        i32 cur_inc = inc_0;
        for (unsigned i = 0; i != opxy::block_frames; ++i) {
            const u32 inc = static_cast<u32>(cur_inc);
            cur_inc += inc_step;
            const u32 prev_carrier = c_phase;
            c_phase += inc;
            if (__builtin_expect(c_phase < prev_carrier, 0)) {
                stamp_grain(shared, ola_buf, ((ola_pos0 + i) & 511u) | damp_packed_hi);
            }
        }
    }
    voice.carrier_phase          = c_phase;
    shared.last_carrier_phase[b] = c_phase;

    // Pass 2 & 3: OLA read + 1-zero Nyquist warmth filter + 11.025 kHz 3-Wall Stone Temple Delay + cubic soft-clipper.
    // Completely free of function calls, carrier phase state, and grain-stamping branches!
    const i32 prev_gain_q14  = static_cast<i32>(shared.overlap_read_pos[b] >> 16);
    const i32 start_gain_q14 = prev_gain_q14;
    const i32 gain_step_q8   = ((grain_gain_q14 - start_gain_q14) << 8) >> 6;
    i32 cur_gain_q22         = start_gain_q14 << 8;

    const i32 dry_gain_q14  = 15000 - ((d_norm * 3200) >> 10);      // 0.92x at Delay=0 -> 0.72x at Delay=100%
    const i32 d_curved      = (d_norm + ((d_norm * d_norm) >> 11)); // Responsive early-to-full delay curve (0 .. 1536)
    const i32 delay_mix_q14 = (d_curved * 6200) >> 10;              // 0.00x at Delay=0 -> 0.57x at Delay=100%
    const i32 fb_q14        = 5898 + ((d_norm * 2949) >> 10);       // 36% at Delay=0 -> 54% at Delay=100% (single-tap clean!)

    const u32 TAP_E = 551u;  //  50.0 ms @ 11.025 kHz (early stone temple wall, feed-forward only)
    const u32 TAP_L = 1102u; // 100.0 ms @ 11.025 kHz (left stone temple wall, stationary feed-forward)
    const u32 TAP_R = 1630u; // 147.8 ms @ 11.025 kHz (right stone temple wall, stationary feedback + output)

    i32 lp1_q29       = shared.current_delay_l_q8[b];
    i32 lp2_q29       = shared.current_delay_r_q8[b];
    i32 dly_in_prev   = shared.dly_in_prev[b];
    i32 dly_echo      = shared.dly_echo[b];
    i32 dly_in_lp     = voice.vowel_smooth;
    i32 dly_s1_q29    = voice.size_smooth;
    i32 dly_s2_q29    = voice.breath_smooth;
    i32 dly_lin_q29   = lp2_q29;
    i32 echo_step_q29 = 0;
    u32 wr            = shared.delay_write_idx[b];

    i16* const dly_buf = shared.delay_buf[b];
    i16* ola_ptr       = &ola_buf[ola_pos0];
    i32* out_ptr       = output;

    auto finish_sample = [&](i32 warm_dry_q29) __attribute__((always_inline)) {
        dly_lin_q29 += echo_step_q29;
        dly_s1_q29  += (dly_lin_q29 - dly_s1_q29) >> 2;
        dly_s2_q29  += (dly_s1_q29  - dly_s2_q29) >> 2;
        dly_echo    += (dly_s2_q29  - dly_echo)   >> 2;

        const i32 scaled_dry_q29 = (warm_dry_q29 >> 14) * dry_gain_q14;
        const i32 wet_q29        = (dly_echo >> 14) * delay_mix_q14;
        *out_ptr++ = scaled_dry_q29 + wet_q29;
    };

    const u32 idx_w = wr & 2047u;
    const u32 idx_e = (wr - TAP_E) & 2047u;
    const u32 idx_l = (wr - TAP_L) & 2047u;
    const u32 idx_r = (wr - TAP_R) & 2047u;

    if (__builtin_expect(gain_step_q8 == 0 && delay_mix_q14 != 0 && wr >= TAP_R + 128u &&
                         idx_w <= 2032u && idx_e <= 2032u && idx_l <= 2032u && idx_r <= 2032u, 1)) {
        // Branchless, maskless steady-state fast path:
        // All 4 delay taps advance linearly via post-increment pointers with zero & 2047 masking!
        const i32 gain_x2 = start_gain_q14 << 1;
        i16* ptr_w       = &dly_buf[idx_w];
        const i16* ptr_e = &dly_buf[idx_e];
        const i16* ptr_l = &dly_buf[idx_l];
        const i16* ptr_r = &dly_buf[idx_r];
        wr += 16u;

        for (unsigned q = 0; q != 16u; ++q) {
            const i32 raw_ola_0 = static_cast<i32>(*ola_ptr);
            *ola_ptr++ = 0;
            const i32 raw_dry_0 = raw_ola_0 * gain_x2;
            const i32 warm_dry_0 = (raw_dry_0 >> 1) + (dly_in_prev >> 1);
            dly_in_lp += (warm_dry_0 - dly_in_lp) >> 2;

            dly_lin_q29 = lp2_q29;
            const i32 tap_e_q29 = static_cast<i32>(*ptr_e++) << 15;
            const i32 tap_l_q29 = static_cast<i32>(*ptr_l++) << 15;
            const i32 tap_r_q29 = static_cast<i32>(*ptr_r++) << 15;

            lp1_q29 += (tap_r_q29 - lp1_q29) >> 1;
            const i32 fb_q29 = (lp1_q29 >> 14) * fb_q14;
            const i32 in_q29 = dly_in_lp + fb_q29;
            *ptr_w++ = static_cast<i16>(clamp_i32(in_q29 >> 15, -32767, 32767));

            const i32 sum_echo_q29 = ((tap_e_q29 >> 3) * 3) + (((tap_l_q29 >> 4) + (tap_r_q29 >> 4)) * 9);
            const i32 raw_echo_q29 = sum_echo_q29 + (sum_echo_q29 >> 2);
            lp2_q29 += (raw_echo_q29 - lp2_q29) >> 1;
            echo_step_q29 = (lp2_q29 - dly_lin_q29) >> 2;
            finish_sample(warm_dry_0);

            const i32 raw_ola_1 = static_cast<i32>(*ola_ptr);
            *ola_ptr++ = 0;
            const i32 raw_dry_1 = raw_ola_1 * gain_x2;
            const i32 warm_dry_1 = (raw_dry_1 >> 1) + (raw_dry_0 >> 1);
            dly_in_lp += (warm_dry_1 - dly_in_lp) >> 2;
            finish_sample(warm_dry_1);

            const i32 raw_ola_2 = static_cast<i32>(*ola_ptr);
            *ola_ptr++ = 0;
            const i32 raw_dry_2 = raw_ola_2 * gain_x2;
            const i32 warm_dry_2 = (raw_dry_2 >> 1) + (raw_dry_1 >> 1);
            dly_in_lp += (warm_dry_2 - dly_in_lp) >> 2;
            finish_sample(warm_dry_2);

            const i32 raw_ola_3 = static_cast<i32>(*ola_ptr);
            *ola_ptr++ = 0;
            const i32 raw_dry_3 = raw_ola_3 * gain_x2;
            const i32 warm_dry_3 = (raw_dry_3 >> 1) + (raw_dry_2 >> 1);
            dly_in_prev = raw_dry_3;
            dly_in_lp += (warm_dry_3 - dly_in_lp) >> 2;
            finish_sample(warm_dry_3);
        }
    } else {
        for (unsigned q = 0; q != 16u; ++q) {
            for (unsigned s = 0; s != 4u; ++s) {
                const i32 raw_ola = static_cast<i32>(*ola_ptr);
                *ola_ptr++ = 0;

                const i32 raw_dry_q29 = (raw_ola * (cur_gain_q22 >> 8)) << 1;
                cur_gain_q22 += gain_step_q8;
                const i32 warm_dry_q29 = (raw_dry_q29 >> 1) + (dly_in_prev >> 1);
                dly_in_prev = raw_dry_q29;

                dly_in_lp += (warm_dry_q29 - dly_in_lp) >> 2;
                if (s == 0u) {
                    dly_lin_q29 = lp2_q29;
                    if (delay_mix_q14 == 0 && lp1_q29 == 0 && lp2_q29 == 0) {
                        dly_buf[wr & 2047u] = static_cast<i16>(clamp_i32(dly_in_lp >> 15, -32767, 32767));
                        wr += 1u;
                        echo_step_q29 = 0;
                    } else {
                        i32 tap_e_q29 = 0;
                        if (wr > TAP_E) {
                            const i32 fade_e = clamp_i32(static_cast<i32>(wr - TAP_E) << 1, 0, 256);
                            tap_e_q29 = (static_cast<i32>(dly_buf[(wr - TAP_E) & 2047u]) * fade_e) << 7;
                        }
                        i32 tap_l_q29 = 0;
                        if (wr > TAP_L) {
                            const i32 fade_l = clamp_i32(static_cast<i32>(wr - TAP_L) << 1, 0, 256);
                            tap_l_q29 = (static_cast<i32>(dly_buf[(wr - TAP_L) & 2047u]) * fade_l) << 7;
                        }
                        i32 tap_r_q29 = 0;
                        if (wr > TAP_R) {
                            const i32 fade_r = clamp_i32(static_cast<i32>(wr - TAP_R) << 1, 0, 256);
                            tap_r_q29 = (static_cast<i32>(dly_buf[(wr - TAP_R) & 2047u]) * fade_r) << 7;
                        }

                        lp1_q29 += (tap_r_q29 - lp1_q29) >> 1;
                        const i32 fb_q29 = (lp1_q29 >> 14) * fb_q14;
                        const i32 in_q29 = dly_in_lp + fb_q29;
                        dly_buf[wr & 2047u] = static_cast<i16>(clamp_i32(in_q29 >> 15, -32767, 32767));
                        wr += 1u;

                        const i32 sum_echo_q29 = ((tap_e_q29 >> 3) * 3) + (((tap_l_q29 >> 4) + (tap_r_q29 >> 4)) * 9);
                        const i32 raw_echo_q29 = sum_echo_q29 + (sum_echo_q29 >> 2);
                        lp2_q29 += (raw_echo_q29 - lp2_q29) >> 1;
                        echo_step_q29 = (lp2_q29 - dly_lin_q29) >> 2;
                    }
                }
                finish_sample(warm_dry_q29);
            }
        }
    }

    for (unsigned i = 0; i != opxy::block_frames; ++i) {
        output[i] = monk_clean_output(output[i]);
    }

    shared.current_delay_l_q8[b] = lp1_q29;
    shared.current_delay_r_q8[b] = lp2_q29;
    shared.dly_in_prev[b]        = dly_in_prev;
    shared.dly_echo[b]           = dly_echo;
    shared.delay_write_idx[b]    = wr;
    shared.overlap_read_pos[b]   = ((ola_pos0 + 64u) & 511u) | (static_cast<u32>(grain_gain_q14) << 16);
    voice.vowel_smooth           = dly_in_lp;
    voice.size_smooth            = dly_s1_q29;
    voice.breath_smooth          = dly_s2_q29;
}
} // namespace ohmonk
