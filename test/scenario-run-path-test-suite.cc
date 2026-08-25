#include "../examples/scenario-config.h"
#include "llm-test-suite.h"

#include <array>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace ns3;

namespace
{

/** Test-runner-owned directory removed when a test case finishes. */
class ScopedTempDirectory
{
  public:
    /**
     * Create a scoped directory at a test-runner-provided path.
     *
     * @param path Path reserved for this test case.
     */
    explicit ScopedTempDirectory(const std::filesystem::path& path);

    /** Remove the test-owned directory. */
    ~ScopedTempDirectory();

    /**
     * Get the absolute temporary directory path.
     *
     * @return Temporary directory path.
     */
    const std::filesystem::path& Get() const;

  private:
    std::filesystem::path m_path; ///< Absolute test-owned directory path.
};

ScopedTempDirectory::ScopedTempDirectory(const std::filesystem::path& path)
    : m_path(std::filesystem::absolute(path).lexically_normal())
{
    std::error_code errorCode;
    std::filesystem::create_directories(m_path, errorCode);
    if (errorCode)
    {
        throw std::runtime_error("failed to create test directory " + m_path.string() + ": " +
                                 errorCode.message());
    }
}

ScopedTempDirectory::~ScopedTempDirectory()
{
    std::error_code errorCode;
    std::filesystem::remove_all(m_path, errorCode);
}

const std::filesystem::path&
ScopedTempDirectory::Get() const
{
    return m_path;
}

std::chrono::system_clock::time_point
MakeLocalLaunchTime()
{
    std::tm localTime{};
    localTime.tm_year = 2026 - 1900;
    localTime.tm_mon = 8 - 1;
    localTime.tm_mday = 25;
    localTime.tm_hour = 14;
    localTime.tm_min = 3;
    localTime.tm_sec = 9;
    localTime.tm_isdst = -1;
    return std::chrono::system_clock::from_time_t(std::mktime(&localTime));
}

void
WriteFile(const std::filesystem::path& path)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path);
    output << "{}\n";
}

template <std::size_t Size>
void
WriteBytes(const std::filesystem::path& path, const std::array<unsigned char, Size>& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

/**
 * @ingroup tests
 *
 * Verify deterministic scenario run path resolution.
 */
class ScenarioRunPathResolutionTestCase : public TestCase
{
  public:
    ScenarioRunPathResolutionTestCase();

  private:
    void DoRun() override;
};

ScenarioRunPathResolutionTestCase::ScenarioRunPathResolutionTestCase()
    : TestCase("resolve scenario run paths against the launch working directory")
{
}

void
ScenarioRunPathResolutionTestCase::DoRun()
{
    ScopedTempDirectory temporaryDirectory(CreateTempDirFilename("scenario-run-path"));
    const auto workingDirectory = temporaryDirectory.Get() / "working";
    const auto launchTime = MakeLocalLaunchTime();

    ScenarioLaunchConfig relativeLaunch;
    relativeLaunch.scenario.general.traceFile = "data/trace.json";
    relativeLaunch.scenario.general.runFolder = "results";
    relativeLaunch.scenario.general.outputName = "custom.json";
    relativeLaunch.configFile = "config/basic.toml";
    relativeLaunch.workingDirectory = workingDirectory;

    const auto relativePaths = ResolveRunPaths(relativeLaunch, launchTime);
    NS_TEST_ASSERT_MSG_EQ(relativePaths.configFile,
                          workingDirectory / "config/basic.toml",
                          "Relative config path used the wrong base");
    NS_TEST_ASSERT_MSG_EQ(relativePaths.traceFile,
                          workingDirectory / "data/trace.json",
                          "Relative trace path used the wrong base");
    NS_TEST_ASSERT_MSG_EQ(relativePaths.runFolder,
                          workingDirectory / "results",
                          "Relative run folder used the wrong base");
    NS_TEST_ASSERT_MSG_EQ(relativePaths.outputFile,
                          workingDirectory / "results/custom.json",
                          "Output path did not use the run folder");
    NS_TEST_ASSERT_MSG_EQ(relativePaths.usesAutomaticRunFolder,
                          false,
                          "Explicit run folder marked automatic");

    relativeLaunch.scenario.general.runFolder.reset();
    const auto automaticPaths = ResolveRunPaths(relativeLaunch, launchTime);
    NS_TEST_ASSERT_MSG_EQ(automaticPaths.runFolder,
                          workingDirectory / "run/26-08-25_14-03-09",
                          "Automatic run folder has the wrong local timestamp");
    NS_TEST_ASSERT_MSG_EQ(automaticPaths.outputFile,
                          workingDirectory / "run/26-08-25_14-03-09/custom.json",
                          "Automatic output path used the wrong folder");
    NS_TEST_ASSERT_MSG_EQ(automaticPaths.usesAutomaticRunFolder,
                          true,
                          "Automatic run folder not marked automatic");

    const auto absoluteBase = temporaryDirectory.Get() / "absolute";
    ScenarioLaunchConfig absoluteLaunch;
    absoluteLaunch.scenario.general.traceFile = (absoluteBase / "data/../trace.json").string();
    absoluteLaunch.scenario.general.runFolder = absoluteBase / "results/../run";
    absoluteLaunch.scenario.general.outputName = "absolute.json";
    absoluteLaunch.configFile = absoluteBase / "config/../basic.toml";
    absoluteLaunch.workingDirectory = workingDirectory;

    const auto absolutePaths = ResolveRunPaths(absoluteLaunch, launchTime);
    NS_TEST_ASSERT_MSG_EQ(absolutePaths.configFile,
                          (absoluteBase / "basic.toml").lexically_normal(),
                          "Absolute config path was prefixed or not normalized");
    NS_TEST_ASSERT_MSG_EQ(absolutePaths.traceFile,
                          (absoluteBase / "trace.json").lexically_normal(),
                          "Absolute trace path was prefixed or not normalized");
    NS_TEST_ASSERT_MSG_EQ(absolutePaths.runFolder,
                          (absoluteBase / "run").lexically_normal(),
                          "Absolute run folder was prefixed or not normalized");
    NS_TEST_ASSERT_MSG_EQ(absolutePaths.outputFile,
                          (absoluteBase / "run/absolute.json").lexically_normal(),
                          "Absolute output path was not normalized");
}

/**
 * @ingroup tests
 *
 * Verify safe scenario run directory preparation.
 */
class ScenarioRunDirectoryTestCase : public TestCase
{
  public:
    ScenarioRunDirectoryTestCase();

  private:
    void DoRun() override;
    void CheckFailure(const ResolvedRunPaths& paths,
                      std::string_view expectedTarget,
                      std::string_view description);
};

ScenarioRunDirectoryTestCase::ScenarioRunDirectoryTestCase()
    : TestCase("prepare scenario run directories without overwriting output")
{
}

void
ScenarioRunDirectoryTestCase::CheckFailure(const ResolvedRunPaths& paths,
                                           std::string_view expectedTarget,
                                           std::string_view description)
{
    try
    {
        PrepareRunDirectory(paths);
        NS_TEST_ASSERT_MSG_EQ(true, false, "Invalid path state accepted: " << description);
    }
    catch (const ScenarioConfigError& error)
    {
        const std::string message = error.what();
        NS_TEST_ASSERT_MSG_NE(message.find(expectedTarget),
                              std::string::npos,
                              "Error lacks target for " << description << ": " << message);
    }
}

void
ScenarioRunDirectoryTestCase::DoRun()
{
    ScopedTempDirectory temporaryDirectory(CreateTempDirFilename("scenario-run-path"));
    const auto root = temporaryDirectory.Get();
    const auto traceFile = root / "trace.json";
    WriteFile(traceFile);

    ResolvedRunPaths explicitPaths;
    explicitPaths.traceFile = traceFile;
    explicitPaths.runFolder = root / "nested/results";
    explicitPaths.outputFile = explicitPaths.runFolder / "output.json";
    PrepareRunDirectory(explicitPaths);
    NS_TEST_ASSERT_MSG_EQ(std::filesystem::is_directory(explicitPaths.runFolder),
                          true,
                          "Explicit nested run folder was not created");
    PrepareRunDirectory(explicitPaths);
    NS_TEST_ASSERT_MSG_EQ(std::filesystem::is_directory(explicitPaths.runFolder),
                          true,
                          "Existing explicit run folder was not reusable");

    WriteFile(explicitPaths.outputFile);
    CheckFailure(explicitPaths, explicitPaths.outputFile.string(), "existing output");

#ifndef _WIN32
    ResolvedRunPaths danglingOutputPaths;
    danglingOutputPaths.traceFile = traceFile;
    danglingOutputPaths.runFolder = root / "dangling-output-results";
    danglingOutputPaths.outputFile = danglingOutputPaths.runFolder / "output.json";
    std::filesystem::create_directories(danglingOutputPaths.runFolder);
    std::filesystem::create_symlink(root / "missing-output-target", danglingOutputPaths.outputFile);
    CheckFailure(danglingOutputPaths,
                 danglingOutputPaths.outputFile.string(),
                 "dangling output symlink");
#endif

    ResolvedRunPaths automaticPaths;
    automaticPaths.traceFile = traceFile;
    automaticPaths.runFolder = root / "run/26-08-25_14-03-09";
    automaticPaths.outputFile = automaticPaths.runFolder / "output.json";
    automaticPaths.usesAutomaticRunFolder = true;
    std::filesystem::create_directories(automaticPaths.runFolder);
    CheckFailure(automaticPaths,
                 automaticPaths.runFolder.string(),
                 "existing automatic timestamp folder");

    ResolvedRunPaths missingTracePaths;
    missingTracePaths.traceFile = root / "missing-trace.json";
    missingTracePaths.runFolder = root / "untouched-run/26-08-25_14-03-09";
    missingTracePaths.outputFile = missingTracePaths.runFolder / "output.json";
    missingTracePaths.usesAutomaticRunFolder = true;
    CheckFailure(missingTracePaths, missingTracePaths.traceFile.string(), "missing trace");
    NS_TEST_ASSERT_MSG_EQ(std::filesystem::exists(missingTracePaths.runFolder.parent_path()),
                          false,
                          "Run parent was created before missing trace rejection");

    const auto traceDirectory = root / "trace-directory";
    std::filesystem::create_directory(traceDirectory);
    ResolvedRunPaths directoryTracePaths;
    directoryTracePaths.traceFile = traceDirectory;
    directoryTracePaths.runFolder = root / "directory-trace-results";
    directoryTracePaths.outputFile = directoryTracePaths.runFolder / "output.json";
    CheckFailure(directoryTracePaths, traceDirectory.string(), "directory used as trace");
    NS_TEST_ASSERT_MSG_EQ(std::filesystem::exists(directoryTracePaths.runFolder),
                          false,
                          "Run folder was created before directory trace rejection");

    constexpr std::array<unsigned char, 7> rar4Signature{'R', 'a', 'r', '!', 0x1a, 0x07, 0x00};
    const auto rar4Trace = root / "rar4-signature.json";
    WriteBytes(rar4Trace, rar4Signature);
    ResolvedRunPaths rar4Paths;
    rar4Paths.traceFile = rar4Trace;
    rar4Paths.runFolder = root / "rar4-explicit-results";
    rar4Paths.outputFile = rar4Paths.runFolder / "output.json";
    CheckFailure(rar4Paths, rar4Trace.string(), "RAR4 trace signature");
    NS_TEST_ASSERT_MSG_EQ(std::filesystem::exists(rar4Paths.runFolder),
                          false,
                          "Explicit run folder was created before RAR4 rejection");

    constexpr std::array<unsigned char, 8>
        rar5Signature{'R', 'a', 'r', '!', 0x1a, 0x07, 0x01, 0x00};
    const auto rar5Trace = root / "rar5-signature.json";
    WriteBytes(rar5Trace, rar5Signature);
    ResolvedRunPaths rar5Paths;
    rar5Paths.traceFile = rar5Trace;
    rar5Paths.runFolder = root / "rar5-run/26-08-25_14-03-09";
    rar5Paths.outputFile = rar5Paths.runFolder / "output.json";
    rar5Paths.usesAutomaticRunFolder = true;
    CheckFailure(rar5Paths, rar5Trace.string(), "RAR5 trace signature");
    NS_TEST_ASSERT_MSG_EQ(std::filesystem::exists(rar5Paths.runFolder.parent_path()),
                          false,
                          "Automatic run parent was created before RAR5 rejection");

    const auto blockingFile = root / "blocking-file";
    WriteFile(blockingFile);
    ResolvedRunPaths filesystemErrorPaths;
    filesystemErrorPaths.traceFile = traceFile;
    filesystemErrorPaths.runFolder = blockingFile / "results";
    filesystemErrorPaths.outputFile = filesystemErrorPaths.runFolder / "output.json";
    CheckFailure(filesystemErrorPaths,
                 filesystemErrorPaths.runFolder.string(),
                 "run folder filesystem failure");
    NS_TEST_ASSERT_MSG_EQ(std::filesystem::is_regular_file(blockingFile),
                          true,
                          "Filesystem failure altered its blocking path");
}

} // namespace

std::vector<TestCase*>
CreateScenarioRunPathTestCases()
{
    return {new ScenarioRunPathResolutionTestCase, new ScenarioRunDirectoryTestCase};
}
