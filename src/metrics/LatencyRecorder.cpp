#include "metrics/LatencyRecorder.hpp"
#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <chrono>
#include <cmath>

LatencyRecorder::LatencyRecorder(uint64_t expected_ops) {
    latencies_us_.reserve(expected_ops);
}

void LatencyRecorder::record(uint64_t duration_us) {
    latencies_us_.push_back(duration_us);
}

void LatencyRecorder::stop_timer() {
    end_ns_ = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
}

BenchmarkResult LatencyRecorder::compute_stats() const {
    BenchmarkResult result;
    if (latencies_us_.empty()) {
        return result;
    }

    std::vector<uint64_t> sorted = latencies_us_;
    std::sort(sorted.begin(), sorted.end());

    result.total_ops = sorted.size();
    result.total_time_ms = static_cast<double>(end_ns_-start_ns_)/1'000'000.0;

    result.p50_us = percentile(sorted, 0.50);
    result.p95_us = percentile(sorted, 0.95);
    result.p99_us = percentile(sorted, 0.99);
    result.min_us = static_cast<double>(sorted.front());
    result.max_us = static_cast<double>(sorted.back());

    double sum = std::accumulate(sorted.begin(), sorted.end(), 0.0);
    result.mean_us = sum /static_cast<double>(sorted.size());
    if(result.total_time_ms > 0.0) {
        result.throughput_ops_sec = static_cast<double>(result.total_ops)/(result.total_time_ms/1000.0);
    }

    return result;

}



uint64_t LatencyRecorder::count() const {
    return latencies_us_.size();
}

void LatencyRecorder::reset() {
    latencies_us_.clear();
    start_ns_ = 0;
    end_ns_ = 0;
}

double LatencyRecorder::percentile(const std::vector<uint64_t>& sorted, double p) const {
    if (sorted.empty()) return 0.0;
    if (p <= 0.0) return static_cast<double>(sorted.front());
    if (p>=1.0) return static_cast<double>(sorted.back());

    double rank = p*static_cast<double>(sorted.size());
    size_t index = static_cast<size_t>(std::ceil(rank)) - 1;
    index = std::min(index, sorted.size() - 1);

    return static_cast<double>(sorted[index]);
}