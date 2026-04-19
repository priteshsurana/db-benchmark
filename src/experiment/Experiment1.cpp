#include "Experiment1.hpp"
#include <chrono>
#include <stdexcept>

void Experiment1::setup() {
    db_->clear_table();
    result_.database = db_->get_db_name();
    result_.experiment_name = "1.Single Insert";
    result_.operation = "insert";
    result_.index_type = "primary";
    result_.thread_count = 1;

}


void Experiment1::run() {
    using clock = std::chrono::steady_clock;
    const uint64_t n = std::min(static_cast<uint64_t>(dataset_->size()), config_.totalrows);

    for(uint64_t i = 0; i<n; i++) {
        const Order& order = (*dataset_)[i];

        auto t0 = clock::now();
        db_->insert_one(order);
        auto t1 = clock:: now();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1-t0).count();
        recorder_.record(static_cast<uint64_t>(us));
    }
}