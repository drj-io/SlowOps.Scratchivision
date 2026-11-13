// AudioFile.hpp - read AIFF/AIFC and MP3 files into float channels (WAV is read by the SDK).
//
// SPDX-License-Identifier: MIT
//
// Runs on the loader thread only (allocates, reads files). Reads at most maxSeconds of audio, so
// a long MP3 never takes more memory than the part that is kept.
//
// AIFF / AIFC: PCM 8/16/24/32-bit big-endian ("NONE"), little-endian ("sowt"), 32/64-bit float
// ("fl32", "fl64"). Written from the format description.
// MP3: minimp3 by lieff (CC0 1.0, public domain), third_party/minimp3/.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef SCRATCHIVISION_MINIMP3_INCLUDED
#define SCRATCHIVISION_MINIMP3_INCLUDED
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT
#define MINIMP3_NO_STDIO
#include "../../../../third_party/minimp3/minimp3_ex.h"
#endif

namespace slowops { namespace scratch {

enum class AudioType { Unknown, Wav, Aiff, Mp3 };

inline AudioType audioTypeFromName(const std::string& name)
{
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos) return AudioType::Unknown;
    std::string e = name.substr(dot + 1);
    for (char& c : e) c = (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    if (e == "wav" || e == "wave") return AudioType::Wav;
    if (e == "aif" || e == "aiff" || e == "aifc") return AudioType::Aiff;
    if (e == "mp3") return AudioType::Mp3;
    return AudioType::Unknown;
}

inline bool readWholeFile(const std::string& path, std::vector<uint8_t>& out)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0) { std::fclose(f); return false; }
    out.resize((size_t)n);
    const size_t got = std::fread(out.data(), 1, (size_t)n, f);
    std::fclose(f);
    return got == (size_t)n;
}

namespace detail {
inline uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
inline uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
// 80-bit IEEE extended (AIFF sample rate).
inline double extended80(const uint8_t* p)
{
    const int exp = ((p[0] & 0x7f) << 8) | p[1];
    uint64_t mant = 0;
    for (int i = 0; i < 8; ++i) mant = (mant << 8) | p[2 + i];
    if (exp == 0 && mant == 0) return 0.0;
    const double v = std::ldexp((double)mant, exp - 16383 - 63);
    return (p[0] & 0x80) ? -v : v;
}
} // namespace detail

inline bool decodeAiff(const std::vector<uint8_t>& d, double maxSeconds, std::vector<std::vector<float>>& ch, double& sr)
{
    using namespace detail;
    if (d.size() < 12 || std::memcmp(d.data(), "FORM", 4) != 0) return false;
    const bool aifc = std::memcmp(d.data() + 8, "AIFC", 4) == 0;
    if (!aifc && std::memcmp(d.data() + 8, "AIFF", 4) != 0) return false;
    int channels = 0, bits = 0;
    uint32_t frames = 0;
    char comp[5] = "NONE";
    const uint8_t* data = nullptr;
    size_t dataLen = 0;
    size_t p = 12;
    while (p + 8 <= d.size())
    {
        const uint8_t* c = d.data() + p;
        const uint32_t len = be32(c + 4);
        const size_t body = p + 8;
        if (body + len > d.size() && std::memcmp(c, "SSND", 4) != 0) break;
        if (std::memcmp(c, "COMM", 4) == 0 && len >= 18)
        {
            channels = be16(c + 8);
            frames = be32(c + 10);
            bits = be16(c + 14);
            sr = extended80(c + 16);
            if (aifc && len >= 22) { std::memcpy(comp, c + 26, 4); comp[4] = 0; }
        }
        else if (std::memcmp(c, "SSND", 4) == 0 && len >= 8)
        {
            const uint32_t offset = be32(c + 8);
            const size_t start = body + 8 + offset;
            if (start <= d.size()) { data = d.data() + start; dataLen = std::min((size_t)len - 8 - offset, d.size() - start); }
        }
        p = body + len + (len & 1);
    }
    if (!data || channels <= 0 || sr <= 0) return false;
    const bool le = std::strcmp(comp, "sowt") == 0;
    const bool f32 = std::strcmp(comp, "fl32") == 0 || std::strcmp(comp, "FL32") == 0;
    const bool f64 = std::strcmp(comp, "fl64") == 0 || std::strcmp(comp, "FL64") == 0;
    if (!le && !f32 && !f64 && std::strcmp(comp, "NONE") != 0) return false;   // compressed AIFC: not supported
    const int bytes = f32 ? 4 : (f64 ? 8 : (bits + 7) / 8);
    if (bytes < 1 || bytes > 8) return false;
    size_t n = std::min((size_t)frames, dataLen / ((size_t)bytes * channels));
    const size_t maxFrames = (size_t)(maxSeconds * sr) + 1;
    if (n > maxFrames) n = maxFrames;
    const int keep = channels > 1 ? 2 : 1;
    ch.assign((size_t)keep, std::vector<float>(n));
    for (size_t i = 0; i < n; ++i)
        for (int k = 0; k < keep; ++k)
        {
            const uint8_t* s = data + (i * channels + k) * bytes;
            float v;
            if (f32) { uint32_t u = be32(s); float x; std::memcpy(&x, &u, 4); v = x; }
            else if (f64) { uint64_t u = ((uint64_t)be32(s) << 32) | be32(s + 4); double x; std::memcpy(&x, &u, 8); v = (float)x; }
            else
            {
                int32_t x = 0;
                for (int b = 0; b < bytes; ++b) x = (x << 8) | (le ? s[bytes - 1 - b] : s[b]);
                x <<= (32 - 8 * bytes);   // sign-extend via the top bits
                v = (float)((double)x / 2147483648.0);
            }
            ch[(size_t)k][i] = v;
        }
    return true;
}

inline bool decodeMp3(const std::vector<uint8_t>& d, double maxSeconds, std::vector<std::vector<float>>& ch, double& sr)
{
    mp3dec_ex_t dec;
    if (mp3dec_ex_open_buf(&dec, d.data(), d.size(), MP3D_SEEK_TO_SAMPLE) != 0) return false;
    const int channels = dec.info.channels;
    sr = dec.info.hz;
    if (channels <= 0 || sr <= 0) { mp3dec_ex_close(&dec); return false; }
    size_t want = (size_t)(maxSeconds * sr + 1) * (size_t)channels;
    if (dec.samples > 0 && dec.samples < want) want = (size_t)dec.samples;
    std::vector<float> pcm(want);
    const size_t got = mp3dec_ex_read(&dec, pcm.data(), want);
    mp3dec_ex_close(&dec);
    const size_t frames = got / (size_t)channels;
    if (frames < 2) return false;
    const int keep = channels > 1 ? 2 : 1;
    ch.assign((size_t)keep, std::vector<float>(frames));
    for (size_t i = 0; i < frames; ++i)
        for (int k = 0; k < keep; ++k) ch[(size_t)k][i] = pcm[i * (size_t)channels + (size_t)k];
    return true;
}

// AIFF or MP3 by extension (WAV goes to the SDK's reader in the plugin).
inline bool loadAiffOrMp3(const std::string& path, double maxSeconds, std::vector<std::vector<float>>& ch, double& sr)
{
    const AudioType t = audioTypeFromName(path);
    if (t != AudioType::Aiff && t != AudioType::Mp3) return false;
    std::vector<uint8_t> bytes;
    if (!readWholeFile(path, bytes)) return false;
    return t == AudioType::Aiff ? decodeAiff(bytes, maxSeconds, ch, sr) : decodeMp3(bytes, maxSeconds, ch, sr);
}

}} // namespace slowops::scratch
