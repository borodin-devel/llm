
set(source_files
    model/agent-distribution.cc
    model/contention-aware-agent-distribution.cc
    model/traffic-sink.cc
    model/ap-generator.cc
    model/sta-llm-generator.cc
)

set(header_files
    lib/json.hpp
    model/traffic-sink.h
    model/agent-distribution.h
    model/contention-aware-agent-distribution.h
    model/ap-generator.h
    model/sta-llm-generator.h
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
)
