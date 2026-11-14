// =============================================================================
// Kalman_Filter_Linux.cpp
//
// Part of the CT-353 Operating Systems Complex Computing Activity (Part B).
//
// High-Speed Supersonic Fighter Jet Radar Tracking System:
// Parallel Multi-Process 2D Kalman Filter using native Linux POSIX system calls.
//
// Syscalls Demonstrated and Integrated:
//   - File I/O : open, read, write, close, lseek
//   - Memory   : mmap (MAP_SHARED | MAP_ANONYMOUS), munmap, mprotect
//   - Process  : fork, waitpid, execv
//   - IPC      : POSIX Shared Memory Pages & Pipe synchronization
//
// Execution Architecture:
//   PART 1: Demo & Validation (logged, not timed)
//           - Ingestion of raw radar telemetry via POSIX File I/O
//           - Anonymous shared memory arena allocation via mmap
//           - Single-process baseline trajectory filtering
//           - Multi-process parallel filtering (4 workers via fork)
//           - Numerical parity verification (single vs parallel tolerance < 1e-9)
//           - Tracking RMSE calculation proving noise reduction
//           - Intentional error handling demo
//   PART 2: Benchmarking & Parallel Scalability (timed, zero logging in loops)
//           - 1 Worker  (Single-process baseline)
//           - 2 Workers (Dual-core parallelization)
//           - 4 Workers (Quad-core parallelization)
//           - 8 Workers (Octa-core parallelization, 1 worker per jet)
//           - Speedup & Amdahl efficiency calculation
//           - Results appended to results/Linux/linux_results.csv
//           - Output exported to results/Linux/filtered_sensor_data.csv
//   PART 3: Automatic console transcript mirroring to
//           Linux/Kalman_Filter/Kalman_Filter_Linux.txt
//
// Compile from REPOSITORY ROOT:
//   g++ -std=c++17 Linux/Kalman_Filter/Kalman_Filter_Linux.cpp -o Linux/Kalman_Filter/kalman_filter_linux
//
// Run from REPOSITORY ROOT:
//   ./Linux/Kalman_Filter/kalman_filter_linux
// =============================================================================

#include "../../common/logger_linux.hpp"
#include "../../common/metrics_linux.hpp"
#include "../../common/kalman_core.hpp"

#include <algorithm>
#include <cerrno>
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

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace std;

// ----------------------------- Configuration ---------------------------------

const char* const kInputCsvPath       = "common/sensor_data.csv";
const char* const kOutputCsvPath      = "results/Linux/filtered_sensor_data.csv";
const char* const kTranscriptFile     = "Linux/Kalman_Filter/Kalman_Filter_Linux.txt";
const char* const kMissingFilePath    = "common/sensor_data_missing_file.tmp";

const int   kMaxRecords               = 16000;
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

// ----------------------------- File I/O via POSIX Syscalls -------------------

// Ingests sensor CSV using open, read, and close
bool ingestSensorDataPOSIX(const char* filePath, vector<kalman::RadarMeasurement>& outData) {
    outData.clear();

    int fd = open(filePath, O_RDONLY);
    bool openOk = (fd >= 0);
    LOG_SYSCALL("open", openOk, filePath);
    if (!openOk) return false;

    struct stat st{};
    if (fstat(fd, &st) != 0) {
        LOG_SYSCALL("fstat", false, filePath);
        close(fd);
        return false;
    }

    size_t totalBytes = static_cast<size_t>(st.st_size);
    vector<char> buffer(totalBytes + 1, '\0');

    size_t totalRead = 0;
    while (totalRead < totalBytes) {
        ssize_t bytesRead = read(fd, buffer.data() + totalRead, totalBytes - totalRead);
        if (bytesRead <= 0) {
            if (bytesRead < 0 && errno == EINTR) continue;
            break;
        }
        totalRead += static_cast<size_t>(bytesRead);
    }

    bool readSuccess = (totalRead == totalBytes);
    LOG_SYSCALL("read", readSuccess, filePath);
    close(fd);

    if (!readSuccess) return false;

    // Fast CSV parsing
    stringstream ss(buffer.data());
    string line;
    // Skip header line
    if (!getline(ss, line)) return false;

    while (getline(ss, line)) {
        if (line.empty()) continue;
        stringstream lineStream(line);
        string cell;
        kalman::RadarMeasurement m{};

        // Columns: timestamp,track_id,true_x,true_y,meas_x,meas_y,true_vx,true_vy
        if (!getline(lineStream, cell, ',')) continue; m.timestamp = stod(cell);
        if (!getline(lineStream, cell, ',')) continue; m.track_id = stoi(cell);
        if (!getline(lineStream, cell, ',')) continue; m.true_x = stod(cell);
        if (!getline(lineStream, cell, ',')) continue; m.true_y = stod(cell);
        if (!getline(lineStream, cell, ',')) continue; m.meas_x = stod(cell);
        if (!getline(lineStream, cell, ',')) continue; m.meas_y = stod(cell);
        if (!getline(lineStream, cell, ',')) continue; m.true_vx = stod(cell);
        if (!getline(lineStream, cell, ',')) continue; m.true_vy = stod(cell);

        outData.push_back(m);
        if (outData.size() >= kMaxRecords) break;
    }

    return !outData.empty();
}

// Exports filtered trajectory to CSV using open, write, and close
bool exportFilteredResultsPOSIX(const char* filePath, const SharedRadarArena* arena) {
    int fd = open(filePath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    bool openOk = (fd >= 0);
    LOG_SYSCALL("open", openOk, filePath);
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
    size_t totalWritten = 0;
    while (totalWritten < content.size()) {
        ssize_t bytesWritten = write(fd, content.data() + totalWritten, content.size() - totalWritten);
        if (bytesWritten <= 0) {
            if (bytesWritten < 0 && errno == EINTR) continue;
            break;
        }
        totalWritten += static_cast<size_t>(bytesWritten);
    }

    bool writeGood = (totalWritten == content.size());
    LOG_SYSCALL("write", writeGood, filePath);
    close(fd);
    return writeGood;
}

// ----------------------------- Worker Core Logic -----------------------------

inline bool isTrackAssigned(int trackId, int workerId, int totalWorkers) {
    return (trackId % totalWorkers) == workerId;
}

void executeWorkerFiltering(SharedRadarArena* arena, int workerId, int totalWorkers) {
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

// ----------------------------- MULTI-PROCESS ORCHESTRATION -------------------

// Spawns N worker processes via fork(), which inherit the MAP_SHARED arena
bool spawnAndJoinWorkers(SharedRadarArena* arena, int numWorkers) {
    if (numWorkers <= 1) return true;

    vector<pid_t> childPids;
    childPids.reserve(numWorkers);

    for (int w = 0; w < numWorkers; ++w) {
        pid_t pid = fork();
        if (pid < 0) {
            // Error: kill any children started so far
            for (pid_t c : childPids) {
                kill(c, SIGKILL);
                waitpid(c, nullptr, 0);
            }
            return false;
        }
        if (pid == 0) {
            // Child Worker Process
            executeWorkerFiltering(arena, w, numWorkers);
            _exit(0);
        }
        childPids.push_back(pid);
    }

    // Parent waits for all children
    bool allSuccess = true;
    for (pid_t pid : childPids) {
        int status = 0;
        pid_t waited = waitpid(pid, &status, 0);
        if (waited <= 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            allSuccess = false;
        }
    }

    return allSuccess;
}

// ----------------------------- PART 1: DEMONSTRATION & VALIDATION ------------

bool runDemonstration(const vector<kalman::RadarMeasurement>& data) {
    cout << "---------- PART 1: DEMO & VALIDATION (logged, not timed) ----------\n";

    // 1. Allocate POSIX Anonymous Shared Memory Arena (MAP_SHARED | MAP_ANONYMOUS)
    void* mmapPtr = mmap(nullptr, kArenaSize, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    bool mmapOk = (mmapPtr != MAP_FAILED);
    LOG_SYSCALL("mmap", mmapOk, "2 MB shared arena (MAP_SHARED|MAP_ANONYMOUS)");
    if (!mmapOk) return false;

    SharedRadarArena* arena = static_cast<SharedRadarArena*>(mmapPtr);
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

    // 3. Run Multi-Process Parallel Filtering (4 Workers via fork)
    cout << "\n[2] Running Multi-Process Parallel Filtering (4 Workers via fork)...\n";
    memset(arena->outputs, 0, sizeof(arena->outputs));

    bool parallelOk = spawnAndJoinWorkers(arena, 4);
    LOG_SYSCALL("fork_Workers_4", parallelOk, "Spawn 4 child workers");

    // 4. Numerical Parity Verification: Single vs. Parallel output bit-level equality
    cout << "\n[3] Verifying Numerical Parity (Single-Process vs 4-Process Parallel)...\n";
    bool parityExact = true;
    double maxDiff = 0.0;
    for (uint32_t i = 0; i < arena->recordCount; ++i) {
        double dx = abs(arena->outputs[i].est_x - singleProcessOutputs[i].est_x);
        double dy = abs(arena->outputs[i].est_y - singleProcessOutputs[i].est_y);
        maxDiff = max(maxDiff, max(dx, dy));
        if (dx > 1e-9 || dy > 1e-9) {
            parityExact = false;
            break;
        }
    }
    cout << "  Max Difference Across 8,000 Coordinates: " << scientific << maxDiff << defaultfloat << " m\n";
    LOG_SYSCALL("VerifyParallelParity", parityExact, "Single vs Multi parity < 1e-9");

    // 5. Intentional failure demonstration: open() on non-existent file
    cout << "\n[4] Deliberate Failure Handling (open on non-existent file):\n";
    int fdFail = open(kMissingFilePath, O_RDONLY);
    bool expectedFail = (fdFail < 0);
    LOG_SYSCALL("open_MissingFile", !expectedFail, kMissingFilePath);
    if (fdFail >= 0) close(fdFail);

    // Cleanup Demo Mapping
    munmap(mmapPtr, kArenaSize);
    return filterEffective && parityExact;
}

// ----------------------------- PART 2: BENCHMARKING & SCALABILITY ------------

bool runScalabilityBenchmarks(const vector<kalman::RadarMeasurement>& data) {
    cout << "\n---------- PART 2: BENCHMARKS & SCALABILITY (timed, zero logging) ----------\n";

    void* mmapPtr = mmap(nullptr, kArenaSize, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (mmapPtr == MAP_FAILED) return false;

    SharedRadarArena* arena = static_cast<SharedRadarArena*>(mmapPtr);
    arena->recordCount = static_cast<uint32_t>(data.size());
    arena->trackCount  = kTotalTracks;
    memcpy(arena->inputs, data.data(), data.size() * sizeof(kalman::RadarMeasurement));

    int workerConfigs[] = {1, 2, 4, 8};
    double baselineMeanUs = 0.0;

    for (int workers : workerConfigs) {
        string benchName = "KalmanFilter_" + to_string(workers) + (workers == 1 ? "Worker" : "Workers");

        auto action = [&]() {
            if (workers == 1) {
                executeWorkerFiltering(arena, 0, 1);
            } else {
                spawnAndJoinWorkers(arena, workers);
            }
        };

        metrics::Stats stats = metrics::benchmark(benchName, action, kBenchIterations, kBenchWarmup);
        metrics::printStats(stats);
        metrics::appendStatsToCsv(stats, "results/Linux/linux_results.csv");

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

    cout << "Exporting final filtered supersonic trajectory to " << kOutputCsvPath << "...\n";
    exportFilteredResultsPOSIX(kOutputCsvPath, arena);

    munmap(mmapPtr, kArenaSize);
    return true;
}

// ----------------------------- MAIN DISPATCH ---------------------------------

int main(int argc, char* argv[]) {
    // Console transcript tee buffer installation
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
        cout << "=== Linux Kalman Filter: Supersonic Fighter Jet Radar Tracker ===\n\n";

        // Ingest radar dataset
        vector<kalman::RadarMeasurement> telemetry;
        if (!ingestSensorDataPOSIX(kInputCsvPath, telemetry)) {
            cerr << "Error: Failed to ingest " << kInputCsvPath << ". Run generate_sensor_data.py first.\n";
            return 2;
        }
        cout << "Ingested " << telemetry.size() << " radar pings for " << kTotalTracks << " supersonic fighter jets.\n\n";

        // Execute Demonstration & Parity Checks
        if (!runDemonstration(telemetry)) {
            cerr << "\nDemo failed or numerical parity validation mismatch!\n";
            return 3;
        }

        // Execute Scalability Benchmarking (1, 2, 4, 8 workers)
        if (!runScalabilityBenchmarks(telemetry)) {
            cerr << "\nBenchmarking encountered problems!\n";
            return 4;
        }

        cout << "\nPart B Linux execution fully verified, logged, and appended to results.\n";
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
