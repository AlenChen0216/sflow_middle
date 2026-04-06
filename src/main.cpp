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

#include "../include/Device.hpp"
#include "../include/Reader.hpp"

#define BUFFER_SIZE 30

namespace
{

    static const RegisterSpec kRegisters[] = { // Notice: NIC will treat 16 bit as 32 bit.
        // flow sample
        {"_pif_register_reg_src_ip", 0x0000400500ULL, 4, BUFFER_SIZE, 25},
        {"_pif_register_reg_dst_ip", 0x0000808700ULL, 4, BUFFER_SIZE, 24},
        {"_pif_register_reg_src_port", 0x0000400400ULL, 4, BUFFER_SIZE, 25},
        {"_pif_register_reg_dst_port", 0x0000808600ULL, 4, BUFFER_SIZE, 24},
        {"_pif_register_reg_protocol", 0x0000400300ULL, 4, BUFFER_SIZE, 25},
        {"_pif_register_reg_frame_length", 0x0000808300ULL, 4, BUFFER_SIZE, 24},
        {"_pif_register_reg_sample_rate", 0x0000808500ULL, 4, BUFFER_SIZE, 24},
        {"_pif_register_reg_tcp_flag", 0x000080a000ULL, 4, BUFFER_SIZE, 24},
        {"_pif_register_reg_agent_ip", 0x0000400200ULL, 4, BUFFER_SIZE, 25},
        {"_pif_register_reg_in_if", 0x0000808400ULL, 4, BUFFER_SIZE, 24},
        {"_pif_register_reg_out_if", 0x0000400100ULL, 4, BUFFER_SIZE, 25},

        // counter sample
        {"_pif_register_reg_agent_ip", 0x0000400800ULL, 4, BUFFER_SIZE, 25},
        {"_pif_register_reg_if_idx", 0x000080a200ULL, 4, BUFFER_SIZE, 24},
        {"_pif_register_reg_if_speed", 0x0000400600ULL, 4, BUFFER_SIZE, 25},
        {"_pif_register_reg_in_octets", 0x0000400700ULL, 4, BUFFER_SIZE, 25},
        {"_pif_register_reg_out_octets", 0x000080a100ULL, 4, BUFFER_SIZE, 24}};

    constexpr std::size_t kRegisterCount = sizeof(kRegisters) / sizeof(kRegisters[0]);

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

    template <typename T>
    T readValueAt(const std::vector<uint8_t> &buffer, std::size_t offset, std::size_t base = 0)
    {
        T value{};
        if (offset + sizeof(T) > buffer.size())
        {
            return value;
        }
        std::memcpy(&value, buffer.data() + base + offset, sizeof(T));
        std::cout << value << "\n";
        return value;
    }

    uint64_t readRegisterValue(const std::vector<uint8_t> &buffer,
                               const RegisterSpec &reg,
                               std::size_t index)
    {
        const std::size_t offset = index * reg.bytes;
        switch (reg.bytes)
        {
        case 4:
            return readValueAt<uint32_t>(buffer, offset);
        case 8:
            return (static_cast<uint64_t>(readValueAt<uint32_t>(buffer, offset - 4, 0)) << 32) | readValueAt<uint32_t>(buffer, offset, 4);
        default:
            return 0;
        }
    }

    void smartNicIoThread(SmartNicReader &reader,
                          SharedQueue &shared,
                          std::atomic<bool> &running,
                          std::chrono::milliseconds period)
    {
        auto nextTick = std::chrono::steady_clock::now();

        while (running.load())
        {
            std::vector<std::vector<uint8_t>> snapshot;
            snapshot.reserve(kRegisterCount);

            bool readOk = true;
            for (const auto &reg : kRegisters)
            {
                auto data = reader.readAndReset(reg);
                if (data.empty())
                {
                    readOk = false;
                    break;
                }
                snapshot.push_back(std::move(data));
            }

            // for (const auto &sn : snapshot)
            // {
            //     for (const auto &byte : sn)
            //     {
            //         std::cout << std::hex << static_cast<int>(byte) << " ";
            //     }
            //     std::cout << "\n";
            // }
            // auto now = std::chrono::steady_clock::now();
            if (readOk && snapshot.size() == kRegisterCount)
            {
                std::map<Tuple, FlowRecord> localFlowMap;
                std::map<CounterAgent, CounterRecord> localCounterMap;

                for (std::size_t index = 0; index < BUFFER_SIZE; ++index)
                {
                    const uint64_t keyMarker = readRegisterValue(snapshot[0], kRegisters[0], index);
                    if (keyMarker != 0)
                    {
                        Tuple key{};
                        key.src_ip = static_cast<uint32_t>(keyMarker);
                        key.dst_ip = static_cast<uint32_t>(readRegisterValue(snapshot[1], kRegisters[1], index));
                        key.src_port = static_cast<uint16_t>(readRegisterValue(snapshot[2], kRegisters[2], index));
                        key.dst_port = static_cast<uint16_t>(readRegisterValue(snapshot[3], kRegisters[3], index));
                        key.protocol = static_cast<uint8_t>(readRegisterValue(snapshot[4], kRegisters[4], index));

                        FlowRecord &record = localFlowMap[key];
                        record.frame_length = static_cast<uint32_t>(readRegisterValue(snapshot[5], kRegisters[5], index));
                        record.sampling_rate = static_cast<uint32_t>(readRegisterValue(snapshot[6], kRegisters[6], index));
                        record.tcp_flag = static_cast<uint8_t>(readRegisterValue(snapshot[7], kRegisters[7], index));

                        Agent agent{};
                        agent.aip = static_cast<uint32_t>(readRegisterValue(snapshot[8], kRegisters[8], index));
                        agent.in_if = static_cast<uint32_t>(readRegisterValue(snapshot[9], kRegisters[9], index));
                        agent.out_if = static_cast<uint32_t>(readRegisterValue(snapshot[10], kRegisters[10], index));
                        record.agent.push_back(agent);
                    }
                    const uint64_t keyMarker2 = readRegisterValue(snapshot[11], kRegisters[11], index);
                    if (keyMarker2 != 0)
                    {
                        // counter part.
                        CounterAgent counterAgent{};
                        counterAgent.agent_ip = static_cast<uint32_t>(readRegisterValue(snapshot[11], kRegisters[11], index));
                        counterAgent.if_idx = static_cast<uint32_t>(readRegisterValue(snapshot[12], kRegisters[12], index));
                        CounterRecord &counterRecord = localCounterMap[counterAgent];
                        counterRecord.if_speed = static_cast<uint32_t>(readRegisterValue(snapshot[13], kRegisters[13], index));
                        counterRecord.in_octets = static_cast<uint32_t>(readRegisterValue(snapshot[14], kRegisters[14], index));
                        counterRecord.out_octets = static_cast<uint32_t>(readRegisterValue(snapshot[15], kRegisters[15], index));
                    }
                }

                if (!localFlowMap.empty() || !localCounterMap.empty()) {
                    std::lock_guard<std::mutex> lock(shared.mtx);
                    shared.queue.push({std::move(localFlowMap), std::move(localCounterMap)});
                    shared.cv.notify_one();
                }
            }
            // std::cout<< "Snapshot taken at " << std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() << " ms\n";

            // for (auto &entry : flowMap)
            // {
            //     const Tuple &key = entry.first;
            //     const FlowRecord &record = entry.second;
            //     std::cout << "Flow: " << inet_ntoa(*(in_addr *)&key.src_ip) << ":" << key.src_port
            //               << " -> " << inet_ntoa(*(in_addr *)&key.dst_ip) << ":" << key.dst_port
            //               << " Proto: " << static_cast<int>(key.protocol)
            //               << " FrameLen: " << record.frame_length
            //               << " SampleRate: " << record.sampling_rate
            //               << " TCPFlag: " << static_cast<int>(record.tcp_flag) << "\n";
            //     for (const auto &agent : record.agent)
            //     {
            //         std::cout << "\tAgent IP: " << inet_ntoa(*(in_addr *)&agent.aip)
            //                   << " In_if: " << agent.in_if
            //                   << " Out_if: " << agent.out_if << "\n";
            //     }
            // }
            // for (auto &entry : counterMap)
            // {
            //     const CounterAgent &key = entry.first;
            //     const CounterRecord &record = entry.second;
            //     std::cout << "Counter: AgentIP: " << inet_ntoa(*(in_addr *)&key.agent_ip)
            //               << " If_idx: " << key.if_idx
            //               << " If_speed: " << record.if_speed
            //               << " In_octets: " << record.in_octets
            //               << " Out_octets: " << record.out_octets << "\n";
            // }
            nextTick += period;
            std::this_thread::sleep_until(nextTick);
            
        }
    }

    void transmitThread(SharedQueue &shared, std::atomic<bool> &running,
                        const std::string& target_ip, uint16_t target_port) {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) {
            std::cerr << "Failed to create socket.\n";
            return;
        }

        struct sockaddr_in dest_addr{};
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(target_port);
        if (inet_pton(AF_INET, target_ip.c_str(), &dest_addr.sin_addr) <= 0) {
            std::cerr << "Invalid address.\n";
            close(sock);
            return;
        }

        while (running.load()) {
            ExportData data;
            {
                std::unique_lock<std::mutex> lock(shared.mtx);
                shared.cv.wait(lock, [&]{ return !shared.queue.empty() || !running.load(); });
                if (!running.load() && shared.queue.empty()) {
                    break;
                }
                data = std::move(shared.queue.front());
                shared.queue.pop();
            }

            nlohmann::json j;
            nlohmann::json flowArray = nlohmann::json::array();
            for (const auto& [key, record] : data.flowMap) {
                nlohmann::json item;
                item["src_ip"] = key.src_ip;
                item["dst_ip"] = key.dst_ip;
                item["src_port"] = key.src_port;
                item["dst_port"] = key.dst_port;
                item["protocol"] = key.protocol;
                item["frame_length"] = record.frame_length;
                item["sampling_rate"] = record.sampling_rate;
                item["tcp_flag"] = record.tcp_flag;

                nlohmann::json agents = nlohmann::json::array();
                for (const auto& ag : record.agent) {
                    agents.push_back({
                        {"aip", ag.aip},
                        {"in_if", ag.in_if},
                        {"out_if", ag.out_if}
                    });
                }
                item["agent"] = agents;
                flowArray.push_back(item);
            }
            j["flowMap"] = flowArray;

            nlohmann::json counterArray = nlohmann::json::array();
            for (const auto& [key, record] : data.counterMap) {
                nlohmann::json item;
                item["agent_ip"] = key.agent_ip;
                item["if_idx"] = key.if_idx;
                item["if_speed"] = record.if_speed;
                item["in_octets"] = record.in_octets;
                item["out_octets"] = record.out_octets;
                counterArray.push_back(item);
            }
            j["counterMap"] = counterArray;

            std::string payload = j.dump();
            std::cout<<"sending data: " << payload << "\n";
            sendto(sock, payload.data(), payload.size(), 0,
                   reinterpret_cast<struct sockaddr*>(&dest_addr), sizeof(dest_addr));
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

    if (argc > 1)
    {
        devnum = static_cast<unsigned int>(std::atoi(argv[1]));
    }
    if (argc > 2)
    {
        runSecs = std::atoi(argv[2]);
    }
    if (argc > 3)
    {
        targetIp = argv[3];
    }
    if (argc > 4)
    {
        targetPort = static_cast<uint16_t>(std::atoi(argv[4]));
    }

    std::unique_ptr<NFPDevice> dev;
    try
    {
        dev = std::make_unique<NFPDevice>(devnum);
    }
    catch (const std::exception &ex)
    {
        std::cerr << "Fatal: " << ex.what() << '\n';
        return 1;
    }

    SmartNicReader reader(dev->cpp(), 0);
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
    if (txThread.joinable()) {
        txThread.join();
    }

    return 0;
}
