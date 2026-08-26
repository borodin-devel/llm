#include "log.h"

// The private header above shares the core header's basename.
// clang-format off
#include "ns3/log.h"
// clang-format on

#include <stdexcept>
#include <string>

namespace ns3::saturated_tcp_example
{

LogComponent&
GetScenarioLog()
{
    static LogComponent component("SaturatedTcpScenario", __FILE__);
    return component;
}

void
ConfigureScenarioLogging(std::string_view level)
{
    static_cast<void>(GetScenarioLog());
    if (level == "off")
    {
        return;
    }

    LogLevel logLevel;
    if (level == "error")
    {
        logLevel = LOG_LEVEL_ERROR;
    }
    else if (level == "warn")
    {
        logLevel = LOG_LEVEL_WARN;
    }
    else if (level == "info")
    {
        logLevel = LOG_LEVEL_INFO;
    }
    else if (level == "debug")
    {
        logLevel = LOG_LEVEL_DEBUG;
    }
    else if (level == "function")
    {
        logLevel = LOG_LEVEL_FUNCTION;
    }
    else if (level == "logic")
    {
        logLevel = LOG_LEVEL_LOGIC;
    }
    else if (level == "all")
    {
        logLevel = LOG_LEVEL_ALL;
    }
    else
    {
        throw std::invalid_argument("unknown saturated scenario log level: " + std::string(level));
    }
    LogComponentEnable("SaturatedTcpScenario", logLevel);
}

} // namespace ns3::saturated_tcp_example
