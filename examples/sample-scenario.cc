// examples/sample-scenario.cc
// Sample ns-3 scenario: 3 APs x 30 stations, 802.11ax, TCP, isolated YansWifiChannels
//
// Usage: ./sample-scenario --config <config.toml> [--section-field <value> ...]
//

#include "scenario-config.h"
#include "scenario-log.h"
#include "scenario-topology.h"
#include "traffic-coordinator.h"
#include "traffic-flow-monitor.h"
#include "wifi-statistics.h"

#include "ns3/contention-aware-agent-distribution.h"
#include "ns3/core-module.h"
#include "ns3/tcp-highspeed.h"
#include "ns3/tcp-linux-reno.h"

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using namespace ns3;

int
main(int argc, char* argv[])
{
    LogComponent& g_log = llm_example::GetScenarioLog();

    const std::vector<std::string> arguments(argv + 1, argv + argc);
    const ScenarioCommandLineResult commandLine =
        ParseScenarioArguments(arguments, std::filesystem::current_path());
    if (commandLine.printUsage)
    {
        PrintScenarioUsage(commandLine.valid ? std::cout : std::cerr, argv[0]);
        return commandLine.valid ? 0 : 1;
    }
    if (!commandLine.valid)
    {
        std::cerr << commandLine.error << std::endl;
        return 1;
    }
    const ScenarioConfig& config = commandLine.launch.scenario;

    RngSeedManager::SetSeed(config.simulation.rngSeed);
    RngSeedManager::SetRun(config.simulation.rngRun);

    Config::SetDefault("ns3::TcpL4Protocol::SocketType", TypeIdValue(TcpHighSpeed::GetTypeId()));

    std::cout << "=== ns-3 Sample Scenario: " << config.topology.bssCount << " APs x "
              << config.topology.stationsPerBss << " Stations ===" << std::endl;
    std::cout << "JSON: " << config.general.traceFile << std::endl;
    std::cout << "Bandwidth: " << config.wifi.bandwidthMhz << " MHz" << std::endl;
    std::cout << "MAC stats JSON: " << config.general.outputName << std::endl;
    std::cout << "Standard: 802.11ax (Wi-Fi 6)" << std::endl;
    std::cout << "Transport: TCP" << std::endl;
    std::cout << "Channel model: separate YansWifiChannel per AP group" << std::endl;
    std::cout << "Channel policy: physically isolated AP groups; default 5 GHz channel"
              << std::endl;

    LogComponentEnable("SampleScenario", LOG_LEVEL_INFO);
    LogComponentEnable("APGenerator", LOG_LEVEL_WARN);
    LogComponentEnable("StaLlmGenerator", LOG_LEVEL_WARN);
    LogComponentEnable("TrafficSink", LOG_LEVEL_WARN);
    LogComponentEnable("ContentionAwareAgentDistribution", LOG_LEVEL_INFO);

    ParsedResult parsedTrace = ParseJsonFile(config.general.traceFile);
    const double traceDurationMs = parsedTrace.experimentDurationMs;
    const double maxExperimentDurationMs =
        config.simulation.durationMode == DurationMode::AUTO
            ? traceDurationMs + config.simulation.autoTailSeconds * 1000.0
            : config.simulation.fixedDurationSeconds * 1000.0;
    TrafficCoordinator trafficCoordinator(traceDurationMs, maxExperimentDurationMs);
    WifiStatistics wifiStatistics(trafficCoordinator);
    TrafficFlowMonitor trafficFlowMonitor(trafficCoordinator, wifiStatistics);

    ContentionAwareDistributionConfig distributionConfig;
    distributionConfig.nAp = config.topology.bssCount;
    distributionConfig.nStationsPerAp = config.topology.stationsPerBss;
    distributionConfig.maxAgentsPerStation = config.distribution.maxAgentsPerStation;
    distributionConfig.lowContentionPriority = config.distribution.lowContentionPriority;
    distributionConfig.slotMs = config.distribution.slotMs;

    DistributionResult distribution =
        DistributeAgentsContentionAware(parsedTrace, distributionConfig);

    for (int bssIndex = 0; bssIndex < config.topology.bssCount; ++bssIndex)
    {
        SetupApGroup(bssIndex,
                     config.wifi.bandwidthMhz,
                     distribution.apStationMaps[bssIndex],
                     distribution.apAgentMaps[bssIndex],
                     distribution.apAddresses[bssIndex],
                     config.topology.stationsPerBss,
                     trafficCoordinator,
                     wifiStatistics);
    }

    trafficCoordinator.FinalizeRegistration();

    if (config.simulation.durationMode == DurationMode::AUTO)
    {
        std::cout << "\nExperiment time mode: auto" << std::endl;
        std::cout << "Experiment duration from JSON: "
                  << trafficCoordinator.GetTraceDurationMs() / 1000.0 << " seconds" << std::endl;
        std::cout << "Max experiment time: "
                  << trafficCoordinator.GetMaxExperimentDurationMs() / 1000.0
                  << " seconds (JSON duration + " << config.simulation.autoTailSeconds
                  << " seconds)" << std::endl;
    }
    else
    {
        std::cout << "\nExperiment time mode: fixed" << std::endl;
        std::cout << "Max experiment time: "
                  << trafficCoordinator.GetMaxExperimentDurationMs() / 1000.0 << " seconds"
                  << std::endl;
        std::cout << "Experiment duration from JSON: "
                  << trafficCoordinator.GetTraceDurationMs() / 1000.0 << " seconds" << std::endl;

        if (trafficCoordinator.GetMaxExperimentDurationMs() <
            trafficCoordinator.GetTraceDurationMs())
        {
            NS_LOG_WARN("Fixed experiment time "
                        << trafficCoordinator.GetMaxExperimentDurationMs() / 1000.0
                        << "s is shorter than JSON duration "
                        << trafficCoordinator.GetTraceDurationMs() / 1000.0
                        << "s; trace tail will be truncated");
        }
    }
    std::cout << "Waiting for " << trafficCoordinator.GetExpectedGeneratorCount()
              << " traffic generators to complete TCP setup..." << std::endl;

    trafficFlowMonitor.ConnectDeviceTraces();

    Config::SetDefault("ns3::TcpL4Protocol::SocketType", TypeIdValue(TcpLinuxReno::GetTypeId()));
    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(1460));
    Config::SetDefault("ns3::TcpSocket::SndBufSize", UintegerValue(32 * 1024 * 1024));
    Config::SetDefault("ns3::TcpSocket::RcvBufSize", UintegerValue(32 * 1024 * 1024));

    const auto wallClockStart = std::chrono::steady_clock::now();

    Simulator::Run();

    const auto wallClockEnd = std::chrono::steady_clock::now();
    const double wallClockSeconds =
        std::chrono::duration<double>(wallClockEnd - wallClockStart).count();

    NS_ABORT_MSG_IF(trafficCoordinator.GetExperimentStartUs() < 0,
                    "Simulation ended before the global traffic barrier opened");

    wifiStatistics.WriteJson(config.general.outputName);
    trafficFlowMonitor.PrintTransmissionTimePerSender();
    wifiStatistics.PrintCrossLayerReport();

    Simulator::Destroy();

    std::cout << "\n=== Simulation Complete ===" << std::endl;
    std::cout << "Total events: " << Simulator::GetEventCount() << std::endl;
    NS_LOG_INFO("[Realtime] Simulator::Run wall-clock time: " << std::fixed << std::setprecision(3)
                                                              << wallClockSeconds << " seconds");
    std::cout << "Realtime simulation runtime: " << std::fixed << std::setprecision(3)
              << wallClockSeconds << " seconds" << std::endl;

    return 0;
}
