#ifndef SCENARIO_CONFIG_H
#define SCENARIO_CONFIG_H

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace ns3
{

/** Configuration values for general scenario input and output. */
struct GeneralConfig
{
    std::string traceFile; ///< Input JSON trace path.
    std::optional<std::string> runFolder; ///< Optional output directory path.
    std::string outputName{"mac-node-stats.json"}; ///< Statistics JSON filename.
};

/** Experiment duration selection policy. */
enum class DurationMode
{
    AUTO,  ///< Derive duration from the input trace.
    FIXED, ///< Use a configured duration.
};

/**
 * Stream a duration mode name.
 *
 * @param output Destination stream.
 * @param durationMode Duration mode to write.
 * @return Destination stream.
 */
std::ostream& operator<<(std::ostream& output, DurationMode durationMode);

/** Configuration values that control simulation execution. */
struct SimulationConfig
{
    DurationMode durationMode{DurationMode::AUTO}; ///< Experiment duration policy.
    double fixedDurationSeconds{0.0}; ///< Fixed duration in seconds.
    double autoTailSeconds{2.0}; ///< Extra automatic-duration tail in seconds.
    uint32_t rngSeed{12345}; ///< ns-3 random-number seed.
    uint64_t rngRun{1}; ///< ns-3 random-number run number.
};

/** Configuration values that control the BSS topology. */
struct TopologyConfig
{
    int bssCount{3}; ///< Number of AP/BSS groups.
    int stationsPerBss{30}; ///< Number of stations per BSS.
    double bssSpacingM{100.0}; ///< AP spacing in meters.
    double stationRadiusM{5.0}; ///< Station-disc radius in meters.
    bool isolateBssChannels{true}; ///< Whether BSS groups use separate channels.
    std::string ssidPrefix{"llm-ap-"}; ///< Prefix used to build BSS SSIDs.
    uint16_t apSinkPort{10000}; ///< TCP sink port on each AP.
    uint16_t stationSinkBasePort{9000}; ///< First TCP sink port for a station.
    double generatorStartSeconds{1.0}; ///< Generator start time in seconds.
};

/** Configuration values that control agent distribution. */
struct DistributionConfig
{
    int maxAgentsPerStation{832}; ///< Agent cap per physical station; zero is unlimited.
    bool lowContentionPriority{true}; ///< Whether to minimize contention first.
    int slotMs{10}; ///< Uplink-overlap slot width in milliseconds.
};

/** Operating bands supported by the fixed 802.11ax scenario. */
enum class WifiBandConfig
{
    BAND_2_4_GHZ, ///< 2.4 GHz operating band.
    BAND_5_GHZ,   ///< 5 GHz operating band.
    BAND_6_GHZ,   ///< 6 GHz operating band.
};

/**
 * Stream a Wi-Fi band name.
 *
 * @param output Destination stream.
 * @param band Wi-Fi band to write.
 * @return Destination stream.
 */
std::ostream& operator<<(std::ostream& output, WifiBandConfig band);

/** Configuration values that control the fixed 802.11ax Wi-Fi setup. */
struct WifiConfig
{
    WifiBandConfig band{WifiBandConfig::BAND_5_GHZ}; ///< Operating band.
    uint16_t channelNumber{0}; ///< IEEE channel number; zero selects the first valid channel.
    int bandwidthMhz{20}; ///< Channel width in MHz.
    uint8_t primary20Index{0}; ///< Primary 20 MHz subchannel index.
    std::string rateManager{"ns3::MinstrelHtWifiManager"}; ///< Wi-Fi rate-manager TypeId name.
    bool activeProbing{true}; ///< Whether stations actively probe for the configured SSID.
};

/** Configuration values that control fixed TCP transport. */
struct TcpConfig
{
    std::string congestionControl{"ns3::TcpHighSpeed"}; ///< TCP congestion-control TypeId name.
    uint32_t segmentSizeBytes{1460}; ///< TCP maximum segment payload in bytes.
    uint32_t sendBufferBytes{32 * 1024 * 1024}; ///< TCP send buffer size in bytes.
    uint32_t receiveBufferBytes{32 * 1024 * 1024}; ///< TCP receive buffer size in bytes.
};

/** Configuration values that control statistics collection. */
struct StatisticsConfig
{
    uint32_t windowMs{10}; ///< Sparse PHY statistics window width in milliseconds.
};

/** Configuration values that control component logging. */
struct LoggingConfig
{
    std::string sampleScenarioLevel{"info"}; ///< SampleScenario log level.
    std::string apGeneratorLevel{"warn"}; ///< APGenerator log level.
    std::string staGeneratorLevel{"warn"}; ///< StaLlmGenerator log level.
    std::string trafficSinkLevel{"warn"}; ///< TrafficSink log level.
    std::string contentionDistributionLevel{"info"}; ///< Contention distribution log level.
};

/** Typed configuration values for the sample scenario. */
struct ScenarioConfig
{
    GeneralConfig general; ///< General input and output configuration.
    SimulationConfig simulation; ///< Simulation execution configuration.
    TopologyConfig topology; ///< BSS topology configuration.
    DistributionConfig distribution; ///< Agent distribution configuration.
    WifiConfig wifi; ///< Fixed 802.11ax Wi-Fi configuration.
    TcpConfig tcp; ///< Fixed TCP transport configuration.
    StatisticsConfig statistics; ///< Statistics collection configuration.
    LoggingConfig logging; ///< Component logging configuration.
};

/** Result of parsing sample-scenario arguments. */
struct ScenarioArgumentResult
{
    bool valid{false}; ///< Whether config is ready for use.
    bool printUsage{false}; ///< Whether the caller should print usage.
    ScenarioConfig config; ///< Parsed settings and defaults.
    std::string error; ///< Validation error, or empty on success.
};

/**
 * Parse legacy positional command-line arguments excluding the executable name.
 *
 * @param arguments Ordered command-line arguments.
 * @return Parsed settings or a validation error.
 */
ScenarioArgumentResult ParseScenarioArguments(const std::vector<std::string>& arguments);

/**
 * Print sample-scenario command-line usage.
 *
 * @param output Destination stream.
 * @param programName Executable name shown in the command synopsis.
 */
void PrintScenarioUsage(std::ostream& output, const std::string& programName);

} // namespace ns3

#endif // SCENARIO_CONFIG_H
