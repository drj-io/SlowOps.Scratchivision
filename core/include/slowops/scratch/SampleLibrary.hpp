// SampleLibrary.hpp - the list of WAVs on the deck, and loading them off the audio thread.
//
// SPDX-License-Identifier: MIT
//
// Listed: the bundled samples (inside the plugin) first, then each user folder in the order the
// folder provider gives them (Scratchivision/ at the root of every drive; the plugin's own drive
// first). Top level only; WAV, AIFF/AIF/AIFC and MP3 in any case; sorted by name within a folder;
// at most kMaxEntries. Files starting with "." (including the "._" files macOS leaves on exFAT)
// are skipped. The provider is asked again on every rescan, so a drive plugged in later counts.
//
// Threads
//   * UI thread (constructor, formatValue, state save/load): the list, under listMutex_.
//   * Worker thread (owned here): rescans and loads. Polls every 10 ms, so the audio thread never
//     has to wake it (no locks, no syscalls from process()).
//   * Audio thread: requestLoad(), acquire(), and the atomics below. Never locks.
//
// Hand-off: the worker builds a LoadedSample and publishes it in pending_. The audio thread
// takes it in acquire() only when retired_ is empty, moving the old sample to retired_; the
// worker deletes retired_. So a sample is never freed while the player may still read it.
//
// Cue points: load() reads "<file>.cues" (see CuePoints.hpp). The audio thread saves changes with
// requestSaveCues(), which copies the path and values into a fixed lock-free ring (no allocation);
// the worker writes the file (via a temporary file and rename) before it deletes anything.
//
// Loading itself (WAV parsing) is injected, so this header has no SDK dependency.

#pragma once

#include "AudioFile.hpp"
#include "CuePoints.hpp"
#include "WaveBars.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <dirent.h>
#include <functional>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

namespace slowops { namespace scratch {

struct LoadedSample
{
    int index = -1;              // position in the list when loaded
    std::string name;            // file name without .wav
    std::string file;            // file name
    std::string path;            // full path
    std::vector<float> left, right;
    double sampleRate = 44100.0;
    bool truncated = false;      // longer than kMaxSeconds, cut
    bool failed = false;         // could not be read (plays silence)
    Envelope env;
    CuePoints cues;              // as loaded from cuePath
    std::string cuePath;         // "" when there is nowhere to save (no Scratchivision folder)
    double seconds() const { return sampleRate > 0 ? left.size() / sampleRate : 0.0; }
};

struct SampleEntry
{
    std::string name;   // shown on the DECK page
    std::string file;
    std::string path;
    bool user = false;
};

class SampleLibrary
{
public:
    static constexpr int kMaxEntries = 64;
    static constexpr double kMaxSeconds = 20.0;

    // path -> channels (1 or 2 used) and sample rate. Return false if unreadable.
    using Loader = std::function<bool(const std::string& path, std::vector<std::vector<float>>& channels, double& sampleRate)>;

    ~SampleLibrary() { stop(); delete pending_.exchange(nullptr); delete retired_.exchange(nullptr); delete current_; }

    using FolderProvider = std::function<std::vector<std::string>()>;
    void setFolders(const std::string& bundled, FolderProvider users) { bundled_ = bundled; users_ = std::move(users); }
    void setFolders(const std::string& bundled, const std::string& user)
    {
        bundled_ = bundled;
        users_ = [user] { return user.empty() ? std::vector<std::string>() : std::vector<std::string>{user}; };
    }
    void setLoader(Loader l) { loader_ = std::move(l); }

    // ---- UI / constructor side -----------------------------------------------------------
    void scanNow()
    {
        std::vector<SampleEntry> list;
        listFolder(bundled_, false, list);
        std::vector<std::string> users;
        if (users_) users = users_();
        for (const std::string& u : users) listFolder(u, true, list);
        if ((int)list.size() > kMaxEntries) list.resize(kMaxEntries);
        std::lock_guard<std::mutex> g(listMutex_);
        list_.swap(list);
        bundledCueDir_ = users.empty() ? std::string() : users[0];
        count_.store((int)list_.size());
    }

    // Load synchronously and publish (constructor: so the first block already has a sample).
    void loadNow(int index) { publish(load(index)); }

    void start()
    {
        if (running_.exchange(true)) return;
        worker_ = std::thread([this] { run(); });
    }
    void stop()
    {
        if (!running_.exchange(false)) return;
        if (worker_.joinable()) worker_.join();
        drainSaves();   // cues changed just before the plugin closed
    }

    int count() const { return count_.load(); }
    std::string name(int index) const
    {
        std::lock_guard<std::mutex> g(listMutex_);
        return (index >= 0 && index < (int)list_.size()) ? list_[(size_t)index].name : std::string();
    }
    std::string file(int index) const
    {
        std::lock_guard<std::mutex> g(listMutex_);
        return (index >= 0 && index < (int)list_.size()) ? list_[(size_t)index].file : std::string();
    }
    int find(const std::string& file) const
    {
        std::lock_guard<std::mutex> g(listMutex_);
        for (size_t i = 0; i < list_.size(); ++i) if (list_[i].file == file) return (int)i;
        return -1;
    }

    // ---- audio thread ---------------------------------------------------------------------
    void requestLoad(int index) { loadReq_.store(index); }
    void requestScan() { scanReq_.store(true); }

    // Save a sample's cues (audio thread: no allocation, no lock). False if the ring is full or the
    // sample has nowhere to save; try again later.
    bool requestSaveCues(const LoadedSample& s, const CuePoints& c)
    {
        if (s.cuePath.empty() || s.cuePath.size() >= sizeof(SaveSlot::path)) return false;
        const uint32_t h = saveHead_.load(std::memory_order_relaxed);
        if (h - saveTail_.load(std::memory_order_acquire) >= kSaveSlots) return false;
        SaveSlot& slot = saves_[h % kSaveSlots];
        std::memcpy(slot.path, s.cuePath.c_str(), s.cuePath.size() + 1);
        slot.cues = c;
        saveHead_.store(h + 1, std::memory_order_release);
        return true;
    }

    // Take a newly loaded sample if there is one. Returns the current sample (may be null).
    LoadedSample* acquire(bool* changed = nullptr)
    {
        if (changed) *changed = false;
        if (retired_.load() == nullptr)
        {
            LoadedSample* p = pending_.exchange(nullptr);
            if (p)
            {
                retired_.store(current_);
                current_ = p;
                if (changed) *changed = true;
            }
        }
        return current_;
    }
    const LoadedSample* current() const { return current_; }

    // After a rescan: the new list index of the loaded file (or of its replacement), once.
    // Returns -1 when there is nothing to report.
    int takeRemap() { return remap_.exchange(-1); }

    // Status for the display (any thread).
    bool loading() const { return loadReq_.load() >= 0 || pending_.load() != nullptr || busy_.load(); }
    int loadedIndex() const { return loadedIndex_.load(); }

private:

    static void listFolder(const std::string& dir, bool user, std::vector<SampleEntry>& out)
    {
        if (dir.empty()) return;
        DIR* d = opendir(dir.c_str());
        if (!d) return;
        std::vector<SampleEntry> found;
        while (dirent* e = readdir(d))
        {
            const std::string n = e->d_name;
            if (n.empty() || n[0] == '.' || audioTypeFromName(n) == AudioType::Unknown) continue;
            const std::string path = dir + "/" + n;
            struct stat st;
            if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
            found.push_back({n.substr(0, n.rfind('.')), n, path, user});
        }
        closedir(d);
        std::sort(found.begin(), found.end(), [](const SampleEntry& a, const SampleEntry& b) {
            std::string x = a.file, y = b.file;
            for (char& c : x) c = (char)std::tolower((unsigned char)c);
            for (char& c : y) c = (char)std::tolower((unsigned char)c);
            return x < y;
        });
        out.insert(out.end(), found.begin(), found.end());
    }

    LoadedSample* load(int index)
    {
        SampleEntry entry;
        std::string cueDir;
        {
            std::lock_guard<std::mutex> g(listMutex_);
            if (index < 0 || index >= (int)list_.size()) return nullptr;
            entry = list_[(size_t)index];
            cueDir = entry.user ? entry.path.substr(0, entry.path.rfind('/')) : bundledCueDir_;
        }
        LoadedSample* s = new LoadedSample();
        s->index = index; s->name = entry.name; s->file = entry.file; s->path = entry.path;
        std::vector<std::vector<float>> ch;
        double sr = 44100.0;
        if (!loader_ || !loader_(entry.path, ch, sr) || ch.empty() || ch[0].size() < 2)
        {
            s->failed = true;
            s->left.assign(2, 0.0f); s->right = s->left;
        }
        else
        {
            s->sampleRate = sr > 0 ? sr : 44100.0;
            const size_t maxFrames = (size_t)(kMaxSeconds * s->sampleRate);
            s->left.swap(ch[0]);
            if (ch.size() > 1 && ch[1].size() == s->left.size()) s->right.swap(ch[1]); else s->right = s->left;
            if (s->left.size() > maxFrames) { s->left.resize(maxFrames); s->right.resize(maxFrames); s->truncated = true; }
        }
        computeEnvelope(s->left.data(), s->right.data(), s->left.size(), s->sampleRate, s->env);
        if (!cueDir.empty())
        {
            s->cuePath = cueDir + "/" + entry.file + ".cues";
            std::vector<uint8_t> text;
            if (readWholeFile(s->cuePath, text))
                s->cues = CuePoints::parse(std::string(text.begin(), text.end()), s->seconds());
        }
        return s;
    }

    void publish(LoadedSample* s)
    {
        if (!s) return;
        loadedIndex_.store(s->index);
        delete pending_.exchange(s);   // an unconsumed older load is simply replaced
    }

    static void writeCues(const char* path, const CuePoints& c)
    {
        if (!c.any()) { std::remove(path); return; }
        const std::string tmp = std::string(path) + ".tmp";
        FILE* f = std::fopen(tmp.c_str(), "w");
        if (!f) return;
        const std::string text = c.format();
        const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
        if (std::fclose(f) == 0 && ok) std::rename(tmp.c_str(), path);
        else std::remove(tmp.c_str());
    }

    void drainSaves()
    {
        uint32_t t = saveTail_.load(std::memory_order_relaxed);
        while (t != saveHead_.load(std::memory_order_acquire))
        {
            const SaveSlot& slot = saves_[t % kSaveSlots];
            writeCues(slot.path, slot.cues);
            saveTail_.store(++t, std::memory_order_release);
        }
    }

    void run()
    {
        while (running_.load())
        {
            drainSaves();   // before anything is loaded again, so a reload sees the saved cues
            delete retired_.exchange(nullptr);
            if (scanReq_.exchange(false))
            {
                busy_.store(true);
                const std::string loadedFile = file(loadedIndex_.load());
                scanNow();
                int idx = loadedFile.empty() ? -1 : find(loadedFile);
                if (idx >= 0) loadedIndex_.store(idx);
                else
                {
                    // The loaded file is gone: fall back to the nearest index and load it.
                    const int n = count_.load();
                    idx = n == 0 ? -1 : std::min(std::max(0, loadedIndex_.load()), n - 1);
                    if (idx >= 0) loadReq_.store(idx);
                }
                if (idx >= 0) remap_.store(idx);
                busy_.store(false);
            }
            const int req = loadReq_.exchange(-1);
            if (req >= 0)
            {
                busy_.store(true);
                publish(load(req));
                busy_.store(false);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    std::string bundled_;
    std::string bundledCueDir_;   // where bundled samples' cue files go (the first user folder)
    FolderProvider users_;
    struct SaveSlot { char path[1024]; CuePoints cues; };
    static constexpr uint32_t kSaveSlots = 8;
    SaveSlot saves_[kSaveSlots];
    std::atomic<uint32_t> saveHead_{0}, saveTail_{0};
    Loader loader_;
    mutable std::mutex listMutex_;
    std::vector<SampleEntry> list_;
    std::atomic<int> count_{0};
    std::atomic<int> loadReq_{-1};
    std::atomic<bool> scanReq_{false};
    std::atomic<int> remap_{-1};
    std::atomic<int> loadedIndex_{-1};
    std::atomic<bool> busy_{false};
    std::atomic<LoadedSample*> pending_{nullptr};
    std::atomic<LoadedSample*> retired_{nullptr};
    LoadedSample* current_ = nullptr;   // audio thread only
    std::atomic<bool> running_{false};
    std::thread worker_;
};

}} // namespace slowops::scratch
