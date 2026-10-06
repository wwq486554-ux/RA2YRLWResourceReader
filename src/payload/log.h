#pragma once
#include <string>

// ============================================================================
// RA2YRLWResourceReader.log —— 排错全靠它
//
// 约定：
//   * 位置：游戏根目录（与 RA2YRLWResourceReader.ini 同级），**不依赖当前工作目录**。
//   * 编码：UTF-8 **带 BOM**。编译时 `cl /utf-8` 让中文字面量就是 UTF-8 字节，
//     所以直接原样写盘即可，不需要转码。BOM 是为了让记事本/VSCode 一眼认出编码。
//   * 每次启动**清空重写**（每次运行一份干净的日志，比翻历史更有用）。
//   * 日志写不进去（目录只读等）时全部退化为空操作 —— 绝不能因为日志失败影响游戏。
// ============================================================================

namespace Log {

// 在 `dir`（游戏目录，结尾不要反斜杠）下打开/重建 RA2YRLWResourceReader.log
void Open(const std::string& dir);
void Close();
bool IsOpen();
std::string Path();

// printf 风格，自动加时间戳与换行，并立即刷盘（崩溃后还能看到最后一行）
void Line(const char* fmt, ...);

}  // namespace Log
