#pragma once
#include <cstdint>
#include <cstddef>

// ============================================================================
// 单一事实来源：gamemd.exe (Build B) 里我们依赖的代码点与数据结构
//
// 这份表被**两处**使用，所以不会漂移：
//   1) 真身 RA2YRLWResourceReader.dll 的运行期探针 —— patch 之前必须先验，验不过就只写日志不动作；
//   2) 离线校验 tools/check_sites.py —— 直接用磁盘上的 gamemd.exe 读字节核对。
//      （它用正则解析本文件，所以**本文件的书写格式是有意义的**，别随手改格式。）
//
// 逐条证据都能用磁盘上的 gamemd.exe 现场核对（本文件只写结论与期望字节）。
// ⚠️ 结论只对 Build B（md5 见下）成立；Build A 布局不同，遇到就报"找不到"而不是改错地方。
// ============================================================================

namespace sites {

// --- 我们真正要 patch 的地方（Syringe 路的挂点）-----------------------------
// MixFileClass::Bootstrap 的**唯一成功出口** 0x53044A：
//     mov al,1        (B0 01)
//     pop edi         (5F)
//     pop esi         (5E)
//     pop ebp         (5D)
// 5 字节整指令；后面 0x53044F 是 `pop ebx`，再 `add esp,0x68; ret`。
//
// ⚠️ 为什么不是入口、也不是调用者续点（真链路实测）：
//   装在 Ares 的环境里，Ares 的 Expand_MIX_Deorg(0x52BB64) 会接管 `call Bootstrap`，
//   控制流**既不经过 0x52BB69 也不经过 0x52BB95**（断点确实装上了也不触发）；
//   而挂在 Bootstrap 自己的成功出口，谁调用的无所谓，一样会走到。
//   另外此处只在**成功**（al=1）时触发 —— 失败路径有 6 个别的 ret，都是 xor al,al。
constexpr uintptr_t kBootstrapOkEpilogue = 0x53044A;
constexpr uint8_t   kBootstrapOkEpilogueBytes[5] = { 0xB0, 0x01, 0x5F, 0x5E, 0x5D };
constexpr size_t    kBootstrapOkEpilogueLen = 5;

// 覆盖 5 字节后，原逻辑要从这里继续（`pop ebx`）
constexpr uintptr_t kBootstrapOkResume = 0x53044F;
constexpr uint8_t   kBootstrapOkResumeBytes[1] = { 0x5B };
constexpr size_t    kBootstrapOkResumeLen = 1;

// --- 旁证（不再是挂点）：MixFileClass::Bootstrap 入口 ------------------------
// 保留它有两个用处：① 判断 build 对不对的强指纹；② 载体路旧方案的历史证据。
// 载体路（没有 SyringeEx 的人）仍然用入口 detour（8 字节，够放 `FF 25 [槽]`）。
constexpr uintptr_t kBootstrapEntry = 0x5301A0;
constexpr uint8_t   kBootstrapEntryBytes[8] = { 0x83, 0xEC, 0x68, 0xA1, 0xD0, 0xC1, 0x81, 0x00 };
constexpr size_t    kBootstrapEntryLen = 8;

// 覆盖 8 字节后，原逻辑要从这里继续（`push ebx`；注意不是 0x5301A9）
constexpr uintptr_t kBootstrapResume = 0x5301A8;
constexpr uint8_t   kBootstrapResumeBytes[1] = { 0x53 };
constexpr size_t    kBootstrapResumeLen = 1;

// --- CSF：基表字符串表加载完成之后的插入点 ----------------------------------
// 0x6BD88B 处的 6 字节整指令：
//     84 C0        test al,al
//     75 15        jne  0x6BD8A4
//     33 C0        xor  eax,eax
//
// 它属于"语言 MIX + 基表 CSF"那个初始化函数（0x6BD7E3 起）：
//   0x6BD84E/0x6BD853  mov ecx,0x840D40("ra2md.str"); call StringTable::LoadFile
//   0x6BD884           mov cl,1
//   0x6BD886           call 0x4A3BF0        ← **预留空桩**（实体是 `mov al,1; ret`）
//   0x6BD88B           test al,al / jne ...（本挂点）
// 即：走到这里时基表已经加载成功、字符串表数组已填好、正式初始化尚未开始。
//
// ⚠️ 为什么挂这里而不是 Ares 占的 0x6BD886：
//   ① Ares 的 CSF_LoadExtraFiles(0x6BD886, size=5) 会让控制流**跳到** 0x6BD88B
//      （它的处理器末尾 `mov [regs+0x24],1`(AL=1) + `ret 0x6BD88B`），
//      所以挂 0x6BD88B 的钩子在"Ares 在场"时照样会执行，且与 Ares 的 5 字节不重叠；
//   ② Syringe 对同一地址的多个 hook 只在**都返回 0** 时才链式共存，Ares 返回非 0
//      （直接跳转），所以同地址共存是不可靠的。
// 证据与推导（Build B 反汇编 0x6BD7E3 那个初始化函数；
// Ares 3.0p1 的 CSF_LoadExtraFiles 处理器）。
constexpr uintptr_t kCsfAfterBaseLoad = 0x6BD88B;
constexpr uint8_t   kCsfAfterBaseLoadBytes[6] = { 0x84, 0xC0, 0x75, 0x15, 0x33, 0xC0 };
constexpr size_t    kCsfAfterBaseLoadLen = 6;

// 覆盖 6 字节后的续点 = 原 0x6BD891（`mov ecx,[ebp-0x10]`）
constexpr uintptr_t kCsfAfterBaseLoadResume = 0x6BD891;
constexpr uint8_t   kCsfAfterBaseLoadResumeBytes[6] = { 0x8B, 0x4D, 0xF0, 0x64, 0x89, 0x0D };
constexpr size_t    kCsfAfterBaseLoadResumeLen = 6;

// StringTable 的全局（与 YRpp/StringTable.h 里同名）
//   Labels[i] 是 40 字节 { char Name[32]; int NumValues; int FirstValueIndex; }
//   Values/ExtraValues 按 FirstValueIndex 索引；查找 = bsearch + 比较器 0x7C8D20
//   ⇒ **数组必须保持有序**（追加后要用引擎的 qsort 重排）
constexpr uintptr_t kStIsLoaded   = 0xB1CF80;
constexpr uintptr_t kStLabels     = 0xB1CF74;
constexpr uintptr_t kStValues     = 0xB1CF78;
constexpr uintptr_t kStExtraVals  = 0xB1CF7C;
constexpr uintptr_t kStLabelCount = 0xB1CF6C;
constexpr uintptr_t kStValueCount = 0xB1CF70;
constexpr uintptr_t kStMaxLabelLen = 0xB1CF58;
constexpr size_t    kCsfLabelSize = 0x28;
constexpr size_t    kCsfLabelNameMax = 0x20;   // 含结尾 NUL；名字上限 31 字节

// StringTable::LoadString（__fastcall：ecx=label, edx=pOutExtra；另有 2 个栈参，ret 8）
// 只在"合并后的自检"里用来问引擎自己要一次字符串 —— 走的就是游戏真实的查找路径。
constexpr uintptr_t kStringTableLoadString = 0x734E60;

// --- 引擎的 CRT 与文件系统（YRpp/CRT.h、YRpp/CCFileClass.h 的同一批）--------
constexpr uintptr_t kCrtNew      = 0x7C8E17;   // operator new
constexpr uintptr_t kCrtDelete   = 0x7C8B3D;   // operator delete
constexpr uintptr_t kCrtQsort    = 0x7C8B48;   // qsort(base, count, width, cmp)
constexpr uintptr_t kCrtBsearch  = 0x7C8E25;   // bsearch(key, base, count, width, cmp)
constexpr uintptr_t kCrtStrcmp   = 0x7C8D20;   // 引擎排序/查找 label 名用的比较器

// CCFileClass：**引擎自己的文件系统**（松散文件 + MIX 包都能找到）。
// 与 StringTable::LoadFile 用的是同一批实现（引擎 0x7346A0 里就是这么调的）。
// 我们按 __thiscall 直接调这些入口，对象只需要一块足够大的缓冲 +
// 4 字节对齐（类实际尺寸 < 0x60，这里留 0x100）。
constexpr uintptr_t kCCFileCtor   = 0x4739F0;  // CCFileClass(const char* pFileName)
constexpr uintptr_t kCCFileExists = 0x473C50;  // bool Exists(bool writeShared)
constexpr uintptr_t kCCFileOpen   = 0x473D10;  // bool Open(FileAccessMode)  → 1 = Read
constexpr uintptr_t kCCFileRead   = 0x473B10;  // int  ReadBytes(void*, int)
constexpr uintptr_t kCCFileSize   = 0x473C00;  // int  GetFileSize()
constexpr uintptr_t kCCFileClose  = 0x473CE0;  // void Close()
constexpr size_t    kCCFileObjectSize = 0x100;

// --- 构造 MixFileClass ------------------------------------------------------
// __thiscall；ecx=this（由 operator new(0x28) 得到）；栈上 2 个参数：
//   arg1 = const char* 文件名，arg2 = 常量全局 0x886980（60/60 调用点一致）
constexpr uintptr_t kMixFileCtor = 0x5B3C20;
constexpr uint8_t   kMixFileCtorBytes[6] = { 0x81, 0xEC, 0xDC, 0x01, 0x00, 0x00 };  // sub esp,0x1dc
constexpr size_t    kMixFileCtorLen = 6;
constexpr uintptr_t kCtorExtraGlobal = 0x886980;
constexpr size_t    kMixFileClassSize = 0x28;
// 引擎自己的 operator new（0x5301FD push 0x28 / 0x5301FF call 0x7c8e17 证实）。
// 必须用它分配 —— 退出期引擎会用自己的 free 释放这些对象，用我们的 CRT 堆会崩。
constexpr uintptr_t kEngineOperatorNew = 0x7C8E17;

// --- MixFileClass::Array（引擎自研 DynamicVectorClass<MixFileClass*>） ------
constexpr uintptr_t kMixArray = 0x884D90;
constexpr size_t    kArrayOffVtable   = 0x00;
constexpr size_t    kArrayOffItems    = 0x04;
constexpr size_t    kArrayOffCapacity = 0x08;   // ← 是 Capacity，不是 Count
constexpr size_t    kArrayOffIsAlloc  = 0x0D;
constexpr size_t    kArrayOffCount    = 0x10;   // ← 是 Count，不是 Capacity
constexpr size_t    kArrayOffCapIncr  = 0x14;
// 生存期 vtable 的槽 2（[vt+0x08]）是 SetCapacity(cap, pMem) __thiscall ret 8 → bool
constexpr size_t    kArrayVtableSlotSetCapacity = 2;

// --- 用于"确认找对了函数 / 找对了 build"的旁证 ------------------------------
// Bootstrap 里 sprintf 的模板串（整个 exe 里唯一）
constexpr uintptr_t kExpandMdTemplate = 0x82668C;
constexpr char      kExpandMdTemplateStr[] = "EXPANDMD%02d.MIX";
constexpr size_t    kExpandMdTemplateLen = 16;   // 含结尾 NUL
// 引用该字符串的那条 `push 0x82668C`（指令 0x5301C4，立即数在 +1）
constexpr uintptr_t kExpandMdPush = 0x5301C4;
constexpr uint8_t   kExpandMdPushBytes[5] = { 0x68, 0x8C, 0x66, 0x82, 0x00 };
constexpr size_t    kExpandMdPushLen = 5;

// 那条模板串的**原版**内容：只作文档与离线自检的参照，**运行期探针默认不检查它**。
//
// 理由：有些注入式扩展会在运行期把这条串就地改写成自己的模板（改成什么由那个扩展决定），
// 所以"它现在应该是什么"取决于玩家装了什么环境，不属于原版事实，也不该由本工具假设。
// 需要检查的人**自己重新编译**并给出期望串：
//     scripts/build.sh --probe-template 'EXPANDMD%02d.MIX'
// 不给开关 = 不检查（探针列表里会显示"已跳过"一行）。
//
// 两种打开方式（都只是"编译期开关"）：
//   ① 构建时给一个期望串（推荐，不用改源码）：
//        scripts/build.sh --probe-template 'EXPANDMD%02d.MIX'
//      脚本会生成 build/probe_template.h 并在编译时包含它；
//   ② 直接改下面那个默认值（源码编译的人最省事）。
#if defined(RA2YRLW_PROBE_TEMPLATE_HEADER)
#include "probe_template.h"   // 由 scripts/build.sh 生成
#endif
#ifndef RA2YRLW_PROBE_MIX_TEMPLATE_NAME
#define RA2YRLW_PROBE_MIX_TEMPLATE_NAME ""
#endif
constexpr const char* kProbeMixTemplateName = RA2YRLW_PROBE_MIX_TEMPLATE_NAME;

// --- 目标 exe 指纹（Build B）-----------------------------------------------
constexpr unsigned long long kExpectedExeSize = 4816864ULL;
constexpr const char* kExpectedExeMd5 = "56d582a1d6f3c144d3adc867d7a4d91b";

}  // namespace sites
