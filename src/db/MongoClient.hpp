#pragma once
#include "db/DbInterface.hpp"
#include "config/ConfigLoader.hpp"
#include <mongocxx/client.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/database.hpp>
#include <mongocxx/collection.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <memory>

class MongoClient: public DBInterface{
    public:
    MongoClient(mongocxx::instance* instance, const DatabaseConfig& config);
    ~MongoClient() override;


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
    mongocxx::instance* instance_;
    DatabaseConfig config_;
    std::unique_ptr<mongocxx::client> client_;
    mongocxx::database db_;
    mongocxx::collection orders_;
    bsoncxx::document::value order_to_bson(const Order& order) const;
    Order bson_to_order(const bsoncxx::document::view& doc) const;

};