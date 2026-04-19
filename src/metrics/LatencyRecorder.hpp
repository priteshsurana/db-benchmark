#pragma once

#include <string>
#include <vector>
#include "BenchmarkResult.hpp"

class LatencyRecorder {
    public:
    explicit LatencyRecorder(uint64_t expected_ops = 1'000'000);

    void record(uint64_t duration_us);
    void start_timer();
    void stop_timer();
    BenchmarkResult compute_stats() const;
    uint64_t count() const;
    void reset();

    private:
    std::vector<uint64_t> latencies_us_;
    uint64_t start_ns_ =0;
    uint64_t end_ns_ = 0;
    double percentile(const std::vector<uint64_t>& sorted, double p) const;
};