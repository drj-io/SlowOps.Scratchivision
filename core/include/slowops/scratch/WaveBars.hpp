// WaveBars.hpp - the live waveform: a peak envelope per sample, and bar heights around the play
// position for the DECK page.
//
// SPDX-License-Identifier: MIT
//
// The MPC cannot draw from plugin code, so the waveform is a row of film-strip images, each
// showing one bar whose height the plugin sets through a parameter. computeEnvelope() runs once
// per sample (on the loader thread); computeBars() runs at the display rate on the audio thread
// (no allocation, at most a few thousand comparisons).

#pragma once

#include <cmath>
#include <cstddef>
#include <vector>

namespace slowops { namespace scratch {

struct Envelope
{
    std::vector<float> peak;   // peak |x| per block (max over channels)
    int blockFrames = 110;     // 2.5 ms at 44.1 kHz
    double sampleRate = 44100.0;
    float maxPeak = 0.0f;
    double blockSec() const { return blockFrames / sampleRate; }
};

inline void computeEnvelope(const float* l, const float* r, size_t frames, double sampleRate, Envelope& e)
{
    e.sampleRate = sampleRate > 0 ? sampleRate : 44100.0;
    e.blockFrames = (int)std::lround(e.sampleRate * 0.0025);
    if (e.blockFrames < 1) e.blockFrames = 1;
    const size_t blocks = (frames + (size_t)e.blockFrames - 1) / (size_t)e.blockFrames;
    e.peak.assign(blocks, 0.0f);
    e.maxPeak = 0.0f;
    for (size_t b = 0; b < blocks; ++b)
    {
        const size_t s0 = b * (size_t)e.blockFrames;
        const size_t s1 = (s0 + (size_t)e.blockFrames < frames) ? s0 + (size_t)e.blockFrames : frames;
        float p = 0.0f;
        for (size_t i = s0; i < s1; ++i)
        {
            const float a = std::fabs(l[i]);
            const float c = r ? std::fabs(r[i]) : a;
            if (a > p) p = a;
            if (c > p) p = c;
        }
        e.peak[b] = p;
        if (p > e.maxPeak) e.maxPeak = p;
    }
}

// Bar heights 0..1 for nBars bars spanning viewSec seconds, centred on posSec (the play position,
// or the middle of the sample when the whole sample is shown) (the playhead sits
// between bar nBars/2 - 1 and bar nBars/2). Scaled so the sample's loudest block is full height;
// zero outside the sample.
inline void computeBars(const Envelope& e, double posSec, double viewSec, int nBars, float* out)
{
    const double w = viewSec / nBars;
    const double bs = e.blockSec();
    const long nb = (long)e.peak.size();
    const float norm = e.maxPeak > 1e-6f ? 1.0f / e.maxPeak : 0.0f;
    for (int k = 0; k < nBars; ++k)
    {
        const double t0 = posSec + (k - nBars / 2) * w;
        const double t1 = t0 + w;
        long b0 = (long)std::floor(t0 / bs), b1 = (long)std::ceil(t1 / bs);
        if (b1 <= b0) b1 = b0 + 1;
        if (b1 <= 0 || b0 >= nb) { out[k] = 0.0f; continue; }   // wholly before the start / after the end
        if (b0 < 0) b0 = 0;
        if (b1 > nb) b1 = nb;
        float p = 0.0f;
        for (long b = b0; b < b1; ++b) if (e.peak[(size_t)b] > p) p = e.peak[(size_t)b];
        const float v = p * norm;
        out[k] = v > 1.0f ? 1.0f : v;
    }
}

}} // namespace slowops::scratch
