#pragma once
#include "experiment/ExperimentBase.hpp"
#include "metrics/LatencyRecorder.hpp"
#include <vector>
#include <array>

//write read symmetry


static constexpr int NUM_RUNS = 3;
class Experiment7 : public ExperimentBase {
    public:
    using ExperimentBase::ExperimentBase;
    void execute();
    std::vector<BenchmarkResult> get_all_run_results() const;
    std::pair<BenchmarkResult, BenchmarkResult> get_averaged_results() const;

    protected:
    void setup() override;
    void run() override;

    private:
    struct RunResult {
        int run_number;
        BenchmarkResult write_result;
        BenchmarkResult read_result;

    };

    std::array<RunResult, NUM_RUNS> run_results_;

    BenchmarkResult avg_write_result_;
    BenchmarkResult avg_read_result_;

    BenchmarkResult write_phase(int run_number, LatencyRecorder& recorder);
    BenchmarkResult read_phase(int run_number, LatencyRecorder& recorder);
    void flush_cache();
    void compute_averages();
    std::vector<std::string> written_order_ids_;

};