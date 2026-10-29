#include "display.h"

namespace ohmonk {
namespace {
using opxy::draw::Node;
using opxy::draw::Point;
using opxy::draw::floor_q10;
inline i32 q10(i32 v) { return v << 10; }

// 13 blended 3-letter formant states across the 5 cardinal vowels (O -> U -> A -> E -> I)
const char* const VOWEL_BLENDS[13] = {
    "OOO", "OOU", "OUU", "UUU", "UUA", "UAA", "AAA", "AAE", "AEE", "EEE", "EEI", "EII", "III"
};

// Evaluates the 3-Zone Shape Curve (shape_1024 = 0..1024, center 512 = 50%):
// - Zone 1 (0..256, first 1/4):    Tiny Body stays constant (0),      Nose sweeps Thin -> Thick (0 -> 1024)
// - Zone 2 (256..768, middle 1/2): Body sweeps Tiny -> Large (0 -> 1024), Nose sweeps Thick -> Thin (1024 -> 0)
//                                  (At shape_1024 == 512 center: Body = 512 Neutral 1.0x, Nose = 512 Neutral 1.0x!)
// - Zone 3 (768..1024, last 1/4):  Large Body stays constant (1024),  Nose sweeps Thin -> Thick (0 -> 1024)
// Maps Body to 19 stepless horizontal-only nodes (Nodes 96..114, center Node 105)
// and Nose to 13 smooth nose nodes (Nodes 126..138, center Node 132).
inline void eval_shape_3zone_nodes(i32 shape_1024, u32& active_body, u32& active_nose) {
    const i32 s = shape_1024 < 0 ? 0 : (shape_1024 > 1024 ? 1024 : shape_1024);
    i32 body_1024 = 512;
    i32 nose_1024 = 512;
    if (s <= 256) {
        body_1024 = 0;
        nose_1024 = s << 2; // 0 -> 1024 (Thin -> Thick on Tiny Body)
    } else if (s <= 768) {
        body_1024 = (s - 256) << 1;          // 0 -> 1024 (Tiny -> Large Body; 512 at s=512)
        nose_1024 = 1024 - ((s - 256) << 1); // 1024 -> 0 (Thick -> Thin Nose; 512 at s=512)
    } else {
        body_1024 = 1024;
        nose_1024 = (s - 768) << 2; // 0 -> 1024 (Thin -> Thick on Large Body)
    }
    const i32 b_step = (body_1024 * 18 + 512) >> 10;
    const i32 n_step = (nose_1024 * 12 + 512) >> 10;
    active_body = 96u + static_cast<u32>(b_step < 0 ? 0 : (b_step > 18 ? 18 : b_step));
    active_nose = 126u + static_cast<u32>(n_step < 0 ? 0 : (n_step > 12 ? 12 : n_step));
}

// Stock OP-XY 00..99 two-digit header formatter (matches sdk/platform/opxy-1.2.1/synth_controls.cpp:224)
inline void format_00_99(char* buf, i32 q31_val) {
    const u32 raw = q31_val < 0 ? 0u : (((static_cast<u32>(q31_val) >> 8) * 100u) >> 23);
    const u32 n = raw > 99u ? 99u : raw;
    buf[0] = static_cast<char>('0' + (n / 10u));
    buf[1] = static_cast<char>('0' + (n % 10u));
    buf[2] = '\0';
}

inline i32 clamp_d(i32 v, i32 lo, i32 hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline i32 abs_d(i32 v) { return v < 0 ? -v : v; }

inline i32 param_1024(i32 q31_val) {
    const i32 v = q31_val < 0 ? 0 : q31_val;
    return clamp_d(v >> 21, 0, 1024);
}

// Positions or hides one of the two 13-node Side-Profile Choir Monks (base_node = 230 for Left, 243 for Right)
inline void draw_side_choir_monk(Frame& frame, u32 base_node, i32 slide_q10, i32 raw_x_monk,
                                 i32 side_env, i32 brow_lift, const opxy::draw::Turn& brow_turn) {
    if (slide_q10 < 16) {
        for (u32 n = base_node; n < base_node + 13u; ++n) {
            opxy::draw::hide(frame, n);
        }
        return;
    }

    const i32 x_monk = (slide_q10 >= 940) ? raw_x_monk : ((raw_x_monk + 2) & ~3);

    const i32 JAW_DROPS[5] = {0, 3, 5, 7, 9};
    u32 side_o_step = 0u;
    if (side_env >= 80) {
        side_o_step = static_cast<u32>(1 + clamp_d(((side_env - 80) * 4) / 945, 0, 3));
    }
    const i32 jaw_drop_side = JAW_DROPS[side_o_step];
    const i32 y_head = 112;

    // +0: Torso, Neck & Head Black Silhouette Mask at y=112 (Shade 8: #000000)
    frame.nodes[base_node + 0u].x = q10(x_monk);
    frame.nodes[base_node + 0u].y = q10(y_head);
    frame.nodes[base_node + 0u].shade = 8;

    // +1: Front Throat Line at y=180 (Shade 1: #B4B4B4)
    frame.nodes[base_node + 1u].x = q10(x_monk);
    frame.nodes[base_node + 1u].y = q10(180);
    frame.nodes[base_node + 1u].shade = 1;

    // +2: True Side-Profile Robe Body & Clean Single-Y Collar at y=180 (Shade 3: #828282)
    frame.nodes[base_node + 2u].x = q10(x_monk);
    frame.nodes[base_node + 2u].y = q10(180);
    frame.nodes[base_node + 2u].shade = 3;

    // +3: Lower Jaw & Chin at y=112 + jaw_drop_side (Shade 1: #B4B4B4)
    frame.nodes[base_node + 3u].x = q10(x_monk);
    frame.nodes[base_node + 3u].y = q10(y_head + jaw_drop_side);
    frame.nodes[base_node + 3u].shade = 1;

    // +4: Upper Cranium, Seamless Nape of Neck, Domed Forehead, Nose & Upper Lip at y=112 (Shade 1: #B4B4B4)
    frame.nodes[base_node + 4u].x = q10(x_monk);
    frame.nodes[base_node + 4u].y = q10(y_head);
    frame.nodes[base_node + 4u].shade = 1;

    // +5: Side-Profile Ear at y=112 (Shade 1: #B4B4B4)
    frame.nodes[base_node + 5u].x = q10(x_monk);
    frame.nodes[base_node + 5u].y = q10(y_head);
    frame.nodes[base_node + 5u].shade = 1;

    // +6: Side-Profile Eye with Half-Ellipse Iris at y=100 (Shade 1: #B4B4B4)
    frame.nodes[base_node + 6u].x = q10(x_monk);
    frame.nodes[base_node + 6u].y = q10(100);
    frame.nodes[base_node + 6u].shade = 1;

    // +7: Side-Profile Arched Eyebrow at x_monk +- 21, y=87 - brow_lift (Shade 0: #CDCDCD)
    const i32 brow_dx = (base_node == 230u) ? 21 : -21;
    frame.nodes[base_node + 7u].x = q10(x_monk + brow_dx);
    frame.nodes[base_node + 7u].y = q10(87 - brow_lift);
    frame.nodes[base_node + 7u].cos = brow_turn.cos;
    frame.nodes[base_node + 7u].sin = brow_turn.sin;
    frame.nodes[base_node + 7u].shade = 0;

    // +8..+12: 5 Open C-Curve Vowel Mouth Steps at y=112 (Shade 0: #CDCDCD)
    const u32 active_m = base_node + 8u + side_o_step;
    for (u32 m = base_node + 8u; m <= base_node + 12u; ++m) {
        if (m == active_m) {
            frame.nodes[m].x = q10(x_monk);
            frame.nodes[m].y = q10(y_head);
            frame.nodes[m].shade = 0;
        } else {
            opxy::draw::hide(frame, m);
        }
    }
}
} // namespace

void display_reset(DisplayState& state) {
    state.vowel_s = 490;
    state.reson_s = 512;
    state.vib_s = 328;
    state.delay_s = 563;
    state.breath_s = 512;
    state.size_s = 512;
    state.rate_s = 512;
    state.wave_phase = 0;
    state.note_env = 0;
    state.mouth_env = 760;
    state.ring_env = 1024;
    state.last_vowel_in = -1;
    state.last_ring_sig = -1;
    state.vowel_hold_ticks = 18u;
    state.ring_hold_ticks = 24u;
    state.last_serial = 0u;
    state.side_l_slide = 0;
    state.side_r_slide = 0;
    state.side_l_env = 0;
    state.side_r_env = 0;
    state.side_l_hold_ticks = 0u;
    state.side_r_hold_ticks = 0u;
    state.vib_bloom_ticks = 0u;
    state.vib_onset = 1024;
    state.last_vibdly_in = -1;
}

void display(DisplayState& state, const opxy::draw::Input& input, Frame& frame) {
    // 1-Page Curated 4-Macro Controls:
    // Slot 0: Vowel (OOO .. III)
    // Slot 1: Shape (0 .. 100 sweeping through 3-Zone Body size & Nose size, Neutral at 50)
    // Slot 2: Vibes (0 .. 100 sweeping Vibrato, Vibrato Delay bloom & Resonance ring phase)
    // Slot 3: Delay (0 .. 100 single-slider Stone Temple Delay & 1..8 concentric rings)
    const i32 p_vowel = param_1024(input.slots[0]);
    const i32 p_shape = param_1024(input.slots[1]);
    const i32 p_vibes = param_1024(input.slots[2]);
    const i32 p_delay = param_1024(input.slots[3]);

    state.vowel_s = p_vowel;
    if (state.last_vowel_in < 0) {
        state.size_s  = p_shape;
        state.vib_s   = p_vibes;
        state.delay_s = p_delay;
    } else {
        auto slew_param = [](i32 target, i32 cur) -> i32 {
            if (abs_d(target - cur) <= 1) return target;
            const i32 step = (target - cur + (target > cur ? 3 : -3)) >> 2;
            return cur + ((step != 0) ? step : ((target > cur) ? 1 : -1));
        };
        state.size_s  = slew_param(p_shape, state.size_s);
        state.vib_s   = slew_param(p_vibes, state.vib_s);
        state.delay_s = slew_param(p_delay, state.delay_s);
    }

    const i32 vowel = clamp_d(state.vowel_s, 0, 1024);
    const i32 shape = clamp_d(state.size_s,  0, 1024);
    const i32 vibes = clamp_d(state.vib_s,   0, 1024);
    const i32 delay = clamp_d(state.delay_s, 0, 1024);

    // Format the 4 macro headers (Vowel: OOO..III; Shape, Vibes, Delay: 00..99 matching factory OP-XY synths):
    for (u32 pos = 0u; pos < 4u; ++pos) {
        const u32 slot = input.shown[pos];
        char hbuf[8] = "";
        const char* val_str = "";
        if (slot == 0u) {
            const u32 v_idx = static_cast<u32>(clamp_d((p_vowel * 12 + 512) >> 10, 0, 12));
            val_str = VOWEL_BLENDS[v_idx];
        } else if (slot == 1u || slot == 2u || slot == 3u) {
            format_00_99(hbuf, input.slots[slot]);
            val_str = hbuf;
        }
        opxy::draw::header(frame, pos, val_str);
        opxy::draw::text(frame, pos, q10(65 + static_cast<i32>(pos) * 120), 17859, 1024, val_str);
    }

    // 1. Detect Vowel knob adjustment -> wakes up the mouth visually even if no notes are playing
    if (state.last_vowel_in < 0) {
        state.last_vowel_in = p_vowel;
    } else if (abs_d(p_vowel - state.last_vowel_in) > 2) {
        state.last_vowel_in = p_vowel;
        state.vowel_hold_ticks = 22u;
    } else if (state.vowel_hold_ticks > 0u && input.elapsed > 0) {
        state.vowel_hold_ticks -= 1u;
    }

    // 2. Detect Ring knob adjustment (Vibes or Delay) -> grows rings into view on tweak!
    const i32 ring_sig = ((p_vibes >> 1) << 6) ^ ((p_delay >> 1) << 15);
    if (state.last_ring_sig < 0) {
        state.last_ring_sig = ring_sig;
    } else if (ring_sig != state.last_ring_sig) {
        state.last_ring_sig = ring_sig;
        state.vib_onset = 1024; // Always show full immediate wave response when turning Vibes/Delay!
        state.ring_hold_ticks = 28u;
    } else if (state.ring_hold_ticks > 0u && input.elapsed > 0) {
        state.ring_hold_ticks -= 1u;
    }

    // 3. Note Activity Envelope + Mouth Envelope + Immediate ADSR-Matched Ring Envelope + Polyphony Choir Monks
    const u32 held_count = (input.held_notes != 0xffffffffu) ? input.held_notes : 0u;
    const bool note_held = (held_count > 0u);
    const bool new_note_event = (input.note_serial != state.last_serial);
    if (new_note_event) {
        state.last_serial = input.note_serial;
        state.note_env  = clamp_d(state.note_env  + 480, 480, 1024);
        // Articulate each sequenced note clearly while keeping rings smoothly open across sequence steps:
        state.mouth_env = (state.mouth_env > 680) ? 680 : clamp_d(state.mouth_env + 520, 520, 1024);
        state.ring_env  = clamp_d(state.ring_env  + 480, 560, 1024);
        if (state.ring_hold_ticks < 14u) state.ring_hold_ticks = 14u;
        if (state.vowel_hold_ticks < 8u) state.vowel_hold_ticks = 8u;
        if (state.ring_env < 600 && state.vib_onset < 256 + (p_vibes >> 1)) {
            state.vib_bloom_ticks = 0u;
            state.vib_onset = 256 + (p_vibes >> 1);
        }
        if (held_count >= 2u) {
            state.side_l_env = clamp_d(state.side_l_env + 420, 320, 1024);
        }
        if (held_count >= 3u) {
            state.side_r_env = clamp_d(state.side_r_env + 420, 320, 1024);
        }
    }

    if (note_held) {
        state.note_env += (1024 - state.note_env + 3) >> 2;
    } else if (input.elapsed > 0) {
        state.note_env -= (state.note_env + 15) >> 4;
        if (state.note_env < 8) state.note_env = 0;
    }

    const bool mouth_active = note_held || (state.vowel_hold_ticks > 0u);
    if (mouth_active) {
        state.mouth_env += (1024 - state.mouth_env + 3) >> 2;
    } else if (input.elapsed > 0) {
        state.mouth_env -= (state.mouth_env + 7) >> 3;
        if (state.mouth_env < 8) state.mouth_env = 0;
    }

    const bool rings_active = note_held || (state.ring_hold_ticks > 0u);
    if (rings_active) {
        state.ring_env += (1024 - state.ring_env + 3) >> 2;
        if (input.elapsed > 0) {
            state.vib_bloom_ticks = (state.vib_bloom_ticks + 1u) & 0x7fffffffu;
            const u32 hold_frames = static_cast<u32>(3 + (((1024 - p_vibes) * 8) >> 10)); // 11..3 frames
            if (state.vib_bloom_ticks > hold_frames) {
                const i32 bloom_step = 18 + (p_vibes >> 5);
                state.vib_onset = clamp_d(state.vib_onset + bloom_step, 0, 1024);
            }
        }
    } else if (input.elapsed > 0 && state.ring_env > 0) {
        state.vib_bloom_ticks = (state.vib_bloom_ticks + 1u) & 0x7fffffffu;
        const i32 decay_rate = 56 - ((delay * 20) >> 10);
        const i32 step = 12 + ((state.ring_env * decay_rate) >> 10);
        state.ring_env = (state.ring_env > step) ? (state.ring_env - step) : 0;
        if (state.ring_env == 0) {
            state.vib_bloom_ticks = 0u;
            state.vib_onset = 0;
        }
    }
    const i32 env = clamp_d(state.note_env, 0, 1024);
    const i32 m_env = clamp_d(state.mouth_env, 0, 1024);
    const i32 r_env = clamp_d(state.ring_env, 0, 1024);

    // Continuous 60fps Temple Delay rotation in tenths of a degree (0..3599):
    // Hypnotic rotation from 0.8 deg/frame at Vibes 00 up to a gentle 1.2 deg/frame max at Vibes 99.
    const i32 active_env = (env > r_env) ? env : r_env;
    if (!input.reduced_motion && active_env > 0 && input.elapsed > 0) {
        const i32 speed_tenths = 8 + ((vibes * 4) >> 10);
        const i32 deg_step = clamp_d((speed_tenths * active_env + 512) >> 10, 2, 12);
        state.wave_phase = (state.wave_phase + deg_step) % 3600;
    }
    const i32 base_deg10 = state.wave_phase % 3600;

    // 4. Polyphony Side-Profile Choir Monks with 1-Second (45-tick) Hold Delay Before Sliding Out
    if (held_count >= 2u) {
        state.side_l_hold_ticks = 45u;
    } else if (state.side_l_hold_ticks > 0u) {
        state.side_l_hold_ticks -= 1u;
    }

    if (held_count >= 3u) {
        state.side_r_hold_ticks = 45u;
    } else if (state.side_r_hold_ticks > 0u) {
        state.side_r_hold_ticks -= 1u;
    }

    const bool l_present = (held_count >= 2u) || (state.side_l_hold_ticks > 0u);
    const bool r_present = (held_count >= 3u) || (state.side_r_hold_ticks > 0u);

    const i32 target_l_slide = (held_count >= 4u) ? 1024 : (l_present ? 940 : 0);
    const i32 target_r_slide = (held_count >= 4u) ? 1024 : (r_present ? 940 : 0);

    if (target_l_slide > state.side_l_slide) {
        state.side_l_slide += (target_l_slide - state.side_l_slide + 3) >> 2;
    } else {
        state.side_l_slide -= (state.side_l_slide - target_l_slide + 7) >> 3;
        if (state.side_l_slide < 8) state.side_l_slide = 0;
    }

    if (target_r_slide > state.side_r_slide) {
        state.side_r_slide += (target_r_slide - state.side_r_slide + 3) >> 2;
    } else {
        state.side_r_slide -= (state.side_r_slide - target_r_slide + 7) >> 3;
        if (state.side_r_slide < 8) state.side_r_slide = 0;
    }

    if (held_count >= 2u) {
        const i32 s_l = opxy::draw::sine((base_deg10 + 900) % 3600);
        const i32 chant_l = 800 + ((s_l * 210) >> 10);
        state.side_l_env += (chant_l - state.side_l_env + 3) >> 2;
    } else {
        state.side_l_env -= (state.side_l_env + 3) >> 2;
        if (state.side_l_env < 8) state.side_l_env = 0;
    }

    if (held_count >= 3u) {
        const i32 s_r = opxy::draw::sine((base_deg10 + 2475) % 3600);
        const i32 chant_r = 800 + ((s_r * 210) >> 10);
        state.side_r_env += (chant_r - state.side_r_env + 3) >> 2;
    } else {
        state.side_r_env -= (state.side_r_env + 3) >> 2;
        if (state.side_r_env < 8) state.side_r_env = 0;
    }

    for (u32 i = 0; i < 256u; ++i) {
        frame.nodes[i] = Node{0, 0, 1024, 1024, 0, 1, opxy::draw::keep};
    }

    const i32 cx = 240;
    const i32 cy = 112;

    // 3-Zone Shape Curve (shape = 0..1024, smoothed across 19 Body steps & 13 Nose steps):
    // - Zone 1 (0..25%):  Tiny Body + Thin->Thick Nose
    // - Zone 2 (25..75%): Tiny->Large Body + Thick->Thin Nose (Neutral 1.0x Body & Nose at 50% center!)
    // - Zone 3 (75..100%): Large Body + Thin->Thick Nose
    u32 active_body = 105u;
    u32 active_nose = 132u;
    eval_shape_3zone_nodes(shape, active_body, active_nose);

    // =========================================================================
    // 1. 19 STEPLESS HORIZONTAL-ONLY BODY STEPS (Nodes 96..114), FIXED NECK & COLLAR (Nodes 115..117)
    // =========================================================================
    for (u32 b = 96u; b <= 114u; ++b) {
        if (b == active_body) {
            frame.nodes[b].x = q10(cx);
            frame.nodes[b].y = q10(180);
            frame.nodes[b].scale = 1024;
            frame.nodes[b].shade = 3; // #000000 torso mask fill + #828282 shoulder stroke
        } else {
            opxy::draw::hide(frame, b);
        }
    }

    frame.nodes[115].x = q10(cx);
    frame.nodes[115].y = q10(180);
    frame.nodes[115].scale = 1024;
    frame.nodes[115].shade = 8; // #000000 fixed-width neck mask

    frame.nodes[116].x = q10(cx);
    frame.nodes[116].y = q10(180);
    frame.nodes[116].scale = 1024;
    frame.nodes[116].shade = 1; // Two vertical neck lines: matches Ears & Head shade!

    frame.nodes[117].x = q10(cx);
    frame.nodes[117].y = q10(180);
    frame.nodes[117].scale = 1024;
    frame.nodes[117].shade = 3; // Curved Single-Y collar & shirt line: Body shade

    // =========================================================================
    // 2. EARS, HEAD OUTLINE & ALMOND EYES (Nodes 118..123, Shade 1) — Unchanged!
    // =========================================================================
    const i32 open_jaw_drop = 1 + ((vowel * 5) >> 10);
    const i32 jaw_drop = (open_jaw_drop * m_env) >> 10;

    frame.nodes[118].x = q10(195);
    frame.nodes[118].y = q10(cy);
    frame.nodes[118].shade = 1;

    frame.nodes[119].x = q10(285);
    frame.nodes[119].y = q10(cy);
    frame.nodes[119].shade = 1;

    frame.nodes[120].x = q10(cx);
    frame.nodes[120].y = q10(cy);
    frame.nodes[120].shade = 1;

    frame.nodes[121].x = q10(cx);
    frame.nodes[121].y = q10(cy + jaw_drop);
    frame.nodes[121].shade = 1;

    frame.nodes[122].x = q10(216);
    frame.nodes[122].y = q10(100);
    frame.nodes[122].shade = 1;

    frame.nodes[123].x = q10(264);
    frame.nodes[123].y = q10(100);
    frame.nodes[123].shade = 1;

    // =========================================================================
    // 3. EYEBROWS (Nodes 124 & 125, Shade 0 Brightest: #CDCDCD) — Driven by Vibes!
    // =========================================================================
    const i32 brow_lift = (vibes * 6) >> 10;
    const i32 base_tenths = -50 + ((vibes * 220) >> 10);
    const i32 angle_tenths = clamp_d(base_tenths, -240, 240);
    const opxy::draw::Turn turn_l = opxy::draw::rotation(-angle_tenths);
    const opxy::draw::Turn turn_r = opxy::draw::rotation(angle_tenths);

    frame.nodes[124].x = q10(216);
    frame.nodes[124].y = q10(87 - brow_lift);
    frame.nodes[124].cos = turn_l.cos;
    frame.nodes[124].sin = turn_l.sin;
    frame.nodes[124].shade = 0;

    frame.nodes[125].x = q10(264);
    frame.nodes[125].y = q10(87 - brow_lift);
    frame.nodes[125].cos = turn_r.cos;
    frame.nodes[125].sin = turn_r.sin;
    frame.nodes[125].shade = 0;

    // =========================================================================
    // 4. 13 ONE-PIECE UNIFIED NOSE STEPS WITH UNIFORM LINE THICKNESS (Nodes 126..138, Shade 0: #CDCDCD)
    // =========================================================================
    for (u32 n = 126u; n <= 138u; ++n) {
        if (n == active_nose) {
            frame.nodes[n].x = q10(cx);
            frame.nodes[n].y = q10(123);
            frame.nodes[n].scale = 1024;
            frame.nodes[n].shade = 0;
        } else {
            opxy::draw::hide(frame, n);
        }
    }

    // =========================================================================
    // 5. DYNAMIC SCULPTED MOUTH WITH ALL 15 VOWEL STEPS x 6 OPEN STEPS (Nodes 139..229, Shade 0: #CDCDCD)
    // =========================================================================
    u32 active_mouth = 139u; // Closed relaxed line when m_env < 64
    i32 m_cy = 141;
    if (m_env >= 64) {
        const u32 v_step = static_cast<u32>(clamp_d((vowel * 14 + 512) >> 10, 0, 14));
        const u32 o_step = static_cast<u32>(clamp_d(((m_env - 64) * 6) / 961, 0, 5));
        active_mouth = 140u + v_step * 6u + o_step;
        m_cy = 141 + static_cast<i32>((o_step + 1u) >> 1);
    }
    for (u32 m = 139u; m <= 229u; ++m) {
        if (m == active_mouth) {
            frame.nodes[m].x = q10(cx);
            frame.nodes[m].y = q10(m_cy);
            frame.nodes[m].scale = 1024;
            frame.nodes[m].shade = 0;
        } else {
            opxy::draw::hide(frame, m);
        }
    }

    // =========================================================================
    // 6. SIDE-PROFILE CHOIR MONKS FOR POLYPHONY (Nodes 230..242 Left, 243..255 Right)
    // =========================================================================
    const i32 x_left_monk  = -85 + ((148 * state.side_l_slide) >> 10);
    const i32 x_right_monk = 565 - ((148 * state.side_r_slide) >> 10);
    draw_side_choir_monk(frame, 230u, state.side_l_slide, x_left_monk,  clamp_d(state.side_l_env, 0, 1024), brow_lift, turn_l);
    draw_side_choir_monk(frame, 243u, state.side_r_slide, x_right_monk, clamp_d(state.side_r_env, 0, 1024), brow_lift, turn_r);

    // =========================================================================
    // 7. 8 FILLED CONCENTRIC CUBIC-BEZIER WAVE RINGS IN 8 SHADES (Nodes 0..95: 12 Vibes steps x 8 rings):
    //    - Back-to-front layered solid fills (Ring 7 largest in back -> Ring 0 smallest in front)
    //    - Inverted Gradient: Bright Center (r_idx=0, Shade 0) -> Dark Outside (r_idx=7, Shade 7)!
    //    - Full 56..175 px radius expansion across Delay 00..99!
    // =========================================================================
    const i32 ring_offset = 9 + ((delay * 8) >> 10); // 9..17 px uniform spacing -> up to 175 px radius!
    const i32 r_hidden = 34;
    const i32 base_rad = 56;
    const i32 onset_scale = clamp_d(state.vib_onset, 0, 1024);

    // 100% Symmetrical Global Mandala Progression across 12 Wave Steps (v_s = 0..11):
    // - Zone 1 (Vibes 00..30, eff_vibes <= 315): 7 smooth global amplitude steps (v_s = 0..6, 0.00 -> 4.50 px in 0.75 px steps) at 3 lobes
    // - Zone 2 (Vibes 31..59, eff_vibes = 316..604): 5 global symmetrical period steps (v_s = 7..11, 4 -> 5 -> 6 -> 7 -> 8 lobes across all rings!)
    // - Zone 3 (Vibes 60..99, eff_vibes > 604): Smooth cubic Hermite S-curve phase alignment (22.5 deg -> 0 deg lockstep on 8 lobes!)
    const i32 eff_vibes = (vibes * onset_scale + 512) >> 10;
    u32 global_v_step = 0u;
    if (eff_vibes <= 26 || r_env <= 0) {
        global_v_step = 0u;
    } else if (eff_vibes <= 315) {
        global_v_step = static_cast<u32>(1 + clamp_d(((eff_vibes - 27) * 6) / 289, 0, 5));
    } else if (eff_vibes <= 604) {
        global_v_step = static_cast<u32>(7 + clamp_d(((eff_vibes - 316) * 5) / 289, 0, 4));
    } else {
        global_v_step = 11u;
    }

    // Exact 180-deg alternating valley symmetry angle (1800 / N lobes) for each global wave step,
    // plus smooth cubic Hermite S-curve phase alignment from 22.5 deg -> 0 deg across Vibes 60..99:
    const i32 SYM_STEP_DEG10[12] = {600, 600, 600, 600, 600, 600, 600, 450, 360, 300, 257, 225};
    i32 step_deg10 = SYM_STEP_DEG10[global_v_step];
    if (vibes > 604) {
        const i32 t_q10 = clamp_d(((vibes - 604) * 1024 + 210) / 420, 0, 1024);
        const i32 t2_q10 = (t_q10 * t_q10) >> 10;
        const i32 smooth_q10 = ((3072 - (t_q10 << 1)) * t2_q10) >> 10; // 3t^2 - 2t^3 in Q10 (0..1024)
        step_deg10 = (225 * (1024 - smooth_q10) + 512) >> 10;
    }
    const i32 THRESHOLDS[8] = {-1, 40, 180, 320, 460, 600, 740, 880};

    for (u32 v_s = 0u; v_s < 12u; ++v_s) {
        for (u32 r_idx = 0u; r_idx < 8u; ++r_idx) {
            const u32 nid = v_s * 8u + r_idx;
            const bool ring_on = (v_s == global_v_step) && (delay > THRESHOLDS[r_idx]) && (r_env > 0);
            if (!ring_on) {
                opxy::draw::hide(frame, nid);
                frame.nodes[nid].scale = 1;
                continue;
            }
            const i32 rad_open = base_rad + static_cast<i32>(r_idx) * ring_offset;
            const i32 rad = r_hidden + (((rad_open - r_hidden) * r_env) >> 10);
            const i32 scale_q10 = clamp_d((rad * 1024 + 44) / 88, 1, 2048);
            const opxy::draw::Turn turn_ring = opxy::draw::rotation(base_deg10 + static_cast<i32>(r_idx) * step_deg10);

            frame.nodes[nid].x = q10(cx);
            frame.nodes[nid].y = q10(cy);
            frame.nodes[nid].scale = scale_q10;
            frame.nodes[nid].cos = turn_ring.cos;
            frame.nodes[nid].sin = turn_ring.sin;
            frame.nodes[nid].shade = r_idx; // Bright Center (0) -> Dark Outside (7)
            frame.nodes[nid].paint = opxy::draw::keep;
        }
    }
}
} // namespace ohmonk
