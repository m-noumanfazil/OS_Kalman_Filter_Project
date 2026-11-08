// metrics.hpp - header-only timing / CPU / memory measurement (Windows only)
// Needs the link flag -lpsapi (for GetProcessMemoryInfo).

#ifndef METRICS_HPP
#define METRICS_HPP

// QueryProcessCycleTime exists only on Windows 7 and newer. MinGW headers hide
// it unless the target Windows version is at least 0x0601, so we ask for it.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <realtimeapiset.h>   // declares QueryProcessCycleTime
#include <psapi.h>            // must come AFTER windows.h

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>
using namespace std;

namespace metrics {

namespace detail {

inline long long ticksNow() {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

// The counter frequency never changes while the system runs, so read it once.
inline double ticksPerSecond() {
    static double frequency = 0.0;
    if (frequency == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        frequency = static_cast<double>(f.QuadPart);
    }
    return frequency;
}

// Converts a DIFFERENCE of two counter values to microseconds.
// (We convert differences, not raw counters, to avoid losing precision.)
inline double ticksToMicroseconds(long long ticks) {
    return static_cast<double>(ticks) * 1000000.0 / ticksPerSecond();
}

// FILETIME counts in 100-nanosecond steps, split over two 32-bit halves.
inline unsigned long long filetimeTo100ns(const FILETIME& ft) {
    ULARGE_INTEGER value;
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return value.QuadPart;
}

inline void createParentFolders(const string& filePath) {
    for (size_t i = 0; i < filePath.size(); ++i) {
        if (filePath[i] == '/' || filePath[i] == '\\') {
            string folder = filePath.substr(0, i);
            if (!folder.empty() && folder.back() != ':') {
                CreateDirectoryA(folder.c_str(), nullptr);
            }
        }
    }
}

// The exact first line of the CSV file. Kept in one place so the header that
// is written and the header that is checked can never disagree.
inline const char* csvHeader() {
    return "call_name,iterations,min_us,median_us,mean_us,max_us,"
           "stddev_us,cpu_us,cpu_per_call_us,cpu_cycles,cycles_per_call,"
           "mem_delta_kb,page_faults";
}

} // namespace detail

// ---------- Raw readings ----------

// Kernel time + user time of this process, in microseconds.
// WARNING: Windows only updates this in steps of about 15.6 ms, so short
// loops give coarse values. Use processCpuCycles() for fine-grained CPU cost.
inline double processCpuMicroseconds() {
    FILETIME creationTime, exitTime, kernelTime, userTime;
    if (!GetProcessTimes(GetCurrentProcess(), &creationTime, &exitTime,
                         &kernelTime, &userTime)) {
        return 0.0;
    }
    unsigned long long total100ns =
        detail::filetimeTo100ns(kernelTime) + detail::filetimeTo100ns(userTime);
    return static_cast<double>(total100ns) / 10.0;   // 10 x 100ns = 1 us
}

// CPU cycles used by all threads of this process so far (user + kernel).
// Unlike GetProcessTimes, this counter is not limited to scheduler ticks,
// so differences over a short loop are meaningful.
// Returns 0 if the call fails.
inline unsigned long long processCpuCycles() {
    ULONG64 cycles = 0;
    if (!QueryProcessCycleTime(GetCurrentProcess(), &cycles)) {
        return 0;
    }
    return static_cast<unsigned long long>(cycles);
}

struct MemoryInfo {
    long long workingSetBytes;
    long long pageFaults;
};

inline MemoryInfo readMemory() {
    PROCESS_MEMORY_COUNTERS counters;
    ZeroMemory(&counters, sizeof(counters));
    MemoryInfo info = {0, 0};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        info.workingSetBytes = static_cast<long long>(counters.WorkingSetSize);
        info.pageFaults = static_cast<long long>(counters.PageFaultCount);
    }
    return info;
}

// ---------- Single measurement ----------

struct Measurement {
    double elapsedUs;          // wall-clock time
    double cpuUs;              // CPU time delta (kernel + user)
    double workingSetDeltaKb;  // working set change in KB (can be negative)
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
    string callName;
    int iterations = 0;
    double minUs = 0.0;
    double maxUs = 0.0;
    double meanUs = 0.0;
    double medianUs = 0.0;
    double stddevUs = 0.0;
    double totalCpuUs = 0.0;               // CPU time of the whole loop (coarse, ~15.6 ms steps)
    double cpuPerCallUs = 0.0;             // totalCpuUs / iterations
    unsigned long long totalCpuCycles = 0; // CPU cycles of the whole loop (not quantized)
    double cyclesPerCall = 0.0;            // totalCpuCycles / iterations
    double memDeltaKb = 0.0;               // working set change over the whole loop
    long long pageFaults = 0;              // page fault change over the whole loop
};

// Turns the list of per-iteration times into min/max/mean/median/stddev and
// carries the loop-level CPU, cycle, memory and page fault numbers through.
// totalCpuCycles has a default of 0, so older calls with 5 arguments still work.
inline Stats computeStats(const string& callName,
                          const vector<double>& timesUs,
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
    s.minUs = *min_element(timesUs.begin(), timesUs.end());
    s.maxUs = *max_element(timesUs.begin(), timesUs.end());

    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) sum += timesUs[i];
    s.meanUs = sum / static_cast<double>(n);

    // Median: middle value of the sorted list (average of two if N is even).
    vector<double> sorted = timesUs;
    sort(sorted.begin(), sorted.end());
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
        s.stddevUs = sqrt(squares / static_cast<double>(n - 1));
    }

    // Per-call averages of the loop-level CPU numbers.
    s.cpuPerCallUs = s.totalCpuUs / static_cast<double>(n);
    s.cyclesPerCall = static_cast<double>(s.totalCpuCycles) / static_cast<double>(n);
    return s;
}

// Runs `action` iterations times and times each run on its own.
//  - action:  the system call you want to measure (this is the timed part)
//  - cleanup: optional, runs after every iteration OUTSIDE the timed region
//             (for example CloseHandle after CreateFile)
// The warm-up runs also call cleanup, but are not measured.
//
// IMPORTANT about what each number covers:
//  - min / median / mean / max / stddev cover ONLY the action itself.
//  - The CPU numbers (cpu_us, cpu_per_call_us, cpu_cycles, cycles_per_call)
//    are measured around the WHOLE loop, so they INCLUDE the optional cleanup
//    callable and the timer overhead (the QueryPerformanceCounter calls and
//    the vector push_back). Do not compare them directly with the mean time.
inline Stats benchmark(const string& callName,
                       const function<void()>& action,
                       int iterations = 1000,
                       int warmupCount = 50,
                       const function<void()>& cleanup = nullptr) {
    if (iterations < 1) iterations = 1;

    // Warm-up: fills caches and lets Windows do first-time work.
    for (int i = 0; i < warmupCount; ++i) {
        action();
        if (cleanup) cleanup();
    }

    vector<double> timesUs;
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
    cout << fixed << setprecision(3);
    cout << "==== " << s.callName << " ====\n"
              << "Iterations    : " << s.iterations << "\n"
              << "Min           : " << s.minUs << " us\n"
              << "Median        : " << s.medianUs << " us\n"
              << "Mean          : " << s.meanUs << " us\n"
              << "Max           : " << s.maxUs << " us\n"
              << "Std dev       : " << s.stddevUs << " us\n"
              << "CPU (total)   : " << s.totalCpuUs << " us (coarse, ~15.6 ms steps)\n"
              << "CPU per call  : " << s.cpuPerCallUs << " us\n"
              << "CPU cycles    : " << s.totalCpuCycles << "\n"
              << "Cycles/call   : " << s.cyclesPerCall << "\n"
              << "Memory delta  : " << s.memDeltaKb << " KB\n"
              << "Page faults   : " << s.pageFaults << "\n"
              << endl;
}

// Appends one row to the CSV file. Call names must not contain commas.
// Returns false if the file could not be opened, or if it already exists with
// an old/different header (appending would mix up the columns).
inline bool appendStatsToCsv(const Stats& s,
                             const string& csvPath = "results/Windows/windows_results.csv") {
    detail::createParentFolders(csvPath);

    // Look at the first line of an existing file. If the file is missing or
    // empty we write the header; if it has a different header we refuse.
    bool needHeader = true;
    {
        ifstream check(csvPath);
        if (check.is_open()) {
            string firstLine;
            if (getline(check, firstLine)) {
                if (!firstLine.empty() && firstLine.back() == '\r') {
                    firstLine.pop_back();   // file may have Windows line endings
                }
                if (firstLine != detail::csvHeader()) {
                    cerr << "metrics: WARNING - " << csvPath
                              << " has an old or different header, so nothing was written.\n"
                              << "         Please delete or rename that file and run again."
                              << endl;
                    return false;
                }
                needHeader = false;
            }
        }
    }   // the read stream is closed here, before we open the file for writing

    ofstream out(csvPath, ios::app);
    if (!out) {
        cerr << "metrics: cannot open " << csvPath << endl;
        return false;
    }

    if (needHeader) {
        out << detail::csvHeader() << "\n";
    }
    out << fixed << setprecision(3)
        << s.callName << "," << s.iterations << ","
        << s.minUs << "," << s.medianUs << "," << s.meanUs << "," << s.maxUs << ","
        << s.stddevUs << "," << s.totalCpuUs << "," << s.cpuPerCallUs << ","
        << s.totalCpuCycles << "," << s.cyclesPerCall << ","
        << s.memDeltaKb << "," << s.pageFaults << "\n";
    out.flush();
    return true;
}

} // namespace metrics

#endif // METRICS_HPP