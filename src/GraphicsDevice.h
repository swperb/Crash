// GraphicsDevice — owns the D3D11/DXGI/DirectComposition/Direct2D/DirectWrite
// stack for a single Win32 window.
//
// Design (matches crash_design_doc.md §3, §5):
//   * A DXGI *flip-model* swapchain (DXGI_SWAP_EFFECT_FLIP_DISCARD) created for
//     composition, so present shares surfaces directly with the desktop
//     compositor (tear-free, GPU-composited, DirectFlip/MPO eligible).
//   * The swapchain is the content of a DirectComposition visual, so we own the
//     whole client area with no HWND-per-element overhead.
//   * A Direct2D device context renders into the swapchain back buffer.
//   * DirectWrite factory is exposed for text layout.
//
// The UI thread is the only caller. It never blocks on disk/shell here.
#pragma once
#include "common.h"

class GraphicsDevice
{
public:
    void Initialize(HWND hwnd);

    // Resize the swapchain to the window's current client size (physical px).
    void Resize(UINT widthPx, UINT heightPx);

    // Update DPI (WM_DPICHANGED). Recreates the size-dependent target.
    void SetDpi(float dpi);

    // Begin/End a frame. BeginFrame returns the device context ready to draw
    // (target set, BeginDraw called, transform identity). EndFrame presents.
    ID2D1DeviceContext* BeginFrame();
    void EndFrame(bool vsync);

    IDWriteFactory* DWrite() const { return dwrite_.Get(); }
    float Dpi() const { return dpi_; }
    UINT WidthPx() const { return widthPx_; }
    UINT HeightPx() const { return heightPx_; }
    float WidthDip() const { return PxToDip(static_cast<float>(widthPx_), dpi_); }
    float HeightDip() const { return PxToDip(static_cast<float>(heightPx_), dpi_); }

private:
    void CreateDeviceResources();   // device-dependent (survives resize)
    void CreateSwapChain();         // composition swapchain + dcomp tree
    void CreateTargetBitmap();      // size-dependent D2D render target
    void HandleDeviceLost();

    HWND hwnd_ = nullptr;
    UINT widthPx_ = 1, heightPx_ = 1;
    float dpi_ = 96.0f;

    // Device-dependent
    ComPtr<ID3D11Device>        d3d_;
    ComPtr<IDXGIDevice>         dxgiDevice_;
    ComPtr<IDXGIFactory2>       dxgiFactory_;
    ComPtr<ID2D1Factory1>       d2dFactory_;
    ComPtr<ID2D1Device>         d2dDevice_;
    ComPtr<ID2D1DeviceContext>  dc_;
    ComPtr<IDWriteFactory>      dwrite_;

    // Composition
    ComPtr<IDCompositionDevice> dcompDevice_;
    ComPtr<IDCompositionTarget> dcompTarget_;
    ComPtr<IDCompositionVisual> dcompVisual_;

    // Size-dependent
    ComPtr<IDXGISwapChain1>     swapChain_;
    ComPtr<ID2D1Bitmap1>        targetBitmap_;

    bool inFrame_ = false;
};
