#include "experiment/Experiment2.hpp"
#include "metrics/MetricsCollector.hpp"
#include <thread>
#include <chrono>
#include <stdexcept>
#include <iostream>
#include <iomanip>

Experiment2::Experiment2(
    DBInterface* db,
    const std::vector<Order>* dataset,
    const BenchmarkConfig& config,
    ConnectionFactory connection_factory):
    ExperimentBase(db, dataset, config),
    connection_factory_(std::move(connection_factory)){}


void Experiment2::setup() {
    db_->clear_table();
    result_.database    = db_->get_db_name();
    result_.experiment_name = "experiment 2 concurrent_insert";
    result_.operation = "insert";
    result_.index_type = "primary";
    result_.thread_count = config_.thread_count;
}

void Experiment2::run() {
    using clock = std::chrono::steady_clock;
    const uint32_t num_threads = config_.thread_count;
    const uint32_t total_rows = std::min(
        static_cast<uint64_t>(dataset_->size()),
        config_.total_rows);
    const uint64_t rows_per_thread = total_rows/num_threads;
    thread_connections_.resize(num_threads, nullptr);
    for (uint32_t i=0; i<num_threads; i++) {
        thread_connections_[i] = connection_factory_(static_cast<int>(i));
    }
    thread_recorders_.clear();
    thread_recorders_.reserve(num_threads);
    for (uint32_t i =0; i<num_threads; i++) {
        thread_recorders_.emplace_back(rows_per_thread);
    }

    auto wall_start = clock::now();
    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (uint32_t t = 0; t<num_threads; ++t) {
        threads.emplace_back([this, t, rows_per_thread, total_rows, num_threads](){
            DBInterface* conn = thread_connections_[t];
            LatencyRecorder& recorder = thread_recorders_[t];
            uint64_t start_idx = static_cast<uint64_t>(t)*rows_per_thread;
            uint64_t end_idx = (t==num_threads-1)
                ? total_rows: start_idx+rows_per_thread;
            for (uint64_t i=start_idx; i<end_idx; i++) {
                const Order& order = (*dataset_)[i];
                auto t0 = std::chrono::steady_clock::now();
                conn->insert_one(order);
                auto t1 = std::chrono::steady_clock::now();

                auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1-t0).count();
                recorder.record(static_cast<uint64_t>(us));
            }
        });
    }

    for (auto& th: threads) {
        th.join();
    }

    auto wall_end = clock::now();
    double wall_ms = std::chrono::duration_cast<
        std::chrono::microseconds>(wall_end - wall_start).count()/1000.0;

    uint64_t start_ns = static_cast<uint64_t>(
        wall_start.time_since_epoch().count());
    uint64_t end_ns = static_cast<uint64_t>(
        wall_end.time_since_epoch().count());
    for (auto& rec: thread_recorders_) {
        rec.start_timer();
        rec.stop_timer();

    }

    MetricsCollector collector;
    BenchmarkResult merged = collector.merge_thread_results(
        thread_recorders_, result_);
    merged.total_ops = total_rows;
    merged.total_time_ms = wall_ms;
    merged.throughput_ops_sec = (wall_ms>0.0)
        ? static_cast<double>(total_rows)/(wall_ms/1000.0) : 0.0;

    result_.total_ops = merged.total_ops;
    result_.total_time_ms = merged.total_time_ms;
    result_.p50_us = merged.p50_us;
    result_.p95_us = merged.p95_us;
    result_.p99_us = merged.p99_us;
    result_.min_us = merged.min_us;
    result_.max_us = merged.max_us;
    result_.mean_us = merged.mean_us;
    result_.throughput_ops_sec = merged.throughput_ops_sec;

    std::cout << "  [" << db_->get_db_name() << "] "
              << num_threads << " threads finished. "
              << "Wall time: " << std::fixed << std::setprecision(1)
              << wall_ms << "ms\n";
}


void Experiment2::teardown() {
    for (auto* conn : thread_connections_) {
        if (conn && conn != db_) {
            // This is a per-thread connection created by the factory —
            // disconnect and delete it.
            try { conn->disconnect(); } catch (...) {}
            delete conn;
        }
        // If conn == db_, it's the shared Cassandra client owned
        // by main.cpp — do not delete.
    }
    thread_connections_.clear();
    thread_recorders_.clear();
}
