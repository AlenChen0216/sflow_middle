#include "../include/CounterReader.hpp"


namespace {
constexpr uint32_t kCppTargetMem = 7;
constexpr uint32_t kCppActionRead = 0;
constexpr uint32_t kCppActionWrite = 32;;
constexpr uint32_t kSemUnlockValue = 0;
constexpr uint32_t kDataClearValue = 0;
constexpr uint32_t kSemDataReady = 3;
}  // namespace

CounterReader::CounterReader(nfp_cpp *cpp,
                               uint64_t semAddr0,
                               uint64_t dataAddr0,
                               uint32_t semIsland0, uint32_t dataIsland0,
                               size_t slotCount)
    : cpp_(cpp), slotCount_(slotCount)
{
    semAddr_ = semAddr0;
    dataAddr_ = dataAddr0;

    semSize_ = static_cast<unsigned long>(slotCount_ * sizeof(uint32_t));
    dataSize_ = static_cast<unsigned long>(slotCount_ * sizeof(counter_data));

    semCppId_ = NFP_CPP_ISLAND_ID(kCppTargetMem, kCppActionWrite, 0, semIsland0);
    dataCppId_ = NFP_CPP_ISLAND_ID(kCppTargetMem, kCppActionWrite, 0, dataIsland0);
}

std::vector<counter_data> CounterReader::readCounterData() noexcept{
    if(!cpp_ || slotCount_ == 0) {
        return {};
    }
    std::vector<counter_data> result;

    // Step 1: Read the _cglobal_semaphores
    nfp_cpp_area *semArea = nfp_cpp_area_alloc(cpp_, semCppId_, semAddr_, semSize_);
    if (!semArea) {
        return {};
    }
    if(nfp_cpp_area_acquire(semArea) < 0) {
        nfp_cpp_area_free(semArea);
        return {};
    }
    std::vector<uint32_t> semaphores(slotCount_, 0);
    int readResult = nfp_cpp_area_read(semArea, 0, semaphores.data(), semSize_);
    if (readResult != static_cast<int>(semSize_)) {
        nfp_cpp_area_release_free(semArea);
        return {};
    }
    nfp_cpp_area_fill(semArea, 0, kSemUnlockValue, semSize_); // Unlock semaphores immediately after reading
    nfp_cpp_area_release_free(semArea);

    // Step 2: Read counter data for slots where semaphore == 3
    std::vector<uint32_t> haveData;
    for(int i=0;i<slotCount_;++i) {
        if(semaphores[i] == kSemDataReady) {
            haveData.push_back(i);
        }
    }
    if(haveData.empty()) {
        return {};
    }
    nfp_cpp_area *dataArea = nfp_cpp_area_alloc(cpp_, dataCppId_, dataAddr_, dataSize_);
    if (!dataArea) {
        return {};
    }
    if (nfp_cpp_area_acquire(dataArea) < 0) {
        nfp_cpp_area_free(dataArea);
        return {};
    }
    const unsigned long counterDataEntrySize = static_cast<unsigned long>(sizeof(stored_counter)); // Exclude the 'index' field which is not used by SmartNIC
    for (uint32_t index : haveData) {
        stored_counter data;
        int readResult = nfp_cpp_area_read(dataArea, index * counterDataEntrySize, &data, sizeof(stored_counter));
        if (readResult != static_cast<int>(sizeof(stored_counter))) {
            continue; // Skip this entry on read error
        }
        counter_data entry{};
        entry.data = data;
        entry.index = index;
        result.push_back(entry);
    }
    nfp_cpp_area_fill(dataArea, 0, kDataClearValue, dataSize_); // Clear data after reading
    nfp_cpp_area_release_free(dataArea);
    return result;
}