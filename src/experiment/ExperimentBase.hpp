#pragma once

#include <string>
#include <vector>
#include "db/DbInterface.hpp"
#include "data/DataGenerator.hpp"
#include "config/ConfigLoader.hpp"
#include "metrics/LatencyRecorder.hpp"




class ExperimentBase {
    public:
    ExperimentBase(DBInterface* db, const std::vector<Order>* dataset, const BenchmarkConfig& config);
    virtual ~ExperimentBase() = default;
    void execute();
    BenchmarkResult get_result() const;

    protected:
    virtual void setup() = 0;
    virtual void run() = 0;
    virtual void teardown() = 0;
    DBInterface* db_;
    const std::vector<Order>* dataset_;
    const BenchmarkConfig& config_;
    LatencyRecorder recorder_;
    BenchmarkResult result_;

    std::vector<Order> pick_random_orders(uint64_t n) const;
};