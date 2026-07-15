#include "AppConfig.hpp"

#include <yaml-cpp/yaml.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void require(bool condition, const std::string &message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void requireEqual(const std::string &actual,
                  const std::string &expected,
                  const std::string &message)
{
    require(actual == expected, message + ": expected '" + expected + "', got '" + actual + "'");
}

void expectConfigurationError(const std::string &yaml, const std::string &needle)
{
    try {
        parseAppConfig(YAML::Load(yaml));
    } catch (const std::exception &ex) {
        require(std::string(ex.what()).find(needle) != std::string::npos,
                "error did not mention '" + needle + "': " + ex.what());
        return;
    }
    throw std::runtime_error("configuration unexpectedly parsed: " + yaml);
}

void testDeviceLists()
{
    const AppConfig single = parseAppConfig(YAML::Load("devices: [0]"));
    require(single.devices == std::vector<unsigned int>{0}, "single device list changed");

    const AppConfig multiple = parseAppConfig(YAML::Load("devices: [0, 2, 7]"));
    require(multiple.devices == std::vector<unsigned int>({0, 2, 7}),
            "device ordering was not preserved");
}

void testInvalidDeviceShapesAndItems()
{
    struct InvalidCase {
        const char *name;
        const char *yaml;
        const char *needle;
    };
    const std::vector<InvalidCase> cases = {
        {"missing", "{}", "devices"},
        {"null", "devices:", "devices"},
        {"scalar", "devices: 0", "devices"},
        {"map", "devices: { first: 0 }", "devices"},
        {"empty", "devices: []", "devices"},
        {"duplicate", "devices: [0, 0]", "devices[1] duplicates device ID 0"},
        {"null item", "devices: [~]", "devices[0]"},
        {"negative item", "devices: [-1]", "devices[0]"},
        {"overflowing item", "devices: [4294967296]", "devices[0] is outside"},
        {"fractional item", "devices: [1.5]", "devices[0]"},
        {"boolean item", "devices: [true]", "devices[0]"},
        {"map item", "devices: [{ id: 0 }]", "devices[0]"},
        {"nested sequence item", "devices: [[0]]", "devices[0]"},
    };

    for (const auto &test : cases) {
        try {
            expectConfigurationError(test.yaml, test.needle);
        } catch (const std::exception &ex) {
            throw std::runtime_error(std::string("invalid devices case '") + test.name + "': " +
                                     ex.what());
        }
    }
}

void testLegacyDeviceKeyIsRejected()
{
    expectConfigurationError("devnum: 0", "devnum");
    expectConfigurationError("devnum: 0\ndevices: [0]", "replace it with 'devices:");
}

void testDefaults()
{
    const AppConfig config = parseAppConfig(YAML::Load("devices: [0]"));
    require(!config.debug && config.run_secs == 14, "global defaults changed");
    requireEqual(config.target_ip, "192.168.1.4", "target IP default changed");
    require(config.target_port == 6343, "target port default changed");
    requireEqual(config.symbols.semaphore, "_global_semaphores", "semaphore default changed");
    requireEqual(config.symbols.semaphore_dup, "_global_semaphores_dup", "duplicate semaphore default changed");
    requireEqual(config.symbols.flow_key, "__flow_key", "flow key default changed");
    requireEqual(config.symbols.flow_key_dup, "__flow_key_dup", "duplicate flow key default changed");
    requireEqual(config.symbols.flow_data, "__flow_data", "flow data default changed");
    requireEqual(config.symbols.flow_data_dup, "__flow_data_dup", "duplicate flow data default changed");
    requireEqual(config.symbols.counter_semaphore, "_cglobal_semaphores", "counter semaphore default changed");
    requireEqual(config.symbols.counter_semaphore_dup, "_cglobal_semaphores_dup", "duplicate counter semaphore default changed");
    requireEqual(config.symbols.counter_data, "__counter_data", "counter data default changed");
    requireEqual(config.symbols.counter_data_dup, "__counter_data_dup", "duplicate counter data default changed");
    requireEqual(config.symbols.cur_state, "__cur_state", "current-state default changed");
    requireEqual(config.symbols.processing_me, "__processing_me", "processing-ME default changed");
    requireEqual(config.time.mac_time_symbol, "mac_time", "MAC time default changed");
    require(config.time.startup_sample_count == 64 && config.time.startup_best_sample_count == 8 &&
                config.time.refresh_burst_size == 8,
            "startup count defaults changed");
    require(config.time.startup_timeout.count() == 2000 &&
                config.time.startup_poll_interval.count() == 1000,
            "startup duration defaults changed");
    require(config.time.maximum_sample_uncertainty_ns == 800000,
            "maximum uncertainty default changed");
    require(config.time.calibration.regression_window_ns == 120000000000LL &&
                config.time.calibration.minimum_regression_span_ns == 5000000000LL &&
                config.time.calibration.maximum_drift_ppm == 1000.0 &&
                config.time.calibration.host_step_threshold_ns == 500000 &&
                config.time.calibration.model_step_threshold_ns == 20000000,
            "calibration defaults changed");
}

void testEverySupportedOverride()
{
    const AppConfig config = parseAppConfig(YAML::Load(R"(
devices: [7, 2]
debug: true
runSecs: 0
targetIp: "2001:db8::5"
targetPort: 9999
semaphore_sym: sem
semaphore_dup_sym: sem_dup
flow_key_sym: flow_key
flow_key_dup_sym: flow_key_dup
flow_data_sym: flow_data
flow_data_dup_sym: flow_data_dup
counter_semaphore_sym: counter_sem
counter_semaphore_dup_sym: counter_sem_dup
counter_data_sym: counter_data
counter_data_dup_sym: counter_data_dup
cur_state_sym: state
processing_me_sym: processing
mac_time_symbol: mac
startup_sample_count: 10
startup_best_sample_count: 4
startup_timeout_ms: 3000
startup_poll_interval_us: 50
calibration_refresh_burst_size: 3
maximum_sample_uncertainty_ns: 400
regression_window_ns: 1000
minimum_regression_span_ns: 500
maximum_drift_ppm: 12.5
host_step_threshold_ns: 20
model_step_threshold_ns: 30
)"));

    require(config.devices == std::vector<unsigned int>({7, 2}), "override devices changed");
    require(config.debug && config.run_secs == 0 && config.target_port == 9999,
            "global overrides did not parse");
    requireEqual(config.target_ip, "2001:db8::5", "target IP override did not parse");
    requireEqual(config.symbols.semaphore, "sem", "semaphore override did not parse");
    requireEqual(config.symbols.semaphore_dup, "sem_dup", "duplicate semaphore override did not parse");
    requireEqual(config.symbols.flow_key, "flow_key", "flow key override did not parse");
    requireEqual(config.symbols.flow_key_dup, "flow_key_dup", "duplicate flow key override did not parse");
    requireEqual(config.symbols.flow_data, "flow_data", "flow data override did not parse");
    requireEqual(config.symbols.flow_data_dup, "flow_data_dup", "duplicate flow data override did not parse");
    requireEqual(config.symbols.counter_semaphore, "counter_sem", "counter semaphore override did not parse");
    requireEqual(config.symbols.counter_semaphore_dup, "counter_sem_dup", "duplicate counter semaphore override did not parse");
    requireEqual(config.symbols.counter_data, "counter_data", "counter data override did not parse");
    requireEqual(config.symbols.counter_data_dup, "counter_data_dup", "duplicate counter data override did not parse");
    requireEqual(config.symbols.cur_state, "state", "current-state override did not parse");
    requireEqual(config.symbols.processing_me, "processing", "processing-ME override did not parse");
    requireEqual(config.time.mac_time_symbol, "mac", "MAC time override did not parse");
    require(config.time.startup_sample_count == 10 && config.time.startup_best_sample_count == 4 &&
                config.time.startup_timeout.count() == 3000 &&
                config.time.startup_poll_interval.count() == 50 && config.time.refresh_burst_size == 3 &&
                config.time.maximum_sample_uncertainty_ns == 400,
            "time overrides did not parse");
    require(config.time.calibration.regression_window_ns == 1000 &&
                config.time.calibration.minimum_regression_span_ns == 500 &&
                config.time.calibration.maximum_drift_ppm == 12.5 &&
                config.time.calibration.host_step_threshold_ns == 20 &&
                config.time.calibration.model_step_threshold_ns == 30,
            "calibration overrides did not parse");
}

void testInvalidGlobalValuesAndRelationships()
{
    struct InvalidCase {
        const char *yaml;
        const char *needle;
    };
    const std::vector<InvalidCase> cases = {
        {"devices: [0]\ntargetIp: not-an-ip", "targetIp"},
        {"devices: [0]\ntargetPort: 0", "targetPort"},
        {"devices: [0]\ntargetPort: 65536", "targetPort"},
        {"devices: [0]\nrunSecs: -1", "runSecs"},
        {"devices: [0]\nflow_key_sym: ''", "flow_key_sym"},
        {"devices: [0]\nstartup_sample_count: 0", "startup_sample_count"},
        {"devices: [0]\nstartup_best_sample_count: 0", "startup_best_sample_count"},
        {"devices: [0]\nstartup_timeout_ms: 0", "startup_timeout_ms"},
        {"devices: [0]\nstartup_poll_interval_us: 0", "startup_poll_interval_us"},
        {"devices: [0]\ncalibration_refresh_burst_size: 0", "calibration_refresh_burst_size"},
        {"devices: [0]\nmaximum_sample_uncertainty_ns: 0", "maximum_sample_uncertainty_ns"},
        {"devices: [0]\nregression_window_ns: 0", "regression_window_ns"},
        {"devices: [0]\nminimum_regression_span_ns: 0", "minimum_regression_span_ns"},
        {"devices: [0]\nhost_step_threshold_ns: 0", "host_step_threshold_ns"},
        {"devices: [0]\nmodel_step_threshold_ns: 0", "model_step_threshold_ns"},
        {"devices: [0]\nmaximum_drift_ppm: 0", "maximum_drift_ppm"},
        {"devices: [0]\nstartup_sample_count: 4\nstartup_best_sample_count: 5", "startup_best_sample_count"},
        {"devices: [0]\nregression_window_ns: 4\nminimum_regression_span_ns: 5", "minimum_regression_span_ns"},
    };

    for (const auto &test : cases)
        expectConfigurationError(test.yaml, test.needle);
}

void testLoadErrorsPropagate()
{
    const std::string missingPath = "/tmp/sflow_app_config_missing_" +
                                    std::to_string(std::chrono::steady_clock::now()
                                                       .time_since_epoch()
                                                       .count()) +
                                    ".yaml";
    try {
        loadAppConfig(missingPath);
        throw std::runtime_error("missing configuration file unexpectedly loaded");
    } catch (const std::exception &ex) {
        require(std::string(ex.what()).find("bad file") != std::string::npos,
                "missing-file error lacked yaml-cpp context: " + std::string(ex.what()));
    }

    const std::string malformedPath = "/tmp/sflow_app_config_malformed_" +
                                      std::to_string(std::chrono::steady_clock::now()
                                                         .time_since_epoch()
                                                         .count()) +
                                      ".yaml";
    {
        FILE *file = std::fopen(malformedPath.c_str(), "w");
        require(file != nullptr, "could not create malformed YAML fixture");
        std::fputs("devices: [0\n", file);
        std::fclose(file);
    }
    try {
        loadAppConfig(malformedPath);
        throw std::runtime_error("malformed YAML unexpectedly loaded");
    } catch (const std::exception &ex) {
        require(std::string(ex.what()).find("yaml") != std::string::npos ||
                    std::string(ex.what()).find("end of") != std::string::npos,
                "malformed-YAML error lacked parser context: " + std::string(ex.what()));
    }
    std::remove(malformedPath.c_str());
}
} // namespace

int main()
{
    try {
        testDeviceLists();
        testInvalidDeviceShapesAndItems();
        testLegacyDeviceKeyIsRejected();
        testDefaults();
        testEverySupportedOverride();
        testInvalidGlobalValuesAndRelationships();
        testLoadErrorsPropagate();
    } catch (const std::exception &ex) {
        std::cerr << "app config test failed: " << ex.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "app config tests passed\n";
    return EXIT_SUCCESS;
}
