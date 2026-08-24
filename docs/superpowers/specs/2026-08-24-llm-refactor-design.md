# LLM Module Refactor Design

Date: 2026-08-24

Status: Approved

## Context

The `llm` contrib module builds and links with ns-3, but several files combine
unrelated responsibilities:

- `examples/sample-scenario.cc` contains command-line parsing, topology
  construction, traffic coordination, trace callbacks, metric aggregation,
  JSON serialization, and reporting.
- `model/contention-aware-agent-distribution.cc` contains the complete BSS and
  STA assignment algorithms plus validation and result construction.
- `model/agent-distribution.h` combines trace data, distribution data, parsing,
  distribution APIs, and the application-to-PHY packet tag.
- The AP and STA traffic generators repeat data conversion, scheduling, and
  time-bucket calculations.
- Mutable scenario state is stored in file-level globals.
- The module has no automated unit tests.

This refactor will improve responsibility boundaries, naming, documentation,
and testability while preserving observable behavior.

## Goals

1. Split large implementation files by cohesive responsibility.
2. Prefer small pure functions and explicit state ownership.
3. Extract common AP/STA logic only where the behavior is genuinely shared.
4. Replace ambiguous private and local names with domain-specific names.
5. Add comments for invariants, units, ownership, lifetimes, and non-obvious
   ns-3 behavior.
6. Add deterministic unit and smoke tests.
7. Keep the implementation simple and avoid framework-like abstractions.

## Non-goals

- Change simulation results, timing, callback ordering, logs, trace sources,
  output schemas, or command-line behavior.
- Change existing public class names, public method signatures, public data
  structures, or exported header names.
- Replace the current distribution heuristics with new algorithms.
- Introduce a common traffic-generator base class.
- Introduce strategy interfaces, dependency-injection machinery, an event bus,
  or a generic statistics framework.
- Add slow full-network regression suites.

## Compatibility contract

The refactor must preserve:

- The `llm_sample` positional command-line interface, validation messages, and
  exit behavior.
- `ParseJsonFile()`, `DistributeAgents()`, and
  `DistributeAgentsContentionAware()` signatures and results.
- `APGenerator`, `StaLlmGenerator`, and `TrafficSink` public APIs, attributes,
  trace sources, and TypeId names.
- `Operation`, `AgentInfo`, `ParsedResult`, `DistributionResult`,
  `ContentionAwareDistributionConfig`, and `AppTxTag` source compatibility.
- Existing TCP setup and traffic scheduling order, including the common
  traffic epoch established after all required connections are ready.
- Existing statistics JSON keys, units, aggregation rules, and output order
  wherever that order is deterministic today.
- Existing log component names and log message text.
- Existing random seed, run number, topology, address assignment, and Wi-Fi
  configuration.

New focused headers may be added, but existing headers will remain compatibility
facades that include or declare the same public API.

## Proposed structure

```text
contrib/llm/
|-- model/
|   |-- agent-data.h
|   |-- app-tx-tag.h
|   |-- app-tx-tag.cc
|   |-- trace-parser.h
|   |-- trace-parser.cc
|   |-- agent-distribution.h
|   |-- agent-distribution.cc
|   |-- contention-aware-agent-distribution.h
|   |-- contention-aware-agent-distribution.cc
|   |-- contention-aware-distribution-internal.h
|   |-- contention-aware-bss-assignment.cc
|   |-- contention-aware-sta-assignment.cc
|   |-- traffic-schedule.h
|   |-- traffic-schedule.cc
|   |-- ap-generator.h
|   |-- ap-generator.cc
|   |-- sta-llm-generator.h
|   |-- sta-llm-generator.cc
|   |-- traffic-sink.h
|   `-- traffic-sink.cc
|-- examples/
|   |-- sample-scenario.cc
|   |-- scenario-config.h
|   |-- scenario-config.cc
|   |-- scenario-topology.h
|   |-- scenario-topology.cc
|   |-- traffic-coordinator.h
|   |-- traffic-coordinator.cc
|   |-- traffic-flow-monitor.h
|   |-- traffic-flow-monitor.cc
|   |-- wifi-statistics.h
|   |-- wifi-statistics.cc
|   `-- wifi-statistics-json.cc
`-- test/
    |-- agent-distribution-test-suite.cc
    |-- trace-parser-test-suite.cc
    |-- traffic-schedule-test-suite.cc
    |-- wifi-statistics-test-suite.cc
    `-- data/
        `-- minimal-trace.json
```

The exact number of implementation files may decrease if an extraction proves
too small to justify its own file. No new file should exist only to forward one
trivial call.

## Model responsibilities

### Agent data

`agent-data.h` owns the existing `Operation`, `AgentInfo`, `ParsedResult`, and
`DistributionResult` structures. Field names and types remain unchanged for
compatibility.

### Application-to-PHY tag

`app-tx-tag.h` and `app-tx-tag.cc` own `AppTxTag`. Serialization behavior and
TypeId remain unchanged. Moving method definitions out of the distribution
header removes an unrelated dependency and permits focused round-trip tests.

### Trace parsing

`trace-parser.cc` owns JSON parsing and type-number assignment. The public
`ParseJsonFile()` wrapper retains its current file-opening behavior. A lower
level stream-based function performs the actual parsing so tests do not need
process-level file-open failures.

Parser behavior remains unchanged:

- Agent keys retain the `id_type` format.
- Numeric type assignment retains current deterministic ordering.
- Operations with non-positive downlink or uplink bytes remain excluded.
- Excluded local operations still extend `experimentDurationMs`.

### Distribution

`agent-distribution.cc` retains the simple distributor and its public entry
point. Shared calculations such as total bytes and activity windows use clear
pure functions.

The contention-aware distributor is divided by phase:

- `contention-aware-agent-distribution.cc` validates configuration, coordinates
  phases, and builds the public result.
- `contention-aware-bss-assignment.cc` performs BSS selection.
- `contention-aware-sta-assignment.cc` performs station placement.
- `contention-aware-distribution-internal.h` contains only the private activity
  and assignment records shared by those phases.

There will be no public strategy hierarchy. Both public algorithms remain
ordinary functions.

### Traffic scheduling

`traffic-schedule.h` and `traffic-schedule.cc` contain the small pure pieces
shared by AP and STA traffic generation:

- Conversion from the existing tuple-based public input.
- Strongly named internal scheduled-operation records.
- Deterministic operation ordering.
- Absolute experiment-time conversion.
- Per-second bucket calculation.

AP and STA generators remain separate classes because their socket ownership,
connection counts, callbacks, and metrics differ substantially. Each class
continues to own its sockets, events, and per-connection state.

## Example responsibilities

### Entry point and configuration

`sample-scenario.cc` becomes a short entry point that:

1. Parses `ScenarioConfig`.
2. Applies the current ns-3 defaults in the current order.
3. Parses and distributes agents.
4. Constructs scenario-owned state objects.
5. Builds each AP group.
6. Runs the simulator.
7. Requests reports and destroys simulator state.

`scenario-config.*` owns positional argument parsing and validation. It retains
the current defaults, messages, and return behavior.

### Topology

`scenario-topology.*` creates AP and station nodes, channels, devices,
mobility, addresses, applications, and trace connections. It receives explicit
references to traffic coordination and statistics objects instead of mutating
file-level globals.

### Traffic coordination

`TrafficCoordinator` owns:

- AP and STA generator references.
- Application references used for stop-time updates.
- Expected and completed readiness counts.
- Trace duration, maximum experiment duration, and common experiment epoch.

It opens the readiness barrier exactly once, schedules traffic from the same
integer-second epoch, and preserves the current application and simulator stop
times.

### Traffic flow monitoring

`TrafficFlowMonitor` owns the current device TX/RX matching maps, byte totals,
and transmission-time report. It does not become a generic flow-monitoring
framework.

### Wi-Fi statistics

`WifiStatistics` owns the current PHY/MAC counters, node labels, topology
registries, trace deduplication state, per-window state, and cross-layer
reporting.

Collection and serialization are separated:

- `wifi-statistics.cc` records and aggregates values.
- `wifi-statistics-json.cc` writes the existing JSON schema from immutable
  aggregated state.

This separation permits tests of bucket boundaries, aggregation, summary
consistency, and serialization without running a complete simulation.

## Runtime data flow

```text
CLI arguments
  -> ScenarioConfig
  -> trace parsing
  -> contention-aware distribution
  -> AP/STA topology construction
  -> TCP readiness barrier
  -> common traffic epoch
  -> simulation callbacks
  -> statistics aggregation
  -> report serialization
```

`main()` owns `ScenarioConfig`, `TrafficCoordinator`, `TrafficFlowMonitor`, and
`WifiStatistics`. These objects outlive `Simulator::Run()`, so callbacks can
safely hold object pointers. Topology functions receive references explicitly.
Application objects continue to use ns-3 `Ptr<>` ownership.

## Error handling

- Command-line validation retains current messages and exit status.
- `ParseJsonFile()` retains its public file-open behavior.
- The lower-level parser reports malformed data with exceptions from the JSON
  layer, matching current parsing behavior.
- Invalid distribution configuration continues to throw
  `std::invalid_argument`.
- Impossible internal assignments continue to throw `std::runtime_error`.
- Invalid simulation state continues to use `NS_ABORT_MSG_IF`.
- The refactor does not add broad catch blocks that hide failures or change
  diagnostics.

## Naming rules

- Include units in time and size names: `startOffsetMs`, `timestampUs`, and
  `payloadBytes`.
- Name counts explicitly: `agentCount`, `readyGeneratorCount`, and
  `stationCount`.
- Name maps by relationship: `operationsByAgent`, `addressByAgent`, and
  `metricsByStation`.
- Use `bssIndex` for algorithmic radio groups and `apNode` for concrete ns-3
  access-point nodes.
- Replace vague local names such as `gen`, `ops`, `idx`, `info`, and `dist`
  where a concise domain name is available.
- Preserve public names required by the compatibility contract.

## Comment and documentation rules

- Add Doxygen documentation for public classes, methods, parameters, return
  values, and member variables according to ns-3 conventions.
- Document units, ownership, callback lifetime, scheduling invariants, slot
  boundaries, and reasons for non-obvious ns-3 choices.
- Keep the explanation of why `AppTxTag` is a byte tag rather than a packet
  tag.
- Use concise English for maintained comments.
- Remove decorative section banners, stale design essays, commented-out
  alternatives, and comments that only repeat the next statement.
- Use ASCII in comments and Doxygen documentation.

More comments means more explanation of intent and invariants, not narration of
straightforward statements.

## Size and simplicity guidelines

- Functions should usually stay below about 60 lines. Longer functions require
  a cohesive reason, not arbitrary extraction.
- Implementation files should usually stay below about 500 to 600 lines.
- Prefer pure free functions and small data structures.
- Introduce a class only when mutable state needs clear ownership and lifetime.
- Do not add an abstraction with only one trivial implementation unless it
  creates a test boundary or removes meaningful duplication.
- Do not add a `helper/` directory merely to reduce file sizes. ns-3 helper
  classes should be reusable APIs for external simulations.

## Test strategy

Tests are added before each extraction and characterize current behavior.

### Trace parser tests

- Deterministic agent and type ordering.
- Operation field conversion.
- Filtering of non-positive network operations.
- Experiment duration including filtered local operations.
- Malformed required fields.

### Distribution tests

- Current simple-distributor assignments for a fixed fixture.
- Current contention-aware assignments for both STA placement policies.
- Slot-boundary behavior.
- Capacity and configuration validation.
- Complete and deterministic result construction.

### Tag and traffic schedule tests

- `AppTxTag` serialized-size and round-trip behavior.
- Legacy tuple conversion.
- Stable operation ordering.
- Absolute-time conversion and second-boundary calculations.

### Statistics tests

- Window-index boundary calculations.
- Uplink and downlink attribution.
- PHY rate weighted averages.
- Sparse summary aggregation.
- Summary consistency checks.
- Existing JSON schema and units.

### Smoke test

One minimal trace runs through `llm_sample` with a bounded experiment time. It
checks successful startup, completion, and statistics output creation. It is
not a performance or full-network regression suite.

## Refactor sequence

1. Add characterization fixtures and tests against current public behavior.
2. Extract agent data, packet tag, and trace parsing.
3. Split simple and contention-aware distribution internals.
4. Extract shared traffic scheduling and simplify generator internals.
5. Extract scenario configuration and topology construction.
6. Replace scenario globals with traffic and statistics owners.
7. Separate statistics collection from JSON serialization.
8. Complete private naming and documentation cleanup.
9. Run module tests, the smoke test, style checks, and warnings-as-errors build.

Each step must leave the module buildable and its completed characterization
tests passing.

## Success criteria

- Existing public consumers remain source-compatible.
- The example retains its current observable behavior.
- No mutable scenario state remains in unrelated file-level globals.
- Major responsibilities match the proposed ownership boundaries.
- Common generator code is shared without a new inheritance hierarchy.
- Unit tests cover parsing, distribution, scheduling, tags, and statistics.
- The example has one lightweight smoke test.
- Comments and Doxygen satisfy ns-3 conventions without narrating simple code.
- The module configures and builds with examples, tests, logs, warnings, and
  warnings-as-errors enabled.
- `./test.py -s llm` passes.
