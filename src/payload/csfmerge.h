#pragma once
#include <string>
#include <vector>

// ============================================================================
// CsfMerge —— 「任意文件名的 CSF 加载」
//
// 目标：`[CSF]` 段里列出的任意命名 CSF，在引擎自己的字符串表（ra2md.csf）加载
// 完成之后**合并**进来。这条功能与 [Packs] 完全独立，但共用同一份真身与日志。
//
// 为什么这么实现（而不是接管引擎的解析例程）：
//   * 引擎的查找是 `bsearch(Labels, LabelCount, 0x28, cmp=0x7C8D20)`
//     ⇒ 追加后**必须保持有序**，我们用引擎自己的 qsort + 同一个比较器重排；
//   * `CSFLabel` 内存布局 40 字节 { char Name[32]; int NumValues; int FirstValueIndex; }，
//     `Values`/`ExtraValues` 按 FirstValueIndex 索引；
//   * 文件读取用引擎自己的 `CCFileClass` ⇒ 松散文件与我们注册的 MIX 包**都能读到**；
//   * 分配/释放/排序/查找全部走引擎的 CRT 入口（0x7C8E17/0x7C8B3D/0x7C8B48/0x7C8E25）。
//
// 时机：**两个触发点，一个一次性闸门**——
//   ① `0x6BD88B`（基表 CSF 加载之后、正式初始化之前；Ares 在场时它的处理器也跳到这里）；
//   ② `0x53044A`（Bootstrap 成功出口，[Packs] 用的那个）。
//   合并条件 =「基表已在内存里」+「[Packs] 那一步已经有结果（或本来就没配置）」，
//   谁先满足谁干活 ⇒ **不依赖两个时机的先后顺序**。
// ============================================================================

namespace CsfMerge {

struct Options {
    std::string gameDir;                    // 游戏目录，结尾**不带**反斜杠
    std::string subDir;                     // "" 或 "LWPack\\"（与 [Packs] 共用）
    int         rangeLo = 1;                // 编号族展开范围（与 [Packs] 共用）
    int         rangeHi = 99;
    bool        caseSensitive = false;      // 名字比较（与 [Packs] 共用）
    bool        overrideExisting = false;   // [CSF] Override：yes=我们的覆盖引擎的
    std::vector<std::string> files;         // [CSF] 的值，按书写顺序
    std::vector<std::string> excludes;      // 通配展开时复用的排除表
    std::vector<std::string> selfTest;      // [CSF] SelfTest：合并后逐个问引擎要一次
};

// 初始化期调用（解析完 INI 之后）。Options 会被复制保存。
void SetOptions(Options opt);

// [CSF] 段是否有内容 —— 没有就不装 CSF 的 carrier detour（Syringe 表是编译期固定的）
bool HasWork();

// 初始化期写日志：把清单、展开结果、缺失项写出来（合并还没发生）
void LogPlan();

// 合并（幂等 + 条件闸门）。两个触发点都调它。
//   trigger = 触发者说明，只用于日志。
// 返回 true 表示"本次真的做了合并"。
bool TryMerge(const char* trigger);

// 合并是否已经完成（诊断用）
bool Done();

}  // namespace CsfMerge
