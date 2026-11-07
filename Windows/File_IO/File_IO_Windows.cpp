#include "../../common/logger.hpp"
#include "../../common/metrics.hpp"

int main() {
    logger::init();   // optional; defaults to logs/syscalls_windows.log

    const char* fileName = "test_input.txt";

    // --- One logged call (not timed) ---
    HANDLE h = CreateFileA(fileName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    LOG_SYSCALL("CreateFile", h != INVALID_HANDLE_VALUE, fileName);

    if (h != INVALID_HANDLE_VALUE) {
        BOOL closed = CloseHandle(h);
        LOG_SYSCALL("CloseHandle", closed != 0, fileName);
    }

    // Deliberate failure, to see an error line in the log:
    HANDLE bad = CreateFileA("does_not_exist.txt", GENERIC_READ, 0, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    LOG_SYSCALL("CreateFile", bad != INVALID_HANDLE_VALUE, "does_not_exist.txt");

    // --- Benchmarked loop (NO logging inside it) ---
    HANDLE benchHandle = INVALID_HANDLE_VALUE;
    int failures = 0;

    auto action = [&]() {
        benchHandle = CreateFileA(fileName, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    };
    auto cleanup = [&]() {          // runs outside the timed region
        if (benchHandle != INVALID_HANDLE_VALUE) {
            CloseHandle(benchHandle);
            benchHandle = INVALID_HANDLE_VALUE;
        } else {
            ++failures;
        }
    };

    metrics::Stats stats = metrics::benchmark("CreateFile", action, 1000, 50, cleanup);
    metrics::printStats(stats);
    metrics::appendStatsToCsv(stats);   // results/Windows/windows_results.csv

    if (failures > 0) {
        LOG_SYSCALL("CreateFile (benchmark)", false, "some iterations failed");
    }

    DeleteFileA(fileName);
    return 0;
}