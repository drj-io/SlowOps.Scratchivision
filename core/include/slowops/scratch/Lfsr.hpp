// Lfsr.hpp - the bit sequence printed on Serato-style control vinyl.
//
// SPDX-License-Identifier: MIT
//
// Written from the published format description (no third-party code). The record's carrier
// carries one bit per cycle; any 20 consecutive bits form a 20-bit register value that appears
// only once on the side, so it identifies the absolute position of the needle.
//
// Register convention used throughout Scratchivision:
//   state(k) holds the bits of cycles k-19 .. k (bit 19 = cycle k, bit 0 = cycle k-19).
//   state(k+1) = (state(k) >> 1) | (parity(state(k) & (taps | 1)) << 19)
//   (bit 0, the bit shifted out, always feeds back, which makes the sequence reversible)
//   bit(k) = bit 19 of state(k)
//
// VERIFY AGAINST A REAL RECORDING: the seed, taps and length below are the values commonly
// published for Serato CV02 side A. The decoder and the synthetic test signal share this table,
// so the unit tests prove internal consistency, not that the constants match the vinyl. If
// absolute position never locks on real vinyl while pitch works, check these first.

#pragma once

#include <cstdint>
#include <vector>

namespace slowops { namespace scratch {

struct TimecodeFormat
{
    const char* name;
    double carrierHz;      // carrier frequency at nominal speed (33 1/3 rpm)
    int bits;              // register width
    uint32_t seed;         // state at cycle 0
    uint32_t taps;         // feedback taps
    int32_t length;        // cycles on the side
};

// Serato Control Vinyl (CV02), side A.
inline const TimecodeFormat& seratoCv02A()
{
    static const TimecodeFormat f = {"Serato CV02 A", 1000.0, 20, 0x59017u, 0x361e4u, 712000};
    return f;
}

inline uint32_t parity32(uint32_t v)
{
    v ^= v >> 16; v ^= v >> 8; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return v & 1u;
}

inline uint32_t lfsrMask(const TimecodeFormat& f) { return (f.bits >= 32) ? 0xffffffffu : ((1u << f.bits) - 1u); }

// One cycle forward.
inline uint32_t lfsrNext(uint32_t s, const TimecodeFormat& f)
{
    const uint32_t nb = parity32(s & (f.taps | 1u));
    return ((s >> 1) | (nb << (f.bits - 1))) & lfsrMask(f);
}

// One cycle back. state(k-1) = ((state(k) << 1) | b) & mask, where b (bit 0 of state(k-1), the
// bit of cycle k-20) is the one bit that makes lfsrNext(state(k-1)) == state(k).
inline uint32_t lfsrPrev(uint32_t s, const TimecodeFormat& f)
{
    const uint32_t mask = lfsrMask(f);
    const uint32_t cand = (s << 1) & mask;
    return (lfsrNext(cand, f) == s) ? cand : ((cand | 1u) & mask);
}

inline uint32_t lfsrBit(uint32_t s, const TimecodeFormat& f) { return (s >> (f.bits - 1)) & 1u; }

// Lookup table: register value -> cycle index (-1 when the value is not on the record).
// Built once (outside the audio thread). 2^20 entries = 4 MB for a 20-bit format.
class LfsrTable
{
public:
    void build(const TimecodeFormat& f)
    {
        fmt_ = &f;
        table_.assign((size_t)1 << f.bits, -1);
        uint32_t s = f.seed & lfsrMask(f);
        for (int32_t k = 0; k < f.length; ++k)
        {
            if (table_[s] < 0) table_[s] = k;
            s = lfsrNext(s, f);
        }
    }
    bool ready() const { return !table_.empty(); }
    int32_t lookup(uint32_t s) const { return s < table_.size() ? table_[s] : -1; }
    const TimecodeFormat* format() const { return fmt_; }

private:
    const TimecodeFormat* fmt_ = nullptr;
    std::vector<int32_t> table_;
};

}} // namespace slowops::scratch
