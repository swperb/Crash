#include "IconCache.h"
#include <shellapi.h>
#include <commoncontrols.h>   // IImageList / SHGetImageList (extra-large icons)
#include <vector>

ID2D1Bitmap* IconCache::Get(ID2D1DeviceContext* dc, const FileEntry& e, bool large)
{
    if (owner_ != dc) { map_.clear(); owner_ = dc; }   // device changed → drop

    std::wstring key = (large ? L"L|" : L"S|");
    if (e.isDrive)       key += L"<drive>";
    else if (e.isFolder) key += L"<dir>";
    else                 key += e.ext.empty() ? L"<file>" : e.ext;

    auto it = map_.find(key);
    if (it != map_.end()) return it->second.Get();

    ComPtr<ID2D1Bitmap> bmp = Resolve(dc, e, large);
    ID2D1Bitmap* raw = bmp.Get();
    map_.emplace(std::move(key), std::move(bmp));
    return raw;
}

ID2D1Bitmap* IconCache::GetForPath(ID2D1DeviceContext* dc, const std::wstring& path, bool large)
{
    if (owner_ != dc) { map_.clear(); owner_ = dc; }
    std::wstring key = (large ? L"LP|" : L"SP|") + path;
    auto it = map_.find(key);
    if (it != map_.end()) return it->second.Get();

    ComPtr<ID2D1Bitmap> bmp;
    SHFILEINFOW sfi{};
    if (SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi),
            SHGFI_ICON | (large ? SHGFI_LARGEICON : SHGFI_SMALLICON)) && sfi.hIcon)
    {
        bmp = FromHIcon(dc, sfi.hIcon);
        DestroyIcon(sfi.hIcon);
    }
    ID2D1Bitmap* raw = bmp.Get();
    map_.emplace(std::move(key), std::move(bmp));
    return raw;
}

ID2D1Bitmap* IconCache::GetLargeForPath(ID2D1DeviceContext* dc, const std::wstring& path)
{
    if (owner_ != dc) { map_.clear(); owner_ = dc; }
    std::wstring key = L"XL|" + path;
    auto it = map_.find(key);
    if (it != map_.end()) return it->second.Get();

    ComPtr<ID2D1Bitmap> bmp;
    SHFILEINFOW sfi{};
    // Resolve the shell icon index, then pull the 48px image from the
    // SHIL_EXTRALARGE system image list — this carries the special coloured
    // known-folder icons (Desktop/Downloads/Pictures/…) at full quality.
    if (SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX))
    {
        IImageList* iml = nullptr;
        if (SUCCEEDED(SHGetImageList(SHIL_EXTRALARGE, IID_IImageList, reinterpret_cast<void**>(&iml))) && iml)
        {
            HICON hic = nullptr;
            if (SUCCEEDED(iml->GetIcon(sfi.iIcon, ILD_TRANSPARENT, &hic)) && hic)
            {
                bmp = FromHIcon(dc, hic);
                DestroyIcon(hic);
            }
            iml->Release();
        }
    }
    ID2D1Bitmap* raw = bmp.Get();
    map_.emplace(std::move(key), std::move(bmp));
    return raw;
}

ComPtr<ID2D1Bitmap> IconCache::Resolve(ID2D1DeviceContext* dc, const FileEntry& e, bool large)
{
    HICON hic = nullptr;
    const UINT sizeFlag = large ? SHGFI_LARGEICON : SHGFI_SMALLICON;

    if (e.isDrive)
    {
        SHSTOCKICONINFO sii{ sizeof(sii) };
        if (SUCCEEDED(SHGetStockIconInfo(SIID_DRIVEFIXED,
                SHGSI_ICON | (large ? SHGSI_LARGEICON : SHGSI_SMALLICON), &sii)))
            hic = sii.hIcon;
    }
    else
    {
        const DWORD attr = e.isFolder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        std::wstring dummy = e.isFolder ? L"folder"
                                        : (L"file" + (e.ext.empty() ? L"" : e.ext));
        SHFILEINFOW sfi{};
        if (SHGetFileInfoW(dummy.c_str(), attr, &sfi, sizeof(sfi),
                SHGFI_ICON | sizeFlag | SHGFI_USEFILEATTRIBUTES))
            hic = sfi.hIcon;
    }

    if (!hic) return nullptr;
    ComPtr<ID2D1Bitmap> bmp = FromHIcon(dc, hic);
    DestroyIcon(hic);
    return bmp;
}

// Convert an HICON to a premultiplied BGRA Direct2D bitmap. Uses the icon's
// color bitmap for alpha when present, falling back to the AND mask for legacy
// icons that carry no per-pixel alpha.
ComPtr<ID2D1Bitmap> IconCache::FromHIcon(ID2D1DeviceContext* dc, HICON hic)
{
    ICONINFO ii{};
    if (!GetIconInfo(hic, &ii)) return nullptr;

    BITMAP bm{};
    if (GetObjectW(ii.hbmColor, sizeof(bm), &bm) == 0 || bm.bmWidth <= 0 || bm.bmHeight <= 0)
    {
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask)  DeleteObject(ii.hbmMask);
        return nullptr;
    }
    const int w = bm.bmWidth, h = bm.bmHeight;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;              // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC hdc = GetDC(nullptr);
    std::vector<uint32_t> pix(static_cast<size_t>(w) * h);
    GetDIBits(hdc, ii.hbmColor, 0, h, pix.data(), &bi, DIB_RGB_COLORS);

    // Detect whether the color bitmap carried any alpha.
    bool hasAlpha = false;
    for (uint32_t px : pix) if ((px >> 24) != 0) { hasAlpha = true; break; }

    if (!hasAlpha && ii.hbmMask)
    {
        // Derive alpha from the AND mask: mask pixel 0 = opaque, nonzero = clear.
        std::vector<uint32_t> mask(static_cast<size_t>(w) * h);
        GetDIBits(hdc, ii.hbmMask, 0, h, mask.data(), &bi, DIB_RGB_COLORS);
        for (size_t i = 0; i < pix.size(); ++i)
        {
            const uint32_t a = (mask[i] & 0x00FFFFFF) ? 0u : 0xFFu;
            pix[i] = (pix[i] & 0x00FFFFFF) | (a << 24);
        }
    }
    ReleaseDC(nullptr, hdc);

    // Premultiply for D2D (B8G8R8A8_UNORM, PREMULTIPLIED).
    for (uint32_t& px : pix)
    {
        const uint32_t a = px >> 24;
        uint32_t b = px & 0xFF, g = (px >> 8) & 0xFF, r = (px >> 16) & 0xFF;
        b = b * a / 255; g = g * a / 255; r = r * a / 255;
        px = (a << 24) | (r << 16) | (g << 8) | b;
    }

    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask)  DeleteObject(ii.hbmMask);

    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.f, 96.f);
    ComPtr<ID2D1Bitmap> bmp;
    if (FAILED(dc->CreateBitmap(D2D1::SizeU(w, h), pix.data(),
            static_cast<UINT32>(w * 4), props, &bmp)))
        return nullptr;
    return bmp;
}
