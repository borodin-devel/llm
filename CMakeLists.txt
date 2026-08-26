
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
    test/traffic-coordinator-test-suite.cc
    test/scenario-config-test-suite.cc
    test/scenario-config-json-test-suite.cc
    test/scenario-config-validation-test-suite.cc
    test/scenario-logging-test-suite.cc
    test/scenario-run-path-test-suite.cc
    test/scenario-topology-test-suite.cc
    test/experiment-json-test-suite.cc
    test/experiment-hierarchy-json-test-suite.cc
    test/experiment-window-test-suite.cc
    test/experiment-app-test-suite.cc
    test/experiment-tcp-test-suite.cc
    test/experiment-device-mac-test-suite.cc
    test/experiment-phy-test-suite.cc
    test/experiment-summary-test-suite.cc
    test/experiment-validation-test-suite.cc
    examples/scenario-log.cc
    examples/scenario-config.cc
    examples/scenario-config-json.cc
    examples/scenario-config-validation.cc
    examples/scenario-config-cli.cc
    examples/scenario-config-toml.cc
    examples/scenario-run-path.cc
    examples/scenario-topology.cc
    examples/traffic-coordinator.cc
    examples/experiment-json.cc
    examples/experiment-statistics-owner.cc
    examples/experiment-statistics-window.cc
    examples/experiment-statistics-app.cc
    examples/experiment-statistics-tcp.cc
    examples/experiment-statistics-device.cc
    examples/experiment-statistics-mac.cc
    examples/experiment-statistics-phy.cc
    examples/experiment-statistics-summary.cc
    examples/experiment-statistics-validation.cc
    examples/experiment-statistics-json.cc
    examples/experiment-statistics-json-general.cc
    examples/experiment-statistics-json-app.cc
    examples/experiment-statistics-json-tcp.cc
    examples/experiment-statistics-json-mac.cc
    examples/experiment-statistics-json-phy.cc
    examples/experiment-statistics-json-entity.cc
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
    ${libwifi}
    ${libconfig-store}
  PRIVATE_HEADER_FILES
    model/distribution/contention-aware-distribution-internal.h
    model/logging/llm-log.h
  TEST_SOURCES ${test_sources}
)
