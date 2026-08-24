build_lib_example(
  NAME llm_sample
  SOURCE_FILES
    sample-scenario.cc
    scenario-log.cc
    traffic-coordinator.cc
  LIBRARIES_TO_LINK ${libllm}
)
