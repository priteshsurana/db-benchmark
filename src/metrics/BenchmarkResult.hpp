#pragma once
#include <string>

struct BenchmarkResult {
    std::string database;
    std::string experiment_name;
    std::string operation;
    std::string index_type;
    std::string phase;

    uint32_t thread_count =1;
    uint64_t total_ops = 0;

    double total_time_ms = 0.0;

    double p50_us = 0.0;
    double p95_us = 0.0;
    double p99_us = 0.0;
    double mean_us = 0.0;
    double min_us = 0.0;
    double max_us = 0.0;

    double throughput_ops_sec = 0.0;

    uint64_t sstable_count=0;
    uint64_t result_set_size = 0;
    std::string notes;

};