// =============================================================================
// IPC_Windows.cpp
//
// Part of an OS project comparing Linux and Windows system calls (Windows side,
// IPC category).
//
// What this program does:
//   PART 1  Demo (logged, not timed):
//             A. anonymous pipe: parent writes 4096 bytes, a child process
//                reads and verifies them, then sees end-of-data
//             B. shared memory: parent and child exchange data in BOTH
//                directions through one mapped block
//             C. one deliberate failure (OpenFileMapping on a missing name)
//   PART 2  Benchmarks (1000 iterations, 50 warm-up each), timed separately,
//           all inside ONE process (no child is started):
//             CreatePipe, PipeWrite_4KB, PipeWrite_64KB, PipeRead_4KB,
//             PipeRead_64KB, CreateFileMapping_1MB, MapViewOfFile_1MB,
//             UnmapViewOfFile_64KB
//           Results are printed and appended to results/Windows/windows_results.csv.
//   PART 3  Everything printed to the console is also saved to
//           Windows/IPC/IPC_Windows.txt (overwritten on every run).
//
// The child is THIS SAME EXE, started with "--child-pipe ..." or
// "--child-shm ...". Because the child is this exe, build it statically:
//   g++ -std=c++17 -static -static-libgcc -static-libstdc++ Windows\IPC\IPC_Windows.cpp -o Windows\IPC\ipc.exe -lpsapi
//   Windows\IPC\ipc.exe
//
// Exit codes of the main program: 0 = everything OK, 1 = demo failed,
// 2 = a benchmark had problems.
// =============================================================================

// logger.hpp includes windows.h first, so this must come BEFORE the helpers.
// It makes QueryProcessCycleTime (used by metrics.hpp) visible on MinGW.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include "../../common/logger.hpp"
#include "../../common/metrics.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <streambuf>
#include <string>
#include <vector>
using namespace std;

// ----------------------------- Settings --------------------------------------

const char* const kTranscriptFile = "Windows/IPC/IPC_Windows.txt";
const char* const kMissingMappingName = "Local\\OsKalmanNoSuchMapping";

const int    kIterations = 1000;
const int    kWarmup     = 50;
// The warm-up runs the action too, so every benchmark that consumes one
// prepared item per call needs warm-up + iterations items (1050).
const size_t kTotalCalls = kWarmup + kIterations;

// Every wait has a timeout, never INFINITE, so the program can never hang.
const DWORD kWaitTimeoutMs = 10000;

// A handle count more than this much higher after Part 2 means a leak.
const DWORD kHandleLeakTolerance = 5;

const DWORD kSize4KB   = 4 * 1024;
const DWORD kSize64KB  = 64 * 1024;
const DWORD kSize1MB   = 1024 * 1024;
const DWORD kHalfSize  = kSize1MB / 2;     // 512 KB: each side of the shared block
const DWORD kDemoPipeMessageSize = 4096;

// TOTAL CHILD PROCESSES STARTED BY THE WHOLE PROGRAM: 2 (one per demo test).
// The benchmarks start no child at all.

// ----------------------------- Tee stream buffer (PART 3) --------------------

// Sends every character to two stream buffers: the original console buffer
// and the text file. Installed into cout so that all normal output,
// including the logger's lines, is copied to the file automatically.
class TeeBuffer : public streambuf {
public:
    TeeBuffer(streambuf* first, streambuf* second)
        : first_(first), second_(second) {}

protected:
    // Called for single characters (we have no internal buffer, so every
    // character that is not handled by xsputn arrives here).
    int_type overflow(int_type ch) override {
        if (traits_type::eq_int_type(ch, traits_type::eof())) {
            return traits_type::not_eof(ch);
        }
        char_type c = traits_type::to_char_type(ch);
        int_type r1 = first_->sputc(c);
        int_type r2 = second_->sputc(c);
        if (traits_type::eq_int_type(r1, traits_type::eof()) ||
            traits_type::eq_int_type(r2, traits_type::eof())) {
            return traits_type::eof();
        }
        return ch;
    }

    // Called for whole strings; faster than going character by character.
    streamsize xsputn(const char_type* text, streamsize count) override {
        streamsize written1 = first_->sputn(text, count);
        streamsize written2 = second_->sputn(text, count);
        return min(written1, written2);
    }

    // endl and flush() end up here: flush both destinations.
    int sync() override {
        int r1 = first_->pubsync();
        int r2 = second_->pubsync();
        return (r1 == 0 && r2 == 0) ? 0 : -1;
    }

private:
    streambuf* first_;
    streambuf* second_;
};

// ----------------------------- Small helpers ---------------------------------

// One byte of the deterministic pattern: first, first+1, ... first+25, repeat.
// 'A' is used for the parent's data, 'a' (lowercase) for the child's data.
char patternChar(size_t index, char first) {
    return static_cast<char>(first + (index % 26));
}

// Deterministic test data: A-Z repeated. The parent and the pipe child both
// use this same function, so they always agree on what the message is.
vector<char> buildTestData(size_t size) {
    vector<char> data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = patternChar(i, 'A');
    }
    return data;
}

// Returns true if the call worked AND moved the expected number of bytes.
// A call that "worked" but moved the wrong amount has no Windows error code,
// so we set one; otherwise the log line would show a stale, unrelated error.
bool transferSucceeded(BOOL apiOk, DWORD actual, DWORD expected) {
    if (apiOk && actual != expected) {
        SetLastError(ERROR_INVALID_DATA);
        return false;
    }
    return apiOk != 0;
}

// Counts failures inside benchmark lambdas. Lambdas must not log (logging
// would ruin the timing), so they only record here; the result is reported
// once, after the benchmark.
struct FailureTracker {
    int   count = 0;
    DWORD firstError = 0;

    // apiFailed = true : a Windows call returned failure, keep its GetLastError().
    // apiFailed = false: some other problem (for example a wrong byte count).
    // Call this IMMEDIATELY after the failing call so the error is not overwritten.
    void record(bool apiFailed) {
        if (count == 0) {
            firstError = apiFailed ? GetLastError() : ERROR_INVALID_DATA;
        }
        ++count;
    }
};

// Prints the stats, flags failures loudly and saves the CSV row.
// A benchmark with failed calls is NOT written to the CSV, so bad numbers
// cannot slip into the report. Returns true only if everything was clean.
bool finishBenchmark(const metrics::Stats& stats, const FailureTracker& failures) {
    metrics::printStats(stats);

    if (failures.count > 0) {
        string logName = stats.callName + " benchmark";
        string detail = to_string(failures.count) +
                        " failed call(s) in warm-up + timed loop";
        cout << "!!! WARNING: " << stats.callName << " had " << failures.count
             << " failed call(s). Results are NOT valid and were NOT saved to the CSV.\n";
        // Printing may have changed the last error, so restore the saved one
        // right before logging.
        SetLastError(failures.firstError);
        LOG_SYSCALL(logName.c_str(), false, detail.c_str());
        return false;
    }

    if (!metrics::appendStatsToCsv(stats)) {
        cout << "!!! WARNING: CSV row for " << stats.callName << " was NOT saved.\n";
        return false;
    }
    return true;
}

// ----------------------------- CHILD MODES -----------------------------------
//
// The child is this same program, started with "--child-pipe" or "--child-shm".
// main() checks for these flags FIRST and calls one of the two functions below.
// A child must do NOTHING except its job and return an exit code:
//   1. It must not open (and so overwrite) the transcript file.
//   2. It must not initialise or write to the log file. So it cannot log, and
//      reports only through its exit code.
//   3. It must never start children of its own (that would multiply processes
//      without end and flood the machine).

// "--child-pipe <handleValue> <messageLength>"
// Exit codes: 0 = message correct and end-of-data seen, 1 = data mismatch,
// 2 = a read failed unexpectedly, 3 = the extra read did not report end of data.
int runPipeChild(int argc, char* argv[]) {
    if (argc < 4) {
        return 2;
    }
    unsigned long long handleValue = strtoull(argv[2], nullptr, 10);
    size_t messageLength = static_cast<size_t>(strtoull(argv[3], nullptr, 10));

    // The parent allowed this handle to be inherited, so it has the same
    // numeric value here as in the parent.
    HANDLE readHandle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handleValue));

    // Read until all bytes arrived. A pipe may hand over the data in pieces.
    vector<char> received(messageLength);
    size_t totalRead = 0;
    while (totalRead < messageLength) {
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(readHandle, received.data() + totalRead,
                           static_cast<DWORD>(messageLength - totalRead), &bytesRead, nullptr);
        if (!ok || bytesRead == 0) {
            CloseHandle(readHandle);
            return 2;
        }
        totalRead += bytesRead;
    }

    vector<char> expected = buildTestData(messageLength);
    if (memcmp(received.data(), expected.data(), messageLength) != 0) {
        CloseHandle(readHandle);
        return 1;
    }

    // One more read. The parent closed its write end, so the pipe has no
    // writer left: this read must FAIL with ERROR_BROKEN_PIPE (end of data).
    char extra = 0;
    DWORD extraRead = 0;
    BOOL extraOk = ReadFile(readHandle, &extra, 1, &extraRead, nullptr);
    DWORD extraError = GetLastError();   // read before CloseHandle can change it
    CloseHandle(readHandle);

    if (extraOk) {
        return 3;   // it returned data or success: no end-of-data
    }
    return (extraError == ERROR_BROKEN_PIPE) ? 0 : 2;
}

// "--child-shm <mappingName>"
// Exit codes: 0 = OK, 1 = the first half did not match, 2 = open or map failed.
int runSharedMemoryChild(int argc, char* argv[]) {
    if (argc < 3) {
        return 2;
    }

    HANDLE mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, argv[2]);
    if (mapping == nullptr) {
        return 2;
    }
    volatile char* memory = static_cast<volatile char*>(
        MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, kSize1MB));
    if (memory == nullptr) {
        CloseHandle(mapping);
        return 2;
    }

    // The parent wrote 'A' + i % 26 into the first half.
    bool firstHalfOk = true;
    for (size_t i = 0; i < kHalfSize; ++i) {
        if (memory[i] != patternChar(i, 'A')) {
            firstHalfOk = false;
            break;
        }
    }

    // Our answer for the parent: lowercase pattern in the second half.
    for (size_t i = 0; i < kHalfSize; ++i) {
        memory[kHalfSize + i] = patternChar(i, 'a');
    }

    UnmapViewOfFile(const_cast<char*>(memory));
    CloseHandle(mapping);
    return firstHalfOk ? 0 : 1;
}

// ----------------------------- Launching the child (parent side) -------------

// Full path of this exe. It is used both as lpApplicationName and inside the
// command line, so a space in the path (hp ZBook) is never split.
bool getOwnPath(string& path) {
    char buffer[1024];
    DWORD length = GetModuleFileNameA(nullptr, buffer, sizeof(buffer));
    bool ok = (length > 0 && length < sizeof(buffer));
    LOG_SYSCALL("GetModuleFileName", ok, "path of this exe");
    if (ok) {
        path.assign(buffer, length);
    }
    return ok;
}

// Everything needed to start one child. The command line is copied into a
// writable buffer, because CreateProcess is allowed to modify it.
struct ChildCommand {
    string exePath;          // goes into lpApplicationName
    string commandText;      // "\"full path\" <arguments>"
    vector<char> buffer;     // writable copy passed as lpCommandLine
    bool inheritHandles;     // TRUE only for the pipe child
};

ChildCommand makeChildCommand(const string& exePath, const string& arguments,
                              bool inheritHandles) {
    ChildCommand cmd;
    cmd.exePath = exePath;
    cmd.commandText = "\"" + exePath + "\" " + arguments;
    cmd.buffer.assign(cmd.commandText.begin(), cmd.commandText.end());
    cmd.buffer.push_back('\0');
    cmd.inheritHandles = inheritHandles;
    return cmd;
}

// Starts the child. Returns the CreateProcessA result (0 = failed). On success
// the caller owns BOTH handles in `info` and must close both.
BOOL launchChild(ChildCommand& cmd, PROCESS_INFORMATION& info) {
    STARTUPINFOA startup;
    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);

    // bInheritHandles = TRUE lets the child receive handles that were marked
    // inheritable (only the pipe read end). FALSE for the shared memory child.
    return CreateProcessA(cmd.exePath.c_str(), cmd.buffer.data(),
                          nullptr, nullptr, cmd.inheritHandles ? TRUE : FALSE,
                          0, nullptr, nullptr, &startup, &info);
}

// Closes both handles of a child and logs each close.
bool closeChildHandlesLogged(PROCESS_INFORMATION& info, const string& detail) {
    BOOL threadClosed = CloseHandle(info.hThread);
    LOG_SYSCALL("CloseHandle", threadClosed != 0, ("thread handle, " + detail).c_str());
    BOOL processClosed = CloseHandle(info.hProcess);
    LOG_SYSCALL("CloseHandle", processClosed != 0, ("process handle, " + detail).c_str());
    info.hThread = nullptr;
    info.hProcess = nullptr;
    return threadClosed && processClosed;
}

// Starts a child, waits for it (10 s), reads its exit code, and ALWAYS closes
// both handles. Every step is logged. Returns true if all calls worked; the
// exit code itself is returned in `exitCode` for the caller to verify.
bool runChildAndGetExitCode(ChildCommand& cmd, const string& detail, DWORD& exitCode) {
    exitCode = 0;
    PROCESS_INFORMATION info;
    ZeroMemory(&info, sizeof(info));

    BOOL created = launchChild(cmd, info);
    LOG_SYSCALL("CreateProcess", created != 0, detail.c_str());
    if (!created) {
        return false;
    }
    cout << "Child process id: " << info.dwProcessId << "\n";

    // WAIT_TIMEOUT has no GetLastError code, so we set one.
    DWORD waitResult = WaitForSingleObject(info.hProcess, kWaitTimeoutMs);
    bool waitOk = (waitResult == WAIT_OBJECT_0);
    if (waitResult == WAIT_TIMEOUT) {
        SetLastError(WAIT_TIMEOUT);
    }
    LOG_SYSCALL("WaitForSingleObject", waitOk, detail.c_str());

    bool gotCode = false;
    if (waitOk) {
        gotCode = (GetExitCodeProcess(info.hProcess, &exitCode) != 0);
        LOG_SYSCALL("GetExitCodeProcess", gotCode, detail.c_str());
    } else if (waitResult == WAIT_TIMEOUT) {
        TerminateProcess(info.hProcess, 1);   // never leave a stuck child behind
    }

    bool closed = closeChildHandlesLogged(info, detail);
    return waitOk && gotCode && closed;
}

// ----------------------------- PART 1: demo ----------------------------------

// A. Anonymous pipe: parent -> child.
bool demoPipe(const string& exePath) {
    cout << "\n-- A. Anonymous pipe: parent writes, child reads --\n";

    // bInheritHandle = TRUE: the handles may be passed to a child.
    SECURITY_ATTRIBUTES security;
    security.nLength = sizeof(security);
    security.lpSecurityDescriptor = nullptr;
    security.bInheritHandle = TRUE;

    HANDLE readHandle = nullptr;
    HANDLE writeHandle = nullptr;
    BOOL ok = CreatePipe(&readHandle, &writeHandle, &security, kSize64KB);
    LOG_SYSCALL("CreatePipe", ok != 0, "64 KB buffer, inheritable");
    if (!ok) {
        return false;
    }

    // Only the READ end may be inherited. If the child also held the write
    // end, the pipe would never report end-of-data to it.
    ok = SetHandleInformation(writeHandle, HANDLE_FLAG_INHERIT, 0);
    LOG_SYSCALL("SetHandleInformation", ok != 0, "write end: not inheritable");
    if (!ok) {
        CloseHandle(readHandle);
        CloseHandle(writeHandle);
        return false;
    }

    // The message (4096 bytes) fits in the 64 KB pipe buffer, so this write
    // cannot block even though the child has not started yet.
    vector<char> message = buildTestData(kDemoPipeMessageSize);
    cout << "Message starts with: " << string(message.begin(), message.begin() + 40) << "\n";
    DWORD bytesWritten = 0;
    BOOL writeOk = WriteFile(writeHandle, message.data(), kDemoPipeMessageSize,
                             &bytesWritten, nullptr);
    bool writeGood = transferSucceeded(writeOk, bytesWritten, kDemoPipeMessageSize);
    LOG_SYSCALL("WriteFile", writeGood, "4096 bytes into the pipe");
    if (!writeGood) {
        CloseHandle(readHandle);
        CloseHandle(writeHandle);
        return false;
    }

    // Closing the write end is what makes end-of-data visible to the child.
    BOOL writeClosed = CloseHandle(writeHandle);
    LOG_SYSCALL("CloseHandle", writeClosed != 0, "pipe write end");
    writeHandle = nullptr;
    if (!writeClosed) {
        CloseHandle(readHandle);
        return false;
    }

    // Start the child and hand it the numeric value of the read handle.
    string arguments = "--child-pipe " +
                       to_string(static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(readHandle))) +
                       " " + to_string(kDemoPipeMessageSize);
    string detail = "--child-pipe (4096 bytes)";
    ChildCommand cmd = makeChildCommand(exePath, arguments, true);
    cout << "Command line: " << cmd.commandText << "\n";

    DWORD exitCode = 0;
    bool childOk = runChildAndGetExitCode(cmd, detail, exitCode);

    // The parent's read end is closed in every case.
    BOOL readClosed = CloseHandle(readHandle);
    LOG_SYSCALL("CloseHandle", readClosed != 0, "pipe read end");

    bool verified = false;
    if (childOk) {
        verified = (exitCode == 0);
        if (verified) {
            cout << "Verification PASSED: the child received all 4096 bytes correctly "
                 << "and then saw end-of-data (exit code 0).\n";
            LOG_SYSCALL("VerifyPipeTransfer", true, detail.c_str());
        } else {
            cout << "Verification FAILED: child exit code " << exitCode
                 << " (1 = data mismatch, 2 = read failed, 3 = no end-of-data).\n";
            SetLastError(ERROR_INVALID_DATA);
            LOG_SYSCALL("VerifyPipeTransfer", false, detail.c_str());
        }
    }
    return childOk && verified && readClosed;
}

// B. Shared memory: data travels in both directions.
bool demoSharedMemory(const string& exePath) {
    cout << "\n-- B. Shared memory: parent <-> child --\n";

    // A name with the process id, so repeated runs never collide.
    string name = "Local\\OsKalmanShm_" + to_string(GetCurrentProcessId());
    cout << "Mapping name: " << name << "\n";

    HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                        0, kSize1MB, name.c_str());
    LOG_SYSCALL("CreateFileMapping", mapping != nullptr, name.c_str());
    if (mapping == nullptr) {
        return false;
    }

    volatile char* memory = static_cast<volatile char*>(
        MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));   // 0 = whole mapping
    LOG_SYSCALL("MapViewOfFile", memory != nullptr, "whole 1 MB");
    if (memory == nullptr) {
        CloseHandle(mapping);
        return false;
    }

    // First half: our pattern. Second half: zeros, so we can tell later
    // whether the child really wrote there.
    for (size_t i = 0; i < kHalfSize; ++i) {
        memory[i] = patternChar(i, 'A');
        memory[kHalfSize + i] = 0;
    }

    // bInheritHandles = FALSE: the child finds the block BY NAME instead.
    string detail = "--child-shm " + name;
    ChildCommand cmd = makeChildCommand(exePath, "--child-shm " + name, false);
    DWORD exitCode = 0;
    bool childOk = runChildAndGetExitCode(cmd, detail, exitCode);

    bool exitCodeOk = childOk && (exitCode == 0);
    if (childOk && !exitCodeOk) {
        cout << "Child exit code " << exitCode
             << " (1 = first half did not match, 2 = open or map failed).\n";
    }

    // Did the child's lowercase pattern arrive in the second half?
    bool secondHalfOk = true;
    if (childOk) {
        for (size_t i = 0; i < kHalfSize; ++i) {
            if (memory[kHalfSize + i] != patternChar(i, 'a')) {
                secondHalfOk = false;
                break;
            }
        }
    }
    bool verified = exitCodeOk && secondHalfOk;
    if (verified) {
        cout << "Verification PASSED: data travelled parent -> child (first half, 'A' pattern "
             << "checked by the child) and child -> parent (second half, 'a' pattern checked here).\n";
        LOG_SYSCALL("VerifySharedMemory", true, name.c_str());
    } else {
        cout << "Verification FAILED: shared memory content is not what was expected.\n";
        SetLastError(ERROR_INVALID_DATA);
        LOG_SYSCALL("VerifySharedMemory", false, name.c_str());
    }

    // Always release the view and the mapping.
    BOOL unmapped = UnmapViewOfFile(const_cast<char*>(memory));
    LOG_SYSCALL("UnmapViewOfFile", unmapped != 0, "whole 1 MB view");
    BOOL closed = CloseHandle(mapping);
    LOG_SYSCALL("CloseHandle", closed != 0, "mapping handle");

    return verified && unmapped && closed;
}

// C. Deliberate failure: open a mapping that does not exist (error 2).
bool demoMissingMapping() {
    cout << "\n-- C. Opening a shared memory block that does not exist --\n";
    cout << "OpenFileMapping on a missing name (this FAILURE is expected, error 2):\n";

    HANDLE mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, kMissingMappingName);
    DWORD errorCode = GetLastError();   // read before anything can change it
    LOG_SYSCALL("OpenFileMapping", mapping != nullptr, kMissingMappingName);

    if (mapping != nullptr) {
        cout << "UNEXPECTED: OpenFileMapping found a mapping that should not exist!\n";
        CloseHandle(mapping);
        return false;
    }
    if (errorCode != ERROR_FILE_NOT_FOUND) {
        cout << "UNEXPECTED: it failed, but with error " << errorCode << " instead of 2.\n";
        return false;
    }
    return true;
}

// Returns true if every step worked and every verification passed.
bool runDemo(const string& exePath) {
    cout << "---------- PART 1: DEMO (logged, not timed) ----------\n";
    cout << "Parent process id: " << GetCurrentProcessId() << "\n";

    bool pipeOk = demoPipe(exePath);
    bool sharedOk = demoSharedMemory(exePath);
    bool missingOk = demoMissingMapping();
    return pipeOk && sharedOk && missingOk;
}

// ----------------------------- PART 2: benchmarks ----------------------------
//
// NOTES FOR THE REPORT
//  (1) Required Windows IPC calls of the assignment: CreatePipe,
//      CreateFileMapping, MapViewOfFile. Helper calls needed to make them work:
//      UnmapViewOfFile, OpenFileMapping, SetHandleInformation, plus
//      ReadFile / WriteFile (already covered by the File_IO category).
//  (2) The pipe is one-way, like Linux pipe(). An anonymous pipe is a kernel
//      buffer: WriteFile copies data IN, ReadFile copies data OUT. The data is
//      copied twice.
//  (3) Shared memory has no copy: after MapViewOfFile both processes read and
//      write the same physical pages. The cost sits in creating and mapping,
//      not in using. Windows does not assign physical pages until the memory
//      is first touched, so the memory delta of the mapping benchmarks is
//      expected to be small.
//  (4) The CPU columns (cpu_us, cpu_per_call_us, cpu_cycles, cycles_per_call)
//      are measured for the PARENT only, around the whole loop, and include
//      cleanup and timer overhead. Only min/median/mean/max/stddev cover the
//      action alone.
//  (5) The benchmarks run inside ONE process. They measure the cost of the
//      calls, not cross-process latency. Cross-process correctness is shown
//      in the demo.
//  (6) The warm-up runs the lambdas too, so failures there are counted as well.

// Setup helper: an anonymous pipe (not inheritable) with the given buffer size.
bool createPipeForBenchmark(HANDLE& readHandle, HANDLE& writeHandle,
                            DWORD bufferSize, const string& detail) {
    BOOL ok = CreatePipe(&readHandle, &writeHandle, nullptr, bufferSize);
    LOG_SYSCALL("CreatePipe_setup", ok != 0, detail.c_str());
    if (!ok) {
        readHandle = nullptr;
        writeHandle = nullptr;
    }
    return ok != 0;
}

// Teardown helper: closes both pipe ends, one log line each.
bool closePipeAfterBenchmark(HANDLE readHandle, HANDLE writeHandle) {
    BOOL readClosed = CloseHandle(readHandle);
    LOG_SYSCALL("CloseHandle_teardown", readClosed != 0, "pipe read end");
    BOOL writeClosed = CloseHandle(writeHandle);
    LOG_SYSCALL("CloseHandle_teardown", writeClosed != 0, "pipe write end");
    return readClosed && writeClosed;
}

// Setup helper: an unnamed, pagefile-backed 1 MB mapping.
bool createMappingForBenchmark(HANDLE& mapping, const char* detail) {
    mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                 0, kSize1MB, nullptr);
    LOG_SYSCALL("CreateFileMapping_setup", mapping != nullptr, detail);
    return mapping != nullptr;
}

bool closeMappingAfterBenchmark(HANDLE mapping) {
    BOOL ok = CloseHandle(mapping);
    LOG_SYSCALL("CloseHandle_teardown", ok != 0, "mapping handle");
    return ok != 0;
}

// 1. Benchmark: CreatePipe (64 KB buffer). The cleanup closes both handles
// outside the timed region so handles do not pile up.
bool benchmarkCreatePipe() {
    FailureTracker failures;
    HANDLE readHandle = nullptr;
    HANDLE writeHandle = nullptr;
    bool created = false;

    auto action = [&]() {
        created = (CreatePipe(&readHandle, &writeHandle, nullptr, kSize64KB) != 0);
        if (!created) {
            failures.record(true);
        }
    };

    auto cleanup = [&]() {
        if (!created) {
            return;
        }
        if (!CloseHandle(readHandle)) {
            failures.record(true);
        }
        if (!CloseHandle(writeHandle)) {
            failures.record(true);
        }
        readHandle = nullptr;
        writeHandle = nullptr;
        created = false;
    };

    metrics::Stats stats = metrics::benchmark("CreatePipe", action, kIterations, kWarmup, cleanup);
    return finishBenchmark(stats, failures);
}

// 2. Benchmark: WriteFile into a pipe (4 KB or 64 KB per call).
// WHY every iteration drains what it wrote: a WriteFile into a FULL anonymous
// pipe blocks forever when nobody reads. The cleanup (untimed) reads back the
// same number of bytes, so the pipe is empty again for the next iteration.
bool benchmarkPipeWrite(const char* callName, DWORD size) {
    FailureTracker failures;
    vector<char> source = buildTestData(size);
    vector<char> drain(size);

    HANDLE readHandle = nullptr;
    HANDLE writeHandle = nullptr;
    if (!createPipeForBenchmark(readHandle, writeHandle, kSize1MB, "1 MB buffer")) {
        return false;
    }

    DWORD lastWritten = 0;   // bytes that really went into the pipe this iteration

    auto action = [&]() {
        lastWritten = 0;
        DWORD bytesWritten = 0;
        BOOL ok = WriteFile(writeHandle, source.data(), size, &bytesWritten, nullptr);
        if (!ok) {
            failures.record(true);
        } else if (bytesWritten != size) {
            failures.record(false);
        }
        if (ok) {
            lastWritten = bytesWritten;
        }
    };

    // Reads back exactly what was written. If nothing was written we must NOT
    // read, because reading an empty pipe would block.
    auto cleanup = [&]() {
        if (lastWritten == 0) {
            return;
        }
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(readHandle, drain.data(), lastWritten, &bytesRead, nullptr);
        if (!ok) {
            failures.record(true);
        } else if (bytesRead != lastWritten) {
            failures.record(false);
        }
    };

    metrics::Stats stats = metrics::benchmark(callName, action, kIterations, kWarmup, cleanup);

    bool closedOk = closePipeAfterBenchmark(readHandle, writeHandle);
    bool clean = finishBenchmark(stats, failures);
    return clean && closedOk;
}

// 3. Benchmark: ReadFile from a pipe (4 KB or 64 KB per call).
// The pipe is filled once before the benchmark. After every read the cleanup
// (untimed) writes the same amount back, so the next read always finds data
// and never blocks.
bool benchmarkPipeRead(const char* callName, DWORD size) {
    FailureTracker failures;
    vector<char> source = buildTestData(size);
    vector<char> target(size);

    HANDLE readHandle = nullptr;
    HANDLE writeHandle = nullptr;
    if (!createPipeForBenchmark(readHandle, writeHandle, kSize1MB, "1 MB buffer")) {
        return false;
    }

    // Setup: put `size` bytes into the pipe once.
    DWORD setupWritten = 0;
    BOOL setupOk = WriteFile(writeHandle, source.data(), size, &setupWritten, nullptr);
    bool setupGood = transferSucceeded(setupOk, setupWritten, size);
    LOG_SYSCALL("WriteFile_setup", setupGood, "fill the pipe once");
    if (!setupGood) {
        closePipeAfterBenchmark(readHandle, writeHandle);
        return false;
    }

    bool lastReadOk = false;

    auto action = [&]() {
        lastReadOk = false;
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(readHandle, target.data(), size, &bytesRead, nullptr);
        if (!ok) {
            failures.record(true);
        } else if (bytesRead != size) {
            failures.record(false);
        } else {
            lastReadOk = true;
        }
    };

    // Put the data back only if the read really took it out.
    auto cleanup = [&]() {
        if (!lastReadOk) {
            return;
        }
        DWORD bytesWritten = 0;
        BOOL ok = WriteFile(writeHandle, source.data(), size, &bytesWritten, nullptr);
        if (!ok) {
            failures.record(true);
        } else if (bytesWritten != size) {
            failures.record(false);
        }
    };

    metrics::Stats stats = metrics::benchmark(callName, action, kIterations, kWarmup, cleanup);

    bool closedOk = closePipeAfterBenchmark(readHandle, writeHandle);
    bool clean = finishBenchmark(stats, failures);
    return clean && closedOk;
}

// 4. Benchmark: CreateFileMapping (unnamed, 1 MB). The cleanup closes the handle.
bool benchmarkCreateFileMapping() {
    FailureTracker failures;
    HANDLE mapping = nullptr;

    auto action = [&]() {
        mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                     0, kSize1MB, nullptr);
        if (mapping == nullptr) {
            failures.record(true);
        }
    };

    auto cleanup = [&]() {
        if (mapping != nullptr) {
            if (!CloseHandle(mapping)) {
                failures.record(true);
            }
            mapping = nullptr;
        }
    };

    metrics::Stats stats = metrics::benchmark("CreateFileMapping_1MB", action,
                                              kIterations, kWarmup, cleanup);
    return finishBenchmark(stats, failures);
}

// 5. Benchmark: MapViewOfFile of a whole 1 MB mapping. The mapping is created
// once; the cleanup unmaps the view outside the timed region.
bool benchmarkMapViewOfFile() {
    FailureTracker failures;
    HANDLE mapping = nullptr;
    if (!createMappingForBenchmark(mapping, "1 MB, unnamed")) {
        return false;
    }

    void* view = nullptr;

    auto action = [&]() {
        view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if (view == nullptr) {
            failures.record(true);
        }
    };

    auto cleanup = [&]() {
        if (view != nullptr) {
            if (!UnmapViewOfFile(view)) {
                failures.record(true);
            }
            view = nullptr;
        }
    };

    metrics::Stats stats = metrics::benchmark("MapViewOfFile_1MB", action,
                                              kIterations, kWarmup, cleanup);

    bool closedOk = closeMappingAfterBenchmark(mapping);
    bool clean = finishBenchmark(stats, failures);
    return clean && closedOk;
}

// 6. Benchmark: UnmapViewOfFile.
// SETUP maps 1050 separate 64 KB views of one 1 MB mapping; each call unmaps
// the next one. 64 KB views are used on purpose: the total address space is
// then about 67 MB instead of 1 GB (a 32-bit build could not hold 1 GB).
bool benchmarkUnmapViewOfFile() {
    FailureTracker failures;
    HANDLE mapping = nullptr;
    if (!createMappingForBenchmark(mapping, "1 MB, unnamed")) {
        return false;
    }

    // Built BEFORE the loop so no string work sits between a failing
    // MapViewOfFile and the LOG_SYSCALL that reads its error code.
    string detail = "64 KB x " + to_string(kTotalCalls);
    vector<void*> views;
    views.reserve(kTotalCalls);

    for (size_t i = 0; i < kTotalCalls; ++i) {
        // Offset 0 every time: the same 64 KB of the mapping, but each view
        // gets its own address range, so each needs its own unmap.
        void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, kSize64KB);
        if (view == nullptr) {
            LOG_SYSCALL("MapViewOfFile_setup", false, detail.c_str());
            for (size_t j = 0; j < views.size(); ++j) {
                UnmapViewOfFile(views[j]);
            }
            CloseHandle(mapping);
            return false;
        }
        views.push_back(view);
    }
    LOG_SYSCALL("MapViewOfFile_setup", true, detail.c_str());

    size_t nextIndex = 0;

    auto action = [&]() {
        if (nextIndex >= views.size()) {   // should never happen
            failures.record(false);
            return;
        }
        BOOL ok = UnmapViewOfFile(views[nextIndex]);
        ++nextIndex;
        if (!ok) {
            failures.record(true);
        }
    };

    // No cleanup: each view is unmapped exactly once by the action.
    metrics::Stats stats = metrics::benchmark("UnmapViewOfFile_64KB", action, kIterations, kWarmup);

    // Safety net: unmap any view the benchmark did not reach (normally none).
    for (size_t i = nextIndex; i < views.size(); ++i) {
        UnmapViewOfFile(views[i]);
    }
    views.clear();

    bool closedOk = closeMappingAfterBenchmark(mapping);
    bool clean = finishBenchmark(stats, failures);
    return clean && closedOk;
}

// Reads the number of open handles of this process and logs the call.
bool readHandleCount(const char* when, DWORD& count) {
    BOOL ok = GetProcessHandleCount(GetCurrentProcess(), &count);
    LOG_SYSCALL("GetProcessHandleCount", ok != 0, when);
    return ok != 0;
}

// Runs all benchmarks plus the handle leak check. Returns how many problems.
int runAllBenchmarks() {
    cout << "\n---------- PART 2: BENCHMARKS (timed, no logging inside) ----------\n";
    cout << "Pointer size: " << sizeof(void*) << " bytes ("
         << sizeof(void*) * 8 << "-bit build)\n";
    int problems = 0;

    DWORD handlesBefore = 0;
    DWORD handlesAfter = 0;
    bool beforeOk = readHandleCount("before Part 2", handlesBefore);
    if (beforeOk) {
        cout << "Open handles before Part 2: " << handlesBefore << "\n";
    }

    if (!benchmarkCreatePipe())                          ++problems;
    if (!benchmarkPipeWrite("PipeWrite_4KB", kSize4KB))  ++problems;
    if (!benchmarkPipeWrite("PipeWrite_64KB", kSize64KB)) ++problems;
    if (!benchmarkPipeRead("PipeRead_4KB", kSize4KB))    ++problems;
    if (!benchmarkPipeRead("PipeRead_64KB", kSize64KB))  ++problems;
    if (!benchmarkCreateFileMapping())                   ++problems;
    if (!benchmarkMapViewOfFile())                       ++problems;
    if (!benchmarkUnmapViewOfFile())                     ++problems;

    bool afterOk = readHandleCount("after Part 2", handlesAfter);
    if (afterOk) {
        cout << "Open handles after Part 2 : " << handlesAfter << "\n";
    }

    // Leak check: a forgotten CloseHandle would grow the count by hundreds.
    if (beforeOk && afterOk) {
        if (handlesAfter > handlesBefore + kHandleLeakTolerance) {
            cout << "!!! WARNING: handles are LEAKING (" << (handlesAfter - handlesBefore)
                 << " more than before).\n";
            ++problems;
        } else {
            cout << "Handle leak check PASSED.\n";
        }
    } else {
        cout << "!!! WARNING: could not read the handle count, leak check skipped.\n";
        ++problems;
    }
    return problems;
}

// ----------------------------- Program flow ----------------------------------

int runAll() {
    logger::init();   // default path: logs/syscalls_windows.log

    cout << "=== Windows IPC: demo + benchmarks ===\n\n";

    string exePath;
    if (!getOwnPath(exePath)) {
        cout << "Cannot find the path of this program, so no child can be started.\n";
        return 1;
    }

    if (!runDemo(exePath)) {
        cout << "\nDemo FAILED, so the benchmarks were skipped.\n";
        return 1;
    }

    int problems = runAllBenchmarks();
    if (problems > 0) {
        cout << "\nFinished with " << problems << " benchmark problem(s). See warnings above.\n";
        return 2;
    }

    cout << "\nAll done. Results appended to results/Windows/windows_results.csv\n";
    return 0;
}

int main(int argc, char* argv[]) {
    // CHILD MODES: this MUST be the very first thing in main, before the
    // transcript, the tee, logger::init or any printing. A child does its job,
    // returns an exit code and does NOTHING else (see the CHILD MODES comment).
    if (argc >= 2 && strcmp(argv[1], "--child-pipe") == 0) {
        return runPipeChild(argc, argv);
    }
    if (argc >= 2 && strcmp(argv[1], "--child-shm") == 0) {
        return runSharedMemoryChild(argc, argv);
    }

    // PART 3: copy everything written to cout into a text file as well.
    ofstream transcript(kTranscriptFile, ios::out | ios::trunc);
    streambuf* originalBuffer = cout.rdbuf();
    TeeBuffer tee(originalBuffer, transcript.rdbuf());

    bool teeActive = false;
    if (transcript.is_open()) {
        cout.rdbuf(&tee);
        teeActive = true;
        cout << "Console output is also being saved to " << kTranscriptFile << "\n";
    } else {
        cerr << "Warning: cannot open " << kTranscriptFile
             << " (run from the repository root). Continuing without the text file.\n";
    }

    int exitCode = 1;
    try {
        exitCode = runAll();
    } catch (const exception& error) {
        cout << "Unexpected error: " << error.what() << endl;
    }

    // Restore the original buffer BEFORE the tee and the file are destroyed.
    // Otherwise cout would point to a dead object and crash at exit.
    cout.flush();
    if (teeActive) {
        cout.rdbuf(originalBuffer);
    }
    return exitCode;
}