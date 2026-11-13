// Scratchivision - MPC insert effect: scratch a sample with Serato-style control vinyl.
//
// SPDX-License-Identifier: MIT
//
// All DSP lives in core/include/slowops/scratch/ (no SDK dependency). This file only:
//   * declares the MPC parameters (ids and order are permanent; append new ones at the end),
//   * lists the samples and has them loaded off the audio thread (SampleLibrary):
//       - bundled: Content/Samples/ inside the plugin folder,
//       - the user's: a Scratchivision/ folder at the root of any drive (internal, SD, USB; the
//         plugin's own drive first, created there on first load). Tests point it elsewhere with
//         SCRATCHIVISION_USER_DIR. WAV, AIFF and MP3 are read.
//   * turns the self-resetting CUE / PREV / NEXT / RESCAN / Cue 1-5 parameters into actions
//     (hot cues are kept per audio file in "<file>.cues", see CuePoints.hpp),
//   * writes the read-only display parameters that drive the DECK and SCOPE pages,
//   * saves the selected sample's file name with the project, so it reopens on the same WAV.
//
// Signal flow (see ScratchEngine.hpp):
//   turntable -> track input -> TimecodeDecoder -> speed / position -> ScratchPlayer -> Gain -> out
//                                   (Thru: the timecode itself goes to the output instead)
//   Test Pattern: an internal synthetic timecode (a slow back-and-forth) replaces the input,
//   so the pages and the sound can be checked without a turntable.
//
// The MPC cannot draw from plugin code. The pages are JSON skins (scripts/gen_skins.py):
//   * waveform: 128 film-strip bars ("Bar n"), the sample's envelope around the play position,
//   * scope: 32 XYPad dots placed by "Dot n X/Y" (the last few carrier cycles, L vs R),
//   * plotters and text readouts bound to the other read-only parameters.

#include "mpc_plugin.h"
#include "dsp/wav.h"

#include "../../core/include/slowops/scratch/AudioFile.hpp"
#include "../../core/include/slowops/scratch/CuePoints.hpp"
#include "../../core/include/slowops/scratch/SampleLibrary.hpp"
#include "../../core/include/slowops/scratch/ScratchEngine.hpp"
#include "../../core/include/slowops/scratch/TimecodeGen.hpp"
#include "../../core/include/slowops/scratch/WaveBars.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace sc = slowops::scratch;

class Scratchivision : public mpc::Plugin
{
public:
    enum Index
    {
        // Controls
        kGain = 0,      // output level, dB (parameter 0: the host nudges it on load; harmless)
        kSample,        // 1..64: which sample is on the deck (the name is shown as its value)
        kRpm,           // 33 / 45: nominal record speed
        kMode,          // Rel / Abs (see ScratchPlayer.hpp)
        kLoop,          // wrap at the sample ends instead of stopping there
        kThru,          // pass the timecode to the output (for checking the signal)
        kCue,           // trigger: back to the start of the sample; the plugin sets it back to Off
        kSwap,          // swap L and R before decoding (fixes reversed direction from crossed wiring)
        kTest,          // Test Pattern: internal timecode instead of the input
        // Read-only displays
        kPitch,         // signed platter speed, 1.00 = nominal
        kLock,          // No Signal / Relative / Locked
        kLevel,         // input level 0..1 (shown in dB)
        kRecPos,        // needle position on the record, seconds
        kDir,           // Stop / Fwd / Rev
        kWaveX,         // unused since 0.2 (was the XYPad waveform); kept so later indices stay put
        kSpeedPlot,     // speed for the SCOPE plotter: 0.5 = stopped, 0 / 1 = -3x / +3x
        kDot0,          // Dot 1 X, Dot 1 Y, Dot 2 X, ... (32 dots)
        // Appended in 0.2
        kPrev = kDot0 + 2 * 32,   // trigger: previous sample
        kNext,          // trigger: next sample
        kRescan,        // trigger: list the folders again (new WAVs appear, the current one is kept)
        kZoom,          // seconds across the waveform window, or All (the whole sample, playhead moves)
        kInfo,          // read-only: "3/12  2.4 s", "Loading...", "Cut to 20 s", "Can't read", "No WAVs"
        kBar0,          // read-only: Bar 1 .. Bar 128, waveform bar heights
        // Appended in 0.3: hot cues
        kCuePad0 = kBar0 + 128,   // Cue 1..5 triggers: unset = store the position, set = jump there
        kCueClear = kCuePad0 + 5, // toggle: the next cue tapped is deleted (then it turns itself off)
        kCueState0,               // read-only Cue n State: Unset / Set (picks the pad image)
        kCueX0 = kCueState0 + 5,  // read-only Cue n X: marker position across the waveform window
        kCueShown0 = kCueX0 + 5,  // read-only Cue n Shown: the cue is set and inside the window
        // Appended in 0.4: zoom buttons and the moving playhead for Zoom = All
        kZoomOut = kCueShown0 + 5,   // trigger: "-" (see more)
        kZoomIn,                     // trigger: "+" (see less, in more detail)
        kPlayX,                      // read-only: playhead position across the window (Zoom = All)
        kEnd
    };
    static constexpr int kNumCues = sc::CuePoints::kCount;
    static constexpr int kNumDots = 32;
    static constexpr int kNumBars = 128;
    static constexpr int kBarSteps = 24;   // bar heights; the film strip has 2 x 24 frames:
                                           // 0..23 = ahead of the playhead, 24..47 = already played
    static constexpr int kNumZooms = 7;    // 0.25, 0.5, 1, 2, 4, 8 s, All
    static constexpr int kZoomAll = kNumZooms - 1;
    static constexpr int kNumParams = kEnd;

    Scratchivision() : mpc::Plugin(makeInfo())
    {
        addParam(mpc::floatParam("gain", "Gain", -24.0f, 12.0f, 0.0f, "dB", 1));
        addParam(mpc::intParam("sample", "Sample", 1, sc::SampleLibrary::kMaxEntries, 1));
        addParam(mpc::choiceParam("rpm", "Speed", {"33", "45"}, 0));
        addParam(mpc::choiceParam("mode", "Mode", {"Rel", "Abs"}, 0));
        addParam(mpc::boolParam("loop", "Loop", false));
        addParam(mpc::boolParam("thru", "Thru", false));
        addParam(mpc::boolParam("cue", "Cue", false));
        addParam(mpc::boolParam("swap", "Swap", false));
        addParam(mpc::boolParam("test", "Test", false));

        addDisplay(mpc::floatParam("pitch", "Pitch", -3.0f, 3.0f, 0.0f, "x", 2));
        addDisplay(mpc::choiceParam("lock", "Lock", {"No Signal", "Relative", "Locked"}, 0));
        addDisplay(mpc::floatParam("level", "Level", 0.0f, 1.0f, 0.0f, "", 3));
        addDisplay(mpc::floatParam("recpos", "Position", 0.0f, 1000.0f, 0.0f, "s", 1));
        addDisplay(mpc::choiceParam("dir", "Direction", {"Stop", "Fwd", "Rev"}, 0));
        addDisplay(mpc::floatParam("wavex", "Wave X", 0.0f, 1.0f, 0.5f, "", 3));
        addDisplay(mpc::floatParam("speedplot", "Speed Plot", 0.0f, 1.0f, 0.5f, "", 3));
        for (int d = 0; d < kNumDots; ++d)
        {
            const std::string n = std::to_string(d + 1);
            addDisplay(mpc::floatParam(("dot" + n + "x").c_str(), ("Dot " + n + " X").c_str(), 0.0f, 1.0f, 0.5f, "", 3));
            addDisplay(mpc::floatParam(("dot" + n + "y").c_str(), ("Dot " + n + " Y").c_str(), 0.0f, 1.0f, 0.5f, "", 3));
        }
        addParam(mpc::boolParam("prev", "Prev", false));
        addParam(mpc::boolParam("next", "Next", false));
        addParam(mpc::boolParam("rescan", "Rescan", false));
        addParam(mpc::choiceParam("zoom", "Zoom", {"0.25 s", "0.5 s", "1 s", "2 s", "4 s", "8 s", "All"}, 2));
        addDisplay(mpc::floatParam("info", "Sample Info", 0.0f, 1.0f, 0.0f, "", 3));
        for (int b = 0; b < kNumBars; ++b)
        {
            const std::string n = std::to_string(b + 1);
            addDisplay(mpc::floatParam(("bar" + n).c_str(), ("Bar " + n).c_str(), 0.0f, 1.0f, 0.0f, "", 3));
        }
        for (int c = 0; c < kNumCues; ++c)
        {
            const std::string n = std::to_string(c + 1);
            addParam(mpc::boolParam(("cue" + n).c_str(), ("Cue " + n).c_str(), false));
        }
        addParam(mpc::boolParam("cueclear", "Clear", false));
        for (int c = 0; c < kNumCues; ++c)
        {
            const std::string n = std::to_string(c + 1);
            addDisplay(mpc::choiceParam(("cue" + n + "state").c_str(), ("Cue " + n + " State").c_str(), {"Unset", "Set"}, 0));
        }
        for (int c = 0; c < kNumCues; ++c)
        {
            const std::string n = std::to_string(c + 1);
            addDisplay(mpc::floatParam(("cue" + n + "x").c_str(), ("Cue " + n + " X").c_str(), 0.0f, 1.0f, 0.5f, "", 3));
        }
        for (int c = 0; c < kNumCues; ++c)
        {
            const std::string n = std::to_string(c + 1);
            addDisplay(mpc::boolParam(("cue" + n + "shown").c_str(), ("Cue " + n + " Shown").c_str(), false));
        }
        addParam(mpc::boolParam("zoomout", "Zoom Out", false));
        addParam(mpc::boolParam("zoomin", "Zoom In", false));
        addDisplay(mpc::floatParam("playx", "Play X", 0.0f, 1.0f, 0.5f, "", 3));
        if (numParams() != kNumParams) mpc::log("Scratchivision: parameter count mismatch");

        ownFolder_ = ownDriveFolder();
        lib_.setFolders(mpc::contentPath("Samples"), [this] { return userFolders(); });
        lib_.setLoader([](const std::string& path, std::vector<std::vector<float>>& ch, double& sr) {
            if (sc::audioTypeFromName(path) != sc::AudioType::Wav)
                return sc::loadAiffOrMp3(path, sc::SampleLibrary::kMaxSeconds, ch, sr);
            dsp::WavData w;
            if (!dsp::loadWav(path, w) || w.numFrames() < 2) return false;
            sr = w.sampleRate;
            ch.swap(w.channels);
            return true;
        });
        lib_.scanNow();
        if (lib_.count() > 0) lib_.loadNow(0);
        requested_ = 0;
        lib_.start();
        testGen_.reset(new sc::test::TimecodeGen(44100.0));
    }

    ~Scratchivision() override { lib_.stop(); }

    void prepare(double sampleRate, int maxBlockSize) override
    {
        (void)maxBlockSize;
        engine_.prepare(sampleRate);               // allocates the 4 MB position table
        testGen_.reset(new sc::test::TimecodeGen(sampleRate));
        displayEvery_ = (int)(sampleRate / 30.0);  // displays at ~30 Hz
        attached_ = nullptr;                       // re-attach the current sample on the next block
    }

    void reset() override
    {
        engine_.reset();
        for (int t : {kCue, kPrev, kNext, kRescan, kZoomOut, kZoomIn}) if (getBool(t)) setNormalised(t, 0.0f);
        for (int c = 0; c < kNumCues; ++c) if (getBool(kCuePad0 + c)) setNormalised(kCuePad0 + c, 0.0f);
        attached_ = nullptr;
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int n = ctx.numFrames;
        const float* inL = ctx.numInputs > 0 ? ctx.inputs[0] : nullptr;
        const float* inR = ctx.numInputs > 1 ? ctx.inputs[1] : inL;
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : ctx.outputs[0];

        handleSampleSelection();

        sc::EngineSettings set;
        set.rpm45 = getInt(kRpm) == 1;
        set.mode = getInt(kMode) == 1 ? sc::PlayMode::Absolute : sc::PlayMode::Relative;
        set.loop = getBool(kLoop);
        set.thru = getBool(kThru);
        set.swap = getBool(kSwap);
        set.gain = std::pow(10.0f, get(kGain) / 20.0f);

        if (trigger(kCue)) engine_.cue();
        handleCues();
        {
            const int z = getInt(kZoom);
            if (trigger(kZoomOut) && z < kNumZooms - 1) { mpc::Plugin::set(kZoom, (float)(z + 1)); notifyHostParameterChanged(kZoom); }
            if (trigger(kZoomIn) && z > 0) { mpc::Plugin::set(kZoom, (float)(z - 1)); notifyHostParameterChanged(kZoom); }
        }

        const bool test = getBool(kTest);
        if (test != testWas_)
        {
            testWas_ = test;
            testT_ = 0;
            testGen_->setCycles(20000.0);
            testGen_->setRpm45(set.rpm45);
            engine_.cue();
        }

        if (test || inL == nullptr)
        {
            // Synthesise the timecode into the output buffers (in place is fine: the engine copies
            // each chunk before writing it).
            const double sr = ctx.sampleRate > 0 ? ctx.sampleRate : 44100.0;
            for (int i = 0; i < n; ++i)
            {
                float sp = 0.0f;
                if (test)
                {
                    // 2 s cycle: forward then back at up to 1.5x, so the waveform sweeps the sample.
                    const double t = (double)(testT_++) / sr;
                    sp = (float)(1.5 * std::sin(2.0 * 3.14159265358979 * 0.5 * t));
                }
                float l = 0.0f, r = 0.0f;
                if (test) testGen_->next(sp, l, r);
                outL[i] = l; outR[i] = r;
            }
            engine_.process(outL, outR, outL, outR, n, set);
        }
        else
        {
            engine_.process(inL, inR, outL, outR, n, set);
        }

        displayAge_ += n;
        if (displayAge_ >= displayEvery_)
        {
            displayAge_ = 0;
            updateDisplays();
        }
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        char buf[64];
        switch (index)
        {
            case kSample:
            {
                const std::string nm = lib_.name((int)std::lround(plain) - 1);
                out = nm.empty() ? std::string("(none)") : nm;
                return true;
            }
            case kInfo:
                out = infoText();
                return true;
            case kPitch:
                std::snprintf(buf, sizeof(buf), "%+.2fx", (double)plain);
                break;
            case kLevel:
                if (plain < 0.001f) std::snprintf(buf, sizeof(buf), "-inf dB");
                else std::snprintf(buf, sizeof(buf), "%.0f dB", 20.0 * std::log10((double)plain));
                break;
            case kRecPos:
            {
                const int m = (int)(plain / 60.0f);
                std::snprintf(buf, sizeof(buf), "%d:%04.1f", m, (double)(plain - 60.0f * (float)m));
                break;
            }
            default:
                return false;
        }
        out = buf;
        return true;
    }

    // The selected sample is saved by file name, so a project reopens on the same WAV even when
    // files were added to or removed from the folder since.
    void saveExtraState(std::vector<uint8_t>& out) override
    {
        const std::string f = lib_.file(getInt(kSample) - 1);
        const char magic[4] = {'S', 'C', 'V', '1'};
        out.insert(out.end(), magic, magic + 4);
        out.insert(out.end(), f.begin(), f.end());
    }

    void loadExtraState(const uint8_t* data, size_t size) override
    {
        if (size < 4 || std::memcmp(data, "SCV1", 4) != 0) return;
        const std::string f((const char*)data + 4, size - 4);
        const int idx = lib_.find(f);
        if (idx >= 0) set(kSample, (float)(idx + 1));
    }

private:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name = "Scratchivision";
        info.vendor = "Slow Ops";
        info.uniqueId = MPC_FOURCC('S', 'o', 'S', 'v');
        info.version = 4000;   // 0.4.0
        info.isInstrument = false;
        info.numInputs = 2;
        info.numOutputs = 2;
        info.tailSamples = 1;
        return info;
    }

    // <drive>/Scratchivision, when the plugin sits in <drive>/VST2 Plugins/<folder>. Created (with a
    // short README) if missing, so it shows up in the MPC's browser. Elsewhere (a test build in the
    // project) there is none. SCRATCHIVISION_USER_DIR overrides it (tests).
    static std::string ownDriveFolder()
    {
        if (const char* env = std::getenv("SCRATCHIVISION_USER_DIR")) return env;
        const std::string pf = mpc::pluginFolder();
        const size_t a = pf.rfind('/');
        if (a == std::string::npos || a == 0) return "";
        const std::string parent = pf.substr(0, a);
        const size_t b = parent.rfind('/');
        if (b == std::string::npos || parent.substr(b + 1) != "VST2 Plugins") return "";
        const std::string dir = parent.substr(0, b) + "/Scratchivision";
        struct stat st;
        if (stat(dir.c_str(), &st) != 0 && mkdir(dir.c_str(), 0777) == 0)
        {
            if (FILE* f = std::fopen((dir + "/README.txt").c_str(), "w"))
            {
                std::fputs("Put WAV, AIFF or MP3 files here (up to 64, 20 s each) to scratch them with\n"
                           "Scratchivision. A Scratchivision folder at the root of any drive works.\n"
                           "Then tap RESCAN on the plugin's DECK page, or reload the plugin.\n", f);
                std::fclose(f);
            }
        }
        return dir;
    }

    // Every Scratchivision/ folder to list: the plugin's own drive first, then the root of each
    // other mounted drive (the MPC mounts its internal drive at /nvme and SD cards / USB drives
    // under /media/, as listed in /proc/mounts). Called again on every RESCAN.
    std::vector<std::string> userFolders() const
    {
        std::vector<std::string> out;
        if (!ownFolder_.empty()) out.push_back(ownFolder_);
        if (std::getenv("SCRATCHIVISION_USER_DIR")) return out;   // tests: that folder only
        FILE* f = std::fopen("/proc/mounts", "r");
        if (!f) return out;
        char line[1024];
        std::vector<std::string> others;
        while (std::fgets(line, sizeof(line), f))
        {
            char dev[256], mnt[512];
            if (std::sscanf(line, "%255s %511s", dev, mnt) != 2) continue;
            std::string m;   // undo /proc/mounts escapes (\040 = space)
            for (const char* c = mnt; *c; ++c)
            {
                if (c[0] == '\\' && c[1] >= '0' && c[1] <= '7' && c[2] && c[3]) { m += (char)((c[1] - '0') * 64 + (c[2] - '0') * 8 + (c[3] - '0')); c += 3; }
                else m += *c;
            }
            if (m != "/nvme" && m.rfind("/media/", 0) != 0) continue;
            const std::string dir = m + "/Scratchivision";
            struct stat st;
            if (dir == ownFolder_ || stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
            bool dup = false;
            for (const std::string& o : others) dup = dup || o == dir;
            if (!dup) others.push_back(dir);
        }
        std::fclose(f);
        std::sort(others.begin(), others.end());
        out.insert(out.end(), others.begin(), others.end());
        return out;
    }

    void addDisplay(mpc::Param p)
    {
        p.automatable = false;
        addParam(p);
    }

    // A self-resetting button: true once per press.
    bool trigger(int i)
    {
        if (!getBool(i)) return false;
        setNormalised(i, 0.0f);
        notifyHostParameterChanged(i);
        return true;
    }

    void setSample(int index)
    {
        set(kSample, (float)(index + 1));
        notifyHostParameterChanged(kSample);
    }

    // Audio thread: never locks; the library's worker does the file I/O.
    void handleSampleSelection()
    {
        const int count = lib_.count();
        const int remap = lib_.takeRemap();   // after a rescan: where the loaded file is now
        if (remap >= 0) { requested_ = remap; setSample(remap); }

        int idx = getInt(kSample) - 1;
        if (count > 0 && idx >= count) { idx = count - 1; setSample(idx); }
        if (trigger(kPrev) && count > 0) { idx = idx > 0 ? idx - 1 : 0; setSample(idx); }
        if (trigger(kNext) && count > 0) { idx = idx < count - 1 ? idx + 1 : count - 1; setSample(idx); }
        if (trigger(kRescan)) lib_.requestScan();
        if (idx != requested_ && idx >= 0 && idx < count) { requested_ = idx; lib_.requestLoad(idx); }

        bool changed = false;
        const sc::LoadedSample* s = lib_.acquire(&changed);
        if (s != attached_)
        {
            sc::SampleView v;
            if (s)
            {
                v.left = s->left.data(); v.right = s->right.data();
                v.frames = (int64_t)s->left.size(); v.sampleRate = s->sampleRate;
            }
            engine_.setSample(v);
            if (changed) engine_.cue();   // a newly chosen sample starts at its beginning
            attached_ = s;
            cues_ = s ? s->cues : sc::CuePoints();
            cuesDirty_ = false;
            infoSeconds_.store(s ? (float)s->seconds() : 0.0f);
            infoFlags_.store(s ? ((s->failed ? 1 : 0) | (s->truncated ? 2 : 0)) : 0);
        }
    }

    // Hot cues (audio thread). Tap an empty cue: store the play position. Tap a set cue: jump there
    // (the record keeps steering). With Clear on: delete the cue tapped, and Clear turns off.
    // Changes go to the library's save ring; if it is full they are retried next block.
    void handleCues()
    {
        for (int c = 0; c < kNumCues; ++c)
        {
            if (!trigger(kCuePad0 + c) || !attached_ || attached_->failed) continue;
            const double sr = attached_->sampleRate;
            if (getBool(kCueClear))
            {
                if (cues_.isSet(c)) { cues_.sec[c] = -1.0; cuesDirty_ = true; }
                setNormalised(kCueClear, 0.0f);
                notifyHostParameterChanged(kCueClear);
            }
            else if (!cues_.isSet(c))
            {
                double t = engine_.positionFrames() / sr;
                const double len = attached_->seconds();
                cues_.sec[c] = t < 0.0 ? 0.0 : (t > len ? len : t);
                cuesDirty_ = true;
            }
            else engine_.jumpTo(cues_.sec[c] * sr);
        }
        if (cuesDirty_ && attached_ && lib_.requestSaveCues(*attached_, cues_)) cuesDirty_ = false;
        if (cuesDirty_ && attached_ && attached_->cuePath.empty()) cuesDirty_ = false;   // nowhere to save: keep in memory only
    }

    std::string infoText() const
    {
        const int count = lib_.count();
        if (count == 0) return "No WAVs";
        const int idx = (int)std::lround(get(kSample)) - 1;
        if (lib_.loading() || lib_.loadedIndex() != idx) return "Loading...";
        const int flags = infoFlags_.load();
        if (flags & 1) return "Can't read";
        char buf[48];
        if (flags & 2) std::snprintf(buf, sizeof(buf), "%d/%d  cut to 20 s", idx + 1, count);
        else std::snprintf(buf, sizeof(buf), "%d/%d  %.1f s", idx + 1, count, (double)infoSeconds_.load());
        return buf;
    }

    void setDisplay(int i, float norm)
    {
        if (getNormalised(i) != norm) { setNormalised(i, norm); notifyHostParameterChanged(i); }
    }

    void updateDisplays()
    {
        const sc::TimecodeState tc = engine_.timecode();
        auto norm = [&](int i, float plain) { return param(i).toNormalised(plain); };
        const float pitch = std::round(tc.speed * 100.0f) / 100.0f;
        setDisplay(kPitch, norm(kPitch, pitch));
        setDisplay(kLock, norm(kLock, !tc.signal ? 0.0f : (tc.locked ? 2.0f : 1.0f)));
        setDisplay(kLevel, norm(kLevel, std::round(tc.level * 200.0f) / 200.0f));
        setDisplay(kRecPos, norm(kRecPos, (float)(std::floor(tc.positionSec * 10.0) / 10.0)));
        setDisplay(kDir, norm(kDir, pitch > 0.02f ? 1.0f : (pitch < -0.02f ? 2.0f : 0.0f)));
        float sp = 0.5f + tc.speed / 6.0f;
        setDisplay(kSpeedPlot, sp < 0.0f ? 0.0f : (sp > 1.0f ? 1.0f : sp));

        // Sample Info: its text comes from infoText(); bump the value when the text would change
        // so the MPC redraws it.
        const int count = lib_.count();
        const int idx = getInt(kSample) - 1;
        const uint32_t key = (uint32_t)count * 131u + (uint32_t)(idx + 1) * 7u + (uint32_t)infoFlags_.load() * 3u
                           + (uint32_t)(lib_.loading() || lib_.loadedIndex() != idx) * 100003u
                           + (uint32_t)std::lround(infoSeconds_.load() * 10.0f) * 1009u;
        if (key != infoKey_) { infoKey_ = key; infoCounter_ = (infoCounter_ + 1) % 100; setDisplay(kInfo, (float)infoCounter_ / 99.0f); }

        // Waveform window. Zoom 0.25..8 s: centred on the play position (the playhead is the fixed
        // image in the middle). Zoom All: the whole sample, still, with the moving playhead.
        static const double zoomSec[kNumZooms] = {0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 0.0};
        const int z = getInt(kZoom);
        const double posSec = attached_ ? engine_.positionFrames() / attached_->sampleRate : 0.0;
        const double len = attached_ ? attached_->seconds() : 1.0;
        const bool all = z >= kZoomAll;
        const double view = all ? (len > 0.01 ? len : 0.01) : zoomSec[z < 0 ? 0 : z];
        const double centre = all ? view * 0.5 : posSec;
        float bars[kNumBars] = {};
        if (attached_ && !attached_->failed) sc::computeBars(attached_->env, centre, view, kNumBars, bars);
        const double barW = view / kNumBars;
        for (int b = 0; b < kNumBars; ++b)
        {
            // Frame = height step, + 24 for bars already played (their centre is behind the playhead).
            const double t = centre + (b - kNumBars / 2 + 0.5) * barW;
            const int frame = (int)std::lround(bars[b] * (kBarSteps - 1)) + (t < posSec ? kBarSteps : 0);
            setDisplay(kBar0 + b, (float)frame / (float)(2 * kBarSteps - 1));
        }
        const double px = 0.5 + (posSec - centre) / view;
        setDisplay(kPlayX, (float)(std::round((px < 0 ? 0 : (px > 1 ? 1 : px)) * 1000.0) / 1000.0));

        // Cue pads and markers (x = 0.5 is the window centre); outside 0..1 the marker is hidden.
        for (int c = 0; c < kNumCues; ++c)
        {
            const bool isSet = attached_ && cues_.isSet(c);
            setDisplay(kCueState0 + c, isSet ? 1.0f : 0.0f);
            const double x = isSet ? 0.5 + (cues_.sec[c] - centre) / view : -1.0;
            const bool shown = x >= 0.0 && x <= 1.0;
            if (shown) setDisplay(kCueX0 + c, (float)(std::round(x * 1000.0) / 1000.0));
            setDisplay(kCueShown0 + c, shown ? 1.0f : 0.0f);
        }

        // Scope: the last 128 input frames, every 4th, scaled to the signal level so the ring
        // fills the view (the level itself is on the readout).
        const float scale = tc.level > 0.02f ? tc.level : 0.02f;
        const int stride = sc::ScratchEngine::kScopeRing / kNumDots;
        for (int d = 0; d < kNumDots; ++d)
        {
            const int i = d * stride;
            float x = 0.5f + 0.46f * engine_.scopeL(i) / scale;
            float y = 0.5f + 0.46f * engine_.scopeR(i) / scale;
            x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
            y = y < 0.0f ? 0.0f : (y > 1.0f ? 1.0f : y);
            setDisplay(kDot0 + 2 * d, std::round(x * 500.0f) / 500.0f);
            setDisplay(kDot0 + 2 * d + 1, std::round(y * 500.0f) / 500.0f);
        }
    }

    std::string ownFolder_;                       // Scratchivision/ on the plugin's own drive ("" if none)
    sc::SampleLibrary lib_;                       // destroyed after the engine
    sc::ScratchEngine engine_;
    std::unique_ptr<sc::test::TimecodeGen> testGen_;
    const sc::LoadedSample* attached_ = nullptr;  // audio thread: the sample the engine reads
    sc::CuePoints cues_;                          // audio thread: the attached sample's cues (authoritative)
    bool cuesDirty_ = false;                      // a change still waiting for room in the save ring
    int requested_ = -1;                          // audio thread: index last asked for
    std::atomic<float> infoSeconds_{0.0f};
    std::atomic<int> infoFlags_{0};
    uint32_t infoKey_ = 0;
    int infoCounter_ = 0;
    bool testWas_ = false;
    int64_t testT_ = 0;
    int displayEvery_ = 1470, displayAge_ = 0;
};

MPC_PLUGIN_ENTRY(Scratchivision)
