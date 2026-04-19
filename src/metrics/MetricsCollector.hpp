#pragma once
#include <vector>
#include "metrics/LatencyRecorder.hpp"

class MetricsCollector {
    public:
    MetricsCollector() = default;

    void add_result(const BenchmarkResult& result);
    BenchmarkResult merge_thread_results(const std::vector<LatencyRecorder>& recorders, const BenchmarkResult& template_result) const;
    const std::vector<BenchmarkResult>& get_all_results() const;
    uint64_t count() const;

    private:
    std::vector<BenchmarkResult> results_;
};