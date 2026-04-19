#pragma once
#include "DatabaseConfig.hpp"
#include "BenchmarkConfig.hpp"

class ConfigLoader {
    public:
    static BenchmarkConfig load(const std::string& path);
};