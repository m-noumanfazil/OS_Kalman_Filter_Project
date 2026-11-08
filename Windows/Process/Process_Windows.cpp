// =============================================================================
// Process_Windows.cpp
//
// Part of an OS project comparing Linux and Windows system calls (Windows side,
// Process category).
//
// What this program does:
//   PART 1  Demo: starts ITSELF as a child ("--child <exitcode>") with
//           CreateProcessA, waits with WaitForSingleObject, reads the exit code
//           with GetExitCodeProcess and verifies it, prints the child's
//           lifetime and CPU time (GetProcessTimes), closes both handles.
//           Done twice (exit codes 42 and 7), plus one deliberate failure
//           (a program that does not exist). Every call is logged (not timed).
//   PART 2  Benchmarks (200 iterations, 10 warm-up each), timed separately:
//             CreateProcess, WaitForSingleObject, CreateProcess_plus_Wait
//           Results are printed and appended to results/Windows/windows_results.csv.
//   PART 3  Everything printed to the console is also saved to
//           Windows/Process/Process_Windows.txt (overwritten on every run).
//
// Compile and run from the REPOSITORY ROOT (the static flags avoid MinGW DLL
// problems, which matter extra here because every child is this same exe):
//   g++ -std=c++17 -static -static-libgcc -static-libstdc++ Windows\Process\Process_Windows.cpp -o Windows\Process\process.exe -lpsapi
//   Windows\Process\process.exe
//
// Exit codes: 0 = everything OK, 1 = demo failed, 2 = a benchmark had problems.
// =============================================================================

// logger.hpp includes windows.h first, so this must come BEFORE the helpers.
// It makes QueryProcessCycleTime (used by metrics.hpp) visible on MinGW.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include "../../common/logger.hpp"
#include "../../common/metrics.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <streambuf>
#include <string>
#include <vector>
using namespace std;

// ----------------------------- Settings --------------------------------------

const char* const kTranscriptFile = "Windows/Process/Process_Windows.txt";

// Process creation takes milliseconds, so we use fewer iterations than the
// other categories (200 instead of 1000, 10 warm-up instead of 50).
const int    kIterations = 200;
const int    kWarmup     = 10;
// The warm-up runs the action too, so every benchmark that consumes one
// prepared item per call needs warm-up + iterations items (210).
const size_t kTotalCalls = kWarmup + kIterations;

// Every wait has a timeout, never INFINITE, so the program can never hang.
const DWORD kWaitTimeoutMs = 10000;

// A handle count more than this much higher after Part 2 means a leak.
const DWORD kHandleLeakTolerance = 5;

// TOTAL CHILD PROCESSES STARTED BY THE WHOLE PROGRAM (stays below about 700):
//   demo 2 + CreateProcess 210 + WaitForSingleObject 210 + CreateProcess_plus_Wait 210
//   = 632 children.

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

// Counts failures inside benchmark lambdas. Lambdas must not log (logging
// would ruin the timing), so they only record here; the result is reported
// once, after the benchmark.
struct FailureTracker {
    int   count = 0;
    DWORD firstError = 0;

    // apiFailed = true : a Windows call returned failure, keep its GetLastError().
    // apiFailed = false: some other problem (for example a nonzero exit code).
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

// FILETIME counts in 100-nanosecond steps, split over two 32-bit halves.
unsigned long long fileTimeTo100ns(const FILETIME& ft) {
    ULARGE_INTEGER value;
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return value.QuadPart;
}

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

// Everything needed to start one child. The command line text is built ONCE;
// before every launch it is copied into a writable buffer, because
// CreateProcess is allowed to modify the command line it receives.
struct ChildCommand {
    string exePath;          // goes into lpApplicationName
    string commandText;      // "\"full path\" --child N"
    vector<char> buffer;     // writable copy passed as lpCommandLine
};

ChildCommand makeChildCommand(const string& exePath, int childExitCode) {
    ChildCommand cmd;
    cmd.exePath = exePath;
    cmd.commandText = "\"" + exePath + "\" --child " + to_string(childExitCode);
    cmd.buffer.assign(cmd.commandText.begin(), cmd.commandText.end());
    cmd.buffer.push_back('\0');
    return cmd;
}

// Starts the child. Returns the CreateProcessA result (0 = failed). On success
// the caller owns BOTH handles in `info` and must close both.
// Call GetLastError() right after this returns if it failed.
BOOL launchChild(ChildCommand& cmd, PROCESS_INFORMATION& info) {
    // Fresh writable copy of the command line (the previous call may have changed it).
    copy(cmd.commandText.begin(), cmd.commandText.end(), cmd.buffer.begin());
    cmd.buffer[cmd.commandText.size()] = '\0';

    STARTUPINFOA startup;
    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);

    // bInheritHandles = FALSE: the child does not need any of our handles.
    // The child shares our console, which is fine because it prints nothing.
    return CreateProcessA(cmd.exePath.c_str(), cmd.buffer.data(),
                          nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                          &startup, &info);
}

// Waits for a child (10 s at most). On timeout it kills the child and records
// a failure; a failed wait is recorded too. Returns true only if the child
// finished normally. Used by the benchmarks, so it never logs.
bool waitOrTerminate(PROCESS_INFORMATION& info, FailureTracker& failures) {
    DWORD result = WaitForSingleObject(info.hProcess, kWaitTimeoutMs);
    if (result == WAIT_OBJECT_0) {
        return true;
    }
    if (result == WAIT_TIMEOUT) {
        SetLastError(WAIT_TIMEOUT);   // a timeout has no GetLastError of its own
        failures.record(true);
        TerminateProcess(info.hProcess, 1);   // never leave a stuck child behind
    } else {
        failures.record(true);   // WAIT_FAILED: GetLastError is meaningful
    }
    return false;
}

// Optionally checks that the child exited with 0, then closes BOTH handles.
// A finished child that is never closed stays in memory as a zombie-like object.
void checkExitAndClose(PROCESS_INFORMATION& info, bool checkExitCode, FailureTracker& failures) {
    if (checkExitCode) {
        DWORD exitCode = 0;
        if (!GetExitCodeProcess(info.hProcess, &exitCode)) {
            failures.record(true);
        } else if (exitCode != 0) {
            failures.record(false);
        }
    }
    if (info.hThread != nullptr && !CloseHandle(info.hThread)) {
        failures.record(true);
    }
    if (info.hProcess != nullptr && !CloseHandle(info.hProcess)) {
        failures.record(true);
    }
    info.hThread = nullptr;
    info.hProcess = nullptr;
}

// Error-path cleanup: kill every child in the list (harmless if it already
// exited), close both handles of each, and empty the list.
void terminateAndCloseAll(vector<PROCESS_INFORMATION>& children) {
    for (size_t i = 0; i < children.size(); ++i) {
        if (children[i].hProcess != nullptr) {
            TerminateProcess(children[i].hProcess, 1);
            CloseHandle(children[i].hProcess);
        }
        if (children[i].hThread != nullptr) {
            CloseHandle(children[i].hThread);
        }
    }
    children.clear();
}

// ----------------------------- PART 1: demo ----------------------------------

// Prints the child's wall-clock lifetime and CPU time (works after the child
// exited, as long as its process handle is still open).
bool reportChildTimes(HANDLE process, const string& detail) {
    FILETIME creationTime, exitTime, kernelTime, userTime;
    BOOL ok = GetProcessTimes(process, &creationTime, &exitTime, &kernelTime, &userTime);
    LOG_SYSCALL("GetProcessTimes", ok != 0, detail.c_str());
    if (!ok) {
        return false;
    }

    // 100 ns units divided by 10 = microseconds.
    double lifetimeUs = static_cast<double>(
        fileTimeTo100ns(exitTime) - fileTimeTo100ns(creationTime)) / 10.0;
    double cpuUs = static_cast<double>(
        fileTimeTo100ns(kernelTime) + fileTimeTo100ns(userTime)) / 10.0;

    cout << fixed << setprecision(1);
    cout << "Child lifetime (exit - creation): " << lifetimeUs << " us\n"
         << "Child CPU time (kernel + user)  : " << cpuUs
         << " us (may be coarse for such a short child)\n";
    return true;
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

// Steps 2 to 6 of the demo for one child that must return `expectedCode`.
bool demoOneChild(const string& exePath, int expectedCode) {
    cout << "\n-- Child with exit code " << expectedCode << " --\n";

    ChildCommand cmd = makeChildCommand(exePath, expectedCode);
    // Built BEFORE the call so no string work happens between a failing
    // CreateProcess and the LOG_SYSCALL that reads its error code.
    string detail = "--child " + to_string(expectedCode);
    cout << "Command line: " << cmd.commandText << "\n";

    PROCESS_INFORMATION info;
    ZeroMemory(&info, sizeof(info));
    BOOL created = launchChild(cmd, info);
    LOG_SYSCALL("CreateProcess", created != 0, detail.c_str());
    if (!created) {
        return false;
    }
    cout << "Child process id: " << info.dwProcessId << "\n";

    // Wait. WAIT_TIMEOUT has no GetLastError code, so we set one.
    DWORD waitResult = WaitForSingleObject(info.hProcess, kWaitTimeoutMs);
    bool waitOk = (waitResult == WAIT_OBJECT_0);
    if (waitResult == WAIT_TIMEOUT) {
        SetLastError(WAIT_TIMEOUT);
    }
    LOG_SYSCALL("WaitForSingleObject", waitOk, detail.c_str());
    if (!waitOk) {
        if (waitResult == WAIT_TIMEOUT) {
            TerminateProcess(info.hProcess, 1);   // do not leave it running
        }
        closeChildHandlesLogged(info, detail);
        return false;
    }

    // Exit code and verification.
    DWORD exitCode = 0;
    BOOL gotCode = GetExitCodeProcess(info.hProcess, &exitCode);
    LOG_SYSCALL("GetExitCodeProcess", gotCode != 0, detail.c_str());

    bool verified = false;
    if (gotCode) {
        verified = (exitCode == static_cast<DWORD>(expectedCode));
        if (verified) {
            cout << "Verification PASSED: child returned exit code " << exitCode
                 << " (expected " << expectedCode << ").\n";
            LOG_SYSCALL("VerifyExitCode", true, detail.c_str());
        } else {
            cout << "Verification FAILED: child returned exit code " << exitCode
                 << " but " << expectedCode << " was expected.\n";
            // The logger has no plain-message function, so we log the
            // verification as a pseudo call with a meaningful error code.
            SetLastError(ERROR_INVALID_DATA);
            LOG_SYSCALL("VerifyExitCode", false, detail.c_str());
        }
    }

    bool timesOk = reportChildTimes(info.hProcess, detail);
    bool closed = closeChildHandlesLogged(info, detail);   // ALWAYS close both

    return waitOk && gotCode && verified && timesOk && closed;
}

// Step 8: CreateProcess on a program that does not exist. It must fail with
// error 2 (file not found) or 3 (path not found).
bool demoMissingProgram() {
    cout << "\n-- Starting a program that does not exist --\n";
    const char* missingProgram = "C:\\this_program_does_not_exist.exe";
    string commandText = string("\"") + missingProgram + "\" --child 0";
    vector<char> commandBuffer(commandText.begin(), commandText.end());
    commandBuffer.push_back('\0');

    STARTUPINFOA startup;
    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info;
    ZeroMemory(&info, sizeof(info));

    cout << "CreateProcess on a missing program (this FAILURE is expected, error 2 or 3):\n";
    BOOL created = CreateProcessA(missingProgram, commandBuffer.data(), nullptr, nullptr,
                                  FALSE, 0, nullptr, nullptr, &startup, &info);
    DWORD errorCode = GetLastError();   // read before anything can change it
    LOG_SYSCALL("CreateProcess", created != 0, "C:\\this_program_does_not_exist.exe, expected to fail");

    if (created) {
        cout << "UNEXPECTED: CreateProcess started a program that should not exist!\n";
        TerminateProcess(info.hProcess, 1);
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
        return false;
    }
    if (errorCode != ERROR_FILE_NOT_FOUND && errorCode != ERROR_PATH_NOT_FOUND) {
        cout << "UNEXPECTED: it failed, but with error " << errorCode
             << " instead of 2 or 3.\n";
        return false;
    }
    return true;
}

// Returns true if every step worked and every verification passed.
bool runDemo(const string& exePath) {
    cout << "---------- PART 1: DEMO (logged, not timed) ----------\n";
    cout << "Parent process id: " << GetCurrentProcessId() << "\n";

    bool firstOk = demoOneChild(exePath, 42);
    bool secondOk = demoOneChild(exePath, 7);   // different code: proves args reach the child
    bool missingOk = demoMissingProgram();
    return firstOk && secondOk && missingOk;
}

// ----------------------------- PART 2: benchmarks ----------------------------
//
// NOTES FOR THE REPORT
//  (1) Windows has no fork(). CreateProcess creates the new process AND loads
//      the program in one call, so it corresponds to Linux fork + exec together.
//      CreateProcess_plus_Wait is the fair comparison with fork + exec + wait.
//  (2) Process creation takes milliseconds because Windows must create the
//      process object, map the program image and start the initial thread.
//      Windows Defender also scans every new process, so timings are noisy;
//      report the median.
//  (3) The CPU columns (cpu_us, cpu_per_call_us, cpu_cycles, cycles_per_call)
//      are measured for the PARENT process only, around the whole loop, and
//      include cleanup and timer overhead. The child's own CPU time is not
//      included. The demo prints one child's CPU time separately.
//  (4) Because each CreateProcess call is slow compared with timer resolution,
//      the median and the spread (stddev, max) are the useful columns here.
//  (5) The memory delta and page fault columns are parent-only and are not
//      expected to be meaningful for this category.
//  (6) The action also copies the command line into the writable buffer and
//      zeroes the STARTUPINFO. That is a few hundred nanoseconds, negligible
//      next to a millisecond-sized call.
//  (7) The warm-up runs the lambdas too, so failures there are counted as well.

// A. Benchmark: CreateProcess alone. The child runs while we return; the
// cleanup (untimed) waits for it, checks its exit code and closes both
// handles, so finished children never pile up and never overlap the next call.
bool benchmarkCreateProcess(const string& exePath) {
    FailureTracker failures;
    ChildCommand cmd = makeChildCommand(exePath, 0);
    PROCESS_INFORMATION info;
    ZeroMemory(&info, sizeof(info));
    bool launched = false;

    auto action = [&]() {
        BOOL ok = launchChild(cmd, info);
        launched = (ok != 0);
        if (!ok) {
            failures.record(true);
        }
    };

    auto cleanup = [&]() {
        if (!launched) {
            return;
        }
        bool finished = waitOrTerminate(info, failures);
        checkExitAndClose(info, finished, failures);   // exit code only if it finished
        launched = false;
    };

    metrics::Stats stats = metrics::benchmark("CreateProcess", action, kIterations, kWarmup, cleanup);
    return finishBenchmark(stats, failures);
}

// Setup for benchmark B: start all children, then wait until every one has
// exited. Logs the outcome. On any failure everything is killed and closed.
bool startAndFinishAllChildren(ChildCommand& cmd, vector<PROCESS_INFORMATION>& children) {
    // Built BEFORE the loops so no string work sits between a failing call
    // and the LOG_SYSCALL that reads its error code.
    string detail = "--child 0 x" + to_string(kTotalCalls);
    children.reserve(kTotalCalls);

    for (size_t i = 0; i < kTotalCalls; ++i) {
        PROCESS_INFORMATION info;
        ZeroMemory(&info, sizeof(info));
        if (!launchChild(cmd, info)) {
            LOG_SYSCALL("CreateProcess_setup", false, detail.c_str());
            terminateAndCloseAll(children);
            return false;
        }
        children.push_back(info);
    }
    LOG_SYSCALL("CreateProcess_setup", true, detail.c_str());

    // Untimed: wait for every child so all of them have already exited.
    FailureTracker waitFailures;
    for (size_t i = 0; i < children.size(); ++i) {
        waitOrTerminate(children[i], waitFailures);
    }
    if (waitFailures.count > 0) {
        SetLastError(waitFailures.firstError);
        LOG_SYSCALL("WaitForSingleObject_setup", false, detail.c_str());
        terminateAndCloseAll(children);
        return false;
    }
    LOG_SYSCALL("WaitForSingleObject_setup", true, detail.c_str());
    return true;
}

// B. Benchmark: WaitForSingleObject.
// Because the children have ALREADY exited, this measures the cost of the wait
// call itself on an already-signaled handle, NOT the lifetime of a child.
bool benchmarkWaitForSingleObject(const string& exePath) {
    FailureTracker failures;
    ChildCommand cmd = makeChildCommand(exePath, 0);

    vector<PROCESS_INFORMATION> children;
    if (!startAndFinishAllChildren(cmd, children)) {
        return false;
    }

    size_t nextIndex = 0;

    auto action = [&]() {
        if (nextIndex >= children.size()) {   // should never happen
            failures.record(false);
            return;
        }
        PROCESS_INFORMATION& child = children[nextIndex];
        ++nextIndex;
        waitOrTerminate(child, failures);   // returns at once: already signaled
    };

    // No cleanup: each handle is waited on exactly once by the action.
    metrics::Stats stats = metrics::benchmark("WaitForSingleObject", action, kIterations, kWarmup);

    // After the benchmark: all exit codes must be 0, then close every handle.
    for (size_t i = 0; i < children.size(); ++i) {
        checkExitAndClose(children[i], true, failures);
    }
    children.clear();

    return finishBenchmark(stats, failures);
}

// C. Benchmark: CreateProcess + WaitForSingleObject in one timed action.
// This is the full launch-and-finish cycle, comparable with Linux
// fork + exec + wait. The cleanup (untimed) checks the exit code and closes
// both handles.
bool benchmarkCreateProcessPlusWait(const string& exePath) {
    FailureTracker failures;
    ChildCommand cmd = makeChildCommand(exePath, 0);
    PROCESS_INFORMATION info;
    ZeroMemory(&info, sizeof(info));
    bool launched = false;
    bool finished = false;

    auto action = [&]() {
        finished = false;
        BOOL ok = launchChild(cmd, info);
        launched = (ok != 0);
        if (!ok) {
            failures.record(true);
            return;
        }
        finished = waitOrTerminate(info, failures);
    };

    auto cleanup = [&]() {
        if (!launched) {
            return;
        }
        checkExitAndClose(info, finished, failures);
        launched = false;
    };

    metrics::Stats stats = metrics::benchmark("CreateProcess_plus_Wait", action,
                                              kIterations, kWarmup, cleanup);
    return finishBenchmark(stats, failures);
}

// Reads the number of open handles of this process and logs the call.
bool readHandleCount(const char* when, DWORD& count) {
    BOOL ok = GetProcessHandleCount(GetCurrentProcess(), &count);
    LOG_SYSCALL("GetProcessHandleCount", ok != 0, when);
    return ok != 0;
}

// Runs all benchmarks plus the handle leak check. Returns how many problems.
int runAllBenchmarks(const string& exePath) {
    cout << "\n---------- PART 2: BENCHMARKS (timed, no logging inside) ----------\n";
    cout << "(Process creation is slow: this part can take from several seconds to a minute or more.)\n";
    int problems = 0;

    DWORD handlesBefore = 0;
    DWORD handlesAfter = 0;
    bool beforeOk = readHandleCount("before Part 2", handlesBefore);
    if (beforeOk) {
        cout << "Open handles before Part 2: " << handlesBefore << "\n";
    }

    if (!benchmarkCreateProcess(exePath))         ++problems;
    if (!benchmarkWaitForSingleObject(exePath))   ++problems;
    if (!benchmarkCreateProcessPlusWait(exePath)) ++problems;

    bool afterOk = readHandleCount("after Part 2", handlesAfter);
    if (afterOk) {
        cout << "Open handles after Part 2 : " << handlesAfter << "\n";
    }

    // Leak check: if we forgot to close hProcess or hThread of any child,
    // the count would have grown by hundreds.
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

    cout << "=== Windows Process: demo + benchmarks ===\n\n";

    string exePath;
    if (!getOwnPath(exePath)) {
        cout << "Cannot find the path of this program, so no child can be started.\n";
        return 1;
    }

    if (!runDemo(exePath)) {
        cout << "\nDemo FAILED, so the benchmarks were skipped.\n";
        return 1;
    }

    int problems = runAllBenchmarks(exePath);
    if (problems > 0) {
        cout << "\nFinished with " << problems << " benchmark problem(s). See warnings above.\n";
        return 2;
    }

    cout << "\nAll done. Results appended to results/Windows/windows_results.csv\n";
    return 0;
}

int main(int argc, char* argv[]) {
    // CHILD MODE: this MUST be the very first thing in main. The child is this
    // same program started with "--child <exitcode>". It must do NOTHING else:
    //   1. It must not open (and so overwrite) the transcript file.
    //   2. It must not initialise or write to the log file.
    //   3. It must never start children of its own (that would multiply
    //      processes without end and flood the machine).
    if (argc >= 2 && strcmp(argv[1], "--child") == 0) {
        return (argc >= 3) ? atoi(argv[2]) : 0;
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