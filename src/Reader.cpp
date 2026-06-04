#include "../include/Reader.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <limits>
#include <vector>
#include <iostream>

namespace {
constexpr uint32_t kCppTargetMem = 7;
constexpr uint32_t kCppActionRead = 0;
constexpr uint32_t kCppActionWrite = 32;
constexpr uint32_t kSemLockValue = 7;
constexpr uint32_t kSemUnlockValue = 0;
constexpr uint32_t kDataClearValue = 0;
constexpr uint32_t kSemDataReady = 3;

static_assert(sizeof(stored_flow_key) == 28, "stored_flow_key wire size changed");
static_assert(sizeof(mac_time_data) == 8, "mac_time_data wire size changed");
static_assert(sizeof(stored_flow_data) == 24, "stored_flow_data wire size changed");
}  // namespace

SmartNicReader::SmartNicReader(nfp_cpp *cpp,
                               uint64_t semAddr0, uint64_t semAddr1,
                               uint64_t keyAddr0, uint64_t keyAddr1,
                               uint64_t dataAddr0, uint64_t dataAddr1,
                               uint32_t semIsland0, uint32_t keyIsland0, uint32_t dataIsland0,
                               uint32_t semIsland1, uint32_t keyIsland1, uint32_t dataIsland1,
                               size_t slotCount)
    : cpp_(cpp), slotCount_(slotCount), bufferState_(0)
{
    semAddr_[0] = semAddr0;
    semAddr_[1] = semAddr1;
    keyAddr_[0] = keyAddr0;
    keyAddr_[1] = keyAddr1;
    dataAddr_[0] = dataAddr0;
    dataAddr_[1] = dataAddr1;

    semSize_ = static_cast<unsigned long>(slotCount_ * sizeof(uint32_t));
    keySize_ = static_cast<unsigned long>(slotCount_ * sizeof(stored_flow_key));
    dataSize_ = static_cast<unsigned long>(slotCount_ * sizeof(stored_flow_data));

    semCppId_[0] = NFP_CPP_ISLAND_ID(kCppTargetMem, kCppActionWrite, 0, semIsland0);
    keyCppId_[0] = NFP_CPP_ISLAND_ID(kCppTargetMem, kCppActionWrite, 0, keyIsland0);
    dataCppId_[0] = NFP_CPP_ISLAND_ID(kCppTargetMem, kCppActionWrite, 0, dataIsland0);
    semCppId_[1] = NFP_CPP_ISLAND_ID(kCppTargetMem, kCppActionWrite, 0, semIsland1);
    keyCppId_[1] = NFP_CPP_ISLAND_ID(kCppTargetMem, kCppActionWrite, 0, keyIsland1);
    dataCppId_[1] = NFP_CPP_ISLAND_ID(kCppTargetMem, kCppActionWrite, 0, dataIsland1);
}

std::vector<flow_data> SmartNicReader::readFlowData() noexcept {
    if (!cpp_ || slotCount_ == 0) {
        return {};
    }

    std::vector<flow_data> result;

    // -------------------------------------------------------------------------
    // Step 1: Read the _global_semaphores of the active buffer
    // -------------------------------------------------------------------------
    std::cout<< "Current stat : "<< bufferState_ << std::endl;
    const uint64_t semAddr = semAddr_[bufferState_];
    const uint32_t semCppId = semCppId_[bufferState_];
    auto unlockSemaphores = [&]() {
        nfp_cpp_area *unlockArea = nfp_cpp_area_alloc(cpp_, semCppId, semAddr, semSize_);
        if (unlockArea) {
            if (nfp_cpp_area_acquire(unlockArea) >= 0) {
                nfp_cpp_area_fill(unlockArea, 0, kSemUnlockValue, semSize_);
            }
            nfp_cpp_area_release_free(unlockArea);
        }
    };

    nfp_cpp_area *semArea = nfp_cpp_area_alloc(cpp_, semCppId, semAddr, semSize_);
    if (!semArea) {
        return {};
    }
    if (nfp_cpp_area_acquire(semArea) < 0) {
        nfp_cpp_area_free(semArea);
        return {};
    }

    std::vector<uint32_t> semaphores(slotCount_, 0);
    int bytesRead = nfp_cpp_area_read(semArea, 0, semaphores.data(), semSize_);
    if (bytesRead != static_cast<int>(semSize_)) {
        nfp_cpp_area_release_free(semArea);
        return {};
    }

    // -------------------------------------------------------------------------
    // Step 2: Fill the _global_semaphores to 7 (lock — tell SmartNIC we are reading)
    // -------------------------------------------------------------------------
    nfp_cpp_area_fill(semArea, 0, kSemLockValue, semSize_);
    nfp_cpp_area_release_free(semArea);
    
    std::vector<size_t> haveData;
    for(size_t i=0;i<slotCount_;++i) {
        if(semaphores[i] == kSemDataReady) {
            haveData.push_back(i);
        }
    }
    // -------------------------------------------------------------------------
    // Step 3 & 4: Read __flow_key and __flow_data for slots where semaphore == 3,
    //             then fill both areas to 0.
    // -------------------------------------------------------------------------
    const uint64_t keyAddr = keyAddr_[bufferState_];
    const uint32_t keyCppId = keyCppId_[bufferState_];
    const uint64_t dataAddr = dataAddr_[bufferState_];
    const uint32_t dataCppId = dataCppId_[bufferState_];

    nfp_cpp_area *keyArea = nfp_cpp_area_alloc(cpp_, keyCppId, keyAddr, keySize_);
    if (!keyArea) {
        unlockSemaphores();
        return {};
    }
    if (nfp_cpp_area_acquire(keyArea) < 0) {
        nfp_cpp_area_free(keyArea);
        unlockSemaphores();
        return {};
    }

    nfp_cpp_area *dataArea = nfp_cpp_area_alloc(cpp_, dataCppId, dataAddr, dataSize_);
    if (!dataArea) {
        nfp_cpp_area_release_free(keyArea);
        unlockSemaphores();
        return {};
    }
    if (nfp_cpp_area_acquire(dataArea) < 0) {
        nfp_cpp_area_free(dataArea);
        nfp_cpp_area_release_free(keyArea);
        unlockSemaphores();
        return {};
    }

    const unsigned long flowKeyEntrySize = static_cast<unsigned long>(sizeof(stored_flow_key));
    const unsigned long flowDataEntrySize = static_cast<unsigned long>(sizeof(stored_flow_data));

    for(auto &i : haveData){
        flow_data entry{};
        int keyBytesRead = nfp_cpp_area_read(keyArea, i * flowKeyEntrySize, &entry.key, sizeof(stored_flow_key));
        if (keyBytesRead != static_cast<int>(sizeof(stored_flow_key))) {
            continue; // Skip this entry on read error
        }
        int dataBytesRead = nfp_cpp_area_read(dataArea, i * flowDataEntrySize, &entry.data, sizeof(stored_flow_data));
        if (dataBytesRead != static_cast<int>(sizeof(stored_flow_data))) {
            continue; // Skip this entry on read error
        }
        entry.index = i;
        result.push_back(entry);
    }
    nfp_cpp_area_fill(keyArea, 0, kDataClearValue, keySize_); // Clear entire area to be safe
    nfp_cpp_area_fill(dataArea, 0, kDataClearValue, dataSize_); // Clear entire area to be safe
    nfp_cpp_area_release_free(keyArea);
    nfp_cpp_area_release_free(dataArea);

    // -------------------------------------------------------------------------
    // Step 5: Fill the _global_semaphores to 0 (unlock — tell SmartNIC we are done)
    // -------------------------------------------------------------------------
    unlockSemaphores();

    // -------------------------------------------------------------------------
    // Step 6: Toggle the active buffer for the next read
    // -------------------------------------------------------------------------
    bufferState_ = 1 - bufferState_;

    return result;
}
