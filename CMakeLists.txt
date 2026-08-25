
set(source_files
    model/app-tx-tag.cc
    model/agent-distribution.cc
    model/contention-aware-agent-distribution.cc
    model/contention-aware-bss-assignment.cc
    model/contention-aware-sta-assignment.cc
    model/llm-log.cc
    model/trace-parser.cc
    model/traffic-sink.cc
    model/traffic-schedule.cc
    model/ap-generator.cc
    model/ap-generator-report.cc
    model/sta-llm-generator.cc
    model/sta-llm-generator-report.cc
)

set(header_files
    lib/json.hpp
    # toml++ v3.4.0, MIT, https://github.com/marzer/tomlplusplus
    lib/toml.hpp
    model/agent-data.h
    model/app-tx-tag.h
    model/traffic-sink.h
    model/traffic-schedule.h
    model/agent-distribution.h
    model/contention-aware-agent-distribution.h
    model/ap-generator.h
    model/sta-llm-generator.h
    model/trace-parser.h
)

set(test_sources
    test/llm-test-suite.cc
    test/trace-parser-test-suite.cc
    test/agent-distribution-test-suite.cc
    test/app-tx-tag-test-suite.cc
    test/traffic-schedule-test-suite.cc
    test/traffic-coordinator-test-suite.cc
    test/scenario-config-test-suite.cc
    test/scenario-config-json-test-suite.cc
    test/scenario-config-validation-test-suite.cc
    test/scenario-run-path-test-suite.cc
    test/scenario-topology-test-suite.cc
    test/experiment-json-test-suite.cc
    examples/scenario-log.cc
    examples/scenario-config.cc
    examples/scenario-config-json.cc
    examples/scenario-config-validation.cc
    examples/scenario-config-cli.cc
    examples/scenario-config-toml.cc
    examples/scenario-run-path.cc
    examples/scenario-topology.cc
    examples/traffic-coordinator.cc
    examples/traffic-flow-monitor.cc
    examples/traffic-flow-summary.cc
    examples/experiment-json.cc
    examples/transmission-summary-json.cc
    examples/cross-layer-summary-json.cc
    examples/wifi-statistics.cc
    examples/wifi-statistics-json.cc
    examples/wifi-statistics-owner.cc
    examples/wifi-statistics-summary.cc
    test/wifi-statistics-test-suite.cc
    test/traffic-flow-summary-test-suite.cc
    test/cross-layer-summary-test-suite.cc
)

build_lib(
  LIBNAME llm
  SOURCE_FILES ${source_files}
  HEADER_FILES ${header_files}
  LIBRARIES_TO_LINK
    ${libinternet-apps}
    ${libflow-monitor}
    ${libspectrum}
    ${libbuildings}
    ${libvirtual-net-device}
    ${libapplications}
    ${libwifi}
    ${libconfig-store}
  PRIVATE_HEADER_FILES
    model/contention-aware-distribution-internal.h
    model/llm-log.h
  TEST_SOURCES ${test_sources}
)
