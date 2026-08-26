# Project Layout and Readable JSON Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reorganize the llm project by responsibility, rename its scenario and starter configuration, and emit readable two-space-indented experiment JSON through a bounded-memory streaming writer.

**Architecture:** Keep the ns-3 module and example CMake inventories centralized while moving implementation and tests into shallow responsibility directories. Replace direct JSON punctuation writes with one stateful streaming `JsonWriter`; the existing typed summaries and schema remain unchanged. Preserve two thin Python entry points and move reusable code and tests into explicit packages.

**Tech Stack:** C++23, ns-3 TestSuite/CMake, nlohmann JSON, Python 3 `unittest`, ijson, rarfile.

**Spec:** `docs/superpowers/specs/2026-08-26-project-layout-and-readable-json-design.md`

## Global Constraints

- Work only in the nested `contrib/llm` repository on the dedicated feature branch.
- Preserve the outer repository's modified `.gitignore`, untracked `VAGUE_TASK.md`, and pre-existing run artifacts.
- No compatibility aliases, wrappers, copies, or symlinks for paths that change.
- Keep `scripts/find_window.py` and `scripts/live_test_traces.py` as the only canonical root script entry points.
- Keep public model header basenames and their `#include "ns3/<header>.h"` consumer API stable.
- Do not add `scenarios/`, generic `helpers/`, generic `common/`, or per-child-directory CMake files.
- Keep the experiment schema, field order, values, schema version, validation, no-clobber behavior, and one final newline unchanged.
- JSON indentation is always two spaces; there is no compact mode or configuration option.
- The JSON writer may retain state proportional only to nesting depth and must never construct the complete output DOM.
- Use `git mv` for tracked moves and exact-path staging for every commit.
- Remove all task-created live outputs, temporary directories, extracted inputs, and Python caches.

---

### Task 1: Add the streaming JSON formatting primitive

**Files:**
- Move: `examples/experiment-output-internal.h` -> `examples/statistics/json/writer.h`
- Modify: `examples/experiment-json.cc`
- Modify: `examples/experiment-statistics-json.cc`
- Modify: `examples/experiment-statistics-json-app.cc`
- Modify: `examples/experiment-statistics-json-entity.cc`
- Modify: `examples/experiment-statistics-json-general.cc`
- Modify: `examples/experiment-statistics-json-mac.cc`
- Modify: `examples/experiment-statistics-json-phy.cc`
- Modify: `examples/experiment-statistics-json-tcp.cc`
- Modify: `test/experiment-json-test-suite.cc`

**Interfaces:**
- Consumes: `std::ostream`, `nlohmann::json`, and existing output DTO declarations.
- Produces: `ns3::JsonWriter` with `BeginObject()`, `EndObject()`, `BeginArray()`, `EndArray()`, `Key(std::string_view)`, `Value(const T&)`, `Null()`, and `Finish()`.
- Produces: existing serializer declarations at `statistics/json/writer.h`; serializer signatures remain stream-based until Task 2.

- [ ] **Step 1: Move the internal JSON header and repair includes without changing behavior**

```bash
mkdir -p examples/statistics/json
git mv examples/experiment-output-internal.h examples/statistics/json/writer.h
```

Change each former include to:

```cpp
#include "statistics/json/writer.h"
```

In the moved header, temporarily include the unmoved DTO with:

```cpp
#include "../../experiment-window-output.h"
```

Run:

```bash
../../ns3 build llm-test llm_sample
../../test.py -s llm --no-build
```

Expected: both targets build and the llm suite passes before semantic edits.

- [ ] **Step 2: Add failing exact-format and invalid-state tests**

Add `JsonWriterFormattingTestCase` to `test/experiment-json-test-suite.cc`. It builds this value solely through writer operations:

```cpp
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
```

Assert byte-for-byte equality with:

```cpp
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
```

Add `JsonWriterStateTestCase`. With a fresh writer for each case, assert `std::logic_error` for: `Key()` outside an object, a value while an object expects a key, ending an object after `Key()` without a value, closing the wrong container kind, `Finish()` before a root value, `Finish()` with an open root container, a second root value, and a second `Finish()` call. Register both cases in `CreateExperimentJsonTestCases()`.

- [ ] **Step 3: Run the new tests and verify RED**

```bash
../../ns3 build llm-test
../../test.py -s llm --no-build
```

Expected: compilation fails because `JsonWriter` is not defined.

- [ ] **Step 4: Implement the minimal stateful writer in the moved header**

Declare this exact API and state:

```cpp
class JsonWriter
{
  public:
    explicit JsonWriter(std::ostream& output);
    void BeginObject();
    void EndObject();
    void BeginArray();
    void EndArray();
    void Key(std::string_view key);

    template <typename T>
    void Value(const T& value)
    {
        BeginValue();
        m_output << nlohmann::json(value).dump();
        CompleteScalarValue();
    }

    void Null();
    void Finish();

  private:
    enum class ContainerKind
    {
        OBJECT,
        ARRAY
    };

    struct ContainerState
    {
        ContainerKind kind;
        bool first{true};
        bool expectsValue{false};
    };

    void BeginValue();
    void CompleteScalarValue();
    void EndContainer(ContainerKind expectedKind, char closingCharacter);
    void WriteIndent(std::size_t depth);
    [[noreturn]] void ThrowStateError(std::string_view operation) const;

    std::ostream& m_output;
    std::vector<ContainerState> m_containers;
    bool m_rootStarted{false};
    bool m_rootComplete{false};
    bool m_finished{false};
};
```

Define the methods inline. `BeginValue()` rejects writes after finish and second roots, emits array separators/indent, and requires a pending object key. `Key()` requires an object expecting a key, emits its separator/indent and encoded key followed by `: `, then marks a pending value. Container closes verify kind and pending-key state, indent non-empty closing delimiters, and complete their parent value. `Null()` uses the scalar path. `Finish()` accepts one complete root with no open containers, appends one newline, and rejects repeats. Every state error includes the operation name.

- [ ] **Step 5: Run focused and regression tests**

```bash
../../ns3 build llm-test
../../test.py -s llm --no-build
../../utils/check-style-clang-format.py examples/statistics/json/writer.h test/experiment-json-test-suite.cc
```

Expected: the llm suite passes, including exact formatting and state-error cases.

- [ ] **Step 6: Commit the writer foundation**

```bash
git add examples/statistics/json/writer.h examples/experiment-json.cc \
  examples/experiment-statistics-json*.cc test/experiment-json-test-suite.cc
git commit -m "llm: Add streaming pretty JSON writer"
```

---

### Task 2: Convert every experiment serializer to structured writes

**Files:**
- Modify: `examples/statistics/json/writer.h`
- Modify: `examples/experiment-json.cc`
- Modify: `examples/experiment-statistics-json.cc`
- Modify: `examples/experiment-statistics-json-app.cc`
- Modify: `examples/experiment-statistics-json-entity.cc`
- Modify: `examples/experiment-statistics-json-general.cc`
- Modify: `examples/experiment-statistics-json-mac.cc`
- Modify: `examples/experiment-statistics-json-phy.cc`
- Modify: `examples/experiment-statistics-json-tcp.cc`
- Modify: `examples/scenario-config-internal.h`
- Modify: `examples/scenario-config-json.cc`
- Modify: `test/experiment-hierarchy-json-test-suite.cc`
- Modify: `test/scenario-config-json-test-suite.cc`

**Interfaces:**
- Consumes: `JsonWriter` from Task 1 and existing typed summaries/config registry.
- Produces: component writers accepting `JsonWriter&`; `WriteExperimentHierarchyJson(std::ostream&, ...)` remains the sole stream boundary.
- Produces: templated `WriteEffectiveConfigurationJson(Writer&, const ScenarioConfig&)` that streams 36 registry values without a configuration DOM.

- [ ] **Step 1: Strengthen hierarchy and configuration tests before conversion**

Retain the emitted hierarchy string before parsing and assert:

```cpp
NS_TEST_ASSERT_MSG_EQ(text.starts_with("{\n  \"schema_version\": 1,"),
                      true,
                      "Root is not two-space formatted");
NS_TEST_ASSERT_MSG_EQ(text.ends_with("\n}\n"), true, "Document lacks one final newline");
NS_TEST_ASSERT_MSG_EQ(text.find("\n    \"measurement_semantics\""),
                      std::string::npos,
                      "Root member has the wrong indentation");
```

Keep every parsed schema/value/order assertion. In the configuration JSON test, write through `JsonWriter`, assert two-space section indentation and four-space field indentation, then retain all 36-field type assertions.

- [ ] **Step 2: Run tests and verify RED**

```bash
../../ns3 build llm-test
../../test.py -s llm --no-build
```

Expected: production-format assertions fail because serializers remain compact.

- [ ] **Step 3: Change internal serializer signatures**

Replace declarations with these signatures:

```cpp
void WriteSampleDistributionJson(JsonWriter& writer,
                                 const SampleDistributionOutput& distribution);
void WriteGeneralDirectionJson(JsonWriter& writer, const GeneralDirectionOutput& direction);
void WriteAppDirectionJson(JsonWriter& writer, const AppDirectionOutput& direction);
void WriteTcpDirectionJson(JsonWriter& writer, const TcpDirectionOutput& direction);
void WriteMacDirectionJson(JsonWriter& writer, const MacDirectionOutput& direction);
void WritePhyCategoryJson(JsonWriter& writer, const PhyCategoryOutput& category);
void WriteEntityStatisticsJson(JsonWriter& writer, const EntityStatisticsOutput& statistics);
void WriteAccessPointStatisticsArrayJson(
    JsonWriter& writer,
    const std::vector<AccessPointStatisticsOutput>& entities);
void WriteStationStatisticsArrayJson(
    JsonWriter& writer,
    const std::vector<StationStatisticsOutput>& entities);
void WriteExperimentWindowsJson(JsonWriter& writer,
                                const std::vector<ExperimentWindowOutput>& windows);
void WriteExperimentOverallJson(JsonWriter& writer, const ExperimentOverallOutput& overall);
void WriteExperimentValidationJson(JsonWriter& writer,
                                   const ExperimentValidationOutput& validation);
```

Keep `WriteExperimentHierarchyJson(std::ostream&, const UnifiedExperimentSummary&, const ScenarioConfig&)` as the public test seam.

- [ ] **Step 4: Make effective configuration use the structured writer**

Move `WriteEffectiveConfigurationJson` into `scenario-config-internal.h` as this template:

```cpp
template <typename Writer>
void
WriteEffectiveConfigurationJson(Writer& writer, const ScenarioConfig& configuration)
{
    writer.BeginObject();
    std::string_view activeSection;
    for (const auto& option : GetScenarioConfigOptions())
    {
        const auto [section, field] = SplitScenarioConfigPath(option.tomlPath);
        if (section != activeSection)
        {
            if (!activeSection.empty())
            {
                writer.EndObject();
            }
            writer.Key(section);
            writer.BeginObject();
            activeSection = section;
        }
        writer.Key(field);
        writer.Value(option.readJson(configuration));
    }
    if (!activeSection.empty())
    {
        writer.EndObject();
    }
    writer.EndObject();
}
```

Remove its stream definition from `scenario-config-json.cc`. Add `JsonWriter::Value(const nlohmann::json&)` that accepts scalar/null values and rejects arrays/objects with `std::logic_error`; registry readers are scalar by contract.

- [ ] **Step 5: Convert serializers without changing field order**

Objects use:

```cpp
writer.BeginObject();
writer.Key("sample_count");
writer.Value(distribution.sampleCount);
writer.Key("average");
writer.Value(distribution.average);
writer.EndObject();
```

Arrays use:

```cpp
writer.BeginArray();
for (const auto& entity : entities)
{
    WriteEntityStatisticsJson(writer, entity.statistics);
}
writer.EndArray();
```

Preserve current source order for every key and array. Replace optional scalar writes with `Value(*value)` or `Null()`. Do not rename fields or alter formulas.

Delete the superseded `WriteJsonScalar` templates after their final caller is converted. The root boundary creates one writer, emits the existing seven root members in order, closes the root, and calls `Finish()`. Remove the separate `output << '\n';` because `Finish()` owns the newline. Leave exclusive-create, failed-write, flush, close, and path-bearing `std::runtime_error` handling unchanged.

- [ ] **Step 6: Prove structured output and green tests**

```bash
if rg -n 'output\s*<<.*[{}\[\],:]' examples/experiment-*.cc; then exit 1; fi
../../ns3 build llm-test llm_sample
../../test.py -s llm --no-build
../../test.py -e 'llm_sample*' --no-build
../../utils/check-style-clang-format.py examples test
```

Expected: direct JSON punctuation is absent, all schema tests pass, and smoke output is indented.

- [ ] **Step 7: Commit readable JSON serialization**

```bash
git add examples test/experiment-hierarchy-json-test-suite.cc \
  test/scenario-config-json-test-suite.cc
git commit -m "llm: Write readable experiment JSON"
```

---

### Task 3: Organize model code and matching tests

**Files:**
- Move: all files listed under the spec's model mapping
- Move: `test/agent-distribution-test-suite.cc` -> `test/model/distribution/agent-distribution-test-suite.cc`
- Move: `test/app-tx-tag-test-suite.cc` -> `test/model/applications/app-tx-tag-test-suite.cc`
- Move: `test/traffic-schedule-test-suite.cc` -> `test/model/applications/traffic-schedule-test-suite.cc`
- Move: `test/trace-parser-test-suite.cc` -> `test/model/traces/trace-parser-test-suite.cc`
- Modify: `CMakeLists.txt`
- Modify: moved model/test includes

**Interfaces:**
- Consumes: existing public model types and central `build_lib()` inventory.
- Produces: four model responsibility directories while retaining public `ns3/<header>.h` includes and the single `llm` test suite.

- [ ] **Step 1: Record public-header and suite baseline**

```bash
../../ns3 build llm-test llm_sample
../../test.py -s llm --no-build
test -e ../../build/include/ns3/agent-distribution.h
test -e ../../build/include/ns3/ap-generator.h
test -e ../../build/include/ns3/trace-parser.h
```

Expected: build and suite pass and representative public headers exist.

- [ ] **Step 2: Move model files with Git history**

```bash
mkdir -p model/applications model/distribution model/logging model/traces
git mv model/agent-data.h model/agent-distribution.cc model/agent-distribution.h \
  model/contention-aware-agent-distribution.cc \
  model/contention-aware-agent-distribution.h \
  model/contention-aware-bss-assignment.cc \
  model/contention-aware-distribution-internal.h \
  model/contention-aware-sta-assignment.cc model/distribution/
git mv model/ap-generator.cc model/ap-generator.h model/sta-llm-generator.cc \
  model/sta-llm-generator.h model/traffic-sink.cc model/traffic-sink.h \
  model/traffic-schedule.cc model/traffic-schedule.h model/app-tx-tag.cc \
  model/app-tx-tag.h model/applications/
git mv model/trace-parser.cc model/trace-parser.h model/traces/
git mv model/llm-log.cc model/llm-log.h model/logging/
```

- [ ] **Step 3: Move matching tests**

```bash
mkdir -p test/model/applications test/model/distribution test/model/traces
git mv test/agent-distribution-test-suite.cc test/model/distribution/
git mv test/app-tx-tag-test-suite.cc test/traffic-schedule-test-suite.cc \
  test/model/applications/
git mv test/trace-parser-test-suite.cc test/model/traces/
```

Moved registry includes become:

```cpp
#include "../../llm-test-suite.h"
```

- [ ] **Step 4: Update CMake and cross-directory includes**

Group `source_files`, `header_files`, private headers, and `test_sources` by directory. Keep every file exactly once. Same-directory implementation includes retain basenames. Private logging includes use:

```cpp
#include "../logging/llm-log.h"
```

Public cross-directory header dependencies use stable installed forms:

```cpp
#include "ns3/app-tx-tag.h"
#include "ns3/trace-parser.h"
```

Do not alter external model consumers unless a current include names a private physical path.

- [ ] **Step 5: Build and verify the move**

```bash
../../ns3 build llm-test llm_sample
../../test.py -s llm --no-build
../../utils/check-style-clang-format.py model test/model
test -e ../../build/include/ns3/agent-distribution.h
test -e ../../build/include/ns3/ap-generator.h
test -e ../../build/include/ns3/trace-parser.h
test -z "$(find model -maxdepth 1 -type f -print -quit)"
```

Expected: build and suite pass, public headers remain available, and `model/` has no flat source files.

- [ ] **Step 6: Commit model hierarchy**

```bash
git add CMakeLists.txt model test/model
git commit -m "llm: Organize model code and tests"
```

---

### Task 4: Organize scenario configuration and runtime support

**Files:**
- Move: all files listed in the spec's configuration and runtime mappings
- Move: scenario config/path tests -> `test/config/`
- Move: scenario logging/topology/coordinator tests -> `test/runtime/`
- Modify: `CMakeLists.txt`
- Modify: `examples/CMakeLists.txt`
- Modify: `examples/sample-scenario.cc`
- Modify: moved includes

**Interfaces:**
- Consumes: model hierarchy from Task 3 and still-flat statistics files.
- Produces: `examples/config/`, `examples/runtime/`, `test/config/`, and `test/runtime/` with unchanged scenario behavior.

- [ ] **Step 1: Move configuration and runtime files**

```bash
mkdir -p examples/config examples/runtime test/config test/runtime
git mv examples/scenario-config.h examples/config/scenario-config.h
git mv examples/scenario-config.cc examples/config/scenario-config.cc
git mv examples/scenario-config-internal.h examples/config/internal.h
git mv examples/scenario-config-cli.cc examples/config/cli.cc
git mv examples/scenario-config-toml.cc examples/config/toml.cc
git mv examples/scenario-config-json.cc examples/config/json.cc
git mv examples/scenario-config-validation.cc examples/config/validation.cc
git mv examples/scenario-run-path.cc examples/config/run-path.cc
git mv examples/scenario-log.cc examples/runtime/log.cc
git mv examples/scenario-log.h examples/runtime/log.h
git mv examples/scenario-topology.cc examples/runtime/topology.cc
git mv examples/scenario-topology.h examples/runtime/topology.h
git mv examples/traffic-coordinator.cc examples/runtime/traffic-coordinator.cc
git mv examples/traffic-coordinator.h examples/runtime/traffic-coordinator.h
git mv test/scenario-config-test-suite.cc test/scenario-config-json-test-suite.cc \
  test/scenario-config-validation-test-suite.cc test/scenario-run-path-test-suite.cc test/config/
git mv test/scenario-logging-test-suite.cc test/scenario-topology-test-suite.cc \
  test/traffic-coordinator-test-suite.cc test/runtime/
```

- [ ] **Step 2: Repair local and cross-directory includes**

Configuration sources use local names:

```cpp
#include "scenario-config.h"
#include "internal.h"
```

Runtime sources use local names and explicit cross-domain paths:

```cpp
#include "log.h"
#include "topology.h"
#include "traffic-coordinator.h"
#include "../config/scenario-config.h"
#include "../experiment-statistics.h"
```

The still-root scenario entry point includes:

```cpp
#include "config/scenario-config.h"
#include "runtime/log.h"
#include "runtime/topology.h"
#include "runtime/traffic-coordinator.h"
```

Moved tests include `../llm-test-suite.h` and implementation headers through `../../examples/config/` or `../../examples/runtime/`.

- [ ] **Step 3: Update both CMake inventories**

Replace every old configuration/runtime path with its exact new path in root `test_sources` and example `SOURCE_FILES`. Keep statistics paths unchanged in this task.

- [ ] **Step 4: Build and test scenario support**

```bash
../../ns3 build llm-test llm_sample
../../test.py -s llm --no-build
../../test.py -e 'llm_sample*' --no-build
../../utils/check-style-clang-format.py examples/config examples/runtime test/config test/runtime
```

Expected: build, suite, and registered example pass with unchanged public target/config names at this checkpoint.

- [ ] **Step 5: Commit scenario support moves**

```bash
git add CMakeLists.txt examples test/config test/runtime
git commit -m "llm: Organize scenario support"
```

---

### Task 5: Organize statistics collection, output, and tests

**Files:**
- Move: all files listed under the spec's statistics mapping
- Move: all `test/experiment-*-test-suite.cc` files -> `test/statistics/`
- Modify: `CMakeLists.txt`
- Modify: `examples/CMakeLists.txt`
- Modify: `examples/sample-scenario.cc`
- Modify: `examples/runtime/topology.cc`
- Modify: moved includes

**Interfaces:**
- Consumes: readable serializers from Tasks 1-2 and organized config/runtime from Task 4.
- Produces: final `examples/statistics/`, `examples/statistics/json/`, and `test/statistics/` hierarchy with unchanged `ExperimentStatistics` API.

- [ ] **Step 1: Move statistics core and JSON serializers**

```bash
mkdir -p test/statistics
git mv examples/experiment-statistics.h examples/statistics/experiment-statistics.h
git mv examples/experiment-statistics-owner.cc examples/statistics/experiment-statistics.cc
git mv examples/experiment-statistics-internal.h examples/statistics/internal.h
git mv examples/experiment-statistics-types.h examples/statistics/types.h
git mv examples/experiment-window-output.h examples/statistics/output-types.h
git mv examples/experiment-statistics-window.cc examples/statistics/window.cc
git mv examples/experiment-statistics-app.cc examples/statistics/app.cc
git mv examples/experiment-statistics-tcp.cc examples/statistics/tcp.cc
git mv examples/experiment-statistics-device.cc examples/statistics/device.cc
git mv examples/experiment-statistics-mac.cc examples/statistics/mac.cc
git mv examples/experiment-statistics-phy.cc examples/statistics/phy.cc
git mv examples/experiment-statistics-summary.cc examples/statistics/summary.cc
git mv examples/experiment-statistics-validation.cc examples/statistics/validation.cc
git mv examples/experiment-json.cc examples/statistics/json/writer.cc
git mv examples/experiment-statistics-json.cc examples/statistics/json/hierarchy.cc
git mv examples/experiment-statistics-json-entity.cc examples/statistics/json/entity.cc
git mv examples/experiment-statistics-json-general.cc examples/statistics/json/general.cc
git mv examples/experiment-statistics-json-app.cc examples/statistics/json/app.cc
git mv examples/experiment-statistics-json-tcp.cc examples/statistics/json/tcp.cc
git mv examples/experiment-statistics-json-mac.cc examples/statistics/json/mac.cc
git mv examples/experiment-statistics-json-phy.cc examples/statistics/json/phy.cc
git mv test/experiment-*-test-suite.cc test/statistics/
```

- [ ] **Step 2: Repair statistics includes**

Core statistics sources use:

```cpp
#include "experiment-statistics.h"
#include "internal.h"
#include "types.h"
#include "output-types.h"
```

Cross-domain includes use:

```cpp
#include "../runtime/log.h"
#include "../runtime/traffic-coordinator.h"
```

JSON sources use:

```cpp
#include "writer.h"
#include "../experiment-statistics.h"
#include "../../config/internal.h"
#include "../../runtime/log.h"
```

Update `writer.h` to include `../output-types.h`. Update the entry point and runtime topology to include `statistics/experiment-statistics.h`. Moved statistics tests include `../llm-test-suite.h` and `../../examples/statistics/` paths.

- [ ] **Step 3: Update centralized CMake inventories**

Group statistics core and statistics JSON paths separately in root `test_sources` and example `SOURCE_FILES`. Include every moved `.cc` exactly once.

- [ ] **Step 4: Verify final C++ directory structure**

```bash
../../ns3 build llm-test llm_sample
../../test.py -s llm --no-build
../../test.py -e 'llm_sample*' --no-build
../../utils/check-style-clang-format.py examples test/statistics
test -z "$(find examples -maxdepth 1 -type f ! -name CMakeLists.txt ! -name sample-scenario.cc -print -quit)"
```

Expected: build, suite, and example pass; only entry source and CMake remain at example root.

- [ ] **Step 5: Commit statistics hierarchy**

```bash
git add CMakeLists.txt examples test/statistics
git commit -m "llm: Organize experiment statistics"
```

---

### Task 6: Package Python implementations and relocate tests

**Files:**
- Create: `scripts/trace_tools/__init__.py`
- Create: `scripts/live_verification/__init__.py`
- Create: `scripts/tests/__init__.py`
- Create: `scripts/tests/trace_tools/__init__.py`
- Create: `scripts/tests/live_verification/__init__.py`
- Move: Python implementation/test files according to the spec
- Modify: `scripts/find_window.py`
- Modify: `scripts/live_test_traces.py`
- Modify: moved imports and path fixtures

**Interfaces:**
- Consumes: existing trace CLI, live matrix policy, schema checks, and deterministic tests.
- Produces: importable `trace_tools` and `live_verification` packages plus package tests behind unchanged root entry points.

- [ ] **Step 1: Add failing package-import smoke checks**

Run from `contrib/llm/scripts/`:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -c 'from trace_tools.stream import validate_path'
PYTHONDONTWRITEBYTECODE=1 python3 -c 'from live_verification.runner import run_live_matrix'
```

Expected: both fail with `ModuleNotFoundError`.

- [ ] **Step 2: Move implementations and tests**

```bash
mkdir -p scripts/trace_tools scripts/live_verification \
  scripts/tests/trace_tools scripts/tests/live_verification
git mv scripts/trace_stream.py scripts/trace_tools/stream.py
git mv scripts/live_trace_cleanup.py scripts/live_verification/cleanup.py
git mv scripts/live_trace_common.py scripts/live_verification/common.py
git mv scripts/live_trace_runner.py scripts/live_verification/runner.py
git mv scripts/live_trace_schema.py scripts/live_verification/schema.py
git mv scripts/live_trace_schema_categories.py scripts/live_verification/schema_categories.py
git mv scripts/test_trace_stream.py scripts/tests/trace_tools/test_stream.py
git mv scripts/test_live_trace_cleanup.py scripts/tests/live_verification/test_cleanup.py
git mv scripts/test_live_trace_runner.py scripts/tests/live_verification/test_runner.py
git mv scripts/test_live_trace_schema_root.py scripts/tests/live_verification/test_schema_root.py
git mv scripts/test_live_trace_schema_ordering.py scripts/tests/live_verification/test_schema_ordering.py
git mv scripts/live_trace_test_fixtures.py scripts/tests/live_verification/fixtures.py
```

Create the package markers with these exact contents:

```python
# scripts/trace_tools/__init__.py
"""Trace processing implementation package."""
```

```python
# scripts/live_verification/__init__.py
"""Live experiment verification package."""
```

```python
# scripts/tests/__init__.py
"""Script test package."""
```

```python
# scripts/tests/trace_tools/__init__.py
"""Trace tool tests."""
```

```python
# scripts/tests/live_verification/__init__.py
"""Live verification tests."""
```

Create these files with `apply_patch`, not shell redirection.

- [ ] **Step 3: Replace implicit imports with package imports**

The root trace CLI imports:

```python
from trace_tools.stream import (
    SliceSummary,
    TraceSummary,
    TraceValidationError,
    Window,
    find_first_window,
    find_high_load_window,
    validate_path,
    validate_stream,
    write_window,
)
```

The root live CLI and production modules import only `live_verification.*`. Tests import production packages and fixtures with:

```python
from live_verification.common import LiveTraceError
from live_verification.runner import _run_captured, run_one_trace
from tests.live_verification.fixtures import valid_document
```

Do not add `sys.path.insert()` calls.

Update `run_self_tests()` to import:

```python
from tests.live_verification.test_cleanup import LiveTraceCleanupTest
from tests.live_verification.test_runner import LiveTraceRunnerTest
from tests.live_verification.test_schema_ordering import LiveTraceSchemaOrderingTest
from tests.live_verification.test_schema_root import LiveTraceSchemaRootTest
```

- [ ] **Step 4: Update script-relative CLI fixtures**

In `test_stream.py`, derive the root entry point as:

```python
SCRIPTS_DIR = Path(__file__).resolve().parents[2]
FIND_WINDOW = SCRIPTS_DIR / "find_window.py"
```

Use `FIND_WINDOW` in every subprocess. Keep repository-relative simulator commands unchanged until Task 7.

- [ ] **Step 5: Run package and entry-point tests**

Run from `contrib/llm/scripts/`:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -c 'from trace_tools.stream import validate_path'
PYTHONDONTWRITEBYTECODE=1 python3 -c 'from live_verification.runner import run_live_matrix'
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -t . -p 'test_*.py' -v
PYTHONDONTWRITEBYTECODE=1 python3 live_test_traces.py --self-test
PYTHONDONTWRITEBYTECODE=1 python3 find_window.py --help
```

Expected: package imports pass, all trace-stream and live-verifier tests pass, and both entry points execute directly.

- [ ] **Step 6: Verify only entry points remain flat and commit**

```bash
test "$(find scripts -maxdepth 1 -type f -name '*.py' -printf '%f\n' | sort | tr '\n' ' ')" = "find_window.py live_test_traces.py "
git diff --check
git add scripts/find_window.py scripts/live_test_traces.py scripts/trace_tools \
  scripts/live_verification scripts/tests
git commit -m "llm: Organize trace tools and tests"
```

---

### Task 7: Rename the scenario executable and starter configuration

**Files:**
- Move: `examples/sample-scenario.cc` -> `examples/llm-scenario.cc`
- Move: `config/basic_config.toml` -> `config/llm_config.toml`
- Modify: `examples/CMakeLists.txt`
- Modify: `test/examples-to-run.py`
- Modify: `examples/config/scenario-config.h`
- Modify: `examples/config/validation.cc`
- Modify: `examples/runtime/log.h`
- Modify: `scripts/live_verification/common.py`
- Modify: `scripts/live_verification/runner.py`
- Modify: `scripts/tests/live_verification/test_runner.py`
- Modify: active source/test strings containing old names

**Interfaces:**
- Consumes: final C++ and Python hierarchies.
- Produces: sole target `llm-scenario`, source `llm-scenario.cc`, and starter config `llm_config.toml`; old names cease to work.

- [ ] **Step 1: Make automated expectations demand new names**

Change the registered example command to start with:

```python
"llm-scenario --config ../../contrib/llm/config/llm_config.toml "
```

Update live runner commands and tests to use `llm-scenario` and `contrib/llm/config/llm_config.toml`. Change executable wording from “sample scenario” to “llm scenario” where it identifies the program. Keep the stable configuration field `sample_scenario_level` unchanged.

- [ ] **Step 2: Run renamed expectations and verify RED**

```bash
../../test.py -e 'llm-scenario*' --no-build
PYTHONDONTWRITEBYTECODE=1 python3 scripts/live_test_traces.py --self-test
```

Expected: example discovery or command tests fail while old names still exist.

- [ ] **Step 3: Rename tracked files and CMake target**

```bash
git mv examples/sample-scenario.cc examples/llm-scenario.cc
git mv config/basic_config.toml config/llm_config.toml
```

The example declaration becomes:

```cmake
build_lib_example(
  NAME llm-scenario
  SOURCE_FILES
    llm-scenario.cc
```

Update active source, test, and script strings. Do not edit historical documents under `docs/superpowers/`.

- [ ] **Step 4: Build and test only the new executable**

```bash
../../ns3 build llm-test llm-scenario
../../test.py -s llm --no-build
../../test.py -e 'llm-scenario*' --no-build
if ../../ns3 show targets | rg -q 'llm_sample'; then exit 1; fi
test ! -e examples/sample-scenario.cc
test ! -e config/basic_config.toml
```

Expected: new target, suite, and smoke example pass; old files and target are absent.

- [ ] **Step 5: Commit the public cutover**

```bash
git add config/llm_config.toml examples/llm-scenario.cc examples/CMakeLists.txt \
  examples/config/scenario-config.h examples/config/validation.cc examples/runtime/log.h \
  test/examples-to-run.py scripts/live_verification scripts/tests/live_verification
git commit -m "llm: Rename scenario and configuration"
```

---

### Task 8: Update current English and Russian documentation

**Files:**
- Modify: `README.md`
- Modify: `README_RU.md`

**Interfaces:**
- Consumes: final paths, executable, script packages, and readable JSON behavior from Tasks 1-7.
- Produces: synchronized current documentation with correct commands, tree, and output-format contract.

- [ ] **Step 1: Update active commands and links**

Use these exact command forms:

```bash
./ns3 run "llm-scenario --config contrib/llm/config/llm_config.toml"
PYTHONDONTWRITEBYTECODE=1 python3 contrib/llm/scripts/find_window.py --help
PYTHONDONTWRITEBYTECODE=1 python3 contrib/llm/scripts/live_test_traces.py --self-test
```

Update configuration links to `config/llm_config.toml` and executable references to `llm-scenario`. Keep `logging.sample_scenario_level` unchanged and identify it as a stable configuration field where needed.

- [ ] **Step 2: Replace both project-structure sections**

Document `examples/config`, `examples/runtime`, `examples/statistics/json`, the four model domains, the two Python packages/test tree, and mirrored C++ test domains. Explain that CMake remains centralized and the two root Python files are intentional entry points.

- [ ] **Step 3: Document readable streaming output**

State in both languages that `output.json` is always two-space indented, has no compact-output option, preserves schema/values, streams without a complete result DOM, and is larger on disk than compact output.

- [ ] **Step 4: Check bilingual current-path parity**

```bash
for file in README.md README_RU.md; do
  rg -q 'llm-scenario' "$file"
  rg -q 'config/llm_config.toml' "$file"
  rg -q 'scripts/live_test_traces.py' "$file"
  rg -q 'statistics/json' "$file"
done
if rg -n 'llm_sample|sample-scenario\.cc|config/basic_config\.toml' README.md README_RU.md; then exit 1; fi
git diff --check
```

Expected: both documents use current paths and remain whitespace-clean.

- [ ] **Step 5: Commit current documentation**

```bash
git add README.md README_RU.md
git commit -m "llm: Document organized project layout"
```

---

### Task 9: Verify the complete project and live traces

**Files:**
- Modify only if verification exposes a concrete defect: files already owned by Tasks 1-8
- Do not modify: `traces/*`, outer `.gitignore`, outer `VAGUE_TASK.md`, or pre-existing run artifacts

**Interfaces:**
- Consumes: complete implementation and deterministic tests.
- Produces: evidence that the final branch builds, tests, runs every JSON trace once, and leaves no generated artifacts.

- [ ] **Step 1: Verify final tree and active reference cutover**

```bash
find examples model scripts test -maxdepth 3 -type f -print | sort
test ! -e examples/sample-scenario.cc
test ! -e config/basic_config.toml
if rg -n 'llm_sample|sample-scenario\.cc|config/basic_config\.toml' \
  CMakeLists.txt README.md README_RU.md config examples model scripts test; then
  exit 1
fi
test -z "$(find model -maxdepth 1 -type f -print -quit)"
test "$(find scripts -maxdepth 1 -type f -name '*.py' -printf '%f\n' | sort | tr '\n' ' ')" = "find_window.py live_test_traces.py "
```

Expected: tree matches the spec and active files contain no obsolete names.

- [ ] **Step 2: Run all deterministic verification from outer ns-3 root**

```bash
PYTHONDONTWRITEBYTECODE=1 python3 contrib/llm/scripts/live_test_traces.py --self-test
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s contrib/llm/scripts/tests -t contrib/llm/scripts -p 'test_*.py' -v
./utils/check-style-clang-format.py contrib/llm/examples contrib/llm/model contrib/llm/test
./ns3 build llm-test llm-scenario
./test.py -s llm --no-build
./test.py -e 'llm-scenario*' --no-build
git -C contrib/llm diff --check
```

Expected: every Python test, style check, target, C++ suite, and example passes.

- [ ] **Step 3: Run every checked-in JSON trace exactly once**

```bash
find contrib/llm/traces -maxdepth 1 -type f -name '*.json' -printf '%f\n' | sort
PYTHONDONTWRITEBYTECODE=1 python3 contrib/llm/scripts/live_test_traces.py
```

Expected: one successful experiment per discovered JSON trace, renamed executable used, pretty output parsed, and all eight validation flags true.

- [ ] **Step 4: Prove cleanup and preserve user artifacts**

```bash
if find /tmp -maxdepth 1 -type d -name 'llm-trace-live-*' -print -quit | rg -q .; then exit 1; fi
if find contrib/llm -type d -name __pycache__ -print -quit | rg -q .; then exit 1; fi
python3 - <<'PY'
from hashlib import sha256
from pathlib import Path

path = Path("run/26-08-25_14-12-55/mac-node-stats.json")
data = path.read_bytes()
assert len(data) == 395609
assert sha256(data).hexdigest() == "bc62df0060a4b7be9b7d4b841c69a64753f34a65f5cdf1df4ddb779ba8703b2a"
PY
git -C contrib/llm status --short --branch
git status --short --branch
```

Expected: no task-created temporary/cache directories remain, the preserved artifact matches its baseline, nested status is clean, and outer status still contains only the user's `.gitignore` modification and `VAGUE_TASK.md`.

- [ ] **Step 5: Commit only concrete verification fixes**

If verification required corrections, stage only their exact paths, rerun Steps 1-4, and commit:

```bash
git commit -m "llm: Fix final layout verification"
```

If no correction was required, create no empty commit.
