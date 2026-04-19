#pragma once
#include <memory>
#include <pqxx/pqxx>
#include "DbInterface.hpp"
#include "../config/DatabaseConfig.hpp"
#include "../config/BenchmarkConfig.hpp"

class PostgresClient : public DBInterface{
    public:
    explicit PostgresClient(const DatabaseConfig& config);
    ~PostgresClient() override;

    void connect() override;
    void disconnect() override;
    void insert_one(const Order& order) override;
    void insert_batch(const std::vector<Order>& orders) override;
    void delete_by_primary_key(const std::string& order_id) override;
    Order find_by_primary_key(const std::string& order_id) override;
    std::vector<Order> find_by_secondary_key(const std::string& user_id) override;
    std::vector<Order> find_by_range(const std::string& startdate, const std::string& enddate) override;

    void clear_table() override;
    void create_secondary_index() override;
    void drop_secondary_index() override;
    std::string get_db_name() const override;

    private:
    DatabaseConfig config_;
    std::unique_ptr<pqxx::connection> conn_;

    void prepare_statements();
    Order row_to_order(const pqxx::row& row) const;

};