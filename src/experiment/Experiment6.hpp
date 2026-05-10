#pragma once
#include "experiment/ExperimentBase.hpp"
#include "metrics/LatencyRecorder.hpp"
#include <string>
#include <cstdint>

//compaction

class Experiment6: public ExperimentBase {
    public:
    Experiment6(DBInterface* db, const std::vector<Order>* dataset, 
        const BenchmarkConfig& config);

    //results before compaction
    BenchmarkResult get_before_result() const;

    //after compaction
    BenchmarkResult get_after_result() const;

    protected:
    void setup() override;
    void run() override;


    private:
    mutable LatencyRecorder before_recorder_;
    mutable LatencyRecorder after_recorder_;
 
    std::vector<std::string> sample_ids_;       // Same IDs used in both phases
    uint64_t before_sstable_count_ = 0;
    uint64_t after_sstable_count_  = 0;
    bool     should_skip_          = false;     // True if db is not Cassandra

    uint64_t get_sstable_count() const;
 
    // Calls scripts/cassandra_compact.sh via system().
    // Blocks until compaction completes and SSTable count = 1.
    void run_compaction();
    
};
