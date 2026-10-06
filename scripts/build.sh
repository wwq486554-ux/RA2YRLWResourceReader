#!/usr/bin/env bash
# ============================================================================
# build.sh —— 构建 RA2YRLWResourceReader 的两个 32 位产物（+ 可选的本地宿主）
#
#   RA2YRLWResourceReader.dll   真身（导出 RA2YRLWResourceReader_Initialize / RA2YRLWResourceReader_Diagnose）
#   version.dll                 载体壳（17 个导出全部转发给系统 version.dll）
#   smoketest.exe               本地冒烟宿主（**可选、不发行**；src/harness/ 不在发行仓库里，缺了就跳过）
#
# 工具链：Wine + 真实 MSVC（封装在 scripts/msvc_env.sh；路径按作者本机写的，
#   换机器要改那里的 MSVC/SDK/prefix 三个路径，见 README「构建」一节）。
#   ⚠️ 同一台机器上别并发跑两次构建 —— 共用同一个 Wine prefix。
#
# 用法：./build.sh [--clean] [--debug|--release] [--probe-template '<期望串>']
#   --probe-template  可选：让运行期探针去核对引擎里那条包名模板串（0x82668C）。
#                     **默认不检查** —— 有些注入式扩展会在运行期改写它，"应该是什么"
#                     取决于玩家装了什么环境。默认的字符串只对原版成立。
#                     例：./build.sh --probe-template 'EXPANDMD%02d.MIX'
# 退出码：0 全部成功 / 1 任一阶段失败
# ============================================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(cd "$HERE/.." && pwd)"

# shellcheck source=./msvc_env.sh
source "$PROJ/scripts/msvc_env.sh"

BUILD="${BUILD_DIR:-$PROJ/build}"
CFG=release
CLEAN=0
PROBE_TEMPLATE=""
while (( $# )); do
  case "$1" in
    --clean)   CLEAN=1 ;;
    --debug)   CFG=debug ;;
    --release) CFG=release ;;
    --probe-template)
      [[ $# -ge 2 ]] || { echo "错误: --probe-template 需要一个期望串" >&2; exit 1; }
      PROBE_TEMPLATE="$2"; shift ;;
    -h|--help) sed -n '2,22p' "$0"; exit 0 ;;
    *) echo "错误: 未知参数 $1" >&2; exit 1 ;;
  esac
  shift
done

MSVC_OBJ="$BUILD/obj/$CFG"
MSVC_LOG="$BUILD/log/$CFG"
MSVC_SCRATCH="$BUILD/scratch"
if (( CLEAN )); then
  rm -rf "$BUILD"
fi
mkdir -p "$MSVC_OBJ" "$MSVC_LOG" "$MSVC_SCRATCH"

if [[ $CFG == release ]]; then
  OPT=( /O2 /Oy /DNDEBUG )
else
  OPT=( /Od /Oy- /D_DEBUG )
fi

CL_FLAGS=(
  /nologo /c /std:c++17
  /EHsc /GR                       # 异常模型 + RTTI；别用 /EHs-c-
  /W4 /WX- /diagnostics:column /FC
  /MT                             # 静态 CRT：拷进游戏目录不用带 vcruntime140.dll
  /GF /Gy /fp:precise /arch:SSE2
  /Z7                             # 调试信息进 obj，绕开 Wine 下脆弱的 mspdbsrv
  /D_WIN32_WINNT=0x0601 /DNTDDI_VERSION=0x06010000
  /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS
  /DUNICODE /D_UNICODE /DWIN32 /D_WINDOWS
  "${OPT[@]}"
)
# 可选：把期望的包名模板串编进去（不加开关 = 空串 = 运行期探针不检查它，见 engine_sites.h）
# 用"生成一个小头文件"的方式传字符串 —— 直接 /D"NAME=\"值\"" 在 Wine 下会被 cl 拒（D8038）。
if [[ -n "$PROBE_TEMPLATE" ]]; then
  printf '#define RA2YRLW_PROBE_MIX_TEMPLATE_NAME "%s"\n' "$PROBE_TEMPLATE" >"$BUILD/probe_template.h"
  CL_FLAGS+=( /I"$(w "$BUILD")" /DRA2YRLW_PROBE_TEMPLATE_HEADER )
  echo "    （已启用包名模板串探针，期望串 = $PROBE_TEMPLATE）"
fi
# 明确不抄 Phobos 的：/GS- /Gz /openmp /analyze- /external:W4 /experimental:module
# （那个工程要跟 gamemd 的调用约定对齐，我们不需要）

echo "=== [1/3] 真身 RA2YRLWResourceReader.dll ==="
PAYLOAD_SRCS=( dllmain.cpp config.cpp log.cpp exeverify.cpp packlist.cpp mixregister.cpp csfmerge.cpp syringe_hook.cpp )
for s in "${PAYLOAD_SRCS[@]}"; do
  [[ -f "$PROJ/src/payload/$s" ]] || fail "源文件缺失: src/payload/$s"
  cc1 "$PROJ/src/payload/$s" "$MSVC_OBJ/payload_${s%.cpp}.obj"
done

rm -f "$BUILD/RA2YRLWResourceReader.dll" "$BUILD/RA2YRLWResourceReader.pdb" "$BUILD/RA2YRLWResourceReader.lib" "$BUILD/RA2YRLWResourceReader.exp"
PAYLOAD_LINK=(
  /NOLOGO /DLL /MACHINE:X86 /INCREMENTAL:NO
  "/OUT:$(w "$BUILD/RA2YRLWResourceReader.dll")"
  "/PDB:$(w "$BUILD/RA2YRLWResourceReader.pdb")"
  "/IMPLIB:$(w "$BUILD/RA2YRLWResourceReader.lib")"
  "/DEF:$(w "$PROJ/src/payload/RA2YRLWResourceReader.def")"
  /SUBSYSTEM:WINDOWS /DEBUG:FULL /MANIFEST:NO
  /OPT:REF /OPT:ICF /DYNAMICBASE /NXCOMPAT
  "/BASE:0x60000000"        # 把首选基址拉离 0x400000，好让本地宿主把 gamemd 放上去
)
for s in "${PAYLOAD_SRCS[@]}"; do
  PAYLOAD_LINK+=( "$(w "$MSVC_OBJ/payload_${s%.cpp}.obj")" )
done
PAYLOAD_LINK+=( kernel32.lib advapi32.lib user32.lib )   # advapi32 = exeverify 的 CryptoAPI
link_it link_payload "${PAYLOAD_LINK[@]}"
assert_pe32_i386 "$BUILD/RA2YRLWResourceReader.dll"

# 真身自检：① 必须带 .syhks00 段（SyringeEx 全靠它识别 + 决定注入）；
#           ② 该段里必须**恰好**是两条期望条目：
#              {0x53044A, 5, "RA2YRLWResourceReader_AfterBootstrap"}
#              {0x6BD88B, 6, "RA2YRLWResourceReader_AfterBaseCsf"}
#           ③ 两个处理器都必须能从导出表按名解析到（SyringeEx 用 GetProcAddress）。
if command -v llvm-readobj >/dev/null 2>&1; then
  grep -aq '\.syhks00' "$BUILD/RA2YRLWResourceReader.dll" \
    || fail "RA2YRLWResourceReader.dll 里没有 .syhks00 段 —— SyringeEx 不会识别它"
  EXPORTS="$(llvm-readobj --coff-exports "$BUILD/RA2YRLWResourceReader.dll")"
  for fn in RA2YRLWResourceReader_AfterBootstrap RA2YRLWResourceReader_AfterBaseCsf; do
    grep -qE "^[[:space:]]+Name: ${fn}\$" <<<"$EXPORTS" \
      || fail "RA2YRLWResourceReader.dll 没有导出 ${fn}（Syringe hook 表靠名字解析）"
  done
  python3 - "$BUILD/RA2YRLWResourceReader.dll" <<'PY'
import struct, sys
data = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', data, 0x3C)[0]
nsec = struct.unpack_from('<H', data, pe+6)[0]
optsz = struct.unpack_from('<H', data, pe+20)[0]
image_base = struct.unpack_from('<I', data, pe+24+28)[0]
sect = pe + 24 + optsz
secs = []
for i in range(nsec):
    off = sect + 40*i
    nm = data[off:off+8].rstrip(b'\0').decode()
    vsize, vaddr, rawsize, rawptr = struct.unpack_from('<IIII', data, off+8)
    secs.append((nm, vaddr, vsize, rawptr, rawsize))
def rva2raw(rva):
    for nm, vaddr, vsize, rawptr, rawsize in secs:
        if vaddr <= rva < vaddr + max(vsize, rawsize):
            return rawptr + (rva - vaddr)
    return None
sec = next((s for s in secs if s[0] == '.syhks00'), None)
if not sec:
    print('  ✗ 映像里没有 .syhks00 段'); sys.exit(1)
_, vaddr, vsize, rawptr, rawsize = sec
print(f'  .syhks00 段: RVA=0x{vaddr:08X} vsize=0x{vsize:X} rawsize=0x{rawsize:X}')
WANT = [(0x53044A, 5, 'RA2YRLWResourceReader_AfterBootstrap'), (0x6BD88B, 6, 'RA2YRLWResourceReader_AfterBaseCsf')]
found = []
for i in range(0, rawsize, 16):
    addr, size, nameptr, _pad = struct.unpack_from('<IIII', data, rawptr+i)
    if not nameptr:
        continue
    raw_name = rva2raw(nameptr - image_base)
    s = data[raw_name:data.index(b'\0', raw_name)].decode(errors='replace')
    print(f'    entry@0x{addr:08X} size={size} name="{s}"')
    found.append((addr, size, s))
missing = [w for w in WANT if w not in found]
extra = [f for f in found if f not in WANT]
if missing:
    print(f'  ✗ .syhks00 缺少条目: {missing}'); sys.exit(1)
if extra:
    print(f'  ✗ .syhks00 有未预期的条目: {extra}'); sys.exit(1)
print('  ✓ Syringe hook 表条目正确（0x53044A size=5 / 0x6BD88B size=6）')
PY
else
  echo "    （llvm-readobj 不在，跳过 Syringe hook 表自检）"
fi

echo
echo "=== [2/3] 载体壳 version.dll ==="
# 17 个导出都是**真实函数**（裸跳板），运行时从系统目录解析真正的 version.dll。
# 不再用 .def 转发器 —— 那条路在 Proton 上会因 WOW64 重定向缺失而整体加载失败，
# 然后被 Wine 的 `native,builtin` 静默回退到内置版（症状：游戏照跑、日志全无）。
# 详见 src/carrier/dllmain.cpp 头部注释。
cc1 "$PROJ/src/carrier/thunks.cpp" "$MSVC_OBJ/carrier_thunks.obj"
cc1 "$PROJ/src/carrier/dllmain.cpp" "$MSVC_OBJ/carrier_dllmain.obj"

rm -f "$BUILD/version.dll" "$BUILD/version.pdb" "$BUILD/version_proxy.lib" "$BUILD/version.exp"
link_it link_carrier \
  /NOLOGO /DLL /MACHINE:X86 /INCREMENTAL:NO \
  "/OUT:$(w "$BUILD/version.dll")" \
  "/PDB:$(w "$BUILD/version.pdb")" \
  "/IMPLIB:$(w "$BUILD/version_proxy.lib")" \
  "/DEF:$(w "$PROJ/src/carrier/version.def")" \
  /SUBSYSTEM:WINDOWS /MANIFEST:NO /DEBUG:FULL \
  /OPT:REF /OPT:ICF /DYNAMICBASE /NXCOMPAT \
  "$(w "$MSVC_OBJ/carrier_thunks.obj")" "$(w "$MSVC_OBJ/carrier_dllmain.obj")" kernel32.lib
assert_pe32_i386 "$BUILD/version.dll"

# 导出表自检：必须 17 个真实导出（RVA），**不能**是转发器
if command -v llvm-readobj >/dev/null 2>&1; then
  EXP="$(llvm-readobj --coff-exports "$BUILD/version.dll")"
  NEXP="$(grep -cE '^\s+Name: ' <<<"$EXP" || true)"
  FWD="$(grep -c 'ForwardedTo' <<<"$EXP" || true)"
  echo "    导出表: 导出数=$NEXP  转发器=$FWD（期望 17 / 0）"
  if [[ "$NEXP" != "17" || "$FWD" != "0" ]]; then
    echo "$EXP" | grep -E 'Name:|ForwardedTo' | head -20 | sed 's/^/      /'
    fail "载体壳导出表不对：导出数=$NEXP 转发器=$FWD（应为 17 个真实导出）"
  fi
  # 名字必须是**不带下划线**的（.def 里的裸名字要能匹配上 cdecl 的 _Name）
  # 注意 `|| true`：grep 无匹配时返回 1，配合 pipefail 会把整个脚本干掉（踩过）
  BAD="$(grep -cE '^[[:space:]]+Name: _' <<<"$EXP" || true)"
  [[ "$BAD" == "0" ]] || fail "有 $BAD 个导出名带前导下划线（.def 没匹配上 cdecl 修饰名）"
else
  echo "    （llvm-readobj 不在，跳过导出表自检）"
fi

echo
echo "=== [3/3] 本地冒烟宿主 smoketest.exe（可选）==="
# 发行仓库里**只有产品源码**：src/harness/ 是作者本机的验证宿主，不随仓库发布。
# 它在就编，不在就跳过 —— 保证"从干净 clone 构建"照样成功。
if [[ -f "$PROJ/src/harness/smoketest.cpp" ]]; then
  cc1 "$PROJ/src/harness/smoketest.cpp" "$MSVC_OBJ/harness_smoketest.obj"

  rm -f "$BUILD/smoketest.exe" "$BUILD/smoketest.pdb"
  link_it link_harness \
    /NOLOGO /MACHINE:X86 /INCREMENTAL:NO /SUBSYSTEM:CONSOLE \
    "/OUT:$(w "$BUILD/smoketest.exe")" \
    "/PDB:$(w "$BUILD/smoketest.pdb")" \
    /DEBUG:FULL /MANIFEST:NO /DYNAMICBASE:NO /FIXED "/BASE:0x10000000" \
    "$(w "$MSVC_OBJ/harness_smoketest.obj")" kernel32.lib user32.lib
  assert_pe32_i386 "$BUILD/smoketest.exe"
  # 宿主固定在高位基址，免得它自己占掉低地址（避免将来做活体探针时添乱）
else
  echo "  （没有 src/harness/smoketest.cpp —— 跳过；发行仓库本来就不含它）"
fi

echo
echo "全部成功 -> $BUILD/{RA2YRLWResourceReader.dll,version.dll}"
