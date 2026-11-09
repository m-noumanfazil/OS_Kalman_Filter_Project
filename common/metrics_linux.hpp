// metrics_linux.hpp - header-only timing / CPU / memory measurement (Linux)
// Linux version of metrics.hpp. Same public API, same statistics maths,
// same CSV header. No extra link flags are needed.
//
// Windows call -> Linux call (what was replaced)
//   +--------------------------------------+-------------------------------------------+
//   | Windows                              | Linux                                     |
//   +--------------------------------------+-------------------------------------------+
//   | QueryPerformanceCounter/Frequency    | clock_gettime(CLOCK_MONOTONIC)            |
//   | GetProcessTimes (kernel+user)        | clock_gettime(CLOCK_PROCESS_CPUTIME_ID)   |
//   | QueryProcessCycleTime                | __rdtsc() (NOT the same thing, see below) |
//   | GetProcessMemoryInfo WorkingSetSize  | /proc/self/statm (resident pages) * page  |
//   | GetProcessMemoryInfo PageFaultCount  | getrusage(RUSAGE_SELF) ru_minflt+ru_majflt|
//   | CreateDirectoryA                     | mkdir(folder, 0755)                       |
//   +--------------------------------------+-------------------------------------------+

#ifndef METRICS_LINUX_HPP
#define METRICS_LINUX_HPP

#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>   // __rdtsc
#endif

namespace metrics {

namespace detail {

// Monotonic clock in nanoseconds. CLOCK_MONOTONIC never jumps when the
// system date/time is changed, which is what we want for measuring durations.
// (We keep the name ticksNow so the structure matches the Windows file.)
inline long long ticksNow() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<long long>(now.tv_sec) * 1000000000LL + now.tv_nsec;
}

// Converts a DIFFERENCE of two ticksNow() values (nanoseconds) to microseconds.
// The difference is computed as an integer first, so no precision is lost.
inline double ticksToMicroseconds(long long nanoseconds) {
    return static_cast<double>(nanoseconds) / 1000.0;
}

inline void createParentFolders(const std::string& filePath) {
    for (size_t i = 0; i < filePath.size(); ++i) {
        if (filePath[i] == '/') {
            std::string folder = filePath.substr(0, i);
            if (!folder.empty()) {
                mkdir(folder.c_str(), 0755);   // EEXIST is normal and ignored
            }
        }
    }
}

// The exact first line of the CSV file. Kept in one place so the header that
// is written and the header that is checked can never disagree.
// IDENTICAL to the Windows header, so both CSV files have the same columns.
inline const char* csvHeader() {
    return "call_name,iterations,min_us,median_us,mean_us,max_us,"
           "stddev_us,cpu_us,cpu_per_call_us,cpu_cycles,cycles_per_call,"
           "mem_delta_kb,page_faults";
}

} // namespace detail

// ---------- Raw readings ----------

// CPU time (user + kernel) used by this process so far, in microseconds.
// CLOCK_PROCESS_CPUTIME_ID has nanosecond resolution, unlike the coarse
// ~15.6 ms steps of GetProcessTimes on Windows.
inline double processCpuMicroseconds() {
    timespec cpu;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu) != 0) {
        return 0.0;
    }
    long long nanoseconds = static_cast<long long>(cpu.tv_sec) * 1000000000LL + cpu.tv_nsec;
    return static_cast<double>(nanoseconds) / 1000.0;
}

// Value of the CPU time-stamp counter (TSC).
// WARNING: this is NOT the same as Windows QueryProcessCycleTime.
//   - It counts TSC ticks of WALL-CLOCK time, not the cycles used by THIS process.
//   - It keeps ticking while the process is blocked or waiting.
//   - So the cpu_cycles and cycles_per_call columns of the Linux CSV are NOT
//     comparable with the Windows ones. They are only filled so that the CSV
//     format matches. The honest CPU comparison between the two systems is
//     the cpu_per_call_us column.
// Returns 0 on CPUs that are not x86.
inline unsigned long long processCpuCycles() {
#if defined(__x86_64__) || defined(__i386__)
    return static_cast<unsigned long long>(__rdtsc());
#else
    return 0;
#endif
}

struct MemoryInfo {
    long long workingSetBytes;   // on Linux: current resident memory
    long long pageFaults;        // minor + major page faults so far
};

// Current resident memory comes from the second field of /proc/self/statm
// (number of resident pages) times the page size. getrusage ru_maxrss is NOT
// used: it is the PEAK and never goes down, so a delta would be wrong after
// memory is freed.
// /proc/self/statm is read by opening and reading a text file with the C++
// library. This is a few system calls, but readMemory() is only called outside
// the timed region (before and after a measurement, not inside the loop).
// If /proc cannot be read, the resident size stays 0 (no crash).
inline MemoryInfo readMemory() {
    MemoryInfo info = {0, 0};

    std::ifstream statm("/proc/self/statm");
    long long totalPages = 0;
    long long residentPages = 0;
    if (statm >> totalPages >> residentPages) {
        long pageSize = sysconf(_SC_PAGESIZE);
        if (pageSize > 0) {
            info.workingSetBytes = residentPages * static_cast<long long>(pageSize);
        }
    }

    rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        info.pageFaults = static_cast<long long>(usage.ru_minflt) +
                          static_cast<long long>(usage.ru_majflt);
    }
    return info;
}

// ---------- Single measurement ----------

struct Measurement {
    double elapsedUs;          // wall-clock time
    double cpuUs;              // CPU time delta (kernel + user)
    double workingSetDeltaKb;  // resident memory change in KB (can be negative)
    long long pageFaultDelta;
};

class Measurer {
public:
    void start() {
        // The timer is read LAST, so the cost of the other readings is not timed.
        startCpuUs_ = processCpuMicroseconds();
        startMemory_ = readMemory();
        startTicks_ = detail::ticksNow();
    }

    Measurement stop() {
        // The timer is read FIRST, for the same reason.
        long long endTicks = detail::ticksNow();
        double endCpuUs = processCpuMicroseconds();
        MemoryInfo endMemory = readMemory();

        Measurement m;
        m.elapsedUs = detail::ticksToMicroseconds(endTicks - startTicks_);
        m.cpuUs = endCpuUs - startCpuUs_;
        m.workingSetDeltaKb =
            static_cast<double>(endMemory.workingSetBytes - startMemory_.workingSetBytes) / 1024.0;
        m.pageFaultDelta = endMemory.pageFaults - startMemory_.pageFaults;
        return m;
    }

private:
    long long startTicks_ = 0;
    double startCpuUs_ = 0.0;
    MemoryInfo startMemory_{};
};

// ---------- Benchmark ----------

struct Stats {
    std::string callName;
    int iterations = 0;
    double minUs = 0.0;
    double maxUs = 0.0;
    double meanUs = 0.0;
    double medianUs = 0.0;
    double stddevUs = 0.0;
    double totalCpuUs = 0.0;               // CPU time of the whole loop (nanosecond clock on Linux)
    double cpuPerCallUs = 0.0;             // totalCpuUs / iterations
    unsigned long long totalCpuCycles = 0; // TSC ticks of the whole loop (see processCpuCycles)
    double cyclesPerCall = 0.0;            // totalCpuCycles / iterations
    double memDeltaKb = 0.0;               // resident memory change over the whole loop
    long long pageFaults = 0;              // page fault change over the whole loop
};

// Turns the list of per-iteration times into min/max/mean/median/stddev and
// carries the loop-level CPU, cycle, memory and page fault numbers through.
// totalCpuCycles has a default of 0, so older calls with 5 arguments still work.
inline Stats computeStats(const std::string& callName,
                          const std::vector<double>& timesUs,
                          double totalCpuUs, double memDeltaKb, long long pageFaults,
                          unsigned long long totalCpuCycles = 0) {
    Stats s;
    s.callName = callName;
    s.iterations = static_cast<int>(timesUs.size());
    s.totalCpuUs = totalCpuUs;
    s.totalCpuCycles = totalCpuCycles;
    s.memDeltaKb = memDeltaKb;
    s.pageFaults = pageFaults;
    if (timesUs.empty()) {
        return s;
    }

    const size_t n = timesUs.size();
    s.minUs = *std::min_element(timesUs.begin(), timesUs.end());
    s.maxUs = *std::max_element(timesUs.begin(), timesUs.end());

    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) sum += timesUs[i];
    s.meanUs = sum / static_cast<double>(n);

    // Median: middle value of the sorted list (average of two if N is even).
    std::vector<double> sorted = timesUs;
    std::sort(sorted.begin(), sorted.end());
    size_t middle = n / 2;
    s.medianUs = (n % 2 == 1) ? sorted[middle]
                              : (sorted[middle - 1] + sorted[middle]) / 2.0;

    // Sample standard deviation (divide by N-1).
    if (n > 1) {
        double squares = 0.0;
        for (size_t i = 0; i < n; ++i) {
            double diff = timesUs[i] - s.meanUs;
            squares += diff * diff;
        }
        s.stddevUs = std::sqrt(squares / static_cast<double>(n - 1));
    }

    // Per-call averages of the loop-level CPU numbers.
    s.cpuPerCallUs = s.totalCpuUs / static_cast<double>(n);
    s.cyclesPerCall = static_cast<double>(s.totalCpuCycles) / static_cast<double>(n);
    return s;
}

// Runs `action` iterations times and times each run on its own.
//  - action:  the system call you want to measure (this is the timed part)
//  - cleanup: optional, runs after every iteration OUTSIDE the timed region
//             (for example close() after open())
// The warm-up runs also call cleanup, but are not measured.
//
// IMPORTANT about what each number covers:
//  - min / median / mean / max / stddev cover ONLY the action itself.
//  - The CPU numbers (cpu_us, cpu_per_call_us, cpu_cycles, cycles_per_call)
//    are measured around the WHOLE loop, so they INCLUDE the optional cleanup
//    callable and the timer overhead (the clock_gettime calls and the
//    vector push_back). Do not compare them directly with the mean time.
inline Stats benchmark(const std::string& callName,
                       const std::function<void()>& action,
                       int iterations = 1000,
                       int warmupCount = 50,
                       const std::function<void()>& cleanup = nullptr) {
    if (iterations < 1) iterations = 1;

    // Warm-up: fills caches and lets the system do first-time work.
    for (int i = 0; i < warmupCount; ++i) {
        action();
        if (cleanup) cleanup();
    }

    std::vector<double> timesUs;
    timesUs.reserve(iterations);

    double cpuBeforeUs = processCpuMicroseconds();
    unsigned long long cyclesBefore = processCpuCycles();
    MemoryInfo memoryBefore = readMemory();

    for (int i = 0; i < iterations; ++i) {
        long long startTicks = detail::ticksNow();
        action();
        long long endTicks = detail::ticksNow();
        timesUs.push_back(detail::ticksToMicroseconds(endTicks - startTicks));

        if (cleanup) cleanup();   // not timed
    }

    double cpuAfterUs = processCpuMicroseconds();
    unsigned long long cyclesAfter = processCpuCycles();
    MemoryInfo memoryAfter = readMemory();

    return computeStats(
        callName, timesUs,
        cpuAfterUs - cpuBeforeUs,
        static_cast<double>(memoryAfter.workingSetBytes - memoryBefore.workingSetBytes) / 1024.0,
        memoryAfter.pageFaults - memoryBefore.pageFaults,
        cyclesAfter - cyclesBefore);
}

// ---------- Output ----------

inline void printStats(const Stats& s) {
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "==== " << s.callName << " ====\n"
              << "Iterations    : " << s.iterations << "\n"
              << "Min           : " << s.minUs << " us\n"
              << "Median        : " << s.medianUs << " us\n"
              << "Mean          : " << s.meanUs << " us\n"
              << "Max           : " << s.maxUs << " us\n"
              << "Std dev       : " << s.stddevUs << " us\n"
              << "CPU (total)   : " << s.totalCpuUs << " us (nanosecond clock on Linux)\n"
              << "CPU per call  : " << s.cpuPerCallUs << " us\n"
              << "CPU cycles    : " << s.totalCpuCycles << "\n"
              << "Cycles/call   : " << s.cyclesPerCall << "\n"
              << "Memory delta  : " << s.memDeltaKb << " KB\n"
              << "Page faults   : " << s.pageFaults << "\n"
              << std::endl;
}

// Appends one row to the CSV file. Call names must not contain commas.
// Returns false if the file could not be opened, or if it already exists with
// an old/different header (appending would mix up the columns).
inline bool appendStatsToCsv(const Stats& s,
                             const std::string& csvPath = "results/Linux/linux_results.csv") {
    detail::createParentFolders(csvPath);

    // Look at the first line of an existing file. If the file is missing or
    // empty we write the header; if it has a different header we refuse.
    bool needHeader = true;
    {
        std::ifstream check(csvPath);
        if (check.is_open()) {
            std::string firstLine;
            if (std::getline(check, firstLine)) {
                if (!firstLine.empty() && firstLine.back() == '\r') {
                    firstLine.pop_back();   // file may have Windows line endings
                }
                if (firstLine != detail::csvHeader()) {
                    std::cerr << "metrics: WARNING - " << csvPath
                              << " has an old or different header, so nothing was written.\n"
                              << "         Please delete or rename that file and run again."
                              << std::endl;
                    return false;
                }
                needHeader = false;
            }
        }
    }   // the read stream is closed here, before we open the file for writing

    std::ofstream out(csvPath, std::ios::app);
    if (!out) {
        std::cerr << "metrics: cannot open " << csvPath << std::endl;
        return false;
    }

    if (needHeader) {
        out << detail::csvHeader() << "\n";
    }
    out << std::fixed << std::setprecision(3)
        << s.callName << "," << s.iterations << ","
        << s.minUs << "," << s.medianUs << "," << s.meanUs << "," << s.maxUs << ","
        << s.stddevUs << "," << s.totalCpuUs << "," << s.cpuPerCallUs << ","
        << s.totalCpuCycles << "," << s.cyclesPerCall << ","
        << s.memDeltaKb << "," << s.pageFaults << "\n";
    out.flush();
    return true;
}

} // namespace metrics

#endif // METRICS_LINUX_HPP