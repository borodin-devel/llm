#include "scenario-config.h"

#include <array>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>

namespace ns3
{

namespace
{

std::filesystem::path
ResolvePath(const std::filesystem::path& path, const std::filesystem::path& workingDirectory)
{
    if (path.is_absolute())
    {
        return path.lexically_normal();
    }
    return (workingDirectory / path).lexically_normal();
}

std::string
FormatLaunchTime(std::chrono::system_clock::time_point launchTime)
{
    const std::time_t time = std::chrono::system_clock::to_time_t(launchTime);
    std::tm localTime{};
#ifdef _WIN32
    if (localtime_s(&localTime, &time) != 0)
#else
    if (localtime_r(&time, &localTime) == nullptr)
#endif
    {
        throw ScenarioConfigError("failed to convert scenario launch time to local time");
    }

    std::array<char, 18> timestamp{};
    if (std::strftime(timestamp.data(), timestamp.size(), "%y-%m-%d_%H-%M-%S", &localTime) == 0)
    {
        throw ScenarioConfigError("failed to format scenario launch time for the run folder");
    }
    return timestamp.data();
}

[[noreturn]] void
ThrowFilesystemError(const std::string& operation,
                     const std::filesystem::path& target,
                     const std::error_code& errorCode)
{
    throw ScenarioConfigError(operation + " '" + target.string() + "': " + errorCode.message());
}

bool
PathExists(const std::filesystem::path& path, const std::string& operation)
{
    std::error_code errorCode;
    const auto status = std::filesystem::symlink_status(path, errorCode);
    if (errorCode == std::errc::no_such_file_or_directory)
    {
        return false;
    }
    if (errorCode)
    {
        ThrowFilesystemError(operation, path, errorCode);
    }
    return std::filesystem::exists(status);
}

void
CreateDirectories(const std::filesystem::path& path)
{
    std::error_code errorCode;
    std::filesystem::create_directories(path, errorCode);
    if (errorCode)
    {
        ThrowFilesystemError("cannot create run directory", path, errorCode);
    }
}

void
RequireDirectory(const std::filesystem::path& path)
{
    std::error_code errorCode;
    const bool isDirectory = std::filesystem::is_directory(path, errorCode);
    if (errorCode)
    {
        ThrowFilesystemError("cannot inspect run directory", path, errorCode);
    }
    if (!isDirectory)
    {
        throw ScenarioConfigError("run directory path is not a directory: '" + path.string() + "'");
    }
}

} // namespace

ResolvedRunPaths
ResolveRunPaths(const ScenarioLaunchConfig& launch,
                std::chrono::system_clock::time_point launchTime)
{
    ResolvedRunPaths paths;
    paths.configFile = ResolvePath(launch.configFile, launch.workingDirectory);
    paths.traceFile = ResolvePath(launch.scenario.general.traceFile, launch.workingDirectory);

    if (launch.scenario.general.runFolder)
    {
        paths.runFolder = ResolvePath(*launch.scenario.general.runFolder, launch.workingDirectory);
    }
    else
    {
        paths.runFolder = ResolvePath(std::filesystem::path("run") / FormatLaunchTime(launchTime),
                                      launch.workingDirectory);
        paths.usesAutomaticRunFolder = true;
    }

    paths.outputFile = (paths.runFolder / launch.scenario.general.outputName).lexically_normal();
    return paths;
}

void
PrepareRunDirectory(const ResolvedRunPaths& paths)
{
    std::error_code errorCode;
    const bool isRegularTrace = std::filesystem::is_regular_file(paths.traceFile, errorCode);
    if (errorCode)
    {
        ThrowFilesystemError("cannot inspect trace file", paths.traceFile, errorCode);
    }
    if (!isRegularTrace)
    {
        throw ScenarioConfigError("trace path is not a regular file: '" + paths.traceFile.string() +
                                  "'");
    }

    if (paths.usesAutomaticRunFolder)
    {
        CreateDirectories(paths.runFolder.parent_path());
        if (PathExists(paths.runFolder, "cannot inspect automatic run directory"))
        {
            throw ScenarioConfigError("automatic run directory already exists: '" +
                                      paths.runFolder.string() + "'");
        }

        const bool created = std::filesystem::create_directory(paths.runFolder, errorCode);
        if (errorCode)
        {
            ThrowFilesystemError("cannot create automatic run directory",
                                 paths.runFolder,
                                 errorCode);
        }
        if (!created)
        {
            throw ScenarioConfigError("automatic run directory already exists: '" +
                                      paths.runFolder.string() + "'");
        }
    }
    else if (PathExists(paths.runFolder, "cannot inspect run directory"))
    {
        RequireDirectory(paths.runFolder);
    }
    else
    {
        CreateDirectories(paths.runFolder);
    }

    if (PathExists(paths.outputFile, "cannot inspect output path"))
    {
        throw ScenarioConfigError("output path already exists: '" + paths.outputFile.string() +
                                  "'");
    }
}

} // namespace ns3
