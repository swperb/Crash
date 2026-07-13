#include "VirtualListView.h"
#include "ThumbnailCache.h"
#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace
{
    constexpr int   kOverscan = 6;
    constexpr float kAutoSpeed = 2600;   // DIP/s for the auto-scroll stress test

    std::wstring JoinPath(const std::wstring& dir, const std::wstring& name)
    {
        if (dir.empty()) return name;
        return (dir.back() == L'\\') ? dir + name : dir + L"\\" + name;
    }

    // File kinds worth requesting a real thumbnail for (photos/video/docs).
    bool Thumbnailable(const FileEntry& e)
    {
        if (e.isFolder || e.isDrive || e.ext.empty()) return false;
        static const std::unordered_set<std::wstring> kExts = {
            L".jpg", L".jpeg", L".png", L".gif", L".bmp", L".webp", L".tif", L".tiff",
            L".heic", L".heif", L".ico", L".svg", L".avif",
            L".mp4", L".mov", L".avi", L".mkv", L".wmv", L".webm", L".m4v", L".mpg", L".mpeg",
            L".pdf", L".psd", L".ai", L".eps", L".cr2", L".nef", L".arw", L".dng", L".raf",
            L".docx", L".pptx", L".xlsx",
        };
        return kExts.count(e.ext) != 0;
    }

    struct Metrics
    {
        float rowH, headerH, nameSize, secSize, headerSize;
        float tileW, tileH, iconSize, gridLabelSize;
    };

    Metrics MetricsFor(Density d)
    {
        if (d == Density::Compact)
            return { 22.f, 26.f, 12.5f, 11.5f, 11.5f, 108.f, 96.f, 40.f, 11.5f };
        return       { 28.f, 30.f, 14.0f, 12.5f, 12.5f, 132.f, 116.f, 48.f, 12.5f };
    }
}

VirtualListView::VirtualListView(FileListModel& model, IDWriteFactory* dwrite,
                                 const Theme& theme, IconCache* icons)
    : model_(model), dwrite_(dwrite), theme_(theme), icons_(icons)
{
    BuildFormats();
}

// ---------------------------------------------------------------- formats ----

void VirtualListView::BuildFormats()
{
    const Metrics m = MetricsFor(density_);
    const wchar_t* kFamily = L"Segoe UI Variable Text";

    auto make = [&](float size, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT ta,
                    DWRITE_PARAGRAPH_ALIGNMENT pa, DWRITE_WORD_WRAPPING wrap,
                    bool ellipsis) -> ComPtr<IDWriteTextFormat>
    {
        ComPtr<IDWriteTextFormat> f;
        HR(dwrite_->CreateTextFormat(kFamily, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f), "CreateTextFormat");
        f->SetTextAlignment(ta);
        f->SetParagraphAlignment(pa);
        f->SetWordWrapping(wrap);
        if (ellipsis)
        {
            DWRITE_TRIMMING trim{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            ComPtr<IDWriteInlineObject> sign;
            if (SUCCEEDED(dwrite_->CreateEllipsisTrimmingSign(f.Get(), &sign)))
                f->SetTrimming(&trim, sign.Get());
        }
        return f;
    };

    fmtName_      = make(m.nameSize,   DWRITE_FONT_WEIGHT_NORMAL,    DWRITE_TEXT_ALIGNMENT_LEADING,  DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP, true);
    fmtNameNum_   = make(m.secSize,    DWRITE_FONT_WEIGHT_NORMAL,    DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP, false);
    fmtSecondary_ = make(m.secSize,    DWRITE_FONT_WEIGHT_NORMAL,    DWRITE_TEXT_ALIGNMENT_LEADING,  DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP, true);
    fmtHeader_    = make(m.headerSize, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING,  DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP, true);
    fmtGridLabel_ = make(m.gridLabelSize, DWRITE_FONT_WEIGHT_NORMAL,DWRITE_TEXT_ALIGNMENT_CENTER,   DWRITE_PARAGRAPH_ALIGNMENT_NEAR,   DWRITE_WORD_WRAPPING_WRAP,    true);
}

void VirtualListView::EnsureBrushes(ID2D1DeviceContext* dc)
{
    if (brushOwner_ == dc && brBg_) return;
    brushOwner_ = dc;
    auto mk = [&](const D2D1_COLOR_F& c) {
        ComPtr<ID2D1SolidColorBrush> b;
        HR(dc->CreateSolidColorBrush(c, &b), "CreateSolidColorBrush");
        return b;
    };
    brBg_           = mk(theme_.windowBg);
    brHeader_       = mk(theme_.headerBg);
    brAlt_          = mk(theme_.rowAlt);
    brHover_        = mk(theme_.rowHover);
    brSel_          = mk(theme_.rowSelected);
    brText_         = mk(theme_.textPrimary);
    brText2_        = mk(theme_.textSecondary);
    brGrid_         = mk(theme_.gridLine);
    brAccent_       = mk(theme_.accent);
    brIconFallback_ = mk(theme_.iconFolder);
}

// --------------------------------------------------------------- geometry ----

void VirtualListView::SetViewport(UINT widthPx, UINT heightPx, float dpi)
{
    widthPx_ = (std::max)(1u, widthPx);
    heightPx_ = (std::max)(1u, heightPx);
    dpi_ = dpi > 0 ? dpi : 96.0f;
    InvalidateLayouts();
    ClampScroll(ComputeLayout());
}

void VirtualListView::SetInsets(float topDip, float bottomDip)
{
    topInset_ = topDip;
    bottomInset_ = bottomDip;
    ClampScroll(ComputeLayout());
}

VirtualListView::Layout VirtualListView::ComputeLayout() const
{
    const Metrics m = MetricsFor(density_);
    Layout L;
    L.viewW = PxToDip(static_cast<float>(widthPx_), dpi_);
    L.viewH = PxToDip(static_cast<float>(heightPx_), dpi_);
    const float bottom = L.viewH - bottomInset_;

    if (mode_ == ViewMode::Details)
    {
        L.headerH = m.headerH;
        L.rowH = m.rowH;
        L.listTop = topInset_ + m.headerH;
        L.listH = (std::max)(0.f, bottom - L.listTop);

        // Columns are responsive: as the pane narrows (e.g. dual-pane) drop the
        // wider columns from the right so Name always stays readable.
        const float pad = 12.f;
        const float iconSz = 16.f;
        L.xIcon = pad;
        L.xName = pad + iconSz + 10.f;
        L.wDate = L.wSize = L.wType = 0.f;
        L.xDate = L.xSize = L.xType = L.viewW;   // offscreen when hidden
        float right = L.viewW - pad;
        if (L.viewW >= 400.f) { L.wDate = colDate_; L.xDate = right - L.wDate; right = L.xDate - 14.f; }
        if (L.viewW >= 280.f) { L.wSize = colSize_; L.xSize = right - L.wSize; right = L.xSize - 14.f; }
        if (L.viewW >= 560.f) { L.wType = colType_; L.xType = right - L.wType; right = L.xType - 18.f; }
        L.wName = (std::max)(60.f, right - L.xName);
    }
    else
    {
        L.listTop = topInset_ + 8.f;
        L.listH = (std::max)(0.f, bottom - L.listTop);
        L.tileW = m.tileW; L.tileH = m.tileH; L.iconSize = m.iconSize;
        L.cols = (std::max)(1, static_cast<int>(L.viewW / L.tileW));
        L.rowH = L.tileH;
    }
    return L;
}

float VirtualListView::ContentHeight(const Layout& L) const
{
    if (mode_ == ViewMode::Details)
        return static_cast<float>(model_.Count()) * L.rowH;
    const int64_t rows = (static_cast<int64_t>(model_.Count()) + L.cols - 1) / L.cols;
    return static_cast<float>(rows) * L.tileH;
}

float VirtualListView::MaxScroll(const Layout& L) const
{
    return (std::max)(0.f, ContentHeight(L) - L.listH);
}

void VirtualListView::ClampScroll(const Layout& L)
{
    const float mx = MaxScroll(L);
    scrollY_ = std::clamp(scrollY_, 0.f, mx);
    targetY_ = std::clamp(targetY_, 0.f, mx);
}

void VirtualListView::InvalidateLayouts() { layoutCache_.clear(); }

IDWriteTextLayout* VirtualListView::NameLayout(size_t row, float maxW, float maxH, IDWriteTextFormat* fmt)
{
    auto it = layoutCache_.find(row);
    if (it != layoutCache_.end()) return it->second.Get();

    const FileEntry& e = model_.At(row);
    ComPtr<IDWriteTextLayout> layout;
    HR(dwrite_->CreateTextLayout(e.name.c_str(), static_cast<UINT32>(e.name.size()),
        fmt, maxW, maxH, &layout), "CreateTextLayout");
    IDWriteTextLayout* raw = layout.Get();
    layoutCache_.emplace(row, std::move(layout));
    return raw;
}

// ------------------------------------------------------------- model hooks ---

void VirtualListView::OnModelReset()
{
    scrollY_ = targetY_ = 0;
    focus_ = anchor_ = -1;
    hoverRow_ = -1;
    autoScroll_ = false;
    sel_.clear();
    InvalidateLayouts();
}

void VirtualListView::OnModelChanged()
{
    InvalidateLayouts();   // indices may have been reordered by a sort
    const int64_t count = static_cast<int64_t>(model_.Count());
    EnsureSelSize();
    if (focus_ >= count) focus_ = count - 1;
    if (hoverRow_ >= count) hoverRow_ = -1;
    ClampScroll(ComputeLayout());
}

void VirtualListView::SetDensity(Density d)
{
    if (d == density_) return;
    density_ = d;
    BuildFormats();
    InvalidateLayouts();
    if (focus_ >= 0) EnsureVisible(focus_);
    ClampScroll(ComputeLayout());
}

void VirtualListView::SetViewMode(ViewMode m)
{
    if (m == mode_) return;
    mode_ = m;
    InvalidateLayouts();
    if (focus_ >= 0) EnsureVisible(focus_);
    ClampScroll(ComputeLayout());
}

void VirtualListView::OnFilterChanged()
{
    // The visible set changed entirely; indices are no longer meaningful.
    InvalidateLayouts();
    sel_.assign(model_.Count(), 0);
    focus_ = anchor_ = hoverRow_ = -1;
    scrollY_ = targetY_ = 0;
    ClampScroll(ComputeLayout());
}

// -------------------------------------------------------------- selection ----

void VirtualListView::EnsureSelSize()
{
    sel_.resize(model_.Count(), 0);   // preserves existing flags, zero-fills new
}

void VirtualListView::ClearSelection()
{
    std::fill(sel_.begin(), sel_.end(), static_cast<char>(0));
}

bool VirtualListView::IsSelected(int64_t r) const
{
    return r >= 0 && r < static_cast<int64_t>(sel_.size()) && sel_[static_cast<size_t>(r)];
}

size_t VirtualListView::SelectedCount() const
{
    size_t n = 0; for (char c : sel_) if (c) ++n; return n;
}

std::vector<size_t> VirtualListView::SelectedIndices() const
{
    std::vector<size_t> out;
    for (size_t i = 0; i < sel_.size(); ++i) if (sel_[i]) out.push_back(i);
    return out;
}

void VirtualListView::SelectSingle(int64_t row)
{
    EnsureSelSize();
    ClearSelection();
    if (row >= 0 && row < static_cast<int64_t>(sel_.size())) sel_[static_cast<size_t>(row)] = 1;
    focus_ = anchor_ = row;
}

void VirtualListView::SetOnlySelection(int64_t row) { SelectSingle(row); }

void VirtualListView::SelectRangeFromAnchor(int64_t row, bool keepExisting)
{
    EnsureSelSize();
    if (anchor_ < 0) anchor_ = row;
    if (!keepExisting) ClearSelection();
    int64_t a = anchor_, b = row;
    if (a > b) std::swap(a, b);
    a = std::max<int64_t>(0, a);
    b = std::min<int64_t>(static_cast<int64_t>(sel_.size()) - 1, b);
    for (int64_t i = a; i <= b; ++i) sel_[static_cast<size_t>(i)] = 1;
    focus_ = row;
}

// ----------------------------------------------------------------- render ----

void VirtualListView::DrawIcon(ID2D1DeviceContext* dc, D2D1_RECT_F box, const FileEntry& e, bool large)
{
    ID2D1Bitmap* bmp = icons_ ? icons_->Get(dc, e, large) : nullptr;
    if (bmp)
    {
        dc->DrawBitmap(bmp, box, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        return;
    }
    // Fallback: a soft rounded square if the shell gave us nothing.
    D2D1_ROUNDED_RECT rr{ box, 3, 3 };
    dc->FillRoundedRectangle(rr, brIconFallback_.Get());
}

void VirtualListView::Render(ID2D1DeviceContext* dc)
{
    EnsureBrushes(dc);
    const Layout L = ComputeLayout();
    dc->Clear(theme_.windowBg);
    if (mode_ == ViewMode::Details) RenderDetails(dc, L);
    else                            RenderGrid(dc, L);

    if (marqueeActive_)   // rubber-band rectangle (content → viewport coords)
    {
        const float x0 = (std::min)(mqAnchorX_, mqCurX_), x1 = (std::max)(mqAnchorX_, mqCurX_);
        const float vy0 = L.listTop + (std::min)(mqAnchorY_, mqCurY_) - scrollY_;
        const float vy1 = L.listTop + (std::max)(mqAnchorY_, mqCurY_) - scrollY_;
        dc->PushAxisAlignedClip({ 0, L.listTop, L.viewW, L.listTop + L.listH }, D2D1_ANTIALIAS_MODE_ALIASED);
        dc->FillRectangle({ x0, vy0, x1, vy1 }, brSel_.Get());
        dc->DrawRectangle({ x0, vy0, x1, vy1 }, brAccent_.Get(), 1.f);
        dc->PopAxisAlignedClip();
    }
}

void VirtualListView::RenderDetails(ID2D1DeviceContext* dc, const Layout& L)
{
    const int64_t count = static_cast<int64_t>(model_.Count());

    D2D1_RECT_F listRect{ 0, L.listTop, L.viewW, L.listTop + L.listH };
    dc->PushAxisAlignedClip(listRect, D2D1_ANTIALIAS_MODE_ALIASED);

    int64_t first = static_cast<int64_t>(std::floor(scrollY_ / L.rowH)) - kOverscan;
    int64_t last  = static_cast<int64_t>(std::floor((scrollY_ + L.listH) / L.rowH)) + kOverscan;
    first = std::max<int64_t>(0, first);
    last  = std::min<int64_t>(count - 1, last);

    firstVisible_ = static_cast<size_t>(std::max<int64_t>(0, static_cast<int64_t>(std::floor(scrollY_ / L.rowH))));
    visibleCount_ = (last >= first) ? static_cast<size_t>(last - first + 1) : 0;

    for (int64_t r = first; r <= last; ++r)
    {
        const float y = L.listTop + static_cast<float>(r) * L.rowH - scrollY_;
        const FileEntry& e = model_.At(static_cast<size_t>(r));

        if (IsSelected(r))          dc->FillRectangle({ 0, y, L.viewW, y + L.rowH }, brSel_.Get());
        else if (r == hoverRow_)    dc->FillRectangle({ 0, y, L.viewW, y + L.rowH }, brHover_.Get());
        else if (r & 1)             dc->FillRectangle({ 0, y, L.viewW, y + L.rowH }, brAlt_.Get());

        if (r == focus_ && SelectedCount() > 1)   // focus marker within a multi-selection
            dc->DrawRectangle({ 0.5f, y + 0.5f, L.viewW - 0.5f, y + L.rowH - 0.5f }, brAccent_.Get(), 1.f);

        D2D1_RECT_F iconBox{ L.xIcon, y + (L.rowH - 16.f) * 0.5f, L.xIcon + 16.f, y + (L.rowH + 16.f) * 0.5f };
        DrawIcon(dc, iconBox, e, false);

        IDWriteTextLayout* nl = NameLayout(static_cast<size_t>(r), L.wName, L.rowH, fmtName_.Get());
        dc->DrawTextLayout({ L.xName, y }, nl, brText_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

        if (L.wType > 0 && !e.typeText.empty())
            dc->DrawText(e.typeText.c_str(), static_cast<UINT32>(e.typeText.size()), fmtSecondary_.Get(),
                { L.xType, y, L.xType + L.wType, y + L.rowH }, brText2_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

        if (L.wSize > 0 && !e.sizeText.empty())
            dc->DrawText(e.sizeText.c_str(), static_cast<UINT32>(e.sizeText.size()), fmtNameNum_.Get(),
                { L.xSize, y, L.xSize + L.wSize, y + L.rowH }, brText2_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

        if (L.wDate > 0 && !e.dateText.empty())
            dc->DrawText(e.dateText.c_str(), static_cast<UINT32>(e.dateText.size()), fmtSecondary_.Get(),
                { L.xDate, y, L.xDate + L.wDate, y + L.rowH }, brText2_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    dc->PopAxisAlignedClip();

    for (auto it = layoutCache_.begin(); it != layoutCache_.end(); )
    {
        const int64_t k = static_cast<int64_t>(it->first);
        if (k < first || k > last) it = layoutCache_.erase(it);
        else ++it;
    }

    // Header (drawn above the rows). Active sort column gets a ▲/▼ indicator.
    dc->FillRectangle({ 0, topInset_, L.viewW, topInset_ + L.headerH }, brHeader_.Get());
    dc->DrawLine({ 0, topInset_ + L.headerH - 0.5f }, { L.viewW, topInset_ + L.headerH - 0.5f }, brGrid_.Get(), 1.f);
    const wchar_t* arrow = sortAsc_ ? L"  \x25B4" : L"  \x25BE";
    auto htext = [&](const wchar_t* s, SortKey key, float x, float w) {
        if (w <= 0) return;
        std::wstring label = s;
        if (sortKey_ == key) label += arrow;
        ID2D1SolidColorBrush* br = (sortKey_ == key) ? brText_.Get() : brText2_.Get();
        dc->DrawText(label.c_str(), static_cast<UINT32>(label.size()), fmtHeader_.Get(),
            { x, topInset_, x + w, topInset_ + L.headerH }, br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };
    htext(L"Name", SortKey::Name, L.xName, L.wName);
    htext(L"Type", SortKey::Type, L.xType, L.wType);
    htext(L"Size", SortKey::Size, L.xSize, L.wSize);
    htext(L"Date modified", SortKey::Date, L.xDate, L.wDate);
}

void VirtualListView::RenderGrid(ID2D1DeviceContext* dc, const Layout& L)
{
    const int64_t count = static_cast<int64_t>(model_.Count());
    const int cols = L.cols;
    const float startX = (L.viewW - cols * L.tileW) * 0.5f;

    D2D1_RECT_F listRect{ 0, L.listTop, L.viewW, L.listTop + L.listH };
    dc->PushAxisAlignedClip(listRect, D2D1_ANTIALIAS_MODE_ALIASED);

    int64_t firstRow = static_cast<int64_t>(std::floor(scrollY_ / L.tileH)) - 1;
    int64_t lastRow  = static_cast<int64_t>(std::floor((scrollY_ + L.listH) / L.tileH)) + 1;
    firstRow = std::max<int64_t>(0, firstRow);

    firstVisible_ = static_cast<size_t>(std::max<int64_t>(0, firstRow) * cols);
    size_t drawn = 0;

    for (int64_t gr = firstRow; gr <= lastRow; ++gr)
    {
        const float ty = L.listTop + static_cast<float>(gr) * L.tileH - scrollY_;
        for (int c = 0; c < cols; ++c)
        {
            const int64_t idx = gr * cols + c;
            if (idx >= count) break;
            const float tx = startX + c * L.tileW;
            const FileEntry& e = model_.At(static_cast<size_t>(idx));

            if (IsSelected(idx) || idx == hoverRow_)
            {
                D2D1_ROUNDED_RECT rr{ { tx + 4, ty + 4, tx + L.tileW - 4, ty + L.tileH - 4 }, 6, 6 };
                dc->FillRoundedRectangle(rr, IsSelected(idx) ? brSel_.Get() : brHover_.Get());
            }

            D2D1_RECT_F iconBox{ tx + (L.tileW - L.iconSize) * 0.5f, ty + 14.f,
                                 tx + (L.tileW + L.iconSize) * 0.5f, ty + 14.f + L.iconSize };
            ID2D1Bitmap* th = (thumbs_ && Thumbnailable(e))
                ? thumbs_->Get(JoinPath(basePath_, e.name), e.mtime, e.sizeBytes) : nullptr;
            if (th)   // real thumbnail: aspect-fit inside the icon box, framed
            {
                D2D1_SIZE_F ts = th->GetSize();
                const float scale = (std::min)((iconBox.right - iconBox.left) / ts.width,
                                               (iconBox.bottom - iconBox.top) / ts.height);
                const float w = ts.width * scale, h = ts.height * scale;
                const float cxp = (iconBox.left + iconBox.right) * 0.5f, cyp = (iconBox.top + iconBox.bottom) * 0.5f;
                D2D1_RECT_F dst{ cxp - w * 0.5f, cyp - h * 0.5f, cxp + w * 0.5f, cyp + h * 0.5f };
                dc->DrawBitmap(th, dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                dc->DrawRectangle(dst, brGrid_.Get(), 1.0f);
            }
            else DrawIcon(dc, iconBox, e, true);

            const float labelTop = ty + 14.f + L.iconSize + 6.f;
            const float labelH = L.tileH - (labelTop - ty) - 6.f;
            IDWriteTextLayout* nl = NameLayout(static_cast<size_t>(idx),
                L.tileW - 12.f, (std::max)(0.f, labelH), fmtGridLabel_.Get());
            dc->DrawTextLayout({ tx + 6.f, labelTop }, nl, brText_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            ++drawn;
        }
    }
    visibleCount_ = drawn;
    dc->PopAxisAlignedClip();

    const int64_t keepFirst = firstRow * cols;
    const int64_t keepLast  = (lastRow + 1) * cols;
    for (auto it = layoutCache_.begin(); it != layoutCache_.end(); )
    {
        const int64_t k = static_cast<int64_t>(it->first);
        if (k < keepFirst || k >= keepLast) it = layoutCache_.erase(it);
        else ++it;
    }
}

// ------------------------------------------------------------------ input ----

int VirtualListView::RowAtPoint(const Layout& L, int xPx, int yPx) const
{
    const float x = PxToDip(static_cast<float>(xPx), dpi_);
    const float y = PxToDip(static_cast<float>(yPx), dpi_);
    if (y < L.listTop || y > L.listTop + L.listH) return -1;

    if (mode_ == ViewMode::Details)
    {
        const int64_t r = static_cast<int64_t>(std::floor((y - L.listTop + scrollY_) / L.rowH));
        if (r < 0 || r >= static_cast<int64_t>(model_.Count())) return -1;
        return static_cast<int>(r);
    }
    const float startX = (L.viewW - L.cols * L.tileW) * 0.5f;
    if (x < startX || x >= startX + L.cols * L.tileW) return -1;
    const int col = static_cast<int>((x - startX) / L.tileW);
    const int64_t gr = static_cast<int64_t>(std::floor((y - L.listTop + scrollY_) / L.tileH));
    const int64_t idx = gr * L.cols + col;
    if (idx < 0 || idx >= static_cast<int64_t>(model_.Count())) return -1;
    return static_cast<int>(idx);
}

int VirtualListView::HeaderHitColumn(int xPx, int yPx) const
{
    if (mode_ != ViewMode::Details) return -1;
    const Layout L = ComputeLayout();
    const float x = PxToDip(static_cast<float>(xPx), dpi_);
    const float y = PxToDip(static_cast<float>(yPx), dpi_);
    if (y < topInset_ || y >= topInset_ + L.headerH) return -1;
    if (L.wType > 0 && x >= L.xType && x < L.xType + L.wType) return static_cast<int>(SortKey::Type);
    if (L.wSize > 0 && x >= L.xSize && x < L.xSize + L.wSize) return static_cast<int>(SortKey::Size);
    if (L.wDate > 0 && x >= L.xDate && x < L.xDate + L.wDate) return static_cast<int>(SortKey::Date);
    if (x >= L.xName) return static_cast<int>(SortKey::Name);
    return -1;
}

int VirtualListView::HeaderDividerAt(int xPx, int yPx) const
{
    if (mode_ != ViewMode::Details) return -1;
    const Layout L = ComputeLayout();
    const float x = PxToDip(static_cast<float>(xPx), dpi_);
    const float y = PxToDip(static_cast<float>(yPx), dpi_);
    if (y < topInset_ || y >= topInset_ + L.headerH) return -1;
    if (L.wType > 0 && std::fabs(x - L.xType) < 5.f) return 0;
    if (L.wSize > 0 && std::fabs(x - L.xSize) < 5.f) return 1;
    if (L.wDate > 0 && std::fabs(x - L.xDate) < 5.f) return 2;
    return -1;
}

void VirtualListView::ResizeColumnTo(int which, int xPx)
{
    const Layout L = ComputeLayout();
    const float x = PxToDip(static_cast<float>(xPx), dpi_);
    const float pad = 12.f, maxW = L.viewW * 0.6f;
    if (which == 2)      colDate_ = std::clamp((L.viewW - pad) - x, 56.f, maxW);
    else if (which == 1) colSize_ = std::clamp((L.xDate - 14.f) - x, 56.f, maxW);
    else if (which == 0) colType_ = std::clamp((L.xSize - 14.f) - x, 56.f, maxW);
    InvalidateLayouts();
}

void VirtualListView::ApplySort()
{
    model_.Sort(sortKey_, sortAsc_);
    InvalidateLayouts();
    // Selection is by index and indices just moved; keep it simple and clear it.
    ClearSelection();
    focus_ = anchor_ = -1;
}

void VirtualListView::FocusTo(int64_t row, bool extend)
{
    if (extend) SelectRangeFromAnchor(row, /*keepExisting*/ false);
    else        SelectSingle(row);
    if (row >= 0) EnsureVisible(row);
}

void VirtualListView::MoveFocus(int64_t delta, bool extend)
{
    const int64_t count = static_cast<int64_t>(model_.Count());
    if (count == 0) return;
    int64_t s = (focus_ < 0) ? 0 : std::clamp<int64_t>(focus_ + delta, 0, count - 1);
    FocusTo(s, extend);
}

void VirtualListView::EnsureVisible(int64_t row)
{
    const Layout L = ComputeLayout();
    float top, h;
    if (mode_ == ViewMode::Details) { top = row * L.rowH; h = L.rowH; }
    else { top = (row / L.cols) * L.tileH; h = L.tileH; }

    if (top < targetY_)                     targetY_ = top;
    else if (top + h > targetY_ + L.listH)  targetY_ = top + h - L.listH;
    targetY_ = std::clamp(targetY_, 0.f, MaxScroll(L));
}

bool VirtualListView::OnWheel(int wheelDelta)
{
    const Layout L = ComputeLayout();
    targetY_ -= (static_cast<float>(wheelDelta) / WHEEL_DELTA) * 3.f * L.rowH;
    targetY_ = std::clamp(targetY_, 0.f, MaxScroll(L));
    return true;
}

bool VirtualListView::OnKey(WPARAM key)
{
    Layout L = ComputeLayout();
    const int64_t count = static_cast<int64_t>(model_.Count());
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const int64_t pageRows = (mode_ == ViewMode::Details)
        ? std::max<int64_t>(1, static_cast<int64_t>(L.listH / L.rowH) - 1)
        : std::max<int64_t>(1, (static_cast<int64_t>(L.listH / L.tileH)) * L.cols);
    const int64_t cols = (mode_ == ViewMode::Grid) ? L.cols : 1;

    switch (key)
    {
    case 'G':
        mode_ = (mode_ == ViewMode::Details) ? ViewMode::Grid : ViewMode::Details;
        InvalidateLayouts(); ClampScroll(ComputeLayout());
        if (focus_ >= 0) EnsureVisible(focus_);
        return true;
    case 'D':
        density_ = (density_ == Density::Comfortable) ? Density::Compact : Density::Comfortable;
        BuildFormats(); InvalidateLayouts(); ClampScroll(ComputeLayout());
        return true;
    case 'S': autoScroll_ = !autoScroll_; return true;
    case 'A':
        if (ctrl) { EnsureSelSize(); std::fill(sel_.begin(), sel_.end(), static_cast<char>(1));
                    if (focus_ < 0 && count > 0) focus_ = 0; return true; }
        return false;
    case VK_RETURN:
        if (focus_ >= 0 && focus_ < count && onActivate_) onActivate_(static_cast<size_t>(focus_));
        return true;
    case VK_UP:    MoveFocus(-cols, shift); return true;
    case VK_DOWN:  MoveFocus(+cols, shift); return true;
    case VK_LEFT:  if (mode_ == ViewMode::Grid) MoveFocus(-1, shift); return true;
    case VK_RIGHT: if (mode_ == ViewMode::Grid) MoveFocus(+1, shift); return true;
    case VK_PRIOR: MoveFocus(-pageRows, shift); return true;
    case VK_NEXT:  MoveFocus(+pageRows, shift); return true;
    case VK_HOME:  if (count > 0) FocusTo(0, shift); return true;
    case VK_END:   if (count > 0) FocusTo(count - 1, shift); return true;
    default: return false;
    }
}

bool VirtualListView::OnMouseMove(int xPx, int yPx)
{
    lastMouseXPx_ = xPx; lastMouseYPx_ = yPx;
    const int64_t r = RowAtPoint(ComputeLayout(), xPx, yPx);
    if (r != hoverRow_) { hoverRow_ = r; return true; }
    return false;
}

bool VirtualListView::OnMouseLeave()
{
    if (hoverRow_ != -1) { hoverRow_ = -1; return true; }
    return false;
}

bool VirtualListView::OnLButtonDown(int xPx, int yPx)
{
    // Clicking a details header sorts by that column (toggling direction).
    const int col = HeaderHitColumn(xPx, yPx);
    if (col >= 0)
    {
        const SortKey key = static_cast<SortKey>(col);
        if (key == sortKey_) sortAsc_ = !sortAsc_;
        else { sortKey_ = key; sortAsc_ = true; }
        ApplySort();
        return true;
    }

    const Layout L = ComputeLayout();
    const int64_t r = RowAtPoint(L, xPx, yPx);
    const bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

    if (r < 0)   // empty list area → begin marquee (unless it's the header band)
    {
        const float xd = PxToDip(static_cast<float>(xPx), dpi_);
        const float yd = PxToDip(static_cast<float>(yPx), dpi_);
        if (yd < L.listTop || yd > L.listTop + L.listH) { ClearSelection(); focus_ = anchor_ = -1; return true; }
        if (!ctrl) { ClearSelection(); focus_ = anchor_ = -1; }
        EnsureSelSize(); baseSel_ = sel_;
        marqueeActive_ = true;
        mqAnchorX_ = xd; mqAnchorY_ = (yd - L.listTop) + scrollY_;
        mqCurX_ = mqAnchorX_; mqCurY_ = mqAnchorY_;
        mqLastYPx_ = yPx;
        return true;
    }

    if (shift)      SelectRangeFromAnchor(r, /*keepExisting*/ ctrl);
    else if (ctrl)  { EnsureSelSize(); sel_[static_cast<size_t>(r)] ^= 1; focus_ = anchor_ = r; }
    else            SelectSingle(r);
    return true;
}

void VirtualListView::MarqueeMove(int xPx, int yPx)
{
    if (!marqueeActive_) return;
    const Layout L = ComputeLayout();
    mqCurX_ = PxToDip(static_cast<float>(xPx), dpi_);
    mqCurY_ = (PxToDip(static_cast<float>(yPx), dpi_) - L.listTop) + scrollY_;
    mqLastYPx_ = yPx;
    UpdateMarqueeSelection();
}

void VirtualListView::UpdateMarqueeSelection()
{
    EnsureSelSize();
    sel_ = baseSel_;
    const Layout L = ComputeLayout();
    const float x0 = (std::min)(mqAnchorX_, mqCurX_), x1 = (std::max)(mqAnchorX_, mqCurX_);
    const float y0 = (std::min)(mqAnchorY_, mqCurY_), y1 = (std::max)(mqAnchorY_, mqCurY_);
    const int64_t count = static_cast<int64_t>(model_.Count());

    if (mode_ == ViewMode::Details)
    {
        int64_t first = (int64_t)std::floor(y0 / L.rowH), last = (int64_t)std::floor(y1 / L.rowH);
        first = std::max<int64_t>(0, first); last = std::min<int64_t>(count - 1, last);
        for (int64_t i = first; i <= last; ++i) sel_[(size_t)i] = 1;
    }
    else
    {
        const float startX = (L.viewW - L.cols * L.tileW) * 0.5f;
        int64_t firstRow = std::max<int64_t>(0, (int64_t)std::floor(y0 / L.tileH));
        int64_t lastRow = (int64_t)std::floor(y1 / L.tileH);
        for (int64_t gr = firstRow; gr <= lastRow; ++gr)
            for (int c = 0; c < L.cols; ++c)
            {
                const int64_t idx = gr * L.cols + c;
                if (idx >= count) break;
                const float tx = startX + c * L.tileW;
                if (x0 < tx + L.tileW && x1 > tx) sel_[(size_t)idx] = 1;   // y already in band
            }
    }
}

bool VirtualListView::OnDoubleClick(int xPx, int yPx)
{
    if (HeaderHitColumn(xPx, yPx) >= 0) return false;   // header handled on down
    const int64_t r = RowAtPoint(ComputeLayout(), xPx, yPx);
    if (r >= 0)
    {
        SelectSingle(r);
        if (onActivate_) onActivate_(static_cast<size_t>(r));
        return true;
    }
    return false;
}

int VirtualListView::HitRow(int xPx, int yPx) const
{
    return RowAtPoint(ComputeLayout(), xPx, yPx);
}

void VirtualListView::SetSelected(int64_t row) { SelectSingle(row); }

bool VirtualListView::FocusRowRectDip(D2D1_RECT_F* out) const
{
    if (focus_ < 0 || focus_ >= static_cast<int64_t>(model_.Count())) return false;
    const Layout L = ComputeLayout();
    if (mode_ == ViewMode::Details)
    {
        const float y = L.listTop + focus_ * L.rowH - scrollY_;
        if (y + L.rowH < L.listTop || y > L.listTop + L.listH) return false;
        *out = { L.xName, y, L.xName + L.wName, y + L.rowH };
        return true;
    }
    const int64_t gr = focus_ / L.cols, c = focus_ % L.cols;
    const float startX = (L.viewW - L.cols * L.tileW) * 0.5f;
    const float tx = startX + c * L.tileW, ty = L.listTop + gr * L.tileH - scrollY_;
    if (ty + L.tileH < L.listTop || ty > L.listTop + L.listH) return false;
    const float labelTop = ty + 14.f + L.iconSize + 6.f;
    *out = { tx + 6.f, labelTop, tx + L.tileW - 6.f, ty + L.tileH - 4.f };
    return true;
}

bool VirtualListView::SelectedAnchorPx(POINT* pt) const
{
    if (focus_ < 0 || focus_ >= static_cast<int64_t>(model_.Count())) return false;
    const Layout L = ComputeLayout();
    float xDip, yDip;
    if (mode_ == ViewMode::Details)
    {
        float yTop = L.listTop + focus_ * L.rowH - scrollY_;
        yTop = std::clamp(yTop, L.listTop, L.listTop + L.listH - L.rowH);
        xDip = L.xName;
        yDip = yTop + L.rowH;
    }
    else
    {
        const int64_t gr = focus_ / L.cols, c = focus_ % L.cols;
        const float startX = (L.viewW - L.cols * L.tileW) * 0.5f;
        float yTop = L.listTop + gr * L.tileH - scrollY_;
        yTop = std::clamp(yTop, L.listTop, L.listTop + (std::max)(0.f, L.listH - L.tileH));
        xDip = startX + c * L.tileW + L.tileW * 0.5f;
        yDip = yTop + L.tileH * 0.5f;
    }
    pt->x = static_cast<LONG>(DipToPx(xDip, dpi_));
    pt->y = static_cast<LONG>(DipToPx(yDip, dpi_));
    return true;
}

// -------------------------------------------------------------- animation ----

bool VirtualListView::Animate(double dtSeconds)
{
    const float dt = static_cast<float>(std::clamp(dtSeconds, 0.0, 0.05));
    const Layout L = ComputeLayout();
    const float mx = MaxScroll(L);

    if (marqueeActive_)   // auto-scroll when the drag reaches the top/bottom edge
    {
        const float ydip = PxToDip(static_cast<float>(mqLastYPx_), dpi_);
        const float edge = 26.f;
        float delta = 0.f;
        if (ydip < L.listTop + edge) delta = -(L.listTop + edge - ydip);
        else if (ydip > L.listTop + L.listH - edge) delta = (ydip - (L.listTop + L.listH - edge));
        if (delta != 0.f)
        {
            scrollY_ = std::clamp(scrollY_ + delta * dt * 6.f, 0.f, mx);
            targetY_ = scrollY_;
            mqCurY_ = (ydip - L.listTop) + scrollY_;
            UpdateMarqueeSelection();
        }
        return true;
    }

    if (autoScroll_)
    {
        scrollY_ += autoDir_ * kAutoSpeed * dt;
        if (scrollY_ >= mx) { scrollY_ = mx; autoDir_ = -1.f; }
        else if (scrollY_ <= 0) { scrollY_ = 0; autoDir_ = 1.f; }
        targetY_ = scrollY_;
        if (lastMouseXPx_ >= 0) hoverRow_ = RowAtPoint(L, lastMouseXPx_, lastMouseYPx_);
        return true;
    }

    const float diff = targetY_ - scrollY_;
    if (std::fabs(diff) < 0.25f) { scrollY_ = targetY_; return false; }
    scrollY_ += diff * (1.f - std::exp(-dt * 22.f));
    scrollY_ = std::clamp(scrollY_, 0.f, mx);
    return true;
}
