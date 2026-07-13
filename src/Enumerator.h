// Enumerator — the async enumeration pipeline (design doc §5, §6.2).
//
// A dedicated worker thread lists directories with GetFileInformationByHandleEx
// (FileIdBothDirectoryInfo — documented, ~50x faster than naive FindFirstFile,
// see §10) and streams results back to the UI thread in batches via a
// thread-safe queue, waking it with PostMessage. Each navigation carries a
// generation number; a newer navigation cancels an in-flight one, and the UI
// discards any stale batches. The UI thread NEVER touches disk here.
#pragma once
#include "common.h"
#include "FileEntry.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

// Posted (wParam/lParam unused) whenever new events are queued.
#define WM_APP_ENUM (WM_APP + 1)

struct EnumEvent
{
    enum Kind { Batch, Done, Error };
    uint64_t                generation = 0;
    Kind                    kind = Batch;
    std::vector<FileEntry>  entries;
    std::wstring            message;   // Error: reason; Done: display path
};

class Enumerator
{
public:
    explicit Enumerator(HWND notify);
    ~Enumerator();

    // Kick off enumeration of `path` ("" => This PC / drive list) under `gen`.
    // Supersedes any in-flight job.
    void Navigate(uint64_t gen, std::wstring path);

    // Recursive subtree search from `root` for names containing `query`.
    // Works on any path (incl. network shares); uses Everything for local drives
    // when available, else walks the tree. Streams results like enumeration.
    void Search(uint64_t gen, std::wstring root, std::wstring query);

    // UI thread: pull and clear queued events.
    std::vector<EnumEvent> Drain();

    // Include hidden files in future enumerations (Settings).
    void SetShowHidden(bool b) { showHidden_.store(b, std::memory_order_relaxed); }

private:
    void WorkerLoop();
    void EnumerateDirectory(uint64_t gen, const std::wstring& path);
    void EnumerateDrives(uint64_t gen);
    void SearchTree(uint64_t gen, const std::wstring& root, const std::wstring& queryLower);
    bool EverythingSearch(uint64_t gen, const std::wstring& root, const std::wstring& queryLower);
    void Post(EnumEvent&& ev);
    bool Superseded(uint64_t gen) const { return liveGen_.load(std::memory_order_acquire) != gen; }

    HWND hwnd_;

    std::thread             worker_;
    std::mutex              jobM_;
    std::condition_variable jobCv_;
    uint64_t                requestedGen_ = 0;
    std::wstring            requestedPath_;
    std::wstring            requestedQuery_;
    bool                    requestedIsSearch_ = false;
    bool                    hasJob_ = false;
    bool                    stop_ = false;
    std::atomic<uint64_t>   liveGen_{ 0 };   // newest requested gen (cancellation)
    std::atomic<bool>       showHidden_{ false };

    std::mutex              outM_;
    std::vector<EnumEvent>  out_;
};
