// ============================================================================
// csfmerge.cpp —— [CSF] 的实现：把任意命名的 CSF 合并进引擎的字符串表
//
// 总流程（本文件只讲"怎么落地"；引擎侧的证据见下面各处地址旁的注释）：
//   ① 初始化期：Config 把 [CSF] 与共用参数塞进 CsfMerge::Options，LogPlan() 写清单；
//   ② 运行期：两个触发点（0x6BD88B / 0x53044A）都调 TryMerge()，
//      条件 =「基表已载」+「[Packs] 那步已有结果」⇒ 不依赖两者先后；
//   ③ 合并：解析我们的 CSF → 查重（引擎自己的 bsearch）→ 追加/覆盖槽位 →
//      用引擎自己的 qsort 重排 → 最后**问引擎自己要一次字符串**作为自检。
//
// 铁律：
//   * 引擎内存一个字节都别动之前，先把所有内存分配完成 —— 分配失败就整体放弃，
//     引擎保持原样（半改状态比不干活危险得多）。
//   * 用引擎的 operator new/delete、qsort、bsearch、比较器 —— 语义与引擎一致。
//   * 解析全程带边界检查：坏文件只写日志，绝不越界读。
// ============================================================================

#include "csfmerge.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "engine_sites.h"
#include "log.h"
#include "mixregister.h"
#include "packlist.h"

namespace CsfMerge {
namespace {

// ---------------------------------------------------------------------------
// 引擎侧访问
// ---------------------------------------------------------------------------
bool SafeRead(unsigned long long addr, void* out, size_t len) {
    const auto a = static_cast<uintptr_t>(addr);
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
        return false;
    }
    const auto base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    if (a + len > base + mbi.RegionSize) {
        return false;
    }
    std::memcpy(out, reinterpret_cast<const void*>(a), len);
    return true;
}

template <typename T>
bool ReadGlobal(unsigned long long addr, T& out) {
    return SafeRead(addr, &out, sizeof(T));
}

template <typename T>
bool WriteGlobal(unsigned long long addr, const T& v) {
    DWORD old = 0;
    auto* p = reinterpret_cast<void*>(static_cast<uintptr_t>(addr));
    if (!VirtualProtect(p, sizeof(T), PAGE_READWRITE, &old)) {
        return false;
    }
    std::memcpy(p, &v, sizeof(T));
    DWORD tmp = 0;
    VirtualProtect(p, sizeof(T), old, &tmp);
    return true;
}

using EngineNewFn    = void*(__cdecl*)(size_t);
using EngineDeleteFn = void(__cdecl*)(void*);
using QsortFn        = void(__cdecl*)(void*, size_t, size_t, int(__cdecl*)(const void*, const void*));
using BsearchFn      = void*(__cdecl*)(const void*, const void*, size_t, size_t,
                                       int(__cdecl*)(const void*, const void*));
using CompareFn      = int(__cdecl*)(const void*, const void*);

EngineNewFn    EngineNew()     { return reinterpret_cast<EngineNewFn>(static_cast<uintptr_t>(sites::kCrtNew)); }
EngineDeleteFn EngineDelete()  { return reinterpret_cast<EngineDeleteFn>(static_cast<uintptr_t>(sites::kCrtDelete)); }
QsortFn        EngineQsort()   { return reinterpret_cast<QsortFn>(static_cast<uintptr_t>(sites::kCrtQsort)); }
BsearchFn      EngineBsearch() { return reinterpret_cast<BsearchFn>(static_cast<uintptr_t>(sites::kCrtBsearch)); }
CompareFn      EngineCompare() { return reinterpret_cast<CompareFn>(static_cast<uintptr_t>(sites::kCrtStrcmp)); }

// ---------------------------------------------------------------------------
// 用引擎自己的文件系统读整个文件（松散文件 + 已注册的 MIX 包都能找到）
//
// CCFileClass 就是 StringTable::LoadFile 用的那套（0x4739F0 / 0x473C50 /
// 0x473D10 / 0x473B10）。我们只借它的 Exists/Open/ReadBytes/GetFileSize/Close。
// 对象放在本地缓冲里（类实际尺寸 < 0x60，这里给 0x100）；不调析构 ——
// 对象里唯一堆分配是文件名的副本，一共只调几次，可忽略。
// ---------------------------------------------------------------------------
bool ReadWholeViaEngine(const char* name, std::vector<unsigned char>& out, std::string& why) {
    using CtorFn   = void*(__thiscall*)(void*, const char*);
    using ExistsFn = bool(__thiscall*)(void*, bool);
    using OpenFn   = bool(__thiscall*)(void*, int);
    using SizeFn   = int(__thiscall*)(void*);
    using ReadFn   = int(__thiscall*)(void*, void*, int);
    using CloseFn  = void(__thiscall*)(void*);

    auto ctor   = reinterpret_cast<CtorFn>(static_cast<uintptr_t>(sites::kCCFileCtor));
    auto exists = reinterpret_cast<ExistsFn>(static_cast<uintptr_t>(sites::kCCFileExists));
    auto open   = reinterpret_cast<OpenFn>(static_cast<uintptr_t>(sites::kCCFileOpen));
    auto getsz  = reinterpret_cast<SizeFn>(static_cast<uintptr_t>(sites::kCCFileSize));
    auto read   = reinterpret_cast<ReadFn>(static_cast<uintptr_t>(sites::kCCFileRead));
    auto close  = reinterpret_cast<CloseFn>(static_cast<uintptr_t>(sites::kCCFileClose));

    alignas(16) unsigned char obj[sites::kCCFileObjectSize];
    std::memset(obj, 0, sizeof(obj));

    ctor(obj, name);
    if (!exists(obj, false)) {
        why = "引擎文件系统里找不到（松散目录与已注册的 MIX 包都没命中）";
        return false;
    }
    if (!open(obj, 1 /*FileAccessMode::Read*/)) {
        why = "打开失败";
        return false;
    }
    const int size = getsz(obj);
    if (size <= 0) {
        close(obj);
        why = "文件为空或取大小失败";
        return false;
    }
    out.resize(static_cast<size_t>(size));
    const int got = read(obj, out.data(), size);
    close(obj);
    if (got != size) {
        out.clear();
        why = "读取字节数与文件大小不符";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// CSF 解析（磁盘格式；与 tools/csf-uiname/csf_tool.py 同一套）
//   header 24B: "CSF "(小端 = 0x43534620) + u32 version + u32 labels
//               + u32 strings + u32 unused + u32 language
//   label  : " LBL"(0x4C424C20) + u32 numStrings + u32 nameLen + name[nameLen]
//   value  : " RTS"(0x53545220) / "WRTS"(0x53545257) + u32 len(字符数)
//            + len*2 字节 UTF-16LE（**逐字节取反** 0xFF-b）
//            + [仅 WRTS] u32 xlen + xlen 字节附加串
//
// 引擎对文本做的空白归一化（0x734B4B..0x734BB4）也在这里复刻：
//   行首空格丢弃、连续空格折叠、换行/制表之前的那个空格删掉。
// ---------------------------------------------------------------------------
constexpr uint32_t kSigCsf  = 0x43534620;  // " FSC"
constexpr uint32_t kSigLbl  = 0x4C424C20;  // " LBL"
constexpr uint32_t kSigRts  = 0x53545220;  // " RTS"
constexpr uint32_t kSigWrts = 0x53545257;  // "WRTS"

struct ParsedValue {
    std::wstring text;
    std::string  extra;   // WRTS 独有，可为空
};

struct ParsedLabel {
    std::string              name;
    std::vector<ParsedValue> values;
};

struct ParsedFile {
    std::string              name;
    std::vector<ParsedLabel> labels;
    uint32_t                 version = 0;
    uint32_t                 language = 0;
};

class Cursor {
public:
    Cursor(const unsigned char* d, size_t n) : d_(d), n_(n) {}

    bool U32(uint32_t& v) {
        if (pos_ + 4 > n_) return false;
        std::memcpy(&v, d_ + pos_, 4);
        pos_ += 4;
        return true;
    }
    const unsigned char* Take(size_t k) {
        if (pos_ + k > n_) return nullptr;
        const unsigned char* p = d_ + pos_;
        pos_ += k;
        return p;
    }
    size_t Left() const { return n_ - pos_; }

private:
    const unsigned char* d_;
    size_t               n_;
    size_t               pos_ = 0;
};

void NormalizeWide(std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    wchar_t prev = 0;
    bool atLineStart = true;
    for (wchar_t c : s) {
        if (c == L'\0') {
            break;
        }
        if (c == L' ') {
            if (prev == L' ' || atLineStart) {
                continue;  // 折叠连续空格 / 丢弃行首空格
            }
            out.push_back(c);
            prev = c;
            continue;
        }
        if (c == L'\n' || c == L'\t') {
            if (prev == L' ' && !out.empty()) {
                out.pop_back();
            }
            out.push_back(c);
            prev = c;
            atLineStart = true;
            continue;
        }
        out.push_back(c);
        prev = c;
        atLineStart = false;
    }
    s.swap(out);
}

bool ParseCsf(const std::vector<unsigned char>& raw, ParsedFile& out, std::string& err,
              unsigned& skippedLongNames) {
    if (raw.size() < 24) {
        err = "文件小于 CSF 头（24 字节）";
        return false;
    }
    uint32_t sig = 0, version = 0, numLabels = 0, numValues = 0, language = 0;
    std::memcpy(&sig, raw.data(), 4);
    std::memcpy(&version, raw.data() + 4, 4);
    std::memcpy(&numLabels, raw.data() + 8, 4);
    std::memcpy(&numValues, raw.data() + 12, 4);
    std::memcpy(&language, raw.data() + 20, 4);
    if (sig != kSigCsf) {
        err = "签名不是 \"CSF \"（0x43534620）—— 不是 CSF 文件";
        return false;
    }
    if (numLabels > 200000u || numValues > 2000000u) {
        err = "头里的 label/string 数量不合理（疑似坏文件）";
        return false;
    }
    out.version = version;
    out.language = language;

    Cursor cur(raw.data(), raw.size());
    for (int i = 0; i < 6; ++i) {  // 跳过 24 字节头
        uint32_t dummy = 0;
        if (!cur.U32(dummy)) {
            err = "文件小得读不完 CSF 头";
            return false;
        }
    }

    for (uint32_t li = 0; li < numLabels; ++li) {
        uint32_t sig2 = 0, numStrings = 0, nameLen = 0;
        if (!cur.U32(sig2) || sig2 != kSigLbl) {
            err = "第 " + std::to_string(li) + " 条 label 的签名不是 \" LBL\"";
            return false;
        }
        // 磁盘上就是两个 u32：先是这条 label 的**值个数**，再是**名字长度**
        // （引擎 0x734A18→[esp+0x1c]=值个数、0x734A28→[esp+0x10]=名字长度；
        //   tools/csf-uiname/csf_tool.py 同样是 n, llen 的顺序）
        if (!cur.U32(numStrings) || !cur.U32(nameLen)) { err = "读 label 头越界"; return false; }
        if (nameLen > 4096u || numStrings > 10000u) {
            err = "第 " + std::to_string(li) + " 条 label 的长度字段不合理";
            return false;
        }
        const unsigned char* np = cur.Take(nameLen);
        if (!np) { err = "读 label 名越界"; return false; }

        ParsedLabel label;
        const bool nameOk = (nameLen >= 1) && (nameLen <= sites::kCsfLabelNameMax - 1);
        if (nameOk) {
            label.name.assign(reinterpret_cast<const char*>(np), nameLen);
        } else {
            // 引擎的名字缓冲是 32 字节（含结尾 NUL），超长名字会让引擎读溢出。
            // 值仍然按格式读完以保持同步，但这条不加入合并。
            ++skippedLongNames;
        }

        for (uint32_t vi = 0; vi < numStrings; ++vi) {
            uint32_t sigV = 0, len = 0;
            if (!cur.U32(sigV) || (sigV != kSigRts && sigV != kSigWrts)) {
                err = "label \"" + label.name + "\" 的值签名不是 \" RTS\"/\"WRTS\"";
                return false;
            }
            if (!cur.U32(len)) { err = "读值长度越界"; return false; }
            if (len > 100000u) { err = "值长度字段不合理"; return false; }
            const unsigned char* tp = cur.Take(static_cast<size_t>(len) * 2);
            if (!tp) { err = "读值文本越界"; return false; }

            ParsedValue val;
            val.text.resize(len);
            for (uint32_t k = 0; k < len; ++k) {
                // 磁盘上的文本是逐字节取反的 UTF-16LE（引擎 0x734B35 的 `not` 同义）
                const unsigned char lo = static_cast<unsigned char>(0xFF - tp[k * 2]);
                const unsigned char hi = static_cast<unsigned char>(0xFF - tp[k * 2 + 1]);
                val.text[k] = static_cast<wchar_t>(static_cast<unsigned>(lo) | (static_cast<unsigned>(hi) << 8));
            }
            NormalizeWide(val.text);

            if (sigV == kSigWrts) {
                uint32_t xlen = 0;
                if (!cur.U32(xlen)) { err = "读 WRTS 附加长度越界"; return false; }
                if (xlen > 100000u) { err = "WRTS 附加长度字段不合理"; return false; }
                if (xlen) {
                    const unsigned char* xp = cur.Take(xlen);
                    if (!xp) { err = "读 WRTS 附加数据越界"; return false; }
                    val.extra.assign(reinterpret_cast<const char*>(xp), xlen);
                }
            }
            label.values.push_back(std::move(val));
        }

        if (nameOk) {
            out.labels.push_back(std::move(label));
        }
    }
    return true;
}

std::string WideToUtf8(const wchar_t* s, size_t n) {
    if (!s || !n) {
        return std::string();
    }
    const int need = WideCharToMultiByte(CP_UTF8, 0, s, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
    if (need <= 0) {
        return std::string();
    }
    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, static_cast<int>(n), &out[0], need, nullptr, nullptr);
    return out;
}

// ---------------------------------------------------------------------------
// 模块状态
// ---------------------------------------------------------------------------
Options       g_opt;
bool          g_hasWork = false;
volatile LONG g_state = 0;          // 0=未做 1=在做 2=已结束
bool          g_mergeOk = false;
bool          g_notified = false;   // "还在等条件"只提示一次

bool BaseTableLoaded() {
    void* labels = nullptr;
    int   count = 0;
    int   isLoaded = 0;
    if (!ReadGlobal(sites::kStLabels, labels) || labels == nullptr) {
        return false;
    }
    if (!ReadGlobal(sites::kStLabelCount, count) || count <= 0) {
        return false;
    }
    if (!ReadGlobal(sites::kStIsLoaded, isLoaded) || isLoaded == 0) {
        return false;
    }
    return true;
}

// [CSF] 的条目展开（普通名 / 编号族 / 通配）—— 规则与 [Packs] 完全一致，
// 直接复用 PackList::Resolve。`missing` 里的名字（松散目录里没有）**也要试**：
// 它可能在我们刚注册进去的 MIX 包里。
std::vector<std::string> BuildCandidates() {
    PackList::Options opt;
    opt.gameDir = g_opt.gameDir;
    opt.subDir = g_opt.subDir;
    opt.rangeLo = g_opt.rangeLo;
    opt.rangeHi = g_opt.rangeHi;
    opt.caseSensitive = g_opt.caseSensitive;
    opt.userExcludes = g_opt.excludes;

    const auto entries = PackList::Resolve(g_opt.files, opt);
    std::vector<std::string> out;
    std::map<std::string, bool> seen;
    const auto push = [&](const std::string& n) {
        if (!n.empty() && !seen[n]) {
            seen[n] = true;
            out.push_back(n);
        }
    };
    for (const auto& e : entries) {
        for (const auto& n : e.names)   push(n);
        for (const auto& n : e.missing) push(n);
    }
    return out;
}

struct Action {
    int                      baseIndex;   // >=0 = 覆盖引擎已有那条；-1 = 追加
    std::string              name;
    std::vector<ParsedValue> values;
};

void SelfTest(const std::vector<Action>& actions) {
    using LoadStringFn = const wchar_t*(__fastcall*)(const char*, char*, const char*, int);
    auto ls = reinterpret_cast<LoadStringFn>(static_cast<uintptr_t>(sites::kStringTableLoadString));

    const auto report = [&](const char* label, const char* source) {
        const wchar_t* got = ls(label, nullptr, "RA2YRLWResourceReader.dll", 0);
        if (!got) {
            Log::Line("[互操作自检] %-24s %-10s 引擎返回空", label, source);
            return;
        }
        std::wstring w(got);
        const std::string txt = WideToUtf8(w.c_str(), w.size());
        Log::Line("[互操作自检] %-24s %-10s = \"%s\"%s", label, source, txt.c_str(),
                  txt.compare(0, 8, "MISSING:") == 0 ? "   ← 仍是 MISSING" : "");
    };

    // 我们这次真正写进去的（最多 3 条，避免日志刷屏）
    int shown = 0;
    for (const auto& a : actions) {
        if (shown >= 3) {
            break;
        }
        report(a.name.c_str(), a.baseIndex >= 0 ? "覆盖" : "新增");
        ++shown;
    }
    // 使用者点名要看的（SelfTest=）—— 用来确认"别人的表没被我们搞坏"（比如 Ares 的）
    int extra = 0;
    for (const auto& s : g_opt.selfTest) {
        if (extra >= 16) {
            break;
        }
        report(s.c_str(), "SelfTest");
        ++extra;
    }
}

bool DoMerge(const char* trigger) {
    void* oldLabels = nullptr;
    void* oldValues = nullptr;
    void* oldExtras = nullptr;
    int   oldLabelCount = 0;
    int   oldValueCount = 0;
    int   oldMaxLen = 0;
    void* oldLabelsProbe = nullptr;
    (void)oldLabelsProbe;

    if (!ReadGlobal(sites::kStLabels, oldLabels) || !oldLabels) {
        Log::Line("[CSF] 引擎的 Labels 数组为空，放弃（基表没载成功？）");
        return false;
    }
    ReadGlobal(sites::kStValues, oldValues);
    ReadGlobal(sites::kStExtraVals, oldExtras);
    ReadGlobal(sites::kStLabelCount, oldLabelCount);
    ReadGlobal(sites::kStValueCount, oldValueCount);
    ReadGlobal(sites::kStMaxLabelLen, oldMaxLen);
    if (oldLabelCount <= 0 || oldValueCount < 0) {
        Log::Line("[CSF] 计数异常（labels=%d values=%d），放弃", oldLabelCount, oldValueCount);
        return false;
    }
    Log::Line("[CSF] 引擎现状：labels=%d values=%d maxLabelLen=%d Labels=%p",
              oldLabelCount, oldValueCount, oldMaxLen, oldLabels);

    // ---- 1) 取候选文件并解析（全程不碰引擎内存）----
    std::vector<Action> actions;
    const auto candidates = BuildCandidates();
    Log::Line("[CSF] 候选文件 %u 个（触发者：%s）", static_cast<unsigned>(candidates.size()), trigger);

    std::vector<ParsedFile> files;
    unsigned skippedLongNames = 0;
    for (const auto& name : candidates) {
        std::vector<unsigned char> raw;
        std::string why;
        if (!ReadWholeViaEngine(name.c_str(), raw, why)) {
            Log::Line("[CSF] 跳过 %-24s %s", name.c_str(), why.c_str());
            continue;
        }
        ParsedFile pf;
        std::string err;
        if (!ParseCsf(raw, pf, err, skippedLongNames)) {
            Log::Line("[CSF] 解析失败 %-22s %s", name.c_str(), err.c_str());
            continue;
        }
        unsigned vals = 0;
        for (const auto& l : pf.labels) {
            vals += static_cast<unsigned>(l.values.size());
        }
        Log::Line("[CSF] 读入 %-24s %4u 条 label / %4u 条 string   version=%u language=%u   %u 字节",
                  name.c_str(), static_cast<unsigned>(pf.labels.size()), vals, pf.version,
                  pf.language, static_cast<unsigned>(raw.size()));
        pf.name = name;
        files.push_back(std::move(pf));
    }
    if (skippedLongNames) {
        Log::Line("[CSF] 有 %u 条 label 名字超过 31 字节，已忽略（引擎的名字缓冲只有 32 字节）",
                  skippedLongNames);
    }
    if (files.empty()) {
        Log::Line("[CSF] 没有任何文件可以合并");
        SelfTest(actions);   // 仍然按 SelfTest= 问一次引擎（用来确认既有表没被动过）
        return false;
    }

    // ---- 2) 决定每条 label 的去向（查重用引擎自己的 bsearch + 引擎比较器）----
    std::map<std::string, int> ours;
    unsigned skippedExisting = 0;
    unsigned overwritten = 0;
    unsigned ourDupes = 0;

    for (const auto& f : files) {
        for (const auto& lb : f.labels) {
            const auto it = ours.find(lb.name);
            if (it != ours.end()) {
                actions[it->second].values = lb.values;  // 我们自己文件里重名：后写的赢
                ++ourDupes;
                continue;
            }
            void* hit = EngineBsearch()(lb.name.c_str(), oldLabels,
                                        static_cast<size_t>(oldLabelCount), sites::kCsfLabelSize,
                                        EngineCompare());
            const int baseIndex =
                hit ? static_cast<int>((static_cast<char*>(hit) - static_cast<char*>(oldLabels)) /
                                       static_cast<int>(sites::kCsfLabelSize))
                    : -1;
            if (baseIndex >= 0 && !g_opt.overrideExisting) {
                if (skippedExisting < 5) {
                    Log::Line("[CSF] %s 引擎已存在 → 保留引擎的（Override=no）", lb.name.c_str());
                }
                ++skippedExisting;
                continue;
            }
            if (baseIndex >= 0) {
                ++overwritten;
            }
            ours[lb.name] = static_cast<int>(actions.size());
            Action a;
            a.baseIndex = baseIndex;
            a.name = lb.name;
            a.values = lb.values;
            actions.push_back(std::move(a));
        }
    }

    unsigned appendCount = 0;
    size_t   newValueCount = 0;
    for (const auto& a : actions) {
        if (a.baseIndex < 0) {
            ++appendCount;
        }
        newValueCount += a.values.size();
    }
    Log::Line("[CSF] 计划：追加 %u 条 label、覆盖 %u 条、跳过引擎已有 %u 条、覆盖自写重名 %u 次；"
              "新增值 %u 条",
              appendCount, overwritten, skippedExisting, ourDupes,
              static_cast<unsigned>(newValueCount));
    if (actions.empty()) {
        Log::Line("[CSF] 没有需要写入的 label（全部已存在且 Override=no）");
        SelfTest(actions);
        return false;
    }

    // ---- 3) 先全部申请好（任何一步失败就整体放弃，引擎保持原样）----
    const size_t labelCap = static_cast<size_t>(oldLabelCount) + appendCount + 64;
    const size_t valueCap = static_cast<size_t>(oldValueCount) + newValueCount + 64;

    auto* newLabels = static_cast<unsigned char*>(EngineNew()(labelCap * sites::kCsfLabelSize));
    auto* newValues = static_cast<void**>(EngineNew()(valueCap * sizeof(void*)));
    auto* newExtras = static_cast<void**>(EngineNew()(valueCap * sizeof(void*)));
    if (!newLabels || !newValues || !newExtras) {
        Log::Line("[CSF] 引擎 operator new 失败（labels=%u values=%u），放弃",
                  static_cast<unsigned>(labelCap), static_cast<unsigned>(valueCap));
        if (newLabels) EngineDelete()(newLabels);
        if (newValues) EngineDelete()(newValues);
        if (newExtras) EngineDelete()(newExtras);
        return false;
    }
    std::memset(newLabels, 0, labelCap * sites::kCsfLabelSize);
    std::memset(newValues, 0, valueCap * sizeof(void*));
    std::memset(newExtras, 0, valueCap * sizeof(void*));

    std::vector<void*> ownedStrings;   // 已分配的字符串；放弃时要自己收
    ownedStrings.reserve(newValueCount);

    std::memcpy(newLabels, oldLabels,
                static_cast<size_t>(oldLabelCount) * sites::kCsfLabelSize);
    if (oldValues && oldValueCount > 0) {
        std::memcpy(newValues, oldValues, static_cast<size_t>(oldValueCount) * sizeof(void*));
    }
    if (oldExtras && oldValueCount > 0) {
        std::memcpy(newExtras, oldExtras, static_cast<size_t>(oldValueCount) * sizeof(void*));
    }

    size_t valueIdx = static_cast<size_t>(oldValueCount);
    size_t labelIdx = static_cast<size_t>(oldLabelCount);
    int    maxNameLen = oldMaxLen;
    bool   oom = false;

    for (const auto& a : actions) {
        const size_t firstValue = valueIdx;
        for (const auto& v : a.values) {
            const size_t bytes = (v.text.size() + 1) * sizeof(wchar_t);
            auto* wbuf = static_cast<wchar_t*>(EngineNew()(bytes));
            if (!wbuf) { oom = true; break; }
            std::memcpy(wbuf, v.text.c_str(), bytes);
            newValues[valueIdx] = wbuf;
            ownedStrings.push_back(wbuf);

            if (!v.extra.empty()) {
                auto* xbuf = static_cast<char*>(EngineNew()(v.extra.size() + 1));
                if (!xbuf) { oom = true; break; }
                std::memcpy(xbuf, v.extra.c_str(), v.extra.size() + 1);
                newExtras[valueIdx] = xbuf;
                ownedStrings.push_back(xbuf);
            }
            ++valueIdx;
        }
        if (oom) {
            break;
        }

        unsigned char* entry = nullptr;
        if (a.baseIndex >= 0) {
            entry = newLabels + static_cast<size_t>(a.baseIndex) * sites::kCsfLabelSize;
        } else {
            entry = newLabels + labelIdx * sites::kCsfLabelSize;
            std::memcpy(entry, a.name.c_str(), a.name.size());
            entry[a.name.size()] = 0;
            ++labelIdx;
        }
        *reinterpret_cast<int*>(entry + 0x20) = static_cast<int>(a.values.size());
        *reinterpret_cast<int*>(entry + 0x24) = static_cast<int>(firstValue);
        if (static_cast<int>(a.name.size()) > maxNameLen) {
            maxNameLen = static_cast<int>(a.name.size());
        }
    }

    if (oom) {
        Log::Line("[CSF] 分配字符串时内存不足，整体放弃（引擎保持原样）");
        for (void* p : ownedStrings) {
            EngineDelete()(p);
        }
        EngineDelete()(newLabels);
        EngineDelete()(newValues);
        EngineDelete()(newExtras);
        return false;
    }

    const int finalLabelCount = static_cast<int>(oldLabelCount) + static_cast<int>(appendCount);
    const int finalValueCount = static_cast<int>(valueIdx);

    // 先在新数组上排好序（发布之前），再一次性改引擎的全局指针 —— 查找依赖有序性
    EngineQsort()(newLabels, static_cast<size_t>(finalLabelCount), sites::kCsfLabelSize,
                  EngineCompare());

    // ---- 4) 发布 ----
    // 旧的三个数组**故意不释放**：可能有别处缓存了指针；泄漏几 MB 换"绝无悬垂"。
    if (!WriteGlobal(sites::kStLabels, newLabels) ||
        !WriteGlobal(sites::kStValues, newValues) ||
        !WriteGlobal(sites::kStExtraVals, newExtras) ||
        !WriteGlobal(sites::kStLabelCount, finalLabelCount) ||
        !WriteGlobal(sites::kStValueCount, finalValueCount)) {
        Log::Line("[CSF] 写引擎全局失败（VirtualProtect？），已放弃；引擎仍是旧表（未改动）");
        return false;
    }
    const int cappedMax = (maxNameLen > 0 && maxNameLen < static_cast<int>(sites::kCsfLabelNameMax))
                              ? maxNameLen
                              : static_cast<int>(sites::kCsfLabelNameMax - 1);
    WriteGlobal(sites::kStMaxLabelLen, cappedMax);

    Log::Line("[CSF] 合并完成：labels %d → %d，values %d → %d（数组容量 %u / %u，maxLabelLen=%d）",
              oldLabelCount, finalLabelCount, oldValueCount, finalValueCount,
              static_cast<unsigned>(labelCap), static_cast<unsigned>(valueCap), cappedMax);

    // ---- 5) 互操作自检：问引擎自己要一次字符串（走的就是游戏真实的查找路径）----
    SelfTest(actions);
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------
void SetOptions(Options opt) {
    g_opt = std::move(opt);
    g_hasWork = !g_opt.files.empty();
}

bool HasWork() { return g_hasWork; }

bool Done() { return g_state == 2; }

void LogPlan() {
    if (!g_hasWork) {
        Log::Line("CSF 清单  : [CSF] 段为空 —— 不加载任何字符串表");
        return;
    }
    Log::Line("CSF 清单  : [CSF]=%u 条  Override=%s",
              static_cast<unsigned>(g_opt.files.size()),
              g_opt.overrideExisting ? "yes（我们的覆盖引擎的）" : "no（引擎的优先，默认）");
    for (const auto& f : g_opt.files) {
        Log::Line("         %s", f.c_str());
    }
    if (!g_opt.selfTest.empty()) {
        std::string s;
        for (size_t i = 0; i < g_opt.selfTest.size(); ++i) {
            if (i) s += ", ";
            s += g_opt.selfTest[i];
        }
        Log::Line("SelfTest  : %s（合并后逐个问引擎要一次）", s.c_str());
    }

    // 展开规则与 [Packs] 一致；此刻（初始化期）MIX 包还没注册，
    // 所以"引擎文件系统里能不能找到"要等合并时才知道 —— 这里只看松散目录。
    PackList::Options opt;
    opt.gameDir = g_opt.gameDir;
    opt.subDir = g_opt.subDir;
    opt.rangeLo = g_opt.rangeLo;
    opt.rangeHi = g_opt.rangeHi;
    opt.caseSensitive = g_opt.caseSensitive;
    opt.userExcludes = g_opt.excludes;

    const auto entries = PackList::Resolve(g_opt.files, opt);
    for (const auto& e : entries) {
        Log::Line("  [%s] %s", e.kind.c_str(), e.entry.c_str());
        for (const auto& n : e.names) {
            Log::Line("         松散目录存在: %s", n.c_str());
        }
        for (const auto& n : e.missing) {
            Log::Line("         松散目录没有: %s（合并时再去 MIX 包里找一次）", n.c_str());
        }
        for (const auto& n : e.excluded) {
            Log::Line("         被排除表丢弃: %s", n.c_str());
        }
        for (const auto& n : e.duplicates) {
            Log::Line("         重复丢弃    : %s", n.c_str());
        }
    }
}

bool TryMerge(const char* trigger) {
    if (g_state == 2) {
        return false;
    }
    if (!g_hasWork) {
        return false;
    }
    if (!MixRegister::PacksReady()) {
        if (!g_notified) {
            g_notified = true;
            Log::Line("[CSF] 等 [Packs] 那一步出结果（触发者：%s）", trigger);
        }
        return false;
    }
    if (!BaseTableLoaded()) {
        if (!g_notified) {
            g_notified = true;
            Log::Line("[CSF] 等引擎基表 CSF 加载完成（触发者：%s）", trigger);
        }
        return false;
    }
    if (InterlockedCompareExchange(&g_state, 1, 0) != 0) {
        return false;  // 别人正在做 / 已经做完
    }

    Log::Line("-------- [CSF] 开始合并（触发者：%s）--------", trigger);
    g_mergeOk = DoMerge(trigger);
    Log::Line("-------- [CSF] 合并结束：%s --------", g_mergeOk ? "成功" : "未做任何改动");
    InterlockedExchange(&g_state, 2);
    return g_mergeOk;
}

}  // namespace CsfMerge
