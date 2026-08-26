build_lib_example(
  NAME llm-scenario
  SOURCE_FILES
    llm-scenario.cc
    config/scenario-config.cc
    config/json.cc
    config/validation.cc
    config/cli.cc
    config/toml.cc
    config/run-path.cc
    runtime/log.cc
    runtime/topology.cc
    runtime/traffic-coordinator.cc
    # Statistics core
    statistics/experiment-statistics.cc
    statistics/window.cc
    statistics/app.cc
    statistics/tcp.cc
    statistics/device.cc
    statistics/mac.cc
    statistics/phy.cc
    statistics/summary.cc
    statistics/validation.cc
    # Statistics JSON
    statistics/json/writer.cc
    statistics/json/hierarchy.cc
    statistics/json/general.cc
    statistics/json/app.cc
    statistics/json/tcp.cc
    statistics/json/mac.cc
    statistics/json/phy.cc
    statistics/json/entity.cc
  LIBRARIES_TO_LINK ${libllm}
)
