#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <queue>
#include <fstream>
#include <pthread.h>
#include <boost/asio.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include "../include/Time.hpp"
#include "../include/Device.hpp"
#include "../include/Reader.hpp"
#include "../include/CounterReader.hpp"

#define BUFFER_SIZE 20000
#define BUFFER_SIZE2 2048
#define MAX_JSON_SIZE 1500

std::atomic<bool> running{true};

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

    bool addShutdownSignal(sigset_t &signals, int signal)
    {
        if (sigaddset(&signals, signal) != 0)
        {
            SPDLOG_ERROR("Failed to add shutdown signal {}: {}",
                         signal, std::strerror(errno));
            return false;
        }
        return true;
    }

    bool setupShutdownSignals(sigset_t &signals)
    {
        if (sigemptyset(&signals) != 0)
        {
            SPDLOG_ERROR("Failed to initialize shutdown signal set: {}",
                         std::strerror(errno));
            return false;
        }

        if (!addShutdownSignal(signals, SIGINT) ||
            !addShutdownSignal(signals, SIGTERM) ||
            !addShutdownSignal(signals, SIGHUP))
        {
            return false;
        }

        const int rc = pthread_sigmask(SIG_BLOCK, &signals, nullptr);
        if (rc != 0)
        {
            SPDLOG_ERROR("Failed to block shutdown signals: {}",
                         std::strerror(rc));
            return false;
        }
        return true;
    }

    void shutdownSignalThread(sigset_t signals, SharedQueue &shared)
    {
        const timespec timeout{0, 200000000};

        while (running.load())
        {
            siginfo_t info{};
            const int signal = sigtimedwait(&signals, &info, &timeout);
            if (signal == -1)
            {
                if (errno == EAGAIN || errno == EINTR)
                {
                    continue;
                }
                SPDLOG_ERROR("Signal wait failed: {}", std::strerror(errno));
                running.store(false);
                shared.cv.notify_all();
                return;
            }

            SPDLOG_INFO("Shutdown signal received ({}). Stopping...", signal);
            running.store(false);
            shared.cv.notify_all();
            return;
        }
    }

    void smartNicIoThread(SmartNicReader &reader,
                          CounterReader &counterReader,
                          SharedQueue &shared,
                          TimeAdjuster &timeAdjuster,
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
                SPDLOG_WARN("SmartNIC clock refresh sample rejected; "
                            "continuing with the previous calibration");
            }
            const CalibrationStatus calibration = timeAdjuster.status();
            if (!calibration.drift_calibrated) {
                driftCalibrationReported = false;
            } else if (!driftCalibrationReported) {
                SPDLOG_INFO("SmartNIC clock drift calibrated: {} ppm, "
                            "median residual {} us, samples {}",
                            (calibration.scale - 1.0) * 1000000.0,
                            calibration.median_residual_ns / 1000,
                            calibration.sample_count);
                driftCalibrationReported = true;
            }

            auto end = std::chrono::steady_clock::now();
            std::chrono::duration<double> elapsed = end - start;
            SPDLOG_INFO("readFlowData() took {} seconds and returned {} entries",
                        elapsed.count(), flowEntries.size());
            
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
        std::this_thread::sleep_for(std::chrono::milliseconds(1000)); // Give the transmit thread a moment to finish processing
        shared.cv.notify_all(); // Notify the transmit thread to exit if it's waiting
    }

    void transmitThread(SharedQueue &shared,
                        const std::string &target_ip, uint16_t target_port, bool debug = false)
    {
        namespace asio = boost::asio;
        using udp = asio::ip::udp;

        asio::io_service io;
        std::unique_ptr<asio::io_service::work> work;
        std::unique_ptr<udp::socket> socket;
        udp::endpoint destination;
        try
        {
            destination = udp::endpoint(
                asio::ip::address::from_string(target_ip), target_port);
            socket.reset(new udp::socket(io, udp::v4()));
            work.reset(new asio::io_service::work(io));
        }
        catch (const boost::system::system_error &error)
        {
            SPDLOG_ERROR("Failed to initialize UDP sender: {}", error.what());
            return;
        }

        std::fstream testFile;
        testFile.open("test_output.json", std::ios::out);
        if( !testFile.is_open() ) {
            SPDLOG_ERROR("Failed to open test_output.json for writing");
            return;
        }

        std::thread ioThread([&io] { io.run(); });
        auto queueSend = [&](std::string payload) {
            auto data = std::make_shared<std::string>(std::move(payload));
            io.post([&, data] {
                socket->async_send_to(
                    asio::buffer(*data), destination,
                    [data](const boost::system::error_code &error, std::size_t) {
                        if (error)
                        {
                            SPDLOG_ERROR("UDP send failed: {}", error.message());
                        }
                    });
            });
        };
        
        while (true)
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
            nlohmann::json for_record;
            nlohmann::json for_record_agents = nlohmann::json::array();
            for (const auto &[key, record] : data.flowMap)
            {
                nlohmann::json j;
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
                nlohmann::json flowArray = nlohmann::json::array({item});
                for_record_agents.push_back(item);
                j["flowMap"] = flowArray;
                queueSend(j.dump());
            }
            for_record["execution_time"] = data.execution_time;
            for_record["flowMap"] = for_record_agents;

            nlohmann::json for_record_counterArray = nlohmann::json::array();
            for (const auto &[key, record] : data.counterMap)
            {
                nlohmann::json j;
                nlohmann::json item;

                item["agent_ip"] = key.agent_ip;
                item["if_idx"] = key.if_idx;
                item["if_speed"] = record.if_speed;
                item["in_octets"] = record.in_octets;
                item["out_octets"] = record.out_octets;
                nlohmann::json counterArray = nlohmann::json::array({item});
                for_record_counterArray.push_back(item);
                j["counterMap"] = counterArray;
                queueSend(j.dump());
            }
            for_record["counterMap"] = for_record_counterArray;
            std::string payload = for_record.dump();
            // std::string payload = for_record.dump();
            if(debug)
                testFile << payload << std::endl;
        }
        work.reset();
        ioThread.join();
        testFile.close();
    }

} // namespace

int main(int argc, char *argv[])
{
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    spdlog::flush_on(spdlog::level::warn);

    unsigned int devnum = 0;
    int runSecs = 14;
    std::string targetIp = "192.168.1.4";
    uint16_t targetPort = 6343;
    std::unique_ptr<NFPDevice> dev;

    sigset_t shutdownSignals;
    if (!setupShutdownSignals(shutdownSignals))
    {
        return 1;
    }

    // Symbol names for the 4 registers (configurable via YAML)
    bool debug = false;
    std::string semSym = "_global_semaphores";
    std::string semDupSym = "_global_semaphores_dup";
    std::string flowKeySym = "__flow_key";
    std::string flowKeyDupSym = "__flow_key_dup";
    std::string flowDataSym = "__flow_data";
    std::string flowDataDupSym = "__flow_data_dup";
    std::string counterSemSym = "_cglobal_semaphores";
    std::string counterSemDupSym = "_cglobal_semaphores_dup";
    std::string counterDataSym = "__counter_data";
    std::string counterDataDupSym = "__counter_data_dup";
    std::string curStateSym = "__cur_state";
    std::string processingMeSym = "__processing_me";
    TimeAdjuster::Config timeConfig;

    try
    {
        YAML::Node config = argc > 1 ? YAML::LoadFile(argv[1]) : YAML::LoadFile("setting.yaml");
        if (config["debug"])
            debug = config["debug"].as<bool>();
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
        if (config["counter_semaphore_dup_sym"])
            counterSemDupSym = config["counter_semaphore_dup_sym"].as<std::string>();
        if (config["counter_data_sym"])
            counterDataSym = config["counter_data_sym"].as<std::string>();
        if (config["counter_data_dup_sym"])
            counterDataDupSym = config["counter_data_dup_sym"].as<std::string>();
        if (config["cur_state_sym"])
            curStateSym = config["cur_state_sym"].as<std::string>();
        if (config["processing_me_sym"])
            processingMeSym = config["processing_me_sym"].as<std::string>();
        if (config["mac_time_symbol"])
            timeConfig.mac_time_symbol = config["mac_time_symbol"].as<std::string>();
        if (config["startup_sample_count"])
            timeConfig.startup_sample_count = config["startup_sample_count"].as<std::size_t>();
        if (config["startup_best_sample_count"])
            timeConfig.startup_best_sample_count = config["startup_best_sample_count"].as<std::size_t>();
        if (config["calibration_refresh_burst_size"])
            timeConfig.refresh_burst_size = config["calibration_refresh_burst_size"].as<std::size_t>();
        if (config["maximum_sample_uncertainty_ns"])
            timeConfig.maximum_sample_uncertainty_ns = config["maximum_sample_uncertainty_ns"].as<int64_t>();
        if (config["regression_window_ns"])
            timeConfig.calibration.regression_window_ns = config["regression_window_ns"].as<int64_t>();
        if (config["minimum_regression_span_ns"])
            timeConfig.calibration.minimum_regression_span_ns = config["minimum_regression_span_ns"].as<int64_t>();
        if (config["maximum_drift_ppm"])
            timeConfig.calibration.maximum_drift_ppm = config["maximum_drift_ppm"].as<double>();
        if (config["host_step_threshold_ns"])
            timeConfig.calibration.host_step_threshold_ns = config["host_step_threshold_ns"].as<int64_t>();
        if (config["model_step_threshold_ns"])
            timeConfig.calibration.model_step_threshold_ns = config["model_step_threshold_ns"].as<int64_t>();
        if (config["startup_timeout_ms"])
            timeConfig.startup_timeout =
                std::chrono::milliseconds(config["startup_timeout_ms"].as<int64_t>());
        if (config["startup_poll_interval_us"])
            timeConfig.startup_poll_interval =
                std::chrono::microseconds(config["startup_poll_interval_us"].as<int64_t>());
        if (config["offset_time"] || config["time_sym"] || config["mac_clock_xpb"])
            SPDLOG_WARN("offset_time and time_sym are deprecated and ignored; "
                        "using exported mac_time calibration");
    }
    catch (const YAML::Exception &e)
    {
        SPDLOG_ERROR("YAML parsing error or setting.yaml not found: {}", e.what());
        // return 1;
    }
    try
    {
        dev = std::make_unique<NFPDevice>(devnum);
    }
    catch (const std::exception &ex)
    {
        SPDLOG_CRITICAL("Fatal: {}", ex.what());
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
    const nfp_rtsym *symCounterSemDup = dev->getSymbolData(counterSemDupSym.c_str());
    const nfp_rtsym *symCounterData = dev->getSymbolData(counterDataSym.c_str());
    const nfp_rtsym *symCounterDataDup = dev->getSymbolData(counterDataDupSym.c_str());
    const nfp_rtsym *symCurState = dev->getSymbolData(curStateSym.c_str());
    const nfp_rtsym *symProcessingMe = dev->getSymbolData(processingMeSym.c_str());
    const nfp_rtsym *symMacTime = dev->getSymbolData(timeConfig.mac_time_symbol.c_str());

    if (!symSem || !symSemDup || !symKey || !symKeyDup ||
        !symData || !symDataDup || !symCounterSem || !symCounterSemDup ||
        !symCounterData || !symCounterDataDup ||
        !symCurState || !symProcessingMe || !symMacTime) {
        SPDLOG_CRITICAL("Fatal: one or more required symbols not found on SmartNIC");
        if (!symSem) SPDLOG_CRITICAL("  missing: {}", semSym);
        if (!symSemDup) SPDLOG_CRITICAL("  missing: {}", semDupSym);
        if (!symKey) SPDLOG_CRITICAL("  missing: {}", flowKeySym);
        if (!symKeyDup) SPDLOG_CRITICAL("  missing: {}", flowKeyDupSym);
        if (!symData) SPDLOG_CRITICAL("  missing: {}", flowDataSym);
        if (!symDataDup) SPDLOG_CRITICAL("  missing: {}", flowDataDupSym);
        if (!symCounterSem) SPDLOG_CRITICAL("  missing: {}", counterSemSym);
        if (!symCounterSemDup) SPDLOG_CRITICAL("  missing: {}", counterSemDupSym);
        if (!symCounterData) SPDLOG_CRITICAL("  missing: {}", counterDataSym);
        if (!symCounterDataDup) SPDLOG_CRITICAL("  missing: {}", counterDataDupSym);
        if (!symCurState) SPDLOG_CRITICAL("  missing: {}", curStateSym);
        if (!symProcessingMe) SPDLOG_CRITICAL("  missing: {}", processingMeSym);
        if (!symMacTime) SPDLOG_CRITICAL("  missing: {}", timeConfig.mac_time_symbol);
        return 1;
    }
    SPDLOG_INFO("Successfully found all required symbols on SmartNIC");
    SPDLOG_INFO("  {}: addr=0x{:x}, domain={}", curStateSym, symCurState->addr, symCurState->domain);
    SPDLOG_INFO("  {}: addr=0x{:x}, domain={}", processingMeSym, symProcessingMe->addr, symProcessingMe->domain);
    SPDLOG_INFO("  {}: addr=0x{:x}, domain={}", semSym, symSem->addr, symSem->domain);
    SPDLOG_INFO("  {}: addr=0x{:x}, domain={}", semDupSym, symSemDup->addr, symSemDup->domain);
    SPDLOG_INFO("  {}: addr=0x{:x}, domain={}", flowKeySym, symKey->addr, symKey->domain);
    SPDLOG_INFO("  {}: addr=0x{:x}, domain={}", flowKeyDupSym, symKeyDup->addr, symKeyDup->domain);
    SPDLOG_INFO("  {}: addr=0x{:x}, domain={}", flowDataSym, symData->addr, symData->domain);
    SPDLOG_INFO("  {}: addr=0x{:x}, domain={}", flowDataDupSym, symDataDup->addr, symDataDup->domain);
    SPDLOG_INFO("  {}: addr=0x{:x}, domain={}", counterSemSym, symCounterSem->addr, symCounterSem->domain);
    SPDLOG_INFO("  {}: addr=0x{:x}, domain={}", counterSemDupSym, symCounterSemDup->addr, symCounterSemDup->domain);
    SPDLOG_INFO("  {}: addr=0x{:x}, domain={}", counterDataSym, symCounterData->addr, symCounterData->domain);
    SPDLOG_INFO("  {}: addr=0x{:x}, domain={}", counterDataDupSym, symCounterDataDup->addr, symCounterDataDup->domain);
    SPDLOG_INFO("  {}: addr=0x{:x}, domain={}", timeConfig.mac_time_symbol, symMacTime->addr, symMacTime->domain);

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
                                symCurState->addr, symCurState->domain,
                                symProcessingMe->addr, symProcessingMe->domain,
                                symCounterSem->addr, symCounterSemDup->addr,
                                symCounterData->addr, symCounterDataDup->addr,
                                symCounterSem->domain, symCounterData->domain,
                                symCounterSemDup->domain, symCounterDataDup->domain,
                                BUFFER_SIZE2);

    SharedQueue shared;
    std::unique_ptr<TimeAdjuster> timeAdjuster;
    try
    {
        timeAdjuster = std::make_unique<TimeAdjuster>(
            dev->cpp(), symMacTime, timeConfig);
    }
    catch (const std::exception &ex)
    {
        SPDLOG_CRITICAL("Fatal: {}", ex.what());
        return 1;
    }
    const CalibrationStatus initialCalibration = timeAdjuster->status();
    SPDLOG_INFO("SmartNIC clock calibrated: minimum latency {} us, samples {}",
                initialCalibration.minimum_latency_ns / 1000,
                initialCalibration.sample_count);

    std::thread nicThread(smartNicIoThread,
                          std::ref(reader),
                          std::ref(counterReader),
                          std::ref(shared),
                          std::ref(*timeAdjuster),
                          std::chrono::seconds(1));

    std::thread txThread(transmitThread,
                         std::ref(shared),
                         targetIp,
                         targetPort,
                         debug);

    std::thread signalThread(shutdownSignalThread,
                             shutdownSignals,
                             std::ref(shared));
                         
    try
    {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(runSecs);
        if(runSecs > 0)
        {
            while (running.load() && std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }else{
            
            while (running.load())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            }
        }
        
    }
    catch(const std::exception &ex)
    {
        SPDLOG_ERROR("Error during sleep: {}", ex.what());
    }

    running.store(false);
    shared.cv.notify_all();

    if (signalThread.joinable())
    {
        signalThread.join();
    }
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
