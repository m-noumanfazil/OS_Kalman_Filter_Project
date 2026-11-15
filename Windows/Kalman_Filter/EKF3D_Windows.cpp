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
        if(!ReadFile(h,buffer,sizeof(buffer),&n,nullptr)) { LOG_SYSCALL("ReadFile",false,"3D sensor input"); std::cerr << "ReadFile: " << GetLastError() << '\n'; ok=false; break; }
        if(!n) break;
        content.append(buffer,n);
        if(content.size()>64*1024*1024) { std::cerr << "Input exceeds 64 MiB\n"; ok=false; break; }
    }
    if(ok) LOG_SYSCALL("ReadFile",true,"3D sensor input");
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
        if(!WriteFile(h,content.data()+offset,size,&n,nullptr) || !n) { LOG_SYSCALL("WriteFile",false,"3D output"); std::cerr << "WriteFile: " << GetLastError() << '\n'; ok=false; break; }
        offset+=n;
    }
    if(ok) LOG_SYSCALL("WriteFile",true,"3D output");
    BOOL closed=CloseHandle(h); LOG_SYSCALL("CloseHandle",closed!=0,"3D output");
    if(!closed) { std::cerr << "CloseHandle output: " << GetLastError() << '\n'; ok=false; }
    return ok;
}
static int child(const std::string& name, int id, int count) {
    HANDLE mapping=OpenFileMappingA(FILE_MAP_ALL_ACCESS,FALSE,name.c_str());
    if(!mapping) return 3;
    void* view=MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(Arena));
    if(!view) { CloseHandle(mapping); return 4; }
    std::string suffix=std::to_string(id);
    HANDLE stop=OpenEventA(SYNCHRONIZE,FALSE,(name+"_stop").c_str());
    HANDLE command=OpenEventA(SYNCHRONIZE,FALSE,(name+"_go_"+suffix).c_str());
    HANDLE done=OpenEventA(EVENT_MODIFY_STATE,FALSE,(name+"_done_"+suffix).c_str());
    int exitCode=0;
    if(!stop || !command || !done) exitCode=5;
    else {
        HANDLE waits[]={stop,command}; std::unordered_map<int,ekf3d::Filter> filters;
        for(;;) {
            DWORD result=WaitForMultipleObjects(2,waits,FALSE,INFINITE);
            if(result==WAIT_OBJECT_0) break;
            if(result!=WAIT_OBJECT_0+1) { exitCode=6; break; }
#ifdef EKF3D_TEST_FAILURES
            if(id==0 && std::getenv("EKF3D_TEST_HANG_WORKER")) Sleep(INFINITE);
#endif
            try { process(static_cast<Arena*>(view),static_cast<unsigned>(id),static_cast<unsigned>(count),filters); }
            catch(const std::exception& ex) { std::cerr << "Worker " << id << ": " << ex.what() << '\n'; exitCode=7; break; }
            if(!SetEvent(done)) { exitCode=8; break; }
        }
    }
    if(stop && !CloseHandle(stop)) exitCode=9;
    if(command && !CloseHandle(command)) exitCode=9;
    if(done && !CloseHandle(done)) exitCode=9;
    if(!UnmapViewOfFile(view)) exitCode=9;
    if(!CloseHandle(mapping)) exitCode=9;
    return exitCode;
}
int main(int argc, char** argv) {
    if(argc==5 && std::strcmp(argv[1],"--worker")==0) {
        try {
            size_t a=0,b=0; int id=std::stoi(argv[3],&a), count=std::stoi(argv[4],&b);
            if(a!=std::strlen(argv[3]) || b!=std::strlen(argv[4]) || count<1 || count>8 || id<0 || id>=count) return 2;
            return child(argv[2],id,count);
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
    if(!ok && stop) { CloseHandle(stop); stop=nullptr; }
    std::vector<Worker> workers;
    char exe[MAX_PATH]; DWORD length=GetModuleFileNameA(nullptr,exe,MAX_PATH);
    if(!length || length>=MAX_PATH) ok=false;
    for(int i=0;ok && i<count;++i) {
        std::string suffix=std::to_string(i);
        HANDLE go=CreateEventA(nullptr,FALSE,FALSE,(name+"_go_"+suffix).c_str());
        bool goOk=go && GetLastError()!=ERROR_ALREADY_EXISTS;
        HANDLE done=CreateEventA(nullptr,FALSE,FALSE,(name+"_done_"+suffix).c_str());
        bool doneOk=done && GetLastError()!=ERROR_ALREADY_EXISTS;
        if(!goOk || !doneOk) { if(go) CloseHandle(go); if(done) CloseHandle(done); ok=false; break; }
        std::string command="\""+std::string(exe)+"\" --worker "+name+" "+suffix+" "+std::to_string(count);
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
        CloseHandle(pi.hThread);
        std::cout << "WORKER_PID " << pi.dwProcessId << std::endl;
        workers.push_back({pi.hProcess,go,done});
    }
    for(unsigned begin=0;ok && begin<arena->count;begin+=batch_size) {
        arena->begin=begin; arena->end=std::min(begin+batch_size,arena->count);
        for(const auto& w:workers) if(!SetEvent(w.command)) { ok=false; break; }
        for(const auto& w:workers) {
            if(!ok) break;
            HANDLE waits[]={w.process,w.done};
            DWORD result=WaitForMultipleObjects(2,waits,FALSE,workerTimeoutMs);
            if(result!=WAIT_OBJECT_0+1) { if(result!=WAIT_FAILED) SetLastError(result==WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_PROCESS_ABORTED); LOG_SYSCALL("WaitForMultipleObjects",false,"3D worker response"); std::cerr << "Worker failed or timed out\n"; ok=false; }
        }
    }
    if(ok) LOG_SYSCALL("SetEvent/WaitForMultipleObjects",true,"3D batch dispatch and completion");
    if(stop && !SetEvent(stop)) ok=false;
    long long workerCpuUs=0, workerPeakKb=0;
    bool workerCpuOk=true, workerMemoryOk=true;
    for(const auto& w:workers) {
        if(!ok) TerminateProcess(w.process,1);
        if(WaitForSingleObject(w.process,5000)!=WAIT_OBJECT_0) { TerminateProcess(w.process,1); WaitForSingleObject(w.process,INFINITE); ok=false; }
        DWORD code=0; if(!GetExitCodeProcess(w.process,&code) || code!=0) ok=false;
        FILETIME creation{}, exitTime{}, kernel{}, user{};
        if(GetProcessTimes(w.process,&creation,&exitTime,&kernel,&user)) {
            ULARGE_INTEGER k{}, u{}; k.LowPart=kernel.dwLowDateTime; k.HighPart=kernel.dwHighDateTime;
            u.LowPart=user.dwLowDateTime; u.HighPart=user.dwHighDateTime;
            workerCpuUs+=static_cast<long long>((k.QuadPart+u.QuadPart)/10);
        } else workerCpuOk=false;
        PROCESS_MEMORY_COUNTERS memory{}; memory.cb=sizeof(memory);
        if(GetProcessMemoryInfo(w.process,&memory,sizeof(memory)))
            workerPeakKb=std::max(workerPeakKb,static_cast<long long>(memory.PeakWorkingSetSize/1024));
        else workerMemoryOk=false;
        if(!CloseHandle(w.process)) ok=false;
        if(!CloseHandle(w.command)) ok=false;
        if(!CloseHandle(w.done)) ok=false;
    }
    if(stop && !CloseHandle(stop)) ok=false;
    if(ok) { try { ok=writeFile(argv[2],format(arena)); } catch(const std::exception& ex) { std::cerr << ex.what() << '\n'; ok=false; } }
    arena->~Arena();
    BOOL unmapped=UnmapViewOfFile(view); LOG_SYSCALL("UnmapViewOfFile",unmapped!=0,"3D shared arena");
    if(!unmapped) ok=false;
    BOOL closed=CloseHandle(mapping); LOG_SYSCALL("CloseHandle",closed!=0,"3D shared mapping");
    if(!closed) ok=false;
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
