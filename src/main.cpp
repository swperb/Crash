// Crash — Phase 2: tabs + dual-pane (design doc §6.5). Tabs and the dual-pane
// split are layout state, not separate windows: each Tab owns its own
// navigation history, model, and virtualized view; each Pane owns a set of tabs
// and one enumeration worker. The window hosts one or two panes side by side.
// Session (panes + tabs) is persisted to %LOCALAPPDATA%\Crash\session.json.
//
// Controls: Ctrl+T new tab · Ctrl+W close tab · Ctrl+Tab next tab ·
//   F6/Tab switch pane · dual-pane toggle button (or F8) · double-click/Enter
//   open · Backspace/↑ up · Alt+←/→ back/forward · click breadcrumb / Ctrl+L
//   edit path · click header sort · Ctrl/Shift-click multi-select ·
//   right-click shell menu · G view · D density · V vsync · Esc quit.
#include "common.h"
#include "GraphicsDevice.h"
#include "VirtualListView.h"
#include "FileEntry.h"
#include "IconCache.h"
#include "Enumerator.h"
#include "ShellContextMenu.h"
#include "ThumbnailCache.h"
#include "DragDrop.h"
#include "Session.h"
#include "Settings.h"
#include "License.h"
#include "Theme.h"

#include <windowsx.h>
#include <shlobj.h>
#include <shellapi.h>
#include <commdlg.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#ifndef DWMSBT_MAINWINDOW
#define DWMSBT_MAINWINDOW 2
#endif

// ============================================================ model + state ==

struct Tab
{
    std::wstring path;                       // "" == This PC
    std::vector<std::wstring> back, forward;
    std::unique_ptr<FileListModel> model;
    std::unique_ptr<VirtualListView> view;
    bool loading = false, loaded = false;
    bool searchMode = false;          // showing recursive-search results
    uint64_t gen = 0;
    std::wstring error;
};

struct Pane
{
    std::vector<std::unique_ptr<Tab>> tabs;
    size_t active = 0;
    std::unique_ptr<Enumerator> enumr;
};

namespace
{
    GraphicsDevice             g_gfx;
    Theme                      g_theme;
    std::unique_ptr<IconCache> g_icons;
    std::unique_ptr<ThumbnailCache> g_thumbs;
    ShellContextMenu           g_ctx;
    HWND                       g_hwnd = nullptr;

    Pane      g_pane[2];
    int       g_activePane = 0;
    bool      g_dual = false;
    uint64_t  g_gen = 0;
    float     g_splitRatio = 0.5f;       // dual-pane divider position (0..1)
    bool      g_draggingSplit = false;
    int       g_listCapture = -1;        // pane whose view is capturing (marquee)
    int       g_colResizePane = -1, g_colResizeWhich = 0;   // Details column resize
    HCURSOR   g_curSizeWE = nullptr;
    constexpr float kMinPaneW = 220.f;

    // Drag-source arming (start an OLE drag once the mouse moves past a threshold).
    bool g_dragArm = false;
    int  g_dragStartX = 0, g_dragStartY = 0, g_dragPane = 0;

    bool g_vsync = true;
    bool g_dirty = true;
    bool g_mouseTracked = false;
    int  g_hoverButton = 0;                      // 0 none, 1 back, 2 fwd, 3 up, 4 dual
    struct { int pane = -1, tab = -1; } g_hoverTab;

    bool g_pro = false;                          // Pro (open-core) unlocked
    std::wstring g_home;

    // Native-Explorer chrome: a left navigation sidebar and a right details pane
    // inset the content region; a distinct Home landing page shows when a tab is
    // at the root (empty path).
    constexpr float kSidebarW = 240.f;
    constexpr float kDetailsW = 300.f;
    bool g_sidebar = true;
    bool g_details = true;
    int  g_hoverNav = -1;                         // hovered sidebar row
    int  g_hoverCard = -1;                        // hovered Home card

    // Expandable navigation tree (sidebar). kind: 0 folder, 2 drive, 3 Home,
    // 4 This PC, 5 separator. Children (drives / subdirectories) load lazily.
    struct NavNode
    {
        std::wstring label, path;
        int  kind = 0;
        bool expandable = false, expanded = false, loaded = false;
        std::vector<NavNode> children;
    };
    struct NavRow { NavNode* node = nullptr; int depth = 0; };
    std::vector<NavNode>     g_navRoots;
    std::vector<NavRow>      g_navRows;            // flattened visible tree (rebuilt on toggle)
    std::vector<D2D1_RECT_F> g_navRects, g_navChevRects;   // parallel g_navRows, rebuilt per draw
    float g_navScroll = 0.f, g_navMaxScroll = 0.f;

    std::vector<std::pair<std::wstring, std::wstring>> g_quickAccess;  // Home cards: (label, path)
    std::vector<std::pair<std::wstring, std::wstring>> g_drives;       // (label, root)

    std::vector<D2D1_RECT_F>  g_homeCardRects;    // parallels the Home cards, rebuilt per draw
    std::vector<std::wstring> g_homeCardPaths;    // path each Home card navigates to

    // Shell parsing names for the virtual-location icons (Home / This PC).
    const wchar_t* kHomeParse   = L"shell:::{f874310e-b6b7-47dc-bc84-b9e6b38f5903}";
    const wchar_t* kThisPCParse = L"shell:::{20D04FE0-3AEA-1069-A2D8-08002B30309D}";

    // Command palette / search overlay (§6.6).
    struct PalItem { int cmd; std::wstring title, cat; int score; };
    struct PaletteState
    {
        bool open = false;
        int  mode = 0;                            // 0 command, 1 filter, 2 license
        std::wstring input;
        size_t caret = 0;
        int  sel = 0;
        std::wstring message;
        std::vector<PalItem> items;
        std::vector<D2D1_RECT_F> itemRects;       // hit rects (DIP), rebuilt per draw
        D2D1_RECT_F panelRect{}, filterClearRect{};
    };
    PaletteState g_pal;
    bool g_searchArmed = false;   // filter box is in recursive-search mode

    AppSettings g_settings;
    bool g_settingsOpen = false;
    struct SettHit { D2D1_RECT_F rect; int action; int value; };
    std::vector<SettHit> g_settHits;
    D2D1_RECT_F g_settPanelRect{};

    // Overlay reveal animation (§4 motion).
    float g_overlayAnim = 0.f;      // 0..1
    int   g_overlayKind = 0;        // 1 palette, 2 settings (valid while anim > 0)

    bool AnimationsEnabled()
    {
        if (!g_settings.animations) return false;
        BOOL ca = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &ca, 0);   // reduced-motion
        return ca != FALSE;
    }

    constexpr float kTabStripH = 34.f;

    std::vector<float> g_pacingMs; size_t g_pacingIdx = 0;
    constexpr size_t kSamples = 180;
    void PushPacing(float v) { if (g_pacingMs.size() < kSamples) g_pacingMs.push_back(v); else { g_pacingMs[g_pacingIdx] = v; g_pacingIdx = (g_pacingIdx + 1) % kSamples; } }
    float AvgPacing() { if (g_pacingMs.empty()) return 0; double s = 0; for (float v : g_pacingMs) s += v; return float(s / g_pacingMs.size()); }

    // ---- path helpers ----
    std::wstring Join(const std::wstring& dir, const std::wstring& name)
    {
        if (dir.empty()) return name;
        return (dir.back() == L'\\') ? dir + name : dir + L"\\" + name;
    }
    std::wstring ParentOf(const std::wstring& p)
    {
        if (p.empty()) return L"";
        if (p == kThisPC) return L"";                                   // This PC → Home
        if (p.size() == 3 && p[1] == L':' && p[2] == L'\\') return kThisPC;   // drive root → This PC
        std::wstring s = p;
        if (s.back() == L'\\') s.pop_back();
        const size_t bs = s.find_last_of(L'\\');
        if (bs == std::wstring::npos) return L"";
        if (bs == 2 && s[1] == L':') return s.substr(0, 3);
        return s.substr(0, bs);
    }
    std::wstring TabTitle(const std::wstring& p)
    {
        if (p.empty()) return L"Home";
        if (p == kThisPC) return L"This PC";
        if (p.size() == 3 && p[1] == L':' && p[2] == L'\\') return p.substr(0, 2);
        std::wstring s = p;
        if (s.back() == L'\\') s.pop_back();
        const size_t bs = s.find_last_of(L'\\');
        return (bs == std::wstring::npos) ? s : s.substr(bs + 1);
    }

    Pane& AP() { return g_pane[g_activePane]; }
    Tab&  AT() { Pane& p = AP(); return *p.tabs[p.active]; }
    int   VisiblePanes() { return g_dual ? 2 : 1; }
}

// ============================================================= chrome (UI) ==

struct ChromeSeg { std::wstring label, path; };

struct ChromeState
{
    std::wstring statusLeft, statusRight;
    bool backEnabled = false, fwdEnabled = false, upEnabled = false, dualOn = false, detailsOn = false;
    int hoverButton = 0;
    std::vector<ChromeSeg> segments;
    bool editing = false;
    std::wstring editText;
    size_t caret = 0;
    bool selAll = false;
    bool caretOn = true;
};

struct AddressEditor
{
    bool active = false;
    std::wstring text;
    size_t caret = 0;
    bool selAll = false;

    void Begin(const std::wstring& t) { active = true; text = t; caret = t.size(); selAll = true; }
    void Cancel() { active = false; selAll = false; }
    void ClearIfSel() { if (selAll) { text.clear(); caret = 0; selAll = false; } }
    void Char(wchar_t c) { if (c < 0x20) return; ClearIfSel(); text.insert(caret, 1, c); ++caret; }
    void Backspace() { if (selAll) { ClearIfSel(); return; } if (caret > 0) { text.erase(caret - 1, 1); --caret; } }
    void Del() { if (selAll) { ClearIfSel(); return; } if (caret < text.size()) text.erase(caret, 1); }
    void Left() { selAll = false; if (caret > 0) --caret; }
    void Right() { selAll = false; if (caret < text.size()) ++caret; }
    void Home() { selAll = false; caret = 0; }
    void End() { selAll = false; caret = text.size(); }
    void SelectAll() { selAll = true; caret = text.size(); }
    void Paste(HWND h)
    {
        if (!OpenClipboard(h)) return;
        if (HANDLE d = GetClipboardData(CF_UNICODETEXT))
            if (const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(d)))
            {
                std::wstring s = p; GlobalUnlock(d);
                ClearIfSel();
                for (wchar_t c : s) if (c >= 0x20) { text.insert(caret, 1, c); ++caret; }
            }
        CloseClipboard();
    }
};
AddressEditor g_addr;

// Inline rename (F2) reuses the same small text editor.
AddressEditor g_ren;
bool g_renaming = false;
int  g_renamePane = 0;
D2D1_RECT_F g_renameRectDip{};
std::wstring g_renameOldPath;

class Chrome
{
public:
    static constexpr float kToolbarH = 48.f;
    static constexpr float kStatusH = 26.f;
    static constexpr float kAddrLeft = 160.f;   // after 4 toolbar buttons

    void Init(IDWriteFactory* dw)
    {
        dwrite_ = dw;
        auto mk = [&](const wchar_t* fam, float size, DWRITE_TEXT_ALIGNMENT ta, bool ellip) {
            ComPtr<IDWriteTextFormat> f;
            HR(dw->CreateTextFormat(fam, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f), "chrome fmt");
            f->SetTextAlignment(ta);
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            if (ellip) { DWRITE_TRIMMING trim{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 }; ComPtr<IDWriteInlineObject> sign; if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(f.Get(), &sign))) f->SetTrimming(&trim, sign.Get()); }
            return f;
        };
        glyph_ = mk(L"Segoe UI", 16.f, DWRITE_TEXT_ALIGNMENT_CENTER, false);
        path_ = mk(L"Segoe UI Variable Text", 13.f, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        status_ = mk(L"Segoe UI", 12.f, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        statusR_ = mk(L"Segoe UI", 12.f, DWRITE_TEXT_ALIGNMENT_TRAILING, true);
    }

    static D2D1_RECT_F Btn(int i) { float x = 8.f + i * 36.f; return { x, 8.f, x + 32.f, 40.f }; }
    static D2D1_RECT_F GearRect(float viewW) { return { viewW - 44.f, 8.f, viewW - 12.f, 40.f }; }
    static D2D1_RECT_F DetailsRect(float viewW) { return { viewW - 88.f, 8.f, viewW - 56.f, 40.f }; }
    int HitButton(float x, float y) const
    {
        for (int i = 0; i < 4; ++i) { auto r = Btn(i); if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) return i + 1; }
        auto g = GearRect(lastViewW_);
        if (x >= g.left && x <= g.right && y >= g.top && y <= g.bottom) return 5;   // settings
        auto d = DetailsRect(lastViewW_);
        if (x >= d.left && x <= d.right && y >= d.top && y <= d.bottom) return 6;    // details toggle
        return 0;
    }
    void InvalidateBrushes() { owner_ = nullptr; }
    int HitAddress(float x, float y, std::wstring* outPath) const
    {
        if (x < lastAddr_.left || x > lastAddr_.right || y < lastAddr_.top || y > lastAddr_.bottom) return 0;
        for (size_t i = 0; i < segRects_.size(); ++i) { const auto& r = segRects_[i]; if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) { if (outPath) *outPath = segPaths_[i]; return 1; } }
        return 2;
    }
    float Measure(const std::wstring& s)
    {
        if (s.empty() || !dwrite_) return 0.f;
        ComPtr<IDWriteTextLayout> l;
        if (FAILED(dwrite_->CreateTextLayout(s.c_str(), (UINT32)s.size(), path_.Get(), 1e5f, 100.f, &l))) return 0.f;
        DWRITE_TEXT_METRICS m{}; l->GetMetrics(&m); return m.widthIncludingTrailingWhitespace;
    }

    void Draw(ID2D1DeviceContext* dc, float viewW, float viewH, const ChromeState& s)
    {
        lastViewW_ = viewW;
        if (owner_ != dc)
        {
            owner_ = dc;
            auto b = [&](const D2D1_COLOR_F& c) { ComPtr<ID2D1SolidColorBrush> br; HR(dc->CreateSolidColorBrush(c, &br), "chrome brush"); return br; };
            brToolbar_ = b(g_theme.toolbarBg); brStatus_ = b(g_theme.statusBg);
            brCtrl_ = b(g_theme.controlBg); brHover_ = b(g_theme.controlHover); brActive_ = b(g_theme.controlActive);
            brText_ = b(g_theme.textPrimary); brText2_ = b(g_theme.textSecondary);
            brDisabled_ = b(g_theme.controlDisabled); brLine_ = b(g_theme.gridLine); brSel_ = b(g_theme.rowSelected);
        }

        dc->FillRectangle({ 0, 0, viewW, kToolbarH }, brToolbar_.Get());
        const wchar_t* glyphs[3] = { L"\x2190", L"\x2192", L"\x2191" };
        const bool en[4] = { s.backEnabled, s.fwdEnabled, s.upEnabled, true };
        for (int i = 0; i < 4; ++i)
        {
            D2D1_RECT_F r = Btn(i);
            const bool on = (i == 3 && s.dualOn);
            if ((en[i] && s.hoverButton == i + 1) || on)
                dc->FillRoundedRectangle({ r, 5, 5 }, on ? brActive_.Get() : brHover_.Get());
            if (i < 3)
                dc->DrawText(glyphs[i], 1, glyph_.Get(), r, en[i] ? brText_.Get() : brDisabled_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            else   // dual-pane toggle: two side-by-side rectangles
            {
                const float cx = (r.left + r.right) * 0.5f;
                dc->DrawRectangle({ r.left + 8, r.top + 9, cx - 1.5f, r.bottom - 9 }, brText_.Get(), 1.3f);
                dc->DrawRectangle({ cx + 1.5f, r.top + 9, r.right - 8, r.bottom - 9 }, brText_.Get(), 1.3f);
            }
        }

        // Details-pane toggle + Settings (gear) at the right end of the toolbar.
        {
            D2D1_RECT_F d = DetailsRect(viewW);
            if (s.detailsOn) dc->FillRoundedRectangle({ d, 5, 5 }, brActive_.Get());
            else if (s.hoverButton == 6) dc->FillRoundedRectangle({ d, 5, 5 }, brHover_.Get());
            // Icon: a panel with a highlighted right column (like Explorer's details toggle).
            const float dl = d.left + 8, dt = d.top + 9, dr = d.right - 8, db = d.bottom - 9;
            dc->DrawRectangle({ dl, dt, dr, db }, brText_.Get(), 1.3f);
            dc->DrawLine({ dr - 8, dt }, { dr - 8, db }, brText_.Get(), 1.3f);
            dc->FillRectangle({ dr - 7.5f, dt + 1, dr - 0.5f, db - 0.5f }, brText2_.Get());
        }
        {
            D2D1_RECT_F g = GearRect(viewW);
            if (s.hoverButton == 5) dc->FillRoundedRectangle({ g, 5, 5 }, brHover_.Get());
            dc->DrawText(L"\x2699", 1, glyph_.Get(), g, brText_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        // Address: breadcrumb or inline editor (leaves room for details + gear).
        D2D1_RECT_F addr{ kAddrLeft, 8.f, viewW - 96.f, 40.f };
        lastAddr_ = addr;
        dc->FillRoundedRectangle({ addr, 4, 4 }, brCtrl_.Get());
        dc->PushAxisAlignedClip({ addr.left + 2, addr.top, addr.right - 2, addr.bottom }, D2D1_ANTIALIAS_MODE_ALIASED);
        if (!s.editing)
        {
            segRects_.clear(); segPaths_.clear();
            float x = addr.left + 10.f;
            for (size_t i = 0; i < s.segments.size(); ++i)
            {
                const std::wstring& lbl = s.segments[i].label;
                const float w = Measure(lbl);
                dc->DrawText(lbl.c_str(), (UINT32)lbl.size(), path_.Get(), { x, addr.top, x + w, addr.bottom }, brText_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                segRects_.push_back({ x - 4, addr.top, x + w + 4, addr.bottom }); segPaths_.push_back(s.segments[i].path);
                x += w;
                if (i + 1 < s.segments.size()) { const wchar_t* chev = L"  \x203A  "; const float cw = Measure(chev); dc->DrawText(chev, 5, path_.Get(), { x, addr.top, x + cw, addr.bottom }, brText2_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP); x += cw; }
                if (x > addr.right) break;
            }
        }
        else
        {
            const float tx = addr.left + 10.f;
            const float prefixW = Measure(s.editText.substr(0, s.caret));
            const float fullW = Measure(s.editText);
            if (s.selAll && !s.editText.empty()) dc->FillRectangle({ tx - 1, addr.top + 5, tx + fullW + 1, addr.bottom - 5 }, brSel_.Get());
            dc->DrawText(s.editText.c_str(), (UINT32)s.editText.size(), path_.Get(), { tx, addr.top, addr.right - 6, addr.bottom }, brText_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            if (s.caretOn) { const float cx = tx + prefixW; dc->DrawLine({ cx, addr.top + 6 }, { cx, addr.bottom - 6 }, brText_.Get(), 1.2f); }
        }
        dc->PopAxisAlignedClip();

        D2D1_RECT_F sb{ 0, viewH - kStatusH, viewW, viewH };
        dc->FillRectangle(sb, brStatus_.Get());
        dc->DrawLine({ 0, viewH - kStatusH + 0.5f }, { viewW, viewH - kStatusH + 0.5f }, brLine_.Get(), 1.f);
        dc->DrawText(s.statusLeft.c_str(), (UINT32)s.statusLeft.size(), status_.Get(), { 12.f, sb.top, viewW * 0.6f, sb.bottom }, brText2_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        dc->DrawText(s.statusRight.c_str(), (UINT32)s.statusRight.size(), statusR_.Get(), { viewW * 0.4f, sb.top, viewW - 12.f, sb.bottom }, brText2_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

private:
    IDWriteFactory* dwrite_ = nullptr;
    ComPtr<IDWriteTextFormat> glyph_, path_, status_, statusR_;
    ID2D1DeviceContext* owner_ = nullptr;
    ComPtr<ID2D1SolidColorBrush> brToolbar_, brStatus_, brCtrl_, brHover_, brActive_, brText_, brText2_, brDisabled_, brLine_, brSel_;
    D2D1_RECT_F lastAddr_{};
    float lastViewW_ = 1200.f;
    std::vector<D2D1_RECT_F> segRects_;
    std::vector<std::wstring> segPaths_;
};
Chrome g_chrome;

// Tab-strip text formats + brushes (owned here; the strips live outside Chrome).
namespace
{
    ComPtr<IDWriteTextFormat> g_tabFmt, g_glyphFmt, g_palTitleFmt, g_palInputFmt;
    ComPtr<IDWriteTextFormat> g_navFmt, g_secFmt, g_fluentFmt, g_chevFmt, g_homeHeadFmt, g_detHeadFmt, g_detKeyFmt;
    ID2D1DeviceContext* g_brOwner = nullptr;
    ComPtr<ID2D1SolidColorBrush> g_brStrip, g_brTabActive, g_brTabHover, g_brText, g_brText2, g_brAccent, g_brLine,
        g_brDim, g_brPanel, g_brSelBg, g_brCtrl;

    void EnsureMainBrushes(ID2D1DeviceContext* dc)
    {
        if (g_brOwner == dc) return;
        g_brOwner = dc;
        auto b = [&](const D2D1_COLOR_F& c) { ComPtr<ID2D1SolidColorBrush> br; HR(dc->CreateSolidColorBrush(c, &br), "main brush"); return br; };
        g_brStrip = b(g_theme.toolbarBg); g_brTabActive = b(g_theme.windowBg); g_brTabHover = b(g_theme.controlHover);
        g_brText = b(g_theme.textPrimary); g_brText2 = b(g_theme.textSecondary); g_brAccent = b(g_theme.accent); g_brLine = b(g_theme.gridLine);
        g_brDim = b(RGBf(0, 0, 0, 0.40f));
        g_brPanel = b(g_theme.dark ? RGBf(46, 46, 46) : RGBf(250, 250, 250));
        g_brSelBg = b(g_theme.rowSelected);
        g_brCtrl = b(g_theme.controlBg);
    }
}

// ================================================================= layout ===

namespace
{
    struct Rc { float l, t, r, b; };
    float ViewW() { return g_gfx.WidthDip(); }
    float ViewH() { return g_gfx.HeightDip(); }
    float ContentTop() { return Chrome::kToolbarH; }
    float ContentBottom() { return ViewH() - Chrome::kStatusH; }
    // The panes live between the sidebar (left) and the details pane (right).
    float ContentLeft()  { return g_sidebar ? kSidebarW : 0.f; }
    float ContentRight() { return ViewW() - (g_details ? kDetailsW : 0.f); }
    float ContentW()     { return ContentRight() - ContentLeft(); }

    float SplitX()
    {
        const float l = ContentLeft(), r = ContentRight(), w = r - l;
        return std::floor(l + std::clamp(w * g_splitRatio, kMinPaneW, (std::max)(kMinPaneW, w - kMinPaneW)));
    }
    Rc PaneRc(int i)
    {
        const float top = ContentTop(), bot = ContentBottom(), l = ContentLeft(), r = ContentRight();
        if (!g_dual) return { l, top, r, bot };
        const float sx = SplitX();
        return (i == 0) ? Rc{ l, top, sx, bot } : Rc{ sx + 1, top, r, bot };
    }
    bool OverSplitter(float dx, float dy)
    {
        if (!g_dual) return false;
        const float sx = SplitX();
        return dy >= ContentTop() && dy < ContentBottom() && dx >= sx - 4.f && dx <= sx + 4.f;
    }
    D2D1_RECT_F SidebarRc() { return { 0, ContentTop(), kSidebarW, ContentBottom() }; }
    D2D1_RECT_F DetailsRc() { return { ViewW() - kDetailsW, ContentTop(), ViewW(), ContentBottom() }; }
    bool IsHome(const Tab& t) { return t.path.empty(); }
    bool IsThisPC(const Tab& t) { return t.path == kThisPC; }
    Rc StripRc(int i) { Rc p = PaneRc(i); return { p.l, p.t, p.r, p.t + kTabStripH }; }
    Rc ListRc(int i) { Rc p = PaneRc(i); return { p.l, p.t + kTabStripH, p.r, p.b }; }

    void TabLayout(int i, std::vector<D2D1_RECT_F>& rects, D2D1_RECT_F& newBtn)
    {
        Rc s = StripRc(i); Pane& p = g_pane[i];
        const float top = s.t + 5.f, bot = s.b, newW = 28.f, gap = 3.f;
        float x = s.l + 6.f;
        const int n = std::max<int>(1, (int)p.tabs.size());
        const float avail = s.r - 6.f - newW - gap - x;
        const float tabW = std::clamp(avail / n - gap, 64.f, 200.f);
        for (size_t t = 0; t < p.tabs.size(); ++t) { rects.push_back({ x, top, x + tabW, bot }); x += tabW + gap; }
        newBtn = { x, top, x + newW, bot };
    }

    int PaneAt(float dx, float dy)
    {
        for (int i = 0; i < VisiblePanes(); ++i) { Rc r = PaneRc(i); if (dx >= r.l && dx < r.r && dy >= r.t && dy < r.b) return i; }
        return -1;
    }
}

// ============================================================ tab / pane ops =

// forward decls
void StartLoad(int paneIdx, Tab* t);
void SetViewports();
void UpdateTitleBar();

void NavigateTab(int paneIdx, Tab* t, const std::wstring& path, bool pushHistory = true)
{
    if (pushHistory && path != t->path) { t->back.push_back(t->path); t->forward.clear(); }
    t->path = path;
    StartLoad(paneIdx, t);
}

void ActivateItem(int paneIdx, Tab* t, size_t index)
{
    g_activePane = paneIdx;
    if (index >= t->model->Count()) return;
    const FileEntry& e = t->model->At(index);
    if (e.isFolder) NavigateTab(paneIdx, t, e.isDrive ? e.target : Join(t->path, e.name));
    else { std::wstring full = Join(t->path, e.name); ShellExecuteW(g_hwnd, L"open", full.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
}

Tab* NewTab(int paneIdx, const std::wstring& path, bool activate)
{
    Pane& p = g_pane[paneIdx];
    auto t = std::make_unique<Tab>();
    t->path = path;
    t->model = std::make_unique<FileListModel>();
    t->view = std::make_unique<VirtualListView>(*t->model, g_gfx.DWrite(), g_theme, g_icons.get());
    t->view->SetInsets(0.f, 0.f);
    t->view->SetThumbnailCache(g_thumbs.get());
    Tab* raw = t.get();
    raw->view->SetOnActivate([paneIdx, raw](size_t idx) { ActivateItem(paneIdx, raw, idx); });
    p.tabs.push_back(std::move(t));
    if (activate) { p.active = p.tabs.size() - 1; SetViewports(); StartLoad(paneIdx, raw); }
    return raw;
}

void StartLoad(int paneIdx, Tab* t)
{
    Pane& p = g_pane[paneIdx];
    t->gen = ++g_gen;
    t->loading = true; t->loaded = false; t->searchMode = false; t->error.clear();
    t->model->Clear();
    t->view->OnModelReset();
    t->view->SetBasePath(t->path);
    p.enumr->Navigate(t->gen, t->path);
    UpdateTitleBar();
    g_dirty = true;
}

void SwitchTab(int paneIdx, size_t idx)
{
    Pane& p = g_pane[paneIdx];
    if (idx >= p.tabs.size()) return;
    g_activePane = paneIdx;
    p.active = idx;
    Tab* t = p.tabs[idx].get();
    SetViewports();
    if (!t->loaded && !t->loading) StartLoad(paneIdx, t);
    UpdateTitleBar();
    g_dirty = true;
}

void CloseTab(int paneIdx, size_t idx)
{
    Pane& p = g_pane[paneIdx];
    if (p.tabs.size() <= 1 || idx >= p.tabs.size()) return;   // keep >=1 tab
    p.tabs.erase(p.tabs.begin() + idx);
    if (p.active >= p.tabs.size()) p.active = p.tabs.size() - 1;
    SetViewports(); UpdateTitleBar(); g_dirty = true;
}

void CycleTab(int dir)
{
    Pane& p = AP();
    if (p.tabs.empty()) return;
    size_t n = p.tabs.size();
    SwitchTab(g_activePane, (p.active + (dir > 0 ? 1 : n - 1)) % n);
}

void ActivatePane(int i)
{
    if (!g_dual) i = 0;
    if (i == g_activePane) return;
    g_activePane = i;
    UpdateTitleBar(); g_dirty = true;
}

void ToggleDual()
{
    g_dual = !g_dual;
    if (g_dual && g_pane[1].tabs.empty())
        NewTab(1, g_pane[0].tabs[g_pane[0].active]->path, true);
    if (!g_dual) g_activePane = 0;
    SetViewports(); g_dirty = true;
}

void GoBack()    { Tab& t = AT(); if (t.back.empty()) return; t.forward.push_back(t.path); t.path = t.back.back(); t.back.pop_back(); StartLoad(g_activePane, &t); }
void GoForward() { Tab& t = AT(); if (t.forward.empty()) return; t.back.push_back(t.path); t.path = t.forward.back(); t.forward.pop_back(); StartLoad(g_activePane, &t); }
void GoUp()      { Tab& t = AT(); if (t.path.empty()) return; NavigateTab(g_activePane, &t, ParentOf(t.path)); }

void SetViewports()
{
    const float dpi = g_gfx.Dpi();
    for (int i = 0; i < VisiblePanes(); ++i)
    {
        Pane& p = g_pane[i];
        if (p.tabs.empty()) continue;
        Rc lr = ListRc(i);
        UINT w = static_cast<UINT>(DipToPx(lr.r - lr.l, dpi));
        UINT h = static_cast<UINT>(DipToPx(lr.b - lr.t, dpi));
        p.tabs[p.active]->view->SetViewport(w, h, dpi);
    }
}

void UpdateTitleBar()
{
    if (!g_hwnd) return;
    const std::wstring& path = AT().path;
    std::wstring disp = path.empty() ? std::wstring(L"Home")
                      : (path == kThisPC ? std::wstring(L"This PC") : path);
    SetWindowTextW(g_hwnd, (disp + L"  —  Crash").c_str());
}

void ForEachView(const std::function<void(VirtualListView*)>& fn)
{
    for (int i = 0; i < 2; ++i) for (auto& t : g_pane[i].tabs) if (t->view) fn(t->view.get());
}

void ReloadVisible()
{
    for (int i = 0; i < VisiblePanes(); ++i) if (!g_pane[i].tabs.empty()) StartLoad(i, g_pane[i].tabs[g_pane[i].active].get());
}

// Apply the current settings across the whole app (theme, density, view, hidden
// files, thumbnails). Called at startup and whenever a setting changes.
void ApplySettings()
{
    g_theme = Theme::Load(g_settings.theme);
    g_brOwner = nullptr;
    g_chrome.InvalidateBrushes();

    const Density dens = g_settings.compact ? Density::Compact : Density::Comfortable;
    const ViewMode vm = g_settings.gridDefault ? ViewMode::Grid : ViewMode::Details;
    ForEachView([&](VirtualListView* v) {
        v->SetTheme(g_theme);
        v->SetDensity(dens);
        v->SetViewMode(vm);
        v->SetThumbnailCache(g_settings.thumbnails ? g_thumbs.get() : nullptr);
    });
    for (int i = 0; i < 2; ++i) if (g_pane[i].enumr) g_pane[i].enumr->SetShowHidden(g_settings.showHidden);

    BOOL dark = g_theme.dark ? TRUE : FALSE;
    DwmSetWindowAttribute(g_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    SetViewports();
    g_dirty = true;
}

void OpenSettings() { g_pal.open = false; g_settingsOpen = true; g_overlayKind = 2; if (!AnimationsEnabled()) g_overlayAnim = 1.f; g_dirty = true; }
void CloseSettings() { g_settingsOpen = false; g_dirty = true; }

// Prompt for a Crash.lic file and install it if the signature verifies.
bool ImportLicenseInteractive(HWND hwnd)
{
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Crash license (*.lic)\0*.lic\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Import your Crash Pro license";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return false;
    return ImportLicense(file);
}

void StartRename()
{
    if (g_renaming) return;
    Tab& t = AT();
    if (t.path.empty()) return;                      // not in This PC
    const int64_t f = t.view->Focus();
    if (f < 0 || f >= static_cast<int64_t>(t.model->Count())) return;
    const FileEntry& e = t.model->At(static_cast<size_t>(f));
    if (e.isDrive) return;
    D2D1_RECT_F local;
    if (!t.view->FocusRowRectDip(&local)) return;
    Rc lr = ListRc(g_activePane);
    g_renameRectDip = { lr.l + local.left, lr.t + local.top, lr.l + local.right, lr.t + local.bottom };
    g_renamePane = g_activePane;
    g_renameOldPath = Join(t.path, e.name);
    g_ren.Begin(e.name);
    g_renaming = true;
    g_dirty = true;
}
void CancelRename() { g_renaming = false; g_dirty = true; }

void CommitRename()
{
    if (!g_renaming) return;
    g_renaming = false;
    std::wstring nn = g_ren.text;
    const size_t a = nn.find_first_not_of(L" \t");
    const size_t b = nn.find_last_not_of(L" \t.");        // Windows: no trailing space/dot
    nn = (a == std::wstring::npos) ? std::wstring() : nn.substr(a, b - a + 1);

    const bool valid = !nn.empty() && nn.find_first_of(L"\\/:*?\"<>|") == std::wstring::npos;
    if (valid)
    {
        Tab* t = g_pane[g_renamePane].tabs[g_pane[g_renamePane].active].get();
        std::wstring newPath = Join(t->path, nn);
        if (newPath != g_renameOldPath && MoveFileW(g_renameOldPath.c_str(), newPath.c_str()))
            StartLoad(g_renamePane, t);
    }
    g_dirty = true;
}

// Begin an OLE drag of the given pane's current selection (modal until dropped).
void BeginDrag(int pane)
{
    Tab* t = g_pane[pane].tabs[g_pane[pane].active].get();
    if (t->path.empty()) return;                       // no files in This PC
    const std::vector<size_t> sels = t->view->SelectedIndices();
    if (sels.empty()) return;
    std::vector<std::wstring> names;
    for (size_t i : sels) names.push_back(t->model->At(i).name);
    StartFileDrag(g_hwnd, t->path, names);
}

std::vector<ChromeSeg> BuildSegments(const std::wstring& path)
{
    std::vector<ChromeSeg> segs;
    segs.push_back({ L"Home", L"" });
    if (path == kThisPC) { segs.push_back({ L"This PC", kThisPC }); return segs; }
    if (!path.empty())
    {
        segs.push_back({ L"This PC", kThisPC });
        std::wstring drive = path.substr(0, 2);
        std::wstring acc = drive + L"\\";
        segs.push_back({ drive, acc });
        if (path.size() > 3)
        {
            std::wstring rest = path.substr(3);
            size_t start = 0;
            while (start < rest.size())
            {
                size_t bs = rest.find(L'\\', start);
                std::wstring seg = (bs == std::wstring::npos) ? rest.substr(start) : rest.substr(start, bs - start);
                if (!seg.empty()) { acc += seg; segs.push_back({ seg, acc }); acc += L"\\"; }
                if (bs == std::wstring::npos) break;
                start = bs + 1;
            }
        }
    }
    return segs;
}

void BeginAddressEdit() { std::wstring t = AT().path; if (t == kThisPC) t.clear(); g_addr.Begin(t); g_dirty = true; }
void CommitAddress()
{
    std::wstring t = g_addr.text;
    size_t a = t.find_first_not_of(L" \t\"");
    size_t b = t.find_last_not_of(L" \t\"");
    t = (a == std::wstring::npos) ? std::wstring() : t.substr(a, b - a + 1);
    g_addr.Cancel();
    NavigateTab(g_activePane, &AT(), t);
    g_dirty = true;
}

void PumpEnumEvents()
{
    for (int i = 0; i < 2; ++i)
    {
        Pane& p = g_pane[i];
        if (!p.enumr) continue;
        for (EnumEvent& ev : p.enumr->Drain())
        {
            Tab* t = nullptr;
            for (auto& tp : p.tabs) if (tp->gen == ev.generation) { t = tp.get(); break; }
            if (!t) continue;   // stale (tab closed or superseded)
            switch (ev.kind)
            {
            case EnumEvent::Batch: t->model->Append(ev.entries); t->view->OnModelChanged(); break;
            case EnumEvent::Done:  t->view->ApplySort(); t->view->OnModelChanged(); t->loading = false; t->loaded = true; break;
            case EnumEvent::Error: t->error = ev.message; t->loading = false; t->loaded = true; t->model->Clear(); t->view->OnModelChanged(); break;
            }
            g_dirty = true;
        }
    }
}

// ========================================================= command palette ==

enum {
    CMD_NEWTAB, CMD_CLOSETAB, CMD_DUAL, CMD_SWITCHPANE, CMD_UP, CMD_BACK, CMD_FORWARD,
    CMD_THISPC, CMD_HOME, CMD_VIEW, CMD_DENSITY, CMD_SORTNAME, CMD_SORTTYPE, CMD_SORTSIZE,
    CMD_SORTDATE, CMD_FILTER, CMD_SEARCH, CMD_COPYPATH, CMD_REFRESH, CMD_VSYNC, CMD_SETTINGS
};
struct CmdDef { int id; const wchar_t* title; const wchar_t* cat; };
const CmdDef kCommands[] = {
    { CMD_NEWTAB,   L"New Tab",                                L"Tabs" },
    { CMD_CLOSETAB, L"Close Tab",                              L"Tabs" },
    { CMD_DUAL,     L"Toggle Dual-Pane",                       L"View" },
    { CMD_SWITCHPANE, L"Switch Active Pane",                   L"View" },
    { CMD_UP,       L"Go Up",                                  L"Navigate" },
    { CMD_BACK,     L"Back",                                   L"Navigate" },
    { CMD_FORWARD,  L"Forward",                                L"Navigate" },
    { CMD_THISPC,   L"Go to This PC",                          L"Navigate" },
    { CMD_HOME,     L"Go to Home Folder",                      L"Navigate" },
    { CMD_REFRESH,  L"Refresh",                                L"Navigate" },
    { CMD_VIEW,     L"Toggle View: Details / Grid",            L"View" },
    { CMD_DENSITY,  L"Toggle Density: Comfortable / Compact",  L"View" },
    { CMD_VSYNC,    L"Toggle VSync",                           L"View" },
    { CMD_SORTNAME, L"Sort by Name",                           L"Sort" },
    { CMD_SORTTYPE, L"Sort by Type",                           L"Sort" },
    { CMD_SORTSIZE, L"Sort by Size",                           L"Sort" },
    { CMD_SORTDATE, L"Sort by Date Modified",                  L"Sort" },
    { CMD_FILTER,   L"Filter Files in Folder…",           L"Search" },
    { CMD_SEARCH,   L"Search This Folder + Subfolders…",   L"Search" },
    { CMD_COPYPATH, L"Copy Current Path",                      L"File" },
    { CMD_SETTINGS, L"Open Settings…",                     L"App" },
};

// Subsequence fuzzy match with bonuses for consecutive / word-start hits.
bool Fuzzy(const std::wstring& needleLower, const std::wstring& hay, int& score)
{
    if (needleLower.empty()) { score = 0; return true; }
    score = 0; size_t hi = 0; int streak = 0;
    for (wchar_t nc : needleLower)
    {
        bool found = false;
        while (hi < hay.size())
        {
            const wchar_t hc = static_cast<wchar_t>(towlower(hay[hi]));
            const bool boundary = (hi == 0) || hay[hi - 1] == L' ';
            if (hc == nc) { score += 1 + streak * 2 + (boundary ? 4 : 0); ++streak; ++hi; found = true; break; }
            streak = 0; ++hi;
        }
        if (!found) return false;
    }
    return true;
}

void RebuildPaletteResults()
{
    g_pal.items.clear();
    std::wstring nl; for (wchar_t c : g_pal.input) nl += static_cast<wchar_t>(towlower(c));
    for (const CmdDef& c : kCommands) { int sc = 0; if (Fuzzy(nl, c.title, sc)) g_pal.items.push_back({ c.id, c.title, c.cat, sc }); }
    std::stable_sort(g_pal.items.begin(), g_pal.items.end(), [](const PalItem& a, const PalItem& b) { return a.score > b.score; });
    if (g_pal.sel >= (int)g_pal.items.size()) g_pal.sel = g_pal.items.empty() ? 0 : (int)g_pal.items.size() - 1;
}

void OpenPalette(int mode)
{
    if (mode == 0 && !g_pro) { mode = 2; g_pal.message = L"The command palette is a Crash Pro feature."; }
    g_pal.open = true; g_pal.mode = mode; g_pal.sel = 0;
    g_pal.input = (mode == 1) ? AT().model->Filter() : std::wstring();
    g_pal.caret = g_pal.input.size();
    if (mode == 0) RebuildPaletteResults();
    if (mode != 1) { g_settingsOpen = false; g_overlayKind = 1; if (!AnimationsEnabled()) g_overlayAnim = 1.f; }
    g_dirty = true;
}
void ClosePalette() { g_pal.open = false; g_dirty = true; }

void OnPaletteInputChanged()
{
    if (g_pal.mode == 0) RebuildPaletteResults();
    else if (g_pal.mode == 1 && !g_searchArmed) { AT().model->SetFilter(g_pal.input); AT().view->OnFilterChanged(); }
    g_dirty = true;   // search mode filters only on Enter, not live
}

// Recursive subtree search of the active tab's folder (Pro; §6.6).
void RunRecursiveSearch(const std::wstring& query)
{
    const int pane = g_activePane;
    Tab& t = AT();
    if (t.path.empty()) return;                        // can't search This PC
    t.gen = ++g_gen;
    t.loading = true; t.loaded = false; t.searchMode = true; t.error.clear();
    t.model->Clear();
    t.view->OnModelReset();
    t.view->SetBasePath(t.path);                       // names are relative → thumbnails/paths resolve
    g_pane[pane].enumr->Search(t.gen, t.path, query);
    UpdateTitleBar();
    g_dirty = true;
}

void CopyToClipboard(const std::wstring& s)
{
    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    const size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) { memcpy(GlobalLock(h), s.c_str(), bytes); GlobalUnlock(h); SetClipboardData(CF_UNICODETEXT, h); }
    CloseClipboard();
}

void ClearFilter() { AT().model->SetFilter(L""); AT().view->OnFilterChanged(); g_dirty = true; }

void RunCommand(int id)
{
    switch (id)
    {
    case CMD_NEWTAB:    NewTab(g_activePane, AT().path, true); break;
    case CMD_CLOSETAB:  CloseTab(g_activePane, AP().active); break;
    case CMD_DUAL:      ToggleDual(); break;
    case CMD_SWITCHPANE:if (g_dual) ActivatePane(g_activePane ^ 1); break;
    case CMD_UP:        GoUp(); break;
    case CMD_BACK:      GoBack(); break;
    case CMD_FORWARD:   GoForward(); break;
    case CMD_THISPC:    NavigateTab(g_activePane, &AT(), L""); break;
    case CMD_HOME:      NavigateTab(g_activePane, &AT(), g_home); break;
    case CMD_REFRESH:   StartLoad(g_activePane, &AT()); break;
    case CMD_VIEW:      AT().view->OnKey('G'); break;
    case CMD_DENSITY:   AT().view->OnKey('D'); break;
    case CMD_VSYNC:     g_vsync = !g_vsync; break;
    case CMD_SORTNAME:  AT().view->SetSort(SortKey::Name, true); break;
    case CMD_SORTTYPE:  AT().view->SetSort(SortKey::Type, true); break;
    case CMD_SORTSIZE:  AT().view->SetSort(SortKey::Size, false); break;
    case CMD_SORTDATE:  AT().view->SetSort(SortKey::Date, false); break;
    case CMD_COPYPATH:  CopyToClipboard(AT().path.empty() ? std::wstring(L"This PC") : AT().path); break;
    case CMD_SETTINGS:  OpenSettings(); break;
    case CMD_SEARCH:    OpenPalette(1); g_searchArmed = true; g_pal.input.clear(); g_pal.caret = 0; break;
    }
    g_dirty = true;
}

// ================================================================ rendering ==

void DrawStrip(ID2D1DeviceContext* dc, int i)
{
    Rc s = StripRc(i); Pane& p = g_pane[i];
    dc->FillRectangle({ s.l, s.t, s.r, s.b }, g_brStrip.Get());

    std::vector<D2D1_RECT_F> rects; D2D1_RECT_F newBtn; TabLayout(i, rects, newBtn);
    for (size_t t = 0; t < rects.size(); ++t)
    {
        const D2D1_RECT_F& r = rects[t];
        const bool act = (t == p.active);
        const bool hov = (g_hoverTab.pane == i && g_hoverTab.tab == (int)t);
        if (act) dc->FillRoundedRectangle({ { r.left, r.top, r.right, r.bottom + 5 }, 6, 6 }, g_brTabActive.Get());
        else if (hov) dc->FillRoundedRectangle({ { r.left, r.top, r.right, r.bottom }, 6, 6 }, g_brTabHover.Get());

        std::wstring title = TabTitle(p.tabs[t]->path);
        dc->DrawText(title.c_str(), (UINT32)title.size(), g_tabFmt.Get(),
            { r.left + 10, r.top, r.right - 22, r.bottom }, act ? g_brText.Get() : g_brText2.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        if ((act || hov) && p.tabs.size() > 1)
            dc->DrawText(L"\x2715", 1, g_glyphFmt.Get(), { r.right - 22, r.top, r.right - 4, r.bottom }, g_brText2.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    dc->DrawText(L"+", 1, g_glyphFmt.Get(), newBtn, g_brText2.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

    if (g_dual && i == g_activePane)   // active-pane marker
        dc->FillRectangle({ s.l, s.b - 2, s.r, s.b }, g_brAccent.Get());
}

// Search box: active input (editing) at top-right of the active pane, or a
// passive chip when a filter is applied but the box is closed.
void DrawFilterBar(ID2D1DeviceContext* dc, bool editing, bool caretOn)
{
    Rc lr = ListRc(g_activePane);
    const float fw = 300.f, fh = 30.f;
    const float fx = (std::max)(lr.l + 8.f, lr.r - fw - 14.f);
    const float fy = lr.t + 8.f;
    D2D1_RECT_F box{ fx, fy, fx + fw, fy + fh };
    dc->FillRoundedRectangle({ box, 6, 6 }, g_brPanel.Get());
    dc->DrawRoundedRectangle({ box, 6, 6 }, editing ? g_brAccent.Get() : g_brLine.Get(), editing ? 1.4f : 1.f);

    const std::wstring text = editing ? g_pal.input : AT().model->Filter();
    const float tx = fx + 12.f;
    if (text.empty())
    {
        const wchar_t* ph = (editing && g_searchArmed) ? L"Search this folder + subfolders…  \x21B5" : L"Filter files…";
        dc->DrawText(ph, (UINT32)wcslen(ph), g_tabFmt.Get(), { tx, fy, fx + fw - 90, fy + fh }, g_brText2.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    else
        dc->DrawText(text.c_str(), (UINT32)text.size(), g_tabFmt.Get(), { tx, fy, fx + fw - 90, fy + fh }, g_brText.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    if (editing && caretOn)
    {
        ComPtr<IDWriteTextLayout> l; float cx = tx;
        if (SUCCEEDED(g_gfx.DWrite()->CreateTextLayout(g_pal.input.c_str(), (UINT32)g_pal.caret, g_tabFmt.Get(), 1e5f, 100.f, &l)))
        { DWRITE_TEXT_METRICS m{}; l->GetMetrics(&m); cx = tx + m.widthIncludingTrailingWhitespace; }
        dc->DrawLine({ cx, fy + 6 }, { cx, fy + fh - 6 }, g_brText.Get(), 1.2f);
    }

    // count + clear (×)
    wchar_t cnt[48]; std::swprintf(cnt, std::size(cnt), L"%zu / %zu", AT().model->Count(), AT().model->TotalCount());
    dc->DrawText(cnt, (UINT32)wcslen(cnt), g_tabFmt.Get(), { fx + fw - 84, fy, fx + fw - 30, fy + fh }, g_brText2.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    D2D1_RECT_F clr{ fx + fw - 26, fy, fx + fw, fy + fh };
    dc->DrawText(L"\x2715", 1, g_glyphFmt.Get(), clr, g_brText2.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    g_pal.filterClearRect = clr;
}

void DrawPalette(ID2D1DeviceContext* dc, float viewW, float viewH, bool caretOn, float anim)
{
    dc->FillRectangle({ 0, 0, viewW, viewH }, g_brDim.Get());   // dim backdrop (identity)
    dc->SetTransform(D2D1::Matrix3x2F::Translation(0.f, (1.f - anim) * -10.f));

    const float pw = (std::min)(640.f, viewW - 80.f), px = (viewW - pw) * 0.5f, py = 96.f;
    const float inputH = 50.f, rowH = 42.f;
    const int nRes = (g_pal.mode == 0) ? (std::min)((int)g_pal.items.size(), 8) : 0;
    const float bodyH = (g_pal.mode == 0) ? nRes * rowH : (g_pal.mode == 2 ? 70.f : 0.f);
    const float ph = inputH + bodyH + 12.f;
    g_pal.panelRect = { px, py, px + pw, py + ph };

    dc->FillRoundedRectangle({ { px - 2, py + 5, px + pw + 2, py + ph + 10 }, 12, 12 }, g_brDim.Get());   // shadow
    dc->FillRoundedRectangle({ g_pal.panelRect, 12, 12 }, g_brPanel.Get());
    dc->DrawRoundedRectangle({ g_pal.panelRect, 12, 12 }, g_brLine.Get(), 1.f);

    // Input line.
    const float ix = px + 20.f;
    const wchar_t* prompt = (g_pal.mode == 2) ? L"\x26BF" : L"\x203A";   // key : ›
    dc->DrawText(prompt, (UINT32)wcslen(prompt), g_palInputFmt.Get(), { ix, py, ix + 24, py + inputH }, g_brAccent.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    const float tx = ix + 28.f;
    if (g_pal.input.empty())
    {
        const wchar_t* ph2 = (g_pal.mode == 2) ? L"Import a license file to unlock Pro" : L"Type a command…";
        dc->DrawText(ph2, (UINT32)wcslen(ph2), g_palInputFmt.Get(), { tx, py, px + pw - 20, py + inputH }, g_brText2.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    else
        dc->DrawText(g_pal.input.c_str(), (UINT32)g_pal.input.size(), g_palInputFmt.Get(), { tx, py, px + pw - 20, py + inputH }, g_brText.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    if (caretOn)
    {
        ComPtr<IDWriteTextLayout> l; float cx = tx;
        if (SUCCEEDED(g_gfx.DWrite()->CreateTextLayout(g_pal.input.c_str(), (UINT32)g_pal.caret, g_palInputFmt.Get(), 1e5f, 100.f, &l)))
        { DWRITE_TEXT_METRICS m{}; l->GetMetrics(&m); cx = tx + m.widthIncludingTrailingWhitespace; }
        dc->DrawLine({ cx, py + 12 }, { cx, py + inputH - 10 }, g_brText.Get(), 1.4f);
    }
    dc->DrawLine({ px + 12, py + inputH }, { px + pw - 12, py + inputH }, g_brLine.Get(), 1.f);

    g_pal.itemRects.clear();
    if (g_pal.mode == 0)
    {
        for (int i = 0; i < nRes; ++i)
        {
            const float ry = py + inputH + i * rowH;
            D2D1_RECT_F row{ px + 8, ry, px + pw - 8, ry + rowH };
            if (i == g_pal.sel) dc->FillRoundedRectangle({ row, 6, 6 }, g_brSelBg.Get());
            dc->DrawText(g_pal.items[i].title.c_str(), (UINT32)g_pal.items[i].title.size(), g_palTitleFmt.Get(),
                { px + 22, ry, px + pw - 120, ry + rowH }, g_brText.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            dc->DrawText(g_pal.items[i].cat.c_str(), (UINT32)g_pal.items[i].cat.size(), g_tabFmt.Get(),
                { px + pw - 120, ry, px + pw - 22, ry + rowH }, g_brText2.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            g_pal.itemRects.push_back(row);
        }
    }
    else if (g_pal.mode == 2)
    {
        const float my = py + inputH + 8.f;
        dc->DrawText(g_pal.message.c_str(), (UINT32)g_pal.message.size(), g_tabFmt.Get(), { px + 22, my, px + pw - 22, my + 24 }, g_brText.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        const wchar_t* hint = L"Press Enter to import your Crash.lic license file   ·   Esc to cancel";
        dc->DrawText(hint, (UINT32)wcslen(hint), g_tabFmt.Get(), { px + 22, my + 28, px + pw - 22, my + 52 }, g_brText2.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
}

void ApplySettingHit(int action, int value)
{
    switch (action)
    {
    case 1: g_settings.theme = value; break;
    case 2: g_settings.compact = (value == 1); break;
    case 3: g_settings.gridDefault = (value == 1); break;
    case 4: g_settings.showHidden = (value == 1); break;
    case 5: g_settings.thumbnails = (value == 1); break;
    case 6: g_settings.animations = (value == 1); break;
    case 7: CloseSettings(); return;
    case 8: if (ImportLicenseInteractive(g_hwnd)) g_pro = true; g_dirty = true; return;   // unlock → import license
    default: return;
    }
    ApplySettings();
    if (action == 4) ReloadVisible();     // show-hidden requires re-enumeration
    SaveSettings(g_settings);
    g_dirty = true;
}

void DrawSettings(ID2D1DeviceContext* dc, float viewW, float viewH, float anim)
{
    dc->FillRectangle({ 0, 0, viewW, viewH }, g_brDim.Get());   // backdrop (identity)

    const float pw = (std::min)(560.f, viewW - 80.f), px = (viewW - pw) * 0.5f, py = 84.f;
    const float titleH = 46.f, rowH = 52.f, footH = 46.f;
    const int rows = 6;
    const float ph = titleH + rows * rowH + footH + 20.f;

    dc->SetTransform(D2D1::Matrix3x2F::Translation(0.f, (1.f - anim) * -10.f));   // subtle slide

    dc->FillRoundedRectangle({ { px - 2, py + 5, px + pw + 2, py + ph + 10 }, 12, 12 }, g_brDim.Get());
    D2D1_RECT_F panel{ px, py, px + pw, py + ph };
    g_settPanelRect = panel;
    dc->FillRoundedRectangle({ panel, 12, 12 }, g_brPanel.Get());
    dc->DrawRoundedRectangle({ panel, 12, 12 }, g_brLine.Get(), 1.f);

    g_settHits.clear();
    dc->DrawText(L"Settings", 8, g_palInputFmt.Get(), { px + 22, py, px + pw - 50, py + titleH }, g_brText.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    D2D1_RECT_F closeR{ px + pw - 42, py + 8, px + pw - 14, py + titleH - 4 };
    dc->DrawText(L"\x2715", 1, g_glyphFmt.Get(), closeR, g_brText2.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    g_settHits.push_back({ closeR, 7, 0 });

    auto segmented = [&](float ry, const std::vector<const wchar_t*>& labels, int active, int action) {
        const float cw = 300.f, cx = px + pw - cw - 22.f, ch = rowH - 16.f, cy = ry + 8.f;
        const float sw = cw / labels.size();
        for (size_t i = 0; i < labels.size(); ++i)
        {
            D2D1_RECT_F sr{ cx + i * sw, cy, cx + (i + 1) * sw, cy + ch };
            if ((int)i == active) dc->FillRoundedRectangle({ sr, 6, 6 }, g_brSelBg.Get());
            dc->DrawText(labels[i], (UINT32)wcslen(labels[i]), g_glyphFmt.Get(), sr, (int)i == active ? g_brText.Get() : g_brText2.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            g_settHits.push_back({ sr, action, (int)i });
        }
        dc->DrawRoundedRectangle({ { cx, cy, cx + cw, cy + ch }, 6, 6 }, g_brLine.Get(), 1.f);
    };
    auto label = [&](const wchar_t* s, float ry) {
        dc->DrawText(s, (UINT32)wcslen(s), g_palTitleFmt.Get(), { px + 24, ry, px + pw - 320, ry + rowH }, g_brText.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };

    float ry = py + titleH;
    label(L"Appearance", ry);      segmented(ry, { L"System", L"Light", L"Dark" }, g_settings.theme, 1); ry += rowH;
    label(L"Density", ry);         segmented(ry, { L"Comfortable", L"Compact" }, g_settings.compact ? 1 : 0, 2); ry += rowH;
    label(L"Default view", ry);    segmented(ry, { L"Details", L"Grid" }, g_settings.gridDefault ? 1 : 0, 3); ry += rowH;
    label(L"Show hidden files", ry); segmented(ry, { L"Off", L"On" }, g_settings.showHidden ? 1 : 0, 4); ry += rowH;
    label(L"Thumbnails", ry);      segmented(ry, { L"Off", L"On" }, g_settings.thumbnails ? 1 : 0, 5); ry += rowH;
    label(L"Animations", ry);      segmented(ry, { L"Off", L"On" }, g_settings.animations ? 1 : 0, 6); ry += rowH;

    // Footer: Pro status.
    std::wstring pro = g_pro ? L"Crash Pro: Unlocked \x2713" : L"Crash Pro: Locked";
    dc->DrawText(pro.c_str(), (UINT32)pro.size(), g_tabFmt.Get(), { px + 24, ry + 8, px + pw - 140, ry + footH }, g_pro ? g_brAccent.Get() : g_brText2.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    if (!g_pro)
    {
        D2D1_RECT_F unlockR{ px + pw - 130, ry + 12, px + pw - 22, ry + footH - 4 };
        dc->FillRoundedRectangle({ unlockR, 6, 6 }, g_brSelBg.Get());
        dc->DrawText(L"Unlock…", 7, g_glyphFmt.Get(), unlockR, g_brText.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        g_settHits.push_back({ unlockR, 8, 0 });
    }

    dc->SetTransform(D2D1::Matrix3x2F::Identity());
}

void DrawRename(ID2D1DeviceContext* dc, bool caretOn)
{
    if (!g_renaming) return;
    D2D1_RECT_F b = g_renameRectDip;
    b.left -= 2; b.right += 2;
    dc->FillRectangle(b, g_brPanel.Get());               // opaque, covers the row text
    dc->DrawRectangle(b, g_brAccent.Get(), 1.4f);

    auto measure = [&](const std::wstring& s) -> float {
        if (s.empty()) return 0.f;
        ComPtr<IDWriteTextLayout> l;
        if (SUCCEEDED(g_gfx.DWrite()->CreateTextLayout(s.c_str(), (UINT32)s.size(), g_palTitleFmt.Get(), 1e5f, 100.f, &l)))
        { DWRITE_TEXT_METRICS m{}; l->GetMetrics(&m); return m.widthIncludingTrailingWhitespace; }
        return 0.f;
    };
    const float tx = b.left + 6.f;
    if (g_ren.selAll && !g_ren.text.empty())
        dc->FillRectangle({ tx - 1, b.top + 3, tx + measure(g_ren.text) + 1, b.bottom - 3 }, g_brSelBg.Get());
    dc->DrawText(g_ren.text.c_str(), (UINT32)g_ren.text.size(), g_palTitleFmt.Get(),
        { tx, b.top, b.right - 4, b.bottom }, g_brText.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    if (caretOn)
    {
        const float cx = tx + measure(g_ren.text.substr(0, g_ren.caret));
        dc->DrawLine({ cx, b.top + 4 }, { cx, b.bottom - 4 }, g_brText.Get(), 1.4f);
    }
}

// ============================================ sidebar / details / home page ==

std::wstring KnownFolder(REFKNOWNFOLDERID id)
{
    PWSTR p = nullptr; std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) { s = p; CoTaskMemFree(p); }
    return s;
}

// Enumerate the machine's drives as (label, root) — used by Home cards, the This
// PC screen, and the This PC subtree. Rebuilt at startup (drives rarely change).
void BuildDriveList()
{
    g_drives.clear();
    wchar_t buf[512] = L"";
    if (GetLogicalDriveStringsW(511, buf))
        for (wchar_t* d = buf; *d; d += wcslen(d) + 1)
        {
            std::wstring root = d;                       // "C:\"
            std::wstring letter = root.substr(0, 2);     // "C:"
            const UINT dt = GetDriveTypeW(root.c_str());
            wchar_t vol[MAX_PATH] = L"";
            if (dt == DRIVE_FIXED)
                GetVolumeInformationW(root.c_str(), vol, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0);
            const wchar_t* kind = dt == DRIVE_REMOTE    ? L"Network Drive"
                                : dt == DRIVE_REMOVABLE ? L"Removable Disk"
                                : dt == DRIVE_CDROM     ? L"CD Drive"
                                :                         L"Local Disk";
            std::wstring disp = (vol[0] ? std::wstring(vol) : std::wstring(kind)) + L" (" + letter + L")";
            g_drives.push_back({ disp, root });
        }
}

// Flatten the tree into the visible-row list (children only under expanded nodes).
void FlattenNavInto(std::vector<NavNode>& nodes, int depth)
{
    for (auto& n : nodes)
    {
        g_navRows.push_back({ &n, depth });
        if (n.expanded && !n.children.empty()) FlattenNavInto(n.children, depth + 1);
    }
}
void FlattenNav() { g_navRows.clear(); FlattenNavInto(g_navRoots, 0); }

// Lazily populate a node's children: This PC → drives; a drive/folder → its
// immediate subdirectories. Synchronous (one level, on demand) — fast for local
// drives; a disconnected network drive could briefly stall on expand.
void LoadNavChildren(NavNode& n)
{
    if (n.loaded) return;
    n.loaded = true;
    n.children.clear();

    if (n.kind == 4)   // This PC → drives
    {
        for (auto& d : g_drives) { NavNode c; c.label = d.first; c.path = d.second; c.kind = 2; c.expandable = true; n.children.push_back(c); }
    }
    else               // drive/folder → subdirectories only
    {
        std::wstring base = n.path;
        if (base.empty()) { if (n.children.empty()) n.expandable = false; return; }
        if (base.back() != L'\\') base += L'\\';
        const bool showHidden = g_settings.showHidden;
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileExW((base + L"*").c_str(), FindExInfoBasic, &fd,
                                    FindExSearchLimitToDirectories, nullptr, 0);
        if (h != INVALID_HANDLE_VALUE)
        {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) continue;
                if (!showHidden && (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
                NavNode c; c.label = fd.cFileName; c.path = base + fd.cFileName; c.kind = 0; c.expandable = true;
                n.children.push_back(std::move(c));
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        std::sort(n.children.begin(), n.children.end(),
                  [](const NavNode& a, const NavNode& b) { return _wcsicmp(a.label.c_str(), b.label.c_str()) < 0; });
    }
    if (n.children.empty()) n.expandable = false;   // nothing to expand → drop the chevron
}

void ToggleNav(NavNode& n)
{
    if (!n.expandable) return;
    if (!n.expanded) { LoadNavChildren(n); n.expanded = !n.children.empty(); }
    else             { n.expanded = false; }
    FlattenNav();
    g_dirty = true;
}

// Build the navigation tree: Home, pinned Quick access folders, a separator, then
// an expandable This PC (drives → folders → subfolders load lazily).
void BuildNav()
{
    BuildDriveList();
    g_quickAccess.clear();
    g_navRoots.clear();

    g_navRoots.push_back({ L"Home", L"", 3 });

    auto add = [&](const wchar_t* label, REFKNOWNFOLDERID id) {
        std::wstring p = KnownFolder(id);
        if (!p.empty()) { NavNode n; n.label = label; n.path = p; n.kind = 0; g_navRoots.push_back(n); g_quickAccess.push_back({ label, p }); }
    };
    add(L"Desktop",   FOLDERID_Desktop);
    add(L"Downloads", FOLDERID_Downloads);
    add(L"Documents", FOLDERID_Documents);
    add(L"Pictures",  FOLDERID_Pictures);
    add(L"Music",     FOLDERID_Music);
    add(L"Videos",    FOLDERID_Videos);

    { NavNode sep; sep.kind = 5; g_navRoots.push_back(sep); }            // separator below Videos

    { NavNode pc; pc.label = L"This PC"; pc.path = kThisPC; pc.kind = 4; pc.expandable = true; g_navRoots.push_back(pc); }

    FlattenNav();
}

void DrawSidebar(ID2D1DeviceContext* dc)
{
    const D2D1_RECT_F sb = SidebarRc();
    dc->FillRectangle(sb, g_brStrip.Get());
    dc->DrawLine({ sb.right - 0.5f, sb.top }, { sb.right - 0.5f, sb.bottom }, g_brLine.Get(), 1.f);
    dc->PushAxisAlignedClip(sb, D2D1_ANTIALIAS_MODE_ALIASED);

    const std::wstring& cur = AT().path;
    g_navRects.assign(g_navRows.size(), D2D1_RECT_F{ 0, 0, 0, 0 });
    g_navChevRects.assign(g_navRows.size(), D2D1_RECT_F{ 0, 0, 0, 0 });
    const float rowH = 30.f, indent = 15.f, baseX = sb.left + 12.f;
    float y = sb.top + 8.f - g_navScroll;

    for (size_t i = 0; i < g_navRows.size(); ++i)
    {
        NavNode* n = g_navRows[i].node;
        const int depth = g_navRows[i].depth;
        if (n->kind == 5)   // separator
        {
            const float ly = std::floor(y + 8.f) + 0.5f;
            dc->DrawLine({ sb.left + 16.f, ly }, { sb.right - 14.f, ly }, g_brLine.Get(), 1.f);
            y += 17.f;
            continue;
        }
        const D2D1_RECT_F r{ sb.left + 4.f, y, sb.right - 6.f, y + rowH };
        g_navRects[i] = r;

        if (y + rowH >= sb.top && y <= sb.bottom)   // cull rows scrolled out of view
        {
            const bool active = (n->path == cur);
            if (active)                    dc->FillRoundedRectangle({ r, 5, 5 }, g_brSelBg.Get());
            else if ((int)i == g_hoverNav) dc->FillRoundedRectangle({ r, 5, 5 }, g_brTabHover.Get());
            if (active) dc->FillRoundedRectangle({ { r.left + 1.f, r.top + 7.f, r.left + 4.f, r.bottom - 7.f }, 1.5f, 1.5f }, g_brAccent.Get());

            const float rowX = baseX + depth * indent;
            if (n->expandable)   // chevron (collapsed ▶ / expanded ▾)
            {
                const D2D1_RECT_F ch{ rowX - 3.f, y, rowX + 14.f, y + rowH };
                g_navChevRects[i] = ch;
                dc->DrawText(n->expanded ? L"\xE70D" : L"\xE76C", 1, g_chevFmt.Get(), ch,
                    ((int)i == g_hoverNav || active) ? g_brText.Get() : g_brText2.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }

            const float ix = rowX + 16.f, iy = y + (rowH - 16.f) * 0.5f;
            ID2D1Bitmap* bmp = (n->kind == 3) ? g_icons->GetForParseName(dc, kHomeParse, false)
                             : (n->kind == 4) ? g_icons->GetForParseName(dc, kThisPCParse, false)
                             :                  g_icons->GetForPath(dc, n->path, false);
            if (bmp) dc->DrawBitmap(bmp, { ix, iy, ix + 16, iy + 16 });
            else if (n->kind == 3) dc->DrawText(L"\xE80F", 1, g_fluentFmt.Get(), { ix - 2, y, ix + 18, y + rowH }, g_brText.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

            dc->DrawText(n->label.c_str(), (UINT32)n->label.size(), g_navFmt.Get(),
                { ix + 22.f, y, sb.right - 12.f, y + rowH }, g_brText.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        y += rowH;
    }

    const float totalH = (y + g_navScroll) - (sb.top + 8.f) + 10.f;
    g_navMaxScroll = (std::max)(0.f, totalH - (sb.bottom - sb.top));
    if (g_navScroll > g_navMaxScroll) g_navScroll = g_navMaxScroll;
    dc->PopAxisAlignedClip();
}

void DrawDetails(ID2D1DeviceContext* dc)
{
    const D2D1_RECT_F dp = DetailsRc();
    dc->FillRectangle(dp, g_brStrip.Get());
    dc->DrawLine({ dp.left + 0.5f, dp.top }, { dp.left + 0.5f, dp.bottom }, g_brLine.Get(), 1.f);
    dc->PushAxisAlignedClip(dp, D2D1_ANTIALIAS_MODE_ALIASED);

    const float pad = 22.f, L = dp.left + pad, R = dp.right - pad;
    Tab& t = AT();
    std::vector<size_t> sels = t.view ? t.view->SelectedIndices() : std::vector<size_t>{};

    if (IsHome(t) || sels.empty())
    {
        const wchar_t* msg = L"Select a file or folder to see its details.";
        dc->DrawText(msg, (UINT32)wcslen(msg), g_navFmt.Get(),
            { L, dp.top + 28.f, R, dp.top + 120.f }, g_brText2.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        dc->PopAxisAlignedClip();
        return;
    }
    if (sels.size() > 1)
    {
        std::wstring head = std::to_wstring(sels.size()) + L" items selected";
        dc->DrawText(head.c_str(), (UINT32)head.size(), g_detHeadFmt.Get(),
            { L, dp.top + 28.f, R, dp.top + 60.f }, g_brText.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        dc->PopAxisAlignedClip();
        return;
    }

    const FileEntry& e = t.model->At(sels[0]);
    const std::wstring full = e.isDrive ? e.target : Join(t.path, e.name);
    const float cx = (dp.left + dp.right) * 0.5f;

    // Large icon, centred near the top: prefer the crisp 48px extra-large shell
    // icon (colour known-folder icons); fall back to the kind/path icon.
    ID2D1Bitmap* bmp = g_icons->GetLargeForPath(dc, full);
    if (!bmp) bmp = (e.isFolder || e.isDrive) ? g_icons->GetForPath(dc, full, true)
                                              : g_icons->Get(dc, e, true);
    float y = dp.top + 30.f;
    if (bmp)
    {
        const D2D1_SIZE_F px = bmp->GetSize();   // draw 1:1 at native size (up to 48)
        const float s = (std::min)(48.f, (std::max)(px.width, px.height));
        dc->DrawBitmap(bmp, { cx - s * 0.5f, y, cx + s * 0.5f, y + s });
    }
    y += 62.f;

    // Name (up to two wrapped lines, centred).
    {
        ComPtr<IDWriteTextLayout> nl;
        if (SUCCEEDED(g_gfx.DWrite()->CreateTextLayout(e.name.c_str(), (UINT32)e.name.size(),
                g_detHeadFmt.Get(), R - L, 60.f, &nl)))
        {
            nl->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            DWRITE_TEXT_METRICS m{}; nl->GetMetrics(&m);
            dc->DrawTextLayout({ L, y }, nl.Get(), g_brText.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            y += (std::min)(m.height, 48.f) + 14.f;
        }
    }
    dc->DrawLine({ L, y }, { R, y }, g_brLine.Get(), 1.f);
    y += 14.f;

    // Metadata rows.
    auto row = [&](const wchar_t* key, const std::wstring& val) {
        if (val.empty()) return;
        dc->DrawText(key, (UINT32)wcslen(key), g_detKeyFmt.Get(),
            { L, y, L + 92.f, y + 20.f }, g_brText2.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        dc->DrawText(val.c_str(), (UINT32)val.size(), g_navFmt.Get(),
            { L + 96.f, y, R, y + 20.f }, g_brText.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        y += 26.f;
    };
    row(L"Type", e.typeText);
    if (!e.isFolder && !e.isDrive) row(L"Size", e.sizeText);
    row(L"Modified", e.dateText);
    dc->PopAxisAlignedClip();
}

// The Home landing page: Quick access + Devices cards. Drawn into a pane's list
// rect (below its tab strip) when the active tab is at the root. Hit rects are
// appended to g_homeCardRects/g_homeCardNav (cleared once per frame in RenderFrame).
void DrawHome(ID2D1DeviceContext* dc, const Rc& area)
{
    const float pad = 24.f;
    const float x0 = area.l + pad;
    const float innerW = (area.r - area.l) - pad * 2.f;
    float y = area.t + pad;

    auto section = [&](const wchar_t* title) {
        dc->DrawText(title, (UINT32)wcslen(title), g_homeHeadFmt.Get(),
            { x0, y, area.r - pad, y + 30.f }, g_brText.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        y += 42.f;
    };
    auto cards = [&](const std::vector<std::pair<std::wstring, std::wstring>>& items) {
        const float cardH = 60.f, gap = 12.f, cardMin = 200.f;
        const int cols = (std::max)(1, (int)((innerW + gap) / (cardMin + gap)));
        const float cw = (innerW - gap * (cols - 1)) / cols;
        int col = 0; float rowY = y;
        for (const auto& it : items)
        {
            const float cxp = x0 + col * (cw + gap);
            const D2D1_RECT_F r{ cxp, rowY, cxp + cw, rowY + cardH };
            const bool hov = ((int)g_homeCardRects.size() == g_hoverCard);
            dc->FillRoundedRectangle({ r, 8, 8 }, hov ? g_brTabHover.Get() : g_brCtrl.Get());
            dc->DrawRoundedRectangle({ r, 8, 8 }, g_brLine.Get(), 1.f);
            const float iy = rowY + (cardH - 32.f) * 0.5f;
            if (ID2D1Bitmap* bmp = g_icons->GetForPath(dc, it.second, true))
                dc->DrawBitmap(bmp, { cxp + 14.f, iy, cxp + 46.f, iy + 32.f });
            dc->DrawText(it.first.c_str(), (UINT32)it.first.size(), g_navFmt.Get(),
                { cxp + 58.f, rowY, r.right - 10.f, rowY + cardH }, g_brText.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            g_homeCardRects.push_back(r); g_homeCardPaths.push_back(it.second);
            if (++col >= cols) { col = 0; rowY += cardH + gap; }
        }
        if (col != 0) rowY += cardH + gap;
        y = rowY + 10.f;
    };

    section(L"Quick access");
    cards(g_quickAccess);
    y += 6.f;
    section(L"Devices and drives");
    cards(g_drives);
}

void RenderFrame(bool caretOn, bool animating)
{
    ID2D1DeviceContext* dc = g_gfx.BeginFrame();
    EnsureMainBrushes(dc);
    g_thumbs->PumpResults(dc);     // upload any thumbnails finished since last frame
    dc->Clear(g_theme.windowBg);

    g_thumbs->BeginFrame();        // collect visible thumbnail keys across all panes
    g_homeCardRects.clear(); g_homeCardPaths.clear();
    for (int i = 0; i < VisiblePanes(); ++i)
    {
        Pane& p = g_pane[i];
        if (p.tabs.empty()) continue;
        Rc lr = ListRc(i);
        D2D1_RECT_F clip{ lr.l, lr.t, lr.r, lr.b };
        if (IsHome(*p.tabs[p.active]))    // Home landing page instead of the file list
        {
            dc->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
            DrawHome(dc, lr);
            dc->PopAxisAlignedClip();
        }
        else
        {
            dc->SetTransform(D2D1::Matrix3x2F::Identity());
            dc->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
            dc->SetTransform(D2D1::Matrix3x2F::Translation(lr.l, lr.t));
            p.tabs[p.active]->view->Render(dc);
            dc->SetTransform(D2D1::Matrix3x2F::Identity());
            dc->PopAxisAlignedClip();
        }
        DrawStrip(dc, i);
    }
    g_thumbs->EndFrame();          // worker may now drop off-screen requests

    if (g_sidebar) DrawSidebar(dc);
    if (g_details) DrawDetails(dc);
    if (g_dual)   // splitter (brighter while dragging)
    {
        const float sx = SplitX();
        dc->DrawLine({ sx + 0.5f, ContentTop() }, { sx + 0.5f, ContentBottom() },
            g_draggingSplit ? g_brAccent.Get() : g_brLine.Get(), g_draggingSplit ? 2.f : 1.f);
    }

    // Chrome (toolbar + address + status) reflects the active tab.
    Tab& at = AT();
    ChromeState cs;
    cs.segments = BuildSegments(at.path);
    cs.backEnabled = !at.back.empty();
    cs.fwdEnabled = !at.forward.empty();
    cs.upEnabled = !at.path.empty();
    cs.dualOn = g_dual;
    cs.detailsOn = g_details;
    cs.hoverButton = g_hoverButton;
    cs.editing = g_addr.active; cs.editText = g_addr.text; cs.caret = g_addr.caret; cs.selAll = g_addr.selAll; cs.caretOn = caretOn;

    const size_t count = at.model->Count();
    if (!at.error.empty()) cs.statusLeft = at.error;
    else if (at.searchMode)
    {
        cs.statusLeft = std::to_wstring(count) + L" results";
        if (at.loading) cs.statusLeft = L"Searching…  " + cs.statusLeft;
        const size_t seln = at.view->SelectedCount();
        if (seln) cs.statusLeft += L"      " + std::to_wstring(seln) + L" selected";
    }
    else
    {
        cs.statusLeft = std::to_wstring(count) + L" items";
        const size_t folders = at.model->FolderCount();
        if (folders) cs.statusLeft += L"  (" + std::to_wstring(folders) + L" folders)";
        if (at.loading) cs.statusLeft = L"Loading…  " + cs.statusLeft;
        const size_t seln = at.view->SelectedCount();
        if (seln) cs.statusLeft += L"      " + std::to_wstring(seln) + L" selected";
    }
    {
        const float ms = AvgPacing();
        wchar_t r[128];
        if (ms > 0) std::swprintf(r, std::size(r), L"%.0f fps  ·  %s  ·  %s%s", 1000.f / ms, at.view->ModeName(), at.view->DensityName(), g_dual ? L"  ·  dual" : L"");
        else        std::swprintf(r, std::size(r), L"%s  ·  %s%s", at.view->ModeName(), at.view->DensityName(), g_dual ? L"  ·  dual" : L"");
        cs.statusRight = r;
    }
    (void)animating;
    g_chrome.Draw(dc, ViewW(), ViewH(), cs);
    DrawRename(dc, caretOn);

    // Search box (active input or passive chip).
    if (g_pal.open && g_pal.mode == 1) DrawFilterBar(dc, /*editing*/ true, caretOn);
    else if (!AT().model->Filter().empty()) DrawFilterBar(dc, /*editing*/ false, caretOn);

    // Modal overlay (command palette / settings) with a fade+slide reveal.
    if (g_overlayKind != 0 && g_overlayAnim > 0.01f)
    {
        D2D1_LAYER_PARAMETERS1 lp{};
        lp.contentBounds = D2D1::InfiniteRect();
        lp.maskAntialiasMode = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
        lp.maskTransform = D2D1::Matrix3x2F::Identity();
        lp.opacity = g_overlayAnim;
        lp.layerOptions = D2D1_LAYER_OPTIONS1_NONE;
        dc->PushLayer(lp, nullptr);
        if (g_overlayKind == 1) DrawPalette(dc, ViewW(), ViewH(), caretOn, g_overlayAnim);
        else if (g_overlayKind == 2) DrawSettings(dc, ViewW(), ViewH(), g_overlayAnim);
        dc->PopLayer();
    }

    g_gfx.EndFrame(g_vsync);
}

// ================================================================== window ==

void ApplyWindowChrome(HWND hwnd)
{
    BOOL dark = g_theme.dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    int backdrop = DWMSBT_MAINWINDOW;
    DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
}

// Translate a client point (px) into a pane's list-local px, for the view.
bool ListLocal(int paneIdx, int xPx, int yPx, int* lx, int* ly)
{
    const float dpi = g_gfx.Dpi();
    Rc lr = ListRc(paneIdx);
    *lx = xPx - static_cast<int>(DipToPx(lr.l, dpi));
    *ly = yPx - static_cast<int>(DipToPx(lr.t, dpi));
    return true;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto dipX = [&](int px) { return PxToDip(static_cast<float>(px), g_gfx.Dpi()); };
    auto dipY = [&](int px) { return PxToDip(static_cast<float>(px), g_gfx.Dpi()); };

    switch (msg)
    {
    case WM_APP_ENUM: PumpEnumEvents(); return 0;
    case WM_APP_THUMB: g_dirty = true; return 0;   // ready thumbnails uploaded next frame

    case WM_SIZE:
        if (g_pane[0].tabs.size())
        {
            g_gfx.Resize(LOWORD(lParam), HIWORD(lParam));
            SetViewports();
            g_dirty = true;
        }
        return 0;

    case WM_DPICHANGED:
    {
        const float dpi = static_cast<float>(HIWORD(wParam));
        RECT* r = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        g_gfx.SetDpi(dpi);
        SetViewports();
        g_dirty = true;
        return 0;
    }

    case WM_MOUSEWHEEL:
    {
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd, &pt);
        // Scroll the navigation sidebar when the cursor is over it.
        if (g_sidebar && dipX(pt.x) < kSidebarW && dipY(pt.y) >= ContentTop() && dipY(pt.y) < ContentBottom())
        {
            g_navScroll = std::clamp(g_navScroll - GET_WHEEL_DELTA_WPARAM(wParam) / 120.f * 48.f, 0.f, g_navMaxScroll);
            g_dirty = true;
            return 0;
        }
        int pane = PaneAt(dipX(pt.x), dipY(pt.y));
        if (pane < 0) pane = g_activePane;
        if (!g_pane[pane].tabs.empty() && g_pane[pane].tabs[g_pane[pane].active]->view->OnWheel(GET_WHEEL_DELTA_WPARAM(wParam))) g_dirty = true;
        return 0;
    }

    case WM_MOUSEMOVE:
    {
        if (!g_mouseTracked) { TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 }; TrackMouseEvent(&tme); g_mouseTracked = true; }
        const int xp = GET_X_LPARAM(lParam), yp = GET_Y_LPARAM(lParam);
        const float dx = dipX(xp), dy = dipY(yp);
        if (g_draggingSplit)
        {
            g_splitRatio = std::clamp((dx - ContentLeft()) / ContentW(), 0.12f, 0.88f);
            SetViewports(); g_dirty = true;
            return 0;
        }
        if (g_colResizePane >= 0)   // dragging a column divider
        {
            int lx, ly; ListLocal(g_colResizePane, xp, yp, &lx, &ly);
            g_pane[g_colResizePane].tabs[g_pane[g_colResizePane].active]->view->ResizeColumnTo(g_colResizeWhich, lx);
            g_dirty = true;
            return 0;
        }
        if (g_listCapture >= 0)   // marquee drag
        {
            int lx, ly; ListLocal(g_listCapture, xp, yp, &lx, &ly);
            g_pane[g_listCapture].tabs[g_pane[g_listCapture].active]->view->MarqueeMove(lx, ly);
            g_dirty = true;
            return 0;
        }
        if (g_dragArm && (wParam & MK_LBUTTON) &&
            (std::abs(xp - g_dragStartX) > GetSystemMetrics(SM_CXDRAG) ||
             std::abs(yp - g_dragStartY) > GetSystemMetrics(SM_CYDRAG)))
        {
            g_dragArm = false;
            BeginDrag(g_dragPane);   // modal OLE drag; returns after drop
            return 0;
        }
        int newBtn = 0, hp = -1, ht = -1;
        if (dy < ContentTop()) newBtn = g_chrome.HitButton(dx, dy);
        else
        {
            for (int i = 0; i < VisiblePanes(); ++i)
            {
                Rc st = StripRc(i);
                if (dx >= st.l && dx < st.r && dy >= st.t && dy < st.b)
                {
                    std::vector<D2D1_RECT_F> rects; D2D1_RECT_F nb; TabLayout(i, rects, nb);
                    for (size_t t = 0; t < rects.size(); ++t) if (dx >= rects[t].left && dx <= rects[t].right) { hp = i; ht = (int)t; break; }
                    break;
                }
            }
        }
        if (newBtn != g_hoverButton) { g_hoverButton = newBtn; g_dirty = true; }
        if (hp != g_hoverTab.pane || ht != g_hoverTab.tab) { g_hoverTab.pane = hp; g_hoverTab.tab = ht; g_dirty = true; }

        // Sidebar + Home-card hover.
        int newNav = -1;
        if (g_sidebar && dx < kSidebarW && dy >= ContentTop() && dy < ContentBottom())
        {
            const size_t nrows = (std::min)(g_navRows.size(), g_navRects.size());
            for (size_t i = 0; i < nrows; ++i)
            {
                if (g_navRows[i].node->kind == 5) continue;
                const auto& r = g_navRects[i];
                if (dx >= r.left && dx <= r.right && dy >= r.top && dy <= r.bottom) { newNav = (int)i; break; }
            }
        }
        if (newNav != g_hoverNav) { g_hoverNav = newNav; g_dirty = true; }
        int newCard = -1;
        if (dy >= ContentTop())
            for (size_t i = 0; i < g_homeCardRects.size(); ++i)
            { const auto& r = g_homeCardRects[i]; if (dx >= r.left && dx <= r.right && dy >= r.top && dy <= r.bottom) { newCard = (int)i; break; } }
        if (newCard != g_hoverCard) { g_hoverCard = newCard; g_dirty = true; }

        int pane = PaneAt(dx, dy);
        if (pane >= 0 && dy >= ListRc(pane).t)
        {
            int lx, ly; ListLocal(pane, xp, yp, &lx, &ly);
            if (g_pane[pane].tabs[g_pane[pane].active]->view->OnMouseMove(lx, ly)) g_dirty = true;
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        g_mouseTracked = false; g_hoverButton = 0; g_hoverTab = { -1, -1 }; g_hoverNav = -1; g_hoverCard = -1;
        for (int i = 0; i < VisiblePanes(); ++i) if (!g_pane[i].tabs.empty()) g_pane[i].tabs[g_pane[i].active]->view->OnMouseLeave();
        g_dirty = true;
        return 0;

    case WM_LBUTTONDOWN:
    {
        SetFocus(hwnd);
        const int xp = GET_X_LPARAM(lParam), yp = GET_Y_LPARAM(lParam);
        const float dx = dipX(xp), dy = dipY(yp);
        auto inRect = [&](const D2D1_RECT_F& r) { return dx >= r.left && dx <= r.right && dy >= r.top && dy <= r.bottom; };

        // Settings overlay intercepts clicks.
        if (g_settingsOpen)
        {
            for (const SettHit& h : g_settHits) if (inRect(h.rect)) { ApplySettingHit(h.action, h.value); return 0; }
            if (!inRect(g_settPanelRect)) CloseSettings();
            return 0;
        }
        // Inline rename: click inside keeps editing; outside commits.
        if (g_renaming)
        {
            if (inRect(g_renameRectDip)) return 0;
            CommitRename();
            return 0;
        }
        // Start dragging the pane splitter.
        if (OverSplitter(dx, dy)) { g_draggingSplit = true; SetCapture(hwnd); return 0; }
        // Modal command/license palette intercepts clicks.
        if (g_pal.open && g_pal.mode != 1)
        {
            if (inRect(g_pal.panelRect))
            {
                for (size_t i = 0; i < g_pal.itemRects.size(); ++i)
                    if (inRect(g_pal.itemRects[i])) { int cmd = g_pal.items[i].cmd; if (cmd == CMD_FILTER) OpenPalette(1); else { ClosePalette(); RunCommand(cmd); } break; }
            }
            else ClosePalette();
            return 0;
        }
        // Filter clear (×), for the open input or a passive filter chip.
        if (((g_pal.open && g_pal.mode == 1) || !AT().model->Filter().empty()) && inRect(g_pal.filterClearRect))
        { ClearFilter(); if (g_pal.open && g_pal.mode == 1) ClosePalette(); return 0; }
        if (g_pal.open && g_pal.mode == 1) ClosePalette();   // click elsewhere closes input, keeps filter

        if (dy < ContentTop())
        {
            const int btn = g_chrome.HitButton(dx, dy);
            if (btn == 1) GoBack();
            else if (btn == 2) GoForward();
            else if (btn == 3) GoUp();
            else if (btn == 4) ToggleDual();
            else if (btn == 5) OpenSettings();
            else if (btn == 6) { g_details = !g_details; SetViewports(); g_dirty = true; }
            else
            {
                std::wstring segPath;
                const int a = g_chrome.HitAddress(dx, dy, &segPath);
                if (a == 1) { g_addr.Cancel(); NavigateTab(g_activePane, &AT(), segPath); }
                else if (a == 2) BeginAddressEdit();
            }
            return 0;
        }
        // Left navigation sidebar: chevron toggles expand; row navigates.
        if (g_sidebar && dx < kSidebarW && dy >= ContentTop() && dy < ContentBottom())
        {
            // Hit rects are (re)sized during the sidebar draw; a click that races a
            // row-count change must not index past whichever is shorter.
            const size_t nrows = (std::min)(g_navRows.size(), g_navRects.size());
            for (size_t i = 0; i < nrows; ++i)
            {
                NavNode* n = g_navRows[i].node;
                const auto& cr = g_navChevRects[i];
                if (cr.right > cr.left && dx >= cr.left && dx <= cr.right && dy >= cr.top && dy <= cr.bottom)
                { ToggleNav(*n); return 0; }
                const auto& r = g_navRects[i];
                if (n->kind != 5 && dx >= r.left && dx <= r.right && dy >= r.top && dy <= r.bottom)
                { g_addr.Cancel(); NavigateTab(g_activePane, &AT(), n->path); return 0; }
            }
            return 0;
        }
        // Details pane is non-interactive; swallow clicks so they don't hit a pane.
        if (g_details && dx >= ViewW() - kDetailsW && dy >= ContentTop() && dy < ContentBottom())
            return 0;
        // tab strips
        for (int i = 0; i < VisiblePanes(); ++i)
        {
            Rc st = StripRc(i);
            if (dx >= st.l && dx < st.r && dy >= st.t && dy < st.b)
            {
                std::vector<D2D1_RECT_F> rects; D2D1_RECT_F nb; TabLayout(i, rects, nb);
                if (dx >= nb.left && dx <= nb.right) { g_activePane = i; NewTab(i, AT().path, true); return 0; }
                for (size_t t = 0; t < rects.size(); ++t)
                    if (dx >= rects[t].left && dx <= rects[t].right)
                    {
                        if (g_pane[i].tabs.size() > 1 && dx >= rects[t].right - 22) CloseTab(i, t);
                        else SwitchTab(i, t);
                        return 0;
                    }
                return 0;
            }
        }
        // list area
        int pane = PaneAt(dx, dy);
        if (pane >= 0 && dy >= ListRc(pane).t)
        {
            if (g_addr.active) { g_addr.Cancel(); g_dirty = true; }
            ActivatePane(pane);
            if (IsHome(AT()))   // Home page cards
            {
                for (size_t i = 0; i < g_homeCardRects.size(); ++i)
                {
                    const auto& r = g_homeCardRects[i];
                    if (dx >= r.left && dx <= r.right && dy >= r.top && dy <= r.bottom)
                    { NavigateTab(pane, &AT(), g_homeCardPaths[i]); break; }
                }
                return 0;
            }
            int lx, ly; ListLocal(pane, xp, yp, &lx, &ly);
            const int div = AT().view->HeaderDividerAt(lx, ly);
            if (div >= 0) { g_colResizePane = pane; g_colResizeWhich = div; SetCapture(hwnd); return 0; }
            if (AT().view->OnLButtonDown(lx, ly)) g_dirty = true;
            if (AT().view->MarqueeActive()) { SetCapture(hwnd); g_listCapture = pane; }
            else if (AT().view->HitRow(lx, ly) >= 0)   // clicked a (now selected) item → arm drag
            { g_dragArm = true; g_dragStartX = xp; g_dragStartY = yp; g_dragPane = pane; }
        }
        return 0;
    }

    case WM_LBUTTONUP:
        g_dragArm = false;
        if (g_colResizePane >= 0) { g_colResizePane = -1; ReleaseCapture(); g_dirty = true; }
        else if (g_listCapture >= 0)
        {
            g_pane[g_listCapture].tabs[g_pane[g_listCapture].active]->view->EndMarquee();
            ReleaseCapture(); g_listCapture = -1; g_dirty = true;
        }
        else if (g_draggingSplit) { g_draggingSplit = false; ReleaseCapture(); g_dirty = true; }
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && g_curSizeWE)
        {
            POINT pt{}; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
            const float mx = dipX(pt.x), my = dipY(pt.y);
            bool resize = g_draggingSplit || g_colResizePane >= 0 || OverSplitter(mx, my);
            if (!resize)
            {
                int p = PaneAt(mx, my);
                if (p >= 0 && !g_pane[p].tabs.empty())
                {
                    int lx, ly; ListLocal(p, pt.x, pt.y, &lx, &ly);
                    if (g_pane[p].tabs[g_pane[p].active]->view->HeaderDividerAt(lx, ly) >= 0) resize = true;
                }
            }
            if (resize) { SetCursor(g_curSizeWE); return TRUE; }
        }
        break;

    case WM_LBUTTONDBLCLK:
    {
        const int xp = GET_X_LPARAM(lParam), yp = GET_Y_LPARAM(lParam);
        int pane = PaneAt(dipX(xp), dipY(yp));
        if (pane >= 0 && dipY(yp) >= ListRc(pane).t)
        {
            ActivatePane(pane);
            int lx, ly; ListLocal(pane, xp, yp, &lx, &ly);
            AT().view->OnDoubleClick(lx, ly);
        }
        return 0;
    }

    case WM_CONTEXTMENU:
    {
        int sx = GET_X_LPARAM(lParam), sy = GET_Y_LPARAM(lParam);
        POINT screen{ sx, sy };
        const bool fromKeyboard = (sx == -1 && sy == -1);
        Tab* t = &AT();
        int pane = g_activePane;

        if (fromKeyboard)
        {
            POINT a{};
            if (t->view->SelectedAnchorPx(&a))
            {
                Rc lr = ListRc(pane);
                a.x += static_cast<int>(DipToPx(lr.l, g_gfx.Dpi()));
                a.y += static_cast<int>(DipToPx(lr.t, g_gfx.Dpi()));
                ClientToScreen(hwnd, &a); screen = a;
            }
            else { RECT rc{}; GetWindowRect(hwnd, &rc); screen = { (rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2 }; }
        }
        else
        {
            POINT cp{ sx, sy }; ScreenToClient(hwnd, &cp);
            pane = PaneAt(dipX(cp.x), dipY(cp.y));
            if (pane < 0 || dipY(cp.y) < ListRc(pane).t) return DefWindowProcW(hwnd, msg, wParam, lParam);
            ActivatePane(pane); t = &AT();
            int lx, ly; ListLocal(pane, cp.x, cp.y, &lx, &ly);
            const int row = t->view->HitRow(lx, ly);
            if (row < 0) t->view->SetSelected(-1);
            else if (!t->view->IsSelected(row)) t->view->SetSelected(row);
            g_dirty = true;
        }

        const std::vector<size_t> sels = t->view->SelectedIndices();
        if (sels.empty()) { if (!t->path.empty()) g_ctx.ShowForFolder(hwnd, t->path, screen); }
        else if (t->path.empty()) { const FileEntry& e = t->model->At(sels[0]); g_ctx.ShowForItem(hwnd, e.isDrive ? e.target : e.name, screen); }
        else { std::vector<std::wstring> names; for (size_t i : sels) names.push_back(t->model->At(i).name); g_ctx.ShowForItems(hwnd, t->path, names, screen); }
        g_dirty = true;
        return 0;
    }

    case WM_INITMENUPOPUP: case WM_DRAWITEM: case WM_MEASUREITEM: case WM_MENUCHAR:
    {
        LRESULT r = 0;
        if (g_ctx.HandleMenuMsg(msg, wParam, lParam, &r)) return r;
        break;
    }

    case WM_SYSKEYDOWN:
        if (wParam == VK_LEFT) { GoBack(); return 0; }
        if (wParam == VK_RIGHT) { GoForward(); return 0; }
        break;

    case WM_CHAR:
        if (g_renaming) { g_ren.Char(static_cast<wchar_t>(wParam)); g_dirty = true; return 0; }
        if (g_pal.open) { wchar_t c = static_cast<wchar_t>(wParam); if (c >= 0x20) { g_pal.input.insert(g_pal.caret, 1, c); ++g_pal.caret; OnPaletteInputChanged(); } return 0; }
        if (g_addr.active) { g_addr.Char(static_cast<wchar_t>(wParam)); g_dirty = true; }
        return 0;

    case WM_KEYDOWN:
    {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (g_settingsOpen) { if (wParam == VK_ESCAPE) CloseSettings(); return 0; }
        if (g_renaming)   // inline rename owns the keyboard while active
        {
            switch (wParam)
            {
            case VK_RETURN: CommitRename(); return 0;
            case VK_ESCAPE: CancelRename(); return 0;
            case VK_BACK: g_ren.Backspace(); g_dirty = true; return 0;
            case VK_DELETE: g_ren.Del(); g_dirty = true; return 0;
            case VK_LEFT: g_ren.Left(); g_dirty = true; return 0;
            case VK_RIGHT: g_ren.Right(); g_dirty = true; return 0;
            case VK_HOME: g_ren.Home(); g_dirty = true; return 0;
            case VK_END: g_ren.End(); g_dirty = true; return 0;
            case 'A': if (ctrl) { g_ren.SelectAll(); g_dirty = true; } return 0;
            case 'V': if (ctrl) { g_ren.Paste(hwnd); g_dirty = true; } return 0;
            default: return 0;
            }
        }
        if (g_pal.open)   // palette owns the keyboard while open
        {
            switch (wParam)
            {
            case VK_ESCAPE:
                if (g_pal.mode == 1) { if (g_searchArmed) g_searchArmed = false; else ClearFilter(); }
                ClosePalette(); return 0;
            case VK_RETURN:
                if (g_pal.mode == 0) { if (!g_pal.items.empty()) { int cmd = g_pal.items[g_pal.sel].cmd; if (cmd == CMD_FILTER) { OpenPalette(1); return 0; } ClosePalette(); RunCommand(cmd); } else ClosePalette(); }
                else if (g_pal.mode == 1) { if (g_searchArmed) { g_searchArmed = false; std::wstring q = g_pal.input; ClosePalette(); RunRecursiveSearch(q); } else ClosePalette(); }
                else if (g_pal.mode == 2) { if (ImportLicenseInteractive(hwnd)) { g_pro = true; OpenPalette(0); } else { g_pal.message = L"That license file couldn't be verified."; g_dirty = true; } }
                return 0;
            case VK_BACK:   if (g_pal.caret > 0) { g_pal.input.erase(g_pal.caret - 1, 1); --g_pal.caret; OnPaletteInputChanged(); } return 0;
            case VK_DELETE: if (g_pal.caret < g_pal.input.size()) { g_pal.input.erase(g_pal.caret, 1); OnPaletteInputChanged(); } return 0;
            case VK_LEFT:   if (g_pal.caret > 0) --g_pal.caret; g_dirty = true; return 0;
            case VK_RIGHT:  if (g_pal.caret < g_pal.input.size()) ++g_pal.caret; g_dirty = true; return 0;
            case VK_HOME:   g_pal.caret = 0; g_dirty = true; return 0;
            case VK_END:    g_pal.caret = g_pal.input.size(); g_dirty = true; return 0;
            case VK_UP:     if (g_pal.mode == 0 && g_pal.sel > 0) { --g_pal.sel; g_dirty = true; } return 0;
            case VK_DOWN:   if (g_pal.mode == 0 && g_pal.sel + 1 < (int)g_pal.items.size()) { ++g_pal.sel; g_dirty = true; } return 0;
            default: return 0;
            }
        }
        if (ctrl && shift && wParam == 'P') { OpenPalette(0); return 0; }
        if (ctrl && shift && wParam == 'F')   // recursive search (Pro)
        {
            if (!g_pro) OpenPalette(0);        // gates to the license prompt
            else { OpenPalette(1); g_searchArmed = true; g_pal.input.clear(); g_pal.caret = 0; }
            return 0;
        }
        if (ctrl && wParam == 'F') { OpenPalette(1); g_searchArmed = false; return 0; }
        if (ctrl && wParam == VK_OEM_COMMA) { OpenSettings(); return 0; }
        if (g_addr.active)
        {
            switch (wParam)
            {
            case VK_RETURN: CommitAddress(); return 0;
            case VK_ESCAPE: g_addr.Cancel(); g_dirty = true; return 0;
            case VK_BACK: g_addr.Backspace(); g_dirty = true; return 0;
            case VK_DELETE: g_addr.Del(); g_dirty = true; return 0;
            case VK_LEFT: g_addr.Left(); g_dirty = true; return 0;
            case VK_RIGHT: g_addr.Right(); g_dirty = true; return 0;
            case VK_HOME: g_addr.Home(); g_dirty = true; return 0;
            case VK_END: g_addr.End(); g_dirty = true; return 0;
            case 'A': if (ctrl) { g_addr.SelectAll(); g_dirty = true; } return 0;
            case 'V': if (ctrl) { g_addr.Paste(hwnd); g_dirty = true; } return 0;
            default: return 0;
            }
        }
        if (ctrl && wParam == 'T') { NewTab(g_activePane, AT().path, true); return 0; }
        if (ctrl && wParam == 'W') { CloseTab(g_activePane, AP().active); return 0; }
        if (ctrl && wParam == VK_TAB) { CycleTab(shift ? -1 : +1); return 0; }
        if (ctrl && wParam == 'L') { BeginAddressEdit(); return 0; }
        switch (wParam)
        {
        case VK_ESCAPE: DestroyWindow(hwnd); return 0;
        case 'V': g_vsync = !g_vsync; g_dirty = true; return 0;
        case VK_BACK: GoUp(); return 0;
        case VK_F2: StartRename(); return 0;
        case VK_F4: BeginAddressEdit(); return 0;
        case VK_F6: if (g_dual) ActivatePane(g_activePane ^ 1); return 0;
        case VK_F8: ToggleDual(); return 0;
        case VK_TAB: if (g_dual) ActivatePane(g_activePane ^ 1); return 0;
        default: if (AT().view->OnKey(wParam)) g_dirty = true; return 0;
        }
    }

    case WM_ERASEBKGND: return 1;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ================================================================ lifecycle ==

void SaveCurrentSession()
{
    SessionData sd;
    sd.dual = g_dual; sd.activePane = g_activePane; sd.split = g_splitRatio;
    for (int i = 0; i < 2; ++i)
    {
        SessionPane sp;
        sp.activeTab = static_cast<int>(g_pane[i].active);
        for (auto& t : g_pane[i].tabs) sp.tabs.push_back(t->path);
        sd.panes.push_back(std::move(sp));
    }
    SaveSession(sd);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HR(OleInitialize(nullptr), "OleInitialize");   // STA + OLE (needed for drag-drop)
    LoadSettings(g_settings);
    g_theme = Theme::Load(g_settings.theme);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"CrashWindow";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Crash", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800, nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return 1;
    ApplyWindowChrome(g_hwnd);
    g_curSizeWE = LoadCursorW(nullptr, IDC_SIZEWE);

    try
    {
        g_gfx.Initialize(g_hwnd);
        g_icons = std::make_unique<IconCache>();
        g_thumbs = std::make_unique<ThumbnailCache>(g_hwnd);
        g_chrome.Init(g_gfx.DWrite());
        g_ctx.onAfterInvoke = [] { StartLoad(g_activePane, &AT()); };
        for (int i = 0; i < 2; ++i) g_pane[i].enumr = std::make_unique<Enumerator>(g_hwnd);

        // Tab-strip formats.
        HR(g_gfx.DWrite()->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.5f, L"en-us", &g_tabFmt), "tabFmt");
        g_tabFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        g_tabFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        { DWRITE_TRIMMING tr{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 }; ComPtr<IDWriteInlineObject> sign; if (SUCCEEDED(g_gfx.DWrite()->CreateEllipsisTrimmingSign(g_tabFmt.Get(), &sign))) g_tabFmt->SetTrimming(&tr, sign.Get()); }
        HR(g_gfx.DWrite()->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.f, L"en-us", &g_glyphFmt), "glyphFmt");
        g_glyphFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        g_glyphFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        // Palette formats.
        HR(g_gfx.DWrite()->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 15.f, L"en-us", &g_palTitleFmt), "palTitle");
        g_palTitleFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        g_palTitleFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        HR(g_gfx.DWrite()->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16.f, L"en-us", &g_palInputFmt), "palInput");
        g_palInputFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        g_palInputFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

        // Sidebar / details / Home formats.
        auto mkfmt = [&](const wchar_t* fam, float size, DWRITE_FONT_WEIGHT w, bool vcenter, bool nowrap, ComPtr<IDWriteTextFormat>& out) {
            HR(g_gfx.DWrite()->CreateTextFormat(fam, nullptr, w, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &out), "fmt");
            if (vcenter) out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            if (nowrap) { out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                DWRITE_TRIMMING tr{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 }; ComPtr<IDWriteInlineObject> sign;
                if (SUCCEEDED(g_gfx.DWrite()->CreateEllipsisTrimmingSign(out.Get(), &sign))) out->SetTrimming(&tr, sign.Get()); }
        };
        mkfmt(L"Segoe UI Variable Text", 13.5f, DWRITE_FONT_WEIGHT_NORMAL,    true,  true,  g_navFmt);
        mkfmt(L"Segoe UI Variable Text", 11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, true,  true,  g_secFmt);
        mkfmt(L"Segoe Fluent Icons",     15.f,  DWRITE_FONT_WEIGHT_NORMAL,    true,  true,  g_fluentFmt);
        g_fluentFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        mkfmt(L"Segoe Fluent Icons",     8.f,   DWRITE_FONT_WEIGHT_NORMAL,    true,  true,  g_chevFmt);
        g_chevFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        mkfmt(L"Segoe UI Variable Display", 18.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, true, true, g_homeHeadFmt);
        mkfmt(L"Segoe UI Variable Text", 16.f,  DWRITE_FONT_WEIGHT_SEMI_BOLD, false, false, g_detHeadFmt);
        mkfmt(L"Segoe UI Variable Text", 12.f,  DWRITE_FONT_WEIGHT_NORMAL,    true,  true,  g_detKeyFmt);
    }
    catch (const std::exception& e) { MessageBoxA(g_hwnd, e.what(), "Crash — init failed", MB_ICONERROR); return 2; }

    g_pro = IsLicensed();
    BuildNav();   // sidebar + Home model (known folders + drives)

    // Restore session, or start fresh at the home folder.
    std::wstring home;
    { PWSTR pr = nullptr; if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &pr))) { home = pr; CoTaskMemFree(pr); } }
    g_home = home;

    SessionData sd;
    if (LoadSession(sd) && !sd.panes.empty() && !sd.panes[0].tabs.empty())
    {
        g_dual = sd.dual;
        g_splitRatio = std::clamp(sd.split, 0.12f, 0.88f);
        for (int i = 0; i < 2 && i < (int)sd.panes.size(); ++i)
        {
            for (const std::wstring& path : sd.panes[i].tabs) NewTab(i, path, false);
            if (!g_pane[i].tabs.empty()) g_pane[i].active = std::min<size_t>(sd.panes[i].activeTab, g_pane[i].tabs.size() - 1);
        }
        g_activePane = (sd.activePane == 1 && g_dual && !g_pane[1].tabs.empty()) ? 1 : 0;
    }
    if (g_pane[0].tabs.empty()) NewTab(0, home, false);

    ApplySettings();   // push density/view/thumbnails/hidden into views + enumerators
    SetViewports();
    for (int i = 0; i < VisiblePanes(); ++i) if (!g_pane[i].tabs.empty()) StartLoad(i, g_pane[i].tabs[g_pane[i].active].get());
    UpdateTitleBar();

    // Accept files dropped into a pane's folder (from Explorer or the other pane).
    InitDragDrop(g_hwnd, [](const std::vector<std::wstring>& paths, POINT screenPt, bool move) {
        POINT cp = screenPt; ScreenToClient(g_hwnd, &cp);
        const float dpi = g_gfx.Dpi();
        int pane = PaneAt(PxToDip((float)cp.x, dpi), PxToDip((float)cp.y, dpi));
        if (pane < 0) pane = g_activePane;
        Tab* t = g_pane[pane].tabs[g_pane[pane].active].get();
        if (t->path.empty()) return;                     // can't drop into This PC
        std::wstring dest = t->path;
        int lx, ly; ListLocal(pane, cp.x, cp.y, &lx, &ly);
        const int row = t->view->HitRow(lx, ly);
        if (row >= 0) { const FileEntry& e = t->model->At((size_t)row); if (e.isFolder && !e.isDrive) dest = Join(t->path, e.name); }
        CopyOrMoveFiles(g_hwnd, paths, dest, move);
        StartLoad(pane, t);
    });

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    LARGE_INTEGER freq, prev, lastRender;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);
    lastRender = prev;
    bool contiguous = false;

    MSG msg{};
    bool running = true;
    while (running)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;

        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        const double dt = double(now.QuadPart - prev.QuadPart) / freq.QuadPart;
        prev = now;

        bool animating = false;
        for (int i = 0; i < VisiblePanes(); ++i) if (!g_pane[i].tabs.empty()) animating |= g_pane[i].tabs[g_pane[i].active]->view->Animate(dt);
        const double seconds = double(now.QuadPart) / freq.QuadPart;
        const bool caretOn = std::fmod(seconds, 1.0) < 0.5;

        // Overlay reveal animation (§4 motion; respects reduced-motion).
        const bool overlayOpen = (g_pal.open && g_pal.mode != 1) || g_settingsOpen;
        const float tgt = overlayOpen ? 1.f : 0.f;
        if (!AnimationsEnabled()) g_overlayAnim = tgt;
        else { g_overlayAnim += (tgt - g_overlayAnim) * (1.f - std::exp(-static_cast<float>(dt) * 16.f)); }
        if (std::fabs(g_overlayAnim - tgt) < 0.004f) g_overlayAnim = tgt;
        if (g_overlayAnim <= 0.005f && tgt == 0.f) g_overlayKind = 0;
        const bool overlayLive = std::fabs(g_overlayAnim - tgt) > 0.001f || g_pal.open || g_settingsOpen;

        if (g_dirty || animating || g_addr.active || overlayLive || g_renaming)
        {
            if (contiguous && animating) PushPacing(float(double(now.QuadPart - lastRender.QuadPart) / freq.QuadPart * 1000.0));
            lastRender = now; contiguous = animating;
            RenderFrame(caretOn, animating);
            g_dirty = false;
        }
        else { contiguous = false; WaitMessage(); }
    }

    SaveCurrentSession();
    ShutdownDragDrop(g_hwnd);
    g_thumbs.reset();                                      // join thumbnail workers
    for (int i = 0; i < 2; ++i) g_pane[i].enumr.reset();   // join enumeration workers
    OleUninitialize();
    return 0;
}
