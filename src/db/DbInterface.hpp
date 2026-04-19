#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include "../data/Order.hpp"


class DBInterface {
    public:
    virtual ~DBInterface() = default;

    virtual void connect() = 0;
    virtual void disconnect() = 0;
    virtual void insert_one(const Order& order)=0;
    virtual void insert_batch(const std::vector<Order>& orders) =0;
    virtual void delete_by_primary_key(const std::string& order_id) = 0;
    virtual Order find_by_primary_key(const std::string& order_id) = 0;
    virtual std::vector<Order> find_by_secondary_key(const std::string& user_id)=0;
    virtual std::vector<Order> find_by_range(const std::string& start_date,
        const std::string& end_date);
    //Some utility function
    virtual void clear_table()=0;
    virtual void create_secondary_index() =0;
    virtual void drop_secondary_index()=0;
    virtual std::string get_db_name() const = 0;
};

