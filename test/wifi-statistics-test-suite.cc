#include "../examples/scenario-config.h"
#include "../examples/traffic-coordinator.h"
#include "../examples/wifi-statistics-internal.h"
#include "../examples/wifi-statistics.h"
#include "llm-test-suite.h"

#include "ns3/json.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace ns3;

namespace
{

constexpr std::array<LogLevel, 10> g_publicLogComponentBits{LOG_ERROR,
                                                            LOG_WARN,
                                                            LOG_INFO,
                                                            LOG_FUNCTION,
                                                            LOG_LOGIC,
                                                            LOG_DEBUG,
                                                            LOG_PREFIX_FUNC,
                                                            LOG_PREFIX_TIME,
                                                            LOG_PREFIX_NODE,
                                                            LOG_PREFIX_LEVEL};

/**
 * Read the enabled public log and prefix bits for one component.
 *
 * @param component Log component to inspect.
 * @return Enabled public log and prefix bits.
 */
LogLevel
GetEnabledPublicBits(const LogComponent& component)
{
    LogLevel enabled = LOG_NONE;
    for (const auto bit : g_publicLogComponentBits)
    {
        if (component.IsEnabled(bit))
        {
            enabled = static_cast<LogLevel>(enabled | bit);
        }
    }
    return enabled;
}

/** Restore the exact public log state of a set of components on scope exit. */
class ScopedLogComponentState
{
  public:
    /**
     * Snapshot component log and prefix bits.
     *
     * @param componentNames Registered component names to preserve.
     */
    explicit ScopedLogComponentState(const std::vector<std::string>& componentNames)
    {
        m_savedStates.reserve(componentNames.size());
        for (const auto& name : componentNames)
        {
            auto& component = GetLogComponent(name);
            m_savedStates.emplace_back(&component, GetEnabledPublicBits(component));
        }
    }

    /** Restore all snapshotted component states. */
    ~ScopedLogComponentState()
    {
        for (const auto& [component, enabledBits] : m_savedStates)
        {
            component->Disable(LOG_LEVEL_ALL);
            component->Disable(LOG_PREFIX_ALL);
            component->Enable(enabledBits);
        }
    }

  private:
    std::vector<std::pair<LogComponent*, LogLevel>> m_savedStates; ///< Saved states by component.
};

/**
 * Set the enabled public bits for one registered component.
 *
 * @param name Registered component name.
 * @param enabledBits Public log and prefix bits to enable.
 */
void
SetEnabledPublicBits(const std::string& name, LogLevel enabledBits)
{
    auto& component = GetLogComponent(name);
    component.Disable(LOG_LEVEL_ALL);
    component.Disable(LOG_PREFIX_ALL);
    component.Enable(enabledBits);
}

/**
 * @ingroup tests
 *
 * Verify statistics window boundaries.
 */
class StatisticsWindowTestCase : public TestCase
{
  public:
    StatisticsWindowTestCase();

  private:
    void DoRun() override;
};

/**
 * @ingroup tests
 *
 * Verify statistics window indices and JSON timestamps beyond 32-bit milliseconds.
 */
class LongDurationStatisticsWindowTestCase : public TestCase
{
  public:
    LongDurationStatisticsWindowTestCase();

  private:
    void DoRun() override;
};

StatisticsWindowTestCase::StatisticsWindowTestCase()
    : TestCase("calculate statistics window boundaries")
{
}

LongDurationStatisticsWindowTestCase::LongDurationStatisticsWindowTestCase()
    : TestCase("preserve 64-bit statistics windows beyond 2^32 milliseconds")
{
}

void
StatisticsWindowTestCase::DoRun()
{
    uint64_t index = 999;
    NS_TEST_ASSERT_MSG_EQ(GetStatisticsWindowIndex(1000000, 1000000, 50.0, 25, index),
                          true,
                          "Epoch must be included");
    NS_TEST_ASSERT_MSG_EQ(index, 0, "Wrong first window");
    NS_TEST_ASSERT_MSG_EQ(GetStatisticsWindowIndex(1024999, 1000000, 50.0, 25, index),
                          true,
                          "Last microsecond of first window must be included");
    NS_TEST_ASSERT_MSG_EQ(index, 0, "Wrong boundary window");
    NS_TEST_ASSERT_MSG_EQ(GetStatisticsWindowIndex(1025000, 1000000, 50.0, 25, index),
                          true,
                          "Second window must be included");
    NS_TEST_ASSERT_MSG_EQ(index, 1, "Wrong second window");
    NS_TEST_ASSERT_MSG_EQ(GetStatisticsWindowIndex(1050000, 1000000, 50.0, 25, index),
                          false,
                          "End boundary must be excluded");
}

void
LongDurationStatisticsWindowTestCase::DoRun()
{
    constexpr uint64_t firstIndexBeyond32Bits = uint64_t{1} << 32;
    constexpr uint64_t durationMs = firstIndexBeyond32Bits + 1;
    constexpr int64_t bucketStartUs = static_cast<int64_t>(firstIndexBeyond32Bits * 1000);

    uint64_t windowIndex = 0;
    NS_TEST_ASSERT_MSG_EQ(
        GetStatisticsWindowIndex(bucketStartUs, 0, static_cast<double>(durationMs), 1, windowIndex),
        true,
        "Long-duration window was rejected");
    NS_TEST_ASSERT_MSG_EQ(windowIndex,
                          firstIndexBeyond32Bits,
                          "Statistics window index wrapped at 32 bits");

    TrafficCoordinator coordinator(static_cast<double>(durationMs),
                                   static_cast<double>(durationMs));
    WifiStatisticsState statistics(coordinator, 1);
    statistics.stationIpsByBss = {{"10.1.0.2"}};
    statistics.phyWindows[windowIndex][0].upBytes["10.1.0.2"] = 1;

    const std::string outputPath = CreateTempDirFilename("llm-long-duration-statistics.json");
    WriteWifiStatisticsJson(statistics, outputPath);

    std::ifstream input(outputPath);
    const nlohmann::json document = nlohmann::json::parse(input);
    NS_TEST_ASSERT_MSG_EQ(document.at("windows").size(),
                          1,
                          "64-bit sparse statistics bucket was not emitted");
    NS_TEST_ASSERT_MSG_EQ(document.at("windows").at(0).at("timestamp").get<uint64_t>(),
                          durationMs,
                          "64-bit statistics timestamp wrapped");
}

/**
 * @ingroup tests
 *
 * Verify configured levels are applied to the intended scenario components.
 */
class ScenarioLoggingTestCase : public TestCase
{
  public:
    ScenarioLoggingTestCase();

  private:
    void DoRun() override;
};

ScenarioLoggingTestCase::ScenarioLoggingTestCase()
    : TestCase("configure scenario logging components")
{
}

void
ScenarioLoggingTestCase::DoRun()
{
    const std::vector<std::string> componentNames{"SampleScenario",
                                                  "APGenerator",
                                                  "StaLlmGenerator",
                                                  "TrafficSink",
                                                  "ContentionAwareAgentDistribution"};
    const ScopedLogComponentState restoreOriginalState(componentNames);

    const std::array seededStates{
        std::pair{"SampleScenario", static_cast<LogLevel>(LOG_ERROR | LOG_PREFIX_FUNC)},
        std::pair{"APGenerator", static_cast<LogLevel>(LOG_WARN)},
        std::pair{"StaLlmGenerator", static_cast<LogLevel>(LOG_INFO | LOG_PREFIX_TIME)},
        std::pair{"TrafficSink", static_cast<LogLevel>(LOG_FUNCTION | LOG_LOGIC | LOG_PREFIX_NODE)},
        std::pair{"ContentionAwareAgentDistribution",
                  static_cast<LogLevel>(LOG_DEBUG | LOG_PREFIX_LEVEL)},
    };
    for (const auto& [name, enabledBits] : seededStates)
    {
        SetEnabledPublicBits(name, enabledBits);
    }

    {
        const ScopedLogComponentState restoreSeededState(componentNames);
        for (const auto& name : componentNames)
        {
            SetEnabledPublicBits(name, LOG_NONE);
        }

        LoggingConfig logging;
        logging.sampleScenarioLevel = "error";
        logging.apGeneratorLevel = "warn";
        logging.staGeneratorLevel = "function";
        logging.trafficSinkLevel = "logic";
        logging.contentionDistributionLevel = "off";
        ConfigureScenarioLogging(logging);

        const auto& scenario = GetLogComponent("SampleScenario");
        NS_TEST_ASSERT_MSG_EQ(scenario.IsEnabled(LOG_ERROR),
                              true,
                              "Scenario error level is disabled");
        NS_TEST_ASSERT_MSG_EQ(scenario.IsEnabled(LOG_WARN),
                              false,
                              "Scenario warning level is enabled");

        const auto& apGenerator = GetLogComponent("APGenerator");
        NS_TEST_ASSERT_MSG_EQ(apGenerator.IsEnabled(LOG_WARN),
                              true,
                              "AP warning level is disabled");
        NS_TEST_ASSERT_MSG_EQ(apGenerator.IsEnabled(LOG_INFO), false, "AP info level is enabled");

        const auto& staGenerator = GetLogComponent("StaLlmGenerator");
        NS_TEST_ASSERT_MSG_EQ(staGenerator.IsEnabled(LOG_FUNCTION),
                              true,
                              "Station function level is disabled");
        NS_TEST_ASSERT_MSG_EQ(staGenerator.IsEnabled(LOG_LOGIC),
                              false,
                              "Station logic level is enabled");

        const auto& trafficSink = GetLogComponent("TrafficSink");
        NS_TEST_ASSERT_MSG_EQ(trafficSink.IsEnabled(LOG_LOGIC),
                              true,
                              "Traffic sink logic level is disabled");
        NS_TEST_ASSERT_MSG_EQ(trafficSink.IsEnabled(LOG_DEBUG),
                              false,
                              "Traffic sink debug level is enabled");

        const auto& distribution = GetLogComponent("ContentionAwareAgentDistribution");
        NS_TEST_ASSERT_MSG_EQ(distribution.IsEnabled(LOG_LEVEL_ALL),
                              false,
                              "Off distribution logging was enabled");
    }

    for (const auto& [name, expectedBits] : seededStates)
    {
        NS_TEST_ASSERT_MSG_EQ(GetEnabledPublicBits(GetLogComponent(name)),
                              expectedBits,
                              "Configured logging test did not restore " << name);
    }
}

/**
 * @ingroup tests
 *
 * Verify airtime-weighted PHY rates.
 */
class PhyRateAccumulatorTestCase : public TestCase
{
  public:
    PhyRateAccumulatorTestCase();

  private:
    void DoRun() override;
};

PhyRateAccumulatorTestCase::PhyRateAccumulatorTestCase()
    : TestCase("calculate airtime-weighted PHY rates")
{
}

void
PhyRateAccumulatorTestCase::DoRun()
{
    PhyRateAccumulator rates;
    rates.Add(10e6, 100.0);
    rates.Add(20e6, 300.0);

    NS_TEST_ASSERT_MSG_EQ(rates.txAttempts, 2, "Wrong transmit attempt count");
    NS_TEST_ASSERT_MSG_EQ_TOL(rates.AverageMbps(), 17.5, 1e-9, "Wrong weighted PHY rate");
}

/**
 * @ingroup tests
 *
 * Verify direction attribution and independent statistics ownership.
 */
class WifiStatisticsAttributionTestCase : public TestCase
{
  public:
    WifiStatisticsAttributionTestCase();

  private:
    void DoRun() override;
};

WifiStatisticsAttributionTestCase::WifiStatisticsAttributionTestCase()
    : TestCase("attribute Wi-Fi payloads without global ownership")
{
}

void
WifiStatisticsAttributionTestCase::DoRun()
{
    TrafficCoordinator coordinator(50.0, 50.0);
    WifiStatistics firstOwner(coordinator, 25);
    WifiStatistics secondOwner(coordinator, 25);

    WifiStatisticsState statistics(coordinator, 25);
    NS_TEST_ASSERT_MSG_EQ(statistics.windowMs, 25, "Wrong stored window width");
    NS_TEST_ASSERT_MSG_EQ(statistics.windowUs, 25000, "Wrong derived window width");
    statistics.stationIpsByBss = {{"10.1.0.2"}, {"10.1.1.2"}};
    statistics.bssByApIp = {{"10.1.0.1", 0}, {"10.1.1.1", 1}};
    statistics.bssByStationIp = {{"10.1.0.2", 0}, {"10.1.1.2", 1}};

    NS_TEST_ASSERT_MSG_EQ(RecordMacPayloadInWindow(statistics, 0, "10.1.0.2", "10.1.0.1", 120),
                          true,
                          "Uplink flow was not attributed");
    NS_TEST_ASSERT_MSG_EQ(RecordMacPayloadInWindow(statistics, 3, "10.1.0.1", "10.1.0.2", 80),
                          true,
                          "Downlink flow was not attributed");
    NS_TEST_ASSERT_MSG_EQ(RecordMacPayloadInWindow(statistics, 0, "10.1.0.2", "10.1.1.1", 20),
                          false,
                          "Cross-BSS flow was attributed");

    NS_TEST_ASSERT_MSG_EQ(statistics.macWindows.at(0).at(0).upBytes.at("10.1.0.2"),
                          120,
                          "Wrong uplink byte total");
    NS_TEST_ASSERT_MSG_EQ(statistics.macWindows.at(3).at(0).downBytes.at("10.1.0.2"),
                          80,
                          "Wrong downlink byte total");
}

/**
 * @ingroup tests
 *
 * Verify sparse JSON windows, summaries, validation fields, and rate units.
 */
class WifiStatisticsJsonTestCase : public TestCase
{
  public:
    WifiStatisticsJsonTestCase();

  private:
    void DoRun() override;
};

/**
 * @ingroup tests
 *
 * Verify writer failures are reported without replacing existing output.
 */
class WifiStatisticsJsonFailureTestCase : public TestCase
{
  public:
    WifiStatisticsJsonFailureTestCase();

  private:
    void DoRun() override;
    void CheckWriteFailure(const WifiStatisticsState& statistics,
                           const std::filesystem::path& outputPath,
                           std::string_view description);
};

WifiStatisticsJsonTestCase::WifiStatisticsJsonTestCase()
    : TestCase("serialize sparse Wi-Fi statistics JSON")
{
}

WifiStatisticsJsonFailureTestCase::WifiStatisticsJsonFailureTestCase()
    : TestCase("report Wi-Fi statistics JSON output failures without clobbering")
{
}

void
WifiStatisticsJsonFailureTestCase::CheckWriteFailure(const WifiStatisticsState& statistics,
                                                     const std::filesystem::path& outputPath,
                                                     std::string_view description)
{
    try
    {
        WriteWifiStatisticsJson(statistics, outputPath.string());
        NS_TEST_ASSERT_MSG_EQ(true, false, "Writer failure was ignored: " << description);
    }
    catch (const std::runtime_error& error)
    {
        NS_TEST_ASSERT_MSG_NE(std::string(error.what()).find(outputPath.string()),
                              std::string::npos,
                              "Writer error lacks output path for " << description << ": "
                                                                    << error.what());
    }
}

void
WifiStatisticsJsonTestCase::DoRun()
{
    TrafficCoordinator coordinator(100.0, 100.0);
    WifiStatisticsState statistics(coordinator, 25);
    statistics.stationIpsByBss = {{"10.1.0.2", "10.1.0.3"}};

    auto& uplinkWindow = statistics.phyWindows[0][0];
    uplinkWindow.upBytes["10.1.0.2"] = 1000;
    uplinkWindow.upPhyRates["10.1.0.2"].Add(12e6, 100.0);
    statistics.phyWindows[3][0].downBytes["10.1.0.3"] = 80;

    const std::string outputPath = CreateTempDirFilename("llm-wifi-statistics.json");
    WriteWifiStatisticsJson(statistics, outputPath);

    std::ifstream input(outputPath);
    NS_TEST_ASSERT_MSG_EQ(input.good(), true, "Statistics JSON was not created");
    const nlohmann::json document = nlohmann::json::parse(input);

    NS_TEST_ASSERT_MSG_EQ(document.at("window_ms").get<uint32_t>(), 25, "Wrong window unit");
    NS_TEST_ASSERT_MSG_EQ(document.at("windows").size(), 2, "Sparse windows were materialized");
    NS_TEST_ASSERT_MSG_EQ(document.at("windows").at(0).at("timestamp").get<uint32_t>(),
                          25,
                          "Wrong first sparse timestamp");
    NS_TEST_ASSERT_MSG_EQ(document.at("windows").at(1).at("timestamp").get<uint32_t>(),
                          100,
                          "Wrong second sparse timestamp");

    const auto& uplinkFlow = document.at("windows").at(0).at("stats").at(0).at("up_flows").at(0);
    NS_TEST_ASSERT_MSG_EQ(uplinkFlow.at("bytes").get<uint64_t>(),
                          1000,
                          "Wrong serialized uplink bytes");
    NS_TEST_ASSERT_MSG_EQ_TOL(uplinkFlow.at("bw").get<double>(),
                              0.32,
                              1e-9,
                              "Wrong serialized bandwidth unit");
    NS_TEST_ASSERT_MSG_EQ_TOL(uplinkFlow.at("avg_phy_data_rate_mbps").get<double>(),
                              12.0,
                              1e-9,
                              "Wrong PHY rate unit");
    NS_TEST_ASSERT_MSG_EQ_TOL(uplinkFlow.at("phy_tx_airtime_us").get<double>(),
                              100.0,
                              1e-9,
                              "Wrong airtime unit");

    const auto& summary = document.at("summary").at(0);
    NS_TEST_ASSERT_MSG_EQ(summary.at("up_total_bytes").get<uint64_t>(),
                          1000,
                          "Wrong uplink summary");
    NS_TEST_ASSERT_MSG_EQ(summary.at("down_total_bytes").get<uint64_t>(),
                          80,
                          "Wrong downlink summary");
    NS_TEST_ASSERT_MSG_EQ(document.at("validation").at("window_totals_consistent").get<bool>(),
                          true,
                          "Window validation failed");
    NS_TEST_ASSERT_MSG_EQ(document.at("validation").at("summary_totals_consistent").get<bool>(),
                          true,
                          "Summary validation failed");
}

void
WifiStatisticsJsonFailureTestCase::DoRun()
{
    TrafficCoordinator coordinator(25.0, 25.0);
    WifiStatisticsState statistics(coordinator, 25);

    const std::filesystem::path collisionPath =
        CreateTempDirFilename("llm-wifi-statistics-existing.json");
    constexpr std::string_view sentinel{"existing statistics must survive\n"};
    {
        std::ofstream output(collisionPath);
        output << sentinel;
    }

    CheckWriteFailure(statistics, collisionPath, "existing output collision");

    std::ifstream collisionInput(collisionPath);
    const std::string preserved((std::istreambuf_iterator<char>(collisionInput)),
                                std::istreambuf_iterator<char>());
    NS_TEST_ASSERT_MSG_EQ(preserved,
                          sentinel,
                          "Existing output content was replaced by the JSON writer");

    const std::filesystem::path missingParentPath =
        std::filesystem::path(CreateTempDirFilename("missing-output-parent")) / "output.json";
    NS_TEST_ASSERT_MSG_EQ(std::filesystem::exists(missingParentPath.parent_path()),
                          false,
                          "Missing-parent fixture unexpectedly exists");
    CheckWriteFailure(statistics, missingParentPath, "missing output parent");
    NS_TEST_ASSERT_MSG_EQ(std::filesystem::exists(missingParentPath),
                          false,
                          "Failed writer created an output file");
}

} // namespace

std::vector<TestCase*>
CreateWifiStatisticsTestCases()
{
    return {new StatisticsWindowTestCase,
            new LongDurationStatisticsWindowTestCase,
            new ScenarioLoggingTestCase,
            new PhyRateAccumulatorTestCase,
            new WifiStatisticsAttributionTestCase,
            new WifiStatisticsJsonTestCase,
            new WifiStatisticsJsonFailureTestCase};
}
