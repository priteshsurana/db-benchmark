#pragma once

#include <cstdint>
#include <string>
#include "DatabaseConfig.hpp"

struct BenchmarkConfig {
    uint64_t totalrows = 1'000'000;
    uint64_t read_sample_size = 10'000;
    uint32_t batch_size = 500;

    uint32_t thread_count = 8;
    uint32_t description_length = 600;

    //Todo Range configs
    int range_window_samll = 1;
    int range_window_medium = 7;
    int range_window_large = 30;

    //Output
    std::string results_dir = "./results";
    uint64_t rng_seed = 42;

    DatabaseConfig postgres;
    DatabaseConfig mongo;
    DatabaseConfig cassandra;

};