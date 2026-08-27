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
    statistics/json/document.cc
    statistics/json/hierarchy.cc
    statistics/json/general.cc
    statistics/json/app.cc
    statistics/json/tcp.cc
    statistics/json/mac.cc
    statistics/json/phy.cc
    statistics/json/entity.cc
  LIBRARIES_TO_LINK ${libllm}
)

build_lib_example(
  NAME saturated-tcp-scenario
  SOURCE_FILES
    saturated-tcp-scenario.cc
    saturated-tcp/config.cc
    saturated-tcp/config-toml.cc
    saturated-tcp/config-cli.cc
    saturated-tcp/config-validation.cc
    saturated-tcp/config-json.cc
    saturated-tcp/bss-link-filter.cc
    saturated-tcp/rssi-placement.cc
    saturated-tcp/data-tx-metrics.cc
    saturated-tcp/benchmark-statistics.cc
    saturated-tcp/output.cc
    saturated-tcp/saturated-tcp-sender.cc
    saturated-tcp/readiness-barrier.cc
    saturated-tcp/traffic.cc
    saturated-tcp/topology.cc
    saturated-tcp/log.cc
    # Shared statistics identity core
    statistics/window.cc
    # Shared generic JSON document and component writers
    statistics/json/document.cc
    statistics/json/hierarchy.cc
    statistics/json/general.cc
    statistics/json/app.cc
    statistics/json/tcp.cc
    statistics/json/mac.cc
    statistics/json/phy.cc
    statistics/json/entity.cc
  LIBRARIES_TO_LINK
    ${libllm}
    ${libapplications}
    ${libinternet}
    ${libmobility}
    ${libpoint-to-point}
    ${libpropagation}
    ${libwifi}
)
