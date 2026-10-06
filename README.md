# RA2YRLWResourceReader

让 YR 引擎在读完自带的 `expandmd*` 等 MIX 包之后，**再按一份外部 INI
追加加载任意名字的 MIX 包**；并且能把**任意文件名的 CSF 字符串表**合并进游戏的文本表
（原版只会读自己那一个 `ra2md.csf`）。

- 目标游戏：`gamemd.exe`（尤里的复仇）
- 目标架构：**Win32 / x86 / 32 位**（`gamemd.exe` 是 32 位进程，x64 DLL 加载不了）
- 依赖：**无**。不需要 Syringe，不需要 Ares，不需要 Phobos（与 Ares 同场也不冲突）

## 产物

| 文件 | 角色 |
|---|---|
| `RA2YRLWResourceReader.dll` | **真身**。全部逻辑：读 INI、校验 exe、装 detour、追加注册 MIX、合并 CSF、写日志。自带 `.syhks00` hook 表（两条）⇒ **能被 SyringeEx 直接注入**（见下节）。 |
| `RA2YRLWResourceReader.ini` | 配置（`[ResourceReader]` / `[Packs]` / `[Packs.Exclude]` / `[CSF]`）。与 DLL 一起放游戏根目录。 |
| `version.dll` | **载体壳**（可选，给没有 SyringeEx 的环境）。借一个游戏本来就导入的系统 DLL 名；17 个导出都是**真实跳板**（不是 `.def` 转发器），运行时按**全路径**从系统目录加载真正的 version.dll 并解析函数；另外加载真身、调它的 `RA2YRLWResourceReader_Initialize`。 |
| `RA2YRLWResourceReader.log` | 运行日志（真身写）。排错全靠它。 |
| `RA2YRLWResourceReader.carrier.log` | 载体壳自己的日志。**真身没被加载起来时，只有这份能说明问题。** |

## 两种装法（推荐第一种）

| 装法 | 要放什么 | 前提 | 启动项 |
|---|---|---|---|
| **① Syringe 路（推荐）** | `RA2YRLWResourceReader.dll` + `RA2YRLWResourceReader.ini` | 游戏目录里有 `SyringeEx.exe`（不少整合包会自带这个注入器） | **不用改** |
| ② 载体壳路 | 上面两个 + `version.dll` | 没有注入器 | Proton 下要加 `version=n,b`（见下节） |

两条路共用同一份真身、同一个工作体，**同时装上也不会重复注册**（内部一次性闸门）。
判据：`RA2YRLWResourceReader.log` 里那行 `Bootstrap 已成功返回（触发者：…）` ——
`Syringe hook 表（.syhks00）` = 走了第①条，`载体路：Bootstrap 入口 detour（0x5301A0）` = 走了第②条。

## ⚠️（仅载体壳路）Proton / Steam 用户必须改一行启动项

Proton 的 Wine 对系统 DLL **默认走内置版**（正因如此它的脚本才要特意声明
`ddraw=n,b`、`winmm=n,b`、`dinput=n,b`）。`version` 不在那份名单里，
所以放在游戏目录的载体壳**会被忽略**，且**没有任何报错**（Wine 的 `native,builtin`
顺序会静默回退内置版，游戏照常运行）。

**用第①条（Syringe 路）就完全不用碰启动项。** 如果确实要用载体壳，
Steam 启动项（`%command%` 之前）必须让 version 走 native：

```
PROTON_USE_WINE3D=1 WINEDLLOVERRIDES="version=n,b;wsock32,ddraw=n,b" %command%
```

判据：游戏目录里出现 `RA2YRLWResourceReader.carrier.log`。

## 为什么现在不需要载体壳了

`gamemd.exe` 的导入表里**没有** Ares / Phobos / 任何 mod DLL，它们全都是被注入器
（SyringeEx）注入的。而 SyringeEx 认 DLL 的**唯一**条件是它带 `.syhks00`（Syringe hook 表）
—— 真身现在自带这个段（挂 Bootstrap 成功出口 `0x53044A`，处理器按名字导出），
所以 SyringeEx 会像加载 Ares/Phobos 一样直接把它 LoadLibrary 进来。
`.syexe00`、`SyringeHandshake`、`.json`、`Syringe.json` 统统**不需要**。

载体壳只留给"游戏目录里没有 SyringeEx"的少数环境。

## 目录

```
RA2YRLWResourceReader/
├─ src/
│  ├─ payload/          真身 RA2YRLWResourceReader.dll
│  │   ├─ engine_sites.h    ★ 单一事实来源：地址与期望字节
│  │   ├─ dllmain.cpp       入口、总流程、两个导出
│  │   ├─ config.cpp/.h     RA2YRLWResourceReader.ini 解析（[ResourceReader] / [Packs] / [CSF]）
│  │   ├─ log.cpp/.h        RA2YRLWResourceReader.log
│  │   ├─ exeverify.cpp/.h  目标 exe 校验（size + md5）
│  │   ├─ packlist.cpp/.h   包名/文件名解析（普通名 / 编号族 / 通配）
│  │   ├─ syringe_hook.cpp   ★ .syhks00 hook 表 + 导出处理器（Syringe 路的入口，两条）
│  │   ├─ mixregister.cpp/.h 探针 + 两处 detour + 追加进 MixFileClass::Array
│  │   └─ csfmerge.cpp/.h   ★ [CSF]：解析 CSF 并合并进引擎的字符串表
│  └─ carrier/          载体壳 version.dll（dllmain.cpp + version.def）
├─ ini/RA2YRLWResourceReader.ini    发行用配置模板
├─ scripts/
│  ├─ msvc_env.sh       MSVC/Wine 工具链封装（按作者本机路径写的，换机器要改）
│  └─ build.sh          构建
├─ LICENSE              GPL-3.0
└─ README.md
```

> 作者本机还留着一批**不随仓库发布**的开发/验证设施：离线自检工具（`tools/check_packs.py`、
> `tools/check_sites.py`、`tools/winreg_known_dlls.py`）、Syringe hook 探针（`tools/probe_hook/`）、
> 冒烟宿主（`src/harness/` + `scripts/smoketest.sh`）。`build.sh` 检测到宿主不存在会自动跳过那一步。

## 构建

产物是 **Win32 / x86 / 32 位**（`gamemd.exe` 是 32 位进程，x64 DLL 加载不了），
用**真实 MSVC**（作者本机是 MSVC 14.44 + Windows SDK 10 跑在 Wine 里）编译：

```bash
scripts/build.sh              # 增量；产物在 build/
scripts/build.sh --clean      # 全量重编
scripts/build.sh --debug      # 调试配置
scripts/build.sh --probe-template 'EXPANDMD%02d.MIX'   # 可选：核对包名模板串（见下）
```

- **包名模板串探针默认关闭**：引擎里那条 `EXPANDMD%02d.MIX`（0x82668C）可能被注入式扩展
  在运行期改写成它自己的模板，所以"应该是什么"取决于玩家装了什么环境 —— 本工具不替你假设。
  想核对就按上面那样在编译时给出期望串（不改源码即可）；不给就显示"已跳过"，
  对挂钩没有任何影响。

- 产物：`build/RA2YRLWResourceReader.dll`（真身）、`build/version.dll`（载体壳）。
- **换机器**：改 `scripts/msvc_env.sh` 顶部的 MSVC `bin/HostX64/x86`、Windows SDK、
  Wine prefix 三个路径即可；脚本本身与项目无关。
- `build.sh` 末尾会自检 `.syhks00` hook 表（**恰好**两条条目 + 两个处理器导出），
  不合格直接构建失败 —— 这是防止"编译过了但注入不进来"的那道闸门。
- ⚠️ 同一台机器上不要并发跑两次构建（共用一个 Wine prefix）。
- 二进制**不进版本库**：每次发布把 `build/RA2YRLWResourceReader.dll` 挂到 Release 附件里。

## `[Packs]`：追加任意名字的 MIX 包

`RA2YRLWResourceReader.ini` 的 `[Packs]` 段一行一个，三种写法（普通名 / 编号族 `%02d` / 通配 `*`），
按**书写顺序**追加到 `MixFileClass::Array` **尾部**（不抢引擎自带包的优先级）。
完整键值与坑见模板 `ini/RA2YRLWResourceReader.ini` 的注释。

## `[CSF]`：合并任意文件名的字符串表

原版只会读自己那一个 `ra2md.csf`，其它命名一律不认（`stringtable%02d.csf` 这类是别人在插件里做的）。
`[CSF]` 段让你在引擎的字符串表**加载完成之后**，把任意命名的 CSF 合并进去：

```ini
[CSF]
Override=no
SelfTest=MY_LABEL,MSG:PingInfo
0=lwtext.csf
1=lwextra%02d.csf
2=lw_*.csf
```

- **展开规则与 `[Packs]` 完全一样**（普通名 / 编号族 / 通配，受 `NumberRange` 与 `[Packs.Exclude]` 约束）。
- 文件走**引擎自己的文件系统**去读 ⇒ **可以放在本 DLL 注册的 MIX 包里**（含 LMD 压缩条目），
  不一定要散在游戏目录；只有**通配**展开时看不见包内文件（那一步是目录扫描）。
- `Override` 默认 **no**：引擎自带的同名 label 优先，我们**只补引擎没有的**（不会动原版文本）。
  写 `yes` 则同名 label 用我们的值（后写的文件覆盖先写的）。
- 找不到 / 格式坏的文件只写日志，**不阻断启动**，也不会改动引擎内存。
- **排错**：日志里搜 `[CSF]`。最关键的一行是合并后的**互操作自检** ——
  它用引擎自己的 `StringTable::LoadString` 把 label 再要一遍：

  ```
  [互操作自检] LWTESTOK                 新增     = "LW-CSF-LOOSE-OK"
  [互操作自检] MSG:PingInfo             SelfTest   = "Ping = %dms"
  ```

  `SelfTest=` 里点名的 label 也会被这样查一次 —— 用来确认**别人的表没被我们搞坏**
  （比如 Ares 的 `stringtable%02d.csf` 里的 label 仍然在）。

### 它怎么工作（给要改这份代码的人）

| 步骤 | 做法 |
|---|---|
| 时机 | 挂 **`0x6BD88B`**（基表 CSF 加载之后、正式初始化之前的 6 字节整指令）。Ares 占的是它前面 5 字节的 `0x6BD886`（引擎预留的空桩 `call`），Ares 的处理器**跳**到 `0x6BD88B`，所以我们挂这里与 Ares **天然共存**。 |
| 判据 | 合并要「基表已在内存」+「`[Packs]` 那步已有结果」⇒ 两个触发点（`0x6BD88B` 与 `0x53044A`）谁后到谁干，不依赖两者先后。 |
| 读文件 | 引擎的 `CCFileClass`（`0x4739F0/0x473C50/0x473D10/0x473B10`）—— 就是 `StringTable::LoadFile` 用的那套。 |
| 解析 | 按 CSF 磁盘格式自己读（24 字节头 + `" LBL"` + `" RTS"/"WRTS"`；文本是**逐字节取反**的 UTF-16LE），并复刻引擎的空白归一化。 |
| 查重 | 引擎自己的 `bsearch` + 引擎比较器 `0x7C8D20`（语义与游戏查找 100% 一致）。 |
| 写入 | 换更大的 `Labels`/`Values`/`ExtraValues` 三数组（引擎 `operator new`），拷旧数据、追加新条目、更新计数，再用引擎自己的 `qsort` **重排**（查找是二分，顺序是硬要求）。旧的三个数组故意不释放，避免任何悬垂引用。 |

**已知限制**

- **语言切换**：引擎切语言时会 `StringTable::Unload` 清空整个表再重载 —— 我们追加的 label
  会一起被清掉（Ares 的额外表同样如此）。切一次语言后重新启动游戏即可。
- **载体壳路**（`version.dll`）在同一台机器上的端到端验证仍未完成（假游戏目录跑不到
  Bootstrap 之后的阶段）；Smoke 能验的是"探针全过 + 两处 patch 在活进程里装上了 + 蹦床字节正确"。
  推荐用 Syringe 路（已验证端到端）。



## Syringe hook 表（`.syhks00`）—— 第①条装法靠的就是它

SyringeEx 认 DLL 的唯一条件是 `.syhks00` 段；认到就 `Recognized DLL` + `LoadLibrary` +
按条目里的**名字**从导出表 `GetProcAddress` 取处理器。所以：

- 处理器必须是**导出函数**：`extern "C" __declspec(dllexport) DWORD __cdecl f(REGISTERS*)`；
- 表结构必须严格符合 Syringe 的 ABI（`pack(push,16)` + `align(16)`，每项 16 字节）；
- 变量没人引用，`/OPT:REF` 会删段 ⇒ 必须 `/include:_<符号>` 强制保留
  （`extern "C"` 变量在 x86 上符号是 `_名字`）；
- **不需要** `.syexe00`、`SyringeHandshake`、`.json`、`Syringe.json`、启动器或启动项改动。

真身现在带**两条**条目，都是**整指令边界**上的覆盖：

| 挂点 | size | 处理器 | 用途 |
|---|---|---|---|
| `0x53044A` | 5 | `RA2YRLWResourceReader_AfterBootstrap` | `[Packs]`：Bootstrap 唯一成功出口，追加注册 MIX |
| `0x6BD88B` | **6** | `RA2YRLWResourceReader_AfterBaseCsf` | `[CSF]`：基表字符串表加载之后合并我们的 CSF |

⚠️ 两条挂点的选择理由（都是实测/反汇编结论，不能随手改）：

- **`0x53044A` 必须是 Bootstrap 的唯一成功出口**（`mov al,1; pop edi; pop esi; pop ebp`），
  **不能**挂在调用点 `0x52BB69` / 成功续点 `0x52BB95` —— Ares 的 `Expand_MIX_Deorg` 接管
  `0x52BB64` 后控制流既不经过前者也不经过后者（断点确实装上了，首字节 `E9`）。
- **`0x6BD88B` 的 size 必须是 6**（`84 C0 75 15 33 C0` = `test al,al` + `jne` + `xor eax,eax`）；
  写 5 会停在 `xor` 的中间。它前面的 `0x6BD886` 是 Ares 的插点，Ares 的处理器末尾**跳到**
  `0x6BD88B`，所以我们的钩子在 Ares 在场时照样执行。

`scripts/build.sh` 会强制自检这一段：**恰好**两条期望条目（地址 / size / 名字全对）+ 两个
处理器都能从导出表解析到，不合格直接构建失败。

## 部署

- **推荐（Syringe 路：游戏目录里已经有 `SyringeEx.exe` 的环境）**：
  把 `RA2YRLWResourceReader.dll` + `RA2YRLWResourceReader.ini` 两个文件复制到**游戏根目录**
  （与 `gamemd.exe` 同级），其他什么都不用动 —— **不用改启动项、不用载体壳**。
- **载体壳路（游戏目录里没有 SyringeEx 时）**：再多放一个 `version.dll`，
  并按上面那节给 Proton / Steam 加启动项。注意 `version.dll` 借的是系统 DLL 的名字，
  同一目录只能有一个，别和别的 cnc-ddraw 类补丁的 `version.dll` 混用。
- ⚠️ **如果之前装过别的版本**（例如旧名 `MixReader.dll`），先把旧文件删干净：
  SyringeEx 是按 `.syhks00` 段识别的，**两个真身同时存在会双双加载**，导致包注册两次、
  字符串表合并两次。
- 排错看游戏根目录的 `RA2YRLWResourceReader.log`（每次启动重建）；载体壳路另有一份
  `RA2YRLWResourceReader.carrier.log`（真身没被加载起来时，只有这份能说明问题）。

## 许可

本项目以 **GPL-3.0** 发布，全文见 [LICENSE](LICENSE)。

- 这是**非官方**的玩家自制工具：与 Westwood / EA 无关，也不属于 Phobos 官方项目；
  不发售、**不附带任何游戏素材**。
- 只对 `gamemd.exe`（尤里的复仇 1.001，Build B）的内存做运行时挂钩；不修改游戏文件、
  不修改任何 MIX / CSF 内容。
- 地址表（`src/payload/engine_sites.h`）只对 **Build B** 的 `gamemd.exe`
  （4,816,864 字节，md5 `56d582a1d6f3c144d3adc867d7a4d91b`）成立；换 exe 会让探针不通过，
  插件会**拒绝挂钩**而不是改错地方（`RA2YRLWResourceReader.log` 里能看见是哪一项不符）。
