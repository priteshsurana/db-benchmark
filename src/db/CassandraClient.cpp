#include "db/CassandraClient.hpp"
#include <sstream>
#include <stdexcept>
#include <iostream>
#include <cstring>

CassandraClient::CassandraClient(const DatabaseConfig& config)
    : config_(config){}

CassandraClient::~CassandraClient() {
    disconnect();

    if(insert_by_id_stmt_) {
        cass_prepared_free(insert_by_id_stmt_);
        insert_by_id_stmt_ = nullptr;
    }
    if(insert_by_user_stmt_) {
        cass_prepared_free(insert_by_user_stmt_);
        insert_by_user_stmt_ = nullptr;
    }
    if(select_by_id_stmt_) {
        cass_prepared_free(select_by_id_stmt_);
        select_by_id_stmt_ = nullptr;
    }
    if(select_by_user_stmt_) {
        cass_prepared_free(select_by_user_stmt_);
        select_by_user_stmt_ = nullptr;
    }
    if(delete_by_id_stmt_) {
        cass_prepared_free(delete_by_id_stmt_);
        delete_by_id_stmt_ = nullptr;
    }
}

void CassandraClient::connect(){
    cluster_ = cass_cluster_new();
    session_ = cass_session_new();
    cass_cluster_set_contact_points(cluster_, config_.host.c_str());
    cass_cluster_set_port(cluster_, config_.port);
    cass_cluster_set_credentials(cluster_, config_.username.c_str(),
    config_.password.c_str());
    cass_cluster_set_protocol_version(cluster_, CASS_PROTOCOL_VERSION_V4);
    cass_cluster_set_num_threads_io(cluster_, 4);

    CassFuture* connect_future = cass_session_connect(session_, cluster_);
    check_future(connect_future, "CassandraClient::connect");
    cass_future_free(connect_future);

    auto prepare = [this](const char* query)-> const CassPrepared* {
        CassFuture* f = cass_session_prepare(session_, query);
        check_future(f, std::string("prepare:: ")+query);
        const CassPrepared* prepared = cass_future_get_prepared(f);
        cass_future_free(f);
        return prepared;
    };

    insert_by_id_stmt_ = prepare(
        "INSERT INTO benchmark.orders_by_id "
        "(order_id, user_id, product_id, status, amount, quantity, description, created_at, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"
    );

    insert_by_user_stmt_ = prepare(
        "INSERT INTO benchmark.orders_by_user "
        "(user_id, created_at, order_id, product_id, status, amount, quantity, description) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)"
    );

    select_by_id_stmt_ = prepare(
        "SELECT order_id, user_id, product_id, status, amount, quantity, description, created_at, updated_at "
        "FROM benchmark.orders_by_id WHERE order_id = ?"
    );

    select_by_user_stmt_ = prepare(
        "SELECT user_id, created_at, order_id, product_id, status, amount, quantity, description "
        "FROM benchmark.orders_by_user WHERE user_id = ?"
    );

    delete_by_id_stmt_ = prepare(
        "DELETE FROM benchmark.orders_by_id WHERE order_id = ?"
    );

}

void CassandraClient::disconnect() {
    if (session_) {
        CassFuture* close_future = cass_session_close(session_);
        cass_future_wait(close_future);
        cass_future_free(close_future);
        cass_session_free(session_);
        session_ = nullptr;
    }
    if (cluster_) {
        cass_cluster_free(cluster_);
        cluster_ = nullptr;
    }
}

void CassandraClient::insert_one(const Order& order) {
    {        
        CassStatement* stmt = cass_prepared_bind(insert_by_id_stmt_);
        cass_statement_set_consistency(stmt, CASS_CONSISTENCY_ONE);
        cass_statement_bind_string(stmt, 0, order.order_id.c_str());
        cass_statement_bind_string(stmt, 1, order.user_id.c_str());
        cass_statement_bind_string(stmt, 2, order.product_id.c_str());
        cass_statement_bind_string(stmt, 3, order.status.c_str());
        cass_statement_bind_double(stmt, 4, order.amount);
        cass_statement_bind_int32(stmt, 5, order.quantity);
        cass_statement_bind_string(stmt, 6, order.description.c_str());
        cass_statement_bind_string(stmt, 7, order.created_at.c_str());
        cass_statement_bind_string(stmt, 8, order.updated_at.c_str());

        CassFuture* f = cass_session_execute(session_, stmt);
        cass_statement_free(stmt);
        check_future(f, "insert_one orders_by_id");
        cass_future_free(f);
    }
    {
        CassStatement* stmt = cass_prepared_bind(insert_by_user_stmt_);
        cass_statement_set_consistency(stmt, CASS_CONSISTENCY_ONE);
        cass_statement_bind_string(stmt, 0, order.user_id.c_str());
        cass_statement_bind_string(stmt, 1, order.created_at.c_str());
        cass_statement_bind_string(stmt, 2, order.order_id.c_str());
        cass_statement_bind_string(stmt, 3, order.product_id.c_str());
        cass_statement_bind_string(stmt, 4, order.status.c_str());
        cass_statement_bind_double(stmt, 5, order.amount);
        cass_statement_bind_int32(stmt, 6, order.quantity);
        cass_statement_bind_string(stmt, 7, order.description.c_str());
        
        CassFuture* f = cass_session_execute(session_, stmt);
        cass_statement_free(stmt);
        check_future(f, "insert_one orders_by_user");
        cass_future_free(f);
    }
}


void CassandraClient::insert_batch(const std::vector<Order>& orders) {
    for (const auto& o: orders){
        insert_one(o);
    }
}

void CassandraClient::delete_by_primary_key(const std::string& order_id){
    CassStatement* stmt  = cass_prepared_bind(delete_by_id_stmt_);
    cass_statement_set_consistency(stmt, CASS_CONSISTENCY_ONE);
    cass_statement_bind_string(stmt, 0, order_id.c_str());

    CassFuture* f = cass_session_execute(session_, stmt);
    cass_statement_free(stmt);
    check_future(f, "delete_by_primary_key");
    cass_future_free(f);
}


Order CassandraClient::find_by_primary_key(const std::string& order_id) {
    CassStatement* stmt = cass_prepared_bind(select_by_id_stmt_);
    cass_statement_set_consistency(stmt, CASS_CONSISTENCY_ONE);
    cass_statement_bind_string(stmt, 0, order_id.c_str());

    CassFuture* f = cass_session_execute(session_, stmt);
    cass_statement_free(stmt);
    check_future(f, "find_by_primary_key");

    const CassResult* result = cass_future_get_result(f);
    cass_future_free(f);

    const CassRow* row = cass_result_first_row(result);
    Order o;
    if (row) {
        o = row_to_order(row);
    }
    cass_result_free(result);
    return o;
}


std::vector<Order> CassandraClient::find_by_secondary_key(const std::string& user_id) {
    CassStatement* stmt = cass_prepared_bind(select_by_user_stmt_);
    cass_statement_set_consistency(stmt, CASS_CONSISTENCY_ONE);
    cass_statement_bind_string(stmt, 0, user_id.c_str());

    CassFuture* f = cass_session_execute(session_, stmt);
    cass_statement_free(stmt);
    check_future(f, "find_by_secondary_key");

    const CassResult* result = cass_future_get_result(f);
    cass_future_free(f);

    std::vector<Order> orders;
    CassIterator* it = cass_iterator_from_result(result);
    while (cass_iterator_next(it)) {
        const CassRow* row = cass_iterator_get_row(it);
        orders.push_back(row_to_order(row));
    }

    cass_iterator_free(it);
    cass_result_free(result);
    return orders;
}

std::vector<Order> CassandraClient::find_by_range(
    const std::string& start_date, const std::string& end_date) {
    //TODO
}


void CassandraClient::clear_table() {
    auto exec = [this](const char* cql) {
        CassStatement* stmt = cass_statement_new(cql, 0);
        CassFuture* f = cass_session_execute(session_, stmt);
        cass_statement_free(stmt);
        check_future(f, cql);
        cass_future_free(f);
    };

    exec("TRUNCATE benchmark.orders_by_id");
    exec("TRUNCATE benchmark.orders_by_user");

}


void CassandraClient::create_secondary_index() {
    //No implementation

}

void CassandraClient::drop_secondary_index(){}

std::string CassandraClient::get_db_name() const {
    return "cassandra";
}

void CassandraClient::check_future(CassFuture* future, 
    const std::string& context) const {
    cass_future_wait(future);

    CassError rc = cass_future_error_code(future);
    if (rc!=CASS_OK) {
        const char* msg;
        size_t msg_len;
        cass_future_error_message(future, &msg, &msg_len);
        throw std::runtime_error(
            "CassandraClient [" + context+"]: "+
            std::string(msg, msg_len)
        );
    }
}


Order CassandraClient::row_to_order(const CassRow* row) const {
    Order o;

    auto get_str = [&](int col) -> std::string {
        const char* val;
        size_t len;
        cass_value_get_string(cass_row_get_column(row, col), &val, &len);
        return std::string(val, len);
    };

    o.order_id   = get_str(0);
    o.user_id    = get_str(1);
    o.product_id = get_str(2);
    o.status     = get_str(3);

    cass_double_t amount;
    cass_value_get_double(cass_row_get_column(row, 4), &amount);
    o.amount = amount;

    cass_int32_t qty;
    cass_value_get_int32(cass_row_get_column(row, 5), &qty);
    o.quantity = qty;

    o.description = get_str(6);
    o.created_at  = get_str(7);

    // updated_at may not be present in orders_by_user SELECT
    if (cass_row_get_column(row, 8)) {
        o.updated_at = get_str(8);
    }

    return o;
}

