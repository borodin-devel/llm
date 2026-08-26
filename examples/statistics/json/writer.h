#ifndef STATISTICS_JSON_WRITER_H
#define STATISTICS_JSON_WRITER_H

#include "../../experiment-window-output.h"

#include "ns3/json.hpp"

#include <cstddef>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ns3
{

struct ScenarioConfig;

/**
 * Stream one indented JSON value while validating its structural state.
 *
 * The writer supports a single root JSON value.  It emits two-space indentation
 * and appends a newline when Finish() completes the document.
 */
class JsonWriter
{
  public:
    /**
     * Construct a JSON writer that writes to an output stream.
     *
     * @param output Destination stream.
     */
    explicit JsonWriter(std::ostream& output);

    /** Begin an object value. */
    void BeginObject();

    /** End the current object value. */
    void EndObject();

    /** Begin an array value. */
    void BeginArray();

    /** End the current array value. */
    void EndArray();

    /**
     * Write the next object member key.
     *
     * @param key Object member name.
     */
    void Key(std::string_view key);

    /**
     * Write a scalar JSON value.
     *
     * @tparam T Scalar value type accepted by nlohmann::json.
     * @param value Value to encode.
     */
    template <typename T>
    void Value(const T& value)
    {
        BeginValue();
        m_output << nlohmann::json(value).dump();
        CompleteScalarValue();
    }

    /** Write a JSON null value. */
    void Null();

    /**
     * Complete the root JSON value and append its terminating newline.
     *
     * @throws std::logic_error If the root value is incomplete or Finish() was already called.
     */
    void Finish();

  private:
    /** Kinds of JSON containers managed by the writer. */
    enum class ContainerKind
    {
        OBJECT, ///< JSON object container.
        ARRAY   ///< JSON array container.
    };

    /** State for one open JSON container. */
    struct ContainerState
    {
        ContainerKind kind;       ///< Kind of the open container.
        bool first{true};         ///< Whether the container has no members or values yet.
        bool expectsValue{false}; ///< Whether an object key is awaiting its value.
    };

    /** Prepare the output stream to receive one value. */
    void BeginValue();

    /** Mark the current scalar or closed container value as complete. */
    void CompleteScalarValue();

    /**
     * Close the current container after validating its kind and state.
     *
     * @param expectedKind Required kind of the currently open container.
     * @param closingCharacter JSON closing delimiter to emit.
     */
    void EndContainer(ContainerKind expectedKind, char closingCharacter);

    /**
     * Write the two-space indentation for a nesting depth.
     *
     * @param depth Number of indentation levels.
     */
    void WriteIndent(std::size_t depth);

    /**
     * Throw a logic error that identifies an invalid writer operation.
     *
     * @param operation Name of the rejected operation.
     */
    [[noreturn]] void ThrowStateError(std::string_view operation) const;

    std::ostream& m_output;                   ///< Destination JSON stream.
    std::vector<ContainerState> m_containers; ///< Stack of currently open JSON containers.
    bool m_rootStarted{false};                ///< Whether the root value has started.
    bool m_rootComplete{false};               ///< Whether the root value has completed.
    bool m_finished{false};                   ///< Whether Finish() has appended the final newline.
};

inline JsonWriter::JsonWriter(std::ostream& output)
    : m_output(output)
{
}

inline void
JsonWriter::BeginObject()
{
    BeginValue();
    m_output << '{';
    m_containers.push_back({ContainerKind::OBJECT});
}

inline void
JsonWriter::EndObject()
{
    EndContainer(ContainerKind::OBJECT, '}');
}

inline void
JsonWriter::BeginArray()
{
    BeginValue();
    m_output << '[';
    m_containers.push_back({ContainerKind::ARRAY});
}

inline void
JsonWriter::EndArray()
{
    EndContainer(ContainerKind::ARRAY, ']');
}

inline void
JsonWriter::Key(std::string_view key)
{
    if (m_finished || m_containers.empty() || m_containers.back().kind != ContainerKind::OBJECT ||
        m_containers.back().expectsValue)
    {
        ThrowStateError("Key");
    }

    auto& object = m_containers.back();
    if (object.first)
    {
        m_output << '\n';
    }
    else
    {
        m_output << ",\n";
    }
    WriteIndent(m_containers.size());
    m_output << nlohmann::json(key).dump() << ": ";
    object.first = false;
    object.expectsValue = true;
}

inline void
JsonWriter::Null()
{
    BeginValue();
    m_output << "null";
    CompleteScalarValue();
}

inline void
JsonWriter::Finish()
{
    if (m_finished || !m_rootStarted || !m_rootComplete || !m_containers.empty())
    {
        ThrowStateError("Finish");
    }
    m_output << '\n';
    m_finished = true;
}

inline void
JsonWriter::BeginValue()
{
    if (m_finished || m_rootComplete)
    {
        ThrowStateError("BeginValue");
    }

    if (m_containers.empty())
    {
        if (m_rootStarted)
        {
            ThrowStateError("BeginValue");
        }
        m_rootStarted = true;
        return;
    }

    auto& container = m_containers.back();
    if (container.kind == ContainerKind::OBJECT)
    {
        if (!container.expectsValue)
        {
            ThrowStateError("BeginValue");
        }
        return;
    }

    if (container.first)
    {
        m_output << '\n';
    }
    else
    {
        m_output << ",\n";
    }
    WriteIndent(m_containers.size());
    container.first = false;
}

inline void
JsonWriter::CompleteScalarValue()
{
    if (m_containers.empty())
    {
        m_rootComplete = true;
        return;
    }

    auto& container = m_containers.back();
    if (container.kind == ContainerKind::OBJECT)
    {
        container.expectsValue = false;
    }
}

inline void
JsonWriter::EndContainer(ContainerKind expectedKind, char closingCharacter)
{
    if (m_finished || m_containers.empty() || m_containers.back().kind != expectedKind ||
        (expectedKind == ContainerKind::OBJECT && m_containers.back().expectsValue))
    {
        ThrowStateError(expectedKind == ContainerKind::OBJECT ? "EndObject" : "EndArray");
    }

    const bool empty = m_containers.back().first;
    if (!empty)
    {
        m_output << '\n';
        WriteIndent(m_containers.size() - 1);
    }
    m_output << closingCharacter;
    m_containers.pop_back();
    CompleteScalarValue();
}

inline void
JsonWriter::WriteIndent(std::size_t depth)
{
    m_output << std::string(depth * 2, ' ');
}

[[noreturn]] inline void
JsonWriter::ThrowStateError(std::string_view operation) const
{
    throw std::logic_error("JsonWriter::" + std::string(operation) + " called in an invalid state");
}

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
 * Write one optional JSON scalar as its value or null.
 *
 * @tparam T Scalar value type.
 * @param output Destination stream.
 * @param value Optional scalar to encode.
 */
template <typename T>
void
WriteJsonScalar(std::ostream& output, const std::optional<T>& value)
{
    if (value)
    {
        WriteJsonScalar(output, *value);
    }
    else
    {
        output << "null";
    }
}

/** @param output Destination stream. @param distribution Distribution to serialize. */
void WriteSampleDistributionJson(std::ostream& output,
                                 const SampleDistributionOutput& distribution);

/** @param output Destination stream. @param direction General direction to serialize. */
void WriteGeneralDirectionJson(std::ostream& output, const GeneralDirectionOutput& direction);

/** @param output Destination stream. @param direction Application direction to serialize. */
void WriteAppDirectionJson(std::ostream& output, const AppDirectionOutput& direction);

/** @param output Destination stream. @param direction TCP direction to serialize. */
void WriteTcpDirectionJson(std::ostream& output, const TcpDirectionOutput& direction);

/** @param output Destination stream. @param direction MAC direction to serialize. */
void WriteMacDirectionJson(std::ostream& output, const MacDirectionOutput& direction);

/** @param output Destination stream. @param category PHY category to serialize. */
void WritePhyCategoryJson(std::ostream& output, const PhyCategoryOutput& category);

/** @param output Destination stream. @param statistics Entity statistics to serialize. */
void WriteEntityStatisticsJson(std::ostream& output, const EntityStatisticsOutput& statistics);

/** @param output Destination stream. @param entities AP records to serialize. */
void WriteAccessPointStatisticsArrayJson(std::ostream& output,
                                         const std::vector<AccessPointStatisticsOutput>& entities);

/** @param output Destination stream. @param entities station records to serialize. */
void WriteStationStatisticsArrayJson(std::ostream& output,
                                     const std::vector<StationStatisticsOutput>& entities);

/** @param output Destination stream. @param windows sparse windows to serialize. */
void WriteExperimentWindowsJson(std::ostream& output,
                                const std::vector<ExperimentWindowOutput>& windows);

/** @param output Destination stream. @param overall dense overall values to serialize. */
void WriteExperimentOverallJson(std::ostream& output, const ExperimentOverallOutput& overall);

/** @param output Destination stream. @param validation validation flags to serialize. */
void WriteExperimentValidationJson(std::ostream& output,
                                   const ExperimentValidationOutput& validation);

/**
 * Stream the complete schema-version-1 hierarchy without constructing a root JSON DOM.
 *
 * @param output Destination stream.
 * @param summary Finalized typed experiment summary.
 * @param configuration Effective scenario configuration.
 */
void WriteExperimentHierarchyJson(std::ostream& output,
                                  const UnifiedExperimentSummary& summary,
                                  const ScenarioConfig& configuration);

} // namespace ns3

#endif // STATISTICS_JSON_WRITER_H
