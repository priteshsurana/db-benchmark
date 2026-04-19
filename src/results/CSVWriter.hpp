#pragma once

#include "metrics/LatencyRecorder.hpp"
#include <string>
#include <vector>

class CSVWriter {
    public:
    explicit CSVWriter(const std::string& output_path);
    void write(const BenchmarkResult& result);
    void write_all (const std::vector<BenchmarkResult>& results);
    std::string output_path() const;

    private:
    std::string output_path_;
    void write_header();
    bool file_exists() const;

    std::string escape(const std::string& field) const;
};