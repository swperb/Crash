// Crash — shared platform/COM includes and helpers.
// Phase 0 renderer spike. C++20, MSVC.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11.h>
#include <dxgi1_3.h>
#include <d2d1_1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <dcomp.h>
#include <dwmapi.h>

#include <wrl/client.h>

#include <cstdio>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;

// Throw on failed HRESULT. Spike-grade error handling: fail loud, not silent.
inline void HR(HRESULT hr, const char* what = "call")
{
    if (FAILED(hr))
    {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s failed: HRESULT 0x%08X", what, static_cast<unsigned>(hr));
        throw std::runtime_error(buf);
    }
}

// Small color helper mirroring D2D1::ColorF but taking 0-255 bytes.
inline D2D1_COLOR_F RGBf(int r, int g, int b, float a = 1.0f)
{
    return D2D1::ColorF(r / 255.0f, g / 255.0f, b / 255.0f, a);
}

// A DIP is 1/96". Convert between physical pixels and DIPs given a DPI.
inline float PxToDip(float px, float dpi) { return px * 96.0f / dpi; }
inline float DipToPx(float dip, float dpi) { return dip * dpi / 96.0f; }
