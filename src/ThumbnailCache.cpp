#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "ThumbnailCache.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <cstdio>
#include <cwctype>
#include <algorithm>

namespace
{
    constexpr int   kThumb = 160;          // target thumbnail edge (px)
    constexpr size_t kMemCap = 512;         // in-memory bitmaps
    constexpr int   kDiskCapFiles = 6000;   // on-disk thumbnail files
    constexpr int   kWorkers = 2;

    struct DiskHdr { char magic[4]; uint32_t w, h; };

    std::wstring ThumbsDir()
    {
        PWSTR local = nullptr; std::wstring d;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
        {
            d = local; CoTaskMemFree(local);
            d += L"\\Crash"; CreateDirectoryW(d.c_str(), nullptr);
            d += L"\\thumbs"; CreateDirectoryW(d.c_str(), nullptr);
            d += L"\\";
        }
        return d;
    }

    // HBITMAP (32bpp from GetImage) → premultiplied BGRA pixels.
    bool BitmapToBGRA(HBITMAP hb, int& w, int& h, std::vector<uint32_t>& px)
    {
        BITMAP bm{};
        if (GetObjectW(hb, sizeof(bm), &bm) == 0 || bm.bmWidth <= 0 || bm.bmHeight <= 0) return false;
        w = bm.bmWidth; h = bm.bmHeight;

        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;   // top-down
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;

        px.assign(static_cast<size_t>(w) * h, 0);
        HDC hdc = GetDC(nullptr);
        const int got = GetDIBits(hdc, hb, 0, h, px.data(), &bi, DIB_RGB_COLORS);
        ReleaseDC(nullptr, hdc);
        if (got == 0) return false;

        // GetImage returns premultiplied alpha, but fully-opaque images often
        // come back with alpha=0. Treat all-zero alpha as opaque; otherwise
        // trust the premultiplied values as-is.
        bool allZero = true;
        for (uint32_t p : px) if ((p >> 24) != 0) { allZero = false; break; }
        if (allZero) for (uint32_t& p : px) p |= 0xFF000000u;
        return true;
    }

    bool Extract(const std::wstring& path, int& w, int& h, std::vector<uint32_t>& px)
    {
        ComPtr<IShellItem> item;
        if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)))) return false;
        ComPtr<IShellItemImageFactory> fac;
        if (FAILED(item.As(&fac))) return false;
        SIZE sz{ kThumb, kThumb };
        HBITMAP hb = nullptr;
        if (FAILED(fac->GetImage(sz, SIIGBF_THUMBNAILONLY | SIIGBF_RESIZETOFIT, &hb)) || !hb) return false;
        const bool ok = BitmapToBGRA(hb, w, h, px);
        DeleteObject(hb);
        return ok;
    }

    std::wstring DiskPath(const std::wstring& dir, uint64_t key)
    {
        if (dir.empty()) return {};
        wchar_t name[32]; std::swprintf(name, std::size(name), L"%016llx.cth", (unsigned long long)key);
        return dir + name;
    }

    bool ReadDisk(const std::wstring& dir, uint64_t key, int& w, int& h, std::vector<uint32_t>& px)
    {
        std::wstring path = DiskPath(dir, key);
        if (path.empty()) return false;
        FILE* f = _wfopen(path.c_str(), L"rb");
        if (!f) return false;
        DiskHdr hd{};
        bool ok = fread(&hd, sizeof(hd), 1, f) == 1 &&
                  hd.magic[0] == 'C' && hd.magic[1] == 'T' && hd.magic[2] == 'H' && hd.magic[3] == '2' &&
                  hd.w >= 1 && hd.w <= 1024 && hd.h >= 1 && hd.h <= 1024;
        if (ok)
        {
            w = (int)hd.w; h = (int)hd.h;
            px.assign(static_cast<size_t>(w) * h, 0);
            ok = fread(px.data(), 1, px.size() * 4, f) == px.size() * 4;
        }
        fclose(f);
        return ok;
    }

    void WriteDisk(const std::wstring& dir, uint64_t key, int w, int h, const std::vector<uint32_t>& px)
    {
        std::wstring path = DiskPath(dir, key);
        if (path.empty()) return;
        FILE* f = _wfopen(path.c_str(), L"wb");
        if (!f) return;
        DiskHdr hd{ { 'C','T','H','2' }, (uint32_t)w, (uint32_t)h };
        fwrite(&hd, sizeof(hd), 1, f);
        fwrite(px.data(), 1, px.size() * 4, f);
        fclose(f);
    }

    // Keep the on-disk store bounded: when it grows past the cap, delete the
    // oldest files down to ~80%.
    void TrimDisk(const std::wstring& dir)
    {
        if (dir.empty()) return;
        std::vector<std::pair<uint64_t, std::wstring>> files;   // (lastWrite, fullpath)
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((dir + L"*.cth").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                ULARGE_INTEGER t{ fd.ftLastWriteTime.dwLowDateTime, fd.ftLastWriteTime.dwHighDateTime };
                files.emplace_back(t.QuadPart, dir + fd.cFileName);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);

        if ((int)files.size() <= kDiskCapFiles) return;
        std::sort(files.begin(), files.end(), [](auto& a, auto& b) { return a.first < b.first; });
        const size_t target = static_cast<size_t>(kDiskCapFiles * 0.8);
        for (size_t i = 0; i + target < files.size(); ++i) DeleteFileW(files[i].second.c_str());
    }
}

ThumbnailCache::ThumbnailCache(HWND notify) : hwnd_(notify)
{
    diskDir_ = ThumbsDir();
    for (int i = 0; i < kWorkers; ++i) workers_.emplace_back(&ThumbnailCache::WorkerLoop, this);
}

ThumbnailCache::~ThumbnailCache()
{
    { std::lock_guard<std::mutex> lk(m_); stop_ = true; }
    cv_.notify_all();
    for (auto& t : workers_) if (t.joinable()) t.join();
}

uint64_t ThumbnailCache::Key(const std::wstring& path, uint64_t mtime, uint64_t size)
{
    uint64_t hsh = 1469598103934665603ULL;
    for (wchar_t c : path) { hsh ^= (uint64_t)towlower(c); hsh *= 1099511628211ULL; }
    hsh ^= mtime; hsh *= 1099511628211ULL;
    hsh ^= size;  hsh *= 1099511628211ULL;
    return hsh;
}

ID2D1Bitmap* ThumbnailCache::Get(const std::wstring& fullPath, uint64_t mtime, uint64_t size)
{
    const uint64_t key = Key(fullPath, mtime, size);
    frameKeys_.insert(key);

    auto it = mem_.find(key);
    if (it != mem_.end()) { lru_.splice(lru_.begin(), lru_, it->second.it); return it->second.bmp.Get(); }
    if (noThumb_.count(key)) return nullptr;
    if (!pending_.count(key)) { pending_.insert(key); Enqueue({ key, fullPath, mtime, size }); }
    return nullptr;
}

void ThumbnailCache::Enqueue(const Req& r)
{
    { std::lock_guard<std::mutex> lk(m_); queue_.push_back(r); wanted_.insert(r.key); }
    cv_.notify_one();
}

void ThumbnailCache::BeginFrame() { frameKeys_.clear(); }

void ThumbnailCache::EndFrame()
{
    std::lock_guard<std::mutex> lk(m_);
    wanted_ = frameKeys_;   // prune worker's notion of what's still on-screen
}

void ThumbnailCache::PushResult(Result&& r)
{
    { std::lock_guard<std::mutex> lk(om_); out_.push_back(std::move(r)); }
    PostMessageW(hwnd_, WM_APP_THUMB, 0, 0);
}

void ThumbnailCache::WorkerLoop()
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    for (;;)
    {
        Req r;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [&] { return !queue_.empty() || stop_; });
            if (stop_) break;
            r = queue_.back(); queue_.pop_back();          // newest first (visible)
            if (!wanted_.count(r.key)) { lk.unlock(); PushResult({ r.key, ST_DROPPED }); continue; }
        }

        int w = 0, h = 0; std::vector<uint32_t> px;
        if (ReadDisk(diskDir_, r.key, w, h, px)) { PushResult({ r.key, ST_READY, w, h, std::move(px) }); continue; }
        if (Extract(r.path, w, h, px))
        {
            WriteDisk(diskDir_, r.key, w, h, px);
            if ((writeCount_.fetch_add(1) % 256) == 255) TrimDisk(diskDir_);
            PushResult({ r.key, ST_READY, w, h, std::move(px) });
        }
        else PushResult({ r.key, ST_NONE });
    }
    CoUninitialize();
}

void ThumbnailCache::PumpResults(ID2D1DeviceContext* dc)
{
    if (owner_ != dc) { mem_.clear(); lru_.clear(); owner_ = dc; }   // device changed

    std::vector<Result> results;
    { std::lock_guard<std::mutex> lk(om_); results.swap(out_); }

    for (Result& r : results)
    {
        pending_.erase(r.key);
        if (r.status == ST_NONE) { noThumb_.insert(r.key); continue; }
        if (r.status == ST_DROPPED) continue;   // re-requested when visible again
        if (mem_.count(r.key)) continue;

        D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
        ComPtr<ID2D1Bitmap> bmp;
        if (FAILED(dc->CreateBitmap(D2D1::SizeU(r.w, r.h), r.px.data(), static_cast<UINT32>(r.w * 4), props, &bmp)))
            continue;

        lru_.push_front(r.key);
        mem_[r.key] = { bmp, lru_.begin() };
        while (mem_.size() > kMemCap) { uint64_t old = lru_.back(); lru_.pop_back(); mem_.erase(old); }
    }
}
