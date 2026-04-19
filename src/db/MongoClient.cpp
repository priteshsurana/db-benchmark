#include "db/MongoClient.hpp"
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/json.hpp>
#include <mongocxx/options/insert.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/index.hpp>
#include <sstream>
#include <stdexcept>
#include <iostream>


using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::document;

MongoClient::MongoClient(mongocxx::instance* instance,
    const DatabaseConfig& config): instance_(instance), config_(config){}

MongoClient::~MongoClient() = default;

void MongoClient::connect() {
    std::ostringstream uri_str;
    uri_str <<"mongodb://"
            << config_.username << ":" << config_.password
            << "@" <<config_.host << ":" <<config_.port
            << "/" << config_.database_name
            << "?authSource=admin";
    try {
        mongocxx::uri uri(uri_str.str());
        client_ = std::make_unique<mongocxx::client>(uri);

    } catch (const std::exception& e) {
        throw std::runtime_error(
            std::string("Mongo connection failed: ")+e.what()
        );
    }

    db_ = (*client_)[config_.database_name];
    orders_ = db_["orders"];
}


void MongoClient::disconnect() {
    client_.reset();
}

void MongoClient::insert_one(const Order& order) {
    mongocxx::write_concern wc;
    wc.acknowledge_level(mongocxx::write_concern::level::k_acknowledged);
    wc.journal(false);

    mongocxx::options::insert opts;
    opts.write_concern(wc);
    auto doc = order_to_bson(order);
    orders_.insert_one(doc.view(), opts);

}



void MongoClient::insert_batch(const std::vector<Order>& orders) {
    mongocxx::write_concern wc;
    wc.acknowledge_level(mongocxx::write_concern::level::k_acknowledged);
    wc.journal(false);

    mongocxx::options::insert opts;
    opts.write_concern(wc);
    std::vector<bsoncxx::document::value> docs;
    docs.reserve(orders.size());
    for(const auto& o:orders){
        docs.push_back(order_to_bson(o));
    }

    //for insert many we need object of type view or value
    std::vector<bsoncxx::document::view_or_value> views;
    views.reserve(docs.size());
    for (const auto& d: docs) {
        views.push_back(d.view());
    }

    orders_.insert_many(views, opts);
}


void MongoClient::delete_by_primary_key(const std::string& order_id) {
    auto filter = make_document(kvp("_id", order_id));
    orders_.delete_one(filter.view());
}

Order MongoClient::find_by_primary_key(const std::string& order_id) {
    auto filter = make_document(kvp("_id", order_id));
    auto result = orders_.find_one(filter.view());

    if (!result) return Order{};
    return bson_to_order(result->view());
}


std::vector<Order> MongoClient::find_by_secondary_key(const std::string& user_id) {
    auto filter = make_document(kvp("user_id", user_id));
    auto cursor = orders_.find(filter.view());
    std::vector<Order> orders;
    for(const auto& doc:cursor) {
        orders.push_back(bson_to_order(doc));
    }
    return orders;
}

std::vector<Order> MongoClient::find_by_range(const std::string& start_date,
    const std::string& end_date) {
    auto filter = make_document(
        kvp("created_at", make_document(
            kvp("$gte", start_date),
            kvp("$lte", end_date)
        ))
    );

    mongocxx::options::find opts;
    auto sort = make_document(kvp("created_at", 1));
    opts.sort(sort.view());
    auto cursor = orders_.find(filter.view(), opts);

    std::vector<Order> orders;
    for (const auto& doc:cursor) {
        orders.push_back(bson_to_order(doc));
    }
    return orders;
}


void MongoClient::clear_table() {
    orders_.drop();
    orders_  = db_["orders"];
}

void MongoClient::create_secondary_index() {
    auto user_idx = make_document(kvp("user_id", 1));
    orders_.create_index(user_idx.view());
    auto date_idx = make_document(kvp("created_at", 1));
    orders_.create_index(date_idx.view());
    std::cout<<"[mongodb] Seocndary index created"<<std::endl;
}

void MongoClient::drop_secondary_index() {
    try {orders_.indexes().drop_one("user_id_1");} catch(...){}
    try {orders_.indexes().drop_one("created_at_1");} catch(...){}
    std::cout<< "[mongodb] Secondary index dropped"<<std::endl;
}

std::string MongoClient::get_db_name() const {
    return "mongo";
}



bsoncxx::document::value MongoClient::order_to_bson(const Order& order) const {
    // _id = order_id so primary key lookups hit the _id index directly.
    // Storing UUID as string — no ObjectId conversion needed for benchmarking.
    return make_document(
        kvp("_id",         order.order_id),
        kvp("user_id",     order.user_id),
        kvp("product_id",  order.product_id),
        kvp("status",      order.status),
        kvp("amount",      order.amount),
        kvp("quantity",    order.quantity),
        kvp("description", order.description),
        kvp("created_at",  order.created_at),
        kvp("updated_at",  order.updated_at)
    );
}

Order MongoClient::bson_to_order(const bsoncxx::document::view& doc) const {
    Order o;

    auto get_str = [&](const char* key) -> std::string {
        auto it = doc.find(key);
        if (it == doc.end()) return "";
        return std::string(it->get_string().value);
    };

    o.order_id   = get_str("_id");
    o.user_id    = get_str("user_id");
    o.product_id = get_str("product_id");
    o.status     = get_str("status");
    o.description = get_str("description");
    o.created_at = get_str("created_at");
    o.updated_at = get_str("updated_at");

    auto amount_it = doc.find("amount");
    if (amount_it != doc.end()) {
        o.amount = amount_it->get_double().value;
    }

    auto qty_it = doc.find("quantity");
    if (qty_it != doc.end()) {
        o.quantity = static_cast<int>(qty_it->get_int32().value);
    }

    return o;
}
