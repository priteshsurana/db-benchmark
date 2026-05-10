#pragma once

#include "experiment/Experiment3.hpp"
#include <string>
#include <chrono>
#include <stdexcept>
#include <cstdlib>
#include <iostream>
#include <algorithm>
#include <random>


//cold primary read

void Experiment3::setup() {
    result_.database		= db_->get_db_name();
    result_.experiment_name	= "exp_03_cold_primary_read";
    result_.operation		= "point_read";
    result_.index_type		= "primary";
    result_.thread_count	= 1;    

    std::vector<Order> sample = sample_random_orders(config_.read_sample_size);
    sample_ids_.reserve(sample.size());
    for (const auto& o: sample) {
        sample_ids_.push_back(o.order_id);
    }

    std::mt19937_64 rng(config_.rng_seed ^ 0xC0FFEE00C0FFEEULL);
    std::shuffle(sample_ids_.begin(), sample_ids_.end(), rng);

    std::cout<< "["<< db_->get_db_name() <<
        "] Flushing caches for cold read ..."<<std::endl;
    std::string container = get_container_name();
    flush_cache(container);

    try {
        db_->connect();
    }
    catch(const std::exception& e) {
        throw std::runtime_error (
            std::string("exp 03 reconnect after flush falied")+e.what());
        
    }
    std::cout<< "["<< db_->get_db_name() <<
        "] Reconnected. Starting cold read ..."<<std::endl;
    
}


void Experiment3::run() {
    using clock = std::chrono::steady_clock;
    for (const auto& order_id :sample_ids_) {
        auto t0 = clock::now();
        Order result = db_->find_by_primary_key(order_id);
        auto t1 = clock::now();

        auto us = std::chrono::duration_cast<
            std::chrono::microseconds>(t1-t0).count();
        recorder_.record(static_cast<uint64_t>(us));
    }
}


void flush_cache(const std::string& container) {
    std::string cmd = "./scripts/flush_cache_mac.sh "+ container;

    int rc = std::system(cmd.c_str());
    if (rc!= 0) {
        throw std::runtime_error(
            "exp 03: flush_cache_mac.sh failed for " + container +
            ". Return code: " + std::to_string(rc) +
            ". Ensure vm is running and in sudo can be performed");
        
    }
}



std::string Experiment3::get_container_name() const {
    const std::string& db  = db_->get_db_name();
    if (db  == "postgres") return "bench_postgres";
    if (db  == "mongo") return "bench_mongo";
    if (db  == "cassandra") return "bench_cassandra";
    throw std::runtime_error("exp 03: unknown db name: "+ db);
}