// logger_linux.hpp - header-only system call logger (Linux version of logger.hpp)
//
// Same interface as the Windows logger: namespace logger, init(), logSyscall(),
// LOG_SYSCALL(...). To switch a program between Windows and Linux, change
// only the #include line.
//
// IMPORTANT: never call the logger inside a timing loop. Printing to the
// console and writing to a file take far longer than most system calls and
// would completely ruin the measurements. Log only in setup/verification code.
//
// Windows call -> Linux call (what was replaced)
//   +--------------------------------------+------------------------------------------+
//   | Windows                              | Linux                                    |
//   +--------------------------------------+------------------------------------------+
//   | GetLastError()                       | errno                                    |
//   | SetLastError(code)                   | errno = code                             |
//   | FormatMessageA(...)                  | strerror_r(...)                          |
//   | GetLocalTime(&SYSTEMTIME)            | clock_gettime(CLOCK_REALTIME)+localtime_r|
//   | CreateDirectoryA(folder)             | mkdir(folder, 0755)                      |
//   | ofstream in append mode              | open(O_WRONLY|O_CREAT|O_APPEND)+write()  |
//   +--------------------------------------+------------------------------------------+

#ifndef LOGGER_LINUX_HPP
#define LOGGER_LINUX_HPP

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace logger {

namespace detail {

// Holds the log path and whether init() has already run.
// A function-local static avoids needing a global variable defined in a .cpp.
struct State {
    std::string path = "logs/syscalls_linux.log";
    bool ready = false;
    bool warnedAboutFile = false;   // warn about an unusable log file only once
};

inline State& state() {
    static State s;
    return s;
}

// Creates every folder in the path of a file, e.g. "logs/x.log" creates "logs".
// Errors are ignored on purpose: "already exists" (EEXIST) is the normal case.
inline void createParentFolders(const std::string& filePath) {
    for (size_t i = 0; i < filePath.size(); ++i) {
        if (filePath[i] == '/') {
            std::string folder = filePath.substr(0, i);
            if (!folder.empty()) {          // skip the leading "/" of absolute paths
                mkdir(folder.c_str(), 0755);
            }
        }
    }
}

// Current local time as HH:MM:SS.mmm
inline std::string timestampNow() {
    timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    time_t seconds = now.tv_sec;
    tm local;
    localtime_r(&seconds, &local);          // the _r version is thread-safe
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d.%03d",
                  local.tm_hour, local.tm_min, local.tm_sec,
                  static_cast<int>(now.tv_nsec / 1000000));
    return buffer;
}

// glibc has TWO incompatible versions of strerror_r:
//   GNU version: returns a char* (it may point to its own static text
//                instead of our buffer)
//   XSI version: returns an int (0 = ok) and always fills our buffer
// g++ defines _GNU_SOURCE by default, so the GNU version is normally used,
// but we do not want the code to break if the other one is selected.
// Two overloaded helpers pick the right behaviour from the RETURN TYPE of
// strerror_r, so the same call compiles with both versions.
inline const char* strerrorResult(const char* gnuReturn, const char* /*buffer*/) {
    return gnuReturn;                                   // GNU: use the returned pointer
}
inline const char* strerrorResult(int xsiReturn, const char* buffer) {
    return (xsiReturn == 0) ? buffer : "Unknown error"; // XSI: text is in the buffer
}

// Turns an errno value into text. strerror() is NOT used because it is not
// thread-safe; strerror_r writes into our own buffer instead.
inline std::string errorText(int code) {
    char buffer[256];
    buffer[0] = '\0';
    const char* text = strerrorResult(strerror_r(code, buffer, sizeof(buffer)), buffer);
    if (text == nullptr || text[0] == '\0') {
        return "Unknown error";
    }
    return text;   // unlike FormatMessage, there is no trailing newline to strip
}

} // namespace detail

// Sets the log file path and creates its folder. Calling it is optional:
// if you never call it, the first log call uses the default path.
inline void init(const std::string& logFilePath = "logs/syscalls_linux.log") {
    detail::state().path = logFilePath;
    detail::createParentFolders(logFilePath);
    detail::state().ready = true;
}

// Records the result of one system call.
// callName: e.g. "open"   success: did the call work?   detail: e.g. a file name
inline void logSyscall(const char* callName, bool success, const char* detail = "") {
    // FIRST statement: read errno before anything else can overwrite it.
    // (detail is a plain const char*, so no string is built before this line.)
    int errorCode = errno;

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

    // File: opened in append mode, the WHOLE line (with its newline) is written
    // with ONE write() call, then the file is closed. With O_APPEND the kernel
    // moves to the end of the file and writes in one atomic step, so a single
    // write() from one process cannot be cut in half by a write() from another
    // process. Several processes can therefore share one log file without
    // their lines mixing. (Two separate writes, for example the text and then
    // the newline, could be interleaved with another process, so we build the
    // full line first.)
    line += "\n";
    int fd = open(st.path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        ssize_t written = write(fd, line.data(), line.size());
        (void)written;   // a failed log write must never stop the program
        close(fd);
    } else if (!st.warnedAboutFile) {
        st.warnedAboutFile = true;
        std::cerr << "logger: cannot open log file " << st.path << std::endl;
    }

    errno = errorCode;   // LAST statement: give the caller back the error it had
}

} // namespace logger

// Short call-site form:  LOG_SYSCALL("open", fd >= 0, "input.txt");
// Pass "" as detail if you have none. If your success expression itself calls
// system functions, compute it into a bool right after the system call first.
#define LOG_SYSCALL(callName, success, detail) \
    logger::logSyscall((callName), (success), (detail))

#endif // LOGGER_LINUX_HPP