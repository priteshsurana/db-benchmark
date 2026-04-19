#include "ExperimentBase.hpp"
#include <stdexcept>
#include <algorithm>
#include <random>

ExperimentBase::ExperimentBase(DBInterface* db,
    const std::vector<Order>* dataset,
    const BenchmarkConfig& config):
    db_(db), dataset_(dataset), config_(config), recorder_(config.totalrows){
        if(!db_) {
            throw:: std::invalid_argument("ExperimentBase: db cannot be null");
        }

        if(!dataset_) {
            throw std::invalid_argument("ExperimentBase: dataset_ cannot be null");
        }
    }

void ExperimentBase::execute() {
    setup();
    recorder_.start_timer();
    run();
    recorder_.stop_timer();

    BenchmarkResult stats = recorder_.compute_stats();

    result_.total_ops = stats.total_ops;
    result_.total_time_ms = stats.total_time_ms;
    result_.p50_us = stats.p50_us;
    result_.p95_us = stats.p95_us;
    result_.p99_us = stats.p99_us;
    result_.mean_us = stats.mean_us;
    result_.min_us = stats.min_us;
    result_.max_us = stats.max_us;
    result_.throughput_ops_sec = stats.throughput_ops_sec;

    teardown();
}

BenchmarkResult ExperimentBase::get_result() const {
    return result_;
}

void ExperimentBase::teardown() {
    //TODO
}

std::vector<Order> ExperimentBase::pick_random_orders(uint64_t n) const {
    if (!dataset_ || dataset_->empty()) {
        throw std::runtime_error("sample random orders dataset is empty");
    }

    if(n> dataset_->size()) {
        n = dataset_->size();
    }

    std::mt19937_64 rng(config_.rng_seed ^ 0xFEEDFACE);
    std::uniform_int_distribution<uint64_t> dist(0, dataset_->size()-1);

    std::vector<Order> sample;
    sample.reserve(n);
    for (uint64_t i =0; i<n; i++) {
        sample.push_back((*dataset_)[dist(rng)]);
    }

    return sample;
}