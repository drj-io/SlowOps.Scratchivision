// ScratchPlayer.hpp - plays a sample at a signed, continuously varying speed.
//
// SPDX-License-Identifier: MIT
//
// The sample is owned elsewhere (loaded outside the audio thread); the player only reads it.
//
// Modes
//   Relative: the play position integrates the platter speed. Needle drops and skips are
//             ignored; CUE puts the position back to the start. Best for scratch samples.
//   Absolute: the play position is the record position minus the position at the last CUE,
//             while the timecode is locked. While unlocked it falls back to integrating speed.
//
// Ends: the position stops just outside each end (silent there), so after running off the end
// the first pull back plays the sample backwards. Loop on: the position wraps instead. In Abs
// mode while locked the position is the needle's, so it is not stopped at the ends.
// Interpolation: 4-point cubic Hermite. A 10 Hz DC blocker removes the offset left when the
// platter stops part-way through a waveform, and a short fade gates the output at rest.

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace slowops { namespace scratch {

struct SampleView
{
    const float* left = nullptr;
    const float* right = nullptr;    // may equal left (mono)
    int64_t frames = 0;
    double sampleRate = 44100.0;
    bool valid() const { return left != nullptr && frames > 1; }
};

enum class PlayMode { Relative = 0, Absolute = 1 };

class ScratchPlayer
{
public:
    void prepare(double hostRate)
    {
        hostRate_ = hostRate > 0 ? hostRate : 44100.0;
        dcCoef_ = (float)std::exp(-2.0 * 3.14159265358979 * 10.0 / hostRate_);
        gateStep_ = (float)(1.0 / (0.005 * hostRate_));   // 5 ms
        reset();
    }

    void reset()
    {
        pos_ = 0.0; speed_ = 0.0f;
        dcInL_ = dcInR_ = dcOutL_ = dcOutR_ = 0.0f;
        gate_ = 0.0f;
        cueRecordSec_ = 0.0; haveCue_ = false; anchorFrames_ = 0.0;
    }

    void setSample(const SampleView& s)
    {
        sample_ = s;
        if (pos_ < 0.0 || pos_ >= (double)s.frames) pos_ = 0.0;
    }
    const SampleView& sample() const { return sample_; }

    void setMode(PlayMode m) { mode_ = m; }
    void setLoop(bool on) { loop_ = on; }

    // Back to the start. In Absolute mode the current record position becomes the start.
    void cue(double recordSec, bool locked) { jumpTo(0.0, recordSec, locked); }

    // Jump to a frame (a hot cue). The record keeps steering from there: in Absolute mode the
    // current record position is re-anchored to this frame.
    void jumpTo(double frames, double recordSec, bool locked)
    {
        pos_ = frames;
        anchorFrames_ = frames;
        cueRecordSec_ = recordSec - frames / sample_.sampleRate;
        haveCue_ = locked;
    }

    // Render n frames. speed: platter speed (1 = nominal). recordSec/locked: decoder output.
    void process(float* outL, float* outR, int n, float speed, double recordSec, bool locked, float gain)
    {
        if (!sample_.valid())
        {
            for (int i = 0; i < n; ++i) { outL[i] = 0.0f; outR[i] = 0.0f; }
            return;
        }
        const double rateRatio = sample_.sampleRate / hostRate_;
        const double len = (double)sample_.frames;
        // Smooth the speed across the block (it arrives once per block from the caller).
        const float s0 = speed_, s1 = speed;
        const float ds = (n > 0) ? (s1 - s0) / (float)n : 0.0f;

        if (mode_ == PlayMode::Absolute && locked)
        {
            if (!haveCue_) { cueRecordSec_ = recordSec - anchorFrames_ / sample_.sampleRate; haveCue_ = true; pos_ = anchorFrames_; }
            // Aim at the absolute target at the end of the block; integrate speed inside the block
            // and correct the drift gently so the waveform does not jump.
            const double target = (recordSec - cueRecordSec_) * sample_.sampleRate;
            const double predicted = pos_ + (double)(s0 + s1) * 0.5 * n * rateRatio;
            const double err = target - predicted;
            pos_ += (std::fabs(err) > len * 0.25) ? err : err * 0.1;   // big jump = needle drop: follow it
        }

        for (int i = 0; i < n; ++i)
        {
            const float sp = s0 + ds * (float)(i + 1);
            pos_ += (double)sp * rateRatio;
            if (loop_)
            {
                if (pos_ >= len) pos_ -= len * std::floor(pos_ / len);
                else if (pos_ < 0.0) pos_ += len * std::ceil(-pos_ / len);
            }
            else if (mode_ == PlayMode::Relative || !locked)
            {
                // Hard stops just outside each end (silent there): running off the end and then
                // pulling back plays the sample backwards at once, instead of only after the
                // platter has come all the way back.
                if (pos_ > len) pos_ = len;
                else if (pos_ < -1.0) pos_ = -1.0;
            }
            float l = 0.0f, r = 0.0f;
            if (pos_ >= 0.0 && pos_ < len) readHermite(pos_, l, r);

            // Gate: fade out while (nearly) stopped so a held platter is silent.
            const float target = std::fabs(sp) > 0.02f ? 1.0f : 0.0f;
            if (gate_ < target) { gate_ += gateStep_; if (gate_ > 1.0f) gate_ = 1.0f; }
            else if (gate_ > target) { gate_ -= gateStep_; if (gate_ < 0.0f) gate_ = 0.0f; }

            // DC blocker.
            const float yl = l - dcInL_ + dcCoef_ * dcOutL_;
            const float yr = r - dcInR_ + dcCoef_ * dcOutR_;
            dcInL_ = l; dcInR_ = r; dcOutL_ = yl; dcOutR_ = yr;

            outL[i] = yl * gate_ * gain;
            outR[i] = yr * gate_ * gain;
        }
        speed_ = s1;
    }

    double positionFrames() const { return pos_; }
    // 0..1 through the sample (clamped).
    float positionNorm() const
    {
        if (!sample_.valid()) return 0.0f;
        const double p = pos_ / (double)(sample_.frames - 1);
        return (float)(p < 0.0 ? 0.0 : (p > 1.0 ? 1.0 : p));
    }

private:
    inline float at(const float* d, int64_t i) const
    {
        if (loop_) { i %= sample_.frames; if (i < 0) i += sample_.frames; return d[i]; }
        return (i >= 0 && i < sample_.frames) ? d[i] : 0.0f;
    }

    inline void readHermite(double p, float& l, float& r) const
    {
        const int64_t i = (int64_t)std::floor(p);
        const float f = (float)(p - (double)i);
        l = hermite(at(sample_.left, i - 1), at(sample_.left, i), at(sample_.left, i + 1), at(sample_.left, i + 2), f);
        if (sample_.right == sample_.left || sample_.right == nullptr) r = l;
        else r = hermite(at(sample_.right, i - 1), at(sample_.right, i), at(sample_.right, i + 1), at(sample_.right, i + 2), f);
    }

    static inline float hermite(float xm1, float x0, float x1, float x2, float t)
    {
        const float c = (x1 - xm1) * 0.5f;
        const float v = x0 - x1;
        const float w = c + v;
        const float a = w + v + (x2 - x0) * 0.5f;
        const float b = w + a;
        return ((((a * t) - b) * t + c) * t + x0);
    }

    SampleView sample_;
    double hostRate_ = 44100.0;
    PlayMode mode_ = PlayMode::Relative;
    bool loop_ = false;
    double pos_ = 0.0;
    float speed_ = 0.0f;
    double cueRecordSec_ = 0.0;
    double anchorFrames_ = 0.0;   // where the last cue/jump put the sample (Absolute mode anchor)
    bool haveCue_ = false;
    float dcCoef_ = 0.9986f, dcInL_ = 0, dcInR_ = 0, dcOutL_ = 0, dcOutR_ = 0;
    float gate_ = 0.0f, gateStep_ = 0.0045f;
};

}} // namespace slowops::scratch
