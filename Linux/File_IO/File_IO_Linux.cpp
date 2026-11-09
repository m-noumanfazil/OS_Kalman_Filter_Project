// =============================================================================
// File_IO_Linux.cpp
//
// Part of an OS project comparing Linux and Windows system calls (Linux side,
// File I/O category). One-for-one port of File_IO_Windows.cpp.
//
// What this program does:
//   PART 1  Demo: open -> write -> close -> open -> read -> close, verifies the
//           data, and deliberately opens a missing file. Every call is logged
//           (not timed).
//   PART 2  Benchmarks: open, write (4 KB, 1 MB), read (4 KB, 1 MB) and close,
//           each timed separately (1000 iterations, 50 warm-up).
//           Results are printed and appended to results/Linux/linux_results.csv.
//   PART 3  Everything printed to the console is also saved to
//           Linux/File_IO/File_IO_Linux.txt (overwritten on every run).
//
// Name mapping for the report:
//   open = CreateFile, read = ReadFile, write = WriteFile, close = CloseHandle
//   lseek = SetFilePointerEx, unlink = DeleteFile
//
// Compile and run from the REPOSITORY ROOT:
//   g++ -std=c++17 Linux/File_IO/File_IO_Linux.cpp -o Linux/File_IO/file_io_linux
//   ./Linux/File_IO/file_io_linux
//
// Exit codes: 0 = everything OK, 1 = demo failed, 2 = a benchmark had problems.
// =============================================================================

#include "../../common/logger_linux.hpp"
#include "../../common/metrics_linux.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <streambuf>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>
using namespace std;

// ----------------------------- Settings --------------------------------------

// Test files are created in the current folder (the repository root) and
// deleted again at the end of the run.
const char* const kDemoFile       = "file_io_demo.txt";
const char* const kMissingFile    = "file_io_does_not_exist.txt";
const char* const kReadTestFile   = "file_io_bench_read.tmp";
const char* const kWriteTestFile  = "file_io_bench_write.tmp";
const char* const kTranscriptFile = "Linux/File_IO/File_IO_Linux.txt";

const int    kIterations = 1000;
const int    kWarmup     = 50;
// The warm-up runs the action too, so the close benchmark needs
// warm-up + iterations descriptors (1050).
const size_t kTotalCalls = kWarmup + kIterations;
const size_t kSize4KB    = 4 * 1024;
const size_t kSize1MB    = 1024 * 1024;

// Descriptor limit settings (see ensureDescriptorLimit).
const rlim_t kWantedSoftLimit  = 2048;
// 1050 test descriptors + stdin/stdout/stderr + transcript file + the
// descriptor used while counting /proc/self/fd + some spare room.
const size_t kDescriptorMargin = 32;

// A leak check allows this much growth (the Windows version allows 5 handles).
const int kDescriptorLeakTolerance = 2;

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

// Deterministic test data: A-Z repeated. Same bytes on every run, so results
// are reproducible and a read-back can be compared byte by byte.
vector<char> buildTestData(size_t size) {
    vector<char> data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<char>('A' + (i % 26));
    }
    return data;
}

// Returns true if read()/write() worked AND moved the expected number of bytes.
// In general read() and write() are ALLOWED to return fewer bytes than
// requested. But for a regular file in these tests a short count means
// something is wrong, so it is treated as a failure.
// A short count has no errno of its own, so we set EIO; otherwise the log line
// would show a stale, unrelated error.
bool transferSucceeded(ssize_t result, size_t expected) {
    if (result >= 0 && static_cast<size_t>(result) != expected) {
        errno = EIO;
        return false;
    }
    return result >= 0;
}

// Deletes a test file and logs it. A file that was never created is not
// reported, because that is not a real problem during cleanup.
void deleteTestFile(const char* path) {
    int result = unlink(path);
    if (result != 0 && errno == ENOENT) {
        return;
    }
    LOG_SYSCALL("unlink", result == 0, path);
}

// Counts failures inside benchmark lambdas. Lambdas must not log (logging
// would ruin the timing), so they only record here; the result is reported
// once, after the benchmark.
struct FailureTracker {
    int count = 0;
    int firstError = 0;

    // apiFailed = true : a system call returned -1, keep its errno.
    // apiFailed = false: the call "worked" but moved the wrong number of bytes.
    // Call this IMMEDIATELY after the failing call so errno is not overwritten.
    void record(bool apiFailed) {
        if (count == 0) {
            firstError = apiFailed ? errno : EIO;
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
        // Printing may have changed errno, so restore the saved one
        // right before logging.
        errno = failures.firstError;
        LOG_SYSCALL(logName.c_str(), false, detail.c_str());
        return false;
    }

    if (!metrics::appendStatsToCsv(stats)) {
        cout << "!!! WARNING: CSV row for " << stats.callName << " was NOT saved.\n";
        return false;
    }
    return true;
}

// Creates a 1 MB file of test data. The read benchmarks and the open / close
// benchmarks need an existing file with enough content.
bool createReadTestFile() {
    vector<char> data = buildTestData(kSize1MB);

    int fd = open(kReadTestFile, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    LOG_SYSCALL("open_setup", fd >= 0, kReadTestFile);
    if (fd < 0) {
        return false;
    }

    ssize_t written = write(fd, data.data(), kSize1MB);
    bool writeGood = transferSucceeded(written, kSize1MB);
    LOG_SYSCALL("write_setup", writeGood, kReadTestFile);

    int closeResult = close(fd);
    LOG_SYSCALL("close_setup", closeResult == 0, kReadTestFile);

    return writeGood && closeResult == 0;
}

// ----------------------------- Descriptor helpers (Linux only) ---------------

// Text for a limit value (RLIM_INFINITY means "no limit").
string limitToText(rlim_t value) {
    if (value == RLIM_INFINITY) {
        return "unlimited";
    }
    return to_string(static_cast<unsigned long long>(value));
}

// LINUX-SPECIFIC OBSERVATION (for the report):
// Every process has a limit on open file descriptors (RLIMIT_NOFILE). The
// default SOFT limit is usually 1024, but the close benchmark needs 1050
// descriptors open at the same time (50 warm-up + 1000 timed) plus the
// standard ones. Without a change, open() would fail with EMFILE
// ("Too many open files") halfway through the setup. Windows has no such
// small default limit, so the Windows program never had this problem.
// A process may raise its own SOFT limit up to the HARD limit without root,
// so we do exactly that. Returns true if at least `needed` descriptors are
// allowed afterwards. getrlimit/setrlimit are helper calls (not among the
// required calls).
bool ensureDescriptorLimit(size_t needed) {
    rlimit limits;
    int gotLimit = getrlimit(RLIMIT_NOFILE, &limits);
    LOG_SYSCALL("getrlimit", gotLimit == 0, "RLIMIT_NOFILE");
    if (gotLimit != 0) {
        return false;
    }
    cout << "Descriptor limit: soft = " << limitToText(limits.rlim_cur)
         << ", hard = " << limitToText(limits.rlim_max) << "\n";

    if (limits.rlim_cur < kWantedSoftLimit) {
        // Raise the soft limit to 2048, but never above the hard limit.
        rlim_t target = (limits.rlim_max < kWantedSoftLimit) ? limits.rlim_max : kWantedSoftLimit;
        if (target > limits.rlim_cur) {
            rlimit raised = limits;
            raised.rlim_cur = target;
            int setResult = setrlimit(RLIMIT_NOFILE, &raised);
            LOG_SYSCALL("setrlimit", setResult == 0, "raise soft limit of RLIMIT_NOFILE");
            if (setResult == 0) {
                limits = raised;
                cout << "Soft limit raised to " << limitToText(limits.rlim_cur) << "\n";
            }
        } else {
            cout << "The hard limit equals the soft limit, so it cannot be raised.\n";
        }
    }

    if (limits.rlim_cur != RLIM_INFINITY && limits.rlim_cur < needed) {
        cout << "!!! WARNING: only " << limitToText(limits.rlim_cur)
             << " descriptors are allowed but " << needed << " are needed.\n";
        errno = EMFILE;   // a limit is not a failing call, so give the log a meaningful code
        LOG_SYSCALL("DescriptorLimit", false, "too low for the close benchmark");
        return false;
    }
    return true;
}

// Counts the entries of /proc/self/fd (one per open descriptor). The
// directory stream itself uses one descriptor and shows up in the list. That
// is the same in every count, so it cancels out when two counts are compared.
// Returns -1 if /proc cannot be read.
int countOpenDescriptors(const char* when) {
    DIR* directory = opendir("/proc/self/fd");
    LOG_SYSCALL("opendir", directory != nullptr, when);
    if (directory == nullptr) {
        return -1;
    }
    int total = 0;
    dirent* entry = nullptr;
    while ((entry = readdir(directory)) != nullptr) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            ++total;
        }
    }
    closedir(directory);
    return total;
}

// ----------------------------- PART 1: demo ----------------------------------

// Logged walk-through of the five calls. Returns true if everything worked and
// the data read back is identical to the data written.
bool runDemo() {
    cout << "---------- PART 1: DEMO (logged, not timed) ----------\n";

    const string text =
        "Hello from the Linux File I/O demo.\n"
        "Second line: operating systems project.\n";
    const size_t textSize = text.size();

    // 1. Create the file (O_TRUNC empties an old one).
    int fd = open(kDemoFile, O_CREAT | O_RDWR | O_TRUNC, 0644);
    LOG_SYSCALL("open", fd >= 0, kDemoFile);
    if (fd < 0) {
        return false;
    }

    // 2. Write the known text and check how many bytes were really written.
    ssize_t written = write(fd, text.data(), textSize);
    bool writeGood = transferSucceeded(written, textSize);
    LOG_SYSCALL("write", writeGood, kDemoFile);

    // 3. Close the descriptor (even if the write failed, so we never leak it).
    int closeResult = close(fd);
    LOG_SYSCALL("close", closeResult == 0, kDemoFile);
    if (!writeGood || closeResult != 0) {
        return false;
    }

    // 4. Reopen read-only and read the content back.
    vector<char> readBuffer(textSize);
    fd = open(kDemoFile, O_RDONLY);
    LOG_SYSCALL("open", fd >= 0, kDemoFile);
    if (fd < 0) {
        return false;
    }

    ssize_t bytesRead = read(fd, readBuffer.data(), textSize);
    bool readGood = transferSucceeded(bytesRead, textSize);
    LOG_SYSCALL("read", readGood, kDemoFile);

    // 5. Close again.
    int closeResult2 = close(fd);
    LOG_SYSCALL("close", closeResult2 == 0, kDemoFile);
    if (!readGood || closeResult2 != 0) {
        return false;
    }

    // Verification: the bytes read must equal the bytes written.
    bool contentMatches = (memcmp(readBuffer.data(), text.data(), textSize) == 0);
    if (contentMatches) {
        cout << "Verification PASSED: " << bytesRead
             << " bytes read back and identical to what was written.\n";
        LOG_SYSCALL("VerifyContent", true, kDemoFile);
    } else {
        cout << "Verification FAILED: data read back differs from data written.\n";
        // The logger has no plain-message function, so we log the verification
        // as a pseudo call and give it a meaningful error code first.
        errno = EIO;
        LOG_SYSCALL("VerifyContent", false, kDemoFile);
    }

    // 6. Deliberate failure: this file does not exist, so the log must show
    //    one FAILED line with error 2 (ENOENT).
    cout << "Opening a file that does not exist (this FAILURE is expected):\n";
    int missing = open(kMissingFile, O_RDONLY);
    LOG_SYSCALL("open", missing >= 0, kMissingFile);
    if (missing >= 0) {
        close(missing);   // the file unexpectedly exists; do not leak the descriptor
    }

    return contentMatches;
}

// ----------------------------- PART 2: benchmarks ----------------------------
//
// NOTES FOR THE REPORT
//  (1) read: after the first read, the data sits in the Linux PAGE CACHE (RAM).
//      The read benchmarks therefore measure CACHED reads and system call
//      overhead, not disk speed.
//  (2) write: the file is opened without O_SYNC or O_DSYNC, so a write only
//      copies the data into the page cache. The kernel writes it to disk
//      later. These results do not include disk speed.
//  (3) The CPU columns (cpu_us, cpu_per_call_us) are measured around the WHOLE
//      loop and include the cleanup call and the timer overhead. Only
//      min/median/mean/max/stddev cover the action alone. The cpu_cycles
//      columns come from the TSC counter and are NOT comparable with the
//      Windows ones (see metrics_linux.hpp).
//  (4) The lambdas do not log. They only count failures (one compare and
//      branch, which is negligible compared with a system call).
//  (5) The warm-up runs the lambdas too, so failures there are counted as well.
//  (6) Name mapping: open = CreateFile, read = ReadFile, write = WriteFile,
//      close = CloseHandle.

// Benchmark: opening an existing file for reading.
bool benchmarkOpen() {
    FailureTracker failures;
    int fd = -1;

    auto action = [&]() {
        fd = open(kReadTestFile, O_RDONLY);
        if (fd < 0) {
            failures.record(true);
        }
    };

    // Outside the timed region: close the descriptor again so they do not pile up.
    auto cleanup = [&]() {
        if (fd >= 0) {
            if (close(fd) != 0) {
                failures.record(true);
            }
            fd = -1;
        }
    };

    metrics::Stats stats = metrics::benchmark("open", action, kIterations, kWarmup, cleanup);
    return finishBenchmark(stats, failures);
}

// Benchmark: writing `size` bytes to a file that is opened once.
bool benchmarkWrite(const char* callName, size_t size) {
    FailureTracker failures;
    vector<char> buffer = buildTestData(size);

    int fd = open(kWriteTestFile, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    LOG_SYSCALL("open_setup", fd >= 0, kWriteTestFile);
    if (fd < 0) {
        return false;
    }

    auto action = [&]() {
        ssize_t written = write(fd, buffer.data(), size);
        if (written < 0) {
            failures.record(true);
        } else if (static_cast<size_t>(written) != size) {
            failures.record(false);
        }
    };

    // Move the position back to the start so every iteration overwrites the
    // same region and the file does not grow to over 1 GB.
    auto cleanup = [&]() {
        if (lseek(fd, 0, SEEK_SET) == static_cast<off_t>(-1)) {
            failures.record(true);
        }
    };

    metrics::Stats stats = metrics::benchmark(callName, action, kIterations, kWarmup, cleanup);

    int closeResult = close(fd);
    LOG_SYSCALL("close_teardown", closeResult == 0, kWriteTestFile);

    bool clean = finishBenchmark(stats, failures);
    return clean && closeResult == 0;
}

// Benchmark: reading `size` bytes from a file that already holds 1 MB.
bool benchmarkRead(const char* callName, size_t size) {
    FailureTracker failures;
    vector<char> buffer(size);

    int fd = open(kReadTestFile, O_RDONLY);
    LOG_SYSCALL("open_setup", fd >= 0, kReadTestFile);
    if (fd < 0) {
        return false;
    }

    auto action = [&]() {
        ssize_t bytesRead = read(fd, buffer.data(), size);
        if (bytesRead < 0) {
            failures.record(true);
        } else if (static_cast<size_t>(bytesRead) != size) {
            failures.record(false);
        }
    };

    // Rewind so the next iteration reads the same data again (never hits EOF).
    auto cleanup = [&]() {
        if (lseek(fd, 0, SEEK_SET) == static_cast<off_t>(-1)) {
            failures.record(true);
        }
    };

    metrics::Stats stats = metrics::benchmark(callName, action, kIterations, kWarmup, cleanup);

    int closeResult = close(fd);
    LOG_SYSCALL("close_teardown", closeResult == 0, kReadTestFile);

    bool clean = finishBenchmark(stats, failures);
    return clean && closeResult == 0;
}

// Benchmark: closing a descriptor. The metrics helper has no per-iteration
// setup, so ALL descriptors are opened first (warm-up + iterations of them)
// and the action simply closes the next one in the list.
bool benchmarkClose() {
    // The default soft limit (1024) is too low for 1050 descriptors.
    if (!ensureDescriptorLimit(kTotalCalls + kDescriptorMargin)) {
        cout << "The close benchmark was SKIPPED because the descriptor limit is too low.\n"
             << "Fix: run  ulimit -n 4096  in your shell and start the program again.\n";
        return false;
    }

    FailureTracker failures;
    vector<int> descriptors;
    descriptors.reserve(kTotalCalls);

    for (size_t i = 0; i < kTotalCalls; ++i) {
        int fd = open(kReadTestFile, O_RDONLY);
        if (fd < 0) {
            LOG_SYSCALL("open_setup", false, kReadTestFile);
            for (size_t j = 0; j < descriptors.size(); ++j) {
                close(descriptors[j]);
            }
            return false;
        }
        descriptors.push_back(fd);
    }
    string setupDetail = string(kReadTestFile) + " x" + to_string(kTotalCalls);
    LOG_SYSCALL("open_setup", true, setupDetail.c_str());

    size_t nextIndex = 0;

    auto action = [&]() {
        if (nextIndex >= descriptors.size()) {   // should never happen
            failures.record(false);
            return;
        }
        int result = close(descriptors[nextIndex]);
        ++nextIndex;
        if (result != 0) {
            failures.record(true);
        }
    };

    // No cleanup is needed: each descriptor is closed exactly once by the action.
    metrics::Stats stats = metrics::benchmark("close", action, kIterations, kWarmup);

    // Safety net: close any descriptor the benchmark did not reach (normally none).
    for (size_t i = nextIndex; i < descriptors.size(); ++i) {
        close(descriptors[i]);
    }

    return finishBenchmark(stats, failures);
}

// Runs all benchmarks plus the descriptor leak check. Returns how many problems.
int runAllBenchmarks() {
    cout << "\n---------- PART 2: BENCHMARKS (timed, no logging inside) ----------\n";
    int problems = 0;

    int before = countOpenDescriptors("before Part 2");
    if (before >= 0) {
        cout << "Open descriptors before Part 2: " << before << "\n";
    }

    if (createReadTestFile()) {
        if (!benchmarkOpen())                      ++problems;
        if (!benchmarkWrite("write_4KB", kSize4KB)) ++problems;
        if (!benchmarkWrite("write_1MB", kSize1MB)) ++problems;
        if (!benchmarkRead("read_4KB", kSize4KB))   ++problems;
        if (!benchmarkRead("read_1MB", kSize1MB))   ++problems;
        if (!benchmarkClose())                     ++problems;
    } else {
        cout << "Could not create the read test file, so the benchmarks were skipped.\n";
        ++problems;
    }

    deleteTestFile(kReadTestFile);
    deleteTestFile(kWriteTestFile);

    int after = countOpenDescriptors("after Part 2");
    if (after >= 0) {
        cout << "Open descriptors after Part 2 : " << after << "\n";
    }

    // Leak check: a forgotten close() would grow the count by hundreds.
    if (before >= 0 && after >= 0) {
        if (after > before + kDescriptorLeakTolerance) {
            cout << "!!! WARNING: descriptors are LEAKING (" << (after - before)
                 << " more than before).\n";
            ++problems;
        } else {
            cout << "Descriptor leak check PASSED.\n";
        }
    } else {
        cout << "!!! WARNING: could not read /proc/self/fd, leak check skipped.\n";
        ++problems;
    }
    return problems;
}

// ----------------------------- Program flow ----------------------------------

int runAll() {
    logger::init();   // default path: logs/syscalls_linux.log

    cout << "=== Linux File I/O: demo + benchmarks ===\n\n";

    bool demoPassed = runDemo();
    deleteTestFile(kDemoFile);   // clean up before deciding the exit code
    if (!demoPassed) {
        cout << "\nDemo FAILED, so the benchmarks were skipped.\n";
        return 1;
    }

    int problems = runAllBenchmarks();
    if (problems > 0) {
        cout << "\nFinished with " << problems << " benchmark problem(s). See warnings above.\n";
        return 2;
    }

    cout << "\nAll done. Results appended to results/Linux/linux_results.csv\n";
    return 0;
}

int main() {
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