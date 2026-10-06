#include "config.h"

#include <windows.h>

#include <cstdlib>
#include <cstring>

namespace Config {
namespace {

std::string GetStr(const std::string& ini, const char* section, const char* key,
                   const char* def) {
    char buf[1024] = {0};
    GetPrivateProfileStringA(section, key, def, buf, sizeof(buf), ini.c_str());
    std::string s(buf);
    // 去掉两端空白（profile API 通常已处理，这里再保险一次）
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    s = (b == std::string::npos) ? std::string() : s.substr(b, e - b + 1);
    return s;
}

bool GetBool(const std::string& ini, const char* section, const char* key, bool def) {
    std::string s = GetStr(ini, section, key, def ? "yes" : "no");
    for (auto& c : s) {
        c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    }
    if (s == "yes" || s == "1" || s == "true" || s == "on") return true;
    if (s == "no" || s == "0" || s == "false" || s == "off") return false;
    return def;
}

// 收集一个段里的所有值，保持**文件书写顺序**
// skipKeys：段里的**配置键**（不是列表项），不要当成条目收集。
//           [CSF] 段的 `Override=` / `SelfTest=` 就靠它排除，
//           否则会被当成文件名去引擎里找。
void CollectSection(const std::string& ini, const char* section,
                    std::vector<std::string>& out, std::vector<std::string>* warnings,
                    std::initializer_list<const char*> skipKeys = {}) {
    // 64 KB 足够放下任何合理的包列表；真的超了要明确报出来，不能静默截断
    constexpr DWORD kSize = 64 * 1024;
    std::vector<char> buf(kSize, 0);
    DWORD n = GetPrivateProfileSectionA(section, buf.data(), kSize, ini.c_str());
    if (n == 0) {
        return;  // 段不存在或为空
    }
    if (n >= kSize - 2) {
        if (warnings) {
            warnings->push_back(std::string("段 [") + section +
                                "] 内容超过 64 KB，尾部可能被截断");
        }
    }
    for (const char* p = buf.data(); *p; p += std::strlen(p) + 1) {
        const char* eq = std::strchr(p, '=');
        const std::string key(p, eq ? static_cast<size_t>(eq - p) : std::strlen(p));
        bool skip = false;
        for (const char* k : skipKeys) {
            if (_stricmp(key.c_str(), k) == 0) {
                skip = true;
                break;
            }
        }
        if (skip) {
            continue;
        }
        std::string value = eq ? std::string(eq + 1) : std::string(p);
        // 去两端空白
        size_t b = value.find_first_not_of(" \t\r\n");
        size_t e = value.find_last_not_of(" \t\r\n");
        if (b == std::string::npos) {
            continue;  // 空值跳过
        }
        out.push_back(value.substr(b, e - b + 1));
    }
}

bool ParseRange(const std::string& text, int& lo, int& hi, std::vector<std::string>& warnings) {
    // 形如 "1-99"；两端都含
    const char* s = text.c_str();
    char* end = nullptr;
    long a = std::strtol(s, &end, 10);
    if (end == s || *end != '-') {
        warnings.push_back("NumberRange 写法应为 起-止（例如 1-99），实际是 \"" + text +
                           "\"，已退回默认 1-99");
        return false;
    }
    const char* s2 = end + 1;
    long b = std::strtol(s2, &end, 10);
    if (end == s2) {
        warnings.push_back("NumberRange 的终点无法解析，已退回默认 1-99");
        return false;
    }
    if (a < 0 || b < 0 || a > b || b > 9999) {
        warnings.push_back("NumberRange 取值不合理（要求 0 <= 起 <= 止 <= 9999），已退回默认 1-99");
        return false;
    }
    lo = static_cast<int>(a);
    hi = static_cast<int>(b);
    return true;
}

}  // namespace

bool Load(const std::string& iniPath, Data& out, std::vector<std::string>& warnings) {
    out = Data();

    if (GetFileAttributesA(iniPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return false;
    }

    out.log           = GetBool(iniPath, "ResourceReader", "Log", true);
    out.validateExe   = GetBool(iniPath, "ResourceReader", "ValidateExe", true);
    out.caseSensitive = GetBool(iniPath, "ResourceReader", "CaseSensitive", false);
    out.dryRun        = GetBool(iniPath, "ResourceReader", "DryRun", false);

    std::string range = GetStr(iniPath, "ResourceReader", "NumberRange", "1-99");
    if (!ParseRange(range, out.rangeLo, out.rangeHi, warnings)) {
        out.rangeLo = 1;
        out.rangeHi = 99;
    }

    std::string sub = GetStr(iniPath, "ResourceReader", "SubDir", "");
    if (!sub.empty()) {
        // 规范化成 "xxx\"（统一用反斜杠；结尾补齐一个）
        for (auto& c : sub) {
            if (c == '/') c = '\\';
        }
        if (sub.back() != '\\') {
            sub.push_back('\\');
        }
        out.subDir = sub;
    }

    CollectSection(iniPath, "Packs", out.packs, &warnings);
    CollectSection(iniPath, "Packs.Exclude", out.excludes, &warnings);

    out.csfOverride = GetBool(iniPath, "CSF", "Override", false);
    CollectSection(iniPath, "CSF", out.csfFiles, &warnings, {"Override", "SelfTest"});

    // SelfTest=LABEL1,LABEL2 —— 逗号/分号分隔，合并后逐个问引擎要一次
    {
        const std::string raw = GetStr(iniPath, "CSF", "SelfTest", "");
        std::string cur;
        const auto flush = [&]() {
            size_t b = cur.find_first_not_of(" \t");
            size_t e = cur.find_last_not_of(" \t");
            if (b != std::string::npos) {
                out.csfSelfTest.push_back(cur.substr(b, e - b + 1));
            }
            cur.clear();
        };
        for (char c : raw) {
            if (c == ',' || c == ';') {
                flush();
            } else {
                cur.push_back(c);
            }
        }
        flush();
    }
    return true;
}

}  // namespace Config
