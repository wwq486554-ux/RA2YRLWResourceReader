#pragma once
#include <string>
#include <vector>

// ============================================================================
// 引擎侧动作：代码点探针 + detour + 把 MIX 追加进 MixFileClass::Array
//
// 全部地址与期望字节来自 engine_sites.h（单一事实来源，离线也能核对）。
//
// 设计上的三条硬约束（都在真链路上复核过）：
//   1. **Syringe 路**：`.syhks00` 挂 **Bootstrap 唯一成功出口 0x53044A**
//      （不是入口，也不是调用者续点 0x52BB69 —— Ares 接管调用点后会绕过它）。
//   2. **载体路**：入口 detour 打在 0x5301A0，patch 用 `FF 25 [slot]` 绝对间接
//      跳转（6 字节）而不是 `E9 rel32` —— 我们的 DLL 可能被加载到离 0x5301A0
//      超过 2 GB 的地方，rel32 会溢出。（0x53044A 只有 5 字节，放不下 `FF 25`，
//      这也是载体路暂时留在入口的原因；两条路共用 OnBootstrapSuccess()。）
//   3. 追加元素**必须经 Array 生存期 vtable 的槽 2 调 SetCapacity**，
//      让引擎用自己的分配器扩容；手工写 Items/Capacity 会让退出期的引擎 free
//      去释放不属于它的指针 → 退出必崩。
// ============================================================================

namespace MixRegister {

struct SiteStatus {
    std::string        name;
    unsigned long long addr = 0;
    bool               readable = false;
    bool               matched = false;
    bool               critical = false;
    std::string        expectHex;
    std::string        gotHex;
    std::string        note;
};

// 运行期校验各代码点（**只读**，绝不写引擎内存）
std::vector<SiteStatus> Probe();
bool AllCriticalOk(const std::vector<SiteStatus>& sites);

// 安装 detour。失败写日志并返回 false（调用方必须不阻断游戏）
bool InstallDetour();
bool IsInstalled();

// 0x53044A 处现在是不是已经被别人（SyringeEx）装上了跳转（首字节 E9）？
// SyringeEx 的钩子在**我们的 DllMain 之后**才创建，所以初始化期看到的可能是
// "还没装"；这个判断只在 OnBootstrapSuccess() 的时机有意义，用作旁证日志。
bool IsHookedBySyringe();

// 指定地址是不是已经被装上了跳转（首字节 E9）——上面那个的通用版
bool IsHookedAt(unsigned long long addr);

// 描述"如果现在安装 detour，会往引擎里写什么字节"—— 不写任何内存。
//
// 两个用途：
//   1) DryRun 模式与诊断输出：让使用者能看见到底要改哪几个字节（DryRun 的价值就在这）；
//   2) 本机自测：patch 的编码（FF 25 + 槽地址）不需要真的能写内存就能被核对 ——
//      而本机因为 Wine 的地址布局占着低地址，确实没法把 gamemd 放到首选基址。
std::vector<std::string> DescribePatch();

// 待注册列表（由 Initialize 填好，hook 里用）
void SetPending(std::vector<std::string> namesWithSubDir);

// 「[Packs] 那一步已经有结果了」——两个来源：
//   * 确实有包要注册 → OnBootstrapSuccess() 里注册完之后标记；
//   * 本来就没有包要注册 → Initialize 里直接标记。
// CsfMerge 的合并条件之一是它（合并要读我们注册进去的 MIX 包里的 CSF）。
void MarkPacksReady(const char* how);
bool PacksReady();

// 「Bootstrap 成功了，开始追加注册」—— 两条入口共用，内部有一次性原子闸门：
//   * 载体路：入口 detour 里 Bootstrap 返回后调用（trigger = 说明文字）；
//   * Syringe 路：`.syhks00` 挂 0x53044A（Bootstrap 成功出口）的处理器调用。
// 谁先到谁干活，第二个是空操作。
// 注册完包之后会再调一次 CsfMerge::TryMerge()（[CSF] 的第二个触发点）。
void OnBootstrapSuccess(const char* trigger);

// 载体路的第二个 detour：0x6BD88B（基表 CSF 加载之后）。
// 只有 [CSF] 有配置时才装 —— 不配就不碰引擎内存。
// 返回是否装上了（或本来就已经装上）。
bool InstallCsfDetour();

// 追加注册（Bootstrap 返回后由 hook 调用）
struct RegisterResult {
    std::string name;
    bool        ok = false;
    std::string detail;
};
std::vector<RegisterResult> RegisterPacks(const std::vector<std::string>& names);

}  // namespace MixRegister
