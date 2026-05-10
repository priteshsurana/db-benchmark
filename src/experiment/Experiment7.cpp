#include "experiment/Experiment7.hpp"
#include <chrono>
#include <algorithm>
#include <numeric>
#include <random>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <stdexcept>


void Experiment7::setup() {
    result_.database        = db_->get_db_name();
    result_.experiment_name = "exp_07_write_read_symmetry";
    result_.thread_count    = 1;
}


void Experiment7::run() {
    for (int run = 1; run <= NUM_RUNS; ++run) {
        std::cout << "\n  [" << db_->get_db_name()
                  << "] EXP-07 Run " << run << "/" << NUM_RUNS << "\n";

        // Step 1: Clean state
        std::cout << "  [" << db_->get_db_name() << "] Clearing table...\n";
        db_->clear_table();

        // Step 2: Write phase
        LatencyRecorder write_rec(config_.total_rows);
        BenchmarkResult write_result = write_phase(run, write_rec);

        // Step 3: Flush all caches
        // Critical: without this, the read phase sees warm data
        // left by the write phase in the buffer pool.
        std::cout << "  [" << db_->get_db_name()
                  << "] Flushing caches before read phase...\n";
        flush_cache();

        // Reconnect after container restart
        db_->connect();

        // Step 4: Read phase
        LatencyRecorder read_rec(config_.total_rows);
        BenchmarkResult read_result = read_phase(run, read_rec);

        // Store this run's results
        run_results_[run - 1] = RunResult{
            run,
            std::move(write_result),
            std::move(read_result)
        };

        std::cout << "  [" << db_->get_db_name()
                  << "] Run " << run << " complete.\n";
        std::cout << "    Write: "
                  << std::fixed << std::setprecision(0)
                  << run_results_[run-1].write_result.throughput_ops_sec
                  << " ops/s\n";
        std::cout << "    Read:  "
                  << std::fixed << std::setprecision(0)
                  << run_results_[run-1].read_result.throughput_ops_sec
                  << " ops/s\n";
    }

    compute_averages();
}

BenchmarkResult Experiment7::write_phase(int run_number,
                                                      LatencyRecorder& recorder) {
    using clock = std::chrono::steady_clock;

    std::cout << "  [" << db_->get_db_name()
              << "] Write phase: inserting "
              << config_.total_rows << " rows...\n";

    const uint64_t n = std::min(
        static_cast<uint64_t>(dataset_->size()),
        config_.total_rows);

    // Collect order_ids as we insert — these are what we will
    // read in read_phase(), guaranteeing 1:1 correspondence.
    written_order_ids_.clear();
    written_order_ids_.reserve(n);

    recorder.start_timer();
    for (uint64_t i = 0; i < n; ++i) {
        const Order& order = (*dataset_)[i];

        auto t0 = clock::now();
        db_->insert_one(order);
        auto t1 = clock::now();

        auto us = std::chrono::duration_cast<
            std::chrono::microseconds>(t1 - t0).count();
        recorder.record(static_cast<uint64_t>(us));

        written_order_ids_.push_back(order.order_id);
    }
    recorder.stop_timer();

    BenchmarkResult r = recorder.compute_stats();
    r.database         = db_->get_db_name();
    r.experiment_name  = "exp_07_write_read_symmetry";
    r.operation        = "insert";
    r.index_type       = "primary";
    r.phase            = "run_" + std::to_string(run_number) + "_write";
    r.thread_count     = 1;

    return r;
}


BenchmarkResult Experiment7::read_phase(int run_number,
                                                     LatencyRecorder& recorder) {
    using clock = std::chrono::steady_clock;

    std::cout << "  [" << db_->get_db_name()
              << "] Read phase: reading "
              << written_order_ids_.size() << " rows (cold)...\n";

    std::vector<std::string> ids_to_read = written_order_ids_;
    std::mt19937_64 rng(config_.rng_seed ^ static_cast<uint64_t>(run_number) * 0xFEEDFACEULL);
    std::shuffle(ids_to_read.begin(), ids_to_read.end(), rng);

    recorder.start_timer();
    for (const auto& order_id : ids_to_read) {
        auto t0 = clock::now();
        db_->find_by_primary_key(order_id);
        auto t1 = clock::now();

        auto us = std::chrono::duration_cast<
            std::chrono::microseconds>(t1 - t0).count();
        recorder.record(static_cast<uint64_t>(us));
    }
    recorder.stop_timer();

    BenchmarkResult r = recorder.compute_stats();
    r.database         = db_->get_db_name();
    r.experiment_name  = "exp_07_write_read_symmetry";
    r.operation        = "point_read";
    r.index_type       = "primary";
    r.phase            = "run_" + std::to_string(run_number) + "_read";
    r.thread_count     = 1;

    return r;
}


void Experiment7::flush_cache() {
    const std::string& db_name = db_->get_db_name();

    std::string container;
    if (db_name == "postgres")  container = "bench_postgres";
    else if (db_name == "mongo") container = "bench_mongo";
    else if (db_name == "cassandra") container = "bench_cassandra";
    else throw std::runtime_error("exp_07: unknown db name: " + db_name);

    std::string cmd = "./scripts/flush_cache_mac.sh " + container;
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        throw std::runtime_error(
            "exp_07: flush_cache_mac.sh failed for " + container +
            ". Return code: " + std::to_string(rc));
    }
}

void Experiment7::compute_averages() {
    auto avg_results = [&](bool is_write) {
        BenchmarkResult avg;
        avg.database        = db_->get_db_name();
        avg.experiment_name = "exp_07_write_read_symmetry";
        avg.operation       = is_write ? "insert" : "point_read";
        avg.index_type      = "primary";
        avg.phase           = is_write ? "avg_write" : "avg_read";
        avg.thread_count    = 1;

        double sum_p50 = 0, sum_p95 = 0, sum_p99 = 0;
        double sum_mean = 0, sum_throughput = 0;
        double sum_total_ms = 0;
        uint64_t sum_ops = 0;
        double min_val = std::numeric_limits<double>::max();
        double max_val = 0;

        std::ostringstream notes;
        notes << "per-run throughput: ";

        for (int i = 0; i < NUM_RUNS; ++i) {
            const BenchmarkResult& r = is_write
                ? run_results_[i].write_result
                : run_results_[i].read_result;

            sum_p50        += r.p50_us;
            sum_p95        += r.p95_us;
            sum_p99        += r.p99_us;
            sum_mean       += r.mean_us;
            sum_throughput += r.throughput_ops_sec;
            sum_total_ms   += r.total_time_ms;
            sum_ops        += r.total_ops;
            min_val = std::min(min_val, r.min_us);
            max_val = std::max(max_val, r.max_us);

            if (i > 0) notes << "/";
            notes << std::fixed << std::setprecision(0)
                  << r.throughput_ops_sec;
        }

        avg.p50_us             = sum_p50        / NUM_RUNS;
        avg.p95_us             = sum_p95        / NUM_RUNS;
        avg.p99_us             = sum_p99        / NUM_RUNS;
        avg.mean_us            = sum_mean       / NUM_RUNS;
        avg.throughput_ops_sec = sum_throughput / NUM_RUNS;
        avg.total_time_ms      = sum_total_ms   / NUM_RUNS;
        avg.total_ops          = sum_ops        / NUM_RUNS;
        avg.min_us             = min_val;
        avg.max_us             = max_val;
        avg.notes              = notes.str() + " ops/s";

        return avg;
    };

    avg_write_result_ = avg_results(true);
    avg_read_result_  = avg_results(false);
}

std::vector<BenchmarkResult> Experiment7::get_all_run_results() const {
    std::vector<BenchmarkResult> out;
    out.reserve(NUM_RUNS * 2);
    for (const auto& rr : run_results_) {
        out.push_back(rr.write_result);
        out.push_back(rr.read_result);
    }
    return out;
}

std::pair<BenchmarkResult, BenchmarkResult>
Experiment7::get_averaged_results() const {
    return { avg_write_result_, avg_read_result_ };
}
