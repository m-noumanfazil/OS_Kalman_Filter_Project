// =============================================================================
// Memory_Linux.cpp
//
// Part of an OS project comparing Linux and Windows system calls (Linux side,
// Memory category). One-for-one port of Memory_Windows.cpp.
//
// What this program does:
//   PART 1  Demo: sysconf (page size), then mmap PROT_NONE ("reserve") ->
//           mprotect ("make usable") -> write/verify pattern -> a deliberate
//           failing munmap -> proper munmap, and finally the common one-call
//           mmap(PROT_READ | PROT_WRITE). Every call is logged (not timed).
//   PART 2  Benchmarks (1000 iterations, 50 warm-up each), timed separately:
//             mmap_4KB, mmap_64KB, mmap_1MB
//             FirstTouch_64KB   (page faults after mmap, NOT a syscall)
//             munmap_4KB, munmap_64KB, munmap_64KB_touched
//           Results are printed and appended to results/Linux/linux_results.csv.
//   PART 3  Everything printed to the console is also saved to
//           Linux/Memory/Memory_Linux.txt (overwritten on every run).
//
// Windows call -> Linux call (name mapping for the report):
//   +-------------------------------------------------+---------------------------+
//   | Windows                                         | Linux                     |
//   +-------------------------------------------------+---------------------------+
//   | VirtualAlloc(MEM_RESERVE | MEM_COMMIT, RW)      | mmap(PROT_READ|PROT_WRITE,|
//   |                                                 |   MAP_PRIVATE|MAP_ANON.)  |
//   | VirtualAlloc(MEM_RESERVE, PAGE_NOACCESS)        | mmap(PROT_NONE, ...)      |
//   | VirtualAlloc(base, MEM_COMMIT, PAGE_READWRITE)  | mprotect(RW)  (helper)    |
//   | VirtualFree(base, 0, MEM_RELEASE)               | munmap(base, size)        |
//   | GetSystemInfo (page size)                       | sysconf(_SC_PAGESIZE)     |
//   +-------------------------------------------------+---------------------------+
//
// Compile and run from the REPOSITORY ROOT:
//   g++ -std=c++17 Linux/Memory/Memory_Linux.cpp -o Linux/Memory/memory_linux
//   ./Linux/Memory/memory_linux
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

#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
using namespace std;

// ----------------------------- Settings --------------------------------------

const char* const kTranscriptFile = "Linux/Memory/Memory_Linux.txt";
const char* const kMaxMapCountFile = "/proc/sys/vm/max_map_count";

const int    kIterations = 1000;
const int    kWarmup     = 50;
// The warm-up runs the action too, so every benchmark that consumes one
// prepared item per call needs warm-up + iterations items (1050).
const size_t kTotalCalls = kWarmup + kIterations;

const size_t kSize4KB  = 4 * 1024;
const size_t kSize64KB = 64 * 1024;
const size_t kSize1MB  = 1024 * 1024;

// The benchmarks that create 1050 separate mappings need a high enough
// vm.max_map_count (the kernel limit of mappings per process). The default is
// 65530, so 1050 is no problem; below this value we skip those benchmarks.
const long kMinMaxMapCount = 4096;

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
    int count = 0;
    int firstError = 0;

    // apiFailed = true : a system call failed, keep its errno.
    // apiFailed = false: some other problem (for example a bad index).
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

// Size of one memory page on this machine (normally 4096 bytes).
// sysconf returns -1 on failure; then we fall back to 4096.
size_t getPageSize() {
    long pageSize = sysconf(_SC_PAGESIZE);
    return (pageSize > 0) ? static_cast<size_t>(pageSize) : 4096;
}

// Writes one byte into every page of a block. The first write to a page makes
// the kernel assign a physical page (a page fault). The pointer is volatile so
// the compiler cannot decide the writes are useless and remove them.
void touchEveryPage(void* block, size_t size, size_t pageSize) {
    volatile char* memory = static_cast<volatile char*>(block);
    for (size_t offset = 0; offset < size; offset += pageSize) {
        memory[offset] = 1;
    }
}

// Reads vm.max_map_count (how many separate mappings one process may have).
// Returns -1 if the file cannot be read. Setup only, never inside a timed loop.
long readMaxMapCount() {
    ifstream file(kMaxMapCountFile);
    long value = -1;
    if (!(file >> value)) {
        return -1;
    }
    return value;
}

// Allocates `count` blocks of `size` bytes (read/write, untouched) into
// `blocks`. mmap reports failure with MAP_FAILED, NOT nullptr. If one mmap
// fails, the failure is logged, everything mapped so far is unmapped again,
// and false is returned (no leak, no crash). All blocks of one benchmark have
// the same size, so a single `size` is enough to unmap them later.
bool allocateBlocks(vector<void*>& blocks, size_t count, size_t size) {
    // Built BEFORE the loop so no string work happens between a failing
    // mmap and the LOG_SYSCALL that reads its error code.
    string detail = to_string(size / 1024) + " KB x " + to_string(count);
    blocks.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        void* block = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (block == MAP_FAILED) {
            LOG_SYSCALL("mmap_setup", false, detail.c_str());
            // Free what we already have.
            for (size_t j = 0; j < blocks.size(); ++j) {
                munmap(blocks[j], size);
            }
            blocks.clear();
            return false;
        }
        blocks.push_back(block);
    }
    LOG_SYSCALL("mmap_setup", true, detail.c_str());
    return true;
}

// Unmaps blocks[startIndex ... end] and empties the vector. Blocks before
// startIndex were already unmapped by the benchmark itself. Logs one teardown
// line. Returns true if every munmap worked.
bool releaseBlocks(vector<void*>& blocks, size_t startIndex, size_t size) {
    int failed = 0;
    int firstError = 0;
    for (size_t i = startIndex; i < blocks.size(); ++i) {
        if (munmap(blocks[i], size) != 0) {
            if (failed == 0) {
                firstError = errno;
            }
            ++failed;
        }
    }
    size_t remaining = (blocks.size() > startIndex) ? blocks.size() - startIndex : 0;
    string detail = to_string(remaining) + " block(s)";
    blocks.clear();

    if (failed > 0) {
        errno = firstError;   // restore it, string work may have changed it
    }
    LOG_SYSCALL("munmap_teardown", failed == 0, detail.c_str());
    return failed == 0;
}

// ----------------------------- PART 1: demo ----------------------------------

// Page size, and how Linux differs from Windows here: an OS-specific fact.
void printSystemInfo() {
    long pageSize = sysconf(_SC_PAGESIZE);
    LOG_SYSCALL("sysconf", pageSize > 0, "_SC_PAGESIZE");

    cout << "Page size              : " << pageSize << " bytes ("
         << pageSize / 1024 << " KB)\n";
    cout << "Linux has no separate \"allocation granularity\" like Windows (which reserves\n"
         << "address space in 64 KB units). mmap works in units of ONE PAGE, so even a\n"
         << "4 KB request uses only 4 KB of address space.\n";
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
        errno = EIO;
        LOG_SYSCALL("VerifyContent", false, detail);
    }
    return matches;
}

// Unmaps a whole block and logs it.
bool unmapAndLog(void* base, size_t size, const char* detail) {
    int result = munmap(base, size);
    LOG_SYSCALL("munmap", result == 0, detail);
    return result == 0;
}

// Two-step version: PROT_NONE mapping ("reserve"), then mprotect ("commit").
bool demoReserveThenProtect() {
    cout << "\n-- Map with PROT_NONE, then make usable, a 1 MB block --\n";

    // PROT_NONE: the address range is claimed but cannot be touched yet.
    // This is the closest Linux match to Windows MEM_RESERVE.
    void* base = mmap(nullptr, kSize1MB, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    LOG_SYSCALL("mmap", base != MAP_FAILED, "1 MB, PROT_NONE");
    if (base == MAP_FAILED) {
        return false;
    }

    // mprotect: the same range becomes readable and writable. This is the
    // closest Linux match to the MEM_COMMIT step (a helper call, not one of
    // the required calls).
    int protectResult = mprotect(base, kSize1MB, PROT_READ | PROT_WRITE);
    LOG_SYSCALL("mprotect", protectResult == 0, "1 MB, PROT_READ | PROT_WRITE");
    if (protectResult != 0) {
        unmapAndLog(base, kSize1MB, "unmap the 1 MB block");   // do not leak the mapping
        return false;
    }

    bool demoOk = writeAndVerifyPattern(base, kSize1MB, "1 MB block");

    // Deliberate failure: base + 1 is not page aligned, so munmap must refuse
    // it with EINVAL (error 22).
    // NOTE: on Linux munmap CAN free part of a mapping (for example base + 4096
    // with length 4096 would succeed and split the mapping in two), while
    // Windows VirtualFree with MEM_RELEASE only accepts the START of an
    // allocation. So the failing test had to be different from the Windows one.
    cout << "munmap on base + 1, not page aligned (this FAILURE is expected, error 22):\n";
    void* notAligned = static_cast<char*>(base) + 1;
    int wrongResult = munmap(notAligned, 4096);
    int wrongError = errno;   // read before anything can change it
    LOG_SYSCALL("munmap", wrongResult == 0, "base+1, expected to fail");
    if (wrongResult == 0) {
        cout << "UNEXPECTED: munmap accepted an address that is not page aligned!\n";
        demoOk = false;
    } else if (wrongError != EINVAL) {
        cout << "UNEXPECTED: it failed, but with error " << wrongError << " instead of 22.\n";
        demoOk = false;
    }

    bool freed = unmapAndLog(base, kSize1MB, "unmap the 1 MB block");
    return demoOk && freed;
}

// The common one-call form: map readable and writable memory directly.
bool demoOneCallMmap() {
    cout << "\n-- One-call mmap (PROT_READ | PROT_WRITE), 64 KB --\n";

    void* block = mmap(nullptr, kSize64KB, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    LOG_SYSCALL("mmap", block != MAP_FAILED, "64 KB in one call");
    if (block == MAP_FAILED) {
        return false;
    }

    bool writeOk = writeAndVerifyPattern(block, kSize64KB, "64 KB block");
    bool freed = unmapAndLog(block, kSize64KB, "unmap the 64 KB block");
    return writeOk && freed;
}

// Returns true if everything worked and the data read back was identical.
bool runDemo() {
    cout << "---------- PART 1: DEMO (logged, not timed) ----------\n";
    printSystemInfo();
    bool firstPartOk = demoReserveThenProtect();
    bool secondPartOk = demoOneCallMmap();
    return firstPartOk && secondPartOk;
}

// ----------------------------- PART 2: benchmarks ----------------------------
//
// NOTES FOR THE REPORT
//  (1) mmap of anonymous memory only creates the address-range mapping. The
//      kernel assigns physical pages lazily on the first touch (demand paging),
//      so mmap itself shows about zero page faults.
//  (2) Linux has no 64 KB allocation granularity: requests are rounded up to a
//      page (4 KB), so a 4 KB request uses 4 KB of address space (on Windows
//      it uses a 64 KB range).
//  (3) Adjacent anonymous mappings with the same permissions may be MERGED by
//      the kernel into one region (VMA), which can change mmap and munmap cost
//      compared with Windows.
//  (4) The CPU columns (cpu_us, cpu_per_call_us) are measured around the WHOLE
//      loop and include cleanup and timer overhead. The cpu_cycles columns come
//      from the TSC and are NOT comparable with the Windows ones.
//  (5) Memory delta and page fault columns are loop totals, so only
//      FirstTouch_64KB (positive) and munmap_64KB_touched (negative memory
//      delta) are expected to show clearly non-zero values. A stray 1 page
//      fault on other rows is background noise (the metrics helper itself
//      reads /proc).
//  (6) Transparent huge pages do not apply to blocks smaller than 2 MB, so the
//      page fault count stays at one per 4 KB page.
//  (7) Name mapping: mmap = VirtualAlloc, munmap = VirtualFree,
//      mprotect (helper) = the commit step of VirtualAlloc.
//  (8) The warm-up runs the lambdas too, so failures there are counted as well.
//
// MEMORY LIMIT: the biggest benchmarks hold 1050 x 64 KB = about 67 MB at once.
// Each benchmark unmaps everything it mapped before the next one starts.

// A. Benchmark: mmap (read/write anonymous memory, NOT touched).
// The cleanup unmaps the block outside the timed region.
bool benchmarkMmap(const char* callName, size_t size) {
    FailureTracker failures;
    void* block = MAP_FAILED;

    auto action = [&]() {
        block = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (block == MAP_FAILED) {
            failures.record(true);
        }
    };

    auto cleanup = [&]() {
        if (block != MAP_FAILED) {
            if (munmap(block, size) != 0) {
                failures.record(true);
            }
            block = MAP_FAILED;
        }
    };

    metrics::Stats stats = metrics::benchmark(callName, action, kIterations, kWarmup, cleanup);
    return finishBenchmark(stats, failures);
}

// B. Benchmark: FirstTouch_64KB.
// IMPORTANT: this is NOT a system call. It measures the cost of the PAGE FAULTS
// that happen when memory returned by mmap is used for the first time
// (16 pages of 4 KB per 64 KB block). This is the benchmark where the memory
// delta and page fault columns become meaningful.
// All blocks are mapped beforehand (untouched); each call touches the next one.
bool benchmarkFirstTouch() {
    FailureTracker failures;
    const size_t size = kSize64KB;
    const size_t pageSize = getPageSize();

    vector<void*> blocks;
    if (!allocateBlocks(blocks, kTotalCalls, size)) {
        return false;
    }

    size_t nextIndex = 0;

    // Touching valid mapped memory has no return value to check; a real
    // problem would crash the program, and the mapping was already checked.
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

    bool teardownOk = releaseBlocks(blocks, 0, size);   // unmap all, touched or not
    bool clean = finishBenchmark(stats, failures);
    return clean && teardownOk;
}

// C and D. Benchmark: munmap of blocks prepared beforehand.
// touchFirst = false: blocks are untouched, so the kernel only drops the
//                     mapping (no physical pages to give back).
// touchFirst = true : every page is written before the benchmark, so the
//                     kernel must also free real physical pages (slower).
// The action unmaps the next block; no cleanup is needed.
bool benchmarkMunmap(const char* callName, size_t size, bool touchFirst) {
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
        int result = munmap(blocks[nextIndex], size);
        ++nextIndex;
        if (result != 0) {
            failures.record(true);
        }
    };

    metrics::Stats stats = metrics::benchmark(callName, action, kIterations, kWarmup);

    // Safety net: unmap any block the benchmark did not reach (normally none).
    bool teardownOk = releaseBlocks(blocks, nextIndex, size);
    bool clean = finishBenchmark(stats, failures);
    return clean && teardownOk;
}

// Runs all benchmarks. Returns how many of them had problems.
int runAllBenchmarks() {
    cout << "\n---------- PART 2: BENCHMARKS (timed, no logging inside) ----------\n";
    int problems = 0;

    // Informational: how many separate mappings the kernel allows per process.
    long maxMapCount = readMaxMapCount();
    if (maxMapCount >= 0) {
        cout << "vm.max_map_count: " << maxMapCount << "\n";
    } else {
        cout << "vm.max_map_count: could not be read (" << kMaxMapCountFile << ")\n";
    }

    if (!benchmarkMmap("mmap_4KB", kSize4KB))   ++problems;
    if (!benchmarkMmap("mmap_64KB", kSize64KB)) ++problems;
    if (!benchmarkMmap("mmap_1MB", kSize1MB))   ++problems;

    // The next four benchmarks create 1050 separate mappings each.
    if (maxMapCount >= 0 && maxMapCount < kMinMaxMapCount) {
        cout << "!!! WARNING: vm.max_map_count is only " << maxMapCount
             << " (below " << kMinMaxMapCount << "). FirstTouch_64KB, munmap_4KB,\n"
             << "    munmap_64KB and munmap_64KB_touched were SKIPPED.\n";
        problems += 4;
        return problems;
    }

    if (!benchmarkFirstTouch()) ++problems;

    if (!benchmarkMunmap("munmap_4KB", kSize4KB, false))           ++problems;
    if (!benchmarkMunmap("munmap_64KB", kSize64KB, false))         ++problems;
    if (!benchmarkMunmap("munmap_64KB_touched", kSize64KB, true))  ++problems;

    return problems;
}

// ----------------------------- Program flow ----------------------------------

int runAll() {
    logger::init();   // default path: logs/syscalls_linux.log

    cout << "=== Linux Memory: demo + benchmarks ===\n\n";

    if (!runDemo()) {
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