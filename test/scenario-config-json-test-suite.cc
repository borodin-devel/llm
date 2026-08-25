#include "../examples/scenario-config-internal.h"
#include "llm-test-suite.h"

#include "ns3/json.hpp"

#include <array>
#include <sstream>

using namespace ns3;

namespace
{

/**
 * @ingroup tests
 *
 * Verify the effective configuration JSON preserves scalar values.
 */
class ScenarioConfigJsonTestCase : public TestCase
{
  public:
    ScenarioConfigJsonTestCase();

  private:
    void DoRun() override;
};

ScenarioConfigJsonTestCase::ScenarioConfigJsonTestCase()
    : TestCase("scenario configuration effective JSON")
{
}

void
ScenarioConfigJsonTestCase::DoRun()
{
    ScenarioConfig config;
    config.general.traceFile = "traces/quoted-\"name\".json";
    config.general.runFolder.reset();
    config.general.outputName = "custom-output.json";
    config.simulation.durationMode = DurationMode::FIXED;
    config.simulation.fixedDurationSeconds = 12.5;
    config.topology.isolateBssChannels = false;
    config.wifi.band = WifiBandConfig::BAND_6_GHZ;
    config.wifi.activeProbing = false;
    config.logging.sampleScenarioLevel = "debug";

    std::ostringstream output;
    WriteEffectiveConfigurationJson(output, config);
    const auto document = nlohmann::json::parse(output.str());

    NS_TEST_ASSERT_MSG_EQ(document.size(), 8, "Wrong configuration section count");
    std::size_t fieldCount = 0;
    for (const auto& section : document.items())
    {
        fieldCount += section.value().size();
    }
    NS_TEST_ASSERT_MSG_EQ(fieldCount, 36, "Wrong effective configuration field count");
    NS_TEST_ASSERT_MSG_EQ(document.at("general").at("run_folder").is_null(),
                          true,
                          "Omitted run folder is not null");
    NS_TEST_ASSERT_MSG_EQ(document.at("general").at("trace_file").get<std::string>(),
                          config.general.traceFile,
                          "Trace string was not escaped and restored");
    NS_TEST_ASSERT_MSG_EQ(document.at("simulation").at("duration_mode"),
                          "fixed",
                          "Wrong duration enum spelling");
    NS_TEST_ASSERT_MSG_EQ(document.at("simulation").at("fixed_duration_seconds"),
                          12.5,
                          "Float lost its JSON type");
    NS_TEST_ASSERT_MSG_EQ(document.at("topology").at("isolate_bss_channels"),
                          false,
                          "Boolean lost its JSON type");
    NS_TEST_ASSERT_MSG_EQ(document.at("wifi").at("band"), "6GHz", "Wrong band spelling");
}

/**
 * @ingroup tests
 *
 * Verify every private registry option has an effective-value reader.
 */
class ScenarioConfigJsonReaderTestCase : public TestCase
{
  public:
    ScenarioConfigJsonReaderTestCase();

  private:
    void DoRun() override;
};

ScenarioConfigJsonReaderTestCase::ScenarioConfigJsonReaderTestCase()
    : TestCase("scenario configuration JSON readers")
{
}

void
ScenarioConfigJsonReaderTestCase::DoRun()
{
    const ScenarioConfig config;
    for (const auto& option : GetScenarioConfigOptions())
    {
        NS_TEST_ASSERT_MSG_EQ(option.readJson.operator bool(),
                              true,
                              "Missing JSON reader for " << option.tomlPath);
        const auto value = option.readJson(config);
        NS_TEST_ASSERT_MSG_EQ(value.is_discarded(),
                              false,
                              "Invalid JSON reader result for " << option.tomlPath);
    }
}

/**
 * @ingroup tests
 *
 * Verify configuration paths contain exactly one non-edge dot.
 */
class ScenarioConfigJsonPathTestCase : public TestCase
{
  public:
    ScenarioConfigJsonPathTestCase();

  private:
    void DoRun() override;
};

ScenarioConfigJsonPathTestCase::ScenarioConfigJsonPathTestCase()
    : TestCase("scenario configuration JSON paths")
{
}

void
ScenarioConfigJsonPathTestCase::DoRun()
{
    const auto [section, field] = SplitScenarioConfigPath("section.field");
    NS_TEST_ASSERT_MSG_EQ(section, "section", "Wrong valid path section");
    NS_TEST_ASSERT_MSG_EQ(field, "field", "Wrong valid path field");

    constexpr std::array<std::string_view, 5> invalidPaths{"field",
                                                           ".field",
                                                           "section.",
                                                           "section..field",
                                                           "section.field.extra"};
    for (const auto path : invalidPaths)
    {
        bool rejected = false;
        try
        {
            (void)SplitScenarioConfigPath(path);
        }
        catch (const ScenarioConfigError&)
        {
            rejected = true;
        }
        NS_TEST_ASSERT_MSG_EQ(rejected, true, "Accepted invalid path " << path);
    }
}

} // namespace

std::vector<TestCase*>
CreateScenarioConfigJsonTestCases()
{
    return {new ScenarioConfigJsonTestCase,
            new ScenarioConfigJsonReaderTestCase,
            new ScenarioConfigJsonPathTestCase};
}
