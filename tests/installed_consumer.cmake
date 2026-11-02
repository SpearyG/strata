set(prefix "${STRATA_BINARY_DIR}/consumer-install")
set(consumer_build "${STRATA_BINARY_DIR}/consumer-build")
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${STRATA_BINARY_DIR}" --prefix "${prefix}"
  --config "${STRATA_BUILD_TYPE}" RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 20)
if(NOT status STREQUAL "0")
  message(FATAL_ERROR "package install failed (${status}): ${output}${errors}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${STRATA_SOURCE_DIR}/tests/consumer" -B "${consumer_build}"
  -G "${STRATA_GENERATOR}" "-DCMAKE_CXX_COMPILER=${STRATA_COMPILER}"
  "-DCMAKE_BUILD_TYPE=${STRATA_BUILD_TYPE}" "-DCMAKE_PREFIX_PATH=${prefix}"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 30)
if(NOT status STREQUAL "0")
  message(FATAL_ERROR "package configure failed (${status}): ${output}${errors}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${consumer_build}" --config "${STRATA_BUILD_TYPE}"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 30)
if(NOT status STREQUAL "0")
  message(FATAL_ERROR "package consumer link failed (${status}): ${output}${errors}")
endif()
execute_process(COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${consumer_build}" --no-tests=error
  --output-on-failure -C "${STRATA_BUILD_TYPE}" RESULT_VARIABLE status
  OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 20)
if(NOT status STREQUAL "0")
  message(FATAL_ERROR "package consumer failed (${status}): ${output}${errors}")
endif()
