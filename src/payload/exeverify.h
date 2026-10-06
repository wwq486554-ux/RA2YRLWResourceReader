#pragma once
#include <string>

// ============================================================================
// 目标 gamemd.exe 校验
//
// 目的：确认我们写的是**对的那份** exe。两份常见 build 的布局有 6 处差异
// ，拿 Build A 的地址去改 Build B 会改错地方。
//
// 注意：校验失败**只告警，不阻断**。真正的安全闸是
// mixregister 里的运行期代码点探针 —— 那个才是"地址对不对"的直接证据。
// ============================================================================

namespace ExeVerify {

struct Result {
    bool                 ok = false;          // 大小与 md5 都对
    bool                 hashed = false;      // md5 是否算出来了（算不出来就只比大小）
    unsigned long long   size = 0;
    std::string          md5;                 // 小写十六进制
    std::string          note;                // 给日志用的一句话
};

Result Check(const std::string& exePath);

}  // namespace ExeVerify
