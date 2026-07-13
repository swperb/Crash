// ShellContextMenu — real Windows shell context menus (design doc §6.4).
//
// Right-clicking an item builds its menu from the shell (IShellFolder ->
// GetUIObjectOf -> IContextMenu), so every installed shell extension shows up
// just like in Explorer. Right-clicking empty space shows the folder's
// background menu (New / Paste / …) via CreateViewObject.
//
// §6.4 wants third-party shell extensions isolated so a broken one can't take
// the whole process down. Full out-of-proc isolation is a later effort; here we
// wrap every call into third-party extension code (QueryContextMenu,
// InvokeCommand, HandleMenuMsg) in structured exception handling so a crashing
// extension is contained rather than fatal.
#pragma once
#include "common.h"
#include <functional>
#include <string>
#include <vector>

struct IContextMenu;   // fwd (full defs come from <shlobj.h> in the .cpp)
struct IContextMenu2;
struct IContextMenu3;

class ShellContextMenu
{
public:
    // Menu for a single filesystem item (file / folder / drive root).
    void ShowForItem(HWND hwnd, const std::wstring& fullPath, POINT screenPt);
    // Menu for one or more items sharing a parent folder (multi-selection).
    void ShowForItems(HWND hwnd, const std::wstring& folderPath,
                      const std::vector<std::wstring>& names, POINT screenPt);
    // Background menu for a filesystem folder.
    void ShowForFolder(HWND hwnd, const std::wstring& folderPath, POINT screenPt);

    // Forward menu messages during tracking (call from WndProc). Returns true
    // if the active context menu consumed the message.
    bool HandleMenuMsg(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT* result);

    // Invoked after a command runs (e.g. to reload the current directory).
    std::function<void()> onAfterInvoke;

private:
    void Track(HWND hwnd, IContextMenu* cm, POINT screenPt);

    ComPtr<IContextMenu2> cm2_;   // set only while a menu is tracking
    ComPtr<IContextMenu3> cm3_;
};
