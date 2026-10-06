#include "exeverify.h"

#include <windows.h>
#include <wincrypt.h>

#include <cstdio>

#include "engine_sites.h"

namespace ExeVerify {
namespace {

// 用 CryptoAPI 算 MD5（advapi32）。不想自己塞一份 MD5 实现，
// 而且这条路径已经在本机工具链上验证过可用。
bool Md5File(const std::string& path, std::string& outHex) {
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return false;
    }

    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    bool ok = false;

    if (CryptAcquireContextA(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        if (CryptCreateHash(prov, CALG_MD5, 0, 0, &hash)) {
            char buf[64 * 1024];
            DWORD read = 0;
            ok = true;
            while (ReadFile(f, buf, sizeof(buf), &read, nullptr) && read > 0) {
                if (!CryptHashData(hash, reinterpret_cast<BYTE*>(buf), read, 0)) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                BYTE digest[16] = {0};
                DWORD len = sizeof(digest);
                if (CryptGetHashParam(hash, HP_HASHVAL, digest, &len, 0)) {
                    char hex[40];
                    for (int i = 0; i < 16; ++i) {
                        _snprintf_s(hex + i * 2, 3, _TRUNCATE, "%02x", digest[i]);
                    }
                    outHex.assign(hex, 32);
                } else {
                    ok = false;
                }
            }
            CryptDestroyHash(hash);
        }
        CryptReleaseContext(prov, 0);
    }

    CloseHandle(f);
    return ok;
}

}  // namespace

Result Check(const std::string& exePath) {
    Result r;

    if (GetFileAttributesA(exePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        r.note = "找不到 gamemd.exe: " + exePath;
        return r;
    }

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(exePath.c_str(), GetFileExInfoStandard, &fad)) {
        r.note = "读不到文件信息: " + exePath;
        return r;
    }
    r.size = (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;

    r.hashed = Md5File(exePath, r.md5);

    const bool sizeOk = (r.size == sites::kExpectedExeSize);
    bool md5Ok = false;
    if (r.hashed) {
        md5Ok = (_stricmp(r.md5.c_str(), sites::kExpectedExeMd5) == 0);
    }

    if (r.hashed) {
        r.ok = sizeOk && md5Ok;
        char buf[256];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "size=%llu(期望 %llu) md5=%s(期望 %s)",
                    r.size, sites::kExpectedExeSize, r.md5.c_str(), sites::kExpectedExeMd5);
        r.note = buf;
    } else {
        // 算不出 md5 就退化成只比大小，并明确说明
        r.ok = false;
        char buf[256];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "size=%llu(期望 %llu)；MD5 计算失败（CryptoAPI 不可用？）", r.size,
                    sites::kExpectedExeSize);
        r.note = buf;
    }
    return r;
}

}  // namespace ExeVerify
