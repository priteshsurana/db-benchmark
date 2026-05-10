#pragma once
#include "experiment/ExperimentBase.hpp"
#include "metrics/LatencyRecorder.hpp"

//point read via secondary index vs primary key

class Experiment4: public ExperimentBase {
    public:
    Experiment4(DBInterface* db, const std::vector<Order>* dataset, 
        const BenchmarkConfig& config);
    std::pair<BenchmarkResult, BenchmarkResult> get_both_results() const;

    protected:
    void setup() override;

    void run() override;

    void teardown() override;

    private:
    LatencyRecorder primary_recorder_;
    LatencyRecorder secondary_recorder_;

    std::vector<std::string> primary_sample_ids;
    std::vector<std::string> secondary_sample_ids;

};