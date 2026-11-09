// =============================================================================
// Permissions_Linux.cpp
//
// Linux side of the OS system-call comparison project.
//
// PART 1: DEMO
//   A. chmod()  - change file permissions and verify write access is blocked.
//   B. chown()  - change the file's group to one the current user belongs to.
//   C. umask()  - change the default permissions of a newly created file.
//   D. One deliberate failure: chmod() on a missing file.
//
// PART 2: BENCHMARKS
//   chmod, chown and umask, each with 1000 iterations and 50 warm-ups.
//
// PART 3: Console output is also saved to:
//   Linux/Permissions/Permissions_Linux.txt
//
// IMPORTANT:
//   - Run as a NORMAL USER, not root. Root can bypass ordinary file permissions.
//   - The program refuses to overwrite an existing test file.
//   - It only deletes files that it successfully created itself.
//   - The original file permissions, ownership and umask are restored.
//
// Compile and run from the REPOSITORY ROOT:
//   g++ -std=c++17 Linux/Permissions/Permissions_Linux.cpp -o Linux/Permissions/permissions
//   ./Linux/Permissions/permissions
//
// Exit codes:
//   0 = everything OK
//   1 = demo failed
//   2 = benchmark problems
// =============================================================================

#include "../../common/logger_linux.hpp"
#include "../../common/metrics_linux.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <exception>
#include <fcntl.h>
#include <fstream>
#include <grp.h>
#include <iostream>
#include <streambuf>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

using namespace std;

// ----------------------------- Settings --------------------------------------

const char* const kTestFile = "permissions_test.txt";
const char* const kUmaskTestFile = "permissions_umask_test.txt";
const char* const kMissingFile = "permissions_missing_does_not_exist.txt";

const char* const kTranscriptFile =
    "Linux/Permissions/Permissions_Linux.txt";

const int kIterations = 1000;
const int kWarmup = 50;

const mode_t kReadOnlyMode = 0400;
const mode_t kOriginalTestMode = 0600;
const mode_t kBenchmarkMode = 0640;

const mode_t kDemoUmask = 0027;
const mode_t kBenchmarkUmask = 0027;

const int kDescriptorLeakTolerance = 5;

const size_t kTestDataSize = 52;

// ----------------------------- Tee stream buffer -----------------------------

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

// ----------------------------- Small helpers --------------------------------

vector<char> buildTestData(size_t size) {
    vector<char> data(size);

    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<char>('A' + (i % 26));
    }

    return data;
}

// Writes every byte, handling partial write() calls and EINTR.
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

        if (written == 0) {
            errno = EIO;
            return false;
        }

        total += static_cast<size_t>(written);
    }

    return true;
}

// Counts failures inside benchmark lambdas.
// Do not log from inside a timed lambda.
struct FailureTracker {
    int count = 0;
    int firstError = 0;

    void record(bool apiFailed) {
        if (count == 0) {
            firstError = apiFailed ? errno : EIO;
        }

        ++count;
    }
};

// Prints benchmark results and saves the CSV row only if every call succeeded.
bool finishBenchmark(const metrics::Stats& stats,
                     const FailureTracker& failures) {
    metrics::printStats(stats);

    if (failures.count > 0) {
        string logName = stats.callName + " benchmark";

        string detail = to_string(failures.count) +
                        " failed call(s) in warm-up + timed loop";

        cout << "!!! WARNING: " << stats.callName << " had "
             << failures.count
             << " failed call(s). Results are NOT valid and were NOT saved.\n";

        errno = failures.firstError;
        LOG_SYSCALL(logName.c_str(), false, detail.c_str());

        return false;
    }

    if (!metrics::appendStatsToCsv(stats)) {
        cout << "!!! WARNING: CSV row for " << stats.callName
             << " was NOT saved.\n";

        return false;
    }

    return true;
}

// Checks whether a path already exists without following symlinks.
bool pathExists(const char* path) {
    struct stat info;
    return lstat(path, &info) == 0;
}

// Creates a test file exclusively.
// O_EXCL ensures that an existing file is never overwritten.
bool createTestFile(const char* path, const char* label) {
    vector<char> data = buildTestData(kTestDataSize);

    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);

    LOG_SYSCALL("open", fd >= 0, label);

    if (fd < 0) {
        return false;
    }

    bool writeOk = writeFully(fd, data.data(), data.size());

    LOG_SYSCALL("write", writeOk, label);

    int savedError = errno;

    bool closeOk = (close(fd) == 0);

    if (!closeOk) {
        LOG_SYSCALL("close", false, label);
    } else {
        LOG_SYSCALL("close", true, label);
    }

    if (!writeOk || !closeOk) {
        if (!writeOk) {
            errno = savedError;
        }

        unlink(path);
        return false;
    }

    return true;
}

// Removes a file created by this program.
bool removeTestFile(const char* path, const char* detail) {
    if (unlink(path) == 0) {
        LOG_SYSCALL("unlink", true, detail);
        return true;
    }

    if (errno == ENOENT) {
        return true;
    }

    LOG_SYSCALL("unlink", false, detail);
    return false;
}

// Returns the permission bits only.
mode_t permissionBits(const struct stat& info) {
    return info.st_mode & 07777;
}

// ----------------------------- Ownership helpers ----------------------------

// Finds another group that the current user belongs to.
// If no alternative group exists, the current group is returned.
//
// A normal user may change a file's group to one of their supplementary groups.
bool findTargetGroup(gid_t currentGroup, gid_t& targetGroup) {
    int count = getgroups(0, nullptr);

    if (count < 0) {
        LOG_SYSCALL("getgroups", false, "count supplementary groups");
        return false;
    }

    vector<gid_t> groups(static_cast<size_t>(count));

    if (count > 0) {
        int received = getgroups(count, groups.data());

        LOG_SYSCALL("getgroups", received >= 0,
                    "read supplementary groups");

        if (received < 0) {
            return false;
        }

        for (int i = 0; i < received; ++i) {
            if (groups[i] != currentGroup) {
                targetGroup = groups[i];
                return true;
            }
        }
    }

    // No alternative supplementary group is available.
    // chown() can still be demonstrated by setting the existing group.
    targetGroup = currentGroup;
    return true;
}

// Prints a group's numeric ID.
void printGroup(const char* label, gid_t group) {
    cout << label << group << "\n";
}

// ----------------------------- PART 1: DEMO ----------------------------------

// A. chmod() demonstration.
//
// The file starts with owner read/write permissions (0600).
// chmod(0400) removes the owner's write permission.
// The program then verifies that writing is denied and reading still works.
bool demoChmod(const struct stat& originalInfo) {
    cout << "\n-- A. chmod() route (change file permissions) --\n";

    if (chmod(kTestFile, kReadOnlyMode) == 0) {
        LOG_SYSCALL("chmod", true, "set permissions to 0400");
    } else {
        LOG_SYSCALL("chmod", false, "set permissions to 0400");
        return false;
    }

    struct stat changedInfo {};

    bool statOk = (stat(kTestFile, &changedInfo) == 0);

    LOG_SYSCALL("stat", statOk, "verify changed permissions");

    if (!statOk) {
        return false;
    }

    cout << "Permissions after chmod: 0"
         << oct << permissionBits(changedInfo) << dec << "\n";

    if (permissionBits(changedInfo) != kReadOnlyMode) {
        cout << "Verification FAILED: permissions are not 0400.\n";
        errno = EIO;
        LOG_SYSCALL("VerifyChmod", false, "expected permissions 0400");
        return false;
    }

    // Try opening the file for writing.
    cout << "Opening the file for writing (this FAILURE is expected):\n";

    int writeFd = open(kTestFile, O_WRONLY);

    int writeError = errno;

    bool writeBlocked = (writeFd < 0 && writeError == EACCES);

    if (writeFd >= 0) {
        close(writeFd);
    }

    LOG_SYSCALL("open", writeBlocked,
                "write blocked, expected to fail");

    if (writeBlocked) {
        cout << "Verification PASSED: chmod removed write access.\n";
        LOG_SYSCALL("VerifyWriteBlocked", true,
                    "chmod removed write access");
    } else {
        cout << "Verification FAILED: write access was not denied.\n";

        errno = EIO;
        LOG_SYSCALL("VerifyWriteBlocked", false,
                    "chmod should block writing");

        return false;
    }

    // Reading should still work.
    int readFd = open(kTestFile, O_RDONLY);

    bool readOk = (readFd >= 0);

    if (readFd >= 0) {
        bool closeOk = (close(readFd) == 0);
        LOG_SYSCALL("close", closeOk, "read verification handle");
        readOk = readOk && closeOk;
    }

    LOG_SYSCALL("open", readOk, "read remains allowed");

    if (readOk) {
        cout << "Verification PASSED: reading is still allowed.\n";
        LOG_SYSCALL("VerifyReadAllowed", true,
                    "read access remains allowed");
    } else {
        cout << "Verification FAILED: reading was not possible.\n";

        errno = EIO;
        LOG_SYSCALL("VerifyReadAllowed", false,
                    "read access should remain allowed");

        return false;
    }

    // Restore the original permission bits.
    bool restored = (chmod(kTestFile,
                           permissionBits(originalInfo)) == 0);

    LOG_SYSCALL("chmod", restored, "restore original permissions");

    if (!restored) {
        return false;
    }

    struct stat restoredInfo {};

    bool verifyRestore = (stat(kTestFile, &restoredInfo) == 0);

    LOG_SYSCALL("stat", verifyRestore, "verify restored permissions");

    if (!verifyRestore ||
        permissionBits(restoredInfo) != permissionBits(originalInfo)) {
        cout << "Verification FAILED: original permissions were not restored.\n";

        errno = EIO;
        LOG_SYSCALL("VerifyChmodRestore", false,
                    "original permissions should be restored");

        return false;
    }

    cout << "Verification PASSED: original permissions restored.\n";
    LOG_SYSCALL("VerifyChmodRestore", true,
                "original permissions restored");

    return true;
}

// B. chown() demonstration.
//
// A normal user cannot usually change a file's owner to another user.
// However, the owner can change the file's group to a group they belong to.
//
// We therefore keep the owner unchanged and change the group.
bool demoChown(const struct stat& originalInfo, gid_t targetGroup) {
    cout << "\n-- B. chown() route (change file ownership/group) --\n";

    printGroup("Original group ID: ", originalInfo.st_gid);
    printGroup("Target group ID  : ", targetGroup);

    if (targetGroup == originalInfo.st_gid) {
        cout << "No alternative supplementary group was found.\n"
             << "The chown() call will set the file to its existing group.\n";
    }

    bool changed = (chown(kTestFile,
                          static_cast<uid_t>(-1),
                          targetGroup) == 0);

    LOG_SYSCALL("chown", changed, "change file group");

    if (!changed) {
        return false;
    }

    struct stat changedInfo {};

    bool statOk = (stat(kTestFile, &changedInfo) == 0);

    LOG_SYSCALL("stat", statOk, "verify changed ownership");

    if (!statOk) {
        return false;
    }

    cout << "Group ID after chown: " << changedInfo.st_gid << "\n";

    if (changedInfo.st_uid != originalInfo.st_uid ||
        changedInfo.st_gid != targetGroup) {
        cout << "Verification FAILED: ownership does not match.\n";

        errno = EIO;
        LOG_SYSCALL("VerifyChown", false,
                    "verify owner and group");

        return false;
    }

    cout << "Verification PASSED: file ownership/group verified.\n";
    LOG_SYSCALL("VerifyChown", true,
                "owner unchanged and group verified");

    // Restore original owner and group.
    bool restored = (chown(kTestFile,
                           originalInfo.st_uid,
                           originalInfo.st_gid) == 0);

    LOG_SYSCALL("chown", restored, "restore original ownership");

    if (!restored) {
        return false;
    }

    cout << "Original ownership restored.\n";

    return true;
}

// C. umask() demonstration.
//
// umask controls which permission bits are removed from newly created files.
//
// Requested mode: 0666
// umask:          0027
// Result:         0640
//
// Formula:
// 0666 & ~0027 = 0640
bool demoUmask() {
    cout << "\n-- C. umask() route (default permissions for new files) --\n";

    if (pathExists(kUmaskTestFile)) {
        cout << "Refusing to overwrite existing file: "
             << kUmaskTestFile << "\n";

        errno = EEXIST;
        LOG_SYSCALL("open", false, "umask test file already exists");

        return false;
    }

    // Save the original mask and set the demonstration mask.
    mode_t originalMask = umask(kDemoUmask);

    cout << "Original umask: 0" << oct << originalMask << dec << "\n";
    cout << "New umask     : 0" << oct << kDemoUmask << dec << "\n";

    LOG_SYSCALL("umask", true, "set demonstration umask to 0027");

    // Create a new file requesting 0666 permissions.
    int fd = open(kUmaskTestFile,
                  O_CREAT | O_EXCL | O_WRONLY,
                  0666);

    int openError = errno;

    // Restore the original mask immediately.
    umask(originalMask);

    LOG_SYSCALL("umask", true, "restore original umask");

    if (fd < 0) {
        errno = openError;
        LOG_SYSCALL("open", false, "create umask test file");
        return false;
    }

    LOG_SYSCALL("open", true, "create file with requested mode 0666");

    struct stat info {};

    bool statOk = (fstat(fd, &info) == 0);

    LOG_SYSCALL("fstat", statOk, "check new file permissions");

    bool closeOk = (close(fd) == 0);

    LOG_SYSCALL("close", closeOk, "close umask test file");

    if (!statOk || !closeOk) {
        removeTestFile(kUmaskTestFile, "remove umask test file");
        return false;
    }

    mode_t actualMode = permissionBits(info);

    cout << "Requested file mode: 0666\n";
    cout << "Actual file mode   : 0"
         << oct << actualMode << dec << "\n";

    bool correct = (actualMode == 0640);

    if (correct) {
        cout << "Verification PASSED: umask produced permissions 0640.\n";
        LOG_SYSCALL("VerifyUmask", true,
                    "0666 masked by 0027 produced 0640");
    } else {
        cout << "Verification FAILED: expected permissions 0640.\n";

        errno = EIO;
        LOG_SYSCALL("VerifyUmask", false,
                    "expected permissions 0640");

    }

    bool removed = removeTestFile(kUmaskTestFile,
                                  "remove umask test file");

    return correct && removed;
}

// D. Deliberate expected failure.
//
// chmod() must fail with ENOENT when the target file does not exist.
bool demoMissingFile() {
    cout << "\n-- D. chmod() on a file that does not exist --\n";

    if (pathExists(kMissingFile)) {
        cout << "Skipped: " << kMissingFile
             << " already exists. Rename or remove it first.\n";

        errno = EEXIST;
        LOG_SYSCALL("chmod", false,
                    "missing-file test path already exists");

        return false;
    }

    cout << "chmod on a missing file (this FAILURE is expected, "
         << "ENOENT):\n";

    int result = chmod(kMissingFile, 0600);
    int errorCode = errno;

    bool expectedFailure = (result == -1 && errorCode == ENOENT);

    LOG_SYSCALL("chmod", expectedFailure,
                "missing file, expected ENOENT");

    if (!expectedFailure) {
        cout << "UNEXPECTED: chmod did not fail with ENOENT.\n";
        return false;
    }

    cout << "Verification PASSED: missing file produced ENOENT.\n";
    LOG_SYSCALL("VerifyExpectedFailure", true,
                "chmod failed with ENOENT as expected");

    return true;
}

// Restores the original state and deletes the main test file.
bool cleanupTestFile(const struct stat& originalInfo) {
    cout << "\n-- E. Cleanup (restore permissions/ownership and delete) --\n";

    bool chmodOk = (chmod(kTestFile,
                          permissionBits(originalInfo)) == 0);

    LOG_SYSCALL("chmod", chmodOk, "cleanup: restore original permissions");

    bool chownOk = (chown(kTestFile,
                          originalInfo.st_uid,
                          originalInfo.st_gid) == 0);

    LOG_SYSCALL("chown", chownOk, "cleanup: restore original ownership");

    bool deleted = removeTestFile(kTestFile,
                                  "delete permissions test file");

    return chmodOk && chownOk && deleted;
}

// Runs the complete demonstration.
bool runDemo(gid_t& targetGroup, struct stat& originalInfo) {
    cout << "---------- PART 1: DEMO (logged, not timed) ----------\n";

    cout << "Test file: " << kTestFile << "\n";

    if (geteuid() == 0) {
        cout << "ERROR: this program must run as a normal user, not root.\n"
             << "Root can bypass the permission-denied test used by chmod().\n";

        errno = EPERM;
        LOG_SYSCALL("geteuid", false, "program must not run as root");

        return false;
    }

    if (pathExists(kTestFile)) {
        cout << "ERROR: " << kTestFile
             << " already exists. It will NOT be overwritten or deleted.\n"
             << "Remove the old test file manually if it belongs to an earlier run.\n";

        errno = EEXIST;
        LOG_SYSCALL("open", false, "test file already exists");

        return false;
    }

    if (!createTestFile(kTestFile, "create permissions test file")) {
        return false;
    }

    bool statOk = (stat(kTestFile, &originalInfo) == 0);

    LOG_SYSCALL("stat", statOk, "save original file ownership and permissions");

    if (!statOk) {
        removeTestFile(kTestFile, "cleanup after stat failure");
        return false;
    }

    cout << "Original permissions: 0"
         << oct << permissionBits(originalInfo) << dec << "\n";

    cout << "Original owner UID : " << originalInfo.st_uid << "\n";
    cout << "Original group GID : " << originalInfo.st_gid << "\n";

    bool groupOk = findTargetGroup(originalInfo.st_gid, targetGroup);

    if (!groupOk) {
        cleanupTestFile(originalInfo);
        return false;
    }

    bool allOk = true;

    if (!demoChmod(originalInfo)) {
        allOk = false;
    }

    if (allOk && !demoChown(originalInfo, targetGroup)) {
        allOk = false;
    }

    if (allOk && !demoUmask()) {
        allOk = false;
    }

    if (allOk && !demoMissingFile()) {
        allOk = false;
    }

    // Cleanup runs regardless of whether an earlier demonstration failed.
    bool cleaned = cleanupTestFile(originalInfo);

    return allOk && cleaned;
}

// ----------------------------- PART 2: BENCHMARKS ----------------------------

// Creates a fresh benchmark file and saves its original metadata.
bool prepareBenchmarkFile(struct stat& originalInfo) {
    if (pathExists(kTestFile)) {
        cout << "ERROR: benchmark test file already exists. Refusing to overwrite it.\n";

        errno = EEXIST;
        LOG_SYSCALL("open", false, "benchmark test file already exists");

        return false;
    }

    if (!createTestFile(kTestFile, "benchmark test file setup")) {
        return false;
    }

    bool statOk = (stat(kTestFile, &originalInfo) == 0);

    LOG_SYSCALL("stat", statOk, "save benchmark file metadata");

    if (!statOk) {
        removeTestFile(kTestFile, "remove benchmark file after setup failure");
        return false;
    }

    return true;
}

// 1. Benchmark chmod().
//
// The action changes permissions to 0640.
// Untimed cleanup restores them to 0600.
// Therefore, each timed call changes the file's permission bits.
bool benchmarkChmod(const struct stat& originalInfo) {
    FailureTracker failures;

    auto action = [&]() {
        if (chmod(kTestFile, kBenchmarkMode) != 0) {
            failures.record(true);
        }
    };

    auto cleanup = [&]() {
        if (chmod(kTestFile, permissionBits(originalInfo)) != 0) {
            failures.record(true);
        }
    };

    metrics::Stats stats = metrics::benchmark(
        "chmod", action, kIterations, kWarmup, cleanup);

    bool restored = (chmod(kTestFile,
                           permissionBits(originalInfo)) == 0);

    LOG_SYSCALL("chmod", restored,
                "final benchmark permission restoration");

    bool clean = finishBenchmark(stats, failures);

    return clean && restored;
}
// 2. Benchmark chown().
//
// The action changes the group to an eligible supplementary group.
// Untimed cleanup restores the original group.
//
// If the user has no alternative supplementary group, the target group
// is the existing group. In that case, the benchmark measures chown()
// without an actual group change; this limitation must be stated in the report.
bool benchmarkChown(gid_t targetGroup, const struct stat& originalInfo) {
    FailureTracker failures;

    auto action = [&]() {
        if (chown(kTestFile,
                  static_cast<uid_t>(-1),
                  targetGroup) != 0) {
            failures.record(true);
        }
    };

    auto cleanup = [&]() {
        if (chown(kTestFile,
                  static_cast<uid_t>(-1),
                  originalInfo.st_gid) != 0) {
            failures.record(true);
        }
    };

    metrics::Stats stats = metrics::benchmark(
        "chown", action, kIterations, kWarmup, cleanup);

    bool restored = (chown(kTestFile,
                           originalInfo.st_uid,
                           originalInfo.st_gid) == 0);

    LOG_SYSCALL("chown", restored,
                "final benchmark ownership restoration");

    bool clean = finishBenchmark(stats, failures);

    return clean && restored;
}

// 3. Benchmark umask().
//
// umask() does not report failure through its return value.
// It returns the previous mask.
//
// The action sets 0027. Untimed cleanup restores the original mask.
bool benchmarkUmask() {
    FailureTracker failures;

    // Save the process's current mask without leaving it changed.
    mode_t originalMask = umask(0);
    umask(originalMask);

    auto action = [&]() {
        mode_t previous = umask(kBenchmarkUmask);

        if (previous != originalMask) {
            failures.record(false);
        }
    };

    auto cleanup = [&]() {
        mode_t previous = umask(originalMask);

        if (previous != kBenchmarkUmask) {
            failures.record(false);
        }
    };

    metrics::Stats stats = metrics::benchmark(
        "umask", action, kIterations, kWarmup, cleanup);

    mode_t previous = umask(originalMask);

    bool restored = (previous == originalMask);
    
    LOG_SYSCALL("umask", true,
                "final benchmark umask restoration");
    
    if (!restored) {
        cout << "WARNING: unexpected previous umask value: 0"
             << oct << previous << dec << "\n";
    }

    bool clean = finishBenchmark(stats, failures);

    return clean && restored;
}

// Counts open descriptors using /proc/self/fd.
// The directory descriptor itself is included in the count both times.
int countOpenDescriptors(const char* when) {
    DIR* directory = opendir("/proc/self/fd");

    LOG_SYSCALL("opendir", directory != nullptr, when);

    if (directory == nullptr) {
        return -1;
    }

    int total = 0;
    dirent* entry = nullptr;

    while ((entry = readdir(directory)) != nullptr) {
        if (strcmp(entry->d_name, ".") != 0 &&
            strcmp(entry->d_name, "..") != 0) {
            ++total;
        }
    }

    bool closeOk = (closedir(directory) == 0);

    LOG_SYSCALL("closedir", closeOk, when);

    if (!closeOk) {
        return -1;
    }

    return total;
}

// Runs all three benchmarks and checks for descriptor leaks.
int runAllBenchmarks(gid_t targetGroup, const struct stat& originalInfo) {
    cout << "\n---------- PART 2: BENCHMARKS (timed, no logging inside) ----------\n";
    cout << "Iterations: " << kIterations
         << " | Warm-up: " << kWarmup << "\n";

    int problems = 0;

    int before = countOpenDescriptors("before Part 2");

    if (before >= 0) {
        cout << "Open descriptors before Part 2: " << before << "\n";
    }

    if (!benchmarkChmod(originalInfo)) {
        ++problems;
    }

    if (!benchmarkChown(targetGroup, originalInfo)) {
        ++problems;
    }

    if (!benchmarkUmask()) {
        ++problems;
    }

    // The benchmark functions must leave the file in its original state.
    bool chmodOk = (chmod(kTestFile,
                          permissionBits(originalInfo)) == 0);

    LOG_SYSCALL("chmod", chmodOk,
                "final cleanup: restore original permissions");

    bool chownOk = (chown(kTestFile,
                          originalInfo.st_uid,
                          originalInfo.st_gid) == 0);

    LOG_SYSCALL("chown", chownOk,
                "final cleanup: restore original ownership");

    bool deleted = removeTestFile(kTestFile,
                                  "delete benchmark test file");

    if (!chmodOk || !chownOk || !deleted) {
        ++problems;
    }

    int after = countOpenDescriptors("after Part 2");

    if (after >= 0) {
        cout << "Open descriptors after Part 2 : " << after << "\n";
    }

    if (before >= 0 && after >= 0) {
        if (after > before + kDescriptorLeakTolerance) {
            cout << "!!! WARNING: file descriptors may be LEAKING ("
                 << (after - before) << " more than before).\n";

            ++problems;
        } else {
            cout << "Descriptor leak check PASSED.\n";
        }
    } else {
        cout << "!!! WARNING: could not check descriptor count.\n";
        ++problems;
    }

    return problems;
}

// ----------------------------- Program flow ----------------------------------

int runAll() {
    logger::init();

    cout << "=== Linux Permissions: demo + benchmarks ===\n\n";

    gid_t targetGroup = 0;
    struct stat originalInfo {};

    if (!runDemo(targetGroup, originalInfo)) {
        cout << "\nDemo FAILED. Benchmarks were skipped.\n";
        return 1;
    }

    cout << "\n---------- PART 1 COMPLETED SUCCESSFULLY ----------\n";

    // Create a separate fresh file for the benchmark section.
    struct stat benchmarkInfo {};

    if (!prepareBenchmarkFile(benchmarkInfo)) {
        cout << "Could not prepare the benchmark file.\n";
        return 2;
    }

    // The benchmark file has the same owner/group as the demo file.
    int problems = runAllBenchmarks(targetGroup, benchmarkInfo);

    if (problems > 0) {
        cout << "\nFinished with " << problems
             << " benchmark problem(s). See warnings above.\n";

        return 2;
    }

    cout << "\nAll done. Results appended to results/Linux/linux_results.csv\n";

    return 0;
}

// ----------------------------- Main ------------------------------------------

int main() {
    // PART 3: copy everything written to cout into the transcript file.
    ofstream transcript(kTranscriptFile, ios::out | ios::trunc);

    streambuf* originalBuffer = cout.rdbuf();
    TeeBuffer tee(originalBuffer, transcript.rdbuf());

    bool teeActive = false;

    if (transcript.is_open()) {
        cout.rdbuf(&tee);
        teeActive = true;

        cout << "Console output is also being saved to "
             << kTranscriptFile << "\n";
    } else {
        cerr << "Warning: cannot open " << kTranscriptFile
             << " (run from the repository root). "
             << "Continuing without the text file.\n";
    }

    int exitCode = 1;

    try {
        exitCode = runAll();
    } catch (const exception& error) {
        cout << "Unexpected error: " << error.what() << endl;
    }

    // Restore cout before the TeeBuffer and transcript are destroyed.
    cout.flush();

    if (teeActive) {
        cout.rdbuf(originalBuffer);
    }

    return exitCode;
}