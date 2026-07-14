// crashlicense — developer tool to generate signing keys and mint Crash Pro
// licenses (see docs/licensing.md, build order step 1).
//
//   crashlicense keygen
//       → crash_private.key + crash_public.key, and prints the public key as a
//         C++ array to paste into src/License.cpp.
//   crashlicense sign <crash_private.key> <out.lic> <license-id> <email>
//       → a signed Crash.lic file.
//
// Crypto: ECDSA P-256 + SHA-256 via Windows CNG (BCrypt) — no vendored crypto.
// The private key never ships; the app embeds only the public key.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

static std::vector<unsigned char> ReadFile(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> v(n > 0 ? n : 0);
    if (n > 0) { size_t r = fread(v.data(), 1, n, f); v.resize(r); }
    fclose(f);
    return v;
}
static void WriteFile(const char* path, const void* d, size_t n)
{
    FILE* f = fopen(path, "wb");
    if (f) { fwrite(d, 1, n, f); fclose(f); }
}
static std::string Base64(const unsigned char* d, DWORD n)
{
    DWORD out = 0;
    CryptBinaryToStringA(d, n, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &out);
    std::string s(out, 0);
    CryptBinaryToStringA(d, n, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, s.data(), &out);
    s.resize(out);
    return s;
}

static int KeyGen()
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) != 0) { printf("alg open failed\n"); return 1; }
    BCRYPT_KEY_HANDLE key = nullptr;
    BCryptGenerateKeyPair(alg, &key, 256, 0);
    BCryptFinalizeKeyPair(key, 0);

    DWORD n = 0;
    BCryptExportKey(key, nullptr, BCRYPT_ECCPRIVATE_BLOB, nullptr, 0, &n, 0);
    std::vector<unsigned char> priv(n);
    BCryptExportKey(key, nullptr, BCRYPT_ECCPRIVATE_BLOB, priv.data(), n, &n, 0);
    WriteFile("crash_private.key", priv.data(), priv.size());

    DWORD m = 0;
    BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, nullptr, 0, &m, 0);
    std::vector<unsigned char> pub(m);
    BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, pub.data(), m, &m, 0);
    WriteFile("crash_public.key", pub.data(), pub.size());

    printf("wrote crash_private.key (%u bytes), crash_public.key (%u bytes)\n",
           (unsigned)priv.size(), (unsigned)pub.size());
    printf("\n// --- paste into src/License.cpp ---\nstatic const unsigned char kPublicKey[] = {\n    ");
    for (size_t i = 0; i < pub.size(); ++i)
    {
        printf("0x%02X,", pub[i]);
        if (i % 12 == 11 && i + 1 < pub.size()) printf("\n    ");
    }
    printf("\n};\n");

    BCryptDestroyKey(key);
    BCryptCloseAlgorithmProvider(alg, 0);
    return 0;
}

static int Sign(int argc, char** argv)
{
    if (argc < 6) { printf("usage: crashlicense sign <crash_private.key> <out.lic> <license-id> <email>\n"); return 1; }
    std::vector<unsigned char> priv = ReadFile(argv[2]);
    if (priv.empty()) { printf("cannot read private key '%s'\n", argv[2]); return 1; }

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0);
    BCRYPT_KEY_HANDLE key = nullptr;
    if (BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPRIVATE_BLOB, &key, priv.data(), (DWORD)priv.size(), 0) != 0)
    { printf("invalid private key\n"); return 1; }

    char payload[1024];
    int pl = snprintf(payload, sizeof(payload),
        "lid=%s\nprod=crash\ned=pro\nemail=%s\niss=%lld\nexp=0\n",
        argv[4], argv[5], (long long)time(nullptr));

    unsigned char hash[32];
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, (PUCHAR)payload, (ULONG)pl, hash, 32) != 0)
    { printf("hash failed\n"); return 1; }

    DWORD sl = 0;
    BCryptSignHash(key, nullptr, hash, 32, nullptr, 0, &sl, 0);
    std::vector<unsigned char> sig(sl);
    if (BCryptSignHash(key, nullptr, hash, 32, sig.data(), sl, &sl, 0) != 0) { printf("sign failed\n"); return 1; }

    std::string s = Base64(sig.data(), sl);
    FILE* f = fopen(argv[3], "wb");
    if (!f) { printf("cannot write '%s'\n", argv[3]); return 1; }
    fwrite(payload, 1, pl, f);
    fprintf(f, "sig=%s\n", s.c_str());
    fclose(f);

    BCryptDestroyKey(key);
    BCryptCloseAlgorithmProvider(alg, 0);
    printf("wrote %s\n", argv[3]);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc >= 2 && strcmp(argv[1], "keygen") == 0) return KeyGen();
    if (argc >= 2 && strcmp(argv[1], "sign") == 0) return Sign(argc, argv);
    printf("crashlicense — Crash Pro license tool\n"
           "  crashlicense keygen\n"
           "  crashlicense sign <crash_private.key> <out.lic> <license-id> <email>\n");
    return 0;
}
