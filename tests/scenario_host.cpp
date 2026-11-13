// scenario_host.cpp - drives the real native plugin build through the VST2 ABI (as the MPC
// does) with generated control-vinyl audio, then checks the sound and the display parameters.
//
// Complements the SDK's host_sim, which can only feed noise. Build:
//   c++ -std=c++17 -O2 -I third_party/mpc-plugin-builder-sdk/framework -I core/include \
//       tests/scenario_host.cpp -o build/scenario_host
// Run:  build/scenario_host plugin/build/native/scratchivision.dylib [out_dir]
//       (out_dir gets baby-scratch-in.wav / baby-scratch-out.wav for listening)

#include "mpc_vst2_abi.h"
#include "slowops/scratch/TimecodeGen.hpp"

#include <dlfcn.h>
#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { if (cond) { ++g_pass; } else { ++g_fail; std::printf("  FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// Parameter indices (plugin/src/Scratchivision.cpp).
enum P { kGain = 0, kSample, kRpm, kMode, kLoop, kThru, kCue, kSwap, kTest,
         kPitch, kLock, kLevel, kRecPos, kDir, kWaveX, kSpeedPlot, kDot0,
         kPrev = kDot0 + 64, kNext, kRescan, kZoom, kInfo, kBar0,
         kCuePad0 = kBar0 + 128, kCueClear = kCuePad0 + 5, kCueState0, kCueX0 = kCueState0 + 5, kCueShown0 = kCueX0 + 5,
         kZoomOut = kCueShown0 + 5, kZoomIn, kPlayX, kNum };
static float zoomNorm(int index) { return (float)index / 6.0f; }   // 0.25, 0.5, 1, 2, 4, 8 s, All
static float sampleNorm(int index) { return (float)index / 63.0f; }   // Sample is 1..64

static int g_automate[512] = {};
static VstIntPtr hostCallback(AEffect*, VstInt32 opcode, VstInt32 index, VstIntPtr, void* ptr, float)
{
    static VstTimeInfo ti{};
    switch (opcode)
    {
        case audioMasterAutomate: if (index >= 0 && index < 512) ++g_automate[index]; return 0;
        case audioMasterVersion: return 2400;
        case audioMasterGetTime: ti.sampleRate = 44100.0; ti.tempo = 120.0; ti.flags = kVstTempoValid; return (VstIntPtr)&ti;
        case audioMasterGetVendorString: if (ptr) std::strcpy((char*)ptr, "MPC 3"); return 1;
        case audioMasterGetProductString: if (ptr) std::strcpy((char*)ptr, "MPC 3"); return 1;
        default: return 0;
    }
}

typedef AEffect* (*MainProc)(audioMasterCallback);
static MainProc g_main = nullptr;
static constexpr double SR = 44100.0;

struct Plug
{
    AEffect* e = nullptr;
    std::vector<float> inL, inR, outL, outR;

    VstIntPtr disp(VstInt32 op, VstInt32 idx = 0, VstIntPtr v = 0, void* p = nullptr, float o = 0.0f) { return e->dispatcher(e, op, idx, v, p, o); }
    Plug()
    {
        e = g_main(hostCallback);
        disp(effSetSampleRate, 0, 0, nullptr, (float)SR);
        disp(effSetBlockSize, 0, 512);
        disp(effOpen);
        disp(effSetProgram, 0, 0);
        disp(effMainsChanged, 0, 1);
        const float p0 = e->getParameter(e, 0);   // the MPC's parameter-0 nudge
        e->setParameter(e, 0, p0 >= 0.5f ? 0.0f : 1.0f);
        e->setParameter(e, 0, p0);
        disp(effStartProcess);
    }
    ~Plug() { disp(effStopProcess); disp(effMainsChanged, 0, 0); disp(effClose); }

    void set(int i, float v) { e->setParameter(e, i, v); }
    float get(int i) { return e->getParameter(e, i); }
    std::string text(int i) { char buf[256] = {0}; disp(effGetParamDisplay, i, 0, buf); return buf; }

    // n frames of input from the generator at the given speed, processed in place like the MPC.
    void block(slowops::scratch::test::TimecodeGen* g, float speed, int n)
    {
        std::vector<float> l((size_t)n, 0.0f), r((size_t)n, 0.0f);
        if (g) for (int i = 0; i < n; ++i) g->next(speed, l[(size_t)i], r[(size_t)i]);
        inL.insert(inL.end(), l.begin(), l.end()); inR.insert(inR.end(), r.begin(), r.end());
        float* io[2] = {l.data(), r.data()};
        e->processReplacing(e, io, io, n);
        outL.insert(outL.end(), l.begin(), l.end()); outR.insert(outR.end(), r.begin(), r.end());
    }
    void run(slowops::scratch::test::TimecodeGen* g, float speed, double sec, int blk = 128)
    {
        for (int k = 0; k < (int)(sec * SR) / blk; ++k) block(g, speed, blk);
    }
};

static double rms(const std::vector<float>& v, size_t from, size_t to)
{
    if (to > v.size()) to = v.size();
    double s = 0; for (size_t i = from; i < to; ++i) s += (double)v[i] * v[i];
    return to > from ? std::sqrt(s / (double)(to - from)) : 0.0;
}

static void writeWav(const std::string& path, const std::vector<float>& l, const std::vector<float>& r)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    const uint32_t n = (uint32_t)l.size(), data = n * 4, rate = 44100;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(36 + data); std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(2); u32(rate); u32(rate * 4); u16(4); u16(16);
    std::fwrite("data", 1, 4, f); u32(data);
    for (uint32_t i = 0; i < n; ++i)
        for (float s : {l[i], r[i]}) { const float c = s > 1 ? 1 : (s < -1 ? -1 : s); u16((uint16_t)(int16_t)std::lround(c * 32767.0f)); }
    std::fclose(f);
}

// Bar values select one of 48 film-strip frames: 0..23 = height ahead of the playhead, 24..47 =
// height of an already-played bar.
static int barFrame(Plug& p, int k) { return (int)std::lround(p.get(kBar0 + k) * 47.0f); }
static float barHeight(Plug& p, int k) { return (float)(barFrame(p, k) % 24) / 23.0f; }
static bool barPlayed(Plug& p, int k) { return barFrame(p, k) >= 24; }
static float barsMax(Plug& p, int from, int to) { float m = 0; for (int k = from; k < to; ++k) m = std::fmax(m, barHeight(p, k)); return m; }

static void writeSine(const std::string& path, double seconds, double hz)
{
    std::vector<float> l((size_t)(seconds * SR));
    for (size_t i = 0; i < l.size(); ++i) l[i] = 0.4f * (float)std::sin(2.0 * 3.14159265358979 * hz * i / SR);
    writeWav(path, l, l);
}

// Run silent blocks until the worker has loaded the selected sample (Sample Info stops saying Loading).
static void settle(Plug& p)
{
    // The loader thread wakes every 10 ms of real time, while this host runs far faster than real
    // time, so wait in real time too.
    for (int i = 0; i < 400; ++i) { p.run(nullptr, 0.0f, 0.02); if (p.text(kInfo) != "Loading...") return; usleep(5000); }
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: scenario_host <plugin.dylib|.so> [out_dir]\n"); return 1; }
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::printf("dlopen failed: %s\n", dlerror()); return 1; }
    g_main = (MainProc)dlsym(lib, "VSTPluginMain");
    if (!g_main) { std::printf("no VSTPluginMain\n"); return 1; }
    const std::string outDir = argc > 2 ? argv[2] : "";
    using slowops::scratch::test::TimecodeGen;

    std::printf("== parameters\n");
    {
        Plug p;
        CHECK(p.e->numParams == kNum, "parameter count %d (expected %d)", p.e->numParams, kNum);
        char name[64] = {0}; p.disp(effGetParamName, kBar0, 0, name);
        CHECK(std::string(name) == "Bar 1", "parameter %d is Bar 1 (got '%s')", kBar0, name);
        CHECK(p.text(kSample) == "ah beep", "Sample shows the file name ('%s')", p.text(kSample).c_str());
        CHECK(p.text(kLock) == "No Signal", "no input: Lock reads No Signal ('%s')", p.text(kLock).c_str());
    }

    std::printf("== normal play: sound follows the record, displays update\n");
    {
        Plug p; TimecodeGen g(SR); g.setCycles(60000.0);
        p.run(&g, 1.0f, 0.6);
        const size_t n = p.outL.size();
        CHECK(rms(p.outL, n / 3, n) > 0.01, "sample audible at speed 1 (rms %.4f)", rms(p.outL, n / 3, n));
        CHECK(p.text(kLock) == "Locked", "Lock = Locked ('%s')", p.text(kLock).c_str());
        CHECK(p.text(kPitch) == "+1.00x", "Pitch = +1.00x ('%s')", p.text(kPitch).c_str());
        CHECK(p.text(kDir) == "Fwd", "Direction = Fwd ('%s')", p.text(kDir).c_str());
        CHECK(p.text(kRecPos) == "1:00.5" || p.text(kRecPos) == "1:00.6", "record position ~1:00.6 ('%s')", p.text(kRecPos).c_str());
        // 0.6 s in (display up to 33 ms behind): position ~0.51 of the sample, so x = 1 - 0.51.
        CHECK(barsMax(p, 0, 64) > 0.3f && barsMax(p, 64, 128) > 0.3f, "waveform bars both sides of the playhead (%.2f / %.2f)", barsMax(p, 0, 64), barsMax(p, 64, 128));
        int moved = 0;
        for (int d = 0; d < 32; ++d) moved += (std::fabs(p.get(kDot0 + 2 * d) - 0.5f) > 0.05f || std::fabs(p.get(kDot0 + 2 * d + 1) - 0.5f) > 0.05f);
        CHECK(moved >= 28, "scope dots off centre (%d of 32)", moved);
        double r2 = 0; for (int d = 0; d < 32; ++d) { const double x = p.get(kDot0 + 2 * d) - 0.5, y = p.get(kDot0 + 2 * d + 1) - 0.5; r2 += std::sqrt(x * x + y * y); }
        CHECK(r2 / 32 > 0.2 && r2 / 32 < 0.5, "scope dots on a ring (mean radius %.3f)", r2 / 32);
        CHECK(g_automate[kBar0 + 64] > 5, "bars notified to the host (%d times)", g_automate[kBar0 + 64]);
    }

    std::printf("== platter held: silence\n");
    {
        Plug p; TimecodeGen g(SR); g.setCycles(5000.0);
        p.run(&g, 1.0f, 0.3);
        p.run(&g, 0.0f, 0.3);
        const size_t n = p.outL.size();
        CHECK(rms(p.outL, n - 4410, n) < 1e-4, "silent while held (rms %.6f)", rms(p.outL, n - 4410, n));
        CHECK(p.text(kDir) == "Stop", "Direction = Stop ('%s')", p.text(kDir).c_str());
    }

    std::printf("== baby scratch (4 Hz back and forth), Sample 2, Loop on\n");
    {
        Plug p; TimecodeGen g(SR); g.setCycles(30000.0);
        p.set(kSample, sampleNorm(1)); p.set(kLoop, 1.0f);
        settle(p);
        for (int k = 0; k < (int)(SR * 2.0) / 64; ++k)
        {
            const float sp = 1.6f * (float)std::sin(2.0 * 3.14159265358979 * 2.0 * k * 64 / SR);
            p.block(&g, sp, 64);
        }
        const size_t n = p.outL.size();
        CHECK(rms(p.outL, 0, n) > 0.01, "scratch audible (rms %.4f)", rms(p.outL, 0, n));
        bool finite = true; for (float v : p.outL) if (!std::isfinite(v)) finite = false;
        CHECK(finite, "output finite");
        if (!outDir.empty())
        {
            writeWav(outDir + "/baby-scratch-in.wav", p.inL, p.inR);
            writeWav(outDir + "/baby-scratch-out.wav", p.outL, p.outR);
            std::printf("  wrote %s/baby-scratch-{in,out}.wav\n", outDir.c_str());
        }
    }

    std::printf("== plugin loaded while the record already plays: pulling back still plays sample 1\n");
    {
        Plug p; TimecodeGen g(SR); g.setCycles(40000.0);
        p.run(&g, 1.0f, 4.0);                       // runs far off the end of the 1.12 s sample
        size_t n = p.outL.size();
        CHECK(rms(p.outL, n - 4410, n) < 1e-3, "silent past the end (rms %.5f)", rms(p.outL, n - 4410, n));
        p.run(&g, -1.0f, 0.15);                     // pull back
        n = p.outL.size();
        CHECK(rms(p.outL, n - 4410, n) > 0.01, "pulling back plays at once (rms %.4f)", rms(p.outL, n - 4410, n));
    }

    std::printf("== Thru passes the timecode; Cue resets and self-clears\n");
    {
        Plug p; TimecodeGen g(SR); g.setCycles(8000.0);
        p.set(kThru, 1.0f);
        p.run(&g, 1.0f, 0.2);
        const size_t n = p.outL.size();
        double diff = 0; for (size_t i = n - 1000; i < n; ++i) diff = std::fmax(diff, std::fabs(p.outL[i] - p.inL[i]));
        CHECK(diff < 1e-6, "Thru output == input (max diff %g)", diff);
        p.set(kThru, 0.0f);
        p.run(&g, 1.0f, 0.5);
        const float before = barsMax(p, 0, 64);
        p.set(kCue, 1.0f);
        p.run(&g, 0.0f, 0.1);
        CHECK(p.get(kCue) < 0.5f, "Cue turns itself off");
        CHECK(before > 0.3f && barsMax(p, 0, 64) < 0.1f && barsMax(p, 64, 128) > 0.3f,   // < 0.1: the platter was still slowing for ~1 ms after CUE
              "Cue: nothing left of the playhead, the sample to its right (%.2f -> %.2f / %.2f)", before, barsMax(p, 0, 64), barsMax(p, 64, 128));
    }

    std::printf("== Test Pattern runs without any input\n");
    {
        Plug p;
        p.set(kTest, 1.0f);
        p.run(nullptr, 0.0f, 1.0);
        const size_t n = p.outL.size();
        CHECK(rms(p.outL, n / 4, n) > 0.01, "Test Pattern makes sound (rms %.4f)", rms(p.outL, n / 4, n));
        CHECK(p.text(kLock) == "Locked", "Test Pattern locks ('%s')", p.text(kLock).c_str());
        CHECK(barsMax(p, 0, 64) > 0.0f, "Test Pattern moves the waveform (played side %.2f)", barsMax(p, 0, 64));
    }

    std::printf("== Swap reverses the direction\n");
    {
        Plug p; TimecodeGen g(SR); g.setCycles(9000.0);
        p.set(kSwap, 1.0f);
        p.run(&g, 1.0f, 0.3);
        CHECK(p.text(kDir) == "Rev", "crossed decode reads Rev ('%s')", p.text(kDir).c_str());
    }

    std::printf("== user WAVs: Scratchivision/ folder, Prev / Next, Rescan, saved by name\n");
    {
        char tmpl[] = "/tmp/scratchivision-user-XXXXXX";
        const std::string dir = mkdtemp(tmpl);
        setenv("SCRATCHIVISION_USER_DIR", dir.c_str(), 1);
        writeSine(dir + "/u1.wav", 0.5, 300.0);
        writeSine(dir + "/u2.wav", 0.5, 500.0);
        writeSine(dir + "/long.wav", 25.0, 200.0);
        { FILE* f = std::fopen((dir + "/bad.wav").c_str(), "wb"); std::fputs("not a wav", f); std::fclose(f); }
        { FILE* f = std::fopen((dir + "/._u1.wav").c_str(), "wb"); std::fputs("mac junk", f); std::fclose(f); }
        // MP3 and AIFF (fixtures made with ffmpeg: 0.5 s of 440 Hz).
        for (const char* fx : {"tone48.mp3", "tone16.aif"})
        {
            std::vector<uint8_t> b; FILE* in = std::fopen((std::string(FIXTURES) + "/" + fx).c_str(), "rb");
            if (in) { int c; while ((c = std::fgetc(in)) != EOF) b.push_back((uint8_t)c); std::fclose(in); }
            FILE* out = std::fopen((dir + "/zz-" + fx).c_str(), "wb"); std::fwrite(b.data(), 1, b.size(), out); std::fclose(out);
        }
        // List: ah beep, fressss (bundled), then bad, long, u1, u2, zz-tone16 (aif), zz-tone48 (mp3).
        std::vector<uint8_t> chunk;
        {
            Plug p;
            settle(p);
            CHECK(p.text(kInfo) == "1/8  1.1 s", "Info '1/8  1.1 s' ('%s')", p.text(kInfo).c_str());
            p.set(kNext, 1.0f); settle(p);
            CHECK(p.text(kSample) == "fressss" && p.get(kNext) < 0.5f, "Next -> fressss, and Next turns itself off ('%s')", p.text(kSample).c_str());
            p.set(kNext, 1.0f); settle(p);
            CHECK(p.text(kSample) == "bad" && p.text(kInfo) == "Can't read", "unreadable WAV: '%s' / '%s'", p.text(kSample).c_str(), p.text(kInfo).c_str());
            p.set(kNext, 1.0f); settle(p);
            CHECK(p.text(kInfo) == "4/8  cut to 20 s", "25 s WAV is cut ('%s')", p.text(kInfo).c_str());
            p.set(kNext, 1.0f); settle(p);
            p.set(kNext, 1.0f); settle(p);
            p.set(kPrev, 1.0f); settle(p);
            CHECK(p.text(kSample) == "u1", "Next x2, Prev -> u1 ('%s')", p.text(kSample).c_str());
            TimecodeGen g(SR); g.setCycles(3000.0);
            p.run(&g, 1.0f, 0.3);
            const size_t n = p.outL.size();
            CHECK(rms(p.outL, n - 4410, n) > 0.01, "user WAV plays (rms %.4f)", rms(p.outL, n - 4410, n));
            p.set(kSample, sampleNorm(63)); settle(p);
            CHECK(p.text(kSample) == "zz-tone48", "Sample beyond the list clamps to the last ('%s')", p.text(kSample).c_str());
            CHECK(p.text(kInfo) == "8/8  0.5 s", "MP3 loads ('%s')", p.text(kInfo).c_str());
            p.run(&g, 1.0f, 0.2);
            const size_t m = p.outL.size();
            CHECK(rms(p.outL, m - 4410, m) > 0.01, "MP3 plays (rms %.4f)", rms(p.outL, m - 4410, m));
            p.set(kPrev, 1.0f); settle(p);
            CHECK(p.text(kSample) == "zz-tone16" && p.text(kInfo) == "7/8  0.5 s", "AIFF loads ('%s', '%s')", p.text(kSample).c_str(), p.text(kInfo).c_str());
            p.set(kPrev, 1.0f); settle(p);
            p.set(kPrev, 1.0f); settle(p);
            // A new file that sorts before u1: after RESCAN, u1 is still selected (index moved).
            writeSine(dir + "/a-new.wav", 0.3, 700.0);
            p.set(kRescan, 1.0f);
            for (int i = 0; i < 400 && p.text(kInfo).rfind("6/9", 0) != 0; ++i) { p.run(nullptr, 0.0f, 0.02); usleep(5000); }
            CHECK(p.text(kSample) == "u1" && p.text(kInfo).rfind("6/9", 0) == 0, "Rescan keeps u1 (now 6/9): '%s' '%s'", p.text(kSample).c_str(), p.text(kInfo).c_str());
            void* data = nullptr;
            const VstIntPtr size = p.disp(effGetChunk, 0, 0, &data);
            if (size > 0 && data) chunk.assign((uint8_t*)data, (uint8_t*)data + size);
        }
        // Another file appears before reopening: the project still comes back on u1.
        writeSine(dir + "/0-first.wav", 0.3, 900.0);
        {
            Plug p;
            p.disp(effSetChunk, 0, (VstIntPtr)chunk.size(), chunk.data());
            settle(p);
            CHECK(p.text(kSample) == "u1", "saved project reopens on u1 by name ('%s', info '%s')", p.text(kSample).c_str(), p.text(kInfo).c_str());
        }
        unsetenv("SCRATCHIVISION_USER_DIR");
    }

    std::printf("== zoom: - / + buttons, All shows the whole sample with a moving playhead\n");
    {
        Plug p; TimecodeGen g(SR); g.setCycles(7000.0);
        CHECK(p.text(kZoom) == "1 s", "default zoom 1 s ('%s')", p.text(kZoom).c_str());
        p.set(kZoomIn, 1.0f); p.run(nullptr, 0.0f, 0.02);
        CHECK(p.text(kZoom) == "0.5 s" && p.get(kZoomIn) < 0.5f, "+ zooms in to 0.5 s and resets ('%s')", p.text(kZoom).c_str());
        for (int i = 0; i < 3; ++i) { p.set(kZoomIn, 1.0f); p.run(nullptr, 0.0f, 0.02); }
        CHECK(p.text(kZoom) == "0.25 s", "+ stops at 0.25 s ('%s')", p.text(kZoom).c_str());
        for (int i = 0; i < 9; ++i) { p.set(kZoomOut, 1.0f); p.run(nullptr, 0.0f, 0.02); }
        CHECK(p.text(kZoom) == "All", "- goes up to All and stops there ('%s')", p.text(kZoom).c_str());
        p.run(&g, 1.0f, 0.3);   // 0.3 s into the 1.12 s sample
        p.run(&g, 0.0f, 0.05);
        int lit = 0; for (int k = 0; k < 128; ++k) lit += barHeight(p, k) > 0.0f;
        CHECK(lit > 100, "All: the whole sample fills the window (%d of 128 bars)", lit);
        const float px = p.get(kPlayX);
        CHECK(px > 0.2f && px < 0.33f, "All: moving playhead at ~0.27 of the window (%.3f)", px);
        int played = 0; for (int k = 0; k < 128; ++k) played += barPlayed(p, k);
        CHECK(std::abs(played - (int)std::lround(px * 128)) <= 1, "All: bars left of the moving playhead are played (%d vs %.0f)", played, px * 128);
        p.set(kZoom, zoomNorm(2)); p.run(nullptr, 0.0f, 0.05);
        played = 0; for (int k = 0; k < 128; ++k) played += barPlayed(p, k);
        CHECK(played == 64, "centred zoom: the left half is played (%d)", played);
    }

    std::printf("== hot cues: set, jump, markers, clear, kept per file\n");
    {
        char tmpl[] = "/tmp/scratchivision-cues-XXXXXX";
        const std::string dir = mkdtemp(tmpl);
        setenv("SCRATCHIVISION_USER_DIR", dir.c_str(), 1);
        writeSine(dir + "/u1.wav", 1.0, 300.0);
        const std::string cueFile = dir + "/u1.wav.cues";
        auto fileText = [&] { std::string t; FILE* f = std::fopen(cueFile.c_str(), "r"); if (f) { int c; while ((c = std::fgetc(f)) != EOF) t += (char)c; std::fclose(f); } return t; };
        auto waitFor = [&](bool want) { for (int i = 0; i < 200 && (!fileText().empty()) != want; ++i) usleep(5000); };
        {
            Plug p;
            p.set(kSample, sampleNorm(2)); settle(p);   // bundled: ah beep, fressss; then u1
            CHECK(p.text(kSample) == "u1", "u1 selected ('%s')", p.text(kSample).c_str());
            TimecodeGen g(SR); g.setCycles(5000.0);
            p.run(&g, 1.0f, 0.4);
            CHECK(p.text(kCueState0 + 1) == "Unset", "Cue 2 starts unset");
            p.set(kCuePad0 + 1, 1.0f);                 // tap Cue 2 (empty): store ~0.4 s
            p.run(&g, 0.0f, 0.05);
            CHECK(p.get(kCuePad0 + 1) < 0.5f && p.text(kCueState0 + 1) == "Set", "Cue 2 set, pad resets ('%s')", p.text(kCueState0 + 1).c_str());
            waitFor(true);
            double saved = -1; std::sscanf(fileText().c_str(), "scratchivision-cues 1\n2 %lf", &saved);
            CHECK(std::fabs(saved - 0.4) < 0.02, "u1.wav.cues has cue 2 at ~0.4 s (%.3f)", saved);
            p.run(&g, 1.0f, 0.3);                      // play on to ~0.7 s
            CHECK(p.get(kCueShown0 + 1) > 0.5f && p.get(kCueX0 + 1) < 0.3f, "marker 2 left of the playhead (x %.3f)", p.get(kCueX0 + 1));
            p.set(kCuePad0 + 1, 1.0f);                 // tap Cue 2 (set): jump back
            p.run(&g, 0.0f, 0.05);
            CHECK(std::fabs(p.get(kCueX0 + 1) - 0.5f) < 0.01f, "after the jump the marker is at the playhead (x %.3f)", p.get(kCueX0 + 1));
            p.run(&g, 1.0f, 0.15);
            const size_t n = p.outL.size();
            CHECK(rms(p.outL, n - 2205, n) > 0.01, "plays on from the cue (rms %.4f)", rms(p.outL, n - 2205, n));
            p.set(kZoom, zoomNorm(1));                 // 0.5 s window; run to the end of the 1 s sample
            p.run(&g, 1.0f, 1.0);
            CHECK(p.get(kCueShown0 + 1) < 0.5f, "cue outside the 0.5 s window is hidden");
            p.set(kZoom, zoomNorm(4));                 // 4 s window: back in view
            p.run(&g, 0.0f, 0.05);
            CHECK(p.get(kCueShown0 + 1) > 0.5f, "zoomed out to 4 s: shown again");
            p.set(kCuePad0 + 3, 1.0f);                 // set Cue 4 at the end
            p.run(&g, 0.0f, 0.05);
            p.set(kCueClear, 1.0f); p.set(kCuePad0 + 1, 1.0f);   // CLEAR, then Cue 2
            p.run(&g, 0.0f, 0.05);
            CHECK(p.text(kCueState0 + 1) == "Unset" && p.get(kCueClear) < 0.5f, "CLEAR deletes Cue 2 and turns itself off");
            CHECK(p.text(kCueState0 + 3) == "Set", "Cue 4 untouched");
            for (int i = 0; i < 200 && fileText().find("2 ") != std::string::npos; ++i) usleep(5000);
            CHECK(fileText().find("4 ") != std::string::npos && fileText().find("\n2 ") == std::string::npos, "file updated: only cue 4 ('%s')", fileText().c_str());
        }
        {
            Plug p;                                    // a new instance (another project): cues come from the file
            p.set(kSample, sampleNorm(2)); settle(p);
            p.run(nullptr, 0.0f, 0.05);
            CHECK(p.text(kCueState0 + 3) == "Set" && p.text(kCueState0 + 1) == "Unset", "cues restored from u1.wav.cues");
            p.set(kCueClear, 1.0f); p.set(kCuePad0 + 3, 1.0f);
            p.run(nullptr, 0.0f, 0.05);
        }
        waitFor(false);
        CHECK(fileText().empty(), "last cue cleared: file removed");
        unsetenv("SCRATCHIVISION_USER_DIR");
    }

    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
