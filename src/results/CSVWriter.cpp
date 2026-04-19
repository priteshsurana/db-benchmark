#include "results/CSVWriter.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>

CSVWriter::CSVWriter(const std::string& output_path):
    output_path_(output_path) {
        if(!file_exists()) {
            write_header();
        }
    }

void CSVWriter::write(const BenchmarkResult& result) {
    std::ofstream file(output_path_, std::ios::app);
    if (!file.is_open()){
        throw std::runtime_error("CSVWriter: cannot open'"+ output_path_+"' for writing");
    }

    auto now = std::chrono::system_clock::now();
    auto now_t = std::chrono::system_clock::to_time_t(now);
    struct tm tm_info;
    gmtime_r(&now_t, &tm_info);
    char ts_buf[32];
    std::strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%m:%SZ", &tm_info);
    file << escape(ts_buf)                              << ','
         << escape(result.database)                     << ','
         << escape(result.experiment_name)              << ','
         << escape(result.operation)                    << ','
         << escape(result.index_type)                   << ','
         << escape(result.phase)                        << ','
         << result.thread_count                         << ','
         << result.total_ops                            << ','
         << std::fixed << std::setprecision(3)
         << result.total_time_ms                        << ','
         << result.p50_us                               << ','
         << result.p95_us                               << ','
         << result.p99_us                               << ','
         << result.min_us                               << ','
         << result.max_us                               << ','
         << result.mean_us                              << ','
         << std::setprecision(1)
         << result.throughput_ops_sec                   << ','
         << result.sstable_count                        << ','
         << result.result_set_size                      << ','
         << escape(result.notes)
         << '\n';

    
}

void CSVWriter::write_all(const std::vector<BenchmarkResult>& results) {
    for(const auto& r: results){
        write(r);
    }
}

std::string CSVWriter::output_path() const {
    return output_path_;

}


void CSVWriter::write_header() {
    std::ofstream file(output_path_ ,  std::ios::trunc);
    if (!file.is_open()) {
        throw std::runtime_error(
                "CSVWriter: cannot create '"+output_path_+"'");
        
    }

        file << "run_timestamp"
         << ",database"
         << ",experiment_name"
         << ",operation"
         << ",index_type"
         << ",phase"
         << ",thread_count"
         << ",total_ops"
         << ",total_time_ms"
         << ",p50_us"
         << ",p95_us"
         << ",p99_us"
         << ",min_us"
         << ",max_us"
         << ",mean_us"
         << ",throughput_ops_sec"
         << ",sstable_count"
         << ",result_set_size"
         << ",notes"
         << '\n';

}

bool CSVWriter::file_exists() const {
    struct stat st;
    return stat(output_path_.c_str(), &st)==0;
}

std::string CSVWriter::escape(const std::string& field) const {
    bool needs_quoting = field.find_first_of(",\"\n\r") !=std::string::npos;
    if(!needs_quoting) {
        return field;
    }

    std::string result;
    result.reserve(field.size()+2);
    result+= '"';
    for(char c:field) {
        if(c=='"') result+='"';
        result+=c;
    }
    result+='"';
    return result;
}