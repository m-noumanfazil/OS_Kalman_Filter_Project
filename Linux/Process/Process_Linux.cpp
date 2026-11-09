// =============================================================================
// Process_Linux.cpp
//
// Part of an OS project comparing Linux and Windows system calls (Linux side,
// Process category). Port of Process_Windows.cpp.
//
// What this program does:
//   PART 1  Demo (logged, not timed): fork + wait (child exits 42), fork +
//           execv of THIS SAME program ("--child 7", child exits 7), and one
//           deliberate failure (execv of a program that does not exist, error 2,
//           child exits 127). Every child is reaped and its exit code verified.
//   PART 2  Benchmarks (200 iterations, 10 warm-up each), timed separately:
//             fork, wait, exec, fork_exec_wait
//           Results are printed and appended to results/Linux/linux_results.csv.
//   PART 3  Everything printed to the console is also saved to
//           Linux/Process/Process_Linux.txt (overwritten on every run).
//
// The child is THIS SAME EXE, started with "--child <exitcode>" or
// "--exec-child <fd> <t0>". main() checks these flags FIRST (see main).
//
// Windows call -> Linux call (name mapping for the report):
//   +----------------------------------+------------------------------------+
//   | Windows                          | Linux                              |
//   +----------------------------------+------------------------------------+
//   | CreateProcess (create + load)    | fork() + execv()  (two steps)      |
//   | WaitForSingleObject(process)     | wait() / waitpid()                 |
//   | GetExitCodeProcess               | WIFEXITED + WEXITSTATUS            |
//   | TerminateProcess                 | kill(pid, SIGKILL)                 |
//   | GetProcessHandleCount            | count entries of /proc/self/fd     |
//   | GetModuleFileNameA               | readlink("/proc/self/exe")         |
//   +----------------------------------+------------------------------------+
//
// Compile and run from the REPOSITORY ROOT:
//   g++ -std=c++17 Linux/Process/Process_Linux.cpp -o Linux/Process/process_linux
//   ./Linux/Process/process_linux
//
// Exit codes: 0 = everything OK, 1 = demo failed, 2 = a benchmark had problems.
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
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace std;

// ----------------------------- Settings --------------------------------------

const char* const kTranscriptFile = "Linux/Process/Process_Linux.txt";
const char* const kMissingProgram = "/this_program_does_not_exist";

// Process creation takes about a millisecond, so we use fewer iterations than
// the other categories (200 instead of 1000, 10 warm-up instead of 50).
const int    kIterations = 200;
const int    kWarmup     = 10;
// The warm-up runs the action too, so every benchmark that consumes one
// child per call needs warm-up + iterations children (210).
const size_t kTotalCalls = kWarmup + kIterations;

// Every wait has a timeout, never an unbounded block.
const long long kWaitTimeoutMs = 10000;
// Polling: spin (with sched_yield) for the first 50 ms, then sleep 1 ms steps.
// Spinning keeps the extra latency of the timeout protection tiny for children
// that finish within a millisecond or two.
const long long kSpinPhaseNs = 50LL * 1000000LL;

// A descriptor count more than this much higher after Part 2 means a leak.
const int kDescriptorLeakTolerance = 5;

// TOTAL CHILD PROCESSES STARTED BY THE WHOLE PROGRAM:
//   demo 3 + fork 210 + wait 210 + exec 210 + fork_exec_wait 210 = 843.
// This is above the "about 700" target of the assignment text, because 200
// iterations + 10 warm-up were required for all four benchmarks. It is safe:
// all benchmarks except "wait" keep only ONE child alive at a time, and "wait"
// keeps 210 short-lived zombies at once (far below the usual process limit).

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

// Counts failures inside benchmark lambdas. Lambdas must not log (logging
// would ruin the timing), so they only record here; the result is reported
// once, after the benchmark.
struct FailureTracker {
    int count = 0;
    int firstError = 0;

    // apiFailed = true : a system call failed, keep its errno.
    // apiFailed = false: some other problem (for example a nonzero exit code).
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

// Monotonic clock in nanoseconds. CLOCK_MONOTONIC is shared by all processes
// on the machine, so a timestamp taken in one process can be compared with a
// timestamp taken in another one (needed by the exec benchmark).
long long monotonicNs() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<long long>(now.tv_sec) * 1000000000LL + now.tv_nsec;
}

void sleepOneMillisecond() {
    timespec pause = {0, 1000000};
    nanosleep(&pause, nullptr);
}

// True if the child ended normally with exactly this exit code.
bool exitedWith(int status, int expectedCode) {
    return WIFEXITED(status) && WEXITSTATUS(status) == expectedCode;
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
//  - On timeout the child gets SIGKILL (the Linux TerminateProcess) and is
//    reaped, so it never stays behind as a zombie.
//  - After SIGKILL a blocking waitpid is safe: the kernel ends the child.

// Kills a child and reaps it. Used on error paths and after a timeout.
void killAndReap(pid_t pid) {
    kill(pid, SIGKILL);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
}

// Kills and reaps every child in a list (error-path cleanup).
void killAndReapAll(vector<pid_t>& pids) {
    for (size_t i = 0; i < pids.size(); ++i) {
        killAndReap(pids[i]);
    }
    pids.clear();
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
            sched_yield();   // let the child run, especially on a single core
        } else {
            sleepOneMillisecond();
        }
    }
}

// Waits (10 s at most) until a child has EXITED, WITHOUT reaping it. waitid
// with WNOWAIT leaves the child waitable, so a later wait() can still collect
// it. (waitpid(WNOHANG) would reap it as soon as it has exited.)
// Returns false on timeout (errno = ETIMEDOUT) or error (errno set).
bool waitUntilExitedWithoutReaping(pid_t pid) {
    const long long start = monotonicNs();
    const long long deadline = start + kWaitTimeoutMs * 1000000LL;

    while (true) {
        siginfo_t info;
        memset(&info, 0, sizeof(info));   // si_pid stays 0 if nothing has exited
        if (waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT) < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (info.si_pid == pid) {
            return true;
        }
        long long now = monotonicNs();
        if (now >= deadline) {
            errno = ETIMEDOUT;
            return false;
        }
        if (now - start < kSpinPhaseNs) {
            sched_yield();
        } else {
            sleepOneMillisecond();
        }
    }
}

// Reaps one child (with timeout) and checks its exit code. Records a failure
// if the wait failed, timed out, or the exit code is not the expected one.
// Used by the benchmarks, so it never logs.
bool reapAndCheck(pid_t pid, int expectedCode, FailureTracker& failures) {
    int status = 0;
    int result = waitForChildWithTimeout(pid, status);
    if (result != 1) {
        failures.record(true);    // errno = ETIMEDOUT or the waitpid error
        return false;
    }
    if (!exitedWith(status, expectedCode)) {
        failures.record(false);
        return false;
    }
    return true;
}

// Zombie check: after our own cleanup there must be no child left at all.
// waitpid(-1) fails with ECHILD when no child exists. Anything else (a
// leftover zombie or a still running child) is reported as a problem.
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

// ----------------------------- CHILD MODES -----------------------------------
//
// The child is this same program. main() checks for the flags FIRST, before the
// transcript, the tee and logger::init. A child does its job and ends with
// _exit. It must do NOTHING else:
//   1. It must not open (and so overwrite) the transcript file.
//   2. It must not initialise or write to the log file.
//   3. It must never start children of its own (that would multiply processes
//      without end and flood the machine).

// Writes all bytes to a descriptor. write() may write fewer bytes than asked,
// so we loop until everything is written.
bool writeFully(int fd, const void* buffer, size_t size) {
    const char* data = static_cast<const char*>(buffer);
    size_t total = 0;
    while (total < size) {
        ssize_t written = write(fd, data + total, size - total);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        total += static_cast<size_t>(written);
    }
    return true;
}

// "--exec-child <fd> <t0>"  (started by the exec benchmark)
// t0 = CLOCK_MONOTONIC timestamp taken by the old process just before execv.
// The FIRST statement takes the startup timestamp t1, then both are sent to the
// parent through the inherited pipe. Exit codes: 0 = OK, 2 = bad arguments,
// 3 = the pipe write failed.
int runExecChild(int argc, char* argv[]) {
    unsigned long long t1 = static_cast<unsigned long long>(monotonicNs());
    if (argc < 4) {
        _exit(2);
    }
    int fd = atoi(argv[2]);
    unsigned long long t0 = strtoull(argv[3], nullptr, 10);

    unsigned long long stamps[2] = {t0, t1};
    bool ok = writeFully(fd, stamps, sizeof(stamps));
    close(fd);
    _exit(ok ? 0 : 3);
}

// ----------------------------- PART 1: demo ----------------------------------

enum DemoKind { kForkOnly, kExecSelf, kExecMissing };

// Code that runs in the forked child of the demo. It never returns.
[[noreturn]] void demoChildBody(DemoKind kind, char* selfPath,
                                char* const selfArgv[], char* const missingArgv[]) {
    if (kind == kForkOnly) {
        cout << "Child: my pid is " << getpid() << ", my parent's pid is "
             << getppid() << endl;   // endl flushes before _exit
        _exit(42);
    }

    if (kind == kExecSelf) {
        cout << "Child: pid " << getpid() << " now replaces itself with this same "
             << "program using execv (--child 7)" << endl;
        execv(selfPath, selfArgv);
        // A successful execv never returns. Getting here means it FAILED.
        LOG_SYSCALL("execv", false, "same program, unexpected failure");
        _exit(125);
    }

    // kExecMissing: the program does not exist, so execv must fail with ENOENT.
    cout << "Child: execv on a program that does not exist "
            "(this FAILURE is expected, error 2):" << endl;
    execv(kMissingProgram, missingArgv);
    int savedErrno = errno;   // capture immediately
    LOG_SYSCALL("execv", false, "/this_program_does_not_exist, expected to fail");
    _exit(savedErrno == ENOENT ? 127 : 126);
}

// Waits for a demo child and reaps it with wait(). Bounded: first we wait
// (10 s at most) until the child has exited without reaping it, then wait()
// returns at once. On timeout the child is killed and reaped.
// Returns the pid that wait() returned, or -1 (errno set).
pid_t waitForDemoChild(pid_t pid, int& status) {
    if (!waitUntilExitedWithoutReaping(pid)) {
        int savedErrno = errno;
        killAndReap(pid);
        errno = savedErrno;
        return -1;
    }
    return wait(&status);
}

// One demo child: fork, (optionally exec), wait, verify the exit code.
bool runDemoChild(DemoKind kind, const string& exePath) {
    int expectedCode = 0;
    const char* detail = "";
    if (kind == kForkOnly) {
        cout << "\n-- 1. fork(): the child exits with code 42 --\n";
        expectedCode = 42;
        detail = "fork, child exits 42";
    } else if (kind == kExecSelf) {
        cout << "\n-- 2. fork() + execv(): the child becomes this same program (--child 7) --\n";
        expectedCode = 7;
        detail = "fork + execv --child 7";
    } else {
        cout << "\n-- 3. fork() + execv() on a program that does not exist --\n";
        expectedCode = 127;
        detail = "fork + execv of a missing program";
    }

    // Arguments are built BEFORE fork(), so the child does no string work.
    char flag[] = "--child";
    char seven[] = "7";
    char ninetyNine[] = "99";   // would only show up if the missing program ran
    char missingPath[] = "/this_program_does_not_exist";
    char* selfPath = const_cast<char*>(exePath.c_str());
    char* selfArgv[] = {selfPath, flag, seven, nullptr};
    char* missingArgv[] = {missingPath, flag, ninetyNine, nullptr};

    // Flush first: otherwise the child gets a copy of unwritten parent output
    // and could write it a second time into the transcript file.
    cout.flush();
    pid_t pid = fork();
    if (pid < 0) {
        int savedErrno = errno;   // capture immediately
        cout << "fork() failed with error " << savedErrno << " (see the log line).\n";
        errno = savedErrno;
        LOG_SYSCALL("fork", false, detail);
        return false;
    }
    if (pid == 0) {
        demoChildBody(kind, selfPath, selfArgv, missingArgv);
    }

    // Parent.
    cout << "Parent: my pid is " << getpid() << ", the child's pid is " << pid << "\n";
    LOG_SYSCALL("fork", true, detail);

    int status = 0;
    pid_t waited = waitForDemoChild(pid, status);
    bool waitOk = (waited == pid);
    if (waited >= 0 && waited != pid) {
        errno = EIO;   // wait() returned a different child: no errno of its own
    }
    LOG_SYSCALL("wait", waitOk, detail);
    if (!waitOk) {
        if (waited >= 0) {
            killAndReap(pid);   // our child is still unreaped
        }
        return false;
    }

    bool exited = WIFEXITED(status);
    int code = exited ? WEXITSTATUS(status) : -1;
    bool verified = exited && (code == expectedCode);
    if (verified) {
        cout << "Verification PASSED: child exited normally with code " << code
             << " (expected " << expectedCode << ").\n";
        if (kind == kExecSelf) {
            // A successful exec never returns, so it cannot be logged where it
            // is called. Exit code 7 proves the new program really started.
            LOG_SYSCALL("execv", true, "verified: the new program started and exited with 7");
        }
        LOG_SYSCALL("VerifyExitCode", true, detail);
    } else {
        cout << "Verification FAILED: ";
        if (!exited) {
            cout << "the child did not exit normally (killed by a signal).\n";
        } else {
            cout << "child exit code " << code << " but " << expectedCode << " was expected.\n";
        }
        if (kind == kExecMissing && code == 126) {
            cout << "execv failed, but with an error other than ENOENT.\n";
        }
        if (kind == kExecMissing && code == 99) {
            cout << "UNEXPECTED: execv started a program that should not exist!\n";
        }
        errno = EIO;
        LOG_SYSCALL("VerifyExitCode", false, detail);
    }
    return verified;
}

// Returns true if every step worked and every verification passed.
bool runDemo(const string& exePath) {
    cout << "---------- PART 1: DEMO (logged, not timed) ----------\n";
    cout << "Parent process id: " << getpid() << "\n";

    bool forkOk = runDemoChild(kForkOnly, exePath);
    bool execOk = runDemoChild(kExecSelf, exePath);
    bool missingOk = runDemoChild(kExecMissing, exePath);

    // Every child was reaped above; make sure no zombie is left.
    bool noLeftovers = verifyNoChildrenLeft("after the demo");
    return forkOk && execOk && missingOk && noLeftovers;
}

// ----------------------------- PART 2: benchmarks ----------------------------
//
// NOTES FOR THE REPORT
//  (1) Linux separates process creation and program execution. fork() creates a
//      child process, while exec() replaces the child's program image. Windows
//      CreateProcess() performs process creation and program loading together.
//      Therefore fork_exec_wait is the fair comparison with Windows
//      CreateProcess_plus_Wait.
//  (2) Process creation is significantly more expensive than ordinary file and
//      memory operations because the OS must establish a new process context
//      and manage its execution resources. Report the MEDIAN, because
//      scheduling and system activity cause timing variation.
//  (3) Linux CPU time is measured with CLOCK_PROCESS_CPUTIME_ID. It measures
//      CPU time consumed by the PARENT process, not the combined CPU time of
//      all children. Child CPU usage is not included. (In fork_exec_wait the
//      parent also polls waitpid, which adds a little parent CPU time.)
//  (4) The cpu_cycles column comes from RDTSC and measures timestamp-counter
//      ticks, not CPU cycles consumed by the process. It must not be compared
//      with Windows QueryProcessCycleTime.
//  (5) Memory delta and page faults are parent-process measurements. They do
//      not represent the resource use of the children.
//  (6) The wait benchmark measures waiting for children that have ALREADY
//      exited. It does not measure the time a child needs to run.
//  (7) The warm-up runs the lambdas too, so failures there are counted as well.
//  (8) The lambdas never log or print. They only count failures.
//  (9) Before each benchmark cout is flushed, so a forked child never inherits
//      unwritten output (nothing is printed inside the timed loops).

// A. Benchmark: fork. The action is fork() only; the child ends at once with
// _exit(0). The cleanup (untimed) reaps the child and checks exit code 0, so
// the measured time covers only the fork itself, not the child's cleanup.
bool benchmarkFork() {
    FailureTracker failures;
    pid_t childPid = -1;
    cout.flush();

    auto action = [&]() {
        childPid = fork();
        if (childPid == 0) {
            _exit(0);   // child: end immediately, no flushing, no destructors
        }
        if (childPid < 0) {
            failures.record(true);
        }
    };

    auto cleanup = [&]() {
        if (childPid > 0) {
            reapAndCheck(childPid, 0, failures);
            childPid = -1;
        }
    };

    metrics::Stats stats = metrics::benchmark("fork", action, kIterations, kWarmup, cleanup);
    return finishBenchmark(stats, failures);
}

// B. Benchmark: wait.
// SETUP: 210 children are created and all of them end at once. waitid with
// WNOWAIT confirms they have exited WITHOUT reaping them, so they stay
// waitable (zombies). waitpid(WNOHANG) would NOT work here: it reaps.
// Because the children have ALREADY exited, this benchmark measures the cost of
// the wait() call on an already-exited child, NOT the lifetime of a child.
// The action itself reaps one child per call, so there is no cleanup.
// wait() has no timeout, but it cannot block here: every child is known to be
// exited, and these are the only children of the process.
bool benchmarkWait() {
    FailureTracker failures;
    string detail = "exit(0) x" + to_string(kTotalCalls);   // built before any call
    vector<pid_t> pids;
    pids.reserve(kTotalCalls);
    cout.flush();

    for (size_t i = 0; i < kTotalCalls; ++i) {
        pid_t pid = fork();
        if (pid == 0) {
            _exit(0);
        }
        if (pid < 0) {
            LOG_SYSCALL("fork_setup", false, detail.c_str());
            killAndReapAll(pids);
            return false;
        }
        pids.push_back(pid);
    }
    LOG_SYSCALL("fork_setup", true, detail.c_str());

    for (size_t i = 0; i < pids.size(); ++i) {
        if (!waitUntilExitedWithoutReaping(pids[i])) {
            LOG_SYSCALL("waitid_setup", false, detail.c_str());
            killAndReapAll(pids);
            return false;
        }
    }
    LOG_SYSCALL("waitid_setup", true, detail.c_str());

    // Results are stored by index inside the action and checked afterwards, so
    // the timed region only contains the wait() call and two array stores.
    vector<pid_t> returnedPids(pids.size(), 0);
    vector<int> statuses(pids.size(), 0);
    size_t nextIndex = 0;

    auto action = [&]() {
        if (nextIndex >= pids.size()) {   // should never happen
            failures.record(false);
            return;
        }
        size_t index = nextIndex++;
        int status = 0;
        pid_t returned = wait(&status);
        if (returned < 0) {
            failures.record(true);
            return;
        }
        returnedPids[index] = returned;
        statuses[index] = status;
    };

    metrics::Stats stats = metrics::benchmark("wait", action, kIterations, kWarmup);

    // After the benchmark: every returned pid must be one of ours, reaped once,
    // with exit code 0. Anything not reaped is reaped now (never leave zombies).
    vector<bool> reaped(pids.size(), false);
    for (size_t i = 0; i < returnedPids.size(); ++i) {
        if (returnedPids[i] == 0) {
            continue;
        }
        size_t match = pids.size();
        for (size_t j = 0; j < pids.size(); ++j) {
            if (pids[j] == returnedPids[i]) {
                match = j;
                break;
            }
        }
        if (match == pids.size() || reaped[match]) {
            failures.record(false);   // unknown pid, or the same pid twice
        } else {
            reaped[match] = true;
            if (!exitedWith(statuses[i], 0)) {
                failures.record(false);
            }
        }
    }
    for (size_t j = 0; j < pids.size(); ++j) {
        if (!reaped[j]) {
            failures.record(false);   // this child was not reaped by the benchmark
            int status = 0;
            waitForChildWithTimeout(pids[j], status);
        }
    }

    return finishBenchmark(stats, failures);
}

// Reads exactly `size` bytes from a pipe with an overall 10 s timeout.
// read() may deliver fewer bytes than asked, so we loop (partial reads).
// Returns false on timeout (ETIMEDOUT), on early end of data (EIO) or on error.
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
        pollfd waitFor;
        waitFor.fd = fd;
        waitFor.events = POLLIN;
        waitFor.revents = 0;
        int ready = poll(&waitFor, 1, static_cast<int>(remainingMs));
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
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

// One exec measurement. Returns true and sets `elapsedUs` if everything worked.
// Every descriptor is closed and the child is reaped on every path.
//
// HOW IT IS MEASURED: the forked child takes a CLOCK_MONOTONIC timestamp t0
// right before execv and puts it into the argument "t0Text" (20 digits, filled
// in with a tiny digit loop: no printf, no extra system call). The new program
// takes t1 as the first statement of main (runExecChild) and sends t0 and t1
// through the inherited pipe. The parent only reads them AFTER the fact, so
// pipe creation, fork, reading the pipe and reaping the child are all outside
// the interval t1 - t0.
bool measureOneExec(const string& exePath, double& elapsedUs, FailureTracker& failures) {
    int fds[2];
    if (pipe(fds) != 0) {   // no O_CLOEXEC on purpose: the write end must survive exec
        failures.record(true);
        return false;
    }

    // Arguments are built BEFORE fork().
    char exeCopy[4096];
    snprintf(exeCopy, sizeof(exeCopy), "%s", exePath.c_str());
    char flag[] = "--exec-child";
    char fdText[16];
    snprintf(fdText, sizeof(fdText), "%d", fds[1]);
    char t0Text[21];
    memset(t0Text, '0', 20);
    t0Text[20] = '\0';
    char* argv[] = {exeCopy, flag, fdText, t0Text, nullptr};

    pid_t pid = fork();
    if (pid < 0) {
        failures.record(true);
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    if (pid == 0) {
        close(fds[0]);   // the child only writes (done BEFORE t0, so not measured)
        unsigned long long t0 = static_cast<unsigned long long>(monotonicNs());
        for (int i = 19; i >= 0; --i) {
            t0Text[i] = static_cast<char>('0' + (t0 % 10));
            t0 /= 10;
        }
        execv(exeCopy, argv);
        _exit(127);      // only reached if execv failed
    }

    close(fds[1]);       // parent: the child is now the only writer
    unsigned long long stamps[2] = {0, 0};
    bool gotStamps = readFully(fds[0], stamps, sizeof(stamps));
    if (!gotStamps) {
        failures.record(true);   // errno = ETIMEDOUT, EIO or the read error
    }
    close(fds[0]);

    if (!gotStamps) {
        kill(pid, SIGKILL);      // the child is stuck or failed: make sure it ends
        int status = 0;
        waitForChildWithTimeout(pid, status);
        return false;
    }
    if (!reapAndCheck(pid, 0, failures)) {
        return false;
    }
    if (stamps[1] < stamps[0]) {
        failures.record(false);  // impossible order of timestamps
        return false;
    }
    elapsedUs = static_cast<double>(stamps[1] - stamps[0]) / 1000.0;
    return true;
}

// C. Benchmark: exec (startup latency).
// A successful exec replaces the process, so the old program can never measure
// its own return time. That is why the time comes from two timestamps (see
// measureOneExec). THIS MEASURES the exec transition PLUS the startup of the
// new program (kernel loads the file, the dynamic loader maps libstdc++/libc,
// static initialisers run, main starts). It is NOT the pure kernel-only
// duration of execve.
// metrics::benchmark cannot time this (the interval is measured in two
// different processes), so the times are collected here and passed to
// metrics::computeStats. The loop-level CPU, cycle, memory and page fault
// numbers are measured around the whole loop like benchmark() does. They
// include fork, pipe and reaping (parent only).
bool benchmarkExec(const string& exePath) {
    FailureTracker failures;
    vector<double> timesUs;
    timesUs.reserve(kIterations);
    cout.flush();

    for (int i = 0; i < kWarmup; ++i) {   // warm-up: not measured
        double ignored = 0.0;
        measureOneExec(exePath, ignored, failures);
    }

    double cpuBeforeUs = metrics::processCpuMicroseconds();
    unsigned long long cyclesBefore = metrics::processCpuCycles();
    metrics::MemoryInfo memoryBefore = metrics::readMemory();

    for (int i = 0; i < kIterations; ++i) {
        double elapsedUs = 0.0;
        if (measureOneExec(exePath, elapsedUs, failures)) {
            timesUs.push_back(elapsedUs);
        }
    }

    double cpuAfterUs = metrics::processCpuMicroseconds();
    unsigned long long cyclesAfter = metrics::processCpuCycles();
    metrics::MemoryInfo memoryAfter = metrics::readMemory();

    metrics::Stats stats = metrics::computeStats(
        "exec", timesUs,
        cpuAfterUs - cpuBeforeUs,
        static_cast<double>(memoryAfter.workingSetBytes - memoryBefore.workingSetBytes) / 1024.0,
        memoryAfter.pageFaults - memoryBefore.pageFaults,
        cyclesAfter - cyclesBefore);
    return finishBenchmark(stats, failures);
}

// D. Benchmark: fork_exec_wait. The timed action is the complete launch-and-
// finish cycle: fork, execv of this same program ("--child 0") and wait.
// This is the number to compare with Windows CreateProcess_plus_Wait.
// The wait is a polling wait with a 10 s timeout (child killed on timeout).
// The cleanup (untimed) checks that the child exited normally with code 0.
// Exit code 127 means execv failed in the child, so exec success is verified
// through the child's exit code.
bool benchmarkForkExecWait(const string& exePath) {
    FailureTracker failures;
    char exeCopy[4096];
    snprintf(exeCopy, sizeof(exeCopy), "%s", exePath.c_str());
    char flag[] = "--child";
    char zero[] = "0";
    char* argv[] = {exeCopy, flag, zero, nullptr};
    cout.flush();

    pid_t childPid = -1;
    int status = 0;
    int waitResult = -2;   // -2 = nothing launched, 1 = reaped, 0 = timeout, -1 = error

    auto action = [&]() {
        waitResult = -2;
        childPid = fork();
        if (childPid == 0) {
            execv(exeCopy, argv);
            _exit(127);    // only reached if execv failed
        }
        if (childPid < 0) {
            failures.record(true);
            return;
        }
        waitResult = waitForChildWithTimeout(childPid, status);
        if (waitResult != 1) {
            failures.record(true);
        }
    };

    auto cleanup = [&]() {
        if (waitResult == 1 && !exitedWith(status, 0)) {
            failures.record(false);
        }
        if (waitResult == -1 && childPid > 0) {
            killAndReap(childPid);   // waitpid failed: do not leave the child behind
        }
        childPid = -1;
        waitResult = -2;
    };

    metrics::Stats stats = metrics::benchmark("fork_exec_wait", action,
                                              kIterations, kWarmup, cleanup);
    return finishBenchmark(stats, failures);
}

// Runs all benchmarks plus the descriptor leak check and the zombie check.
// Returns how many problems there were.
int runAllBenchmarks(const string& exePath) {
    cout << "\n---------- PART 2: BENCHMARKS (timed, no logging inside) ----------\n";
    cout << "(Process creation is slow: this part can take several seconds.)\n";
    int problems = 0;

    int before = countOpenDescriptors("before Part 2");
    if (before >= 0) {
        cout << "Open descriptors before Part 2: " << before << "\n";
    }

    if (!benchmarkFork())                  ++problems;
    if (!benchmarkWait())                  ++problems;
    if (!benchmarkExec(exePath))           ++problems;
    if (!benchmarkForkExecWait(exePath))   ++problems;

    if (!verifyNoChildrenLeft("after Part 2")) ++problems;

    int after = countOpenDescriptors("after Part 2");
    if (after >= 0) {
        cout << "Open descriptors after Part 2 : " << after << "\n";
    }

    // Leak check: a forgotten close() of a pipe end would grow the count.
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

    cout << "=== Linux Process: demo + benchmarks ===\n\n";

    string exePath;
    if (!getOwnPath(exePath)) {
        cout << "Cannot find the path of this program, so no child can be started.\n";
        return 1;
    }

    if (!runDemo(exePath)) {
        cout << "\nDemo FAILED, so the benchmarks were skipped.\n";
        return 1;
    }

    int problems = runAllBenchmarks(exePath);
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
    if (argc >= 2 && strcmp(argv[1], "--exec-child") == 0) {
        return runExecChild(argc, argv);
    }
    if (argc >= 2 && strcmp(argv[1], "--child") == 0) {
        _exit(argc >= 3 ? atoi(argv[2]) : 0);
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