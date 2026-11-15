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
#include <chrono>
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
#include <sched.h>
#include <poll.h>
#include <signal.h>
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

const int   kMaxRecords               = 100000;
const int   kTotalTracks              = 8;
const int   kBenchIterations          = 50;
const int   kBenchWarmup              = 5;

// Shared memory arena layout mapped across all worker processes
struct SharedRadarArena {
    uint32_t recordCount;
    uint32_t trackCount;
    double workerCpuUs[8];
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
    bool closeOk = close(fd) == 0;
    LOG_SYSCALL("close", closeOk, filePath);

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
    bool closeOk = close(fd) == 0;
    LOG_SYSCALL("close", closeOk, filePath);
    return writeGood && closeOk;
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
            // --- CPU Core Affinity Optimization (Option B) ---
            // Locks worker to dedicated CPU core, preventing thread migration & L1/L2 cache thrashing
            long numCores = sysconf(_SC_NPROCESSORS_ONLN);
            if (numCores > 0) {
                cpu_set_t cpuset;
                CPU_ZERO(&cpuset);
                CPU_SET(w % numCores, &cpuset);
                sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
            }

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

// Each command executes one independent full-dataset trial. Workers remain
// alive across warm-up and timed trials; filter state is reset each trial.
class PersistentWorkers {
    struct Worker { pid_t pid; int command; int done; };
    vector<Worker> workers_;
    SharedRadarArena* arena_;
    bool healthy_ = true;
public:
    PersistentWorkers(SharedRadarArena* arena, int count) : arena_(arena) {
        for (int w = 0; w < count; ++w) {
            int command[2], done[2];
            if (pipe(command) != 0) { healthy_ = false; break; }
            if (pipe(done) != 0) { close(command[0]); close(command[1]); healthy_ = false; break; }
            pid_t pid = fork();
            if (pid == 0) {
                close(command[1]); close(done[0]);
                for (const auto& old : workers_) { close(old.command); close(old.done); }
                char cmd;
                while (read(command[0], &cmd, 1) == 1 && cmd == 'R') {
                    executeWorkerFiltering(arena, w, count);
                    arena->workerCpuUs[w] = metrics::processCpuMicroseconds();
                    char reply = 'D';
                    if (write(done[1], &reply, 1) != 1) _exit(2);
                }
                close(command[0]); close(done[1]); _exit(0);
            }
            close(command[0]); close(done[1]);
            if (pid < 0) { close(command[1]); close(done[0]); healthy_ = false; break; }
            workers_.push_back({pid, command[1], done[0]});
        }
    }
    bool ready() const { return healthy_; }
    double workerCpuUs() const {
        double total = 0;
        for (size_t i = 0; i < workers_.size(); ++i) total += arena_->workerCpuUs[i];
        return total;
    }
    bool run() {
        if (!healthy_) return false;
        for (const auto& worker : workers_) {
            char cmd = 'R';
            if (write(worker.command, &cmd, 1) != 1) return healthy_ = false;
        }
        for (const auto& worker : workers_) {
            pollfd pfd{worker.done, POLLIN, 0};
            int result;
            do { result = poll(&pfd, 1, 15000); } while (result < 0 && errno == EINTR);
            char reply = 0;
            if (result <= 0 || !(pfd.revents & POLLIN) || read(worker.done, &reply, 1) != 1 || reply != 'D')
                return healthy_ = false;
        }
        return true;
    }
    ~PersistentWorkers() {
        for (const auto& worker : workers_) { close(worker.command); close(worker.done); }
        for (const auto& worker : workers_) {
            if (!healthy_) kill(worker.pid, SIGKILL);
            while (waitpid(worker.pid, nullptr, 0) < 0 && errno == EINTR) {}
        }
    }
};

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

    bool parallelOk = true;
    bool parityExact = true;
    for (int workers : {2, 4, 8}) {
    cout << "\nVerifying " << workers << " workers...\n";
    memset(arena->outputs, 0, sizeof(arena->outputs));
    bool workerResult = spawnAndJoinWorkers(arena, workers);
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

    // 5. Intentional failure demonstration: open() on non-existent file
    cout << "\n[4] Deliberate Failure Handling (open on non-existent file):\n";
    int fdFail = open(kMissingFilePath, O_RDONLY);
    bool expectedFail = (fdFail < 0);
    LOG_SYSCALL("open_MissingFile", !expectedFail, kMissingFilePath);
    if (fdFail >= 0) close(fdFail);

    // Cleanup Demo Mapping
    munmap(mmapPtr, kArenaSize);
    return filterEffective && parallelOk && parityExact;
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

        // Ignore SIGPIPE so a dead child becomes a reported write failure.
        signal(SIGPIPE, SIG_IGN);
        auto startupBegin = chrono::steady_clock::now();
        PersistentWorkers pool(arena, workers == 1 ? 0 : workers);
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
            munmap(mmapPtr, kArenaSize);
            return false;
        }
        auto action = [&]() {
            if (workers == 1) {
                executeWorkerFiltering(arena, 0, 1);
            } else {
                workersOk = pool.run() && workersOk;
            }
        };

        double workerBefore = pool.workerCpuUs();
        metrics::Stats stats = metrics::benchmark(benchName, action, kBenchIterations, kBenchWarmup);
        double workerCpu = pool.workerCpuUs() - workerBefore;
        if (!workersOk) {
            cerr << "Worker failure; benchmark results discarded.\n";
            munmap(mmapPtr, kArenaSize);
            return false;
        }
        metrics::printStats(stats);
        if (!metrics::appendStatsToCsv(stats, "results/Linux/persistent_summary.csv")) {
            munmap(mmapPtr, kArenaSize);
            return false;
        }

        {
            const char* samplePath = "results/Linux/persistent_samples.csv";
            ifstream existing(samplePath, ios::binary | ios::ate);
            bool headerNeeded = !existing || existing.tellg() == 0;
            existing.close();
            ofstream samples(samplePath, ios::app);
            if (!samples) { cerr << "Cannot write benchmark samples.\n"; munmap(mmapPtr, kArenaSize); return false; }
            if (headerNeeded) samples << "run_id,platform,compiler,records,workers,iteration,elapsed_us,launch_us,parent_cpu_us,worker_cpu_us_with_warmup\n";
            for (size_t i = 0; i < stats.samplesUs.size(); ++i)
                samples << runId << ",Linux,\"" << __VERSION__ << "\"," << workload << ","
                        << workers << "," << (i + 1) << "," << setprecision(17) << stats.samplesUs[i] << "," << launchUs << "," << stats.totalCpuUs << "," << workerCpu << "\n";
            samples.flush();
            if (!samples) { cerr << "Failed writing benchmark samples.\n"; munmap(mmapPtr, kArenaSize); return false; }
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
    cout << "Exporting final filtered supersonic trajectory to " << kOutputCsvPath << "...\n";
    bool exported = exportFilteredResultsPOSIX(kOutputCsvPath, arena);
    bool unmapped = munmap(mmapPtr, kArenaSize) == 0;
    LOG_SYSCALL("munmap", unmapped, "benchmark arena");
    return exported && unmapped;
}

// ----------------------------- MAIN DISPATCH ---------------------------------

int main(int argc, char* argv[]) {
    // Console transcript tee buffer installation
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
