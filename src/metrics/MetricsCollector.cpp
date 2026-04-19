#include "metrics/MetricsCollector.hpp"
#include <algorithm>
#include <numeric>
#include <chrono>
#include <stdexcept>

void MetricsCollector::add_result(const BenchmarkResult& result) {
    results_.push_back(result);
}

uint64_t MetricsCollector::count() const {
    return results_.size();
}

const std::vector<BenchmarkResult>& MetricsCollector::get_all_results() const {
    return results_;

}

BenchmarkResult MetricsCollector::merge_thread_results(
    const std::vector<LatencyRecorder>& recorders, const BenchmarkResult& template_result) const{
    if (recorders.empty()) {
        throw::std::invalid_argument("no recorder for merge thread results");
    }

    std::vector<BenchmarkResult> thread_stats;
    thread_stats.reserve(recorders.size());
    for (const auto& r: recorders) {
        thread_stats.push_back(r.compute_stats());
    }

    uint64_t total_ops = 0;
    for (const auto& s: thread_stats) {
        total_ops+=s.total_ops;
    }

    double total_time_ms = 0.0;

    for(const auto& s: thread_stats) {
        total_time_ms = std::max(total_time_ms, s.total_time_ms);
    }

    std::vector<uint64_t> all_latencies;
    all_latencies.reserve(total_ops);

    BenchmarkResult merged = template_result;

    merged.total_ops = total_ops;
    merged.total_time_ms = total_time_ms;
    merged.throughput_ops_sec = (total_time_ms > 0.0) ? 
        static_cast<double>(total_ops) / (total_time_ms/ 1000.0): 0.0;
    
    double p50 = 0, p95 = 0, p99 = 0, mean = 0, min_v = 0, max_v = 0;
    bool first = true;
    for (const auto& s: thread_stats) {
        double w= static_cast<double>(s.total_ops)/static_cast<double>(total_ops);
        p50+= s.p50_us * w;
        p95+= s.p95_us * w;
        p99+= s.p99_us * w;
        if (first) {
            min_v = s.min_us;
            max_v = s.max_us;
            first = false;

        } else {
            min_v = std::min(min_v, s.min_us);
            max_v = std::max(max_v, s.max_us);
        }
    }

    merged.p50_us = p50;
    merged.p95_us = p95;
    merged.p99_us = p99;
    merged.mean_us = mean;
    merged.min_us = min_v;
    merged.max_us = max_v;


}