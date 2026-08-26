#ifndef SATURATED_TCP_CONFIG_H
#define SATURATED_TCP_CONFIG_H

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <stdexcept>
#include <string>

namespace ns3
{

class JsonWriter;

/** RSSI operating points in the saturated benchmark matrix. */
enum class SaturatedRssiRange
{
    HIGH,   ///< High-RSSI operating point.
    MEDIUM, ///< Medium-RSSI operating point.
    LOW,    ///< Low-RSSI operating point.
};

/** Inter-BSS interference modes in the saturated benchmark matrix. */
enum class SaturatedInterferenceMode
{
    ISOLATED,          ///< Place each BSS on an independent channel.
    AP_ONLY_COCHANNEL, ///< Share one channel while allowing only AP-to-AP cross-BSS links.
};

/** Traffic directions in the saturated benchmark matrix. */
enum class SaturatedTrafficMode
{
    UL,    ///< Saturated uplink traffic only.
    DL,    ///< Saturated downlink traffic only.
    UL_DL, ///< Simultaneous saturated uplink and downlink traffic.
};

/** Spatial-transmission modes exposed by saturated configuration metadata. */
enum class SaturatedMimoMode
{
    SU, ///< Single-user MIMO.
    MU, ///< Multi-user MIMO, currently unsupported by the benchmark.
};

/**
 * Stream an RSSI range name.
 *
 * @param output Destination stream.
 * @param value RSSI range to write.
 * @return Destination stream.
 */
std::ostream& operator<<(std::ostream& output, SaturatedRssiRange value);

/**
 * Stream an interference mode name.
 *
 * @param output Destination stream.
 * @param value Interference mode to write.
 * @return Destination stream.
 */
std::ostream& operator<<(std::ostream& output, SaturatedInterferenceMode value);

/**
 * Stream a traffic mode name.
 *
 * @param output Destination stream.
 * @param value Traffic mode to write.
 * @return Destination stream.
 */
std::ostream& operator<<(std::ostream& output, SaturatedTrafficMode value);

/**
 * Stream a MIMO mode name.
 *
 * @param output Destination stream.
 * @param value MIMO mode to write.
 * @return Destination stream.
 */
std::ostream& operator<<(std::ostream& output, SaturatedMimoMode value);

/** General saturated benchmark output configuration. */
struct SaturatedGeneralConfig
{
    std::string outputName{"output.json"}; ///< Statistics JSON filename.
    std::optional<std::string> runFolder;  ///< Optional exact output directory.
};

/** Matrix-runner metadata accepted by the one-run C++ executable. */
struct SaturatedScriptConfig
{
    uint32_t repetitions{1}; ///< Number of attempts for the external runner; never looped in C++.
};

/** Random-number configuration for one saturated benchmark run. */
struct SaturatedSimulationConfig
{
    uint32_t rngSeed{12345}; ///< ns-3 random-number seed.
    uint64_t rngRun{1};      ///< ns-3 run/substream number.
};

/** Matrix coordinates for one saturated benchmark run. */
struct SaturatedBenchmarkConfig
{
    uint32_t stationCountPerBss{5}; ///< Number of stations in each of the three BSSs.
    SaturatedRssiRange rssiRange{SaturatedRssiRange::HIGH}; ///< Target RSSI range.
    SaturatedInterferenceMode interferenceMode{
        SaturatedInterferenceMode::ISOLATED};                   ///< Inter-BSS interference mode.
    SaturatedTrafficMode trafficMode{SaturatedTrafficMode::UL}; ///< Traffic direction.
    SaturatedMimoMode mimoMode{SaturatedMimoMode::SU};          ///< Spatial mode.
};

/** Fixed 802.11ax radio configuration for the saturated benchmark. */
struct SaturatedWifiConfig
{
    std::string band{"5GHz"};   ///< Wi-Fi operating band.
    uint16_t channelNumber{42}; ///< IEEE channel number.
    uint16_t bandwidthMhz{80};  ///< Channel width in MHz.
    uint8_t primary20Index{0};  ///< Primary 20 MHz subchannel index.
    double txPowerDbm{20.0};    ///< Fixed transmit power in dBm.
    std::string rateManager{
        "ns3::MinstrelHtWifiManager"}; ///< Registered Wi-Fi rate-manager TypeId.
    uint8_t antennas{2};               ///< Number of antennas on every AP and station.
    uint8_t maxTxSpatialStreams{2};    ///< Maximum transmit spatial streams.
    uint8_t maxRxSpatialStreams{2};    ///< Maximum receive spatial streams.
};

/** TCP and wired-backhaul configuration for the saturated benchmark. */
struct SaturatedTcpTransportConfig
{
    std::string congestionControl{"ns3::TcpHighSpeed"}; ///< TCP congestion-control TypeId.
    uint32_t segmentSizeBytes{1460};                    ///< TCP maximum segment payload.
    uint32_t sendBufferBytes{32 * 1024 * 1024};         ///< TCP send-buffer size.
    uint32_t receiveBufferBytes{32 * 1024 * 1024};      ///< TCP receive-buffer size.
    std::string wiredRate{"10Gbps"};                    ///< Dedicated wired-link data rate.
    std::string wiredDelay{"0.1ms"};                    ///< Dedicated wired-link delay.
};

/** Statistics configuration for the saturated benchmark. */
struct SaturatedStatisticsConfig
{
    uint32_t windowMs{10}; ///< Statistics window width in milliseconds.
};

/** Component logging configuration for the saturated benchmark. */
struct SaturatedLoggingConfig
{
    std::string scenarioLevel{"info"}; ///< Saturated scenario log level.
};

/** Complete independent configuration for one saturated TCP benchmark run. */
struct SaturatedTcpConfig
{
    SaturatedGeneralConfig general;       ///< Output configuration.
    SaturatedScriptConfig script;         ///< External-runner metadata.
    SaturatedSimulationConfig simulation; ///< Random-number configuration.
    SaturatedBenchmarkConfig benchmark;   ///< Matrix coordinates.
    SaturatedWifiConfig wifi;             ///< Fixed Wi-Fi configuration.
    SaturatedTcpTransportConfig tcp;      ///< TCP and wired-link configuration.
    SaturatedStatisticsConfig statistics; ///< Statistics configuration.
    SaturatedLoggingConfig logging;       ///< Logging configuration.
};

/** Error raised by saturated benchmark configuration parsing or validation. */
class SaturatedTcpConfigError : public std::runtime_error
{
  public:
    /**
     * Construct a saturated configuration error.
     *
     * @param message Error description.
     */
    explicit SaturatedTcpConfigError(const std::string& message);
};

/**
 * Validate a fully merged saturated benchmark configuration.
 *
 * @param config Configuration after defaults, TOML, and CLI overrides.
 * @throws SaturatedTcpConfigError if a value or cross-field relation is invalid.
 */
void ValidateSaturatedTcpConfig(const SaturatedTcpConfig& config);

/**
 * Parse one saturated benchmark invocation.
 *
 * The argument vector includes the executable name. Exactly one explicit
 * `--config` path is required. TOML is loaded before all CLI overrides are applied.
 *
 * @param argc Argument count.
 * @param argv Argument vector including the executable name.
 * @return Validated configuration for exactly one simulation run.
 * @throws SaturatedTcpConfigError if command-line, TOML, or validation fails.
 */
SaturatedTcpConfig ParseSaturatedTcpConfig(int argc, char** argv);

/**
 * Write ordered effective saturated configuration metadata.
 *
 * @param writer Structured JSON writer.
 * @param config Valid saturated benchmark configuration.
 */
void WriteEffectiveSaturatedTcpConfigurationJson(JsonWriter& writer,
                                                 const SaturatedTcpConfig& config);

} // namespace ns3

#endif // SATURATED_TCP_CONFIG_H
