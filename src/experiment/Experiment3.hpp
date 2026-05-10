#pragma once

#include "experiment/ExperimentBase.hpp"
#include <string>

//cold primary read

class Experiment3: public ExperimentBase {
    public:
    using ExperimentBase::ExperimentBase;

    protected:
    void setup() override;
    void run() override;

    private:
    std::vector<std::string> sample_ids_;
    std::string get_container_name() const;


};