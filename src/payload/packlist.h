#pragma once
#include <string>
#include <vector>

// ============================================================================
// 包名解析 —— **必须与 tools/check_packs.py 行为完全一致**
//
// 那份 Python 工具是同一套规则的可执行规格（spec），用来在不开游戏的情况下
// 验证包名解析。谁改了规则，两边一起改，并用 check_packs.py --selftest 兜住。
//
// 规则（逐条）：
//   1. 注册顺序 = [Packs] 在文件里的书写顺序（不是键名排序）。
//   2. 普通名：原样；不存在则跳过（告警，不是错误）。
//   3. 编号族（含 %d / %02d / %03d）：按 NumberRange 展开，逐个查存在性。
//   4. 通配（含 * ? [ ）：对目标目录里**实际存在的文件**匹配，结果按
//      **不区分大小写的文件名**排序（保证联机双方顺序一致）。
//   5. 排除表**只作用于通配展开的结果**。普通名与编号族是使用者明确写下的枚举，
//      一律放行 —— 明确写 `ra2md.mix` 就是要它，不该被内建表拦掉。
//   6. 同一个包被多条规则命中时只注册一次，以**最先命中**的那条为准。
// ============================================================================

namespace PackList {

struct Options {
    std::string gameDir;      // 游戏目录（结尾**不带**反斜杠）
    std::string subDir;       // "" 或 "LWPack\\"（结尾带反斜杠）
    int         rangeLo = 1;
    int         rangeHi = 99;
    bool        caseSensitive = false;
    std::vector<std::string> userExcludes;
};

struct Entry {
    std::string entry;                        // INI 里写的原文
    std::string kind;                         // literal / family / wildcard
    std::vector<std::string> names;           // 最终要注册的名字（不存在的不在内）
    std::vector<std::string> missing;         // 规则命中但不存在的
    std::vector<std::string> excluded;        // 被排除表丢弃的
    std::vector<std::string> duplicates;      // 与前面条目重复、被丢弃的
};

// 按书写顺序展开全部条目
std::vector<Entry> Resolve(const std::vector<std::string>& entries, const Options& opt);

// 内建排除表（防 `*.mix` 把引擎自带的包重复注册一遍）
const std::vector<std::string>& BuiltinExcludes();

// 判断某个名字是否"看起来是引擎自带的"（只用于告警，不拦截普通名/编号族）
bool LooksLikeEnginePack(const std::string& name);

// 大小写不敏感的通配匹配（支持 * ? [seq]），与 Python fnmatch 语义对齐
bool WildcardMatch(const std::string& pattern, const std::string& name);

}  // namespace PackList
