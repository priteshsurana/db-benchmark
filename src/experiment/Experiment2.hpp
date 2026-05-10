#pragma once
#include "experiment/ExperimentBase.hpp"
#include "metrics/LatencyRecorder.hpp"
#include <vector>
#include <thread>
#include <functional>


//multithreading concurrent insert
// write throughput under concurrent load how will each storage enginer handle multiple writes simultaneiouly
// my expectations:


class Experiment2: public ExperimentBase {
    public:
    using ConnectionFactory = std::function<DBInterface*(int)>;
    Experiment2 (DBInterface* db, const std::vector<Order>* dataset,
        const BenchmarkConfig& config,
        ConnectionFactory connection_factory);
    
    protected:
    // calling db cleartable to start clean
    void setup() override;

    void run() override;
    void teardown() override;

    private:
    ConnectionFactory connection_factory_;
    std::vector<DBInterface*> thread_connections_;
    std::vector<LatencyRecorder> thread_recorders_;
    

};