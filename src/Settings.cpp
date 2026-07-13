#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "Settings.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <string>

namespace
{
    std::wstring SettingsPath()
    {
        PWSTR local = nullptr; std::wstring p;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
        {
            p = local; CoTaskMemFree(local);
            p += L"\\Crash"; CreateDirectoryW(p.c_str(), nullptr);
            p += L"\\settings.cfg";
        }
        return p;
    }
}

bool LoadSettings(AppSettings& out)
{
    std::wstring path = SettingsPath();
    if (path.empty()) return false;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    char line[128];
    while (fgets(line, sizeof(line), f))
    {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const int v = atoi(eq + 1);
        std::string key(line);
        if (key == "theme") out.theme = v;
        else if (key == "compact") out.compact = v != 0;
        else if (key == "gridDefault") out.gridDefault = v != 0;
        else if (key == "showHidden") out.showHidden = v != 0;
        else if (key == "thumbnails") out.thumbnails = v != 0;
        else if (key == "animations") out.animations = v != 0;
    }
    fclose(f);
    return true;
}

void SaveSettings(const AppSettings& in)
{
    std::wstring path = SettingsPath();
    if (path.empty()) return;
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    fprintf(f, "theme=%d\n", in.theme);
    fprintf(f, "compact=%d\n", in.compact ? 1 : 0);
    fprintf(f, "gridDefault=%d\n", in.gridDefault ? 1 : 0);
    fprintf(f, "showHidden=%d\n", in.showHidden ? 1 : 0);
    fprintf(f, "thumbnails=%d\n", in.thumbnails ? 1 : 0);
    fprintf(f, "animations=%d\n", in.animations ? 1 : 0);
    fclose(f);
}
