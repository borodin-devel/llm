#ifndef SCENARIO_CONFIG_H
#define SCENARIO_CONFIG_H

#include "ns3/log.h"
#include "ns3/type-id.h"
#include "ns3/wifi-phy-band.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ns3
{

/** Configuration values for general scenario input and output. */
struct GeneralConfig
{
    std::string traceFile;                 ///< Input JSON trace path.
    std::optional<std::string> runFolder;  ///< Optional output directory path.
    std::string outputName{"output.json"}; ///< Statistics JSON filename.
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
    double fixedDurationSeconds{0.0};              ///< Fixed duration in seconds.
    double autoTailSeconds{2.0};                   ///< Extra automatic-duration tail in seconds.
    uint32_t rngSeed{12345};                       ///< ns-3 random-number seed.
    uint64_t rngRun{1};                            ///< ns-3 random-number run number.
};

/** Configuration values that control the BSS topology. */
struct TopologyConfig
{
    int bssCount{3};                    ///< Number of AP/BSS groups.
    int stationsPerBss{30};             ///< Number of stations per BSS.
    double bssSpacingM{100.0};          ///< AP spacing in meters.
    double stationRadiusM{5.0};         ///< Station-disc radius in meters.
    bool isolateBssChannels{true};      ///< Whether BSS groups use separate channels.
    std::string ssidPrefix{"llm-ap-"};  ///< Prefix used to build BSS SSIDs.
    uint16_t apSinkPort{10000};         ///< TCP sink port on each AP.
    uint16_t stationSinkBasePort{9000}; ///< First TCP sink port for a station.
    double generatorStartSeconds{1.0};  ///< Generator start time in seconds.
};

/** Configuration values that control agent distribution. */
struct DistributionConfig
{
    int maxAgentsPerStation{832};     ///< Agent cap per physical station; zero is unlimited.
    bool lowContentionPriority{true}; ///< Whether to minimize contention first.
    int slotMs{10};                   ///< Uplink-overlap slot width in milliseconds.
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
    int bandwidthMhz{20};      ///< Channel width in MHz.
    uint8_t primary20Index{0}; ///< Primary 20 MHz subchannel index.
    std::string rateManager{"ns3::MinstrelHtWifiManager"}; ///< Wi-Fi rate-manager TypeId name.
    bool activeProbing{true}; ///< Whether stations actively probe for the configured SSID.
};

/** Configuration values that control fixed TCP transport. */
struct TcpConfig
{
    std::string congestionControl{"ns3::TcpHighSpeed"}; ///< TCP congestion-control TypeId name.
    uint32_t segmentSizeBytes{1460};                    ///< TCP maximum segment payload in bytes.
    uint32_t sendBufferBytes{32 * 1024 * 1024};         ///< TCP send buffer size in bytes.
    uint32_t receiveBufferBytes{32 * 1024 * 1024};      ///< TCP receive buffer size in bytes.
};

/** Configuration values that control statistics collection. */
struct StatisticsConfig
{
    uint32_t windowMs{10}; ///< Sparse PHY statistics window width in milliseconds.
};

/** Configuration values that control component logging. */
struct LoggingConfig
{
    std::string sampleScenarioLevel{"info"};         ///< SampleScenario log level.
    std::string apGeneratorLevel{"warn"};            ///< APGenerator log level.
    std::string staGeneratorLevel{"warn"};           ///< StaLlmGenerator log level.
    std::string trafficSinkLevel{"warn"};            ///< TrafficSink log level.
    std::string contentionDistributionLevel{"info"}; ///< Contention distribution log level.
};

/** Typed configuration values for the sample scenario. */
struct ScenarioConfig
{
    GeneralConfig general;           ///< General input and output configuration.
    SimulationConfig simulation;     ///< Simulation execution configuration.
    TopologyConfig topology;         ///< BSS topology configuration.
    DistributionConfig distribution; ///< Agent distribution configuration.
    WifiConfig wifi;                 ///< Fixed 802.11ax Wi-Fi configuration.
    TcpConfig tcp;                   ///< Fixed TCP transport configuration.
    StatisticsConfig statistics;     ///< Statistics collection configuration.
    LoggingConfig logging;           ///< Component logging configuration.
};

/**
 * Validate a fully merged typed scenario configuration.
 *
 * @param config Configuration after defaults, TOML, and CLI overrides.
 * @throws ScenarioConfigError if a field or cross-field relation is invalid.
 */
void ValidateScenarioConfig(const ScenarioConfig& config);

/**
 * Convert a configured Wi-Fi band to the ns-3 runtime band.
 *
 * @param band Configured Wi-Fi band.
 * @return Corresponding ns-3 Wi-Fi PHY band.
 * @throws ScenarioConfigError if the configured enum value is invalid.
 */
WifiPhyBand ToWifiPhyBand(WifiBandConfig band);

/**
 * Parse a configured scenario log level.
 *
 * @param level Configured log-level name.
 * @return ns-3 log level, or no value when logging is off.
 * @throws ScenarioConfigError if the level name is invalid.
 */
std::optional<LogLevel> ParseScenarioLogLevel(std::string_view level);

/**
 * Enable scenario log components at their configured levels.
 *
 * Components configured as off cause no LogComponentEnable() call and retain their existing state.
 *
 * @param logging Scenario component logging configuration.
 * @throws ScenarioConfigError if a level name is invalid.
 */
void ConfigureScenarioLogging(const LoggingConfig& logging);

/**
 * Resolve and validate a Wi-Fi rate-manager TypeId.
 *
 * @param name Fully qualified TypeId name.
 * @return TypeId derived from WifiRemoteStationManager.
 * @throws ScenarioConfigError if the TypeId is missing or has the wrong parent.
 */
TypeId ResolveWifiManagerType(std::string_view name);

/**
 * Resolve and validate a TCP congestion-control TypeId.
 *
 * @param name Fully qualified TypeId name.
 * @return TypeId derived from TcpCongestionOps.
 * @throws ScenarioConfigError if the TypeId is missing or has the wrong parent.
 */
TypeId ResolveTcpCongestionType(std::string_view name);

/** Scalar categories supported by scenario configuration options. */
enum class ConfigValueType
{
    STRING,  ///< TOML string value.
    INTEGER, ///< TOML integer value.
    FLOAT,   ///< TOML floating-point value.
    BOOLEAN, ///< TOML Boolean value.
    ENUM,    ///< TOML string selected from an enumerated set.
};

/** Public metadata for one scenario configuration option. */
struct ConfigOptionInfo
{
    std::string_view tomlPath;    ///< Dotted TOML key.
    std::string_view cliFlag;     ///< Derived section-prefixed CLI flag.
    ConfigValueType valueType;    ///< Required scalar category.
    std::string_view description; ///< Concise help and diagnostic text.
};

/** Error raised while parsing or applying scenario configuration. */
class ScenarioConfigError : public std::runtime_error
{
  public:
    /**
     * Construct a scenario configuration error.
     *
     * @param message Error description.
     */
    explicit ScenarioConfigError(const std::string& message);
};

/**
 * Get metadata projected from the private scenario option registry.
 *
 * @return Complete scenario option metadata.
 */
const std::vector<ConfigOptionInfo>& GetScenarioConfigOptionInfo();

/**
 * Load a scenario configuration from a strict TOML document.
 *
 * Omitted optional values retain their compiled typed defaults.
 *
 * @param path TOML document path.
 * @return Parsed typed scenario configuration.
 * @throws ScenarioConfigError if parsing or schema application fails.
 */
ScenarioConfig LoadTomlConfig(const std::filesystem::path& path);

/** Inputs retained from a successfully parsed scenario launch. */
struct ScenarioLaunchConfig
{
    ScenarioConfig scenario;                ///< Scenario settings after TOML and CLI application.
    std::filesystem::path configFile;       ///< Resolved TOML configuration path.
    std::filesystem::path workingDirectory; ///< Working directory captured at launch.
};

/** Filesystem paths resolved for one scenario run. */
struct ResolvedRunPaths
{
    std::filesystem::path configFile;   ///< Resolved TOML configuration path.
    std::filesystem::path traceFile;    ///< Resolved input trace path.
    std::filesystem::path runFolder;    ///< Directory that owns run output.
    std::filesystem::path outputFile;   ///< Final statistics output path.
    bool usesAutomaticRunFolder{false}; ///< Whether the run folder uses a launch timestamp.
};

/**
 * Resolve scenario paths against the captured launch working directory.
 *
 * @param launch Validated scenario and captured launch paths.
 * @param launchTime Launch time used for an automatic run folder.
 * @return Normalized absolute paths for the scenario run.
 * @throws ScenarioConfigError if the launch time cannot be converted to local time.
 */
ResolvedRunPaths ResolveRunPaths(const ScenarioLaunchConfig& launch,
                                 std::chrono::system_clock::time_point launchTime);

/**
 * Validate the trace and prepare a run directory without overwriting output.
 *
 * @param paths Resolved paths for the scenario run.
 * @throws ScenarioConfigError if a path is invalid, collides, or cannot be accessed.
 */
void PrepareRunDirectory(const ResolvedRunPaths& paths);

/** Result of parsing sample-scenario command-line arguments. */
struct ScenarioCommandLineResult
{
    bool valid{false};           ///< Whether the request is valid.
    bool printUsage{false};      ///< Whether the caller should print usage.
    ScenarioLaunchConfig launch; ///< Launch inputs when a real launch is valid.
    std::string error;           ///< Parsing error, or empty on success.
};

/**
 * Parse scenario command-line arguments excluding the executable name.
 *
 * @param arguments Ordered command-line arguments.
 * @param workingDirectory Working directory used to resolve the configuration path.
 * @return Parsed launch inputs, a help request, or an error.
 */
ScenarioCommandLineResult ParseScenarioArguments(const std::vector<std::string>& arguments,
                                                 const std::filesystem::path& workingDirectory);

/**
 * Print sample-scenario command-line usage.
 *
 * @param output Destination stream.
 * @param programName Executable name shown in the command synopsis.
 */
void PrintScenarioUsage(std::ostream& output, const std::string& programName);

} // namespace ns3

#endif // SCENARIO_CONFIG_H
