#pragma once

#include "metrics/LatencyRecorder.hpp"
#include <vector>

class SummaryPrinter {
    public:
    SummaryPrinter() = default;

    void print_result(const BenchmarkResult& result);
    void print_experiment_header(const std::string& experiment_name);
    void print_separator();
    void print_final_summary(const std::vector<BenchmarkResult>& results);
    void print_compaction_summary(const BenchmarkResult& before, const BenchmarkResult& after);
    void print_write_vs_read_summary(const BenchmarkResult& avg_write, const BenchmarkResult& avg_read);

    private:
    std::string format_latency(double us) const;
    std::string format_throughput(double ops_sec) const;

}