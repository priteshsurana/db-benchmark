#pragma once
#include <string>
#include <vector>


struct Order {
    std::string order_id; //UUID
    std::string user_id; //UUID
    std::string product_id; //UUID
    std::string status;
    double amount;
    int quantity;
    std::string description;
    std::string created_at; //ISO 8601, e.g. "2026-03-15T10:30:00Z"
    std::string updated_at; //ISO 8601, e.g. "2026-03-15T10:30:00Z"
};

