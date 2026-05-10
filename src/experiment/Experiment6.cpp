#include "experiment/Experiment6.hpp"
#include <fstream>
#include <chrono>
#include <algorithm>
#include <stdexcept>
#include <iostream>
#include <cstdlib>
#include <random>

//compaction

Experiment6::Experiment6(DBInterface* db, const std::vector<Order>* dataset, 
    const BenchmarkConfig& config):
    ExperimentBase(db, dataset, config),
    before_recorder_(config.read_sample_size),
    after_recorder_(config.read_sample_size){}


void Experiment6::setup() {
    result_.database        = db_->get_db_name();
    result_.experiment_name = "exp_06_compaction";
    result_.operation       = "point_read";
    result_.index_type      = "primary";
    result_.thread_count    = 1;
 
    if (db_->get_db_name() != "cassandra") {
        should_skip_ = true;
        std::cout << "  [" << db_->get_db_name()
                  << "] EXP-06 is Cassandra-only. Skipping.\n";
        return;
    }
 

    std::vector<Order> sample = sample_random_orders(config_.read_sample_size);
    sample_ids_.reserve(sample.size());
    for (const auto& o : sample) {
        sample_ids_.push_back(o.order_id);
    }
 
    // Shuffle to prevent sequential access patterns.
    std::mt19937_64 rng(config_.rng_seed ^ 0xDEADC0DE12345678ULL);
    std::shuffle(sample_ids_.begin(), sample_ids_.end(), rng);

    before_sstable_count_ = get_sstable_count();
    if (before_sstable_count_ <= 1) {
        std::cout << "  [cassandra] WARNING: Only " << before_sstable_count_
                  << " SSTable(s) found. EXP-06 is most meaningful with multiple "
                  << "SSTables.\n"
                  << "  Consider reducing memtable_heap_space in cassandra.yaml "
                  << "to force more frequent flushes during EXP-01.\n";
    }
    std::cout << "  [cassandra] SSTables before compaction: "
              << before_sstable_count_ << "\n";
}

void Experiment6::run() {
    if (should_skip_) return;
 
    using clock = std::chrono::steady_clock;
 
    // Phase 1: Reads before compaction 
    std::cout << "  [cassandra] Phase 1: reading before compaction...\n";
 
    before_recorder_.start_timer();
    for (const auto& order_id : sample_ids_) {
        auto t0 = clock::now();
        db_->find_by_primary_key(order_id);
        auto t1 = clock::now();
 
        auto us = std::chrono::duration_cast<
            std::chrono::microseconds>(t1 - t0).count();
        before_recorder_.record(static_cast<uint64_t>(us));
    }
    before_recorder_.stop_timer();
 
    //  Phase 2: Trigger compaction 
    std::cout << "  [cassandra] Phase 2: running compaction...\n";
    run_compaction();
 
    after_sstable_count_ = get_sstable_count();
    std::cout << "  [cassandra] SSTables after compaction: "
              << after_sstable_count_ << "\n";
 
    //  Phase 3: Reads after compaction 
    // Same sample_ids_, same read pattern — the only variable
    // that changed is the SSTable structure on disk.
    std::cout << "  [cassandra] Phase 3: reading after compaction...\n";
 
    after_recorder_.start_timer();
    for (const auto& order_id : sample_ids_) {
        auto t0 = clock::now();
        db_->find_by_primary_key(order_id);
        auto t1 = clock::now();
 
        auto us = std::chrono::duration_cast<
            std::chrono::microseconds>(t1 - t0).count();
        after_recorder_.record(static_cast<uint64_t>(us));
    }
    after_recorder_.stop_timer();
}


BenchmarkResult Experiment6::get_before_result() const {
    if (should_skip_) {
        BenchmarkResult r;
        r.database        = db_->get_db_name();
        r.experiment_name = "exp_06_compaction";
        r.phase           = "";    // Empty phase = skipped
        r.notes           = "Skipped — not Cassandra.";
        return r;
    }
 
    BenchmarkResult r = before_recorder_.compute_stats();
    r.database         = "cassandra";
    r.experiment_name  = "exp_06_compaction";
    r.operation        = "point_read";
    r.index_type       = "primary";
    r.phase            = "before_compaction";
    r.thread_count     = 1;
    r.sstable_count    = before_sstable_count_;
    return r;
}
 
BenchmarkResult Experiment6::get_after_result() const {
    if (should_skip_) {
        BenchmarkResult r;
        r.database        = db_->get_db_name();
        r.experiment_name = "exp_06_compaction";
        r.phase           = "";
        r.notes           = "Skipped — not Cassandra.";
        return r;
    }
 
    BenchmarkResult r = after_recorder_.compute_stats();
    r.database         = "cassandra";
    r.experiment_name  = "exp_06_compaction";
    r.operation        = "point_read";
    r.index_type       = "primary";
    r.phase            = "after_compaction";
    r.thread_count     = 1;
    r.sstable_count    = after_sstable_count_;
 
    if (before_recorder_.count() > 0) {
        BenchmarkResult before = before_recorder_.compute_stats();
        double ratio = (r.p99_us > 0) ? before.p99_us / r.p99_us : 0.0;
        r.notes = "p99 improvement: " +
                  std::to_string(ratio).substr(0, 4) + "x after compaction";
    }
 
    return r;
}

uint64_t Experiment6::get_sstable_count() const {
    const std::string cmd =
        "docker exec bench_cassandra nodetool tablehistograms "
        "benchmark orders_by_id 2>/dev/null "
        "| grep -i 'sstable' | awk '{print $NF}' | head -1 "
        "> /tmp/sstable_count.txt";
 
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::cerr << "  [cassandra] Warning: could not get SSTable count\n";
        return 0;
    }
 
    std::ifstream f("/tmp/sstable_count.txt");
    uint64_t count = 0;
    if (f.is_open()) {
        f >> count;
    }
    return count;
}
 
void Experiment6::run_compaction() {
    int rc = std::system("./scripts/cassandra_compact.sh");
    if (rc != 0) {
        throw std::runtime_error(
            "exp_06: cassandra_compact.sh failed. "
            "Return code: " + std::to_string(rc));
    }
}
