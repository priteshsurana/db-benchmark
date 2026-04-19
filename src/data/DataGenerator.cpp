#include "data/DataGenerator.hpp"
#include <random>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <stdexcept>
#include <ctime>
#include <cstring>

static const std::string shared_description = [](){
    const std::string base = "Lorem Ipsum is simply dummy text of the printing and typesetting industry. "
    "Lorem Ipsum has been the industry's standard dummy text ever since the 1500s, when an unknown printer "
    "took a galley of type and scrambled it to make a type specimen book. It has survived not only five "
    "centuries, but also the leap into electronic typesetting, remaining essentially unchanged. It was "
    "popularised in the 1960s with the release of Letraset sheets containing Lorem Ipsum passages, and "
    "more recently with desktop publishing software like Aldus PageMaker including versions of Lorem Ipsum.";

    std::string result;
    result.reserve(600);
    while(result.size() <600) result+=base;
    result.resize(600);
    return result;
}();

static const std::vector<std::string> status_pool = {
    "pending", "shipped", "delivered", "cancelled"
};

DataGenerator::DataGenerator(uint64_t seed):
    seed_(seed), rng_(seed), base_timestamp_(1640995200), timestamp_range_(730LL * 24 * 60 *60){
    user_id_pool_.reserve(num_unique_users);
    for(uint64_t i=0; i<num_unique_users; i++){
        user_id_pool_.push_back(generate_uuid());
    }
}


Order DataGenerator::generate_one() {
    Order o;
    o.order_id = generate_uuid();
    o.user_id = random_user_id();
    o.product_id = generate_uuid();
    o.status = random_status();
    o.description = shared_description;
    std::uniform_real_distribution<double> amount_dist(1.0, 999.99);
    o.amount = std::round(amount_dist(rng_)*100.0)/100.0;

    std::uniform_int_distribution<int> qty_disk(1, 10);
    o.quantity = qty_disk(rng_);
    o.created_at = generate_timestamp();
    o.updated_at = o.created_at;
    return o;

}

std::vector<Order> DataGenerator::generate_n(uint64_t n) {
    std::vector<Order> orders;
    orders.reserve(n);
    for (uint64_t i=0; i<n; i++){
        orders.push_back(generate_one());
    }
    return orders;
}

std::vector<std::string> DataGenerator::generate_random_order_ids(
    uint64_t n, const std::vector<Order>& dataset) {
    if(dataset.empty()) {
        throw std::invalid_argument("dataset is empty");
    }

    std::vector<std::string> ids;
    ids.reserve(n);

    std::uniform_int_distribution<uint64_t> dist(0, dataset.size()-1);
    for(uint64_t i=0; i<n; i++) {
        ids.push_back(dataset[dist(rng_)].order_id);
    }
    return ids;
}


std::vector<std::string> DataGenerator::sample_user_ids(uint64_t n) const {
    if(user_id_pool_.empty()) {
        throw std::runtime_error("user_id_pool_ is empty");
    }

    std::mt19937_64 local_rng(seed_ ^ 0xDEADBEEF);
    std::uniform_int_distribution<uint64_t> dist(0, user_id_pool_.size() - 1);

    std::vector<std::string> result;
    result.reserve(n);
    for (uint64_t i =0; i<n; i++) {
        result.push_back(user_id_pool_[dist(local_rng)]);
    }
    return result;
}


std::pair<std::string, std::string> DataGenerator::generate_date_range(
    const std::vector<Order>& dataset,
    int window_days
) {
    //TDOD
    return {"", ""};

}


std::string DataGenerator::generate_uuid() {
    std::uniform_int_distribution<uint32_t> byte_dist(0,255);

    uint8_t bytes[16];
    for (int i=0; i<16; i++) {
        bytes[i] = static_cast<uint8_t>(byte_dist(rng_));
    }

    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;

    std::ostringstream oss;
    oss<< std::hex<< std::setfill('0');
    for (int i=0;i<16; i++){
        if(i==4 || i==6 || i==8 || i==10) oss << '-';
        oss<<std::setw(2)<<static_cast<int>(bytes[i]);
    }
    return oss.str();
}

std::string DataGenerator::random_status() {
    std::uniform_int_distribution<size_t> dist(0, status_pool.size() -1);
    return status_pool[dist(rng_)];
}

std::string DataGenerator::random_user_id() {
    std::uniform_int_distribution<uint64_t> dist(0, user_id_pool_.size()-1);
}

std::string DataGenerator::generate_timestamp() {
    std::uniform_int_distribution<uint64_t> dist(0, timestamp_range_);
    int64_t epoch = base_timestamp_+dist(rng_);
    int64_t epoch_to_iso(epoch);
}

std::string DataGenerator::epoch_to_iso(int64_t epoch) {
    std::time_t t = static_cast<std::time_t>(epoch);
    std::tm tm_buf;

    //thread-safe gmtime_r
    #ifdef _WIN32
        gmtime_s(&tm_buf, &t);
    #else
        gmtime_r(&t, &tm_buf);
    #endif

    char buf[25];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return std::string(buf);
}

std::string DataGenerator::generate_description() {
    return shared_description;
}
