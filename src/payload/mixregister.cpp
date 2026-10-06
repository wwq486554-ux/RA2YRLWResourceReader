#include "mixregister.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "engine_sites.h"
#include "csfmerge.h"
#include "log.h"

// ============================================================================
// 文件级状态
//
// ⚠️ 刻意放在**全局作用域**（不是命名空间、不是匿名命名空间）：
//    RA2YRLWResourceReader_BootstrapHook 是 __declspec(naked) 的内联汇编函数，
//    它的 `call dword ptr [符号]` 只能引用一个**简单符号名**，
//    写 MixRegister::xxx 这种限定名 MSVC 的内联汇编解析不了。
// ============================================================================
namespace {

int   g_installed = 0;
void* g_pTrampoline = nullptr;

// 两个"指针槽"：patch 与 trampoline 用 `FF 25 [槽地址]` 做绝对间接跳转。
// 不直接内嵌目标地址、也不用 E9 rel32 —— x86 没有"绝对直接 jmp"单指令，
// 而 rel32 受 ±2GB 限制，我们的 DLL 可能被加载到很远的地方。
void* volatile g_hookTarget = nullptr;    // → RA2YRLWResourceReader_BootstrapHook
void* volatile g_resumeTarget = nullptr;  // → 0x5301A8（Bootstrap 被打断处的续点）

std::vector<std::string> g_pending;

// [CSF] 的载体路 detour（0x6BD88B）用的槽与蹦床指针。
// 同样必须在全局作用域：naked 函数的内联汇编只能引用简单符号名。
int  g_csfInstalled = 0;
void* g_pCsfTrampoline = nullptr;         // 我们自己搭的 22 字节蹦床
void* volatile g_csfHookTarget = nullptr;   // → RA2YRLWResourceReader_CsfHook
void* volatile g_csfResumeTarget = nullptr; // → 0x6BD891（al==0 的续点）
void* volatile g_csfTakenTarget = nullptr;  // → 0x6BD8A4（al!=0 的跳转目标）

bool g_packsReady = false;

}  // namespace

// ---------------------------------------------------------------------------
// hook 与它的 C++ 工作体
//
// 调用链（注意栈平衡）：
//   0x52BB64 call 0x5301A0      ← 栈上已压好返回地址 0x52BB69
//   → patch 成 `FF 25 [槽]`，jmp 到 RA2YRLWResourceReader_BootstrapHook（jmp，不压栈）
//   → `call trampoline`：压入 hook 内的返回地址
//        trampoline = 原 8 字节 + `FF 25 [续点槽]` → 跳进 0x5301A8 继续跑原 Bootstrap
//   → Bootstrap 跑到 ret，正好回到 hook 内（call 的下一条）
//   → pushad 保存（含 al = Bootstrap 的返回值）、干活、popad、ret → 回 0x52BB69
// ---------------------------------------------------------------------------
extern "C" void RA2YRLWResourceReader_OnBootstrapDone();

extern "C" __declspec(naked) void RA2YRLWResourceReader_BootstrapHook() {
    __asm {
        call dword ptr [g_pTrampoline]
        pushad
        call RA2YRLWResourceReader_OnBootstrapDone
        popad
        ret
    }
}

extern "C" void RA2YRLWResourceReader_OnBootstrapDone() {
    MixRegister::OnBootstrapSuccess("载体路：Bootstrap 入口 detour（0x5301A0）");
}

// ---------------------------------------------------------------------------
// [CSF] 的载体路 detour：0x6BD88B
//
// ⚠️ 与 Bootstrap 那个钩子不同，这个点**不能**"先跑原逻辑再干活"：
//    0x6BD88B 在 0x6BD7E3 那个大初始化函数的**中间**，跳到续点 0x6BD891 之后
//    控制流要等整个函数结束才返回，我们拿不回控制权。
//    所以顺序是：**先干活（pushad/popad 保护全部寄存器，含 AL）→ 再复刻原来那
//    6 个字节**（`test al,al` + `jne 0x6BD8A4` + `xor eax,eax`）**→ 跳续点**。
//    这 6 字节的复刻放在我们自己搭的蹦床里（见 InstallCsfDetour）：`jne` 是个
//    条件跳转，原样复制到别处会跳错，所以蹦床里把两条出口都展开成绝对跳转。
// ---------------------------------------------------------------------------
extern "C" void RA2YRLWResourceReader_OnBaseCsfDone();

extern "C" __declspec(naked) void RA2YRLWResourceReader_CsfHook() {
    __asm {
        pushad
        call RA2YRLWResourceReader_OnBaseCsfDone
        popad
        jmp dword ptr [g_pCsfTrampoline]
    }
}

extern "C" void RA2YRLWResourceReader_OnBaseCsfDone() {
    CsfMerge::TryMerge("载体路：基表 CSF 加载之后（0x6BD88B）");
}

namespace MixRegister {
namespace {

// 安全读内存：非游戏进程里 0x5301A0 根本没映射，直接解引用会崩
bool SafeRead(unsigned long long addr, void* out, size_t len) {
    const auto a = static_cast<uintptr_t>(addr);
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    if (mbi.State != MEM_COMMIT) {
        return false;
    }
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) {
        return false;
    }
    const auto base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    if (a + len > base + mbi.RegionSize) {
        return false;
    }
    std::memcpy(out, reinterpret_cast<const void*>(a), len);
    return true;
}

std::string ToHex(const unsigned char* p, size_t n) {
    std::string s;
    char b[4];
    for (size_t i = 0; i < n; ++i) {
        _snprintf_s(b, sizeof(b), _TRUNCATE, "%02X", p[i]);
        if (i) s += ' ';
        s += b;
    }
    return s;
}

char* ArrayBase() { return reinterpret_cast<char*>(static_cast<uintptr_t>(sites::kMixArray)); }

using SetCapFn = bool(__thiscall*)(void*, int, void*);

// 逐条复刻引擎自己的内联 push_back（0x530222 起那 15 条指令）
bool AppendToArray(void* p) {
    char* base = ArrayBase();
    void** vtable = *reinterpret_cast<void***>(base + sites::kArrayOffVtable);
    void** items = *reinterpret_cast<void***>(base + sites::kArrayOffItems);
    int capacity = *reinterpret_cast<int*>(base + sites::kArrayOffCapacity);
    bool isAllocated = *reinterpret_cast<bool*>(base + sites::kArrayOffIsAlloc);
    int count = *reinterpret_cast<int*>(base + sites::kArrayOffCount);
    int increment = *reinterpret_cast<int*>(base + sites::kArrayOffCapIncr);

    if (count >= capacity) {
        if (!isAllocated && capacity != 0) {
            Log::Line("[array] 放弃：Count>=Capacity 且未分配、Capacity 非 0");
            return false;
        }
        if (increment <= 0) {
            Log::Line("[array] 放弃：CapacityIncrement=%d <= 0", increment);
            return false;
        }
        if (!vtable) {
            Log::Line("[array] 放弃：Array vtable 为空（引擎还没初始化？）");
            return false;
        }
        auto setCapacity = reinterpret_cast<SetCapFn>(vtable[sites::kArrayVtableSlotSetCapacity]);
        if (!setCapacity) {
            Log::Line("[array] 放弃：vtable 槽 2（SetCapacity）为空");
            return false;
        }
        const int newCap = capacity + increment;
        if (!setCapacity(base, newCap, nullptr)) {
            Log::Line("[array] SetCapacity(%d) 失败", newCap);
            return false;
        }
        // SetCapacity 会改 Items/Capacity，重新读
        items = *reinterpret_cast<void***>(base + sites::kArrayOffItems);
        count = *reinterpret_cast<int*>(base + sites::kArrayOffCount);
    }

    items[count] = p;
    *reinterpret_cast<int*>(base + sites::kArrayOffCount) = count + 1;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// 探针（只读，绝不写引擎内存）
// ---------------------------------------------------------------------------
std::vector<SiteStatus> Probe() {
    struct Spec {
        const char*          name;
        unsigned long long   addr;
        const unsigned char* expect;
        size_t               len;
        bool                 critical;
    };

    const Spec specs[] = {
        // ★ 真正的挂点（Syringe 路）—— 也是"这些包什么时候追加"的时机判据
        { "Bootstrap 成功出口 0x53044A", sites::kBootstrapOkEpilogue,
          sites::kBootstrapOkEpilogueBytes, sites::kBootstrapOkEpilogueLen, true },
        { "Bootstrap 出口续点 0x53044F", sites::kBootstrapOkResume,
          sites::kBootstrapOkResumeBytes, sites::kBootstrapOkResumeLen, true },
        // 旁证：入口（载体路的旧挂点，保留作 build 指纹）+ 它的续点
        { "Bootstrap 入口 0x5301A0", sites::kBootstrapEntry,
          sites::kBootstrapEntryBytes, sites::kBootstrapEntryLen, true },
        { "Bootstrap 入口续点 0x5301A8", sites::kBootstrapResume,
          sites::kBootstrapResumeBytes, sites::kBootstrapResumeLen, true },
        { "MixFileClass 构造 0x5B3C20", sites::kMixFileCtor,
          sites::kMixFileCtorBytes, sites::kMixFileCtorLen, true },
        { "EXPANDMD push 0x5301C4", sites::kExpandMdPush,
          sites::kExpandMdPushBytes, sites::kExpandMdPushLen, true },
        // ★ [CSF] 的挂点（Syringe 路）—— 基表 CSF 加载之后、正式初始化之前。
        // 它是 6 字节整指令，所以 hook 的 size 也要写 6（Syringe 的补充规则见 §15）。
        { "基表 CSF 之后 0x6BD88B", sites::kCsfAfterBaseLoad,
          sites::kCsfAfterBaseLoadBytes, sites::kCsfAfterBaseLoadLen, true },
        { "CSF 挂点续点 0x6BD891", sites::kCsfAfterBaseLoadResume,
          sites::kCsfAfterBaseLoadResumeBytes, sites::kCsfAfterBaseLoadResumeLen, true },
    };

    std::vector<SiteStatus> out;

    for (const Spec& s : specs) {
        SiteStatus st;
        st.name = s.name;
        st.addr = s.addr;
        st.critical = s.critical;
        st.expectHex = ToHex(s.expect, s.len);

        unsigned char got[16] = {0};
        st.readable = SafeRead(s.addr, got, s.len);
        if (!st.readable) {
            st.gotHex = "(不可读)";
            st.note = "该地址未映射 —— 当前进程里基本可以确定没加载 gamemd.exe";
            out.push_back(st);
            continue;
        }
        st.gotHex = ToHex(got, s.len);
        st.matched = (std::memcmp(got, s.expect, s.len) == 0);
        if (!st.matched) {
            st.note = "字节与期望不符 —— exe 版本不对或地址表过期";
        }
        out.push_back(st);
    }

    // 包名模板串（0x82668C）—— **默认不检查**（理由见 engine_sites.h）：
    // 它可能被注入式扩展在运行期改写成自己的模板，"期望值"取决于玩家装了什么环境。
    // 想检查就在编译时给期望串：scripts/build.sh --probe-template '<期望串>'
    {
        SiteStatus st;
        st.name = "模板串 0x82668C";
        st.addr = sites::kExpandMdTemplate;
        st.critical = false;

        if (sites::kProbeMixTemplateName[0] == '\0') {
            st.readable = true;
            st.matched = true;
            st.expectHex = "(默认不检查)";
            st.gotHex = "(已跳过)";
            st.note = "默认不检查：这条串可能被注入式扩展在运行期改写，期望值取决于环境；"
                      "要检查就重新编译并加 --probe-template '<期望串>'";
        } else {
            st.expectHex = std::string("\"") + sites::kProbeMixTemplateName + "\"";
            char got[64] = {0};
            st.readable = SafeRead(sites::kExpandMdTemplate, got, sizeof(got) - 1);
            if (!st.readable) {
                st.gotHex = "(不可读)";
                st.note = "该地址未映射";
            } else {
                st.gotHex = std::string("\"") + got + "\"";
                st.matched = (std::strcmp(got, sites::kProbeMixTemplateName) == 0);
                if (!st.matched) {
                    st.note = "与编译时指定的期望串不符：这个环境可能改写了它，"
                              "或者 exe 版本不同。本项非关键，不影响挂钩";
                }
            }
        }
        out.push_back(st);
    }

    // 下面两项在初始化期还没被填（BSS = 0），所以只看"可读性"，不是失败判据
    {
        SiteStatus st;
        st.name = "数组 Array 0x884D90";
        st.addr = sites::kMixArray;
        st.critical = false;
        int capacity = 0;
        int count = 0;
        const bool okCap = SafeRead(sites::kMixArray + sites::kArrayOffCapacity, &capacity, 4);
        const bool okCnt = SafeRead(sites::kMixArray + sites::kArrayOffCount, &count, 4);
        st.readable = okCap && okCnt;
        if (st.readable) {
            char b[128];
            _snprintf_s(b, sizeof(b), _TRUNCATE, "Capacity=%d Count=%d（Bootstrap 之后才会被填）",
                        capacity, count);
            st.gotHex = b;
            st.matched = true;
        } else {
            st.gotHex = "(不可读)";
        }
        out.push_back(st);
    }
    {
        SiteStatus st;
        st.name = "构造 arg2 全局 0x886980";
        st.addr = sites::kCtorExtraGlobal;
        st.critical = false;
        int probe = 0;
        st.readable = SafeRead(sites::kCtorExtraGlobal, &probe, sizeof(probe));
        st.gotHex = st.readable ? "可读" : "(不可读)";
        st.matched = st.readable;
        st.note = "常量地址（60/60 调用点一致）；语义未确证，照抄即可";
        out.push_back(st);
    }

    return out;
}

bool AllCriticalOk(const std::vector<SiteStatus>& sites) {
    for (const auto& s : sites) {
        if (s.critical && !s.matched) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// detour
// ---------------------------------------------------------------------------
bool IsInstalled() { return g_installed != 0; }

// 0x53044A 首字节变成 E9 就说明有人（SyringeEx）在那儿装了跳转。
// 注意：SyringeEx 是在 LoadLibrary 完所有 DLL **之后**才创建钩子的，
// 所以 DllMain 期通常还看不到；只有在 0x53044A 真的执行到时才有意义。
bool IsHookedAt(unsigned long long addr) {
    unsigned char b = 0;
    if (!SafeRead(addr, &b, 1)) {
        return false;
    }
    return b == 0xE9;
}

bool IsHookedBySyringe() {
    return IsHookedAt(sites::kBootstrapOkEpilogue);
}

bool InstallDetour() {
    if (g_installed) {
        return true;
    }

    g_pTrampoline = VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_pTrampoline) {
        Log::Line("[detour] VirtualAlloc 失败，GetLastError=%lu", GetLastError());
        return false;
    }

    g_resumeTarget = reinterpret_cast<void*>(static_cast<uintptr_t>(sites::kBootstrapResume));
    g_hookTarget = reinterpret_cast<void*>(&RA2YRLWResourceReader_BootstrapHook);

    // trampoline = 原 8 字节 + `FF 25 [&g_resumeTarget]`
    auto* t = static_cast<unsigned char*>(g_pTrampoline);
    std::memcpy(t, reinterpret_cast<const void*>(static_cast<uintptr_t>(sites::kBootstrapEntry)),
                sites::kBootstrapEntryLen);
    t[8] = 0xFF;
    t[9] = 0x25;
    *reinterpret_cast<unsigned int*>(t + 10) =
        static_cast<unsigned int>(reinterpret_cast<uintptr_t>(&g_resumeTarget));

    // patch = `FF 25 [&g_hookTarget]` + 2×NOP（正好覆盖原来的 8 字节）
    unsigned char patch[8] = {0xFF, 0x25, 0, 0, 0, 0, 0x90, 0x90};
    *reinterpret_cast<unsigned int*>(patch + 2) =
        static_cast<unsigned int>(reinterpret_cast<uintptr_t>(&g_hookTarget));

    auto* target = reinterpret_cast<void*>(static_cast<uintptr_t>(sites::kBootstrapEntry));
    DWORD oldProtect = 0;
    if (!VirtualProtect(target, sites::kBootstrapEntryLen, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log::Line("[detour] VirtualProtect 失败，GetLastError=%lu", GetLastError());
        return false;
    }
    std::memcpy(target, patch, sizeof(patch));
    DWORD tmp = 0;
    VirtualProtect(target, sites::kBootstrapEntryLen, oldProtect, &tmp);
    FlushInstructionCache(GetCurrentProcess(), target, sites::kBootstrapEntryLen);

    g_installed = 1;
    Log::Line("[detour] 已挂钩 0x%08X  patch=%s  trampoline=%p", sites::kBootstrapEntry,
              ToHex(patch, sizeof(patch)).c_str(), g_pTrampoline);
    return true;
}

void SetPending(std::vector<std::string> namesWithSubDir) { g_pending = std::move(namesWithSubDir); }

void MarkPacksReady(const char* how) {
    if (g_packsReady) {
        return;
    }
    g_packsReady = true;
    Log::Line("[packs] 注册步骤已就绪（%s）—— [CSF] 若要读 MIX 包里的字符串表，从这里开始可以了", how);
}

bool PacksReady() { return g_packsReady; }

// ---------------------------------------------------------------------------
// [CSF] 的载体路 detour：0x6BD88B，覆盖 6 字节整指令
//
//   现场： 84 C0        test al,al
//          75 15        jne  0x6BD8A4
//          33 C0        xor  eax,eax
//   我们写：FF 25 [&g_csfHookTarget]        （6 字节，绝对间接跳转 → RA2YRLWResourceReader_CsfHook）
//   蹦床  ：复刻那 6 字节的语义 + 两条出口都用绝对跳转
//          test al,al
//          jne  <abs 0x6BD8A4>
//          xor  eax,eax
//          jmp  <abs 0x6BD891>
//
// 为什么这 6 字节刚好：它是**两条半指令**（test 2 + jne 2 + xor 2），整 6 字节；
// 而 `jne` 是条件跳转，直接复制到蹦床会跳错，所以在蹦床里展开成绝对跳转。
// ---------------------------------------------------------------------------
bool InstallCsfDetour() {
    if (g_csfInstalled) {
        return true;
    }

    g_pCsfTrampoline = VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_pCsfTrampoline) {
        Log::Line("[csf-detour] VirtualAlloc 失败，GetLastError=%lu", GetLastError());
        return false;
    }

    g_csfHookTarget    = reinterpret_cast<void*>(&RA2YRLWResourceReader_CsfHook);
    g_csfResumeTarget  = reinterpret_cast<void*>(static_cast<uintptr_t>(sites::kCsfAfterBaseLoadResume));
    g_csfTakenTarget   = reinterpret_cast<void*>(static_cast<uintptr_t>(0x6BD8A4));

    auto* t = static_cast<unsigned char*>(g_pCsfTrampoline);
    t[0] = 0x84; t[1] = 0xC0;                       // test al,al
    t[2] = 0x0F; t[3] = 0x85;                       // jne rel32
    *reinterpret_cast<int*>(t + 4) = static_cast<int>(
        0x6BD8A4u - (reinterpret_cast<uintptr_t>(t) + 8));
    t[8] = 0x33; t[9] = 0xC0;                       // xor eax,eax
    t[10] = 0xFF; t[11] = 0x25;                     // jmp dword ptr [&g_csfResumeTarget]
    *reinterpret_cast<unsigned int*>(t + 12) =
        static_cast<unsigned int>(reinterpret_cast<uintptr_t>(&g_csfResumeTarget));
    t[16] = 0xFF; t[17] = 0x25;                     // jmp dword ptr [&g_csfTakenTarget]
    *reinterpret_cast<unsigned int*>(t + 18) =
        static_cast<unsigned int>(reinterpret_cast<uintptr_t>(&g_csfTakenTarget));

    unsigned char patch[6] = {0xFF, 0x25, 0, 0, 0, 0};
    *reinterpret_cast<unsigned int*>(patch + 2) =
        static_cast<unsigned int>(reinterpret_cast<uintptr_t>(&g_csfHookTarget));

    auto* target = reinterpret_cast<void*>(static_cast<uintptr_t>(sites::kCsfAfterBaseLoad));
    DWORD oldProtect = 0;
    if (!VirtualProtect(target, sites::kCsfAfterBaseLoadLen, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log::Line("[csf-detour] VirtualProtect 失败，GetLastError=%lu", GetLastError());
        return false;
    }
    std::memcpy(target, patch, sizeof(patch));
    DWORD tmp = 0;
    VirtualProtect(target, sites::kCsfAfterBaseLoadLen, oldProtect, &tmp);
    FlushInstructionCache(GetCurrentProcess(), target, sites::kCsfAfterBaseLoadLen);

    g_csfInstalled = 1;
    Log::Line("[csf-detour] 已挂钩 0x%08X  patch=%s  trampoline=%p", sites::kCsfAfterBaseLoad,
              ToHex(patch, sizeof(patch)).c_str(), g_pCsfTrampoline);
    Log::Line("[csf-detour] 蹦床 22 字节 = %s", ToHex(t, 22).c_str());
    return true;
}

// ---------------------------------------------------------------------------
// 「Bootstrap 成功了」—— 两条入口共用的工作体，一次性闸门
//
// 两条路可能同时存在（载体壳在、SyringeEx 也在）：入口 detour 会在 Bootstrap
// 返回后喊一次，0x53044A 的 Syringe 处理器也会喊一次。这里保证只干一次活。
// ---------------------------------------------------------------------------
void OnBootstrapSuccess(const char* trigger) {
    static volatile LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0) != 0) {
        Log::Line("-------- 追加注册已由更早的入口完成，忽略本次触发（%s）--------", trigger);
        return;
    }

    Log::Line("-------- Bootstrap 已成功返回（触发者：%s）--------", trigger);
    const bool bootstrapHooked = IsInstalled() || IsHookedAt(sites::kBootstrapOkEpilogue);
    const bool csfHooked = g_csfInstalled || IsHookedAt(sites::kCsfAfterBaseLoad);
    if (!bootstrapHooked && !csfHooked) {
        Log::Line("[hook] 既没有我们的 detour、也没看到 Syringe 装的钩子，放弃（不该发生）");
        return;
    }

    if (!g_pending.empty()) {
        Log::Line("-------- 开始追加注册（%u 个包）--------", static_cast<unsigned>(g_pending.size()));
        const auto results = MixRegister::RegisterPacks(g_pending);
        unsigned ok = 0;
        for (const auto& r : results) {
            Log::Line("[注册] %-28s %s  %s", r.name.c_str(), r.ok ? "OK  " : "失败", r.detail.c_str());
            if (r.ok) ++ok;
        }
        Log::Line("[注册] 完成：成功 %u / 共 %u", ok, static_cast<unsigned>(results.size()));
    } else {
        Log::Line("[注册] [Packs] 为空，没有包要注册");
    }
    MarkPacksReady("Bootstrap 成功之后");

    // [CSF] 的第二个触发点（若基表 CSV 已载，这里就把字符串表合并掉）
    CsfMerge::TryMerge("Bootstrap 成功之后（0x53044A）");
}

std::vector<std::string> DescribePatch() {
    std::vector<std::string> out;
    char buf[320];

    // patch = `FF 25 [&g_hookTarget]` + 2×NOP（正好覆盖原来的 8 字节）
    unsigned char patch[8] = {0xFF, 0x25, 0, 0, 0, 0, 0x90, 0x90};
    *reinterpret_cast<unsigned int*>(patch + 2) =
        static_cast<unsigned int>(reinterpret_cast<uintptr_t>(&g_hookTarget));
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "目标 0x%08X  patch = %s", sites::kBootstrapEntry,
                ToHex(patch, sizeof(patch)).c_str());
    out.push_back(buf);

    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "槽   0x%08X -> hook %p（RA2YRLWResourceReader_BootstrapHook）",
                static_cast<unsigned>(reinterpret_cast<uintptr_t>(&g_hookTarget)),
                reinterpret_cast<void*>(&RA2YRLWResourceReader_BootstrapHook));
    out.push_back(buf);

    // trampoline = 原 8 字节 + `FF 25 [&g_resumeTarget]`
    unsigned char orig[sites::kBootstrapEntryLen] = {0};
    const bool readable = SafeRead(sites::kBootstrapEntry, orig, sites::kBootstrapEntryLen);
    unsigned char tramp[14] = {0};
    std::memcpy(tramp, orig, sites::kBootstrapEntryLen);
    tramp[8] = 0xFF;
    tramp[9] = 0x25;
    *reinterpret_cast<unsigned int*>(tramp + 10) =
        static_cast<unsigned int>(reinterpret_cast<uintptr_t>(&g_resumeTarget));
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "蹦床 %s%s", ToHex(tramp, sizeof(tramp)).c_str(),
                readable ? "" : "（原字节当前不可读，前 8 字节按 0 显示）");
    out.push_back(buf);

    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "续点 0x%08X（跑完原 8 字节后跳到这里）",
                sites::kBootstrapResume);
    out.push_back(buf);

    // [CSF] 的载体路 patch（只有配了 [CSF] 才装；这里同样先描述，便于 DryRun 核对）
    if (CsfMerge::HasWork()) {
        unsigned char csfPatch[6] = {0xFF, 0x25, 0, 0, 0, 0};
        *reinterpret_cast<unsigned int*>(csfPatch + 2) =
            static_cast<unsigned int>(reinterpret_cast<uintptr_t>(&g_csfHookTarget));
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "目标 0x%08X  patch = %s（[CSF] 用）",
                    sites::kCsfAfterBaseLoad, ToHex(csfPatch, sizeof(csfPatch)).c_str());
        out.push_back(buf);

        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "蹦床（%u 字节，自己搭）：84 C0 / 0F 85 <0x6BD8A4> / 33 C0 / FF 25 <0x6BD891> / FF 25 <0x6BD8A4>",
                    static_cast<unsigned>(22));
        out.push_back(buf);
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "续点 0x%08X（al==0）；al!=0 时跳 0x%08X",
                    sites::kCsfAfterBaseLoadResume, 0x6BD8A4);
        out.push_back(buf);
    }

    return out;
}

// ---------------------------------------------------------------------------
// 注册
// ---------------------------------------------------------------------------
std::vector<RegisterResult> RegisterPacks(const std::vector<std::string>& names) {
    using EngineNewFn = void*(__cdecl*)(size_t);
    using CtorFn = void*(__thiscall*)(void*, const char*, void*);

    auto engineNew = reinterpret_cast<EngineNewFn>(static_cast<uintptr_t>(sites::kEngineOperatorNew));
    auto ctor = reinterpret_cast<CtorFn>(static_cast<uintptr_t>(sites::kMixFileCtor));

    std::vector<RegisterResult> out;
    out.reserve(names.size());

    for (const auto& name : names) {
        RegisterResult rr;
        rr.name = name;

        // 必须用引擎自己的 operator new —— 退出期引擎用自己的 free 释放这些对象
        void* mem = engineNew(sites::kMixFileClassSize);
        if (!mem) {
            rr.detail = "引擎 operator new(0x28) 返回空";
            out.push_back(rr);
            continue;
        }

        void* obj = ctor(mem, name.c_str(),
                         reinterpret_cast<void*>(static_cast<uintptr_t>(sites::kCtorExtraGlobal)));
        if (!obj) {
            rr.detail = "构造函数返回空";
            out.push_back(rr);
            continue;
        }

        const auto* raw = static_cast<const char*>(obj);
        const int countFiles = *reinterpret_cast<const int*>(raw + 0x14);
        const int fileSize = *reinterpret_cast<const int*>(raw + 0x18);
        char d[160];
        _snprintf_s(d, sizeof(d), _TRUNCATE, "CountFiles=%d FileSize=%d", countFiles, fileSize);
        rr.detail = d;

        if (fileSize == 0) {
            // 引擎打开失败也会给个空对象；注册进去只会污染搜索表。
            // 对象已挂在引擎的链上，交给它的退出期清理，这里不重复注册。
            rr.detail += "；FileSize=0（疑未打开成功），跳过注册";
            out.push_back(rr);
            continue;
        }

        if (!AppendToArray(obj)) {
            rr.detail += "；追加进 Array 失败";
            out.push_back(rr);
            continue;
        }

        rr.ok = true;
        out.push_back(rr);
    }
    return out;
}

}  // namespace MixRegister
