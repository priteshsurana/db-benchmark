#include "experiment/Experiment5.hpp"
#include "data/DataGenerator.hpp"
#include <chrono>
#include <algorithm>
#include <random>
#include <numeric>
#include <iostream>


//range scan

Experiment5::Experiment5(DBInterface* db, 
    const std::vector<Order>* dataset, 
    const BenchmarkConfig& config):
    ExperimentBase(db, dataset, config),
    small_recorder_(config.read_sample_size),
    medium_recorder_(config.read_sample_size),
    large_recorder_(config.read_sample_size){}


void Experiment5::setup() {
    result_.database        = db_->get_db_name();
    result_.experiment_name = "exp5 range scan";
    result_.operation = "range_scan";
    result_.thread_count = 1;
    std::cout<< "["<< db_->get_db_name() <<
        "] Creating created_at index for range scan ..."<<std::endl;
    db_->create_secondary_index();
}

void Experiment5::run() {
    using clock = std::chrono::steady_clock;
    const std::string db_name = db_->get_db_name();
    std::vector<std::string> cassandra_user_ids;
    if(db_name == "cassandra") {
        DataGenerator gen(config_.rng_seed);
        cassandra_user_ids = gen.sample_user_ids(config_.read_sample_size);
        std::mt19937_64 rng(config_.rng_seed ^ 0x12345678ABCDEFULL);
        std::shuffle(cassandra_user_ids.begin(), cassandra_user_ids.end(), rng);
    }

    auto run_window = [&](int window_days,
                          LatencyRecorder& recorder) -> uint64_t {
        recorder.start_timer();
        uint64_t total_rows_returned = 0;
 
        DataGenerator gen(config_.rng_seed ^ static_cast<uint64_t>(window_days));
 
        for (uint64_t q = 0; q < config_.read_sample_size; ++q) {
            auto [start_date, end_date] = gen.generate_date_range(
                *dataset_, window_days);
 
            auto t0 = clock::now();
            std::vector<Order> rows;
 
            if (db_name == "cassandra") {
                // Cassandra: range within a single user's partition.
                // Call find_by_secondary_key to get the user's partition,
                // then filter by date client-side.
                const std::string& user_id = cassandra_user_ids[q];
                std::vector<Order> user_orders = db_->find_by_secondary_key(user_id);
                for (const auto& o : user_orders) {
                    if (o.created_at >= start_date && o.created_at <= end_date) {
                        rows.push_back(o);
                    }
                }
            } else {
                // PostgreSQL / MongoDB: range scan across all rows.
                // Uses the created_at index (B+Tree linked leaf walk).
                rows = db_->find_by_range(start_date, end_date);
            }
 
            auto t1 = clock::now();
            auto us = std::chrono::duration_cast<
                std::chrono::microseconds>(t1 - t0).count();
            recorder.record(static_cast<uint64_t>(us));
 
            total_rows_returned += rows.size();
        }
 
        recorder.stop_timer();
        return total_rows_returned;
    };
 
    // --- Small window (1 day) ---
    std::cout << "  [" << db_name << "] Range scan: "
              << config_.range_window_small << "-day window...\n";
    uint64_t small_rows = run_window(config_.range_window_small, small_recorder_);
    avg_result_count_small_ = small_rows / config_.read_sample_size;
 
    // --- Medium window (7 days) ---
    std::cout << "  [" << db_name << "] Range scan: "
              << config_.range_window_medium << "-day window...\n";
    uint64_t medium_rows = run_window(config_.range_window_medium, medium_recorder_);
    avg_result_count_medium_ = medium_rows / config_.read_sample_size;
 
    // --- Large window (30 days) ---
    std::cout << "  [" << db_name << "] Range scan: "
              << config_.range_window_large << "-day window...\n";
    uint64_t large_rows = run_window(config_.range_window_large, large_recorder_);
    avg_result_count_large_ = large_rows / config_.read_sample_size;
}
 

// ============================================================
void Experiment5::teardown() {
    db_->drop_secondary_index();
}

std::vector<BenchmarkResult> Experiment5::get_all_results() const {
 
    auto make_result = [&](LatencyRecorder& recorder,
                            int window_days,
                            uint64_t avg_rows,
                            const std::string& label) -> BenchmarkResult {
        BenchmarkResult r = recorder.compute_stats();
        r.database         = db_->get_db_name();
        r.experiment_name  = "exp_05_range_scan";
        r.operation        = "range_scan";
        r.index_type       = "secondary";       // created_at index
        r.phase            = label;             // "1d", "7d", "30d"
        r.thread_count     = 1;
        r.result_set_size  = avg_rows;
 
        if (db_->get_db_name() == "cassandra" && window_days > 0) {
            r.notes =
                "Cassandra: range within single user partition. "
                "Cross-partition range requires ALLOW FILTERING "
                "(full cluster scan — not used in production).";
        }
 
        return r;
    };
 
    // Cast away const on recorders for compute_stats() —
    // compute_stats() is logically const (reads only).
    // The mutable keyword on recorder members would be cleaner
    // but requires header change. Use const_cast here as a
    // pragmatic workaround.
    return {
        make_result(const_cast<LatencyRecorder&>(small_recorder_),
                    config_.range_window_small,
                    avg_result_count_small_,
                    std::to_string(config_.range_window_small) + "d"),
 
        make_result(const_cast<LatencyRecorder&>(medium_recorder_),
                    config_.range_window_medium,
                    avg_result_count_medium_,
                    std::to_string(config_.range_window_medium) + "d"),
 
        make_result(const_cast<LatencyRecorder&>(large_recorder_),
                    config_.range_window_large,
                    avg_result_count_large_,
                    std::to_string(config_.range_window_large) + "d")
    };
}
 
