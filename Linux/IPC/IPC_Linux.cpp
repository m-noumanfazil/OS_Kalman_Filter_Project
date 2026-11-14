// =============================================================================
// IPC_Linux.cpp
//
// Part of an OS project comparing Linux and Windows system calls (Linux side,
// IPC category). One-for-one port of IPC_Windows.cpp.
//
// What this program does:
//   PART 1  Demo (logged, not timed):
//             A. pipe: parent writes 4096 bytes, a child process (this same
//                exe, started with fork + execv) reads and verifies them, then
//                sees end-of-data
//             B. System V shared memory: parent and child exchange data in BOTH
//                directions through one segment
//             C. one deliberate failure (shmget on a key that does not exist)
//   PART 2  Benchmarks (1000 iterations, 50 warm-up each), timed separately,
//           all inside ONE process (no child is started):
//             pipe, pipe_write_4KB, pipe_write_64KB, pipe_read_4KB,
//             pipe_read_64KB, shmget_1MB, shmat_1MB, shmdt_64KB
//           Results are printed and appended to results/Linux/linux_results.csv.
//   PART 3  Everything printed to the console is also saved to
//           Linux/IPC/IPC_Linux.txt (overwritten on every run).
//
// The child is THIS SAME EXE, started with "--child-pipe <fd> <length>" or
// "--child-shm <id>". main() checks these flags FIRST (see main).
//
// Windows call -> Linux call (name mapping for the report):
//   +-------------------------------------+-----------------------------------+
//   | Windows                             | Linux                             |
//   +-------------------------------------+-----------------------------------+
//   | CreatePipe                          | pipe(fds)                         |
//   | WriteFile / ReadFile on a pipe      | write / read                      |
//   | CloseHandle on a pipe end           | close                             |
//   | CreateFileMapping(1 MB)             | shmget(IPC_PRIVATE, 1 MB, ...)    |
//   | MapViewOfFile                       | shmat(id, nullptr, 0)             |
//   | UnmapViewOfFile                     | shmdt(address)                    |
//   | CloseHandle(mapping)                | shmctl(id, IPC_RMID, nullptr)     |
//   | OpenFileMapping (missing name)      | shmget(missingKey, no IPC_CREAT)  |
//   | SetHandleInformation (inherit)      | not needed (fork/exec keep fds)   |
//   | CreateProcess                       | fork + execv                      |
//   | WaitForSingleObject / ExitCode      | waitpid / WEXITSTATUS             |
//   +-------------------------------------+-----------------------------------+
//
// SYSTEM V SHARED MEMORY STAYS IN THE KERNEL until it is removed with
// shmctl(IPC_RMID). If a run is ever killed half-way, a segment can be left
// behind. List the segments with "ipcs -m" and remove the leftover one with:
//     ipcrm -m <id>
// (the demo prints the id of its segment).
//
// Compile and run from the REPOSITORY ROOT:
//   g++ -std=c++17 Linux/IPC/IPC_Linux.cpp -o Linux/IPC/ipc_linux
//   ./Linux/IPC/ipc_linux
//
// Exit codes of the main program: 0 = everything OK, 1 = demo failed,
// 2 = a benchmark had problems.
// =============================================================================

#include "../../common/logger_linux.hpp"
#include "../../common/metrics_linux.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <fstream>
#include <iostream>
#include <streambuf>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace std;

// ----------------------------- Settings --------------------------------------

const char* const kTranscriptFile = "Linux/IPC/IPC_Linux.txt";

// A System V key that no segment uses, for the deliberate failure.
const key_t kMissingShmKey = 0x4F534B4D;

const int    kIterations = 1000;
const int    kWarmup     = 50;
// The warm-up runs the action too, so every benchmark that consumes one
// prepared item per call needs warm-up + iterations items (1050).
const size_t kTotalCalls = kWarmup + kIterations;

// Every wait has a timeout, never an unbounded block.
const long long kWaitTimeoutMs = 10000;
// Polling: spin (with sched_yield) for the first 50 ms, then sleep 1 ms steps.
const long long kSpinPhaseNs = 50LL * 1000000LL;

// A descriptor count more than this much higher after Part 2 means a leak.
const int kDescriptorLeakTolerance = 5;

const size_t kSize4KB  = 4 * 1024;
const size_t kSize64KB = 64 * 1024;
const size_t kSize1MB  = 1024 * 1024;
const size_t kHalfSize = kSize1MB / 2;          // 512 KB: each side of the shared block
const size_t kDemoPipeMessageSize = 4096;

// The benchmark pipes are enlarged to 1 MB. If that fails we only continue if
// the pipe is at least this big (a 64 KB write needs room to spare).
const int kWantedPipeSize = 1024 * 1024;
const int kMinPipeSize    = 128 * 1024;

// TOTAL CHILD PROCESSES STARTED BY THE WHOLE PROGRAM: 2 (one per demo test).
// The benchmarks start no child at all.

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

// One byte of the deterministic pattern: first, first+1, ... first+25, repeat.
// 'A' is used for the parent's data, 'a' (lowercase) for the child's data.
char patternChar(size_t index, char first) {
    return static_cast<char>(first + (index % 26));
}

// Deterministic test data: A-Z repeated. The parent and the pipe child both
// use this same function, so they always agree on what the message is.
vector<char> buildTestData(size_t size) {
    vector<char> data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = patternChar(i, 'A');
    }
    return data;
}

// Returns true if read()/write() worked AND moved the expected number of bytes.
// In general read() and write() are ALLOWED to return fewer bytes than
// requested. But for these tests a short count means something is wrong, so it
// is treated as a failure. A short count has no errno of its own, so we set
// EIO; otherwise the log line would show a stale, unrelated error.
bool transferSucceeded(ssize_t result, size_t expected) {
    if (result >= 0 && static_cast<size_t>(result) != expected) {
        errno = EIO;
        return false;
    }
    return result >= 0;
}

// Counts failures inside benchmark lambdas. Lambdas must not log (logging
// would ruin the timing), so they only record here; the result is reported
// once, after the benchmark.
struct FailureTracker {
    int count = 0;
    int firstError = 0;

    // apiFailed = true : a system call failed, keep its errno.
    // apiFailed = false: some other problem (for example a wrong byte count).
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

// Monotonic clock in nanoseconds (used for deadlines).
long long monotonicNs() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<long long>(now.tv_sec) * 1000000000LL + now.tv_nsec;
}

void sleepOneMillisecond() {
    timespec pause = {0, 1000000};
    nanosleep(&pause, nullptr);
}

// Full path of this exe (used for execv, so the child is the same program).
bool getOwnPath(string& path) {
    char buffer[4096];
    ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    bool ok = (length > 0);
    LOG_SYSCALL("readlink", ok, "/proc/self/exe");
    if (ok) {
        path.assign(buffer, static_cast<size_t>(length));
    }
    return ok;
}

// ----------------------------- Timeout and cleanup helpers -------------------
//
// HOW TIMEOUTS AND CLEANUP WORK
//  - A plain blocking wait could hang forever if a child never ends. So we poll
//    waitpid(WNOHANG) until the child is done or 10 s have passed.
//  - On timeout the child gets SIGKILL and is reaped, so it never stays behind
//    as a zombie. After SIGKILL a blocking waitpid is safe.

// Kills a child and reaps it. Used on error paths and after a timeout.
void killAndReap(pid_t pid) {
    kill(pid, SIGKILL);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
}

// Waits for one child with a 10 s timeout and REAPS it.
// Returns  1 = reaped, `status` is valid
//          0 = timed out: child was killed and reaped, errno = ETIMEDOUT
//         -1 = waitpid itself failed, errno is set
int waitForChildWithTimeout(pid_t pid, int& status) {
    const long long start = monotonicNs();
    const long long deadline = start + kWaitTimeoutMs * 1000000LL;

    while (true) {
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            return 1;
        }
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        // result == 0: the child is still running.
        long long now = monotonicNs();
        if (now >= deadline) {
            killAndReap(pid);
            errno = ETIMEDOUT;
            return 0;
        }
        if (now - start < kSpinPhaseNs) {
            sched_yield();
        } else {
            sleepOneMillisecond();
        }
    }
}

// Zombie check: after our own cleanup there must be no child left at all.
// waitpid(-1) fails with ECHILD when no child exists.
bool verifyNoChildrenLeft(const char* when) {
    int leftover = 0;
    bool stillRunning = false;
    while (true) {
        pid_t result = waitpid(-1, nullptr, WNOHANG);
        if (result > 0) {
            ++leftover;            // a zombie we forgot; it is reaped now
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result == 0) {
            stillRunning = true;   // a child that is still alive
        }
        break;                     // result < 0 with ECHILD: no children, good
    }

    bool ok = (leftover == 0 && !stillRunning);
    if (ok) {
        cout << "No child processes left (" << when << ").\n";
        LOG_SYSCALL("VerifyNoChildrenLeft", true, when);
    } else {
        cout << "!!! WARNING: " << leftover << " leftover child process(es) found ("
             << when << (stillRunning ? ", at least one still running" : "") << ").\n";
        errno = EIO;
        LOG_SYSCALL("VerifyNoChildrenLeft", false, when);
    }
    return ok;
}

// Counts the entries of /proc/self/fd (one per open descriptor). The
// directory stream itself uses one descriptor and shows up in the list. That
// is the same in every count, so it cancels out when two counts are compared.
// Returns -1 if /proc cannot be read.
int countOpenDescriptors(const char* when) {
    DIR* directory = opendir("/proc/self/fd");
    LOG_SYSCALL("opendir", directory != nullptr, when);
    if (directory == nullptr) {
        return -1;
    }
    int total = 0;
    dirent* entry = nullptr;
    while ((entry = readdir(directory)) != nullptr) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            ++total;
        }
    }
    closedir(directory);
    return total;
}

// ----------------------------- Shared memory helpers (Linux only) ------------

// shmat reports failure by returning (void*)-1, NOT nullptr.
bool shmatFailed(void* address) {
    return address == reinterpret_cast<void*>(-1);
}

// Counts the System V segments of the whole machine: every line of
// /proc/sysvipc/shm after the header line is one segment. ("ipcs -m" shows the
// same list.) Other users' segments are included, so the count can also change
// if some other program creates or removes a segment while we run.
// Returns -1 if the file cannot be read.
int countSharedMemorySegments() {
    ifstream file("/proc/sysvipc/shm");
    if (!file.is_open()) {
        return -1;
    }
    string line;
    int total = 0;
    bool header = true;
    while (getline(file, line)) {
        if (header) {
            header = false;
            continue;
        }
        if (!line.empty()) {
            ++total;
        }
    }
    return total;
}

// Prints the segment leak check. Returns true if no segment was left behind.
bool checkSegmentLeak(int before, const char* when) {
    int after = countSharedMemorySegments();
    if (after >= 0) {
        cout << "Shared memory segments " << when << ": " << after << "\n";
    }
    if (before < 0 || after < 0) {
        cout << "!!! WARNING: could not read /proc/sysvipc/shm, segment leak check skipped.\n";
        return false;
    }
    if (after > before) {
        cout << "!!! WARNING: shared memory segments are LEAKING (" << (after - before)
             << " more than before). List them with: ipcs -m   remove with: ipcrm -m <id>\n";
        return false;
    }
    cout << "Segment leak check PASSED.\n";
    return true;
}

// Removes a segment (IPC_RMID) and logs it. The segment is really freed once
// the last attachment is gone.
bool removeSegmentLogged(int id, const char* logName, const char* detail) {
    int result = shmctl(id, IPC_RMID, nullptr);
    LOG_SYSCALL(logName, result == 0, detail);
    return result == 0;
}

bool detachLogged(void* address, const char* logName, const char* detail) {
    int result = shmdt(address);
    LOG_SYSCALL(logName, result == 0, detail);
    return result == 0;
}

// ----------------------------- Pipe reading helpers --------------------------

// Waits until a descriptor can be read (or has reached end of data).
// Returns 1 = readable, 0 = timeout, -1 = error (errno set).
int pollReadable(int fd, int timeoutMs) {
    while (true) {
        pollfd waitFor;
        waitFor.fd = fd;
        waitFor.events = POLLIN;
        waitFor.revents = 0;
        int ready = poll(&waitFor, 1, timeoutMs);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        return ready;
    }
}

// Reads exactly `size` bytes from a pipe with an overall 10 s timeout.
// read() may deliver fewer bytes than asked (partial reads are legal), so we
// loop. Returns false on timeout (ETIMEDOUT), on early end of data (EIO) or
// on error.
bool readFully(int fd, void* buffer, size_t size) {
    char* out = static_cast<char*>(buffer);
    size_t total = 0;
    const long long deadline = monotonicNs() + kWaitTimeoutMs * 1000000LL;

    while (total < size) {
        long long remainingMs = (deadline - monotonicNs()) / 1000000LL;
        if (remainingMs <= 0) {
            errno = ETIMEDOUT;
            return false;
        }
        int ready = pollReadable(fd, static_cast<int>(remainingMs));
        if (ready < 0) {
            return false;
        }
        if (ready == 0) {
            errno = ETIMEDOUT;
            return false;
        }
        ssize_t got = read(fd, out + total, size - total);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (got == 0) {
            errno = EIO;   // the writer ended before sending everything
            return false;
        }
        total += static_cast<size_t>(got);
    }
    return true;
}

// ----------------------------- CHILD MODES -----------------------------------
//
// The child is this same program, started by fork + execv with "--child-pipe"
// or "--child-shm". main() checks for these flags FIRST, before the transcript,
// the tee and logger::init. A child does its job and ends with _exit. It must
// do NOTHING else:
//   1. It must not open (and so overwrite) the transcript file.
//   2. It must not initialise or write to the log file, and must not print.
//      So it cannot log, and reports only through its exit code.
//   3. It must never start children of its own (that would multiply processes
//      without end and flood the machine).

// "--child-pipe <readFd> <messageLength>"
// The read descriptor was inherited through fork + exec: pipe() sets no
// FD_CLOEXEC, so it stays open in the new program with the same number.
// Exit codes: 0 = message correct and end of data seen, 1 = data mismatch,
// 2 = a read failed or ended early, 3 = the extra read returned data.
[[noreturn]] void runPipeChild(int argc, char* argv[]) {
    if (argc < 4) {
        _exit(2);
    }
    int fd = atoi(argv[2]);
    size_t messageLength = static_cast<size_t>(strtoull(argv[3], nullptr, 10));
    if (messageLength == 0 || messageLength > kSize1MB) {
        _exit(2);   // refuse silly sizes instead of allocating them
    }

    vector<char> received(messageLength);
    if (!readFully(fd, received.data(), messageLength)) {
        close(fd);
        _exit(2);
    }

    vector<char> expected = buildTestData(messageLength);
    if (memcmp(received.data(), expected.data(), messageLength) != 0) {
        close(fd);
        _exit(1);
    }

    // One more read. The parent closed its write end, so no writer is left:
    // read() must return 0 (end of data). poll keeps this bounded.
    int ready = pollReadable(fd, static_cast<int>(kWaitTimeoutMs));
    if (ready <= 0) {
        close(fd);
        _exit(2);
    }
    char extra = 0;
    ssize_t got = read(fd, &extra, 1);
    close(fd);
    if (got == 0) {
        _exit(0);
    }
    _exit(got > 0 ? 3 : 2);
}

// "--child-shm <shmid>"
// Exit codes: 0 = OK, 1 = first half mismatch, 2 = shmat failed or bad arguments.
[[noreturn]] void runSharedMemoryChild(int argc, char* argv[]) {
    if (argc < 3) {
        _exit(2);
    }
    int id = atoi(argv[2]);
    void* address = shmat(id, nullptr, 0);
    if (shmatFailed(address)) {
        _exit(2);
    }
    volatile char* memory = static_cast<volatile char*>(address);

    // The parent wrote 'A' + i % 26 into the first half.
    bool firstHalfOk = true;
    for (size_t i = 0; i < kHalfSize; ++i) {
        if (memory[i] != patternChar(i, 'A')) {
            firstHalfOk = false;
            break;
        }
    }

    // Our answer for the parent: lowercase pattern in the second half.
    for (size_t i = 0; i < kHalfSize; ++i) {
        memory[kHalfSize + i] = patternChar(i, 'a');
    }

    shmdt(address);
    _exit(firstHalfOk ? 0 : 1);
}

// ----------------------------- Launching the child (parent side) -------------

// Starts this same exe as a child with up to two arguments (fork + execv),
// waits for it (10 s, polling), and reaps it. Every step is logged. Returns
// true if the child was reaped and ended normally; its exit code is returned in
// `exitCode` for the caller to verify. A child that times out is killed and
// reaped inside the wait helper, so no zombie is left on any path.
bool runChildAndGetExitCode(const string& exePath, const string& flag,
                            const string& arg1, const string& arg2,
                            const string& detail, int& exitCode) {
    exitCode = -1;

    // Everything the child needs is built BEFORE fork(), so the child does no
    // string work between fork and exec.
    vector<string> texts = {exePath, flag, arg1};
    if (!arg2.empty()) {
        texts.push_back(arg2);
    }
    vector<vector<char>> buffers;
    vector<char*> argvList;
    for (size_t i = 0; i < texts.size(); ++i) {
        buffers.push_back(vector<char>(texts[i].begin(), texts[i].end()));
        buffers.back().push_back('\0');
    }
    for (size_t i = 0; i < buffers.size(); ++i) {
        argvList.push_back(buffers[i].data());
    }
    argvList.push_back(nullptr);

    cout << "Command line: " << exePath << " " << flag << " " << arg1
         << (arg2.empty() ? "" : " ") << arg2 << "\n";

    // Flush first: otherwise the child gets a copy of unwritten parent output
    // and could write it a second time into the transcript file.
    cout.flush();
    pid_t pid = fork();
    if (pid < 0) {
        LOG_SYSCALL("fork", false, detail.c_str());   // errno is still the fork error
        return false;
    }
    if (pid == 0) {
        execv(argvList[0], argvList.data());
        _exit(127);   // only reached if execv failed
    }

    cout << "Child process id: " << pid << "\n";
    LOG_SYSCALL("fork", true, detail.c_str());

    int status = 0;
    int waitResult = waitForChildWithTimeout(pid, status);
    LOG_SYSCALL("waitpid", waitResult == 1, detail.c_str());   // errno = ETIMEDOUT on timeout
    if (waitResult != 1) {
        if (waitResult == -1) {
            killAndReap(pid);   // waitpid failed: do not leave the child behind
        }
        return false;
    }

    if (!WIFEXITED(status)) {
        cout << "The child did not exit normally (killed by a signal).\n";
        return false;
    }
    exitCode = WEXITSTATUS(status);
    return true;
}

// ----------------------------- PART 1: demo ----------------------------------

// A. Pipe: parent -> child.
bool demoPipe(const string& exePath) {
    cout << "\n-- A. Pipe: parent writes, child reads --\n";

    int fds[2];   // fds[0] = read end, fds[1] = write end
    int created = pipe(fds);
    LOG_SYSCALL("pipe", created == 0, "default 64 KB buffer");
    if (created != 0) {
        return false;
    }

    // The message (4096 bytes) fits in the 64 KB pipe buffer, so this write
    // cannot block even though the child has not started yet.
    vector<char> message = buildTestData(kDemoPipeMessageSize);
    cout << "Message starts with: " << string(message.begin(), message.begin() + 40) << "\n";
    ssize_t written = write(fds[1], message.data(), kDemoPipeMessageSize);
    bool writeGood = transferSucceeded(written, kDemoPipeMessageSize);
    LOG_SYSCALL("write", writeGood, "4096 bytes into the pipe");
    if (!writeGood) {
        close(fds[0]);
        close(fds[1]);
        return false;
    }

    // Closing the write end BEFORE the child starts is what makes end-of-data
    // visible to the child. Unlike Windows (where the child's last read FAILS
    // with ERROR_BROKEN_PIPE), end-of-data on Linux is read() returning 0, not
    // an error.
    int writeClosed = close(fds[1]);
    LOG_SYSCALL("close", writeClosed == 0, "pipe write end");
    if (writeClosed != 0) {
        close(fds[0]);
        return false;
    }

    string detail = "--child-pipe (4096 bytes)";
    int exitCode = 0;
    bool childOk = runChildAndGetExitCode(exePath, "--child-pipe", to_string(fds[0]),
                                          to_string(kDemoPipeMessageSize), detail, exitCode);

    // The parent's read end is closed in every case.
    int readClosed = close(fds[0]);
    LOG_SYSCALL("close", readClosed == 0, "pipe read end");

    bool verified = false;
    if (childOk) {
        verified = (exitCode == 0);
        if (verified) {
            cout << "Verification PASSED: the child received all 4096 bytes correctly "
                 << "and then saw end-of-data (exit code 0).\n";
            LOG_SYSCALL("VerifyPipeTransfer", true, detail.c_str());
        } else {
            cout << "Verification FAILED: child exit code " << exitCode
                 << " (1 = data mismatch, 2 = read failed, 3 = no end-of-data, 127 = exec failed).\n";
            errno = EIO;
            LOG_SYSCALL("VerifyPipeTransfer", false, detail.c_str());
        }
    }
    return childOk && verified && readClosed == 0;
}

// B. System V shared memory: data travels in both directions.
bool demoSharedMemory(const string& exePath) {
    cout << "\n-- B. Shared memory (System V): parent <-> child --\n";

    int id = shmget(IPC_PRIVATE, kSize1MB, IPC_CREAT | 0600);
    LOG_SYSCALL("shmget", id >= 0, "1 MB, IPC_PRIVATE");
    if (id < 0) {
        return false;
    }
    cout << "Segment id: " << id << "\n"
         << "(If a run is ever killed half-way, remove the segment with: ipcrm -m " << id << ")\n";

    void* address = shmat(id, nullptr, 0);
    LOG_SYSCALL("shmat", !shmatFailed(address), "whole 1 MB segment");
    if (shmatFailed(address)) {
        removeSegmentLogged(id, "shmctl", "remove the 1 MB segment");
        return false;
    }
    volatile char* memory = static_cast<volatile char*>(address);

    // First half: our pattern. Second half: zeros, so we can tell later
    // whether the child really wrote there.
    for (size_t i = 0; i < kHalfSize; ++i) {
        memory[i] = patternChar(i, 'A');
        memory[kHalfSize + i] = 0;
    }

    // The child finds the segment BY ID (passed as an argument).
    string detail = "--child-shm " + to_string(id);
    int exitCode = 0;
    bool childOk = runChildAndGetExitCode(exePath, "--child-shm", to_string(id), "",
                                          detail, exitCode);

    bool exitCodeOk = childOk && (exitCode == 0);
    if (childOk && !exitCodeOk) {
        cout << "Child exit code " << exitCode
             << " (1 = first half did not match, 2 = shmat failed, 127 = exec failed).\n";
    }

    // Did the child's lowercase pattern arrive in the second half?
    bool secondHalfOk = true;
    if (childOk) {
        for (size_t i = 0; i < kHalfSize; ++i) {
            if (memory[kHalfSize + i] != patternChar(i, 'a')) {
                secondHalfOk = false;
                break;
            }
        }
    }
    bool verified = exitCodeOk && secondHalfOk;
    if (verified) {
        cout << "Verification PASSED: data travelled parent -> child (first half, 'A' pattern "
             << "checked by the child) and child -> parent (second half, 'a' pattern checked here).\n";
        LOG_SYSCALL("VerifySharedMemory", true, detail.c_str());
    } else {
        cout << "Verification FAILED: shared memory content is not what was expected.\n";
        errno = EIO;
        LOG_SYSCALL("VerifySharedMemory", false, detail.c_str());
    }

    // Always detach and remove the segment (it would otherwise stay in the kernel).
    bool detached = detachLogged(address, "shmdt", "whole 1 MB attachment");
    bool removed = removeSegmentLogged(id, "shmctl", "IPC_RMID, remove the 1 MB segment");

    return verified && detached && removed;
}

// C. Deliberate failure: shmget on a key that does not exist (error 2).
bool demoMissingSegment() {
    cout << "\n-- C. Opening a shared memory segment that does not exist --\n";
    cout << "shmget without IPC_CREAT on a missing key (this FAILURE is expected, error 2):\n";

    int id = shmget(kMissingShmKey, kSize1MB, 0600);
    int errorCode = errno;   // read before anything can change it
    LOG_SYSCALL("shmget", id >= 0, "key 0x4F534B4D, expected to fail");

    if (id >= 0) {
        cout << "UNEXPECTED: shmget found a segment that should not exist!\n";
        removeSegmentLogged(id, "shmctl", "remove the unexpected segment");
        return false;
    }
    if (errorCode != ENOENT) {
        cout << "UNEXPECTED: it failed, but with error " << errorCode << " instead of 2.\n";
        return false;
    }
    return true;
}

// Returns true if every step worked and every verification passed.
bool runDemo(const string& exePath) {
    cout << "---------- PART 1: DEMO (logged, not timed) ----------\n";
    cout << "Parent process id: " << getpid() << "\n";

    bool pipeOk = demoPipe(exePath);
    bool sharedOk = demoSharedMemory(exePath);
    bool missingOk = demoMissingSegment();

    // Every child was reaped above; make sure no zombie is left.
    bool noLeftovers = verifyNoChildrenLeft("after the demo");
    return pipeOk && sharedOk && missingOk && noLeftovers;
}

// ----------------------------- PART 2: benchmarks ----------------------------
//
// NOTES FOR THE REPORT
//  (1) Required Linux IPC calls of the assignment: pipe, shmget, shmat.
//      Helper calls (NOT counted among the 15): shmdt, shmctl, fcntl, fork,
//      execv, waitpid, read, write, close, poll. Pseudo calls named Verify...
//      are not system calls either.
//  (2) A pipe is one-way, like Windows CreatePipe. It is a kernel buffer:
//      write copies data IN, read copies data OUT (the data is copied twice).
//      The default capacity is 64 KB.
//  (3) Shared memory has no copy after shmat: both processes see the same
//      physical pages. The cost sits in creating and attaching, not in using.
//      Pages are assigned lazily on first touch.
//  (4) System V segments persist until removed (shmctl IPC_RMID); Windows file
//      mappings disappear automatically when the last handle is closed. This
//      is an OS-specific difference for the report.
//  (5) The benchmarks run inside ONE process: they measure call cost, not
//      cross-process latency. Cross-process correctness is shown by the demo.
//  (6) The CPU columns (cpu_us, cpu_per_call_us) are measured around the WHOLE
//      loop and include the cleanup call and timer overhead. Only
//      min/median/mean/max/stddev cover the action alone. The cpu_cycles
//      columns come from the TSC and are NOT comparable with Windows.
//  (7) The warm-up runs the lambdas too, so failures there are counted as well.
//  (8) The lambdas never log or print. They only count failures.

// Setup helper: a pipe enlarged to 1 MB with fcntl(F_SETPIPE_SZ). The default
// buffer is only 64 KB, so a 64 KB write would fill it completely. fcntl is a
// helper call. The unprivileged maximum is /proc/sys/fs/pipe-max-size
// (normally exactly 1 MB). If enlarging fails, we print the size that is
// available and continue only if it is at least 128 KB.
// On failure everything is closed and false is returned.
bool createBigPipeForBenchmark(int fds[2]) {
    int created = pipe(fds);
    LOG_SYSCALL("pipe_setup", created == 0, "benchmark pipe");
    if (created != 0) {
        return false;
    }

    int newSize = fcntl(fds[1], F_SETPIPE_SZ, kWantedPipeSize);
    LOG_SYSCALL("fcntl", newSize >= 0, "F_SETPIPE_SZ 1 MB");
    if (newSize >= 0) {
        cout << "Pipe buffer size: " << newSize << " bytes\n";
        return true;
    }

    int currentSize = fcntl(fds[1], F_GETPIPE_SZ);
    LOG_SYSCALL("fcntl", currentSize >= 0, "F_GETPIPE_SZ");
    cout << "F_SETPIPE_SZ failed. The available pipe buffer is "
         << currentSize << " bytes.\n";
    if (currentSize < kMinPipeSize) {
        cout << "!!! WARNING: the pipe buffer is smaller than 128 KB, so this pipe "
             << "benchmark was SKIPPED.\n";
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    return true;
}

// Teardown helper: closes both pipe ends, one log line each.
bool closePipeAfterBenchmark(int fds[2]) {
    int readClosed = close(fds[0]);
    LOG_SYSCALL("close_teardown", readClosed == 0, "pipe read end");
    int writeClosed = close(fds[1]);
    LOG_SYSCALL("close_teardown", writeClosed == 0, "pipe write end");
    return readClosed == 0 && writeClosed == 0;
}

// 1. Benchmark: pipe. The cleanup closes both ends outside the timed region
// so descriptors do not pile up.
bool benchmarkPipe() {
    FailureTracker failures;
    int fds[2] = {-1, -1};
    bool created = false;

    auto action = [&]() {
        created = (pipe(fds) == 0);
        if (!created) {
            failures.record(true);
        }
    };

    auto cleanup = [&]() {
        if (!created) {
            return;
        }
        if (close(fds[0]) != 0) {
            failures.record(true);
        }
        if (close(fds[1]) != 0) {
            failures.record(true);
        }
        created = false;
    };

    metrics::Stats stats = metrics::benchmark("pipe", action, kIterations, kWarmup, cleanup);
    return finishBenchmark(stats, failures);
}

// 2. Benchmark: write into a pipe (4 KB or 64 KB per call).
// WHY every iteration drains what it wrote: a write into a FULL pipe blocks
// until somebody reads. The default pipe buffer is only 64 KB, so a 64 KB write
// would fill it completely. The pipe is enlarged to 1 MB, and the cleanup
// (untimed) reads back the same number of bytes, so the pipe is empty again for
// the next iteration and never fills.
bool benchmarkPipeWrite(const char* callName, size_t size) {
    FailureTracker failures;
    vector<char> source = buildTestData(size);
    vector<char> drain(size);

    int fds[2] = {-1, -1};
    if (!createBigPipeForBenchmark(fds)) {
        return false;
    }

    size_t lastWritten = 0;   // bytes that really went into the pipe this iteration

    auto action = [&]() {
        lastWritten = 0;
        ssize_t written = write(fds[1], source.data(), size);
        if (written < 0) {
            failures.record(true);
        } else {
            lastWritten = static_cast<size_t>(written);
            if (lastWritten != size) {
                failures.record(false);
            }
        }
    };

    // Reads back exactly what was written. If nothing was written we must NOT
    // read, because reading an empty pipe would block.
    auto cleanup = [&]() {
        if (lastWritten == 0) {
            return;
        }
        ssize_t got = read(fds[0], drain.data(), lastWritten);
        if (got < 0) {
            failures.record(true);
        } else if (static_cast<size_t>(got) != lastWritten) {
            failures.record(false);
        }
    };

    metrics::Stats stats = metrics::benchmark(callName, action, kIterations, kWarmup, cleanup);

    bool closedOk = closePipeAfterBenchmark(fds);
    bool clean = finishBenchmark(stats, failures);
    return clean && closedOk;
}

// 3. Benchmark: read from a pipe (4 KB or 64 KB per call).
// The pipe is filled once before the benchmark. After every read the cleanup
// (untimed) writes the same amount back, so the next read always finds data
// and never blocks.
bool benchmarkPipeRead(const char* callName, size_t size) {
    FailureTracker failures;
    vector<char> source = buildTestData(size);
    vector<char> target(size);

    int fds[2] = {-1, -1};
    if (!createBigPipeForBenchmark(fds)) {
        return false;
    }

    // Setup: put `size` bytes into the pipe once.
    ssize_t setupWritten = write(fds[1], source.data(), size);
    bool setupGood = transferSucceeded(setupWritten, size);
    LOG_SYSCALL("write_setup", setupGood, "fill the pipe once");
    if (!setupGood) {
        closePipeAfterBenchmark(fds);
        return false;
    }

    bool lastReadOk = false;

    auto action = [&]() {
        lastReadOk = false;
        ssize_t got = read(fds[0], target.data(), size);
        if (got < 0) {
            failures.record(true);
        } else if (static_cast<size_t>(got) != size) {
            failures.record(false);
        } else {
            lastReadOk = true;
        }
    };

    // Put the data back only if the read really took it out.
    auto cleanup = [&]() {
        if (!lastReadOk) {
            return;
        }
        ssize_t written = write(fds[1], source.data(), size);
        if (written < 0) {
            failures.record(true);
        } else if (static_cast<size_t>(written) != size) {
            failures.record(false);
        }
    };

    metrics::Stats stats = metrics::benchmark(callName, action, kIterations, kWarmup, cleanup);

    bool closedOk = closePipeAfterBenchmark(fds);
    bool clean = finishBenchmark(stats, failures);
    return clean && closedOk;
}

// 4. Benchmark: shmget (1 MB segment). shmget creates the segment object;
// physical pages are assigned lazily on first touch, so the cost is small.
// The cleanup removes the segment (IPC_RMID) outside the timed region, so
// segments never pile up in the kernel.
bool benchmarkShmget() {
    FailureTracker failures;
    int id = -1;

    auto action = [&]() {
        id = shmget(IPC_PRIVATE, kSize1MB, IPC_CREAT | 0600);
        if (id < 0) {
            failures.record(true);
        }
    };

    auto cleanup = [&]() {
        if (id >= 0) {
            if (shmctl(id, IPC_RMID, nullptr) != 0) {
                failures.record(true);
            }
            id = -1;
        }
    };

    metrics::Stats stats = metrics::benchmark("shmget_1MB", action, kIterations, kWarmup, cleanup);
    return finishBenchmark(stats, failures);
}

// 5. Benchmark: shmat of a whole 1 MB segment. The segment is created once;
// the cleanup detaches the view outside the timed region. The segment is
// removed after the benchmark.
bool benchmarkShmat() {
    FailureTracker failures;
    int id = shmget(IPC_PRIVATE, kSize1MB, IPC_CREAT | 0600);
    LOG_SYSCALL("shmget_setup", id >= 0, "1 MB, IPC_PRIVATE");
    if (id < 0) {
        return false;
    }

    void* view = nullptr;

    auto action = [&]() {
        view = shmat(id, nullptr, 0);
        if (shmatFailed(view)) {
            failures.record(true);
            view = nullptr;
        }
    };

    auto cleanup = [&]() {
        if (view != nullptr) {
            if (shmdt(view) != 0) {
                failures.record(true);
            }
            view = nullptr;
        }
    };

    metrics::Stats stats = metrics::benchmark("shmat_1MB", action, kIterations, kWarmup, cleanup);

    bool removedOk = removeSegmentLogged(id, "shmctl_teardown", "IPC_RMID, remove the 1 MB segment");
    bool clean = finishBenchmark(stats, failures);
    return clean && removedOk;
}

// 6. Benchmark: shmdt.
// SETUP attaches ONE 64 KB segment 1050 times (each shmat gives a separate
// address); each call detaches the next address. A 64 KB segment is used on
// purpose: the total address space is then about 67 MB (1050 x 1 MB would be
// about 1 GB). NOTE: shmat always attaches the WHOLE segment, so this differs
// from the Windows benchmark, where 64 KB views of a 1 MB mapping were used.
bool benchmarkShmdt() {
    FailureTracker failures;
    int id = shmget(IPC_PRIVATE, kSize64KB, IPC_CREAT | 0600);
    LOG_SYSCALL("shmget_setup", id >= 0, "64 KB, IPC_PRIVATE");
    if (id < 0) {
        return false;
    }

    // Built BEFORE the loop so no string work sits between a failing shmat
    // and the LOG_SYSCALL that reads its error code.
    string detail = "64 KB x " + to_string(kTotalCalls);
    vector<void*> addresses;
    addresses.reserve(kTotalCalls);

    for (size_t i = 0; i < kTotalCalls; ++i) {
        void* address = shmat(id, nullptr, 0);
        if (shmatFailed(address)) {
            LOG_SYSCALL("shmat_setup", false, detail.c_str());
            for (size_t j = 0; j < addresses.size(); ++j) {
                shmdt(addresses[j]);
            }
            removeSegmentLogged(id, "shmctl_teardown", "IPC_RMID after a failed setup");
            return false;
        }
        addresses.push_back(address);
    }
    LOG_SYSCALL("shmat_setup", true, detail.c_str());

    size_t nextIndex = 0;

    auto action = [&]() {
        if (nextIndex >= addresses.size()) {   // should never happen
            failures.record(false);
            return;
        }
        int result = shmdt(addresses[nextIndex]);
        ++nextIndex;
        if (result != 0) {
            failures.record(true);
        }
    };

    // No cleanup: each address is detached exactly once by the action.
    metrics::Stats stats = metrics::benchmark("shmdt_64KB", action, kIterations, kWarmup);

    // Safety net: detach any address the benchmark did not reach (normally none).
    for (size_t i = nextIndex; i < addresses.size(); ++i) {
        shmdt(addresses[i]);
    }
    addresses.clear();

    bool removedOk = removeSegmentLogged(id, "shmctl_teardown", "IPC_RMID, remove the 64 KB segment");
    bool clean = finishBenchmark(stats, failures);
    return clean && removedOk;
}

// Runs all benchmarks plus the descriptor leak check. Returns how many problems.
int runAllBenchmarks() {
    cout << "\n---------- PART 2: BENCHMARKS (timed, no logging inside) ----------\n";
    cout << "Pointer size: " << sizeof(void*) << " bytes ("
         << sizeof(void*) * 8 << "-bit build)\n";
    int problems = 0;

    int before = countOpenDescriptors("before Part 2");
    if (before >= 0) {
        cout << "Open descriptors before Part 2: " << before << "\n";
    }

    if (!benchmarkPipe())                                  ++problems;
    if (!benchmarkPipeWrite("pipe_write_4KB", kSize4KB))   ++problems;
    if (!benchmarkPipeWrite("pipe_write_64KB", kSize64KB)) ++problems;
    if (!benchmarkPipeRead("pipe_read_4KB", kSize4KB))     ++problems;
    if (!benchmarkPipeRead("pipe_read_64KB", kSize64KB))   ++problems;
    if (!benchmarkShmget())                                ++problems;
    if (!benchmarkShmat())                                 ++problems;
    if (!benchmarkShmdt())                                 ++problems;

    int after = countOpenDescriptors("after Part 2");
    if (after >= 0) {
        cout << "Open descriptors after Part 2 : " << after << "\n";
    }

    // Leak check: a forgotten close() would grow the count by hundreds.
    if (before >= 0 && after >= 0) {
        if (after > before + kDescriptorLeakTolerance) {
            cout << "!!! WARNING: file descriptors may be LEAKING (" << (after - before)
                 << " more than before).\n";
            ++problems;
        } else {
            cout << "Descriptor leak check PASSED.\n";
        }
    } else {
        cout << "!!! WARNING: could not read /proc/self/fd, leak check skipped.\n";
        ++problems;
    }
    return problems;
}

// ----------------------------- Program flow ----------------------------------

int runAll() {
    logger::init();   // default path: logs/syscalls_linux.log

    cout << "=== Linux IPC: demo + benchmarks ===\n\n";

    string exePath;
    if (!getOwnPath(exePath)) {
        cout << "Cannot find the path of this program, so no child can be started.\n";
        return 1;
    }

    // Segment leak check: count the System V segments before anything is created.
    int segmentsBefore = countSharedMemorySegments();
    if (segmentsBefore >= 0) {
        cout << "Shared memory segments before Part 1: " << segmentsBefore << "\n";
    }

    if (!runDemo(exePath)) {
        cout << "\nDemo FAILED, so the benchmarks were skipped.\n";
        checkSegmentLeak(segmentsBefore, "after the failed demo");   // information only
        return 1;
    }

    int problems = runAllBenchmarks();
    if (!checkSegmentLeak(segmentsBefore, "after Part 2")) {
        ++problems;
    }
    if (problems > 0) {
        cout << "\nFinished with " << problems << " benchmark problem(s). See warnings above.\n";
        return 2;
    }

    cout << "\nAll done. Results appended to results/Linux/linux_results.csv\n";
    return 0;
}

int main(int argc, char* argv[]) {
    // CHILD MODES: this MUST be the very first thing in main, before the
    // transcript, the tee, logger::init or any printing. A child does its job,
    // ends with _exit and does NOTHING else (see the CHILD MODES comment).
    if (argc >= 2 && strcmp(argv[1], "--child-pipe") == 0) {
        runPipeChild(argc, argv);
        _exit(0);
    }
    if (argc >= 2 && strcmp(argv[1], "--child-shm") == 0) {
        runSharedMemoryChild(argc, argv);
        _exit(0);
    }

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