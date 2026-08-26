# Project Layout and Readable JSON Design

## Goal

Reorganize the `llm` contrib module into a shallow, responsibility-based
directory hierarchy, give the scenario and starter configuration precise
names, and make the experiment JSON readable without sacrificing streaming
output.

The change favors clarity over path compatibility. Names and internal paths
that change are removed rather than retained through aliases, wrappers,
copies, or symlinks. The two script paths explicitly retained below remain
canonical entry points, not compatibility shims.

## Naming contract

- Rename `examples/sample-scenario.cc` to `examples/llm-scenario.cc`.
- Rename the `llm_sample` example target and executable to `llm-scenario`.
- Launch the example as `./ns3 run "llm-scenario ..."`.
- Rename `config/basic_config.toml` to `config/llm_config.toml`.
- Update current CMake, tests, scripts, README examples, and README links to
  the new names.
- Preserve historical files under `docs/superpowers/plans/` and
  `docs/superpowers/specs/` as records of the designs they originally
  described. Active documentation must not advertise historical paths.

Public C++ type names remain unchanged unless a physical move exposes an
actual ambiguity. In particular, moving public model headers must preserve
their installed `#include "ns3/<header>.h"` spelling. The requested change is
an organization and executable-path cutover, not a model API redesign.

## Organization principles

1. Group files by responsibility, not by extension.
2. Keep a `.cc` file beside its corresponding `.h` file.
3. Keep the hierarchy shallow: normally one domain directory and at most one
   focused child directory.
4. Keep only true entry points and ns-3 registry files at area roots.
5. Do not introduce a generic `helpers/`, `common/`, or `misc/` directory for
   unrelated code.
6. Do not introduce one `CMakeLists.txt` per child directory. The module and
   example CMake files remain the authoritative build inventories and group
   paths by responsibility.
7. Mirror source responsibilities in tests so a test's owner is apparent
   from its path.
8. Do not add a `scenarios/` directory.

## Target directory structure

```text
contrib/llm/
|-- config/
|   `-- llm_config.toml
|-- examples/
|   |-- CMakeLists.txt
|   |-- llm-scenario.cc
|   |-- config/
|   |-- runtime/
|   `-- statistics/
|       `-- json/
|-- model/
|   |-- applications/
|   |-- distribution/
|   |-- logging/
|   `-- traces/
|-- scripts/
|   |-- find_window.py
|   |-- live_test_traces.py
|   |-- trace_tools/
|   |-- live_verification/
|   `-- tests/
|       |-- trace_tools/
|       `-- live_verification/
`-- test/
    |-- llm-test-suite.cc
    |-- llm-test-suite.h
    |-- examples-to-run.py
    |-- data/
    |-- config/
    |-- model/
    |   |-- applications/
    |   |-- distribution/
    |   `-- traces/
    |-- runtime/
    `-- statistics/
```

Root files such as `README.md`, `README_RU.md`, `CMakeLists.txt`, and the
vendored `lib/` directory are outside this reorganization.

## Model mapping

Public header basenames remain descriptive because ns-3 installs them into
the shared `ns3/` include namespace.

| Destination | Files |
|---|---|
| `model/distribution/` | `agent-data.h`, `agent-distribution.*`, all `contention-aware-*` files |
| `model/applications/` | `ap-generator.*`, `sta-llm-generator.*`, `traffic-sink.*`, `traffic-schedule.*`, `app-tx-tag.*` |
| `model/traces/` | `trace-parser.*` |
| `model/logging/` | `llm-log.*` |

The move must not reverse dependency direction: model code remains independent
of example configuration, runtime assembly, and statistics code.

## Example mapping

### Configuration

`examples/config/` owns scenario configuration definition, input parsing,
validation, effective-configuration serialization, and path resolution:

| Current file | Destination filename |
|---|---|
| `scenario-config.h` | `config/scenario-config.h` |
| `scenario-config.cc` | `config/scenario-config.cc` |
| `scenario-config-internal.h` | `config/internal.h` |
| `scenario-config-cli.cc` | `config/cli.cc` |
| `scenario-config-toml.cc` | `config/toml.cc` |
| `scenario-config-json.cc` | `config/json.cc` |
| `scenario-config-validation.cc` | `config/validation.cc` |
| `scenario-run-path.cc` | `config/run-path.cc` |

### Runtime assembly

`examples/runtime/` owns the pieces used to assemble and coordinate one
simulation after configuration is resolved:

| Current file | Destination filename |
|---|---|
| `scenario-log.*` | `runtime/log.*` |
| `scenario-topology.*` | `runtime/topology.*` |
| `traffic-coordinator.*` | `runtime/traffic-coordinator.*` |

### Statistics

`examples/statistics/` owns collection, windowing, summary construction, and
validation. The `ExperimentStatistics` class retains its existing name.

| Current file | Destination filename |
|---|---|
| `experiment-statistics.h` | `statistics/experiment-statistics.h` |
| `experiment-statistics-owner.cc` | `statistics/experiment-statistics.cc` |
| `experiment-statistics-internal.h` | `statistics/internal.h` |
| `experiment-statistics-types.h` | `statistics/types.h` |
| `experiment-window-output.h` | `statistics/output-types.h` |
| `experiment-statistics-window.cc` | `statistics/window.cc` |
| `experiment-statistics-app.cc` | `statistics/app.cc` |
| `experiment-statistics-tcp.cc` | `statistics/tcp.cc` |
| `experiment-statistics-device.cc` | `statistics/device.cc` |
| `experiment-statistics-mac.cc` | `statistics/mac.cc` |
| `experiment-statistics-phy.cc` | `statistics/phy.cc` |
| `experiment-statistics-summary.cc` | `statistics/summary.cc` |
| `experiment-statistics-validation.cc` | `statistics/validation.cc` |

`examples/statistics/json/` owns JSON mechanics and serialization of already
computed output values:

| Current file | Destination filename |
|---|---|
| `experiment-output-internal.h` | `statistics/json/writer.h` |
| `experiment-json.cc` | `statistics/json/writer.cc` |
| `experiment-statistics-json.cc` | `statistics/json/hierarchy.cc` |
| `experiment-statistics-json-entity.cc` | `statistics/json/entity.cc` |
| `experiment-statistics-json-general.cc` | `statistics/json/general.cc` |
| `experiment-statistics-json-app.cc` | `statistics/json/app.cc` |
| `experiment-statistics-json-tcp.cc` | `statistics/json/tcp.cc` |
| `experiment-statistics-json-mac.cc` | `statistics/json/mac.cc` |
| `experiment-statistics-json-phy.cc` | `statistics/json/phy.cc` |

The scenario entry point depends on configuration, runtime assembly,
statistics, and the public model. JSON code depends on statistics output types
and configuration metadata. Collectors may depend on runtime topology and
coordinator state plus model trace sources. No reverse dependency is allowed.

## Script mapping

`scripts/find_window.py` and `scripts/live_test_traces.py` remain the only
root-level scripts. They are canonical, thin command-line entry points, not
compatibility wrappers.

`scripts/trace_tools/` becomes a Python package and contains the reusable
streaming trace implementation:

- `trace_stream.py` becomes `trace_tools/stream.py`.
- `find_window.py` imports the package rather than a sibling module.

`scripts/live_verification/` becomes a Python package. The repeated
`live_trace_` filename prefix is removed because the directory supplies that
context:

| Current file | Destination filename |
|---|---|
| `live_trace_cleanup.py` | `live_verification/cleanup.py` |
| `live_trace_common.py` | `live_verification/common.py` |
| `live_trace_runner.py` | `live_verification/runner.py` |
| `live_trace_schema.py` | `live_verification/schema.py` |
| `live_trace_schema_categories.py` | `live_verification/schema_categories.py` |

Python tests move out of the production implementation directories:

- `test_trace_stream.py` becomes `tests/trace_tools/test_stream.py`.
- `test_live_trace_cleanup.py` becomes
  `tests/live_verification/test_cleanup.py`.
- `test_live_trace_runner.py` becomes
  `tests/live_verification/test_runner.py`.
- `test_live_trace_schema_root.py` becomes
  `tests/live_verification/test_schema_root.py`.
- `test_live_trace_schema_ordering.py` becomes
  `tests/live_verification/test_schema_ordering.py`.
- `live_trace_test_fixtures.py` becomes
  `tests/live_verification/fixtures.py`.

Package markers and explicit package imports replace implicit same-directory
imports. The two entry points must still work directly from the documented
repository-root commands. The live entry point retains `--self-test` and
discovers the relocated deterministic tests explicitly.

## C++ test mapping

The test registry and ns-3 example registry remain at `test/` root. Test data
remains in `test/data/`. Other suites mirror the implementation area:

| Destination | Test suites |
|---|---|
| `test/model/distribution/` | agent distribution |
| `test/model/applications/` | application tag and traffic schedule |
| `test/model/traces/` | trace parser |
| `test/config/` | scenario config, config JSON, config validation, run path |
| `test/runtime/` | logging, topology, traffic coordinator |
| `test/statistics/` | every `experiment-*` suite |

Test factory names and the `llm` suite name remain unchanged. The directory
move must not fragment the suite into independently registered suites.

## Streaming readable JSON

The output schema, field order, values, schema version, measurement semantics,
validation logic, no-clobber behavior, and final newline remain unchanged.
Only whitespace changes.

All serializers use one focused `JsonWriter` owned by
`examples/statistics/json/writer.h`. It wraps an `std::ostream` and exposes
operations for:

- beginning and ending objects;
- beginning and ending arrays;
- writing an object key;
- writing scalar and null values; and
- finishing exactly one complete root value and writing its final newline.

The writer keeps a stack containing only container kind, first-element state,
and whether an object expects a key or value. It inserts newlines and two
spaces per nesting level. Its finish operation accepts only one closed root
value and appends exactly one newline. This requires memory proportional to
nesting depth, not output size. Scalar escaping and numeric spelling continue
to use `nlohmann::json(value).dump()`; a complete output DOM is never
constructed.

Serializers must stop writing JSON punctuation directly to the stream. This
centralizes comma placement, indentation, escaping, and structural validity.
There is no compact mode and no formatting configuration option.

The writer throws `std::logic_error` for invalid operation sequences such as
writing a value while an object expects a key, closing the wrong container,
or finishing an incomplete document. These errors indicate programming bugs.
The top-level file writer retains the existing `std::runtime_error` handling
for exclusive creation, write, flush, and close failures.

## Migration sequence

The implementation remains buildable at review checkpoints:

1. Add failing formatting/state tests and implement `JsonWriter` in the
   current layout.
2. Convert all JSON serializers and prove parsed output equivalence.
3. Move model files and matching model tests; update the root CMake inventory.
4. Move configuration/runtime/statistics example files and matching tests;
   update both CMake inventories and local includes.
5. Reorganize Python implementation and tests behind the two entry points.
6. Rename the source, executable, and starter configuration; update automated
   example commands and live-run construction.
7. Update `README.md` and `README_RU.md`, including commands, links, structure,
   and output-format wording.
8. Scan active code and documentation for obsolete paths and verify the final
   tree.

Moves should preserve Git history. Mechanical movement and semantic writer
changes should be kept in separately reviewable commits where practical.

## Verification

### Focused tests

- Exact two-space formatting for nested objects, arrays, empty containers,
  strings, numbers, booleans, and null.
- One final newline at the complete-document boundary.
- Rejection of invalid `JsonWriter` state transitions.
- Parsing the emitted hierarchy and asserting the existing schema and values.
- A formatting assertion on real scenario output, not only a writer fixture.
- Direct execution of both Python entry points after package relocation.
- Python test discovery from the new `scripts/tests/` hierarchy.

### Project checks

- Run ns-3 style checks on the moved C++ areas.
- Build `llm-test` and `llm-scenario` with examples and tests enabled.
- Run `./test.py -s llm`.
- Run the registered `llm-scenario` smoke example.
- Run all deterministic Python tests.
- Use `git diff --check` and active-tree stale-path scans.

### Live trace checks

Run the live-verification matrix sequentially for exactly one experiment per
JSON trace discovered under `contrib/llm/traces/`. Preserve its existing
duration policy and validate every generated document, including all eight
statistics validation flags. The matrix must use the renamed executable and
starter configuration.

All live-run directories, generated outputs, extracted data, and Python cache
directories created by verification must be removed. Tracked trace inputs and
pre-existing user artifacts must remain unchanged.

## Acceptance criteria

- `llm-scenario` is the only example target and documented executable name.
- `config/llm_config.toml` is the only starter configuration path.
- `examples/`, `model/`, `scripts/`, and `test/` match the approved
  responsibility hierarchy.
- Current documentation contains no obsolete active path or command.
- Public model headers remain usable through their established `ns3/` include
  names.
- `output.json` is always valid, two-space-indented JSON produced by a
  bounded-memory streaming writer.
- All C++, Python, smoke, and live trace checks pass with no task-created
  leftovers.
