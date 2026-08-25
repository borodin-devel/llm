#ifndef SCENARIO_CONFIG_INTERNAL_H
#define SCENARIO_CONFIG_INTERNAL_H

#include "scenario-config.h"

#include "ns3/json.hpp"
#include "ns3/toml.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ns3
{

/** Private option definition shared by TOML and command-line parsing. */
struct ConfigOption
{
    std::string tomlPath;      ///< Dotted TOML key.
    std::string cliFlag;       ///< Derived section-prefixed CLI flag.
    ConfigValueType valueType; ///< Required scalar category.
    std::function<void(ScenarioConfig&, const toml::node&)> applyToml;    ///< TOML setter.
    std::function<void(ScenarioConfig&, std::string_view)> applyOverride; ///< CLI setter.
    std::function<nlohmann::json(const ScenarioConfig&)> readJson; ///< Effective-value reader.
    std::string description;                                       ///< Help and diagnostic text.
};

/**
 * Get the private scenario option registry.
 *
 * @return Complete callback-bearing option registry.
 */
const std::vector<ConfigOption>& GetScenarioConfigOptions();

/**
 * Write the effective scenario configuration as a JSON object.
 *
 * @param output Destination stream.
 * @param configuration Effective scenario configuration.
 * @throws ScenarioConfigError if an option path is malformed.
 */
void WriteEffectiveConfigurationJson(std::ostream& output, const ScenarioConfig& configuration);

/**
 * Get the diagnostic name of a configuration scalar category.
 *
 * @param valueType Scalar category.
 * @return Human-readable expected type.
 */
std::string_view GetScenarioConfigValueTypeName(ConfigValueType valueType);

} // namespace ns3

#endif // SCENARIO_CONFIG_INTERNAL_H
