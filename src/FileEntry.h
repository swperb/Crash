// FileEntry / FileListModel — the real-filesystem data the list renders.
//
// Display strings (size/date/type) are precomputed on the enumeration worker so
// the UI/render thread never formats or calls a shell API (design doc §5).
// The model grows progressively as batches stream in, then is sorted once the
// directory finishes loading.
#pragma once
#include <algorithm>
#include <string>
#include <vector>
#include <cwchar>
#include <cwctype>

struct FileEntry
{
    std::wstring name;        // display name
    std::wstring sizeText;    // "12.4 MB" / "" for folders
    std::wstring dateText;    // "2025-03-14 09:21"
    std::wstring typeText;    // "PNG image" / "File folder" / "Local Disk"
    std::wstring ext;         // ".png" lowercased, "" if none — icon/type key
    std::wstring target;      // explicit nav target (drives); else join(cwd,name)
    uint64_t     sizeBytes = 0;   // raw size for sorting (0 for folders)
    uint64_t     mtime = 0;       // raw FILETIME ticks for sorting
    bool isFolder = false;
    bool isDrive  = false;
};

// Column the list is sorted by.
enum class SortKey { Name, Type, Size, Date };

// The model holds all entries but exposes a *filtered projection* (Count/At walk
// `view_`), so the in-folder search (§6.6) narrows the list without copying data
// and everything downstream (view, selection, context menu) is unchanged.
class FileListModel
{
public:
    size_t Count() const { return view_.size(); }
    const FileEntry& At(size_t i) const { return all_[view_[i]]; }
    size_t TotalCount() const { return all_.size(); }
    const std::wstring& Filter() const { return filter_; }

    void Clear() { all_.clear(); view_.clear(); filter_.clear(); }

    void Append(std::vector<FileEntry>& batch)
    {
        const size_t base = all_.size();
        all_.reserve(all_.size() + batch.size());
        for (auto& e : batch) all_.push_back(std::move(e));
        for (size_t i = base; i < all_.size(); ++i)
            if (Matches(all_[i].name)) view_.push_back(i);
    }

    void SetFilter(const std::wstring& f)
    {
        filter_.clear();
        for (wchar_t c : f) filter_ += static_cast<wchar_t>(towlower(c));
        Rebuild();
    }

    // Folders (and drives) first, then by the chosen column. Explorer keeps
    // folders grouped at the top regardless of the sort key/direction.
    void Sort(SortKey key, bool ascending)
    {
        auto byName = [](const FileEntry& a, const FileEntry& b) {
            return _wcsicmp(a.name.c_str(), b.name.c_str());
        };
        auto cmp = [&](const FileEntry& a, const FileEntry& b) -> int {
            switch (key)
            {
            case SortKey::Type: { int t = _wcsicmp(a.typeText.c_str(), b.typeText.c_str());
                                  return t ? t : byName(a, b); }
            case SortKey::Size: if (a.sizeBytes != b.sizeBytes) return a.sizeBytes < b.sizeBytes ? -1 : 1;
                                return byName(a, b);
            case SortKey::Date: if (a.mtime != b.mtime) return a.mtime < b.mtime ? -1 : 1;
                                return byName(a, b);
            case SortKey::Name:
            default:            return byName(a, b);
            }
        };
        std::sort(all_.begin(), all_.end(), [&](const FileEntry& a, const FileEntry& b) {
            if (a.isFolder != b.isFolder) return a.isFolder;
            const int c = cmp(a, b);
            return ascending ? c < 0 : c > 0;
        });
        Rebuild();
    }

    size_t FolderCount() const
    {
        size_t n = 0; for (size_t i : view_) if (all_[i].isFolder) ++n; return n;
    }

private:
    bool Matches(const std::wstring& name) const
    {
        if (filter_.empty()) return true;
        const size_t n = name.size(), m = filter_.size();
        if (m > n) return false;
        for (size_t i = 0; i + m <= n; ++i)
        {
            size_t j = 0;
            for (; j < m; ++j) if (static_cast<wchar_t>(towlower(name[i + j])) != filter_[j]) break;
            if (j == m) return true;
        }
        return false;
    }
    void Rebuild()
    {
        view_.clear(); view_.reserve(all_.size());
        for (size_t i = 0; i < all_.size(); ++i) if (Matches(all_[i].name)) view_.push_back(i);
    }

    std::vector<FileEntry> all_;
    std::vector<size_t>    view_;
    std::wstring           filter_;
};
