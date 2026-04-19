#pragma once

#include <string>
#include <cstdint>

struct DatabaseConfig {
    std::string host;
    uint16_t port;
    std::string username;
    std::string password;
    std::string database_name;
    bool enabled = true;
};