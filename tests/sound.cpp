#include "../engine.cpp"
#include "../display.cpp"
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <algorithm>

static opxy::u32 midi_to_pitch_inc(int midi) {
    double freq = 440.0 * std::pow(2.0, (midi - 69) / 12.0);
    return static_cast<opxy::u32>((freq / 44100.0) * 4294967296.0);
}

int main() {
    using namespace ohmonk;

    // 1. Single-note waveform purity & block-boundary continuity across octaves
    for (int m_idx = 0; m_idx < 6; ++m_idx) {
        const int midis[6] = {36, 48, 60, 72, 84, 92};
        const int midi = midis[m_idx];
        Shared shared;
        std::memset(&shared, 0, sizeof(shared));
        Voice voice;
        std::memset(&voice, 0, sizeof(voice));

        initialize(voice, shared, 1u);
        start(voice, shared, 1u);

        opxy::i32 params[8] = {0x3d70a3d7, 0x40000000, 0x28f5c28f, 0x46666666, 0, 0, 0, 0};
        opxy::u32 pitch[64];
        for (int i = 0; i < 64; ++i) pitch[i] = midi_to_pitch_inc(midi);
        opxy::i32 out[64] = {};

        double sum_sq = 0.0;
        opxy::i32 max_abs = 0;
        opxy::i32 max_intra_diff = 0;
        opxy::i32 max_boundary_diff = 0;
        opxy::i32 prev_sample = 0;

        for (int blk = 0; blk < 300; ++blk) {
            render(voice, shared, 1u, out, params, pitch);
            for (int i = 0; i < 64; ++i) {
                opxy::i32 s = out[i];
                sum_sq += static_cast<double>(s) * static_cast<double>(s);
                max_abs = std::max(max_abs, std::abs(s));
                if (blk > 0 || i > 0) {
                    opxy::i32 diff = std::abs(s - prev_sample);
                    if (i == 0) max_boundary_diff = std::max(max_boundary_diff, diff);
                    else max_intra_diff = std::max(max_intra_diff, diff);
                }
                prev_sample = s;
            }
        }
        double rms = std::sqrt(sum_sq / (300.0 * 64.0));
        double boundary_ratio = static_cast<double>(max_boundary_diff) / std::max(1, max_intra_diff);
        assert(rms > 50000000.0);
        assert(max_abs <= 950682000);
        assert(boundary_ratio <= 1.05);
    }

    // 2. Envelope release cut off by new notes across octaves (36 <-> 84)
    {
        Shared shared;
        std::memset(&shared, 0, sizeof(shared));
        Voice voices[5];
        std::memset(voices, 0, sizeof(voices));

        for (opxy::u32 v = 0; v < 5; ++v) {
            initialize(voices[v], shared, v + 1u);
            start(voices[v], shared, v + 1u);
        }

        opxy::i32 params[8] = {0x3d70a3d7, 0x40000000, 0x28f5c28f, 0x46666666, 0, 0, 0, 0};
        int notes[5] = {48, 52, 55, 60, 64};
        opxy::i32 out[64] = {};
        opxy::i32 max_intra_diff = 0;

        for (int blk = 0; blk < 40; ++blk) {
            for (opxy::u32 v = 0; v < 5; ++v) {
                opxy::u32 pitch[1] = {midi_to_pitch_inc(notes[v])};
                render(voices[v], shared, v + 1u, out, params, pitch);
                for (int i = 1; i < 64; ++i) {
                    max_intra_diff = std::max(max_intra_diff, std::abs(out[i] - out[i - 1]));
                }
            }
        }

        const int jump_notes[10] = {84, 36, 88, 40, 91, 43, 79, 38, 86, 48};
        for (int j = 0; j < 10; ++j) {
            opxy::u32 target_v = static_cast<opxy::u32>(j % 5);
            if ((j & 1) == 0) {
                destroy(voices[target_v], shared, 0u);
                std::memset(&voices[target_v], 0, sizeof(Voice));
                initialize(voices[target_v], shared, target_v + 10u);
                start(voices[target_v], shared, target_v + 10u);
            } else {
                start(voices[target_v], shared, target_v + 1u);
            }
            notes[target_v] = jump_notes[j];
            params[0] = (j & 1) ? 0x10000000 : 0x70000000;

            for (int blk = 0; blk < 5; ++blk) {
                for (opxy::u32 v = 0; v < 5; ++v) {
                    opxy::u32 pitch[1] = {midi_to_pitch_inc(notes[v])};
                    render(voices[v], shared, v + 1u, out, params, pitch);
                    if (blk == 0 && v == target_v) {
                        assert(out[0] == 0 && out[1] == 0);
                        for (int i = 0; i < 8; ++i) {
                            assert(std::abs(out[i]) < 5000000);
                        }
                    }
                    for (int i = 1; i < 64; ++i) {
                        max_intra_diff = std::max(max_intra_diff, std::abs(out[i] - out[i - 1]));
                    }
                }
            }
        }
        assert(max_intra_diff > 0);
    }

    // 3. Most-Recent-Note Priority (MRU) & Fallback with Random LFO
    {
        Shared shared;
        std::memset(&shared, 0, sizeof(shared));
        Voice v0 = {}, v1 = {}, v2 = {};
        opxy::i32 out[64] = {};
        opxy::u32 pitch[1] = {midi_to_pitch_inc(60)};

        initialize(v0, shared, 1u);
        start(v0, shared, 1u);
        opxy::i32 p0[8] = {214748364, 0x40000000, 0x28f5c28f, 0, 0, 0, 0, 0};
        render(v0, shared, 1u, out, p0, pitch);
        assert(shared.vowel_smooth == 214748364);

        initialize(v1, shared, 2u);
        start(v1, shared, 2u);
        opxy::i32 p1[8] = {1717986918, 0x40000000, 0x28f5c28f, 0, 0, 0, 0, 0};
        for (int blk = 0; blk < 10; ++blk) {
            render(v0, shared, 1u, out, p0, pitch);
            render(v1, shared, 2u, out, p1, pitch);
        }
        assert(shared.vowel_smooth == 1717986918);

        initialize(v2, shared, 3u);
        start(v2, shared, 3u);
        opxy::i32 p2[8] = {966367641, 0x40000000, 0x28f5c28f, 0, 0, 0, 0, 0};
        for (int blk = 0; blk < 5; ++blk) {
            render(v0, shared, 1u, out, p0, pitch);
            render(v1, shared, 2u, out, p1, pitch);
            render(v2, shared, 3u, out, p2, pitch);
        }
        assert(shared.vowel_smooth == 966367641);

        destroy(v2, shared, 3u);
        for (int blk = 0; blk < 70; ++blk) {
            render(v0, shared, 1u, out, p0, pitch);
            render(v1, shared, 2u, out, p1, pitch);
        }
        assert(shared.vowel_smooth == 1717986918);
    }

    // 4. Display frame validity
    DisplayState ds = {};
    display_reset(ds);
    opxy::draw::Input in = {};
    in.elapsed = 16;
    in.held_notes = 1;
    in.note_serial = 1;
    opxy::i32 dparams[4] = {0x38000000, 0x40000000, 0x30000000, 0x48000000};
    for (int i = 0; i < 4; ++i) {
        in.parameters[i] = dparams[i];
        in.shown[i] = i;
        in.slots[i] = dparams[i];
    }
    ohmonk::Frame frame = {};
    display(ds, in, frame);
    assert(opxy::draw::valid(frame));
    return 0;
}
