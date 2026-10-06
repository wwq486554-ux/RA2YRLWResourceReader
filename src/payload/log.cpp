#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace Log {
namespace {

HANDLE g_file = INVALID_HANDLE_VALUE;
std::string g_path;

void RawWrite(const char* data, DWORD len) {
    if (g_file == INVALID_HANDLE_VALUE || len == 0) {
        return;
    }
    DWORD written = 0;
    WriteFile(g_file, data, len, &written, nullptr);
}

}  // namespace

void Open(const std::string& dir) {
    Close();
    if (dir.empty()) {
        return;
    }
    g_path = dir + "\\RA2YRLWResourceReader.log";

    // 每次启动重建：不追加历史，避免上一局的日志混进来误导排查
    g_file = CreateFileA(g_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file == INVALID_HANDLE_VALUE) {
        g_path.clear();
        return;
    }
    // UTF-8 BOM
    RawWrite("\xEF\xBB\xBF", 3);
    Line("RA2YRLWResourceReader.log 开始（每次启动重建）");
}

void Close() {
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
}

bool IsOpen() { return g_file != INVALID_HANDLE_VALUE; }

std::string Path() { return g_path; }

void Line(const char* fmt, ...) {
    if (g_file == INVALID_HANDLE_VALUE) {
        return;
    }

    char body[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);

    char line[2304];
    int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "[%02u:%02u:%02u.%03u] %s\r\n",
                        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, body);
    if (n > 0) {
        RawWrite(line, (DWORD)n);
    }
}

}  // namespace Log
