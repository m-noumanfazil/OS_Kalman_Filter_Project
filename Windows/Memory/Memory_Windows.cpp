// =============================================================================
// Memory_Windows.cpp
//
// Part of an OS project comparing Linux and Windows system calls (Windows side,
// Memory category).
//
// What this program does:
//   PART 1  Demo: GetSystemInfo (page size, allocation granularity), then
//           VirtualAlloc reserve -> commit -> write/verify pattern -> a
//           deliberate failing VirtualFree -> proper VirtualFree, and finally
//           the common one-call VirtualAlloc(MEM_RESERVE | MEM_COMMIT).
//           Every call is logged (not timed).
//   PART 2  Benchmarks (1000 iterations, 50 warm-up each), timed separately:
//             VirtualAlloc_4KB, VirtualAlloc_64KB, VirtualAlloc_1MB
//             FirstTouch_64KB   (page faults after VirtualAlloc, NOT a syscall)
//             VirtualFree_4KB, VirtualFree_64KB, VirtualFree_64KB_touched
//           Results are printed and appended to results/Windows/windows_results.csv.
//   PART 3  Everything printed to the console is also saved to
//           Windows/Memory/Memory_Windows.txt (overwritten on every run).
//
// Compile and run from the REPOSITORY ROOT:
//   g++ -std=c++17 Windows\Memory\Memory_Windows.cpp -o Windows\Memory\memory.exe -lpsapi
//   Windows\Memory\memory.exe
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
#include <exception>
#include <fstream>
#include <iostream>
#include <streambuf>
#include <string>
#include <vector>
using namespace std;

// ----------------------------- Settings --------------------------------------

const char* const kTranscriptFile = "Windows/Memory/Memory_Windows.txt";

const int    kIterations = 1000;
const int    kWarmup     = 50;
// The warm-up runs the action too, so every benchmark that consumes one
// prepared item per call needs warm-up + iterations items (1050).
const size_t kTotalCalls = kWarmup + kIterations;

const size_t kSize4KB  = 4 * 1024;
const size_t kSize64KB = 64 * 1024;
const size_t kSize1MB  = 1024 * 1024;

// Offset used for the deliberate failure in the demo.
const size_t kWrongOffset = 4096;

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

// Deterministic test data: A-Z repeated. Same bytes on every run, so the
// verification is reproducible.
vector<char> buildTestData(size_t size) {
    vector<char> data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<char>('A' + (i % 26));
    }
    return data;
}

// Counts failures inside benchmark lambdas. Lambdas must not log (logging
// would ruin the timing), so they only record here; the result is reported
// once, after the benchmark.
struct FailureTracker {
    int   count = 0;
    DWORD firstError = 0;

    // apiFailed = true : a Windows call returned failure, keep its GetLastError().
    // apiFailed = false: some other problem (for example a bad index).
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

// Size of one memory page on this machine (normally 4096 bytes).
size_t getPageSize() {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwPageSize;
}

// Writes one byte into every page of a block. The first write to a page makes
// Windows assign a physical page (a page fault). The pointer is volatile so
// the compiler cannot decide the writes are useless and remove them.
void touchEveryPage(void* block, size_t size, size_t pageSize) {
    volatile char* memory = static_cast<volatile char*>(block);
    for (size_t offset = 0; offset < size; offset += pageSize) {
        memory[offset] = 1;
    }
}

// Allocates `count` blocks of `size` bytes (reserve + commit, untouched) into
// `blocks`. If one allocation fails, the failure is logged, everything
// allocated so far is freed again, and false is returned (no leak, no crash).
bool allocateBlocks(vector<void*>& blocks, size_t count, size_t size) {
    // Built BEFORE the loop so no string work happens between a failing
    // VirtualAlloc and the LOG_SYSCALL that reads its error code.
    string detail = to_string(size / 1024) + " KB x " + to_string(count);
    blocks.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        void* block = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (block == nullptr) {
            LOG_SYSCALL("VirtualAlloc_setup", false, detail.c_str());
            
            // Free what we already have.
            for (size_t j = 0; j < blocks.size(); ++j) {
                VirtualFree(blocks[j], 0, MEM_RELEASE);
            }
            blocks.clear();
            return false;
        }
        blocks.push_back(block);
    }
    LOG_SYSCALL("VirtualAlloc_setup", true, detail.c_str());
    return true;
}

// Frees blocks[startIndex ... end] and empties the vector. Blocks before
// startIndex were already freed by the benchmark itself. Logs one teardown
// line. Returns true if every VirtualFree worked.
bool releaseBlocks(vector<void*>& blocks, size_t startIndex) {
    int failed = 0;
    DWORD firstError = 0;
    for (size_t i = startIndex; i < blocks.size(); ++i) {
        if (blocks[i] != nullptr && !VirtualFree(blocks[i], 0, MEM_RELEASE)) {
            if (failed == 0) {
                firstError = GetLastError();
            }
            ++failed;
        }
    }
    size_t remaining = (blocks.size() > startIndex) ? blocks.size() - startIndex : 0;
    string detail = to_string(remaining) + " block(s)";
    blocks.clear();

    if (failed > 0) {
        SetLastError(firstError);   // restore it, string work may have changed it
    }
    LOG_SYSCALL("VirtualFree_teardown", failed == 0, detail.c_str());
    return failed == 0;
}

// ----------------------------- PART 1: demo ----------------------------------

// Page size vs allocation granularity: an OS-specific fact for the report.
void printSystemInfo() {
    SYSTEM_INFO info;
    GetSystemInfo(&info);   // returns void, so it cannot fail
    LOG_SYSCALL("GetSystemInfo", true, "page size and allocation granularity");

    cout << "Page size              : " << info.dwPageSize << " bytes ("
         << info.dwPageSize / 1024 << " KB)\n";
    cout << "Allocation granularity : " << info.dwAllocationGranularity << " bytes ("
         << info.dwAllocationGranularity / 1024 << " KB)\n";
    cout << "Windows RESERVES address space in allocation-granularity units (normally 64 KB)\n"
         << "but COMMITS memory in page units (normally 4 KB). So a reserved range always\n"
         << "starts on a 64 KB boundary, and even a 4 KB request uses a 64 KB address range.\n";
}

// Writes the A-Z pattern across the whole block, reads it back and compares.
// Prints and logs PASSED or FAILED. Returns true if the data matches.
bool writeAndVerifyPattern(void* block, size_t size, const char* detail) {
    vector<char> pattern = buildTestData(size);
    volatile char* memory = static_cast<volatile char*>(block);

    for (size_t i = 0; i < size; ++i) {
        memory[i] = pattern[i];
    }

    bool matches = true;
    for (size_t i = 0; i < size; ++i) {
        if (memory[i] != pattern[i]) {
            matches = false;
            break;
        }
    }

    if (matches) {
        cout << "Verification PASSED: " << size
             << " bytes written, read back and identical.\n";
        LOG_SYSCALL("VerifyContent", true, detail);
    } else {
        cout << "Verification FAILED: data read back differs from data written.\n";
        // The logger has no plain-message function, so we log the verification
        // as a pseudo call and give it a meaningful error code first.
        SetLastError(ERROR_INVALID_DATA);
        LOG_SYSCALL("VerifyContent", false, detail);
    }
    return matches;
}

// Releases a whole allocation and logs it.
bool freeAndLog(void* base, const char* detail) {
    BOOL ok = VirtualFree(base, 0, MEM_RELEASE);
    LOG_SYSCALL("VirtualFree", ok != 0, detail);
    return ok != 0;
}

// Two-step allocation: reserve address space, then commit it.
bool demoReserveCommit() {
    cout << "\n-- Reserve, then commit, a 1 MB block --\n";

    // Reserve: only address space is claimed. The memory cannot be used yet.
    void* base = VirtualAlloc(nullptr, kSize1MB, MEM_RESERVE, PAGE_NOACCESS);
    LOG_SYSCALL("VirtualAlloc", base != nullptr, "reserve 1 MB, PAGE_NOACCESS");
    if (base == nullptr) {
        return false;
    }

    // Commit: the same range becomes usable (readable and writable).
    void* committed = VirtualAlloc(base, kSize1MB, MEM_COMMIT, PAGE_READWRITE);
    LOG_SYSCALL("VirtualAlloc", committed != nullptr, "commit 1 MB, PAGE_READWRITE");
    if (committed == nullptr) {
        freeAndLog(base, "release reserved 1 MB block");   // do not leak the reservation
        return false;
    }

    bool demoOk = writeAndVerifyPattern(base, kSize1MB, "1 MB block");

    // Deliberate failure: MEM_RELEASE only accepts the START of an allocation,
    // so base + 4096 must be refused with error 487 (ERROR_INVALID_ADDRESS).
    cout << "Releasing base + " << kWrongOffset
         << " (this FAILURE is expected, error 487):\n";
    void* notTheBase = static_cast<char*>(base) + kWrongOffset;
    BOOL wrongFree = VirtualFree(notTheBase, 0, MEM_RELEASE);
    LOG_SYSCALL("VirtualFree", wrongFree != 0, "base+4096, expected to fail");
    if (wrongFree) {
        cout << "UNEXPECTED: VirtualFree accepted an address that is not the allocation base!\n";
        demoOk = false;
    }

    bool freed = freeAndLog(base, "release 1 MB block");
    return demoOk && freed;
}

// The common one-call form: reserve and commit together.
bool demoOneCallAlloc() {
    cout << "\n-- One-call VirtualAlloc (MEM_RESERVE | MEM_COMMIT), 64 KB --\n";

    void* block = VirtualAlloc(nullptr, kSize64KB, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    LOG_SYSCALL("VirtualAlloc", block != nullptr, "reserve+commit 64 KB in one call");
    if (block == nullptr) {
        return false;
    }

    bool writeOk = writeAndVerifyPattern(block, kSize64KB, "64 KB block");
    bool freed = freeAndLog(block, "release 64 KB block");
    return writeOk && freed;
}

// Returns true if everything worked and the data read back was identical.
bool runDemo() {
    cout << "---------- PART 1: DEMO (logged, not timed) ----------\n";
    printSystemInfo();
    bool firstPartOk = demoReserveCommit();
    bool secondPartOk = demoOneCallAlloc();
    return firstPartOk && secondPartOk;
}

// ----------------------------- PART 2: benchmarks ----------------------------
//
// NOTES FOR THE REPORT
//  (1) VirtualAlloc with MEM_COMMIT only reserves the address range and charges
//      the system commit limit. Windows does not assign physical pages until the
//      memory is first touched, so VirtualAlloc itself shows ~0 page faults.
//  (2) Address space is reserved in allocation-granularity units (64 KB), so a
//      4 KB request still consumes a 64 KB address range (only 4 KB is committed).
//  (3) The CPU columns (cpu_us, cpu_per_call_us, cpu_cycles, cycles_per_call)
//      are measured around the WHOLE loop, so they include the cleanup call and
//      the timer overhead. Only min/median/mean/max/stddev cover the action alone.
//  (4) The memory delta and page fault columns are loop-level totals, so only
//      FirstTouch_64KB (positive) and VirtualFree_64KB_touched (memory delta
//      negative) are expected to show clearly non-zero values.
//  (5) The warm-up runs the lambdas too, so failures there are counted as well.
//
// MEMORY LIMIT: the biggest benchmarks hold 1050 x 64 KB = about 67 MB at once.
// Each benchmark frees everything it allocated before the next one starts.

// A. Benchmark: VirtualAlloc (reserve + commit, memory NOT touched).
// The cleanup frees the block outside the timed region.
bool benchmarkVirtualAlloc(const char* callName, size_t size) {
    FailureTracker failures;
    void* block = nullptr;

    auto action = [&]() {
        block = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (block == nullptr) {
            failures.record(true);
        }
    };

    auto cleanup = [&]() {
        if (block != nullptr) {
            if (!VirtualFree(block, 0, MEM_RELEASE)) {
                failures.record(true);
            }
            block = nullptr;
        }
    };

    metrics::Stats stats = metrics::benchmark(callName, action, kIterations, kWarmup, cleanup);
    return finishBenchmark(stats, failures);
}

// B. Benchmark: FirstTouch_64KB.
// IMPORTANT: this is NOT a system call. It measures the cost of the PAGE FAULTS
// that happen when memory returned by VirtualAlloc is used for the first time
// (16 pages of 4 KB per 64 KB block). This is the benchmark where the memory
// delta and page fault columns become meaningful.
// All blocks are allocated beforehand (untouched); each call touches the next one.
bool benchmarkFirstTouch() {
    FailureTracker failures;
    const size_t size = kSize64KB;
    const size_t pageSize = getPageSize();

    vector<void*> blocks;
    if (!allocateBlocks(blocks, kTotalCalls, size)) {
        return false;
    }

    size_t nextIndex = 0;

    // Touching valid committed memory has no return value to check; a real
    // problem would crash the program, and allocation was already checked.
    auto action = [&]() {
        if (nextIndex >= blocks.size()) {   // should never happen
            failures.record(false);
            return;
        }
        void* block = blocks[nextIndex];
        ++nextIndex;
        touchEveryPage(block, size, pageSize);
    };

    metrics::Stats stats = metrics::benchmark("FirstTouch_64KB", action, kIterations, kWarmup);

    bool teardownOk = releaseBlocks(blocks, 0);   // free all, touched or not
    bool clean = finishBenchmark(stats, failures);
    return clean && teardownOk;
}

// C and D. Benchmark: VirtualFree of blocks prepared beforehand.
// touchFirst = false: blocks are untouched, so Windows only drops address space
//                     and commit charge.
// touchFirst = true : every page is written before the benchmark, so Windows
//                     must also release real physical pages (slower).
// The action frees the next block; no cleanup is needed.
bool benchmarkVirtualFree(const char* callName, size_t size, bool touchFirst) {
    FailureTracker failures;

    vector<void*> blocks;
    if (!allocateBlocks(blocks, kTotalCalls, size)) {
        return false;
    }

    if (touchFirst) {
        // Setup only: this is outside the timed region.
        size_t pageSize = getPageSize();
        for (size_t i = 0; i < blocks.size(); ++i) {
            touchEveryPage(blocks[i], size, pageSize);
        }
    }

    size_t nextIndex = 0;

    auto action = [&]() {
        if (nextIndex >= blocks.size()) {   // should never happen
            failures.record(false);
            return;
        }
        BOOL ok = VirtualFree(blocks[nextIndex], 0, MEM_RELEASE);
        ++nextIndex;
        if (!ok) {
            failures.record(true);
        }
    };

    metrics::Stats stats = metrics::benchmark(callName, action, kIterations, kWarmup);

    // Safety net: free any block the benchmark did not reach (normally none).
    bool teardownOk = releaseBlocks(blocks, nextIndex);
    bool clean = finishBenchmark(stats, failures);
    return clean && teardownOk;
}

// Runs all benchmarks. Returns how many of them had problems.
int runAllBenchmarks() {
    cout << "\n---------- PART 2: BENCHMARKS (timed, no logging inside) ----------\n";
    int problems = 0;

    if (!benchmarkVirtualAlloc("VirtualAlloc_4KB", kSize4KB))   ++problems;
    if (!benchmarkVirtualAlloc("VirtualAlloc_64KB", kSize64KB)) ++problems;
    if (!benchmarkVirtualAlloc("VirtualAlloc_1MB", kSize1MB))   ++problems;

    if (!benchmarkFirstTouch()) ++problems;

    if (!benchmarkVirtualFree("VirtualFree_4KB", kSize4KB, false))           ++problems;
    if (!benchmarkVirtualFree("VirtualFree_64KB", kSize64KB, false))         ++problems;
    if (!benchmarkVirtualFree("VirtualFree_64KB_touched", kSize64KB, true))  ++problems;

    return problems;
}

// ----------------------------- Program flow ----------------------------------

int runAll() {
    logger::init();   // default path: logs/syscalls_windows.log

    cout << "=== Windows Memory: demo + benchmarks ===\n\n";

    if (!runDemo()) {
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