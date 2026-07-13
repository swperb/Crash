// ThumbnailCache — two-tier, background, cancellable thumbnail store (design
// doc §5 thumbnail workers + §6.3 icon/thumbnail cache).
//
//   * Tier 1: in-memory LRU of Direct2D bitmaps (instant re-scroll).
//   * Tier 2: on-disk store keyed by path+mtime+size (instant re-visit after
//     restart; a changed file yields a new key, so edits auto-invalidate).
//   * Generation runs on low-priority worker threads via IShellItemImageFactory
//     and NEVER blocks list paint: a row shows the generic icon immediately and
//     the real thumbnail swaps in when ready.
//   * Off-screen work is dropped, not queued forever: each render reports the
//     currently-visible keys, and a worker abandons any request no longer wanted.
#pragma once
#include "common.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define WM_APP_THUMB (WM_APP + 2)

class ThumbnailCache
{
public:
    explicit ThumbnailCache(HWND notify);
    ~ThumbnailCache();

    // UI thread. Returns a cached bitmap or nullptr; enqueues generation on miss.
    ID2D1Bitmap* Get(const std::wstring& fullPath, uint64_t mtime, uint64_t size);

    // UI thread, around a frame: PumpResults uploads any ready thumbnails to D2D
    // bitmaps; BeginFrame/EndFrame bracket the visible-key collection so the
    // worker can drop requests that scrolled off-screen.
    void PumpResults(ID2D1DeviceContext* dc);
    void BeginFrame();
    void EndFrame();

private:
    enum Status { ST_READY, ST_NONE, ST_DROPPED };
    struct Req { uint64_t key; std::wstring path; uint64_t mtime, size; };
    struct Result { uint64_t key; Status status; int w = 0, h = 0; std::vector<uint32_t> px; };

    static uint64_t Key(const std::wstring& path, uint64_t mtime, uint64_t size);
    void Enqueue(const Req& r);
    void WorkerLoop();
    void PushResult(Result&& r);

    HWND hwnd_;

    // --- shared with workers ---
    std::mutex m_;
    std::condition_variable cv_;
    std::vector<Req> queue_;
    std::unordered_set<uint64_t> wanted_;
    bool stop_ = false;
    std::vector<std::thread> workers_;
    std::mutex om_;
    std::vector<Result> out_;
    std::wstring diskDir_;
    std::atomic<int> writeCount_{ 0 };

    // --- UI-thread only ---
    struct MemEntry { ComPtr<ID2D1Bitmap> bmp; std::list<uint64_t>::iterator it; };
    std::unordered_map<uint64_t, MemEntry> mem_;
    std::list<uint64_t> lru_;                      // front = most recent
    std::unordered_set<uint64_t> pending_, noThumb_;
    std::unordered_set<uint64_t> frameKeys_;       // visible this frame
    ID2D1DeviceContext* owner_ = nullptr;
};
