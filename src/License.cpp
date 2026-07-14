#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "License.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <shlobj.h>
#include <cstdio>
#include <string>
#include <vector>

// Public verification key (BCRYPT_ECCPUBLIC_BLOB, P-256) — from `crashlicense
// keygen`. Only the public half; the private key is held by the license service.
static const unsigned char kPublicKey[] = {
    0x45,0x43,0x53,0x31,0x20,0x00,0x00,0x00,0x23,0x45,0x9C,0xB1,
    0x68,0xA5,0xB0,0x95,0xA3,0x99,0xB5,0x19,0xA4,0x8D,0x41,0x03,
    0x09,0xAD,0xAD,0xBD,0xCB,0xFC,0x92,0x19,0xDB,0xA8,0xF2,0x6D,
    0xBD,0xB5,0xA9,0x80,0x19,0x3B,0xE8,0x97,0x7A,0x62,0xFD,0x05,
    0x4B,0xCA,0x10,0x7E,0xBC,0x1B,0x26,0x42,0x06,0xFF,0x0D,0x2A,
    0x21,0x9F,0x7A,0xFF,0x13,0x9B,0x3B,0x76,0x35,0x3B,0x92,0xD5,
};

namespace
{
    std::wstring LicensePath()
    {
        PWSTR local = nullptr; std::wstring p;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
        {
            p = local; CoTaskMemFree(local);
            p += L"\\Crash"; CreateDirectoryW(p.c_str(), nullptr);
            p += L"\\Crash.lic";
        }
        return p;
    }

    std::string ReadAll(const std::wstring& path)
    {
        FILE* f = _wfopen(path.c_str(), L"rb");
        if (!f) return {};
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        std::string s(n > 0 ? n : 0, 0);
        if (n > 0) { size_t r = fread(s.data(), 1, n, f); s.resize(r); }
        fclose(f);
        return s;
    }

    bool Base64Decode(const std::string& in, std::vector<unsigned char>& out)
    {
        DWORD n = 0;
        if (!CryptStringToBinaryA(in.c_str(), (DWORD)in.size(), CRYPT_STRING_BASE64, nullptr, &n, nullptr, nullptr)) return false;
        out.resize(n);
        return CryptStringToBinaryA(in.c_str(), (DWORD)in.size(), CRYPT_STRING_BASE64, out.data(), &n, nullptr, nullptr) != FALSE;
    }

    // Verify the whole .lic file: signature over the payload, then field checks.
    bool VerifyLicense(const std::string& file)
    {
        const size_t sp = file.find("\nsig=");
        if (sp == std::string::npos) return false;
        const std::string payload = file.substr(0, sp + 1);        // through the trailing '\n'
        size_t sigEnd = file.find('\n', sp + 5);
        if (sigEnd == std::string::npos) sigEnd = file.size();
        const std::string sigB64 = file.substr(sp + 5, sigEnd - (sp + 5));

        std::vector<unsigned char> sig;
        if (!Base64Decode(sigB64, sig) || sig.empty()) return false;

        unsigned char hash[32];
        if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                (PUCHAR)payload.data(), (ULONG)payload.size(), hash, sizeof(hash)) != 0)
            return false;

        BCRYPT_ALG_HANDLE alg = nullptr;
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) != 0) return false;
        BCRYPT_KEY_HANDLE key = nullptr;
        bool ok = false;
        if (BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key,
                (PUCHAR)kPublicKey, (ULONG)sizeof(kPublicKey), 0) == 0)
        {
            ok = BCryptVerifySignature(key, nullptr, hash, sizeof(hash), sig.data(), (ULONG)sig.size(), 0) == 0;
            BCryptDestroyKey(key);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
        if (!ok) return false;

        // Field checks: right product/edition. (exp==0 is perpetual; a dated
        // license would compare exp to the current time here.)
        return payload.find("prod=crash\n") != std::string::npos
            && payload.find("ed=pro\n") != std::string::npos;
    }
}

bool IsLicensed()
{
    const std::wstring p = LicensePath();
    if (p.empty()) return false;
    const std::string file = ReadAll(p);
    return !file.empty() && VerifyLicense(file);
}

bool ImportLicense(const std::wstring& srcPath)
{
    const std::string file = ReadAll(srcPath);
    if (file.empty() || !VerifyLicense(file)) return false;
    const std::wstring dst = LicensePath();
    if (dst.empty()) return false;
    FILE* f = _wfopen(dst.c_str(), L"wb");
    if (!f) return false;
    fwrite(file.data(), 1, file.size(), f);
    fclose(f);
    return true;
}
