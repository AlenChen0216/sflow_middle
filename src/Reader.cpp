#include "../include/Reader.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <thread>
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

static_assert(sizeof(stored_flow_key) == 32, "stored_flow_key wire size changed");
static_assert(sizeof(mac_time_data) == 8, "mac_time_data wire size changed");
static_assert(sizeof(stored_flow_data) == 32, "stored_flow_data wire size changed");
}  // namespace

SmartNicReader::SmartNicReader(nfp_cpp *cpp,
                               uint64_t staAddr, uint64_t staIsland,
                               uint64_t proAddr, uint64_t proIsland,
                               uint64_t semAddr0, uint64_t semAddr1,
                               uint64_t keyAddr0, uint64_t keyAddr1,
                               uint64_t dataAddr0, uint64_t dataAddr1,
                               uint32_t semIsland0, uint32_t keyIsland0, uint32_t dataIsland0,
                               uint32_t semIsland1, uint32_t keyIsland1, uint32_t dataIsland1,
                               size_t slotCount)
    : cpp_(cpp), slotCount_(slotCount), bufferState_(0)
{
    staAddr_ = staAddr;
    proAddr_ = proAddr;
    semAddr_[0] = semAddr0;
    semAddr_[1] = semAddr1;
    keyAddr_[0] = keyAddr0;
    keyAddr_[1] = keyAddr1;
    dataAddr_[0] = dataAddr0;
    dataAddr_[1] = dataAddr1;


    semSize_ = static_cast<unsigned long>(slotCount_ * sizeof(uint32_t));
    keySize_ = static_cast<unsigned long>(slotCount_ * sizeof(stored_flow_key));
    dataSize_ = static_cast<unsigned long>(slotCount_ * sizeof(stored_flow_data));

    staCppId_ = NFP_CPP_ISLAND_ID(kCppTargetMem, kCppActionWrite, 0, staIsland);
    proCppId_ = NFP_CPP_ISLAND_ID(kCppTargetMem, kCppActionWrite, 0, proIsland);

    semCppId_[0] = NFP_CPP_ISLAND_ID(kCppTargetMem, kCppActionWrite, 0, semIsland0);
    keyCppId_[0] = NFP_CPP_ISLAND_ID(kCppTargetMem, kCppActionWrite, 0, keyIsland0);
    dataCppId_[0] = NFP_CPP_ISLAND_ID(kCppTargetMem, kCppActionWrite, 0, dataIsland0);
    semCppId_[1] = NFP_CPP_ISLAND_ID(kCppTargetMem, kCppActionWrite, 0, semIsland1);
    keyCppId_[1] = NFP_CPP_ISLAND_ID(kCppTargetMem, kCppActionWrite, 0, keyIsland1);
    dataCppId_[1] = NFP_CPP_ISLAND_ID(kCppTargetMem, kCppActionWrite, 0, dataIsland1);
}

std::vector<flow_data> SmartNicReader::readFlowData() noexcept {
    // 1. Read the currently active bank from _cur_state.
    // 2. Switch _cur_state to the other bank.
    // 3. Wait until the old bank has no ME contexts using it.
    // 4. Read key/data from the old bank.
    // 5. Clear key/data first, then clear semaphores last.

    if (!cpp_ || slotCount_ == 0) {
        return {};
    }

    std::vector<flow_data> result;

    const uint64_t staAddr = staAddr_;
    const uint32_t staCppId = staCppId_;
    const uint64_t proAddr = proAddr_;
    const uint32_t proCppId = proCppId_;

    // Step 1: read the current hardware state instead of trusting local state.
    nfp_cpp_area *staArea = nfp_cpp_area_alloc(cpp_, staCppId, staAddr, sizeof(uint32_t));
    if (!staArea) {
        return {};
    }
    if (nfp_cpp_area_acquire(staArea) < 0) {
        nfp_cpp_area_free(staArea);
        return {};
    }

    uint32_t staValue = 0;
    int staBytesRead = nfp_cpp_area_read(staArea, 0, &staValue, sizeof(uint32_t));
    if (staBytesRead != static_cast<int>(sizeof(uint32_t))) {
        nfp_cpp_area_release_free(staArea);
        return {};
    }

    const int ori_bufferState = static_cast<int>(staValue & 1);
    const int next_bufferState = 1 - ori_bufferState;

    const uint64_t semAddr = semAddr_[ori_bufferState];
    const uint32_t semCppId = semCppId_[ori_bufferState];
    const uint64_t keyAddr = keyAddr_[ori_bufferState];
    const uint32_t keyCppId = keyCppId_[ori_bufferState];
    const uint64_t dataAddr = dataAddr_[ori_bufferState];
    const uint32_t dataCppId = dataCppId_[ori_bufferState];

    // Step 2: publish the new active bank.
    staValue = static_cast<uint32_t>(next_bufferState);
    int staBytesWritten =  nfp_cpp_area_write(staArea, 0, &staValue,
                           sizeof(staValue));
    nfp_cpp_area_release_free(staArea);
    if (staBytesWritten != static_cast<int>(sizeof(uint32_t))) {
        return {};
    }
    bufferState_ = next_bufferState;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    // Step 3: wait until the old bank is quiescent.
    nfp_cpp_area *proArea = nfp_cpp_area_alloc(cpp_, proCppId, proAddr, sizeof(uint32_t) * 2);
    if (!proArea) {
        return {};
    }
    if (nfp_cpp_area_acquire(proArea) < 0) {
        nfp_cpp_area_free(proArea);
        return {};
    }
    while (true) {
        uint32_t proValue = 0;
        int proBytesRead = nfp_cpp_area_read(proArea, ori_bufferState * sizeof(uint32_t), &proValue, sizeof(uint32_t));
        if (proBytesRead != static_cast<int>(sizeof(uint32_t))) {
            nfp_cpp_area_release_free(proArea);
            return {};
        }
        if (proValue == 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    nfp_cpp_area_release_free(proArea);

    // Step 4: read the semaphores of the old bank and find the valid data.
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

    std::vector<size_t> haveData;
    for (size_t i = 0; i < slotCount_; ++i) {
        if (semaphores[i] == kSemDataReady) {
            haveData.push_back(i);
        }
    }

    // Step 5: read the valid data and clear the old bank.
    nfp_cpp_area *keyArea = nfp_cpp_area_alloc(cpp_, keyCppId, keyAddr, keySize_);
    if (!keyArea) {
        nfp_cpp_area_release_free(semArea);
        return {};
    }
    if (nfp_cpp_area_acquire(keyArea) < 0) {
        nfp_cpp_area_free(keyArea);
        nfp_cpp_area_release_free(semArea);
        return {};
    }
    nfp_cpp_area *dataArea = nfp_cpp_area_alloc(cpp_, dataCppId, dataAddr, dataSize_);
    if (!dataArea) {
        nfp_cpp_area_release_free(keyArea);
        nfp_cpp_area_release_free(semArea);
        return {};
    }
    if (nfp_cpp_area_acquire(dataArea) < 0) {
        nfp_cpp_area_free(dataArea);
        nfp_cpp_area_release_free(keyArea);
        nfp_cpp_area_release_free(semArea);
        return {};
    }

    for (auto &i : haveData) {
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

    const int keyBytesCleared = nfp_cpp_area_fill(keyArea, 0, kDataClearValue, keySize_);
    const int dataBytesCleared = nfp_cpp_area_fill(dataArea, 0, kDataClearValue, dataSize_);
    const int semBytesCleared = nfp_cpp_area_fill(semArea, 0, kSemUnlockValue, semSize_);

    nfp_cpp_area_release_free(keyArea);
    nfp_cpp_area_release_free(dataArea);
    nfp_cpp_area_release_free(semArea);

    // if (keyBytesCleared != static_cast<int>(keySize_) ||
    //     dataBytesCleared != static_cast<int>(dataSize_) ||
    //     semBytesCleared != static_cast<int>(semSize_)) {
    //     return {};
    // }

    return result;
}
