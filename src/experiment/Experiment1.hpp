#pragma once

#include "experiment/ExperimentBase.hpp"

class Experiment1 : public ExperimentBase {
    public:
    using ExperimentBase::ExperimentBase;

    protected:
    void setup() override;
    void run() override;
};