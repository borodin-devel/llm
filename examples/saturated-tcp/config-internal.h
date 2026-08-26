#ifndef SATURATED_TCP_CONFIG_INTERNAL_H
#define SATURATED_TCP_CONFIG_INTERNAL_H

#include "config.h"

#include "ns3/json.hpp"
#include "ns3/toml.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ns3
{

/** Scalar categories supported by the saturated option registry. */
enum class SaturatedConfigValueType
{
    STRING,  ///< TOML string value.
    INTEGER, ///< TOML integer value.
    FLOAT,   ///< TOML floating-point value.
    ENUM,    ///< TOML string selected from an enumerated set.
};

/** Private standalone option definition shared by saturated configuration layers. */
struct SaturatedConfigOption
{
    std::string tomlPath;               ///< Dotted saturated TOML key.
    std::string cliFlag;                ///< Section-prefixed saturated CLI flag.
    SaturatedConfigValueType valueType; ///< Required scalar category.
    std::function<void(SaturatedTcpConfig&, const toml::node&)> applyToml;    ///< TOML setter.
    std::function<void(SaturatedTcpConfig&, std::string_view)> applyOverride; ///< CLI setter.
    std::function<nlohmann::json(const SaturatedTcpConfig&)> readJson;        ///< Metadata reader.
    std::string description; ///< Concise option description.
};

/**
 * Get the callback-bearing saturated option registry.
 *
 * @return Complete registry in canonical TOML and JSON order.
 */
const std::vector<SaturatedConfigOption>& GetSaturatedTcpConfigOptions();

/**
 * Split a saturated dotted path into section and field names.
 *
 * @param path Configuration path with exactly one dot.
 * @return Section and field names.
 * @throws SaturatedTcpConfigError if the path is malformed.
 */
std::pair<std::string_view, std::string_view> SplitSaturatedTcpConfigPath(std::string_view path);

/**
 * Get a human-readable saturated scalar category name.
 *
 * @param valueType Scalar category.
 * @return Diagnostic name.
 */
std::string_view GetSaturatedTcpConfigValueTypeName(SaturatedConfigValueType valueType);

/**
 * Load strict saturated benchmark TOML without applying CLI values.
 *
 * @param path Explicit TOML document path.
 * @return Typed TOML values merged over compiled defaults.
 * @throws SaturatedTcpConfigError if parsing or schema application fails.
 */
SaturatedTcpConfig LoadSaturatedTcpTomlConfig(const std::filesystem::path& path);

} // namespace ns3

#endif // SATURATED_TCP_CONFIG_INTERNAL_H
