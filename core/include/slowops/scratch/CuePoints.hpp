// CuePoints.hpp - five hot cues per sample, and their small text file.
//
// SPDX-License-Identifier: MIT
//
// A sample's cues live in "<audio file>.cues" (in the Scratchivision/ folder next to the audio;
// for bundled samples, the Scratchivision/ folder on the plugin's own drive). Format:
//   scratchivision-cues 1
//   1 0.523000
//   3 1.200000
// One line per set cue: number (1..5), position in seconds. Unknown or bad lines are ignored.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>

namespace slowops { namespace scratch {

struct CuePoints
{
    static constexpr int kCount = 5;
    double sec[kCount] = {-1, -1, -1, -1, -1};   // < 0: not set

    bool isSet(int i) const { return i >= 0 && i < kCount && sec[i] >= 0.0; }
    bool any() const { for (double s : sec) if (s >= 0.0) return true; return false; }
    bool operator==(const CuePoints& o) const { for (int i = 0; i < kCount; ++i) if (sec[i] != o.sec[i]) return false; return true; }

    // maxSec: cues past the end of the sample (e.g. the file was replaced by a shorter one) are dropped.
    static CuePoints parse(const std::string& text, double maxSec)
    {
        CuePoints c;
        size_t p = 0;
        bool header = false;
        while (p < text.size())
        {
            size_t e = text.find('\n', p);
            if (e == std::string::npos) e = text.size();
            const std::string line = text.substr(p, e - p);
            p = e + 1;
            if (!header) { header = line.rfind("scratchivision-cues", 0) == 0; if (!header) return c; continue; }
            int n = 0; double s = -1;
            if (std::sscanf(line.c_str(), "%d %lf", &n, &s) == 2 && n >= 1 && n <= kCount && s >= 0.0 && s <= maxSec)
                c.sec[n - 1] = s;
        }
        return c;
    }

    std::string format() const
    {
        std::string out = "scratchivision-cues 1\n";
        char buf[64];
        for (int i = 0; i < kCount; ++i)
            if (sec[i] >= 0.0) { std::snprintf(buf, sizeof(buf), "%d %.6f\n", i + 1, sec[i]); out += buf; }
        return out;
    }
};

}} // namespace slowops::scratch
