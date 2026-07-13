#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "License.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cwctype>

namespace
{
    std::wstring LicensePath()
    {
        PWSTR local = nullptr; std::wstring p;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
        {
            p = local; CoTaskMemFree(local);
            p += L"\\Crash"; CreateDirectoryW(p.c_str(), nullptr);
            p += L"\\license.key";
        }
        return p;
    }

    std::wstring Normalize(const std::wstring& s)
    {
        std::wstring o;
        for (wchar_t c : s) if (!iswspace(c)) o += static_cast<wchar_t>(towupper(c));
        return o;
    }

    bool ValidKey(const std::wstring& key) { return Normalize(key) == Normalize(kDemoLicenseKey); }
}

bool IsLicensed()
{
    std::wstring path = LicensePath();
    if (path.empty()) return false;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    char buf[256]{};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    std::wstring key;
    for (size_t i = 0; i < n; ++i) key += static_cast<wchar_t>(buf[i]);
    return ValidKey(key);
}

bool UnlockPro(const std::wstring& key)
{
    if (!ValidKey(key)) return false;
    std::wstring path = LicensePath();
    if (path.empty()) return false;
    if (FILE* f = _wfopen(path.c_str(), L"wb"))
    {
        std::wstring norm = Normalize(key);
        for (wchar_t c : norm) { char b = static_cast<char>(c); fwrite(&b, 1, 1, f); }
        fclose(f);
    }
    return true;
}
