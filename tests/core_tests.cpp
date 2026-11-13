// core_tests.cpp - unit tests for the host-agnostic Scratchivision core.
//
// SPDX-License-Identifier: MIT
//
// Build: c++ -std=c++17 -O2 -Wall -Wextra -I core/include tests/core_tests.cpp -o build/core_tests

#include "slowops/scratch/AudioFile.hpp"
#include "slowops/scratch/CuePoints.hpp"
#include "slowops/scratch/SampleLibrary.hpp"
#include "slowops/scratch/ScratchEngine.hpp"
#include "slowops/scratch/WaveBars.hpp"
#include "slowops/scratch/TimecodeGen.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace slowops::scratch;
using test::TimecodeGen;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { if (cond) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static constexpr double SR = 44100.0;

// Run the decoder on generated timecode at a constant speed.
static void run(TimecodeDecoder& d, TimecodeGen& g, float speed, double seconds)
{
    const int n = (int)(seconds * SR);
    for (int i = 0; i < n; ++i) { float l, r; g.next(speed, l, r); d.process(l, r); }
}

static void testLfsr()
{
    const TimecodeFormat& f = seratoCv02A();
    uint32_t s = f.seed;
    bool inverse = true;
    for (int i = 0; i < 5000; ++i) { const uint32_t n = lfsrNext(s, f); if (lfsrPrev(n, f) != s) inverse = false; s = n; }
    CHECK(inverse, "lfsrPrev undoes lfsrNext");

    LfsrTable t; t.build(f);
    s = f.seed;
    int bad = 0;
    for (int32_t k = 0; k < f.length; ++k) { if (t.lookup(s) != k) ++bad; s = lfsrNext(s, f); }
    CHECK(bad == 0, "every register value on the side is unique (%d collisions)", bad);
}

static void testSpeed(float speed, double tol)
{
    TimecodeDecoder d; d.prepare(SR);
    TimecodeGen g(SR);
    g.setCycles(speed >= 0 ? 5000.0 : 400000.0);
    run(d, g, speed, 0.25);
    CHECK(std::fabs(d.speed() - speed) <= tol * std::fabs(speed), "speed %.2f measured %.4f", speed, d.speed());
    CHECK(d.locked(), "locked at speed %.2f", speed);
    const double err = d.positionCycles() - g.cycles();
    CHECK(std::fabs(err) < 1.0, "position at speed %.2f: decoder %.2f vs record %.2f cycles", speed, d.positionCycles(), g.cycles());
}

static void testLockTime()
{
    TimecodeDecoder d; d.prepare(SR);
    TimecodeGen g(SR); g.setCycles(123456.0);
    int samples = 0;
    while (!d.locked() && samples < (int)SR) { float l, r; g.next(1.0f, l, r); d.process(l, r); ++samples; }
    const double ms = samples * 1000.0 / SR;
    CHECK(d.locked() && ms < 40.0, "locks within 40 ms at normal speed (took %.1f ms)", ms);
    const double sec = d.state().positionSec;
    CHECK(std::fabs(sec - g.cycles() / 1000.0) < 0.002, "absolute position %.4f s vs %.4f s", sec, g.cycles() / 1000.0);
}

// No false locks: from many start positions and both directions, the first lock is correct.
static void testNoFalseLock()
{
    int wrong = 0, never = 0;
    uint32_t rng = 99;
    for (int trial = 0; trial < 200; ++trial)
    {
        rng = rng * 1664525u + 1013904223u;
        const double start = 1000.0 + (rng >> 8) % 700000;
        const float sp = (trial & 1) ? -1.0f : (trial % 4 == 0 ? 0.6f : 1.3f);
        TimecodeDecoder d; d.prepare(SR);
        TimecodeGen g(SR); g.setCycles(start);
        int n = 0;
        while (!d.locked() && n < (int)SR) { float l, r; g.next(sp, l, r); d.process(l, r); ++n; }
        if (!d.locked()) ++never;
        else if (std::fabs(d.positionCycles() - g.cycles()) > 1.0) ++wrong;
    }
    CHECK(wrong == 0, "first lock correct from 200 random starts (%d wrong)", wrong);
    CHECK(never == 0, "locks from every start (%d never locked)", never);
}

static void testStopAndReverse()
{
    TimecodeDecoder d; d.prepare(SR);
    TimecodeGen g(SR); g.setCycles(20000.0);
    run(d, g, 1.0f, 0.1);
    run(d, g, 0.0f, 0.15);
    CHECK(d.speed() == 0.0f, "held platter reads speed 0 (got %.4f)", d.speed());

    run(d, g, 1.0f, 0.1);
    run(d, g, -1.0f, 0.01);
    CHECK(d.speed() < -0.9f, "reversal shows within 10 ms (speed %.3f)", d.speed());
    run(d, g, -1.0f, 0.1);
    CHECK(d.locked(), "relocks after reversing");
    CHECK(std::fabs(d.positionCycles() - g.cycles()) < 1.0, "position after reversal %.2f vs %.2f", d.positionCycles(), g.cycles());
}

static void testBabyScratch()
{
    // Back and forth at +-1.5 with 8 Hz cycles, as a hand would do. Position must stay with the record.
    TimecodeDecoder d; d.prepare(SR);
    TimecodeGen g(SR); g.setCycles(50000.0);
    double maxErr = 0.0;
    for (int i = 0; i < (int)(SR * 1.0); ++i)
    {
        const float sp = 1.5f * (float)std::sin(2.0 * 3.14159265358979 * 4.0 * i / SR);
        float l, r; g.next(sp, l, r); d.process(l, r);
        if (i > (int)(SR * 0.1)) { const double e = std::fabs(d.positionCycles() - g.cycles()); if (e > maxErr) maxErr = e; }
    }
    CHECK(maxErr < 1.5, "baby scratch: position error at most %.2f cycles", maxErr);
}

static void testNoiseAndLevel()
{
    TimecodeDecoder d; d.prepare(SR);
    TimecodeGen g(SR); g.setCycles(300000.0);
    g.setAmplitude(0.05f, 0.03f);   // quiet phono input
    g.setNoise(0.004f);
    run(d, g, 1.0f, 0.3);
    CHECK(std::fabs(d.speed() - 1.0f) < 0.02f, "quiet + noisy input: speed %.4f", d.speed());
    CHECK(d.locked(), "quiet + noisy input still locks");
}

static void test45()
{
    TimecodeDecoder d; d.prepare(SR); d.setNominalRpm45(true);
    TimecodeGen g(SR); g.setRpm45(true); g.setCycles(9000.0);
    run(d, g, 1.0f, 0.2);
    CHECK(std::fabs(d.speed() - 1.0f) < 0.01f, "45 rpm: speed %.4f", d.speed());
}

static void testSwap()
{
    TimecodeDecoder d; d.prepare(SR); d.setSwap(true);
    TimecodeGen g(SR); g.setCycles(9000.0);
    std::vector<float> L, R;
    g.render(1.0f, (int)(SR * 0.1), L, R);
    for (size_t i = 0; i < L.size(); ++i) d.process(R[i], L[i]);   // wired the other way round
    CHECK(d.speed() > 0.99f && d.speed() < 1.01f, "Swap fixes crossed channels (speed %.4f)", d.speed());
}

// --- player / engine -------------------------------------------------------

struct TestSample
{
    std::vector<float> l, r;
    explicit TestSample(int frames) : l((size_t)frames), r((size_t)frames)
    {
        for (int i = 0; i < frames; ++i) { l[(size_t)i] = 0.5f * (float)std::sin(2.0 * 3.14159265358979 * 440.0 * i / SR); r[(size_t)i] = l[(size_t)i]; }
    }
    SampleView view() const { SampleView v; v.left = l.data(); v.right = r.data(); v.frames = (int64_t)l.size(); v.sampleRate = SR; return v; }
};

static double rms(const std::vector<float>& v, size_t from, size_t to)
{
    double s = 0; for (size_t i = from; i < to; ++i) s += (double)v[i] * v[i];
    return std::sqrt(s / (double)(to - from > 0 ? to - from : 1));
}

static void testPlayer()
{
    TestSample smp((int)SR);   // 1 s
    ScratchPlayer p; p.prepare(SR); p.setSample(smp.view());
    std::vector<float> L(4096), R(4096);
    // Rest: silent.
    p.process(L.data(), R.data(), 4096, 0.0f, 0.0, false, 1.0f);
    CHECK(rms(L, 0, 4096) < 1e-6, "silent at rest");
    // Normal speed for 0.5 s.
    for (int b = 0; b < (int)(SR * 0.5) / 512; ++b) p.process(L.data(), R.data(), 512, 1.0f, 0.0, false, 1.0f);
    CHECK(std::fabs(p.positionFrames() - SR * 0.5) < 600.0, "position after 0.5 s at speed 1: %.0f", p.positionFrames());
    CHECK(rms(L, 0, 512) > 0.2, "plays the sample (rms %.3f)", rms(L, 0, 512));
    // Past the end without loop: silent.
    for (int b = 0; b < 100; ++b) p.process(L.data(), R.data(), 512, 2.0f, 0.0, false, 1.0f);
    CHECK(rms(L, 0, 512) < 0.01, "silent past the end (rms %.4f)", rms(L, 0, 512));
    // Loop wraps.
    p.setLoop(true);
    p.process(L.data(), R.data(), 512, 1.0f, 0.0, false, 1.0f);
    p.process(L.data(), R.data(), 512, 1.0f, 0.0, false, 1.0f);
    CHECK(p.positionFrames() >= 0.0 && p.positionFrames() < SR, "loop wraps (pos %.0f)", p.positionFrames());
    CHECK(rms(L, 0, 512) > 0.2, "loop plays (rms %.3f)", rms(L, 0, 512));
    // Backwards past the start without loop: stops just before the start, silent.
    p.setLoop(false); p.cue(0.0, false);
    for (int b = 0; b < 20; ++b) p.process(L.data(), R.data(), 512, -1.0f, 0.0, false, 1.0f);
    CHECK(p.positionFrames() == -1.0, "reverse stops at the start (%.0f)", p.positionFrames());
    CHECK(rms(L, 0, 512) < 0.01, "silent before the start (rms %.4f)", rms(L, 0, 512));
    // Run far off the end (3 s at speed 1 into a 1 s sample), then pull back: sound at once.
    for (int b = 0; b < (int)(SR * 3.0) / 512; ++b) p.process(L.data(), R.data(), 512, 1.0f, 0.0, false, 1.0f);
    CHECK(p.positionFrames() == SR, "forward stops at the end (%.0f)", p.positionFrames());
    p.process(L.data(), R.data(), 512, -1.0f, 0.0, false, 1.0f);
    p.process(L.data(), R.data(), 512, -1.0f, 0.0, false, 1.0f);
    CHECK(rms(L, 0, 512) > 0.2, "pulling back after running off the end plays at once (rms %.3f)", rms(L, 0, 512));
}

static std::vector<float> renderEngine(int block, bool inPlace, const std::vector<float>& inL, const std::vector<float>& inR,
                                       double* finalPos = nullptr)
{
    TestSample smp((int)SR * 2);
    ScratchEngine e; e.prepare(SR); e.setSample(smp.view());
    EngineSettings set;
    std::vector<float> outL(inL.size()), outR(inR.size()), tl(inL), tr(inR);
    for (size_t off = 0; off < inL.size(); off += (size_t)block)
    {
        const int n = (int)std::min((size_t)block, inL.size() - off);
        if (inPlace) e.process(tl.data() + off, tr.data() + off, tl.data() + off, tr.data() + off, n, set);
        else e.process(inL.data() + off, inR.data() + off, outL.data() + off, outR.data() + off, n, set);
    }
    if (finalPos) *finalPos = e.positionFrames();
    return inPlace ? tl : outL;
}

static void testEngine()
{
    TimecodeGen g(SR); g.setCycles(77777.0);
    std::vector<float> L, R;
    g.render(1.0f, (int)(SR * 0.5), L, R);
    double pos = 0;
    const std::vector<float> a = renderEngine(512, false, L, R, &pos);
    CHECK(std::fabs(pos - SR * 0.5) < SR * 0.02, "engine follows timecode: %.0f frames after 0.5 s", pos);
    CHECK(rms(a, (size_t)(SR * 0.2), (size_t)(SR * 0.5)) > 0.2, "engine output plays the sample");
    // Block sizes that are multiples of the 32-frame chunk give identical output; in-place too.
    const std::vector<float> b = renderEngine(128, false, L, R);
    const std::vector<float> c = renderEngine(512, true, L, R);
    double d1 = 0, d2 = 0;
    for (size_t i = 0; i < a.size(); ++i) { d1 = std::max(d1, (double)std::fabs(a[i] - b[i])); d2 = std::max(d2, (double)std::fabs(a[i] - c[i])); }
    CHECK(d1 < 1e-6, "block size 128 vs 512 identical (max diff %g)", d1);
    CHECK(d2 < 1e-6, "in-place identical (max diff %g)", d2);
    // Odd block sizes still work and stay close.
    double pos2 = 0;
    renderEngine(37, false, L, R, &pos2);
    CHECK(std::fabs(pos2 - pos) < 64.0, "odd block size: position %.0f vs %.0f", pos2, pos);
}

static void testAbsolute()
{
    TestSample smp((int)SR * 2);
    ScratchEngine e; e.prepare(SR); e.setSample(smp.view());
    EngineSettings set; set.mode = PlayMode::Absolute;
    TimecodeGen g(SR); g.setCycles(200000.0);
    std::vector<float> L(512), R(512);
    auto play = [&](float sp, double sec) {
        for (int b = 0; b < (int)(sec * SR) / 512; ++b)
        {
            for (int i = 0; i < 512; ++i) g.next(sp, L[(size_t)i], R[(size_t)i]);
            e.process(L.data(), R.data(), L.data(), R.data(), 512, set);
        }
    };
    play(1.0f, 0.1);
    e.cue();
    const double cueAt = g.cycles();
    play(1.0f, 0.3);
    const double expect = (g.cycles() - cueAt) / 1000.0 * SR;
    CHECK(std::fabs(e.positionFrames() - expect) < SR * 0.005, "absolute: %.0f vs %.0f frames", e.positionFrames(), expect);
    // Needle drop 0.4 s further on: the play position follows.
    g.setCycles(g.cycles() + 400.0);
    play(1.0f, 0.1);
    const double expect2 = (g.cycles() - cueAt) / 1000.0 * SR;
    CHECK(std::fabs(e.positionFrames() - expect2) < SR * 0.005, "needle drop followed: %.0f vs %.0f frames", e.positionFrames(), expect2);
}


// --- sample library / bars ----------------------------------------------------

static void touch(const std::string& p) { std::ofstream(p) << "x"; }

// Fake loader: every frame holds the file name's length, so a read can be checked against the
// sample it came from. "bad" files fail; "long" files are 30 s.
static bool fakeLoad(const std::string& path, std::vector<std::vector<float>>& ch, double& sr)
{
    sr = SR;
    const std::string f = path.substr(path.rfind('/') + 1);
    if (f.find("bad") != std::string::npos) return false;
    const size_t n = f.find("long") != std::string::npos ? (size_t)(SR * 30) : 4410;
    ch.assign(2, std::vector<float>(n, (float)f.size() / 100.0f));
    return true;
}

static std::string tempDir(const char* tag)
{
    char tmpl[256];
    std::snprintf(tmpl, sizeof(tmpl), "/tmp/scratchivision-%s-XXXXXX", tag);
    const char* d = mkdtemp(tmpl);
    return d ? d : "";
}

static void testLibraryScan()
{
    const std::string b = tempDir("bundled"), u = tempDir("user");
    touch(b + "/zed.wav"); touch(b + "/alpha.wav");
    touch(u + "/b.wav"); touch(u + "/A.WAV"); touch(u + "/._a.wav"); touch(u + "/.hidden.wav"); touch(u + "/notes.txt");
    touch(u + "/c.mp3"); touch(u + "/d.aif"); touch(u + "/e.flac");
    mkdir((u + "/folder.wav").c_str(), 0755);
    SampleLibrary lib; lib.setFolders(b, u); lib.scanNow();
    CHECK(lib.count() == 6, "6 audio files listed (got %d)", lib.count());
    CHECK(lib.name(0) == "alpha" && lib.name(1) == "zed", "bundled first, sorted (%s, %s)", lib.name(0).c_str(), lib.name(1).c_str());
    CHECK(lib.name(2) == "A" && lib.name(3) == "b", "user next, case-insensitive order, .WAV accepted (%s, %s)", lib.name(2).c_str(), lib.name(3).c_str());
    CHECK(lib.find("b.wav") == 3 && lib.find("._a.wav") < 0, "find by file; ._ files skipped");
    CHECK(lib.name(4) == "c" && lib.name(5) == "d" && lib.find("e.flac") < 0, "MP3 and AIFF listed, FLAC not (%s, %s)", lib.name(4).c_str(), lib.name(5).c_str());

    // Several user folders (one per drive): listed in the provider's order after the bundled ones.
    const std::string u2 = tempDir("usb");
    touch(u2 + "/usb-sample.mp3");
    SampleLibrary multi;
    multi.setFolders(b, [u, u2] { return std::vector<std::string>{u, u2}; });
    multi.scanNow();
    CHECK(multi.count() == 7 && multi.name(6) == "usb-sample", "second drive's folder listed last (%d, %s)", multi.count(), multi.name(6).c_str());

    for (int i = 0; i < 70; ++i) { char n[64]; std::snprintf(n, sizeof(n), "/many%02d.wav", i); touch(u + n); }
    lib.scanNow();
    CHECK(lib.count() == SampleLibrary::kMaxEntries, "capped at %d (got %d)", SampleLibrary::kMaxEntries, lib.count());

    SampleLibrary none; none.setFolders(b, "/nonexistent/folder"); none.scanNow();
    CHECK(none.count() == 2, "missing user folder: bundled only (%d)", none.count());
}

static void testLibraryLoadAndRescan()
{
    const std::string u = tempDir("load");
    touch(u + "/b.wav"); touch(u + "/bad.wav"); touch(u + "/long.wav");
    SampleLibrary lib; lib.setFolders("", u); lib.setLoader(fakeLoad); lib.scanNow();
    // order: b, bad, long
    lib.loadNow(2);
    LoadedSample* s = lib.acquire();
    CHECK(s && s->truncated && std::fabs(s->seconds() - SampleLibrary::kMaxSeconds) < 0.01, "30 s file cut to 20 s (%.2f s)", s ? s->seconds() : 0.0);
    lib.loadNow(1);
    s = lib.acquire();
    CHECK(s && s->failed, "unreadable file marked failed");

    // Worker: load b, then add a file that sorts before it and rescan: b keeps its selection.
    lib.start();
    lib.requestLoad(0);
    for (int i = 0; i < 200 && !(lib.current() && lib.current()->file == "b.wav"); ++i) { lib.acquire(); usleep(5000); }
    CHECK(lib.current() && lib.current()->file == "b.wav", "worker loaded b.wav");
    touch(u + "/a.wav");
    lib.requestScan();
    int remap = -1;
    for (int i = 0; i < 200 && remap < 0; ++i) { remap = lib.takeRemap(); usleep(5000); }
    CHECK(remap == 1, "after rescan b.wav is index 1 (remap %d)", remap);
    lib.stop();
}

static void testLibraryHandoffStress()
{
    // The audio thread swaps samples while the worker loads as fast as it can; every read must
    // see intact data from one sample (ASan build catches use-after-free and leaks).
    const std::string u = tempDir("stress");
    const char* names[] = {"a.wav", "bb.wav", "ccc.wav", "dddd.wav"};
    for (const char* n : names) touch(u + "/" + n);
    SampleLibrary lib; lib.setFolders("", u); lib.setLoader(fakeLoad); lib.scanNow();
    lib.start();
    int bad = 0, swaps = 0;
    for (int i = 0; i < 4000; ++i)
    {
        if (i % 7 == 0) lib.requestLoad(i % 4);
        bool changed = false;
        const LoadedSample* s = lib.acquire(&changed);
        swaps += changed;
        if (s)
        {
            const float expect = (float)s->file.size() / 100.0f;
            if (s->left.empty() || s->left[s->left.size() / 2] != expect || s->right[0] != expect) ++bad;
        }
        if (i % 50 == 0) usleep(1000);
    }
    lib.stop();
    CHECK(bad == 0 && swaps > 3, "hand-off stress: %d bad reads, %d swaps", bad, swaps);
}

static void testBars()
{
    // 1 s sample, silent but for a burst at 0.50-0.51 s.
    std::vector<float> l((size_t)SR, 0.0f);
    for (size_t i = (size_t)(SR * 0.5); i < (size_t)(SR * 0.51); ++i) l[i] = 0.5f;
    Envelope e; computeEnvelope(l.data(), l.data(), l.size(), SR, e);
    float bars[128];
    computeBars(e, 0.5, 1.0, 128, bars);
    int loud = -1; for (int k = 0; k < 128; ++k) if (bars[k] > 0.99f) { loud = k; break; }
    CHECK(loud == 63 || loud == 64, "burst at the play position is at the playhead, within one 2.5 ms block (bar %d)", loud);
    float rest = 0; for (int k = 0; k < 128; ++k) if (k < 63 || k > 66) rest = std::fmax(rest, bars[k]);
    CHECK(rest == 0.0f, "silence elsewhere (max %.3f)", rest);
    computeBars(e, 3.0, 1.0, 128, bars);
    float past = 0; for (float v : bars) past = std::fmax(past, v);
    CHECK(past == 0.0f, "past the end: all bars empty");
    computeBars(e, 0.25, 4.0, 128, bars);   // zoomed out: the burst is 0.25 s right of centre = 8 bars
    loud = -1; for (int k = 0; k < 128; ++k) if (bars[k] > 0.99f) { loud = k; break; }
    CHECK(loud == 71 || loud == 72, "zoom 4 s: burst 8 bars right of the playhead (bar %d)", loud);
}


// --- AIFF / MP3 ---------------------------------------------------------------

static void checkTone(const char* file, int expectCh, double expectSr)
{
    const std::string path = std::string(FIXTURES) + "/" + file;
    std::vector<std::vector<float>> ch; double sr = 0;
    const bool ok = loadAiffOrMp3(path, 20.0, ch, sr);
    CHECK(ok, "%s decodes", file);
    if (!ok) return;
    const size_t n = ch[0].size();
    float peak = 0; int zc = 0;
    for (size_t i = 0; i < n; ++i) { peak = std::fmax(peak, std::fabs(ch[0][i])); if (i && (ch[0][i] >= 0) != (ch[0][i - 1] >= 0)) ++zc; }
    CHECK((int)ch.size() == expectCh && sr == expectSr, "%s: %d ch at %.0f Hz (got %d ch, %.0f Hz)", file, expectCh, expectSr, (int)ch.size(), sr);
    CHECK(std::fabs(n / sr - 0.5) < 0.06, "%s: 0.5 s long (%.3f s)", file, n / sr);
    CHECK(peak > 0.08f && peak < 0.14f, "%s: ffmpeg's sine peaks at 1/8, -3 dB when made stereo (%.3f)", file, peak);
    CHECK(zc > 420 && zc < 460, "%s: 440 Hz (%d zero crossings in 0.5 s)", file, zc);
}

static void testAudioFiles()
{
    checkTone("tone16.aif", 2, 44100);
    checkTone("tone24.aif", 2, 44100);
    checkTone("tone-sowt.aifc", 2, 44100);
    checkTone("tone-float.aifc", 1, 44100);
    checkTone("tone48.mp3", 2, 48000);
    std::vector<std::vector<float>> ch; double sr = 0;
    loadAiffOrMp3(std::string(FIXTURES) + "/tone16.aif", 0.1, ch, sr);
    CHECK(!ch.empty() && ch[0].size() <= 4411 + 1, "AIFF read stops at maxSeconds (%zu frames)", ch.empty() ? (size_t)0 : ch[0].size());
    loadAiffOrMp3(std::string(FIXTURES) + "/tone48.mp3", 0.1, ch, sr);
    CHECK(!ch.empty() && ch[0].size() <= 4801 + 1, "MP3 decode stops at maxSeconds (%zu frames)", ch.empty() ? (size_t)0 : ch[0].size());
    CHECK(!loadAiffOrMp3("/nonexistent.mp3", 1.0, ch, sr), "missing file fails cleanly");
    std::vector<uint8_t> junk(1000, 0x55);
    CHECK(!decodeAiff(junk, 1.0, ch, sr) && !decodeMp3(junk, 1.0, ch, sr), "junk fails cleanly");
    CHECK(audioTypeFromName("a.WAV") == AudioType::Wav && audioTypeFromName("b.Aiff") == AudioType::Aiff &&
          audioTypeFromName("c.aifc") == AudioType::Aiff && audioTypeFromName("d.MP3") == AudioType::Mp3 &&
          audioTypeFromName("e.flac") == AudioType::Unknown && audioTypeFromName("noext") == AudioType::Unknown, "extensions recognised");
}


// --- cue points -------------------------------------------------------------------

static void testCuePointsFormat()
{
    CuePoints c; c.sec[0] = 0.5; c.sec[2] = 1.25;
    const CuePoints r = CuePoints::parse(c.format(), 10.0);
    CHECK(r == c, "cue file round trip");
    const CuePoints bad = CuePoints::parse("scratchivision-cues 1\n1 0.5\n7 1.0\nx y\n2 -3\n3 99\n4 abc\n5 2.0\n", 10.0);
    CHECK(bad.isSet(0) && !bad.isSet(1) && !bad.isSet(2) && !bad.isSet(3) && bad.sec[4] == 2.0,
          "bad lines, bad numbers and cues past the end are ignored");
    CHECK(!CuePoints::parse("hello\n1 0.5\n", 10.0).any(), "no header: nothing loaded");
}

static void testJumpTo()
{
    TestSample smp((int)SR * 2);
    // Relative: jump, then the platter moves the sample on from the cue.
    ScratchPlayer p; p.prepare(SR); p.setSample(smp.view());
    std::vector<float> L(512), R(512);
    for (int b = 0; b < 20; ++b) p.process(L.data(), R.data(), 512, 1.0f, 0.0, false, 1.0f);
    p.jumpTo(SR * 0.3, 0.0, false);
    for (int b = 0; b < 10; ++b) p.process(L.data(), R.data(), 512, 1.0f, 0.0, false, 1.0f);
    CHECK(std::fabs(p.positionFrames() - (SR * 0.3 + 5120)) < 300, "Rel: jump then play on (%.0f)", p.positionFrames());

    // Absolute: the jump re-anchors; the needle keeps steering (including a needle drop).
    ScratchEngine e; e.prepare(SR); e.setSample(smp.view());
    EngineSettings set; set.mode = PlayMode::Absolute;
    TimecodeGen g(SR); g.setCycles(300000.0);
    auto play = [&](double sec) {
        for (int b = 0; b < (int)(sec * SR) / 512; ++b)
        {
            for (int i = 0; i < 512; ++i) g.next(1.0f, L[(size_t)i], R[(size_t)i]);
            e.process(L.data(), R.data(), L.data(), R.data(), 512, set);
        }
    };
    play(0.2);
    e.jumpTo(SR * 1.0);
    const double at = g.cycles();
    play(0.2);
    double expect = SR * 1.0 + (g.cycles() - at) / 1000.0 * SR;
    CHECK(std::fabs(e.positionFrames() - expect) < SR * 0.006, "Abs: jump re-anchors (%.0f vs %.0f)", e.positionFrames(), expect);
    g.setCycles(g.cycles() - 300.0);   // needle drop 0.3 s back
    play(0.1);
    expect = SR * 1.0 + (g.cycles() - at) / 1000.0 * SR;
    CHECK(std::fabs(e.positionFrames() - expect) < SR * 0.006, "Abs: needle drop after a jump (%.0f vs %.0f)", e.positionFrames(), expect);
}

static std::string slurp(const std::string& path)
{
    std::ifstream f(path); std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); return s;
}

static void testCueSaving()
{
    const std::string u = tempDir("cues");
    touch(u + "/b.wav");
    SampleLibrary lib; lib.setFolders("", u); lib.setLoader(fakeLoad); lib.scanNow();
    lib.loadNow(0);
    LoadedSample* s = lib.acquire();
    CHECK(s && s->cuePath == u + "/b.wav.cues" && !s->cues.any(), "cue path next to the audio, none yet ('%s')", s ? s->cuePath.c_str() : "");
    lib.start();
    CuePoints c; c.sec[1] = 0.05;
    CHECK(lib.requestSaveCues(*s, c), "save queued");
    for (int i = 0; i < 100 && slurp(u + "/b.wav.cues").empty(); ++i) usleep(5000);
    CHECK(slurp(u + "/b.wav.cues") == c.format(), "worker wrote the cue file");
    // Reloading the sample reads them back.
    lib.requestLoad(0);
    const LoadedSample* r = nullptr;
    for (int i = 0; i < 100; ++i) { bool ch = false; r = lib.acquire(&ch); if (ch) break; usleep(5000); }
    CHECK(r && r->cues == c, "reload reads the cues back");
    // Clearing all removes the file.
    CHECK(lib.requestSaveCues(*r, CuePoints()), "clear queued");
    for (int i = 0; i < 100 && !slurp(u + "/b.wav.cues").empty(); ++i) usleep(5000);
    struct stat st;
    CHECK(stat((u + "/b.wav.cues").c_str(), &st) != 0, "no cues left: file deleted");
    // Many saves while the worker runs (ring of 8): the last one wins, nothing breaks.
    int queued = 0;
    for (int k = 0; k < 200; ++k) { CuePoints x; x.sec[0] = 0.001 * k; if (lib.requestSaveCues(*r, x)) ++queued; if (k % 10 == 0) usleep(2000); }
    lib.stop();
    CHECK(queued > 8, "save ring keeps up (%d queued)", queued);
    const CuePoints last = CuePoints::parse(slurp(u + "/b.wav.cues"), 10.0);
    CHECK(last.isSet(0), "file still valid after a save storm");
    // Bundled samples keep their cues in the first user folder.
    const std::string b = tempDir("bund");
    touch(b + "/x.wav");
    SampleLibrary lib2; lib2.setFolders(b, u); lib2.setLoader(fakeLoad); lib2.scanNow();
    lib2.loadNow(0);
    const LoadedSample* bs = lib2.acquire();
    CHECK(bs && bs->cuePath == u + "/x.wav.cues", "bundled sample's cues go to the user folder ('%s')", bs ? bs->cuePath.c_str() : "");
}

int main()
{
    testLfsr();
    testLockTime();
    for (float s : {1.0f, 0.5f, 0.25f, 1.5f, 2.0f, -1.0f, -0.5f, -2.0f}) testSpeed(s, 0.01);
    testNoFalseLock();
    testStopAndReverse();
    testBabyScratch();
    testNoiseAndLevel();
    test45();
    testSwap();
    testPlayer();
    testEngine();
    testAbsolute();
    testLibraryScan();
    testLibraryLoadAndRescan();
    testLibraryHandoffStress();
    testBars();
    testAudioFiles();
    testCuePointsFormat();
    testJumpTo();
    testCueSaving();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
