// ScratchEngine.hpp - timecode in, scratched sample out. Host-agnostic (no SDK dependency).
//
// SPDX-License-Identifier: MIT
//
// Signal flow:
//   in L/R ──> TimecodeDecoder ──> speed, record position, lock
//         │                              │
//         │                              v
//         │                        ScratchPlayer (sample, Hermite, DC block, gate) ──> Gain ──┐
//         └──> scope ring (last 128 frames, for the SCOPE page)                               │
//   in L/R ─────────────────────────────────────────────── Thru ─────────────────────────────┴──> out
//
// Audio is handled in chunks of up to 32 frames: decode a chunk, then play it with the speed
// measured at the end of that chunk (0.7 ms of latency, smooth enough for scratching).
// Input and output buffers may be the same memory (the MPC processes in place).

#pragma once

#include "ScratchPlayer.hpp"
#include "TimecodeDecoder.hpp"

#include <cstdint>

namespace slowops { namespace scratch {

struct EngineSettings
{
    bool rpm45 = false;
    bool swap = false;
    PlayMode mode = PlayMode::Relative;
    bool loop = false;
    bool thru = false;
    float gain = 1.0f;
};

class ScratchEngine
{
public:
    static constexpr int kChunk = 32;
    static constexpr int kScopeRing = 128;

    // Allocates the decoder's lookup table. Call outside the audio thread.
    void prepare(double sampleRate)
    {
        decoder_.prepare(sampleRate);
        player_.prepare(sampleRate);
        reset();
    }

    void reset()
    {
        decoder_.reset();
        player_.reset();
        scopeHead_ = 0;
        for (int i = 0; i < kScopeRing; ++i) { scopeL_[i] = scopeR_[i] = 0.0f; }
    }

    void setSample(const SampleView& s) { player_.setSample(s); }
    const SampleView& sample() const { return player_.sample(); }

    void cue() { jumpTo(0.0); }

    // Hot cue: move the sample to this frame; the record keeps steering from there.
    void jumpTo(double frames)
    {
        const TimecodeState s = decoder_.state();
        player_.jumpTo(frames, s.positionSec, s.locked);
    }

    void process(const float* inL, const float* inR, float* outL, float* outR, int n, const EngineSettings& set)
    {
        decoder_.setNominalRpm45(set.rpm45);
        decoder_.setSwap(set.swap);
        player_.setMode(set.mode);
        player_.setLoop(set.loop);

        float bufL[kChunk], bufR[kChunk];
        for (int off = 0; off < n; off += kChunk)
        {
            const int m = (n - off) < kChunk ? (n - off) : kChunk;
            for (int i = 0; i < m; ++i)
            {
                const float l = inL[off + i], r = inR[off + i];
                bufL[i] = l; bufR[i] = r;
                decoder_.process(l, r);
                scopeL_[scopeHead_] = l; scopeR_[scopeHead_] = r;
                scopeHead_ = (scopeHead_ + 1) & (kScopeRing - 1);
            }
            const TimecodeState s = decoder_.state();
            if (set.thru)
            {
                // Advance the player silently so the display keeps moving, then pass the input.
                float dummyL[kChunk], dummyR[kChunk];
                player_.process(dummyL, dummyR, m, s.speed, s.positionSec, s.locked, 0.0f);
                for (int i = 0; i < m; ++i) { outL[off + i] = bufL[i]; outR[off + i] = bufR[i]; }
            }
            else
            {
                player_.process(outL + off, outR + off, m, s.speed, s.positionSec, s.locked, set.gain);
            }
        }
    }

    TimecodeState timecode() const { return decoder_.state(); }
    float positionNorm() const { return player_.positionNorm(); }
    double positionFrames() const { return player_.positionFrames(); }

    // Recent input, oldest first: i = 0 .. kScopeRing-1.
    float scopeL(int i) const { return scopeL_[(scopeHead_ + i) & (kScopeRing - 1)]; }
    float scopeR(int i) const { return scopeR_[(scopeHead_ + i) & (kScopeRing - 1)]; }

    TimecodeDecoder& decoder() { return decoder_; }

private:
    TimecodeDecoder decoder_;
    ScratchPlayer player_;
    float scopeL_[kScopeRing] = {}, scopeR_[kScopeRing] = {};
    int scopeHead_ = 0;
};

}} // namespace slowops::scratch
