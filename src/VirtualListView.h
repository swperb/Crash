// VirtualListView — the core differentiator (design doc §6.1).
//
//   * Custom Direct2D control; renders only visible rows + a small overscan
//     buffer with NO per-item object graph — items are indices into the model.
//   * Details and Grid are two render modes of the *same* model.
//   * Text via IDWriteTextLayout cached per visible row (invalidated on
//     resize / mode / density change); icons via the system IconCache.
//   * Smooth scrolling, keyboard selection, hover, and folder/file activation.
//
// Phase 1 points it at the live FileListModel fed by the async enumerator.
// Input coordinates are physical pixels; internal layout is in DIPs. The list
// occupies the window minus top/bottom chrome insets (toolbar / status bar).
#pragma once
#include "common.h"
#include "FileEntry.h"
#include "IconCache.h"
#include "Theme.h"
#include <functional>
#include <unordered_map>
#include <vector>

enum class ViewMode { Details, Grid };

class ThumbnailCache;

class VirtualListView
{
public:
    VirtualListView(FileListModel& model, IDWriteFactory* dwrite,
                    const Theme& theme, IconCache* icons);

    void SetViewport(UINT widthPx, UINT heightPx, float dpi);
    void SetInsets(float topDip, float bottomDip);   // chrome reserved space
    void SetThumbnailCache(ThumbnailCache* t) { thumbs_ = t; }
    void SetBasePath(const std::wstring& p) { basePath_ = p; }   // folder being shown
    void Render(ID2D1DeviceContext* dc);

    void SetOnActivate(std::function<void(size_t)> cb) { onActivate_ = std::move(cb); }

    // Call when the backing model changes (navigation / new batch / sort).
    void OnModelReset();     // new directory: clear selection + scroll
    void OnModelChanged();   // rows appended/sorted: keep selection valid

    // Input (pixels). Return true if a redraw is needed.
    bool OnWheel(int wheelDelta);
    bool OnKey(WPARAM key);
    bool OnMouseMove(int xPx, int yPx);
    bool OnMouseLeave();
    bool OnLButtonDown(int xPx, int yPx);
    bool OnDoubleClick(int xPx, int yPx);

    // Marquee (rubber-band) selection — driven by main while the mouse is captured.
    bool MarqueeActive() const { return marqueeActive_; }
    void MarqueeMove(int xPx, int yPx);
    void EndMarquee() { marqueeActive_ = false; }

    // Context-menu support: hit-test a point to a row, force a selection, and
    // find a client-space anchor for the keyboard menu key.
    int  HitRow(int xPx, int yPx) const;
    void SetSelected(int64_t row);
    bool SelectedAnchorPx(POINT* outClientPx) const;
    bool FocusRowRectDip(D2D1_RECT_F* outLocalDip) const;   // name cell of focused row

    // Resizable Details columns. HeaderDividerAt returns 0=Type,1=Size,2=Date, else -1.
    int  HeaderDividerAt(int xPx, int yPx) const;
    void ResizeColumnTo(int which, int xPx);

    bool Animate(double dtSeconds);

    // State (status bar / overlay).
    ViewMode Mode() const { return mode_; }
    bool     AutoScroll() const { return autoScroll_; }
    size_t   VisibleCount() const { return visibleCount_; }
    size_t   LayoutCacheSize() const { return layoutCache_.size(); }
    const wchar_t* ModeName() const { return mode_ == ViewMode::Details ? L"Details" : L"Grid"; }
    const wchar_t* DensityName() const { return density_ == Density::Comfortable ? L"Comfortable" : L"Compact"; }

    // Selection (multi-select).
    int64_t  Focus() const { return focus_; }
    bool     IsSelected(int64_t r) const;
    size_t   SelectedCount() const;
    std::vector<size_t> SelectedIndices() const;
    void     SetOnlySelection(int64_t r);

    // Sorting.
    SortKey  GetSortKey() const { return sortKey_; }
    bool     SortAscending() const { return sortAsc_; }
    void     ApplySort();   // (re)sort the model by the current key/direction
    void     SetSort(SortKey k, bool asc) { sortKey_ = k; sortAsc_ = asc; ApplySort(); }

    // Filtering: call after the model's filter changes (resets selection/scroll).
    void     OnFilterChanged();

    // Settings-driven setters (applied live).
    void     SetDensity(Density d);
    void     SetViewMode(ViewMode m);
    void     SetTheme(const Theme& t) { theme_ = t; brushOwner_ = nullptr; }

private:
    struct Layout
    {
        float viewW = 0, viewH = 0;
        float headerH = 0, rowH = 0, listTop = 0, listH = 0;
        float xIcon = 0, xName = 0, wName = 0, xType = 0, wType = 0;
        float xSize = 0, wSize = 0, xDate = 0, wDate = 0;
        int   cols = 1;
        float tileW = 0, tileH = 0, iconSize = 0;
    };

    void  BuildFormats();
    void  EnsureBrushes(ID2D1DeviceContext* dc);
    Layout ComputeLayout() const;
    float ContentHeight(const Layout& L) const;
    float MaxScroll(const Layout& L) const;
    void  ClampScroll(const Layout& L);
    void  InvalidateLayouts();
    IDWriteTextLayout* NameLayout(size_t row, float maxW, float maxH, IDWriteTextFormat* fmt);

    void RenderDetails(ID2D1DeviceContext* dc, const Layout& L);
    void RenderGrid(ID2D1DeviceContext* dc, const Layout& L);
    void DrawIcon(ID2D1DeviceContext* dc, D2D1_RECT_F box, const FileEntry& e, bool large);

    int  RowAtPoint(const Layout& L, int xPx, int yPx) const;
    int  HeaderHitColumn(int xPx, int yPx) const;   // -1, else (int)SortKey
    void UpdateMarqueeSelection();
    void MoveFocus(int64_t delta, bool extend);
    void FocusTo(int64_t row, bool extend);
    void EnsureVisible(int64_t row);
    void EnsureSelSize();
    void ClearSelection();
    void SelectSingle(int64_t row);
    void SelectRangeFromAnchor(int64_t row, bool keepExisting);

    FileListModel&  model_;
    IDWriteFactory* dwrite_;
    Theme           theme_;
    IconCache*      icons_;
    ThumbnailCache* thumbs_ = nullptr;
    std::wstring    basePath_;

    ViewMode mode_ = ViewMode::Details;
    Density  density_ = Density::Comfortable;

    UINT  widthPx_ = 1, heightPx_ = 1;
    float dpi_ = 96.0f;
    float topInset_ = 0, bottomInset_ = 0;

    float scrollY_ = 0, targetY_ = 0;
    bool  autoScroll_ = false;
    float autoDir_ = 1.0f;

    int64_t hoverRow_ = -1;
    std::vector<char> sel_;      // per-index selected flag (bounded to model size)
    int64_t focus_ = -1;         // current item (keyboard / activation target)
    int64_t anchor_ = -1;        // range-selection anchor
    int     lastMouseXPx_ = -1, lastMouseYPx_ = -1;

    // Marquee state (anchor/current in CONTENT dip so it survives scrolling).
    bool  marqueeActive_ = false;
    float mqAnchorX_ = 0, mqAnchorY_ = 0, mqCurX_ = 0, mqCurY_ = 0;
    int   mqLastYPx_ = 0;
    std::vector<char> baseSel_;  // selection snapshot at marquee start

    SortKey sortKey_ = SortKey::Name;
    bool    sortAsc_ = true;
    float   colType_ = 150.f, colSize_ = 96.f, colDate_ = 140.f;   // resizable widths

    size_t firstVisible_ = 0, visibleCount_ = 0;

    std::function<void(size_t)> onActivate_;

    ComPtr<IDWriteTextFormat> fmtName_, fmtNameNum_, fmtSecondary_, fmtHeader_, fmtGridLabel_;
    std::unordered_map<size_t, ComPtr<IDWriteTextLayout>> layoutCache_;

    ID2D1DeviceContext* brushOwner_ = nullptr;
    ComPtr<ID2D1SolidColorBrush> brBg_, brHeader_, brAlt_, brHover_, brSel_,
        brText_, brText2_, brGrid_, brAccent_, brIconFallback_;
};
