#include "Enumerator.h"

#include <shellapi.h>
#include <unordered_map>

namespace
{
    constexpr size_t kBatchSize = 512;      // entries streamed per wakeup
    constexpr size_t kEnumBuffer = 128 * 1024;

    std::wstring FormatSize(uint64_t bytes)
    {
        const wchar_t* u[] = { L"B", L"KB", L"MB", L"GB", L"TB", L"PB" };
        double v = static_cast<double>(bytes); int i = 0;
        while (v >= 1024.0 && i < 5) { v /= 1024.0; ++i; }
        wchar_t b[32];
        std::swprintf(b, std::size(b), (i == 0) ? L"%.0f %s" : L"%.1f %s", v, u[i]);
        return b;
    }

    std::wstring FormatDate(const FILETIME& ft)
    {
        if (ft.dwLowDateTime == 0 && ft.dwHighDateTime == 0) return L"";
        FILETIME local{}; SYSTEMTIME st{};
        FileTimeToLocalFileTime(&ft, &local);
        if (!FileTimeToSystemTime(&local, &st)) return L"";
        wchar_t b[32];
        std::swprintf(b, std::size(b), L"%04d-%02d-%02d %02d:%02d",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
        return b;
    }

    std::wstring LowerExt(const std::wstring& name)
    {
        const size_t dot = name.find_last_of(L'.');
        if (dot == std::wstring::npos || dot == 0 || dot + 1 >= name.size()) return L"";
        std::wstring e = name.substr(dot);
        for (auto& c : e) c = static_cast<wchar_t>(towlower(c));
        return e;
    }

    // Real Explorer-style type name for an extension, keyed/cached by ext.
    // Uses SHGFI_USEFILEATTRIBUTES so it never touches the real file (safe even
    // for network/cloud paths) — and runs on the worker thread regardless.
    std::wstring TypeName(const std::wstring& ext, std::unordered_map<std::wstring, std::wstring>& cache)
    {
        auto it = cache.find(ext);
        if (it != cache.end()) return it->second;

        std::wstring dummy = L"x" + ext;
        SHFILEINFOW sfi{};
        std::wstring result;
        if (SHGetFileInfoW(dummy.c_str(), FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES))
            result = sfi.szTypeName;
        if (result.empty())
            result = ext.empty() ? L"File" : (ext.substr(1) + L" File");
        cache.emplace(ext, result);
        return result;
    }

    std::wstring JoinPath(const std::wstring& dir, const std::wstring& name)
    {
        if (dir.empty()) return name;
        if (dir.back() == L'\\') return dir + name;
        return dir + L"\\" + name;
    }

    bool NameMatches(const std::wstring& name, const std::wstring& qLower)
    {
        if (qLower.empty()) return true;
        std::wstring n; n.reserve(name.size());
        for (wchar_t c : name) n += static_cast<wchar_t>(towlower(c));
        return n.find(qLower) != std::wstring::npos;
    }

    bool RootIsLocalFixed(const std::wstring& root)
    {
        if (root.size() < 3 || root[1] != L':') return false;   // UNC / odd → not fixed
        std::wstring drive = root.substr(0, 3);
        return GetDriveTypeW(drive.c_str()) == DRIVE_FIXED;
    }
}

Enumerator::Enumerator(HWND notify) : hwnd_(notify)
{
    worker_ = std::thread(&Enumerator::WorkerLoop, this);
}

Enumerator::~Enumerator()
{
    {
        std::lock_guard<std::mutex> lk(jobM_);
        stop_ = true;
    }
    jobCv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void Enumerator::Navigate(uint64_t gen, std::wstring path)
{
    {
        std::lock_guard<std::mutex> lk(jobM_);
        requestedGen_ = gen;
        requestedPath_ = std::move(path);
        requestedIsSearch_ = false;
        hasJob_ = true;
    }
    liveGen_.store(gen, std::memory_order_release);   // cancels older in-flight
    jobCv_.notify_all();
}

void Enumerator::Search(uint64_t gen, std::wstring root, std::wstring query)
{
    {
        std::lock_guard<std::mutex> lk(jobM_);
        requestedGen_ = gen;
        requestedPath_ = std::move(root);
        requestedQuery_ = std::move(query);
        requestedIsSearch_ = true;
        hasJob_ = true;
    }
    liveGen_.store(gen, std::memory_order_release);
    jobCv_.notify_all();
}

std::vector<EnumEvent> Enumerator::Drain()
{
    std::lock_guard<std::mutex> lk(outM_);
    std::vector<EnumEvent> ev = std::move(out_);
    out_.clear();
    return ev;
}

void Enumerator::Post(EnumEvent&& ev)
{
    {
        std::lock_guard<std::mutex> lk(outM_);
        out_.push_back(std::move(ev));
    }
    PostMessageW(hwnd_, WM_APP_ENUM, 0, 0);
}

void Enumerator::WorkerLoop()
{
    for (;;)
    {
        uint64_t gen; std::wstring path, query; bool isSearch;
        {
            std::unique_lock<std::mutex> lk(jobM_);
            jobCv_.wait(lk, [&] { return hasJob_ || stop_; });
            if (stop_) return;
            gen = requestedGen_;
            path = requestedPath_;
            query = requestedQuery_;
            isSearch = requestedIsSearch_;
            hasJob_ = false;
        }

        if (isSearch)
        {
            std::wstring qLower; for (wchar_t c : query) qLower += static_cast<wchar_t>(towlower(c));
            SearchTree(gen, path, qLower);
        }
        else if (path.empty() || path == kThisPC) EnumerateDrives(gen);
        else                   EnumerateDirectory(gen, path);
    }
}

void Enumerator::EnumerateDrives(uint64_t gen)
{
    wchar_t buf[512];
    const DWORD n = GetLogicalDriveStringsW(static_cast<DWORD>(std::size(buf)), buf);
    for (const wchar_t* p = buf; p < buf + n && *p; )
    {
        std::wstring root = p;                 // e.g. "C:\"
        p += root.size() + 1;
        if (Superseded(gen)) return;

        const UINT type = GetDriveTypeW(root.c_str());
        const wchar_t* typeName = L"Drive";
        switch (type)
        {
        case DRIVE_FIXED:     typeName = L"Local Disk";      break;
        case DRIVE_REMOVABLE: typeName = L"Removable Disk";  break;
        case DRIVE_REMOTE:    typeName = L"Network Drive";   break;
        case DRIVE_CDROM:     typeName = L"CD Drive";        break;
        case DRIVE_RAMDISK:   typeName = L"RAM Disk";        break;
        default: break;
        }

        // Only read the volume label for local fixed disks — GetVolumeInformation
        // can block for seconds on network/optical/removable drives, and we must
        // not delay the whole listing behind one slow drive.
        std::wstring label;
        if (type == DRIVE_FIXED)
        {
            wchar_t vol[MAX_PATH + 1] = {};
            if (GetVolumeInformationW(root.c_str(), vol, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0))
                label = vol;
        }

        std::wstring letter = root.substr(0, 2);            // "C:"
        FileEntry e;
        e.name = (label.empty() ? std::wstring(typeName) : label) + L" (" + letter + L")";
        e.typeText = typeName;
        e.isFolder = true;
        e.isDrive = true;
        e.target = root;

        // Stream each drive as its own batch so fast drives appear immediately.
        std::vector<FileEntry> one;
        one.push_back(std::move(e));
        Post(EnumEvent{ gen, EnumEvent::Batch, std::move(one), {} });
    }

    if (Superseded(gen)) return;
    Post(EnumEvent{ gen, EnumEvent::Done, {}, L"This PC" });
}

void Enumerator::EnumerateDirectory(uint64_t gen, const std::wstring& path)
{
    HANDLE h = CreateFileW(path.c_str(), FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        const DWORD err = GetLastError();
        std::wstring msg = (err == ERROR_ACCESS_DENIED) ? L"Access denied"
            : (err == ERROR_PATH_NOT_FOUND || err == ERROR_FILE_NOT_FOUND) ? L"Path not found"
            : L"Unable to open folder";
        Post(EnumEvent{ gen, EnumEvent::Error, {}, msg });
        return;
    }

    std::vector<char> buffer(kEnumBuffer);
    std::unordered_map<std::wstring, std::wstring> typeCache;
    std::vector<FileEntry> batch;
    batch.reserve(kBatchSize);

    for (;;)
    {
        if (Superseded(gen)) { CloseHandle(h); return; }

        if (!GetFileInformationByHandleEx(h, FileIdBothDirectoryInfo,
                buffer.data(), static_cast<DWORD>(buffer.size())))
        {
            const DWORD err = GetLastError();
            if (err == ERROR_NO_MORE_FILES) break;
            CloseHandle(h);
            Post(EnumEvent{ gen, EnumEvent::Error, {}, L"Enumeration failed" });
            return;
        }

        auto* info = reinterpret_cast<FILE_ID_BOTH_DIR_INFO*>(buffer.data());
        for (;;)
        {
            const size_t nameLen = info->FileNameLength / sizeof(WCHAR);
            std::wstring name(info->FileName, nameLen);

            if (name != L"." && name != L"..")
            {
                const bool isDir = (info->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                const bool hidden = (info->FileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
                if (showHidden_.load(std::memory_order_relaxed) || !hidden)
                {
                    FileEntry e;
                    e.name = std::move(name);
                    e.isFolder = isDir;
                    FILETIME mtime{ info->LastWriteTime.LowPart,
                                    static_cast<DWORD>(info->LastWriteTime.HighPart) };
                    e.dateText = FormatDate(mtime);
                    e.mtime = static_cast<uint64_t>(info->LastWriteTime.QuadPart);
                    if (isDir)
                    {
                        e.typeText = L"File folder";
                    }
                    else
                    {
                        e.ext = LowerExt(e.name);
                        e.typeText = TypeName(e.ext, typeCache);
                        e.sizeBytes = static_cast<uint64_t>(info->EndOfFile.QuadPart);
                        e.sizeText = FormatSize(e.sizeBytes);
                    }
                    batch.push_back(std::move(e));

                    if (batch.size() >= kBatchSize)
                    {
                        Post(EnumEvent{ gen, EnumEvent::Batch, std::move(batch), {} });
                        batch.clear();
                        batch.reserve(kBatchSize);
                        if (Superseded(gen)) { CloseHandle(h); return; }
                    }
                }
            }

            if (info->NextEntryOffset == 0) break;
            info = reinterpret_cast<FILE_ID_BOTH_DIR_INFO*>(
                reinterpret_cast<char*>(info) + info->NextEntryOffset);
        }
    }

    CloseHandle(h);
    if (Superseded(gen)) return;
    if (!batch.empty()) Post(EnumEvent{ gen, EnumEvent::Batch, std::move(batch), {} });
    Post(EnumEvent{ gen, EnumEvent::Done, {}, path });
}

// Recursive subtree search. Names are stored relative to the root so the result
// location is visible; the full path is kept in FileEntry::target for activation.
void Enumerator::SearchTree(uint64_t gen, const std::wstring& root, const std::wstring& qLower)
{
    // Local fixed drives: try Everything (instant). Network/removable and any
    // failure fall through to the live walker below.
    if (RootIsLocalFixed(root) && EverythingSearch(gen, root, qLower)) return;

    std::vector<std::wstring> stack{ root };
    std::vector<FileEntry> batch; batch.reserve(kBatchSize);
    std::unordered_map<std::wstring, std::wstring> typeCache;
    std::vector<char> buffer(kEnumBuffer);
    const size_t rootLen = root.size();

    while (!stack.empty())
    {
        if (Superseded(gen)) return;
        std::wstring dir = std::move(stack.back()); stack.pop_back();

        HANDLE h = CreateFileW(dir.c_str(), FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;   // skip inaccessible folders

        for (;;)
        {
            if (Superseded(gen)) { CloseHandle(h); return; }
            if (!GetFileInformationByHandleEx(h, FileIdBothDirectoryInfo, buffer.data(), static_cast<DWORD>(buffer.size())))
                break;   // ERROR_NO_MORE_FILES or any error → done with this dir

            auto* info = reinterpret_cast<FILE_ID_BOTH_DIR_INFO*>(buffer.data());
            for (;;)
            {
                const size_t nameLen = info->FileNameLength / sizeof(WCHAR);
                std::wstring name(info->FileName, nameLen);
                if (name != L"." && name != L"..")
                {
                    const bool isDir = (info->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                    const bool hidden = (info->FileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
                    if (showHidden_.load(std::memory_order_relaxed) || !hidden)
                    {
                        std::wstring full = JoinPath(dir, name);
                        if (isDir) stack.push_back(full);
                        if (NameMatches(name, qLower))
                        {
                            FileEntry e;
                            std::wstring rel = (full.size() > rootLen) ? full.substr(rootLen) : name;
                            if (!rel.empty() && rel[0] == L'\\') rel.erase(0, 1);
                            e.name = std::move(rel);
                            e.target = full;
                            e.isFolder = isDir;
                            FILETIME mt{ info->LastWriteTime.LowPart, static_cast<DWORD>(info->LastWriteTime.HighPart) };
                            e.dateText = FormatDate(mt);
                            e.mtime = static_cast<uint64_t>(info->LastWriteTime.QuadPart);
                            if (isDir) e.typeText = L"File folder";
                            else { e.ext = LowerExt(name); e.typeText = TypeName(e.ext, typeCache);
                                   e.sizeBytes = static_cast<uint64_t>(info->EndOfFile.QuadPart); e.sizeText = FormatSize(e.sizeBytes); }
                            batch.push_back(std::move(e));
                            if (batch.size() >= kBatchSize)
                            {
                                Post(EnumEvent{ gen, EnumEvent::Batch, std::move(batch), {} });
                                batch.clear(); batch.reserve(kBatchSize);
                                if (Superseded(gen)) { CloseHandle(h); return; }
                            }
                        }
                    }
                }
                if (info->NextEntryOffset == 0) break;
                info = reinterpret_cast<FILE_ID_BOTH_DIR_INFO*>(reinterpret_cast<char*>(info) + info->NextEntryOffset);
            }
        }
        CloseHandle(h);
    }

    if (Superseded(gen)) return;
    if (!batch.empty()) Post(EnumEvent{ gen, EnumEvent::Batch, std::move(batch), {} });
    Post(EnumEvent{ gen, EnumEvent::Done, {}, root });
}

// Optional acceleration via a locally-installed Everything (voidtools). Loaded
// dynamically; if Everything64.dll is absent or Everything isn't running, returns
// false so the caller walks the tree instead.
bool Enumerator::EverythingSearch(uint64_t gen, const std::wstring& root, const std::wstring& qLower)
{
    HMODULE m = LoadLibraryW(L"Everything64.dll");
    if (!m) return false;

    using SetSearchFn = void(__stdcall*)(LPCWSTR);
    using QueryFn = BOOL(__stdcall*)(BOOL);
    using GetNumFn = DWORD(__stdcall*)();
    using GetFullFn = DWORD(__stdcall*)(DWORD, LPWSTR, DWORD);
    using IsFolderFn = BOOL(__stdcall*)(DWORD);
    using SetFlagsFn = void(__stdcall*)(DWORD);

    auto SetSearch = reinterpret_cast<SetSearchFn>(GetProcAddress(m, "Everything_SetSearchW"));
    auto Query = reinterpret_cast<QueryFn>(GetProcAddress(m, "Everything_QueryW"));
    auto GetNum = reinterpret_cast<GetNumFn>(GetProcAddress(m, "Everything_GetNumResults"));
    auto GetFull = reinterpret_cast<GetFullFn>(GetProcAddress(m, "Everything_GetResultFullPathNameW"));
    auto IsFolder = reinterpret_cast<IsFolderFn>(GetProcAddress(m, "Everything_IsFolderResult"));
    auto SetFlags = reinterpret_cast<SetFlagsFn>(GetProcAddress(m, "Everything_SetRequestFlags"));
    if (!SetSearch || !Query || !GetNum || !GetFull) { FreeLibrary(m); return false; }

    if (SetFlags) SetFlags(0x00000004 /*FULL_PATH_AND_FILE_NAME*/);
    std::wstring s = L"\"" + root + L"\" " + qLower;   // scope to folder + name filter
    SetSearch(s.c_str());
    if (!Query(TRUE)) { FreeLibrary(m); return false; }   // Everything not running

    const DWORD n = GetNum();
    std::unordered_map<std::wstring, std::wstring> typeCache;
    std::vector<FileEntry> batch; batch.reserve(kBatchSize);
    const size_t rootLen = root.size();
    for (DWORD i = 0; i < n; ++i)
    {
        if (Superseded(gen)) { FreeLibrary(m); return true; }
        wchar_t full[1024]; GetFull(i, full, static_cast<DWORD>(std::size(full)));
        std::wstring fp = full;
        FileEntry e;
        std::wstring rel = (fp.size() > rootLen) ? fp.substr(rootLen) : fp;
        if (!rel.empty() && rel[0] == L'\\') rel.erase(0, 1);
        e.name = rel.empty() ? fp : rel;
        e.target = fp;
        e.isFolder = IsFolder && IsFolder(i);
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(fp.c_str(), GetFileExInfoStandard, &fad))
        {
            e.dateText = FormatDate(fad.ftLastWriteTime);
            e.mtime = (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) | fad.ftLastWriteTime.dwLowDateTime;
            if (!e.isFolder) { e.sizeBytes = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow; e.sizeText = FormatSize(e.sizeBytes); }
        }
        if (e.isFolder) e.typeText = L"File folder";
        else { const size_t dot = e.name.find_last_of(L'.'); if (dot != std::wstring::npos) { e.ext = e.name.substr(dot); for (auto& c : e.ext) c = static_cast<wchar_t>(towlower(c)); } e.typeText = TypeName(e.ext, typeCache); }
        batch.push_back(std::move(e));
        if (batch.size() >= kBatchSize) { Post(EnumEvent{ gen, EnumEvent::Batch, std::move(batch), {} }); batch.clear(); batch.reserve(kBatchSize); }
    }
    FreeLibrary(m);
    if (Superseded(gen)) return true;
    if (!batch.empty()) Post(EnumEvent{ gen, EnumEvent::Batch, std::move(batch), {} });
    Post(EnumEvent{ gen, EnumEvent::Done, {}, root });
    return true;
}
