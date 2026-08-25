#include "../examples/scenario-config.h"
#include "llm-test-suite.h"

#include <array>
#include <string>
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
 * Read every enabled public log and prefix bit for one component.
 *
 * @param component Log component to inspect.
 * @return Exact enabled public log and prefix bits.
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

/** Restore exact public log and prefix state on scope exit. */
class ScopedLogComponentState
{
  public:
    /**
     * Snapshot registered component states.
     *
     * @param componentNames Exact registered component names to preserve.
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

    ScopedLogComponentState(const ScopedLogComponentState&) = delete;
    ScopedLogComponentState& operator=(const ScopedLogComponentState&) = delete;

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
    std::vector<std::pair<LogComponent*, LogLevel>> m_savedStates; ///< Saved component states.
};

/**
 * Replace every public log and prefix bit for one registered component.
 *
 * @param name Registered component name.
 * @param enabledBits Exact public bits to enable.
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
 * Verify five distinct scenario levels, off behavior, and exact state restoration.
 */
class ScenarioLoggingTestCase : public TestCase
{
  public:
    ScenarioLoggingTestCase();

  private:
    void DoRun() override;
};

ScenarioLoggingTestCase::ScenarioLoggingTestCase()
    : TestCase("configure scenario logging components and restore public state")
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
    const ScopedLogComponentState restoreIncomingState(componentNames);

    const std::array seededStates{
        std::pair{"SampleScenario", static_cast<LogLevel>(LOG_ERROR | LOG_PREFIX_FUNC)},
        std::pair{"APGenerator", static_cast<LogLevel>(LOG_WARN | LOG_PREFIX_TIME)},
        std::pair{"StaLlmGenerator", static_cast<LogLevel>(LOG_INFO | LOG_PREFIX_NODE)},
        std::pair{"TrafficSink", static_cast<LogLevel>(LOG_FUNCTION | LOG_PREFIX_LEVEL)},
        std::pair{"ContentionAwareAgentDistribution",
                  static_cast<LogLevel>(LOG_LOGIC | LOG_DEBUG | LOG_PREFIX_FUNC)},
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

        NS_TEST_ASSERT_MSG_EQ(GetEnabledPublicBits(GetLogComponent("SampleScenario")),
                              LOG_ERROR,
                              "Scenario did not receive only the error level");
        NS_TEST_ASSERT_MSG_EQ(GetEnabledPublicBits(GetLogComponent("APGenerator")),
                              LOG_LEVEL_WARN,
                              "AP generator did not receive the warn level");
        NS_TEST_ASSERT_MSG_EQ(GetEnabledPublicBits(GetLogComponent("StaLlmGenerator")),
                              LOG_LEVEL_FUNCTION,
                              "Station generator did not receive the function level");
        NS_TEST_ASSERT_MSG_EQ(GetEnabledPublicBits(GetLogComponent("TrafficSink")),
                              LOG_LEVEL_LOGIC,
                              "Traffic sink did not receive the logic level");
        NS_TEST_ASSERT_MSG_EQ(
            GetEnabledPublicBits(GetLogComponent("ContentionAwareAgentDistribution")),
            LOG_NONE,
            "Off distribution logging changed the cleared component state");
    }

    for (const auto& [name, expectedBits] : seededStates)
    {
        NS_TEST_ASSERT_MSG_EQ(GetEnabledPublicBits(GetLogComponent(name)),
                              expectedBits,
                              "Scope exit did not restore exact public state for " << name);
    }
}

} // namespace

std::vector<TestCase*>
CreateScenarioLoggingTestCases()
{
    return {new ScenarioLoggingTestCase};
}
