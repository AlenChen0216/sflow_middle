#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <queue>
#include <fstream>
#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

#include "../include/Time.hpp"
#include "../include/Device.hpp"
#include "../include/Reader.hpp"
#include "../include/CounterReader.hpp"

#define BUFFER_SIZE 20000
#define BUFFER_SIZE2 2048

namespace
{

    struct ExportData
    {
        double execution_time; // Time taken to read and process data from SmartNIC
        std::map<Tuple, FlowRecord> flowMap;
        std::map<CounterAgent, CounterRecord> counterMap;
    };

    struct SharedQueue
    {
        std::mutex mtx;
        std::condition_variable cv;
        std::queue<ExportData> queue;
    };

    void smartNicIoThread(SmartNicReader &reader,
                          CounterReader &counterReader,
                          SharedQueue &shared,
                          TimeAdjuster &timeAdjuster,
                          std::atomic<bool> &running,
                          std::chrono::milliseconds period
                          )
    {
        auto nextTick = std::chrono::steady_clock::now();
        bool driftCalibrationReported = false;

        while (running.load())
        {
            nextTick += period;
            std::this_thread::sleep_until(nextTick);
            // measure time taken by readFlowData() for debugging
            auto start = std::chrono::steady_clock::now();

            std::vector<flow_data> flowEntries = reader.readFlowData();
            std::vector<counter_data> counterEntries = counterReader.readCounterData();
            if (!timeAdjuster.refresh()) {
                std::cerr << "Warning: SmartNIC clock refresh sample rejected; "
                             "continuing with the previous calibration\n";
            }
            const CalibrationStatus calibration = timeAdjuster.status();
            if (!calibration.drift_calibrated) {
                driftCalibrationReported = false;
            } else if (!driftCalibrationReported) {
                std::cout << "SmartNIC clock drift calibrated: "
                          << (calibration.scale - 1.0) * 1000000.0
                          << " ppm, median residual "
                          << calibration.median_residual_ns / 1000
                          << " us, samples " << calibration.sample_count
                          << "\n";
                driftCalibrationReported = true;
            }

            auto end = std::chrono::steady_clock::now();
            std::chrono::duration<double> elapsed = end - start;
            std::cout << "readFlowData() took " << elapsed.count() << " seconds and returned " << flowEntries.size() << " entries\n";
            
            std::map<Tuple, FlowRecord> localFlowMap;
            std::map<CounterAgent, CounterRecord> localCounterMap;

            if (!flowEntries.empty())
            {

                for (const auto &entry : flowEntries)
                {
                    Tuple key{};
                    key.src_ip = entry.key.src_ip;
                    key.dst_ip = entry.key.dst_ip;
                    key.src_port = entry.key.dst_port;
                    key.dst_port = entry.key.src_port;
                    key.protocol = static_cast<uint16_t>(entry.key.protocol);

                    Agent agent{};
                    agent.aip = entry.key.agent_ip;
                    agent.in_if = static_cast<uint32_t>(entry.key.out_if);
                    agent.out_if = static_cast<uint32_t>(entry.key.in_if);
                    agent.byte_cnt = entry.data.byte_cnt;
                    agent.packet_cnt = entry.data.packet_cnt;
                    agent.sampling_rate = entry.key.sampling_rate;
                    agent.tcp_flag = static_cast<uint16_t>(entry.key.tcp_flag);
                    agent.index = entry.index;
                    
                    const int64_t startTimeNs =
                        timeAdjuster.toUnixNanoseconds(
                            entry.data.start_time.sec,
                            entry.data.start_time.nsec);
                    const int64_t endTimeNs =
                        timeAdjuster.toUnixNanoseconds(
                            entry.data.end_time.sec,
                            entry.data.end_time.nsec);

                    FlowRecord &record = localFlowMap[key];
                    record.agent.push_back(agent);
                    record.start_time_ns =
                        std::min(record.start_time_ns, startTimeNs);
                    record.end_time_ns =
                        std::max(record.end_time_ns, endTimeNs);
                    record.start_time = record.start_time_ns / 1000000;
                    record.end_time = record.end_time_ns / 1000000;
                }

                
            }
            if(!counterEntries.empty())
            {
                for(const auto &entry : counterEntries)
                {
                    CounterAgent key{};
                    key.agent_ip = entry.data.key.agent_ip;
                    key.if_idx = entry.data.key.if_idx;

                    CounterRecord record{};
                    record.if_speed = entry.data.if_speed;
                    record.in_octets = entry.data.in_octets;
                    record.out_octets = entry.data.out_octets;

                    localCounterMap[key] = record;
                }
            }

            if (!localFlowMap.empty() || !localCounterMap.empty())
            {
                std::lock_guard<std::mutex> lock(shared.mtx);
                shared.queue.push({elapsed.count(), std::move(localFlowMap), std::move(localCounterMap)});
                shared.cv.notify_one();
            }
            
        }
    }

    void transmitThread(SharedQueue &shared, std::atomic<bool> &running,
                        const std::string &target_ip, uint16_t target_port)
    {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0)
        {
            std::cerr << "Failed to create socket.\n";
            return;
        }
        std::fstream testFile;
        testFile.open("test_output.json", std::ios::out);
        if( !testFile.is_open() ) {
            std::cerr << "Failed to open test_output.json for writing.\n";
            close(sock);
            return;
        }
        struct sockaddr_in dest_addr{};
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(target_port);
        if (inet_pton(AF_INET, target_ip.c_str(), &dest_addr.sin_addr) <= 0)
        {
            std::cerr << "Invalid address.\n";
            close(sock);
            return;
        }
        
        while (running.load())
        {
            ExportData data;
            {
                std::unique_lock<std::mutex> lock(shared.mtx);
                shared.cv.wait(lock, [&]
                               { return !shared.queue.empty() || !running.load(); });
                if (!running.load() && shared.queue.empty())
                {
                    break;
                }
                data = std::move(shared.queue.front());
                shared.queue.pop();
            }

            nlohmann::json j;
            nlohmann::json flowArray = nlohmann::json::array();
            for (const auto &[key, record] : data.flowMap)
            {
                nlohmann::json item;
                item["src_ip"] = key.src_ip;
                item["dst_ip"] = key.dst_ip;
                item["src_port"] = key.src_port;
                item["dst_port"] = key.dst_port;
                item["protocol"] = key.protocol;
                item["start_time"] = record.start_time;
                item["end_time"] = record.end_time;
                // std::cout<<"Start time: "<<record.start_time<<", End time: "<<record.end_time<<std::endl;

                nlohmann::json agents = nlohmann::json::array();
                for (const auto &ag : record.agent)
                {
                    agents.push_back({{"aip", ag.aip},
                                      {"in_if", ag.in_if},
                                      {"out_if", ag.out_if},
                                      {"byte_cnt", ag.byte_cnt},
                                      {"packet_cnt", ag.packet_cnt},
                                      {"sampling_rate", ag.sampling_rate},
                                      {"tcp_flag", ag.tcp_flag},
                                      {"index", ag.index}});
                }
                item["agent"] = agents;
                flowArray.push_back(item);
            }
            j["flowMap"] = flowArray;

            nlohmann::json counterArray = nlohmann::json::array();
            for (const auto &[key, record] : data.counterMap)
            {
                nlohmann::json item;
                item["agent_ip"] = key.agent_ip;
                item["if_idx"] = key.if_idx;
                item["if_speed"] = record.if_speed;
                item["in_octets"] = record.in_octets;
                item["out_octets"] = record.out_octets;
                counterArray.push_back(item);
            }
            j["execution_time"] = data.execution_time;
            j["counterMap"] = counterArray;
            if (j["flowMap"].empty() && j["counterMap"].empty())
            {
                std::cout << "No data to send.\n";
                continue;
            }
            std::string payload = j.dump();
            // std::cout << "sending data: " << payload << "\n";
            testFile << payload << std::endl;
            sendto(sock, payload.data(), payload.size(), 0,
                   reinterpret_cast<struct sockaddr *>(&dest_addr), sizeof(dest_addr));
        }
        close(sock);
        testFile.close();
    }

} // namespace




int main(int argc, char *argv[])
{
    unsigned int devnum = 0;
    int runSecs = 14;
    std::string targetIp = "192.168.1.4";
    uint16_t targetPort = 6343;
    std::unique_ptr<NFPDevice> dev;

    // Symbol names for the 4 registers (configurable via YAML)
    std::string semSym = "_global_semaphores";
    std::string semDupSym = "_global_semaphores_dup";
    std::string flowKeySym = "__flow_key";
    std::string flowKeyDupSym = "__flow_key_dup";
    std::string flowDataSym = "__flow_data";
    std::string flowDataDupSym = "__flow_data_dup";
    std::string counterSemSym = "_cglobal_semaphores";
    std::string counterDataSym = "__counter_data";
    std::string curStateSym = "__cur_state";
    std::string processingMeSym = "__processing_me";
    uint32_t macClockXpb = TimeAdjuster::kDefaultMacClockXpbAddress;

    try
    {
        YAML::Node config = argc > 1 ? YAML::LoadFile(argv[1]) : YAML::LoadFile("setting.yaml");

        if (config["devnum"])
            devnum = config["devnum"].as<unsigned int>();
        if (config["runSecs"])
            runSecs = config["runSecs"].as<int>();
        if (config["targetIp"])
            targetIp = config["targetIp"].as<std::string>();
        if (config["targetPort"])
            targetPort = config["targetPort"].as<uint16_t>();
        if (config["semaphore_sym"])
            semSym = config["semaphore_sym"].as<std::string>();
        if (config["semaphore_dup_sym"])
            semDupSym = config["semaphore_dup_sym"].as<std::string>();
        if (config["flow_key_sym"])
            flowKeySym = config["flow_key_sym"].as<std::string>();
        if (config["flow_key_dup_sym"])
            flowKeyDupSym = config["flow_key_dup_sym"].as<std::string>();
        if (config["flow_data_sym"])
            flowDataSym = config["flow_data_sym"].as<std::string>();
        if (config["flow_data_dup_sym"])
            flowDataDupSym = config["flow_data_dup_sym"].as<std::string>();
        if (config["counter_semaphore_sym"])
            counterSemSym = config["counter_semaphore_sym"].as<std::string>();
        if (config["counter_data_sym"])
            counterDataSym = config["counter_data_sym"].as<std::string>();
        if (config["cur_state_sym"])
            curStateSym = config["cur_state_sym"].as<std::string>();
        if (config["processing_me_sym"])
            processingMeSym = config["processing_me_sym"].as<std::string>();
        if (config["mac_clock_xpb"])
            macClockXpb = config["mac_clock_xpb"].as<uint32_t>();
        if (config["offset_time"] || config["time_sym"])
            std::cerr << "Warning: offset_time and time_sym are deprecated "
                         "and ignored; using the live NBI MAC clock\n";
    }
    catch (const YAML::Exception &e)
    {
        std::cerr << "YAML parsing error or setting.yaml not found: " << e.what() << "\n";
        // return 1;
    }
    try
    {
        dev = std::make_unique<NFPDevice>(devnum);
    }
    catch (const std::exception &ex)
    {
        std::cerr << "Fatal: " << ex.what() << '\n';
        return 1;
    }
    // Look up the 4 rtsym symbols
    const nfp_rtsym *symSem = dev->getSymbolData(semSym.c_str());
    const nfp_rtsym *symSemDup = dev->getSymbolData(semDupSym.c_str());
    const nfp_rtsym *symKey = dev->getSymbolData(flowKeySym.c_str());
    const nfp_rtsym *symKeyDup = dev->getSymbolData(flowKeyDupSym.c_str());
    const nfp_rtsym *symData = dev->getSymbolData(flowDataSym.c_str());
    const nfp_rtsym *symDataDup = dev->getSymbolData(flowDataDupSym.c_str());
    const nfp_rtsym *symCounterSem = dev->getSymbolData(counterSemSym.c_str());
    const nfp_rtsym *symCounterData = dev->getSymbolData(counterDataSym.c_str());
    const nfp_rtsym *symCurState = dev->getSymbolData(curStateSym.c_str());
    const nfp_rtsym *symProcessingMe = dev->getSymbolData(processingMeSym.c_str());

    if (!symSem || !symSemDup || !symKey || !symKeyDup ||
        !symData || !symDataDup || !symCounterSem || !symCounterData ||
        !symCurState || !symProcessingMe) {
        std::cerr << "Fatal: one or more required symbols not found on SmartNIC\n";
        if (!symSem)      std::cerr << "  missing: " << semSym << "\n";
        if (!symSemDup)   std::cerr << "  missing: " << semDupSym << "\n";
        if (!symKey)      std::cerr << "  missing: " << flowKeySym << "\n";
        if (!symKeyDup)   std::cerr << "  missing: " << flowKeyDupSym << "\n";
        if (!symData)     std::cerr << "  missing: " << flowDataSym << "\n";
        if (!symDataDup)  std::cerr << "  missing: " << flowDataDupSym << "\n";
        if (!symCounterSem) std::cerr << "  missing: " << counterSemSym << "\n";
        if (!symCounterData) std::cerr << "  missing: " << counterDataSym << "\n";
        if (!symCurState) std::cerr << "  missing: " << curStateSym << "\n";
        if (!symProcessingMe) std::cerr << "  missing: " << processingMeSym << "\n";
        return 1;
    }
    std::cout<< "Successfully found all required symbols on SmartNIC\n";
    std::cout<< ""  << "  " << curStateSym << ": addr=0x" << std::hex << symCurState->addr << std::dec << ", domain=" << symCurState->domain << "\n";
    std::cout<< ""  << "  " << processingMeSym << ": addr=0x" << std::hex << symProcessingMe->addr << std::dec << ", domain=" << symProcessingMe->domain << "\n";
    std::cout<< ""  << "  " << semSym << ": addr=0x" << std::hex << symSem->addr << std::dec << ", domain=" << symSem->domain << "\n";
    std::cout<< ""  << "  " << semDupSym << ": addr=0x" << std::hex << symSemDup->addr << std::dec << ", domain=" << symSemDup->domain << "\n";
    std::cout<< ""  << "  " << flowKeySym << ": addr=0x" << std::hex << symKey->addr << std::dec << ", domain=" << symKey->domain << "\n";
    std::cout<< ""  << "  " << flowKeyDupSym << ": addr=0x" << std::hex << symKeyDup->addr << std::dec << ", domain=" << symKeyDup->domain << "\n";
    std::cout<< ""  << "  " << flowDataSym << ": addr=0x" << std::hex << symData->addr << std::dec << ", domain=" << symData->domain << "\n";
    std::cout<< ""  << "  " << flowDataDupSym << ": addr=0x" << std::hex << symDataDup->addr << std::dec << ", domain=" << symDataDup->domain << "\n";
    std::cout<< ""  << "  " << counterSemSym << ": addr=0x" << std::hex << symCounterSem->addr << std::dec << ", domain=" << symCounterSem->domain << "\n";
    std::cout<< ""  << "  " << counterDataSym << ": addr=0x" << std::hex << symCounterData->addr << std::dec << ", domain=" << symCounterData->domain << "\n";
    std::cout << "  MAC clock XPB: 0x" << std::hex << macClockXpb
              << std::dec << "\n";

    SmartNicReader reader(dev->cpp(),
                          symCurState->addr, symCurState->domain,
                          symProcessingMe->addr, symProcessingMe->domain,
                          symSem->addr, symSemDup->addr,
                          symKey->addr, symKeyDup->addr,
                          symData->addr, symDataDup->addr,
                          symSem->domain, symKey->domain, symData->domain,
                          symSemDup->domain, symKeyDup->domain, symDataDup->domain,
                          BUFFER_SIZE);

    CounterReader counterReader(dev->cpp(),
                                symCounterSem->addr,
                                symCounterData->addr,
                                symCounterSem->domain,
                                symCounterData->domain,
                                BUFFER_SIZE2);

    SharedQueue shared;
    std::atomic<bool> running{true};

    std::unique_ptr<TimeAdjuster> timeAdjuster;
    try
    {
        timeAdjuster = std::make_unique<TimeAdjuster>(
            dev->cpp(), macClockXpb);
    }
    catch (const std::exception &ex)
    {
        std::cerr << "Fatal: " << ex.what() << '\n';
        return 1;
    }
    const CalibrationStatus initialCalibration = timeAdjuster->status();
    std::cout << "SmartNIC clock calibrated: minimum latency "
              << initialCalibration.minimum_latency_ns / 1000
              << " us, samples " << initialCalibration.sample_count << "\n";

    std::thread nicThread(smartNicIoThread,
                          std::ref(reader),
                          std::ref(counterReader),
                          std::ref(shared),
                          std::ref(*timeAdjuster),
                          std::ref(running),
                          std::chrono::seconds(1));

    std::thread txThread(transmitThread,
                         std::ref(shared),
                         std::ref(running),
                         targetIp,
                         targetPort);

    std::this_thread::sleep_for(std::chrono::seconds(runSecs));

    running.store(false);
    shared.cv.notify_all();

    if (nicThread.joinable())
    {
        nicThread.join();
    }
    if (txThread.joinable())
    {
        txThread.join();
    }

    return 0;
}
