// License — the open-core Pro gate (design doc §2.3, §6.6). Pro features (the
// command palette, later scripting/advanced search) unlock when a valid license
// is present. Real builds would validate a signed key from the commerce backend;
// this is a stand-in with a demo key so the flow is exercisable end-to-end.
#pragma once
#include <string>

// The demo unlock key (shown in the unlock prompt).
inline const wchar_t* kDemoLicenseKey = L"CRASH-PRO-2026";

// True if a valid license file is present.
bool IsLicensed();

// Validate `key`; on success persist it and return true.
bool UnlockPro(const std::wstring& key);
