#ifndef EXPERIMENT_OUTPUT_INTERNAL_H
#define EXPERIMENT_OUTPUT_INTERNAL_H

#include "experiment-output.h"

#include "ns3/json.hpp"

#include <iosfwd>

namespace ns3
{

struct WifiStatisticsState;

/** Validation results from streaming the Wi-Fi statistics members. */
struct WifiJsonValidation
{
    bool windowPayloadTotalsConsistent{true};  ///< Whether window flow totals match sparse state.
    bool summaryPayloadTotalsConsistent{true}; ///< Whether summary totals match sparse state.
};

/**
 * Write one JSON scalar to a stream.
 *
 * @tparam T Scalar value type.
 * @param output Destination stream.
 * @param value Scalar value to encode.
 */
template <typename T>
void
WriteJsonScalar(std::ostream& output, const T& value)
{
    output << nlohmann::json(value).dump();
}

/**
 * Stream the statistics window, Wi-Fi windows, and Wi-Fi summary root members.
 *
 * @param output Destination stream.
 * @param statistics Collected Wi-Fi statistics state.
 * @return Payload-total validation results.
 */
WifiJsonValidation WriteWifiStatisticsJsonMembers(std::ostream& output,
                                                  const WifiStatisticsState& statistics);

/**
 * Stream a typed transmission summary as a JSON object.
 *
 * @param output Destination stream.
 * @param summary Typed transmission summary.
 */
void WriteTransmissionSummaryJson(std::ostream& output, const TransmissionSummary& summary);

/**
 * Stream a typed cross-layer summary as a JSON object.
 *
 * @param output Destination stream.
 * @param summary Typed cross-layer summary.
 */
void WriteCrossLayerSummaryJson(std::ostream& output, const CrossLayerSummary& summary);

} // namespace ns3

#endif // EXPERIMENT_OUTPUT_INTERNAL_H
