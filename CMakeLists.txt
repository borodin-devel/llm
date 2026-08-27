
set(source_files
    # Model applications
    model/applications/ap-generator.cc
    model/applications/app-tx-tag.cc
    model/applications/sta-llm-generator.cc
    model/applications/traffic-sink.cc
    model/applications/traffic-schedule.cc
    # Model distribution
    model/distribution/agent-distribution.cc
    model/distribution/contention-aware-agent-distribution.cc
    model/distribution/contention-aware-bss-assignment.cc
    model/distribution/contention-aware-sta-assignment.cc
    # Model logging
    model/logging/llm-log.cc
    # Model traces
    model/traces/trace-parser.cc
)

set(header_files
    lib/json.hpp
    # toml++ v3.4.0, MIT, https://github.com/marzer/tomlplusplus
    lib/toml.hpp
    # Model applications
    model/applications/ap-generator.h
    model/applications/app-tx-tag.h
    model/applications/sta-llm-generator.h
    model/applications/traffic-sink.h
    model/applications/traffic-schedule.h
    # Model distribution
    model/distribution/agent-data.h
    model/distribution/agent-distribution.h
    model/distribution/contention-aware-agent-distribution.h
    # Model traces
    model/traces/trace-parser.h
)

file(REMOVE
    "${CMAKE_HEADER_OUTPUT_DIRECTORY}/agent-data.h"
    "${CMAKE_HEADER_OUTPUT_DIRECTORY}/agent-distribution.h"
    "${CMAKE_HEADER_OUTPUT_DIRECTORY}/ap-generator.h"
    "${CMAKE_HEADER_OUTPUT_DIRECTORY}/app-tx-tag.h"
    "${CMAKE_HEADER_OUTPUT_DIRECTORY}/contention-aware-agent-distribution.h"
    "${CMAKE_HEADER_OUTPUT_DIRECTORY}/contention-aware-distribution-internal.h"
    "${CMAKE_HEADER_OUTPUT_DIRECTORY}/llm-log.h"
    "${CMAKE_HEADER_OUTPUT_DIRECTORY}/sta-llm-generator.h"
    "${CMAKE_HEADER_OUTPUT_DIRECTORY}/trace-parser.h"
    "${CMAKE_HEADER_OUTPUT_DIRECTORY}/traffic-schedule.h"
    "${CMAKE_HEADER_OUTPUT_DIRECTORY}/traffic-sink.h"
)

set(test_sources
    test/llm-test-suite.cc
    # Model applications
    test/model/applications/app-tx-tag-test-suite.cc
    test/model/applications/traffic-schedule-test-suite.cc
    # Model distribution
    test/model/distribution/agent-distribution-test-suite.cc
    # Model traces
    test/model/traces/trace-parser-test-suite.cc
    test/config/scenario-config-test-suite.cc
    test/config/scenario-config-json-test-suite.cc
    test/config/scenario-config-validation-test-suite.cc
    test/config/scenario-run-path-test-suite.cc
    test/saturated-tcp/config-test-suite.cc
    test/saturated-tcp/propagation-test-suite.cc
    test/saturated-tcp/access-wait-test-suite.cc
    test/saturated-tcp/sta-phy-metrics-test-suite.cc
    test/saturated-tcp/data-tx-metrics-test-suite.cc
    test/saturated-tcp/benchmark-output-test-suite.cc
    test/saturated-tcp/traffic-test-suite.cc
    test/saturated-tcp/topology-test-suite.cc
    test/runtime/scenario-logging-test-suite.cc
    test/runtime/scenario-topology-test-suite.cc
    test/runtime/traffic-coordinator-test-suite.cc
    # Statistics
    test/statistics/experiment-json-test-suite.cc
    test/statistics/experiment-hierarchy-json-test-suite.cc
    test/statistics/experiment-window-test-suite.cc
    test/statistics/experiment-app-test-suite.cc
    test/statistics/experiment-tcp-test-suite.cc
    test/statistics/experiment-device-mac-test-suite.cc
    test/statistics/experiment-phy-test-suite.cc
    test/statistics/experiment-summary-test-suite.cc
    test/statistics/experiment-validation-test-suite.cc
    examples/config/scenario-config.cc
    examples/config/json.cc
    examples/config/validation.cc
    examples/config/cli.cc
    examples/config/toml.cc
    examples/config/run-path.cc
    examples/saturated-tcp/config.cc
    examples/saturated-tcp/config-toml.cc
    examples/saturated-tcp/config-cli.cc
    examples/saturated-tcp/config-validation.cc
    examples/saturated-tcp/config-json.cc
    examples/saturated-tcp/bss-link-filter.cc
    examples/saturated-tcp/rssi-placement.cc
    examples/saturated-tcp/access-tracking-sta-wifi-mac.cc
    examples/saturated-tcp/access-wait-tracker.cc
    examples/saturated-tcp/sta-phy-metrics.cc
    examples/saturated-tcp/data-tx-metrics.cc
    examples/saturated-tcp/benchmark-statistics.cc
    examples/saturated-tcp/output.cc
    examples/saturated-tcp/saturated-tcp-sender.cc
    examples/saturated-tcp/readiness-barrier.cc
    examples/saturated-tcp/traffic.cc
    examples/saturated-tcp/topology.cc
    examples/saturated-tcp/log.cc
    examples/runtime/log.cc
    examples/runtime/topology.cc
    examples/runtime/traffic-coordinator.cc
    # Statistics core
    examples/statistics/experiment-statistics.cc
    examples/statistics/window.cc
    examples/statistics/app.cc
    examples/statistics/tcp.cc
    examples/statistics/device.cc
    examples/statistics/mac.cc
    examples/statistics/phy.cc
    examples/statistics/summary.cc
    examples/statistics/validation.cc
    # Statistics JSON
    examples/statistics/json/writer.cc
    examples/statistics/json/document.cc
    examples/statistics/json/hierarchy.cc
    examples/statistics/json/general.cc
    examples/statistics/json/app.cc
    examples/statistics/json/tcp.cc
    examples/statistics/json/mac.cc
    examples/statistics/json/phy.cc
    examples/statistics/json/entity.cc
)

build_lib(
  LIBNAME llm
  SOURCE_FILES ${source_files}
  HEADER_FILES ${header_files}
  LIBRARIES_TO_LINK
    ${libinternet-apps}
    ${libspectrum}
    ${libbuildings}
    ${libvirtual-net-device}
    ${libapplications}
    ${libpoint-to-point}
    ${libwifi}
    ${libconfig-store}
  PRIVATE_HEADER_FILES
    model/distribution/contention-aware-distribution-internal.h
    model/logging/llm-log.h
  TEST_SOURCES ${test_sources}
)
