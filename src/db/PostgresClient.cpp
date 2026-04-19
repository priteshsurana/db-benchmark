#include "db/PostgresClient.hpp"
#include <sstream>
#include <stdexcept>
#include <iostream>

PostgresClient::PostgresClient(const DatabaseConfig& config) : config_(config) {}

PostgresClient::~PostgresClient() {
    if (conn_ && conn_->is_open()) {
        conn_->close();
    }
}

void PostgresClient::connect() {
    std::ostringstream conn_str;
    conn_str <<"host=" <<config_.host
            <<" port=" <<config_.port
            <<" dbname=" <<config_.database_name
            <<" user=" <<config_.username
            <<" password=" <<config_.password
            <<" connect_timeout=10";

    try {
        conn_ = std::make_unique<pqxx::connection>(conn_str.str());
    }
    catch (const pqxx::broken_connection& e) {
        throw std::runtime_error(
            std::string("Postgres connection failed. Error ")+e.what());
        
    }

    prepare_statements();
}


void PostgresClient::disconnect() {
    if(conn_ && conn_->is_open()) {
        conn_ ->close();
    }
}


void PostgresClient::prepare_statements() {
    conn_-> prepare("insert_order", 
    "INSERT INTO orders "
    "(order_id, user_id, product_id, status, amount, "
    " quantity, description, created_at, updated_at) "
    "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)"
    );

    //Primary id
    conn_->prepare("find_by_pk",
    "SELECT order_id, user_id, product_id, status, amount, "
    " quantity, description, created_at, updated_at "
    "FROM orders WHERE order_id = $1"
    );

    //Secondary id
    conn_->prepare("find_by_user",
    "SELECT order_id, user_id, product_id, status, amount, "
    " quantity, description, created_at, updated_at "
    "FROM orders WHERE user_id = $1"
    );

    //Date range
    conn_->prepare("find_by_range",
    "SELECT order_id, user_id, product_id, status, amount, "
    " quantity, description, created_at, updated_at "
    " FROM orders WHERE created_at>= $1 AND created_at <=$2 "
    "ORDER BY created_at"
    );

}

void PostgresClient::insert_one(const Order& order) {
    pqxx::nontransaction ntxn(*conn_);
    ntxn.exec_prepared("insert_order",
    order.order_id, order.user_id, order.product_id,
    order.status, order.amount, order.quantity, order.description,
    order.created_at, order.updated_at
    );
}

void PostgresClient::insert_batch(const std::vector<Order>& orders){
    pqxx::work txn(*conn_);

    auto stream = pqxx:: stream_to::table( txn,
        {"orders"},
        {"order_id", "user_id", "product_id", "status", "amount",
            "quantity", "description", "created_at", "updated_at"}
    );
    for (const auto& o: orders) {
        stream<< std::make_tuple(
            o.order_id, o.user_id, o.product_id, o.status, o.amount, o.quantity,
            o.description, o.created_at, o.updated_at
        );
    };

    stream.complete();
    txn.commit();
}


Order PostgresClient::find_by_primary_key(const std::string& order_id) {
    pqxx::nontransaction ntxn(*conn_);
    pqxx::result r = ntxn.exec_prepared("find_by_pk", order_id);

    if (r.empty()) return Order{};
    return row_to_order(r[0]);

}

std::vector<Order> PostgresClient::find_by_secondary_key(const std::string& user_id) {
    pqxx::nontransaction ntxn(*conn_);
    pqxx::result r = ntxn.exec_prepared("find_by_user", user_id);
        std::vector<Order> orders;
        orders.reserve(r.size());
        for (const auto& row : r) {
            orders.push_back(row_to_order(row));
        }
        return orders;
    }



std::vector<Order> PostgresClient::find_by_range(const std::string& start_date,
    const std::string& end_date) {
    pqxx::nontransaction ntxn(*conn_);
    pqxx::result r = ntxn.exec_prepared("find_by_range", start_date, end_date);
        std::vector<Order> orders;
        orders.reserve(r.size());
        for (const auto& row : r) {
            orders.push_back(row_to_order(row));
        }
        return orders;
    }
