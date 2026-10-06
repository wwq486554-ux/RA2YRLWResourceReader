#pragma once

// 版本与构建标识。放在头文件里而不是编译开关，省掉 /D 的引号转义麻烦。
// 发版时改这里。
// 0.2.0-dev = 加了 Syringe hook 表（.syhks00，挂 Bootstrap 成功出口 0x53044A）：
//             可以只用 RA2YRLWResourceReader.dll + RA2YRLWResourceReader.ini，**不需要载体壳、不需要改启动项**。
// 0.3.0-dev = 加了 [CSF]：把任意文件名的 CSF 合并进引擎的字符串表
//             （挂 0x6BD88B，基表 CSF 加载之后；与 Ares 的插点 0x6BD886 相邻且共存）。
// 0.3.0     = **首个公开发布版**：项目由 LW-MixReader / MixReader 改名为
//             RA2YRLWResourceReader（INI 段名 [ResourceReader]），
//             MIX 追加 + 任意文件名 CSF + 双入口（Syringe 路 / 载体壳路）全部实机通过。
#define RA2YRLW_VERSION "0.3.0"
#define RA2YRLW_BUILD "release"
