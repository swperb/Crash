#include "Session.h"

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cstdlib>
#include <cwctype>

namespace
{
    std::wstring SessionPath()
    {
        PWSTR local = nullptr;
        std::wstring path;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
        {
            path = local; CoTaskMemFree(local);
            path += L"\\Crash";
            CreateDirectoryW(path.c_str(), nullptr);
            path += L"\\session.json";
        }
        return path;
    }

    std::string W2U(const std::wstring& w)
    {
        if (w.empty()) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string s(n, 0);
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
        return s;
    }
    std::wstring U2W(const std::string& s)
    {
        if (s.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
        std::wstring w(n, 0);
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
        return w;
    }
    std::wstring Esc(const std::wstring& s)
    {
        std::wstring o;
        for (wchar_t c : s) { if (c == L'\\' || c == L'"') o += L'\\'; o += c; }
        return o;
    }

    // Minimal JSON value + recursive-descent parser (objects/arrays/strings/
    // numbers/bools). Enough to read what SaveSession writes, tolerantly.
    struct JVal
    {
        enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
        bool b = false; double num = 0; std::wstring str;
        std::vector<JVal> arr;
        std::vector<std::pair<std::wstring, JVal>> obj;
        const JVal* get(const wchar_t* k) const { for (auto& kv : obj) if (kv.first == k) return &kv.second; return nullptr; }
    };
    struct P
    {
        const wchar_t* s; const wchar_t* e;
        void ws() { while (s < e && (*s == L' ' || *s == L'\t' || *s == L'\n' || *s == L'\r')) ++s; }
        bool parse(JVal& v)
        {
            ws(); if (s >= e) return false;
            switch (*s)
            {
            case L'{': return obj(v);
            case L'[': return arr(v);
            case L'"': return str(v);
            case L't': case L'f': return boolean(v);
            case L'n': s += (e - s >= 4) ? 4 : (e - s); v.t = JVal::Null; return true;
            default: return num(v);
            }
        }
        bool str(JVal& v)
        {
            v.t = JVal::Str; ++s; std::wstring o;
            while (s < e && *s != L'"')
            {
                if (*s == L'\\' && s + 1 < e) { ++s; wchar_t c = *s; o += (c == L'n') ? L'\n' : (c == L't') ? L'\t' : c; }
                else o += *s;
                ++s;
            }
            if (s < e) ++s;
            v.str = std::move(o); return true;
        }
        bool num(JVal& v)
        {
            v.t = JVal::Num; const wchar_t* st = s;
            while (s < e && (iswdigit(*s) || *s == L'-' || *s == L'+' || *s == L'.' || *s == L'e' || *s == L'E')) ++s;
            v.num = _wtof(std::wstring(st, s).c_str()); return s > st;
        }
        bool boolean(JVal& v)
        {
            v.t = JVal::Bool;
            if (*s == L't') { v.b = true;  s += (e - s >= 4) ? 4 : (e - s); }
            else            { v.b = false; s += (e - s >= 5) ? 5 : (e - s); }
            return true;
        }
        bool arr(JVal& v)
        {
            v.t = JVal::Arr; ++s; ws(); if (s < e && *s == L']') { ++s; return true; }
            for (;;)
            {
                JVal it; if (!parse(it)) return false; v.arr.push_back(std::move(it)); ws();
                if (s < e && *s == L',') { ++s; continue; }
                if (s < e && *s == L']') { ++s; break; }
                break;
            }
            return true;
        }
        bool obj(JVal& v)
        {
            v.t = JVal::Obj; ++s; ws(); if (s < e && *s == L'}') { ++s; return true; }
            for (;;)
            {
                ws(); if (s >= e || *s != L'"') return false;
                JVal key; str(key); ws(); if (s < e && *s == L':') ++s;
                JVal val; if (!parse(val)) return false;
                v.obj.push_back({ key.str, std::move(val) }); ws();
                if (s < e && *s == L',') { ++s; continue; }
                if (s < e && *s == L'}') { ++s; break; }
                break;
            }
            return true;
        }
    };
}

void SaveSession(const SessionData& in)
{
    std::wstring path = SessionPath();
    if (path.empty()) return;

    std::wstring j = L"{\n";
    j += L"  \"dualPane\": "; j += in.dual ? L"true" : L"false"; j += L",\n";
    j += L"  \"activePane\": " + std::to_wstring(in.activePane) + L",\n";
    j += L"  \"splitRatio\": " + std::to_wstring(in.split) + L",\n";
    j += L"  \"panes\": [\n";
    for (size_t i = 0; i < in.panes.size(); ++i)
    {
        const SessionPane& p = in.panes[i];
        j += L"    { \"activeTab\": " + std::to_wstring(p.activeTab) + L", \"tabs\": [";
        for (size_t t = 0; t < p.tabs.size(); ++t) { j += L"\"" + Esc(p.tabs[t]) + L"\""; if (t + 1 < p.tabs.size()) j += L", "; }
        j += L"] }"; if (i + 1 < in.panes.size()) j += L","; j += L"\n";
    }
    j += L"  ]\n}\n";

    std::string bytes = W2U(j);
    if (FILE* f = _wfopen(path.c_str(), L"wb")) { fwrite(bytes.data(), 1, bytes.size(), f); fclose(f); }
}

bool LoadSession(SessionData& out)
{
    std::wstring path = SessionPath();
    if (path.empty()) return false;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return false; }
    std::string bytes(static_cast<size_t>(n), 0);
    size_t rd = fread(bytes.data(), 1, static_cast<size_t>(n), f);
    fclose(f);
    bytes.resize(rd);

    std::wstring j = U2W(bytes);
    JVal root; P p{ j.c_str(), j.c_str() + j.size() };
    if (!p.parse(root) || root.t != JVal::Obj) return false;

    if (const JVal* dp = root.get(L"dualPane")) out.dual = (dp->t == JVal::Bool) ? dp->b : false;
    if (const JVal* ap = root.get(L"activePane")) out.activePane = (ap->t == JVal::Num) ? (int)ap->num : 0;
    if (const JVal* sp = root.get(L"splitRatio")) out.split = (sp->t == JVal::Num) ? (float)sp->num : 0.5f;
    const JVal* panes = root.get(L"panes");
    if (!panes || panes->t != JVal::Arr) return false;
    for (const JVal& pv : panes->arr)
    {
        if (pv.t != JVal::Obj) continue;
        SessionPane sp;
        if (const JVal* at = pv.get(L"activeTab")) sp.activeTab = (at->t == JVal::Num) ? (int)at->num : 0;
        if (const JVal* tabs = pv.get(L"tabs"); tabs && tabs->t == JVal::Arr)
            for (const JVal& tv : tabs->arr) if (tv.t == JVal::Str) sp.tabs.push_back(tv.str);
        out.panes.push_back(std::move(sp));
    }
    return !out.panes.empty();
}
