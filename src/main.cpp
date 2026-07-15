#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>
#include <pthread.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include "../include/AppConfig.hpp"
#include "../include/DeviceRuntime.hpp"
#include "../include/ExportPipeline.hpp"

namespace
{
constexpr std::size_t kMaxLogFileSize = 10 * 1024 * 1024;
constexpr std::size_t kMaxRotatedLogFiles = 1;
constexpr const char *kLogFile = "logs/sflow_middle.log";
constexpr const char *kLogPattern = "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v";

void configureLogging()
{
    spdlog::set_pattern(kLogPattern);
    spdlog::flush_on(spdlog::level::warn);

    try {
        std::filesystem::create_directories("logs");
        auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            kLogFile, kMaxLogFileSize, kMaxRotatedLogFiles);
        fileSink->set_pattern(kLogPattern);
        spdlog::default_logger()->sinks().push_back(std::move(fileSink));
    } catch (const std::exception &ex) {
        SPDLOG_ERROR("Failed to configure rotating log file '{}': {}",
                     kLogFile, ex.what());
    }
}

bool addShutdownSignal(sigset_t &signals, int signal)
{
    if (sigaddset(&signals, signal) != 0) {
        SPDLOG_ERROR("Failed to add shutdown signal {}: {}", signal,
                     std::strerror(errno));
        return false;
    }
    return true;
}

bool setupShutdownSignals(sigset_t &signals)
{
    if (sigemptyset(&signals) != 0) {
        SPDLOG_ERROR("Failed to initialize shutdown signal set: {}",
                     std::strerror(errno));
        return false;
    }
    if (!addShutdownSignal(signals, SIGINT) ||
        !addShutdownSignal(signals, SIGTERM) ||
        !addShutdownSignal(signals, SIGHUP)) {
        return false;
    }

    const int rc = pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    if (rc != 0) {
        SPDLOG_ERROR("Failed to block shutdown signals: {}", std::strerror(rc));
        return false;
    }
    return true;
}

void requestStop(ShutdownState &shutdown, SharedQueue &shared,
                 bool failed = false) noexcept
{
    shutdown.requestStop(failed);
    shared.notifyAll();
}

void shutdownSignalThread(sigset_t signals, ShutdownState &shutdown,
                          SharedQueue &shared)
{
    // sigtimedwait has no condition-variable equivalent. A short timeout lets
    // this joinable thread observe a peer-initiated shutdown without consuming
    // or synthesizing a process signal.
    const timespec timeout{0, 100000000};
    while (!shutdown.stopRequested()) {
        siginfo_t info{};
        const int signal = sigtimedwait(&signals, &info, &timeout);
        if (signal == -1) {
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            }
            SPDLOG_ERROR("Signal wait failed: {}", std::strerror(errno));
            requestStop(shutdown, shared, true);
            return;
        }

        SPDLOG_INFO("Shutdown signal received ({}). Stopping...", signal);
        requestStop(shutdown, shared);
        return;
    }
}

void transmitThread(SharedQueue &shared, const std::string &targetIp,
                    std::uint16_t targetPort, bool debug,
                    ShutdownState &shutdown, std::promise<void> ready)
{
    namespace asio = boost::asio;
    using udp = asio::ip::udp;

    asio::io_service io;
    std::unique_ptr<asio::io_service::work> work;
    std::unique_ptr<udp::socket> socket;
    udp::endpoint destination;
    std::unique_ptr<std::thread> ioThread;
    bool readinessReported = false;

    try {
        destination = udp::endpoint(asio::ip::address::from_string(targetIp),
                                    targetPort);
        socket = std::make_unique<udp::socket>(io, udp::v4());
        work = std::make_unique<asio::io_service::work>(io);

        std::ofstream debugFile;
        if (debug) {
            debugFile.open("test_output.json", std::ios::out);
            if (!debugFile.is_open()) {
                throw std::runtime_error("Failed to open test_output.json for writing");
            }
        }

        ioThread = std::make_unique<std::thread>([&io] { io.run(); });
        ready.set_value();
        readinessReported = true;

        auto queueSend = [&](unsigned int devnum, std::string payload) {
            auto data = std::make_shared<std::string>(std::move(payload));
            io.post([&, data, devnum] {
                socket->async_send_to(
                    asio::buffer(*data), destination,
                    [data, devnum](const boost::system::error_code &error,
                                   std::size_t) {
                        if (error) {
                            SPDLOG_ERROR("[devnum={}] UDP send failed: {}", devnum,
                                         error.message());
                        }
                    });
            });
        };

        ExportData batch;
        while (shared.waitPop(batch)) {
            for (const auto &[key, record] : batch.flow_map) {
                queueSend(batch.devnum,
                          makeFlowPayload(batch.devnum, key, record).dump());
            }
            for (const auto &[key, record] : batch.counter_map) {
                queueSend(batch.devnum,
                          makeCounterPayload(batch.devnum, key, record).dump());
            }
            if (debug) {
                debugFile << makeDebugPayload(batch).dump() << '\n';
                if (!debugFile) {
                    throw std::runtime_error("Failed to write test_output.json");
                }
            }
        }
    } catch (...) {
        const std::exception_ptr error = std::current_exception();
        try {
            std::rethrow_exception(error);
        } catch (const std::exception &ex) {
            SPDLOG_ERROR("UDP transmitter failed: {}", ex.what());
        } catch (...) {
            SPDLOG_ERROR("UDP transmitter failed: unknown error");
        }
        if (!readinessReported) {
            try {
                ready.set_exception(error);
            } catch (const std::future_error &) {
            }
        }
        requestStop(shutdown, shared, true);
    }

    // Release only after the consumer has stopped accepting batches; joining
    // the I/O thread then completes all sends posted before queue drain.
    work.reset();
    if (ioThread && ioThread->joinable()) {
        ioThread->join();
    }
}

void releaseUnstartedProducers(SharedQueue &shared, std::size_t count)
{
    for (std::size_t index = 0; index < count; ++index) {
        try {
            shared.producerFinished();
        } catch (const std::exception &ex) {
            SPDLOG_CRITICAL("Failed to release producer obligation: {}", ex.what());
            break;
        }
    }
}
} // namespace

int main(int argc, char *argv[])
{
    configureLogging();

    sigset_t shutdownSignals;
    if (!setupShutdownSignals(shutdownSignals)) {
        return EXIT_FAILURE;
    }

    AppConfig config;
    const std::string configPath = argc > 1 ? argv[1] : "setting.yaml";
    try {
        config = loadAppConfig(configPath);
    } catch (const std::exception &ex) {
        SPDLOG_CRITICAL("Fatal configuration error in '{}': {}", configPath, ex.what());
        return EXIT_FAILURE;
    }

    // This is deliberately a complete transaction: no background thread is
    // created until every device has opened, resolved symbols, and calibrated.
    std::vector<std::unique_ptr<DeviceRuntime>> runtimes;
    runtimes.reserve(config.devices.size());
    try {
        for (unsigned int devnum : config.devices) {
            runtimes.push_back(std::make_unique<DeviceRuntime>(devnum, config));
        }
    } catch (const std::exception &ex) {
        SPDLOG_CRITICAL("Fatal device initialization error: {}", ex.what());
        return EXIT_FAILURE;
    }

    SharedQueue shared(runtimes.size());
    ShutdownState shutdown;
    std::thread transmitter;
    std::thread signalThread;
    std::size_t workersStarted = 0;

    try {
        std::promise<void> transmitterReady;
        std::future<void> transmitterReadyFuture = transmitterReady.get_future();
        transmitter = std::thread(transmitThread, std::ref(shared),
                                  std::cref(config.target_ip), config.target_port,
                                  config.debug, std::ref(shutdown),
                                  std::move(transmitterReady));

        try {
            transmitterReadyFuture.get();
        } catch (const std::exception &ex) {
            SPDLOG_CRITICAL("Fatal transmitter initialization error: {}", ex.what());
            releaseUnstartedProducers(shared, runtimes.size());
            requestStop(shutdown, shared, true);
            transmitter.join();
            return EXIT_FAILURE;
        }

        signalThread = std::thread(shutdownSignalThread, shutdownSignals,
                                   std::ref(shutdown), std::ref(shared));

        for (; workersStarted < runtimes.size(); ++workersStarted) {
            runtimes[workersStarted]->startWorker(shared, shutdown);
        }

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(config.run_secs);
        if (config.run_secs == 0) {
            shutdown.waitUntil(std::chrono::steady_clock::time_point::max());
        } else if (!shutdown.waitUntil(deadline)) {
            SPDLOG_INFO("Configured runtime of {} seconds elapsed. Stopping...",
                        config.run_secs);
            requestStop(shutdown, shared);
        }
    } catch (const std::exception &ex) {
        SPDLOG_CRITICAL("Fatal thread launch error: {}", ex.what());
        // Started workers own their completion through DeviceRuntime's guard.
        // The failed launch and every later runtime still need their obligation.
        releaseUnstartedProducers(shared, runtimes.size() - workersStarted);
        requestStop(shutdown, shared, true);
    } catch (...) {
        SPDLOG_CRITICAL("Fatal unknown thread launch error");
        releaseUnstartedProducers(shared, runtimes.size() - workersStarted);
        requestStop(shutdown, shared, true);
    }

    // Stop is idempotent. It is needed after normal signal shutdown too, and
    // it makes workers wake promptly before we begin joining their runtimes.
    requestStop(shutdown, shared);
    for (auto &runtime : runtimes) {
        runtime->joinWorker();
    }
    if (transmitter.joinable()) {
        transmitter.join();
    }
    if (signalThread.joinable()) {
        signalThread.join();
    }

    // Runtimes are destroyed only after all consumers and producers are gone.
    return shutdown.hasFailed() ? EXIT_FAILURE : EXIT_SUCCESS;
}
