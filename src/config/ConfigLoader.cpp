#include "config/ConfigLoader.hpp"

#include <yaml-cpp/yaml.h>
#include <stdexcept>
#include <string>


template<typename T>
static T get_or(const YAML::Node& node, const std::string& key, T default_val) {
    if(node[key]) return node[key].as<T>();
    return default_val;

}


static DatabaseConfig parse_db_config(const YAML::Node& node, 
    const std::string& default_host, uint16_t default_port, 
    const std::string& default_user, const std::string& default_pass) {
    DatabaseConfig cfg;
    if(!node) {
        cfg.host =     "default_host";
        cfg.port =     1234;
        cfg.username = "default_user";
        cfg.password = "default_pass";
        cfg.database_name = "benchmark";
        cfg.enabled = true;
        return cfg;
    }


    cfg.host =    get_or<std::string>(node, "host", default_host);
    cfg.port =     get_or<uint16_t>(node, "port", default_port);
    cfg.username = get_or<std::string>(node, "username", default_user);
    cfg.password = get_or<std::string>(node, "password", default_pass);
    cfg.database_name = get_or<std::string>(node, "database_name", "benchmark");
    cfg.enabled = get_or<bool>(node, "enabled", true);
    return cfg;
}

BenchmarkConfig ConfigLoader::load(const std::string& path) {
    YAML::Node yaml;
    try {
        yaml = YAML::LoadFile(path);
    }
    catch (const YAML::BadFile& e) {
        throw std::runtime_error("ConfigLoader cannot open config file: "+path+"\n"+e.what());
    }
    catch(const YAML::ParserException& e) {
        throw std::runtime_error("ConfigLoader YAML parse error inn "+path+"\n"+e.what());
    }
    BenchmarkConfig config;

    config.totalrows = get_or<uint64_t>(yaml, "total_rows", 1'000'000ULL);
    config.read_sample_size = get_or<uint64_t>(yaml, "read_sample_size", 10'000ULL);
    config.batch_size = get_or<uint32_t>(yaml, "batch_size", 500U);
    config.thread_count = get_or<uint32_t>(yaml, "thread_count", 8U);
    config.description_length = get_or<uint32_t>(yaml, "description_length", 500U);
    config.rng_seed = get_or<uint64_t>(yaml, "rng_seed", 42ULL);

    config.range_window_samll = get_or<int>(yaml, "range_window_small", 1);
    config.range_window_medium = get_or<int>(yaml, "range_window_medium", 7);
    config.range_window_large = get_or<int>(yaml, "range_window_large", 30);
    
    config.results_dir = get_or<std::string>(yaml, "results_dir", "./results");

    config.postgres = parse_db_config(yaml["postgres"], "localhost", 5432, "bench", "bench");
    config.mongo = parse_db_config(yaml["mongo"], "localhost", 27017, "bench", "bench");
    config.cassandra = parse_db_config(yaml["cassandra"], "localhost", 9042, "cassandra", "cassandra");

    //experiement flags
    //TODO

    return config;
}