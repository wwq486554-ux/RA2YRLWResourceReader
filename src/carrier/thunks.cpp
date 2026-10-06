// ============================================================================
// version.dll 载体壳的 17 个导出跳板
//
// ⚠️ 这个文件**故意不 include 任何头文件**（尤其是 <windows.h>）：
//    windows.h 会带进 winver.h，那里已经声明了 `GetFileVersionInfoA` 等 17 个同名
//    函数；我们再定义同名函数就会报 C2733「无法重载具有外部 "C" 链接的函数」。
//    跳板只需要 `extern "C"` + 内联汇编，本来就一个头文件都不需要。
//    —— 如果哪天有人手滑加了 windows.h，会立刻编译失败，这正是我们要的提醒。
//
// 每个跳板就是一句 `jmp dword ptr [槽]`：
//   * 用 **jmp 而不是 call** —— 被转发的函数都是 WINAPI(stdcall)，由**被调用方**清栈；
//     尾跳过去之后它的 `ret N` 直接生效，所以我们不必知道每个函数的参数个数，
//     跳板自己声明的调用约定也就无关紧要。
//   * 槽在 DllMain 里由 dllmain.cpp 填（LoadLibraryExA(SYSTEM32) + GetProcAddress）。
//   * 真实导出名由 src/carrier/version.def 指定（裸名字，链接器会自动匹配 cdecl 的 `_名字`）。
// ============================================================================

extern "C" void* g_slot_GetFileVersionInfoA = nullptr;
extern "C" void* g_slot_GetFileVersionInfoW = nullptr;
extern "C" void* g_slot_GetFileVersionInfoExA = nullptr;
extern "C" void* g_slot_GetFileVersionInfoExW = nullptr;
extern "C" void* g_slot_GetFileVersionInfoByHandle = nullptr;
extern "C" void* g_slot_GetFileVersionInfoSizeA = nullptr;
extern "C" void* g_slot_GetFileVersionInfoSizeW = nullptr;
extern "C" void* g_slot_GetFileVersionInfoSizeExA = nullptr;
extern "C" void* g_slot_GetFileVersionInfoSizeExW = nullptr;
extern "C" void* g_slot_VerFindFileA = nullptr;
extern "C" void* g_slot_VerFindFileW = nullptr;
extern "C" void* g_slot_VerInstallFileA = nullptr;
extern "C" void* g_slot_VerInstallFileW = nullptr;
extern "C" void* g_slot_VerLanguageNameA = nullptr;
extern "C" void* g_slot_VerLanguageNameW = nullptr;
extern "C" void* g_slot_VerQueryValueA = nullptr;
extern "C" void* g_slot_VerQueryValueW = nullptr;

#define RA2YRLW_THUNK(name)                                              \
    extern "C" __declspec(naked) void name(void) {                         \
        __asm { jmp dword ptr [g_slot_##name] }                            \
    }

RA2YRLW_THUNK(GetFileVersionInfoA)
RA2YRLW_THUNK(GetFileVersionInfoW)
RA2YRLW_THUNK(GetFileVersionInfoExA)
RA2YRLW_THUNK(GetFileVersionInfoExW)
RA2YRLW_THUNK(GetFileVersionInfoByHandle)
RA2YRLW_THUNK(GetFileVersionInfoSizeA)
RA2YRLW_THUNK(GetFileVersionInfoSizeW)
RA2YRLW_THUNK(GetFileVersionInfoSizeExA)
RA2YRLW_THUNK(GetFileVersionInfoSizeExW)
RA2YRLW_THUNK(VerFindFileA)
RA2YRLW_THUNK(VerFindFileW)
RA2YRLW_THUNK(VerInstallFileA)
RA2YRLW_THUNK(VerInstallFileW)
RA2YRLW_THUNK(VerLanguageNameA)
RA2YRLW_THUNK(VerLanguageNameW)
RA2YRLW_THUNK(VerQueryValueA)
RA2YRLW_THUNK(VerQueryValueW)
