// DragDrop — OLE drag-and-drop for files (drag out / between panes, drop in).
//
// Drop target: RegisterDragDrop on the window; dropped files are copied/moved
// into the target folder via SHFileOperation. Drag source: builds a shell
// IDataObject from the selection and runs DoDragDrop, so items can be dragged to
// Explorer, other apps, or the other pane.
#pragma once
#include "common.h"
#include <functional>
#include <string>
#include <vector>

// Register a drop target on hwnd. onDrop(paths, screenPt, move) performs the op.
void InitDragDrop(HWND hwnd, std::function<void(const std::vector<std::wstring>&, POINT, bool)> onDrop);
void ShutdownDragDrop(HWND hwnd);

// Begin an OLE drag of `names` (children of `folder`). Blocks until dropped.
void StartFileDrag(HWND hwnd, const std::wstring& folder, const std::vector<std::wstring>& names);

// Copy or move files into destFolder (used by the drop handler).
void CopyOrMoveFiles(HWND hwnd, const std::vector<std::wstring>& src, const std::wstring& destFolder, bool move);
