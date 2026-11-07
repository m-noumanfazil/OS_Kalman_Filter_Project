// =============================================================================
// File_IO_Windows.cpp
//
// Part of an OS project comparing Linux and Windows system calls (Windows side,
// File I/O category).
//
// What this program does:
//   PART 1  Demo: CreateFile -> WriteFile -> CloseHandle -> CreateFile ->
//           ReadFile -> CloseHandle, verifies the data, and deliberately opens
//           a missing file. Every call is logged (not timed).
//   PART 2  Benchmarks: CreateFile, WriteFile (4 KB, 1 MB), ReadFile (4 KB, 1 MB)
//           and CloseHandle, each timed separately (1000 iterations, 50 warm-up).
//           Results are printed and appended to results/Windows/windows_results.csv.
//   PART 3  Everything printed to the console is also saved to
//           Windows/File_IO/File_IO_Windows.txt (overwritten on every run).
//
// Compile and run from the REPOSITORY ROOT:
//   g++ -std=c++17 Windows\File_IO\File_IO_Windows.cpp -o Windows\File_IO\file_io.exe -lpsapi
//   Windows\File_IO\file_io.exe
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
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <streambuf>
#include <string>
#include <vector>

// ----------------------------- Settings --------------------------------------

// Test files are created in the current folder (the repository root) and
// deleted again at the end of the run.
const char* const kDemoFile       = "file_io_demo.txt";
const char* const kMissingFile    = "file_io_does_not_exist.txt";
const char* const kReadTestFile   = "file_io_bench_read.tmp";
const char* const kWriteTestFile  = "file_io_bench_write.tmp";
const char* const kTranscriptFile = "Windows/File_IO/File_IO_Windows.txt";

const int   kIterations = 1000;
const int   kWarmup     = 50;
const DWORD kSize4KB    = 4 * 1024;
const DWORD kSize1MB    = 1024 * 1024;

// ----------------------------- Tee stream buffer (PART 3) --------------------

// Sends every character to two stream buffers: the original console buffer
// and the text file. Installed into std::cout so that all normal output,
// including the logger's lines, is copied to the file automatically.
class TeeBuffer : public std::streambuf {
public:
    TeeBuffer(std::streambuf* first, std::streambuf* second)
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
    std::streamsize xsputn(const char_type* text, std::streamsize count) override {
        std::streamsize written1 = first_->sputn(text, count);
        std::streamsize written2 = second_->sputn(text, count);
        return std::min(written1, written2);
    }

    // std::endl and flush() end up here: flush both destinations.
    int sync() override {
        int r1 = first_->pubsync();
        int r2 = second_->pubsync();
        return (r1 == 0 && r2 == 0) ? 0 : -1;
    }

private:
    std::streambuf* first_;
    std::streambuf* second_;
};

// ----------------------------- Small helpers ---------------------------------

// Deterministic test data: A-Z repeated. Same bytes on every run, so results
// are reproducible and a read-back can be compared byte by byte.
std::vector<char> buildTestData(size_t size) {
    std::vector<char> data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<char>('A' + (i % 26));
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

// Deletes a test file and logs it. A file that was never created is not
// reported, because that is not a real problem during cleanup.
void deleteTestFile(const char* path) {
    BOOL ok = DeleteFileA(path);
    if (!ok && GetLastError() == ERROR_FILE_NOT_FOUND) {
        return;
    }
    LOG_SYSCALL("DeleteFile", ok != 0, path);
}

// Counts failures inside benchmark lambdas. Lambdas must not log (logging
// would ruin the timing), so they only record here; the result is reported
// once, after the benchmark.
struct FailureTracker {
    int   count = 0;
    DWORD firstError = 0;

    // apiFailed = true : a Windows call returned failure, keep its GetLastError().
    // apiFailed = false: the call "worked" but moved the wrong number of bytes.
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
        std::string logName = stats.callName + " benchmark";
        std::string detail = std::to_string(failures.count) +
                             " failed call(s) in warm-up + timed loop";
        std::cout << "!!! WARNING: " << stats.callName << " had " << failures.count
                  << " failed call(s). Results are NOT valid and were NOT saved to the CSV.\n";
        // Printing may have changed the last error, so restore the saved one
        // right before logging.
        SetLastError(failures.firstError);
        LOG_SYSCALL(logName.c_str(), false, detail.c_str());
        return false;
    }

    if (!metrics::appendStatsToCsv(stats)) {
        std::cout << "!!! WARNING: CSV row for " << stats.callName << " was NOT saved.\n";
        return false;
    }
    return true;
}

// Creates a 1 MB file of test data. The read benchmarks and the CreateFile /
// CloseHandle benchmarks need an existing file with enough content.
bool createReadTestFile() {
    std::vector<char> data = buildTestData(kSize1MB);

    HANDLE handle = CreateFileA(kReadTestFile, GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    LOG_SYSCALL("CreateFile_setup", handle != INVALID_HANDLE_VALUE, kReadTestFile);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD bytesWritten = 0;
    BOOL writeOk = WriteFile(handle, data.data(), kSize1MB, &bytesWritten, nullptr);
    bool writeGood = transferSucceeded(writeOk, bytesWritten, kSize1MB);
    LOG_SYSCALL("WriteFile_setup", writeGood, kReadTestFile);

    BOOL closeOk = CloseHandle(handle);
    LOG_SYSCALL("CloseHandle_setup", closeOk != 0, kReadTestFile);

    return writeGood && closeOk;
}

// ----------------------------- PART 1: demo ----------------------------------

// Logged walk-through of the five calls. Returns true if everything worked and
// the data read back is identical to the data written.
bool runDemo() {
    std::cout << "---------- PART 1: DEMO (logged, not timed) ----------\n";

    const std::string text =
        "Hello from the Windows File I/O demo.\n"
        "Second line: operating systems project.\n";
    const DWORD textSize = static_cast<DWORD>(text.size());

    // 1. Create the file (CREATE_ALWAYS overwrites an old one).
    HANDLE handle = CreateFileA(kDemoFile, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    LOG_SYSCALL("CreateFile", handle != INVALID_HANDLE_VALUE, kDemoFile);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    // 2. Write the known text and check how many bytes were really written.
    DWORD bytesWritten = 0;
    BOOL writeOk = WriteFile(handle, text.data(), textSize, &bytesWritten, nullptr);
    bool writeGood = transferSucceeded(writeOk, bytesWritten, textSize);
    LOG_SYSCALL("WriteFile", writeGood, kDemoFile);

    // 3. Close the handle (even if the write failed, so we never leak it).
    BOOL closeOk = CloseHandle(handle);
    LOG_SYSCALL("CloseHandle", closeOk != 0, kDemoFile);
    if (!writeGood || !closeOk) {
        return false;
    }

    // 4. Reopen read-only and read the content back.
    std::vector<char> readBuffer(textSize);
    handle = CreateFileA(kDemoFile, GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    LOG_SYSCALL("CreateFile", handle != INVALID_HANDLE_VALUE, kDemoFile);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD bytesRead = 0;
    BOOL readOk = ReadFile(handle, readBuffer.data(), textSize, &bytesRead, nullptr);
    bool readGood = transferSucceeded(readOk, bytesRead, textSize);
    LOG_SYSCALL("ReadFile", readGood, kDemoFile);

    // 5. Close again.
    BOOL closeOk2 = CloseHandle(handle);
    LOG_SYSCALL("CloseHandle", closeOk2 != 0, kDemoFile);
    if (!readGood || !closeOk2) {
        return false;
    }

    // Verification: the bytes read must equal the bytes written.
    bool contentMatches = (std::memcmp(readBuffer.data(), text.data(), textSize) == 0);
    if (contentMatches) {
        std::cout << "Verification PASSED: " << bytesRead
                  << " bytes read back and identical to what was written.\n";
        LOG_SYSCALL("VerifyContent", true, kDemoFile);
    } else {
        std::cout << "Verification FAILED: data read back differs from data written.\n";
        // The logger has no plain-message function, so we log the verification
        // as a pseudo call and give it a meaningful error code first.
        SetLastError(ERROR_INVALID_DATA);
        LOG_SYSCALL("VerifyContent", false, kDemoFile);
    }

    // 6. Deliberate failure: this file does not exist, so the log must show
    //    one FAILED line with error 2 (file not found).
    std::cout << "Opening a file that does not exist (this FAILURE is expected):\n";
    HANDLE missing = CreateFileA(kMissingFile, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    LOG_SYSCALL("CreateFile", missing != INVALID_HANDLE_VALUE, kMissingFile);
    if (missing != INVALID_HANDLE_VALUE) {
        CloseHandle(missing);   // the file unexpectedly exists; do not leak the handle
    }

    return contentMatches;
}

// ----------------------------- PART 2: benchmarks ----------------------------
//
// NOTES FOR THE REPORT
//  (1) ReadFile: after the first read, the data sits in the Windows file cache
//      (RAM). The read benchmarks therefore measure CACHED reads and system call
//      overhead, not physical disk speed.
//  (2) WriteFile: the file is opened without FILE_FLAG_WRITE_THROUGH, so a write
//      only copies the data into the cache. Windows writes it to the physical
//      disk later. These results do not include disk speed.
//  (3) CPU columns (cpu_us, cpu_per_call_us, cpu_cycles, cycles_per_call) are
//      measured around the WHOLE loop, so they include the cleanup call and the
//      timer overhead. Only min/median/mean/max/stddev cover the action alone.
//  (4) The lambdas do not log. They only count failures (one compare and branch,
//      which is negligible compared with a system call).
//  (5) The warm-up runs the lambdas too, so failures there are counted as well.

// Benchmark: opening an existing file for reading.
bool benchmarkCreateFile() {
    FailureTracker failures;
    HANDLE handle = INVALID_HANDLE_VALUE;

    auto action = [&]() {
        handle = CreateFileA(kReadTestFile, GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            failures.record(true);
        }
    };

    // Outside the timed region: close the handle again so handles do not pile up.
    auto cleanup = [&]() {
        if (handle != INVALID_HANDLE_VALUE) {
            if (!CloseHandle(handle)) {
                failures.record(true);
            }
            handle = INVALID_HANDLE_VALUE;
        }
    };

    metrics::Stats stats = metrics::benchmark("CreateFile", action, kIterations, kWarmup, cleanup);
    return finishBenchmark(stats, failures);
}

// Benchmark: writing `size` bytes to a file that is opened once.
bool benchmarkWriteFile(const char* callName, DWORD size) {
    FailureTracker failures;
    std::vector<char> buffer = buildTestData(size);

    HANDLE handle = CreateFileA(kWriteTestFile, GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    LOG_SYSCALL("CreateFile_setup", handle != INVALID_HANDLE_VALUE, kWriteTestFile);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER fileStart;
    fileStart.QuadPart = 0;

    auto action = [&]() {
        DWORD bytesWritten = 0;
        BOOL ok = WriteFile(handle, buffer.data(), size, &bytesWritten, nullptr);
        if (!ok) {
            failures.record(true);
        } else if (bytesWritten != size) {
            failures.record(false);
        }
    };

    // Move the file pointer back to the start so every iteration overwrites the
    // same region and the file does not grow to over 1 GB.
    auto cleanup = [&]() {
        if (!SetFilePointerEx(handle, fileStart, nullptr, FILE_BEGIN)) {
            failures.record(true);
        }
    };

    metrics::Stats stats = metrics::benchmark(callName, action, kIterations, kWarmup, cleanup);

    BOOL closeOk = CloseHandle(handle);
    LOG_SYSCALL("CloseHandle_teardown", closeOk != 0, kWriteTestFile);

    bool clean = finishBenchmark(stats, failures);
    return clean && closeOk;
}

// Benchmark: reading `size` bytes from a file that already holds 1 MB.
bool benchmarkReadFile(const char* callName, DWORD size) {
    FailureTracker failures;
    std::vector<char> buffer(size);

    HANDLE handle = CreateFileA(kReadTestFile, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    LOG_SYSCALL("CreateFile_setup", handle != INVALID_HANDLE_VALUE, kReadTestFile);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER fileStart;
    fileStart.QuadPart = 0;

    auto action = [&]() {
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(handle, buffer.data(), size, &bytesRead, nullptr);
        if (!ok) {
            failures.record(true);
        } else if (bytesRead != size) {
            failures.record(false);
        }
    };

    // Rewind so the next iteration reads the same data again (never hits EOF).
    auto cleanup = [&]() {
        if (!SetFilePointerEx(handle, fileStart, nullptr, FILE_BEGIN)) {
            failures.record(true);
        }
    };

    metrics::Stats stats = metrics::benchmark(callName, action, kIterations, kWarmup, cleanup);

    BOOL closeOk = CloseHandle(handle);
    LOG_SYSCALL("CloseHandle_teardown", closeOk != 0, kReadTestFile);

    bool clean = finishBenchmark(stats, failures);
    return clean && closeOk;
}

// Benchmark: closing a handle. The metrics helper has no per-iteration setup,
// so ALL handles are opened first (warm-up + iterations of them) and the action
// simply closes the next one in the list.
bool benchmarkCloseHandle() {
    const int totalCalls = kWarmup + kIterations;   // the warm-up closes handles too
    FailureTracker failures;
    std::vector<HANDLE> handles;
    handles.reserve(totalCalls);

    // FILE_SHARE_READ | FILE_SHARE_WRITE lets many handles to one file coexist.
    for (int i = 0; i < totalCalls; ++i) {
        HANDLE handle = CreateFileA(kReadTestFile, GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            LOG_SYSCALL("CreateFile_setup", false, kReadTestFile);
            for (size_t j = 0; j < handles.size(); ++j) {
                CloseHandle(handles[j]);
            }
            return false;
        }
        handles.push_back(handle);
    }
    std::string setupDetail = std::string(kReadTestFile) + " x" + std::to_string(totalCalls);
    LOG_SYSCALL("CreateFile_setup", true, setupDetail.c_str());

    size_t nextIndex = 0;

    auto action = [&]() {
        if (nextIndex >= handles.size()) {   // should never happen
            failures.record(false);
            return;
        }
        BOOL ok = CloseHandle(handles[nextIndex]);
        ++nextIndex;
        if (!ok) {
            failures.record(true);
        }
    };

    // No cleanup is needed: each handle is closed exactly once by the action.
    metrics::Stats stats = metrics::benchmark("CloseHandle", action, kIterations, kWarmup);

    // Safety net: close any handle the benchmark did not reach (normally none).
    for (size_t i = nextIndex; i < handles.size(); ++i) {
        CloseHandle(handles[i]);
    }

    return finishBenchmark(stats, failures);
}

// Runs all benchmarks. Returns how many of them had problems.
int runAllBenchmarks() {
    std::cout << "\n---------- PART 2: BENCHMARKS (timed, no logging inside) ----------\n";
    int problems = 0;

    if (createReadTestFile()) {
        if (!benchmarkCreateFile())                       ++problems;
        if (!benchmarkWriteFile("WriteFile_4KB", kSize4KB)) ++problems;
        if (!benchmarkWriteFile("WriteFile_1MB", kSize1MB)) ++problems;
        if (!benchmarkReadFile("ReadFile_4KB", kSize4KB))   ++problems;
        if (!benchmarkReadFile("ReadFile_1MB", kSize1MB))   ++problems;
        if (!benchmarkCloseHandle())                      ++problems;
    } else {
        std::cout << "Could not create the read test file, so the benchmarks were skipped.\n";
        ++problems;
    }

    deleteTestFile(kReadTestFile);
    deleteTestFile(kWriteTestFile);
    return problems;
}

// ----------------------------- Program flow ----------------------------------

int runAll() {
    logger::init();   // default path: logs/syscalls_windows.log

    std::cout << "=== Windows File I/O: demo + benchmarks ===\n\n";

    bool demoPassed = runDemo();
    deleteTestFile(kDemoFile);   // clean up before deciding the exit code
    if (!demoPassed) {
        std::cout << "\nDemo FAILED, so the benchmarks were skipped.\n";
        return 1;
    }

    int problems = runAllBenchmarks();
    if (problems > 0) {
        std::cout << "\nFinished with " << problems << " benchmark problem(s). See warnings above.\n";
        return 2;
    }

    std::cout << "\nAll done. Results appended to results/Windows/windows_results.csv\n";
    return 0;
}

int main() {
    // PART 3: copy everything written to std::cout into a text file as well.
    std::ofstream transcript(kTranscriptFile, std::ios::out | std::ios::trunc);
    std::streambuf* originalBuffer = std::cout.rdbuf();
    TeeBuffer tee(originalBuffer, transcript.rdbuf());

    bool teeActive = false;
    if (transcript.is_open()) {
        std::cout.rdbuf(&tee);
        teeActive = true;
        std::cout << "Console output is also being saved to " << kTranscriptFile << "\n";
    } else {
        std::cerr << "Warning: cannot open " << kTranscriptFile
                  << " (run from the repository root). Continuing without the text file.\n";
    }

    int exitCode = 1;
    try {
        exitCode = runAll();
    } catch (const std::exception& error) {
        std::cout << "Unexpected error: " << error.what() << std::endl;
    }

    // Restore the original buffer BEFORE the tee and the file are destroyed.
    // Otherwise std::cout would point to a dead object and crash at exit.
    std::cout.flush();
    if (teeActive) {
        std::cout.rdbuf(originalBuffer);
    }
    return exitCode;
}