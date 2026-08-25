# Scenario TOML Configuration Design

## Goal

Replace the positional `llm_sample` command line with a required TOML
configuration file plus position-independent, typed command-line overrides.
Move every scenario control approved in this design behind one clear typed
schema while preserving the current effective defaults.

## Approved Decisions

- Vendor the official `toml++` v3.4.0 single header and MIT license under
  `contrib/llm/lib/`.
- Use typed nested configuration structs.
- Use one option registry as the source of truth for TOML keys, CLI flags,
  types, application, and diagnostics.
- Require one `--config PATH` argument for every real launch.
- Use section-prefixed kebab-case overrides such as
  `--wifi-bandwidth-mhz 80`.
- Use explicit `true` or `false` values for boolean overrides.
- Resolve relative config, trace, and run paths against the process current
  working directory.
- Treat an explicit `general.run_folder` as the exact output directory.
- When `general.run_folder` is absent, create
  `./run/YY-MM-DD_hh-mm-ss` using local launch time.
- Refuse automatically generated directory collisions and output-file
  collisions.
- Keep `general.output_name` optional with default
  `mac-node-stats.json` and require a plain `.json` filename.
- Remove legacy positional arguments rather than maintaining two interfaces.
- Keep Wi-Fi fixed to 802.11ax and transport fixed to TCP. The configuration
  controls their scenario parameters, not arbitrary protocol replacement.

## Non-Goals

- Supporting JSON, YAML, INI, or legacy positional configuration.
- Supporting TOML arrays of multiple scenarios or parameter sweeps.
- Supporting arbitrary ns-3 attributes through untyped key/value strings.
- Making the config path the base directory for other relative paths.
- Overwriting existing results.
- Adding a full experiment-management database.
- Making IEEE 802.11ax features such as OFDMA or MU-MIMO configurable in this
  change.

## Vendored TOML Dependency

Add these files:

```text
lib/toml.hpp                 official toml++ v3.4.0 single header
lib/tomlplusplus-LICENSE     upstream MIT license
```

Record the upstream version and source URL in a short comment adjacent to the
header registration in `CMakeLists.txt` or in the license notice. Do not edit
the assembled upstream header. Register `lib/toml.hpp` as a module header and
include it as `ns3/toml.hpp`.

`toml++` is appropriate because it is C++17-compatible, header-only,
dependency-free, supports TOML 1.0, and reports parse errors with source
locations. The ns-3 build already uses a newer C++ language level.

Official upstream references:

- <https://github.com/marzer/tomlplusplus>
- <https://github.com/marzer/tomlplusplus/blob/v3.4.0/toml.hpp>
- <https://github.com/marzer/tomlplusplus/blob/v3.4.0/LICENSE>

## Complete Configuration Schema

Create `config/basic_config.toml` with a comment immediately above every
field. Every field below appears in that file. `general.run_folder` is shown
as a commented optional assignment so its absence demonstrates timestamped
run-directory behavior.

```toml
[general]
# Required input JSON trace. Relative paths use the process working directory.
trace_file = "contrib/llm/test/data/minimal-trace.json"

# Optional exact output directory. Omit to create ./run/YY-MM-DD_hh-mm-ss.
# run_folder = "./run/custom"

# JSON filename written inside the resolved run folder.
output_name = "mac-node-stats.json"

[simulation]
# Experiment duration policy: "auto" or "fixed".
duration_mode = "auto"

# Positive duration in seconds when duration_mode is "fixed".
fixed_duration_seconds = 0.0

# Extra seconds after trace end in automatic mode.
auto_tail_seconds = 2.0

# Deterministic ns-3 random-number seed.
rng_seed = 12345

# Deterministic ns-3 run/substream number.
rng_run = 1

[topology]
# Number of independent AP/BSS groups.
bss_count = 3

# Number of physical stations created in each BSS.
stations_per_bss = 30

# Coordinate increment on X, Y, and Z between neighboring APs, in meters.
bss_spacing_m = 100.0

# Radius of the uniform station disc around each AP, in meters.
station_radius_m = 5.0

# True creates one YansWifiChannel per BSS; false shares one channel.
isolate_bss_channels = true

# Prefix used to build one SSID per BSS.
ssid_prefix = "llm-ap-"

# TCP sink port on every AP.
ap_sink_port = 10000

# First TCP sink port assigned to STA 0; later STAs increment it by one.
station_sink_base_port = 9000

# Simulation time when generators begin TCP setup, in seconds.
generator_start_seconds = 1.0

[distribution]
# Maximum application agents assigned to one physical STA; zero is unlimited.
max_agents_per_station = 832

# True minimizes active physical STA/slot pairs; false maximizes STA use first.
low_contention_priority = true

# Uplink-overlap slot width in milliseconds.
slot_ms = 10

[wifi]
# Operating band for fixed 802.11ax: "2.4GHz", "5GHz", or "6GHz".
band = "5GHz"

# IEEE channel number; zero asks ns-3 to select the first valid channel.
channel_number = 0

# Channel width in MHz: 20, 40, 80, or 160.
bandwidth_mhz = 20

# Index of the primary 20 MHz subchannel.
primary_20_index = 0

# Registered ns-3 WifiRemoteStationManager TypeId name.
rate_manager = "ns3::MinstrelHtWifiManager"

# Whether non-AP STAs actively probe for their configured SSID.
active_probing = true

[tcp]
# Registered ns-3 TcpCongestionOps TypeId used before Internet stack creation.
congestion_control = "ns3::TcpHighSpeed"

# TCP maximum segment payload in bytes.
segment_size_bytes = 1460

# TCP send-buffer size in bytes.
send_buffer_bytes = 33554432

# TCP receive-buffer size in bytes.
receive_buffer_bytes = 33554432

[statistics]
# Width of sparse PHY statistics windows in milliseconds.
window_ms = 10

[logging]
# SampleScenario level: off/error/warn/info/debug/function/logic/all.
sample_scenario_level = "info"

# APGenerator level: off/error/warn/info/debug/function/logic/all.
ap_generator_level = "warn"

# StaLlmGenerator level: off/error/warn/info/debug/function/logic/all.
sta_generator_level = "warn"

# TrafficSink level: off/error/warn/info/debug/function/logic/all.
traffic_sink_level = "warn"

# ContentionAwareAgentDistribution level: off/error/warn/info/debug/function/logic/all.
contention_distribution_level = "info"
```

## Typed C++ Model

Replace the current flat `ScenarioConfig` with nested structs whose member
names match the TOML intent while following ns-3 C++ naming conventions:

```cpp
struct GeneralConfig
{
    std::string traceFile;
    std::optional<std::string> runFolder;
    std::string outputName{"mac-node-stats.json"};
};

enum class DurationMode
{
    AUTO,
    FIXED,
};

struct SimulationConfig
{
    DurationMode durationMode{DurationMode::AUTO};
    double fixedDurationSeconds{0.0};
    double autoTailSeconds{2.0};
    uint32_t rngSeed{12345};
    uint64_t rngRun{1};
};

struct TopologyConfig
{
    int bssCount{3};
    int stationsPerBss{30};
    double bssSpacingM{100.0};
    double stationRadiusM{5.0};
    bool isolateBssChannels{true};
    std::string ssidPrefix{"llm-ap-"};
    uint16_t apSinkPort{10000};
    uint16_t stationSinkBasePort{9000};
    double generatorStartSeconds{1.0};
};

struct DistributionConfig
{
    int maxAgentsPerStation{832};
    bool lowContentionPriority{true};
    int slotMs{10};
};

enum class WifiBandConfig
{
    BAND_2_4_GHZ,
    BAND_5_GHZ,
    BAND_6_GHZ,
};

struct WifiConfig
{
    WifiBandConfig band{WifiBandConfig::BAND_5_GHZ};
    uint16_t channelNumber{0};
    int bandwidthMhz{20};
    uint8_t primary20Index{0};
    std::string rateManager{"ns3::MinstrelHtWifiManager"};
    bool activeProbing{true};
};

struct TcpConfig
{
    std::string congestionControl{"ns3::TcpHighSpeed"};
    uint32_t segmentSizeBytes{1460};
    uint32_t sendBufferBytes{32 * 1024 * 1024};
    uint32_t receiveBufferBytes{32 * 1024 * 1024};
};

struct StatisticsConfig
{
    uint32_t windowMs{10};
};

struct LoggingConfig
{
    std::string sampleScenarioLevel{"info"};
    std::string apGeneratorLevel{"warn"};
    std::string staGeneratorLevel{"warn"};
    std::string trafficSinkLevel{"warn"};
    std::string contentionDistributionLevel{"info"};
};

struct ScenarioConfig
{
    GeneralConfig general;
    SimulationConfig simulation;
    TopologyConfig topology;
    DistributionConfig distribution;
    WifiConfig wifi;
    TcpConfig tcp;
    StatisticsConfig statistics;
    LoggingConfig logging;
};
```

All new public declarations receive concise Doxygen comments and unit-bearing
member comments.

## Single Option Registry

One registry is the source of truth for every configurable field. Each entry
contains:

- dotted TOML path, for example `wifi.bandwidth_mhz`;
- CLI flag, for example `--wifi-bandwidth-mhz`;
- expected scalar type;
- a TOML-node setter;
- a string CLI setter;
- a concise description used by CLI help and diagnostics.

A practical shape is:

```cpp
struct ConfigOption
{
    std::string_view tomlPath;
    std::string_view cliFlag;
    ConfigValueType valueType;
    std::function<void(ScenarioConfig&, const toml::node&)> applyToml;
    std::function<void(ScenarioConfig&, std::string_view)> applyOverride;
    std::string_view description;
};
```

Small typed helper factories create integer, floating-point, boolean, string,
enum, and optional-path entries. The registry prevents TOML and CLI behavior
from drifting apart. A test iterates the registry and proves that every field
in `basic_config.toml` has exactly one CLI flag.

Do not use the registry to bypass typed validation in scenario code. It ends
with a complete `ScenarioConfig` value.

## TOML Parsing

`LoadTomlConfig()` starts from default-constructed `ScenarioConfig`, parses the
file with `toml::parse_file()`, and applies every present registered field.

Rules:

- The root may contain only the eight declared tables.
- Each table may contain only registered scalar fields.
- Duplicate TOML keys are rejected by `toml++`.
- Wrong scalar types are errors; numeric strings are not converted.
- Integer TOML values must fit the destination type before casting.
- `general.trace_file` must be present and non-empty.
- Other omitted fields retain their typed defaults.
- Parse errors print the upstream description, source path, line, and column.
- Schema errors print the dotted key and expected type.

The parser does not resolve filesystem paths or create directories.

## Position-Independent CLI Parsing

The old positional syntax is removed. Supported syntax is:

```text
llm_sample --config PATH [--section-field VALUE ...]
```

Parsing occurs in two passes:

1. Scan argument pairs for `--help` and exactly one `--config PATH`. Reject
   missing values, duplicates, positional tokens, and unknown flags.
2. Load TOML, then apply the collected overrides through the same option
   registry.

Properties:

- `--config` and overrides may appear in any order.
- Each override may appear at most once.
- Every non-help flag requires the next token as its value, even when that
  value begins with `-`.
- Boolean values are exactly `true` or `false`, case-sensitive.
- Enum/log strings use the documented spelling.
- `--help` may be used without `--config` and prints every registry flag.
- A real run without `--config` returns 1 and prints usage.

Precedence is:

```text
compiled defaults < TOML values < CLI overrides
```

Examples:

```bash
./ns3 run \
  "llm_sample --config contrib/llm/config/basic_config.toml"

./ns3 run \
  "llm_sample --wifi-bandwidth-mhz 80 \
  --config contrib/llm/config/basic_config.toml \
  --simulation-duration-mode fixed \
  --simulation-fixed-duration-seconds 120"
```

## Validation

Validation runs after TOML and CLI application and before directory creation,
trace parsing, or ns-3 object creation.

### General

- `trace_file` is present, resolves to an existing regular file, and is not a
  RAR archive.
- `run_folder`, when present, is non-empty.
- `output_name` is non-empty, ends in `.json`, and equals its filesystem
  filename component. It may not contain `/`, `\`, `..`, a root name, or a
  parent path.

### Simulation

- `duration_mode` is `auto` or `fixed`.
- `fixed_duration_seconds > 0` when mode is fixed.
- `fixed_duration_seconds >= 0` when mode is auto; it is ignored in auto mode.
- `auto_tail_seconds >= 0`.
- RNG seed/run values fit the ns-3 APIs.

### Topology

- `bss_count > 0`.
- `stations_per_bss > 0`.
- spacing, radius, and generator start are finite and non-negative.
- SSID prefix is non-empty.
- ports are nonzero.
- `station_sink_base_port + stations_per_bss - 1 <= 65535`.

### Distribution

- `max_agents_per_station >= 0`; zero means unlimited.
- `slot_ms > 0`.
- When the cap is positive, total placement capacity is checked again against
  the parsed agent count by the existing distribution algorithm.

### Wi-Fi

- band is `2.4GHz`, `5GHz`, or `6GHz`.
- bandwidth is 20, 40, 80, or 160 MHz.
- `primary_20_index < bandwidth_mhz / 20`.
- channel number fits the ns-3 channel tuple.
- rate manager TypeId exists and derives from `WifiRemoteStationManager`.
- Final standard/band/channel/width validity is also checked by ns-3 when
  `WifiHelper::SetStandard(WIFI_STANDARD_80211ax)` is applied.

### TCP

- congestion-control TypeId exists and derives from `TcpCongestionOps`.
- segment and buffer sizes are positive.
- segment size does not exceed either buffer size.

### Statistics and logging

- statistics window is positive and fits `uint32_t` milliseconds.
- log levels are one of
  `off/error/warn/info/debug/function/logic/all`.

Errors use clear field names and include the equivalent CLI flag.

## Path and Run Directory Lifecycle

Paths are based on `std::filesystem::current_path()` captured once during
startup.

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

Resolution:

1. Resolve relative `--config` against the captured working directory.
2. Resolve relative `general.trace_file` against the same working directory.
3. If `general.run_folder` exists, resolve it against the same working
   directory and use it directly.
4. Otherwise format local launch time as `YY-MM-DD_hh-mm-ss`, set the folder
   to `<cwd>/run/<timestamp>`, and mark it automatic.
5. Join the validated plain `output_name` to the run folder.

Directory preparation is a separate operation:

- explicit run folder: create missing parents/directories; an existing
  directory is allowed;
- automatic run folder: create `./run` if needed, then require the timestamp
  directory not to exist before creating it;
- either mode: require the final output path not to exist;
- any filesystem error stops startup before ns-3 objects are created.

The clock-dependent resolver accepts an injected
`std::chrono::system_clock::time_point` in tests. Production passes the launch
timestamp captured once at program start. Local timezone is used because the
required directory format describes local launch time.

## Source Integration

### Main orchestration

`sample-scenario.cc` changes startup order to:

1. capture launch time and working directory;
2. parse CLI and obtain config path/overrides;
3. parse TOML and apply overrides;
4. validate config and resolve paths;
5. prepare run directory/output path;
6. configure RNG, logging, TCP defaults, and Wi-Fi-independent defaults;
7. parse trace and compute duration;
8. distribute agents;
9. create topology/applications/statistics;
10. run and write `<runFolder>/<outputName>`.

Startup prints the config path, resolved trace path, run folder, output path,
duration mode, topology, distribution, Wi-Fi, and TCP choices.

### TCP ordering cleanup

Resolve `tcp.congestion_control` to a TypeId and set
`ns3::TcpL4Protocol::SocketType` before any `InternetStackHelper::Install()`.
Set segment and buffer defaults before applications create sockets. Remove the
late `TcpLinuxReno` default. The default remains the current effective
`TcpHighSpeed` behavior.

### Topology and mobility

Pass typed topology and Wi-Fi config into topology construction.

- AP position for BSS `i` becomes
  `(spacing * i, spacing * i, spacing * i)`.
- Station disc radius uses `station_radius_m`.
- SSID uses `ssid_prefix + bssIndex`.
- sink ports and generator start use config values.
- active probing uses `wifi.active_probing`.
- `ChannelSettings` is built from channel number, width, band, and primary-20
  index.
- rate manager uses the validated TypeId name.

When `isolate_bss_channels = true`, preserve one
`YansWifiChannelHelper::Default().Create()` per BSS. When false, create one
shared channel before the BSS loop and pass it to every group. Do not change
the default propagation delay/loss models in this task.

### Distribution

Map topology counts and distribution fields into
`ContentionAwareDistributionConfig`. Keep the existing algorithm and
deterministic tie-breaking unchanged.

### Statistics

Pass `statistics.window_ms` into `WifiStatistics` and store it in
`WifiStatisticsState`. Replace duplicated file constants with state access in
window selection, PHY collection, JSON timestamps, bandwidth calculation, and
JSON metadata. Unit calculations remain:

```text
window_us = window_ms * 1000
bw_mbps = bytes * 8 / window_us
```

### Logging

Map level strings to ns-3 `LogLevel` values and call `LogComponentEnable()`
for the five configured components. `off` performs no enable call. Reject
unknown levels during configuration validation.

## Files and Responsibilities

Create:

```text
config/basic_config.toml
examples/scenario-config-toml.cc
examples/scenario-config-cli.cc
examples/scenario-run-path.cc
test/scenario-run-path-test-suite.cc
lib/toml.hpp
lib/tomlplusplus-LICENSE
```

Modify:

```text
CMakeLists.txt
examples/CMakeLists.txt
examples/scenario-config.h
examples/scenario-config.cc
examples/sample-scenario.cc
examples/scenario-topology.h
examples/scenario-topology.cc
examples/wifi-statistics.h
examples/wifi-statistics-internal.h
examples/wifi-statistics*.cc
test/llm-test-suite.h
test/llm-test-suite.cc
test/scenario-config-test-suite.cc
test/wifi-statistics-test-suite.cc
test/examples-to-run.py
README.md
README_RU.md
```

Keep each implementation file below 600 lines. If the option registry grows
beyond that, split schema construction by section rather than combining
unrelated logic.

## Basic Config and Smoke Test

`basic_config.toml` uses the full schema above and points to
`contrib/llm/test/data/minimal-trace.json`, which works when launched from the
ns-3 root.

The registered example runs from a temporary test directory. It therefore
uses the config file plus CWD-relative overrides:

```text
llm_sample
--config ../../contrib/llm/config/basic_config.toml
--general-trace-file ../../contrib/llm/test/data/minimal-trace.json
--general-run-folder .
--general-output-name llm-smoke-stats.json
--simulation-duration-mode fixed
--simulation-fixed-duration-seconds 0.2
```

The test runner removes its temporary directory, including the generated JSON.

## Testing Strategy

### Schema/default tests

- Every typed default exactly matches current behavior.
- `basic_config.toml` contains every registered field or the documented
  commented optional `run_folder`.
- Every registry entry has one unique dotted TOML key and one unique CLI flag.
- Every TOML field has a CLI override.

### TOML tests

- minimal `[general]` with required trace;
- full config;
- missing `general` and missing `trace_file`;
- unknown section and unknown field;
- wrong scalar type;
- integer overflow;
- upstream syntax error with file/line/column;
- omitted optional/defaulted values.

### CLI tests

- required `--config`;
- `--help` without config;
- config before, between, or after overrides;
- multiple overrides in arbitrary order;
- TOML values overridden by CLI;
- unknown, duplicate, missing-value, positional, and invalid boolean cases;
- negative numeric value consumed as a value and then rejected by validation;
- all registry flags listed in help.

### Validation tests

- every single-field boundary and enum;
- cross-field fixed-duration, port-range, primary-20, and TCP-size rules;
- Wi-Fi and TCP TypeId inheritance;
- error includes TOML key and CLI flag.

### Run path tests

- absolute and CWD-relative config/trace/run paths;
- explicit run folder creation;
- omitted run folder produces deterministic local timestamp with injected
  time;
- existing automatic directory fails;
- existing output file fails;
- invalid output filenames fail;
- no directories created when configuration validation fails.

### Integration tests

- isolated topology gives distinct channel pointers;
- shared topology gives the same channel pointer;
- configurable statistics window changes boundary indexing, timestamps, and
  `bw` units;
- TCP defaults are applied before stack installation;
- `llm_sample` target builds with warnings as errors;
- `./test.py -s llm` passes;
- `./test.py -e 'llm_sample*'` passes and writes output only inside the test
  runner directory.

## Documentation

Update English and Russian README files together:

- replace positional invocation examples with `--config`;
- document precedence and every section;
- explain run-folder/output naming;
- state that relative paths use current working directory;
- explain shared versus isolated BSS channels;
- update TCP configuration caveat after removing the late default;
- document configurable statistics window units;
- retain the 802.11ax-mode compliance limitations.

## Compatibility

This intentionally breaks the old positional CLI. Commands such as:

```text
llm_sample trace.json 20 stats.json auto
```

return an error and usage text. Users must create a TOML file and pass
`--config`.

The model defaults and default `basic_config.toml` reproduce current effective
behavior except for output directory organization:

- trace input remains the required external choice;
- output filename remains `mac-node-stats.json`;
- auto duration, 2-second tail, RNG, topology, distribution, Wi-Fi, TCP,
  statistics, and logging defaults remain unchanged;
- omitted run folder now places output in a timestamped run directory as
  required.

## Success Criteria

- `config/basic_config.toml` exists, is fully commented, and covers the entire
  approved schema.
- `llm_sample --help` lists `--config` and every override.
- Missing `--config` or missing `general.trace_file` fails clearly.
- TOML and CLI precedence is deterministic and fully tested.
- Relative and absolute paths behave exactly as specified.
- Run directories and output collisions are safe and tested.
- All scenario controls in the approved schema reach the owning subsystem.
- Effective hardcoded defaults remain unchanged.
- Vendored `toml++` license and provenance are present.
- All C++ and example builds pass with warnings as errors.
- Unit tests and registered smoke test pass.
- README documents remain synchronized in English and Russian.
