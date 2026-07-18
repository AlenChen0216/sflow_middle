#pragma once

#include <cstdint>
#include <string>
#include <map>
#include <tuple>
#include <vector>

extern "C"
{
#include <nfp.h>
#include <nfp_cpp.h>
}

typedef struct counter_key
{
    uint32_t agent_ip : 32;
    uint32_t if_idx : 32;
    bool operator<(const counter_key &agent) const
    {
        return std::tie(agent_ip, if_idx) < std::tie(agent.agent_ip, agent.if_idx);
    }
} counter_key;

typedef struct stored_counter
{
    counter_key key;
    uint64_t in_octets : 64;
    uint64_t out_octets : 64;
    uint64_t if_speed : 64;
} stored_counter;

typedef struct counter_data
{
    stored_counter data;
    uint32_t index : 32;
};

typedef struct CounterAgent{
    uint32_t agent_ip;
    uint32_t if_idx;
    bool operator<(const CounterAgent& agent) const{
        return std::tie(agent_ip, if_idx) < std::tie(agent.agent_ip, agent.if_idx);
    }
} CounterAgent;

typedef struct CounterRecord{
    uint64_t in_octets : 64;
    uint64_t out_octets : 64;
    uint64_t if_speed : 64;
} CounterRecord;

class CounterReader
{
public:
    CounterReader(nfp_cpp *cpp,
                  uint64_t staAddr, uint64_t staIsland,
                  uint64_t proAddr, uint64_t proIsland,
                  uint64_t semAddr0, uint64_t semAddr1,
                  uint64_t dataAddr0, uint64_t dataAddr1,
                  uint32_t semIsland0, uint32_t dataIsland0,
                  uint32_t semIsland1, uint32_t dataIsland1,
                  size_t slotCount);

    std::vector<counter_data> readCounterData() noexcept;

private:
    nfp_cpp *cpp_;

    uint64_t staAddr_;
    uint64_t proAddr_;
    uint64_t semAddr_[2];
    uint64_t dataAddr_[2];

    uint32_t staCppId_;
    uint32_t proCppId_;
    uint32_t semCppId_[2];
    uint32_t dataCppId_[2];

    unsigned long semSize_;
    unsigned long dataSize_;
    size_t slotCount_;
};
