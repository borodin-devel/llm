// examples/llm-scenario.cc
// Configurable ns-3 scenario: 802.11ax infrastructure BSSs with TCP trace replay
//
// Usage: ./llm-scenario --config <config.toml> [--section-field <value> ...]
//

#include "config/scenario-config.h"
#include "runtime/log.h"
#include "runtime/topology.h"
#include "runtime/traffic-coordinator.h"
#include "statistics/experiment-statistics.h"

#include "ns3/contention-aware-agent-distribution.h"
#include "ns3/core-module.h"

#include <chrono>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ns3;

int
main(int argc, char* argv[])
{
    const auto launchTime = std::chrono::system_clock::now();
    const auto workingDirectory = std::filesystem::current_path();
    const std::vector<std::string> arguments(argv + 1, argv + argc);
    const ScenarioCommandLineResult commandLine =
        ParseScenarioArguments(arguments, workingDirectory);
    if (commandLine.valid && commandLine.printUsage)
    {
        PrintScenarioUsage(std::cout, argv[0]);
        return 0;
    }
    if (!commandLine.valid)
    {
        std::cerr << "error: " << commandLine.error << "\n\n";
        PrintScenarioUsage(std::cerr, argv[0]);
        return 1;
    }
    const ScenarioConfig& config = commandLine.launch.scenario;

    ResolvedRunPaths resolvedPaths;
    try
    {
        ValidateScenarioConfig(config);
        resolvedPaths = ResolveRunPaths(commandLine.launch, launchTime);
        PrepareRunDirectory(resolvedPaths);
    }
    catch (const ScenarioConfigError& error)
    {
        std::cerr << "error: " << error.what() << "\n\n";
        PrintScenarioUsage(std::cerr, argv[0]);
        return 1;
    }

    ParsedResult parsedTrace;
    try
    {
        parsedTrace = ParseJsonFile(resolvedPaths.traceFile.string());
    }
    catch (const std::exception& error)
    {
        std::cerr << "error: cannot parse trace '" << resolvedPaths.traceFile.string()
                  << "': " << error.what() << "\n\n";
        PrintScenarioUsage(std::cerr, argv[0]);
        return 1;
    }

    const double maximumDurationMs =
        config.simulation.durationMode == DurationMode::AUTO
            ? parsedTrace.experimentDurationMs + config.simulation.autoTailSeconds * 1000.0
            : config.simulation.fixedDurationSeconds * 1000.0;

    LogComponent& g_log = llm_example::GetScenarioLog();
    RngSeedManager::SetSeed(config.simulation.rngSeed);
    RngSeedManager::SetRun(config.simulation.rngRun);
    ConfigureScenarioLogging(config.logging);

    Config::SetDefault("ns3::TcpL4Protocol::SocketType",
                       TypeIdValue(ResolveTcpCongestionType(config.tcp.congestionControl)));
    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(config.tcp.segmentSizeBytes));
    Config::SetDefault("ns3::TcpSocket::SndBufSize", UintegerValue(config.tcp.sendBufferBytes));
    Config::SetDefault("ns3::TcpSocket::RcvBufSize", UintegerValue(config.tcp.receiveBufferBytes));

    std::cout << "=== ns-3 configurable LLM trace replay ===\n";
    std::cout << "Config file: " << resolvedPaths.configFile << '\n';
    std::cout << "Trace file: " << resolvedPaths.traceFile << '\n';
    std::cout << "Run folder: " << resolvedPaths.runFolder << '\n';
    std::cout << "Output file: " << resolvedPaths.outputFile << '\n';
    std::cout << "Duration: " << config.simulation.durationMode << ", maximum "
              << maximumDurationMs / 1000.0 << " s\n";
    std::cout << "RNG: seed " << config.simulation.rngSeed << ", run " << config.simulation.rngRun
              << '\n';
    std::cout << "Topology: " << config.topology.bssCount << " BSSs x "
              << config.topology.stationsPerBss << " STAs, AP sink port "
              << config.topology.apSinkPort << ", STA sink base port "
              << config.topology.stationSinkBasePort << '\n';
    std::cout << "Channel model: "
              << (config.topology.isolateBssChannels ? "separate YansWifiChannel per AP group"
                                                     : "shared YansWifiChannel across AP groups")
              << '\n';
    std::cout << "Distribution: max " << config.distribution.maxAgentsPerStation << " agents/STA, "
              << (config.distribution.lowContentionPriority ? "low-contention priority"
                                                            : "STA-use priority")
              << ", " << config.distribution.slotMs << " ms slots\n";
    std::cout << "Wi-Fi: 802.11ax, " << config.wifi.band << ", channel "
              << config.wifi.channelNumber << ", " << config.wifi.bandwidthMhz
              << " MHz, primary-20 index " << static_cast<uint32_t>(config.wifi.primary20Index)
              << ", rate manager " << config.wifi.rateManager << '\n';
    std::cout << "TCP: " << config.tcp.congestionControl << ", segment "
              << config.tcp.segmentSizeBytes << " bytes, send/receive buffers "
              << config.tcp.sendBufferBytes << '/' << config.tcp.receiveBufferBytes << " bytes\n";
    std::cout << "Statistics: " << config.statistics.windowMs << " ms windows" << std::endl;

    const double traceDurationMs = parsedTrace.experimentDurationMs;
    TrafficCoordinator trafficCoordinator(traceDurationMs, maximumDurationMs);
    ExperimentStatistics experimentStatistics(trafficCoordinator, config.statistics.windowMs);

    ContentionAwareDistributionConfig distributionConfig;
    distributionConfig.nAp = config.topology.bssCount;
    distributionConfig.nStationsPerAp = config.topology.stationsPerBss;
    distributionConfig.maxAgentsPerStation = config.distribution.maxAgentsPerStation;
    distributionConfig.lowContentionPriority = config.distribution.lowContentionPriority;
    distributionConfig.slotMs = config.distribution.slotMs;

    DistributionResult distribution;
    try
    {
        distribution = DistributeAgentsContentionAware(parsedTrace, distributionConfig);
    }
    catch (const std::invalid_argument& error)
    {
        std::cerr << "error: agent distribution failed: " << error.what() << "\n\n";
        PrintScenarioUsage(std::cerr, argv[0]);
        return 1;
    }

    Ptr<YansWifiChannel> sharedChannel;
    if (!config.topology.isolateBssChannels)
    {
        sharedChannel = CreateDefaultYansChannel();
    }

    for (int bssIndex = 0; bssIndex < config.topology.bssCount; ++bssIndex)
    {
        SetupApGroup(bssIndex,
                     config.topology,
                     config.wifi,
                     sharedChannel,
                     distribution.apStationMaps[bssIndex],
                     distribution.apAgentMaps[bssIndex],
                     distribution.apAddresses[bssIndex],
                     trafficCoordinator,
                     experimentStatistics);
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

    experimentStatistics.ConnectDeviceTraces();

    const auto wallClockStart = std::chrono::steady_clock::now();

    Simulator::Run();

    const auto wallClockEnd = std::chrono::steady_clock::now();
    const double wallClockSeconds =
        std::chrono::duration<double>(wallClockEnd - wallClockStart).count();

    NS_ABORT_MSG_IF(trafficCoordinator.GetExperimentStartUs() < 0,
                    "Simulation ended before the global traffic barrier opened");

    try
    {
        experimentStatistics.Finalize();
        experimentStatistics.WriteExperimentJson(resolvedPaths.outputFile.string(), config);
    }
    catch (const std::exception& error)
    {
        Simulator::Destroy();
        std::cerr << "error: cannot write experiment output '" << resolvedPaths.outputFile.string()
                  << "': " << error.what() << std::endl;
        return 1;
    }

    Simulator::Destroy();

    std::cout << "\n=== Simulation Complete ===" << std::endl;
    std::cout << "Total events: " << Simulator::GetEventCount() << std::endl;
    NS_LOG_INFO("[Realtime] Simulator::Run wall-clock time: " << std::fixed << std::setprecision(3)
                                                              << wallClockSeconds << " seconds");
    std::cout << "Realtime simulation runtime: " << std::fixed << std::setprecision(3)
              << wallClockSeconds << " seconds" << std::endl;

    return 0;
}
