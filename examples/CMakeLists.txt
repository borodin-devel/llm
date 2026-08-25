build_lib_example(
  NAME llm_sample
  SOURCE_FILES
    sample-scenario.cc
    scenario-config.cc
    scenario-config-toml.cc
    scenario-log.cc
    scenario-topology.cc
    traffic-coordinator.cc
    traffic-flow-monitor.cc
    wifi-statistics.cc
    wifi-statistics-json.cc
    wifi-statistics-owner.cc
    wifi-statistics-report.cc
  LIBRARIES_TO_LINK ${libllm}
)
