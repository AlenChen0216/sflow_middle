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
#include <queue>
#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

#include "../include/Device.hpp"
#include "../include/Reader.hpp"

#define BUFFER_SIZE 10

namespace
{

    struct ExportData
    {
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
                          SharedQueue &shared,
                          std::atomic<bool> &running,
                          std::chrono::milliseconds period)
    {
        auto nextTick = std::chrono::steady_clock::now();

        while (running.load())
        {
            std::vector<flow_data> flowEntries = reader.readFlowData();

            if (!flowEntries.empty())
            {
                std::map<Tuple, FlowRecord> localFlowMap;
                std::map<CounterAgent, CounterRecord> localCounterMap;

                for (const auto &entry : flowEntries)
                {
                    Tuple key{};
                    key.src_ip = entry.key.src_ip;
                    key.dst_ip = entry.key.dst_ip;
                    key.src_port = entry.key.src_port;
                    key.dst_port = entry.key.dst_port;
                    key.protocol = static_cast<uint8_t>(entry.key.protocol);

                    Agent agent{};
                    agent.aip = entry.key.agent_ip;
                    agent.in_if = static_cast<uint32_t>(entry.key.in_if);
                    agent.out_if = static_cast<uint32_t>(entry.key.out_if);
                    agent.frame_length = static_cast<uint64_t>(entry.frame_length);
                    agent.sampling_rate = entry.key.sampling_rate;
                    agent.tcp_flag = static_cast<uint8_t>(entry.key.tcp_flag);

                    localFlowMap[key].agent.push_back(agent);
                }

                if (!localFlowMap.empty() || !localCounterMap.empty())
                {
                    std::lock_guard<std::mutex> lock(shared.mtx);
                    shared.queue.push({std::move(localFlowMap), std::move(localCounterMap)});
                    shared.cv.notify_one();
                }
            }

            nextTick += period;
            std::this_thread::sleep_until(nextTick);
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

                nlohmann::json agents = nlohmann::json::array();
                for (const auto &ag : record.agent)
                {
                    agents.push_back({{"aip", ag.aip},
                                      {"in_if", ag.in_if},
                                      {"out_if", ag.out_if},
                                      {"frame_length", ag.frame_length},
                                      {"sampling_rate", ag.sampling_rate},
                                      {"tcp_flag", ag.tcp_flag}});
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
            j["counterMap"] = counterArray;
            if (j["flowMap"].empty() && j["counterMap"].empty())
            {
                std::cout << "No data to send.\n";
                continue;
            }
            std::string payload = j.dump();
            std::cout << "sending data: " << payload << "\n";
            sendto(sock, payload.data(), payload.size(), 0,
                   reinterpret_cast<struct sockaddr *>(&dest_addr), sizeof(dest_addr));
        }
        close(sock);
    }
} // namespace

int main(int argc, char *argv[])
{
    unsigned int devnum = 0;
    int runSecs = 30;
    std::string targetIp = "127.0.0.1";
    uint16_t targetPort = 8080;
    std::unique_ptr<NFPDevice> dev;

    // Symbol names for the 4 registers (configurable via YAML)
    std::string semSym = "_global_semaphores";
    std::string semDupSym = "_global_semaphores_dup";
    std::string flowDataSym = "__flow_data";
    std::string flowDataDupSym = "__flow_data_dup";

    try
    {
        YAML::Node config = YAML::LoadFile(argv[1]);

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
        if (config["flow_data_sym"])
            flowDataSym = config["flow_data_sym"].as<std::string>();
        if (config["flow_data_dup_sym"])
            flowDataDupSym = config["flow_data_dup_sym"].as<std::string>();

        try
        {
            dev = std::make_unique<NFPDevice>(devnum);
        }
        catch (const std::exception &ex)
        {
            std::cerr << "Fatal: " << ex.what() << '\n';
            return 1;
        }
    }
    catch (const YAML::Exception &e)
    {
        std::cerr << "YAML parsing error or setting.yaml not found: " << e.what() << "\n";
        return 1;
    }

    // Look up the 4 rtsym symbols
    const nfp_rtsym *symSem = dev->getSymbolData(semSym.c_str());
    const nfp_rtsym *symSemDup = dev->getSymbolData(semDupSym.c_str());
    const nfp_rtsym *symData = dev->getSymbolData(flowDataSym.c_str());
    const nfp_rtsym *symDataDup = dev->getSymbolData(flowDataDupSym.c_str());

    if (!symSem || !symSemDup || !symData || !symDataDup) {
        std::cerr << "Fatal: one or more required symbols not found on SmartNIC\n";
        if (!symSem)      std::cerr << "  missing: " << semSym << "\n";
        if (!symSemDup)   std::cerr << "  missing: " << semDupSym << "\n";
        if (!symData)     std::cerr << "  missing: " << flowDataSym << "\n";
        if (!symDataDup)  std::cerr << "  missing: " << flowDataDupSym << "\n";
        return 1;
    }

    SmartNicReader reader(dev->cpp(),
                          symSem->addr, symSemDup->addr,
                          symData->addr, symDataDup->addr,
                          symSem->domain, symData->domain,
                          BUFFER_SIZE);

    SharedQueue shared;
    std::atomic<bool> running{true};

    std::thread nicThread(smartNicIoThread,
                          std::ref(reader),
                          std::ref(shared),
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
