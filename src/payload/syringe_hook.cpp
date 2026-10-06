// ============================================================================
// syringe_hook.cpp —— 真身的 Syringe hook 表（`.syhks00`）
//
// 作用：让 SyringeEx **像加载 Ares / Phobos 一样直接注入 RA2YRLWResourceReader.dll** ——
//      不需要载体壳 version.dll、不需要改 Steam 启动项（`version=n,b` 那句可以删掉）。
//
// 机制（源码级证据：SyringeEx 的 FindDLLs / ParseHooksSection）：
//   * SyringeEx 扫游戏目录的 *.dll，**只认 `.syhks00` 段**；认到就
//     `Recognized DLL` + `LoadLibrary` + 按条目里的**名字**做 `GetProcAddress`。
//   * 所以处理器必须是**导出函数**（下面用 __declspec(dllexport)）。
//   * 条目结构必须严格符合 Syringe 的 ABI，`pack(push,16)` + `align(16)`：
//     每项 16 字节 {u32 地址; u32 覆盖字节数; const char* 名字}。
//   * 符号没人引用，`/OPT:REF` 会把整个段删掉 ⇒ 必须 `/include:` 强制保留。
//     （`extern "C"` 的变量在 x86 上符号是 `_名字`，写成 `__名字` 会 LNK2001。）
//   * 不需要 `.syexe00`、不需要导出 `SyringeHandshake`、不需要 `.json`、
//     不需要 `Syringe.json` 条目 —— 这些全是可选的（§12.1 逐条核过）。
//
// hook 语义（`.syringe_debugger.cpp` 第 507-614 行）：
//   调用块 = PUSHAD/PUSHFD → PUSH hookAddr → PUSH ESP → CALL 处理器 →
//            `MOV fs:0x14,EAX; CMP/JE`；返回 0 ⇒ POPFD/POPAD 后**重新执行被覆盖的
//            原指令**再跳回 hookAddr+5；返回非 0 ⇒ 直接跳到那个地址。
//   ⇒ 我们在处理器里随便改寄存器都无所谓（会被 POPAD 复原），
//     那 5 个原字节（`mov al,1; pop edi; pop esi; pop ebp`）由 Syringe 替我们执行，
//     所以**不需要 trampoline、不需要 VirtualProtect/VirtualAlloc**。
//
// 与载体路的关系：两条路都会调 MixRegister::OnBootstrapSuccess()，
// 内部有一次性的原子闸门 —— 谁先到谁干活，不会重复注册。
// ============================================================================

#include <windows.h>

#include "engine_sites.h"
#include "csfmerge.h"
#include "mixregister.h"

// ---------------------------------------------------------------------------
// Syringe hook 表
// ---------------------------------------------------------------------------
#pragma pack(push, 16)
__declspec(align(16)) struct hookdecl
{
    unsigned int hookAddr;   // 挂点（gamemd 里的绝对地址）
    unsigned int hookSize;   // 被覆盖的**整指令**字节数
    const char*  hookName;   // 处理器名 —— 必须能从本 DLL 导出表里解析到
};
#pragma pack(pop)

#pragma section(".syhks00", read, write)

// 挂点 = Bootstrap 唯一成功出口；size=5 覆盖 5 字节整指令（见 engine_sites.h 的说明）
extern "C" __declspec(allocate(".syhks00"))
hookdecl hk_RA2YRLWResourceReader_AfterBootstrap = {
    static_cast<unsigned int>(sites::kBootstrapOkEpilogue),
    static_cast<unsigned int>(sites::kBootstrapOkEpilogueLen),
    "RA2YRLWResourceReader_AfterBootstrap"
};

// [CSF] 的挂点 = 基表 CSF 加载之后（0x6BD88B），size=6 覆盖 6 字节整指令：
//     84 C0  test al,al / 75 15 jne 0x6BD8A4 / 33 C0 xor eax,eax
// ⚠️ size 必须是 6（不是 5）：5 字节会停在 `xor` 的第二个字节上。Syringe 的
//    patch = `E9 rel32` + NOP 填充到 size，蹦床**重建**这 6 字节（会把短跳转
//    加宽）后跳回 hook+5（补 NOP 处，滑到 hook+6）—— 语义与原来一致。
//    证据：.syringe_debugger.cpp 第 541-640 行（`code.assign(max(overridden, 5), NOP)`）。
extern "C" __declspec(allocate(".syhks00"))
hookdecl hk_RA2YRLWResourceReader_AfterBaseCsf = {
    static_cast<unsigned int>(sites::kCsfAfterBaseLoad),
    static_cast<unsigned int>(sites::kCsfAfterBaseLoadLen),
    "RA2YRLWResourceReader_AfterBaseCsf"
};

#pragma comment(linker, "/include:_hk_RA2YRLWResourceReader_AfterBootstrap")
#pragma comment(linker, "/include:_hk_RA2YRLWResourceReader_AfterBaseCsf")

// ---------------------------------------------------------------------------
// 处理器：SyringeEx 按名字 GetProcAddress 找到它们
//
// 参数是 Syringe 的 REGISTERS*（指向 PUSHAD/PUSHFD 帧），我们不需要它 —— 但
// 签名必须与调用约定一致：`extern "C" DWORD __cdecl f(REGISTERS*)`。
// 返回值：0 = 让 Syringe 继续执行被覆盖的原指令。
// ---------------------------------------------------------------------------
extern "C" __declspec(dllexport) unsigned long __cdecl
RA2YRLWResourceReader_AfterBootstrap(void* /*Syringe REGISTERS**/)
{
    MixRegister::OnBootstrapSuccess("Syringe hook 表（.syhks00）");
    return 0;
}

extern "C" __declspec(dllexport) unsigned long __cdecl
RA2YRLWResourceReader_AfterBaseCsf(void* /*Syringe REGISTERS**/)
{
    // 走到这里时基表字符串表已经载好；[CSF] 是否合并由内部闸门决定
    // （还要等 [Packs] 那一步出结果 —— 见 CsfMerge::TryMerge）。
    CsfMerge::TryMerge("Syringe hook 表：基表 CSF 之后（0x6BD88B）");
    return 0;
}
