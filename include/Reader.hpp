#pragma once

#include <cstdint>
#include <string>
#include <map>
#include <tuple>
#include <vector>

extern "C" {
#include <nfp.h>
#include <nfp_cpp.h>
}

struct RegisterSpec {
    std::string name;
    uint64_t offset;
    uint32_t bytes;
    uint32_t buffer_size;
    uint32_t reg_island;
};

typedef struct Tuple{
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t protocol;
    
    bool operator<(const Tuple& tuple) const{
        return std::tie(src_ip, dst_ip, src_port, dst_port, protocol) < 
               std::tie(tuple.src_ip, tuple.dst_ip, tuple.src_port, tuple.dst_port, tuple.protocol);
    }
} Tuple;

typedef struct Agent{
    uint32_t aip;
    uint32_t in_if;
    uint32_t out_if;
} Agent;

typedef struct FlowRecord{
    std::vector<Agent> agent;
    uint64_t frame_length;
    uint32_t sampling_rate;
    uint8_t tcp_flag;
} FlowRecord;

typedef struct CounterAgent{
    uint32_t agent_ip;
    uint32_t if_idx;
    bool operator<(const CounterAgent& agent) const{
        return std::tie(agent_ip, if_idx) < std::tie(agent.agent_ip, agent.if_idx);
    }
} CounterAgent;

typedef struct CounterRecord{
    uint32_t if_speed;
    uint32_t in_octets;
    uint32_t out_octets;
} CounterRecord;

class SmartNicReader {
public:
    explicit SmartNicReader(nfp_cpp *cpp, uint64_t ememBase);

    std::vector<uint8_t> readAndReset(const RegisterSpec &reg) noexcept;

private:
    nfp_cpp *cpp_;
    uint64_t ememBase_;
};
