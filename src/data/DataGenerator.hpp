#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "../data/Order.hpp"

class DataGenerator {
    public:
        explicit DataGenerator(uint64_t seed = 42);
        Order generate_one();
        std::vector<Order> generate_n(uint64_t n);
        std::vector<std::string> generate_random_order_ids(uint64_t n, const std::vector<Order>& dataset);
        std::vector<std::string> sample_user_ids(uint64_t n) const;
        std::pair<std::string, std::string> DataGenerator::generate_date_range(
            const std::vector<Order>& dataset, int window_days
        );

    private:
        std::string generate_description();
        std::string random_status();

        std::string generate_uuid();
        std::string generate_timestamp();
        uint64_t seed_;

        static constexpr uint64_t num_unique_users = 100'000;
        std::mt19937_64 rng_;
        std::vector<std::string> user_id_pool_;
        int64_t base_timestamp_;
        int64_t timestamp_range_;

        std::string random_user_id();
        std::string epoch_to_iso(int64_t epoch);
};