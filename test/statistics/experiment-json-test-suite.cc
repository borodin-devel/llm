#include "../../examples/config/scenario-config.h"
#include "../../examples/runtime/traffic-coordinator.h"
#include "../../examples/statistics/experiment-statistics.h"
#include "../../examples/statistics/json/writer.h"
#include "../llm-test-suite.h"

#include "ns3/json.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>

using namespace ns3;

namespace
{

/** Stream buffer that rejects every body write. */
class RejectingStreamBuffer : public std::streambuf
{
  private:
    std::streamsize xsputn(const char*, std::streamsize) override
    {
        return 0;
    }

    int_type overflow(int_type) override
    {
        return traits_type::eof();
    }
};

/**
 * @ingroup tests
 *
 * Verify the public writer preserves exclusive-create and path-bearing errors.
 */
class ExperimentJsonIoTestCase : public TestCase
{
  public:
    ExperimentJsonIoTestCase();

  private:
    void DoRun() override;
    void CheckWriteFailure(ExperimentStatistics& statistics,
                           const std::filesystem::path& outputPath,
                           std::string_view description);
};

/**
 * @ingroup tests
 *
 * Verify a rejected hierarchy body write leaves the destination stream failed.
 */
class ExperimentJsonWriteStateTestCase : public TestCase
{
  public:
    ExperimentJsonWriteStateTestCase();

  private:
    void DoRun() override;
};

/**
 * @ingroup tests
 *
 * Verify JsonWriter produces the required indented JSON representation.
 */
class JsonWriterFormattingTestCase : public TestCase
{
  public:
    /** Construct the JSON writer formatting test case. */
    JsonWriterFormattingTestCase();

  private:
    /** Execute the JSON writer formatting test. */
    void DoRun() override;
};

/**
 * @ingroup tests
 *
 * Verify JsonWriter rejects invalid operation sequences.
 */
class JsonWriterStateTestCase : public TestCase
{
  public:
    /** Construct the JSON writer state test case. */
    JsonWriterStateTestCase();

  private:
    /** Execute the JSON writer state test. */
    void DoRun() override;
};

ExperimentJsonIoTestCase::ExperimentJsonIoTestCase()
    : TestCase("write experiment JSON exclusively and report IO paths")
{
}

void
ExperimentJsonIoTestCase::CheckWriteFailure(ExperimentStatistics& statistics,
                                            const std::filesystem::path& outputPath,
                                            std::string_view description)
{
    try
    {
        statistics.WriteExperimentJson(outputPath.string(), {});
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
ExperimentJsonIoTestCase::DoRun()
{
    TrafficCoordinator coordinator(25.0, 25.0);
    ExperimentStatistics statistics(coordinator, 25);
    statistics.Finalize();
    statistics.Finalize();

    const std::filesystem::path outputPath =
        CreateTempDirFilename("llm-experiment-final-output.json");
    statistics.WriteExperimentJson(outputPath.string(), {});
    std::ifstream input(outputPath);
    NS_TEST_ASSERT_MSG_EQ(input.good(), true, "Experiment JSON was not created");
    const auto document = nlohmann::json::parse(input);
    NS_TEST_ASSERT_MSG_EQ(document.size(), 7, "Wrong final root member count");

    const std::filesystem::path collisionPath =
        CreateTempDirFilename("llm-experiment-existing.json");
    constexpr std::string_view sentinel{"existing experiment must survive\n"};
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
                          "Existing output content was replaced by the experiment writer");

    const std::filesystem::path missingParentPath =
        std::filesystem::path(CreateTempDirFilename("missing-experiment-parent")) / "output.json";
    NS_TEST_ASSERT_MSG_EQ(std::filesystem::exists(missingParentPath.parent_path()),
                          false,
                          "Missing-parent fixture unexpectedly exists");
    CheckWriteFailure(statistics, missingParentPath, "missing output parent");
    NS_TEST_ASSERT_MSG_EQ(std::filesystem::exists(missingParentPath),
                          false,
                          "Failed writer created an output file");
}

ExperimentJsonWriteStateTestCase::ExperimentJsonWriteStateTestCase()
    : TestCase("preserve failed stream state while writing hierarchy JSON")
{
}

void
ExperimentJsonWriteStateTestCase::DoRun()
{
    RejectingStreamBuffer buffer;
    std::ostream output(&buffer);
    WriteExperimentHierarchyJson(output, {}, {});
    NS_TEST_ASSERT_MSG_EQ(output.fail(), true, "Rejected hierarchy body write was not observable");
}

JsonWriterFormattingTestCase::JsonWriterFormattingTestCase()
    : TestCase("format nested JSON through streaming writer operations")
{
}

void
JsonWriterFormattingTestCase::DoRun()
{
    std::ostringstream output;
    JsonWriter writer(output);
    writer.BeginObject();
    writer.Key("name");
    writer.Value("quoted \"value\"");
    writer.Key("items");
    writer.BeginArray();
    writer.Value(7);
    writer.Null();
    writer.BeginObject();
    writer.Key("enabled");
    writer.Value(true);
    writer.EndObject();
    writer.EndArray();
    writer.Key("empty");
    writer.BeginArray();
    writer.EndArray();
    writer.EndObject();
    writer.Finish();

    constexpr std::string_view expected = R"({
  "name": "quoted \"value\"",
  "items": [
    7,
    null,
    {
      "enabled": true
    }
  ],
  "empty": []
}
)";
    NS_TEST_ASSERT_MSG_EQ(output.str(), expected, "JSON writer format differs from the contract");
}

JsonWriterStateTestCase::JsonWriterStateTestCase()
    : TestCase("reject invalid streaming JSON writer state transitions")
{
}

void
JsonWriterStateTestCase::DoRun()
{
    {
        std::ostringstream output;
        JsonWriter writer(output);
        try
        {
            writer.Key("outside");
            NS_TEST_ASSERT_MSG_EQ(true, false, "Key outside an object was accepted");
        }
        catch (const std::logic_error&)
        {
        }
    }
    {
        std::ostringstream output;
        JsonWriter writer(output);
        writer.BeginObject();
        try
        {
            writer.Value(1);
            NS_TEST_ASSERT_MSG_EQ(true, false, "Object value without a key was accepted");
        }
        catch (const std::logic_error&)
        {
        }
    }
    {
        std::ostringstream output;
        JsonWriter writer(output);
        writer.BeginObject();
        writer.Key("pending");
        try
        {
            writer.EndObject();
            NS_TEST_ASSERT_MSG_EQ(true, false, "Object with a pending key was closed");
        }
        catch (const std::logic_error&)
        {
        }
    }
    {
        std::ostringstream output;
        JsonWriter writer(output);
        writer.BeginArray();
        try
        {
            writer.EndObject();
            NS_TEST_ASSERT_MSG_EQ(true, false, "Wrong container kind was closed");
        }
        catch (const std::logic_error&)
        {
        }
    }
    {
        std::ostringstream output;
        JsonWriter writer(output);
        try
        {
            writer.Finish();
            NS_TEST_ASSERT_MSG_EQ(true, false, "Finish without a root value was accepted");
        }
        catch (const std::logic_error&)
        {
        }
    }
    {
        std::ostringstream output;
        JsonWriter writer(output);
        writer.BeginArray();
        try
        {
            writer.Finish();
            NS_TEST_ASSERT_MSG_EQ(true, false, "Finish with an open root array was accepted");
        }
        catch (const std::logic_error&)
        {
        }
    }
    {
        std::ostringstream output;
        JsonWriter writer(output);
        writer.Value(1);
        try
        {
            writer.Null();
            NS_TEST_ASSERT_MSG_EQ(true, false, "A second root value was accepted");
        }
        catch (const std::logic_error&)
        {
        }
    }
    {
        std::ostringstream output;
        JsonWriter writer(output);
        writer.Null();
        writer.Finish();
        try
        {
            writer.Finish();
            NS_TEST_ASSERT_MSG_EQ(true, false, "A second Finish call was accepted");
        }
        catch (const std::logic_error&)
        {
        }
    }
}

} // namespace

std::vector<TestCase*>
CreateExperimentJsonTestCases()
{
    return {new ExperimentJsonIoTestCase,
            new ExperimentJsonWriteStateTestCase,
            new JsonWriterFormattingTestCase,
            new JsonWriterStateTestCase};
}
