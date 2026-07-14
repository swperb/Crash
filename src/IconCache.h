// IconCache — maps a file's extension/kind to a Direct2D bitmap of its system
// icon (design doc §9 Phase 1: "basic icons (no thumbnails yet)").
//
// Icons are resolved with SHGetFileInfo + SHGFI_USEFILEATTRIBUTES, so lookup is
// keyed purely by extension/kind and never touches the real file — cheap enough
// to do lazily on the UI thread for the handful of visible extensions, and safe
// against network/cloud stalls. Thumbnails (per-file, backgrounded) come later.
#pragma once
#include "common.h"
#include "FileEntry.h"
#include <unordered_map>

class IconCache
{
public:
    // Returns a cached bitmap for the entry's kind, or nullptr if unavailable.
    // `large` selects the 32px system icon (grid) vs the 16px one (details).
    ID2D1Bitmap* Get(ID2D1DeviceContext* dc, const FileEntry& e, bool large);

    // Real icon for a specific path (folder/drive/file) — used by the navigation
    // sidebar and Home page, where the distinct known-folder/drive icons matter.
    // Keyed by path (a handful of local roots), so the SHGetFileInfo touch is rare.
    ID2D1Bitmap* GetForPath(ID2D1DeviceContext* dc, const std::wstring& path, bool large);

    // 48px extra-large icon for a path, from the shell's system image list — used
    // by the details pane so known folders show their crisp, fully-coloured icon.
    ID2D1Bitmap* GetLargeForPath(ID2D1DeviceContext* dc, const std::wstring& path);

private:
    ComPtr<ID2D1Bitmap> Resolve(ID2D1DeviceContext* dc, const FileEntry& e, bool large);
    ComPtr<ID2D1Bitmap> FromHIcon(ID2D1DeviceContext* dc, HICON hic);

    std::unordered_map<std::wstring, ComPtr<ID2D1Bitmap>> map_;
    ID2D1DeviceContext* owner_ = nullptr;   // brushes/bitmaps are device-bound
};
