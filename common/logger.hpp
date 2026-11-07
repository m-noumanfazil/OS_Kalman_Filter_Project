// logger.hpp - header-only system call logger (Windows only)
//
// IMPORTANT: never call the logger inside a timing loop. Printing to the
// console and writing to a file take far longer than most system calls and
// would completely ruin the measurements. Log only in setup/verification code.

#ifndef LOGGER_HPP
#define LOGGER_HPP

#ifndef NOMINMAX
#define NOMINMAX            // keeps std::min / std::max working
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN // smaller windows.h, fewer name clashes
#endif
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

namespace logger {

namespace detail {

// Holds the log path and whether init() has already run.
// A function-local static avoids needing a global variable defined in a .cpp.
struct State {
    std::string path = "logs/syscalls_windows.log";
    bool ready = false;
};

inline State& state() {
    static State s;
    return s;
}

// Creates every folder in the path of a file, e.g. "logs/x.log" creates "logs".
// Errors are ignored on purpose: "already exists" is the normal case.
inline void createParentFolders(const std::string& filePath) {
    for (size_t i = 0; i < filePath.size(); ++i) {
        if (filePath[i] == '/' || filePath[i] == '\\') {
            std::string folder = filePath.substr(0, i);
            if (!folder.empty() && folder.back() != ':') {   // skip "C:"
                CreateDirectoryA(folder.c_str(), nullptr);
            }
        }
    }
}

// Current local time as HH:MM:SS.mmm
inline std::string timestampNow() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d.%03d",
                  t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    return buffer;
}

// Turns a Windows error code into text and removes the trailing "\r\n".
inline std::string errorText(DWORD code) {
    char buffer[512];
    DWORD length = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        buffer, sizeof(buffer), nullptr);
    if (length == 0) {
        return "Unknown error";
    }
    std::string text(buffer, length);
    while (!text.empty() &&
           (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) {
        text.pop_back();
    }
    return text;
}

} // namespace detail

// Sets the log file path and creates its folder. Calling it is optional:
// if you never call it, the first log call uses the default path.
inline void init(const std::string& logFilePath = "logs/syscalls_windows.log") {
    detail::state().path = logFilePath;
    detail::createParentFolders(logFilePath);
    detail::state().ready = true;
}

// Records the result of one system call.
// callName: e.g. "CreateFile"   success: did the call work?   detail: e.g. a file name
inline void logSyscall(const char* callName, bool success, const char* detail = "") {
    // FIRST statement: read the error code before anything else can overwrite it.
    // (detail is a plain const char*, so no string is built before this line.)
    DWORD errorCode = GetLastError();

    detail::State& st = detail::state();
    if (!st.ready) {
        std::string path = st.path;
        init(path);
    }

    std::string line = "[" + detail::timestampNow() + "] ";
    line += callName;
    line += "(";
    if (detail != nullptr && detail[0] != '\0') {
        line += "\"";
        line += detail;
        line += "\"";
    }
    line += ") -> ";

    if (success) {
        line += "OK";
    } else {
        line += "FAILED, error " + std::to_string(errorCode) + ": " +
                detail::errorText(errorCode);
    }

    // Console
    std::cout << line << std::endl;   // endl flushes

    // File: opened in append mode for every line, written in one piece and
    // flushed, so several processes can share the same log file.
    std::ofstream file(st.path, std::ios::app);
    if (file) {
        file << (line + "\n");
        file.flush();
    } else {
        std::cerr << "logger: cannot open log file " << st.path << std::endl;
    }

    SetLastError(errorCode);   // give the caller back the error it had
}

} // namespace logger

// Short call-site form:  LOG_SYSCALL("CreateFile", h != INVALID_HANDLE_VALUE, "input.txt");
// Pass "" as detail if you have none. If your success expression itself calls
// Windows functions, compute it into a bool right after the system call first.
#define LOG_SYSCALL(callName, success, detail) \
    logger::logSyscall((callName), (success), (detail))

#endif // LOGGER_HPP