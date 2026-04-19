#pragma once
#include "db/DbInterface.hpp"
#include "config/ConfigLoader.hpp"
#include <cassandra.h>
#include <memory>
#include <string>

class CassandraClient: public DBInterface {
    public:
    explicit CassandraClient(const DatabaseConfig& config);
    ~CassandraClient() override;

    void connect() override;
    void disconnect() override;
    void insert_one(const Order& order) override;
    void insert_batch(const std::vector<Order>& orders) override;
    void delete_by_primary_key(const std::string& order_id) override;
    Order find_by_primary_key(const std::string& order_id) override;
    std::vector<Order> find_by_secondary_key(const std::string& user_id) override;
    std::vector<Order> find_by_range(const std::string& start_date,
        const std::string& end_date) override;
    void clear_table() override;
    void create_secondary_index() override;
    void drop_secondary_index() override;
    std::string get_db_name() const override;

    private:
    DatabaseConfig config_;
    CassCluster* cluster_ = nullptr;
    CassSession* session_ = nullptr;

    CassPrepared const* insert_by_id_stmt_ = nullptr;
    CassPrepared const* insert_by_user_stmt_ = nullptr;
    CassPrepared const* select_by_id_stmt_ = nullptr;
    CassPrepared const* select_by_user_stmt_ = nullptr;
    CassPrepared const* delete_by_id_stmt_ = nullptr;

    void check_future(CassFuture* future, const std::string& context) const;
    Order row_to_order(const CassRow* row) const;
};