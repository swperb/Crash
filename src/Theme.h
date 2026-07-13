// Theme — reads the live Windows light/dark preference and accent color, and
// derives the palette Crash draws with. Matches design doc §4 (adopt Windows 11
// Fluent look; follow the system theme/accent rather than a manual toggle).
#pragma once
#include "common.h"

enum class Density { Comfortable, Compact };

struct Theme
{
    bool dark = true;
    D2D1_COLOR_F windowBg;
    D2D1_COLOR_F headerBg;
    D2D1_COLOR_F rowAlt;        // alternating row wash
    D2D1_COLOR_F rowHover;
    D2D1_COLOR_F rowSelected;
    D2D1_COLOR_F textPrimary;
    D2D1_COLOR_F textSecondary;
    D2D1_COLOR_F gridLine;
    D2D1_COLOR_F accent;
    D2D1_COLOR_F iconFolder;
    D2D1_COLOR_F overlayBg;
    D2D1_COLOR_F overlayText;
    D2D1_COLOR_F overlayGood;   // meeting perf target
    D2D1_COLOR_F overlayWarn;   // missing perf target
    // chrome (Phase 1)
    D2D1_COLOR_F toolbarBg;
    D2D1_COLOR_F controlBg;     // address field / button rest
    D2D1_COLOR_F controlHover;
    D2D1_COLOR_F controlActive;
    D2D1_COLOR_F controlDisabled;
    D2D1_COLOR_F statusBg;

    // mode: 0 = follow system, 1 = force light, 2 = force dark.
    static Theme Load(int mode = 0)
    {
        // Light/dark: HKCU Personalize\AppsUseLightTheme (0 = dark).
        DWORD light = 1, cb = sizeof(light);
        RegGetValueW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &cb);
        const bool dark = (mode == 1) ? false : (mode == 2) ? true : (light == 0);

        // Accent color via DWM colorization (0x00RRGGBB-ish AARRGGBB).
        D2D1_COLOR_F accent = RGBf(0, 120, 215);
        DWORD col = 0; BOOL opaque = FALSE;
        if (SUCCEEDED(DwmGetColorizationColor(&col, &opaque)))
            accent = RGBf((col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF);

        Theme t;
        t.dark = dark;
        t.accent = accent;

        if (dark)
        {
            t.windowBg     = RGBf(32, 32, 32);
            t.headerBg     = RGBf(44, 44, 44);
            t.rowAlt       = RGBf(255, 255, 255, 0.035f);
            t.rowHover     = RGBf(255, 255, 255, 0.07f);
            t.rowSelected  = D2D1::ColorF(accent.r, accent.g, accent.b, 0.28f);
            t.textPrimary  = RGBf(255, 255, 255, 0.92f);
            t.textSecondary= RGBf(255, 255, 255, 0.55f);
            t.gridLine     = RGBf(255, 255, 255, 0.06f);
            t.iconFolder   = RGBf(230, 190, 90);
            t.overlayBg    = RGBf(12, 12, 12, 0.78f);
            t.overlayText  = RGBf(240, 240, 240);
            t.toolbarBg    = RGBf(40, 40, 40);
            t.controlBg    = RGBf(255, 255, 255, 0.06f);
            t.controlHover = RGBf(255, 255, 255, 0.10f);
            t.controlActive= RGBf(255, 255, 255, 0.16f);
            t.controlDisabled = RGBf(255, 255, 255, 0.22f);
            t.statusBg     = RGBf(38, 38, 38);
        }
        else
        {
            t.windowBg     = RGBf(243, 243, 243);
            t.headerBg     = RGBf(251, 251, 251);
            t.rowAlt       = RGBf(0, 0, 0, 0.022f);
            t.rowHover     = RGBf(0, 0, 0, 0.045f);
            t.rowSelected  = D2D1::ColorF(accent.r, accent.g, accent.b, 0.20f);
            t.textPrimary  = RGBf(0, 0, 0, 0.90f);
            t.textSecondary= RGBf(0, 0, 0, 0.55f);
            t.gridLine     = RGBf(0, 0, 0, 0.06f);
            t.iconFolder   = RGBf(240, 200, 90);
            t.overlayBg    = RGBf(250, 250, 250, 0.85f);
            t.overlayText  = RGBf(20, 20, 20);
            t.toolbarBg    = RGBf(249, 249, 249);
            t.controlBg    = RGBf(0, 0, 0, 0.05f);
            t.controlHover = RGBf(0, 0, 0, 0.09f);
            t.controlActive= RGBf(0, 0, 0, 0.14f);
            t.controlDisabled = RGBf(0, 0, 0, 0.25f);
            t.statusBg     = RGBf(246, 246, 246);
        }
        t.overlayGood = RGBf(80, 210, 120);
        t.overlayWarn = RGBf(240, 150, 70);
        return t;
    }
};
