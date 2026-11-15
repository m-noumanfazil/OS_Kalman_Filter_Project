// 3D EKF: POSIX file I/O, mmap shared arena, persistent forked workers and pipes.
#include "../../common/ekf3d_pipeline.hpp"
#include "../../common/logger_linux.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <new>
#include <poll.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace ekf3d_pipeline;
struct Worker { pid_t pid; int command; int done; };

static bool readFile(const char* path, std::string& content) {
    int fd=open(path,O_RDONLY);
    LOG_SYSCALL("open",fd>=0,"3D sensor input");
    if(fd<0) { perror("open input"); return false; }
    char buffer[16384]; bool ok=true;
    for (;;) {
        ssize_t n=read(fd,buffer,sizeof(buffer));
        if(n<0 && errno==EINTR) continue;
        if(n<0) { LOG_SYSCALL("read",false,"3D sensor input"); perror("read input"); ok=false; break; }
        if(n==0) break;
        content.append(buffer,static_cast<size_t>(n));
        if(content.size()>64*1024*1024) { std::cerr << "Input exceeds 64 MiB\n"; ok=false; break; }
    }
    if(ok) LOG_SYSCALL("read",true,"3D sensor input");
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
        if(n<0 && errno==EINTR) continue;
        if(n<=0) { LOG_SYSCALL("write",false,"3D output"); perror("write output"); ok=false; break; }
        offset+=static_cast<size_t>(n);
    }
    if(ok) LOG_SYSCALL("write",true,"3D output");
    int closed=close(fd); LOG_SYSCALL("close",closed==0,"3D output");
    if(closed!=0) { perror("close output"); ok=false; }
    return ok;
}

static bool shutdownWorkers(std::vector<Worker>& workers, bool abort) {
    bool ok=true;
    for(auto& w:workers) {
        if(close(w.command)!=0) ok=false;
        if(close(w.done)!=0) ok=false;
        if(abort && kill(w.pid,SIGKILL)!=0 && errno!=ESRCH) ok=false;
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
    if(argc!=4) { std::cerr << "Usage: EKF3D_Linux <input.csv> <new-output.csv> <workers 1..8>\n"; return 2; }
    int count=0;
    try { std::size_t n=0; count=std::stoi(argv[3],&n); if(n!=std::strlen(argv[3]) || count<1 || count>8) throw std::invalid_argument("workers"); }
    catch(const std::exception&) { std::cerr << "Invalid worker count\n"; return 2; }
    logger::init(std::string(argv[2])+".log");
    std::string contents;
    if(!readFile(argv[1],contents)) return 1;
    std::vector<Record> records;
    try { records=parse(contents); } catch(const std::exception& ex) { std::cerr << "Invalid input: " << ex.what() << '\n'; return 1; }
    void* mapping=mmap(nullptr,sizeof(Arena),PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    LOG_SYSCALL("mmap",mapping!=MAP_FAILED,"3D shared arena");
    if(mapping==MAP_FAILED) { perror("mmap"); return 1; }
    auto* arena=new(mapping) Arena{};
    arena->count=static_cast<unsigned>(records.size());
    std::copy(records.begin(),records.end(),arena->records);
    std::vector<Worker> workers; bool ok=true;
    std::signal(SIGPIPE,SIG_IGN);
    for(int i=0;i<count;++i) {
        int commands[2], replies[2];
        int commandResult=pipe(commands); LOG_SYSCALL("pipe",commandResult==0,"3D worker commands");
        if(commandResult!=0) { perror("pipe commands"); ok=false; break; }
        int replyResult=pipe(replies); LOG_SYSCALL("pipe",replyResult==0,"3D worker replies");
        if(replyResult!=0) { perror("pipe replies"); close(commands[0]); close(commands[1]); ok=false; break; }
        pid_t pid=fork();
        if(pid==0) {
            close(commands[1]); close(replies[0]);
            for(const auto& w:workers) { close(w.command); close(w.done); }
            std::unordered_map<int,ekf3d::Filter> filters; char command;
            while(read(commands[0],&command,1)==1 && command=='R') {
                try { process(arena,static_cast<unsigned>(i),static_cast<unsigned>(count),filters); }
                catch(const std::exception& ex) { std::cerr << "Worker " << i << ": " << ex.what() << '\n'; _exit(3); }
                char done='D'; if(write(replies[1],&done,1)!=1) _exit(4);
            }
            close(commands[0]); close(replies[1]); _exit(0);
        }
        LOG_SYSCALL("fork",pid>=0,"3D persistent worker");
        close(commands[0]); close(replies[1]);
        if(pid<0) { perror("fork"); close(commands[1]); close(replies[0]); ok=false; break; }
        workers.push_back({pid,commands[1],replies[0]});
    }
    for(unsigned begin=0;ok && begin<arena->count;begin+=batch_size) {
        arena->begin=begin; arena->end=std::min(begin+batch_size,arena->count);
        for(const auto& w:workers) { char command='R'; if(write(w.command,&command,1)!=1) { perror("send work"); ok=false; break; } }
        for(const auto& w:workers) {
            if(!ok) break;
            pollfd wait{w.done,POLLIN,0}; int ready;
            do { ready=poll(&wait,1,15000); } while(ready<0 && errno==EINTR);
            char reply=0;
            if(ready<=0 || !(wait.revents&POLLIN) || read(w.done,&reply,1)!=1 || reply!='D') {
                if(ready==0) errno=ETIMEDOUT; else if(ready>0) errno=EIO;
                LOG_SYSCALL("poll/read",false,"3D worker response");
                std::cerr << "Worker failed or timed out\n"; ok=false;
            }
        }
    }
    if(ok) LOG_SYSCALL("pipe write/read",true,"3D batch dispatch and completion");
    bool workersOk=shutdownWorkers(workers,!ok);
    if(!workersOk) { std::cerr << "Worker cleanup failed\n"; ok=false; }
    if(ok) {
        try { ok=writeFile(argv[2],format(arena)); }
        catch(const std::exception& ex) { std::cerr << "Output error: " << ex.what() << '\n'; ok=false; }
    }
    arena->~Arena();
    int unmapped=munmap(mapping,sizeof(Arena)); LOG_SYSCALL("munmap",unmapped==0,"3D shared arena");
    if(unmapped!=0) { perror("munmap"); ok=false; }
    if(ok) std::cout << "Processed " << records.size() << " observations using " << count << " persistent workers.\n";
    return ok ? 0 : 1;
}
