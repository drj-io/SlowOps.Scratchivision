// TimecodeGen.hpp - synthetic control-vinyl signal (unit tests and the plugin's Test Pattern).
//
// SPDX-License-Identifier: MIT
//
// Produces L = a*sin(phi), R = a*cos(phi), with phi advancing at carrierHz * speed. Each
// channel's amplitude only changes at that channel's own zero crossings, so there are no steps
// in the waveform. R's positive half-cycle centred on phi = 2*pi*k uses the loud level when
// bit k of the LFSR sequence is 1. L uses the same bit for the half-cycle starting at cycle k.

#pragma once

#include "Lfsr.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

namespace slowops { namespace scratch { namespace test {

class TimecodeGen
{
public:
    explicit TimecodeGen(double sampleRate, const TimecodeFormat& f = seratoCv02A())
        : sr_(sampleRate), fmt_(f)
    {
        // Precompute the bit of every cycle on the side.
        bits_.resize((size_t)f.length);
        uint32_t s = f.seed & lfsrMask(f);
        for (int32_t k = 0; k < f.length; ++k) { bits_[(size_t)k] = (uint8_t)lfsrBit(s, f); s = lfsrNext(s, f); }
    }

    void setCycles(double cycles) { cycles_ = cycles; }   // needle position in carrier cycles
    double cycles() const { return cycles_; }
    void setAmplitude(float loud, float quiet) { loud_ = loud; quiet_ = quiet; }
    void setNoise(float n) { noise_ = n; }
    void setRpm45(bool is45) { scale_ = is45 ? 1.35 : 1.0; }

    // One sample at the given speed (1 = nominal).
    void next(float speed, float& l, float& r)
    {
        cycles_ += (double)speed * fmt_.carrierHz * scale_ / sr_;
        const double ph = cycles_ * 2.0 * 3.14159265358979323846;
        const double sn = std::sin(ph), cs = std::cos(ph);
        // R's level for cycle k holds from its zero at k-0.25 to its zero at k+0.75 (so the positive
        // half-cycle centred on k is whole); L's from its zero at k to its zero at k+1.
        const int64_t kr = (int64_t)std::floor(cycles_ + 0.25);
        const int64_t kl = (int64_t)std::floor(cycles_);
        const float ar = level(kr), al = level(kl);
        l = (float)(al * sn) + noise();
        r = (float)(ar * cs) + noise();
    }

    void render(float speed, int n, std::vector<float>& L, std::vector<float>& R)
    {
        for (int i = 0; i < n; ++i) { float l, r; next(speed, l, r); L.push_back(l); R.push_back(r); }
    }

private:
    float level(int64_t k) const
    {
        if (k < 0 || k >= (int64_t)bits_.size()) return quiet_;
        return bits_[(size_t)k] ? loud_ : quiet_;
    }
    float noise()
    {
        if (noise_ <= 0.0f) return 0.0f;
        rng_ = rng_ * 1664525u + 1013904223u;
        return noise_ * ((float)(rng_ >> 8) / 8388608.0f - 1.0f);
    }

    double sr_;
    const TimecodeFormat& fmt_;
    std::vector<uint8_t> bits_;
    double cycles_ = 1000.0;
    double scale_ = 1.0;
    float loud_ = 0.5f, quiet_ = 0.3f, noise_ = 0.0f;
    uint32_t rng_ = 12345u;
};

}}} // namespace slowops::scratch::test
