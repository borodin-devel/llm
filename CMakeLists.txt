
set(source_files
    model/app-tx-tag.cc
    model/agent-distribution.cc
    model/contention-aware-agent-distribution.cc
    model/contention-aware-bss-assignment.cc
    model/contention-aware-sta-assignment.cc
    model/llm-log.cc
    model/trace-parser.cc
    model/traffic-sink.cc
    model/ap-generator.cc
    model/sta-llm-generator.cc
)

set(header_files
    lib/json.hpp
    model/agent-data.h
    model/app-tx-tag.h
    model/traffic-sink.h
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
