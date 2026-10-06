#include "packlist.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>

namespace PackList {
namespace {

std::string ToLower(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string ToUpper(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

bool HasWildcard(const std::string& s) {
    return s.find_first_of("*?[") != std::string::npos;
}

// 目标目录的索引：一次枚举，后面全部查它。
// 这样做是为了复现 Windows 的**不区分大小写**语义（本机是 Linux，直接
// GetFileAttributes 会区分大小写），也为了拿到文件名在磁盘上的真实写法。
struct DirIndex {
    std::vector<std::string> names;               // 按不区分大小写排序
    std::set<std::string>    lowerSet;
    std::set<std::string>    exactSet;

    void Build(const std::string& dir) {
        names.clear();
        lowerSet.clear();
        exactSet.clear();
        std::string pattern = dir + "\\*";
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) {
            return;
        }
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                continue;
            }
            names.emplace_back(fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);

        for (const auto& n : names) {
            lowerSet.insert(ToLower(n));
            exactSet.insert(n);
        }
        std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
            std::string la = ToLower(a), lb = ToLower(b);
            return la == lb ? a < b : la < lb;
        });
    }

    bool Has(const std::string& name, bool caseSensitive) const {
        return caseSensitive ? (exactSet.count(name) != 0) : (lowerSet.count(ToLower(name)) != 0);
    }

    // 返回磁盘上的真实写法（找不到就原样返回）
    std::string Actual(const std::string& name, bool caseSensitive) const {
        if (caseSensitive) {
            return exactSet.count(name) ? name : name;
        }
        std::string want = ToLower(name);
        for (const auto& n : names) {
            if (ToLower(n) == want) {
                return n;
            }
        }
        return name;
    }
};

// 编号族展开：把 %d / %02d / %03d 按 sprintf 语义代进去
std::vector<std::string> ExpandFamily(const std::string& entry, int lo, int hi) {
    std::vector<std::string> out;
    for (int n = lo; n <= hi; ++n) {
        std::string s;
        s.reserve(entry.size() + 8);
        for (size_t i = 0; i < entry.size();) {
            if (entry[i] != '%') {
                s.push_back(entry[i++]);
                continue;
            }
            // %[width]d
            size_t j = i + 1;
            std::string width;
            while (j < entry.size() && ::isdigit(static_cast<unsigned char>(entry[j]))) {
                width.push_back(entry[j++]);
            }
            if (j < entry.size() && entry[j] == 'd') {
                char num[32];
                if (width.empty()) {
                    _snprintf_s(num, sizeof(num), _TRUNCATE, "%d", n);
                } else {
                    int w = std::atoi(width.c_str());
                    if (w > 20) w = 20;
                    _snprintf_s(num, sizeof(num), _TRUNCATE, "%0*d", w, n);
                }
                s += num;
                i = j + 1;
            } else {
                s.push_back(entry[i++]);  // 不是 %d，原样保留
            }
        }
        out.push_back(s);
    }
    return out;
}

bool HasFamilyFormat(const std::string& s) {
    for (size_t i = 0; i + 1 < s.size(); ++i) {
        if (s[i] != '%') continue;
        size_t j = i + 1;
        while (j < s.size() && ::isdigit(static_cast<unsigned char>(s[j]))) ++j;
        if (j < s.size() && s[j] == 'd') return true;
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// 通配匹配：* ? [seq]，不区分大小写（与 Python fnmatch 的常用子集对齐）
// ---------------------------------------------------------------------------
bool WildcardMatch(const std::string& pattern, const std::string& name) {
    const std::string p = ToUpper(pattern);
    const std::string s = ToUpper(name);
    size_t pi = 0, si = 0, star = std::string::npos, mark = 0;

    while (si < s.size()) {
        if (pi < p.size() && (p[pi] == '?' || p[pi] == s[si])) {
            ++pi; ++si;
        } else if (pi < p.size() && p[pi] == '[') {
            // 字符集：[abc] / [a-z] / [!abc] 或 [^abc]
            size_t close = p.find(']', pi + 1);
            if (close == std::string::npos) {
                return false;  // 未闭合，判为不匹配
            }
            bool negate = (p[pi + 1] == '!' || p[pi + 1] == '^');
            size_t k = negate ? pi + 2 : pi + 1;
            bool hit = false;
            for (; k < close; ++k) {
                if (k + 2 < close && p[k + 1] == '-') {
                    if (s[si] >= p[k] && s[si] <= p[k + 2]) hit = true;
                    k += 2;
                } else if (s[si] == p[k]) {
                    hit = true;
                }
            }
            if (hit == negate) {
                return false;
            }
            pi = close + 1;
            ++si;
        } else if (pi < p.size() && p[pi] == '*') {
            star = pi++;
            mark = si;
        } else if (star != std::string::npos) {
            pi = star + 1;
            si = ++mark;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == '*') ++pi;
    return pi == p.size();
}

const std::vector<std::string>& BuiltinExcludes() {
    // **只列原版引擎自己会加载的包名**。别的注入式扩展额外加载什么名字，是它们自己定的，
    // 我们不去猜、也不写死在这里 —— 那种环境下使用者把名字填进 MixReader.ini 的
    // [Packs.Exclude] 即可（那边的说明里写了怎么用）。
    // 与 tools/check_packs.py 的 BUILTIN_EXCLUDES **必须逐条一致**。
    static const std::vector<std::string> kList = {
        // 引擎自带入口
        "expandmd*.mix",
        "ra2md.mix", "langmd.mix", "ra2.mix", "language.mix",
        // 地图 / 影片 / 缓存 / 主题 / 多人
        "mapsmd*.mix", "maps*.mix",
        "movmd*.mix", "moviemd*.mix", "movies*.mix",
        "ecache*.mix", "elocal*.mix",
        "thememd*.mix",
        "multimd*.mix", "multi*.mix",
        "local*.mix", "cache*.mix",
    };
    return kList;
}

bool LooksLikeEnginePack(const std::string& name) {
    for (const auto& p : BuiltinExcludes()) {
        if (WildcardMatch(p, name)) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// 主流程
// ---------------------------------------------------------------------------
std::vector<Entry> Resolve(const std::vector<std::string>& entries, const Options& opt) {
    DirIndex index;
    index.Build(opt.subDir.empty() ? opt.gameDir : (opt.gameDir + "\\" + opt.subDir));

    std::vector<std::string> allExcludes = BuiltinExcludes();
    allExcludes.insert(allExcludes.end(), opt.userExcludes.begin(), opt.userExcludes.end());

    std::set<std::string> seen;  // 大写名 → 已注册
    std::vector<Entry> out;

    for (const auto& raw : entries) {
        // 去两端空白
        size_t b = raw.find_first_not_of(" \t\r\n");
        size_t e = raw.find_last_not_of(" \t\r\n");
        if (b == std::string::npos) {
            continue;
        }
        const std::string entry = raw.substr(b, e - b + 1);

        Entry r;
        r.entry = entry;

        const bool isWild = HasWildcard(entry);
        const bool isFamily = !isWild && HasFamilyFormat(entry);
        r.kind = isWild ? "wildcard" : (isFamily ? "family" : "literal");

        std::vector<std::string> matched;
        if (isWild) {
            for (const auto& n : index.names) {
                if (WildcardMatch(entry, n)) {
                    matched.push_back(n);
                }
            }
        } else if (isFamily) {
            for (const auto& n : ExpandFamily(entry, opt.rangeLo, opt.rangeHi)) {
                if (index.Has(n, opt.caseSensitive)) {
                    matched.push_back(index.Actual(n, opt.caseSensitive));
                } else {
                    r.missing.push_back(n);
                }
            }
        } else {
            if (index.Has(entry, opt.caseSensitive)) {
                matched.push_back(index.Actual(entry, opt.caseSensitive));
            } else {
                r.missing.push_back(entry);
            }
        }

        for (const auto& name : matched) {
            // 排除表只作用于通配（见头部规则 5）
            if (isWild) {
                bool drop = false;
                for (const auto& p : allExcludes) {
                    if (WildcardMatch(p, name)) {
                        drop = true;
                        break;
                    }
                }
                if (drop) {
                    r.excluded.push_back(name);
                    continue;
                }
            }
            const std::string key = ToUpper(name);
            if (seen.count(key)) {
                r.duplicates.push_back(name);
                continue;
            }
            seen.insert(key);
            r.names.push_back(name);
        }
        out.push_back(std::move(r));
    }
    return out;
}

}  // namespace PackList
