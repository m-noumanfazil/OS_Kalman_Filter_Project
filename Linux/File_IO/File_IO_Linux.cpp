// Smoke test for logger_linux.hpp and metrics_linux.hpp.
// Compile and run from the repository root (see the commands below).

#include "../../common/logger_linux.hpp"
#include "../../common/metrics_linux.hpp"

#include <fcntl.h>
#include <unistd.h>

int main() {
    logger::init();   // default path: logs/syscalls_linux.log

    const char* fileName = "test_input.txt";

    // --- Logged calls (not timed) ---
    int fd = open(fileName, O_CREAT | O_RDWR | O_TRUNC, 0644);
    LOG_SYSCALL("open", fd >= 0, fileName);

    if (fd >= 0) {
        int closed = close(fd);
        LOG_SYSCALL("close", closed == 0, fileName);
    }

    // Deliberate failure: this file does not exist (errno 2).
    int bad = open("does_not_exist.txt", O_RDONLY);
    LOG_SYSCALL("open", bad >= 0, "does_not_exist.txt");
    if (bad >= 0) {
        close(bad);   // unexpected, but do not leak the descriptor
    }

    // --- Benchmarked loop (NO logging inside the lambdas) ---
    int benchFd = -1;
    int failures = 0;

    auto action = [&]() {
        benchFd = open(fileName, O_RDONLY);
    };
    auto cleanup = [&]() {          // runs outside the timed region
        if (benchFd >= 0) {
            close(benchFd);
            benchFd = -1;
        } else {
            ++failures;             // open() failed in this iteration
        }
    };

    metrics::Stats stats = metrics::benchmark("open", action, 1000, 50, cleanup);
    metrics::printStats(stats);
    metrics::appendStatsToCsv(stats);   // results/Linux/linux_results.csv

    if (failures > 0) {
        LOG_SYSCALL("open (benchmark)", false, "some iterations failed");
    }

    int removed = unlink(fileName);
    LOG_SYSCALL("unlink", removed == 0, fileName);
    return 0;
}