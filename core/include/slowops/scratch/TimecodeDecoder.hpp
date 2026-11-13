// TimecodeDecoder.hpp - turns control-vinyl audio into platter speed and needle position.
//
// SPDX-License-Identifier: MIT
//
// Written from scratch from the format description (no xwax or other GPL code).
//
// Signal model (forward play, phase phi advancing at carrierHz * speed cycles per second):
//   L = a(t) * sin(phi),   R = a(t) * cos(phi)        (R leads L by a quarter cycle)
// The amplitude of R's positive half-cycle centred on phi = 2*pi*k carries bit k of the LFSR
// sequence (loud = 1, quiet = 0). See Lfsr.hpp for the register convention.
//
// What the decoder does per sample, with no allocation and no transcendental functions:
//   1. Schmitt-trigger zero-crossing detector on each channel (hysteresis scales with level).
//      Every confirmed crossing is a quarter cycle; the other channel's sign at that moment
//      gives the direction. Crossing times are interpolated between samples.
//   2. Speed = quarters / time over the last few crossings in the same direction, divided by
//      the nominal carrier. It decays towards 0 when crossings stop (platter held).
//   3. Bits: each finished positive half-cycle of R gives a peak; peak > running average = 1.
//      Twenty bits in one direction give a register value; the lookup table gives the cycle
//      index. Consecutive lookups that agree with the direction of travel build confidence;
//      twelve in a row = locked. Between bits the position advances by quarter cycles, so the
//      position is continuous even when unlocked (it is then relative only).
//
// Swap: some wiring swaps L and R (or inverts one), which flips the direction. The Swap
// option exchanges the channels before decoding.

#pragma once

#include "Lfsr.hpp"

#include <cmath>
#include <cstdint>

namespace slowops { namespace scratch {

struct TimecodeState
{
    float speed = 0.0f;          // signed, 1.0 = nominal (33 1/3 or 45 as configured)
    double positionSec = 0.0;    // needle position in record seconds at nominal speed
    bool locked = false;         // positionSec is absolute (decoded from the bit sequence)
    float level = 0.0f;          // signal peak level, 0..1
    bool signal = false;         // a usable carrier is present
};

class TimecodeDecoder
{
public:
    // Allocates the lookup table (4 MB). Call outside the audio thread.
    void prepare(double sampleRate, const TimecodeFormat& fmt = seratoCv02A())
    {
        sampleRate_ = sampleRate > 0 ? sampleRate : 44100.0;
        if (table_.format() != &fmt || !table_.ready()) table_.build(fmt);
        fmt_ = &fmt;
        envRelease_ = (float)std::exp(-1.0 / (0.1 * sampleRate_));
        reset();
    }

    void reset()
    {
        for (int c = 0; c < 2; ++c) { ch_[c] = Channel(); }
        t_ = 0.0;
        nEvents_ = 0; evHead_ = 0;
        lastEventT_ = -1e9;
        speed_ = 0.0f; lastDir_ = 0;
        reg_ = 0; bitsFilled_ = 0; regDir_ = 0;
        lastCycle_ = -1; confidence_ = 0; misses_ = 0;
        positionCycles_ = 0.0;
        env_ = 0.0f; peakAvg_ = 0.0f; bitRef_ = 0.0f;
    }

    void setNominalRpm45(bool is45) { speedScale_ = is45 ? 1.35 : 1.0; }
    void setSwap(bool swap) { swap_ = swap; }

    // One stereo sample.
    inline void process(float inL, float inR)
    {
        const float x[2] = {swap_ ? inR : inL, swap_ ? inL : inR};
        t_ += 1.0;

        // Level envelope: instant attack, ~100 ms release.
        const float a = std::fabs(x[0]) > std::fabs(x[1]) ? std::fabs(x[0]) : std::fabs(x[1]);
        env_ = a > env_ ? a : env_ * envRelease_;

        const float h = hysteresis();
        for (int c = 0; c < 2; ++c)
        {
            Channel& k = ch_[c];
            const float v = x[c];
            if ((v >= 0.0f) != (k.prev >= 0.0f))
            {
                const float d = k.prev - v;
                const double frac = (d != 0.0f) ? (double)(k.prev / d) : 0.5;
                k.zeroT = t_ - 1.0 + frac;
            }
            const float av = std::fabs(v);
            if (av > k.halfPeak) k.halfPeak = av;

            if (!k.high && v > h) { k.high = true; crossing(c, true, x); }
            else if (k.high && v < -h) { k.high = false; crossing(c, false, x); }
            k.prev = v;
        }

        // Crossings stopped: the speed cannot be higher than one quarter per elapsed time.
        const double since = t_ - lastEventT_;
        if (since > sampleRate_ * 0.08 || env_ < kMinLevel) { speed_ = 0.0f; }
        else if (speed_ != 0.0f)
        {
            const float bound = (float)(0.25 * sampleRate_ / since / carrier());
            if (std::fabs(speed_) > bound) speed_ = speed_ > 0 ? bound : -bound;
        }
    }

    void processBlock(const float* l, const float* r, int n)
    {
        for (int i = 0; i < n; ++i) process(l[i], r[i]);
    }

    TimecodeState state() const
    {
        TimecodeState s;
        s.speed = speed_;
        s.positionSec = positionCycles_ / carrier();
        s.locked = confidence_ >= kLockConfidence;
        s.level = env_;
        s.signal = env_ >= kMinLevel && (t_ - lastEventT_) < sampleRate_ * 0.25;
        return s;
    }

    float speed() const { return speed_; }
    double positionCycles() const { return positionCycles_; }
    bool locked() const { return confidence_ >= kLockConfidence; }
    const TimecodeFormat& format() const { return *fmt_; }

private:
    static constexpr float kMinLevel = 0.004f;      // about -48 dBFS
    static constexpr int kLockConfidence = 12;   // a random register passes n checks with p ~ 0.68 * 0.5^(n-1)
    static constexpr int kWindow = 8;               // crossings used for the speed estimate

    struct Channel
    {
        float prev = 0.0f;
        double zeroT = 0.0;
        float halfPeak = 0.0f;
        bool high = false;
    };

    double carrier() const { return fmt_->carrierHz * speedScale_; }

    float hysteresis() const
    {
        const float h = 0.2f * peakAvg_;
        return h > kMinLevel * 0.5f ? h : kMinLevel * 0.5f;
    }

    void crossing(int c, bool rising, const float* x)
    {
        Channel& k = ch_[c];
        const float other = x[1 - c];
        // L: rising with R > 0 is forward. R: rising with L < 0 is forward.
        const int dir = (c == 0) ? ((rising == (other > 0.0f)) ? 1 : -1)
                                 : ((rising == (other < 0.0f)) ? 1 : -1);
        const float peak = k.halfPeak;
        k.halfPeak = 0.0f;
        peakAvg_ += (peak - peakAvg_) * (peakAvg_ == 0.0f ? 1.0f : 0.05f);

        // Speed from the run of crossings in the current direction.
        evT_[evHead_] = k.zeroT; evDir_[evHead_] = (int8_t)dir;
        evHead_ = (evHead_ + 1) % kWindow;
        if (nEvents_ < kWindow) ++nEvents_;
        int run = 1;
        double first = k.zeroT;
        for (int i = 1; i < nEvents_; ++i)
        {
            const int j = (evHead_ - 1 - i + 2 * kWindow) % kWindow;
            if (evDir_[j] != dir) break;
            first = evT_[j]; ++run;
        }
        if (run >= 2 && k.zeroT > first)
        {
            const double quarters = run - 1;
            const double hz = quarters * 0.25 * sampleRate_ / (k.zeroT - first);
            speed_ = (float)(dir * hz / carrier());
        }
        else if (dir != lastDir_) speed_ = 0.0f;   // just reversed: one crossing says nothing yet
        lastDir_ = dir;
        lastEventT_ = t_;

        positionCycles_ += 0.25 * dir;

        // R's positive half-cycle finished (it always ends with a falling crossing): one bit.
        if (c == 1 && !rising) bit(peak, dir);
    }

    void bit(float peak, int dir)
    {
        bitRef_ += (peak - bitRef_) * (bitRef_ == 0.0f ? 1.0f : 0.03f);
        const uint32_t b = peak > bitRef_ ? 1u : 0u;
        const TimecodeFormat& f = *fmt_;
        const uint32_t mask = lfsrMask(f);
        if (dir != regDir_) { regDir_ = dir; bitsFilled_ = 0; confidence_ = 0; misses_ = 0; lastCycle_ = -1; }
        if (dir > 0) reg_ = ((reg_ >> 1) | (b << (f.bits - 1))) & mask;
        else reg_ = ((reg_ << 1) | b) & mask;
        if (bitsFilled_ < f.bits) { ++bitsFilled_; if (bitsFilled_ < f.bits) return; }

        int32_t k = table_.lookup(reg_);
        if (k >= 0 && dir < 0) k -= (f.bits - 1);   // reading backwards: register = state(current + 19)
        const bool wasLocked = confidence_ >= kLockConfidence;
        const int32_t predicted = lastCycle_ >= 0 ? lastCycle_ + dir : -1;
        if (k >= 0 && k == predicted)
        {
            if (confidence_ < 64) ++confidence_;
            misses_ = 0;
            lastCycle_ = k;
        }
        else if (wasLocked)
        {
            // One bad bit (dust, a scratch) spoils the next 20 registers. Ride through it on the
            // predicted cycle; give up the lock only after more misses in a row than that.
            lastCycle_ = predicted;
            if (++misses_ > f.bits + 4) { confidence_ = 0; misses_ = 0; lastCycle_ = k; }
        }
        else
        {
            confidence_ = (k >= 0) ? 1 : 0;
            lastCycle_ = k;
        }
        // Locked: snap to the decoded cycle. The bit is read at the R peak (phi = 2*pi*k) but the
        // falling crossing that reports it is a quarter cycle later in the direction of travel.
        if (confidence_ >= kLockConfidence) positionCycles_ = (double)lastCycle_ + 0.25 * dir;
    }

    const TimecodeFormat* fmt_ = &seratoCv02A();
    LfsrTable table_;
    double sampleRate_ = 44100.0;
    double speedScale_ = 1.0;
    bool swap_ = false;

    Channel ch_[2];
    double t_ = 0.0;
    double evT_[kWindow] = {};
    int8_t evDir_[kWindow] = {};
    int nEvents_ = 0, evHead_ = 0;
    double lastEventT_ = -1e9;
    float speed_ = 0.0f;
    int lastDir_ = 0;

    uint32_t reg_ = 0;
    int bitsFilled_ = 0, regDir_ = 0;
    int32_t lastCycle_ = -1;
    int confidence_ = 0, misses_ = 0;
    double positionCycles_ = 0.0;

    float env_ = 0.0f, peakAvg_ = 0.0f, bitRef_ = 0.0f;
    float envRelease_ = 0.99977f;   // ~100 ms at 44.1 kHz
};

}} // namespace slowops::scratch
