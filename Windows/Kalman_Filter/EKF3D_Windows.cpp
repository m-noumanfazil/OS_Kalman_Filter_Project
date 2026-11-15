// 3D EKF: native file I/O, mapped arena, persistent processes and events.
#include "../../common/ekf3d_pipeline.hpp"
#include "../../common/logger.hpp"
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <new>

using namespace ekf3d_pipeline;
struct Worker { HANDLE process; HANDLE command; HANDLE done; };
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
    if(argc!=4) { std::cerr << "Usage: EKF3D_Windows <input.csv> <new-output.csv> <workers 1..8>\n"; return 2; }
    int count=0;
    try { size_t n=0; count=std::stoi(argv[3],&n); if(n!=std::strlen(argv[3]) || count<1 || count>8) throw std::invalid_argument("workers"); }
    catch(const std::exception&) { std::cerr << "Invalid worker count\n"; return 2; }
    logger::init(std::string(argv[2])+".log");
    std::string content;
    if(!readFile(argv[1],content)) return 1;
    std::vector<Record> records;
    try { records=parse(content); } catch(const std::exception& ex) { std::cerr << "Invalid input: " << ex.what() << '\n'; return 1; }
    DWORD handlesBefore=0, handlesAfter=0;
    bool countBeforeOk=GetProcessHandleCount(GetCurrentProcess(),&handlesBefore)!=0;
    std::string name="Local\\EKF3D_"+std::to_string(GetCurrentProcessId());
    HANDLE mapping=CreateFileMappingA(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,static_cast<DWORD>(sizeof(Arena)),name.c_str());
    LOG_SYSCALL("CreateFileMappingA",mapping!=nullptr,"3D shared arena");
    if(!mapping) { std::cerr << "CreateFileMapping: " << GetLastError() << '\n'; return 1; }
    if(GetLastError()==ERROR_ALREADY_EXISTS) { CloseHandle(mapping); std::cerr << "Mapping exists\n"; return 1; }
    void* view=MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(Arena));
    LOG_SYSCALL("MapViewOfFile",view!=nullptr,"3D shared arena");
    if(!view) { std::cerr << "MapViewOfFile: " << GetLastError() << '\n'; CloseHandle(mapping); return 1; }
    auto* arena=new(view) Arena{}; arena->count=static_cast<unsigned>(records.size());
    std::copy(records.begin(),records.end(),arena->records);
    HANDLE stop=CreateEventA(nullptr,TRUE,FALSE,(name+"_stop").c_str());
    bool ok=stop && GetLastError()!=ERROR_ALREADY_EXISTS;
    LOG_SYSCALL("CreateEventA",stop!=nullptr,"3D stop event");
    if(!ok && stop) { CloseHandle(stop); stop=nullptr; }
    std::vector<Worker> workers;
    char exe[MAX_PATH]; DWORD length=GetModuleFileNameA(nullptr,exe,MAX_PATH);
    if(!length || length>=MAX_PATH) ok=false;
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
        BOOL started;
#ifdef EKF3D_TEST_FAILURES
        if(i==1 && std::getenv("EKF3D_TEST_FAIL_START")) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); started=FALSE; }
        else
#endif
        started=CreateProcessA(nullptr,buffer.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&si,&pi);
        LOG_SYSCALL("CreateProcessA",started!=0,"3D persistent worker");
        if(!started) {
            std::cerr << "CreateProcess: " << GetLastError() << '\n'; CloseHandle(go); CloseHandle(done); ok=false; break;
        }
        BOOL threadClosed=CloseHandle(pi.hThread); LOG_SYSCALL("CloseHandle",threadClosed!=0,"worker primary thread");
        if(!threadClosed) ok=false;
        std::cout << "WORKER_PID " << pi.dwProcessId << std::endl;
        workers.push_back({pi.hProcess,go,done});
    }
    for(unsigned begin=0;ok && begin<arena->count;begin+=batch_size) {
        arena->begin=begin; arena->end=std::min(begin+batch_size,arena->count);
        for(const auto& w:workers) { BOOL signaled=SetEvent(w.command); LOG_SYSCALL("SetEvent",signaled!=0,"dispatch worker batch"); if(!signaled) { ok=false; break; } }
        for(const auto& w:workers) {
            if(!ok) break;
            HANDLE waits[]={w.process,w.done};
            DWORD result=WaitForMultipleObjects(2,waits,FALSE,workerTimeoutMs);
            if(result!=WAIT_OBJECT_0+1) { if(result!=WAIT_FAILED) SetLastError(result==WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_PROCESS_ABORTED); LOG_SYSCALL("WaitForMultipleObjects",false,"3D worker response"); std::cerr << "Worker failed or timed out\n"; ok=false; }
            else LOG_SYSCALL("WaitForMultipleObjects",true,"3D worker response");
        }
    }
    if(ok) LOG_SYSCALL("SetEvent/WaitForMultipleObjects",true,"3D batch dispatch and completion");
    if(stop) { BOOL signaled=SetEvent(stop); LOG_SYSCALL("SetEvent",signaled!=0,"stop workers"); if(!signaled) ok=false; }
    long long workerCpuUs=0, workerPeakKb=0;
    bool workerCpuOk=true, workerMemoryOk=true;
    for(const auto& w:workers) {
        if(!ok) { BOOL terminated=TerminateProcess(w.process,1); LOG_SYSCALL("TerminateProcess",terminated!=0,"abort 3D worker"); }
        DWORD waited=WaitForSingleObject(w.process,5000);
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
        LOG_SYSCALL("GetProcessMemoryInfo",memoryOk!=0,"3D worker peak memory");
        if(memoryOk)
            workerPeakKb=std::max(workerPeakKb,static_cast<long long>(memory.PeakWorkingSetSize/1024));
        else workerMemoryOk=false;
        BOOL closed=CloseHandle(w.process); LOG_SYSCALL("CloseHandle",closed!=0,"3D worker process"); if(!closed) ok=false;
        closed=CloseHandle(w.command); LOG_SYSCALL("CloseHandle",closed!=0,"3D worker command event"); if(!closed) ok=false;
        closed=CloseHandle(w.done); LOG_SYSCALL("CloseHandle",closed!=0,"3D worker completion event"); if(!closed) ok=false;
    }
    if(stop) { BOOL closed=CloseHandle(stop); LOG_SYSCALL("CloseHandle",closed!=0,"3D stop event"); if(!closed) ok=false; }
    if(ok) { try { ok=writeFile(argv[2],format(arena)); } catch(const std::exception& ex) { std::cerr << ex.what() << '\n'; ok=false; } }
    arena->~Arena();
    BOOL unmapped=UnmapViewOfFile(view); LOG_SYSCALL("UnmapViewOfFile",unmapped!=0,"3D shared arena");
    if(!unmapped) ok=false;
    BOOL closed=CloseHandle(mapping); LOG_SYSCALL("CloseHandle",closed!=0,"3D shared mapping");
    if(!closed) ok=false;
    bool countAfterOk=GetProcessHandleCount(GetCurrentProcess(),&handlesAfter)!=0;
    std::cout << "RESOURCE_COUNTS before=" << (countBeforeOk ? static_cast<long long>(handlesBefore) : -1)
              << " after=" << (countAfterOk ? static_cast<long long>(handlesAfter) : -1) << '\n';
    if(ok) {
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
