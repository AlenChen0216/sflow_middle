#include "AppConfig.hpp"

#include <boost/asio/ip/address.hpp>
#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include <charconv>
#include <cmath>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace
{
[[noreturn]] void configError(const std::string &message)
{
    throw std::invalid_argument("configuration error: " + message);
}

const YAML::Node optionalNode(const YAML::Node &root, const char *key)
{
    return root[key];
}

template <typename T>
void readOptional(const YAML::Node &root, const char *key, T &destination)
{
    const YAML::Node value = optionalNode(root, key);
    if (!value)
        return;
    if (!value.IsScalar())
        configError(std::string("'") + key + "' must be a scalar");
    try {
        destination = value.as<T>();
    } catch (const YAML::Exception &ex) {
        configError(std::string("invalid '") + key + "': " + ex.what());
    }
}

void requirePositive(const char *key, int64_t value)
{
    if (value <= 0)
        configError(std::string("'") + key + "' must be greater than zero");
}

void requirePositive(const char *key, std::size_t value)
{
    if (value == 0)
        configError(std::string("'") + key + "' must be greater than zero");
}

void requireFinitePositive(const char *key, double value)
{
    if (!std::isfinite(value) || value <= 0.0)
        configError(std::string("'") + key + "' must be finite and greater than zero");
}

unsigned int parseDeviceId(const YAML::Node &node, std::size_t index)
{
    const std::string prefix = "devices[" + std::to_string(index) + "]";
    if (!node.IsScalar())
        configError(prefix + " must be a base-10 unsigned integer");

    const std::string value = node.Scalar();
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
        configError(prefix + " must be a base-10 unsigned integer");

    unsigned int result = 0;
    const char *begin = value.data();
    const char *end = begin + value.size();
    const auto parsed = std::from_chars(begin, end, result, 10);
    if (parsed.ec == std::errc::result_out_of_range || parsed.ptr != end)
        configError(prefix + " is outside the unsigned int range");
    if (parsed.ec != std::errc{})
        configError(prefix + " must be a base-10 unsigned integer");
    return result;
}

void validateSymbol(const char *key, const std::string &value)
{
    if (value.empty())
        configError(std::string("'") + key + "' must not be empty");
}

void validate(const AppConfig &config)
{
    if (config.run_secs < 0)
        configError("'runSecs' must be greater than or equal to zero");
    if (config.target_ip.empty())
        configError("'targetIp' must not be empty");

    boost::system::error_code addressError;
    boost::asio::ip::address::from_string(config.target_ip, addressError);
    if (addressError)
        configError("'targetIp' is not a valid IP address");
    if (config.target_port == 0)
        configError("'targetPort' must be in 1..65535");

    validateSymbol("semaphore_sym", config.symbols.semaphore);
    validateSymbol("semaphore_dup_sym", config.symbols.semaphore_dup);
    validateSymbol("flow_key_sym", config.symbols.flow_key);
    validateSymbol("flow_key_dup_sym", config.symbols.flow_key_dup);
    validateSymbol("flow_data_sym", config.symbols.flow_data);
    validateSymbol("flow_data_dup_sym", config.symbols.flow_data_dup);
    validateSymbol("counter_semaphore_sym", config.symbols.counter_semaphore);
    validateSymbol("counter_semaphore_dup_sym", config.symbols.counter_semaphore_dup);
    validateSymbol("counter_data_sym", config.symbols.counter_data);
    validateSymbol("counter_data_dup_sym", config.symbols.counter_data_dup);
    validateSymbol("cur_state_sym", config.symbols.cur_state);
    validateSymbol("processing_me_sym", config.symbols.processing_me);
    validateSymbol("mac_time_symbol", config.time.mac_time_symbol);

    requirePositive("startup_sample_count", config.time.startup_sample_count);
    requirePositive("startup_best_sample_count", config.time.startup_best_sample_count);
    requirePositive("calibration_refresh_burst_size", config.time.refresh_burst_size);
    requirePositive("startup_timeout_ms", config.time.startup_timeout.count());
    requirePositive("startup_poll_interval_us", config.time.startup_poll_interval.count());
    requirePositive("maximum_sample_uncertainty_ns", config.time.maximum_sample_uncertainty_ns);
    requirePositive("regression_window_ns", config.time.calibration.regression_window_ns);
    requirePositive("minimum_regression_span_ns", config.time.calibration.minimum_regression_span_ns);
    requirePositive("host_step_threshold_ns", config.time.calibration.host_step_threshold_ns);
    requirePositive("model_step_threshold_ns", config.time.calibration.model_step_threshold_ns);
    requireFinitePositive("maximum_drift_ppm", config.time.calibration.maximum_drift_ppm);

    if (config.time.startup_best_sample_count > config.time.startup_sample_count)
        configError("'startup_best_sample_count' must be less than or equal to 'startup_sample_count'");
    if (config.time.calibration.minimum_regression_span_ns >
        config.time.calibration.regression_window_ns)
        configError("'minimum_regression_span_ns' must be less than or equal to 'regression_window_ns'");
}
} // namespace

AppConfig parseAppConfig(const YAML::Node &root)
{
    if (!root || !root.IsMap())
        configError("root document must be a map");
    if (root["devnum"])
        configError("'devnum' is no longer supported; replace it with 'devices: [<device-id>, ...]'");

    const YAML::Node deviceNodes = root["devices"];
    if (!deviceNodes || !deviceNodes.IsSequence())
        configError("'devices' must be a non-empty sequence");
    if (deviceNodes.size() == 0)
        configError("'devices' must be a non-empty sequence");

    AppConfig config;
    std::set<unsigned int> seenDevices;
    config.devices.reserve(deviceNodes.size());
    for (std::size_t index = 0; index < deviceNodes.size(); ++index) {
        const unsigned int device = parseDeviceId(deviceNodes[index], index);
        if (!seenDevices.insert(device).second)
            configError("devices[" + std::to_string(index) + "] duplicates device ID " +
                        std::to_string(device));
        config.devices.push_back(device);
    }

    readOptional(root, "debug", config.debug);
    readOptional(root, "runSecs", config.run_secs);
    readOptional(root, "targetIp", config.target_ip);

    unsigned int targetPort = config.target_port;
    readOptional(root, "targetPort", targetPort);
    if (targetPort > std::numeric_limits<std::uint16_t>::max())
        configError("'targetPort' must be in 1..65535");
    config.target_port = static_cast<std::uint16_t>(targetPort);

    readOptional(root, "semaphore_sym", config.symbols.semaphore);
    readOptional(root, "semaphore_dup_sym", config.symbols.semaphore_dup);
    readOptional(root, "flow_key_sym", config.symbols.flow_key);
    readOptional(root, "flow_key_dup_sym", config.symbols.flow_key_dup);
    readOptional(root, "flow_data_sym", config.symbols.flow_data);
    readOptional(root, "flow_data_dup_sym", config.symbols.flow_data_dup);
    readOptional(root, "counter_semaphore_sym", config.symbols.counter_semaphore);
    readOptional(root, "counter_semaphore_dup_sym", config.symbols.counter_semaphore_dup);
    readOptional(root, "counter_data_sym", config.symbols.counter_data);
    readOptional(root, "counter_data_dup_sym", config.symbols.counter_data_dup);
    readOptional(root, "cur_state_sym", config.symbols.cur_state);
    readOptional(root, "processing_me_sym", config.symbols.processing_me);

    readOptional(root, "mac_time_symbol", config.time.mac_time_symbol);
    readOptional(root, "startup_sample_count", config.time.startup_sample_count);
    readOptional(root, "startup_best_sample_count", config.time.startup_best_sample_count);
    int64_t startupTimeoutMs = config.time.startup_timeout.count();
    readOptional(root, "startup_timeout_ms", startupTimeoutMs);
    config.time.startup_timeout = std::chrono::milliseconds(startupTimeoutMs);
    int64_t startupPollIntervalUs = config.time.startup_poll_interval.count();
    readOptional(root, "startup_poll_interval_us", startupPollIntervalUs);
    config.time.startup_poll_interval =
        std::chrono::microseconds(startupPollIntervalUs);
    readOptional(root, "calibration_refresh_burst_size", config.time.refresh_burst_size);
    readOptional(root, "maximum_sample_uncertainty_ns", config.time.maximum_sample_uncertainty_ns);
    readOptional(root, "regression_window_ns", config.time.calibration.regression_window_ns);
    readOptional(root, "minimum_regression_span_ns", config.time.calibration.minimum_regression_span_ns);
    readOptional(root, "maximum_drift_ppm", config.time.calibration.maximum_drift_ppm);
    readOptional(root, "host_step_threshold_ns", config.time.calibration.host_step_threshold_ns);
    readOptional(root, "model_step_threshold_ns", config.time.calibration.model_step_threshold_ns);

    if (root["offset_time"] || root["time_sym"] || root["mac_clock_xpb"])
        SPDLOG_WARN("offset_time, time_sym, and mac_clock_xpb are deprecated and ignored; "
                    "using exported mac_time calibration");

    validate(config);
    return config;
}

AppConfig loadAppConfig(const std::string &path)
{
    return parseAppConfig(YAML::LoadFile(path));
}
