// examples/saturated-tcp-scenario.cc
// Deterministic routed three-BSS saturated TCP calibration benchmark.

#include "saturated-tcp/benchmark-statistics.h"
#include "saturated-tcp/config.h"
#include "saturated-tcp/log.h"
#include "saturated-tcp/readiness-barrier.h"
#include "saturated-tcp/topology.h"
#include "saturated-tcp/traffic.h"

#include "ns3/config.h"
#include "ns3/ipv4.h"
#include "ns3/rng-seed-manager.h"
#include "ns3/simulator.h"
#include "ns3/tcp-l4-protocol.h"
#include "ns3/tcp-socket.h"
#include "ns3/type-id.h"
#include "ns3/uinteger.h"
#include "ns3/wifi-net-device.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace ns3;

namespace
{

/** Output paths prepared for one direct scenario run. */
struct SaturatedOutputPaths
{
    std::filesystem::path runFolder;  ///< Directory that owns the JSON output.
    std::filesystem::path outputFile; ///< Exclusive JSON destination.
};

/** Observable completion state returned after one simulation run. */
struct SaturatedRunResult
{
    int64_t experimentStartNs;        ///< Readiness-selected epoch.
    uint32_t registeredSenders;       ///< Number of installed TCP senders.
    uint32_t readySenders;            ///< Number of ready TCP senders.
    SaturatedOutputPaths outputPaths; ///< Prepared and written output paths.
};

/**
 * Convert an IPv4 address to canonical dotted notation.
 *
 * @param address Address to format.
 * @return Canonical string form.
 */
std::string
Ipv4ToString(Ipv4Address address)
{
    std::ostringstream output;
    address.Print(output);
    return output.str();
}

/**
 * Resolve the Wi-Fi address installed on one access point.
 *
 * @param bss BSS topology to inspect.
 * @return Access-point Wi-Fi IPv4 address.
 * @throws std::runtime_error if the Wi-Fi interface or address is absent.
 */
Ipv4Address
GetAccessPointAddress(const SaturatedTcpBssTopology& bss)
{
    const auto ipv4 = bss.accessPointNode->GetObject<Ipv4>();
    const int32_t interface = ipv4->GetInterfaceForDevice(bss.accessPointDevice);
    if (interface < 0 || ipv4->GetNAddresses(static_cast<uint32_t>(interface)) == 0)
    {
        throw std::runtime_error("saturated access point has no Wi-Fi IPv4 interface");
    }
    return ipv4->GetAddress(static_cast<uint32_t>(interface), 0).GetLocal();
}

/**
 * Register deterministic topology identities and station-only trace connections.
 *
 * @param topology Built three-BSS topology.
 * @param statistics Benchmark statistics owner.
 */
void
RegisterStatistics(const SaturatedTcpTopology& topology, SaturatedTcpStatistics& statistics)
{
    for (const auto& bss : topology.bss)
    {
        statistics.RegisterAccessPoint(bss.bssId,
                                       bss.accessPointNode->GetId(),
                                       "AP" + std::to_string(bss.bssId),
                                       Ipv4ToString(GetAccessPointAddress(bss)));
        for (uint32_t stationIndex = 0; stationIndex < bss.stationNodes.GetN(); ++stationIndex)
        {
            const auto stationNode = bss.stationNodes.Get(stationIndex);
            statistics.RegisterStation(
                bss.bssId,
                stationIndex,
                stationNode->GetId(),
                "AP" + std::to_string(bss.bssId) + "/STA" + std::to_string(stationIndex),
                Ipv4ToString(bss.stationInterfaces.GetAddress(stationIndex)));
            const auto stationDevice =
                DynamicCast<WifiNetDevice>(bss.stationDevices.Get(stationIndex));
            statistics.ConnectStation(stationDevice);
        }
    }
}

/**
 * Project built topology addresses into the Task 7 flow endpoint contract.
 *
 * @param topology Built three-BSS topology.
 * @return Server and ordered station endpoints for all BSSs.
 */
std::array<SaturatedTcpBssEndpoints, 3>
BuildTrafficEndpoints(const SaturatedTcpTopology& topology)
{
    std::array<SaturatedTcpBssEndpoints, 3> endpoints;
    for (uint32_t bssIndex = 0; bssIndex < topology.bss.size(); ++bssIndex)
    {
        const auto& bss = topology.bss.at(bssIndex);
        auto& bssEndpoints = endpoints.at(bssIndex);
        bssEndpoints.server = {bss.serverNode, bss.serverAddress};
        bssEndpoints.stations.reserve(bss.stationNodes.GetN());
        for (uint32_t stationIndex = 0; stationIndex < bss.stationNodes.GetN(); ++stationIndex)
        {
            bssEndpoints.stations.push_back({bss.stationNodes.Get(stationIndex),
                                             bss.stationInterfaces.GetAddress(stationIndex)});
        }
    }
    return endpoints;
}

/**
 * Create a directory and report path-bearing filesystem failures.
 *
 * @param path Directory to create, including missing parents.
 */
void
CreateDirectories(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::create_directories(path, error);
    if (error)
    {
        throw std::runtime_error("cannot create saturated run folder '" + path.string() +
                                 "': " + error.message());
    }
}

/**
 * Resolve and create one explicit or automatic output directory.
 *
 * @param config Effective benchmark configuration.
 * @return Absolute run folder and exclusive output destination.
 */
SaturatedOutputPaths
PrepareOutputPaths(const SaturatedTcpConfig& config)
{
    const auto workingDirectory = std::filesystem::current_path();
    std::filesystem::path runFolder;
    if (config.general.runFolder)
    {
        runFolder = *config.general.runFolder;
        if (runFolder.is_relative())
        {
            runFolder = workingDirectory / runFolder;
        }
        runFolder = runFolder.lexically_normal();
        CreateDirectories(runFolder);
    }
    else
    {
        const auto parent = (workingDirectory / "run").lexically_normal();
        CreateDirectories(parent);
        const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();
        runFolder = parent / ("saturated_tcp_" + std::to_string(timestamp));
        std::error_code error;
        const bool created = std::filesystem::create_directory(runFolder, error);
        if (error || !created)
        {
            throw std::runtime_error("cannot exclusively create saturated run folder '" +
                                     runFolder.string() +
                                     "': " + (error ? error.message() : "path already exists"));
        }
    }

    std::error_code error;
    const bool directory = std::filesystem::is_directory(runFolder, error);
    if (error || !directory)
    {
        throw std::runtime_error("saturated run folder is not a directory: '" + runFolder.string() +
                                 "'");
    }
    return {runFolder, (runFolder / config.general.outputName).lexically_normal()};
}

/**
 * Print the exact effective one-run configuration without metric reports.
 *
 * @param config Effective benchmark configuration.
 */
void
PrintConfiguration(const SaturatedTcpConfig& config)
{
    std::cout << "=== ns-3 saturated TCP Wi-Fi benchmark ===\n";
    std::cout << "Topology: 3 dedicated server--AP BSSs x " << config.benchmark.stationCountPerBss
              << " STAs\n";
    std::cout << "Matrix: RSSI " << config.benchmark.rssiRange << " ("
              << GetSaturatedTcpStationTargetRssiDbm(config.benchmark.rssiRange)
              << " dBm), interference " << config.benchmark.interferenceMode << ", traffic "
              << config.benchmark.trafficMode << ", MIMO " << config.benchmark.mimoMode << '\n';
    std::cout << "Wi-Fi: 802.11ax, " << config.wifi.band << " channel " << config.wifi.channelNumber
              << ", " << config.wifi.bandwidthMhz << " MHz, primary-20 "
              << static_cast<uint32_t>(config.wifi.primary20Index) << ", " << config.wifi.txPowerDbm
              << " dBm, " << config.wifi.rateManager << ", "
              << static_cast<uint32_t>(config.wifi.antennas) << 'x'
              << static_cast<uint32_t>(config.wifi.antennas) << " SU\n";
    std::cout << "TCP/backhaul: " << config.tcp.congestionControl << ", segment "
              << config.tcp.segmentSizeBytes << " bytes, " << config.tcp.wiredRate << "/"
              << config.tcp.wiredDelay << '\n';
    std::cout << "Statistics: exactly 1 s in " << config.statistics.windowMs << " ms windows; RNG "
              << config.simulation.rngSeed << '/' << config.simulation.rngRun << std::endl;
}

/**
 * Execute one fully configured saturated benchmark run.
 *
 * @param config Effective benchmark configuration.
 * @return Readiness and written-output state.
 */
SaturatedRunResult
RunScenario(const SaturatedTcpConfig& config)
{
    RngSeedManager::SetSeed(config.simulation.rngSeed);
    RngSeedManager::SetRun(config.simulation.rngRun);
    saturated_tcp_example::ConfigureScenarioLogging(config.logging.scenarioLevel);

    Config::SetDefault("ns3::TcpL4Protocol::SocketType",
                       TypeIdValue(TypeId::LookupByName(config.tcp.congestionControl)));
    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(config.tcp.segmentSizeBytes));
    Config::SetDefault("ns3::TcpSocket::SndBufSize", UintegerValue(config.tcp.sendBufferBytes));
    Config::SetDefault("ns3::TcpSocket::RcvBufSize", UintegerValue(config.tcp.receiveBufferBytes));

    PrintConfiguration(config);

    SaturatedRunResult result{};
    {
        const auto topology = BuildSaturatedTcpTopology(config);
        std::cout << "Placement: AP triangle side " << topology.accessPointDistanceM << " m ("
                  << GetSaturatedTcpAccessPointTargetRssiDbm() << " dBm), station radius "
                  << topology.stationDistanceM << " m ("
                  << GetSaturatedTcpStationTargetRssiDbm(config.benchmark.rssiRange) << " dBm)\n";

        SaturatedTcpStatistics statistics(config.statistics.windowMs);
        RegisterStatistics(topology, statistics);
        SaturatedReadinessBarrier barrier(
            MakeCallback(&SaturatedTcpStatistics::Start, &statistics),
            MakeCallback(&SaturatedTcpStatistics::Finalize, &statistics));
        const auto endpoints = BuildTrafficEndpoints(topology);
        const auto traffic = InstallSaturatedTcpTraffic(endpoints, config, barrier);
        barrier.FinalizeRegistration();
        std::cout << "Readiness: waiting for " << traffic.flows.size() << " independent TCP senders"
                  << std::endl;

        Simulator::Run();
        if (!barrier.IsMeasurementComplete() || barrier.GetExperimentStartNs() < 0)
        {
            throw std::runtime_error(
                "simulation ended before the saturated one-second measurement completed");
        }

        const auto summary = statistics.BuildSummary();
        result.outputPaths = PrepareOutputPaths(config);
        WriteSaturatedTcpExperimentJson(result.outputPaths.outputFile.string(), summary, config);
        result.experimentStartNs = barrier.GetExperimentStartNs();
        result.registeredSenders = barrier.GetRegisteredSenderCount();
        result.readySenders = barrier.GetReadySenderCount();
    }
    Simulator::Destroy();
    return result;
}

} // namespace

int
main(int argc, char* argv[])
{
    try
    {
        const auto config = ParseSaturatedTcpConfig(argc, argv);
        const auto result = RunScenario(config);
        std::cout << "Readiness complete: " << result.readySenders << '/'
                  << result.registeredSenders << "; measurement epoch "
                  << static_cast<double>(result.experimentStartNs) / 1e9
                  << " s; duration exactly 1 s\n";
        std::cout << "Output: " << result.outputPaths.outputFile << std::endl;
        return 0;
    }
    catch (const std::exception& error)
    {
        Simulator::Destroy();
        std::cerr << "error: saturated TCP scenario failed: " << error.what() << std::endl;
        return 1;
    }
}
