#!/usr/bin/env bash
# ============================================================================
# msvc_env.sh —— 在 Linux 上用 Wine 跑**真实 MSVC**，产出 32 位 Windows 目标
#
# 被 scripts/build.sh source。
#
# 为什么不用 mingw / clang-cl：产物要和 gamemd.exe 的调用约定、MSVC 的名称修饰、
# 以及 Syringe 的 hook 机制严格对齐，**真实 MSVC** 是唯一被验证过的工具链。
#
# ⚠️ 下面的路径常量按**作者本机**写的（Windows 分区挂在 /mnt/Windows-SSD，
#    Wine prefix 在工作区内）。换机器改这几处即可，脚本本身与项目无关：
#    PBS_WINDOWS_ROOT / PBS_MSVC_VER / PBS_SDK_VER / PBS_WINEPREFIX。
#
# 本机事实（踩过的坑，换机器前先核对）：
#   * Wine prefix 必须在**可写的 Linux 原生 FS** 上（NTFS/FUSE/只读 → cl.exe 报 D8037）。
#   * 临时目录可能被清空 → 日志与中间文件只写仓库自己的工作目录。
#   * `$HOME`、`/` 可能只读 → 不依赖 `~/.bashrc`，一切默认值自足。
#   * link.exe 的宿主是 x64，**默认 /MACHINE:X64** → 必须显式 /MACHINE:X86。
#   * 同一台机器上多个工程共用同一个 Wine prefix → **禁止并发构建**。
#
# 提供：环境变量 + w() / wlist() / cc1() / link_it() / show_log() / fail()
# 调用方需先定义：CL_FLAGS（数组）、MSVC_OBJ、MSVC_LOG、MSVC_SCRATCH
# ============================================================================

set -euo pipefail

# ---------------------------------------------------------------------------
# 路径常量（默认值即本机实测值；可用环境变量覆盖）
# ---------------------------------------------------------------------------
: "${PBS_WINDOWS_ROOT:=/mnt/Windows-SSD}"
: "${PBS_VS_VER:=18}"
: "${PBS_MSVC_VER:=14.44.35207}"
: "${PBS_SDK_VER:=10.0.26100.0}"
: "${PBS_WINEPREFIX:=/home/laowang/pbswork/.wine-pbsnew}"
: "${PBS_JOBS:=$(nproc 2>/dev/null || echo 4)}"

export PBS_VS_ROOT="$PBS_WINDOWS_ROOT/Program Files/Microsoft Visual Studio/$PBS_VS_VER/Community"
export PBS_MSVC="$PBS_VS_ROOT/VC/Tools/MSVC/$PBS_MSVC_VER"
export PBS_SDK="$PBS_WINDOWS_ROOT/Program Files (x86)/Windows Kits/10"
export PBS_BIN="$PBS_MSVC/bin/HostX64/x86"          # 64 位宿主、x86 目标
export PBS_RC="$PBS_SDK/bin/$PBS_SDK_VER/x86/rc.exe" # 真 32 位 rc.exe
export PBS_WINEPREFIX PBS_JOBS

# ---------------------------------------------------------------------------
# 前置检查 —— 失败要在这里立刻退出，别等 cl.exe 抛 D8037
# ---------------------------------------------------------------------------
fail() { echo "错误: $*" >&2; exit 1; }

[[ -f "$PBS_BIN/cl.exe" ]]   || fail "找不到 cl.exe: $PBS_BIN/cl.exe"
[[ -f "$PBS_BIN/link.exe" ]] || fail "找不到 link.exe: $PBS_BIN/link.exe"
[[ -f "$PBS_RC" ]]           || fail "找不到 rc.exe: $PBS_RC"
[[ -d "$PBS_WINEPREFIX/drive_c/windows" ]] \
  || fail "Wine prefix 未初始化: $PBS_WINEPREFIX"
command -v wine >/dev/null || fail "未安装 wine"

case "$(stat -f -c %T "$PBS_WINEPREFIX" 2>/dev/null)" in
  fuseblk|ntfs|ntfs3|vfat|msdos|exfat|nfs|cifs|9p|fuse*)
    fail "Wine prefix 在非 Linux 原生文件系统上（会触发 D8037）: $PBS_WINEPREFIX" ;;
esac

# ---------------------------------------------------------------------------
# Wine 环境
# ---------------------------------------------------------------------------
export WINEPREFIX="$PBS_WINEPREFIX"
export WINEDEBUG="${WINEDEBUG:--all}"

# wineserver 的 socket：继承来的 XDG_RUNTIME_DIR（/run/user/1000）本机只读，
# 必须回退到 prefix 内，否则 wineserver 重启会把 registry 写坏。
if [[ -z "${XDG_RUNTIME_DIR:-}" || ! -w "${XDG_RUNTIME_DIR:-/nonexistent}" ]]; then
  export XDG_RUNTIME_DIR="$WINEPREFIX/.wine-runtime"
fi
mkdir -p "$XDG_RUNTIME_DIR" 2>/dev/null || true
chmod 700 "$XDG_RUNTIME_DIR" 2>/dev/null || true

# Wine 只认 LC_ALL（不认 LANG）；不设的话中文 MSVC 的报错会全变成 ???????
[[ -n "${LC_ALL:-}" ]] || export LC_ALL=zh_CN.UTF-8

# cl.exe 的中间文件（_CL_*.tmp）目录必须可写且持久
export TMP='C:\rlw-tmp' TEMP='C:\rlw-tmp'
mkdir -p "$WINEPREFIX/drive_c/rlw-tmp"

# ---------------------------------------------------------------------------
# 路径转换与 INCLUDE/LIB
#   不用 winepath：它会往 stderr 混 MESA 噪音，且本机纯 bash 转换已足够
#   （prefix 里 z: → /，所以任何 Linux 路径都有 Z:\ 映射）
# ---------------------------------------------------------------------------
w() { printf 'Z:%s' "${1//\//\\}"; }
wlist() {
  local out="" d
  for d in "$@"; do
    [[ -n "$out" ]] && out+=";"
    out+="$(w "$d")"
  done
  printf '%s' "$out"
}

export INCLUDE="$(wlist \
  "$PBS_MSVC/include" \
  "$PBS_SDK/Include/$PBS_SDK_VER/ucrt" \
  "$PBS_SDK/Include/$PBS_SDK_VER/shared" \
  "$PBS_SDK/Include/$PBS_SDK_VER/um")"

export LIB="$(wlist \
  "$PBS_SDK/Lib/$PBS_SDK_VER/um/x86" \
  "$PBS_SDK/Lib/$PBS_SDK_VER/ucrt/x86" \
  "$PBS_MSVC/lib/x86")"

# ---------------------------------------------------------------------------
# 日志：MSVC 是中文版（CP936），Wine 自己的输出是 ASCII
# ---------------------------------------------------------------------------
show_log() {
  local f="${1:-}"
  [[ -f "$f" ]] || return 0
  if iconv -f UTF-8 -t UTF-8 "$f" >/dev/null 2>&1; then
    cat "$f"
  else
    iconv -f GBK -t UTF-8 -c "$f" 2>/dev/null || cat "$f"
  fi | grep -av 'MESA' | tail -n 30 || true
}

# ---------------------------------------------------------------------------
# 编译一个编译单元：  cc1 <源文件> <obj 输出> [额外开关...]
#
# `/utf-8` 是本工程的**硬性要求**，不是可选项：源码里有中文注释，而本机 MSVC
# 默认按代码页 936(GBK) 解读源文件 —— 一个 UTF-8 汉字是 3 字节，按 GBK 解码会
# 吃掉后一个字节，把下一行首字符吞掉（实测：`BOOL WINAPI DllMain` 被读成
# `OOL WINAPI DllMain`，报 C4430 / C2146 / C1004）。
# 它同时把 execution charset 设为 UTF-8，于是日志里的中文字面量天然就是 UTF-8
# 字节 —— 正好符合"RA2YRLWResourceReader.log 用 UTF-8"的要求，不必再转码。
# ---------------------------------------------------------------------------
cc1() {
  local src="$1" obj="$2"; shift 2
  local log="$MSVC_LOG/$(basename "${src//\//__}").log"
  mkdir -p "$MSVC_OBJ" "$MSVC_LOG" "$MSVC_SCRATCH"
  ( cd "$MSVC_SCRATCH" && wine "$PBS_BIN/cl.exe" /utf-8 "${CL_FLAGS[@]}" "$@" \
      "/Fo$(w "$obj")" "$(w "$src")" ) >"$log" 2>&1 \
    || { show_log "$log" >&2; fail "编译失败: $src"; }
}

# ---------------------------------------------------------------------------
# 链接：  link_it <日志名> <link.exe 参数...>
# 每次链接前调用方负责删掉旧产物（中断的构建会留下损坏的 .lib → LNK1136）
# ---------------------------------------------------------------------------
link_it() {
  local name="$1"; shift
  local log="$MSVC_LOG/$name.log"
  mkdir -p "$MSVC_LOG" "$MSVC_SCRATCH"
  ( cd "$MSVC_SCRATCH" && wine "$PBS_BIN/link.exe" "$@" ) >"$log" 2>&1 \
    || { show_log "$log" >&2; fail "链接失败（见 $log）"; }
  if grep -qai 'warning' "$log" 2>/dev/null; then
    echo "[链接告警] $name:"
    show_log "$log"
  fi
}

# ---------------------------------------------------------------------------
# 由一份"只有导出名"的 .def 生成导入库（lib.exe /DEF:）
#
# 用途：link.exe 解析 `.def` 里的转发写法 `X=module.symbol` 时，**必须在输入库中
# 找到那个符号**，否则它退化成"按内部符号解析"→ LNK2001。实测 SDK 的 version.lib
# 只声明 8 个函数，而载体壳要转发系统 version.dll 的全部 16 个导出，缺的用自建库补。
#
# 注意：**不能**把转发形式（`X=mod.Y`）喂给 lib.exe —— 它只接受裸导出名，
#       转发是 link.exe 的事，两者是两个不同的 .def。
# ---------------------------------------------------------------------------
mkimplib() {  # mkimplib <导出名 .def> <输出 .lib>
  local def="$1"
  local out="$2"
  # 注意：必须分开写。写成 `local def="$1" out="$2" log="...$(basename "$out")"`
  # 会踩 bash 的坑 —— 命令的所有参数在 `local` 执行**之前**就展开完了，
  # 于是 `$out` 还没赋值就被引用，在 set -u 下报"未绑定的变量"（且不中断脚本）。
  local log="$MSVC_LOG/$(basename "$out").log"
  mkdir -p "$MSVC_LOG" "$MSVC_SCRATCH" "$(dirname "$out")"
  ( cd "$MSVC_SCRATCH" && wine "$PBS_BIN/lib.exe" /NOLOGO \
      "/DEF:$(w "$def")" /MACHINE:X86 "/OUT:$(w "$out")" ) >"$log" 2>&1 \
    || { show_log "$log" >&2; fail "生成导入库失败: $out"; }
}

# ---------------------------------------------------------------------------
# 32 位架构自检 —— 防止悄悄链成 x64
# ---------------------------------------------------------------------------
assert_pe32_i386() {
  local f="$1"
  [[ -f "$f" ]] || fail "产物不存在: $f"
  file "$f" | grep -q 'PE32 executable.*Intel i386' \
    || { file "$f"; fail "$(basename "$f") 不是 32 位 PE32/i386（漏了 /MACHINE:X86？）"; }
}
