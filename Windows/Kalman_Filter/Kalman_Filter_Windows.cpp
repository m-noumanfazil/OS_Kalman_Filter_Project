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
#include <chrono>
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
    auto parseDouble = [](const string& value) {
        size_t used = 0;
        double number = stod(value, &used);
        if (value.find_first_not_of(" \t\r", used) != string::npos) throw invalid_argument("invalid numeric field");
        return number;
    };
    auto parseInt = [](const string& value) {
        size_t used = 0;
        int number = stoi(value, &used);
        if (value.find_first_not_of(" \t\r", used) != string::npos) throw invalid_argument("invalid integer field");
        return number;
    };
    while (getline(ss, line)) {
        if (line.empty()) continue;
        stringstream lineStream(line);
        string cell;
        kalman::RadarMeasurement m{};

        // Columns: timestamp,track_id,true_x,true_y,meas_x,meas_y,true_vx,true_vy
        if (!getline(lineStream, cell, ',')) return false; m.timestamp = parseDouble(cell);
        if (!getline(lineStream, cell, ',')) return false; m.track_id = parseInt(cell);
        if (!getline(lineStream, cell, ',')) return false; m.true_x = parseDouble(cell);
        if (!getline(lineStream, cell, ',')) return false; m.true_y = parseDouble(cell);
        if (!getline(lineStream, cell, ',')) return false; m.meas_x = parseDouble(cell);
        if (!getline(lineStream, cell, ',')) return false; m.meas_y = parseDouble(cell);
        if (!getline(lineStream, cell, ',')) return false; m.true_vx = parseDouble(cell);
        if (!getline(lineStream, cell, ',')) return false; m.true_vy = parseDouble(cell);
        if (getline(lineStream, cell, ',')) return false;

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

    int workerId, totalWorkers;
    try {
        size_t usedId = 0, usedCount = 0;
        workerId = stoi(argv[2], &usedId);
        totalWorkers = stoi(argv[3], &usedCount);
        if (usedId != strlen(argv[2]) || usedCount != strlen(argv[3])) return 2;
    } catch (const exception&) { return 2; }
    const char* shmName = argv[4];
    if (totalWorkers < 2 || totalWorkers > kTotalTracks || workerId < 0 || workerId >= totalWorkers) return 2;
#ifdef KALMAN_TEST_FAILURES
    if (strcmp(argv[1], "--pool") == 0 && workerId == 0 && getenv("KALMAN_TEST_FAIL_POOL_WORKER")) return 91;
#endif

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
    if (strcmp(argv[1], "--pool") == 0) {
        string prefix = string(shmName);
        string suffix = to_string(workerId);
        HANDLE go = OpenEventA(SYNCHRONIZE, FALSE, (prefix + "_go_" + suffix).c_str());
        HANDLE done = OpenEventA(EVENT_MODIFY_STATE, FALSE, (prefix + "_done_" + suffix).c_str());
        HANDLE stop = OpenEventA(SYNCHRONIZE, FALSE, (prefix + "_stop").c_str());
        if (!go || !done || !stop) {
            if (go) CloseHandle(go); if (done) CloseHandle(done); if (stop) CloseHandle(stop);
            UnmapViewOfFile(view); CloseHandle(hMap); return 5;
        }
        HANDLE waits[] = {stop, go};
        for (;;) {
            DWORD result = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (result == WAIT_OBJECT_0) break;
            if (result != WAIT_OBJECT_0 + 1) { CloseHandle(go); CloseHandle(done); CloseHandle(stop); UnmapViewOfFile(view); CloseHandle(hMap); return 6; }
            executeWorkerFiltering(arena, workerId, totalWorkers);
            if (!SetEvent(done)) { CloseHandle(go); CloseHandle(done); CloseHandle(stop); UnmapViewOfFile(view); CloseHandle(hMap); return 7; }
        }
        CloseHandle(go); CloseHandle(done); CloseHandle(stop);
    } else {
        executeWorkerFiltering(arena, workerId, totalWorkers);
    }

    bool unmapped = UnmapViewOfFile(view) != 0;
    bool closed = CloseHandle(hMap) != 0;
    return unmapped && closed ? 0 : 8;
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

// Named events signal full independent trials to long-lived workers.
class PersistentWorkers {
    vector<HANDLE> processes_, go_, done_;
    HANDLE stop_ = nullptr;
    bool healthy_ = true;
public:
    PersistentWorkers(const string& exe, const string& name, int count) {
        if (!count) return;
        stop_ = CreateEventA(nullptr, TRUE, FALSE, (name + "_stop").c_str());
        if (!stop_ || GetLastError() == ERROR_ALREADY_EXISTS) { healthy_ = false; return; }
        for (int w = 0; w < count; ++w) {
            string suffix = to_string(w);
            HANDLE go = CreateEventA(nullptr, FALSE, FALSE, (name + "_go_" + suffix).c_str());
            bool goNew = go && GetLastError() != ERROR_ALREADY_EXISTS;
            HANDLE done = CreateEventA(nullptr, FALSE, FALSE, (name + "_done_" + suffix).c_str());
            bool doneNew = done && GetLastError() != ERROR_ALREADY_EXISTS;
            if (!goNew || !doneNew) { if (go) CloseHandle(go); if (done) CloseHandle(done); healthy_ = false; break; }
            go_.push_back(go); done_.push_back(done);
            string cmd = "\"" + exe + "\" --pool " + suffix + " " + to_string(count) + " " + name;
            vector<char> buffer(cmd.begin(), cmd.end()); buffer.push_back('\0');
            STARTUPINFOA si{}; si.cb = sizeof(si); PROCESS_INFORMATION pi{};
            if (!CreateProcessA(nullptr, buffer.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                healthy_ = false; break;
            }
            CloseHandle(pi.hThread); processes_.push_back(pi.hProcess);
        }
    }
    bool ready() const { return healthy_; }
    double workerCpuUs() const {
        double total = 0;
        for (HANDLE process : processes_) {
            FILETIME creation, exit, kernel, user;
            if (!GetProcessTimes(process, &creation, &exit, &kernel, &user)) return -1;
            ULARGE_INTEGER k{}, u{};
            k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
            u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
            total += static_cast<double>(k.QuadPart + u.QuadPart) / 10.0;
        }
        return total;
    }
    bool run() {
        if (!healthy_) return false;
        for (HANDLE event : go_) if (!SetEvent(event)) return healthy_ = false;
        for (size_t i = 0; i < processes_.size(); ++i) {
            HANDLE waits[] = {processes_[i], done_[i]};
            // A dead worker takes precedence over a possibly signaled event.
            if (WaitForMultipleObjects(2, waits, FALSE, 15000) != WAIT_OBJECT_0 + 1)
                return healthy_ = false;
        }
        return true;
    }
    ~PersistentWorkers() {
        if (stop_) SetEvent(stop_);
        for (HANDLE process : processes_) {
            if (WaitForSingleObject(process, 15000) != WAIT_OBJECT_0) {
                TerminateProcess(process, 1); WaitForSingleObject(process, INFINITE);
            }
            CloseHandle(process);
        }
        for (HANDLE event : go_) CloseHandle(event);
        for (HANDLE event : done_) CloseHandle(event);
        if (stop_) CloseHandle(stop_);
    }
};

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

    bool parallelOk = true;
    bool parityExact = true;
    for (int workers : {2, 4, 8}) {
    cout << "\nVerifying " << workers << " workers...\n";
    memset(arena->outputs, 0, sizeof(arena->outputs));
    bool workerResult = spawnAndJoinWorkers(exePath, shmName, workers);
    LOG_SYSCALL("Workers", workerResult, "parallel worker run");
    parallelOk = parallelOk && workerResult;

    // 4. Numerical Parity Verification: Single vs. Parallel output bit-level equality
    cout << "Verifying numerical parity against single-process output...\n";
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
    cout << "  Maximum state difference: " << scientific << maxDiff << defaultfloat << "\n";
    LOG_SYSCALL("VerifyParallelParity", parityExact, "Single vs Multi parity < 1e-9");
    }

    // 5. Intentional failure demonstration: OpenFileMapping on non-existent mapping
    cout << "\n[4] Deliberate Failure Handling (OpenFileMapping on non-existent object):\n";
    HANDLE hFail = OpenFileMappingA(FILE_MAP_READ, FALSE, kMissingShmName);
    bool expectedFail = (hFail == nullptr);
    LOG_SYSCALL("OpenFileMappingA_Missing", !expectedFail, kMissingShmName);
    if (hFail) CloseHandle(hFail);

    // Cleanup Demo Mapping
    bool unmapped = UnmapViewOfFile(view) != 0;
    LOG_SYSCALL("UnmapViewOfFile", unmapped, "demonstration arena");
    bool closed = CloseHandle(hMap) != 0;
    LOG_SYSCALL("CloseHandle", closed, "demonstration mapping");
    return filterEffective && parallelOk && parityExact && unmapped && closed;
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
    const auto runId = chrono::duration_cast<chrono::nanoseconds>(
        chrono::system_clock::now().time_since_epoch()).count();
    vector<size_t> workloads = {max<size_t>(1, data.size() / 4), max<size_t>(1, data.size() / 2), data.size()};
    workloads.erase(unique(workloads.begin(), workloads.end()), workloads.end());
    for (size_t workload : workloads) {
    arena->recordCount = static_cast<uint32_t>(workload);
    double baselineMeanUs = 0.0;
    executeWorkerFiltering(arena, 0, 1);
    vector<kalman::FilteredOutput> reference(arena->outputs, arena->outputs + arena->recordCount);

    for (int workers : workerConfigs) {
        string benchName = "KalmanFilter_" + to_string(workers) + (workers == 1 ? "Worker" : "Workers") + "_" + to_string(workload) + "Records";

        auto startupBegin = chrono::steady_clock::now();
        PersistentWorkers pool(exePath, shmName, workers == 1 ? 0 : workers);
        auto startupEnd = chrono::steady_clock::now();
        double launchUs = chrono::duration<double, micro>(startupEnd - startupBegin).count();
        cout << "  Worker launch (us; excludes readiness): " << launchUs << "\n";
        bool workersOk = pool.ready();
        if (workers > 1 && workersOk) {
            memset(arena->outputs, 0, sizeof(arena->outputs));
            workersOk = pool.run();
            for (uint32_t i = 0; workersOk && i < arena->recordCount; ++i) {
                const auto& a = arena->outputs[i];
                const auto& b = reference[i];
                double diffs[] = {abs(a.est_x - b.est_x), abs(a.est_y - b.est_y),
                                  abs(a.est_vx - b.est_vx), abs(a.est_vy - b.est_vy)};
                workersOk = a.track_id == b.track_id && a.timestamp == b.timestamp;
                for (double d : diffs) if (!isfinite(d) || d > 1e-9) workersOk = false;
            }
        }
        if (!workersOk) {
            cerr << "Persistent worker startup or parity failed.\n";
            UnmapViewOfFile(view); CloseHandle(hMap);
            return false;
        }
        auto action = [&]() {
            if (workers == 1) {
                executeWorkerFiltering(arena, 0, 1);
            } else {
                workersOk = pool.run() && workersOk;
            }
        };

        double workerBefore = 0;
        metrics::Stats stats = metrics::benchmark(benchName, action, kBenchIterations, kBenchWarmup,
                                                  nullptr, [&]() { workerBefore = pool.workerCpuUs(); });
        double workerAfter = pool.workerCpuUs();
        double workerCpu = workerBefore >= 0 && workerAfter >= 0 ? workerAfter - workerBefore : -1;
        if (!workersOk) {
            cerr << "Worker failure; benchmark results discarded.\n";
            UnmapViewOfFile(view);
            CloseHandle(hMap);
            return false;
        }
        metrics::printStats(stats);
        if (!metrics::appendStatsToCsv(stats, "results/Windows/persistent_summary.csv")) {
            UnmapViewOfFile(view);
            CloseHandle(hMap);
            return false;
        }

        {
            const char* samplePath = "results/Windows/persistent_samples.csv";
            const string expectedHeader = "run_id,platform,compiler,records,workers,iteration,elapsed_us,launch_us,parent_cpu_us,worker_cpu_us";
            ifstream existing(samplePath);
            string header;
            bool headerNeeded = !getline(existing, header) || header.empty();
            if (!header.empty() && header.back() == '\r') header.pop_back();
            if (!headerNeeded && header != expectedHeader) {
                cerr << "Incompatible benchmark sample header; choose a fresh output file.\n";
                UnmapViewOfFile(view); CloseHandle(hMap);
                return false;
            }
            existing.close();
            ofstream samples(samplePath, ios::app);
            if (!samples) { cerr << "Cannot write benchmark samples.\n"; UnmapViewOfFile(view); CloseHandle(hMap); return false; }
            if (headerNeeded) samples << expectedHeader << "\n";
            for (size_t i = 0; i < stats.samplesUs.size(); ++i)
                samples << runId << ",Windows,\"" << __VERSION__ << "\"," << workload << ","
                        << workers << "," << (i + 1) << "," << setprecision(17) << stats.samplesUs[i] << "," << launchUs << "," << stats.totalCpuUs << "," << workerCpu << "\n";
            samples.flush();
            if (!samples) { cerr << "Failed writing benchmark samples.\n"; UnmapViewOfFile(view); CloseHandle(hMap); return false; }
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
    if (argc >= 2 && (strcmp(argv[1], "--worker") == 0 || strcmp(argv[1], "--pool") == 0)) {
        return runWorkerChild(argc, argv);
    }

    // 2. PART 3: Console transcript tee buffer installation
    ofstream transcript(kTranscriptFile, ios::out | ios::trunc);
    streambuf* originalBuffer = cout.rdbuf();
    TeeBuffer tee(originalBuffer, transcript.rdbuf());
    struct RestoreCout {
        streambuf* original;
        ~RestoreCout() { cout.rdbuf(original); }
    } restoreCout{originalBuffer};

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
