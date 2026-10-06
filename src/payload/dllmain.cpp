// ============================================================================
// RA2YRLWResourceReader.dll（真身）—— 入口与总流程
//
// 两个导出（名字由 RA2YRLWResourceReader.def 指定，避免 stdcall 名字修饰的麻烦）：
//   RA2YRLWResourceReader_Initialize —— 载体壳调它；被 SyringeEx 注入时由 DllMain 自己调
//   RA2YRLWResourceReader_Diagnose   —— 只读诊断：重跑探针与包名解析并写日志，不碰引擎内存
//
// 为什么 DllMain 里就做初始化：SyringeEx 只是把 DLL LoadLibrary 进来，
// **不会**调我们的导出（它没有"调用导出"的概念）。所以要让"同一份真身两种入口"
// 成立，初始化必须能自己跑起来。载体壳那条路会再调一次导出，靠原子标志去重。
//
// 安全性：初始化只做 读INI / 校验文件 / 只读探针 / VirtualAlloc+VirtualProtect /
// 写日志 —— 不 LoadLibrary、不建线程、不做会死锁的事，在 DllMain 里是安全的。
// ============================================================================

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "config.h"
#include "csfmerge.h"
#include "engine_sites.h"
#include "exeverify.h"
#include "log.h"
#include "mixregister.h"
#include "packlist.h"
#include "version.h"

namespace {

LONG        g_state = 0;  // 0=未开始 1=进行中 2=已完成
std::string g_gameDir;

std::string DirNameOf(const std::string& path) {
    const size_t p = path.find_last_of("\\/");
    return (p == std::string::npos) ? path : path.substr(0, p);
}

// 游戏目录取**主模块**（gamemd.exe）所在目录，绝不依赖当前工作目录 ——
// 这是 cnc-ddraw 这类载体最常翻车的地方。
std::string SelfDir() {
    char buf[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return DirNameOf(buf);
}

void LogConfigSummary(const Config::Data& cfg, bool haveIni, const std::string& ini) {
    Log::Line("INI       : %s —— %s", ini.c_str(), haveIni ? "已读取" : "不存在（全部用默认值）");
    Log::Line("开关      : Log=%s ValidateExe=%s CaseSensitive=%s DryRun=%s",
              cfg.log ? "yes" : "no", cfg.validateExe ? "yes" : "no",
              cfg.caseSensitive ? "yes" : "no", cfg.dryRun ? "yes" : "no");
    Log::Line("编号族    : NumberRange=%d-%d   SubDir=%s", cfg.rangeLo, cfg.rangeHi,
              cfg.subDir.empty() ? "(无)" : cfg.subDir.c_str());
    Log::Line("条目数    : [Packs]=%u  [Packs.Exclude]=%u  [CSF]=%u",
              static_cast<unsigned>(cfg.packs.size()),
              static_cast<unsigned>(cfg.excludes.size()),
              static_cast<unsigned>(cfg.csfFiles.size()));
}

void LogPackEntries(const std::vector<PackList::Entry>& entries) {
    const auto dump = [](const char* label, const std::vector<std::string>& v) {
        if (v.empty()) {
            return;
        }
        const size_t kMax = 8;
        std::string s;
        for (size_t i = 0; i < v.size() && i < kMax; ++i) {
            if (i) s += ", ";
            s += v[i];
        }
        if (v.size() > kMax) {
            char buf[64];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, " …（共 %u 个）", static_cast<unsigned>(v.size()));
            s += buf;
        }
        Log::Line("         %s %s", label, s.c_str());
    };

    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        Log::Line("  [%u] %s   (%s)", static_cast<unsigned>(i), e.entry.c_str(), e.kind.c_str());
        for (const auto& n : e.names) {
            Log::Line("         注册 %s%s", n.c_str(),
                      PackList::LooksLikeEnginePack(n) ? "   ⚠ 疑似引擎自带名，重复注册会改变搜索顺序" : "");
        }
        dump("跳过（不存在）:", e.missing);
        dump("排除表丢弃    :", e.excluded);
        dump("重复丢弃      :", e.duplicates);
    }
}

// 探针 + 包名解析（+ 可选安装 detour）。Diagnose 与 Initialize 共用。
void RunChecksAndResolve(const Config::Data& cfg, const std::string& dir, bool install) {
    // ---- 1. exe 校验（只告警，不阻断）----
    if (cfg.validateExe) {
        const std::string exe = dir + "\\gamemd.exe";
        const ExeVerify::Result r = ExeVerify::Check(exe);
        Log::Line("exe 校验  : %s  %s", r.ok ? "通过" : "不匹配", r.note.c_str());
    } else {
        Log::Line("exe 校验  : 已关闭（ValidateExe=no）");
    }

    // ---- 2. 代码点探针（只读）----
    const auto sites = MixRegister::Probe();
    Log::Line("---- 代码点探针（只读）----");
    for (const auto& s : sites) {
        Log::Line("  [%s] %-24s 期望 %-24s 实测 %-24s%s", s.matched ? "通过" : "不符", s.name.c_str(),
                  s.expectHex.c_str(), s.gotHex.c_str(),
                  s.note.empty() ? "" : ("  ← " + s.note).c_str());
    }
    const bool criticalOk = MixRegister::AllCriticalOk(sites);
    Log::Line("探针结论  : %s", criticalOk ? "关键项全部通过" : "**有关键项未通过**");

    // 把"将要写入的字节"打出来 —— DryRun 的价值就在这；本机自测也靠它核对编码
    Log::Line("---- 若安装 detour，将写入的字节 ----");
    for (const auto& line : MixRegister::DescribePatch()) {
        Log::Line("  %s", line.c_str());
    }

    // ---- 3. 包名解析 ----
    PackList::Options opt;
    opt.gameDir = dir;
    opt.subDir = cfg.subDir;
    opt.rangeLo = cfg.rangeLo;
    opt.rangeHi = cfg.rangeHi;
    opt.caseSensitive = cfg.caseSensitive;
    opt.userExcludes = cfg.excludes;

    const auto entries = PackList::Resolve(cfg.packs, opt);
    Log::Line("---- 包名解析（规则与 tools/check_packs.py 一致）----");
    LogPackEntries(entries);

    std::vector<std::string> registerNames;
    for (const auto& e : entries) {
        for (const auto& n : e.names) {
            registerNames.push_back(cfg.subDir + n);  // 交给引擎的名字带子目录前缀
        }
    }
    Log::Line("待注册    : %u 个（按上面的顺序追加到搜索表尾部）",
              static_cast<unsigned>(registerNames.size()));
    MixRegister::SetPending(registerNames);

    // ---- 4. [CSF] 的参数与清单（真正的合并在运行期、基表载好之后）----
    CsfMerge::Options copt;
    copt.gameDir = dir;
    copt.subDir = cfg.subDir;
    copt.rangeLo = cfg.rangeLo;
    copt.rangeHi = cfg.rangeHi;
    copt.caseSensitive = cfg.caseSensitive;
    copt.overrideExisting = cfg.csfOverride;
    copt.files = cfg.csfFiles;
    copt.excludes = cfg.excludes;
    copt.selfTest = cfg.csfSelfTest;
    CsfMerge::SetOptions(copt);
    Log::Line("---- 字符串表合并（[CSF]）----");
    CsfMerge::LogPlan();

    if (!install) {
        return;
    }

    // ---- 5. 安装 detour（两处：Bootstrap 出口 / 基表 CSF 之后）----
    if (cfg.dryRun) {
        Log::Line("[dryrun] DryRun=yes —— 只做上面的检查，绝不改引擎内存");
        return;
    }
    if (!criticalOk) {
        Log::Line("[中止] 关键探针未通过，不安装 detour（避免改错地方）");
        return;
    }
    const bool wantPacks = !registerNames.empty();
    const bool wantCsf = CsfMerge::HasWork();
    if (!wantPacks && !wantCsf) {
        Log::Line("[跳过] [Packs] 与 [CSF] 都为空，不安装任何 detour");
        return;
    }
    if (wantPacks) {
        MixRegister::InstallDetour();
    }
    if (wantCsf) {
        MixRegister::InstallCsfDetour();
    }
    if (!wantPacks) {
        // 没有包要注册：[Packs] 那一步的"结果"此刻就成立，
        // 否则 [CSF] 会一直等它（Syringe 路下 OnBootstrapSuccess 也会标记一次）。
        MixRegister::MarkPacksReady("[Packs] 为空");
    }
}

void DoInitialize() {
    if (InterlockedCompareExchange(&g_state, 1, 0) != 0) {
        return;  // 已经在初始化 / 已完成
    }

    g_gameDir = SelfDir();
    const std::string ini = g_gameDir + "\\RA2YRLWResourceReader.ini";

    Config::Data cfg;
    std::vector<std::string> warnings;
    const bool haveIni = Config::Load(ini, cfg, warnings);

    if (cfg.log) {
        Log::Open(g_gameDir);
    }

    Log::Line("================ RA2YRLWResourceReader %s (%s) ================", RA2YRLW_VERSION,
              RA2YRLW_BUILD);
    Log::Line("游戏目录  : %s", g_gameDir.c_str());
    Log::Line("日志文件  : %s", Log::IsOpen() ? Log::Path().c_str() : "(未开启)");
    LogConfigSummary(cfg, haveIni, ini);
    for (const auto& w : warnings) {
        Log::Line("[INI 警告] %s", w.c_str());
    }

    RunChecksAndResolve(cfg, g_gameDir, true);

    Log::Line("初始化结束（[Packs] detour %s；[CSF] 参数 %s）",
              MixRegister::IsInstalled() ? "已安装，等 Bootstrap 返回" : "未安装",
              CsfMerge::HasWork() ? "已就绪，等基表载好" : "无");
    InterlockedExchange(&g_state, 2);
}

}  // namespace

extern "C" void RA2YRLWResourceReader_Initialize();

// DllMain 自己跑初始化 —— 这是"被 SyringeEx 直接注入"那条路能生效的关键
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        DoInitialize();
    } else if (reason == DLL_PROCESS_DETACH) {
        if (reserved == nullptr) {  // 正常卸载才写日志（进程终止时堆可能已不可用）
            Log::Line("进程卸载");
        }
        Log::Close();
    }
    return TRUE;
}

// 载体壳调用的入口；已被 DllMain 调过就是空操作
extern "C" void RA2YRLWResourceReader_Initialize() { DoInitialize(); }

// 只读诊断：不安装 detour，可随时调用
extern "C" void RA2YRLWResourceReader_Diagnose() {
    const std::string dir = g_gameDir.empty() ? SelfDir() : g_gameDir;
    if (!Log::IsOpen()) {
        Log::Open(dir);
    }
    const std::string ini = dir + "\\RA2YRLWResourceReader.ini";
    Config::Data cfg;
    std::vector<std::string> warnings;
    Config::Load(ini, cfg, warnings);

    Log::Line("================ RA2YRLWResourceReader_Diagnose ================");
    Log::Line("detour 状态: %s", MixRegister::IsInstalled() ? "已安装" : "未安装");
    RunChecksAndResolve(cfg, dir, false);
    Log::Line("诊断结束");
}
