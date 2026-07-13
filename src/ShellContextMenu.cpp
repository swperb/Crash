#include "ShellContextMenu.h"
#include <shlobj.h>
#include <shellapi.h>

namespace
{
    constexpr UINT kFirst = 1;
    constexpr UINT kLast = 0x7FFF;

    // --- SEH containment wrappers (design doc §6.4) ---------------------------
    // These call INTO third-party shell-extension code. Each is a standalone
    // function with no C++ objects requiring unwinding, so __try/__except is
    // legal and a faulting extension is turned into a failure code, not a crash.

    HRESULT SafeQuery(IContextMenu* cm, HMENU m, UINT flags)
    {
        __try { return cm->QueryContextMenu(m, 0, kFirst, kLast, flags); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return E_FAIL; }
    }

    bool SafeInvoke(IContextMenu* cm, CMINVOKECOMMANDINFOEX* ici)
    {
        __try { return SUCCEEDED(cm->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(ici))); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    HRESULT SafeMsg3(IContextMenu3* cm, UINT msg, WPARAM w, LPARAM l, LRESULT* r)
    {
        __try { return cm->HandleMenuMsg2(msg, w, l, r); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return E_FAIL; }
    }

    HRESULT SafeMsg2(IContextMenu2* cm, UINT msg, WPARAM w, LPARAM l)
    {
        __try { return cm->HandleMenuMsg(msg, w, l); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return E_FAIL; }
    }
}

void ShellContextMenu::Track(HWND hwnd, IContextMenu* cm, POINT pt)
{
    // Grab the richer interfaces for owner-draw / submenu message forwarding.
    cm->QueryInterface(IID_PPV_ARGS(&cm3_));
    if (!cm3_) cm->QueryInterface(IID_PPV_ARGS(&cm2_));

    HMENU menu = CreatePopupMenu();
    if (menu)
    {
        if (SUCCEEDED(SafeQuery(cm, menu, CMF_NORMAL | CMF_EXPLORE)))
        {
            const UINT cmd = TrackPopupMenuEx(menu,
                TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN,
                pt.x, pt.y, hwnd, nullptr);

            if (cmd >= kFirst)
            {
                CMINVOKECOMMANDINFOEX ici{};
                ici.cbSize = sizeof(ici);
                ici.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
                ici.hwnd = hwnd;
                ici.lpVerb = MAKEINTRESOURCEA(cmd - kFirst);
                ici.lpVerbW = MAKEINTRESOURCEW(cmd - kFirst);
                ici.nShow = SW_SHOWNORMAL;
                ici.ptInvoke = pt;
                SafeInvoke(cm, &ici);
                if (onAfterInvoke) onAfterInvoke();
            }
        }
        DestroyMenu(menu);
    }

    cm2_.Reset();
    cm3_.Reset();
}

void ShellContextMenu::ShowForItem(HWND hwnd, const std::wstring& fullPath, POINT pt)
{
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(fullPath.c_str(), nullptr, &pidl, 0, nullptr)) || !pidl)
        return;

    IShellFolder* parent = nullptr;
    PCUITEMID_CHILD child = nullptr;
    if (SUCCEEDED(SHBindToParent(pidl, IID_IShellFolder, reinterpret_cast<void**>(&parent), &child)) && parent)
    {
        IContextMenu* cm = nullptr;
        if (SUCCEEDED(parent->GetUIObjectOf(hwnd, 1, &child, IID_IContextMenu, nullptr,
                reinterpret_cast<void**>(&cm))) && cm)
        {
            Track(hwnd, cm, pt);
            cm->Release();
        }
        parent->Release();
    }
    CoTaskMemFree(pidl);
}

void ShellContextMenu::ShowForItems(HWND hwnd, const std::wstring& folderPath,
                                    const std::vector<std::wstring>& names, POINT pt)
{
    if (names.empty()) return;
    if (names.size() == 1) { ShowForItem(hwnd, folderPath + L"\\" + names[0], pt); return; }

    PIDLIST_ABSOLUTE folderPidl = nullptr;
    if (FAILED(SHParseDisplayName(folderPath.c_str(), nullptr, &folderPidl, 0, nullptr)) || !folderPidl)
        return;

    IShellFolder* folder = nullptr;
    if (SUCCEEDED(SHBindToObject(nullptr, folderPidl, nullptr, IID_IShellFolder,
            reinterpret_cast<void**>(&folder))) && folder)
    {
        // Resolve each selected name to a child PIDL under this folder.
        std::vector<PITEMID_CHILD> children;
        for (const std::wstring& n : names)
        {
            PIDLIST_RELATIVE child = nullptr;
            if (SUCCEEDED(folder->ParseDisplayName(hwnd, nullptr,
                    const_cast<LPWSTR>(n.c_str()), nullptr, &child, nullptr)) && child)
                children.push_back(reinterpret_cast<PITEMID_CHILD>(child));
        }
        if (!children.empty())
        {
            IContextMenu* cm = nullptr;
            if (SUCCEEDED(folder->GetUIObjectOf(hwnd, static_cast<UINT>(children.size()),
                    (PCUITEMID_CHILD_ARRAY)children.data(),   // C-style: adds constness
                    IID_IContextMenu, nullptr, reinterpret_cast<void**>(&cm))) && cm)
            {
                Track(hwnd, cm, pt);
                cm->Release();
            }
        }
        for (PITEMID_CHILD c : children) CoTaskMemFree(c);
        folder->Release();
    }
    CoTaskMemFree(folderPidl);
}

void ShellContextMenu::ShowForFolder(HWND hwnd, const std::wstring& folderPath, POINT pt)
{
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(folderPath.c_str(), nullptr, &pidl, 0, nullptr)) || !pidl)
        return;

    IShellFolder* folder = nullptr;
    if (SUCCEEDED(SHBindToObject(nullptr, pidl, nullptr, IID_IShellFolder,
            reinterpret_cast<void**>(&folder))) && folder)
    {
        IContextMenu* cm = nullptr;
        if (SUCCEEDED(folder->CreateViewObject(hwnd, IID_IContextMenu,
                reinterpret_cast<void**>(&cm))) && cm)
        {
            Track(hwnd, cm, pt);
            cm->Release();
        }
        folder->Release();
    }
    CoTaskMemFree(pidl);
}

bool ShellContextMenu::HandleMenuMsg(UINT msg, WPARAM w, LPARAM l, LRESULT* result)
{
    switch (msg)
    {
    case WM_INITMENUPOPUP: case WM_DRAWITEM: case WM_MEASUREITEM: case WM_MENUCHAR: break;
    default: return false;
    }

    if (cm3_)
    {
        LRESULT r = 0;
        if (SUCCEEDED(SafeMsg3(cm3_.Get(), msg, w, l, &r))) { if (result) *result = r; return true; }
        return false;
    }
    if (cm2_)
    {
        if (SUCCEEDED(SafeMsg2(cm2_.Get(), msg, w, l)))
        {
            if (result) *result = (msg == WM_MENUCHAR) ? 0 : TRUE;
            return true;
        }
    }
    return false;
}
