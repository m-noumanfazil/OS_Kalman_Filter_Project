// 3D EKF: POSIX file I/O, mmap shared arena, persistent forked workers and pipes.
#include "../../common/ekf3d_pipeline.hpp"
#include "../../common/logger_linux.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <new>
#include <poll.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace ekf3d_pipeline;
struct Worker { pid_t pid; int command; int done; };
#ifdef EKF3D_TEST_FAILURES
constexpr int workerTimeoutMs=500;
#else
constexpr int workerTimeoutMs=15000;
#endif

// The directory descriptor appears in both counts and therefore cancels out.
static int descriptorCount() {
    DIR* directory=opendir("/proc/self/fd");
    if(!directory) return -1;
    int count=0;
    while(dirent* entry=readdir(directory))
        if(std::strcmp(entry->d_name,".") && std::strcmp(entry->d_name,"..")) ++count;
    if(closedir(directory)!=0) return -1;
    return count;
}

static bool readFile(const char* path, std::string& content) {
    int fd=open(path,O_RDONLY);
    LOG_SYSCALL("open",fd>=0,"3D sensor input");
    if(fd<0) { perror("open input"); return false; }
    char buffer[16384]; bool ok=true;
    for (;;) {
        ssize_t n=read(fd,buffer,sizeof(buffer));
        LOG_SYSCALL("read",n>=0,"3D sensor input");
        if(n<0 && errno==EINTR) continue;
        if(n<0) { perror("read input"); ok=false; break; }
        if(n==0) break;
        content.append(buffer,static_cast<size_t>(n));
        if(content.size()>64*1024*1024) { std::cerr << "Input exceeds 64 MiB\n"; ok=false; break; }
    }
    int closed=close(fd); LOG_SYSCALL("close",closed==0,"3D sensor input");
    if(closed!=0) { perror("close input"); ok=false; }
    return ok;
}

static bool writeFile(const char* path, const std::string& content) {
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0644);
    LOG_SYSCALL("open",fd>=0,"3D output, exclusive creation");
    if(fd<0) { perror("open output"); return false; }
    bool ok=true; size_t offset=0;
    while(offset<content.size()) {
        ssize_t n=write(fd,content.data()+offset,content.size()-offset);
        if(n==0) errno=EIO;
        LOG_SYSCALL("write",n>0,"3D output");
        if(n<0 && errno==EINTR) continue;
        if(n<=0) { perror("write output"); ok=false; break; }
        offset+=static_cast<size_t>(n);
    }
    int closed=close(fd); LOG_SYSCALL("close",closed==0,"3D output");
    if(closed!=0) { perror("close output"); ok=false; }
    return ok;
}

static bool shutdownWorkers(std::vector<Worker>& workers, bool abort) {
    bool ok=true;
    for(auto& w:workers) {
        int commandClose=close(w.command); LOG_SYSCALL("close",commandClose==0,"worker command pipe");
        if(commandClose!=0) ok=false;
        int doneClose=close(w.done); LOG_SYSCALL("close",doneClose==0,"worker completion pipe");
        if(doneClose!=0) ok=false;
        if(abort) {
            int killed=kill(w.pid,SIGKILL); int error=errno;
            LOG_SYSCALL("kill",killed==0,"abort worker");
            if(killed!=0 && error!=ESRCH) ok=false;
        }
    }
    for(auto& w:workers) {
        int status=0; pid_t result;
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        do {
            result=waitpid(w.pid,&status,WNOHANG);
            if(result==0 && std::chrono::steady_clock::now()>=deadline) {
                kill(w.pid,SIGKILL); ok=false;
                do { result=waitpid(w.pid,&status,0); } while(result<0 && errno==EINTR);
                break;
            }
            if(result==0) usleep(1000);
        } while(result==0 || (result<0 && errno==EINTR));
        if(result!=w.pid || !WIFEXITED(status) || WEXITSTATUS(status)!=0) ok=false;
        bool childOk=result==w.pid && WIFEXITED(status) && WEXITSTATUS(status)==0;
        if(result==w.pid && !childOk) errno=EIO;
        LOG_SYSCALL("waitpid",childOk,"3D worker shutdown");
    }
    workers.clear(); return ok;
}

int main(int argc, char** argv) {
    if(argc!=4 && argc!=5) { std::cerr << "Usage: EKF3D_Linux <input.csv> <new-output.csv> <workers 1..8> [batch size 1..100000]\n"; return 2; }
    int count=0; unsigned batchSize=batch_size;
    try {
        std::size_t n=0; count=std::stoi(argv[3],&n);
        if(n!=std::strlen(argv[3]) || count<1 || count>8) throw std::invalid_argument("workers");
        if(argc==5) { n=0; int value=std::stoi(argv[4],&n); if(n!=std::strlen(argv[4]) || value<1 || value>static_cast<int>(capacity)) throw std::invalid_argument("batch"); batchSize=static_cast<unsigned>(value); }
    }
    catch(const std::exception&) { std::cerr << "Invalid worker count or batch size\n"; return 2; }
    logger::init(std::string(argv[2])+".log");
    std::string contents;
    if(!readFile(argv[1],contents)) return 1;
    std::vector<Record> records;
    try { records=parse(contents); } catch(const std::exception& ex) { std::cerr << "Invalid input: " << ex.what() << '\n'; return 1; }
    int descriptorsBefore=descriptorCount();
    void* mapping=mmap(nullptr,sizeof(Arena),PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    LOG_SYSCALL("mmap",mapping!=MAP_FAILED,"3D shared arena");
    if(mapping==MAP_FAILED) { perror("mmap"); return 1; }
    auto* arena=new(mapping) Arena{};
    arena->count=static_cast<unsigned>(records.size());
    std::copy(records.begin(),records.end(),arena->records);
    std::vector<Worker> workers; bool ok=true;
    std::signal(SIGPIPE,SIG_IGN);
    auto launchStart=std::chrono::steady_clock::now();
    for(int i=0;ok && i<count;++i) {
        int commands[2], replies[2];
        int commandResult=pipe(commands); LOG_SYSCALL("pipe",commandResult==0,"3D worker commands");
        if(commandResult!=0) { perror("pipe commands"); ok=false; break; }
        int replyResult=pipe(replies); LOG_SYSCALL("pipe",replyResult==0,"3D worker replies");
        if(replyResult!=0) { perror("pipe replies"); close(commands[0]); close(commands[1]); ok=false; break; }
        pid_t pid;
#ifdef EKF3D_TEST_FAILURES
        if(i==1 && std::getenv("EKF3D_TEST_FAIL_START")) { errno=EAGAIN; pid=-1; }
        else
#endif
        pid=fork();
        if(pid==0) {
            logger::init(std::string(argv[2])+".worker-"+std::to_string(i)+".log");
            logger::setConsoleEnabled(false);
            int closed=close(commands[1]); LOG_SYSCALL("close",closed==0,"unused command write end");
            closed=close(replies[0]); LOG_SYSCALL("close",closed==0,"unused completion read end");
            for(const auto& w:workers) {
                closed=close(w.command); LOG_SYSCALL("close",closed==0,"inherited worker command");
                closed=close(w.done); LOG_SYSCALL("close",closed==0,"inherited worker completion");
            }
            std::unordered_map<int,ekf3d::Filter> filters; char command;
            for(;;) {
                ssize_t received=read(commands[0],&command,1);
                LOG_SYSCALL("read",received>=0,"worker command");
                if(received<0 && errno==EINTR) continue;
                if(received==0) break;
                if(received!=1 || command!='R') _exit(5);
#ifdef EKF3D_TEST_FAILURES
                if(i==0 && std::getenv("EKF3D_TEST_HANG_WORKER")) for(;;) pause();
#endif
                try { process(arena,static_cast<unsigned>(i),static_cast<unsigned>(count),filters); }
                catch(const std::exception& ex) { std::cerr << "Worker " << i << ": " << ex.what() << '\n'; _exit(3); }
                char done='D'; ssize_t sent=write(replies[1],&done,1);
                LOG_SYSCALL("write",sent==1,"worker completion");
                if(sent!=1) _exit(4);
            }
            int commandClose=close(commands[0]); LOG_SYSCALL("close",commandClose==0,"worker command pipe");
            int replyClose=close(replies[1]); LOG_SYSCALL("close",replyClose==0,"worker completion pipe");
            _exit(commandClose==0 && replyClose==0 ? 0 : 6);
        }
        int forkError=errno;
        LOG_SYSCALL("fork",pid>=0,"3D persistent worker");
        int commandClose=close(commands[0]); LOG_SYSCALL("close",commandClose==0,"parent unused command read end");
        int replyClose=close(replies[1]); LOG_SYSCALL("close",replyClose==0,"parent unused completion write end");
        if(commandClose!=0 || replyClose!=0) ok=false;
        if(pid<0) { errno=forkError; perror("fork"); close(commands[1]); close(replies[0]); ok=false; break; }
        std::cout << "WORKER_PID " << pid << std::endl;
        workers.push_back({pid,commands[1],replies[0]});
    }
    auto launchEnd=std::chrono::steady_clock::now();
    double firstBatchUs=0, remainingBatchesUs=0; unsigned batches=0;
    for(unsigned begin=0;ok && begin<arena->count;begin+=batchSize) {
        auto batchStart=std::chrono::steady_clock::now();
        arena->begin=begin; arena->end=std::min(begin+batchSize,arena->count);
        for(const auto& w:workers) { char command='R'; ssize_t sent=write(w.command,&command,1); LOG_SYSCALL("write",sent==1,"dispatch worker batch"); if(sent!=1) { perror("send work"); ok=false; break; } }
        for(const auto& w:workers) {
            if(!ok) break;
            pollfd wait{w.done,POLLIN,0}; int ready;
            do { ready=poll(&wait,1,workerTimeoutMs); LOG_SYSCALL("poll",ready>=0,"wait worker completion"); } while(ready<0 && errno==EINTR);
            char reply=0;
            ssize_t received=-1;
            if(ready>0 && (wait.revents&POLLIN)) { received=read(w.done,&reply,1); LOG_SYSCALL("read",received>=0,"worker completion"); }
            if(ready<=0 || !(wait.revents&POLLIN) || received!=1 || reply!='D') {
                if(ready==0) errno=ETIMEDOUT; else if(ready>0) errno=EIO;
                LOG_SYSCALL("poll/read",false,"3D worker response");
                std::cerr << "Worker failed or timed out\n"; ok=false;
            }
        }
        double elapsed=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-batchStart).count();
        if(batches++==0) firstBatchUs=elapsed; else remainingBatchesUs+=elapsed;
    }
    if(ok) LOG_SYSCALL("pipe write/read",true,"3D batch dispatch and completion");
    auto shutdownStart=std::chrono::steady_clock::now();
    bool workersOk=shutdownWorkers(workers,!ok);
    auto shutdownEnd=std::chrono::steady_clock::now();
    if(!workersOk) { std::cerr << "Worker cleanup failed\n"; ok=false; }
    if(ok) {
        std::cout << "TIMING batch_size=" << batchSize
                  << " launch_us=" << std::chrono::duration<double,std::micro>(launchEnd-launchStart).count()
                  << " first_batch_us=" << firstBatchUs << " remaining_batches_us=" << remainingBatchesUs
                  << " remaining_batches=" << (batches ? batches-1 : 0)
                  << " shutdown_us=" << std::chrono::duration<double,std::micro>(shutdownEnd-shutdownStart).count() << '\n';
        try { ok=writeFile(argv[2],format(arena)); }
        catch(const std::exception& ex) { std::cerr << "Output error: " << ex.what() << '\n'; ok=false; }
    }
    arena->~Arena();
    int unmapped=munmap(mapping,sizeof(Arena)); LOG_SYSCALL("munmap",unmapped==0,"3D shared arena");
    if(unmapped!=0) { perror("munmap"); ok=false; }
    int descriptorsAfter=descriptorCount();
    std::cout << "RESOURCE_COUNTS before=" << descriptorsBefore << " after=" << descriptorsAfter << '\n';
    if(ok) {
        rusage parent{}, children{};
        bool parentOk=getrusage(RUSAGE_SELF,&parent)==0;
        bool childrenOk=getrusage(RUSAGE_CHILDREN,&children)==0;
        auto cpu=[](const rusage& r) -> long long {
            return (static_cast<long long>(r.ru_utime.tv_sec)+r.ru_stime.tv_sec)*1000000LL + r.ru_utime.tv_usec+r.ru_stime.tv_usec;
        };
        std::cout << "RESOURCE parent_cpu_us=" << (parentOk ? cpu(parent) : -1)
                  << " worker_cpu_us=" << (childrenOk ? cpu(children) : -1)
                  << " parent_peak_kb=" << (parentOk ? parent.ru_maxrss : -1)
                  << " worker_peak_kb=" << (childrenOk ? children.ru_maxrss : -1) << '\n';
        std::cout << "Processed " << records.size() << " observations using " << count << " persistent workers.\n";
    }
    return ok ? 0 : 1;
}
