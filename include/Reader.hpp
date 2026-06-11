#pragma once

#include <cstdint>
#include <string>
#include <map>
#include <tuple>
#include <vector>
#include <iostream>

extern "C" {
#include <nfp.h>
#include <nfp_cpp.h>
}

struct stored_flow_key
{
    uint32_t src_ip :32;
    uint32_t dst_ip :32;
    uint32_t sampling_rate :32;
    uint32_t agent_ip :32;
    uint16_t in_if :16;
    uint16_t out_if :16;
    uint16_t src_port :16;
    uint16_t dst_port :16;
    uint32_t protocol :32;
    uint32_t tcp_flag :32;
};

struct mac_time_data
{
    uint32_t sec :32;
    uint32_t nsec :32;
};

struct stored_flow_data
{
    uint64_t byte_cnt :64;
    uint64_t packet_cnt :64;
    struct mac_time_data start_time;
    struct mac_time_data end_time;
};

struct flow_data
{
    struct stored_flow_key key;
    struct stored_flow_data data;
    uint32_t index;
};

typedef struct Tuple{
    uint32_t src_ip :32;
    uint32_t dst_ip :32;
    uint16_t src_port :16;
    uint16_t dst_port :16;
    uint16_t protocol :16;
    
    bool operator<(const Tuple& tuple) const{
        return std::tie(src_ip, dst_ip, src_port, dst_port, protocol) < 
               std::tie(tuple.src_ip, tuple.dst_ip, tuple.src_port, tuple.dst_port, tuple.protocol);
    }
} Tuple;

typedef struct Agent{
    uint32_t aip :32;
    uint32_t in_if :32;
    uint32_t out_if :32;
    uint64_t byte_cnt :64;
    uint64_t packet_cnt :64;
    uint32_t sampling_rate :32;
    uint16_t tcp_flag :16;
    uint32_t index :32; // Store the slot index for later clearing
} Agent;

typedef struct FlowRecord{
    std::vector<Agent> agent;
    int64_t start_time = std::numeric_limits<int64_t>::max();
    int64_t end_time = std::numeric_limits<int64_t>::min();
    int64_t start_time_ns = std::numeric_limits<int64_t>::max();
    int64_t end_time_ns = std::numeric_limits<int64_t>::min();
} FlowRecord;

class SmartNicReader {
public:
    /**
     * @param cpp       NFP CPP handle
     * @param semAddr0  Address of _global_semaphores (primary buffer)
     * @param semAddr1  Address of _global_semaphores_dup (secondary buffer)
     * @param keyAddr0  Address of __flow_key (primary buffer)
     * @param keyAddr1  Address of __flow_key_dup (secondary buffer)
     * @param dataAddr0 Address of __flow_data (primary buffer)
     * @param dataAddr1 Address of __flow_data_dup (secondary buffer)
     * @param semIsland Island ID for semaphore registers
     * @param keyIsland Island ID for flow key registers
     * @param dataIsland Island ID for flow data registers
     * @param slotCount Number of slots in the array (BUFFER_SIZE)
     */
    SmartNicReader(nfp_cpp *cpp,
                   uint64_t staAddr, uint64_t staIsland,
                   uint64_t proAddr, uint64_t proIsland,
                   uint64_t semAddr0, uint64_t semAddr1,
                   uint64_t keyAddr0, uint64_t keyAddr1,
                   uint64_t dataAddr0, uint64_t dataAddr1,
                   uint32_t semIsland0, uint32_t keyIsland0, uint32_t dataIsland0,
                   uint32_t semIsland1, uint32_t keyIsland1, uint32_t dataIsland1,
                   size_t slotCount);

    /**
     * Read flow data from SmartNIC using the semaphore-guarded double-buffer protocol:
     *   1. Read _global_semaphores
     *   2. Fill _global_semaphores to 7 (lock)
     *   3. For each slot where semaphore == 3, read the __flow_key and __flow_data entries
     *   4. Fill __flow_key and __flow_data to 0
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
    uint64_t keyAddr_[2];
    uint64_t dataAddr_[2];

    // CPP IDs for area operations
    uint32_t semCppId_[2];
    uint32_t keyCppId_[2];
    uint32_t dataCppId_[2];

    uint64_t staAddr_;
    uint64_t proAddr_;

    uint32_t staCppId_;
    uint32_t proCppId_;

    // Sizes
    unsigned long semSize_;
    unsigned long keySize_;
    unsigned long dataSize_;

    const unsigned long flowKeyEntrySize = static_cast<unsigned long>(sizeof(stored_flow_key));
    const unsigned long flowDataEntrySize = static_cast<unsigned long>(sizeof(stored_flow_data));

    // Number of slots
    size_t slotCount_;

    // Current active buffer index (0 or 1), toggled after each read
    int bufferState_;

};
