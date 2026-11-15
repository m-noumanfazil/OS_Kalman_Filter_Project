// =============================================================================
// Kalman_Filter_Windows.cpp
//
// Part of the CT-353 Operating Systems Complex Computing Activity (Part B).
//
// High-Speed Supersonic Fighter Jet Radar Tracking System:
// Parallel Multi-Process 2D Kalman Filter using native Win32 system calls.
//
// Syscalls Demonstrated and Integrated:
//   - File I/O : CreateFileA, ReadFile, WriteFile, CloseHandle
//   - Memory   : CreateFileMappingA, MapViewOfFile, UnmapViewOfFile, VirtualAlloc
//   - Process  : CreateProcessA, WaitForMultipleObjects, GetExitCodeProcess
//   - IPC      : Named Shared Memory Arena (Local\Kalman_Radar_Shm_...)
//
// Execution Architecture:
//   PART 1: Demo & Validation (logged, not timed)
//           - Ingestion of raw radar telemetry via Win32 File I/O
//           - Shared memory arena allocation and mapping
//           - Single-process baseline trajectory filtering
//           - Multi-process parallel filtering (4 workers)
//           - Numerical parity verification (single vs parallel tolerance < 1e-9)
//           - Tracking RMSE calculation proving noise reduction
//           - Intentional error handling demo
//   PART 2: Benchmarking & Parallel Scalability (timed, zero logging in loops)
//           - 1 Worker  (Single-process baseline)
//           - 2 Workers (Dual-core parallelization)
//           - 4 Workers (Quad-core parallelization)
//           - 8 Workers (Octa-core parallelization, 1 worker per jet)
//           - Speedup & Amdahl efficiency calculation
//           - Results appended to results/Windows/windows_results.csv
//           - Output exported to results/Windows/filtered_sensor_data.csv
//   PART 3: Automatic console transcript mirroring to
//           Windows/Kalman_Filter/Kalman_Filter_Windows.txt
//
// Compile from REPOSITORY ROOT:
//   g++ -std=c++17 -static -static-libgcc -static-libstdc++ Windows\Kalman_Filter\Kalman_Filter_Windows.cpp -o Windows\Kalman_Filter\kalman_filter.exe -lpsapi -ladvapi32
//
// Run from REPOSITORY ROOT:
//   .\Windows\Kalman_Filter\kalman_filter.exe
// =============================================================================

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include "../../common/logger.hpp"
#include "../../common/metrics.hpp"
#include "../../common/kalman_core.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <streambuf>
#include <string>
#include <vector>

using namespace std;

// ----------------------------- Configuration ---------------------------------

const char* const kInputCsvPath       = "common/sensor_data.csv";
const char* const kOutputCsvPath      = "results/Windows/filtered_sensor_data.csv";
const char* const kTranscriptFile     = "Windows/Kalman_Filter/Kalman_Filter_Windows.txt";
const char* const kMissingShmName     = "Local\\KalmanRadarNonExistentShm";

const int   kMaxRecords               = 100000;
const int   kTotalTracks              = 8;
const int   kBenchIterations          = 50;
const int   kBenchWarmup              = 5;

// Shared memory arena layout mapped across all worker processes
struct SharedRadarArena {
    uint32_t recordCount;
    uint32_t trackCount;
    kalman::RadarMeasurement inputs[kMaxRecords];
    kalman::FilteredOutput   outputs[kMaxRecords];
};

const size_t kArenaSize = sizeof(SharedRadarArena);

// ----------------------------- Tee stream buffer (PART 3) --------------------

class TeeBuffer : public streambuf {
public:
    TeeBuffer(streambuf* first, streambuf* second)
        : first_(first), second_(second) {}

protected:
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

    streamsize xsputn(const char_type* text, streamsize count) override {
        streamsize written1 = first_->sputn(text, count);
        streamsize written2 = second_->sputn(text, count);
        return min(written1, written2);
    }

    int sync() override {
        int r1 = first_->pubsync();
        int r2 = second_->pubsync();
        return (r1 == 0 && r2 == 0) ? 0 : -1;
    }

private:
    streambuf* first_;
    streambuf* second_;
};

// ----------------------------- Path & Process Helpers ------------------------

bool getOwnExecutablePath(string& outPath) {
    char buffer[MAX_PATH];
    DWORD length = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        LOG_SYSCALL("GetModuleFileNameA", false, "resolving own exe path");
        return false;
    }
    outPath = buffer;
    return true;
}

// ----------------------------- File I/O via Win32 Syscalls -------------------

// Ingests sensor CSV using CreateFileA and ReadFile
bool ingestSensorDataWin32(const char* filePath, vector<kalman::RadarMeasurement>& outData) {
    outData.clear();

    HANDLE hFile = CreateFileA(filePath, GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool openOk = (hFile != INVALID_HANDLE_VALUE);
    LOG_SYSCALL("CreateFileA", openOk, filePath);
    if (!openOk) return false;

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize)) {
        LOG_SYSCALL("GetFileSizeEx", false, filePath);
        CloseHandle(hFile);
        return false;
    }

    size_t totalBytes = static_cast<size_t>(fileSize.QuadPart);
    vector<char> buffer(totalBytes + 1, '\0');
    DWORD bytesRead = 0;
    BOOL readOk = ReadFile(hFile, buffer.data(), static_cast<DWORD>(totalBytes), &bytesRead, nullptr);
    bool readSuccess = (readOk && bytesRead == totalBytes);
    LOG_SYSCALL("ReadFile", readSuccess, filePath);
    bool closeOk = CloseHandle(hFile) != 0;
    LOG_SYSCALL("CloseHandle", closeOk, filePath);

    if (!readSuccess || !closeOk) return false;

    // Fast CSV parsing
    stringstream ss(buffer.data());
    string line;
    // Skip header line
    if (!getline(ss, line)) return false;

    double lastTimestamp[kTotalTracks] = {};
    bool seenTrack[kTotalTracks] = {};
    while (getline(ss, line)) {
        if (line.empty()) continue;
        stringstream lineStream(line);
        string cell;
        kalman::RadarMeasurement m{};

        // Columns: timestamp,track_id,true_x,true_y,meas_x,meas_y,true_vx,true_vy
        if (!getline(lineStream, cell, ',')) return false; m.timestamp = stod(cell);
        if (!getline(lineStream, cell, ',')) return false; m.track_id = stoi(cell);
        if (!getline(lineStream, cell, ',')) return false; m.true_x = stod(cell);
        if (!getline(lineStream, cell, ',')) return false; m.true_y = stod(cell);
        if (!getline(lineStream, cell, ',')) return false; m.meas_x = stod(cell);
        if (!getline(lineStream, cell, ',')) return false; m.meas_y = stod(cell);
        if (!getline(lineStream, cell, ',')) return false; m.true_vx = stod(cell);
        if (!getline(lineStream, cell, ',')) return false; m.true_vy = stod(cell);

        if (m.track_id < 0 || m.track_id >= kTotalTracks ||
            !isfinite(m.timestamp) || m.timestamp < 0 || !isfinite(m.true_x) || !isfinite(m.true_y) ||
            !isfinite(m.meas_x) || !isfinite(m.meas_y) || !isfinite(m.true_vx) || !isfinite(m.true_vy) ||
            (seenTrack[m.track_id] && m.timestamp <= lastTimestamp[m.track_id]) ||
            outData.size() >= kMaxRecords) return false;
        seenTrack[m.track_id] = true;
        lastTimestamp[m.track_id] = m.timestamp;

        outData.push_back(m);
    }

    return !outData.empty();
}

// Exports filtered trajectory to CSV using CreateFileA and WriteFile
bool exportFilteredResultsWin32(const char* filePath, const SharedRadarArena* arena) {
    HANDLE hFile = CreateFileA(filePath, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool openOk = (hFile != INVALID_HANDLE_VALUE);
    LOG_SYSCALL("CreateFileA", openOk, filePath);
    if (!openOk) return false;

    stringstream ss;
    ss << "timestamp,track_id,est_x,est_y,est_vx,est_vy\n";
    ss << fixed << setprecision(3);

    for (uint32_t i = 0; i < arena->recordCount; ++i) {
        const auto& out = arena->outputs[i];
        ss << out.timestamp << ","
           << out.track_id << ","
           << out.est_x << ","
           << out.est_y << ","
           << out.est_vx << ","
           << out.est_vy << "\n";
    }

    string content = ss.str();
    DWORD bytesWritten = 0;
    BOOL writeOk = WriteFile(hFile, content.data(), static_cast<DWORD>(content.size()), &bytesWritten, nullptr);
    bool writeGood = (writeOk && bytesWritten == content.size());
    LOG_SYSCALL("WriteFile", writeGood, filePath);
    bool closeOk = CloseHandle(hFile) != 0;
    LOG_SYSCALL("CloseHandle", closeOk, filePath);
    return writeGood && closeOk;
}

// ----------------------------- Worker Core Logic -----------------------------

// Determines if a track is assigned to a specific worker
inline bool isTrackAssigned(int trackId, int workerId, int totalWorkers) {
    return (trackId % totalWorkers) == workerId;
}

// Processes assigned tracks inside the shared memory arena
void executeWorkerFiltering(SharedRadarArena* arena, int workerId, int totalWorkers) {
    // Each jet track maintains its own 2D Kalman Filter instance
    vector<kalman::Jet2DKalmanFilter> filters(kTotalTracks);

    for (uint32_t i = 0; i < arena->recordCount; ++i) {
        const auto& in = arena->inputs[i];
        int track = in.track_id;

        if (isTrackAssigned(track, workerId, totalWorkers)) {
            filters[track].processMeasurement(in.timestamp, in.meas_x, in.meas_y);

            arena->outputs[i].timestamp = in.timestamp;
            arena->outputs[i].track_id  = track;
            arena->outputs[i].est_x     = filters[track].posX();
            arena->outputs[i].est_y     = filters[track].posY();
            arena->outputs[i].est_vx    = filters[track].velX();
            arena->outputs[i].est_vy    = filters[track].velY();
        }
    }
}

// ----------------------------- CHILD PROCESS MODE ----------------------------

// Worker process entry point: maps shared arena, runs Kalman filter, exits
int runWorkerChild(int argc, char* argv[]) {
    // Arguments: --worker <workerId> <totalWorkers> <shmName>
    if (argc != 5) return 2;

    int workerId     = atoi(argv[2]);
    int totalWorkers = atoi(argv[3]);
    const char* shmName = argv[4];
    if (totalWorkers < 2 || totalWorkers > kTotalTracks || workerId < 0 || workerId >= totalWorkers) return 2;

    // --- CPU Core Affinity Optimization (Option B) ---
    // Locks worker process to dedicated CPU core, preventing OS thread migration & L1/L2 cache thrashing
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    DWORD numCores = sysInfo.dwNumberOfProcessors;
    if (numCores > 0) {
        DWORD_PTR affinityMask = (1ULL << (workerId % min<DWORD>(numCores, 64)));
        SetProcessAffinityMask(GetCurrentProcess(), affinityMask);
    }

    HANDLE hMap = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, shmName);
    if (hMap == nullptr) return 3;

    void* view = MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (view == nullptr) {
        CloseHandle(hMap);
        return 4;
    }

    SharedRadarArena* arena = static_cast<SharedRadarArena*>(view);
    executeWorkerFiltering(arena, workerId, totalWorkers);

    UnmapViewOfFile(view);
    CloseHandle(hMap);
    return 0;
}

// ----------------------------- MULTI-PROCESS ORCHESTRATION -------------------

bool spawnAndJoinWorkers(const string& exePath, const string& shmName, int numWorkers) {
    if (numWorkers <= 1) return true; // Handled sequentially by caller

    vector<HANDLE> processHandles;
    vector<HANDLE> threadHandles;
    processHandles.reserve(numWorkers);
    threadHandles.reserve(numWorkers);

    for (int w = 0; w < numWorkers; ++w) {
        string cmd = "\"" + exePath + "\" --worker " +
                     to_string(w) + " " +
                     to_string(numWorkers) + " " +
                     shmName;

        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        ZeroMemory(&pi, sizeof(pi));

        vector<char> cmdBuffer(cmd.begin(), cmd.end());
        cmdBuffer.push_back('\0');

        BOOL ok = CreateProcessA(nullptr, cmdBuffer.data(), nullptr, nullptr,
                                 FALSE, 0, nullptr, nullptr, &si, &pi);
        if (!ok) {
            // Cleanup already started workers
            for (HANDLE h : processHandles) {
                TerminateProcess(h, 1);
                WaitForSingleObject(h, INFINITE);
                CloseHandle(h);
            }
            for (HANDLE h : threadHandles) CloseHandle(h);
            return false;
        }

        // Apply CPU affinity to child process handle
        SYSTEM_INFO sysInfo;
        GetSystemInfo(&sysInfo);
        DWORD numCores = sysInfo.dwNumberOfProcessors;
        if (numCores > 0) {
            DWORD_PTR affinityMask = (1ULL << (w % min<DWORD>(numCores, 64)));
            SetProcessAffinityMask(pi.hProcess, affinityMask);
        }

        processHandles.push_back(pi.hProcess);
        threadHandles.push_back(pi.hThread);
    }

    // Wait for all workers to finish
    DWORD waitResult = WaitForMultipleObjects(
        static_cast<DWORD>(processHandles.size()),
        processHandles.data(),
        TRUE,
        15000 // 15 seconds safety timeout
    );

    bool allOk = (waitResult >= WAIT_OBJECT_0 && waitResult < WAIT_OBJECT_0 + processHandles.size());
    if (!allOk) {
        for (HANDLE h : processHandles) TerminateProcess(h, 1);
        for (HANDLE h : processHandles) WaitForSingleObject(h, INFINITE);
    }

    // Check exit codes and clean up handles
    for (size_t i = 0; i < processHandles.size(); ++i) {
        DWORD exitCode = 0;
        if (!GetExitCodeProcess(processHandles[i], &exitCode) || exitCode != 0) allOk = false;
        CloseHandle(processHandles[i]);
        CloseHandle(threadHandles[i]);
    }

    return allOk;
}

// ----------------------------- PART 1: DEMONSTRATION & VALIDATION ------------

bool runDemonstration(const string& exePath, const vector<kalman::RadarMeasurement>& data) {
    cout << "---------- PART 1: DEMO & VALIDATION (logged, not timed) ----------\n";

    // 1. Create and map shared memory arena
    string shmName = "Local\\KalmanRadarShm_" + to_string(GetCurrentProcessId());
    HANDLE hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                     0, static_cast<DWORD>(kArenaSize), shmName.c_str());
    bool mapCreated = (hMap != nullptr);
    LOG_SYSCALL("CreateFileMappingA", mapCreated, shmName.c_str());
    if (!mapCreated) return false;

    void* view = MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, kArenaSize);
    bool viewOk = (view != nullptr);
    LOG_SYSCALL("MapViewOfFile", viewOk, "2 MB shared arena");
    if (!viewOk) {
        CloseHandle(hMap);
        return false;
    }

    SharedRadarArena* arena = static_cast<SharedRadarArena*>(view);
    arena->recordCount = static_cast<uint32_t>(data.size());
    arena->trackCount  = kTotalTracks;
    memcpy(arena->inputs, data.data(), data.size() * sizeof(kalman::RadarMeasurement));

    // 2. Run Single-Process Baseline
    cout << "\n[1] Running Single-Process Baseline (1 Worker)...\n";
    executeWorkerFiltering(arena, 0, 1);
    vector<kalman::FilteredOutput> singleProcessOutputs(arena->outputs, arena->outputs + arena->recordCount);

    // Compute RMSE reduction
    auto metrics = kalman::computeRMSE(data, singleProcessOutputs);
    cout << fixed << setprecision(2);
    cout << "  Raw Radar Sensor RMSE    : " << metrics.raw_rmse << " m\n";
    cout << "  Kalman Filtered RMSE     : " << metrics.filtered_rmse << " m\n";
    cout << "  Tracking Error Reduction : " << metrics.noise_reduction_pct << "%\n";
    bool filterEffective = (metrics.filtered_rmse < metrics.raw_rmse && metrics.noise_reduction_pct > 50.0);
    LOG_SYSCALL("VerifyFilterAccuracy", filterEffective, "Noise reduction > 50%");

    // 3. Run Multi-Process Parallel Filtering (4 Workers)
    cout << "\n[2] Running Multi-Process Parallel Filtering (4 Workers via CreateProcess)...\n";
    memset(arena->outputs, 0, sizeof(arena->outputs)); // clear outputs

    bool parallelOk = spawnAndJoinWorkers(exePath, shmName, 4);
    LOG_SYSCALL("CreateProcess_Workers_4", parallelOk, "Spawn 4 child workers");

    // 4. Numerical Parity Verification: Single vs. Parallel output bit-level equality
    cout << "\n[3] Verifying Numerical Parity (Single-Process vs 4-Process Parallel)...\n";
    bool parityExact = true;
    double maxDiff = 0.0;
    for (uint32_t i = 0; i < arena->recordCount; ++i) {
        const auto& actual = arena->outputs[i];
        const auto& expected = singleProcessOutputs[i];
        double differences[] = {abs(actual.est_x - expected.est_x), abs(actual.est_y - expected.est_y),
                                abs(actual.est_vx - expected.est_vx), abs(actual.est_vy - expected.est_vy)};
        bool valid = actual.track_id == expected.track_id && actual.timestamp == expected.timestamp;
        for (double difference : differences) {
            if (!isfinite(difference) || difference > 1e-9) valid = false;
            else maxDiff = max(maxDiff, difference);
        }
        if (!valid) {
            parityExact = false;
            break;
        }
    }
    cout << "  Max Difference Across 8,000 Coordinates: " << scientific << maxDiff << defaultfloat << " m\n";
    LOG_SYSCALL("VerifyParallelParity", parityExact, "Single vs Multi parity < 1e-9");

    // 5. Intentional failure demonstration: OpenFileMapping on non-existent mapping
    cout << "\n[4] Deliberate Failure Handling (OpenFileMapping on non-existent object):\n";
    HANDLE hFail = OpenFileMappingA(FILE_MAP_READ, FALSE, kMissingShmName);
    bool expectedFail = (hFail == nullptr);
    LOG_SYSCALL("OpenFileMappingA_Missing", !expectedFail, kMissingShmName);
    if (hFail) CloseHandle(hFail);

    // Cleanup Demo Mapping
    UnmapViewOfFile(view);
    CloseHandle(hMap);
    return filterEffective && parallelOk && parityExact;
}

// ----------------------------- PART 2: BENCHMARKING & SCALABILITY ------------

bool runScalabilityBenchmarks(const string& exePath, const vector<kalman::RadarMeasurement>& data) {
    cout << "\n---------- PART 2: BENCHMARKS & SCALABILITY (timed, zero logging) ----------\n";

    string shmName = "Local\\KalmanRadarBenchShm_" + to_string(GetCurrentProcessId());
    HANDLE hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                     0, static_cast<DWORD>(kArenaSize), shmName.c_str());
    if (hMap == nullptr) return false;

    void* view = MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, kArenaSize);
    if (view == nullptr) {
        CloseHandle(hMap);
        return false;
    }

    SharedRadarArena* arena = static_cast<SharedRadarArena*>(view);
    arena->recordCount = static_cast<uint32_t>(data.size());
    arena->trackCount  = kTotalTracks;
    memcpy(arena->inputs, data.data(), data.size() * sizeof(kalman::RadarMeasurement));

    int workerConfigs[] = {1, 2, 4, 8};
    double baselineMeanUs = 0.0;

    for (int workers : workerConfigs) {
        string benchName = "KalmanFilter_" + to_string(workers) + (workers == 1 ? "Worker" : "Workers");

        bool workersOk = true;
        auto action = [&]() {
            if (workers == 1) {
                executeWorkerFiltering(arena, 0, 1);
            } else {
                workersOk = spawnAndJoinWorkers(exePath, shmName, workers) && workersOk;
            }
        };

        metrics::Stats stats = metrics::benchmark(benchName, action, kBenchIterations, kBenchWarmup);
        if (!workersOk) {
            cerr << "Worker failure; benchmark results discarded.\n";
            UnmapViewOfFile(view);
            CloseHandle(hMap);
            return false;
        }
        metrics::printStats(stats);
        if (!metrics::appendStatsToCsv(stats, "results/Windows/windows_results.csv")) {
            UnmapViewOfFile(view);
            CloseHandle(hMap);
            return false;
        }

        if (workers == 1) {
            baselineMeanUs = stats.meanUs;
            cout << "  [Baseline] 1 Worker Execution Time: " << fixed << setprecision(2) << baselineMeanUs << " us\n\n";
        } else {
            double speedup = baselineMeanUs / stats.meanUs;
            double efficiency = (speedup / workers) * 100.0;
            cout << fixed << setprecision(2);
            cout << "  Parallel Speedup    : " << speedup << "x (Amdahl scaling)\n";
            cout << "  Parallel Efficiency : " << efficiency << "%\n\n";
        }
    }

    // Export final filtered trajectory
    cout << "Exporting final filtered supersonic trajectory to " << kOutputCsvPath << "...\n";
    bool exported = exportFilteredResultsWin32(kOutputCsvPath, arena);
    bool unmapped = UnmapViewOfFile(view) != 0;
    LOG_SYSCALL("UnmapViewOfFile", unmapped, "benchmark arena");
    bool closed = CloseHandle(hMap) != 0;
    LOG_SYSCALL("CloseHandle", closed, "benchmark mapping");
    return exported && unmapped && closed;
}

// ----------------------------- MAIN DISPATCH ---------------------------------

int main(int argc, char* argv[]) {
    // 1. CHILD WORKER MODE: Must check FIRST before transcript or logging
    if (argc >= 2 && strcmp(argv[1], "--worker") == 0) {
        return runWorkerChild(argc, argv);
    }

    // 2. PART 3: Console transcript tee buffer installation
    ofstream transcript(kTranscriptFile, ios::out | ios::trunc);
    streambuf* originalBuffer = cout.rdbuf();
    TeeBuffer tee(originalBuffer, transcript.rdbuf());

    bool teeActive = false;
    if (transcript.is_open()) {
        cout.rdbuf(&tee);
        teeActive = true;
        cout << "Console output is also being saved to " << kTranscriptFile << "\n";
    }

    int exitCode = 0;
    try {
        logger::init();
        cout << "=== Windows Kalman Filter: Supersonic Fighter Jet Radar Tracker ===\n\n";

        string exePath;
        if (!getOwnExecutablePath(exePath)) {
            cerr << "Error: Could not resolve executable path.\n";
            return 1;
        }

        // Ingest radar dataset
        vector<kalman::RadarMeasurement> telemetry;
        if (!ingestSensorDataWin32(kInputCsvPath, telemetry)) {
            cerr << "Error: Failed to ingest " << kInputCsvPath << ". Run generate_sensor_data.py first.\n";
            return 2;
        }
        cout << "Ingested " << telemetry.size() << " radar pings for " << kTotalTracks << " supersonic fighter jets.\n\n";

        // Execute Demonstration & Parity Checks
        if (!runDemonstration(exePath, telemetry)) {
            cerr << "\nDemo failed or numerical parity validation mismatch!\n";
            return 3;
        }

        // Execute Scalability Benchmarking (1, 2, 4, 8 workers)
        if (!runScalabilityBenchmarks(exePath, telemetry)) {
            cerr << "\nBenchmarking encountered problems!\n";
            return 4;
        }

        cout << "\nPart B Windows execution fully verified, logged, and appended to results.\n";
    } catch (const exception& ex) {
        cout << "Unexpected Exception: " << ex.what() << endl;
        exitCode = 5;
    }

    cout.flush();
    if (teeActive) {
        cout.rdbuf(originalBuffer);
    }
    return exitCode;
}
