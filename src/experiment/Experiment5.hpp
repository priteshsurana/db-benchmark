#pragma once
#include "experiment/ExperimentBase.hpp"
#include "metrics/LatencyRecorder.hpp"
#include <vector>

//range scan

class Experiment5 : public ExperimentBase {
    public:
    Experiment5(DBInterface* db, 
        const std::vector<Order>* dataset, 
        const BenchmarkConfig& config);

    std::vector<BenchmarkResult> get_all_results() const;

    protected:
    void setup() override;
    void run() override;
    void teardown() override;
    

    private:
    LatencyRecorder small_recorder_;
    LatencyRecorder medium_recorder_;
    LatencyRecorder large_recorder_;

    uint64_t avg_result_count_small_ = 0;
    uint64_t avg_result_count_medium_ = 0;
    uint64_t avg_result_count_large_ = 0;

    //std::pair<std::string, std::string> random_window(int window_days) const;
};