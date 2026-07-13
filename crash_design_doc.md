# Crash — Technical Design Doc

*A Windows file manager named after the thing it refuses to do.*

## 1. Vision & Positioning

Crash exists in the gap nobody has filled: **fast, stable, and visually modern, all three at once.**

| | Fast | Stable | Modern UI |
|---|---|---|---|
| File Explorer | ❌ | ❌ | partial |
| Directory Opus / Total Commander | ✅ | ✅ | ❌ (dated) |
| Files (files.community) | ❌ | ⚠️ | ✅ |
| **Crash** | ✅ | ✅ | ✅ |

Non-goals (at least for v1): plugin/scripting ecosystem depth to match 25 years of Directory Opus, FTP/cloud-protocol breadth, mobile/cross-platform support.

## 2. Business Model & Distribution

### 2.1 Model: Freemium, one-time Pro unlock, Microsoft Store as primary channel

- **Free tier**: the full core experience — native rendering, tabs, dual-pane, basic search, shell integration, theming. This is the adoption engine. In a category this trust-sensitive, a generous free tier is what earns the right to be someone's daily driver in the first place.
- **Pro tier (one-time purchase, not subscription)**: command palette + scripting/automation, advanced search/filtering, duplicate-finder/sync tools, and any cloud-protocol/FTP support added later. Unlocked via license key purchased in-app or through the Store listing.
- **Price point**: ~$15–25 one-time, undercutting XYplorer ($40–80 lifetime) and Directory Opus ($35–89 tiers) while still being a real, sustainable price — not a token "tip."

### 2.2 Maximizing the developer's cut

The Microsoft Store's current commercial terms are unusually favorable for this decision, and the model should be built around exploiting them fully:

- **Use our own commerce platform for the Pro unlock**, not Microsoft's in-app purchase pipe. Microsoft's terms let developers choose their own commerce platform and keep 100% of the revenue for non-gaming apps, versus a 15% fee if routed through Microsoft's own commerce system. A self-hosted license-key store (e.g. a lightweight Stripe/Paddle-backed checkout, validated by the app at launch) keeps the full purchase price.
- **List the base app free on the Store** — individual developer accounts publish for free, so there's no listing-fee friction to offset.
- If for simplicity for the initial launch the Pro unlock *is* routed through Microsoft's commerce platform instead, the fee is still just 15% (85% developer share) — far better than the 30% norm on the Apple/Google stores, and Microsoft has been pushing this developer share even higher (up to 85–95%, tiered by discovery channel) for non-gaming apps. Either path is acceptable; self-hosted commerce is simply the higher-margin option once volume justifies the extra engineering.
- **Avoid the "free on GitHub, paid on the Store" pattern** Files uses. It muddies the pricing story (is it free or isn't it?) and functions more like a tip jar than a monetization model. A clean free/Pro split, marketed consistently everywhere Crash is distributed, is both clearer to users and more sustainable for funding continued development.

### 2.3 Licensing: MIT, open source on GitHub — open-core structure

Ship the **core engine as MIT-licensed, open source on GitHub**: the renderer, enumeration pipeline, base UI, shell integration layer. The Pro-tier features (scripting engine, advanced tools) can live in a separate, closed-source module that the open core loads when a valid license is present — a standard "open core" split.

Why open source, given this is a trust-and-access-heavy category:

- **Trust is the actual product here.** A file manager gets full read/write access to someone's entire filesystem — a materially higher trust bar than most software categories. Files' own user reviews specifically cite "anyone can review the source, no hidden backdoors" as a reason they're comfortable installing it. That's not a generic open-source talking point; it's specific to what this class of app is asking users to hand over.
- **The architecture isn't a durable secret anyway.** A custom Direct2D renderer over Win32 isn't unprecedented — Windows Terminal already proved the pattern publicly. Once Crash ships and people notice it doesn't lag, a competent Windows engineer can infer the broad shape of the approach from behavior alone. Closed source would protect the literal code from copy-paste, but not the idea, which is the part that actually generalizes. Execution and continued investment are the real moat, not the header files.
- **Free contributor labor is a real, proven effect at this scale.** Files has accumulated 300+ contributors under its MIT license. Bug fixes, shell-extension edge cases, and localization are exactly the kind of long-tail work a small core team can't fully staff alone — open source turns that into community capacity instead of a permanent backlog.

Risk to name explicitly: an MIT core means Files itself (or anyone else) could legally fork or copy Crash's rendering approach once it's public and working. This is an accepted tradeoff — see §7 for why we're betting on execution speed and brand over secrecy.

## 3. Core Architectural Decision

**No XAML/WinUI in the hot path.** The main content surface (file list/grid) is a custom-rendered control on **Direct2D + DirectWrite**, hosted in a plain **Win32 window**. This is the same pattern Windows Terminal uses for its text grid: heavy, frequently-redrawn content gets a hand-written DirectX renderer; anything else (settings dialogs, first-run screens) can use higher-level tooling because it isn't perf-critical.

Language: **C++20**, or **Rust** with `windows-rs` bindings as the memory-safety-focused alternative. (Decision needed before Phase 1 — see §9.)

Why not WinUI 3: as of the 2026 "Windows K2" performance push, Microsoft's own benchmarks show ~25% improvement to WinUI-coded portions of Explorer, opt-in only, with WebView2-hosting scenarios still explicitly unresolved until 2027. That's real progress but nowhere near the ceiling we're targeting, and it isn't the default behavior of the framework — we'd be building on shifting ground.

## 4. Visual Design Language

Crash's visual identity is deliberately **not** a novel design language — it should read as a faster, smoother sibling of Windows 11's own File Explorer, not a competitor to its aesthetic. Familiarity here is a feature: the whole pitch is "everything you already know, none of the lag," so the UI shouldn't ask users to relearn anything visually. The Fluent Design System is mimicked closely, then quietly outperformed underneath.

- **Materials**: Mica as the default window backdrop (the same subtle, desktop-tinted translucency Windows 11 uses for File Explorer's own window chrome), with Acrylic reserved for transient surfaces — flyouts, context menus, command palette overlay — matching where Windows 11 itself uses each material.
- **Corner radius & geometry**: 8px corner radius on the window frame and top-level containers, 4px on inner elements (list rows, buttons, chips) — matching Windows 11's system-wide rounding convention rather than introducing a competing radius scale.
- **Typography**: Segoe UI Variable, the same variable font Windows 11 uses across Settings, Explorer, and other inbox apps, at matching weight/size steps (Display, Title, Body, Caption) so text never feels "off-brand" next to the rest of the OS.
- **Color & theming**: full support for the user's Windows accent color and light/dark theme, read live from system settings rather than requiring a manual toggle — File Explorer does this, and departing from it would make Crash feel like a guest in its own OS.
- **Iconography**: Fluent System Icons (the same icon set as Explorer, Settings, and Store) for all chrome and file-type glyphs, so folder/file icons look native rather than introducing a competing icon language.
- **Title bar & chrome**: content extends into the title bar the way modern Explorer does (Mica bleeding to the top edge, integrated tab strip in the title bar area), rather than a legacy-style separate menu bar.
- **Motion**: restrained, purposeful animation only — tab open/close, pane resize, context-menu reveal — mirroring the subtlety of Explorer's own transitions. No animation is added that isn't cheap to render; see the performance budget in §6. Reduced-motion system setting is respected everywhere.
- **Where Crash deliberately breaks from Explorer**: information density. Explorer's default list view has grown sparse in Windows 11; Crash should offer a genuine "compact" density option (closer to the old Windows 10/classic row height) as a first-class setting, since that's a frequent complaint from power users switching away from Explorer — while keeping the default comfortable and Explorer-like for everyone else.

The net effect: someone opening Crash for the first time should recognize it immediately as "a Windows 11 app," with the differentiation showing up in *feel* (speed, responsiveness) rather than in visual novelty they need to adjust to.

## 5. Process & Threading Model

```
┌─────────────────────────────────────────────┐
│                 UI Thread                     │
│  - Win32 message pump                         │
│  - Direct2D/DirectComposition frame present    │
│  - Input handling, hit-testing                │
│  - NEVER touches disk or shell APIs directly   │
└───────────────┬───────────────────────────────┘
                │ posts work / receives results via
                │ lock-free queue + PostMessage wakeup
┌───────────────▼───────────────────────────────┐
│           Enumeration Thread Pool             │
│  - Directory listing (FindFirstFile/NtQuery-   │
│    DirectoryFile for bulk perf)                │
│  - Streams results back in batches (e.g. 200   │
│    entries at a time) so large folders paint   │
│    progressively instead of blocking           │
└───────────────┬───────────────────────────────┘
                │
┌───────────────▼───────────────────────────────┐
│         Thumbnail / Metadata Workers            │
│  - IThumbnailProvider / IExtractImage calls     │
│  - Lowest thread priority; cancellable per-     │
│    scroll-position so off-screen work is        │
│    dropped, not queued forever                  │
└───────────────┬───────────────────────────────┘
                │
┌───────────────▼───────────────────────────────┐
│              Disk Cache Layer                  │
│  - Persistent icon/thumbnail cache (SQLite or   │
│    custom flat-file store, keyed by path+mtime) │
│  - Survives app restart — this is what makes    │
│    "revisit a folder" instant                   │
└─────────────────────────────────────────────────┘
```

Golden rule: **the UI thread never calls a blocking Win32/shell API.** Every File Explorer stability complaint we researched traces back to shell-extension or filesystem calls (network drives, cloud placeholder files, corrupted metadata) stalling the thread that also owns window messages. Structural separation is the actual fix — not try/catch, not "optimize later."

## 6. Key Components

### 6.1 Virtualized List/Grid Control (the core differentiator)
- Custom Direct2D control, not a wrapped Win32 ListView or WinUI ItemsView
- Renders only visible rows + small overscan buffer; no per-item DOM/visual-tree objects
- Supports list, details, and grid/icon layouts as render modes of the *same* data model, not separate controls
- Text via DirectWrite with cached `IDWriteTextLayout` objects per visible row (invalidate only on rename/resize)

### 6.2 Async Enumeration Pipeline
- Batch-streamed results (as above)
- Cancellation token per navigation — if the user clicks another folder mid-load, in-flight work is discarded, not queued
- Optional: `NtQueryDirectoryFile` for enumeration instead of `FindFirstFile`/`FindNextFile` — meaningfully faster on large directories, at the cost of using an undocumented-but-stable NT API (fallback to Win32 API on failure)

### 6.3 Icon/Thumbnail Cache
- Two tiers: in-memory LRU (instant re-scroll) + on-disk persistent store (instant re-visit after restart)
- Cache key: full path + last-write-time + file size, so edits invalidate automatically
- Generation is background/cancellable, never blocks list paint — rows show a generic icon immediately, then swap in the real thumbnail when ready (this alone fixes the "3 seconds to load icons" complaint we found in Files' issue tracker)

### 6.4 Shell Integration Layer
- Thin wrapper around `IShellFolder`, `IExtractIcon`, context menu (`IContextMenu`) so right-click still shows every installed shell extension
- Isolate this in its own DLL/thread boundary where feasible — third-party shell extensions are a known source of File Explorer instability, and we don't want a broken extension able to bring down our whole process

### 6.5 Tab / Pane Manager
- Tabs and dual/quad-pane splits are layout state, not separate windows — cheap to create/destroy
- Session persistence (restore tabs on relaunch) stored as flat JSON, loaded async so it never delays first paint

### 6.6 Command Palette / Search *(Pro tier)*
- Fuzzy-match command palette (VS Code-style) as the primary power-user entry point, reducing menu-diving
- Search backed by filename index for instant-as-you-type filtering within a folder; full-drive content search can defer to or integrate with Everything's API rather than reinventing NTFS MFT indexing
- Basic in-folder search remains in the free tier; saved searches, indexed full-drive search, and the command palette itself are Pro-gated

## 7. Performance Budget (targets, not aspirations)

| Metric | Target |
|---|---|
| Cold start to interactive | < 300ms |
| Open folder, 1k items | < 100ms to first paint, fully populated < 300ms |
| Open folder, 100k items | progressive paint starts < 150ms, no UI-thread stall regardless of count |
| Scroll | steady 60fps (or display refresh rate) on integrated GPUs |
| Tab switch | < 50ms |
| Memory, idle with 10 tabs | comparable to Total Commander's footprint, not Electron-app territory |

These numbers should be captured as automated perf-regression tests from week one — the Files project's multi-year struggle with performance complaints is partly a story of not having this instrumented early enough to prevent regressions.

## 8. What we deliberately borrow vs. rebuild

**Borrow (as product/UX reference, not code, unless MIT-compatible reuse is verified per-file):**
- Tabs, dual-pane, tags, command palette concepts from Files
- Keyboard-driven workflow philosophy from XYplorer/Total Commander
- Visual language directly from Windows 11 / Fluent Design (see §4) — intentionally, not borrowed so much as adopted wholesale

**Rebuild from scratch:**
- Rendering pipeline (Direct2D, not XAML)
- Threading/enumeration model
- Icon/thumbnail cache

## 9. Phased Roadmap

1. **Phase 0 — Renderer spike (2-3 weeks):** prove the Direct2D virtualized list can hit the scroll/paint targets above with a synthetic 100k-row dataset before writing any other feature.
2. **Phase 1 — Single-pane MVP:** navigation, enumeration pipeline, basic icons (no thumbnails yet), context menu integration.
3. **Phase 2 — Tabs, dual-pane, thumbnail cache.**
4. **Phase 3 — Command palette, search, tagging** (Pro-tier feature work begins here).
5. **Phase 4 — Polish pass: Fluent-style theming, animations (budgeted, not free), settings/customization.**
6. **Phase 5 — Store listing, licensing/commerce integration, GitHub public release.**

Phase 0 is the load-bearing decision point: if the renderer can't clear the bar, everything downstream needs re-scoping before more is built on top of it.

## 10. Open Decisions

- **C++ vs. Rust** — Rust removes a whole class of memory-safety bugs (a real source of crashes in native shell code historically) at the cost of a smaller pool of Win32-experienced contributors and rougher COM interop ergonomics. Recommend a small spike in both before committing.
- **NtQueryDirectoryFile vs. standard Win32 enumeration** — perf win vs. API stability/support risk.
- **Self-hosted commerce vs. Microsoft commerce platform for the Pro unlock** — higher margin vs. lower engineering/support overhead at launch (see §2.2).
- **Exact open-core boundary** — which features live in the closed Pro module vs. the MIT core needs a firm list before Phase 3, since moving something from open to closed later is a much worse community signal than defining the line up front.
