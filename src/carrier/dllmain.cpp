// ============================================================================
// version.dll —— 载体壳（**真实导出 + 运行时解析**，不用 .def 转发器）
//
// 借一个游戏本来就导入的系统 DLL 名，从而**完全不需要注入器**（SyringeEx）。
// 职责：① 把 17 个导出都实现成"跳板"，转到真正的系统 version.dll；
//       ② 加载真身 RA2YRLWResourceReader.dll 并调 RA2YRLWResourceReader_Initialize。
//
// ---------------------------------------------------------------------------
// ⚠️ 为什么不用 .def 转发器（`X = C:\windows\system32\version.X`）—— 踩过的坑：
//
// Wine 的默认模块加载序是 **`native,builtin`**：先试本地（游戏目录）那份，
// **失败就静默回退到内置版**。原来那份壳是**纯转发**，转发目标写成
// `C:\windows\system32\version`；在 **win64 prefix 里 system32 是 64 位目录**，
// 32 位进程要靠 WOW64 重定向才能落到 syswow64。
//   系统 Wine 11.19 会做这个重定向 → 本机自测全过；
//   **Proton 那条链没做 → 转发解析失败 → 整个壳加载失败 → 静默回退内置版**。
// 症状极具迷惑性：游戏照跑、日志一行没有、真身连模块列表都进不去。
//
// 现在改成**真实导出**：17 个裸跳板 `jmp dword ptr [槽]`，槽在 DllMain 里用
// `LoadLibraryExA("version.dll", LOAD_LIBRARY_SEARCH_SYSTEM32)` + `GetProcAddress` 填。
// 这条路不依赖 WOW64 重定向、不依赖路径形态、不依赖加载器的转发解析，
// 而且顺带能记录"哪个导出没解析到"。
//
// 为什么用 `jmp`（不是 `call`）：被转发的函数都是 WINAPI(stdcall)，由**被调用方**
// 清栈。我们尾跳到真函数，它的 `ret N` 直接生效；跳板的声明约定无关紧要，
// 也就不需要知道每个函数的参数个数。
// ============================================================================

#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// 17 个导出跳板与它们的槽 **定义在 thunks.cpp**
//
// 为什么分开：本文件必须 include <windows.h>（DllMain、LoadLibrary 等），
// 而 windows.h 会带进 winver.h 里那 17 个同名函数的声明 —— 在同一个翻译单元里
// 再定义同名函数会报 C2733。thunks.cpp 刻意不含任何头文件，就能用真实导出名。
// 这里只做声明，DllMain 里填槽。
// ---------------------------------------------------------------------------
extern "C" {
extern void* g_slot_GetFileVersionInfoA;
extern void* g_slot_GetFileVersionInfoW;
extern void* g_slot_GetFileVersionInfoExA;
extern void* g_slot_GetFileVersionInfoExW;
extern void* g_slot_GetFileVersionInfoByHandle;
extern void* g_slot_GetFileVersionInfoSizeA;
extern void* g_slot_GetFileVersionInfoSizeW;
extern void* g_slot_GetFileVersionInfoSizeExA;
extern void* g_slot_GetFileVersionInfoSizeExW;
extern void* g_slot_VerFindFileA;
extern void* g_slot_VerFindFileW;
extern void* g_slot_VerInstallFileA;
extern void* g_slot_VerInstallFileW;
extern void* g_slot_VerLanguageNameA;
extern void* g_slot_VerLanguageNameW;
extern void* g_slot_VerQueryValueA;
extern void* g_slot_VerQueryValueW;
}

namespace {

HANDLE g_logFile = INVALID_HANDLE_VALUE;

void CarrierLog(const char* fmt, ...) {
    if (g_logFile == INVALID_HANDLE_VALUE) {
        return;
    }
    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    char line[1200];
    int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "%s\r\n", body);
    if (n > 0) {
        DWORD written = 0;
        WriteFile(g_logFile, line, static_cast<DWORD>(n), &written, nullptr);
    }
}

// 在 selfDir（结尾带反斜杠）下重建 RA2YRLWResourceReader.carrier.log。
// 单独立一个日志文件的原因：真身会把 RA2YRLWResourceReader.log 清空重写，而"真身根本没被加载起来"
// 这种故障只有载体自己知道 —— 那份证据必须放在真身碰不到的文件里。
void OpenCarrierLog(const char* selfDir) {
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%sRA2YRLWResourceReader.carrier.log", selfDir);
    g_logFile = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_logFile != INVALID_HANDLE_VALUE) {
        const char bom[] = "\xEF\xBB\xBF";
        DWORD written = 0;
        WriteFile(g_logFile, bom, 3, &written, nullptr);
        CarrierLog("version.dll 载体壳启动（每次启动重建）");
    }
}

bool SelfDirOf(HINSTANCE self, char* out, size_t cap) {
    char path[MAX_PATH] = {0};
    if (GetModuleFileNameA(self, path, MAX_PATH) == 0) {
        return false;
    }
    char* slash = strrchr(path, '\\');
    if (!slash) {
        return false;
    }
    *(slash + 1) = '\0';
    _snprintf_s(out, cap, _TRUNCATE, "%s", path);
    return true;
}

// ---------------------------------------------------------------------------
// 解析系统 version.dll 并填满 17 个槽
// ---------------------------------------------------------------------------
struct SlotEntry {
    const char* name;
    void**      slot;
};

const SlotEntry kSlots[] = {
    {"GetFileVersionInfoA", &g_slot_GetFileVersionInfoA},
    {"GetFileVersionInfoW", &g_slot_GetFileVersionInfoW},
    {"GetFileVersionInfoExA", &g_slot_GetFileVersionInfoExA},
    {"GetFileVersionInfoExW", &g_slot_GetFileVersionInfoExW},
    {"GetFileVersionInfoByHandle", &g_slot_GetFileVersionInfoByHandle},
    {"GetFileVersionInfoSizeA", &g_slot_GetFileVersionInfoSizeA},
    {"GetFileVersionInfoSizeW", &g_slot_GetFileVersionInfoSizeW},
    {"GetFileVersionInfoSizeExA", &g_slot_GetFileVersionInfoSizeExA},
    {"GetFileVersionInfoSizeExW", &g_slot_GetFileVersionInfoSizeExW},
    {"VerFindFileA", &g_slot_VerFindFileA},
    {"VerFindFileW", &g_slot_VerFindFileW},
    {"VerInstallFileA", &g_slot_VerInstallFileA},
    {"VerInstallFileW", &g_slot_VerInstallFileW},
    {"VerLanguageNameA", &g_slot_VerLanguageNameA},
    {"VerLanguageNameW", &g_slot_VerLanguageNameW},
    {"VerQueryValueA", &g_slot_VerQueryValueA},
    {"VerQueryValueW", &g_slot_VerQueryValueW},
};

// 取本模块在内存里的地址范围（直接读自己的 PE 头，不依赖 psapi）。
// 用途：识别"解析到的函数其实落在我们自己模块里"这种自引用。
bool SelfRange(HINSTANCE self, uintptr_t& base, uintptr_t& end) {
    const auto* dos = reinterpret_cast<const unsigned char*>(self);
    base = reinterpret_cast<uintptr_t>(self);
    if (!dos || dos[0] != 'M' || dos[1] != 'Z') {
        return false;
    }
    const uint32_t e_lfanew = *reinterpret_cast<const uint32_t*>(dos + 0x3C);
    const unsigned char* pe = dos + e_lfanew;
    if (std::memcmp(pe, "PE\0\0", 4) != 0) {
        return false;
    }
    const uint32_t sizeOfImage = *reinterpret_cast<const uint32_t*>(pe + 24 + 56);  // PE32
    end = base + sizeOfImage;
    return true;
}

// ⚠️ 必须用**全路径**加载系统 version.dll。
//
// 按名字加载（哪怕带 LOAD_LIBRARY_SEARCH_SYSTEM32）会被加载器的
// "已加载模块按基名去重"命中 —— 而进程里那个叫 version.dll 的模块**就是我们自己**。
// 实测症状：返回的 handle 与本模块基址完全相同（都是 77F90000），
// 于是 17 个函数全部解析到**自己的跳板**上 → 任何调用都变成无限自递归。
// 这个坑很隐蔽：日志会显示"17/17 成功、0 缺失"，看起来一切正常。
HMODULE LoadRealVersionDll(HINSTANCE self) {
    char sysDir[MAX_PATH] = {0};
    const UINT n = GetSystemDirectoryA(sysDir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        CarrierLog("GetSystemDirectoryA 失败 err=%lu", GetLastError());
        return nullptr;
    }
    char full[MAX_PATH];
    _snprintf_s(full, sizeof(full), _TRUNCATE, "%s\\version.dll", sysDir);
    CarrierLog("系统目录 = %s，准备按全路径加载 %s", sysDir, full);

    // 注意：32 位进程下 GetSystemDirectoryA 返回的是 SysWOW64，正是我们要的那份 32 位 DLL
    HMODULE real = LoadLibraryExA(full, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!real) {
        CarrierLog("LoadLibraryExA(全路径) 失败 err=%lu，改试普通 LoadLibraryA", GetLastError());
        real = LoadLibraryA(full);
    }
    if (real == reinterpret_cast<HMODULE>(self)) {
        CarrierLog("**自引用：解析到的还是我们自己** —— 判为失败（不能用）");
        return nullptr;
    }
    CarrierLog("真系统 version.dll = %p（壳自身基址 = %p）", real, (void*)self);
    return real;
}

// 返回值 = 成功填上的槽数；-1 表示连真 DLL 都没加载起来
int ResolveAll(HINSTANCE self, /*out*/ int& missing, /*out*/ int& selfRefs) {
    missing = 0;
    selfRefs = 0;

    uintptr_t selfBase = 0, selfEnd = 0;
    const bool haveRange = SelfRange(self, selfBase, selfEnd);

    HMODULE real = LoadRealVersionDll(self);
    if (!real) {
        return -1;
    }

    int ok = 0;
    for (const SlotEntry& e : kSlots) {
        FARPROC p = GetProcAddress(real, e.name);

        // 自引用防线：解析结果一旦落在本模块内，说明拿到的还是我们自己的跳板。
        // 这种情况必须当失败处理 —— 否则调用时会无限自递归，而且表面上"全部解析成功"。
        if (p && haveRange) {
            const auto a = reinterpret_cast<uintptr_t>(p);
            if (a >= selfBase && a < selfEnd) {
                ++selfRefs;
                *e.slot = nullptr;
                CarrierLog("  ✘ %s 解析到了本模块内（0x%08X）—— 自引用，已置空", e.name,
                           static_cast<unsigned>(a));
                continue;
            }
        }

        *e.slot = reinterpret_cast<void*>(p);
        if (p) {
            ++ok;
        } else {
            ++missing;
            CarrierLog("  ⚠ 系统 version.dll 没有导出 %s（该槽留空；只有真的有人调用才会出事）",
                       e.name);
        }
    }
    const int total = static_cast<int>(sizeof(kSlots) / sizeof(kSlots[0]));
    CarrierLog("导出解析: %d/%d 成功，%d 个缺失，%d 个自引用", ok, total, missing, selfRefs);
    if (selfRefs > 0) {
        CarrierLog("**有自引用 —— 说明拿到的不是系统 DLL，壳不可用**");
    }
    return ok;
}

void LoadPayload(const char* selfDir) {
    char payloadPath[MAX_PATH];
    _snprintf_s(payloadPath, sizeof(payloadPath), _TRUNCATE, "%sRA2YRLWResourceReader.dll", selfDir);
    CarrierLog("加载真身: %s", payloadPath);

    HMODULE payload = LoadLibraryA(payloadPath);
    if (!payload) {
        CarrierLog("**真身加载失败** err=%lu —— 检查 RA2YRLWResourceReader.dll 是否存在且为 32 位",
                   GetLastError());
        return;
    }
    CarrierLog("真身已加载 handle=%p，准备调 RA2YRLWResourceReader_Initialize", payload);

    using InitFn = void(WINAPI*)();
    auto init = reinterpret_cast<InitFn>(GetProcAddress(payload, "RA2YRLWResourceReader_Initialize"));
    if (!init) {
        CarrierLog("**找不到导出 RA2YRLWResourceReader_Initialize** err=%lu", GetLastError());
        return;
    }
    init();
    CarrierLog("RA2YRLWResourceReader_Initialize 已调用");
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);

        char selfDir[MAX_PATH] = {0};
        if (SelfDirOf(hinst, selfDir, sizeof(selfDir))) {
            OpenCarrierLog(selfDir);
        }

        int missing = 0;
        int selfRefs = 0;
        const int resolved = ResolveAll(hinst, missing, selfRefs);

        CarrierLog("壳的自身基址=%p，自身路径目录=%s", (void*)hinst,
                   selfDir[0] ? selfDir : "(取不到)");

        // 一个槽都没填上（真 DLL 没加载起来 / 全是自引用）→ 直接让加载失败。
        // 宁可让游戏起不来（错误明确），也不要留 17 个空槽或自递归跳板等着崩。
        // 注：Wine 的 `native,builtin` 序会在我们失败时自动回退到内置版，实际不会真的挡住游戏。
        if (resolved <= 0 || selfRefs > 0) {
            CarrierLog("**壳不可用（resolved=%d, selfRefs=%d），拒绝初始化**（返回 FALSE 让加载失败）",
                       resolved, selfRefs);
            if (g_logFile != INVALID_HANDLE_VALUE) {
                CloseHandle(g_logFile);
                g_logFile = INVALID_HANDLE_VALUE;
            }
            return FALSE;
        }

        if (selfDir[0]) {
            LoadPayload(selfDir);
        }

        if (g_logFile != INVALID_HANDLE_VALUE) {
            CloseHandle(g_logFile);
            g_logFile = INVALID_HANDLE_VALUE;
        }
    }
    return TRUE;
}
