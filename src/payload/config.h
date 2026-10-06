#pragma once
#include <string>
#include <vector>

// ============================================================================
// RA2YRLWResourceReader.ini 解析
//
// 与 Win32 profile API 的语义严格对齐（因为我们就用它读）：
//   * 注释必须**独占一行** —— GetPrivateProfileString 不剥行尾注释，
//     写 `Log=yes  ; 说明` 会把 "  ; 说明" 当成值。这一点已写进 INI 模板的头部说明。
//   * 段名/键名不区分大小写。
//   * [Packs] 的顺序 = **文件书写顺序** —— 用 GetPrivateProfileSectionA 拿，
//     它按文件顺序返回 "key=value\0key=value\0\0"。不能用 GetPrivateProfileString 逐个取。
//
// 全部键都有默认值；INI 缺失或键缺失都不算错误。
// ============================================================================

namespace Config {

struct Data {
    bool        log           = true;    // [ResourceReader] Log
    bool        validateExe   = true;    // [ResourceReader] ValidateExe
    bool        caseSensitive = false;   // [ResourceReader] CaseSensitive
    bool        dryRun        = false;   // [ResourceReader] DryRun
    int         rangeLo       = 1;       // [ResourceReader] NumberRange 的起点
    int         rangeHi       = 99;      //                      终点（两端都含）
    std::string subDir;                  // [ResourceReader] SubDir，规范化成 "xxx\\" 或 ""
    std::vector<std::string> packs;      // [Packs]          值，按文件顺序
    std::vector<std::string> excludes;   // [Packs.Exclude]  值

    // [CSF]：要合并的字符串表（任意文件名），按文件顺序；先读的先合并
    std::vector<std::string> csfFiles;
    // [CSF] Override=：同名 label 谁赢。默认 **no** = 引擎的优先，我们只补引擎没有的。
    bool csfOverride = false;
    // [CSF] SelfTest=：合并后逐个用引擎自己的 LoadString 要一次，结果写日志（排错用）
    std::vector<std::string> csfSelfTest;
};

// 返回值：INI 是否存在。解析问题写进 warnings（不算致命）。
bool Load(const std::string& iniPath, Data& out, std::vector<std::string>& warnings);

}  // namespace Config
