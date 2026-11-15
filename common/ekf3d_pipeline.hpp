#ifndef EKF3D_PIPELINE_HPP
#define EKF3D_PIPELINE_HPP

#include "ekf3d.hpp"
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace ekf3d_pipeline {
constexpr std::size_t capacity = 100000;
constexpr unsigned batch_size = 128;
struct Record { double time; int track; bool available; ekf3d::Measurement measurement; };
struct Result { ekf3d::State state; double covariance[6]; };
struct Arena { unsigned count, begin, end; Record records[capacity]; Result results[capacity]; };

inline double number(const std::string& text) {
    std::size_t used = 0; double value = std::stod(text, &used);
    if (used != text.size() || !std::isfinite(value)) throw std::invalid_argument("invalid numeric field");
    return value;
}

inline std::vector<Record> parse(const std::string& input) {
    std::istringstream stream(input); std::string line;
    if (!std::getline(stream, line)) throw std::invalid_argument("missing header");
    if (!line.empty() && line.back()=='\r') line.pop_back();
    if (line != "timestamp,track_id,range_m,azimuth_rad,elevation_rad,available") throw std::invalid_argument("invalid header");
    std::vector<Record> records; std::unordered_map<int, double> last;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back()=='\r') line.pop_back();
        if (line.empty()) continue;
        std::istringstream row(line); std::string fields[6], extra;
        for (auto& field : fields) if (!std::getline(row,field,',')) throw std::invalid_argument("missing field");
        if (std::getline(row,extra,',')) throw std::invalid_argument("extra field");
        std::size_t used=0; int id=std::stoi(fields[1],&used);
        if (used != fields[1].size() || id<0 || (fields[5]!="0" && fields[5]!="1")) throw std::invalid_argument("invalid track or availability");
        Record r{}; r.time=number(fields[0]); r.track=id; r.available=fields[5]=="1";
        auto old=last.find(id);
        if (r.time<0 || (old!=last.end() && r.time<=old->second) || (old==last.end() && !r.available))
            throw std::invalid_argument("invalid timestamp or first observation");
        if (r.available) for(int i=0;i<3;++i) r.measurement[i]=number(fields[i+2]);
        else if (!fields[2].empty() || !fields[3].empty() || !fields[4].empty()) throw std::invalid_argument("invalid missing observation");
        if(records.size()>=capacity) throw std::invalid_argument("record limit exceeded");
        last[id]=r.time; records.push_back(r);
    }
    if(records.empty()) throw std::invalid_argument("empty input");
    return records;
}

inline void process(Arena* arena, unsigned worker, unsigned workers, std::unordered_map<int, ekf3d::Filter>& filters) {
    for(unsigned i=arena->begin; i<arena->end; ++i) {
        const auto& r=arena->records[i];
        if (static_cast<unsigned>(r.track)%workers != worker) continue;
        auto& filter=filters[r.track];
        filter.step(r.time, r.available ? &r.measurement : nullptr);
        auto& out=arena->results[i]; out.state=filter.state();
        const auto& cov=filter.covariance(); int k=0;
        for(int a=0;a<3;++a) for(int b=a;b<3;++b) out.covariance[k++]=cov[a][b];
    }
}

inline std::string format(const Arena* arena) {
    std::ostringstream output;
    output << "timestamp,track_id,x,y,z,vx,vy,vz,ax,ay,az,p_xx,p_xy,p_xz,p_yy,p_yz,p_zz\n" << std::setprecision(17);
    for(unsigned i=0;i<arena->count;++i) {
        output << arena->records[i].time << ',' << arena->records[i].track;
        for(double v : arena->results[i].state) output << ',' << v;
        for(double v : arena->results[i].covariance) output << ',' << v;
        output << '\n';
    }
    if(!output) throw std::runtime_error("could not format output");
    return output.str();
}
}
#endif
