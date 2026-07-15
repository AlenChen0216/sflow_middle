#pragma once

#include "Time.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace YAML {
class Node;
}

struct SymbolConfig
{
    std::string semaphore = "_global_semaphores";
    std::string semaphore_dup = "_global_semaphores_dup";
    std::string flow_key = "__flow_key";
    std::string flow_key_dup = "__flow_key_dup";
    std::string flow_data = "__flow_data";
    std::string flow_data_dup = "__flow_data_dup";
    std::string counter_semaphore = "_cglobal_semaphores";
    std::string counter_semaphore_dup = "_cglobal_semaphores_dup";
    std::string counter_data = "__counter_data";
    std::string counter_data_dup = "__counter_data_dup";
    std::string cur_state = "__cur_state";
    std::string processing_me = "__processing_me";
};

struct AppConfig
{
    std::vector<unsigned int> devices;
    bool debug = false;
    int run_secs = 14;
    std::string target_ip = "192.168.1.4";
    std::uint16_t target_port = 6343;
    SymbolConfig symbols;
    TimeAdjuster::Config time;
};

// Throws std::invalid_argument for schema and validation failures, and lets
// yaml-cpp parser/load errors propagate to the caller.
AppConfig parseAppConfig(const YAML::Node &root);
AppConfig loadAppConfig(const std::string &path);
