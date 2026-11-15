// 3D EKF: native file I/O, mapped arena, persistent processes and events.
#include "../../common/ekf3d_pipeline.hpp"
#include "../../common/logger.hpp"
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <new>

using namespace ekf3d_pipeline;
struct Worker { HANDLE process; HANDLE command; HANDLE done; };
#ifdef EKF3D_HANDLE_DIAGNOSTICS
struct DebugHandleEntry {
    HANDLE value;
    ULONG_PTR handleCount, pointerCount;
    ULONG grantedAccess, objectTypeIndex, attributes, reserved;
};
struct DebugHandleSnapshot {
    ULONG_PTR count, reserved;
    DebugHandleEntry entries[1];
};
struct DebugUnicodeString { USHORT length, maximumLength; PWSTR buffer; };
using DebugQueryProcess = LONG (WINAPI*)(HANDLE,ULONG,PVOID,ULONG,PULONG);
using DebugQueryObject = LONG (WINAPI*)(HANDLE,ULONG,PVOID,ULONG,PULONG);
static std::vector<HANDLE> debugSnapshot() {
    auto query=reinterpret_cast<DebugQueryProcess>(GetProcAddress(GetModuleHandleA("ntdll.dll"),"NtQueryInformationProcess"));
    if(!query) return {};
    ULONG size=65536, required=0;
    for(int attempt=0;attempt<4;++attempt) {
        std::vector<unsigned char> bytes(size);
        LONG status=query(GetCurrentProcess(),51,bytes.data(),size,&required);
        if(status>=0) {
            auto* snapshot=reinterpret_cast<DebugHandleSnapshot*>(bytes.data());
            std::vector<HANDLE> handles;
            for(ULONG_PTR i=0;i<snapshot->count;++i) handles.push_back(snapshot->entries[i].value);
            return handles;
        }
        size=std::max(size*2,required+1024);
    }
    return {};
}
static void debugAddedHandles(const std::vector<HANDLE>& before) {
    auto query=reinterpret_cast<DebugQueryObject>(GetProcAddress(GetModuleHandleA("ntdll.dll"),"NtQueryObject"));
    if(!query || before.empty()) return;
    for(HANDLE handle:debugSnapshot()) {
        if(std::find(before.begin(),before.end(),handle)!=before.end()) continue;
        std::vector<unsigned char> bytes(8192); ULONG required=0;
        LONG status=query(handle,2,bytes.data(),static_cast<ULONG>(bytes.size()),&required);
        std::wstring type=L"unavailable";
        if(status>=0) {
            auto* value=reinterpret_cast<DebugUnicodeString*>(bytes.data());
            if(value->buffer) type.assign(value->buffer,value->length/sizeof(wchar_t));
        }
        std::wcerr << L"HANDLE_ADDED " << handle << L" " << type << L'\n';
    }
}
static void debugHandles(const char* stage) {
    DWORD count=0;
    if(GetProcessHandleCount(GetCurrentProcess(),&count)) std::cerr << "HANDLE_STAGE " << stage << ' ' << count << '\n';
}
#else
static void debugHandles(const char*) {}
#endif
#ifdef EKF3D_TEST_FAILURES
constexpr DWORD workerTimeoutMs=500;
#else
constexpr DWORD workerTimeoutMs=15000;
#endif

static bool readFile(const char* path, std::string& content) {
    HANDLE h=CreateFileA(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    LOG_SYSCALL("CreateFileA",h!=INVALID_HANDLE_VALUE,"3D sensor input");
    if(h==INVALID_HANDLE_VALUE) { std::cerr << "CreateFile input: " << GetLastError() << '\n'; return false; }
    char buffer[16384]; bool ok=true;
    for(;;) {
        DWORD n=0;
        BOOL readOk=ReadFile(h,buffer,sizeof(buffer),&n,nullptr);
        LOG_SYSCALL("ReadFile",readOk!=0,"3D sensor input");
        if(!readOk) { std::cerr << "ReadFile: " << GetLastError() << '\n'; ok=false; break; }
        if(!n) break;
        content.append(buffer,n);
        if(content.size()>64*1024*1024) { std::cerr << "Input exceeds 64 MiB\n"; ok=false; break; }
    }
    BOOL closed=CloseHandle(h); LOG_SYSCALL("CloseHandle",closed!=0,"3D sensor input");
    if(!closed) { std::cerr << "CloseHandle input: " << GetLastError() << '\n'; ok=false; }
    return ok;
}
static bool writeFile(const char* path, const std::string& content) {
    HANDLE h=CreateFileA(path,GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    LOG_SYSCALL("CreateFileA",h!=INVALID_HANDLE_VALUE,"3D output, exclusive creation");
    if(h==INVALID_HANDLE_VALUE) { std::cerr << "CreateFile output: " << GetLastError() << '\n'; return false; }
    bool ok=true; size_t offset=0;
    while(offset<content.size()) {
        DWORD n=0, size=static_cast<DWORD>(std::min<size_t>(content.size()-offset,65536));
        BOOL writeOk=WriteFile(h,content.data()+offset,size,&n,nullptr);
        if(writeOk && !n) SetLastError(ERROR_WRITE_FAULT);
        LOG_SYSCALL("WriteFile",writeOk && n,"3D output");
        if(!writeOk || !n) { std::cerr << "WriteFile: " << GetLastError() << '\n'; ok=false; break; }
        offset+=n;
    }
    BOOL closed=CloseHandle(h); LOG_SYSCALL("CloseHandle",closed!=0,"3D output");
    if(!closed) { std::cerr << "CloseHandle output: " << GetLastError() << '\n'; ok=false; }
    return ok;
}
static int child(const std::string& name, int id, int count, const std::string& logPath) {
    logger::init(logPath);
    logger::setConsoleEnabled(false);
    HANDLE mapping=OpenFileMappingA(FILE_MAP_ALL_ACCESS,FALSE,name.c_str());
    LOG_SYSCALL("OpenFileMappingA",mapping!=nullptr,"3D worker shared arena");
    if(!mapping) return 3;
    void* view=MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(Arena));
    LOG_SYSCALL("MapViewOfFile",view!=nullptr,"3D worker shared arena");
    if(!view) { BOOL closed=CloseHandle(mapping); LOG_SYSCALL("CloseHandle",closed!=0,"worker mapping after map failure"); return 4; }
    std::string suffix=std::to_string(id);
    HANDLE stop=OpenEventA(SYNCHRONIZE,FALSE,(name+"_stop").c_str());
    LOG_SYSCALL("OpenEventA",stop!=nullptr,"worker stop event");
    HANDLE command=OpenEventA(SYNCHRONIZE,FALSE,(name+"_go_"+suffix).c_str());
    LOG_SYSCALL("OpenEventA",command!=nullptr,"worker command event");
    HANDLE done=OpenEventA(EVENT_MODIFY_STATE,FALSE,(name+"_done_"+suffix).c_str());
    LOG_SYSCALL("OpenEventA",done!=nullptr,"worker completion event");
    int exitCode=0;
    if(!stop || !command || !done) exitCode=5;
    else {
        HANDLE waits[]={stop,command}; std::unordered_map<int,ekf3d::Filter> filters;
        for(;;) {
            DWORD result=WaitForMultipleObjects(2,waits,FALSE,INFINITE);
            LOG_SYSCALL("WaitForMultipleObjects",result==WAIT_OBJECT_0 || result==WAIT_OBJECT_0+1,"worker command or shutdown");
            if(result==WAIT_OBJECT_0) break;
            if(result!=WAIT_OBJECT_0+1) { exitCode=6; break; }
#ifdef EKF3D_TEST_FAILURES
            if(id==0 && std::getenv("EKF3D_TEST_HANG_WORKER")) Sleep(INFINITE);
#endif
            try { process(static_cast<Arena*>(view),static_cast<unsigned>(id),static_cast<unsigned>(count),filters); }
            catch(const std::exception& ex) { std::cerr << "Worker " << id << ": " << ex.what() << '\n'; exitCode=7; break; }
            BOOL signaled=SetEvent(done); LOG_SYSCALL("SetEvent",signaled!=0,"worker completion");
            if(!signaled) { exitCode=8; break; }
        }
    }
    if(stop) { BOOL ok=CloseHandle(stop); LOG_SYSCALL("CloseHandle",ok!=0,"worker stop event"); if(!ok) exitCode=9; }
    if(command) { BOOL ok=CloseHandle(command); LOG_SYSCALL("CloseHandle",ok!=0,"worker command event"); if(!ok) exitCode=9; }
    if(done) { BOOL ok=CloseHandle(done); LOG_SYSCALL("CloseHandle",ok!=0,"worker completion event"); if(!ok) exitCode=9; }
    BOOL unmapped=UnmapViewOfFile(view); LOG_SYSCALL("UnmapViewOfFile",unmapped!=0,"worker shared arena"); if(!unmapped) exitCode=9;
    BOOL closed=CloseHandle(mapping); LOG_SYSCALL("CloseHandle",closed!=0,"worker shared mapping"); if(!closed) exitCode=9;
    return exitCode;
}
int main(int argc, char** argv) {
    if(argc==6 && std::strcmp(argv[1],"--worker")==0) {
        try {
            size_t a=0,b=0; int id=std::stoi(argv[3],&a), count=std::stoi(argv[4],&b);
            if(a!=std::strlen(argv[3]) || b!=std::strlen(argv[4]) || count<1 || count>8 || id<0 || id>=count) return 2;
            return child(argv[2],id,count,argv[5]);
        } catch(const std::exception&) { return 2; }
    }
    if(argc!=4 && argc!=5) { std::cerr << "Usage: EKF3D_Windows <input.csv> <new-output.csv> <workers 1..8> [batch size 1..100000]\n"; return 2; }
    int count=0; unsigned batchSize=batch_size;
    try {
        size_t n=0; count=std::stoi(argv[3],&n);
        if(n!=std::strlen(argv[3]) || count<1 || count>8) throw std::invalid_argument("workers");
        if(argc==5) { n=0; int value=std::stoi(argv[4],&n); if(n!=std::strlen(argv[4]) || value<1 || value>static_cast<int>(capacity)) throw std::invalid_argument("batch"); batchSize=static_cast<unsigned>(value); }
    }
    catch(const std::exception&) { std::cerr << "Invalid worker count or batch size\n"; return 2; }
    logger::init(std::string(argv[2])+".log");
    std::string content;
    if(!readFile(argv[1],content)) return 1;
    std::vector<Record> records;
    try { records=parse(content); } catch(const std::exception& ex) { std::cerr << "Invalid input: " << ex.what() << '\n'; return 1; }
    DWORD handlesBefore=0, handlesAfter=0;
    bool countBeforeOk=GetProcessHandleCount(GetCurrentProcess(),&handlesBefore)!=0;
    debugHandles("baseline");
    std::string name="Local\\EKF3D_"+std::to_string(GetCurrentProcessId());
    HANDLE mapping=CreateFileMappingA(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,static_cast<DWORD>(sizeof(Arena)),name.c_str());
    LOG_SYSCALL("CreateFileMappingA",mapping!=nullptr,"3D shared arena");
    if(!mapping) { std::cerr << "CreateFileMapping: " << GetLastError() << '\n'; return 1; }
    if(GetLastError()==ERROR_ALREADY_EXISTS) { CloseHandle(mapping); std::cerr << "Mapping exists\n"; return 1; }
    void* view=MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(Arena));
    LOG_SYSCALL("MapViewOfFile",view!=nullptr,"3D shared arena");
    if(!view) { std::cerr << "MapViewOfFile: " << GetLastError() << '\n'; CloseHandle(mapping); return 1; }
    debugHandles("mapped");
    auto* arena=new(view) Arena{}; arena->count=static_cast<unsigned>(records.size());
    std::copy(records.begin(),records.end(),arena->records);
    HANDLE stop=CreateEventA(nullptr,TRUE,FALSE,(name+"_stop").c_str());
    bool ok=stop && GetLastError()!=ERROR_ALREADY_EXISTS;
    LOG_SYSCALL("CreateEventA",stop!=nullptr,"3D stop event");
    if(!ok && stop) { CloseHandle(stop); stop=nullptr; }
    std::vector<Worker> workers;
    char exe[MAX_PATH]; DWORD length=GetModuleFileNameA(nullptr,exe,MAX_PATH);
    if(!length || length>=MAX_PATH) ok=false;
    auto launchStart=std::chrono::steady_clock::now();
    for(int i=0;ok && i<count;++i) {
        std::string suffix=std::to_string(i);
        HANDLE go=CreateEventA(nullptr,FALSE,FALSE,(name+"_go_"+suffix).c_str());
        bool goOk=go && GetLastError()!=ERROR_ALREADY_EXISTS;
        LOG_SYSCALL("CreateEventA",go!=nullptr,"3D command event");
        HANDLE done=CreateEventA(nullptr,FALSE,FALSE,(name+"_done_"+suffix).c_str());
        bool doneOk=done && GetLastError()!=ERROR_ALREADY_EXISTS;
        LOG_SYSCALL("CreateEventA",done!=nullptr,"3D completion event");
        if(!goOk || !doneOk) { if(go) CloseHandle(go); if(done) CloseHandle(done); ok=false; break; }
        std::string workerLog=std::string(argv[2])+".worker-"+suffix+".log";
        std::string command="\""+std::string(exe)+"\" --worker "+name+" "+suffix+" "+std::to_string(count)+" \""+workerLog+"\"";
        std::vector<char> buffer(command.begin(),command.end()); buffer.push_back('\0');
        STARTUPINFOA si{}; si.cb=sizeof(si); PROCESS_INFORMATION pi{};
        debugHandles("before_create_process");
#ifdef EKF3D_HANDLE_DIAGNOSTICS
        auto handlesBeforeCreate=debugSnapshot();
#endif
        BOOL started;
#ifdef EKF3D_TEST_FAILURES
        if(i==1 && std::getenv("EKF3D_TEST_FAIL_START")) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); started=FALSE; }
        else
#endif
        started=CreateProcessA(nullptr,buffer.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&si,&pi);
        LOG_SYSCALL("CreateProcessA",started!=0,"3D persistent worker");
        debugHandles("after_create_process");
#ifdef EKF3D_HANDLE_DIAGNOSTICS
        if(i==0) debugAddedHandles(handlesBeforeCreate);
#endif
        if(!started) {
            std::cerr << "CreateProcess: " << GetLastError() << '\n'; CloseHandle(go); CloseHandle(done); ok=false; break;
        }
        BOOL threadClosed=CloseHandle(pi.hThread); LOG_SYSCALL("CloseHandle",threadClosed!=0,"worker primary thread");
        debugHandles("thread_closed");
        if(!threadClosed) ok=false;
        std::cout << "WORKER_PID " << pi.dwProcessId << std::endl;
        workers.push_back({pi.hProcess,go,done});
    }
    debugHandles("workers_started");
    auto launchEnd=std::chrono::steady_clock::now();
    double firstBatchUs=0, remainingBatchesUs=0; unsigned batches=0;
    for(unsigned begin=0;ok && begin<arena->count;begin+=batchSize) {
        auto batchStart=std::chrono::steady_clock::now();
        arena->begin=begin; arena->end=std::min(begin+batchSize,arena->count);
        for(const auto& w:workers) { BOOL signaled=SetEvent(w.command); LOG_SYSCALL("SetEvent",signaled!=0,"dispatch worker batch"); if(!signaled) { ok=false; break; } }
        for(const auto& w:workers) {
            if(!ok) break;
            HANDLE waits[]={w.process,w.done};
            DWORD result=WaitForMultipleObjects(2,waits,FALSE,workerTimeoutMs);
            if(result!=WAIT_OBJECT_0+1) { if(result!=WAIT_FAILED) SetLastError(result==WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_PROCESS_ABORTED); LOG_SYSCALL("WaitForMultipleObjects",false,"3D worker response"); std::cerr << "Worker failed or timed out\n"; ok=false; }
            else LOG_SYSCALL("WaitForMultipleObjects",true,"3D worker response");
        }
        double elapsed=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-batchStart).count();
        if(batches++==0) firstBatchUs=elapsed; else remainingBatchesUs+=elapsed;
    }
    if(ok) LOG_SYSCALL("SetEvent/WaitForMultipleObjects",true,"3D batch dispatch and completion");
    debugHandles("processing_done");
    auto shutdownStart=std::chrono::steady_clock::now();
    if(stop) { BOOL signaled=SetEvent(stop); LOG_SYSCALL("SetEvent",signaled!=0,"stop workers"); if(!signaled) ok=false; }
    long long workerCpuUs=0, workerPeakKb=0;
    bool workerCpuOk=true, workerMemoryOk=true;
    for(const auto& w:workers) {
        if(!ok) { BOOL terminated=TerminateProcess(w.process,1); LOG_SYSCALL("TerminateProcess",terminated!=0,"abort 3D worker"); }
        DWORD waited=WaitForSingleObject(w.process,5000);
        debugHandles("worker_waited");
        if(waited!=WAIT_OBJECT_0 && waited!=WAIT_FAILED) SetLastError(waited==WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_INVALID_DATA);
        LOG_SYSCALL("WaitForSingleObject",waited==WAIT_OBJECT_0,"3D worker shutdown");
        if(waited!=WAIT_OBJECT_0) {
            BOOL terminated=TerminateProcess(w.process,1); LOG_SYSCALL("TerminateProcess",terminated!=0,"timed-out 3D worker");
            DWORD reaped=WaitForSingleObject(w.process,INFINITE); LOG_SYSCALL("WaitForSingleObject",reaped==WAIT_OBJECT_0,"terminated 3D worker"); ok=false;
        }
        DWORD code=0; BOOL exitOk=GetExitCodeProcess(w.process,&code); LOG_SYSCALL("GetExitCodeProcess",exitOk!=0,"3D worker exit code");
        if(!exitOk || code!=0) ok=false;
        FILETIME creation{}, exitTime{}, kernel{}, user{};
        BOOL timesOk=GetProcessTimes(w.process,&creation,&exitTime,&kernel,&user);
        LOG_SYSCALL("GetProcessTimes",timesOk!=0,"3D worker CPU");
        if(timesOk) {
            ULARGE_INTEGER k{}, u{}; k.LowPart=kernel.dwLowDateTime; k.HighPart=kernel.dwHighDateTime;
            u.LowPart=user.dwLowDateTime; u.HighPart=user.dwHighDateTime;
            workerCpuUs+=static_cast<long long>((k.QuadPart+u.QuadPart)/10);
        } else workerCpuOk=false;
        PROCESS_MEMORY_COUNTERS memory{}; memory.cb=sizeof(memory);
        BOOL memoryOk=GetProcessMemoryInfo(w.process,&memory,sizeof(memory));
        debugHandles("worker_memory_read");
        LOG_SYSCALL("GetProcessMemoryInfo",memoryOk!=0,"3D worker peak memory");
        if(memoryOk)
            workerPeakKb=std::max(workerPeakKb,static_cast<long long>(memory.PeakWorkingSetSize/1024));
        else workerMemoryOk=false;
        BOOL closed=CloseHandle(w.process); LOG_SYSCALL("CloseHandle",closed!=0,"3D worker process"); if(!closed) ok=false;
        closed=CloseHandle(w.command); LOG_SYSCALL("CloseHandle",closed!=0,"3D worker command event"); if(!closed) ok=false;
        closed=CloseHandle(w.done); LOG_SYSCALL("CloseHandle",closed!=0,"3D worker completion event"); if(!closed) ok=false;
        debugHandles("worker_handles_closed");
    }
    if(stop) { BOOL closed=CloseHandle(stop); LOG_SYSCALL("CloseHandle",closed!=0,"3D stop event"); if(!closed) ok=false; }
    auto shutdownEnd=std::chrono::steady_clock::now();
    debugHandles("workers_closed");
    if(ok) { try { ok=writeFile(argv[2],format(arena)); } catch(const std::exception& ex) { std::cerr << ex.what() << '\n'; ok=false; } }
    debugHandles("output_closed");
    arena->~Arena();
    BOOL unmapped=UnmapViewOfFile(view); LOG_SYSCALL("UnmapViewOfFile",unmapped!=0,"3D shared arena");
    if(!unmapped) ok=false;
    BOOL closed=CloseHandle(mapping); LOG_SYSCALL("CloseHandle",closed!=0,"3D shared mapping");
    if(!closed) ok=false;
    debugHandles("mapping_closed");
    bool countAfterOk=GetProcessHandleCount(GetCurrentProcess(),&handlesAfter)!=0;
    std::cout << "RESOURCE_COUNTS before=" << (countBeforeOk ? static_cast<long long>(handlesBefore) : -1)
              << " after=" << (countAfterOk ? static_cast<long long>(handlesAfter) : -1) << '\n';
    if(ok) {
        std::cout << "TIMING batch_size=" << batchSize
                  << " launch_us=" << std::chrono::duration<double,std::micro>(launchEnd-launchStart).count()
                  << " first_batch_us=" << firstBatchUs << " remaining_batches_us=" << remainingBatchesUs
                  << " remaining_batches=" << (batches ? batches-1 : 0)
                  << " shutdown_us=" << std::chrono::duration<double,std::micro>(shutdownEnd-shutdownStart).count() << '\n';
        FILETIME creation{}, exitTime{}, kernel{}, user{};
        bool cpuOk=GetProcessTimes(GetCurrentProcess(),&creation,&exitTime,&kernel,&user)!=0;
        ULARGE_INTEGER k{}, u{}; k.LowPart=kernel.dwLowDateTime; k.HighPart=kernel.dwHighDateTime;
        u.LowPart=user.dwLowDateTime; u.HighPart=user.dwHighDateTime;
        PROCESS_MEMORY_COUNTERS memory{}; memory.cb=sizeof(memory);
        bool memoryOk=GetProcessMemoryInfo(GetCurrentProcess(),&memory,sizeof(memory))!=0;
        std::cout << "RESOURCE parent_cpu_us=" << (cpuOk ? static_cast<long long>((k.QuadPart+u.QuadPart)/10) : -1)
                  << " worker_cpu_us=" << (workerCpuOk ? workerCpuUs : -1)
                  << " parent_peak_kb=" << (memoryOk ? static_cast<long long>(memory.PeakWorkingSetSize/1024) : -1)
                  << " worker_peak_kb=" << (workerMemoryOk ? workerPeakKb : -1) << '\n';
        std::cout << "Processed " << records.size() << " observations using " << count << " persistent workers.\n";
    }
    return ok ? 0 : 1;
}
