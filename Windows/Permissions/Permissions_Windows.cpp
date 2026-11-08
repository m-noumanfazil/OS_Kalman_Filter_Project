// =============================================================================
// Permissions_Windows.cpp
//
// Part of an OS project comparing Linux and Windows system calls (Windows side,
// Permissions category).
//
// What this program does:
//   PART 1  Demo (logged, not timed), on ONE test file "permissions_test.txt":
//             A. SetFileSecurity route: save the original DACL, replace it with
//                a "deny write" DACL, prove that writing is blocked and reading
//                still works, restore the original DACL, prove writing works
//             B. SetFileAttributes route: read-only flag blocks writing
//             C. one deliberate failure (SetFileSecurity on a missing file)
//             D. cleanup (restore, clear read-only, delete the test file)
//   PART 2  Benchmarks (1000 iterations, 50 warm-up each), timed separately:
//             SetFileSecurity, GetFileSecurity, SetFileAttributes
//           Results are printed and appended to results/Windows/windows_results.csv.
//   PART 3  Everything printed to the console is also saved to
//           Windows/Permissions/Permissions_Windows.txt (overwritten each run).
//
// SAFETY: the program only ever modifies permissions_test.txt, only uses
// DACL_SECURITY_INFORMATION (never owner, group or SACL), and needs no admin
// rights. If a run is killed and the file is left with a deny rule, repair it:
//     icacls permissions_test.txt /reset
//
// Compile and run from the REPOSITORY ROOT:
//   g++ -std=c++17 Windows\Permissions\Permissions_Windows.cpp -o Windows\Permissions\permissions.exe -lpsapi -ladvapi32
//   Windows\Permissions\permissions.exe
//
// Exit codes: 0 = everything OK, 1 = demo failed, 2 = a benchmark had problems.
// =============================================================================

// logger.hpp includes windows.h first, so this must come BEFORE the helpers.
// It makes QueryProcessCycleTime (used by metrics.hpp) visible on MinGW.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include "../../common/logger.hpp"
#include "../../common/metrics.hpp"
#include <sddl.h>   // SDDL strings <-> security descriptors (after the helpers)

#include <algorithm>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <streambuf>
#include <string>
#include <vector>
using namespace std;

// ----------------------------- Settings --------------------------------------

// The ONLY file this program ever modifies. If something goes wrong, run:
//     icacls permissions_test.txt /reset
const char* const kTestFile       = "permissions_test.txt";
const char* const kMissingFile    = "permissions_does_not_exist.txt";
const char* const kTranscriptFile = "Windows/Permissions/Permissions_Windows.txt";

// DENY list ("deny write"), written in SDDL (a text format for security rules):
//   D:              this is a DACL (the list of allow/deny rules)
//   (D;;0x6;;;WD)   DENY rights 0x6 = FILE_WRITE_DATA (0x2) | FILE_APPEND_DATA (0x4)
//                   to WD = "World" = Everyone
//   (A;;FA;;;WD)    ALLOW FA = File All access to Everyone
// Windows checks the rules IN ORDER. The deny rule comes first, so it wins for
// write access, while reading is still granted by the allow rule.
const char* const kDenyWriteSddl = "D:(D;;0x6;;;WD)(A;;FA;;;WD)";

// RECOVERY list: allow everything to Everyone. Only used to rescue a file.
const char* const kAllowAllSddl = "D:(A;;FA;;;WD)";

const int    kIterations = 1000;
const int    kWarmup     = 50;
const size_t kTestDataSize = 52;               // short deterministic content
const DWORD  kHandleLeakTolerance = 5;         // same as the other programs
const DWORD  kSpareBufferBytes = 256;          // extra room for GetFileSecurity

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

// Deterministic test data: A-Z repeated. Same bytes on every run.
vector<char> buildTestData(size_t size) {
    vector<char> data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<char>('A' + (i % 26));
    }
    return data;
}

// Returns true if the call worked AND moved the expected number of bytes.
// A call that "worked" but moved the wrong amount has no Windows error code,
// so we set one; otherwise the log line would show a stale, unrelated error.
bool transferSucceeded(BOOL apiOk, DWORD actual, DWORD expected) {
    if (apiOk && actual != expected) {
        SetLastError(ERROR_INVALID_DATA);
        return false;
    }
    return apiOk != 0;
}

// Counts failures inside benchmark lambdas. Lambdas must not log (logging
// would ruin the timing), so they only record here; the result is reported
// once, after the benchmark.
struct FailureTracker {
    int   count = 0;
    DWORD firstError = 0;

    // apiFailed = true : a Windows call returned failure, keep its GetLastError().
    // apiFailed = false: some other problem.
    // Call this IMMEDIATELY after the failing call so the error is not overwritten.
    void record(bool apiFailed) {
        if (count == 0) {
            firstError = apiFailed ? GetLastError() : ERROR_INVALID_DATA;
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
        // Printing may have changed the last error, so restore the saved one
        // right before logging.
        SetLastError(failures.firstError);
        LOG_SYSCALL(logName.c_str(), false, detail.c_str());
        return false;
    }

    if (!metrics::appendStatsToCsv(stats)) {
        cout << "!!! WARNING: CSV row for " << stats.callName << " was NOT saved.\n";
        return false;
    }
    return true;
}

// Owns one block of memory that Windows allocated for us (a security
// descriptor or an SDDL string) and gives it back with LocalFree when the
// object goes out of scope. This way no early return can leak it.
class LocalMemory {
public:
    LocalMemory() : pointer_(nullptr) {}
    ~LocalMemory() { reset(nullptr); }
    LocalMemory(const LocalMemory&) = delete;             // two owners would free twice
    LocalMemory& operator=(const LocalMemory&) = delete;

    void* get() const { return pointer_; }

    // Takes ownership of `newPointer` (and frees the old one, if any).
    void reset(void* newPointer) {
        if (pointer_ != nullptr) {
            LocalFree(pointer_);
        }
        pointer_ = newPointer;
    }

private:
    void* pointer_;
};

// ----------------------------- Test file helpers -----------------------------

bool testFileExists() {
    return GetFileAttributesA(kTestFile) != INVALID_FILE_ATTRIBUTES;
}

// Turns an SDDL text into a security descriptor owned by `holder`.
// The result is logged under `logName`. Returns false on failure.
bool buildDescriptor(const char* sddl, LocalMemory& holder, const char* logName) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    BOOL ok = ConvertStringSecurityDescriptorToSecurityDescriptorA(
        sddl, SDDL_REVISION_1, &descriptor, nullptr);
    LOG_SYSCALL(logName, ok != 0, sddl);
    if (ok) {
        holder.reset(descriptor);
    }
    return ok != 0;
}

// Last-resort rescue of the test file: allow everything, clear read-only,
// delete. Errors are ignored on purpose (this is a best-effort recovery).
void forceDeleteTestFile() {
    PSECURITY_DESCRIPTOR allowAll = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorA(
            kAllowAllSddl, SDDL_REVISION_1, &allowAll, nullptr)) {
        SetFileSecurityA(kTestFile, DACL_SECURITY_INFORMATION, allowAll);
        LocalFree(allowAll);
    }
    SetFileAttributesA(kTestFile, FILE_ATTRIBUTE_NORMAL);
    DeleteFileA(kTestFile);
}

// If a crashed earlier run left permissions_test.txt behind (possibly with a
// deny rule or read-only flag), clean it up before we start.
void recoverLeftoverFile() {
    if (!testFileExists()) {
        return;
    }
    forceDeleteTestFile();
    if (testFileExists()) {
        cout << "Warning: a leftover " << kTestFile << " was found but could NOT be removed.\n"
             << "         Repair it with: icacls " << kTestFile << " /reset\n";
    } else {
        cout << "A leftover " << kTestFile << " from an earlier run was found and cleaned.\n";
    }
}

// Creates the test file with a short known text. `forBenchmark` only changes
// the names in the log ("_setup" suffix), so setup lines are easy to spot.
bool createTestFile(bool forBenchmark) {
    const char* createName = forBenchmark ? "CreateFile_setup"  : "CreateFile";
    const char* writeName  = forBenchmark ? "WriteFile_setup"   : "WriteFile";
    const char* closeName  = forBenchmark ? "CloseHandle_setup" : "CloseHandle";

    vector<char> data = buildTestData(kTestDataSize);
    const DWORD dataSize = static_cast<DWORD>(data.size());

    HANDLE handle = CreateFileA(kTestFile, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    LOG_SYSCALL(createName, handle != INVALID_HANDLE_VALUE, kTestFile);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD bytesWritten = 0;
    BOOL writeOk = WriteFile(handle, data.data(), dataSize, &bytesWritten, nullptr);
    bool writeGood = transferSucceeded(writeOk, bytesWritten, dataSize);
    LOG_SYSCALL(writeName, writeGood, kTestFile);

    BOOL closeOk = CloseHandle(handle);   // always close, even if the write failed
    LOG_SYSCALL(closeName, closeOk != 0, kTestFile);

    return writeGood && closeOk;
}

// Reads the DACL of the test file into `descriptor` (a self-relative security
// descriptor stored in a vector). Uses the standard TWO-CALL PATTERN:
//   1st call: no buffer, size 0. It is EXPECTED to fail with
//             ERROR_INSUFFICIENT_BUFFER and tells us how many bytes are needed.
//   2nd call: buffer of exactly that size. This call is the one that is logged.
bool readDacl(const char* logName, const char* detail, vector<char>& descriptor) {
    DWORD needed = 0;
    BOOL first = GetFileSecurityA(kTestFile, DACL_SECURITY_INFORMATION, nullptr, 0, &needed);
    DWORD firstError = GetLastError();   // read IMMEDIATELY
    if (first || firstError != ERROR_INSUFFICIENT_BUFFER || needed == 0) {
        // Something other than the expected "buffer too small" happened
        // (for example the volume is not NTFS).
        SetLastError(first ? ERROR_INVALID_DATA : firstError);
        LOG_SYSCALL(logName, false, "size query failed unexpectedly");
        return false;
    }

    descriptor.assign(needed, 0);
    DWORD bufferSize = needed;
    BOOL second = GetFileSecurityA(kTestFile, DACL_SECURITY_INFORMATION,
                                   descriptor.data(), bufferSize, &needed);
    LOG_SYSCALL(logName, second != 0, detail);
    if (!second) {
        descriptor.clear();
    }
    return second != 0;
}

// Prints a descriptor's DACL as readable SDDL text (and logs the conversion).
// The text block comes from Windows, so it is freed with LocalFree.
bool printDacl(const vector<char>& descriptor, const char* title) {
    LPSTR text = nullptr;
    BOOL ok = ConvertSecurityDescriptorToStringSecurityDescriptorA(
        const_cast<char*>(descriptor.data()), SDDL_REVISION_1,
        DACL_SECURITY_INFORMATION, &text, nullptr);
    LOG_SYSCALL("ConvertSecurityDescriptorToString", ok != 0, title);
    LocalMemory textOwner;
    textOwner.reset(text);   // freed automatically at the end of this function
    if (!ok) {
        return false;
    }
    cout << title << ":\n    " << text << "\n";
    return true;
}

// Puts the test file back into a clean state: original DACL (if we saved one)
// and no read-only flag. Called on every path before the file is deleted.
bool restoreFileState(const vector<char>& original) {
    bool allOk = true;
    if (!original.empty()) {
        BOOL securityOk = SetFileSecurityA(kTestFile, DACL_SECURITY_INFORMATION,
                                           const_cast<char*>(original.data()));
        LOG_SYSCALL("SetFileSecurity_restore", securityOk != 0, "restore original DACL");
        if (!securityOk) {
            allOk = false;
        }
    }
    BOOL attributesOk = SetFileAttributesA(kTestFile, FILE_ATTRIBUTE_NORMAL);
    LOG_SYSCALL("SetFileAttributes_restore", attributesOk != 0, "clear read-only");
    if (!attributesOk) {
        allOk = false;
    }
    return allOk;
}

// Full cleanup: restore, then delete. If restoring fails, fall back to the
// force-delete rescue so no file is left with a deny rule. A file that does
// not exist (never created) is fine and stays silent.
bool cleanupTestFile(const vector<char>& original) {
    if (!testFileExists()) {
        return true;
    }
    bool restored = restoreFileState(original);
    if (!restored) {
        cout << "Warning: could not restore the original permissions, trying the rescue.\n";
        forceDeleteTestFile();
        return !testFileExists();
    }
    BOOL deleted = DeleteFileA(kTestFile);
    LOG_SYSCALL("DeleteFile", deleted != 0, kTestFile);
    return deleted != 0;
}

// ----------------------------- PART 1: demo ----------------------------------

// Tries to open the test file for writing and checks the answer.
//   expectBlocked = true : the open MUST fail with error 5 (access denied)
//   expectBlocked = false: the open MUST succeed
// Prints and logs PASSED or FAILED as the pseudo call `verifyName`.
bool verifyWriteAccess(bool expectBlocked, const char* verifyName, const char* verifyDetail) {
    bool verified = false;

    if (expectBlocked) {
        cout << "Opening the file for writing (this FAILURE is expected, error 5):\n";
        HANDLE handle = CreateFileA(kTestFile, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD errorCode = GetLastError();   // read before anything can change it
        LOG_SYSCALL("CreateFile", handle != INVALID_HANDLE_VALUE, "write blocked, expected to fail");
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
            cout << "UNEXPECTED: the file could be opened for writing!\n";
        } else if (errorCode != ERROR_ACCESS_DENIED) {
            cout << "UNEXPECTED: it failed, but with error " << errorCode << " instead of 5.\n";
        } else {
            verified = true;
        }
    } else {
        HANDLE handle = CreateFileA(kTestFile, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        LOG_SYSCALL("CreateFile", handle != INVALID_HANDLE_VALUE, "write allowed");
        if (handle != INVALID_HANDLE_VALUE) {
            BOOL closed = CloseHandle(handle);
            LOG_SYSCALL("CloseHandle", closed != 0, kTestFile);
            verified = (closed != 0);
        }
    }

    if (verified) {
        cout << "Verification PASSED: " << verifyDetail << ".\n";
        LOG_SYSCALL(verifyName, true, verifyDetail);
    } else {
        cout << "Verification FAILED: " << verifyDetail << " did not hold.\n";
        // The logger has no plain-message function, so we log the verification
        // as a pseudo call and give it a meaningful error code first.
        SetLastError(ERROR_INVALID_DATA);
        LOG_SYSCALL(verifyName, false, verifyDetail);
    }
    return verified;
}

// Proves that READING still works while writing is denied.
bool verifyReadStillAllowed() {
    HANDLE handle = CreateFileA(kTestFile, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    LOG_SYSCALL("CreateFile", handle != INVALID_HANDLE_VALUE, "read still allowed");
    bool verified = false;
    if (handle != INVALID_HANDLE_VALUE) {
        BOOL closed = CloseHandle(handle);
        LOG_SYSCALL("CloseHandle", closed != 0, kTestFile);
        verified = (closed != 0);
    }
    if (verified) {
        cout << "Verification PASSED: reading is still allowed (the deny rule covers write only).\n";
        LOG_SYSCALL("VerifyReadStillAllowed", true, "read still allowed");
    } else {
        cout << "Verification FAILED: reading was not possible.\n";
        SetLastError(ERROR_INVALID_DATA);
        LOG_SYSCALL("VerifyReadStillAllowed", false, "read still allowed");
    }
    return verified;
}

// A. The assignment's required call: SetFileSecurity. `original` receives the
// saved original security descriptor, so the caller can restore it later.
bool demoSetFileSecurityRoute(vector<char>& original) {
    cout << "\n-- A. SetFileSecurity route (replace the DACL) --\n";

    if (!createTestFile(false)) {
        return false;
    }

    // Save the ORIGINAL descriptor (two-call pattern inside readDacl).
    cout << "GetFileSecurity is called twice: the first call only asks how big the buffer must be\n"
         << "(it is EXPECTED to fail with ERROR_INSUFFICIENT_BUFFER), the second call fills it.\n";
    if (!readDacl("GetFileSecurity", "save original DACL", original)) {
        return false;
    }
    if (!printDacl(original, "Original DACL")) {
        return false;
    }

    // Replace the DACL with the "deny write" list.
    LocalMemory denyDescriptor;
    if (!buildDescriptor(kDenyWriteSddl, denyDescriptor,
                         "ConvertStringSecurityDescriptorToSecurityDescriptor")) {
        return false;
    }
    BOOL setOk = SetFileSecurityA(kTestFile, DACL_SECURITY_INFORMATION, denyDescriptor.get());
    LOG_SYSCALL("SetFileSecurity", setOk != 0, "apply DENY write DACL");
    if (!setOk) {
        return false;
    }

    // Read the new rules back, so the changed list can be shown in the report.
    vector<char> changed;
    if (!readDacl("GetFileSecurity", "read DACL after deny", changed)) {
        return false;
    }
    if (!printDacl(changed, "DACL after SetFileSecurity (deny write)")) {
        return false;
    }

    // Prove the rule works: writing blocked, reading allowed.
    if (!verifyWriteAccess(true, "VerifyWriteBlocked", "the DACL blocks writing")) {
        return false;
    }
    if (!verifyReadStillAllowed()) {
        return false;
    }

    // Restore the original and prove writing works again.
    BOOL restoredOk = SetFileSecurityA(kTestFile, DACL_SECURITY_INFORMATION, original.data());
    LOG_SYSCALL("SetFileSecurity", restoredOk != 0, "restore original DACL");
    if (!restoredOk) {
        return false;
    }
    return verifyWriteAccess(false, "VerifyWriteAllowedAgain", "writing works again after the restore");
}

// B. SetFileAttributes: the closest Windows match to a simple chmod.
bool demoAttributesRoute() {
    cout << "\n-- B. SetFileAttributes route (read-only flag) --\n";

    BOOL readOnlySet = SetFileAttributesA(kTestFile, FILE_ATTRIBUTE_READONLY);
    LOG_SYSCALL("SetFileAttributes", readOnlySet != 0, "read-only on");
    if (!readOnlySet) {
        return false;
    }
    if (!verifyWriteAccess(true, "VerifyReadOnlyBlocksWrite", "the read-only flag blocks writing")) {
        return false;
    }

    BOOL cleared = SetFileAttributesA(kTestFile, FILE_ATTRIBUTE_NORMAL);
    LOG_SYSCALL("SetFileAttributes", cleared != 0, "read-only off");
    if (!cleared) {
        return false;
    }
    return verifyWriteAccess(false, "VerifyWriteAfterClearingReadOnly",
                             "writing works after clearing read-only");
}

// C. Deliberate failure: SetFileSecurity on a file that does not exist.
bool demoMissingFile() {
    cout << "\n-- C. SetFileSecurity on a file that does not exist --\n";

    // Safety: never touch a file that is not our own test file.
    if (GetFileAttributesA(kMissingFile) != INVALID_FILE_ATTRIBUTES) {
        cout << "Skipped: " << kMissingFile << " exists. Rename or delete it and run again.\n";
        return false;
    }

    LocalMemory denyDescriptor;
    if (!buildDescriptor(kDenyWriteSddl, denyDescriptor,
                         "ConvertStringSecurityDescriptorToSecurityDescriptor")) {
        return false;
    }

    cout << "SetFileSecurity on a missing file (this FAILURE is expected, error 2):\n";
    BOOL result = SetFileSecurityA(kMissingFile, DACL_SECURITY_INFORMATION, denyDescriptor.get());
    DWORD errorCode = GetLastError();   // read before anything can change it
    LOG_SYSCALL("SetFileSecurity", result != 0, "permissions_does_not_exist.txt, expected to fail");

    if (result) {
        cout << "UNEXPECTED: SetFileSecurity succeeded on a file that should not exist!\n";
        return false;
    }
    if (errorCode != ERROR_FILE_NOT_FOUND) {
        cout << "UNEXPECTED: it failed, but with error " << errorCode << " instead of 2.\n";
        return false;
    }
    return true;
}

// Returns true if every step worked and every verification passed.
// The cleanup (D) ALWAYS runs, also when an earlier step failed.
bool runDemo() {
    cout << "---------- PART 1: DEMO (logged, not timed) ----------\n";
    cout << "Test file: " << kTestFile << "\n"
         << "If a run is ever killed half-way, repair the file with: icacls "
         << kTestFile << " /reset\n";

    recoverLeftoverFile();

    vector<char> original;   // filled by step A; used by the cleanup
    bool allOk = demoSetFileSecurityRoute(original);
    if (allOk) allOk = demoAttributesRoute();
    if (allOk) allOk = demoMissingFile();

    cout << "\n-- D. Cleanup (restore, clear read-only, delete) --\n";
    bool cleaned = cleanupTestFile(original);
    if (!cleaned) {
        cout << "Warning: the test file could not be removed. Run: icacls "
             << kTestFile << " /reset\n";
    }
    return allOk && cleaned;
}

// ----------------------------- PART 2: benchmarks ----------------------------
//
// NOTES FOR THE REPORT
//  (1) Linux chmod corresponds to SetFileSecurity (DACL) or SetFileAttributes
//      (read-only flag), but Windows is far more detailed: per-user and per-group
//      allow/deny rules, not just owner/group/others. Linux chown corresponds to
//      the OWNER field of the same security descriptor; changing it needs
//      special privileges, so it is not attempted here. Linux umask has no
//      Windows equivalent; the nearest idea is that new files inherit rules
//      from their parent folder.
//  (2) Microsoft marks SetFileSecurity as superseded by SetNamedSecurityInfo.
//      The assignment requires SetFileSecurity, so it is used here.
//  (3) These calls write NTFS metadata, so they are expected to cost tens of
//      microseconds, much more than a plain handle call. They only work on
//      NTFS volumes.
//  (4) Helper calls that are NOT among the assignment's required calls and must
//      not be counted in the 15: GetFileSecurity, SetFileAttributes,
//      ConvertStringSecurityDescriptorToSecurityDescriptor,
//      ConvertSecurityDescriptorToStringSecurityDescriptor, LocalFree, and the
//      pseudo calls named Verify...
//  (5) The CPU columns (cpu_us, cpu_per_call_us, cpu_cycles, cycles_per_call)
//      are measured for the program only, around the whole loop, and include
//      the cleanup call and the timer overhead. Only min/median/mean/max/stddev
//      cover the action alone.
//  (6) Memory delta and page fault columns are not expected to be meaningful
//      for this category.
//  (7) The warm-up runs the lambdas too, so failures there are counted as well.

// Creates a fresh test file and saves its original descriptor again.
// On failure everything is cleaned up and false is returned.
bool prepareBenchmarkFile(vector<char>& original) {
    if (!createTestFile(true)) {
        cleanupTestFile(original);
        return false;
    }
    if (!readDacl("GetFileSecurity_setup", "save original DACL", original)) {
        cleanupTestFile(original);
        return false;
    }
    return true;
}

// 1. Benchmark: SetFileSecurity.
// WHY the cleanup sets the ORIGINAL list back: alternating between two
// different rule lists makes every timed call a real change. Setting identical
// rules over and over could take a shortcut and give a flattering number.
bool benchmarkSetFileSecurity() {
    FailureTracker failures;
    vector<char> original;
    if (!prepareBenchmarkFile(original)) {
        return false;
    }

    LocalMemory denyDescriptor;
    if (!buildDescriptor(kDenyWriteSddl, denyDescriptor,
                         "ConvertStringSecurityDescriptorToSecurityDescriptor_setup")) {
        cleanupTestFile(original);
        return false;
    }
    PSECURITY_DESCRIPTOR denySd = denyDescriptor.get();
    PSECURITY_DESCRIPTOR originalSd = original.data();

    auto action = [&]() {
        if (!SetFileSecurityA(kTestFile, DACL_SECURITY_INFORMATION, denySd)) {
            failures.record(true);
        }
    };

    // Outside the timed region: put the original rules back.
    auto cleanup = [&]() {
        if (!SetFileSecurityA(kTestFile, DACL_SECURITY_INFORMATION, originalSd)) {
            failures.record(true);
        }
    };

    metrics::Stats stats = metrics::benchmark("SetFileSecurity", action, kIterations, kWarmup, cleanup);

    bool restored = restoreFileState(original);   // always leave the file clean
    bool clean = finishBenchmark(stats, failures);
    return clean && restored;
}

// 2. Benchmark: GetFileSecurity into a buffer that is already big enough
// (exact size from the two-call pattern, plus spare room). No cleanup.
bool benchmarkGetFileSecurity() {
    FailureTracker failures;
    vector<char> original;
    if (!prepareBenchmarkFile(original)) {
        return false;
    }

    vector<char> buffer(original.size() + kSpareBufferBytes);
    const DWORD bufferSize = static_cast<DWORD>(buffer.size());

    auto action = [&]() {
        DWORD needed = 0;
        BOOL ok = GetFileSecurityA(kTestFile, DACL_SECURITY_INFORMATION,
                                   buffer.data(), bufferSize, &needed);
        if (!ok) {
            failures.record(true);
        }
    };

    metrics::Stats stats = metrics::benchmark("GetFileSecurity", action, kIterations, kWarmup);

    bool restored = restoreFileState(original);
    bool clean = finishBenchmark(stats, failures);
    return clean && restored;
}

// 3. Benchmark: SetFileAttributes (read-only on). The cleanup (untimed)
// switches it off again, so every timed call is a real change.
bool benchmarkSetFileAttributes() {
    FailureTracker failures;
    vector<char> original;
    if (!prepareBenchmarkFile(original)) {
        return false;
    }

    auto action = [&]() {
        if (!SetFileAttributesA(kTestFile, FILE_ATTRIBUTE_READONLY)) {
            failures.record(true);
        }
    };

    auto cleanup = [&]() {
        if (!SetFileAttributesA(kTestFile, FILE_ATTRIBUTE_NORMAL)) {
            failures.record(true);
        }
    };

    metrics::Stats stats = metrics::benchmark("SetFileAttributes", action, kIterations, kWarmup, cleanup);

    bool restored = restoreFileState(original);
    bool clean = finishBenchmark(stats, failures);
    return clean && restored;
}

// Reads the number of open handles of this process and logs the call.
bool readHandleCount(const char* when, DWORD& count) {
    BOOL ok = GetProcessHandleCount(GetCurrentProcess(), &count);
    LOG_SYSCALL("GetProcessHandleCount", ok != 0, when);
    return ok != 0;
}

// Runs all benchmarks plus the handle leak check. Returns how many problems.
int runAllBenchmarks() {
    cout << "\n---------- PART 2: BENCHMARKS (timed, no logging inside) ----------\n";
    int problems = 0;

    DWORD handlesBefore = 0;
    DWORD handlesAfter = 0;
    bool beforeOk = readHandleCount("before Part 2", handlesBefore);
    if (beforeOk) {
        cout << "Open handles before Part 2: " << handlesBefore << "\n";
    }

    if (!benchmarkSetFileSecurity())   ++problems;
    if (!benchmarkGetFileSecurity())   ++problems;
    if (!benchmarkSetFileAttributes()) ++problems;

    // Each benchmark left the file in its original state; now delete it.
    vector<char> nothingSaved;
    if (!cleanupTestFile(nothingSaved)) {
        cout << "!!! WARNING: the test file could not be deleted. Run: icacls "
             << kTestFile << " /reset\n";
        ++problems;
    }

    bool afterOk = readHandleCount("after Part 2", handlesAfter);
    if (afterOk) {
        cout << "Open handles after Part 2 : " << handlesAfter << "\n";
    }

    // Leak check: a forgotten CloseHandle would grow the count.
    if (beforeOk && afterOk) {
        if (handlesAfter > handlesBefore + kHandleLeakTolerance) {
            cout << "!!! WARNING: handles are LEAKING (" << (handlesAfter - handlesBefore)
                 << " more than before).\n";
            ++problems;
        } else {
            cout << "Handle leak check PASSED.\n";
        }
    } else {
        cout << "!!! WARNING: could not read the handle count, leak check skipped.\n";
        ++problems;
    }
    return problems;
}

// ----------------------------- Program flow ----------------------------------

int runAll() {
    logger::init();   // default path: logs/syscalls_windows.log

    cout << "=== Windows Permissions: demo + benchmarks ===\n\n";

    if (!runDemo()) {
        cout << "\nDemo FAILED, so the benchmarks were skipped.\n";
        return 1;
    }

    int problems = runAllBenchmarks();
    if (problems > 0) {
        cout << "\nFinished with " << problems << " benchmark problem(s). See warnings above.\n";
        return 2;
    }

    cout << "\nAll done. Results appended to results/Windows/windows_results.csv\n";
    return 0;
}

int main() {
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