#pragma once
#include "experiment/Experiment4.hpp"
#include "data/DataGenerator.hpp"
#include <chrono>
#include <algorithm>
#include <random>
#include <stdexcept>
#include <iostream>



//point read via secondary index vs primary key

Experiment4::Experiment4(
    DBInterface*    db,
    const std::vector<Order>* dataset,
    const BenchmarkConfig& config)
    : ExperimentBase(db, dataset, config),
    primary_recorder_(config.read_sample_size),
    secondary_recorder_(config.read_sample_size){}

void Experiment4::setup() {
    result_.database        = db_->get_db_name();
    result_.experiment_name = "exp4 secondary index read";
    result_.thread_count = 1;

    //sample the primary key. these are already there in db as part of exp 1
    
    std::vector<Order> sample = sample_random_orders(config_.read_sample_size);
    primary_sample_ids.reserve(sample.size());
    for (const auto& o:sample){
        primary_sample_ids.push_back(o.order_id);
    }
    DataGenerator gen(config_.rng_seed);
    secondary_sample_ids = gen.sample_user_ids(config_.read_sample_size);
    
    std::mt19937_64 rng(config_.rng_seed ^ 0xABCDEF1234567890ULL);
    std::shuffle(primary_sample_ids.begin(), primary_sample_ids.end(), rng);
    std::shuffle(secondary_sample_ids.begin(), secondary_sample_ids.end(), rng);

    std::cout<< "["<< db_->get_db_name() <<
        "] Building secondary indexes ..."<<std::endl;
    db_->create_secondary_index();
    std::cout<< "["<< db_->get_db_name() <<
        "] Secondary index created. Starting experiment ..."<<std::endl;
}


void Experiment4::run() {
    using clock = std::chrono::steady_clock;
    std::cout<< "["<< db_->get_db_name() <<
        "] Running primary key reads ..."<<std::endl;
    primary_recorder_.start_timer();
    for (const auto& order_id : primary_sample_ids) {
        auto t0 = clock::now();
        Order result = db_->find_by_primary_key(order_id);
        auto t1 = clock::now();

        auto us = std::chrono::duration_cast<
            std::chrono::microseconds>(t1-t0).count();
        primary_recorder_.record(static_cast<uint64_t>(us));
    }
    primary_recorder_.stop_timer();

    std::cout<< "["<< db_->get_db_name() <<
        "] Running secondary key reads ..."<<std::endl;
    secondary_recorder_.start_timer();

    for (const auto& user_id: secondary_sample_ids) {
        auto t0 = clock::now();
        std::vector<Order> results = db_->find_by_secondary_key(user_id);
        auto t1 = clock::now();

        auto us = std::chrono::duration_cast<
            std::chrono::microseconds>(t1-t0).count();
        secondary_recorder_.record(static_cast<uint64_t>(us));
    }
    secondary_recorder_.stop_timer();
}

void Experiment4::teardown() {
    std::cout<< "["<< db_->get_db_name() <<
        "] Dropping secondary index ..."<<std::endl;
    db_->drop_secondary_index();
}

std::pair<BenchmarkResult, BenchmarkResult> Experiment4::get_both_results() const {
    BenchmarkResult primary_stats = primary_recorder_.compute_stats();
    BenchmarkResult secondary_stats = secondary_recorder_.compute_stats();

    primary_stats.database = db_->get_db_name();
    primary_stats.experiment_name = "exp4 secondary index read";
    primary_stats.operation = "point_read";
    primary_stats.index_type = "primary";
    primary_stats.thread_count = 1;

    secondary_stats.database = db_->get_db_name();
    secondary_stats.experiment_name = "exp4 secondary index read";
    secondary_stats.operation = "point_read";
    secondary_stats.index_type = "secondary";
    secondary_stats.thread_count = 1;

    if (db_->get_db_name() == "cassandra") {
        secondary_stats.notes = 
        "Cassandra secondary read hits orders_by_user table "
        "primary key lookup on denormalzed table. "
        "Fast read - write amplication paid at insert time.";
    }

    return {primary_stats, secondary_stats};
}