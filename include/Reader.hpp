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

typedef struct stored_flow_key
{
    uint32_t src_ip : 32;
    uint32_t dst_ip : 32;
    uint32_t sampling_rate : 32;
    uint32_t agent_ip : 32;
    uint16_t in_if : 16;
    uint16_t out_if : 16;
    uint16_t src_port : 16;
    uint16_t dst_port : 16;
    uint16_t protocol : 16;
    uint16_t tcp_flag : 16;
};

typedef struct flow_data
{
    struct stored_flow_key key;
    uint32_t frame_length : 32;
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
    uint64_t frame_length;
    uint32_t sampling_rate;
    uint8_t tcp_flag;
} Agent;

typedef struct FlowRecord{
    std::vector<Agent> agent;
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
    /**
     * @param cpp       NFP CPP handle
     * @param semAddr0  Address of _global_semaphores (primary buffer)
     * @param semAddr1  Address of _global_semaphores_dup (secondary buffer)
     * @param dataAddr0 Address of __flow_data (primary buffer)
     * @param dataAddr1 Address of __flow_data_dup (secondary buffer)
     * @param semIsland Island ID for semaphore registers
     * @param dataIsland Island ID for flow data registers
     * @param slotCount Number of slots in the array (BUFFER_SIZE)
     */
    SmartNicReader(nfp_cpp *cpp,
                   uint64_t semAddr0, uint64_t semAddr1,
                   uint64_t dataAddr0, uint64_t dataAddr1,
                   uint32_t semIsland, uint32_t dataIsland,
                   size_t slotCount);

    /**
     * Read flow data from SmartNIC using the semaphore-guarded double-buffer protocol:
     *   1. Read _global_semaphores
     *   2. Fill _global_semaphores to 4 (lock)
     *   3. For each slot where semaphore == 3, read the flow_data entry
     *   4. Fill that flow_data slot to 0
     *   5. Fill _global_semaphores to 0 (unlock)
     *   6. Toggle the active buffer
     *
     * @return Vector of flow_data entries that had valid data (semaphore == 3)
     */
    std::vector<flow_data> readFlowData() noexcept;

private:
    nfp_cpp *cpp_;

    // Addresses for double buffers: [0] = primary, [1] = dup
    uint64_t semAddr_[2];
    uint64_t dataAddr_[2];

    // CPP IDs for area operations
    uint32_t semCppId_;
    uint32_t dataCppId_;

    // Sizes
    unsigned long semSize_;
    unsigned long dataSize_;

    // Number of slots
    size_t slotCount_;

    // Current active buffer index (0 or 1), toggled after each read
    int bufferState_;
};
