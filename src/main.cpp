#include <iostream>
#include <memory>
#include <vector>
#include <string>
#include <stdexcept>

#include "config/ConfigLoader.hpp"
#include "data/DataGenerator.hpp"
#include "db/PostgresClient.hpp"

#include "experiment/Experiment1.hpp"

#include "metrics/MetricsCollector.hpp"

//helper functions

static void verify_connections(const std::vector<std::pair<std::string, DBInterface*>>& clients) {
    for(auto& [name, client] : clients) {
        try {
            client->connect();
            std::cout<< "DB "<< name<<" OK"<<std::endl;
        }
        catch (const std::exception& e) {
            std::cerr<< "DB "<<name<< ": "<<e.what()<< "\n";
            throw;
        }
    }
}

static void disconnect_all(const std::vector<std::pair<std::string, DBInterface*>>& clients) {
    for(auto& [name, client]: clients) {
        try {
            client->disconnect(); 
        } catch (...) {}
    }
}

int main(int argc, char* argv[]) {
    const std::string config_path = (argc>1) ? argv[1]: "./config/benchmark.yaml";
    BenchmarkConfig config;
    try {
        config = ConfigLoader::load(config_path);
        std::cout<< "Config loaded from "<<config_path<<std::endl;
    }
    catch(const std::exception& e) {
        std::cerr << "Failed to load benchmark config"<<std::endl;
        return 1;
    }

    auto pg_client = std::make_unique<PostgresClient>(config.postgres);

    std::vector<std::pair<std::string, DBInterface*>> all_clients;
    if (config.postgres.enabled) all_clients.emplace_back("postgres", pg_client.get());

    try {
        verify_connections(all_clients);
    }
    catch (...) {
        return 1;
    }

    //Generate data and hold in mem
    DataGenerator generator(config.rng_seed);
    std::vector<Order> dataset = generator.generate_n(config.totalrows);
    std::cout<<"Data Generated"<<std::endl;

}