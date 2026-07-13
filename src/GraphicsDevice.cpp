#include "GraphicsDevice.h"

void GraphicsDevice::Initialize(HWND hwnd)
{
    hwnd_ = hwnd;
    dpi_ = static_cast<float>(GetDpiForWindow(hwnd));
    if (dpi_ <= 0) dpi_ = 96.0f;

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    widthPx_  = static_cast<UINT>((std::max)(1L, rc.right - rc.left));
    heightPx_ = static_cast<UINT>((std::max)(1L, rc.bottom - rc.top));

    CreateDeviceResources();
    CreateSwapChain();
    CreateTargetBitmap();
}

void GraphicsDevice::CreateDeviceResources()
{
    // D3D11 device. BGRA support is required for Direct2D interop.
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    // Only request the debug layer if it is actually installed.
    if (SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            flags | D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION,
            nullptr, nullptr, nullptr)))
    {
        flags |= D3D11_CREATE_DEVICE_DEBUG;
    }
#endif

    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        d3d_.ReleaseAndGetAddressOf(), nullptr, nullptr);

    if (FAILED(hr))
    {
        // Fall back to WARP so the spike still runs on machines without a
        // usable hardware GPU (RDP, some VMs).
        HR(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
            d3d_.ReleaseAndGetAddressOf(), nullptr, nullptr), "D3D11CreateDevice(WARP)");
    }

    HR(d3d_.As(&dxgiDevice_), "QI IDXGIDevice");

    // A modest max-frame-latency keeps present latency low for scrolling.
    // (SetMaximumFrameLatency lives on IDXGIDevice1.)
    ComPtr<IDXGIDevice1> dxgiDevice1;
    if (SUCCEEDED(d3d_.As(&dxgiDevice1)))
        dxgiDevice1->SetMaximumFrameLatency(1);

    ComPtr<IDXGIAdapter> adapter;
    HR(dxgiDevice_->GetAdapter(&adapter), "GetAdapter");
    HR(adapter->GetParent(IID_PPV_ARGS(&dxgiFactory_)), "GetParent IDXGIFactory2");

    // The Direct2D factory and DirectWrite factory are device-INDEPENDENT.
    // Create them once and keep them across device-loss so the view's cached
    // text formats/layouts (and the IDWriteFactory* it holds) stay valid.
    if (!d2dFactory_)
    {
        D2D1_FACTORY_OPTIONS opts{};
#if defined(_DEBUG)
        opts.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
        HR(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory1), &opts,
            reinterpret_cast<void**>(d2dFactory_.ReleaseAndGetAddressOf())),
            "D2D1CreateFactory");
    }
    if (!dwrite_)
    {
        HR(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(dwrite_.ReleaseAndGetAddressOf())),
            "DWriteCreateFactory");
    }

    HR(d2dFactory_->CreateDevice(dxgiDevice_.Get(), &d2dDevice_), "CreateDevice(D2D)");
    HR(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_),
        "CreateDeviceContext");
    dc_->SetDpi(dpi_, dpi_);
    dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    // DirectComposition device + target bound to the HWND.
    HR(DCompositionCreateDevice(dxgiDevice_.Get(), IID_PPV_ARGS(&dcompDevice_)),
        "DCompositionCreateDevice");
    HR(dcompDevice_->CreateTargetForHwnd(hwnd_, TRUE, &dcompTarget_),
        "CreateTargetForHwnd");
    HR(dcompDevice_->CreateVisual(&dcompVisual_), "CreateVisual");
}

void GraphicsDevice::CreateSwapChain()
{
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width  = widthPx_;
    desc.Height = heightPx_;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;                              // flip model needs >= 2
    desc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;  // modern flip model
    desc.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;         // opaque content surface
    desc.Scaling     = DXGI_SCALING_STRETCH;           // required for composition

    HR(dxgiFactory_->CreateSwapChainForComposition(
        d3d_.Get(), &desc, nullptr, swapChain_.ReleaseAndGetAddressOf()),
        "CreateSwapChainForComposition");

    // Wire the swapchain into the composition tree once.
    HR(dcompVisual_->SetContent(swapChain_.Get()), "SetContent");
    HR(dcompTarget_->SetRoot(dcompVisual_.Get()), "SetRoot");
    HR(dcompDevice_->Commit(), "Commit");
}

void GraphicsDevice::CreateTargetBitmap()
{
    // In the D3D11 flip model the app always renders to buffer 0; DXGI rotates
    // internally, so a bitmap wrapping buffer 0 is stable across presents.
    ComPtr<IDXGISurface> surface;
    HR(swapChain_->GetBuffer(0, IID_PPV_ARGS(&surface)), "GetBuffer(0)");

    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        dpi_, dpi_);

    HR(dc_->CreateBitmapFromDxgiSurface(surface.Get(), &props, &targetBitmap_),
        "CreateBitmapFromDxgiSurface");
}

void GraphicsDevice::Resize(UINT widthPx, UINT heightPx)
{
    widthPx  = (std::max)(1u, widthPx);
    heightPx = (std::max)(1u, heightPx);
    if (widthPx == widthPx_ && heightPx == heightPx_) return;

    widthPx_ = widthPx;
    heightPx_ = heightPx;

    // Release the size-dependent target before resizing buffers.
    dc_->SetTarget(nullptr);
    targetBitmap_.Reset();

    HRESULT hr = swapChain_->ResizeBuffers(0, widthPx_, heightPx_,
        DXGI_FORMAT_UNKNOWN, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
    {
        HandleDeviceLost();
        return;
    }
    HR(hr, "ResizeBuffers");
    CreateTargetBitmap();
}

void GraphicsDevice::SetDpi(float dpi)
{
    if (dpi <= 0) return;
    dpi_ = dpi;
    dc_->SetDpi(dpi_, dpi_);
    // Rebuild the target so its bitmap DPI matches (affects DIP mapping).
    dc_->SetTarget(nullptr);
    targetBitmap_.Reset();
    CreateTargetBitmap();
}

ID2D1DeviceContext* GraphicsDevice::BeginFrame()
{
    dc_->SetTarget(targetBitmap_.Get());
    dc_->BeginDraw();
    dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    inFrame_ = true;
    return dc_.Get();
}

void GraphicsDevice::EndFrame(bool vsync)
{
    inFrame_ = false;
    HRESULT hr = dc_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED ||
        hr == DXGI_ERROR_DEVICE_RESET)
    {
        HandleDeviceLost();
        return;
    }
    HR(hr, "EndDraw");

    // Present. sync-interval 1 = vsync-paced; 0 = uncapped (headroom probe).
    hr = swapChain_->Present(vsync ? 1 : 0, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
    {
        HandleDeviceLost();
        return;
    }
    HR(hr, "Present");
}

void GraphicsDevice::HandleDeviceLost()
{
    // Tear the whole stack down and rebuild. Callers keep no cached device
    // resources across a frame except the view's brushes, which lazily
    // recreate, so a full rebuild here is safe for the spike.
    dc_->SetTarget(nullptr);
    targetBitmap_.Reset();
    swapChain_.Reset();
    dcompVisual_.Reset();
    dcompTarget_.Reset();
    dcompDevice_.Reset();
    dc_.Reset();
    d2dDevice_.Reset();
    dxgiFactory_.Reset();
    dxgiDevice_.Reset();
    d3d_.Reset();
    // NB: d2dFactory_ and dwrite_ are device-independent — kept alive on purpose.

    CreateDeviceResources();
    CreateSwapChain();
    CreateTargetBitmap();
}
