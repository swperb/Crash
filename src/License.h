// License — the open-core Pro gate (design doc §2.3, §6.6; see docs/licensing.md).
//
// A license is a signed `Crash.lic` file: `key=value` lines plus an ECDSA
// (P-256 / SHA-256) signature. The app embeds only the PUBLIC key and verifies
// the signature offline — no phone-home. Signing keys are minted by the
// `crashlicense` dev tool; the private key never ships.
#pragma once
#include <string>

// True if a valid signed license is installed (%LOCALAPPDATA%\Crash\Crash.lic).
bool IsLicensed();

// Validate the .lic at srcPath; on success install it and return true.
bool ImportLicense(const std::wstring& srcPath);
