build_lib_example(
  NAME llm_sample
  SOURCE_FILES
    sample-scenario.cc
    scenario-config.cc
    scenario-config-json.cc
    scenario-config-validation.cc
    scenario-config-cli.cc
    scenario-config-toml.cc
    scenario-run-path.cc
    scenario-log.cc
    scenario-topology.cc
    traffic-coordinator.cc
    traffic-flow-monitor.cc
    traffic-flow-summary.cc
    wifi-statistics.cc
    wifi-statistics-json.cc
    wifi-statistics-owner.cc
    wifi-statistics-report.cc
  LIBRARIES_TO_LINK ${libllm}
)
