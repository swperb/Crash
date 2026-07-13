#include "DragDrop.h"

#include <ole2.h>
#include <shlobj.h>
#include <shellapi.h>

namespace
{
    std::function<void(const std::vector<std::wstring>&, POINT, bool)> g_onDrop;

    DWORD EffectFor(DWORD keys, bool canDrop)
    {
        if (!canDrop) return DROPEFFECT_NONE;
        return (keys & MK_SHIFT) ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
    }

    bool HasHDrop(IDataObject* d)
    {
        FORMATETC fe{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        return d && d->QueryGetData(&fe) == S_OK;
    }

    std::vector<std::wstring> HDropPaths(IDataObject* d)
    {
        std::vector<std::wstring> out;
        FORMATETC fe{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM sm{};
        if (d && d->GetData(&fe, &sm) == S_OK)
        {
            if (HDROP hd = static_cast<HDROP>(GlobalLock(sm.hGlobal)))
            {
                const UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
                for (UINT i = 0; i < n; ++i)
                {
                    wchar_t buf[MAX_PATH];
                    if (DragQueryFileW(hd, i, buf, MAX_PATH)) out.push_back(buf);
                }
                GlobalUnlock(sm.hGlobal);
            }
            ReleaseStgMedium(&sm);
        }
        return out;
    }

    // --- IDropSource ---------------------------------------------------------
    class DropSource : public IDropSource
    {
        LONG ref_ = 1;
    public:
        STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
        {
            if (riid == IID_IUnknown || riid == IID_IDropSource) { *ppv = static_cast<IDropSource*>(this); AddRef(); return S_OK; }
            *ppv = nullptr; return E_NOINTERFACE;
        }
        STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
        STDMETHODIMP_(ULONG) Release() override { LONG c = InterlockedDecrement(&ref_); if (!c) delete this; return c; }
        STDMETHODIMP QueryContinueDrag(BOOL esc, DWORD keys) override
        {
            if (esc) return DRAGDROP_S_CANCEL;
            if (!(keys & (MK_LBUTTON | MK_RBUTTON))) return DRAGDROP_S_DROP;
            return S_OK;
        }
        STDMETHODIMP GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
    };

    // --- IDropTarget ---------------------------------------------------------
    class DropTarget : public IDropTarget
    {
        LONG ref_ = 1;
        bool canDrop_ = false;
    public:
        STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
        {
            if (riid == IID_IUnknown || riid == IID_IDropTarget) { *ppv = static_cast<IDropTarget*>(this); AddRef(); return S_OK; }
            *ppv = nullptr; return E_NOINTERFACE;
        }
        STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
        STDMETHODIMP_(ULONG) Release() override { LONG c = InterlockedDecrement(&ref_); if (!c) delete this; return c; }
        STDMETHODIMP DragEnter(IDataObject* d, DWORD keys, POINTL, DWORD* eff) override
        {
            canDrop_ = HasHDrop(d); *eff = EffectFor(keys, canDrop_); return S_OK;
        }
        STDMETHODIMP DragOver(DWORD keys, POINTL, DWORD* eff) override { *eff = EffectFor(keys, canDrop_); return S_OK; }
        STDMETHODIMP DragLeave() override { canDrop_ = false; return S_OK; }
        STDMETHODIMP Drop(IDataObject* d, DWORD keys, POINTL pt, DWORD* eff) override
        {
            const std::vector<std::wstring> paths = HDropPaths(d);
            const bool move = (keys & MK_SHIFT) != 0;
            if (!paths.empty() && g_onDrop) { POINT p{ pt.x, pt.y }; g_onDrop(paths, p, move); }
            *eff = move ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
            return S_OK;
        }
    };

    DropTarget* g_target = nullptr;
}

void InitDragDrop(HWND hwnd, std::function<void(const std::vector<std::wstring>&, POINT, bool)> onDrop)
{
    g_onDrop = std::move(onDrop);
    g_target = new DropTarget();
    RegisterDragDrop(hwnd, g_target);
}

void ShutdownDragDrop(HWND hwnd)
{
    RevokeDragDrop(hwnd);
    if (g_target) { g_target->Release(); g_target = nullptr; }
    g_onDrop = nullptr;
}

void StartFileDrag(HWND hwnd, const std::wstring& folder, const std::vector<std::wstring>& names)
{
    if (folder.empty() || names.empty()) return;

    PIDLIST_ABSOLUTE folderPidl = nullptr;
    if (FAILED(SHParseDisplayName(folder.c_str(), nullptr, &folderPidl, 0, nullptr)) || !folderPidl) return;

    IShellFolder* sf = nullptr;
    if (SUCCEEDED(SHBindToObject(nullptr, folderPidl, nullptr, IID_IShellFolder, reinterpret_cast<void**>(&sf))) && sf)
    {
        std::vector<PITEMID_CHILD> kids;
        for (const std::wstring& n : names)
        {
            PIDLIST_RELATIVE child = nullptr;
            if (SUCCEEDED(sf->ParseDisplayName(hwnd, nullptr, const_cast<LPWSTR>(n.c_str()), nullptr, &child, nullptr)) && child)
                kids.push_back(reinterpret_cast<PITEMID_CHILD>(child));
        }
        if (!kids.empty())
        {
            IDataObject* data = nullptr;
            if (SUCCEEDED(sf->GetUIObjectOf(hwnd, static_cast<UINT>(kids.size()),
                    (PCUITEMID_CHILD_ARRAY)kids.data(), IID_IDataObject, nullptr,
                    reinterpret_cast<void**>(&data))) && data)
            {
                DropSource* src = new DropSource();
                DWORD effect = 0;
                DoDragDrop(data, src, DROPEFFECT_COPY | DROPEFFECT_MOVE, &effect);
                src->Release();
                data->Release();
            }
            for (PITEMID_CHILD c : kids) CoTaskMemFree(c);
        }
        sf->Release();
    }
    CoTaskMemFree(folderPidl);
}

void CopyOrMoveFiles(HWND hwnd, const std::vector<std::wstring>& src, const std::wstring& destFolder, bool move)
{
    if (src.empty() || destFolder.empty()) return;
    std::wstring from;
    for (const std::wstring& p : src) { from += p; from.push_back(L'\0'); }
    from.push_back(L'\0');
    std::wstring to = destFolder; to.push_back(L'\0'); to.push_back(L'\0');

    SHFILEOPSTRUCTW op{};
    op.hwnd = hwnd;
    op.wFunc = move ? FO_MOVE : FO_COPY;
    op.pFrom = from.c_str();
    op.pTo = to.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR;
    SHFileOperationW(&op);
}
