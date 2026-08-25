# Scenario TOML Configuration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `llm_sample` positional arguments with a required TOML file, typed position-independent overrides for all 36 scenario fields, and safe run-directory/output handling.

**Architecture:** Vendored `toml++` parses a strict eight-section schema into nested typed structs. One registry owns each dotted TOML key and section-prefixed CLI override; focused files handle TOML, CLI, validation, and filesystem resolution. Scenario subsystems consume typed config sections while preserving current effective defaults.

**Tech Stack:** C++23/ns-3, vendored toml++ v3.4.0 single header, CMake, `std::filesystem`, ns-3 TestSuite.

**Spec:** `docs/superpowers/specs/2026-08-25-scenario-toml-config-design.md`

## Global Constraints

- Keep Wi-Fi fixed to `WIFI_STANDARD_80211ax` and transport fixed to TCP.
- Preserve effective defaults: `TcpHighSpeed`, 3 BSSs, 30 STAs/BSS, isolated channels, 10 ms statistics, seed 12345, and run 1.
- Require `general.trace_file` in TOML and exactly one `--config PATH` for a real run.
- Remove legacy positional parsing.
- Apply `compiled defaults < TOML < CLI` precedence.
- Resolve config, trace, and run paths relative to process CWD.
- Never overwrite output or reuse an automatic timestamp directory.
- Register 36 unique TOML/CLI fields; `basic_config.toml` has 35 active assignments plus commented optional `run_folder`.
- Reject unknown names, duplicate flags, and implicit boolean flags.
- Keep implementation files below 600 lines and document new public APIs.
- Update English and Russian documentation together.
- Verify with examples/tests/logs/warnings/warnings-as-errors enabled.

---

### Task 1: Vendor toml++ and define the typed schema

**Files:**
- Create: `lib/toml.hpp`
- Create: `lib/tomlplusplus-LICENSE`
- Create: `config/basic_config.toml`
- Modify: `CMakeLists.txt`
- Modify: `examples/scenario-config.h`
- Modify: `examples/scenario-config.cc`
- Modify: `examples/sample-scenario.cc`
- Modify: `test/scenario-config-test-suite.cc`

**Interfaces:**
- Consumes: hardcoded defaults in scenario, topology, distribution, statistics, TCP, RNG, and logging.
- Produces: `GeneralConfig`, `DurationMode`, `SimulationConfig`, `TopologyConfig`, `DistributionConfig`, `WifiBandConfig`, `WifiConfig`, `TcpConfig`, `StatisticsConfig`, `LoggingConfig`, and nested `ScenarioConfig` exactly as specified.

- [ ] **Step 1: Write failing default-schema tests**

Replace old positional-default tests with literal assertions for every typed default. The test body must include:

```cpp
ScenarioConfig config;
NS_TEST_ASSERT_MSG_EQ(config.general.traceFile.empty(), true, "Trace must have no default");
NS_TEST_ASSERT_MSG_EQ(config.general.runFolder.has_value(), false, "Run folder must be optional");
NS_TEST_ASSERT_MSG_EQ(config.general.outputName, "mac-node-stats.json", "Wrong output name");
NS_TEST_ASSERT_MSG_EQ(config.simulation.durationMode, DurationMode::AUTO, "Wrong duration mode");
NS_TEST_ASSERT_MSG_EQ(config.simulation.fixedDurationSeconds, 0.0, "Wrong fixed duration");
NS_TEST_ASSERT_MSG_EQ(config.simulation.autoTailSeconds, 2.0, "Wrong tail");
NS_TEST_ASSERT_MSG_EQ(config.simulation.rngSeed, 12345, "Wrong seed");
NS_TEST_ASSERT_MSG_EQ(config.simulation.rngRun, 1, "Wrong run");
NS_TEST_ASSERT_MSG_EQ(config.topology.bssCount, 3, "Wrong BSS count");
NS_TEST_ASSERT_MSG_EQ(config.topology.stationsPerBss, 30, "Wrong STA count");
NS_TEST_ASSERT_MSG_EQ(config.topology.bssSpacingM, 100.0, "Wrong spacing");
NS_TEST_ASSERT_MSG_EQ(config.topology.stationRadiusM, 5.0, "Wrong radius");
NS_TEST_ASSERT_MSG_EQ(config.topology.isolateBssChannels, true, "Wrong isolation");
NS_TEST_ASSERT_MSG_EQ(config.topology.ssidPrefix, "llm-ap-", "Wrong SSID prefix");
NS_TEST_ASSERT_MSG_EQ(config.topology.apSinkPort, 10000, "Wrong AP port");
NS_TEST_ASSERT_MSG_EQ(config.topology.stationSinkBasePort, 9000, "Wrong STA port");
NS_TEST_ASSERT_MSG_EQ(config.topology.generatorStartSeconds, 1.0, "Wrong start");
NS_TEST_ASSERT_MSG_EQ(config.distribution.maxAgentsPerStation, 832, "Wrong cap");
NS_TEST_ASSERT_MSG_EQ(config.distribution.lowContentionPriority, true, "Wrong policy");
NS_TEST_ASSERT_MSG_EQ(config.distribution.slotMs, 10, "Wrong slot");
NS_TEST_ASSERT_MSG_EQ(config.wifi.band, WifiBandConfig::BAND_5_GHZ, "Wrong band");
NS_TEST_ASSERT_MSG_EQ(config.wifi.channelNumber, 0, "Wrong channel");
NS_TEST_ASSERT_MSG_EQ(config.wifi.bandwidthMhz, 20, "Wrong bandwidth");
NS_TEST_ASSERT_MSG_EQ(config.wifi.primary20Index, 0, "Wrong primary index");
NS_TEST_ASSERT_MSG_EQ(config.wifi.rateManager, "ns3::MinstrelHtWifiManager", "Wrong manager");
NS_TEST_ASSERT_MSG_EQ(config.wifi.activeProbing, true, "Wrong probing");
NS_TEST_ASSERT_MSG_EQ(config.tcp.congestionControl, "ns3::TcpHighSpeed", "Wrong TCP type");
NS_TEST_ASSERT_MSG_EQ(config.tcp.segmentSizeBytes, 1460, "Wrong segment");
NS_TEST_ASSERT_MSG_EQ(config.tcp.sendBufferBytes, 33554432, "Wrong send buffer");
NS_TEST_ASSERT_MSG_EQ(config.tcp.receiveBufferBytes, 33554432, "Wrong receive buffer");
NS_TEST_ASSERT_MSG_EQ(config.statistics.windowMs, 10, "Wrong window");
NS_TEST_ASSERT_MSG_EQ(config.logging.sampleScenarioLevel, "info", "Wrong scenario log");
NS_TEST_ASSERT_MSG_EQ(config.logging.apGeneratorLevel, "warn", "Wrong AP log");
NS_TEST_ASSERT_MSG_EQ(config.logging.staGeneratorLevel, "warn", "Wrong STA log");
NS_TEST_ASSERT_MSG_EQ(config.logging.trafficSinkLevel, "warn", "Wrong sink log");
NS_TEST_ASSERT_MSG_EQ(config.logging.contentionDistributionLevel, "info", "Wrong placement log");
```

- [ ] **Step 2: Build and verify red**

```bash
./ns3 build llm-test
```

Expected: FAIL because nested config types do not exist.

- [ ] **Step 3: Vendor pinned upstream files**

```bash
vendor_tmp="$(mktemp -d /tmp/llm-toml-vendor.XXXXXX)"
curl -fsSL https://raw.githubusercontent.com/marzer/tomlplusplus/v3.4.0/toml.hpp \
  -o "$vendor_tmp/toml.hpp"
curl -fsSL https://raw.githubusercontent.com/marzer/tomlplusplus/v3.4.0/LICENSE \
  -o "$vendor_tmp/tomlplusplus-LICENSE"
printf '%s  %s\n' 6b5172ad4dd6519aec67b919181fa7a38a2234131e5b2afa232dfe444819783e \
  "$vendor_tmp/toml.hpp" | sha256sum -c -
printf '%s  %s\n' 529bc3900a9571e49db285b0df432397e70b881cc3bf48de6667ae74ff4b06d8 \
  "$vendor_tmp/tomlplusplus-LICENSE" | sha256sum -c -
install -m 0644 "$vendor_tmp/toml.hpp" contrib/llm/lib/toml.hpp
install -m 0644 "$vendor_tmp/tomlplusplus-LICENSE" contrib/llm/lib/tomlplusplus-LICENSE
VENDOR_TMP="$vendor_tmp" python3 -c \
  'import os,shutil; p=os.environ["VENDOR_TMP"]; assert p.startswith("/tmp/llm-toml-vendor."); shutil.rmtree(p)'
```

Expected sizes: 485931 and 1094 bytes.

- [ ] **Step 4: Register dependency and implement structs**

Add `lib/toml.hpp` to `header_files` with an adjacent v3.4.0/MIT/source comment. Keep the license tracked. Replace flat config with exact spec structs, including `<optional>` and `<cstdint>`, complete Doxygen, and unit comments.

Keep the tree buildable during migration: update the temporary positional
parser to fill nested members and update `sample-scenario.cc` field reads to
`general`, `simulation`, `topology`, and `wifi`. Do not add TOML behavior yet.
Task 3 removes this transitional positional parser.

- [ ] **Step 5: Create the full starter TOML**

Copy the exact eight-section block from the approved spec into `config/basic_config.toml`. Keep a comment directly above every field and leave only `run_folder` commented.

Verify counts:

```bash
python3 - <<'PY'
from pathlib import Path
import re
s=Path('contrib/llm/config/basic_config.toml').read_text()
assert len(re.findall(r'^\[[a-z]+\]$',s,re.M)) == 8
assert len(re.findall(r'^[a-z][a-z0-9_]*\s*=',s,re.M)) == 35
assert '# run_folder = ' in s
PY
```

- [ ] **Step 6: Build, test, and commit**

```bash
./ns3 build llm-test
./test.py -s llm --no-build
git -C contrib/llm add CMakeLists.txt config lib examples/scenario-config.h \
  examples/scenario-config.cc examples/sample-scenario.cc test/scenario-config-test-suite.cc
git -C contrib/llm commit -m "llm: Add typed TOML configuration schema"
```

---

### Task 2: Option registry and strict TOML loading

**Files:**
- Create: `examples/scenario-config-internal.h`
- Create: `examples/scenario-config-toml.cc`
- Modify: `examples/scenario-config.h`
- Modify: `examples/scenario-config.cc`
- Modify: `examples/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `test/scenario-config-test-suite.cc`

**Interfaces:**
- Consumes: nested config and `ns3/toml.hpp`.
- Produces public `ConfigValueType`, `ConfigOptionInfo`, `GetScenarioConfigOptionInfo()`, `ScenarioConfigError`, and `LoadTomlConfig(path)`; private `ConfigOption` owns TOML/CLI setter callbacks.

- [ ] **Step 1: Add failing registry tests**

```cpp
const auto& options = GetScenarioConfigOptionInfo();
NS_TEST_ASSERT_MSG_EQ(options.size(), 36, "Wrong option count");
std::set<std::string> tomlPaths;
std::set<std::string> cliFlags;
for (const auto& option : options)
{
    NS_TEST_ASSERT_MSG_EQ(tomlPaths.insert(option.tomlPath).second, true,
                          "Duplicate TOML path " << option.tomlPath);
    NS_TEST_ASSERT_MSG_EQ(cliFlags.insert(option.cliFlag).second, true,
                          "Duplicate CLI flag " << option.cliFlag);
}
```

Parse a fixture overriding one string, enum, integer, float, and boolean:

```toml
[general]
trace_file = "trace.json"
output_name = "custom.json"
[simulation]
duration_mode = "fixed"
fixed_duration_seconds = 12.5
[topology]
bss_count = 4
[distribution]
low_contention_priority = false
[wifi]
band = "6GHz"
bandwidth_mhz = 80
[tcp]
segment_size_bytes = 1200
[statistics]
window_ms = 25
[logging]
sample_scenario_level = "debug"
```

Assert every literal typed result. Add failure fixtures for missing `general`, missing `trace_file`, unknown section, unknown field, wrong scalar type, `4294967296` into uint32, and malformed TOML with known line number.

- [ ] **Step 2: Build and verify red**

```bash
./ns3 build llm-test
```

Expected: FAIL because registry/loading APIs are absent.

- [ ] **Step 3: Implement exactly 36 registry mappings**

Derive each CLI name deterministically as `--<section>-<field>` with every
underscore changed to a hyphen. For example,
`topology.station_sink_base_port` becomes
`--topology-station-sink-base-port`. Factories must perform exact TOML scalar
access, destination range checks, strict enum parsing, and strict CLI parsing.
Public option info is projected from the same private registry.

Registry coverage begins with:

```text
general.trace_file -> --general-trace-file
general.run_folder -> --general-run-folder
general.output_name -> --general-output-name
```

and ends with:

```text
logging.sample_scenario_level -> --logging-sample-scenario-level
logging.ap_generator_level -> --logging-ap-generator-level
logging.sta_generator_level -> --logging-sta-generator-level
logging.traffic_sink_level -> --logging-traffic-sink-level
logging.contention_distribution_level -> --logging-contention-distribution-level
```

Required counts:

```text
general 3, simulation 5, topology 9, distribution 3,
wifi 6, tcp 4, statistics 1, logging 5 = 36
```

- [ ] **Step 4: Implement strict TOML loading**

Use `toml::parse_file()`. Validate root tables and child fields against registry sets before applying values. Require `general.trace_file` to be present/nonempty. Preserve defaults for omissions. Convert `toml::parse_error` to `ScenarioConfigError` retaining source path, line, column, and description.

- [ ] **Step 5: Register sources, run, and commit**

Add `scenario-config-toml.cc` to example and test sources; keep the internal header private.

```bash
./ns3 build llm-test llm_sample
./test.py -s llm --no-build
git -C contrib/llm add CMakeLists.txt examples test
git -C contrib/llm commit -m "llm: Add strict TOML scenario loading"
```

---

### Task 3: Position-independent CLI and overrides

**Files:**
- Create: `examples/scenario-config-cli.cc`
- Modify: `examples/scenario-config.h`
- Modify: `examples/scenario-config-internal.h`
- Modify: `examples/scenario-config.cc`
- Modify: `examples/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `test/scenario-config-test-suite.cc`

**Interfaces:**
- Consumes: registry and `LoadTomlConfig()`.
- Produces `ScenarioLaunchConfig`, `ScenarioCommandLineResult`, `ParseScenarioArguments(arguments, workingDirectory)`, and generated `PrintScenarioUsage()`.

- [ ] **Step 1: Write failing order/precedence tests**

Given TOML bandwidth 40, these all produce 80:

```cpp
{"--config", path, "--wifi-bandwidth-mhz", "80"}
{"--wifi-bandwidth-mhz", "80", "--config", path}
{"--wifi-bandwidth-mhz", "80", "--simulation-rng-run", "9", "--config", path}
```

Add failures for empty args, positional token, missing config value, duplicate config, missing override value, duplicate override, unknown flag, and boolean `TRUE`. Assert `{"--help"}` succeeds without config and help contains `--config` plus all 36 flags.

- [ ] **Step 2: Build and verify red**

```bash
./ns3 build llm-test
```

Expected: FAIL because new CLI result/API is absent.

- [ ] **Step 3: Implement two-pass parsing**

First consume recognized flag/value pairs and record raw overrides. Treat the token after a recognized flag as its value even when it starts with `-`. Reject duplicates via a set. Resolve relative config path against supplied CWD, require a regular file, load TOML, then apply overrides through registry callbacks.

Update `sample-scenario.cc` in this task to call the new overload with
`std::filesystem::current_path()` and read
`commandLine.launch.scenario`. Continue using the direct trace/output members
temporarily; Task 5/8 installs final run-path preparation. This keeps
`llm_sample` buildable after the old parser is removed.

CLI parsers consume the complete string, range-check numerics, accept only lowercase `true`/`false`, and use strict enum spelling.

- [ ] **Step 4: Generate help from registry**

Print:

```text
Usage: PROGRAM --config <config.toml> [--section-field <value> ...]
```

Then list `--help`, `--config`, and every registry flag with expected type, description, and TOML key. No second hand-maintained option list.

- [ ] **Step 5: Run and commit**

```bash
./ns3 build llm-test llm_sample
./test.py -s llm --no-build
git -C contrib/llm add CMakeLists.txt examples test
git -C contrib/llm commit -m "llm: Add typed scenario CLI overrides"
```

### Task 4: Cross-field validation and runtime conversions

**Files:**
- Modify: `examples/scenario-config.h`
- Modify: `examples/scenario-config.cc`
- Create: `test/scenario-config-validation-test-suite.cc`
- Modify: `test/llm-test-suite.h`
- Modify: `test/llm-test-suite.cc`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: complete typed config after overrides.
- Produces `ValidateScenarioConfig()`, `ToWifiPhyBand()`, `ParseScenarioLogLevel()`, `ResolveWifiManagerType()`, and `ResolveTcpCongestionType()`.

- [ ] **Step 1: Add failing table-driven validation tests**

Each row mutates one valid config and asserts the dotted key appears in the error. Cover:

```text
general.trace_file empty
general.output_name empty, nested/path.json, ..json, result.txt
simulation fixed with zero duration
simulation negative, NaN, or Inf duration/tail
topology nonpositive counts
topology negative, NaN, or Inf spacing/radius/start
topology empty SSID prefix
topology zero ports and station-port overflow
distribution negative cap and nonpositive slot
wifi invalid bandwidth and primary index
wifi unknown manager and wrong-parent TypeId
tcp unknown/wrong-parent type
tcp zero segment/buffers and segment larger than buffer
statistics zero window
invalid log level
```

Positive cases cover all three bands, all four widths, eight log levels, unlimited cap, and fixed duration.

- [ ] **Step 2: Build and verify red**

```bash
./ns3 build llm-test
```

Expected: FAIL because validation/conversion APIs are absent.

- [ ] **Step 3: Implement pure validation and errors**

Use `std::isfinite`, checked integer arithmetic, filesystem filename-component checks, and strict enum maps. Error shape:

```text
invalid wifi.bandwidth_mhz (--wifi-bandwidth-mhz): expected 20, 40, 80, or 160; got 30
```

Do not mutate ns-3 defaults or filesystem state.

- [ ] **Step 4: Validate ns-3 TypeIds and mappings**

Use `TypeId::LookupByNameFailSafe()`. Require manager types to derive from `WifiRemoteStationManager` and congestion types from `TcpCongestionOps`. Implement:

```text
2.4GHz -> WIFI_PHY_BAND_2_4GHZ
5GHz   -> WIFI_PHY_BAND_5GHZ
6GHz   -> WIFI_PHY_BAND_6GHZ

off      -> nullopt
error    -> LOG_LEVEL_ERROR
warn     -> LOG_LEVEL_WARN
info     -> LOG_LEVEL_INFO
debug    -> LOG_LEVEL_DEBUG
function -> LOG_LEVEL_FUNCTION
logic    -> LOG_LEVEL_LOGIC
all      -> LOG_LEVEL_ALL
```

- [ ] **Step 5: Register, test, and commit**

```bash
./ns3 build llm-test
./test.py -s llm --no-build
git -C contrib/llm add CMakeLists.txt examples test
git -C contrib/llm commit -m "llm: Validate scenario configuration"
```

---

### Task 5: Run path resolution and directory safety

**Files:**
- Create: `examples/scenario-run-path.cc`
- Modify: `examples/scenario-config.h`
- Create: `test/scenario-run-path-test-suite.cc`
- Modify: `test/llm-test-suite.h`
- Modify: `test/llm-test-suite.cc`
- Modify: `examples/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: validated config, resolved config file, captured CWD, and launch time.
- Produces:

```cpp
struct ResolvedRunPaths
{
    std::filesystem::path configFile;
    std::filesystem::path traceFile;
    std::filesystem::path runFolder;
    std::filesystem::path outputFile;
    bool usesAutomaticRunFolder{false};
};
```

and exact signatures from the spec for `ResolveRunPaths()` and `PrepareRunDirectory()`.

- [ ] **Step 1: Add failing pure path tests**

With temp CWD and injected local time `2026-08-25 14:03:09`, assert:

```text
relative config -> <cwd>/config/basic.toml
relative trace  -> <cwd>/data/trace.json
explicit run    -> <cwd>/results
output          -> <cwd>/results/custom.json
automatic run   -> <cwd>/run/26-08-25_14-03-09
```

Add absolute path cases proving no CWD prefix.

- [ ] **Step 2: Add failing filesystem tests**

Cover explicit nested creation, reuse of existing explicit directory,
existing output rejection, existing automatic timestamp rejection, missing
trace rejection before directory creation, directory-as-trace rejection, and
filesystem-error path diagnostics.

- [ ] **Step 3: Build and verify red**

```bash
./ns3 build llm-test
```

Expected: FAIL because run-path APIs are absent.

- [ ] **Step 4: Implement deterministic resolution**

Normalize absolute paths with `lexically_normal()`. Prefix relative values
with supplied CWD. Format local time `%y-%m-%d_%H-%M-%S`, using `localtime_r`
on POSIX and `localtime_s` on Windows.

- [ ] **Step 5: Implement no-overwrite preparation**

Validate trace regular-file status first. Explicit mode creates/reuses a
directory. Automatic mode creates parent `run`, rejects an existing timestamp
folder, and creates exactly it. Both modes reject existing output and convert
`std::error_code` failures to `ScenarioConfigError`.

- [ ] **Step 6: Register, test, and commit**

```bash
./ns3 build llm-test
./test.py -s llm --no-build
git -C contrib/llm add CMakeLists.txt examples test
git -C contrib/llm commit -m "llm: Add safe scenario run directories"
```

---

### Task 6: Topology, Wi-Fi, distribution, RNG, and TCP integration

**Files:**
- Modify: `examples/scenario-topology.h`
- Modify: `examples/scenario-topology.cc`
- Modify: `examples/sample-scenario.cc`
- Create: `test/scenario-topology-test-suite.cc`
- Modify: `test/llm-test-suite.h`
- Modify: `test/llm-test-suite.cc`
- Modify: `examples/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: validated topology, Wi-Fi, distribution, simulation, and TCP config.
- Produces `CreateDefaultYansChannel()`, `SelectBssChannel()`, and `BuildChannelSettings()` plus config-driven `SetupApGroup()`.

- [ ] **Step 1: Add failing topology helper tests**

```cpp
Ptr<YansWifiChannel> shared = CreateDefaultYansChannel();
NS_TEST_ASSERT_MSG_EQ(SelectBssChannel(false, shared), shared, "Shared channel changed");
NS_TEST_ASSERT_MSG_NE(SelectBssChannel(true, shared), shared, "Isolation reused shared channel");
NS_TEST_ASSERT_MSG_NE(SelectBssChannel(true, shared),
                      SelectBssChannel(true, shared),
                      "Two isolated BSSs shared one channel");

WifiConfig wifi;
wifi.channelNumber = 36;
wifi.bandwidthMhz = 80;
wifi.primary20Index = 2;
NS_TEST_ASSERT_MSG_EQ(BuildChannelSettings(wifi),
                      "{36, 80, BAND_5GHZ, 2}",
                      "Wrong channel tuple");
```

- [ ] **Step 2: Build and verify red**

```bash
./ns3 build llm-test llm_sample
```

Expected: FAIL because helper and topology signatures are absent.

- [ ] **Step 3: Refactor topology inputs**

Pass typed topology/Wi-Fi sections and shared-channel pointer to
`SetupApGroup()`. Replace constants for positions, radius, SSID, sink ports,
generator start, probing, channel tuple, and rate manager. Preserve default
Yans propagation models.

- [ ] **Step 4: Configure RNG and TCP before topology**

Before the first Internet stack installation:

```cpp
RngSeedManager::SetSeed(config.simulation.rngSeed);
RngSeedManager::SetRun(config.simulation.rngRun);
Config::SetDefault("ns3::TcpL4Protocol::SocketType",
                   TypeIdValue(ResolveTcpCongestionType(config.tcp.congestionControl)));
Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(config.tcp.segmentSizeBytes));
Config::SetDefault("ns3::TcpSocket::SndBufSize", UintegerValue(config.tcp.sendBufferBytes));
Config::SetDefault("ns3::TcpSocket::RcvBufSize", UintegerValue(config.tcp.receiveBufferBytes));
```

Remove both old congestion defaults, especially late `TcpLinuxReno`. Map
typed topology/distribution fields into the existing placement call.

- [ ] **Step 5: Build, test, and commit**

```bash
./ns3 build llm-test llm_sample
./test.py -s llm --no-build
git -C contrib/llm add CMakeLists.txt examples test
git -C contrib/llm commit -m "llm: Configure topology and transport from TOML"
```

### Task 7: Configurable statistics and logging

**Files:**
- Modify: `examples/wifi-statistics.h`
- Modify: `examples/wifi-statistics-internal.h`
- Modify: `examples/wifi-statistics-owner.cc`
- Modify: `examples/wifi-statistics.cc`
- Modify: `examples/wifi-statistics-json.cc`
- Modify: `examples/wifi-statistics-report.cc`
- Modify: `examples/sample-scenario.cc`
- Modify: `test/wifi-statistics-test-suite.cc`

**Interfaces:**
- Consumes: `StatisticsConfig.windowMs` and `LoggingConfig`.
- Produces `WifiStatistics(coordinator, windowMs)` and `ConfigureScenarioLogging(logging)`.

- [ ] **Step 1: Add failing non-default-window tests**

For 25 ms windows assert:

```text
epoch -> window 0
epoch + 24,999 us -> window 0
epoch + 25,000 us -> window 1
1000 bytes / 25 ms -> bw 0.32 Mbit/s
JSON window_ms -> 25
first emitted timestamp -> 25
```

Read JSON from the real serializer for the last three assertions.

- [ ] **Step 2: Add failing log mapping tests**

Assert each allowed string maps to its exact optional `LogLevel`, `off` maps to
`nullopt`, and `verbose` is rejected.

- [ ] **Step 3: Build and verify red**

```bash
./ns3 build llm-test
```

Expected: FAIL because window state and logging configuration are absent.

- [ ] **Step 4: Move window width into state**

Store checked `windowMs` and `windowUs` in `WifiStatisticsState`. Replace all
duplicated 10 ms constants in collection and JSON. Keep:

```cpp
const double bwMbps = static_cast<double>(bytes) * 8.0 /
                      static_cast<double>(statistics.windowUs);
```

- [ ] **Step 5: Implement configured logging**

Enable exactly `SampleScenario`, `APGenerator`, `StaLlmGenerator`,
`TrafficSink`, and `ContentionAwareAgentDistribution` at mapped levels. Skip
enable calls for `off`; remove hardcoded calls from main.

- [ ] **Step 6: Run and commit**

```bash
./ns3 build llm-test llm_sample
./test.py -s llm --no-build
git -C contrib/llm add examples test
git -C contrib/llm commit -m "llm: Configure statistics and logging"
```

---

### Task 8: Final startup lifecycle, smoke migration, and documentation

**Files:**
- Modify: `examples/sample-scenario.cc`
- Modify: `test/examples-to-run.py`
- Modify: `README.md`
- Modify: `README_RU.md`
- Modify: `config/basic_config.toml` only for diagnostic wording corrections

**Interfaces:**
- Consumes: all APIs from Tasks 1-7.
- Produces final `--config` public contract and resolved run output.

- [ ] **Step 1: Install final main startup order**

```cpp
const auto launchTime = std::chrono::system_clock::now();
const auto workingDirectory = std::filesystem::current_path();
const std::vector<std::string> arguments(argv + 1, argv + argc);
const ScenarioCommandLineResult commandLine =
    ParseScenarioArguments(arguments, workingDirectory);
```

Help prints usage and returns 0. Invalid input prints error plus usage and
returns 1. Otherwise validate, resolve paths, and prepare output before ns-3
object creation.

Compute duration as:

```cpp
const double maximumDurationMs =
    config.simulation.durationMode == DurationMode::AUTO
        ? parsedTrace.experimentDurationMs + config.simulation.autoTailSeconds * 1000.0
        : config.simulation.fixedDurationSeconds * 1000.0;
```

Print resolved config/trace/run/output and major choices. Write JSON only to
`resolvedPaths.outputFile.string()`.

- [ ] **Step 2: Migrate registered smoke command**

```python
(
    "llm_sample --config ../../contrib/llm/config/basic_config.toml "
    "--general-trace-file ../../contrib/llm/test/data/minimal-trace.json "
    "--general-run-folder . "
    "--general-output-name llm-smoke-stats.json "
    "--simulation-duration-mode fixed "
    "--simulation-fixed-duration-seconds 0.2",
    "True",
    "False",
),
```

- [ ] **Step 3: Verify public executable behavior**

```bash
./ns3 run "llm_sample --help"
```

Expected: return 0, `--config`, and all 36 flags.

```bash
./ns3 run "llm_sample"
```

Expected: return 1 with missing-config diagnostic.

Run one temporary small smoke:

```bash
run_tmp="$(mktemp -d /tmp/llm-config-smoke.XXXXXX)"
./ns3 run \
  "llm_sample --config contrib/llm/config/basic_config.toml \
  --general-run-folder $run_tmp \
  --general-output-name config-smoke.json \
  --simulation-duration-mode fixed \
  --simulation-fixed-duration-seconds 0.2"
test -s "$run_tmp/config-smoke.json"
RUN_TMP="$run_tmp" python3 -c \
  'import os,shutil; p=os.environ["RUN_TMP"]; assert p.startswith("/tmp/llm-config-smoke."); shutil.rmtree(p)'
```

- [ ] **Step 4: Update English documentation**

Replace positional commands with `--config`. Document all eight sections,
precedence, CWD path base, run/output collision rules, every override family,
shared channels, corrected TCP setup, and configurable statistics units.

- [ ] **Step 5: Mirror semantic changes in Russian**

Keep commands, field names, formulas, table rows, and links equal between
README files while translating explanations naturally.

- [ ] **Step 6: Run complete formatting and verification**

```bash
./utils/check-style-clang-format.py --fix \
  contrib/llm/examples contrib/llm/model contrib/llm/test
./utils/check-style-clang-format.py \
  contrib/llm/examples contrib/llm/model contrib/llm/test
./ns3 configure --enable-examples --enable-tests --enable-logs \
  --enable-warnings --enable-werror
./ns3 build llm-test llm_sample
./test.py -s llm
./test.py -e 'llm_sample*'
git -C contrib/llm diff --check
```

Expected: all pass; smoke output stays in test-runner temp; no ns-3-root `run/`
directory remains.

- [ ] **Step 7: Check structure and coverage**

```bash
wc -l contrib/llm/examples/*.cc contrib/llm/examples/*.h | sort -nr | head -25
python3 - <<'PY'
from pathlib import Path
import re
s=Path('contrib/llm/config/basic_config.toml').read_text()
assert len(re.findall(r'^\[[a-z]+\]$',s,re.M)) == 8
assert len(re.findall(r'^[a-z][a-z0-9_]*\s*=',s,re.M)) == 35
assert '# run_folder = ' in s
PY
git -C contrib/llm status --short
```

Expected: no implementation file exceeds 600 lines without a documented
cohesive reason; schema counts match; only intended changes remain.

- [ ] **Step 8: Commit final integration**

```bash
git -C contrib/llm add config examples test README.md README_RU.md CMakeLists.txt
git -C contrib/llm commit -m "llm: Complete TOML scenario configuration"
```

- [ ] **Step 9: Review implementation range**

```bash
git -C contrib/llm log --oneline 999ac35..HEAD
git -C contrib/llm diff --stat 999ac35..HEAD
```

Expected: focused commits, no trace-data changes, no outer ns-3 edits, and no
unrelated files.
