// Sequential Phase 2 driver. Reads measurements only; ground truth is not input.
#include "ekf3d.hpp"
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>

int main(int argc, char** argv) {
    if (argc != 3 || std::string(argv[1]) == argv[2]) {
        std::cerr << "Usage: run_ekf3d <measurements.csv> <new-output.csv>\n";
        return 2;
    }
    std::ifstream input(argv[1]);
    if (!input) { std::cerr << "Cannot open measurements\n"; return 1; }
    std::string line;
    if (!std::getline(input, line)) { std::cerr << "Missing input header\n"; return 1; }
    if (!line.empty() && line.back()=='\r') line.pop_back();
    if (line != "timestamp,track_id,range_m,azimuth_rad,elevation_rad,available") {
        std::cerr << "Unexpected input header\n"; return 1;
    }
    std::error_code ec;
    bool exists = std::filesystem::exists(argv[2], ec);
    if (ec || exists) { std::cerr << "Output exists or cannot be inspected; choose a new path\n"; return 1; }
    std::ofstream output(argv[2]);
    if (!output) { std::cerr << "Cannot create output\n"; return 1; }
    output << "timestamp,track_id,x,y,z,vx,vy,vz,ax,ay,az,p_xx,p_xy,p_xz,p_yy,p_yz,p_zz\n" << std::setprecision(17);
    std::unordered_map<int, ekf3d::Filter> filters;
    size_t count=0;
    try {
        while (std::getline(input, line)) {
            if (!line.empty() && line.back()=='\r') line.pop_back();
            if (line.empty()) continue;
            std::stringstream row(line); std::string fields[6];
            for(auto& field : fields) if (!std::getline(row, field, ',')) throw std::invalid_argument("missing field");
            std::string extra; if (std::getline(row,extra,',')) throw std::invalid_argument("extra field");
            auto strictDouble = [](const std::string& text) {
                size_t used = 0; double value = std::stod(text, &used);
                if (used != text.size()) throw std::invalid_argument("invalid numeric field");
                return value;
            };
            size_t idLength = 0;
            double timestamp=strictDouble(fields[0]); int id=std::stoi(fields[1], &idLength);
            if (idLength != fields[1].size()) throw std::invalid_argument("invalid track");
            if (id<0 || (fields[5]!="0" && fields[5]!="1")) throw std::invalid_argument("invalid track or availability");
            auto& filter=filters[id];
            ekf3d::Measurement z{};
            if (fields[5]=="1") {
                for(int i=0;i<3;++i) z[i]=strictDouble(fields[i+2]);
                filter.step(timestamp,&z);
            } else {
                if (!fields[2].empty() || !fields[3].empty() || !fields[4].empty())
                    throw std::invalid_argument("missing measurement must have empty values");
                filter.step(timestamp,nullptr);
            }
            output << timestamp << ',' << id;
            for(double value : filter.state()) output << ',' << value;
            const auto& covariance = filter.covariance();
            for (int i=0;i<3;++i) for (int j=i;j<3;++j) output << ',' << covariance[i][j];
            output << '\n'; ++count;
        }
    } catch(const std::exception& error) {
        std::cerr << "Invalid measurement at row " << count+2 << ": " << error.what() << '\n'; return 1;
    }
    output.flush();
    if (!count || !output || input.bad()) { std::cerr << "Empty input or I/O error\n"; return 1; }
    std::cout << "Processed " << count << " observations across " << filters.size() << " tracks.\n";
}
